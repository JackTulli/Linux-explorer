/* icon.c -- the desktop's icons.
 *
 * Every icon is genuine Windows 2000 artwork: the 16x16 and 32x32 images
 * are baked into icon_data.inc by tools/genicons.py from icons/win2k, and
 * any of them can be replaced at run time by dropping <slug>.ico into
 * ~/.w2k/icons (see iconload.c). Each icon is cached as a Pixmap plus a
 * 1-bit mask and blitted through its own GC, so drawing is one XCopyArea.
 *
 * With "Smooth edges of icons" (an Effect) an icon is kept instead as an
 * ARGB picture and laid on with XRender, so the art's real transparency
 * shows at its edges -- the 1-bit mask cut them at half -- and art whose
 * transparency is all-or-nothing, as Windows 2000's is, has its stepped
 * edges softened first: drawn twice as large with the cubic filter and
 * averaged back down, the inside kept as it was. */
#include "w2k.h"
#include <X11/extensions/Xrender.h>
#include <stdlib.h>
#include <zlib.h>
#include <string.h>

#include "icon_data.inc"

extern unsigned char *w2k_icon_user16[N_ICONS], *w2k_icon_user32[N_ICONS];
extern int w2k_clip_on, w2k_clip_x, w2k_clip_y, w2k_clip_w, w2k_clip_h;

typedef struct { Pixmap pm, mask; Picture pic; int built, smooth; } Cached;

/* Ids from 0 to N_ICONS-1 are the built-in artwork. Anything above that is
 * registered at run time -- an application's own icon out of the icon theme,
 * say -- and kept in these parallel arrays, which grow as needed. */
typedef struct { unsigned char *p16, *p32; } Extra;
static Extra *extra;
static int    nextra, extra_cap;

static Cached cache16[N_ICONS], cache32[N_ICONS], dim16[N_ICONS];
static Cached *ecache16, *ecache32, *edim16;

int w2k_icon_register(unsigned char *rgba16, unsigned char *rgba32)
{
    if (!rgba16 && !rgba32) return ICO_APP;
    if (nextra == extra_cap) {
        int cap = extra_cap ? extra_cap * 2 : 64;
        /* Committed one at a time: a realloc that succeeds has already
         * freed the old block, so a later failure must not leave the old
         * pointer in place -- it would be a dangling one. */
        Extra *e = realloc(extra, (size_t)cap * sizeof *e);
        if (!e) return ICO_APP;
        extra = e;
        Cached *c1 = realloc(ecache16, (size_t)cap * sizeof *c1);
        if (!c1) return ICO_APP;
        ecache16 = c1;
        Cached *c2 = realloc(ecache32, (size_t)cap * sizeof *c2);
        if (!c2) return ICO_APP;
        ecache32 = c2;
        Cached *c3 = realloc(edim16, (size_t)cap * sizeof *c3);
        if (!c3) return ICO_APP;
        edim16 = c3;
        memset(e + extra_cap, 0, (size_t)(cap - extra_cap) * sizeof *e);
        memset(c1 + extra_cap, 0, (size_t)(cap - extra_cap) * sizeof *c1);
        memset(c2 + extra_cap, 0, (size_t)(cap - extra_cap) * sizeof *c2);
        memset(c3 + extra_cap, 0, (size_t)(cap - extra_cap) * sizeof *c3);
        extra = e; ecache16 = c1; ecache32 = c2; edim16 = c3;
        extra_cap = cap;
    }
    extra[nextra].p16 = rgba16 ? rgba16 : rgba32;
    extra[nextra].p32 = rgba32 ? rgba32 : rgba16;
    return N_ICONS + nextra++;
}

int w2k_icon_valid(int id) { return id >= 0 && id < N_ICONS + nextra; }

static void cached_drop(Cached *c)
{
    if (!c->built) return;
    if (c->pic) XRenderFreePicture(w2k.dpy, c->pic);
    w2k_free_pixmap(c->pm);
    if (c->mask) w2k_free_pixmap(c->mask);
    c->pm = c->mask = 0;
    c->pic = 0;
    c->built = 0;
}

/* Is the effect on, and can this server draw it? */
static int smooth_wanted(void)
{
    static int render_ok = -1;
    if (!w2k_effects[FX_SMOOTH_ICONS]) return 0;
    if (render_ok < 0) {
        int ev, err;
        render_ok = XRenderQueryExtension(w2k.dpy, &ev, &err) ? 1 : 0;
    }
    return render_ok;
}

/* draw_scaled()'s pictures: any size other than 16 and 32, and every size
 * at a desktop scale other than 100%. */
