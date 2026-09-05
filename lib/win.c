/* win.c -- the top-level window framework, the simple drawn controls and
 * the common dialogs. */
#include "w2kui.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

static W2kWin *win_list;
void (*w2k_win_foreign_event)(XEvent *e);

/* ------------------------------------------------------------------ *
 * Timers
 * ------------------------------------------------------------------ */
static struct { int ms; void (*fn)(void *); void *user; long due; } timers[8];
static int ntimers;

void w2k_add_timer(int ms, void (*fn)(void *), void *user)
{
    /* The same timer added twice is one timer, re-armed. */
    for (int i = 0; i < ntimers; i++)
        if (timers[i].fn == fn && timers[i].user == user) {
            timers[i].ms = ms;
            timers[i].due = w2k_now_ms() + ms;
            return;
        }
    if (ntimers >= 8) return;
    timers[ntimers].ms = ms;
    timers[ntimers].fn = fn;
    timers[ntimers].user = user;
    timers[ntimers].due = w2k_now_ms() + ms;
    ntimers++;
}

void w2k_del_timer(void (*fn)(void *), void *user)
{
    for (int i = 0; i < ntimers; i++)
        if (timers[i].fn == fn && timers[i].user == user) {
            timers[i] = timers[--ntimers];
            return;
        }
}

static int timers_run(void)
{
    long now = w2k_now_ms();
    int wait = 1000;
    for (int i = 0; i < ntimers; i++) {
        if (now >= timers[i].due) {
            timers[i].due = now + timers[i].ms;
            timers[i].fn(timers[i].user);
        }
        int d = (int)(timers[i].due - now);
        if (d < wait) wait = d;
    }
    return wait < 5 ? 5 : wait;
}

/* ------------------------------------------------------------------ *
 * Windows
 * ------------------------------------------------------------------ */
W2kWin *w2k_win_new(const char *title, const char *cls, int w, int h,
                    int resizable)
{
    W2kWin *o = w2k_alloc(sizeof *o);
    o->w = w; o->h = h;
    o->pw = w2k_px(w); o->ph = w2k_px(h);
    o->alive = 1;
    o->dirty = 1;
    o->resizable = resizable;
    o->min_w = resizable ? 160 : w;
    o->min_h = resizable ? 100 : h;

    /* No server-side background: with one, every resize had the server
     * wipe the window to grey before the repaint at the next turn of the
     * loop, a flash on each step of a drag. The old picture is kept where
     * it was (bit gravity) and the repaint is done on the spot instead. */
    XSetWindowAttributes a = {
        .background_pixmap = None,
        .bit_gravity = NorthWestGravity,
        .event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                      ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | StructureNotifyMask |
                      FocusChangeMask
    };
    o->win = XCreateWindow(w2k.dpy, w2k.root, 0, 0, (unsigned)o->pw,
                           (unsigned)o->ph, 0, CopyFromParent,
                           InputOutput, CopyFromParent,
                           CWBackPixmap | CWBitGravity | CWEventMask, &a);
    /* Our own cursor, so the window never shows what the frame around it
     * last had -- a sizing arrow, after the pointer crossed the border. */
    if (w2k.cur_arrow) XDefineCursor(w2k.dpy, o->win, w2k.cur_arrow);

    XClassHint ch = { (char *)cls, (char *)"W2k" };
    XSetClassHint(w2k.dpy, o->win, &ch);
    w2k_set_wm_name(o->win, title);

    XSizeHints sh = { 0 };
    sh.flags = PMinSize;
    sh.min_width = w2k_px(o->min_w);
    sh.min_height = w2k_px(o->min_h);
    if (!resizable) {
        sh.flags |= PMaxSize;
        sh.max_width = o->pw;
        sh.max_height = o->ph;
    }
    XSetWMNormalHints(w2k.dpy, o->win, &sh);

    Atom protos[] = { w2k.a_wm_delete, w2k.a_wm_take_focus };
    XSetWMProtocols(w2k.dpy, o->win, protos, 2);

    o->next = win_list;
    win_list = o;
    return o;
}

