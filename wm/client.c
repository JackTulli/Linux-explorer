/* client.c -- managing application windows: reparenting, geometry, hints. */
#include "wm.h"
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

Client *clients, *stack, *focused;

/* ------------------------------------------------------------------ *
 * Lookup
 * ------------------------------------------------------------------ */
Client *client_find(Window w)
{
    for (Client *c = clients; c; c = c->next)
        if (c->win == w) return c;
    return NULL;
}

Client *client_find_frame(Window w)
{
    for (Client *c = clients; c; c = c->next)
        if (c->frame == w) return c;
    return NULL;
}

int client_border(Client *c)
{
    if (!c->decorate || c->fullscreen) return 0;
    /* Windows 7 Basic frames are ten pixels all round, dialogs included. */
    if (w2k_theme == THEME_BASIC7) return 10;
    return c->resizable ? FRAME_SIZE : FRAME_FIXED;
}

int client_caption_h(Client *c)
{
    if (!c->decorate || c->fullscreen) return 0;
    /* Luna's caption is taller than the classic one: its buttons are 19
     * pixels square where Windows 2000's are 16 by 14. */
    return w2k_theme == THEME_CLASSIC ? CAPTION_H
                                      : w2k_theme_caption_h(w2k_theme);
}

int client_frame_w(Client *c) { return c->w + 2 * client_border(c); }

int client_frame_h(Client *c)
{
    if (c->shaded) return client_caption_h(c) + 2 * client_border(c);
    return c->h + 2 * client_border(c) + client_caption_h(c);
}

/* ------------------------------------------------------------------ *
 * Properties
 * ------------------------------------------------------------------ */
static char *get_text_prop(Window w, Atom prop)
{
    XTextProperty tp;
    char *out = NULL;
    if (!XGetTextProperty(w2k.dpy, w, &tp, prop) || !tp.nitems) return NULL;
    if (tp.encoding == XA_STRING || tp.encoding == w2k.a_utf8) {
        out = w2k_strdup((char *)tp.value);
    } else {
        char **list = NULL;
        int n = 0;
        if (XmbTextPropertyToTextList(w2k.dpy, &tp, &list, &n) >= Success && n > 0) {
            out = w2k_strdup(list[0]);
            XFreeStringList(list);
        }
    }
    XFree(tp.value);
    return out;
}

void client_update_name(Client *c)
{
    char *n = get_text_prop(c->win, w2k.a_net_wm_name);
    if (!n) n = get_text_prop(c->win, XA_WM_NAME);
    free(c->name);
    c->name = n ? n : w2k_strdup("(Untitled)");
}

/* Guess an icon from WM_CLASS, so our own apps get their real icons and
 * everything else gets the generic application window. */
/* The icon an application publishes in _NET_WM_ICON: a list of
 * width, height, then width*height ARGB pixels, largest last as a rule.
 * The one nearest 32 pixels is taken and scaled to the two sizes the shell
 * draws. This is where Firefox, terminals and the rest get a real taskbar icon
 * instead of a generic one. */
static int icon_from_property(Window win)
{
    Atom type;
    int fmt;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(w2k.dpy, win, w2k.a_net_wm_icon, 0, 1L << 20, False,
                           XA_CARDINAL, &type, &fmt, &n, &after,
                           &data) != Success || !data)
        return -1;
    if (fmt != 32 || n < 3) { XFree(data); return -1; }

    const long *p = (const long *)data;
    unsigned long i = 0;
    long best_off = -1;
    int best_w = 0, best_h = 0, best_score = -1;
    while (i + 2 < n) {
        int w = (int)p[i], h = (int)p[i + 1];
        if (w <= 0 || h <= 0 || (unsigned long)w * h > n - i - 2) break;
        int score = 1000 - (w > 32 ? w - 32 : (32 - w) * 2);   /* prefer >=32 */
        if (score > best_score) {
            best_score = score;
            best_off = (long)(i + 2);
            best_w = w;
            best_h = h;
        }
        i += 2 + (unsigned long)w * h;
    }
    if (best_off < 0) { XFree(data); return -1; }

    unsigned char *rgba = malloc((size_t)best_w * best_h * 4);
    if (!rgba) { XFree(data); return -1; }
    for (long k = 0; k < (long)best_w * best_h; k++) {
        unsigned long v = (unsigned long)p[best_off + k];
        unsigned char *o = rgba + k * 4;
        o[0] = (v >> 16) & 0xff;
        o[1] = (v >> 8) & 0xff;
        o[2] = v & 0xff;
        o[3] = (v >> 24) & 0xff;
    }
    XFree(data);

    unsigned char *i16 = w2k_rgba_scale(rgba, best_w, best_h, 16);
    unsigned char *i32 = w2k_rgba_scale(rgba, best_w, best_h, 32);
    free(rgba);
    if (!i16 || !i32) { free(i16); free(i32); return -1; }
    return w2k_icon_register(i16, i32);
}

