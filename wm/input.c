/* input.c -- moving, sizing, the system menu, Alt+Tab and global hotkeys. */
#include "wm.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* System-menu command ids. */
enum { SC_RESTORE = 1, SC_MOVE, SC_SIZE, SC_MINIMIZE, SC_MAXIMIZE, SC_CLOSE };

/* ------------------------------------------------------------------ *
 * Interactive move / size
 *
 * Windows 2000 ships with "show window contents while dragging" on, so we
 * track live rather than rubber-banding an outline.
 * ------------------------------------------------------------------ */
/* Dropping a window against an edge of its monitor snaps it there, the way
 * Windows has done since 7: the top fills the screen, the sides take half. */
#define SNAP_EDGE 8          /* how close to the edge counts */
enum { SNAP_NONE, SNAP_MAX, SNAP_LEFT, SNAP_RIGHT, SNAP_BOTTOM };

typedef struct {
    int mode;            /* 0 = move, otherwise the HT_* edge being dragged */
    int gx, gy;          /* pointer position when the drag began            */
    int ox, oy, ow, oh;  /* client rect when the drag began                 */
    int snap;            /* zone the pointer is currently in                */
} Drag;

/* Which snap zone is the pointer in, if any? Measured against the monitor
 * the pointer is on, so it works the same on every screen. */
static int snap_zone_at(int px, int py)
{
    const W2kMonitor *m = w2k_monitor_at(px, py);
    int wx, wy, ww, wh;
    wm_workarea_of(m, &wx, &wy, &ww, &wh);
    if (py <= m->y + SNAP_EDGE)               return SNAP_MAX;
    if (px <= m->x + SNAP_EDGE)               return SNAP_LEFT;
    if (px >= m->x + m->w - 1 - SNAP_EDGE)    return SNAP_RIGHT;
    if (py >= wy + wh - 1 - SNAP_EDGE)        return SNAP_BOTTOM;
    (void)ww;
    return SNAP_NONE;
}

/* Put the window where the zone says. */
static void snap_apply(Client *c, int zone, int px, int py)
{
    if (zone == SNAP_NONE) return;
    if (zone == SNAP_MAX) { client_maximize(c, 1); return; }

    int wx, wy, ww, wh;
    wm_workarea_of(w2k_monitor_at(px, py), &wx, &wy, &ww, &wh);
    int b = client_border(c), cap = client_caption_h(c);
    int x = wx, y = wy, w = ww, h = wh;
    if (zone == SNAP_LEFT)        { w = ww / 2; }
    else if (zone == SNAP_RIGHT)  { w = ww / 2; x = wx + ww - w; }
    else if (zone == SNAP_BOTTOM) { h = wh / 2; y = wy + wh - h; }

    /* Remember where it was, so Restore puts it back. */
    if (!c->maximized) { c->rx = c->x; c->ry = c->y; c->rw = c->w; c->rh = c->h; }
    client_move_resize(c, x + b, y + b + cap, w - 2 * b, h - 2 * b - cap);
}

/* Where this drag puts the client rectangle, without applying it: the live
 * drag and the wire frame need the same arithmetic. */
static void drag_rect(Client *c, Drag *d, int px, int py,
                      int *out_x, int *out_y, int *out_w, int *out_h)
{
    int dx = px - d->gx, dy = py - d->gy;

    if (d->mode == 0) {
        int nx = d->ox + dx, ny = d->oy + dy;
        /* Never let the caption be dragged above the top of the screen. */
        int cap = client_caption_h(c), b = client_border(c);
        if (ny - cap - b < 0) ny = cap + b;
        *out_x = nx; *out_y = ny; *out_w = c->w; *out_h = c->h;
        return;
    }

    int x = d->ox, y = d->oy, w = d->ow, h = d->oh;
    switch (d->mode) {
    case HT_LEFT:        x += dx; w -= dx; break;
    case HT_RIGHT:       w += dx; break;
    case HT_TOP:         y += dy; h -= dy; break;
    case HT_BOTTOM:      h += dy; break;
    case HT_TOPLEFT:     x += dx; w -= dx; y += dy; h -= dy; break;
    case HT_TOPRIGHT:    w += dx;          y += dy; h -= dy; break;
    case HT_BOTTOMLEFT:  x += dx; w -= dx; h += dy; break;
    case HT_BOTTOMRIGHT: w += dx;          h += dy; break;
    }
    int cw = w, ch = h;
    client_constrain(c, &cw, &ch);
    /* Constraining must not move the anchored edge. */
    if (d->mode == HT_LEFT || d->mode == HT_TOPLEFT || d->mode == HT_BOTTOMLEFT)
        x += w - cw;
    if (d->mode == HT_TOP || d->mode == HT_TOPLEFT || d->mode == HT_TOPRIGHT)
        y += h - ch;
    *out_x = x; *out_y = y; *out_w = cw; *out_h = ch;
}