void w2k_win_resize(W2kWin *w, int width, int height)
{
    if (!w || width <= 0 || height <= 0) return;
    w->w = width; w->h = height;
    w->pw = w2k_px(width); w->ph = w2k_px(height);
    if (w->buf) { w2k_free_pixmap(w->buf); w->buf = 0; }
    XResizeWindow(w2k.dpy, w->win, (unsigned)w->pw, (unsigned)w->ph);
    w->dirty = 1;
}

void w2k_win_title(W2kWin *w, const char *title)
{
    w2k_set_wm_name(w->win, title);
}

void w2k_win_center(W2kWin *w, W2kWin *over)
{
    /* With no parent to centre over, use the monitor under the pointer:
     * centring on the virtual screen lands the window on a bezel. */
    const W2kMonitor *m = w2k_monitor_of_pointer();
    int px = m->x, py = m->y, pw = m->w, ph = m->h;
    if (over) {
        Window child;
        int rx, ry;
        XTranslateCoordinates(w2k.dpy, over->win, w2k.root, 0, 0, &rx, &ry,
                              &child);
        px = rx; py = ry; pw = over->w; ph = over->h;
    }
    /* Centre the frame, not the client area: the window manager adds a
     * border and a caption above, so centring the client alone leaves the
     * window sitting low. The frame is what the eye sees. */
    int b = w2k_px(w->resizable ? W2K_FRAME_SIZE : W2K_FRAME_FIXED);
    int fw = w->pw + 2 * b, fh = w->ph + 2 * b + w2k_px(W2K_CAPTION_H);
    int x = px + (pw - fw) / 2, y = py + (ph - fh) / 2;
    const W2kMonitor *t = w2k_monitor_at(x + fw / 2, y + fh / 2);
    if (x < t->x) x = t->x;
    if (y < t->y) y = t->y;
    XMoveWindow(w2k.dpy, w->win, x, y);

    XSizeHints sh = { 0 };
    long sup;
    XGetWMNormalHints(w2k.dpy, w->win, &sh, &sup);
    /* USPosition as well as PPosition: this is where the window is to go,
     * not a hint, and the window manager (which may be this very process)
     * must not cascade it somewhere else. */
    sh.flags |= PPosition | USPosition;
    sh.x = x; sh.y = y;
    XSetWMNormalHints(w2k.dpy, w->win, &sh);
}

/* Development aid: with W2K_RENDER=<file.ppm> in the environment, the first
 * window an app shows is painted into an off-screen pixmap, written out as a
 * PPM and the app exits. It lets a dialog be inspected pixel by pixel on a
 * machine that is not running the desktop -- and without putting a window on
 * someone's screen to look at it. */
static void render_and_exit(W2kWin *w, const char *path)
{
    if (w->w <= 0 || w->h <= 0) exit(1);
    int pw = w2k_px(w->w), ph = w2k_px(w->h);
    Pixmap pm = XCreatePixmap(w2k.dpy, w->win, (unsigned)pw, (unsigned)ph, w2k.depth);
    w2k_fill(pm, 0, 0, w->w, w->h, C_FACE);
    if (w->paint) w->paint(w, pm);

    XImage *im = XGetImage(w2k.dpy, pm, 0, 0, (unsigned)pw, (unsigned)ph, AllPlanes, ZPixmap);
    FILE *f = fopen(path, "wb");
    if (f && im) {
        fprintf(f, "P6\n%d %d\n255\n", pw, ph);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                unsigned long p = XGetPixel(im, x, y);
                unsigned char rgb[3] = { (p >> 16) & 0xff, (p >> 8) & 0xff,
                                         p & 0xff };
                fwrite(rgb, 1, 3, f);
            }
    }
    if (f) fclose(f);
    if (im) XDestroyImage(im);
    exit(0);
}

/* A process that is itself the window manager has to be told about its
 * own windows: X sends MapRequest only to a *different* client than the
 * one asking, so the shell's dialogs -- Run, New Folder, the Start menu
 * settings -- mapped straight to the screen with no frame around them.
 * The window manager points this at client_manage(). */
