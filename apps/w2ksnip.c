/* w2ksnip.c -- Snipping Tool.
 *
 * The Windows accessory as it stood by Windows 10: a small window with
 * New, Mode, Delay, Cancel and Options; a dimmed screen to drag a snip out
 * of (rectangle, free-form, a window, or the whole screen), optionally
 * after a countdown so that a menu or a tooltip can be opened first; and
 * the mark-up window it opens into, with a pen, a highlighter and an
 * eraser, Save As and Copy.
 *
 * One window plays both parts: the little toolbar strip to begin with,
 * and after a snip it grows into the editor -- which is how the real one
 * behaves. While the snip is taken the tool asks the window manager to
 * minimise it, so that it comes back exactly where it was; the screen is
 * read once (XGetImage of the root), dimmed, and put up as an
 * override-redirect window the size of the desktop with a pointer grab,
 * and the snip is cut out of the same picture. */
#include "w2kui.h"
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum { SNIP_FREEFORM, SNIP_RECT, SNIP_WINDOW, SNIP_FULL };
enum { TOOL_NONE, TOOL_PEN, TOOL_HIGHLIGHT, TOOL_ERASER };
enum {
    ID_NEW = 1, ID_MODE, ID_DELAY, ID_CANCELSNIP, ID_OPTIONS, ID_SAVE, ID_COPY,
    ID_SEND, ID_PEN, ID_PENMENU, ID_HIGHLIGHT, ID_ERASER, ID_EXIT, ID_PRINT,
    ID_ABOUT, ID_HELP, ID_SELECTALL,
    ID_TYPE_FREE = 100, ID_TYPE_RECT, ID_TYPE_WINDOW, ID_TYPE_FULL,
    ID_DELAY_0 = 150,                     /* .. ID_DELAY_0 + MAX_DELAY */
    ID_PEN_RED = 210, ID_PEN_BLUE, ID_PEN_BLACK,
};
#define MAX_DELAY 5                        /* seconds, as the Delay menu offers */

#define MAXPTS 4096
typedef struct {
    int type;                    /* TOOL_PEN or TOOL_HIGHLIGHT */
    int r, g, b;
    int n;
    short *x, *y;
} Stroke;

/* How the tool is out of the way while a snip is taken. */
enum { HIDE_NONE, HIDE_ICONIC, HIDE_UNMAPPED, HIDE_NOT_YET_SHOWN };

static struct {
    W2kWin     *win;
    W2kToolbar *tb;
    W2kMenubar *mb;
    int         shown;           /* w2k_win_show() has happened */
    int         hidden;          /* HIDE_* while capturing */
    int         editing;         /* the editor, not the little strip */
    int         type;            /* SNIP_* for the next snip */
    int         delay;           /* seconds to wait before the next snip */
    int         pending;         /* a delayed snip is counting down */
    int         countdown;       /* seconds left of it */
    int         tool;            /* TOOL_* in the editor */
    int         ink_r, ink_g, ink_b;      /* the selection ink, Options */
    int         pen_r, pen_g, pen_b;      /* the pen's colour */
    /* Options */
    int         opt_hide_text, opt_copy, opt_prompt_save, opt_overlay, opt_show_ink;

    unsigned char *rgba;         /* the snip */
    int         iw, ih;
    Pixmap      pm;              /* the snip, with the strokes drawn over it */
    int         pm_dirty;
    Stroke     *strokes;
    int         nstrokes, strokes_cap;
    Stroke     *cur;             /* the stroke being drawn */
    int         saved;           /* nothing to lose */
    W2kScroll   vsb, hsb;
    char        last_dir[1024];
} st;

/* ------------------------------------------------------------------ *
 * Options, kept in ~/.w2k/snippingtool
 * ------------------------------------------------------------------ */
static void options_path(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/.w2k/snippingtool", home ? home : ".");
}

static void options_load(void)
{
    st.type = SNIP_RECT;
    st.delay = 0;
    st.ink_r = 255; st.ink_g = 0; st.ink_b = 0;
    st.pen_r = 0; st.pen_g = 0; st.pen_b = 255;
    st.opt_copy = 1; st.opt_prompt_save = 1; st.opt_overlay = 1; st.opt_show_ink = 1;
    char path[1024];
    options_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "Type")) st.type = v;
        else if (!strcmp(line, "Delay")) st.delay = v;
        else if (!strcmp(line, "HideText")) st.opt_hide_text = v;
        else if (!strcmp(line, "Copy")) st.opt_copy = v;
        else if (!strcmp(line, "PromptSave")) st.opt_prompt_save = v;
        else if (!strcmp(line, "Overlay")) st.opt_overlay = v;
        else if (!strcmp(line, "ShowInk")) st.opt_show_ink = v;
        else if (!strcmp(line, "Ink")) { st.ink_r = (v >> 16) & 255; st.ink_g = (v >> 8) & 255; st.ink_b = v & 255; }
        else if (!strcmp(line, "Pen")) { st.pen_r = (v >> 16) & 255; st.pen_g = (v >> 8) & 255; st.pen_b = v & 255; }
    }
    fclose(f);
    if (st.type < SNIP_FREEFORM || st.type > SNIP_FULL) st.type = SNIP_RECT;
    if (st.delay < 0) st.delay = 0;
    if (st.delay > MAX_DELAY) st.delay = MAX_DELAY;
}

static void options_save(void)
{
    char path[1024];
    options_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "Type=%d\nDelay=%d\nHideText=%d\nCopy=%d\nPromptSave=%d\nOverlay=%d\nShowInk=%d\n"
               "Ink=%d\nPen=%d\n", st.type, st.delay, st.opt_hide_text, st.opt_copy,
            st.opt_prompt_save, st.opt_overlay, st.opt_show_ink,
            (st.ink_r << 16) | (st.ink_g << 8) | st.ink_b,
            (st.pen_r << 16) | (st.pen_g << 8) | st.pen_b);
    fclose(f);
}

/* ------------------------------------------------------------------ *
 * Getting the tool out of the picture
 * ------------------------------------------------------------------ *
 * The obvious way to hide -- unmap the window -- withdraws it, and the
 * window manager then treats the remap as a brand new window: it lands at
 * the next cascade position, on whichever monitor the pointer is over, and
 * the hints that made it fixed-size are read afresh. So the tool asks to
 * be minimised instead, exactly as if its taskbar button had been clicked,
 * and restores itself afterwards; the manager keeps everything else. */
static int wm_state_of(Window w)
{
    Atom type;
    int fmt, state = -1;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(w2k.dpy, w, w2k.a_wm_state, 0, 2, False, w2k.a_wm_state,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        if (type == w2k.a_wm_state && n >= 1) state = (int)((long *)data)[0];
        XFree(data);
    }
    return state;
}

static void hide_tool(void)
{
    if (!st.shown) { st.hidden = HIDE_NOT_YET_SHOWN; return; }
    XIconifyWindow(w2k.dpy, st.win->win, w2k.screen);
    XFlush(w2k.dpy);
    st.hidden = HIDE_ICONIC;
    long t0 = w2k_now_ms();
    while (wm_state_of(st.win->win) != IconicState) {
        if (w2k_now_ms() - t0 > 1500) {
            /* No window manager, or one that will not: fall back to
             * withdrawing, and accept the re-placement that follows. */
            XUnmapWindow(w2k.dpy, st.win->win);
            st.hidden = HIDE_UNMAPPED;
            break;
        }
        usleep(10000);
    }
    XSync(w2k.dpy, False);
    usleep(150000);                          /* let what was beneath repaint */
}

static void show_tool(void)
{
    switch (st.hidden) {
    case HIDE_NOT_YET_SHOWN:
        w2k_win_show(st.win);
        st.shown = 1;
        break;
    case HIDE_UNMAPPED:
        XMapWindow(w2k.dpy, st.win->win);
        break;
    case HIDE_ICONIC: {
        /* ICCCM says map to deiconify; a manager that only hid the frame
         * sees nothing in that, so ask for activation as well. */
        XMapWindow(w2k.dpy, st.win->win);
        XEvent e;
        memset(&e, 0, sizeof e);
        e.xclient.type = ClientMessage;
        e.xclient.window = st.win->win;
        e.xclient.message_type = w2k.a_net_active_window;
        e.xclient.format = 32;
        e.xclient.data.l[0] = 1;             /* the request is an application's */
        e.xclient.data.l[1] = CurrentTime;
        XSendEvent(w2k.dpy, w2k.root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &e);
        break;
    }
    }
    st.hidden = HIDE_NONE;
    XFlush(w2k.dpy);
}