#define N_SCALED 256
static struct { int id, size, dimmed, method, smooth; Cached c; } scaled[N_SCALED];
static int nscaled, next_slot;

void w2k_icon_cache_drop(int id)
{
    /* The scaled pictures of it too: they kept showing the old artwork
     * after an icon set changed, at 150% everywhere and in the Start
     * panel's 24-pixel icons at any scale. */
    for (int i = 0; i < nscaled; i++)
        if (scaled[i].id == id) {
            cached_drop(&scaled[i].c);
            scaled[i].id = -1;
        }

    if (id >= N_ICONS) {
        int k = id - N_ICONS;
        if (k >= nextra) return;
        Cached *e[] = { &ecache16[k], &ecache32[k], &edim16[k] };
        for (int i = 0; i < 3; i++) cached_drop(e[i]);
        return;
    }
    Cached *c[] = { &cache16[id], &cache32[id], &dim16[id] };
    for (int i = 0; i < 3; i++) cached_drop(c[i]);
}

/* The built-in artwork is stored deflated (see icon_data.inc). Inflate on
 * first use and keep it: a program draws a dozen icons, not all of them. */
static unsigned char *builtin16[N_ICONS], *builtin32[N_ICONS];

static const unsigned char *builtin(int id, int size)
{
    unsigned char **slot = (size == 16) ? &builtin16[id] : &builtin32[id];
    if (*slot) return *slot;

    const unsigned char *z = (size == 16) ? real_icons[id].p16
                                          : real_icons[id].p32;
    int zn = (size == 16) ? real_icons[id].n16 : real_icons[id].n32;
    if (!z || zn <= 0) return NULL;

    uLongf n = (uLongf)size * size * 4;
    unsigned char *out = malloc(n);
    if (!out) return NULL;
    if (uncompress(out, &n, z, (uLong)zn) != Z_OK ||
        n != (uLongf)size * size * 4) {
        free(out);
        return NULL;
    }
    *slot = out;
    return out;
}

static const unsigned char *rgba_for(int id, int size)
{
    if (id >= N_ICONS) {
        int k = id - N_ICONS;
        if (k >= nextra) return NULL;
        return size == 16 ? extra[k].p16 : extra[k].p32;
    }
    if (size == 16 && w2k_icon_user16[id]) return w2k_icon_user16[id];
    if (size != 16 && w2k_icon_user32[id]) return w2k_icon_user32[id];
    return builtin(id, size);
}

/* Art whose transparency is all-or-nothing gets its stepped edges
 * softened: drawn twice as large with the cubic filter and averaged
 * back, which gives the steps in-between coverage. Pixels that were
 * fully opaque and stay so keep their own colour, so the inside of the
 * picture is not blurred. */
static unsigned char *soften(const unsigned char *rgba, int n)
{
    for (int i = 0; i < n * n; i++)
        if (rgba[i * 4 + 3] != 0 && rgba[i * 4 + 3] != 255) return NULL;   /* already soft */
    unsigned char *up = w2k_rgba_resample(rgba, n, n, 2 * n, 2 * n, RS_CUBIC);
    if (!up) return NULL;
    unsigned char *down = w2k_rgba_scale(up, 2 * n, 2 * n, n);
    free(up);
    if (!down) return NULL;
    for (int i = 0; i < n * n; i++) {
        const unsigned char *o = rgba + i * 4;
        unsigned char *d = down + i * 4;
        if (o[3] == 255 && d[3] >= 250) { d[0] = o[0]; d[1] = o[1]; d[2] = o[2]; d[3] = 255; }
        else if (o[3] == 0 && d[3] < 6) d[3] = 0;
    }
    return down;
}

/* The ARGB picture for the smooth path: premultiplied, 32 bits deep. */
static void build_smooth(Cached *c, const unsigned char *rgba, int n, int dimmed)
{
    unsigned char *soft = soften(rgba, n);
    const unsigned char *src = soft ? soft : rgba;
    Pixmap apm = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)n, (unsigned)n, 32);
    char *pixels = malloc((size_t)n * n * 4);
    XImage *im = pixels ? XCreateImage(w2k.dpy, w2k.visual, 32, ZPixmap, 0, pixels,
                                       (unsigned)n, (unsigned)n, 32, 0) : NULL;
    if (!im) { free(pixels); free(soft); w2k_free_pixmap(apm); return; }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const unsigned char *p = src + ((size_t)y * n + x) * 4;
            int r = p[0], g = p[1], b = p[2], a = p[3];
            if (dimmed) { r = (r + 212 * 2) / 3; g = (g + 208 * 2) / 3; b = (b + 200 * 2) / 3; }
            unsigned long v = ((unsigned long)a << 24) | ((unsigned long)(r * a / 255) << 16) |
                              ((unsigned long)(g * a / 255) << 8) | (unsigned long)(b * a / 255);
            XPutPixel(im, x, y, v);
        }
    GC g32 = XCreateGC(w2k.dpy, apm, 0, NULL);
    XPutImage(w2k.dpy, apm, g32, im, 0, 0, 0, 0, (unsigned)n, (unsigned)n);
    XFreeGC(w2k.dpy, g32);
    XDestroyImage(im);
    free(soft);
    XRenderPictFormat *fmt = XRenderFindStandardFormat(w2k.dpy, PictStandardARGB32);
    c->pic = XRenderCreatePicture(w2k.dpy, apm, fmt, 0, NULL);
    c->pm = apm;                       /* kept only to be freed with the picture */
    c->mask = 0;
    c->smooth = 1;
    c->built = 1;
}