void (*w2k_win_mapped)(Window w);

void w2k_win_show(W2kWin *w)
{
    const char *render = getenv("W2K_RENDER");
    if (render) render_and_exit(w, render);
    XMapWindow(w2k.dpy, w->win);
    if (w2k_win_mapped) w2k_win_mapped(w->win);
}
void w2k_win_dirty(W2kWin *w) { if (w) w->dirty = 1; }


void w2k_win_close(W2kWin *w, int result)
{
    w->result = result;
    w->alive = 0;
}

void w2k_win_destroy(W2kWin *w)
{
    W2kWin **p = &win_list;
    while (*p) { if (*p == w) { *p = w->next; break; } p = &(*p)->next; }
    if (w->buf) w2k_free_pixmap(w->buf);
    w2k_font_forget(w->win);
    XDestroyWindow(w2k.dpy, w->win);
    free(w);
}

static void repaint(W2kWin *w)
{
    if (!w->alive || w->w <= 0 || w->h <= 0) return;
    if (w->pw <= 0) { w->pw = w2k_px(w->w); w->ph = w2k_px(w->h); }
    if (!w->buf)
        w->buf = XCreatePixmap(w2k.dpy, w->win, (unsigned)w->pw,
                               (unsigned)w->ph, w2k.depth);
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_FACE]);
    XFillRectangle(w2k.dpy, w->buf, w2k.gc, 0, 0, (unsigned)w->pw,
                   (unsigned)w->ph);
    /* Programs paint in logical pixels whatever the caller's mode: the
     * window manager's own dialogs go through here too. */
    int raw = w2k_scale_raw;
    w2k_scale_raw = 0;
    if (w->paint) w->paint(w, w->buf);
    w2k_scale_raw = raw;
    XCopyArea(w2k.dpy, w->buf, w->win, w2k.gc, 0, 0, (unsigned)w->pw,
              (unsigned)w->ph, 0, 0);
    w->dirty = 0;
}
/* Repaint now rather than at the next turn of the loop -- for animations,
 * which are a sequence of frames inside one event. */
void w2k_win_repaint_now(W2kWin *w)
{
    if (!w || !w->alive) return;
    repaint(w);
    XFlush(w2k.dpy);
}

static W2kWin *win_for(Window x)
{
    for (W2kWin *w = win_list; w; w = w->next)
        if (w->win == x) return w;
    return NULL;
}

/* Is this one of our own windows? The window manager needs to know, because
 * it is a client of itself: its dialogs are managed like anybody else's. */
int w2k_win_owns(Window win)
{
    for (W2kWin *w = win_list; w; w = w->next)
        if (w->win == win) return 1;
    return 0;
}

static void dispatch_win(W2kWin *w, XEvent *e);

