/* aero.c -- Windows 7 Aero: glass over the wallpaper.
 *
 * Aero's frames, taskbar and Start menu are translucent: what lies behind
 * them shows through, blurred, with a colour laid over it. Without a
 * compositing manager nothing can be seen through a window, so the glass
 * here is the wallpaper -- kept blurred, a quarter the screen's size each
 * way -- with the colour applied to it, painted into the frame. A window
 * behind another's frame is not seen through it; the wallpaper is what
 * shows there.
 *
 * The colouring was measured off Windows 7 (the same window over a
 * black, a white and a blue desktop). Per channel, from the blurred
 * background's luminance L and the pixel's own value:
 *
 *     out = c0 + (cw - c0) * (L/255)^gamma + k * (bg - L)
 *
 * c0 is what shows over black, cw over white; gamma bends the middle
 * (the dark Start menu glass lifts dark backgrounds, gamma 0.44) and k is
 * how much of the background's own hue survives. Lines and edges follow
 * the same law with their own c0 and cw. */
#include "w2k.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * The blurred wallpaper
 * ------------------------------------------------------------------ */
#define Q 4                     /* the source is 1/Q the screen each way */
static unsigned char *src;      /* RGB, src_w by src_h */
static int src_w, src_h, src_ready;
static unsigned char flat[3] = { 58, 110, 165 };

static void pixel_rgb(unsigned long px, unsigned char *rgb)
{
    static unsigned long rm, gm, bm;
    static int rs, gs, bs, rb, gb, bb, ready;
    if (!ready) {
        rm = w2k.visual->red_mask; gm = w2k.visual->green_mask; bm = w2k.visual->blue_mask;
        for (rs = 0; rs < 32 && !((rm >> rs) & 1); rs++) ;
        for (gs = 0; gs < 32 && !((gm >> gs) & 1); gs++) ;
        for (bs = 0; bs < 32 && !((bm >> bs) & 1); bs++) ;
        for (rb = 0; rb < 32 && ((rm >> (rs + rb)) & 1); rb++) ;
        for (gb = 0; gb < 32 && ((gm >> (gs + gb)) & 1); gb++) ;
        for (bb = 0; bb < 32 && ((bm >> (bs + bb)) & 1); bb++) ;
        ready = 1;
    }
    unsigned long r = (px & rm) >> rs, g = (px & gm) >> gs, b = (px & bm) >> bs;
    rgb[0] = (unsigned char)(rb >= 8 ? r >> (rb - 8) : r << (8 - rb));
    rgb[1] = (unsigned char)(gb >= 8 ? g >> (gb - 8) : g << (8 - gb));
    rgb[2] = (unsigned char)(bb >= 8 ? b >> (bb - 8) : b << (8 - bb));
}

void w2k_glass_source_begin(int sw, int sh, int r, int g, int b)
{
    free(src);
    src = NULL;
    src_ready = 0;
    flat[0] = (unsigned char)r; flat[1] = (unsigned char)g; flat[2] = (unsigned char)b;
    if (sw <= 0 || sh <= 0) return;
    src_w = (sw + Q - 1) / Q;
    src_h = (sh + Q - 1) / Q;
    src = malloc((size_t)src_w * src_h * 3);
    if (!src) return;
    for (size_t i = 0; i < (size_t)src_w * src_h; i++) memcpy(src + i * 3, flat, 3);
}

/* `im` shows the root rectangle (x, y, w, h): the middle pixel of each
 * block of Q by Q goes into the source; the blur smooths the rest. */
void w2k_glass_source_put(int x, int y, int w, int h, XImage *im)
{
    if (!src || !im) return;
    for (int qy = 0; qy < src_h; qy++) {
        int cy = qy * Q + Q / 2;
        if (cy < y || cy >= y + h) continue;
        for (int qx = 0; qx < src_w; qx++) {
            int cx = qx * Q + Q / 2;
            if (cx < x || cx >= x + w) continue;
            pixel_rgb(XGetPixel(im, cx - x, cy - y), src + ((size_t)qy * src_w + qx) * 3);
        }
    }
}

/* Three box blurs, radius 3 in source pixels (a dozen on the screen):
 * close enough to the Gaussian Aero uses. */