/* ------------------------------------------------------------------ *
 * Taking the snip
 * ------------------------------------------------------------------ */
/* Everything the capture needs while the overlay is up. */
typedef struct {
    XImage *shot;                /* the screen, as it was */
    Window  ov;
    Pixmap  bright;              /* the screen, on the server */
    Pixmap  dim;                 /* the screen, dimmed: the overlay's ground */
    int     sw, sh;
    int     x0, y0, x1, y1;      /* the rectangle, or the window's */
    int     have_rect;
    short  *px, *py;             /* the free-form path */
    int     npts;
    int     dragging;
} Capture;

/* Where the channels sit in a pixel of this image, and how wide they are. */
typedef struct { int rs, gs, bs, rbits, gbits, bbits; unsigned long rm, gm, bm; } PixLayout;

static PixLayout pix_layout(const XImage *im)
{
    PixLayout l = { 0, 0, 0, 0, 0, 0, im->red_mask, im->green_mask, im->blue_mask };
    /* An image read back from a pixmap carries no visual, so Xlib leaves
     * its masks at zero; the pixels are laid out as the screen's visual
     * says. (Without this every saved snip came out black.) */
    if (!l.rm && !l.gm && !l.bm) {
        l.rm = w2k.visual->red_mask;
        l.gm = w2k.visual->green_mask;
        l.bm = w2k.visual->blue_mask;
    }
    while (l.rs < 32 && !((l.rm >> l.rs) & 1)) l.rs++;
    while (l.gs < 32 && !((l.gm >> l.gs) & 1)) l.gs++;
    while (l.bs < 32 && !((l.bm >> l.bs) & 1)) l.bs++;
    for (unsigned long t = l.rm >> l.rs; t & 1; t >>= 1) l.rbits++;
    for (unsigned long t = l.gm >> l.gs; t & 1; t >>= 1) l.gbits++;
    for (unsigned long t = l.bm >> l.bs; t & 1; t >>= 1) l.bbits++;
    return l;
}

static void pix_rgb(const PixLayout *l, unsigned long p, int *r, int *g, int *b)
{
    *r = (int)((p & l->rm) >> l->rs);
    *g = (int)((p & l->gm) >> l->gs);
    *b = (int)((p & l->bm) >> l->bs);
    /* masks may be narrower than 8 bits; scale up */
    if (l->rbits && l->rbits < 8) *r = *r * 255 / ((1 << l->rbits) - 1);
    if (l->gbits && l->gbits < 8) *g = *g * 255 / ((1 << l->gbits) - 1);
    if (l->bbits && l->bbits < 8) *b = *b * 255 / ((1 << l->bbits) - 1);
}

/* The screen, half way to white: what the tool puts over the desktop
 * while a snip is dragged out. */
static Pixmap dimmed_screen(XImage *shot, int sw, int sh)
{
    Pixmap pm = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)sw, (unsigned)sh, w2k.depth);
    size_t bytes = (size_t)shot->bytes_per_line * (size_t)sh;
    if (shot->bits_per_pixel == 32 && shot->depth == 24 && bytes % 4 == 0) {
        /* The usual case, done on the bytes: every channel of every pixel
         * halved and lifted by 128. The fourth byte is padding, and gets
         * the same treatment for free. A whole 4K desktop takes a few
         * milliseconds this way; going through XGetPixel took seconds. */
        char *copy = malloc(bytes);
        if (copy) {
            memcpy(copy, shot->data, bytes);
            uint32_t *p = (uint32_t *)copy;
            for (size_t i = 0, n = bytes / 4; i < n; i++)
                p[i] = ((p[i] >> 1) & 0x7f7f7f7fu) | 0x80808080u;
            XImage im = *shot;
            im.data = copy;
            XPutImage(w2k.dpy, pm, w2k.gc, &im, 0, 0, 0, 0, (unsigned)sw, (unsigned)sh);
            free(copy);
            return pm;
        }
    }
    /* Anything else -- a 16-bit visual, say -- goes the slow way. */
    char *pixels = malloc((size_t)sw * sh * 4);
    XImage *im = pixels ? XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap, 0,
                                       pixels, (unsigned)sw, (unsigned)sh, 32, 0) : NULL;
    if (!im) { free(pixels); XPutImage(w2k.dpy, pm, w2k.gc, shot, 0, 0, 0, 0, (unsigned)sw, (unsigned)sh); return pm; }
    PixLayout l = pix_layout(shot);
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            int r, g, b;
            pix_rgb(&l, XGetPixel(shot, x, y), &r, &g, &b);
            XPutPixel(im, x, y, w2k_rgb((r + 255) / 2, (g + 255) / 2, (b + 255) / 2));
        }
    XPutImage(w2k.dpy, pm, w2k.gc, im, 0, 0, 0, 0, (unsigned)sw, (unsigned)sh);
    XDestroyImage(im);
    return pm;
}

/* RGBA of a rectangle of the screenshot. */
static unsigned char *cut_out(XImage *shot, int x0, int y0, int w, int h)
{
    unsigned char *out = malloc((size_t)w * h * 4);
    if (!out) return NULL;
    PixLayout l = pix_layout(shot);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int r, g, b;
            pix_rgb(&l, XGetPixel(shot, x0 + x, y0 + y), &r, &g, &b);
            unsigned char *o = out + ((size_t)y * w + x) * 4;
            o[0] = (unsigned char)r; o[1] = (unsigned char)g; o[2] = (unsigned char)b; o[3] = 255;
        }
    return out;
}

/* Even-odd test, for the free-form snip's outline. */
static int inside_path(const Capture *c, int x, int y)
{
    int in = 0;
    for (int i = 0, j = c->npts - 1; i < c->npts; j = i++) {
        int xi = c->px[i], yi = c->py[i], xj = c->px[j], yj = c->py[j];
        if ((yi > y) != (yj > y) &&
            x < (xj - xi) * (double)(y - yi) / (double)(yj - yi) + xi)
            in = !in;
    }
    return in;
}

/* "Show selection ink after snips are captured": the ink outline drawn
 * into the picture itself, two pixels wide along its edge. */
static void ink_border(unsigned char *rgba, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2) {
                unsigned char *o = rgba + ((size_t)y * w + x) * 4;
                o[0] = (unsigned char)st.ink_r; o[1] = (unsigned char)st.ink_g; o[2] = (unsigned char)st.ink_b;
            }
}

/* The top-level window under a screen point: the child of the root
 * there, which under the shell is a frame -- so the snip takes the
 * window with its title bar, as the real tool does. Override-redirect
 * windows (our own overlay, the taskbar, menus) are looked through. */
static int window_under(int rx, int ry, int *x, int *y, int *w, int *h)
{
    Window *kids = NULL, dummy;
    unsigned n = 0;
    if (!XQueryTree(w2k.dpy, w2k.root, &dummy, &dummy, &kids, &n) || !kids) return 0;
    int found = 0;
    for (int i = (int)n - 1; i >= 0 && !found; i--) {
        XWindowAttributes a;
        if (!XGetWindowAttributes(w2k.dpy, kids[i], &a)) continue;
        if (a.map_state != IsViewable || a.override_redirect) continue;
        if (rx >= a.x && rx < a.x + a.width && ry >= a.y && ry < a.y + a.height) {
            *x = a.x; *y = a.y; *w = a.width; *h = a.height;
            found = 1;
        }
    }
    XFree(kids);
    return found;
}

static void pick_window(Capture *c, int rx, int ry)
{
    int x, y, w, h;
    if (!window_under(rx, ry, &x, &y, &w, &h)) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->sw) w = c->sw - x;
    if (y + h > c->sh) h = c->sh - y;
    c->x0 = x; c->y0 = y; c->x1 = x + w; c->y1 = y + h;
    c->have_rect = 1;
}

