/* balloon.c -- the notification balloon.
 *
 * The yellow rounded tip that rises out of the notification area with an
 * icon, a bold title, a line or two of text and a close box, and goes away
 * after a few seconds: Windows 2000 introduced it and XP made it the one
 * everybody remembers. The body is a rounded rectangle with a tail that
 * points down at the notification area, cut out of the screen with the
 * Shape extension so the desktop shows round the corners.
 *
 * Every program's notifications come here: the shell's own through the
 * _W2K_NOTIFY property on the root window, everybody else's through the
 * org.freedesktop.Notifications service the shell provides (notifyd.c).
 * They queue, and show one after another, as Windows did. */
#include "wm.h"
#include "w2kui.h"
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BALLOON_MAXW  330
#define BALLOON_MINW  180
#define BALLOON_MS   7000
#define TAIL_H         16
#define TAIL_W         22
#define RADIUS          9
#define PAD            10
#define MAXLINES        5
#define QUEUE_MAX      16

typedef struct {
    char     title[128];
    char     text[512];
    int      icon;
    int      ms;
    unsigned id;                    /* D-Bus notification id, 0 for ours */
} Note;

static Note   queue[QUEUE_MAX];
static int    nqueue;

static Window balloon;
static Note   cur;
static long   b_until;
static int    b_w, b_h;
static int    b_tail_x;             /* where the tail's tip is, in the window */
static int    b_flip;               /* tail on top: the bar is along the top edge */
static W2kRect b_close;
static int    close_down;

/* Word-wrap the text to the balloon's inner width. */
static int wrap_text(const char *text, int width, char lines[MAXLINES][128])
{
    int n = 0;
    const char *p = text;
    while (*p && n < MAXLINES) {
        const char *nl = strchr(p, '\n');
        int fit = 0, last_space = 0;
        for (int i = 0; p[i] && i < 127; i++) {
            if (nl && p + i >= nl) break;
            char probe[128];
            snprintf(probe, sizeof probe, "%.*s", i + 1, p);
            if (w2k_text_width(F_UI, probe, -1) > width) break;
            fit = i + 1;
            if (p[i] == ' ') last_space = i + 1;
        }
        if (!fit) break;
        if (p[fit] && p[fit] != '\n' && last_space) fit = last_space;
        snprintf(lines[n], 128, "%.*s", fit, p);
        /* trim the trailing space the break left */
        size_t l = strlen(lines[n]);
        while (l && lines[n][l - 1] == ' ') lines[n][--l] = 0;
        p += fit;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
        n++;
    }
    return n;
}

/* The balloon's outline, filled: a rounded rectangle plus the tail, drawn
 * `inset` pixels inside the window's edge. Drawn once in black for the
 * border and the shape, then once more inset by one in the balloon's
 * colour, which leaves a one-pixel border all round. */
/* The balloon is laid out in logical pixels (b_w, b_h); this draws in
 * screen pixels, so every measure is mapped on its way to X. */
