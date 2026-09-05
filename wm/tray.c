/* tray.c -- the notification area.
 *
 * Implements the freedesktop system tray protocol, which is how everything
 * from a network monitor to a chat client puts an icon next to the clock:
 *
 *   1. own the _NET_SYSTEM_TRAY_S<screen> selection, and announce it;
 *   2. a client sends SYSTEM_TRAY_REQUEST_DOCK naming its window;
 *   3. we reparent that window into the taskbar, size it, map it and tell
 *      it it has been embedded (XEMBED_EMBEDDED_NOTIFY);
 *   4. when it goes away, close the gap.
 *
 * The icons are real windows owned by their applications -- they draw
 * themselves. All the taskbar does is give each one a slot. */
#include "wm.h"
#include <stdio.h>
#include <string.h>

#define TRAY_ICON   16       /* the size every icon is given */
#define TRAY_GAP     2
#define MAX_TRAY    24

/* system tray protocol */
#define SYSTEM_TRAY_REQUEST_DOCK 0

/* XEMBED */
#define XEMBED_EMBEDDED_NOTIFY   0
#define XEMBED_VERSION           0

static Window manager;               /* owns the selection            */
static Window parent;                /* the taskbar                   */
static struct { Window win; int mapped; } icons[MAX_TRAY];
static int nicons;
static Atom a_tray_sel, a_tray_opcode, a_tray_orientation, a_xembed,
            a_xembed_info, a_manager;

int tray_count(void) { return nicons; }

/* Does the tray own this window? The window manager must not try to manage
 * a docked icon as an ordinary application window. */
int tray_owns(Window w)
{
    for (int i = 0; i < nicons; i++)
        if (icons[i].win == w) return 1;
    return 0;
}

/* Width the notification area needs, or 0 when it is empty. */
int tray_width(void)
{
    if (!nicons) return 0;
    return nicons * TRAY_ICON + (nicons - 1) * TRAY_GAP;
}

static void tray_remove(int i)
{
    if (i < 0 || i >= nicons) return;
    for (int k = i; k < nicons - 1; k++) icons[k] = icons[k + 1];
    nicons--;
}

static int tray_find(Window w)
{
    for (int i = 0; i < nicons; i++)
        if (icons[i].win == w) return i;
    return -1;
}

/* Put the icons in a row starting at x, vertically centred in a bar of
 * height h. Called from the taskbar's layout. */
void tray_layout(int x, int y, int h)
{
    int iy = y + (h - TRAY_ICON) / 2;
    for (int i = 0; i < nicons; i++) {
        int ix = x + i * (TRAY_ICON + TRAY_GAP);
        XMoveResizeWindow(w2k.dpy, icons[i].win, w2k_px(ix), w2k_px(iy),
                          (unsigned)w2k_px(TRAY_ICON), (unsigned)w2k_px(TRAY_ICON));
        if (!icons[i].mapped) {
            XMapRaised(w2k.dpy, icons[i].win);
            icons[i].mapped = 1;
        }
    }
}

/* The same, down a column (the bar at the left or right). */
void tray_layout_column(int x, int y)
{
    for (int i = 0; i < nicons; i++) {
        int iy = y + i * (TRAY_ICON + TRAY_GAP);
        XMoveResizeWindow(w2k.dpy, icons[i].win, w2k_px(x), w2k_px(iy),
                          (unsigned)w2k_px(TRAY_ICON), (unsigned)w2k_px(TRAY_ICON));
        if (!icons[i].mapped) {
            XMapRaised(w2k.dpy, icons[i].win);
            icons[i].mapped = 1;
        }
    }
}

static void dock(Window w)
{
    if (nicons >= MAX_TRAY || tray_find(w) >= 0) return;

    XWindowAttributes wa;
    if (!XGetWindowAttributes(w2k.dpy, w, &wa)) return;

    icons[nicons].win = w;
    icons[nicons].mapped = 0;
    nicons++;

    XSelectInput(w2k.dpy, w, StructureNotifyMask | PropertyChangeMask);
    XReparentWindow(w2k.dpy, w, parent, 0, 0);
    XResizeWindow(w2k.dpy, w, (unsigned)w2k_px(TRAY_ICON), (unsigned)w2k_px(TRAY_ICON));

    /* Tell the client it is embedded; without this many toolkits never
     * draw anything. */
    XEvent e = { 0 };
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = a_xembed;
    e.xclient.format = 32;
    e.xclient.data.l[0] = CurrentTime;
    e.xclient.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
    e.xclient.data.l[2] = 0;
    e.xclient.data.l[3] = (long)parent;
    e.xclient.data.l[4] = XEMBED_VERSION;
    XSendEvent(w2k.dpy, w, False, NoEventMask, &e);

    taskbar_paint();                 /* the bar has to make room */
}