static void build(Cached *c, const unsigned char *rgba, int n, int dimmed)
{
    c->smooth = 0;
    c->pic = 0;
    if (smooth_wanted()) { build_smooth(c, rgba, n, dimmed); if (c->built) return; }
    c->pm = XCreatePixmap(w2k.dpy, w2k.root, n, n, w2k.depth);
    /* Allocate first: if XCreateImage fails it does not take ownership,
     * and passing malloc() straight in would lose the block. */
    char *pixels = malloc((size_t)n * n * 4);
    XImage *im = pixels ? XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap,
                                       0, pixels, n, n, 32, 0) : NULL;
    if (!im) free(pixels);
    int stride = (n + 7) / 8;
    unsigned char *bits = w2k_alloc((size_t)stride * n);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const unsigned char *p = rgba + ((size_t)y * n + x) * 4;
            int r = p[0], g = p[1], b = p[2];
            if (dimmed) {               /* greyed: blend toward the face colour */
                r = (r + 212 * 2) / 3; g = (g + 208 * 2) / 3; b = (b + 200 * 2) / 3;
            }
            if (im) XPutPixel(im, x, y, w2k_rgb(r, g, b));
            if (p[3] >= 128) bits[y * stride + (x >> 3)] |= 1 << (x & 7);
        }
    if (im) {
        XPutImage(w2k.dpy, c->pm, w2k.gc_icon, im, 0, 0, 0, 0, n, n);
        XDestroyImage(im);
    }
    c->mask = XCreateBitmapFromData(w2k.dpy, w2k.root, (char *)bits, n, n);
    free(bits);
    c->built = 1;
}

static void blit_cached(Drawable d, int x, int y, Cached *c, int size);
static void draw_scaled(Drawable d, int x, int y, int id, int size, int dimmed);

/* Masked copy, intersected with the toolkit's current clip rectangle so
 * icons inside scrolled views are cut off where the view is. */
static void blit(Drawable d, int x, int y, int id, int size, int dimmed)
{
    if (!w2k_icon_valid(id)) return;
    const unsigned char *rgba = rgba_for(id, size);
    if (!rgba) return;
    Cached *c;
    if (id >= N_ICONS) {
        int k = id - N_ICONS;
        c = dimmed ? &edim16[k] : (size == 16 ? &ecache16[k] : &ecache32[k]);
    } else {
        c = dimmed ? &dim16[id] : (size == 16 ? &cache16[id] : &cache32[id]);
    }
    if (c->built && c->smooth != smooth_wanted()) cached_drop(c);   /* the effect changed */

    if (w2k_ui_scale != 100) {
        /* Built below, not here: draw_scaled keeps its own cache, so
         * building this one first made a pixmap and a mask per icon that
         * nothing ever drew. */
        /* On a scaled desktop the 16-pixel icon is drawn from the 32-pixel
         * art (exact at 200%), and the 32 from itself, enlarged. */
        draw_scaled(d, x, y, id, size, dimmed);
        return;
    }
    if (!c->built) build(c, rgba, size, dimmed);
    blit_cached(d, x, y, c, size);
}

/* Masked copy of a built pixmap at a physical position, intersected with
 * the (physical) clip rectangle. */
