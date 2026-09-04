/* dnd.c -- drag and drop, the XDND protocol.
 *
 * Dragging files between our own windows could have been done with a
 * private message, but then nothing could be dragged to or from Firefox,
 * a terminal, or any other application. XDND is what everything else on
 * an X desktop speaks, so this speaks it too -- version 5, with
 * text/uri-list as the only data type, which is what file managers
 * exchange.
 *
 * The shape of a drag:
 *
 *   source                          target
 *   ------                          ------
 *   own XdndSelection
 *   XdndEnter        ------------>  note the types offered
 *   XdndPosition     ------------>
 *                    <-----------   XdndStatus (will I accept, and where)
 *   XdndDrop         ------------>  ask for the selection
 *                    <-----------   SelectionRequest
 *   send the URIs    ----------->
 *                    <-----------   XdndFinished
 *
 * The caller drives it: it owns the pointer grab (it started the drag from
 * a button press) and hands us each motion and button event. */
#include "w2k.h"
#include <X11/cursorfont.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XDND_VERSION 5

static Atom a_aware, a_selection, a_enter, a_leave, a_position, a_status,
            a_drop, a_finished, a_actioncopy, a_actionmove, a_actionprivate,
            a_typelist, a_urilist, a_prop;

/* Source state, while a drag is in flight. */
static Window src_win;              /* the window the drag started from */
static Window cur_target;           /* the XDND-aware window under the pointer */
static int    cur_version;
static int    target_accepts;
static char  *drag_data;            /* the URI list being offered */
static int    drag_move;            /* move rather than copy */

/* Target state, for a drag arriving over one of our windows. */
static Window in_source;
static int    in_ok;
static int    in_x, in_y;           /* the last position, window-relative */
static int    in_move;              /* the source asked for a move */
static Window in_window;            /* the window it was over */
static Time   last_time = CurrentTime;
static Cursor cur_nodrop;

void w2k_dnd_set_time(Time t) { last_time = t; }

void (*w2k_dnd_on_drop)(Window w, int x, int y, const char *uris, int move);
int  (*w2k_dnd_will_accept)(Window w, int x, int y);

static void atoms(void)
{
    if (a_aware) return;
    a_aware      = XInternAtom(w2k.dpy, "XdndAware", False);
    a_selection  = XInternAtom(w2k.dpy, "XdndSelection", False);
    a_enter      = XInternAtom(w2k.dpy, "XdndEnter", False);
    a_leave      = XInternAtom(w2k.dpy, "XdndLeave", False);
    a_position   = XInternAtom(w2k.dpy, "XdndPosition", False);
    a_status     = XInternAtom(w2k.dpy, "XdndStatus", False);
    a_drop       = XInternAtom(w2k.dpy, "XdndDrop", False);
    a_finished   = XInternAtom(w2k.dpy, "XdndFinished", False);
    a_actioncopy = XInternAtom(w2k.dpy, "XdndActionCopy", False);
    a_actionmove = XInternAtom(w2k.dpy, "XdndActionMove", False);
    a_actionprivate = XInternAtom(w2k.dpy, "XdndActionPrivate", False);
    a_typelist   = XInternAtom(w2k.dpy, "XdndTypeList", False);
    a_urilist    = XInternAtom(w2k.dpy, "text/uri-list", False);
    a_prop       = XInternAtom(w2k.dpy, "_W2K_DND", False);
}

/* Mark a window as willing to receive drops. */
void w2k_dnd_accept(Window w)
{
    atoms();
    long version = XDND_VERSION;
    XChangeProperty(w2k.dpy, w, a_aware, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&version, 1);
}

