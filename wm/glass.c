/* glass.c -- Aero glass that shows the windows behind it.
 *
 * lib/aero.c draws its glass from the wallpaper, blurred, because without
 * a compositor nothing can be seen through a window. With the "Show
 * windows through Aero glass" effect on, the X Composite extension keeps
 * every top-level window's picture in a pixmap of its own (automatic
 * redirection: the server still puts the screen together itself), and
 * the glass is drawn from what is stacked under it: the desktop, then
 * each window below the one being painted, gathered, blurred and handed
 * to the law. It costs a few round trips per repaint, so it is off
 * unless asked for. */
#include "wm.h"
#include <X11/extensions/Xcomposite.h>
#include <stdlib.h>
#include <string.h>

static int have, enabled;

static int quiet(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

/* The pixel format of an XImage, for reading it. */
static void masks(XImage *im, int *rs, int *gs, int *bs, int *rb, int *gb, int *bb)
{
    /* An image read from a pixmap carries no masks: the screen's apply. */
    unsigned long rm = im->red_mask ? im->red_mask : w2k.visual->red_mask;
    unsigned long gm = im->green_mask ? im->green_mask : w2k.visual->green_mask;
    unsigned long bm = im->blue_mask ? im->blue_mask : w2k.visual->blue_mask;
    for (*rs = 0; *rs < 32 && !((rm >> *rs) & 1); (*rs)++) ;
    for (*gs = 0; *gs < 32 && !((gm >> *gs) & 1); (*gs)++) ;
    for (*bs = 0; *bs < 32 && !((bm >> *bs) & 1); (*bs)++) ;
    for (*rb = 0; *rb < 32 && ((rm >> (*rs + *rb)) & 1); (*rb)++) ;
    for (*gb = 0; *gb < 32 && ((gm >> (*gs + *gb)) & 1); (*gb)++) ;
    for (*bb = 0; *bb < 32 && ((bm >> (*bs + *bb)) & 1); (*bb)++) ;
}

static void blur(unsigned char *p, int w, int h, int rad)
{
    unsigned char *tmp = malloc((size_t)w * h * 3);
    if (!tmp) return;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++)
            for (int c = 0; c < 3; c++) {
                const unsigned char *row = p + (size_t)y * w * 3;
                int sum = 0, n = 0;
                for (int x = -rad; x <= rad; x++) { int xx = x < 0 ? 0 : x >= w ? w - 1 : x; sum += row[xx * 3 + c]; n++; }
                for (int x = 0; x < w; x++) {
                    tmp[((size_t)y * w + x) * 3 + c] = (unsigned char)(sum / n);
                    int xo = x - rad, xi = x + rad + 1;
                    sum -= row[(xo < 0 ? 0 : xo) * 3 + c];
                    sum += row[(xi >= w ? w - 1 : xi) * 3 + c];
                }
            }
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                int sum = 0, n = 0;
                for (int y = -rad; y <= rad; y++) { int yy = y < 0 ? 0 : y >= h ? h - 1 : y; sum += tmp[((size_t)yy * w + x) * 3 + c]; n++; }
                for (int y = 0; y < h; y++) {
                    p[((size_t)y * w + x) * 3 + c] = (unsigned char)(sum / n);
                    int yo = y - rad, yi = y + rad + 1;
                    sum -= tmp[((size_t)(yo < 0 ? 0 : yo) * w + x) * 3 + c];
                    sum += tmp[((size_t)(yi >= h ? h - 1 : yi) * w + x) * 3 + c];
                }
            }
    }
    free(tmp);
}

/* What lies under the root rectangle, below w2k_glass_above (the window
 * being painted), blurred: RGB, malloc'd; NULL to fall back on the
 * wallpaper. */
/* The stacking order, held for the length of one batch: a frame asks for
 * four pieces and each used to re-walk the tree and re-ask for every
 * window's geometry. Batches nest, and one can paint several windows
 * (every frame and then the bar, after a raise): the root's children are
 * listed once and each is asked for its geometry once, the first time a
 * paint looks that far up the stack. */