/* Claim the tray selection and tell everybody it exists. */
void tray_init(Window taskbar)
{
    parent = taskbar;

    char name[64];
    snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", w2k.screen);
    a_tray_sel         = XInternAtom(w2k.dpy, name, False);
    a_tray_opcode      = XInternAtom(w2k.dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    a_tray_orientation = XInternAtom(w2k.dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
    a_xembed           = XInternAtom(w2k.dpy, "_XEMBED", False);
    a_xembed_info      = XInternAtom(w2k.dpy, "_XEMBED_INFO", False);
    a_manager          = XInternAtom(w2k.dpy, "MANAGER", False);

    if (XGetSelectionOwner(w2k.dpy, a_tray_sel) != None) {
        fprintf(stderr, "l2kwm: another system tray is running\n");
        return;
    }

    manager = XCreateSimpleWindow(w2k.dpy, w2k.root, -1, -1, 1, 1, 0, 0, 0);
    long orientation = 0;            /* horizontal */
    XChangeProperty(w2k.dpy, manager, a_tray_orientation, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&orientation, 1);
    XSetSelectionOwner(w2k.dpy, a_tray_sel, manager, CurrentTime);
    if (XGetSelectionOwner(w2k.dpy, a_tray_sel) != manager) {
        XDestroyWindow(w2k.dpy, manager);
        manager = None;
        return;
    }

    XEvent e = { 0 };
    e.xclient.type = ClientMessage;
    e.xclient.window = w2k.root;
    e.xclient.message_type = a_manager;
    e.xclient.format = 32;
    e.xclient.data.l[0] = CurrentTime;
    e.xclient.data.l[1] = (long)a_tray_sel;
    e.xclient.data.l[2] = (long)manager;
    XSendEvent(w2k.dpy, w2k.root, False, StructureNotifyMask, &e);
}

/* Returns 1 if the event was the tray's business. */
int tray_event(XEvent *e)
{
    if (!manager) return 0;

    switch (e->type) {
    case ClientMessage:
        if (e->xclient.message_type == a_tray_opcode &&
            e->xclient.data.l[1] == SYSTEM_TRAY_REQUEST_DOCK) {
            dock((Window)e->xclient.data.l[2]);
            return 1;
        }
        return 0;

    case DestroyNotify: {
        int i = tray_find(e->xdestroywindow.window);
        if (i < 0) return 0;
        tray_remove(i);
        taskbar_paint();
        return 1;
    }
    case UnmapNotify: {
        int i = tray_find(e->xunmap.window);
        if (i < 0) return 0;
        /* A tray icon that unmaps itself is gone: reparent it back to the
         * root so the application still owns a live window. */
        icons[i].mapped = 0;
        XReparentWindow(w2k.dpy, icons[i].win, w2k.root, 0, 0);
        tray_remove(i);
        taskbar_paint();
        return 1;
    }
    case ConfigureRequest: {
        /* Icons sometimes ask to resize; they get the slot they are given. */
        int i = tray_find(e->xconfigurerequest.window);
        if (i < 0) return 0;
        XResizeWindow(w2k.dpy, icons[i].win, (unsigned)w2k_px(TRAY_ICON),
                      (unsigned)w2k_px(TRAY_ICON));
        return 1;
    }
    case SelectionClear:
        if (e->xselectionclear.selection == a_tray_sel) {
            manager = None;
            return 1;
        }
        return 0;
    }
    return 0;
}

void tray_fini(void)
{
    for (int i = 0; i < nicons; i++)
        XReparentWindow(w2k.dpy, icons[i].win, w2k.root, 0, 0);
    nicons = 0;
    if (manager) {
        XDestroyWindow(w2k.dpy, manager);
        manager = None;
    }
}