static void capture_paint(Capture *c)
{
    XCopyArea(w2k.dpy, c->dim, c->ov, w2k.gc, 0, 0, (unsigned)c->sw, (unsigned)c->sh, 0, 0);
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(st.ink_r, st.ink_g, st.ink_b));
    XSetLineAttributes(w2k.dpy, w2k.gc, 2, LineSolid, CapRound, JoinRound);
    if (st.type == SNIP_FREEFORM && c->npts > 1) {
        XPoint *pts = malloc(sizeof *pts * (size_t)c->npts);
        if (pts) {
            for (int i = 0; i < c->npts; i++) { pts[i].x = c->px[i]; pts[i].y = c->py[i]; }
            XDrawLines(w2k.dpy, c->ov, w2k.gc, pts, c->npts, CoordModeOrigin);
            free(pts);
        }
    } else if (c->have_rect && c->x1 > c->x0 && c->y1 > c->y0) {
        /* The chosen part shows undimmed inside the ink. */
        XCopyArea(w2k.dpy, c->bright, c->ov, w2k.gc, c->x0, c->y0,
                  (unsigned)(c->x1 - c->x0), (unsigned)(c->y1 - c->y0), c->x0, c->y0);
        XDrawRectangle(w2k.dpy, c->ov, w2k.gc, c->x0, c->y0,
                       (unsigned)(c->x1 - c->x0), (unsigned)(c->y1 - c->y0));
    }
    XSetLineAttributes(w2k.dpy, w2k.gc, 0, LineSolid, CapButt, JoinMiter);
}

/* Put the overlay up and let the user pick; 1 and the snip in st.rgba. */
static int take_snip(void)
{
    Capture c;
    memset(&c, 0, sizeof c);
    c.sw = w2k.sw; c.sh = w2k.sh;
    XSync(w2k.dpy, False);

    if (st.type == SNIP_FULL) {
        /* "Full screen" is the screen the user is at -- the monitor under
         * the pointer -- not the whole framebuffer, which with several
         * monitors of different sizes is all of them side by side with
         * black where the short ones end. */
        const W2kMonitor *m = w2k_monitor_of_pointer();
        int x = m->x, y = m->y, w = m->w, h = m->h;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > c.sw) w = c.sw - x;
        if (y + h > c.sh) h = c.sh - y;
        if (w < 1 || h < 1) return 0;
        XImage *shot = XGetImage(w2k.dpy, w2k.root, x, y, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
        if (!shot) return 0;
        unsigned char *rgba = cut_out(shot, 0, 0, w, h);
        XDestroyImage(shot);
        if (!rgba) return 0;
        free(st.rgba);
        st.rgba = rgba;
        st.iw = w; st.ih = h;
        return 1;
    }

    c.shot = XGetImage(w2k.dpy, w2k.root, 0, 0, (unsigned)c.sw, (unsigned)c.sh, AllPlanes, ZPixmap);
    if (!c.shot) return 0;

    c.bright = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)c.sw, (unsigned)c.sh, w2k.depth);
    XPutImage(w2k.dpy, c.bright, w2k.gc, c.shot, 0, 0, 0, 0, (unsigned)c.sw, (unsigned)c.sh);
    c.dim = st.opt_overlay ? dimmed_screen(c.shot, c.sw, c.sh) : c.bright;

    XSetWindowAttributes a = { .override_redirect = True, .event_mask = ExposureMask |
                               ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask };
    c.ov = XCreateWindow(w2k.dpy, w2k.root, 0, 0, (unsigned)c.sw, (unsigned)c.sh, 0,
                         CopyFromParent, InputOutput, CopyFromParent,
                         CWOverrideRedirect | CWEventMask, &a);
    XMapRaised(w2k.dpy, c.ov);
    Cursor cross = XCreateFontCursor(w2k.dpy, XC_crosshair);
    XGrabPointer(w2k.dpy, c.ov, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, cross, CurrentTime);
    XGrabKeyboard(w2k.dpy, c.ov, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    /* Once more after the grabs: a manager that restacks on MapNotify has
     * had its say by now. */
    XRaiseWindow(w2k.dpy, c.ov);
    c.px = malloc(sizeof *c.px * MAXPTS);
    c.py = malloc(sizeof *c.py * MAXPTS);

    if (st.type == SNIP_WINDOW) {
        /* Outline whatever is under the pointer before it moves. */
        Window r, ch;
        int rx, ry, wx, wy;
        unsigned m;
        if (XQueryPointer(w2k.dpy, w2k.root, &r, &ch, &rx, &ry, &wx, &wy, &m))
            pick_window(&c, rx, ry);
    }
    capture_paint(&c);

    int done = 0, ok = 0;
    while (!done && c.px && c.py) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.window == c.ov && e.xexpose.count == 0) capture_paint(&c);
            break;
        case KeyPress:
            if (XLookupKeysym(&e.xkey, 0) == XK_Escape) done = 1;
            break;
        case MotionNotify: {
            /* Only the latest position matters; skip the backlog. */
            XEvent n;
            while (XCheckTypedWindowEvent(w2k.dpy, c.ov, MotionNotify, &n)) e = n;
            int mx = e.xmotion.x_root, my = e.xmotion.y_root;
            if (st.type == SNIP_WINDOW) {
                pick_window(&c, mx, my);
                capture_paint(&c);
            } else if (c.dragging) {
                if (st.type == SNIP_FREEFORM) {
                    if (c.npts < MAXPTS) { c.px[c.npts] = (short)mx; c.py[c.npts] = (short)my; c.npts++; }
                } else {
                    c.x0 = c.px[0] < mx ? c.px[0] : mx;  c.x1 = c.px[0] < mx ? mx : c.px[0];
                    c.y0 = c.py[0] < my ? c.py[0] : my;  c.y1 = c.py[0] < my ? my : c.py[0];
                    c.have_rect = 1;
                }
                capture_paint(&c);
            }
            break;
        }
        case ButtonPress:
            if (e.xbutton.button != Button1) { done = 1; break; }    /* right-click cancels */
            if (st.type == SNIP_WINDOW) {
                if (c.have_rect && c.x1 > c.x0 && c.y1 > c.y0) { ok = 1; done = 1; }
                break;
            }
            c.dragging = 1;
            c.npts = 1;
            c.px[0] = (short)e.xbutton.x_root; c.py[0] = (short)e.xbutton.y_root;
            c.x0 = c.x1 = e.xbutton.x_root; c.y0 = c.y1 = e.xbutton.y_root;
            c.have_rect = 0;
            break;
        case ButtonRelease:
            if (e.xbutton.button != Button1 || !c.dragging || st.type == SNIP_WINDOW) break;
            c.dragging = 0;
            if (st.type == SNIP_FREEFORM) {
                if (c.npts < 3) break;
                int minx = c.sw, miny = c.sh, maxx = 0, maxy = 0;
                for (int i = 0; i < c.npts; i++) {
                    if (c.px[i] < minx) minx = c.px[i];
                    if (c.px[i] > maxx) maxx = c.px[i];
                    if (c.py[i] < miny) miny = c.py[i];
                    if (c.py[i] > maxy) maxy = c.py[i];
                }
                if (minx < 0) minx = 0;
                if (miny < 0) miny = 0;
                if (maxx >= c.sw) maxx = c.sw - 1;
                if (maxy >= c.sh) maxy = c.sh - 1;
                c.x0 = minx; c.y0 = miny; c.x1 = maxx + 1; c.y1 = maxy + 1;
            }
            if (c.x1 - c.x0 >= 2 && c.y1 - c.y0 >= 2) ok = 1;
            done = 1;
            break;
        }
    }
    XUngrabKeyboard(w2k.dpy, CurrentTime);
    XUngrabPointer(w2k.dpy, CurrentTime);
    XFreeCursor(w2k.dpy, cross);
    XDestroyWindow(w2k.dpy, c.ov);
    if (c.dim != c.bright) w2k_free_pixmap(c.dim);
    w2k_free_pixmap(c.bright);
    XFlush(w2k.dpy);

    if (ok) {
        if (c.x0 < 0) c.x0 = 0;
        if (c.y0 < 0) c.y0 = 0;
        if (c.x1 > c.sw) c.x1 = c.sw;
        if (c.y1 > c.sh) c.y1 = c.sh;
        int w = c.x1 - c.x0, h = c.y1 - c.y0;
        unsigned char *rgba = w >= 2 && h >= 2 ? cut_out(c.shot, c.x0, c.y0, w, h) : NULL;
        if (rgba) {
            if (st.type == SNIP_FREEFORM) {
                /* Outside the outline is white, as in the original. */
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        if (!inside_path(&c, c.x0 + x, c.y0 + y)) {
                            unsigned char *o = rgba + ((size_t)y * w + x) * 4;
                            o[0] = o[1] = o[2] = 255;
                        }
            } else if (st.opt_show_ink)
                ink_border(rgba, w, h);
            free(st.rgba);
            st.rgba = rgba;
            st.iw = w; st.ih = h;
        } else ok = 0;
    }
    free(c.px);
    free(c.py);
    XDestroyImage(c.shot);
    return ok;
}

