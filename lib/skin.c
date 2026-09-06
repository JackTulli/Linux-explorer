/* skin.c -- bitmap skins: an RGBA image on the server, drawn in pieces.
 *
 * The icon cache handles square 16- and 32-pixel artwork; this is for the
 * odd-sized pieces a themed shell needs -- Windows XP's Start button is
 * 97x30 and comes as a strip of three states. One pixmap and one mask are
 * built per file; drawing takes a source rectangle out of it.
 *
 * Alpha is thresholded into a mask rather than blended, which is what the
 * icon path does too: these are shell bitmaps with hard edges, and X has
 * no alpha compositing without an extension we do not use. */
#include "w2k.h"
#include <stdlib.h>
#include <string.h>

struct W2kSkin {
    Pixmap pm, mask;
    int    w, h;
    int    opaque;               /* no transparent pixel: drawn without the mask */
    /* The artwork, kept so a scaled desktop can build an enlarged copy of
     * the sheet (nearest neighbour: the chrome stays crisp, and at 200%
     * it is exact). */
    unsigned char *rgba;
    Pixmap spm, smask;           /* the enlarged sheet, once built */
    int    sscale, spw, sph, smethod;
    /* Pieces of the sheet kept as tiles for w2k_skin_tile(), so a column
     * repeated across a caption is one filled rectangle, not one copy per
     * pixel. */
    struct { int sx, sy, sw, sh; Pixmap pm; } tiles[8];
    int    ntiles, next_tile;
};

W2kSkin *w2k_skin_load(const char *path)
{
    return w2k_skin_load_scaled(path, 256);
}

/* `scale` multiplies every colour channel (256 = unchanged): a button's
 * hot and pressed states, when no screenshot of them exists, are the
 * measured button made lighter or darker. */
W2kSkin *w2k_skin_load_scaled(const char *path, int scale)
{
    int w = 0, h = 0;
    unsigned char *rgba = w2k_image_load(path, &w, &h);
    if (!rgba || w <= 0 || h <= 0) { free(rgba); return NULL; }
    if (scale != 256)
        for (size_t i = 0; i < (size_t)w * h * 4; i++) {
            if (i % 4 == 3) continue;
            int v = rgba[i] * scale / 256;
            rgba[i] = (unsigned char)(v > 255 ? 255 : v);
        }
    W2kSkin *s = w2k_skin_from_rgba(rgba, w, h);
    free(rgba);
    return s;
}

/* Server pixmap and 1-bit mask from straight RGBA. */
static void make_pixmaps(const unsigned char *rgba, int w, int h,
                         Pixmap *pm, Pixmap *mask, int *opaque)
{
    *pm = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)w, (unsigned)h,
                        w2k.depth);

    char *pixels = malloc((size_t)w * h * 4);
    XImage *im = pixels ? XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap,
                                       0, pixels, (unsigned)w, (unsigned)h,
                                       32, 0) : NULL;
    if (!im) free(pixels);

    int stride = (w + 7) / 8;
    unsigned char *bits = w2k_alloc((size_t)stride * h);
    int op = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const unsigned char *p = rgba + ((size_t)y * w + x) * 4;
            if (im) XPutPixel(im, x, y, w2k_rgb(p[0], p[1], p[2]));
            if (p[3] >= 128) bits[y * stride + (x >> 3)] |= 1 << (x & 7);
            else op = 0;
        }

    if (im) {
        XPutImage(w2k.dpy, *pm, w2k.gc_icon, im, 0, 0, 0, 0,
                  (unsigned)w, (unsigned)h);
        XDestroyImage(im);
    }
    *mask = XCreateBitmapFromData(w2k.dpy, w2k.root, (char *)bits,
                                  (unsigned)w, (unsigned)h);
    free(bits);
    if (opaque) *opaque = op;
}

W2kSkin *w2k_skin_from_rgba(const unsigned char *rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0) return NULL;
    W2kSkin *s = w2k_alloc(sizeof *s);
    s->w = w;
    s->h = h;
    make_pixmaps(rgba, w, h, &s->pm, &s->mask, &s->opaque);
    if (w2k_ui_scale != 100) {
        s->rgba = malloc((size_t)w * h * 4);
        if (s->rgba) memcpy(s->rgba, rgba, (size_t)w * h * 4);
    }
    return s;
}

/* The enlarged sheet for the current scale, built on first use. Source
 * rectangles are mapped with w2k_px() at both ends, so neighbouring
 * pieces of the sheet stay neighbours. Returns 0 without artwork. */