static void drag_apply(Client *c, Drag *d, int px, int py)
{
    int x, y, w, h;
    drag_rect(c, d, px, py, &x, &y, &w, &h);
    client_move_resize(c, x, y, w, h);
    if (d->mode == 0) d->snap = snap_zone_at(px, py);
}

/* The same, as the outer frame rectangle, for the wire frame. */
static void drag_geometry(Client *c, Drag *d, int px, int py,
                          int *fx, int *fy, int *fw, int *fh)
{
    int x, y, w, h;
    drag_rect(c, d, px, py, &x, &y, &w, &h);
    int b = client_border(c), cap = client_caption_h(c);
    *fx = x - b;
    *fy = y - b - cap;
    *fw = w + 2 * b;
    *fh = h + 2 * b + cap;
}

/* Shared drag loop. `keyboard` enters the menu-driven variant, where the
 * arrow keys nudge and Enter/Escape finish. */
/* ------------------------------------------------------------------ *
 * The drag outline
 * ------------------------------------------------------------------ *
 * With "show window contents while dragging" turned off, a drag moves an
 * XOR-drawn wire frame on the root and the window jumps to it on release --
 * which is what Windows did before the machines were fast enough to move
 * the real thing. Drawing the same rectangle twice erases it, so no repaint
 * of what is underneath is needed. */
static GC outline_gc(void)
{
    static GC gc;
    if (gc) return gc;
    XGCValues gv;
    gv.function = GXxor;
    gv.foreground = w2k.col[C_WHITE] ^ w2k.col[C_DESKTOP];
    gv.subwindow_mode = IncludeInferiors;
    gv.line_width = 3;
    gv.graphics_exposures = False;
    gc = XCreateGC(w2k.dpy, w2k.root,
                   GCFunction | GCForeground | GCSubwindowMode | GCLineWidth |
                   GCGraphicsExposures, &gv);
    return gc;
}

static void outline_draw(int x, int y, int w, int h)
{
    if (w < 3 || h < 3) return;
    XDrawRectangle(w2k.dpy, w2k.root, outline_gc(), x + 1, y + 1, w - 3, h - 3);
}

/* Zoom a wire frame from one rectangle to another -- the animation Windows
 * plays when a window minimises to its taskbar button, and back. Each step
 * is drawn and erased, so nothing underneath needs repainting. */
void wm_animate_rect(int fx, int fy, int fw, int fh,
                     int tx, int ty, int tw, int th)
{
    if (!w2k_effects[FX_ANIM_MINMAX]) return;
    if (fw < 4 || fh < 4 || tw < 4 || th < 4) return;

    XGrabServer(w2k.dpy);
    for (int i = 1; i <= 7; i++) {
        int x = fx + (tx - fx) * i / 8;
        int y = fy + (ty - fy) * i / 8;
        int w = fw + (tw - fw) * i / 8;
        int h = fh + (th - fh) * i / 8;
        outline_draw(x, y, w, h);
        XFlush(w2k.dpy);
        usleep(12000);
        outline_draw(x, y, w, h);          /* drawn twice = erased */
    }
    XUngrabServer(w2k.dpy);
    XFlush(w2k.dpy);
}