static void blit_cached(Drawable d, int x, int y, Cached *c, int size)
{
    int sx = 0, sy = 0, w = size, h = size, dx = x, dy = y;
    if (w2k_clip_on) {
        int x0 = x > w2k_clip_x ? x : w2k_clip_x;
        int y0 = y > w2k_clip_y ? y : w2k_clip_y;
        int cx1 = w2k_clip_x + w2k_clip_w, cy1 = w2k_clip_y + w2k_clip_h;
        int x1 = x + size < cx1 ? x + size : cx1;
        int y1 = y + size < cy1 ? y + size : cy1;
        if (x1 <= x0 || y1 <= y0) return;
        sx = x0 - x; sy = y0 - y; w = x1 - x0; h = y1 - y0; dx = x0; dy = y0;
    }
    if (c->pic) {
        /* Laid on with its real transparency. */
        XRenderPictFormat *fmt = XRenderFindVisualFormat(w2k.dpy, w2k.visual);
        Picture dst = XRenderCreatePicture(w2k.dpy, d, fmt, 0, NULL);
        XRenderComposite(w2k.dpy, PictOpOver, c->pic, None, dst, sx, sy, 0, 0, dx, dy,
                         (unsigned)w, (unsigned)h);
        XRenderFreePicture(w2k.dpy, dst);
        return;
    }
    XSetClipOrigin(w2k.dpy, w2k.gc_icon, x, y);
    XSetClipMask(w2k.dpy, w2k.gc_icon, c->mask);
    XCopyArea(w2k.dpy, c->pm, d, w2k.gc_icon, sx, sy, w, h, dx, dy);
    XSetClipMask(w2k.dpy, w2k.gc_icon, None);
}

/* Any size, from the 32-pixel art: for the 24-pixel icons Luna's Start
 * panel and its footer use. Kept per (icon, size) once built. */
/* `size` is logical; the pixmap built is w2k_px(size) wide and drawn at
 * the mapped position. Kept per (icon, physical size, dimmed). */
static void draw_scaled(Drawable d, int x, int y, int id, int size, int dimmed)
{
    if (!w2k_icon_valid(id) || size < 4 || size > 128) return;
    int ps = w2k_px(size);
    if (ps < 1 || ps > 512) return;
    int slot = -1, want = smooth_wanted();
    for (int i = 0; i < nscaled; i++)
        if (scaled[i].id == id && scaled[i].size == ps && scaled[i].smooth == want &&
            scaled[i].dimmed == dimmed && scaled[i].method == w2k_resample) { slot = i; break; }
    if (slot < 0) {
        /* From the 32-pixel art unless the wanted size is exactly the
         * 16-pixel one, which has its own drawing. */
        int from = ps == 16 ? 16 : 32;
        const unsigned char *src = rgba_for(id, from);
        if (!src) { from = 32; src = rgba_for(id, 32); }
        if (!src) return;
        /* Shrinking from the 32-pixel art is a box average, which suits
         * pixel art; enlarging by a fraction uses the chosen filter. */
        unsigned char *px = ps < from ? w2k_rgba_scale(src, from, from, ps)
                          : w2k_rgba_resample(src, from, from, ps, ps, w2k_resample_for(from, ps));
        if (!px) return;
        if (nscaled < N_SCALED) slot = nscaled++;
        else {
            slot = next_slot++ % N_SCALED;
            cached_drop(&scaled[slot].c);
        }
        scaled[slot].id = id;
        scaled[slot].size = ps;
        scaled[slot].dimmed = dimmed;
        scaled[slot].method = w2k_resample;
        scaled[slot].smooth = want;
        memset(&scaled[slot].c, 0, sizeof scaled[slot].c);
        build(&scaled[slot].c, px, ps, dimmed);
        free(px);
    }
    blit_cached(d, w2k_cx(x), w2k_cx(y), &scaled[slot].c, ps);
}

void w2k_icon_draw_scaled(Drawable d, int x, int y, int id, int size)
{
    if ((size == 16 || size == 32) && w2k_ui_scale == 100) {
        blit(d, x, y, id, size, 0);
        return;
    }
    draw_scaled(d, x, y, id, size, 0);
}

void w2k_icon_draw(Drawable d, int x, int y, int id)          { blit(d, x, y, id, 16, 0); }
void w2k_icon_draw_disabled(Drawable d, int x, int y, int id) { blit(d, x, y, id, 16, 1); }
void w2k_bigicon_draw(Drawable d, int x, int y, int id)       { blit(d, x, y, id, 32, 0); }

/* A shortcut wears a small arrow in its bottom-left corner. Windows
 * composites this at draw time from a separate overlay image rather than
 * baking it into each icon, and so does this. */
void w2k_icon_draw_link(Drawable d, int x, int y, int id)
{
    w2k_icon_draw(d, x, y, id);
    w2k_icon_draw(d, x, y, ICO_LINK_OVERLAY);
}

void w2k_bigicon_draw_link(Drawable d, int x, int y, int id)
{
    w2k_bigicon_draw(d, x, y, id);
    w2k_bigicon_draw(d, x, y, ICO_LINK_OVERLAY);
}