static int ensure_scaled(W2kSkin *s)
{
    int method = w2k_ui_scale % 100 == 0 ? RS_NEAREST : w2k_resample;
    if (s->sscale == w2k_ui_scale && s->smethod == method && s->spm) return 1;
    if (!s->rgba) return 0;
    if (s->spm) { XFreePixmap(w2k.dpy, s->spm); s->spm = 0; }
    if (s->smask) { XFreePixmap(w2k.dpy, s->smask); s->smask = 0; }
    for (int i = 0; i < s->ntiles; i++)
        if (s->tiles[i].pm) { XFreePixmap(w2k.dpy, s->tiles[i].pm); s->tiles[i].pm = 0; }
    s->ntiles = s->next_tile = 0;

    int pw = w2k_px(s->w), ph = w2k_px(s->h);
    if (pw < 1 || ph < 1) return 0;
    if (method != RS_NEAREST) {
        /* A filtered enlargement: the sheet as a picture. */
        unsigned char *big = w2k_rgba_resample(s->rgba, s->w, s->h, pw, ph, method);
        if (!big) return 0;
        make_pixmaps(big, pw, ph, &s->spm, &s->smask, NULL);
        free(big);
        s->sscale = w2k_ui_scale;
        s->smethod = method;
        s->spw = pw; s->sph = ph;
        return 1;
    }
    unsigned char *big = malloc((size_t)pw * ph * 4);
    if (!big) return 0;
    /* Each source pixel c owns the destination columns w2k_px(c) up to
     * w2k_px(c + 1): the inverse of the mapping the callers' source
     * rectangles go through, so a rectangle cut from the sheet lands on
     * exactly the pixels it names. (Dividing by the enlarged size instead
     * drifts at a fractional scale and cut a neighbour's column into the
     * caption's tile.) */
    int sc = w2k_ui_scale;
    for (int y = 0; y < ph; y++) {
        int sy = (int)(((long)(y + 1) * 100 + sc - 1) / sc) - 1;
        if (sy < 0) sy = 0;
        if (sy >= s->h) sy = s->h - 1;
        for (int x = 0; x < pw; x++) {
            int sx = (int)(((long)(x + 1) * 100 + sc - 1) / sc) - 1;
            if (sx < 0) sx = 0;
            if (sx >= s->w) sx = s->w - 1;
            memcpy(big + ((size_t)y * pw + x) * 4,
                   s->rgba + ((size_t)sy * s->w + sx) * 4, 4);
        }
    }
    make_pixmaps(big, pw, ph, &s->spm, &s->smask, NULL);
    free(big);
    s->sscale = w2k_ui_scale;
    s->smethod = method;
    s->spw = pw; s->sph = ph;
    return 1;
}

void w2k_skin_free(W2kSkin *s)
{
    if (!s) return;
    if (s->pm) XFreePixmap(w2k.dpy, s->pm);
    if (s->mask) XFreePixmap(w2k.dpy, s->mask);
    if (s->spm) XFreePixmap(w2k.dpy, s->spm);
    if (s->smask) XFreePixmap(w2k.dpy, s->smask);
    free(s->rgba);
    for (int i = 0; i < s->ntiles; i++)
        if (s->tiles[i].pm) XFreePixmap(w2k.dpy, s->tiles[i].pm);
    free(s);
}

/* Plain copies go through a GC of their own: w2k.gc_icon carries whatever
 * clip mask the last icon left on it, and a copy made through that would
 * be cut to the icon's shape. */
GC w2k_copy_gc(void)
{
    static GC gc;
    if (!gc) {
        XGCValues v = { .graphics_exposures = False };
        gc = XCreateGC(w2k.dpy, w2k.root, GCGraphicsExposures, &v);
    }
    return gc;
}

int w2k_skin_w(const W2kSkin *s) { return s ? s->w : 0; }
int w2k_skin_h(const W2kSkin *s) { return s ? s->h : 0; }