/* ------------------------------------------------------------------ *
 * The picture with its strokes
 * ------------------------------------------------------------------ */
static void strokes_free(void)
{
    for (int i = 0; i < st.nstrokes; i++) { free(st.strokes[i].x); free(st.strokes[i].y); }
    st.nstrokes = 0;
}

static Stroke *stroke_new(int type)
{
    if (st.nstrokes == st.strokes_cap) {
        st.strokes_cap = st.strokes_cap ? st.strokes_cap * 2 : 16;
        st.strokes = realloc(st.strokes, sizeof *st.strokes * (size_t)st.strokes_cap);
    }
    Stroke *s = &st.strokes[st.nstrokes++];
    memset(s, 0, sizeof *s);
    s->type = type;
    if (type == TOOL_HIGHLIGHT) { s->r = 255; s->g = 255; s->b = 0; }
    else { s->r = st.pen_r; s->g = st.pen_g; s->b = st.pen_b; }
    s->x = malloc(sizeof *s->x * MAXPTS);
    s->y = malloc(sizeof *s->y * MAXPTS);
    return s;
}

static void stroke_add(Stroke *s, int x, int y)
{
    if (!s || s->n >= MAXPTS) return;
    s->x[s->n] = (short)x; s->y[s->n] = (short)y; s->n++;
}

static void draw_stroke(Drawable d, const Stroke *s, int ox, int oy)
{
    if (s->n < 1) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(s->r, s->g, s->b));
    if (s->type == TOOL_HIGHLIGHT) {
        /* A translucent band: every other pixel of a fat line, which is
         * how a highlighter looked without alpha. */
        XSetLineAttributes(w2k.dpy, w2k.gc, 14, LineSolid, CapButt, JoinRound);
        XSetFillStyle(w2k.dpy, w2k.gc, FillStippled);
        XSetStipple(w2k.dpy, w2k.gc, w2k.pm_dither);
    } else
        XSetLineAttributes(w2k.dpy, w2k.gc, 2, LineSolid, CapRound, JoinRound);
    XPoint *pts = malloc(sizeof *pts * (size_t)(s->n > 1 ? s->n : 2));
    if (pts) {
        for (int i = 0; i < s->n; i++) { pts[i].x = (short)(s->x[i] + ox); pts[i].y = (short)(s->y[i] + oy); }
        if (s->n == 1) { pts[1] = pts[0]; pts[1].x++; }
        XDrawLines(w2k.dpy, d, w2k.gc, pts, s->n > 1 ? s->n : 2, CoordModeOrigin);
        free(pts);
    }
    XSetFillStyle(w2k.dpy, w2k.gc, FillSolid);
    XSetLineAttributes(w2k.dpy, w2k.gc, 0, LineSolid, CapButt, JoinMiter);
}

static void rebuild_pixmap(void)
{
    if (!st.rgba) return;
    if (!st.pm) st.pm = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)st.iw, (unsigned)st.ih, w2k.depth);
    char *pixels = malloc((size_t)st.iw * st.ih * 4);
    XImage *im = pixels ? XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap, 0, pixels,
                                       (unsigned)st.iw, (unsigned)st.ih, 32, 0) : NULL;
    if (!im) { free(pixels); return; }
    for (int y = 0; y < st.ih; y++)
        for (int x = 0; x < st.iw; x++) {
            const unsigned char *p = st.rgba + ((size_t)y * st.iw + x) * 4;
            XPutPixel(im, x, y, w2k_rgb(p[0], p[1], p[2]));
        }
    XPutImage(w2k.dpy, st.pm, w2k.gc, im, 0, 0, 0, 0, (unsigned)st.iw, (unsigned)st.ih);
    XDestroyImage(im);
    for (int i = 0; i < st.nstrokes; i++) draw_stroke(st.pm, &st.strokes[i], 0, 0);
    st.pm_dirty = 0;
}

/* The picture with the strokes burnt in, for saving and the clipboard. */
static unsigned char *flattened(void)
{
    if (!st.rgba) return NULL;
    if (st.pm_dirty || !st.pm) rebuild_pixmap();
    XImage *im = XGetImage(w2k.dpy, st.pm, 0, 0, (unsigned)st.iw, (unsigned)st.ih, AllPlanes, ZPixmap);
    if (!im) return NULL;
    unsigned char *out = cut_out(im, 0, 0, st.iw, st.ih);
    XDestroyImage(im);
    return out;
}

/* ------------------------------------------------------------------ *
 * The window: the strip, then the editor
 * ------------------------------------------------------------------ */
#define STRIP_W 340
#define STRIP_H 78
#define VIEW_TOP (MENUBAR_H + TOOLBAR_H)

static W2kRect view_rect(void)
{
    return (W2kRect){ 0, VIEW_TOP, st.win->w, st.win->h - VIEW_TOP };
}

static void layout_scroll(void)
{
    W2kRect v = view_rect();
    int need_v = st.ih > v.h - 4, need_h = st.iw > v.w - 4;
    if (need_v && st.iw > v.w - 4 - SCROLL_W) need_h = 1;
    if (need_h && st.ih > v.h - 4 - SCROLL_W) need_v = 1;
    int cw = v.w - 4 - (need_v ? SCROLL_W : 0), ch = v.h - 4 - (need_h ? SCROLL_W : 0);
    st.vsb.vertical = 1;
    st.vsb.r = (W2kRect){ v.x + v.w - 2 - SCROLL_W, v.y + 2, SCROLL_W, ch };
    st.vsb.total = need_v ? st.ih : 0; st.vsb.page = ch; st.vsb.line = 16;
    st.hsb.vertical = 0;
    st.hsb.r = (W2kRect){ v.x + 2, v.y + v.h - 2 - SCROLL_W, cw, SCROLL_W };
    st.hsb.total = need_h ? st.iw : 0; st.hsb.page = cw; st.hsb.line = 16;
    w2k_scroll_clamp(&st.vsb);
    w2k_scroll_clamp(&st.hsb);
}

static void draw_arrow(Drawable d, int x, int y, int disabled)
{
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[disabled ? C_GRAYTEXT : C_TEXT]);
    for (int i = 0; i < 3; i++)
        XFillRectangle(w2k.dpy, d, w2k.gc, x - 2 + i, y + i, (unsigned)(5 - 2 * i), 1);
}

/* The drop-down arrows: at the right of Mode and Delay, which carry text,
 * and in the middle of the narrow button beside the pen. */
static void paint_arrows(Drawable d)
{
    for (int i = 0; i < st.tb->n; i++) {
        int id = st.tb->b[i].id;
        int down = (i == st.tb->pressed && i == st.tb->hot);
        if (id == ID_MODE || id == ID_DELAY)
            draw_arrow(d, st.tb->b[i].x + st.tb->b[i].w - 9 + down, st.tb->r.y + 11 + down, st.tb->b[i].disabled);
        else if (id == ID_PENMENU)
            draw_arrow(d, st.tb->b[i].x + st.tb->b[i].w / 2 + down, st.tb->r.y + 11 + down, st.tb->b[i].disabled);
    }
}

/* The mark-up buttons' pictures: a pen in its colour, a highlighter, an
 * eraser, an envelope for Send. Drawn, since no shell icon is any of them. */