static void drag_loop(Client *c, int mode, int px, int py, int keyboard)
{
    Drag d = { mode, px, py, c->x, c->y, c->w, c->h, SNAP_NONE };
    Cursor cur = mode ? frame_cursor(mode) : w2k.cur_move;

    if (XGrabPointer(w2k.dpy, w2k.root, False,
                     ButtonReleaseMask | ButtonPressMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, cur,
                     CurrentTime) != GrabSuccess)
        return;
    if (keyboard)
        XGrabKeyboard(w2k.dpy, w2k.root, False, GrabModeAsync, GrabModeAsync,
                      CurrentTime);

    /* Frame geometry, for the outline: it traces the whole window, not the
     * client area inside it. */
    int b = client_border(c), cap = client_caption_h(c);
    int outline = !w2k_effects[FX_DRAG_CONTENTS];
    int ox = c->x - b, oy = c->y - b - cap;
    int ow = client_frame_w(c), oh = client_frame_h(c);
    if (outline) {
        XGrabServer(w2k.dpy);
        outline_draw(ox, oy, ow, oh);
    }

    int done = 0, gone = 0;
    Window cw = c->win;
    long last = 0;
    while (!done && running) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        switch (e.type) {
        case MotionNotify: {
            /* Coalesce: only the newest motion matters. */
            while (XCheckTypedEvent(w2k.dpy, MotionNotify, &e)) ;
            long now = w2k_now_ms();
            if (now - last < 8) break;        /* ~120 Hz cap */
            last = now;
            if (!outline) {
                drag_apply(c, &d, e.xmotion.x_root, e.xmotion.y_root);
                break;
            }
            /* Wire frame: work out where the window would land, and move
             * the rectangle there without touching the window. */
            outline_draw(ox, oy, ow, oh);
            drag_geometry(c, &d, e.xmotion.x_root, e.xmotion.y_root,
                          &ox, &oy, &ow, &oh);
            outline_draw(ox, oy, ow, oh);
            d.snap = (d.mode == 0)
                   ? snap_zone_at(e.xmotion.x_root, e.xmotion.y_root)
                   : SNAP_NONE;
            break;
        }
        case ButtonRelease:
            if (!keyboard) {
                /* Dropping against an edge snaps: top fills the monitor,
                 * the sides take half of it. */
                if (d.mode == 0) snap_apply(c, d.snap, e.xbutton.x_root,
                                            e.xbutton.y_root);
                done = 1;
            }
            break;
        case ButtonPress:
            if (keyboard) done = 1;
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            int step = (e.xkey.state & ShiftMask) ? 1 : 8;
            int nx = 0, ny = 0;
            if      (ks == XK_Left)  nx = -step;
            else if (ks == XK_Right) nx =  step;
            else if (ks == XK_Up)    ny = -step;
            else if (ks == XK_Down)  ny =  step;
            else if (ks == XK_Return || ks == XK_KP_Enter) { done = 1; break; }
            else if (ks == XK_Escape) {
                client_move_resize(c, d.ox, d.oy, d.ow, d.oh);
                done = 1;
                break;
            }
            if (nx || ny) {
                d.gx -= nx; d.gy -= ny;
                Window r, ch;
                int rx, ry, wx, wy;
                unsigned m;
                XQueryPointer(w2k.dpy, w2k.root, &r, &ch, &rx, &ry, &wx, &wy, &m);
                drag_apply(c, &d, rx, ry);
            }
            break;
        }
        default:
            /* Everything else -- exposes of the windows we are dragging over,
             * map requests, title changes -- goes to the normal dispatcher so
             * the desktop keeps working mid-drag. The window being dragged
             * may be destroyed by it: then the drag is over. */
            wm_handle_event(&e);
            if (client_find(cw) != c) { gone = 1; done = 1; }
            break;
        }
    }
    if (keyboard) XUngrabKeyboard(w2k.dpy, CurrentTime);
    if (outline) {
        outline_draw(ox, oy, ow, oh);          /* erase */
        XUngrabServer(w2k.dpy);
        /* Now put the window where the wire frame ended up. */
        if (!gone)
            client_move_resize(c, ox + b, oy + b + cap,
                               ow - 2 * b, oh - 2 * b - cap);
    }
    XUngrabPointer(w2k.dpy, CurrentTime);
}

void do_move(Client *c, XButtonEvent *e)
{
    /* Dragging a maximised window pulls it back to its restored size, with
     * the caption still under the pointer. */
    if (c->maximized) {
        int grab_frac = c->w ? (e->x_root - c->x) * 1000 / c->w : 500;
        client_maximize(c, 0);
        int nx = e->x_root - c->w * grab_frac / 1000;
        client_move_resize(c, nx, e->y_root + client_caption_h(c) / 2,
                           c->w, c->h);
    }
    if (c->maximized) return;
    drag_loop(c, 0, e->x_root, e->y_root, 0);
}

void do_resize(Client *c, XButtonEvent *e, int ht)
{
    if (c->maximized || !c->resizable) return;
    drag_loop(c, ht, e->x_root, e->y_root, 0);
}

/* ------------------------------------------------------------------ *
 * The system menu (the icon at the far left of the caption)
 * ------------------------------------------------------------------ */
