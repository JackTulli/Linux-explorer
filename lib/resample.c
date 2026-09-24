/* resample.c -- resizing RGBA pictures: the desktop's icons, the XP and
 * Windows 7 chrome, the pointer, the wallpaper and Imaging's pictures.
 *
 * Separable and phase-correct: rows are filtered across, then the results
 * down, each output sample centred where it belongs in the source; the
 * kernel widens when shrinking so every source pixel counts. Colour is
 * premultiplied by alpha through the filter so a transparent edge does
 * not bleed its hidden colour. A whole-number enlargement is exact
 * blocks whatever the method (the callers ask for nearest then).
 *
 * The filter weights for each output row and column are worked out once
 * -- a kernel with a sine in it is evaluated a few thousand times, not a
 * few hundred million -- and the picture streams through: a ring of
 * across-filtered source rows, just enough for one output row's taps, is
 * all that is held, so a 4K wallpaper costs well under a megabyte rather
 * than three float copies of itself. */
#include "w2k.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int w2k_resample = RS_CUBIC;

static double kern(int method, double x)
{
    x = fabs(x);
    switch (method) {
    case RS_BILINEAR:
        return x < 1.0 ? 1.0 - x : 0.0;
    case RS_LANCZOS:
        if (x < 1e-9) return 1.0;
        if (x >= 3.0) return 0.0;
        {
            double px = M_PI * x;
            return sin(px) / px * sin(px / 3.0) / (px / 3.0);
        }
    default: {                       /* Catmull-Rom, a = -1/2 */
        if (x < 1.0) return 1.5 * x * x * x - 2.5 * x * x + 1.0;
        if (x < 2.0) return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
        return 0.0;
    }
    }
}

static double support(int method)
{
    return method == RS_BILINEAR ? 1.0 : method == RS_LANCZOS ? 3.0 : 2.0;
}

/* The taps of one axis: for every output position, the first source
 * index and the normalised weights of the run that feeds it. */
typedef struct { int j0, n; } Tap;

typedef struct {
    Tap   *tap;          /* n_out of them */
    double *w;           /* n_out * stride weights */
    int    stride;       /* the most taps any position has */
} Taps;

static int make_taps(Taps *t, int n_in, int n_out, int method)
{
    double scale = (double)n_in / n_out;         /* source per dest */
    double widen = scale > 1.0 ? scale : 1.0;
    double sup = support(method) * widen;
    int stride = (int)(2.0 * sup) + 3;
    t->stride = stride;
    t->tap = malloc((size_t)n_out * sizeof *t->tap);
    t->w = malloc((size_t)n_out * stride * sizeof *t->w);
    if (!t->tap || !t->w) { free(t->tap); free(t->w); t->tap = NULL; t->w = NULL; return 0; }
    for (int i = 0; i < n_out; i++) {
        double centre = (i + 0.5) * scale - 0.5;
        int j0 = (int)ceil(centre - sup), j1 = (int)floor(centre + sup);
        if (j0 < 0) j0 = 0;
        if (j1 > n_in - 1) j1 = n_in - 1;
        if (j1 - j0 + 1 > stride) j1 = j0 + stride - 1;
        double *w = t->w + (size_t)i * stride;
        double wsum = 0;
        int n = 0;
        for (int j = j0; j <= j1; j++) {
            double k = kern(method, (j - centre) / widen);
            w[n++] = k;
            wsum += k;
        }
        if (wsum != 0.0)
            for (int k = 0; k < n; k++) w[k] /= wsum;
        else
            n = 0;
        t->tap[i].j0 = j0;
        t->tap[i].n = n;
    }
    return 1;
}

/* One source row, premultiplied and filtered across into `out`, `nc`
 * doubles a pixel: 3 for an opaque picture, whose alpha is left out. */
static void across(const unsigned char *src, int sw, double *srow,
                   const Taps *hx, int dw, double *out, int nc)
{
    if (nc == 3) {
        for (int x = 0; x < sw; x++) {
            const unsigned char *p = src + (size_t)x * 4;
            double *q = srow + (size_t)x * 3;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
        }
        for (int x = 0; x < dw; x++) {
            const Tap *t = &hx->tap[x];
            const double *w = hx->w + (size_t)x * hx->stride;
            double a0 = 0, a1 = 0, a2 = 0;
            const double *q = srow + (size_t)t->j0 * 3;
            for (int k = 0; k < t->n; k++, q += 3) {
                double wk = w[k];
                a0 += wk * q[0]; a1 += wk * q[1]; a2 += wk * q[2];
            }
            double *o = out + (size_t)x * 3;
            o[0] = a0; o[1] = a1; o[2] = a2;
        }
        return;
    }
    for (int x = 0; x < sw; x++) {
        const unsigned char *p = src + (size_t)x * 4;
        double al = p[3] / 255.0;
        double *q = srow + (size_t)x * 4;
        q[0] = p[0] * al; q[1] = p[1] * al; q[2] = p[2] * al; q[3] = p[3];
    }
    for (int x = 0; x < dw; x++) {
        const Tap *t = &hx->tap[x];
        const double *w = hx->w + (size_t)x * hx->stride;
        double a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        const double *q = srow + (size_t)t->j0 * 4;
        for (int k = 0; k < t->n; k++, q += 4) {
            double wk = w[k];
            a0 += wk * q[0]; a1 += wk * q[1]; a2 += wk * q[2]; a3 += wk * q[3];
        }
        double *o = out + (size_t)x * 4;
        o[0] = a0; o[1] = a1; o[2] = a2; o[3] = a3;
    }
}

