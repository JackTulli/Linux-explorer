/* bmp.c -- a reader for the wallpaper format Windows 2000 actually used.
 * Uncompressed 1/4/8/24/32-bit BI_RGB, bottom-up or top-down. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static long le32(const unsigned char *p)
{
    return (long)(p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long)p[3] << 24));
}

unsigned char *w2k_bmp_load(const char *path, int *wout, int *hout)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 54 || n > (64L << 20)) { fclose(f); return NULL; }
    unsigned char *buf = w2k_alloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    if (buf[0] != 'B' || buf[1] != 'M') { free(buf); return NULL; }

    long off = le32(buf + 10), hdr = le32(buf + 14);
    int w = (int)le32(buf + 18), h = (int)le32(buf + 22);
    int bpp = le16(buf + 28);
    long comp = le32(buf + 30);
    long ncol = le32(buf + 46);
    int topdown = h < 0;
    if (topdown) h = -h;
    if (comp != 0 || w <= 0 || h <= 0 || w > 8192 || h > 8192) { free(buf); return NULL; }
    if (bpp <= 8 && ncol == 0) ncol = 1L << bpp;
    const unsigned char *pal = buf + 14 + hdr;
    long stride = ((long)w * bpp + 31) / 32 * 4;
    if (off + stride * h > n) { free(buf); return NULL; }

    unsigned char *out = w2k_alloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        const unsigned char *row = buf + off + stride * (topdown ? y : h - 1 - y);
        for (int x = 0; x < w; x++) {
            unsigned char *o = out + ((size_t)y * w + x) * 4;
            unsigned r = 0, g = 0, b = 0, idx = 0;
            switch (bpp) {
            case 1:  idx = (row[x >> 3] >> (7 - (x & 7))) & 1; break;
            case 4:  idx = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15; break;
            case 8:  idx = row[x]; break;
            case 24: b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2]; break;
            case 32: b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2]; break;
            default: free(out); free(buf); return NULL;
            }
            if (bpp <= 8) {
                if ((long)idx >= ncol) idx = 0;
                b = pal[idx * 4]; g = pal[idx * 4 + 1]; r = pal[idx * 4 + 2];
            }
            o[0] = r; o[1] = g; o[2] = b; o[3] = 255;
        }
    }
    free(buf);
    *wout = w;
    *hout = h;
    return out;
}