static void send_client(Window to, Atom type, long d0, long d1, long d2,
                        long d3, long d4)
{
    XEvent e = { 0 };
    e.xclient.type = ClientMessage;
    e.xclient.window = to;
    e.xclient.message_type = type;
    e.xclient.format = 32;
    e.xclient.data.l[0] = d0;
    e.xclient.data.l[1] = d1;
    e.xclient.data.l[2] = d2;
    e.xclient.data.l[3] = d3;
    e.xclient.data.l[4] = d4;
    XSendEvent(w2k.dpy, to, False, NoEventMask, &e);
}

/* XdndAware version on a window, or 0. */
static int aware_version(Window w)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    int v = 0;
    if (XGetWindowProperty(w2k.dpy, w, a_aware, 0, 1, False, XA_ATOM, &type,
                           &fmt, &n, &after, &data) == Success && data) {
        if (fmt == 32 && n >= 1) v = (int)*(long *)data;
        XFree(data);
    }
    return v;
}

/* The XDND-aware window under the pointer: walk down from the root, since
 * the aware window is often an ancestor of whatever is directly under. */
static Window target_at(int rx, int ry, int *version)
{
    Window w = w2k.root, child;
    int x = rx, y = ry;
    for (int depth = 0; depth < 16; depth++) {
        int v = aware_version(w);
        if (v > 0 && w != src_win) { *version = v; return w; }
        int cx, cy;
        if (!XTranslateCoordinates(w2k.dpy, w2k.root, w, rx, ry, &cx, &cy,
                                   &child) || !child)
            break;
        w = child;
        x = cx;
        y = cy;
    }
    (void)x; (void)y;
    *version = 0;
    return None;
}

/* ------------------------------------------------------------------ *
 * Source
 * ------------------------------------------------------------------ */
int w2k_dnd_active(void) { return drag_data != NULL; }

void w2k_dnd_begin(Window from, const char *uri_list, int move)
{
    atoms();
    free(drag_data);
    drag_data = uri_list ? w2k_strdup(uri_list) : NULL;
    if (!drag_data) return;

    src_win = from;
    cur_target = None;
    cur_version = 0;
    target_accepts = 0;
    drag_move = move;
    XSetSelectionOwner(w2k.dpy, a_selection, from, CurrentTime);
}

/* Follow the pointer. Returns 1 if a target is willing to take the drop. */
int w2k_dnd_motion(int rx, int ry)
{
    if (!drag_data) return 0;

    int version = 0;
    Window t = target_at(rx, ry, &version);
    if (t != cur_target) {
        if (cur_target) send_client(cur_target, a_leave, (long)src_win, 0, 0, 0, 0);
        cur_target = t;
        cur_version = version;
        target_accepts = 0;
        if (cur_target)
            send_client(cur_target, a_enter, (long)src_win,
                        (long)((version < XDND_VERSION ? version : XDND_VERSION) << 24),
                        (long)a_urilist, 0, 0);
    }
    if (cur_target)
        send_client(cur_target, a_position, (long)src_win, 0,
                    (long)((rx << 16) | (ry & 0xffff)), (long)last_time,
                    (long)(drag_move ? a_actionmove : a_actioncopy));
    /* The pointer says whether the drop would be taken: the arrow over a
     * target, the "no" cursor over anything else. The caller holds the
     * grab, so this changes its cursor. */
    if (!cur_nodrop) cur_nodrop = w2k.cur_no ? w2k.cur_no : XCreateFontCursor(w2k.dpy, XC_circle);
    XChangeActivePointerGrab(w2k.dpy, ButtonReleaseMask | PointerMotionMask,
                             (cur_target && target_accepts) ? w2k.cur_arrow : cur_nodrop,
                             CurrentTime);
    return target_accepts;
}

/* The button came up: drop if anybody wants it. Returns 1 if dropped. */
int w2k_dnd_drop(void)
{
    if (!drag_data) return 0;
    int dropped = 0;
    if (cur_target && target_accepts) {
        send_client(cur_target, a_drop, (long)src_win, 0, (long)last_time, 0, 0);
        dropped = 1;
    } else if (cur_target) {
        send_client(cur_target, a_leave, (long)src_win, 0, 0, 0, 0);
    }
    cur_target = None;
    target_accepts = 0;
    /* drag_data stays until the target has asked for it. */
    return dropped;
}