static int icon_for_class(const char *cls)
{
    if (!cls) return ICO_APP;
    if (!strcasecmp(cls, "w2knotepad"))  return ICO_NOTEPAD;
    if (!strcasecmp(cls, "w2kexplorer")) return ICO_EXPLORER;
    if (!strcasecmp(cls, "w2ktaskmgr"))  return ICO_TASKMGR;
    if (!strcasecmp(cls, "w2kcalc"))     return ICO_CALC;
    if (!strcasecmp(cls, "w2kcharmap"))  return ICO_CHARMAP;
    if (!strcasecmp(cls, "w2kimage"))    return ICO_PAINT;
    if (!strcasecmp(cls, "w2ksnip"))     return ICO_SNIP;
    if (!strcasecmp(cls, "w2kcontrol"))  return ICO_CONTROLPANEL;
    if (!strcasecmp(cls, "w2kdisplay"))  return ICO_SETTINGS;
    if (strcasestr(cls, "term"))         return ICO_TERMINAL;
    if (strcasestr(cls, "xcalc"))        return ICO_CALC;
    return ICO_APP;
}

/* Pick the icon for a window: the shell's own artwork for the shell's own
 * programs, then whatever the application publishes, then the icon theme by
 * class name. Results are kept per class, so a hundred Firefox windows cost
 * one lookup and one pair of pixmaps. */
static int icon_for_client(Client *c)
{
    static struct { char cls[64]; int id; } cache[64];
    static int ncache;

    int built_in = icon_for_class(c->cls);
    if (built_in == ICO_APP) built_in = icon_for_class(c->cls_name);
    if (built_in != ICO_APP) return built_in;
    if (!c->cls || !c->cls[0]) return ICO_APP;

    for (int i = 0; i < ncache; i++)
        if (!strcasecmp(cache[i].cls, c->cls)) return cache[i].id;

    int id = icon_from_property(c->win);
    /* Either half of WM_CLASS may be the icon theme's name for it. */
    if (id < 0) id = w2k_icon_by_name(c->cls);
    if (id == ICO_APP && c->cls_name) id = w2k_icon_by_name(c->cls_name);
    static int next;
    int slot = ncache < (int)(sizeof cache / sizeof *cache) ? ncache++ : next++ % (int)(sizeof cache / sizeof *cache);
    snprintf(cache[slot].cls, sizeof cache[slot].cls, "%.63s", c->cls);
    cache[slot].id = id;
    return id;
}

