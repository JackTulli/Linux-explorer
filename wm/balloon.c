/* balloon.c -- the notification balloon.
 *
 * Windows 2000 introduced these: a rounded tip that rises out of the
 * notification area with a title, a line or two of text, and a close box,
 * fading away after a few seconds.
 *
 * Applications ask for one through _W2K_NOTIFY on the root window: the
 * title and text arrive as two NUL-separated strings in the property, and
 * the shell shows it. That is the same shape as the tray protocol -- a
 * property and a client message -- rather than anything invented here. */
#include "wm.h"
#include "w2kui.h"
#include <stdio.h>
#include <string.h>

#define BALLOON_W    260
#define BALLOON_MS  7000
#define TAIL_H        12

static Window balloon;
static char   b_title[128], b_text[512];
static long   b_until;
static int    b_w, b_h;
static W2kRect b_close;

static int wrap_text(const char *text, char lines[4][96])
{
    int n = 0, used = 0;
    const char *p = text;
    while (*p && n < 4) {
        int fit = 0, last_space = 0;
        for (int i = 0; p[i] && i < 95; i++) {
            char probe[96];
            snprintf(probe, sizeof probe, "%.*s", i + 1, p);
            if (w2k_text_width(F_UI, probe, -1) > BALLOON_W - 28) break;
            fit = i + 1;
            if (p[i] == ' ') last_space = i + 1;
        }
        if (!fit) break;
        if (p[fit] && last_space) fit = last_space;
        snprintf(lines[n], 96, "%.*s", fit, p);
        p += fit;
        while (*p == ' ') p++;
        n++;
        used = 1;
    }
    return used ? n : 0;
}

static void balloon_paint(void)
{
    char lines[4][96];
    int n = wrap_text(b_text, lines);
    int fh = w2k_font_height(F_UI);

    int body_h = b_h - TAIL_H;
    int tail_x = b_w - 40;                 /* where the tail leaves the body */

    w2k_fill(balloon, 0, 0, b_w, b_h, C_TOOLTIP);

    /* Body outline, with a gap along the bottom where the tail joins it --
     * otherwise the balloon looks like a box with a triangle stuck on. */
    w2k_hline(balloon, 0, 0, b_w, C_WINDOWFRAME);
    w2k_vline(balloon, 0, 0, body_h, C_WINDOWFRAME);
    w2k_vline(balloon, b_w - 1, 0, body_h, C_WINDOWFRAME);
    w2k_hline(balloon, 0, body_h - 1, tail_x, C_WINDOWFRAME);
    w2k_hline(balloon, tail_x + TAIL_H, body_h - 1,
              b_w - tail_x - TAIL_H, C_WINDOWFRAME);

    /* The tail: a right triangle dropping from the gap, filled in the
     * balloon's own colour and outlined down both sides. */
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_TOOLTIP]);
    for (int i = 0; i < TAIL_H; i++)
        XFillRectangle(w2k.dpy, balloon, w2k.gc, tail_x + i,
                       body_h - 1 + i, TAIL_H - i, 1);
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_WINDOWFRAME]);
    for (int i = 0; i < TAIL_H; i++) {
        XFillRectangle(w2k.dpy, balloon, w2k.gc, tail_x + i,
                       body_h - 1 + i, 1, 1);                  /* slope */
        XFillRectangle(w2k.dpy, balloon, w2k.gc, tail_x + TAIL_H - 1,
                       body_h - 1 + i, 1, 1);                  /* upright */
    }

    w2k_icon_draw(balloon, 8, 8, ICO_INFO);
    w2k_text(balloon, F_UI_BOLD, 30, 8, b_title, C_TOOLTIPTEXT);
    for (int i = 0; i < n; i++)
        w2k_text(balloon, F_UI, 30, 10 + fh + 4 + i * (fh + 1), lines[i],
                 C_TOOLTIPTEXT);

    /* The close box, top right. */
    b_close = (W2kRect){ b_w - 18, 6, 12, 12 };
    w2k_capglyph_close(balloon, b_close.x - 4, b_close.y - 3, C_TOOLTIPTEXT);
}

void balloon_show(const char *title, const char *text)
{
    if (!title || !text) return;
    snprintf(b_title, sizeof b_title, "%s", title);
    snprintf(b_text, sizeof b_text, "%s", text);

    char lines[4][96];
    int n = wrap_text(b_text, lines);
    int fh = w2k_font_height(F_UI);
    b_w = BALLOON_W;
    b_h = 10 + fh + 4 + (n ? n : 1) * (fh + 1) + 10 + TAIL_H;

    /* Sit above the notification area, on the primary monitor. */
    const W2kMonitor *m = w2k_monitor_primary();
    int wx, wy, ww, wh;
    wm_workarea_of(m, &wx, &wy, &ww, &wh);
    int x = wx + ww - b_w - 8;
    int y = wy + wh - b_h - 2;
    if (w2k_taskbar_edge == TB_TOP) y = wy + 2;

    if (!balloon) {
        XSetWindowAttributes a = {
            .override_redirect = True, .save_under = True,
            .background_pixel = w2k.col[C_TOOLTIP],
            .event_mask = ExposureMask | ButtonPressMask
        };
        balloon = XCreateWindow(w2k.dpy, w2k.root, x, y, b_w, b_h, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWSaveUnder | CWBackPixel |
                                CWEventMask, &a);
    } else {
        XMoveResizeWindow(w2k.dpy, balloon, x, y, b_w, b_h);
    }
    XMapRaised(w2k.dpy, balloon);
    balloon_paint();
    b_until = w2k_now_ms() + BALLOON_MS;
}

void balloon_hide(void)
{
    if (!balloon) return;
    XDestroyWindow(w2k.dpy, balloon);
    balloon = 0;
    b_until = 0;
}

/* Called from the main loop: take it down when its time is up. */
void balloon_tick(void)
{
    if (balloon && b_until && w2k_now_ms() > b_until) balloon_hide();
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
        balloon_hide();          /* clicking anywhere dismisses it */
        return 1;
    }
    return 0;
}