static void dispatch(XEvent *e)
{
    if (w2k_dnd_event(e)) return;          /* drag and drop protocol */

    /* Alt reveals the underlines under menu mnemonics; they stay revealed
     * until the window loses the keyboard, which is what Windows does. */
    if (e->type == KeyPress && !w2k_accel_shown) {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Alt_L || ks == XK_Alt_R || ks == XK_Meta_L ||
            ks == XK_Meta_R || (e->xkey.state & Mod1Mask)) {
            w2k_accel_show();
            for (W2kWin *w = win_list; w; w = w->next) w->dirty = 1;
        }
    } else if (e->type == FocusOut && w2k_effects[FX_HIDE_ACCEL] &&
               w2k_accel_shown) {
        w2k_accel_reset();
        for (W2kWin *w = win_list; w; w = w->next) w->dirty = 1;
    }
    if (e->type == SelectionRequest || e->type == SelectionClear) {
        w2k_clipboard_event(e);
        return;
    }
    if (e->type == PropertyNotify && e->xproperty.window == w2k.root &&
        e->xproperty.atom == w2k.a_w2k_scheme) {
        w2k_scheme_load(NULL);
        w2k_font_reload();          /* the smoothing setting lives there */
        w2k_accel_reset();
        for (W2kWin *w = win_list; w; w = w->next) {
            w->dirty = 1;
            w->dirty = 1;                 /* new colours: repaint from the buffer */
        }
        return;
    }
    W2kWin *w = win_for(e->xany.window);
    if (!w) {
        if (w2k_win_foreign_event) w2k_win_foreign_event(e);
        return;
    }
    /* Pointer positions arrive in the window's physical pixels; the
     * program's controls are laid out in logical ones. Root coordinates
     * are left alone: they name screen positions (menus, tooltips). */
    if (w2k_ui_scale != 100) {
        switch (e->type) {
        case ButtonPress: case ButtonRelease:
            e->xbutton.x = w2k_lp(e->xbutton.x);
            e->xbutton.y = w2k_lp(e->xbutton.y);
            break;
        case MotionNotify:
            e->xmotion.x = w2k_lp(e->xmotion.x);
            e->xmotion.y = w2k_lp(e->xmotion.y);
            break;
        case EnterNotify: case LeaveNotify:
            e->xcrossing.x = w2k_lp(e->xcrossing.x);
            e->xcrossing.y = w2k_lp(e->xcrossing.y);
            break;
        case KeyPress: case KeyRelease:
            e->xkey.x = w2k_lp(e->xkey.x);
            e->xkey.y = w2k_lp(e->xkey.y);
            break;
        }
    }
    int raw = w2k_scale_raw;
    w2k_scale_raw = 0;
    dispatch_win(w, e);
    w2k_scale_raw = raw;
}

static void dispatch_win(W2kWin *w, XEvent *e)
{

    /* The window manager's resize border is a few pixels wide, and the size
     * grip in the corner of a status bar is inside the client area, where
     * the manager never sees the click. Windows solves this by having the
     * application report the corner as a resize handle; the X equivalent is
     * to ask the manager to take over the drag, which is what this does. */
    if (e->type == ButtonPress && e->xbutton.button == Button1 && w->resizable &&
        e->xbutton.x >= w->w - 16 && e->xbutton.y >= w->h - 16) {
        XUngrabPointer(w2k.dpy, e->xbutton.time);
        XEvent m = { 0 };
        m.xclient.type = ClientMessage;
        m.xclient.window = w->win;
        m.xclient.message_type = w2k.a_net_wm_moveresize;
        m.xclient.format = 32;
        m.xclient.data.l[0] = e->xbutton.x_root;
        m.xclient.data.l[1] = e->xbutton.y_root;
        m.xclient.data.l[2] = 4;                 /* _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT */
        m.xclient.data.l[3] = Button1;
        m.xclient.data.l[4] = 1;                 /* the source is an application */
        XSendEvent(w2k.dpy, w2k.root, False,
                   SubstructureNotifyMask | SubstructureRedirectMask, &m);
        XFlush(w2k.dpy);
        return;
    }

    switch (e->type) {
    case Expose:
        if (e->xexpose.count == 0) w->dirty = 1;
        return;

    case ConfigureNotify: {
        /* A drag delivers a burst of these: only the newest size matters. */
        XEvent next;
        while (XCheckTypedWindowEvent(w2k.dpy, w->win, ConfigureNotify, &next))
            *e = next;
        if (e->xconfigure.width != w->pw || e->xconfigure.height != w->ph) {
            w->pw = e->xconfigure.width;
            w->ph = e->xconfigure.height;
            w->w = w2k_lp(w->pw);
            w->h = w2k_lp(w->ph);
            if (w->buf) { w2k_free_pixmap(w->buf); w->buf = 0; }
            if (w->resized) w->resized(w);
            /* Painted now, not at the next turn: nothing is shown between
             * the server's resize and the new picture. */
            repaint(w);
        }
        return;
    }

    case ClientMessage:
        if (e->xclient.message_type == w2k.a_wm_protocols) {
            if ((Atom)e->xclient.data.l[0] == w2k.a_wm_delete) {
                if (!w->closing || w->closing(w)) w2k_win_close(w, ID_CANCEL);
            } else if ((Atom)e->xclient.data.l[0] == w2k.a_wm_take_focus) {
                XSetInputFocus(w2k.dpy, w->win, RevertToParent,
                               e->xclient.data.l[1]);
            }
            return;
        }
        break;
    }
    if (w->event && w->event(w, e)) return;
}

