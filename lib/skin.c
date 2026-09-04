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

W2kSkin *w2k_skin_from_rgba(const unsigned char *rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0) return NULL;
    W2kSkin *s = w2k_alloc(sizeof *s);
    s->w = w;
    s->h = h;
    s->pm = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)w, (unsigned)h,
                          w2k.depth);

    char *pixels = malloc((size_t)w * h * 4);
    XImage *im = pixels ? XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap,
                                       0, pixels, (unsigned)w, (unsigned)h,
                                       32, 0) : NULL;
    if (!im) free(pixels);

    int stride = (w + 7) / 8;
    unsigned char *bits = w2k_alloc((size_t)stride * h);
    s->opaque = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const unsigned char *p = rgba + ((size_t)y * w + x) * 4;
            if (im) XPutPixel(im, x, y, w2k_rgb(p[0], p[1], p[2]));
            if (p[3] >= 128) bits[y * stride + (x >> 3)] |= 1 << (x & 7);
            else s->opaque = 0;
        }

    if (im) {
        XPutImage(w2k.dpy, s->pm, w2k.gc_icon, im, 0, 0, 0, 0,
                  (unsigned)w, (unsigned)h);
        XDestroyImage(im);
    }
    s->mask = XCreateBitmapFromData(w2k.dpy, w2k.root, (char *)bits,
                                    (unsigned)w, (unsigned)h);
    free(bits);
    return s;
}

void w2k_skin_free(W2kSkin *s)
{
    if (!s) return;
    if (s->pm) XFreePixmap(w2k.dpy, s->pm);
    if (s->mask) XFreePixmap(w2k.dpy, s->mask);
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

void w2k_skin_draw(Drawable d, const W2kSkin *s, int x, int y,
                   int sx, int sy, int sw, int sh)
{
    if (!s || sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0 || sx + sw > s->w || sy + sh > s->h) return;
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
        XCopyArea(w2k.dpy, s->pm, t, w2k_copy_gc(), sx, sy, (unsigned)sw,
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