static void draw_shape(Drawable d, GC g, int inset)
{
    int P = w2k_ui_scale;
    #define M(v) ((int)((long)(v) * P / 100))
    int x0 = M(inset), y0 = M(inset + (b_flip ? TAIL_H : 0));
    int w = M(b_w - inset) - x0, body_h = M(b_h - TAIL_H - inset) - y0;
    int r = M(RADIUS - inset);
    if (r < 1) r = 1;
    XFillRectangle(w2k.dpy, d, g, x0 + r, y0, (unsigned)(w - 2 * r), (unsigned)body_h);
    XFillRectangle(w2k.dpy, d, g, x0, y0 + r, (unsigned)w, (unsigned)(body_h - 2 * r));
    XFillArc(w2k.dpy, d, g, x0, y0, (unsigned)(2 * r), (unsigned)(2 * r), 90 * 64, 90 * 64);
    XFillArc(w2k.dpy, d, g, x0 + w - 2 * r - 1, y0, (unsigned)(2 * r), (unsigned)(2 * r), 0, 90 * 64);
    XFillArc(w2k.dpy, d, g, x0, y0 + body_h - 2 * r - 1, (unsigned)(2 * r), (unsigned)(2 * r), 180 * 64, 90 * 64);
    XFillArc(w2k.dpy, d, g, x0 + w - 2 * r - 1, y0 + body_h - 2 * r - 1, (unsigned)(2 * r), (unsigned)(2 * r), 270 * 64, 90 * 64);
    /* The tail: from a base on the bottom edge down to its tip -- or, under
     * a bar along the top, from the top edge up. */
    int base_y = b_flip ? y0 : y0 + body_h - 1;
    int tip_x = M(b_tail_x), tip_y = b_flip ? M(inset) : M(b_h) - 1 - M(inset);
    int bl = M(b_tail_x - TAIL_W + 2 * inset), br = M(b_tail_x + 2 + inset);
    XPoint tri[3] = { { (short)bl, (short)base_y }, { (short)br, (short)base_y },
                      { (short)tip_x, (short)tip_y } };
    XFillPolygon(w2k.dpy, d, g, tri, 3, Convex, CoordModeOrigin);
    #undef M
}

static void apply_shape(void)
{
    int pw = w2k_px(b_w), ph = w2k_px(b_h);
    Pixmap mask = XCreatePixmap(w2k.dpy, balloon, (unsigned)pw, (unsigned)ph, 1);
    GC g = XCreateGC(w2k.dpy, mask, 0, NULL);
    XSetForeground(w2k.dpy, g, 0);
    XFillRectangle(w2k.dpy, mask, g, 0, 0, (unsigned)pw, (unsigned)ph);
    XSetForeground(w2k.dpy, g, 1);
    draw_shape(mask, g, 0);
    XShapeCombineMask(w2k.dpy, balloon, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreeGC(w2k.dpy, g);
    XFreePixmap(w2k.dpy, mask);
}

static void balloon_paint(void)
{
    char lines[MAXLINES][128];
    int n = wrap_text(cur.text, b_w - 2 * PAD - 4, lines);
    int fh = w2k_font_height(F_UI);

    XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_WINDOWFRAME]);
    draw_shape(balloon, w2k.gc, 0);
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_TOOLTIP]);
    draw_shape(balloon, w2k.gc, 1);

    /* Icon and bold title on the first row; the text under them, from
     * the icon's left edge, as XP set it. */
    int ty = PAD + (b_flip ? TAIL_H : 0);
    w2k_icon_draw(balloon, PAD, ty, cur.icon);
    w2k_text(balloon, F_UI_BOLD, PAD + 16 + 6, ty + (16 - w2k_font_height(F_UI_BOLD)) / 2,
             cur.title, C_TOOLTIPTEXT);
    for (int i = 0; i < n; i++)
        w2k_text(balloon, F_UI, PAD, ty + 16 + 6 + i * (fh + 2), lines[i], C_TOOLTIPTEXT);

    /* The close box: a small bordered square with an X, top right. */
    b_close = (W2kRect){ b_w - PAD - 13, ty - 1, 13, 13 };
    if (close_down) w2k_fill(balloon, b_close.x + 1, b_close.y + 1, 11, 11, C_FACE);
    w2k_frame(balloon, b_close.x, b_close.y, 13, 13, C_TOOLTIPTEXT);
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_TOOLTIPTEXT]);
    int cx = b_close.x + 3 + close_down, cy = b_close.y + 3 + close_down;
    for (int i = 0; i < 7; i++) {
        w2k_fill_fg(balloon, cx + i, cy + i, 1, 1);
        w2k_fill_fg(balloon, cx + 6 - i, cy + i, 1, 1);
    }
}

static void show_next(void);

/* Take the balloon down. `reason` is the freedesktop one: 1 expired,
 * 2 dismissed by the user, 3 closed by a program. */