static int any_alive(void)
{
    for (W2kWin *w = win_list; w; w = w->next)
        if (w->alive) return 1;
    return 0;
}

/* Set from a signal handler to unwind every loop this file runs: without
 * it a modal dialog swallows a termination request, because the modal
 * loop has no reason of its own to look at the program's quit flag. */
volatile sig_atomic_t w2k_win_abort;

static void pump(int *quit, W2kWin *until)
{
    int fd = ConnectionNumber(w2k.dpy);

    if (w2k_win_abort) {
        for (W2kWin *w = win_list; w; w = w->next) w->alive = 0;
        *quit = 1;
        return;
    }
    while (XPending(w2k.dpy)) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        /* Monitors can be plugged in or rearranged under a running app. */
        w2k_monitors_event(&e);
        dispatch(&e);
        if (until && !until->alive) { *quit = 1; return; }
        if (!until && !any_alive())  { *quit = 1; return; }
    }
    for (W2kWin *w = win_list; w; w = w->next)
        if (w->alive && w->dirty) repaint(w);

    int wait = timers_run();
    for (W2kWin *w = win_list; w; w = w->next)
        if (w->alive && w->dirty) repaint(w);
    XFlush(w2k.dpy);

    if (XPending(w2k.dpy)) return;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    struct timeval tv = { .tv_sec = wait / 1000, .tv_usec = (wait % 1000) * 1000 };
    select(fd + 1, &r, NULL, NULL, &tv);
}

int w2k_run(void)
{
    int quit = 0;
    while (!quit && any_alive()) pump(&quit, NULL);
    return 0;
}

int w2k_win_modal(W2kWin *dlg)
{
    w2k_win_show(dlg);
    int quit = 0;
    while (!quit && dlg->alive) pump(&quit, dlg);
    int r = dlg->result;
    w2k_win_destroy(dlg);
    return r;
}

/* ------------------------------------------------------------------ *
 * Simple controls
 * ------------------------------------------------------------------ */