void client_update_hints(Client *c)
{
    XSizeHints sh;
    long supplied = 0;
    c->minw = c->minh = 0;
    c->maxw = c->maxh = 0;
    c->incw = c->inch = 0;
    c->basew = c->baseh = 0;

    if (XGetWMNormalHints(w2k.dpy, c->win, &sh, &supplied)) {
        if (sh.flags & PMinSize)    { c->minw = sh.min_width; c->minh = sh.min_height; }
        if (sh.flags & PMaxSize)    { c->maxw = sh.max_width; c->maxh = sh.max_height; }
        if (sh.flags & PResizeInc)  { c->incw = sh.width_inc; c->inch = sh.height_inc; }
        if (sh.flags & PBaseSize)   { c->basew = sh.base_width; c->baseh = sh.base_height; }
        else if (sh.flags & PMinSize) { c->basew = c->minw; c->baseh = c->minh; }
    }
    c->resizable = 1;
    if (c->minw && c->maxw && c->minw == c->maxw &&
        c->minh && c->maxh && c->minh == c->maxh)
        c->resizable = 0;

    XWMHints *wmh = XGetWMHints(w2k.dpy, c->win);
    c->takes_focus = 1;
    if (wmh) {
        if (wmh->flags & InputHint) c->takes_focus = wmh->input ? 1 : 0;
        XFree(wmh);
    }

    /* WM_PROTOCOLS: does it understand a graceful close? */
    Atom *pr = NULL;
    int np = 0;
    c->deleteable = 0;
    if (XGetWMProtocols(w2k.dpy, c->win, &pr, &np)) {
        for (int i = 0; i < np; i++) {
            if (pr[i] == w2k.a_wm_delete)     c->deleteable = 1;
            if (pr[i] == w2k.a_wm_take_focus) c->take_focus_proto = 1;
        }
        XFree(pr);
    }
}

/* _MOTIF_WM_HINTS: the traditional "please draw no decorations" channel. */
static int motif_undecorated(Window w)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    int undec = 0;
    if (XGetWindowProperty(w2k.dpy, w, w2k.a_motif_hints, 0, 5, False,
                           AnyPropertyType, &type, &fmt, &n, &after,
                           &data) == Success && data) {
        /* flags at [0]; bit 1 == MWM_HINTS_DECORATIONS, value at [2] */
        if (fmt == 32 && n >= 3) {
            long *l = (long *)data;
            if ((l[0] & 2) && l[2] == 0) undec = 1;
        }
        XFree(data);
    }
    return undec;
}

void client_update_type(Client *c)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;

    c->decorate = 1;
    c->skip_taskbar = 0;
    c->is_dialog = 0;

    if (XGetWindowProperty(w2k.dpy, c->win, w2k.a_net_wm_window_type, 0, 8,
                           False, XA_ATOM, &type, &fmt, &n, &after,
                           &data) == Success && data) {
        Atom *a = (Atom *)data;
        for (unsigned long i = 0; i < n; i++) {
            if (a[i] == w2k.a_net_wm_wt_dock || a[i] == w2k.a_net_wm_wt_splash ||
                a[i] == w2k.a_net_wm_wt_menu || a[i] == w2k.a_net_wm_wt_toolbar) {
                c->decorate = 0;
                c->skip_taskbar = 1;
            } else if (a[i] == w2k.a_net_wm_wt_dialog) {
                c->is_dialog = 1;
                c->skip_taskbar = 1;
            } else if (a[i] == w2k.a_net_wm_wt_utility) {
                c->skip_taskbar = 1;
            }
        }
        XFree(data);
    }
    /* An application asking for no decorations at all is a convention from
     * toolkits that draw their own title bar. Plenty of them then draw
     * nothing, leaving a window with no way to move, resize or close it --
     * so on this desktop the frame is the window manager's business and the
     * request is overridden for ordinary windows. ForceDecorations=0 in
     * ~/.w2k/scheme gives the application its way back. Windows that are
     * chrome by type (dock, splash, menu, toolbar) were already left bare
     * above and are not reconsidered here. */
    if (motif_undecorated(c->win) && c->decorate && !w2k_force_decorations)
        c->decorate = 0;

    /* A transient is somebody's dialog: it belongs to its owner's button. */
    Window tr = None;
    if (XGetTransientForHint(w2k.dpy, c->win, &tr) && tr != None) {
        c->skip_taskbar = 1;
        c->is_dialog = 1;
    }

    /* The app's initial _NET_WM_STATE, once: after that the property is
     * ours, written back as the state changes, and re-reading it on a
     * theme change would resurrect a maximize the user has undone. */
    if (!c->state_read &&
        XGetWindowProperty(w2k.dpy, c->win, w2k.a_net_wm_state, 0, 16, False,
                           XA_ATOM, &type, &fmt, &n, &after, &data) == Success
        && data) {
        Atom *a = (Atom *)data;
        for (unsigned long i = 0; i < n; i++) {
            if (a[i] == w2k.a_net_wm_state_skip_taskbar) c->skip_taskbar = 1;
            if (a[i] == w2k.a_net_wm_state_above)        c->above = 1;
            if (a[i] == w2k.a_net_wm_state_maxv ||
                a[i] == w2k.a_net_wm_state_maxh)         c->maximized = 1;
            if (a[i] == w2k.a_net_wm_state_fullscreen)   c->fullscreen = 1;
        }
        XFree(data);
    }
    c->state_read = 1;
}