static void dismiss(int reason)
{
    if (!balloon) return;
    XDestroyWindow(w2k.dpy, balloon);
    balloon = 0;
    b_until = 0;
    close_down = 0;
    if (cur.id) notifyd_closed(cur.id, reason);
    cur.id = 0;
    show_next();
}

static void show_next(void)
{
    if (balloon || !nqueue) return;
    cur = queue[0];
    memmove(queue, queue + 1, (size_t)(nqueue - 1) * sizeof *queue);
    nqueue--;

    /* Sized to the words: as wide as the longest line wants, within the
     * limits, and as tall as the wrapped text needs. */
    int fhb = w2k_font_height(F_UI_BOLD), fh = w2k_font_height(F_UI);
    int want = w2k_text_width(F_UI_BOLD, cur.title, -1) + PAD + 16 + 6 + 20 + PAD;
    int tw = w2k_text_width(F_UI, cur.text, -1) + 2 * PAD + 4;
    if (tw > want) want = tw;
    b_w = want > BALLOON_MAXW ? BALLOON_MAXW : want < BALLOON_MINW ? BALLOON_MINW : want;
    char lines[MAXLINES][128];
    int n = wrap_text(cur.text, b_w - 2 * PAD - 4, lines);
    if (n < 1) n = 1;
    (void)fhb;
    b_h = PAD + 16 + 6 + n * (fh + 2) + PAD + TAIL_H;

    /* Above the notification area, the tail pointing at it; below it when
     * the bar is at the top. */
    int ax, ay, top;
    taskbar_tray_anchor(&ax, &ay, &top);
    const W2kMonitor *m = w2k_monitor_primary();
    int wx, wy, ww, wh;
    wm_workarea_of(m, &wx, &wy, &ww, &wh);
    int pw = w2k_px(b_w), ph = w2k_px(b_h);
    int x = ax - w2k_px(b_w - 40);
    if (x + pw > wx + ww - 2) x = wx + ww - 2 - pw;
    if (x < wx + 2) x = wx + 2;
    b_tail_x = w2k_lp(ax - x);
    if (b_tail_x > b_w - RADIUS - 6) b_tail_x = b_w - RADIUS - 6;
    if (b_tail_x < RADIUS + TAIL_W) b_tail_x = RADIUS + TAIL_W;
    b_flip = top;
    int y = top ? ay : ay - ph;

    XSetWindowAttributes a = {
        .override_redirect = True, .save_under = True,
        .background_pixmap = None,
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask
    };
    balloon = XCreateWindow(w2k.dpy, w2k.root, x, y, (unsigned)pw, (unsigned)ph, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWSaveUnder | CWBackPixmap |
                            CWEventMask, &a);
    apply_shape();
    XMapRaised(w2k.dpy, balloon);
    balloon_paint();
    b_until = w2k_now_ms() + (cur.ms > 0 ? cur.ms : BALLOON_MS);
}

/* Queue a balloon. An id that is already queued or showing is replaced. */
void balloon_queue(const char *title, const char *text, int icon, int ms, unsigned id)
{
    if (!title) title = "";
    if (!text) text = "";
    if (!(id && balloon && cur.id == id)) w2k_sound_play(SND_NOTIFICATION);
    if (id && balloon && cur.id == id) {
        /* Replacing the one on screen: repaint it in place. */
        snprintf(cur.title, sizeof cur.title, "%s", title);
        snprintf(cur.text, sizeof cur.text, "%s", text);
        cur.icon = icon;
        XDestroyWindow(w2k.dpy, balloon);
        balloon = 0;
        Note again = cur;
        memmove(queue + 1, queue, (size_t)(nqueue < QUEUE_MAX - 1 ? nqueue : QUEUE_MAX - 1) * sizeof *queue);
        queue[0] = again;
        if (nqueue < QUEUE_MAX) nqueue++;
        show_next();
        return;
    }
    for (int i = 0; id && i < nqueue; i++)
        if (queue[i].id == id) {
            snprintf(queue[i].title, sizeof queue[i].title, "%s", title);
            snprintf(queue[i].text, sizeof queue[i].text, "%s", text);
            queue[i].icon = icon;
            queue[i].ms = ms;
            return;
        }
    if (nqueue >= QUEUE_MAX) {
        if (queue[0].id) notifyd_closed(queue[0].id, 3);
        memmove(queue, queue + 1, (size_t)(QUEUE_MAX - 1) * sizeof *queue);
        nqueue = QUEUE_MAX - 1;
    }
    Note *n = &queue[nqueue++];
    memset(n, 0, sizeof *n);
    snprintf(n->title, sizeof n->title, "%s", title);
    snprintf(n->text, sizeof n->text, "%s", text);
    n->icon = w2k_icon_valid(icon) ? icon : ICO_INFO;
    n->ms = ms;
    n->id = id;
    show_next();
}

