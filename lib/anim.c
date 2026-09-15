/* anim.c -- the "slide" and "zoom" of the Visual Effects list, drawn
 * without a compositor and without flicker.
 *
 * The picture is painted once, before anything moves, and made the
 * window's background: the server then puts it up by itself wherever the
 * window newly shows, with no round trip to us and no frame where the
 * window is its plain background colour. The earlier slides resized the
 * window at every step, and every resize had the server wipe it to that
 * colour before the picture was copied back -- a flash per frame -- and
 * the combo box's grew an empty white box that was only painted once it
 * had stopped.
 *
 * Steps follow the clock, not a count: on a slow server the animation
 * takes the time it should, with fewer frames, rather than longer. */
#include "w2k.h"
#include <X11/extensions/shape.h>
#include <time.h>
#include <unistd.h>

/* Called between frames, when set: the window manager points it at
 * something that repaints its own windows' exposures -- the bar flying
 * off the taskbar uncovers the task button, which otherwise stayed a
 * blank patch until the flight was over. */
void (*w2k_anim_frame)(void);

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

static void shape_rect(Window win, int x, int y, int w, int h)
{
    XRectangle r = { (short)x, (short)y, (unsigned short)(w > 0 ? w : 1),
                     (unsigned short)(h > 0 ? h : 1) };
    XShapeCombineRectangles(w2k.dpy, win, ShapeBounding, 0, 0, &r, 1, ShapeSet, Unsorted);
}

void w2k_slide_in(Window win, Pixmap picture, int x, int y, int pw, int ph,
                  int upward, int ms)
{
    int ev, er;
    if (!XShapeQueryExtension(w2k.dpy, &ev, &er) || pw <= 0 || ph <= 0 || ms <= 0) {
        XMoveResizeWindow(w2k.dpy, win, x, y, (unsigned)pw, (unsigned)ph);
        XMapRaised(w2k.dpy, win);
        return;
    }
    if (picture) XSetWindowBackgroundPixmap(w2k.dpy, win, picture);
    /* The content moves in from the anchored edge: at first only its far
     * end shows, just inside the edge, and it slides to where it lands.
     * The window is full size throughout and shaped to the part in view. */
    long t0 = now_us(), dur = (long)ms * 1000L;
    int first = 1;
    for (;;) {
        long el = now_us() - t0;
        double t = el >= dur ? 1.0 : (double)el / (double)dur;
        int sh = (int)(ph * ease(t) + 0.5);
        if (sh < 1) sh = 1;
        if (sh > ph) sh = ph;
        int wy = upward ? y + (ph - sh) : y - (ph - sh);
        shape_rect(win, 0, upward ? 0 : ph - sh, pw, sh);
        XMoveResizeWindow(w2k.dpy, win, x, wy, (unsigned)pw, (unsigned)ph);
        if (first) { XMapRaised(w2k.dpy, win); first = 0; }
        XSync(w2k.dpy, False);
        if (t >= 1.0) break;
        usleep(8000);
    }
    XShapeCombineMask(w2k.dpy, win, ShapeBounding, 0, 0, None, ShapeSet);
    XMoveResizeWindow(w2k.dpy, win, x, y, (unsigned)pw, (unsigned)ph);
    XFlush(w2k.dpy);
}

/* A bar in the caption's colours flying from one rectangle to another --
 * a window to its task button when it is minimised, and back -- the way
 * Windows 2000 animates a caption. A window of its own rather than an XOR
 * outline on the root: that needed the whole server grabbed for the
 * length of the flight (every other program stopped), left trails where
 * anything repainted underneath, and could not be seen through the
 * nested compositor at all. */
void w2k_zoom_rect(int fx, int fy, int fw, int fh, int tx, int ty, int tw, int th,
                   int ms, unsigned long c1, unsigned long c2)
{
    if (fw < 2 || fh < 2 || tw < 2 || th < 2 || ms <= 0) return;
    XSetWindowAttributes a = { .override_redirect = True, .background_pixel = c1,
                               .save_under = True };
    Window win = XCreateWindow(w2k.dpy, w2k.root, fx, fy, (unsigned)fw, (unsigned)fh, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWSaveUnder, &a);
    /* The caption gradient, as one row tiled down the bar. */
    int gw = fw > tw ? fw : tw;
    Pixmap grad = XCreatePixmap(w2k.dpy, win, (unsigned)gw, 1, w2k.depth);
    if (grad) {
        XColor a1 = { .pixel = c1 }, a2 = { .pixel = c2 };
        XQueryColor(w2k.dpy, w2k.cmap, &a1);
        XQueryColor(w2k.dpy, w2k.cmap, &a2);
        for (int i = 0; i < gw; i++) {
            int r = (a1.red >> 8) + ((a2.red >> 8) - (a1.red >> 8)) * i / (gw > 1 ? gw - 1 : 1);
            int g = (a1.green >> 8) + ((a2.green >> 8) - (a1.green >> 8)) * i / (gw > 1 ? gw - 1 : 1);
            int b = (a1.blue >> 8) + ((a2.blue >> 8) - (a1.blue >> 8)) * i / (gw > 1 ? gw - 1 : 1);
            XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
            XDrawPoint(w2k.dpy, grad, w2k.gc, i, 0);
        }
        XSetWindowBackgroundPixmap(w2k.dpy, win, grad);
    }
    long t0 = now_us(), dur = (long)ms * 1000L;
    int first = 1;
    for (;;) {
        long el = now_us() - t0;
        double t = el >= dur ? 1.0 : (double)el / (double)dur;
        double e = ease(t);
        int x = fx + (int)((tx - fx) * e), y = fy + (int)((ty - fy) * e);
        int w = fw + (int)((tw - fw) * e), h = fh + (int)((th - fh) * e);
        XMoveResizeWindow(w2k.dpy, win, x, y, (unsigned)(w > 1 ? w : 1), (unsigned)(h > 1 ? h : 1));
        if (first) { XMapRaised(w2k.dpy, win); first = 0; }
        XSync(w2k.dpy, False);
        if (w2k_anim_frame) w2k_anim_frame();
        if (t >= 1.0) break;
        usleep(8000);
    }
    XDestroyWindow(w2k.dpy, win);
    if (grad) w2k_free_pixmap(grad);
    XFlush(w2k.dpy);
}