static void draw_tool_glyph(Drawable d, int id, int x, int y, int disabled)
{
    int cx = x + 12, cy = y + 12;
    if (id == ID_PEN) {
        XSetForeground(w2k.dpy, w2k.gc, disabled ? w2k.col[C_GRAYTEXT] : w2k_rgb(st.pen_r, st.pen_g, st.pen_b));
        XSetLineAttributes(w2k.dpy, w2k.gc, 3, LineSolid, CapRound, JoinRound);
        XDrawLine(w2k.dpy, d, w2k.gc, cx - 5, cy + 5, cx + 5, cy - 5);
        XSetLineAttributes(w2k.dpy, w2k.gc, 0, LineSolid, CapButt, JoinMiter);
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_TEXT]);
        XDrawLine(w2k.dpy, d, w2k.gc, cx - 7, cy + 7, cx - 5, cy + 5);
    } else if (id == ID_HIGHLIGHT) {
        XSetForeground(w2k.dpy, w2k.gc, disabled ? w2k.col[C_GRAYTEXT] : w2k_rgb(255, 255, 0));
        XFillRectangle(w2k.dpy, d, w2k.gc, cx - 7, cy - 2, 14, 6);
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_TEXT]);
        XDrawRectangle(w2k.dpy, d, w2k.gc, cx - 7, cy - 2, 14, 6);
        XDrawLine(w2k.dpy, d, w2k.gc, cx - 7, cy + 6, cx + 7, cy + 6);
    } else if (id == ID_ERASER) {
        XSetForeground(w2k.dpy, w2k.gc, disabled ? w2k.col[C_GRAYTEXT] : w2k_rgb(255, 170, 200));
        XPoint p[4] = { { (short)(cx - 6), (short)(cy + 2) }, { (short)(cx + 1), (short)(cy - 5) },
                        { (short)(cx + 6), (short)cy }, { (short)(cx - 1), (short)(cy + 7) } };
        XFillPolygon(w2k.dpy, d, w2k.gc, p, 4, Convex, CoordModeOrigin);
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_TEXT]);
        XDrawLines(w2k.dpy, d, w2k.gc, p, 4, CoordModeOrigin);
        XDrawLine(w2k.dpy, d, w2k.gc, p[3].x, p[3].y, p[0].x, p[0].y);
    } else if (id == ID_SEND) {
        XSetForeground(w2k.dpy, w2k.gc, disabled ? w2k.col[C_GRAYTEXT] : w2k.col[C_TEXT]);
        XDrawRectangle(w2k.dpy, d, w2k.gc, cx - 7, cy - 4, 14, 9);
        XDrawLine(w2k.dpy, d, w2k.gc, cx - 7, cy - 4, cx, cy + 1);
        XDrawLine(w2k.dpy, d, w2k.gc, cx + 7, cy - 4, cx, cy + 1);
    }
}

static void paint_tool_glyphs(Drawable d)
{
    for (int i = 0; i < st.tb->n; i++) {
        int id = st.tb->b[i].id;
        if (id == ID_PEN || id == ID_HIGHLIGHT || id == ID_ERASER || id == ID_SEND) {
            int down = (i == st.tb->pressed && i == st.tb->hot);
            draw_tool_glyph(d, id, st.tb->b[i].x + down, st.tb->r.y + 2 + down + (st.tb->r.h - 5 - 24) / 2,
                            st.tb->b[i].disabled);
        }
    }
}

static void paint(W2kWin *w, Drawable d)
{
    if (!st.editing) {
        st.tb->r = (W2kRect){ 0, 0, w->w, TOOLBAR_H + 2 };
        w2k_toolbar_draw(d, st.tb);
        paint_arrows(d);
        w2k_hline(d, 0, TOOLBAR_H + 2, w->w, C_SHADOW);
        w2k_hline(d, 0, TOOLBAR_H + 3, w->w, C_HILIGHT);
        const char *text = NULL;
        char buf[96];
        if (st.pending) {
            snprintf(buf, sizeof buf, "The snip will be taken in %d second%s. Click Cancel to stop.",
                     st.countdown, st.countdown == 1 ? "" : "s");
            text = buf;
        } else if (!st.opt_hide_text)
            text = "Select a snip type from the menu or click the New button.";
        if (text) {
            int fh = w2k_font_height(F_UI);
            w2k_icon_draw(d, 10, TOOLBAR_H + 14, ICO_SNIP);
            w2k_text(d, F_UI, 34, TOOLBAR_H + 14 + (16 - fh) / 2, text, C_TEXT);
        }
        return;
    }
    w2k_menubar_draw(d, st.mb);
    st.tb->r = (W2kRect){ 0, MENUBAR_H, w->w, TOOLBAR_H };
    w2k_toolbar_draw(d, st.tb);
    paint_arrows(d);
    paint_tool_glyphs(d);

    W2kRect v = view_rect();
    w2k_edge(d, v.x, v.y, v.w, v.h, EDGE_SUNKEN, BF_RECT);
    layout_scroll();
    int cw = st.hsb.page, ch = st.vsb.page;
    w2k_fill(d, v.x + 2, v.y + 2, cw, ch, C_WINDOW);
    if (st.rgba) {
        if (st.pm_dirty || !st.pm) rebuild_pixmap();
        int dw = st.iw < cw ? st.iw : cw, dh = st.ih < ch ? st.ih : ch;
        XCopyArea(w2k.dpy, st.pm, d, w2k_copy_gc(), st.hsb.pos, st.vsb.pos,
                  (unsigned)dw, (unsigned)dh, v.x + 2, v.y + 2);
        if (st.cur) draw_stroke(d, st.cur, v.x + 2 - st.hsb.pos, v.y + 2 - st.vsb.pos);
    }
    if (st.vsb.total) w2k_scroll_draw(d, &st.vsb);
    if (st.hsb.total) w2k_scroll_draw(d, &st.hsb);
    if (st.vsb.total && st.hsb.total)
        w2k_fill(d, st.vsb.r.x, st.hsb.r.y, SCROLL_W, SCROLL_W, C_FACE);
}

/* ------------------------------------------------------------------ *
 * Menus, options, commands
 * ------------------------------------------------------------------ */
static W2kMenu *mode_menu(void)
{
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_TYPE_FREE, "&Free-form Snip", NULL, ICO_NONE);
    w2k_menu_radio(m, st.type == SNIP_FREEFORM);
    w2k_menu_item(m, ID_TYPE_RECT, "&Rectangular Snip", NULL, ICO_NONE);
    w2k_menu_radio(m, st.type == SNIP_RECT);
    w2k_menu_item(m, ID_TYPE_WINDOW, "&Window Snip", NULL, ICO_NONE);
    w2k_menu_radio(m, st.type == SNIP_WINDOW);
    w2k_menu_item(m, ID_TYPE_FULL, "Full-&screen Snip", NULL, ICO_NONE);
    w2k_menu_radio(m, st.type == SNIP_FULL);
    return m;
}

static W2kMenu *delay_menu(void)
{
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_DELAY_0, "&No delay", NULL, ICO_NONE);
    w2k_menu_radio(m, st.delay == 0);
    for (int s = 1; s <= MAX_DELAY; s++) {
        char t[32];
        snprintf(t, sizeof t, "&%d second%s", s, s == 1 ? "" : "s");
        w2k_menu_item(m, ID_DELAY_0 + s, t, NULL, ICO_NONE);
        w2k_menu_radio(m, st.delay == s);
    }
    return m;
}

static W2kMenu *build_file(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_NEW, "&New Snip", "Ctrl+N", ICO_SNIP);
    w2k_menu_item(m, ID_SAVE, "Save &As...", "Ctrl+S", ICO_NONE);
    w2k_menu_item(m, ID_SEND, "Sen&d To", NULL, ICO_NONE);
    w2k_menu_disable(m);
    w2k_menu_item(m, ID_PRINT, "&Print...", "Ctrl+P", ICO_NONE);
    w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_EXIT, "E&xit", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_edit(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_COPY, "&Copy", "Ctrl+C", ICO_NONE);
    w2k_menu_item(m, ID_SELECTALL, "Select &All", "Ctrl+A", ICO_NONE);
    w2k_menu_disable(m);
    return m;
}

static W2kMenu *build_tools(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_PEN, "&Pen", NULL, ICO_NONE);
    w2k_menu_check(m, st.tool == TOOL_PEN);
    w2k_menu_item(m, ID_HIGHLIGHT, "&Highlighter", NULL, ICO_NONE);
    w2k_menu_check(m, st.tool == TOOL_HIGHLIGHT);
    w2k_menu_item(m, ID_ERASER, "&Eraser", NULL, ICO_NONE);
    w2k_menu_check(m, st.tool == TOOL_ERASER);
    w2k_menu_sep(m);
    w2k_menu_sub(m, "&Mode", ICO_NONE, mode_menu());
    w2k_menu_sub(m, "&Delay", ICO_NONE, delay_menu());
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_OPTIONS, "&Options...", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_help(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_HELP, "&Help Topics", "F1", ICO_HELP);
    w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_ABOUT, "&About Snipping Tool", NULL, ICO_NONE);
    return m;
}

/* The Options dialog: the original's check boxes and the ink colour. */
typedef struct { const char *label; int *on; int off; W2kRect r; } OptChk;
typedef struct {
    OptChk    chk[6];
    W2kCombo *ink;
    W2kRect   ok, cancel;
    int       down;
} OptDlg;