void sysmenu_popup(Client *c, int x, int y)
{
    W2kMenu *m = w2k_menu_new();

    w2k_menu_item(m, SC_RESTORE, "&Restore", NULL, ICO_NONE);
    if (!c->maximized && !c->minimized) w2k_menu_disable(m);
    w2k_menu_item(m, SC_MOVE, "&Move", NULL, ICO_NONE);
    if (c->maximized) w2k_menu_disable(m);
    w2k_menu_item(m, SC_SIZE, "&Size", NULL, ICO_NONE);
    if (c->maximized || !c->resizable) w2k_menu_disable(m);
    w2k_menu_item(m, SC_MINIMIZE, "Mi&nimize", NULL, ICO_NONE);
    w2k_menu_item(m, SC_MAXIMIZE, "Ma&ximize", NULL, ICO_NONE);
    if (c->maximized || !c->resizable) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, SC_CLOSE, "&Close", "Alt+F4", ICO_NONE);
    w2k_menu_default(m);

    Window cw = c->win;
    int id = w2k_menu_popup(m, x, y, MPOP_LEFT);
    w2k_menu_free(m);
    if (client_find(cw) != c) return;      /* closed while the menu was up */

    Window r, ch;
    int rx, ry, wx, wy;
    unsigned mask;
    switch (id) {
    case SC_RESTORE:
        if (c->minimized) client_restore(c);
        else              client_maximize(c, 0);
        break;
    case SC_MOVE:
        XQueryPointer(w2k.dpy, w2k.root, &r, &ch, &rx, &ry, &wx, &wy, &mask);
        drag_loop(c, 0, rx, ry, 1);
        break;
    case SC_SIZE:
        XQueryPointer(w2k.dpy, w2k.root, &r, &ch, &rx, &ry, &wx, &wy, &mask);
        drag_loop(c, HT_BOTTOMRIGHT, rx, ry, 1);
        break;
    case SC_MINIMIZE: client_minimize(c); break;
    case SC_MAXIMIZE: client_maximize(c, 1); break;
    case SC_CLOSE:    client_close(c); break;
    }
}

/* ------------------------------------------------------------------ *
 * Alt+Tab -- the "cool switch" box
 * ------------------------------------------------------------------ */
#define SW_CELL   43        /* icon cell: 32px icon plus padding */
#define SW_COLS    7
#define SW_PAD     8

static int switch_list(Client **out, int max)
{
    int n = 0;
    /* Most-recently-used order: that is exactly our stacking list. */
    for (Client *c = stack; c && n < max; c = c->snext)
        if (!c->skip_taskbar && c->mapped) out[n++] = c;
    return n;
}

static void switch_paint(Window win, int w, int h, Client **list, int n, int sel)
{
    Pixmap pm = XCreatePixmap(w2k.dpy, win, w, h, w2k.depth);
    w2k_fill(pm, 0, 0, w, h, C_FACE);
    w2k_edge(pm, 0, 0, w, h, EDGE_RAISED, BF_RECT);

    for (int i = 0; i < n; i++) {
        int cx = SW_PAD + (i % SW_COLS) * SW_CELL;
        int cy = SW_PAD + (i / SW_COLS) * SW_CELL;
        if (i == sel) {
            w2k_fill(pm, cx, cy, SW_CELL - 3, SW_CELL - 3, C_HIGHLIGHT);
            w2k_focus_rect(pm, cx, cy, SW_CELL - 3, SW_CELL - 3);
        }
        w2k_bigicon_draw(pm, cx + (SW_CELL - 3 - 32) / 2,
                         cy + (SW_CELL - 3 - 32) / 2, list[i]->icon);
    }
    if (sel >= 0 && sel < n) {
        char buf[128];
        w2k_ellipsis(F_UI, list[sel]->name, w - 2 * SW_PAD, buf, sizeof buf);
        int tw = w2k_text_width(F_UI, buf, -1);
        w2k_text(pm, F_UI, (w - tw) / 2, h - SW_PAD - w2k_font_height(F_UI),
                 buf, C_TEXT);
    }
    XCopyArea(w2k.dpy, pm, win, w2k.gc, 0, 0, w, h, 0, 0);
    w2k_free_pixmap(pm);
}

