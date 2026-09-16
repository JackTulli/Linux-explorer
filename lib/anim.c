/* anim.c -- the "slide" of the Visual Effects list (menus, combo boxes),
 * drawn without a compositor and without disturbing anything around it.
 *
 * The window is mapped at the place it ends up, and never anywhere else:
 * what was on the screen there is copied first and made its background,
 * so it appears showing exactly what it covers, and the finished picture
 * is then copied in over that, a growing band of it at each step, moving
 * out from the edge it hangs from. Nothing outside the window's rectangle
 * is ever covered, so nothing has to repaint -- the taskbar under a Start
 * menu stays put -- and nothing inside it is ever cleared to a plain
 * colour, so nothing flashes. This is how Windows animated a menu before
 * compositors: a copy of the screen beneath and blits over it.
 *
 * Earlier attempts each broke one of those. Growing the window had the
 * server wipe every new strip to the background colour before the picture
 * arrived. Moving a full-size shaped window in from above its place
 * uncovered the taskbar and whatever else it passed over, all left blank
 * until the program that owned them caught up -- the window manager, busy
 * running the very animation, last of all.
 *
 * Under a compositor the screen beneath cannot be read like this (the
 * windows are drawn off screen), so there the window simply appears.
 *
 * Steps follow the clock, not a count: on a slow server the animation
 * takes the time it should, with fewer frames, rather than longer. */
#include "w2k.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

/* Ease out: quick at first, settling at the end. */
static double ease(double t)
{
    double u = 1.0 - t;
    return 1.0 - u * u * u;
}

/* A compositing manager draws the windows itself, off the screen: then the
 * screen's own pixels are not what the user sees. */
static int composited(void)
{
    if (w2k_compositor == COMPOSITOR_COMPOSITE) return 1;
    char name[32];
    snprintf(name, sizeof name, "_NET_WM_CM_S%d", DefaultScreen(w2k.dpy));
    Atom a = XInternAtom(w2k.dpy, name, True);
    return a != None && XGetSelectionOwner(w2k.dpy, a) != None;
}

void w2k_slide_in(Window win, Pixmap picture, int x, int y, int pw, int ph,
                  int upward, int ms)
{
    if (pw <= 0 || ph <= 0) return;
    XMoveResizeWindow(w2k.dpy, win, x, y, (unsigned)pw, (unsigned)ph);
    if (!picture || ms <= 0 || composited()) {
        if (picture) XSetWindowBackgroundPixmap(w2k.dpy, win, picture);
        XMapRaised(w2k.dpy, win);
        return;
    }
    Pixmap under = XCreatePixmap(w2k.dpy, win, (unsigned)pw, (unsigned)ph, w2k.depth);
    XGCValues gv = { .subwindow_mode = IncludeInferiors, .graphics_exposures = False };
    GC gc = XCreateGC(w2k.dpy, win, GCSubwindowMode | GCGraphicsExposures, &gv);
    XCopyArea(w2k.dpy, w2k.root, under, gc, x, y, (unsigned)pw, (unsigned)ph, 0, 0);
    XSetWindowBackgroundPixmap(w2k.dpy, win, under);
    XMapRaised(w2k.dpy, win);

    long t0 = now_us(), dur = (long)ms * 1000L;
    for (;;) {
        long el = now_us() - t0;
        double t = el >= dur ? 1.0 : (double)el / (double)dur;
        int sh = (int)(ph * ease(t) + 0.5);
        if (sh > ph) sh = ph;
        if (sh > 0) {
            if (upward)        /* rising from its bottom edge: its top shows first */
                XCopyArea(w2k.dpy, picture, win, gc, 0, 0, (unsigned)pw, (unsigned)sh, 0, ph - sh);
            else               /* hanging from its top edge: its bottom shows first */
                XCopyArea(w2k.dpy, picture, win, gc, 0, ph - sh, (unsigned)pw, (unsigned)sh, 0, 0);
        }
        XSync(w2k.dpy, False);
        if (t >= 1.0) break;
        usleep(8000);
    }
    /* Exposures from here on are the finished picture's. */
    XSetWindowBackgroundPixmap(w2k.dpy, win, picture);
    XFreeGC(w2k.dpy, gc);
    XFreePixmap(w2k.dpy, under);
}