int w2k_rect_hit(const W2kRect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

void w2k_draw_pushbutton(Drawable d, const W2kRect *r, const char *text,
                         int state)
{
    int x = r->x, y = r->y, w = r->w, h = r->h;

    if (state & BS_DEFAULT) {
        /* The default button wears an extra hard black ring. */
        w2k_frame(d, x, y, w, h, C_WINDOWFRAME);
        x++; y++; w -= 2; h -= 2;
    }
    int pressed = (state & BS_PRESSED) != 0;
    w2k_button(d, x, y, w, h, pressed);

    if (text && *text) {
        int tw = w2k_mnemonic_width(F_UI, text);
        int tx = x + (w - tw) / 2 + pressed;
        int ty = y + (h - w2k_font_height(F_UI)) / 2 + pressed;
        if (state & BS_DISABLED) {
            w2k_text_mnemonic(d, F_UI, tx + 1, ty + 1, text, C_HILIGHT, 1);
            w2k_text_mnemonic(d, F_UI, tx, ty, text, C_GRAYTEXT, 1);
        } else {
            w2k_text_mnemonic(d, F_UI, tx, ty, text, C_TEXT, 1);
        }
    }
    if (state & BS_FOCUS)
        w2k_focus_rect(d, x + 3, y + 3, w - 6, h - 6);
}

/* The 13x13 check box, drawn exactly as USER32 does. */
static void check_glyph(Drawable d, int x, int y, int checked, int disabled)
{
    w2k_edge(d, x, y, 13, 13, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, x + 2, y + 2, 9, 9, disabled ? C_FACE : C_WINDOW);
    if (!checked) return;

    /* USER32's check mark: two strokes, two pixels thick, occupying a
     * 7x6 box inside the 9x9 well. Drawn as a literal bitmap because
     * that is what it is -- there is no curve to compute. */
    static const char *const tick[6] = {
        "......#",
        ".....##",
        "#...##.",
        "##.##..",
        ".###...",
        "..#....",
    };
    XSetForeground(w2k.dpy, w2k.gc,
                   w2k.col[disabled ? C_GRAYTEXT : C_WINDOWTEXT]);
    for (int r = 0; r < 6; r++)
        for (int c = 0; tick[r][c]; c++)
            if (tick[r][c] == '#')
                w2k_fill_fg(d, x + 3 + c, y + 3 + r, 1, 1);
}

void w2k_draw_checkbox(Drawable d, int x, int y, const char *text,
                       int checked, int focused, int disabled)
{
    int fh = w2k_font_height(F_UI);
    check_glyph(d, x, y + (fh > 13 ? (fh - 13) / 2 : 0), checked, disabled);
    int tx = x + 13 + 5, ty = y + (13 > fh ? (13 - fh) / 2 : 0);
    if (disabled) {
        w2k_text_mnemonic(d, F_UI, tx + 1, ty + 1, text, C_HILIGHT, 1);
        w2k_text_mnemonic(d, F_UI, tx, ty, text, C_GRAYTEXT, 1);
    } else {
        w2k_text_mnemonic(d, F_UI, tx, ty, text, C_TEXT, 1);
    }
    if (focused)
        w2k_focus_rect(d, tx - 2, ty - 1, w2k_mnemonic_width(F_UI, text) + 4,
                       fh + 2);
}

/* The 12x12 radio button: a dark arc above-left, a light arc below-right. */
static void radio_glyph(Drawable d, int x, int y, int checked, int disabled)
{
    const double cx = 5.5, cy = 5.5;
    for (int j = 0; j < 12; j++) {
        for (int i = 0; i < 12; i++) {
            double dx = i - cx, dy = j - cy;
            double r2 = dx * dx + dy * dy;
            int col = -1;
            if (r2 <= 16.0)       col = disabled ? C_FACE : C_WINDOW;
            else if (r2 <= 25.0)  col = (dx + dy < 0) ? C_DKSHADOW : C_LIGHT;
            else if (r2 <= 36.0)  col = (dx + dy < 0) ? C_SHADOW : C_HILIGHT;
            if (col >= 0) {
                XSetForeground(w2k.dpy, w2k.gc, w2k.col[col]);
                w2k_fill_fg(d, x + i, y + j, 1, 1);
            }
        }
    }
    if (!checked) return;
    XSetForeground(w2k.dpy, w2k.gc,
                   w2k.col[disabled ? C_GRAYTEXT : C_WINDOWTEXT]);
    for (int j = 0; j < 12; j++)
        for (int i = 0; i < 12; i++) {
            double dx = i - cx, dy = j - cy;
            if (dx * dx + dy * dy <= 4.0)
                w2k_fill_fg(d, x + i, y + j, 1, 1);
        }
}

void w2k_draw_radio(Drawable d, int x, int y, const char *text, int checked,
                    int focused, int disabled)
{
    int fh = w2k_font_height(F_UI);
    radio_glyph(d, x, y + (fh > 12 ? (fh - 12) / 2 : 0), checked, disabled);
    int tx = x + 12 + 5, ty = y + (12 > fh ? (12 - fh) / 2 : 0);
    if (disabled) {
        w2k_text_mnemonic(d, F_UI, tx + 1, ty + 1, text, C_HILIGHT, 1);
        w2k_text_mnemonic(d, F_UI, tx, ty, text, C_GRAYTEXT, 1);
    } else {
        w2k_text_mnemonic(d, F_UI, tx, ty, text, C_TEXT, 1);
    }
    if (focused)
        w2k_focus_rect(d, tx - 2, ty - 1, w2k_mnemonic_width(F_UI, text) + 4,
                       fh + 2);
}

void w2k_draw_groupbox(Drawable d, const W2kRect *r, const char *text)
{
    int fh = w2k_font_height(F_UI);
    int top = r->y + fh / 2;
    w2k_edge(d, r->x, top, r->w, r->h - (top - r->y), EDGE_ETCHED, BF_RECT);
    if (text && *text) {
        int tw = w2k_mnemonic_width(F_UI, text);
        w2k_fill(d, r->x + 8, top - fh / 2, tw + 6, fh, C_FACE);
        w2k_text_mnemonic(d, F_UI, r->x + 11, top - fh / 2, text, C_TEXT, 1);
    }
}

void w2k_draw_well(Drawable d, const W2kRect *r)
{
    w2k_edge(d, r->x, r->y, r->w, r->h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, r->x + 2, r->y + 2, r->w - 4, r->h - 4, C_WINDOW);
}

/* ------------------------------------------------------------------ *
 * Message box
 * ------------------------------------------------------------------ */
#define MB_MAXLINES 12

typedef struct {
    char        *lines[MB_MAXLINES];
    int          nlines;
    int          icon;
    W2kRect      btn[3];
    const char  *label[3];
    int          ids[3];
    int          nbtn, focus, down;
    W2kWin      *w;
} MsgBox;

/* Greedy word wrap. Returns the widest resulting line. */
static int wrap_text(const char *text, int maxw, char **out, int maxlines,
                     int *nlines)
{
    int n = 0, widest = 0;
    const char *p = text;
    while (*p && n < maxlines) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        const char *cut = end;
        if (w2k_text_width(F_UI, p, (int)(end - p)) > maxw) {
            cut = p;
            for (const char *s = p; s < end; s++) {
                if (*s != ' ') continue;
                if (w2k_text_width(F_UI, p, (int)(s - p)) <= maxw) cut = s;
                else break;
            }
            if (cut == p) {              /* one very long word: hard break */
                cut = p;
                while (cut < end &&
                       w2k_text_width(F_UI, p, (int)(cut - p + 1)) <= maxw)
                    cut++;
            }
        }
        int len = (int)(cut - p);
        char *line = w2k_alloc(len + 1);
        memcpy(line, p, len);
        line[len] = 0;
        int lw = w2k_text_width(F_UI, line, len);
        if (lw > widest) widest = lw;
        out[n++] = line;
        p = cut;
        while (*p == ' ') p++;
        if (nl && p <= nl) p = nl + 1;
    }
    *nlines = n;
    return widest;
}

