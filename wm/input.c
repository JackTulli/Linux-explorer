/* input.c -- moving, sizing, the system menu, Alt+Tab and global hotkeys. */
#include "wm.h"
#include <X11/XF86keysym.h>
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

/* A window of one fixed size, a full-screen one and a bare one keep
 * their size: Win+Left made a fixed dialog half the screen, and a
 * full-screen video a half-screen one that still hid the taskbar. */
static int snap_ok(const Client *c)
{
    return !c->fullscreen && c->decorate && c->resizable;
}

/* Put the window where the zone says. */
static void snap_apply(Client *c, int zone, int px, int py)
{
    if (zone == SNAP_NONE || !snap_ok(c)) return;
    if (zone == SNAP_MAX) { client_maximize(c, 1); return; }

    int wx, wy, ww, wh;
    wm_workarea_of(w2k_monitor_at(px, py), &wx, &wy, &ww, &wh);
    int b = client_border(c), cap = client_caption_h(c);
    int x = wx, y = wy, w = ww, h = wh;
    if (zone == SNAP_LEFT)        { w = ww / 2; }
    else if (zone == SNAP_RIGHT)  { w = ww / 2; x = wx + ww - w; }
    else if (zone == SNAP_BOTTOM) { h = wh / 2; y = wy + wh - h; }

    /* Remember where it was, so Restore puts it back -- for a maximised
     * window, the size it had before that, and it is maximised no longer:
     * it stayed flagged so, half the screen wide, and could then be
     * neither dragged nor resized. */
    if (c->maximized) { c->maximized = 0; client_publish_state(c); }
    else { c->rx = c->x; c->ry = c->y; c->rw = c->w; c->rh = c->h; }
    /* Within the sizes the window allows, still against its edge. */
    int cw = w - 2 * b, ch = h - 2 * b - cap;
    client_constrain(c, &cw, &ch);
    if (zone == SNAP_RIGHT)  x = wx + ww - (cw + 2 * b);
    if (zone == SNAP_BOTTOM) y = wy + wh - (ch + 2 * b + cap);
    client_move_resize(c, x + b, y + b + cap, cw, ch);
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
 * is drawn and erased, so nothing underneath needs repainting. (1.36.0
 * flew a caption-coloured bar instead; it looked worse, and this is back.) */
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

    int done = 0, gone = 0, cancel = 0, snap = SNAP_NONE, rel_x = px, rel_y = py;
    Window cw = c->win;
    if (!keyboard) {
        /* A program hands its drag over (_NET_WM_MOVERESIZE) while its
         * button is down; a quick flick can be over before the grab, and
         * the release went to the program: the window then followed the
         * pointer, no button held, until the next click. */
        Window r, ch;
        int rx, ry, wx, wy;
        unsigned m = 0;
        XQueryPointer(w2k.dpy, w2k.root, &r, &ch, &rx, &ry, &wx, &wy, &m);
        if (!(m & (Button1Mask | Button2Mask | Button3Mask))) done = 1;
    }
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
                /* Where the button came up is where it goes: motion is
                 * taken at most every 8 ms, and the last few pixels of
                 * travel were lost -- the window landed short of the
                 * pointer, with the snap zone from before. */
                rel_x = e.xbutton.x_root;
                rel_y = e.xbutton.y_root;
                if (outline) {
                    outline_draw(ox, oy, ow, oh);
                    drag_geometry(c, &d, rel_x, rel_y, &ox, &oy, &ow, &oh);
                    outline_draw(ox, oy, ow, oh);
                } else
                    drag_apply(c, &d, rel_x, rel_y);
                /* Dropping against an edge snaps: top fills the monitor,
                 * the sides take half of it. Done below, once. One
                 * that cannot snap lands where it was dropped. */
                if (d.mode == 0 && snap_ok(c)) snap = snap_zone_at(rel_x, rel_y);
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
            else if (ks == XK_Escape) { cancel = 1; done = 1; break; }
            if (nx || ny) {
                d.gx -= nx; d.gy -= ny;
                Window r, ch;
                int rx, ry, wx, wy;
                unsigned m;
                XQueryPointer(w2k.dpy, w2k.root, &r, &ch, &rx, &ry, &wx, &wy, &m);
                /* With the wire frame it is the frame the keys move: the
                 * window moved under it, and Enter put it back. */
                if (outline) {
                    outline_draw(ox, oy, ow, oh);
                    drag_geometry(c, &d, rx, ry, &ox, &oy, &ow, &oh);
                    outline_draw(ox, oy, ow, oh);
                } else
                    drag_apply(c, &d, rx, ry);
            }
            break;
        }
        default:
            /* The program ending the drag it handed over: its own
             * _NET_WM_MOVERESIZE_CANCEL, sent when it saw the release. */
            if (e.type == ClientMessage && e.xclient.window == cw &&
                e.xclient.message_type == w2k.a_net_wm_moveresize &&
                e.xclient.data.l[2] == 11) {
                done = 1;
                break;
            }
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
        outline_draw(ox, oy, ow, oh);          /* erase, before anything moves */
        XUngrabServer(w2k.dpy);
    }
    /* Exactly one ending: back where it was, snapped, or where the wire
     * frame ended up. A snap used to be applied with the frame still on
     * the screen and then overridden by the move to it -- a window dropped
     * on the top edge was left flagged maximised at the dropped size. */
    if (!gone) {
        if (cancel) {
            if (!outline) client_move_resize(c, d.ox, d.oy, d.ow, d.oh);
        } else if (snap != SNAP_NONE)
            snap_apply(c, snap, rel_x, rel_y);
        else if (outline)
            client_move_resize(c, ox + b, oy + b + cap, ow - 2 * b, oh - 2 * b - cap);
    }
    XUngrabPointer(w2k.dpy, CurrentTime);
    glass_live_refresh();          /* the glass over and under it shows the new place */
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
/* What Minimize puts down: a dialog has no task button to come back
 * from, so it goes down with the window that owns it and comes back with
 * it. Minimize did nothing on a resizable one -- a file chooser, say.
 * NULL when no window with a task button owns it. */