unsigned char *w2k_rgba_resample(const unsigned char *src, int sw, int sh,
                                 int dw, int dh, int method)
{
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return NULL;
    unsigned char *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;

    if (method == RS_NEAREST || (sw == dw && sh == dh)) {
        for (int y = 0; y < dh; y++) {
            int sy = (int)((long)y * sh / dh);
            const unsigned char *srow = src + (size_t)sy * sw * 4;
            unsigned char *drow = dst + (size_t)y * dw * 4;
            if (sw == dw) { memcpy(drow, srow, (size_t)dw * 4); continue; }
            for (int x = 0; x < dw; x++) {
                int sx = (int)((long)x * sw / dw);
                memcpy(drow + (size_t)x * 4, srow + (size_t)sx * 4, 4);
            }
        }
        return dst;
    }

    Taps hx = { 0 }, vy = { 0 };
    if (!make_taps(&hx, sw, dw, method) || !make_taps(&vy, sh, dh, method)) {
        free(hx.tap); free(hx.w); free(vy.tap); free(vy.w); free(dst);
        return NULL;
    }
    /* A picture with no transparency in it -- a wallpaper, a photo -- is
     * filtered as three channels, not four: multiplying by an alpha of
     * 1.0, carrying a constant 255 through every tap and dividing it out
     * again was a fifth of a 4K wallpaper's resizing. The colour sums are
     * the same ones, in the same order; only that division is gone. */
    int nc = 3;
    for (size_t i = 3, n = (size_t)sw * sh * 4; i < n; i += 4)
        if (src[i] != 255) { nc = 4; break; }

    /* The ring of across-filtered source rows: one output row's taps
     * plus one, addressed by source row number, so the rows a window
     * shares with the next are filtered once. */
    int ring = vy.stride + 1;
    double *rows = malloc((size_t)ring * dw * nc * sizeof *rows);
    int *row_of = malloc((size_t)ring * sizeof *row_of);
    double *srow = malloc((size_t)sw * 4 * sizeof *srow);
    double *acc = malloc((size_t)dw * nc * sizeof *acc);
    if (!rows || !row_of || !srow || !acc) {
        free(rows); free(row_of); free(srow); free(acc);
        free(hx.tap); free(hx.w); free(vy.tap); free(vy.w); free(dst);
        return NULL;
    }
    for (int i = 0; i < ring; i++) row_of[i] = -1;

    for (int y = 0; y < dh; y++) {
        const Tap *t = &vy.tap[y];
        const double *w = vy.w + (size_t)y * vy.stride;
        memset(acc, 0, (size_t)dw * nc * sizeof *acc);
        for (int k = 0; k < t->n; k++) {
            int j = t->j0 + k;
            int slot = j % ring;
            if (row_of[slot] != j) {
                across(src + (size_t)j * sw * 4, sw, srow, &hx, dw, rows + (size_t)slot * dw * nc, nc);
                row_of[slot] = j;
            }
            const double *r = rows + (size_t)slot * dw * nc;
            double wk = w[k];
            for (int x = 0; x < dw * nc; x++) acc[x] += wk * r[x];
        }
        unsigned char *o = dst + (size_t)y * dw * 4;
        if (nc == 3) {
            /* Rounded as below; every tap's alpha was 255, so is the
             * result's, and there is nothing to divide by. */
            for (int x = 0; x < dw; x++) {
                for (int c = 0; c < 3; c++) {
                    double v = acc[x * 3 + c];
                    o[x * 4 + c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5 + 1e-9);
                }
                o[x * 4 + 3] = 255;
            }
            continue;
        }
        for (int x = 0; x < dw; x++) {
            /* Halves round up, and a little more than half of a unit is
             * added so that an exact tie -- common at 150%, where a
             * hard edge lands at phase 1/2 -- rounds the same way whatever
             * the order the weights were summed in. */
            double al = acc[x * 4 + 3];
            if (al < 0) al = 0;
            if (al > 255) al = 255;
            for (int c = 0; c < 3; c++) {
                double v = al > 0.5 ? acc[x * 4 + c] * 255.0 / al : 0.0;
                o[x * 4 + c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5 + 1e-9);
            }
            o[x * 4 + 3] = (unsigned char)(al + 0.5 + 1e-9);
        }
    }
    free(rows); free(row_of); free(srow); free(acc);
    free(hx.tap); free(hx.w); free(vy.tap); free(vy.w);
    return dst;
}