static void msgbox_paint(W2kWin *w, Drawable d)
{
    MsgBox *m = w->user;
    int fh = w2k_font_height(F_UI);
    int tx = 18, ty = 18;
    if (m->icon >= 0) {
        w2k_bigicon_draw(d, 18, 18, m->icon);
        tx = 18 + 32 + 14;
    }
    for (int i = 0; i < m->nlines; i++)
        w2k_text(d, F_UI, tx, ty + i * (fh + 2), m->lines[i], C_TEXT);

    for (int i = 0; i < m->nbtn; i++) {
        int st = 0;
        if (i == m->focus) st |= BS_FOCUS | BS_DEFAULT;
        if (i == m->down)  st |= BS_PRESSED;
        w2k_draw_pushbutton(d, &m->btn[i], m->label[i], st);
    }
}

static int msgbox_event(W2kWin *w, XEvent *e)
{
    MsgBox *m = w->user;
    if (e->type == ButtonPress && e->xbutton.button == Button1) {
        for (int i = 0; i < m->nbtn; i++)
            if (w2k_rect_hit(&m->btn[i], e->xbutton.x, e->xbutton.y)) {
                m->down = i;
                m->focus = i;
                w2k_win_dirty(w);
                return 1;
            }
    } else if (e->type == ButtonRelease) {
        if (m->down >= 0) {
            int i = m->down;
            m->down = -1;
            if (w2k_rect_hit(&m->btn[i], e->xbutton.x, e->xbutton.y))
                w2k_win_close(w, m->ids[i]);
            w2k_win_dirty(w);
            return 1;
        }
    } else if (e->type == KeyPress) {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) {
            /* Escape maps to Cancel, or No, or the sole OK. */
            for (int i = 0; i < m->nbtn; i++)
                if (m->ids[i] == ID_CANCEL) { w2k_win_close(w, ID_CANCEL); return 1; }
            w2k_win_close(w, m->ids[m->nbtn - 1]);
            return 1;
        }
        if (ks == XK_Return || ks == XK_KP_Enter || ks == XK_space) {
            w2k_win_close(w, m->ids[m->focus]);
            return 1;
        }
        if (ks == XK_Tab || ks == XK_Right || ks == XK_Left) {
            int dir = (ks == XK_Left || (e->xkey.state & ShiftMask)) ? -1 : 1;
            m->focus = (m->focus + dir + m->nbtn) % m->nbtn;
            w2k_win_dirty(w);
            return 1;
        }
    }
    return 0;
}