void client_publish_state(Client *c)
{
    Atom st[8];
    int n = 0;
    if (c->maximized)    { st[n++] = w2k.a_net_wm_state_maxv; st[n++] = w2k.a_net_wm_state_maxh; }
    if (c->minimized)    st[n++] = w2k.a_net_wm_state_hidden;
    if (c->fullscreen)   st[n++] = w2k.a_net_wm_state_fullscreen;
    if (c->above)        st[n++] = w2k.a_net_wm_state_above;
    if (c->skip_taskbar) st[n++] = w2k.a_net_wm_state_skip_taskbar;
    XChangeProperty(w2k.dpy, c->win, w2k.a_net_wm_state, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)st, n);
}

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */
void client_constrain(Client *c, int *w, int *h)
{
    if (*w < MIN_CLIENT_W) *w = MIN_CLIENT_W;
    if (*h < MIN_CLIENT_H) *h = MIN_CLIENT_H;
    if (c->minw && *w < c->minw) *w = c->minw;
    if (c->minh && *h < c->minh) *h = c->minh;
    if (c->maxw && *w > c->maxw) *w = c->maxw;
    if (c->maxh && *h > c->maxh) *h = c->maxh;
    if (c->incw > 1) {
        int base = c->basew;
        *w = base + ((*w - base) / c->incw) * c->incw;
        if (*w < MIN_CLIENT_W) *w += c->incw;
    }
    if (c->inch > 1) {
        int base = c->baseh;
        *h = base + ((*h - base) / c->inch) * c->inch;
        if (*h < MIN_CLIENT_H) *h += c->inch;
    }
}