static void opt_paint(W2kWin *w, Drawable d)
{
    OptDlg *o = w->user;
    int fh = w2k_font_height(F_UI_BOLD);
    w2k_text(d, F_UI_BOLD, 12, 10, "Application", C_TEXT);
    for (int i = 0; i < 4; i++)
        w2k_draw_checkbox(d, o->chk[i].r.x, o->chk[i].r.y, o->chk[i].label,
                          *o->chk[i].on, 0, o->chk[i].off);
    /* The Selection group: its heading, then Ink color, then the two boxes. */
    w2k_text(d, F_UI_BOLD, 12, o->ink->r.y - fh - 8, "Selection", C_TEXT);
    w2k_text(d, F_UI, 28, o->ink->r.y + (21 - w2k_font_height(F_UI)) / 2, "Ink color:", C_TEXT);
    w2k_combo_draw(d, o->ink);
    for (int i = 4; i < 6; i++)
        w2k_draw_checkbox(d, o->chk[i].r.x, o->chk[i].r.y, o->chk[i].label,
                          *o->chk[i].on, 0, o->chk[i].off);
    w2k_draw_pushbutton(d, &o->ok, "OK", BS_DEFAULT | (o->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &o->cancel, "Cancel", o->down == 2 ? BS_PRESSED : 0);
}

static int opt_event(W2kWin *w, XEvent *e)
{
    OptDlg *o = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_combo_press(o->ink, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        for (int i = 0; i < 6; i++)
            if (!o->chk[i].off && w2k_rect_hit(&o->chk[i].r, x, y)) {
                *o->chk[i].on = !*o->chk[i].on;
                w2k_win_dirty(w);
                return 1;
            }
        if (w2k_rect_hit(&o->ok, x, y)) o->down = 1;
        else if (w2k_rect_hit(&o->cancel, x, y)) o->down = 2;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int x = e->xbutton.x, y = e->xbutton.y, b = o->down;
        o->down = 0;
        if (b == 1 && w2k_rect_hit(&o->ok, x, y)) w2k_win_close(w, ID_OK);
        else if (b == 2 && w2k_rect_hit(&o->cancel, x, y)) w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) w2k_win_close(w, ID_CANCEL);
        if (ks == XK_Return) w2k_win_close(w, ID_OK);
        return 1;
    }
    }
    return 0;
}

static void open_options(void)
{
    OptDlg o;
    memset(&o, 0, sizeof o);
    int hide = st.opt_hide_text, copy = st.opt_copy, prompt = st.opt_prompt_save;
    int overlay = st.opt_overlay, ink = st.opt_show_ink, url = 0;
    o.chk[0] = (OptChk){ "&Hide instruction text", &hide, 0, { 28, 32, 340, 18 } };
    o.chk[1] = (OptChk){ "&Always copy snips to the Clipboard", &copy, 0, { 28, 54, 340, 18 } };
    o.chk[2] = (OptChk){ "Include &URL below snips (HTML only)", &url, 1, { 28, 76, 340, 18 } };
    o.chk[3] = (OptChk){ "&Prompt to save snips before exiting", &prompt, 0, { 28, 98, 340, 18 } };
    o.chk[4] = (OptChk){ "&Show screen overlay when Snipping Tool is active", &overlay, 0, { 28, 184, 340, 18 } };
    o.chk[5] = (OptChk){ "Show selection &ink after snips are captured", &ink, 0, { 28, 206, 340, 18 } };
    W2kWin *w = w2k_win_new("Snipping Tool Options", "w2ksnip", 380, 270, 0);
    o.ink = w2k_combo_new(0);
    w2k_combo_add(o.ink, "Red");
    w2k_combo_add(o.ink, "Blue");
    w2k_combo_add(o.ink, "Black");
    w2k_combo_add(o.ink, "Custom");
    o.ink->sel = (st.ink_r == 255 && !st.ink_g) ? 0 : (st.ink_b == 255 && !st.ink_r) ? 1
               : (!st.ink_r && !st.ink_g && !st.ink_b) ? 2 : 3;
    o.ink->r = (W2kRect){ 100, 150, 120, 21 };      /* first row of Selection */
    o.ok = (W2kRect){ 380 - 12 - 75 * 2 - 6, 270 - 12 - 23, 75, 23 };
    o.cancel = (W2kRect){ 380 - 12 - 75, 270 - 12 - 23, 75, 23 };
    w->user = &o;
    w->paint = opt_paint;
    w->event = opt_event;
    w2k_win_center(w, st.win);
    if (w2k_win_modal(w) == ID_OK) {
        st.opt_hide_text = hide; st.opt_copy = copy; st.opt_prompt_save = prompt;
        st.opt_overlay = overlay; st.opt_show_ink = ink;
        if (o.ink->sel == 0) { st.ink_r = 255; st.ink_g = 0; st.ink_b = 0; }
        else if (o.ink->sel == 1) { st.ink_r = 0; st.ink_g = 0; st.ink_b = 255; }
        else if (o.ink->sel == 2) { st.ink_r = st.ink_g = st.ink_b = 0; }
        options_save();
    }
    w2k_combo_free(o.ink);
    w2k_win_dirty(st.win);
}

/* Enabled and checked states follow the countdown and the current tool. */
static void sync_toolbar(void)
{
    w2k_toolbar_enable(st.tb, ID_NEW, !st.pending);
    w2k_toolbar_enable(st.tb, ID_MODE, !st.pending);
    w2k_toolbar_enable(st.tb, ID_DELAY, !st.pending);
    w2k_toolbar_enable(st.tb, ID_CANCELSNIP, st.pending);
    for (int i = 0; i < st.tb->n; i++) {
        int id = st.tb->b[i].id;
        st.tb->b[i].checked = (id == ID_PEN && st.tool == TOOL_PEN) ||
                              (id == ID_HIGHLIGHT && st.tool == TOOL_HIGHLIGHT) ||
                              (id == ID_ERASER && st.tool == TOOL_ERASER);
    }
    w2k_win_dirty(st.win);
}

static void set_tool(int tool)
{
    st.tool = st.tool == tool ? TOOL_NONE : tool;
    sync_toolbar();
}

static void build_toolbar(void)
{
    if (st.tb) w2k_toolbar_free(st.tb);
    st.tb = w2k_toolbar_new(NULL, NULL);
    st.tb->show_text = 1;
    w2k_toolbar_add(st.tb, ID_NEW, ICO_SNIP, "New");
    w2k_toolbar_add(st.tb, ID_MODE, ICO_NONE, "Mode   ");     /* room for the arrow */
    w2k_toolbar_add(st.tb, ID_DELAY, ICO_NONE, "Delay   ");
    if (!st.editing) {
        w2k_toolbar_add(st.tb, ID_CANCELSNIP, ICO_NONE, "Cancel");
        w2k_toolbar_add(st.tb, ID_OPTIONS, ICO_NONE, "Options");
    } else {
        w2k_toolbar_sep(st.tb);
        w2k_toolbar_add(st.tb, ID_SAVE, ICO_DRIVE_FLOPPY, "");
        w2k_toolbar_add(st.tb, ID_COPY, ICO_COPY, "");
        w2k_toolbar_add(st.tb, ID_SEND, ICO_NONE, "");
        w2k_toolbar_enable(st.tb, ID_SEND, 0);
        w2k_toolbar_sep(st.tb);
        w2k_toolbar_add(st.tb, ID_PEN, ICO_NONE, "");
        w2k_toolbar_add(st.tb, ID_PENMENU, ICO_NONE, "");
        w2k_toolbar_add(st.tb, ID_HIGHLIGHT, ICO_NONE, "");
        w2k_toolbar_add(st.tb, ID_ERASER, ICO_NONE, "");
    }
    sync_toolbar();
}

static void set_title(void)
{
    char t[80];
    if (st.pending)
        snprintf(t, sizeof t, "Snipping Tool (snip in %d second%s)", st.countdown,
                 st.countdown == 1 ? "" : "s");
    else
        snprintf(t, sizeof t, "Snipping Tool");
    w2k_win_title(st.win, t);
}