int w2k_msgbox(W2kWin *over, const char *title, const char *text, int flags)
{
    MsgBox m = { .focus = 0, .down = -1, .icon = ICO_NONE };

    switch (flags & 0xf0) {
    case MB_ICONINFO:     m.icon = ICO_INFO; w2k_sound_play(SND_ASTERISK); break;
    case MB_ICONWARNING:  m.icon = ICO_WARNING; w2k_sound_play(SND_EXCLAMATION); break;
    case MB_ICONQUESTION: m.icon = ICO_QUESTION; w2k_sound_play(SND_QUESTION); break;
    case MB_ICONERROR:    m.icon = ICO_ERROR; w2k_sound_play(SND_HAND); break;
    default:              w2k_sound_play(SND_DEFAULT); break;
    }
    switch (flags & 0x0f) {
    case MB_OKCANCEL:
        m.nbtn = 2;
        m.label[0] = "OK";     m.ids[0] = ID_OK;
        m.label[1] = "Cancel"; m.ids[1] = ID_CANCEL;
        break;
    case MB_YESNO:
        m.nbtn = 2;
        m.label[0] = "&Yes"; m.ids[0] = ID_YES;
        m.label[1] = "&No";  m.ids[1] = ID_NO;
        break;
    case MB_YESNOCANCEL:
        m.nbtn = 3;
        m.label[0] = "&Yes";    m.ids[0] = ID_YES;
        m.label[1] = "&No";     m.ids[1] = ID_NO;
        m.label[2] = "Cancel";  m.ids[2] = ID_CANCEL;
        break;
    default:
        m.nbtn = 1;
        m.label[0] = "OK"; m.ids[0] = ID_OK;
        break;
    }

    int textw = wrap_text(text, 340, m.lines, MB_MAXLINES, &m.nlines);
    int fh = w2k_font_height(F_UI);
    int left = (m.icon >= 0) ? 18 + 32 + 14 : 18;
    int cw = left + textw + 18;
    int bw = 75, bh = 23, gap = 6;
    int btnw = m.nbtn * bw + (m.nbtn - 1) * gap;
    if (cw < btnw + 36) cw = btnw + 36;
    if (cw < 220) cw = 220;
    int texth = m.nlines * (fh + 2);
    if (m.icon >= 0 && texth < 32) texth = 32;
    int chh = 18 + texth + 16 + bh + 14;

    W2kWin *w = w2k_win_new(title, "w2kdialog", cw, chh, 0);
    w->user = &m;
    w->paint = msgbox_paint;
    w->event = msgbox_event;
    m.w = w;

    int bx = (cw - btnw) / 2, by = chh - 14 - bh;
    for (int i = 0; i < m.nbtn; i++) {
        m.btn[i].x = bx + i * (bw + gap);
        m.btn[i].y = by;
        m.btn[i].w = bw;
        m.btn[i].h = bh;
    }
    w2k_win_center(w, over);

    /* Announce ourselves as a dialog so the WM gives us a fixed frame. */
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);

    int r = w2k_win_modal(w);
    for (int i = 0; i < m.nlines; i++) free(m.lines[i]);
    return r;
}