/* Move/resize by *client* rectangle in root coordinates. */
void client_move_resize(Client *c, int x, int y, int w, int h)
{
    int b = client_border(c), cap = client_caption_h(c);
    c->x = x; c->y = y; c->w = w; c->h = h;

    XMoveResizeWindow(w2k.dpy, c->frame, x - b, y - b - cap,
                      client_frame_w(c), client_frame_h(c));
    if (!c->shaded)
        XMoveResizeWindow(w2k.dpy, c->win, b, b + cap, w, h);
    else
        XMoveWindow(w2k.dpy, c->win, b, b + cap);   /* rolled up behind the caption */

    /* ICCCM synthetic ConfigureNotify: the client must learn its true
     * root-relative position, which reparenting otherwise hides from it. */
    XConfigureEvent ce = {
        .type = ConfigureNotify, .display = w2k.dpy, .event = c->win,
        .window = c->win, .x = x, .y = y, .width = w, .height = h,
        .border_width = 0, .above = None, .override_redirect = False
    };
    XSendEvent(w2k.dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
    frame_shape(c);            /* the corners follow the new size */
    frame_paint(c);
}

/* ------------------------------------------------------------------ *
 * Stacking / focus
 * ------------------------------------------------------------------ */
static void stack_remove(Client *c)
{
    Client **p = &stack;
    while (*p) {
        if (*p == c) { *p = c->snext; c->snext = NULL; return; }
        p = &(*p)->snext;
    }
}

/* Rebuild the X stacking order from our list: always-on-top clients first,
 * then normal ones, with the taskbar above everything and the desktop below. */
void clients_restack(void)
{
    Window wins[256];
    int n = 0;
    /* The auto-hide trigger has to stay above everything: it is InputOnly,
     * so being covered means it stops seeing the pointer. */
    Window trigger = taskbar_trigger_window();
    if (trigger) wins[n++] = trigger;
    /* "Always on top" is what puts the taskbar at the head of the stack;
     * without it the bar is just another window and can be covered. */
    /* A full-screen window covers the bar: that is the point of it. */
    for (Client *c = stack; c && n < 250; c = c->snext)
        if (c->fullscreen && !c->minimized) wins[n++] = c->frame;
    if (w2k_taskbar_ontop) wins[n++] = taskbar_window();
    for (Client *c = stack; c && n < 250; c = c->snext)
        if (c->above && !c->minimized && !c->fullscreen) wins[n++] = c->frame;
    for (Client *c = stack; c && n < 250; c = c->snext)
        if (!c->above && !c->minimized && !c->fullscreen) wins[n++] = c->frame;
    if (!w2k_taskbar_ontop) wins[n++] = taskbar_window();
    wins[n++] = desktop_window();
    XRestackWindows(w2k.dpy, wins, n);
}

void client_raise(Client *c)
{
    if (!c) return;
    stack_remove(c);
    c->snext = stack;
    stack = c;
    clients_restack();
}

void client_focus(Client *c)
{
    if (focused == c && c && !c->minimized) return;

    Client *old = focused;
    if (c && (c->minimized || !c->mapped)) return;

    focused = c;
    if (old && old != c) frame_paint(old);

    if (c) {
        if (c->takes_focus)
            XSetInputFocus(w2k.dpy, c->win, RevertToPointerRoot, CurrentTime);
        if (c->take_focus_proto)
            client_send_protocol(c, w2k.a_wm_take_focus);
        frame_paint(c);
    } else {
        XSetInputFocus(w2k.dpy, w2k.root, RevertToPointerRoot, CurrentTime);
    }
    wm_set_active(c);
    taskbar_paint();
}

void client_send_protocol(Client *c, Atom proto)
{
    XClientMessageEvent ev = {
        .type = ClientMessage, .window = c->win,
        .message_type = w2k.a_wm_protocols, .format = 32
    };
    ev.data.l[0] = proto;
    /* ICCCM: the time of the event that caused it, never CurrentTime. */
    ev.data.l[1] = (long)(wm_last_time ? wm_last_time : CurrentTime);
    XSendEvent(w2k.dpy, c->win, False, NoEventMask, (XEvent *)&ev);
}

void client_close(Client *c)
{
    if (!c) return;
    if (c->deleteable) { client_send_protocol(c, w2k.a_wm_delete); return; }
    /* Killing the client means killing its connection to the server --
     * and the shell's own dialogs are managed windows too, so that would
     * be this process. Take those down through the toolkit instead. */
    if (w2k_win_owns(c->win)) {
        XDestroyWindow(w2k.dpy, c->win);
        return;
    }
    XKillClient(w2k.dpy, c->win);
}

/* ------------------------------------------------------------------ *
 * Show / hide states
 * ------------------------------------------------------------------ */
void client_minimize(Client *c)
{
    /* Fly the wire frame down to the task button on the way out. */
    int bx, by, bw, bh;
    if (c && c->mapped && !c->minimized && taskbar_button_rect(c, &bx, &by, &bw, &bh))
        wm_animate_rect(c->x - client_border(c),
                        c->y - client_border(c) - client_caption_h(c),
                        client_frame_w(c), client_frame_h(c), bx, by, bw, bh);

    if (!c || c->minimized) return;
    /* A dialog has no task button to come back from: it stays. */
    if (c->skip_taskbar) return;
    c->minimized = 1;
    XUnmapWindow(w2k.dpy, c->frame);
    wm_set_state(c->win, IconicState);
    client_publish_state(c);
    if (focused == c) {
        focused = NULL;
        /* Hand focus to the next visible window in stacking order. */
        for (Client *n = stack; n; n = n->snext)
            if (!n->minimized && n->mapped) { client_focus(n); break; }
        if (!focused) client_focus(NULL);
    }
    clients_restack();
    taskbar_paint();
}

void client_restore(Client *c)
{
    /* ...and back out of it on the way in. */
    int bx, by, bw, bh;
    if (c && c->minimized && taskbar_button_rect(c, &bx, &by, &bw, &bh))
        wm_animate_rect(bx, by, bw, bh,
                        c->x - client_border(c),
                        c->y - client_border(c) - client_caption_h(c),
                        client_frame_w(c), client_frame_h(c));

    if (!c) return;
    if (c->minimized) {
        c->minimized = 0;
        XMapWindow(w2k.dpy, c->frame);
        wm_set_state(c->win, NormalState);
        client_publish_state(c);
    }
    client_raise(c);
    client_focus(c);
    taskbar_paint();
}

void client_maximize(Client *c, int on)
{
    if (!c || !c->decorate) return;
    if (on && !c->maximized) {
        c->rx = c->x; c->ry = c->y; c->rw = c->w; c->rh = c->h;
        c->maximized = 1;
        /* Fills the monitor the window is on, not the whole desktop. */
        int wx, wy, ww, wh;
        wm_workarea_of_client(c, &wx, &wy, &ww, &wh);
        int b = client_border(c), cap = client_caption_h(c);
        client_move_resize(c, wx + b, wy + b + cap,
                           ww - 2 * b, wh - 2 * b - cap);
    } else if (!on && c->maximized) {
        c->maximized = 0;
        client_move_resize(c, c->rx, c->ry, c->rw, c->rh);
    }
    client_publish_state(c);
    frame_paint(c);
}

/* Full screen: the window covers its monitor, taskbar and all, with no
 * frame (client_border and client_caption_h are 0 while it lasts). Media
 * players and browsers ask for this through _NET_WM_STATE. */
void client_fullscreen(Client *c, int on)
{
    if (!c) return;
    if (on && !c->fullscreen) {
        if (!c->maximized) { c->rx = c->x; c->ry = c->y; c->rw = c->w; c->rh = c->h; }
        c->fullscreen = 1;
        const W2kMonitor *m = w2k_monitor_at(c->x + c->w / 2, c->y + c->h / 2);
        int mx = m ? m->x : 0, my = m ? m->y : 0;
        int mw = m ? m->w : w2k.sw, mh = m ? m->h : w2k.sh;
        client_move_resize(c, mx, my, mw, mh);
        client_raise(c);
    } else if (!on && c->fullscreen) {
        c->fullscreen = 0;
        if (c->maximized) {
            c->maximized = 0;           /* re-applied below, at frame size */
            client_maximize(c, 1);
        } else
            client_move_resize(c, c->rx, c->ry, c->rw, c->rh);
        clients_restack();
    }
    client_publish_state(c);
    frame_shape(c);
    frame_paint(c);
}

/* ------------------------------------------------------------------ *
 * manage / unmanage
 * ------------------------------------------------------------------ */
static void place_new(Client *c, int had_position)
{
    int b = client_border(c), cap = client_caption_h(c);
    int fw = client_frame_w(c), fh = client_frame_h(c);

    /* Windows open on the monitor the user is working on -- the one under
     * the pointer -- even when the application asks for a position on
     * another screen (apps that remember where they were last time do this
     * constantly, and the answer is rarely the screen being used now). The
     * requested position is carried across as an offset within the monitor,
     * so a window that wanted to be near the top-left still is.
     *
     * Dialogs are the exception: they were positioned against the window
     * they belong to, so they stay with it.
     *
     * Either way a single monitor's work area bounds the window -- never the
     * union of them all, which would let one open in the gap between two
     * screens or be sized to their combined width. */
    const W2kMonitor *src = had_position
        ? w2k_monitor_at(c->x + c->w / 2, c->y + c->h / 2) : NULL;
    const W2kMonitor *dst = (had_position && c->is_dialog)
        ? src : w2k_monitor_of_pointer();

    if (had_position && src != dst) {
        c->x += dst->x - src->x;
        c->y += dst->y - src->y;
    }

    int ax, ay, aw, ah;
    wm_workarea_of(dst, &ax, &ay, &aw, &ah);

    if (fw > aw) { c->w = aw - 2 * b; fw = client_frame_w(c); }
    if (fh > ah) { c->h = ah - 2 * b - cap; fh = client_frame_h(c); }

    if (had_position) {
        /* The position an application asks for is where its *frame* goes:
         * that is what the default north-west gravity means, and it is why
         * a dialog that centred itself used to sit a caption's height too
         * high once it was framed. A window asking for StaticGravity means
         * the client area itself, and gets it. */
        int fx = c->static_gravity ? c->x - b : c->x;
        int fy = c->static_gravity ? c->y - b - cap : c->y;
        if (fx + fw > ax + aw) fx = ax + aw - fw;
        if (fy + fh > ay + ah) fy = ay + ah - fh;
        if (fx < ax) fx = ax;
        if (fy < ay) fy = ay;
        c->x = fx + b; c->y = fy + b + cap;
        return;
    }

    if (c->is_dialog) {                    /* dialogs open centred */
        int fx = ax + (aw - fw) / 2, fy = ay + (ah - fh) / 2;
        c->x = fx + b; c->y = fy + b + cap;
        return;
    }

    /* Otherwise cascade, exactly like the shell's default window placement. */
    static int step;
    int off = (step % 8) * 24;
    int fx = ax + 4 + off, fy = ay + 4 + off;
    if (fx + fw > ax + aw || fy + fh > ay + ah) {
        step = 0;
        fx = ax + 4; fy = ay + 4;
    }
    step++;
    c->x = fx + b; c->y = fy + b + cap;
}

/* _NET_FRAME_EXTENTS: how much border the window wears on each side.
 * Toolkits read it to work out where their window really is -- without it
 * an application that remembers its position and asks for it again drifts
 * down the screen by one frame each time it opens. */
static void client_publish_extents(Client *c)
{
    int b = client_border(c), cap = client_caption_h(c);
    long v[4] = { b, b, b + cap, b };      /* left, right, top, bottom */
    XChangeProperty(w2k.dpy, c->win, w2k.a_net_frame_extents, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)v, 4);
}