#define STACK_MAX 256
static struct { Window w; int x, y, cw, ch; unsigned k; } stack_snap[STACK_MAX];
static int stack_n = -1, batching;
static Window *stack_kids;              /* the root's children, bottom up */
static unsigned stack_nk, stack_seen;   /* how many, how many looked at */

static void stack_forget(void)
{
    if (stack_kids) XFree(stack_kids);
    stack_kids = NULL;
    stack_nk = stack_seen = 0;
    stack_n = -1;
}

/* How many of stack_snap lie below w2k_glass_above. */
static int stack_gather(void)
{
    if (stack_n < 0) {
        stack_n = 0;
        Window root, parent;
        if (!XQueryTree(w2k.dpy, w2k.root, &root, &parent, &stack_kids, &stack_nk)) {
            stack_kids = NULL;
            stack_nk = 0;
        }
    }
    unsigned cut = 0;
    while (cut < stack_nk && stack_kids[cut] != w2k_glass_above) cut++;
    for (; stack_seen < cut && stack_n < STACK_MAX; stack_seen++) {
        Window k = stack_kids[stack_seen];
        XWindowAttributes wa;
        if (!XGetWindowAttributes(w2k.dpy, k, &wa)) continue;
        if (wa.map_state != IsViewable || wa.class == InputOnly) continue;
        stack_snap[stack_n].w = k;
        stack_snap[stack_n].k = stack_seen;
        /* wa.x/y name the inside of the border; the window covers from
         * bw before it. */
        stack_snap[stack_n].x = wa.x - wa.border_width;
        stack_snap[stack_n].y = wa.y - wa.border_width;
        stack_snap[stack_n].cw = wa.width + 2 * wa.border_width;
        stack_snap[stack_n].ch = wa.height + 2 * wa.border_width;
        stack_n++;
    }
    int n = 0;
    while (n < stack_n && stack_snap[n].k < cut) n++;
    return n;
}

/* Only the outermost batch starts and ends the snapshot: the WM makes no
 * change to the stack inside one. */
static void glass_batch(int begin)
{
    if (begin) batching++;
    else if (batching > 0) batching--;
    if (batching == (begin ? 1 : 0))
        stack_forget();                  /* re-gathered on the next ask */
}