void w2k_skin_draw(Drawable d, const W2kSkin *s0, int x, int y,
                   int sx, int sy, int sw, int sh)
{
    W2kSkin *s = (W2kSkin *)s0;
    if (!s || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx + sw > s->w || sy + sh > s->h) return;
    if (w2k_ui_scale != 100 && ensure_scaled(s)) {
        int px = w2k_cx(x), py = w2k_cx(y);
        int sx0 = w2k_px(sx), sy0 = w2k_px(sy);
        int pw = w2k_px(sx + sw) - sx0, ph = w2k_px(sy + sh) - sy0;
        if (pw <= 0 || ph <= 0) return;
        if (s->opaque) {
            XCopyArea(w2k.dpy, s->spm, d, w2k_copy_gc(), sx0, sy0,
                      (unsigned)pw, (unsigned)ph, px, py);
            return;
        }
        XSetClipOrigin(w2k.dpy, w2k.gc_icon, px - sx0, py - sy0);
        XSetClipMask(w2k.dpy, w2k.gc_icon, s->smask);
        XCopyArea(w2k.dpy, s->spm, d, w2k.gc_icon, sx0, sy0, (unsigned)pw,
                  (unsigned)ph, px, py);
        XSetClipMask(w2k.dpy, w2k.gc_icon, None);
        return;
    }
    if (s->opaque) {
        /* Most skins are solid: one copy, and no clip mask to set up and
         * take down around it. */
        XCopyArea(w2k.dpy, s->pm, d, w2k_copy_gc(), sx, sy, (unsigned)sw,
                  (unsigned)sh, x, y);
        return;
    }
    /* The mask is the whole sheet, so its origin has to be offset by the
     * source rectangle as well as the destination. */
    XSetClipOrigin(w2k.dpy, w2k.gc_icon, x - sx, y - sy);
    XSetClipMask(w2k.dpy, w2k.gc_icon, s->mask);
    XCopyArea(w2k.dpy, s->pm, d, w2k.gc_icon, sx, sy, (unsigned)sw,
              (unsigned)sh, x, y);
    XSetClipMask(w2k.dpy, w2k.gc_icon, None);
}

/* Fill a rectangle with a piece of the sheet repeated -- the one column of
 * a caption's middle across its width, the one row of a border down its
 * height. The piece becomes a tile pixmap the server repeats itself: a
 * single request however wide the caption, where copying the column
 * across took one request per pixel (and four, with the clip mask). The
 * mask is not consulted: pieces that are tiled are solid. */
void w2k_skin_tile(Drawable d, W2kSkin *s, int x, int y, int w, int h,
                   int sx, int sy, int sw, int sh)
{
    static GC tile_gc;
    if (!s || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx + sw > s->w || sy + sh > s->h) return;

    Pixmap sheet = s->pm;
    if (w2k_ui_scale != 100 && ensure_scaled(s)) {
        /* Both rectangles in physical pixels; the tile cache below is
         * keyed by the mapped source, and was emptied when the sheet was
         * rebuilt. */
        int pw = w2k_cw(x, w), ph = w2k_cw(y, h);
        x = w2k_cx(x); y = w2k_cx(y); w = pw; h = ph;
        /* A one-pixel column of the sheet is meant as a single colour
         * ramp to repeat: at a fractional scale the enlarged column can
         * be two physical columns of different sources, and tiling both
         * would stripe the caption. Take one. */
        int sx0 = w2k_px(sx), sy0 = w2k_px(sy);
        int sw1 = sw == 1, sh1 = sh == 1;
        sw = sw1 ? 1 : w2k_px(sx + sw) - sx0;
        sh = sh1 ? 1 : w2k_px(sy + sh) - sy0;
        sx = sx0; sy = sy0;
        sheet = s->spm;
        if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
    }

    Pixmap t = 0;
    for (int i = 0; i < s->ntiles; i++)
        if (s->tiles[i].sx == sx && s->tiles[i].sy == sy &&
            s->tiles[i].sw == sw && s->tiles[i].sh == sh) {
            t = s->tiles[i].pm;
            break;
        }
    if (!t) {
        t = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)sw, (unsigned)sh,
                          w2k.depth);
        XCopyArea(w2k.dpy, sheet, t, w2k_copy_gc(), sx, sy, (unsigned)sw,
                  (unsigned)sh, 0, 0);
        int i;
        if (s->ntiles < 8) i = s->ntiles++;
        else {
            i = s->next_tile++ % 8;
            XFreePixmap(w2k.dpy, s->tiles[i].pm);
        }
        s->tiles[i].sx = sx; s->tiles[i].sy = sy;
        s->tiles[i].sw = sw; s->tiles[i].sh = sh;
        s->tiles[i].pm = t;
    }
    if (!tile_gc) {
        XGCValues v = { .fill_style = FillTiled, .graphics_exposures = False };
        tile_gc = XCreateGC(w2k.dpy, w2k.root,
                            GCFillStyle | GCGraphicsExposures, &v);
    }
    XSetTile(w2k.dpy, tile_gc, t);
    XSetTSOrigin(w2k.dpy, tile_gc, x, y);
    XFillRectangle(w2k.dpy, d, tile_gc, x, y, (unsigned)w, (unsigned)h);
}