static void blur_pass(unsigned char *p, int w, int h, int rad)
{
    unsigned char *tmp = malloc((size_t)w * h * 3);
    if (!tmp) return;
    for (int y = 0; y < h; y++) {                   /* horizontal */
        const unsigned char *row = p + (size_t)y * w * 3;
        unsigned char *out = tmp + (size_t)y * w * 3;
        for (int c = 0; c < 3; c++) {
            int sum = 0, n = 0;
            for (int x = -rad; x <= rad; x++) { int xx = x < 0 ? 0 : x >= w ? w - 1 : x; sum += row[xx * 3 + c]; n++; }
            for (int x = 0; x < w; x++) {
                out[x * 3 + c] = (unsigned char)(sum / n);
                int xo = x - rad, xi = x + rad + 1;
                sum -= row[(xo < 0 ? 0 : xo) * 3 + c];
                sum += row[(xi >= w ? w - 1 : xi) * 3 + c];
            }
        }
    }
    for (int x = 0; x < w; x++) {                   /* vertical */
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

void w2k_glass_source_end(void)
{
    if (!src) return;
    for (int i = 0; i < 3; i++) blur_pass(src, src_w, src_h, 3);
    src_ready = 1;
}

void w2k_glass_source_free(void)
{
    free(src);
    src = NULL;
    src_ready = 0;
}

int w2k_glass_source_ready(void) { return src_ready; }

unsigned char *(*w2k_glass_live)(int rx, int ry, int w, int h);
Window w2k_glass_above;
void (*w2k_glass_batch)(int begin);

/* The blurred wallpaper under the root rectangle, RGB, malloc'd -- or,
 * when the window manager offers it, what really lies there. */
unsigned char *w2k_glass_bg(int rx, int ry, int w, int h)
{
    if (w <= 0 || h <= 0) return NULL;
    if (w2k_glass_live) {
        unsigned char *live = w2k_glass_live(rx, ry, w, h);
        if (live) return live;
    }
    unsigned char *out = malloc((size_t)w * h * 3);
    if (!out) return NULL;
    if (!src_ready || !src) {
        for (size_t i = 0; i < (size_t)w * h; i++) memcpy(out + i * 3, flat, 3);
        return out;
    }
    /* Bilinear from the quarter-size source: the sample points sit at
     * the block centres. 8.8 fixed point. */
    for (int y = 0; y < h; y++) {
        int fy = ((ry + y) * 256 - (Q / 2) * 256) / Q;
        int y0 = fy >> 8, ty = fy & 255;
        if (y0 < 0) { y0 = 0; ty = 0; }
        if (y0 >= src_h - 1) { y0 = src_h - 1; ty = 0; }
        int y1 = y0 + (ty ? 1 : 0);
        const unsigned char *r0 = src + (size_t)y0 * src_w * 3, *r1 = src + (size_t)y1 * src_w * 3;
        unsigned char *o = out + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            int fx = ((rx + x) * 256 - (Q / 2) * 256) / Q;
            int x0 = fx >> 8, tx = fx & 255;
            if (x0 < 0) { x0 = 0; tx = 0; }
            if (x0 >= src_w - 1) { x0 = src_w - 1; tx = 0; }
            int x1 = x0 + (tx ? 1 : 0);
            for (int c = 0; c < 3; c++) {
                int a = r0[x0 * 3 + c] * (256 - tx) + r0[x1 * 3 + c] * tx;
                int b = r1[x0 * 3 + c] * (256 - tx) + r1[x1 * 3 + c] * tx;
                o[x * 3 + c] = (unsigned char)((a * (256 - ty) + b * ty) >> 16);
            }
        }
    }
    return out;
}

/* ------------------------------------------------------------------ *
 * The law
 * ------------------------------------------------------------------ */
static struct { const W2kGlass *g; unsigned char lut[3][256]; int k256; } laws[16];
static int nlaws;

static const unsigned char *law_lut(const W2kGlass *g, int *k256)
{
    for (int i = 0; i < nlaws; i++)
        if (laws[i].g == g) { *k256 = laws[i].k256; return &laws[i].lut[0][0]; }
    int i = nlaws < 16 ? nlaws++ : 0;
    laws[i].g = g;
    laws[i].k256 = (int)(g->k * 256 + 0.5);
    for (int c = 0; c < 3; c++)
        for (int l = 0; l < 256; l++) {
            double t = pow(l / 255.0, g->gamma);
            int v = (int)(g->c0[c] + (g->cw[c] - g->c0[c]) * t + 0.5);
            laws[i].lut[c][l] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    *k256 = laws[i].k256;
    return &laws[i].lut[0][0];
}

static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void w2k_glass_law(const unsigned char *bg, unsigned char *out, size_t n, const W2kGlass *g)
{
    int k256;
    const unsigned char *lut = law_lut(g, &k256);
    for (size_t i = 0; i < n; i++, bg += 3, out += 3) {
        int l = (77 * bg[0] + 150 * bg[1] + 29 * bg[2]) >> 8;
        for (int c = 0; c < 3; c++)
            out[c] = (unsigned char)clamp8(lut[c * 256 + l] + ((k256 * (bg[c] - l)) >> 8));
    }
}

/* Put an RGB buffer on a drawable. */
void w2k_rgb_put(Drawable d, int dx, int dy, const unsigned char *rgb, int w, int h)
{
    if (w <= 0 || h <= 0 || !rgb) return;
    char *data = malloc((size_t)w * h * 4);
    if (!data) return;
    XImage *im = XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap, 0, data,
                              (unsigned)w, (unsigned)h, 32, 0);
    if (!im) { free(data); return; }
    static const uint32_t one = 1;
    int host_lsb = *(const unsigned char *)&one == 1;
    int fast = im->bits_per_pixel == 32 && (im->byte_order == LSBFirst) == host_lsb &&
               w2k.visual->red_mask == 0xff0000 && w2k.visual->green_mask == 0xff00 &&
               w2k.visual->blue_mask == 0xff;
    for (int y = 0; y < h; y++) {
        const unsigned char *p = rgb + (size_t)y * w * 3;
        if (fast) {
            uint32_t *o = (uint32_t *)(im->data + (size_t)y * im->bytes_per_line);
            for (int x = 0; x < w; x++, p += 3)
                o[x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        } else {
            for (int x = 0; x < w; x++, p += 3)
                XPutPixel(im, x, y, w2k_rgb(p[0], p[1], p[2]));
        }
    }
    XPutImage(w2k.dpy, d, w2k.gc, im, 0, 0, dx, dy, (unsigned)w, (unsigned)h);
    XDestroyImage(im);           /* frees data */
}

/* ------------------------------------------------------------------ *
 * The measured glasses and lines
 * ------------------------------------------------------------------ */
/* A window's glass: all but transparent, a breath of white. */
const W2kGlass w2k_glass_frame = { { 9, 15, 20 }, { 255, 255, 255 }, 1.0f, 0.75f };
/* The taskbar: darker, cooler, and it keeps less of the picture's hue. */
const W2kGlass w2k_glass_bar   = { { 8, 12, 16 }, { 142, 169, 195 }, 1.0f, 0.39f };
/* The Start menu's dark glass, which lifts a dark picture. */
const W2kGlass w2k_glass_dark  = { { 0, 0, 0 }, { 83, 99, 114 }, 0.44f, 0.10f };


/* White laid over a pixel, alpha 0..256. */
static inline void lighten(unsigned char *p, int a)
{
    if (a <= 0) return;
    if (a > 256) a = 256;
    for (int c = 0; c < 3; c++) p[c] = (unsigned char)(p[c] + (((255 - p[c]) * a) >> 8));
}
static inline void darken(unsigned char *p, int a)
{
    if (a <= 0) return;
    if (a > 256) a = 256;
    for (int c = 0; c < 3; c++) p[c] = (unsigned char)(p[c] - ((p[c] * a) >> 8));
}

/* A grey `c` laid over a pixel, alpha 0..256: the frame's lines are
 * white, black or grey laid over the glass, measured so. */
static inline void overlay(unsigned char *p, int c, int a)
{
    for (int i = 0; i < 3; i++) p[i] = (unsigned char)(p[i] + (((c - p[i]) * a) >> 8));
}
static void ov_h(unsigned char *out, int w, int h, int x, int y, int n, int c, int a)
{
    if (y < 0 || y >= h) return;
    for (int i = 0; i < n; i++) if (x + i >= 0 && x + i < w) overlay(out + ((size_t)y * w + x + i) * 3, c, a);
}
static void ov_v(unsigned char *out, int w, int h, int x, int y, int n, int c, int a)
{
    if (x < 0 || x >= w) return;
    for (int i = 0; i < n; i++) if (y + i >= 0 && y + i < h) overlay(out + ((size_t)(y + i) * w + x) * 3, c, a);
}

/* RGBA over RGB, straight alpha. */
static void composite(unsigned char *out, int ow, int oh, int dx, int dy,
                      const unsigned char *rgba, int sw, int sh, int sx, int sy, int cw, int ch)
{
    for (int y = 0; y < ch; y++) {
        int oy = dy + y, iy = sy + y;
        if (oy < 0 || oy >= oh || iy < 0 || iy >= sh) continue;
        for (int x = 0; x < cw; x++) {
            int ox = dx + x, ix = sx + x;
            if (ox < 0 || ox >= ow || ix < 0 || ix >= sw) continue;
            const unsigned char *s = rgba + ((size_t)iy * sw + ix) * 4;
            unsigned char *o = out + ((size_t)oy * ow + ox) * 3;
            int a = s[3];
            if (!a) continue;
            for (int c = 0; c < 3; c++) o[c] = (unsigned char)((s[c] * a + o[c] * (255 - a)) / 255);
        }
    }
}

/* smoothstep, 0..256 in and out */
static int smooth(int t)
{
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    return (t * t * (3 * 256 - 2 * t)) >> 16;
}

/* ------------------------------------------------------------------ *
 * Skins: the caption buttons and the frame's corners, RGBA
 * ------------------------------------------------------------------ */
static unsigned char *btn_rgba;  static int btn_w, btn_h, btn_tried;
static unsigned char *btn2_rgba; static int btn2_w, btn2_h, btn2_tried;   /* the inactive window's */
static unsigned char *cor_rgba;  static int cor_w, cor_h, cor_tried;
static int skins_scale;

static unsigned char *load_rgba(const char *name, int *w, int *h)
{
    char path[1024];
    if (!w2k_skin_path(name, path, sizeof path)) return NULL;
    int iw = 0, ih = 0;
    unsigned char *rgba = w2k_image_load(path, &iw, &ih);
    if (!rgba || iw <= 0 || ih <= 0) { free(rgba); return NULL; }
    if (w2k_ui_scale != 100) {
        int sw = w2k_px(iw), sh = w2k_px(ih);
        unsigned char *sc = w2k_rgba_resample(rgba, iw, ih, sw, sh, RS_CUBIC);
        if (sc) { free(rgba); rgba = sc; iw = sw; ih = sh; }
    }
    *w = iw; *h = ih;
    return rgba;
}

static void skins(void)
{
    if (skins_scale != w2k_ui_scale) {
        free(btn_rgba); btn_rgba = NULL; btn_tried = 0;
        free(btn2_rgba); btn2_rgba = NULL; btn2_tried = 0;
        free(cor_rgba); cor_rgba = NULL; cor_tried = 0;
        skins_scale = w2k_ui_scale;
    }
    if (!btn_tried) { btn_tried = 1; btn_rgba = load_rgba("aero-capbtn.png", &btn_w, &btn_h); }
    if (!btn2_tried) { btn2_tried = 1; btn2_rgba = load_rgba("aero-capbtn-inactive.png", &btn2_w, &btn2_h); }
    if (!cor_tried) { cor_tried = 1; cor_rgba = load_rgba("aero-corners.png", &cor_w, &cor_h); }
}

/* The corner cut per row: where the corner art turns opaque. Rows count
 * from the top for the top corners and from the bottom for the bottom
 * ones; the answer is in screen pixels. */
int w2k_aero_corner_inset(int row, int bottom)
{
    skins();
    int n = cor_h;                      /* the art is n rows of n-pixel corners */
    int cell = bottom ? 2 : 0;          /* TL and BL */
    /* A corner sheet from ~/.w2k/skins narrower than the cell was read
     * past its end. */
    if (!cor_rgba || row < 0 || row >= n || cor_w < (cell + 1) * n) return 0;
    /* The curve never steps back out as it goes in: each row's cut is at
     * most the row's before. */
    int best = n;
    for (int r = 0; r <= row; r++) {
        int y = bottom ? n - 1 - r : r;
        /* A row with no line pixel at all lies wholly inside the curve. */
        int cut = 0;
        for (int x = 0; x < n; x++)
            if (cor_rgba[((size_t)y * cor_w + cell * n + x) * 4 + 3] >= 96) { cut = x; break; }
        if (cut < best) best = cut;
    }
    return best;
}

int w2k_aero_corner_rows(void)
{
    skins();
    return cor_rgba ? cor_h : 0;
}

/* ------------------------------------------------------------------ *
 * A window's frame
 * ------------------------------------------------------------------ */
#define AERO_B   8              /* the border, each side and along the bottom */
#define AERO_TOP 36             /* from the frame's top edge to the client */
#define AERO_BTN_X 112          /* the button cluster's left edge, from the frame's right */

void w2k_aero_frame(Drawable d, int dx, int dy, int rx, int ry, int fw, int fh,
                    int row0, int row1, int active, int btn_hot, int btn_down,
                    int close_only, int buttons)
{
    skins();
    int s = w2k_px(1) > 0 ? w2k_px(1) : 1;      /* line thickness */
    int b = w2k_px(AERO_B), top = w2k_px(AERO_TOP);
    if (row0 < 0) row0 = 0;
    if (row1 > fh) row1 = fh;
    if (row1 <= row0 || fw <= 0) return;

    /* Three pieces: the caption band, the two side strips, the bottom. */
    struct { int x, y, w, h; } parts[4];
    int np = 0;
    int cap1 = row1 < top ? row1 : top;
    if (row0 < top) { parts[np].x = 0; parts[np].y = row0; parts[np].w = fw; parts[np].h = cap1 - row0; np++; }
    int mid0 = row0 > top ? row0 : top, mid1 = row1 < fh - b ? row1 : fh - b;
    if (mid1 > mid0) {
        parts[np].x = 0;      parts[np].y = mid0; parts[np].w = b; parts[np].h = mid1 - mid0; np++;
        parts[np].x = fw - b; parts[np].y = mid0; parts[np].w = b; parts[np].h = mid1 - mid0; np++;
    }
    int bot0 = row0 > fh - b ? row0 : fh - b;
    if (row1 > bot0) { parts[np].x = 0; parts[np].y = bot0; parts[np].w = fw; parts[np].h = row1 - bot0; np++; }

    /* The pieces share one walk of the window stack. */
    if (w2k_glass_batch) w2k_glass_batch(1);
    for (int p = 0; p < np; p++) {
        int px = parts[p].x, py = parts[p].y, pw = parts[p].w, ph = parts[p].h;
        if (pw <= 0 || ph <= 0) continue;
        unsigned char *bg = w2k_glass_bg(rx + px, ry + py, pw, ph);
        unsigned char *out = malloc((size_t)pw * ph * 3);
        if (!bg || !out) { free(bg); free(out); continue; }
        w2k_glass_law(bg, out, (size_t)pw * ph, &w2k_glass_frame);
        /* The reflection. Its shape along x does not change down the
         * piece and its fade down y does not change across it, so both
         * are worked out once instead of two divisions and two
         * smoothsteps for every pixel. */
        int top = w2k_px(AERO_TOP);
        int *col = malloc((size_t)pw * sizeof *col);
        if (col) {
            for (int x = 0; x < pw; x++) {
                int gx = px + x;
                int l = gx < 42 * s ? 256 : 256 - (gx - 42 * s) * 256 / (132 * s);
                int r = 256 - (fw - 1 - gx) * 256 / (190 * s);
                int a = smooth(l) > smooth(r) ? smooth(l) : smooth(r);
                col[x] = (a * 95) >> 8;                 /* 0.37 of white */
            }
            for (int y = 0; y < ph; y++) {
                int gy = py + y, fade = 256;
                if (gy >= top) fade = smooth(256 - (gy - top) * 256 / (184 * s));
                if (!active) fade = fade * 3 / 5;
                if (fade <= 0) continue;
                unsigned char *row = out + (size_t)y * pw * 3;
                for (int x = 0; x < pw; x++) lighten(row + x * 3, col[x] * fade >> 8);
            }
            free(col);
        }
        /* Lines, in frame coordinates, clipped to the piece: white, black
         * or grey laid over the glass at the measured strengths. */
#define HL(Y, C, A) do { int yy = (Y); for (int i = 0; i < s; i++) ov_h(out, pw, ph, 0, yy + i - py, pw, C, A); } while (0)
#define VL(X, C, A) do { int xx = (X); for (int i = 0; i < s; i++) ov_v(out, pw, ph, xx + i - px, 0, ph, C, A); } while (0)
        HL(0, 0, 154);                 /* the outline: black 0.6 */
        HL(s, 255, 154);               /* the light line inside it: white 0.6 */
        HL(fh - 2 * s, 255, 154);
        HL(fh - s, 0, 154);
        VL(0, 0, 154);
        VL(s, 255, 154);
        VL(fw - 2 * s, 255, 154);
        VL(fw - s, 0, 154);
#undef HL
#undef VL
        /* The corners' curves. */
        if (cor_rgba) {
            int n = cor_h;
            composite(out, pw, ph, 0 - px, 0 - py, cor_rgba, cor_w, cor_h, 0, 0, n, n);
            composite(out, pw, ph, fw - n - px, 0 - py, cor_rgba, cor_w, cor_h, n, 0, n, n);
            composite(out, pw, ph, 0 - px, fh - n - py, cor_rgba, cor_w, cor_h, 2 * n, 0, n, n);
            composite(out, pw, ph, fw - n - px, fh - n - py, cor_rgba, cor_w, cor_h, 3 * n, 0, n, n);
        }
        /* The caption buttons: one strip of art, lit or pressed where the
         * pointer is, grey all over when the window is not the active one. */
        if (buttons && btn_rgba && py < top) {
            int bx = fw - w2k_px(AERO_BTN_X), by = 0;
            int x0 = close_only ? w2k_px(56) : 0;
            /* The inactive window's buttons are their own art (measured:
             * grey glass, Close no longer red); failing that, the active
             * ones with the colour taken out. */
            int inact = !active && btn2_rgba && btn2_w == btn_w && btn2_h == btn_h;
            size_t cell = (size_t)btn_w * btn_h * 4;
            /* Kept: the strip depends only on these four, so it is
             * composed once per state rather than on every repaint. */
            static unsigned char *art;
            static int art_key = -1, art_bytes;
            int key = (active ? 1 : 0) | (inact ? 2 : 0) | ((btn_hot + 1) << 2) |
                      ((btn_down + 1) << 5) | (w2k_ui_scale << 8);
            if (art && art_bytes != (int)cell) { free(art); art = NULL; art_key = -1; }
            if (!art) { art = malloc(cell); art_bytes = (int)cell; art_key = -1; }
            if (art && art_key != key) {
                art_key = key;
                memcpy(art, inact ? btn2_rgba : btn_rgba, cell);
                /* Skin columns of each button: Minimise 1..29, Maximise
                 * 30..56, Close 57..106 -- measured. */
                int lo[3] = { w2k_px(1), w2k_px(30), w2k_px(57) }, hi[3] = { w2k_px(30), w2k_px(57), btn_w };
                for (int k = 0; k < 3; k++) {
                    int state = btn_down == k ? 2 : btn_hot == k ? 1 : 0;
                    for (int y = 0; y < btn_h; y++)
                        for (int x = lo[k]; x < hi[k] && x < btn_w; x++) {
                            unsigned char *q = art + ((size_t)y * btn_w + x) * 4;
                            if (!q[3]) continue;
                            if (!active && !inact) {
                                int l = (77 * q[0] + 150 * q[1] + 29 * q[2]) >> 8;
                                q[0] = q[1] = q[2] = (unsigned char)l;
                            }
                            if (state == 1) lighten(q, 90);
                            else if (state == 2) darken(q, 70);
                        }
                }
            }
            if (art)
                composite(out, pw, ph, bx + x0 - px, by - py, art, btn_w, btn_h,
                          x0, 0, btn_w - x0, btn_h);
        }
        w2k_rgb_put(d, dx + px, dy + py, out, pw, ph);
        free(bg);
        free(out);
    }
    if (w2k_glass_batch) w2k_glass_batch(0);
}

/* ------------------------------------------------------------------ *
 * The taskbar
 * ------------------------------------------------------------------ */
/* edge: 0 bottom, 1 top, 2 left, 3 right -- the side that faces the
 * desktop gets the dark line and the highlight. `sliver` draws Show
 * Desktop at the far end. */
void w2k_aero_bar(Drawable d, int rx, int ry, int w, int h, int edge, int sliver)
{
    unsigned char *bg = w2k_glass_bg(rx, ry, w, h);
    unsigned char *out = malloc((size_t)w * h * 3);
    if (!bg || !out) { free(bg); free(out); return; }
    int s = w2k_px(1) > 0 ? w2k_px(1) : 1;
    w2k_glass_law(bg, out, (size_t)w * h, &w2k_glass_bar);
    /* The edge that faces the desktop: black 0.5, then white 0.42. */
    for (int i = 0; i < s; i++) {
        switch (edge) {
        case 1:  ov_h(out, w, h, 0, h - 1 - i, w, 0, 128); ov_h(out, w, h, 0, h - 1 - s - i, w, 255, 108); break;
        case 2:  ov_v(out, w, h, w - 1 - i, 0, h, 0, 128); ov_v(out, w, h, w - 1 - s - i, 0, h, 255, 108); break;
        case 3:  ov_v(out, w, h, i, 0, h, 0, 128);         ov_v(out, w, h, s + i, 0, h, 255, 108); break;
        default: ov_h(out, w, h, 0, i, w, 0, 128);         ov_h(out, w, h, 0, s + i, w, 255, 108); break;
        }
    }
    if (sliver && edge < 2) {
        /* Show Desktop: a shade darker, in a dark line with a light one
         * inside it on the left and a faint one on the right. Measured. */
        int sw = w2k_px(15), x0 = w - sw, y0 = edge == 1 ? 0 : 2 * s, sh = h - 2 * s;
        for (int y = y0; y < y0 + sh; y++) ov_h(out, w, h, x0, y, sw, 0, 28);
        for (int i = 0; i < s; i++) {
            ov_v(out, w, h, x0 + i, y0, sh, 0, 72);
            ov_v(out, w, h, x0 + s + i, y0, sh, 255, 84);
            ov_v(out, w, h, w - 2 * s + i, y0, sh, 255, 33);
            ov_v(out, w, h, w - s + i, y0, sh, 0, 72);
        }
    }
    w2k_rgb_put(d, 0, 0, out, w, h);
    free(bg);
    free(out);
}

/* A task button: the bar's glass with white laid over it, brightest along
 * the top, in a dark outline with a light line inside. Measured on the
 * active button; the idle one is a faint echo of it, the lit one brighter.
 * The rectangle is in the bar's own coordinates; (rx, ry) is the bar's
 * root position. */
void w2k_aero_taskbutton(Drawable d, int x, int y, int w, int h, int state, int rx, int ry)
{
    if (w <= 2 || h <= 2) return;
    unsigned char *bg = w2k_glass_bg(rx + x, ry + y, w, h);
    unsigned char *out = malloc((size_t)w * h * 3);
    if (!bg || !out) { free(bg); free(out); return; }
    w2k_glass_law(bg, out, (size_t)w * h, &w2k_glass_bar);
    int s = w2k_px(1) > 0 ? w2k_px(1) : 1;
    /* The white over each row of the thirty-row reference, 0..256. */
    static const unsigned char ref[30] = {
        0, 230, 184, 176, 168, 160, 152, 142, 132, 120, 110, 100,  92,  84,
        76,  68,  60,  52,  46,  40,  34,  28,  24,  24,  24,  24,  24,  30,  82, 0
    };
    int body = state == W2K_TB_DOWN ? 256 : state == W2K_TB_HOT ? 256 : 90;
    int lift = state == W2K_TB_HOT ? 80 : 0;           /* the lit button's extra */
    int line = state == W2K_TB_NORMAL ? 128 : 256;
    for (int yy = 0; yy < h; yy++) {
        int r = yy * 30 / h;
        int a = (ref[r] * body >> 8) + lift;
        if (yy < s || yy >= h - s) a = 0;
        for (int xx = s; xx < w - s; xx++) lighten(out + ((size_t)yy * w + xx) * 3, a);
    }
    /* The outline: dark, with a light line inside it. */
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            unsigned char *p = out + ((size_t)yy * w + xx) * 3;
            int ox = xx < s || xx >= w - s, oy = yy < s || yy >= h - s;
            int ix = (xx >= s && xx < 2 * s) || (xx >= w - 2 * s && xx < w - s);
            int iy = (yy >= s && yy < 2 * s) || (yy >= h - 2 * s && yy < h - s);
            if (ox || oy) darken(p, 190 * line >> 8);
            else if (ix || iy) lighten(p, 200 * line >> 8);
        }
    w2k_rgb_put(d, x, y, out, w, h);
    free(bg);
    free(out);
}

/* ------------------------------------------------------------------ *
 * The Start menu's slab
 * ------------------------------------------------------------------ */
/* The panel's ground: dark glass with a lighter band along the top and a
 * blue-white one along the bottom, in a dark line with a light one inside;
 * the white pane cut into it, outlined, with the search band across its
 * foot. All in the panel's pixels; (rx, ry) is the slab's root position. */
static unsigned char *tile_rgba; static int tile_w, tile_h, tile_tried, tile_scale;

void w2k_aero_panel(Drawable d, int dx, int dy, int rx, int ry, int w, int h, int over,
                    int pane_x, int pane_y, int pane_w, int pane_h, int band_y, int tile_x)
{
    /* The whole pixmap: `over` rows of desktop above the slab, where only
     * the user's tile is window (the rest is shaped away), then the slab. */
    int th = over + h;
    unsigned char *bg = w2k_glass_bg(rx, ry, w, th);
    unsigned char *out = malloc((size_t)w * th * 3);
    if (!bg || !out) { free(bg); free(out); return; }
    int s = w2k_px(1) > 0 ? w2k_px(1) : 1;
    memcpy(out, bg, (size_t)w * over * 3);
    /* The slab is the taskbar's glass: one piece with the bar it stands on. */
    w2k_glass_law(bg + (size_t)w * over * 3, out + (size_t)w * over * 3, (size_t)w * h, &w2k_glass_bar);
    /* The tile: its frame, cut from Windows 7 with its transparency; the
     * picture inside is the caller's. */
    /* Keyed on its own scale: skins_scale belongs to skins(), which this
     * function never calls, so testing it reloaded the art every time. */
    if (tile_scale != w2k_ui_scale) { free(tile_rgba); tile_rgba = NULL; tile_tried = 0; tile_scale = w2k_ui_scale; }
    if (!tile_tried) { tile_tried = 1; tile_rgba = load_rgba("aero-usertile.png", &tile_w, &tile_h); }
    unsigned char *slab = out + (size_t)w * over * 3;
    bg += (size_t)w * over * 3;
    out = slab;
#define OUT_BASE (slab - (size_t)w * over * 3)
    /* Its edge: the bar's dark line with the light one inside it. */
    ov_h(out, w, h, 0, 0, w, 0, 128);          ov_h(out, w, h, 0, s, w, 255, 108);
    ov_h(out, w, h, 0, h - s, w, 0, 128);      ov_h(out, w, h, 0, h - 2 * s, w, 255, 108);
    ov_v(out, w, h, 0, 0, h, 0, 128);          ov_v(out, w, h, s, 0, h, 255, 108);
    ov_v(out, w, h, w - s, 0, h, 0, 128);      ov_v(out, w, h, w - 2 * s, 0, h, 255, 108);
    /* The white pane and its outline. */
    if (pane_w > 0 && pane_h > 0) {
        /* A light line, then a dark one, round the pane. */
        ov_h(out, w, h, pane_x - 2 * s, pane_y - 2 * s, pane_w + 4 * s, 255, 80);
        ov_h(out, w, h, pane_x - 2 * s, pane_y + pane_h + s, pane_w + 4 * s, 255, 80);
        ov_v(out, w, h, pane_x - 2 * s, pane_y - 2 * s, pane_h + 4 * s, 255, 80);
        ov_v(out, w, h, pane_x + pane_w + s, pane_y - 2 * s, pane_h + 4 * s, 255, 80);
        ov_h(out, w, h, pane_x - s, pane_y - s, pane_w + 2 * s, 0, 90);
        ov_h(out, w, h, pane_x - s, pane_y + pane_h, pane_w + 2 * s, 0, 90);
        ov_v(out, w, h, pane_x - s, pane_y - s, pane_h + 2 * s, 0, 90);
        ov_v(out, w, h, pane_x + pane_w, pane_y - s, pane_h + 2 * s, 0, 90);
        for (int y = pane_y; y < pane_y + pane_h && y < h; y++) {
            /* White, then the search band: (241,245,251) under a five-row
             * shadow from (204,217,234). */
            unsigned char col[3] = { 255, 255, 255 };
            if (band_y >= 0 && y >= band_y) {
                int i = (y - band_y) / s;
                static const unsigned char sh[5][3] = { {204,217,234},{217,227,240},{232,238,247},{237,242,249},{240,244,250} };
                if (i < 5) memcpy(col, sh[i], 3);
                else { col[0] = 241; col[1] = 245; col[2] = 251; }
                if (y >= pane_y + pane_h - s) { col[0] = 237; col[1] = 243; col[2] = 250; }
            }
            for (int x = pane_x; x < pane_x + pane_w && x < w; x++)
                if (x >= 0 && y >= 0) memcpy(out + ((size_t)y * w + x) * 3, col, 3);
        }
    }
    if (tile_rgba && over > 0)
        composite(OUT_BASE, w, th, tile_x, 0, tile_rgba, tile_w, tile_h, 0, 0, tile_w, tile_h);
    w2k_rgb_put(d, dx, dy, OUT_BASE, w, th);
    free(bg - (size_t)w * over * 3);
    free(OUT_BASE);
#undef OUT_BASE
}

/* A glass button on the Start menu's slab (Shut down, and the lit row
 * under the pointer): the dark glass a shade lighter, in a light line
 * with a fainter one inside it; `divider` splits it at that column
 * (-1 for none); `hot` lights it. Rectangle and root position in screen
 * pixels. */
void w2k_aero_button(Drawable d, int dx, int dy, int rx, int ry, int w, int h, int divider, int hot)
{
    if (w <= 2 || h <= 2) return;
    unsigned char *bg = w2k_glass_bg(rx, ry, w, h);
    unsigned char *out = malloc((size_t)w * h * 3);
    if (!bg || !out) { free(bg); free(out); return; }
    int s = w2k_px(1) > 0 ? w2k_px(1) : 1;
    w2k_glass_law(bg, out, (size_t)w * h, &w2k_glass_bar);
    int fill = hot ? 70 : 22;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *p = out + ((size_t)y * w + x) * 3;
            int ox = x < s || x >= w - s, oy = y < s || y >= h - s;
            int ix = (x >= s && x < 2 * s) || (x >= w - 2 * s && x < w - s);
            int iy = (y >= s && y < 2 * s) || (y >= h - 2 * s && y < h - s);
            int corner = (x < s || x >= w - s) && (y < s || y >= h - s);
            if (corner) continue;                       /* rounded: the glass shows */
            if (ox || oy) lighten(p, 60);
            else if (ix || iy) lighten(p, 90);
            else lighten(p, fill);
            if (divider >= 0 && (x == divider || x == divider + s)) lighten(p, x == divider ? 60 : 30);
        }
    w2k_rgb_put(d, dx, dy, out, w, h);
    free(bg);
    free(out);
}

/* A plain piece of glass (the Start menu's hover rows, tooltips...). */
void w2k_aero_glass(Drawable d, int dx, int dy, int rx, int ry, int w, int h, const W2kGlass *g)
{
    unsigned char *bg = w2k_glass_bg(rx, ry, w, h);
    unsigned char *out = malloc((size_t)w * h * 3);
    if (!bg || !out) { free(bg); free(out); return; }
    w2k_glass_law(bg, out, (size_t)w * h, g);
    w2k_rgb_put(d, dx, dy, out, w, h);
    free(bg);
    free(out);
}