void w2k_dnd_cancel(void)
{
    if (cur_target) send_client(cur_target, a_leave, (long)src_win, 0, 0, 0, 0);
    cur_target = None;
    free(drag_data);
    drag_data = NULL;
}

/* ------------------------------------------------------------------ *
 * Events -- both roles
 * ------------------------------------------------------------------ */
static int read_and_deliver(Window w, int x, int y, int move)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    int ok = 0;
    if (XGetWindowProperty(w2k.dpy, w, a_prop, 0, 65536, True,
                           AnyPropertyType, &type, &fmt, &n, &after,
                           &data) == Success && data) {
        if (type != a_urilist && type != w2k.a_utf8 && type != XA_STRING) {
            /* An INCR transfer, or something that is not text: refused
             * rather than parsed as a list of files. */
        } else if (w2k_dnd_on_drop) {
            w2k_dnd_on_drop(w, x, y, (const char *)data, move);
            ok = 1;
        }
        XFree(data);
    }
    return ok;
}

int w2k_dnd_event(XEvent *e)
{
    atoms();

    if (e->type == SelectionRequest &&
        e->xselectionrequest.selection == a_selection) {
        /* The target is asking for the files. */
        XSelectionRequestEvent *r = &e->xselectionrequest;
        XEvent n = { 0 };
        n.xselection.type = SelectionNotify;
        n.xselection.display = r->display;
        n.xselection.requestor = r->requestor;
        n.xselection.selection = r->selection;
        n.xselection.target = r->target;
        n.xselection.time = r->time;
        n.xselection.property = None;

        if (drag_data && r->target == a_urilist) {
            XChangeProperty(w2k.dpy, r->requestor, r->property, a_urilist, 8,
                            PropModeReplace, (unsigned char *)drag_data,
                            (int)strlen(drag_data));
            n.xselection.property = r->property;
        }
        XSendEvent(w2k.dpy, r->requestor, False, NoEventMask, &n);
        return 1;
    }

    if (e->type == SelectionNotify &&
        e->xselection.selection == a_selection) {
        /* Our request for the dropped files came back: deliver it where
         * the pointer last was, as the move or copy the source asked for. */
        /* XdndFinished says whether the drop was taken: a source doing a
         * move deletes its originals on a yes, so it must be true. */
        int ok = e->xselection.property != None &&
                 read_and_deliver(e->xselection.requestor, in_x, in_y, in_move);
        if (in_source)
            send_client(in_source, a_finished, (long)e->xselection.requestor,
                        ok, ok ? (long)(in_move ? a_actionmove : a_actioncopy) : 0,
                        0, 0);
        in_source = None;
        return 1;
    }

    if (e->type != ClientMessage) return 0;
    Atom mt = e->xclient.message_type;

    /* --- as the source --- */
    if (mt == a_status) {
        if ((Window)e->xclient.data.l[0] == cur_target)
            target_accepts = (e->xclient.data.l[1] & 1) != 0;
        return 1;
    }
    if (mt == a_finished) {
        free(drag_data);
        drag_data = NULL;
        return 1;
    }

    /* --- as the target --- */
    if (mt == a_enter) {
        in_source = (Window)e->xclient.data.l[0];
        in_ok = 0;
        /* Only text/uri-list is understood; look in the three inline slots
         * and, if the source said there were more, its type list. */
        for (int i = 2; i <= 4; i++)
            if ((Atom)e->xclient.data.l[i] == a_urilist) in_ok = 1;
        if (!in_ok && (e->xclient.data.l[1] & 1)) {
            Atom type;
            int fmt;
            unsigned long n, after;
            unsigned char *data = NULL;
            if (XGetWindowProperty(w2k.dpy, in_source, a_typelist, 0, 64, False,
                                   XA_ATOM, &type, &fmt, &n, &after,
                                   &data) == Success && data) {
                Atom *list = (Atom *)data;
                for (unsigned long i = 0; i < n; i++)
                    if (list[i] == a_urilist) in_ok = 1;
                XFree(data);
            }
        }
        return 1;
    }
    if (mt == a_position) {
        Window w = e->xclient.window;
        int rx = (int)(e->xclient.data.l[2] >> 16);
        int ry = (int)(e->xclient.data.l[2] & 0xffff);
        int wx = 0, wy = 0;
        Window child;
        XTranslateCoordinates(w2k.dpy, w2k.root, w, rx, ry, &wx, &wy, &child);

        int accept = in_ok;
        if (accept && w2k_dnd_will_accept) accept = w2k_dnd_will_accept(w, wx, wy);
        in_window = w;
        in_x = wx;
        in_y = wy;
        Atom action = (Atom)e->xclient.data.l[4];
        in_move = action == a_actionmove;
        /* No rectangle is offered, so we are asked again on every move --
         * which is what we want, since what is under the pointer decides.
         * The action answered is the one asked for, so a source that
         * offered a move knows the files went rather than were copied. */
        send_client((Window)e->xclient.data.l[0], a_status, (long)w,
                    accept ? 1 : 0, 0, 0,
                    (long)(accept ? (in_move ? a_actionmove : a_actioncopy) : None));
        return 1;
    }
    if (mt == a_leave) {
        in_source = None;
        in_ok = 0;
        return 1;
    }
    if (mt == a_drop) {
        Window w = e->xclient.window;
        if (!in_ok) {
            send_client((Window)e->xclient.data.l[0], a_finished, (long)w, 0,
                        0, 0, 0);
            in_source = None;
            return 1;
        }
        Time t = (Time)e->xclient.data.l[2];
        XConvertSelection(w2k.dpy, a_selection, a_urilist, a_prop, w,
                          t ? t : CurrentTime);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * URI lists
 * ------------------------------------------------------------------ */
/* "file:///home/jack/a%20file" -> "/home/jack/a file". Returns how many
 * paths were written. */
int w2k_uri_list_paths(const char *uris, char paths[][1024], int max)
{
    int n = 0;
    const char *p = uris;
    while (*p && n < max) {
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != '\r' && *end != '\n') end++;
        if (*p != '#' && !strncmp(p, "file://", 7)) {
            const char *s = p + 7;
            /* skip a hostname if there is one */
            const char *slash = memchr(s, '/', (size_t)(end - s));
            if (slash) s = slash;
            int o = 0;
            while (s < end && o < 1023) {
                if (*s == '%' && s + 2 < end) {
                    int hi = s[1], lo = s[2];
                    hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
                    lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
                    if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                        paths[n][o++] = (char)(hi * 16 + lo);
                        s += 3;
                        continue;
                    }
                }
                paths[n][o++] = *s++;
            }
            paths[n][o] = 0;
            if (o) n++;
        }
        p = end;
    }
    return n;
}

/* The reverse: build a URI list from paths, for a drag. Caller frees. */
char *w2k_uri_list_build(char paths[][1024], int n)
{
    /* Every byte may become %XX: room for that, so nothing truncates and
     * the offset never runs past the buffer. */
    size_t cap = (size_t)n * (7 + 3 * 1024 + 2) + 16;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        o += (size_t)snprintf(out + o, cap - o, "file://");
        for (const char *s = paths[i]; *s && o + 4 < cap; s++) {
            unsigned char c = (unsigned char)*s;
            if (c == '/' || c == '.' || c == '-' || c == '_' || c == '~' ||
                (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z'))
                out[o++] = (char)c;
            else
                o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
        }
        o += (size_t)snprintf(out + o, cap - o, "\r\n");
    }
    out[o] = 0;
    return out;
}