void alt_tab(int backwards)
{
    Client *list[64];
    int n = switch_list(list, 64);
    if (n < 2) {
        if (n == 1) { client_restore(list[0]); }
        return;
    }

    int rows = (n + SW_COLS - 1) / SW_COLS;
    int cols = n < SW_COLS ? n : SW_COLS;
    int w = cols * SW_CELL + 2 * SW_PAD - 3;
    int h = rows * SW_CELL + 2 * SW_PAD - 3 + w2k_font_height(F_UI) + 6;
    /* Centred on the monitor holding the active window -- centring on the
     * virtual screen would put it across a bezel, or on a panel the user is
     * not looking at. */
    const W2kMonitor *m = focused ? w2k_monitor_at(focused->x + focused->w / 2,
                                                   focused->y + focused->h / 2)
                                  : w2k_monitor_of_pointer();
    int x = m->x + (m->w - w) / 2, y = m->y + (m->h - h) / 2;

    XSetWindowAttributes a = {
        .override_redirect = True, .save_under = True,
        .background_pixel = w2k.col[C_FACE], .event_mask = ExposureMask
    };
    Window win = XCreateWindow(w2k.dpy, w2k.root, x, y, w, h, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWSaveUnder | CWBackPixel |
                               CWEventMask, &a);
    XMapRaised(w2k.dpy, win);

    int sel = backwards ? n - 1 : 1;
    switch_paint(win, w, h, list, n, sel);

    /* The keyboard is already grabbed by the passive Alt+Tab grab; take an
     * active grab so we keep seeing keys until Alt comes back up. */
    XGrabKeyboard(w2k.dpy, w2k.root, False, GrabModeAsync, GrabModeAsync,
                  CurrentTime);

    for (int done = 0; !done && running; ) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        if (e.type == KeyPress) {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Tab) {
                sel += (e.xkey.state & ShiftMask) ? -1 : 1;
                if (sel < 0) sel = n - 1;
                if (sel >= n) sel = 0;
                switch_paint(win, w, h, list, n, sel);
            } else if (ks == XK_Escape) {
                sel = -1;
                done = 1;
            }
        } else if (e.type == KeyRelease) {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Alt_L || ks == XK_Alt_R || ks == XK_Meta_L)
                done = 1;
        } else if (e.type == Expose && e.xexpose.window == win) {
            switch_paint(win, w, h, list, n, sel);
        } else if (e.type != KeyPress && e.type != KeyRelease) {
            wm_handle_event(&e);
            /* Anything that closed meanwhile leaves the list. */
            for (int i = 0; i < n; ) {
                int alive = 0;
                for (Client *k = clients; k; k = k->next) if (k == list[i]) alive = 1;
                if (alive) { i++; continue; }
                for (int j = i; j + 1 < n; j++) list[j] = list[j + 1];
                n--;
                if (sel > i || sel >= n) sel = sel > 0 ? sel - 1 : 0;
                switch_paint(win, w, h, list, n, sel);
            }
            if (!n) done = 1;
        }
    }
    XUngrabKeyboard(w2k.dpy, CurrentTime);
    XDestroyWindow(w2k.dpy, win);
    if (sel >= 0 && sel < n) client_restore(list[sel]);
}

/* ------------------------------------------------------------------ *
 * Global hotkeys
 * ------------------------------------------------------------------ */
static const struct { unsigned mod; KeySym key; } bindings[] = {
    { Mod1Mask,              XK_Tab    },   /* task switch            */
    { Mod1Mask | ShiftMask,  XK_Tab    },   /* task switch, backwards */
    { Mod1Mask,              XK_F4     },   /* close                  */
    { Mod1Mask,              XK_space  },   /* system menu            */
    { Mod1Mask,              XK_Escape },   /* send to back           */
    { ControlMask,           XK_Escape },   /* Start menu             */
    { ControlMask | Mod1Mask, XK_Delete },  /* Task Manager           */
    { Mod4Mask,              XK_e      },   /* Explorer               */
    { Mod4Mask,              XK_r      },   /* Run...                 */
    { Mod4Mask,              XK_d      },   /* show desktop           */
    { Mod4Mask,              XK_Left   },   /* snap left              */
    { Mod4Mask,              XK_Right  },   /* snap right             */
    { Mod4Mask,              XK_Up     },   /* snap: maximise         */
    { Mod4Mask,              XK_Down   },   /* snap: restore / bottom */
};