/* How much frame the manager put around us, for growing in place. */
static void frame_extents(int *l, int *t, int *r, int *b)
{
    *l = *r = *b = W2K_FRAME_SIZE;
    *t = W2K_FRAME_SIZE + W2K_CAPTION_H;
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *d = NULL;
    if (XGetWindowProperty(w2k.dpy, st.win->win, w2k.a_net_frame_extents, 0, 4, False,
                           XA_CARDINAL, &type, &fmt, &n, &after, &d) == Success && d) {
        if (n == 4) {
            long *v = (long *)d;
            *l = (int)v[0]; *r = (int)v[1]; *t = (int)v[2]; *b = (int)v[3];
        }
        XFree(d);
    }
}

static void do_save(void);

/* Grow the strip into the editor, or the other way. The editor opens where
 * the strip was, sized to the snip, and pushed back onto the monitor if
 * that would run off it. */
static void become_editor(int yes)
{
    if (st.editing == yes) return;
    st.editing = yes;
    build_toolbar();
    XSizeHints sh;
    long sup;
    if (!XGetWMNormalHints(w2k.dpy, st.win->win, &sh, &sup)) memset(&sh, 0, sizeof sh);
    if (yes) {
        int l, t, r, b;
        frame_extents(&l, &t, &r, &b);
        int rx, ry;
        Window child;
        XTranslateCoordinates(w2k.dpy, st.win->win, w2k.root, 0, 0, &rx, &ry, &child);
        const W2kMonitor *m = w2k_monitor_at(rx + 8, ry + 8);
        int aw = m->w - 8, ah = m->h - 40;                  /* room for the taskbar */
        int w = st.iw + 4 + SCROLL_W + 8, h = VIEW_TOP + st.ih + 4 + SCROLL_W + 8;
        if (w + l + r > aw) w = aw - l - r;
        if (h + t + b > ah) h = ah - t - b;
        if (w < 400) w = 400;
        if (h < 300) h = 300;
        int fx = rx - l, fy = ry - t;
        if (fx + w + l + r > m->x + aw) fx = m->x + aw - w - l - r;
        if (fy + h + t + b > m->y + ah) fy = m->y + ah - h - t - b;
        if (fx < m->x) fx = m->x;
        if (fy < m->y) fy = m->y;
        st.win->resizable = 1;
        st.win->min_w = 300; st.win->min_h = 200;
        sh.flags = (sh.flags & ~(long)PMaxSize) | PMinSize | PPosition | USPosition;
        sh.min_width = 300; sh.min_height = 200;
        sh.x = fx; sh.y = fy;
        XSetWMNormalHints(w2k.dpy, st.win->win, &sh);
        XMoveResizeWindow(w2k.dpy, st.win->win, fx, fy, (unsigned)w, (unsigned)h);
    } else {
        st.win->resizable = 0;
        sh.flags |= PMinSize | PMaxSize;
        sh.min_width = sh.max_width = STRIP_W;
        sh.min_height = sh.max_height = STRIP_H;
        XSetWMNormalHints(w2k.dpy, st.win->win, &sh);
        XResizeWindow(w2k.dpy, st.win->win, STRIP_W, STRIP_H);
    }
    w2k_win_dirty(st.win);
}

static int confirm_discard(void)
{
    if (!st.rgba || st.saved || !st.opt_prompt_save) return 1;
    int r = w2k_msgbox(st.win, "Snipping Tool", "Do you want to save changes to the snip?",
                       MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == ID_CANCEL) return 0;
    if (r == ID_YES) {
        do_save();
        return st.saved;
    }
    return 1;
}