static unsigned char *live_bg(int rx, int ry, int w, int h)
{
    if (!enabled || w <= 0 || h <= 0) return NULL;
    const int R = 8;                            /* the blur's reach */
    int ex = rx - R, ey = ry - R, ew = w + 2 * R, eh = h + 2 * R;
    if (ex < 0) { ew += ex; ex = 0; }
    if (ey < 0) { eh += ey; ey = 0; }
    if (ex + ew > w2k.sw) ew = w2k.sw - ex;
    if (ey + eh > w2k.sh) eh = w2k.sh - ey;
    if (ew <= 0 || eh <= 0) return NULL;

    unsigned char *buf = malloc((size_t)ew * eh * 3);
    if (!buf) return NULL;
    const unsigned char *dc = w2k_scheme_rgb(C_DESKTOP);
    for (size_t i = 0; i < (size_t)ew * eh; i++) memcpy(buf + i * 3, dc, 3);

    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(quiet);
    int below = stack_gather();
    {
        for (int i = 0; i < below; i++) {          /* bottom to top */
            Window k = stack_snap[i].w;
            int wx = stack_snap[i].x, wy = stack_snap[i].y;
            int ww = stack_snap[i].cw, wh = stack_snap[i].ch;
            int ix = ex > wx ? ex : wx, iy = ey > wy ? ey : wy;
            int ix1 = ex + ew < wx + ww ? ex + ew : wx + ww, iy1 = ey + eh < wy + wh ? ey + eh : wy + wh;
            if (ix1 <= ix || iy1 <= iy) continue;
            Pixmap pm = XCompositeNameWindowPixmap(w2k.dpy, k);
            if (!pm) continue;
            XImage *im = XGetImage(w2k.dpy, pm, ix - wx, iy - wy, (unsigned)(ix1 - ix),
                                   (unsigned)(iy1 - iy), AllPlanes, ZPixmap);
            XFreePixmap(w2k.dpy, pm);
            if (!im) continue;
            int rs, gs, bs, rb, gb, bb;
            masks(im, &rs, &gs, &bs, &rb, &gb, &bb);
            unsigned long rm = im->red_mask ? im->red_mask : w2k.visual->red_mask;
            unsigned long gm = im->green_mask ? im->green_mask : w2k.visual->green_mask;
            unsigned long bm = im->blue_mask ? im->blue_mask : w2k.visual->blue_mask;
            for (int y = 0; y < iy1 - iy; y++)
                for (int x = 0; x < ix1 - ix; x++) {
                    unsigned long px = XGetPixel(im, x, y);
                    unsigned long r = (px & rm) >> rs, g = (px & gm) >> gs, b = (px & bm) >> bs;
                    unsigned char *o = buf + ((size_t)(iy - ey + y) * ew + (ix - ex + x)) * 3;
                    o[0] = (unsigned char)(rb >= 8 ? r >> (rb - 8) : r << (8 - rb));
                    o[1] = (unsigned char)(gb >= 8 ? g >> (gb - 8) : g << (8 - gb));
                    o[2] = (unsigned char)(bb >= 8 ? b >> (bb - 8) : b << (8 - bb));
                }
            XDestroyImage(im);
        }
    }
    if (!batching) stack_forget();          /* walked afresh next time */
    XSync(w2k.dpy, False);
    XSetErrorHandler(old);

    blur(buf, ew, eh, R / 2);
    unsigned char *out = malloc((size_t)w * h * 3);
    if (!out) { free(buf); return NULL; }
    for (int y = 0; y < h; y++) {
        int sy = ry + y - ey;
        if (sy < 0) sy = 0;
        if (sy >= eh) sy = eh - 1;
        for (int x = 0; x < w; x++) {
            int sx = rx + x - ex;
            if (sx < 0) sx = 0;
            if (sx >= ew) sx = ew - 1;
            memcpy(out + ((size_t)y * w + x) * 3, buf + ((size_t)sy * ew + sx) * 3, 3);
        }
    }
    free(buf);
    return out;
}

/* The glass shows what was under it when it was painted: after a window
 * moves or the stack changes, every Aero frame and the bar are painted
 * again so they show what is under them now. In one batch: nothing is
 * restacked while they paint, so one walk of the stack serves them all,
 * where each frame used to walk it twice and the bar once a button. */
void glass_live_refresh(void)
{
    if (!enabled) return;
    glass_batch(1);
    for (Client *c = clients; c; c = c->next)
        if (!c->minimized && c->decorate) frame_paint(c);
    taskbar_paint();
    glass_batch(0);
}

/* Called at start and whenever the scheme changes: the effect and the
 * look decide whether the windows are redirected and the glass is live. */
void glass_live_apply(void)
{
    static int asked;
    if (!asked) {
        int ev, err;
        have = XCompositeQueryExtension(w2k.dpy, &ev, &err);
        asked = 1;
    }
    int want = have && w2k_theme == THEME_AERO && w2k_effects[FX_AERO_WINDOWS];
    if (want == enabled) return;
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(quiet);
    if (want) XCompositeRedirectSubwindows(w2k.dpy, w2k.root, CompositeRedirectAutomatic);
    else      XCompositeUnredirectSubwindows(w2k.dpy, w2k.root, CompositeRedirectAutomatic);
    XSync(w2k.dpy, False);
    XSetErrorHandler(old);
    enabled = want;
    w2k_glass_live = want ? live_bg : NULL;
    w2k_glass_batch = want ? glass_batch : NULL;
    batching = 0;
    stack_forget();
}