void client_manage(Window w, int initial_map)
{
    if (client_find(w)) return;
    if (w == taskbar_window() || w == desktop_window() || w == wm_check) return;
    if (tray_owns(w)) return;           /* a docked notification icon */

    XWindowAttributes wa;
    if (!XGetWindowAttributes(w2k.dpy, w, &wa)) return;
    if (wa.override_redirect) return;
    if (!initial_map && wa.map_state != IsViewable) return;

    Client *c = w2k_alloc(sizeof *c);
    c->win = w;
    c->x = wa.x; c->y = wa.y;
    c->w = wa.width > 0 ? wa.width : 320;
    c->h = wa.height > 0 ? wa.height : 200;

    /* WM_CLASS is two strings: the instance (Firefox's is "Navigator")
     * and the class ("Firefox"). The instance is the better key for icon
     * themes, the class is the application's name -- keep both, or a pin
     * ends up called Navigator. */
    XClassHint ch = { NULL, NULL };
    if (XGetClassHint(w2k.dpy, w, &ch)) {
        if (ch.res_name)  c->cls = w2k_strdup(ch.res_name);
        if (ch.res_class) c->cls_name = w2k_strdup(ch.res_class);
        if (!c->cls && c->cls_name) c->cls = w2k_strdup(c->cls_name);
        if (ch.res_name)  XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
    c->icon = icon_for_client(c);

    client_update_hints(c);
    client_update_type(c);
    client_update_name(c);

    XSizeHints sh;
    long sup;
    int had_pos = 0;
    if (XGetWMNormalHints(w2k.dpy, w, &sh, &sup)) {
        if ((sh.flags & (USPosition | PPosition)) && (wa.x || wa.y))
            had_pos = 1;
        if ((sh.flags & PWinGravity) && sh.win_gravity == StaticGravity)
            c->static_gravity = 1;
    }
    place_new(c, had_pos);

    int b = client_border(c), cap = client_caption_h(c);
    XSetWindowAttributes fa = {
        .background_pixel  = w2k.col[C_FACE],
        .border_pixel      = w2k.col[C_BLACK],
        .override_redirect = False,
        .event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                      ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                      ExposureMask | EnterWindowMask | LeaveWindowMask
    };
    c->frame = XCreateWindow(w2k.dpy, w2k.root,
                             c->x - b, c->y - b - cap,
                             client_frame_w(c), client_frame_h(c), 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWBorderPixel | CWOverrideRedirect |
                             CWEventMask, &fa);

    /* Add to whatever this client already selected rather than replacing it.
     * Event masks are per client, so for another application's window the
     * old mask is empty and this is the same thing -- but the shell's own
     * dialogs (Run, Shut Down) are created by this process, and replacing
     * their mask here silently took away KeyPressMask and ButtonPressMask.
     * That is why the Run box could not be typed into. */
    XSelectInput(w2k.dpy, w, wa.your_event_mask |
                             PropertyChangeMask | StructureNotifyMask);
    XSetWindowBorderWidth(w2k.dpy, w, 0);
    XAddToSaveSet(w2k.dpy, w);
    /* Reparenting a mapped window unmaps and remaps it; that UnmapNotify is
     * ours, not a withdrawal, so remember to swallow it. */
    if (wa.map_state == IsViewable) c->ignore_unmap++;
    XReparentWindow(w2k.dpy, w, c->frame, b, b + cap);

    /* Clicking anywhere in an unfocused client should focus it, but the click
     * must still reach the application: sync-grab, focus, then replay.
     *
     * Not on our own windows: the replay is issued from the ButtonPress
     * handler below, which never runs for them -- their events are consumed
     * by the toolkit's dispatcher first -- so the grab would freeze the
     * pointer with nothing left to thaw it. They are focused already anyway,
     * being modal. */
    if (!w2k_win_owns(w))
        XGrabButton(w2k.dpy, Button1, AnyModifier, w, False,
                    ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);

    c->next = clients;
    clients = c;
    c->snext = stack;
    stack = c;

    client_publish_extents(c);
    XMapWindow(w2k.dpy, w);
    XMapWindow(w2k.dpy, c->frame);
    c->mapped = 1;
    wm_set_state(w, NormalState);
    client_move_resize(c, c->x, c->y, c->w, c->h);
    if (c->maximized) { c->maximized = 0; client_maximize(c, 1); }

    client_raise(c);
    client_focus(c);
    wm_update_client_list();
    taskbar_sync();
}

void client_unmanage(Client *c, int destroyed)
{
    if (!c) return;
    if (c->capbuf) { w2k_free_pixmap(c->capbuf); c->capbuf = 0; }

    if (!destroyed) {
        /* The client may already be gone; swallow errors rather than
         * letting Xlib's default handler abort the whole session. */
        XGrabServer(w2k.dpy);
        XSetErrorHandler(wm_xerror_ignore);
        XUngrabButton(w2k.dpy, Button1, AnyModifier, c->win);
        XSetWindowBorderWidth(w2k.dpy, c->win, 0);
        /* Hand the window back to the root at its true position. */
        int ub = c->static_gravity ? 0 : client_border(c);
        int ucap = c->static_gravity ? 0 : client_caption_h(c);
        XReparentWindow(w2k.dpy, c->win, w2k.root, c->x - ub, c->y - ub - ucap);
        XRemoveFromSaveSet(w2k.dpy, c->win);
        wm_set_state(c->win, WithdrawnState);
        XSync(w2k.dpy, False);
        XSetErrorHandler(wm_xerror);
        XUngrabServer(w2k.dpy);
    }
    XDestroyWindow(w2k.dpy, c->frame);

    Client **p = &clients;
    while (*p) { if (*p == c) { *p = c->next; break; } p = &(*p)->next; }
    Client **q = &stack;
    while (*q) { if (*q == c) { *q = c->snext; break; } q = &(*q)->snext; }

    if (focused == c) {
        focused = NULL;
        for (Client *n = stack; n; n = n->snext)
            if (!n->minimized && n->mapped) { client_focus(n); break; }
        if (!focused) client_focus(NULL);
    }
    free(c->name);
    free(c->cls);
    free(c->cls_name);
    free(c);
    if (!clients) wm_logout_check();
    wm_update_client_list();
    taskbar_sync();
    clients_restack();
}