void grab_keys(void)
{
    /* Ignore the lock modifiers so bindings work with Caps/Num Lock on. */
    static const unsigned locks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    XUngrabKey(w2k.dpy, AnyKey, AnyModifier, w2k.root);
    for (size_t i = 0; i < sizeof bindings / sizeof *bindings; i++) {
        KeyCode kc = XKeysymToKeycode(w2k.dpy, bindings[i].key);
        if (!kc) continue;
        for (size_t j = 0; j < sizeof locks / sizeof *locks; j++)
            XGrabKey(w2k.dpy, kc, bindings[i].mod | locks[j], w2k.root,
                     True, GrabModeAsync, GrabModeAsync);
    }
    /* The Windows key alone opens the Start menu. */
    for (int i = 0; i < 2; i++) {
        KeyCode kc = XKeysymToKeycode(w2k.dpy, i ? XK_Super_R : XK_Super_L);
        if (kc) XGrabKey(w2k.dpy, kc, AnyModifier, w2k.root, True,
                         GrabModeAsync, GrabModeAsync);
    }
}

static void show_desktop(void)
{
    int any = 0;
    for (Client *c = clients; c; c = c->next)
        if (!c->minimized && !c->skip_taskbar) { any = 1; break; }
    for (Client *c = clients; c; c = c->next) {
        if (c->skip_taskbar) continue;
        if (any) client_minimize(c);
        else if (c->minimized) client_restore(c);
    }
}

/* The Windows key opens the Start menu when pressed and released on its
 * own; pressed with another key it is the Win+E, Win+R... modifier and
 * must not open anything. */
static int super_down, super_used;

void handle_key_release(XKeyEvent *e)
{
    KeySym ks = XLookupKeysym(e, 0);
    if (ks != XK_Super_L && ks != XK_Super_R) return;
    if (super_down && !super_used) {
        if (startmenu_is_open()) startmenu_close(); else startmenu_open();
    }
    super_down = 0;
    super_used = 0;
}

void handle_key(XKeyEvent *e)
{
    KeySym ks = XLookupKeysym(e, 0);
    unsigned mod = e->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);

    if (ks == XK_Super_L || ks == XK_Super_R) {
        super_down = 1;
        super_used = 0;
        return;
    }
    if (mod & Mod4Mask) super_used = 1;
    if ((mod & Mod4Mask) && focused &&
        (ks == XK_Left || ks == XK_Right || ks == XK_Up || ks == XK_Down)) {
        int cx = focused->x + focused->w / 2, cy = focused->y + focused->h / 2;
        if (ks == XK_Up)          snap_apply(focused, SNAP_MAX, cx, cy);
        else if (ks == XK_Left)   snap_apply(focused, SNAP_LEFT, cx, cy);
        else if (ks == XK_Right)  snap_apply(focused, SNAP_RIGHT, cx, cy);
        else if (focused->maximized) client_maximize(focused, 0);
        else                      snap_apply(focused, SNAP_BOTTOM, cx, cy);
        return;
    }
    if ((mod & Mod1Mask) && ks == XK_Tab)    { alt_tab(mod & ShiftMask); return; }
    if ((mod & Mod1Mask) && ks == XK_F4)     { client_close(focused); return; }
    if ((mod & Mod1Mask) && ks == XK_space) {
        if (focused)
            sysmenu_popup(focused, focused->x - client_border(focused), focused->y);
        return;
    }
    if ((mod & Mod1Mask) && ks == XK_Escape) {
        /* Send the active window to the back and focus what surfaces. */
        if (focused) {
            Client *c = focused;
            Client **p = &stack;
            while (*p) { if (*p == c) { *p = c->snext; break; } p = &(*p)->snext; }
            Client **t = &stack;
            while (*t) t = &(*t)->snext;
            *t = c; c->snext = NULL;
            clients_restack();
            for (Client *s = stack; s; s = s->snext)
                if (!s->minimized && s->mapped) { client_focus(s); break; }
        }
        return;
    }
    if ((mod & ControlMask) && ks == XK_Escape) {
        if (startmenu_is_open()) startmenu_close(); else startmenu_open();
        return;
    }
    if ((mod & ControlMask) && (mod & Mod1Mask) && ks == XK_Delete) {
        wm_spawn("l2ktaskmgr");
        return;
    }
    if ((mod & Mod4Mask) && ks == XK_e) { wm_spawn("l2kexplorer"); return; }
    if ((mod & Mod4Mask) && ks == XK_r) { wm_run_dialog(); return; }
    if ((mod & Mod4Mask) && ks == XK_d) { show_desktop(); return; }
}
