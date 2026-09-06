/* resample.c -- resizing pictures with a choice of filter.
 *
 * The desktop enlarges its own artwork -- icons, the XP and 7 chrome, the
 * pointer, the wallpaper -- when it is scaled by a fraction, and the
 * filter that does it is the user's choice: nearest neighbour (blocks),
 * bilinear (soft), a cubic spline (Catmull-Rom: crisp with a little ring)
 * or Lanczos-3 (crispest, rings more). Separable, phase-correct, in
 * premultiplied colour so transparent edges do not bleed black, and the
 * kernel widens when shrinking so nothing is skipped. */
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

/* One axis: `n_in` premultiplied float pixels (4 channels, `stride`
 * floats apart) to `n_out`. */
static void pass(const float *in, int n_in, int in_stride,
                 float *out, int n_out, int out_stride, int method)
{
    double scale = (double)n_in / n_out;         /* source per dest */
    double widen = scale > 1.0 ? scale : 1.0;
    double sup = support(method) * widen;
    for (int i = 0; i < n_out; i++) {
        double centre = (i + 0.5) * scale - 0.5;
        int j0 = (int)ceil(centre - sup), j1 = (int)floor(centre + sup);
        if (j0 < 0) j0 = 0;
        if (j1 > n_in - 1) j1 = n_in - 1;
        double acc[4] = { 0, 0, 0, 0 }, wsum = 0;
        for (int j = j0; j <= j1; j++) {
            double w = kern(method, (j - centre) / widen);
            if (w == 0.0) continue;
            const float *p = in + (size_t)j * in_stride;
            acc[0] += w * p[0]; acc[1] += w * p[1]; acc[2] += w * p[2]; acc[3] += w * p[3];
            wsum += w;
        }
        float *o = out + (size_t)i * out_stride;
        if (wsum == 0.0) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
        o[0] = (float)(acc[0] / wsum); o[1] = (float)(acc[1] / wsum);
        o[2] = (float)(acc[2] / wsum); o[3] = (float)(acc[3] / wsum);
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
            for (int x = 0; x < dw; x++) {
                int sx = (int)((long)x * sw / dw);
                memcpy(dst + ((size_t)y * dw + x) * 4, src + ((size_t)sy * sw + sx) * 4, 4);
            }
        }
        return dst;
    }

    /* Premultiply into floats, resample rows then columns. */
    float *a = malloc((size_t)sw * sh * 4 * sizeof *a);
    float *b = malloc((size_t)dw * sh * 4 * sizeof *b);
    float *c = malloc((size_t)dw * dh * 4 * sizeof *c);
    if (!a || !b || !c) { free(a); free(b); free(c); free(dst); return NULL; }
    for (size_t i = 0; i < (size_t)sw * sh; i++) {
        const unsigned char *p = src + i * 4;
        float al = p[3] / 255.0f;
        a[i * 4] = p[0] * al; a[i * 4 + 1] = p[1] * al; a[i * 4 + 2] = p[2] * al; a[i * 4 + 3] = p[3];
    }
    for (int y = 0; y < sh; y++)
        pass(a + (size_t)y * sw * 4, sw, 4, b + (size_t)y * dw * 4, dw, 4, method);
    for (int x = 0; x < dw; x++)
        pass(b + (size_t)x * 4, sh, dw * 4, c + (size_t)x * 4, dh, dw * 4, method);
    for (size_t i = 0; i < (size_t)dw * dh; i++) {
        float al = c[i * 4 + 3];
        if (al < 0) al = 0;
        if (al > 255) al = 255;
        unsigned char *o = dst + i * 4;
        for (int k = 0; k < 3; k++) {
            float v = al > 0.5f ? c[i * 4 + k] * 255.0f / al : 0.0f;
            o[k] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
        }
        o[3] = (unsigned char)(al + 0.5f);
    }
    free(a); free(b); free(c);
    return dst;
}