void balloon_show(const char *title, const char *text)
{
    balloon_queue(title, text, ICO_INFO, 0, 0);
}

/* A program withdrew its notification (CloseNotification). */
void balloon_close_id(unsigned id)
{
    if (!id) return;
    if (balloon && cur.id == id) { dismiss(3); return; }
    for (int i = 0; i < nqueue; i++)
        if (queue[i].id == id) {
            memmove(queue + i, queue + i + 1, (size_t)(nqueue - i - 1) * sizeof *queue);
            nqueue--;
            notifyd_closed(id, 3);
            return;
        }
}

void balloon_hide(void)
{
    dismiss(2);
}

/* Called from the main loop: take it down when its time is up. */
void balloon_tick(void)
{
    if (balloon && b_until && w2k_now_ms() > b_until) dismiss(1);
}

/* When the balloon next needs to go away, or -1 if there is none up. */
int balloon_next_tick_ms(void)
{
    if (!balloon || !b_until) return -1;
    long left = b_until - w2k_now_ms();
    return left > 0 ? (int)left : 0;
}

int balloon_event(XEvent *e)
{
    if (!balloon) return 0;
    if (e->type == Expose && e->xexpose.window == balloon) {
        balloon_paint();
        return 1;
    }
    if (e->type == ButtonPress && e->xbutton.window == balloon) {
        if (w2k_rect_hit(&b_close, w2k_lp(e->xbutton.x), w2k_lp(e->xbutton.y))) {
            close_down = 1;
            balloon_paint();
        } else {
            /* Clicking the balloon itself is the "act on it" click. */
            if (cur.id) notifyd_action(cur.id, "default");
            dismiss(2);
        }
        return 1;
    }
    if (e->type == ButtonRelease && e->xbutton.window == balloon) {
        if (close_down) dismiss(2);
        return 1;
    }
    return 0;
}

/* Development aid, like the other W2K_RENDER hooks: show a balloon and
 * write its picture out as a PPM. */
int balloon_render(const char *path)
{
    balloon_queue("Take a tour of Windows XP",
                  "To learn about the exciting new features in XP now, click here. "
                  "To take the tour later, click All Programs on the Start menu, "
                  "and then click Accessories.", ICO_INFO, 0, 0);
    if (!balloon) return 0;
    /* No bar in this mode, so it was placed off the top: bring it on. */
    XMoveWindow(w2k.dpy, balloon, 40, 40);
    XSync(w2k.dpy, False);
    balloon_paint();
    XSync(w2k.dpy, False);
    int pw = w2k_px(b_w), ph = w2k_px(b_h);
    XImage *im = XGetImage(w2k.dpy, balloon, 0, 0, (unsigned)pw, (unsigned)ph, AllPlanes, ZPixmap);
    FILE *f = fopen(path, "wb");
    if (f && im) {
        fprintf(f, "P6\n%d %d\n255\n", pw, ph);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                unsigned long v = XGetPixel(im, x, y);
                unsigned char rgb[3] = { (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff };
                fwrite(rgb, 1, 3, f);
            }
    }
    if (f) fclose(f);
    if (im) XDestroyImage(im);
    dismiss(3);
    return 1;
}