static void do_save(void)
{
    if (!st.rgba) return;
    char path[1024];
    snprintf(path, sizeof path, "%.1000s/Capture.png", st.last_dir);
    if (!w2k_file_dialog_filter(st.win, 1, path, sizeof path,
                                "Portable Network Graphic file (PNG) (*.png)|*.png|"
                                "JPEG file (*.jpg)|*.jpg;*.jpeg|Bitmap (*.bmp)|*.bmp"))
        return;
    unsigned char *flat = flattened();
    if (!flat) {
        w2k_msgbox(st.win, "Snipping Tool", "The snip could not be read back for saving.", MB_OK | MB_ICONERROR);
        return;
    }
    const char *base = strrchr(path, '/');
    const char *dot = strrchr(base ? base : path, '.');
    int ok;
    if (dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")))
        ok = w2k_jpeg_save(path, flat, st.iw, st.ih);
    else if (dot && !strcasecmp(dot, ".bmp"))
        ok = w2k_bmp_save(path, flat, st.iw, st.ih);
    else {
        if (!dot) strncat(path, ".png", sizeof path - strlen(path) - 1);
        ok = w2k_png_save(path, flat, st.iw, st.ih);
    }
    free(flat);
    if (!ok) w2k_msgbox(st.win, "Snipping Tool", "The snip could not be saved.", MB_OK | MB_ICONERROR);
    else {
        st.saved = 1;
        char *slash = strrchr(path, '/');
        if (slash) { *slash = 0; snprintf(st.last_dir, sizeof st.last_dir, "%s", path); }
    }
}

static void do_copy(void)
{
    unsigned char *flat = flattened();
    if (!flat) return;
    w2k_clipboard_set_image(flat, st.iw, st.ih);
    free(flat);
}

/* ------------------------------------------------------------------ *
 * New: now, or after the countdown
 * ------------------------------------------------------------------ */
static void capture_now(void)
{
    hide_tool();
    int ok = take_snip();
    /* Development aid: W2K_SNIP_DEBUG reports what was taken. */
    if (ok && getenv("W2K_SNIP_DEBUG")) fprintf(stderr, "w2ksnip: snip %dx%d\n", st.iw, st.ih);
    show_tool();
    if (!ok) { w2k_win_dirty(st.win); return; }
    strokes_free();
    if (st.pm) { w2k_free_pixmap(st.pm); st.pm = 0; }
    st.pm_dirty = 1;
    st.saved = 0;
    st.vsb.pos = st.hsb.pos = 0;
    become_editor(1);
    if (st.opt_copy) do_copy();
    w2k_win_dirty(st.win);
}

static void tick(void *u)
{
    (void)u;
    if (--st.countdown > 0) {
        set_title();
        w2k_win_dirty(st.win);
        return;
    }
    w2k_del_timer(tick, NULL);
    st.pending = 0;
    set_title();
    sync_toolbar();
    capture_now();
}

static void cancel_pending(void)
{
    if (!st.pending) return;
    w2k_del_timer(tick, NULL);
    st.pending = 0;
    set_title();
    sync_toolbar();
}

static void request_snip(void)
{
    if (st.pending) return;
    if (st.delay <= 0) { capture_now(); return; }
    st.pending = 1;
    st.countdown = st.delay;
    set_title();
    sync_toolbar();
    w2k_add_timer(1000, tick, NULL);
}

/* Drop a menu under a toolbar button; the chosen id, or 0. */
static int popup_under(int button_id, W2kMenu *m)
{
    int x = 0;
    for (int i = 0; i < st.tb->n; i++) if (st.tb->b[i].id == button_id) x = st.tb->b[i].x;
    int rx, ry;
    Window child;
    XTranslateCoordinates(w2k.dpy, st.win->win, w2k.root, x, st.tb->r.y + st.tb->r.h,
                          &rx, &ry, &child);
    int c = w2k_menu_popup(m, rx, ry, MPOP_LEFT);
    w2k_menu_free(m);
    return c;
}

static void command(void *u, int id)
{
    (void)u;
    if (id >= ID_DELAY_0 && id <= ID_DELAY_0 + MAX_DELAY) {
        st.delay = id - ID_DELAY_0;
        options_save();
        return;
    }
    switch (id) {
    case ID_NEW:      request_snip(); break;
    case ID_MODE: {
        int c = popup_under(ID_MODE, mode_menu());
        if (c) command(NULL, c);
        break;
    }
    case ID_DELAY: {
        int c = popup_under(ID_DELAY, delay_menu());
        if (c) command(NULL, c);
        break;
    }
    case ID_TYPE_FREE: case ID_TYPE_RECT: case ID_TYPE_WINDOW: case ID_TYPE_FULL:
        st.type = id - ID_TYPE_FREE;
        options_save();
        request_snip();
        break;
    case ID_CANCELSNIP: cancel_pending(); break;
    case ID_OPTIONS:  open_options(); break;
    case ID_SAVE:     do_save(); break;
    case ID_COPY:     do_copy(); break;
    case ID_PEN:      set_tool(TOOL_PEN); break;
    case ID_PENMENU: {
        W2kMenu *m = w2k_menu_new();
        w2k_menu_item(m, ID_PEN_RED, "&Red Pen", NULL, ICO_NONE);
        w2k_menu_radio(m, st.pen_r == 255 && !st.pen_g && !st.pen_b);
        w2k_menu_item(m, ID_PEN_BLUE, "&Blue Pen", NULL, ICO_NONE);
        w2k_menu_radio(m, st.pen_b == 255 && !st.pen_r && !st.pen_g);
        w2k_menu_item(m, ID_PEN_BLACK, "Blac&k Pen", NULL, ICO_NONE);
        w2k_menu_radio(m, !st.pen_r && !st.pen_g && !st.pen_b);
        int c = popup_under(ID_PEN, m);
        if (c) command(NULL, c);
        break;
    }
    case ID_PEN_RED:   st.pen_r = 255; st.pen_g = 0; st.pen_b = 0; st.tool = TOOL_NONE; set_tool(TOOL_PEN); options_save(); break;
    case ID_PEN_BLUE:  st.pen_r = 0; st.pen_g = 0; st.pen_b = 255; st.tool = TOOL_NONE; set_tool(TOOL_PEN); options_save(); break;
    case ID_PEN_BLACK: st.pen_r = st.pen_g = st.pen_b = 0; st.tool = TOOL_NONE; set_tool(TOOL_PEN); options_save(); break;
    case ID_HIGHLIGHT: set_tool(TOOL_HIGHLIGHT); break;
    case ID_ERASER:    set_tool(TOOL_ERASER); break;
    case ID_ABOUT:
        w2k_msgbox(st.win, "About Snipping Tool",
                   "Snipping Tool\n\nCapture a part of the screen, mark it up, "
                   "save it or copy it.", MB_OK | MB_ICONINFO);
        break;
    case ID_EXIT:
        cancel_pending();
        if (confirm_discard()) w2k_win_close(st.win, 0);
        break;
    }
}

/* ------------------------------------------------------------------ *
 * Events
 * ------------------------------------------------------------------ */
static int in_view(int x, int y, int *ix, int *iy)
{
    W2kRect v = view_rect();
    if (x < v.x + 2 || y < v.y + 2 || x >= v.x + 2 + st.hsb.page || y >= v.y + 2 + st.vsb.page)
        return 0;
    *ix = x - v.x - 2 + st.hsb.pos;
    *iy = y - v.y - 2 + st.vsb.pos;
    return 1;
}

static void erase_at(int x, int y)
{
    for (int i = st.nstrokes - 1; i >= 0; i--) {
        Stroke *s = &st.strokes[i];
        for (int k = 0; k < s->n; k++)
            if (abs(s->x[k] - x) <= 8 && abs(s->y[k] - y) <= 8) {
                free(s->x); free(s->y);
                memmove(s, s + 1, sizeof *s * (size_t)(st.nstrokes - 1 - i));
                st.nstrokes--;
                st.pm_dirty = 1;
                st.saved = 0;
                w2k_win_dirty(st.win);
                return;
            }
    }
}

static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (st.editing && w2k_menubar_press(st.mb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_toolbar_press(st.tb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (!st.editing) return 1;
        if (st.vsb.total && w2k_rect_hit(&st.vsb.r, x, y)) { w2k_scroll_press(&st.vsb, x, y); w2k_win_dirty(w); return 1; }
        if (st.hsb.total && w2k_rect_hit(&st.hsb.r, x, y)) { w2k_scroll_press(&st.hsb, x, y); w2k_win_dirty(w); return 1; }
        if (e->xbutton.button == Button4 || e->xbutton.button == Button5) {
            w2k_scroll_wheel(&st.vsb, e->xbutton.button == Button4 ? -1 : 1);
            w2k_win_dirty(w);
            return 1;
        }
        int ix, iy;
        if (e->xbutton.button == Button1 && st.rgba && in_view(x, y, &ix, &iy)) {
            if (st.tool == TOOL_ERASER) erase_at(ix, iy);
            else if (st.tool == TOOL_PEN || st.tool == TOOL_HIGHLIGHT) {
                st.cur = stroke_new(st.tool);
                stroke_add(st.cur, ix, iy);
                w2k_win_dirty(w);
            }
        }
        return 1;
    }
    case MotionNotify: {
        int x = e->xmotion.x, y = e->xmotion.y;
        if (w2k_toolbar_motion(st.tb, &e->xmotion)) w2k_win_dirty(w);
        if (!st.editing) return 1;
        if (st.vsb.pressed && w2k_scroll_motion(&st.vsb, x, y)) w2k_win_dirty(w);
        if (st.hsb.pressed && w2k_scroll_motion(&st.hsb, x, y)) w2k_win_dirty(w);
        if (st.cur) {
            W2kRect v = view_rect();
            stroke_add(st.cur, x - v.x - 2 + st.hsb.pos, y - v.y - 2 + st.vsb.pos);
            w2k_win_dirty(w);
        }
        return 1;
    }
    case ButtonRelease: {
        /* The toolbar reports its command on release. Find out which
         * button that was before the release is handed over, because a
         * command may rebuild the toolbar underneath it. */
        int hit = st.tb->pressed >= 0 && st.tb->pressed == st.tb->hot &&
                  !st.tb->b[st.tb->pressed].disabled ? st.tb->b[st.tb->pressed].id : 0;
        w2k_toolbar_release(st.tb);
        w2k_scroll_release(&st.vsb);
        w2k_scroll_release(&st.hsb);
        if (st.cur) { st.cur = NULL; st.pm_dirty = 1; st.saved = 0; }
        w2k_win_dirty(w);
        if (hit) command(NULL, hit);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        int ctrl = (e->xkey.state & ControlMask) != 0;
        if (ctrl && (ks == XK_n || ks == XK_N)) { command(NULL, ID_NEW); return 1; }
        if (ctrl && (ks == XK_s || ks == XK_S) && st.editing) { command(NULL, ID_SAVE); return 1; }
        if (ctrl && (ks == XK_c || ks == XK_C) && st.editing) { command(NULL, ID_COPY); return 1; }
        if (ks == XK_Escape) {
            if (st.pending) { cancel_pending(); return 1; }
            if (!st.editing) { w2k_win_close(w, 0); return 1; }
        }
        if (st.editing && w2k_menubar_key(st.mb, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void resized(W2kWin *w)
{
    st.mb->r = (W2kRect){ 0, 0, w->w, MENUBAR_H };
    layout_scroll();
}

static int closing(W2kWin *w)
{
    (void)w;
    cancel_pending();
    return confirm_discard();
}

int main(int argc, char **argv)
{
    if (w2k_init("w2ksnip") < 0) return 1;
    options_load();
    snprintf(st.last_dir, sizeof st.last_dir, "%s", getenv("HOME") ? getenv("HOME") : ".");

    st.win = w2k_win_new("Snipping Tool", "w2ksnip", STRIP_W, STRIP_H, 0);
    st.win->paint = paint;
    st.win->event = event;
    st.win->resized = resized;
    st.win->closing = closing;
    st.mb = w2k_menubar_new(NULL, command);
    w2k_menubar_add(st.mb, "&File", build_file);
    w2k_menubar_add(st.mb, "&Edit", build_edit);
    w2k_menubar_add(st.mb, "&Tools", build_tools);
    w2k_menubar_add(st.mb, "&Help", build_help);
    build_toolbar();
    w2k_scroll_bind(&st.vsb, st.win);
    w2k_scroll_bind(&st.hsb, st.win);
    /* Development aid: W2K_SNIP_DEMO=<picture> opens the editor on it. */
    const char *demo = getenv("W2K_SNIP_DEMO");
    if (demo) {
        st.rgba = w2k_image_load(demo, &st.iw, &st.ih);
        if (st.rgba) {
            st.pm_dirty = 1;
            become_editor(1);
            st.win->w = st.iw + 28 < 400 ? 400 : st.iw + 28;
            st.win->h = VIEW_TOP + st.ih + 28 < 300 ? 300 : VIEW_TOP + st.ih + 28;
            resized(st.win);
        }
    }
    w2k_win_center(st.win, NULL);
    /* "w2ksnip new" starts straight into a snip, as a shortcut may want:
     * the strip is not shown first, so it cannot be in the picture. */
    if (argc > 1 && !strcmp(argv[1], "new")) request_snip();
    if (!st.shown) { w2k_win_show(st.win); st.shown = 1; }
    int rc = w2k_run();
    w2k_fini();
    return rc;
}