static Client *minimize_target(Client *c)
{
    for (int i = 0; c && c->skip_taskbar && i < 8; i++)
        c = c->transient_for ? client_find(c->transient_for) : NULL;
    return c;
}

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
    if (!minimize_target(c)) w2k_menu_disable(m);
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
    case SC_MINIMIZE: client_minimize(minimize_target(c)); break;
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
    Pixmap pm = XCreatePixmap(w2k.dpy, win, (unsigned)w2k_px(w), (unsigned)w2k_px(h), w2k.depth);
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
    XCopyArea(w2k.dpy, pm, win, w2k.gc, 0, 0, (unsigned)w2k_px(w), (unsigned)w2k_px(h), 0, 0);
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
    int x = m->x + (m->w - w2k_px(w)) / 2, y = m->y + (m->h - w2k_px(h)) / 2;

    XSetWindowAttributes a = {
        .override_redirect = True, .save_under = True,
        .background_pixel = w2k.col[C_FACE], .event_mask = ExposureMask
    };
    Window win = XCreateWindow(w2k.dpy, w2k.root, x, y, (unsigned)w2k_px(w),
                               (unsigned)w2k_px(h), 0,
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

    /* Alt may already be up: a quick Alt+Tab while the shell was busy
     * let go of it before the grab, the release went to the program, and
     * the box stayed up holding the keyboard until Alt was pressed and
     * released again. Then the switch is to the one already chosen. */
    int done = 0;
    XModifierKeymap *mm = XGetModifierMapping(w2k.dpy);
    if (mm) {
        char keys[32];
        XQueryKeymap(w2k.dpy, keys);
        int alt = 0;
        for (int i = 0; i < mm->max_keypermod; i++) {
            KeyCode kc = mm->modifiermap[Mod1MapIndex * mm->max_keypermod + i];
            if (kc && (keys[kc / 8] & (1 << (kc % 8)))) alt = 1;
        }
        XFreeModifiermap(mm);
        if (!alt) done = 1;
    }

    while (!done && running) {
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
    { Mod4Mask,              XK_Pause  },   /* System Properties      */
    { Mod4Mask,              XK_Left   },   /* snap left              */
    { Mod4Mask,              XK_Right  },   /* snap right             */
    { Mod4Mask,              XK_Up     },   /* snap: maximise         */
    { Mod4Mask,              XK_Down   },   /* snap: restore / bottom */
    /* The keyboard's media keys: the speaker by the clock, and whatever
     * is playing (an MPRIS player, over D-Bus). */
    { 0, XF86XK_AudioRaiseVolume },
    { 0, XF86XK_AudioLowerVolume },
    { 0, XF86XK_AudioMute },
    { 0, XF86XK_AudioPlay },
    { 0, XF86XK_AudioPause },
    { 0, XF86XK_AudioStop },
};

/* Volume keys move the mixer in steps of five, landing on a multiple of
 * five as Windows' keys do; the mute key is the speaker's own mute. */
static void media_key(KeySym ks)
{
    if (ks == XF86XK_AudioPlay)  { media_control("PlayPause"); return; }
    if (ks == XF86XK_AudioPause) { media_control("Pause"); return; }
    if (ks == XF86XK_AudioStop)  { media_control("Stop"); return; }
    if (!volume_available()) return;
    if (ks == XF86XK_AudioMute) {
        volume_toggle_mute();
    } else {
        int v = volume_level();
        /* Not read yet -- the sound server came up after the bar did.
         * Ask now; if it still will not say, step from wherever it
         * really is: made up as half, the first key press jumped a
         * quiet speaker to 55%. */
        if (v < 0) { volume_poll(); v = volume_level(); }
        if (v < 0) {
            int up = ks == XF86XK_AudioRaiseVolume;
            wm_spawn(up ? "{ pactl set-sink-volume @DEFAULT_SINK@ +5% || amixer -q set Master 5%+; } >/dev/null 2>&1"
                        : "{ pactl set-sink-volume @DEFAULT_SINK@ -5% || amixer -q set Master 5%-; } >/dev/null 2>&1");
            return;
        }
        const int step = 5;
        v = ks == XF86XK_AudioRaiseVolume ? (v / step + 1) * step
                                          : ((v + step - 1) / step - 1) * step;
        volume_set(v);
    }
    taskbar_paint();
}

/* Grab every key that gives `ks` unshifted -- which is how handle_key()
 * reads it. A keymap can put one symbol on several keys: the usual one has
 * Play on three (a keyboard's play/pause key, a headset's play, a remote's
 * play), and XKeysymToKeycode() names only the first, so the others did
 * nothing. */
static void grab_sym(const KeySym *map, int min, int max, int per,
                     KeySym ks, unsigned mod)
{
    /* Ignore the lock modifiers so bindings work with Caps/Num Lock on. */
    static const unsigned locks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    for (int kc = min; kc <= max; kc++) {
        if (map[(size_t)(kc - min) * (size_t)per] != ks) continue;
        if (mod == AnyModifier) {
            XGrabKey(w2k.dpy, kc, AnyModifier, w2k.root, True,
                     GrabModeAsync, GrabModeAsync);
            continue;
        }
        for (size_t j = 0; j < sizeof locks / sizeof *locks; j++)
            XGrabKey(w2k.dpy, kc, mod | locks[j], w2k.root,
                     True, GrabModeAsync, GrabModeAsync);
    }
}

void grab_keys(void)
{
    XUngrabKey(w2k.dpy, AnyKey, AnyModifier, w2k.root);
    int min = 0, max = 0, per = 0;
    XDisplayKeycodes(w2k.dpy, &min, &max);
    KeySym *map = XGetKeyboardMapping(w2k.dpy, (KeyCode)min, max - min + 1, &per);
    if (!map || per < 1) { if (map) XFree(map); return; }
    for (size_t i = 0; i < sizeof bindings / sizeof *bindings; i++)
        grab_sym(map, min, max, per, bindings[i].key, bindings[i].mod);
    /* The Windows key alone opens the Start menu. */
    grab_sym(map, min, max, per, XK_Super_L, AnyModifier);
    grab_sym(map, min, max, per, XK_Super_R, AnyModifier);
    XFree(map);
}

/* The windows the last Win+D put down: pressed again, it brings back
 * those and no others. It brought back every minimised window, the ones
 * the user had put away before included. */
static Window desk_hid[256];
static int desk_nhid;

static int desk_was_hidden(const Client *c)
{
    for (int i = 0; i < desk_nhid; i++)
        if (desk_hid[i] == c->win) return 1;
    return 0;
}

/* Win+D, the Quick Launch icon and the Windows 7 sliver: everything down,
 * or everything back, quietly -- one sound, one restack, one repaint of
 * the bar, where each window used to take its own flight and sound (a
 * second of grabbed server for a dozen windows). Back in the order they
 * were stacked, so the window that was on top is on top again and has
 * the focus; it used to be the oldest. */
void wm_show_desktop(void)
{
    int any = 0;
    for (Client *c = clients; c; c = c->next)
        if (!c->minimized && !c->skip_taskbar) { any = 1; break; }
    w2k_sound_play(any ? SND_MINIMIZE : SND_RESTOREUP);
    if (any) {
        desk_nhid = 0;
        for (Client *c = clients; c; c = c->next) {
            if (c->skip_taskbar || c->minimized) continue;
            client_minimize_quiet(c);
            if (c->minimized && desk_nhid < 256) desk_hid[desk_nhid++] = c->win;
        }
    } else {
        Client *order[256];
        int n = 0;
        for (Client *c = stack; c && n < 256; c = c->snext) order[n++] = c;
        /* Nothing of the last Win+D still down -- or no Win+D yet: then
         * everything comes back, as it always did. */
        int ours = 0;
        for (int i = 0; i < n; i++)
            if (order[i]->minimized && desk_was_hidden(order[i])) ours = 1;
        for (int i = n - 1; i >= 0; i--)
            if (!order[i]->skip_taskbar && (!ours || desk_was_hidden(order[i])))
                client_restore_quiet(order[i]);
        desk_nhid = 0;
        for (Client *c = stack; c; c = c->snext)
            if (!c->minimized && c->mapped && !c->skip_taskbar) { client_focus(c); break; }
    }
    clients_restack();
    taskbar_paint();
}

/* The Windows key opens the Start menu when pressed and released on its
 * own; pressed with another key it is the Win+E, Win+R... modifier and
 * must not open anything. */
static int super_down, super_used;

void handle_key_release(XKeyEvent *e)
{
    KeySym ks = XLookupKeysym(e, 0);
    if (ks != XK_Super_L && ks != XK_Super_R) return;
    /* Cleared before the menu opens: it runs its own loop until it
     * closes, and the second press's release came back through here with
     * the first press still on the books -- which "closed" the menu by
     * dropping its pointer grab and left it on screen holding the
     * keyboard. (The second press itself closes it, inside its loop.) */
    int open = super_down && !super_used;
    super_down = 0;
    super_used = 0;
    if (open) {
        if (startmenu_is_open()) startmenu_close(); else startmenu_open();
    }
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
    /* Before the media keys: Win+Volume Up changed the volume and then
     * opened the Start menu when the Windows key came up. */
    if (mod & Mod4Mask) super_used = 1;
    if (ks == XF86XK_AudioRaiseVolume || ks == XF86XK_AudioLowerVolume ||
        ks == XF86XK_AudioMute || ks == XF86XK_AudioPlay ||
        ks == XF86XK_AudioPause || ks == XF86XK_AudioStop) {
        media_key(ks);
        return;
    }
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
    if ((mod & Mod4Mask) && ks == XK_d) { wm_show_desktop(); return; }
    if ((mod & Mod4Mask) && ks == XK_Pause) { wm_spawn("l2kcontrol system"); return; }
}
