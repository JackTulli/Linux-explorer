/* png.c -- just enough PNG to read an icon.
 *
 * Application icons come from the icon themes as PNG, so the toolkit needs
 * to read them. This handles what those files actually are: 8-bit greyscale,
 * palette, RGB and RGBA, non-interlaced, which covers every icon theme on a
 * normal system. Anything else (16-bit samples, Adam7 interlace) is refused
 * rather than half-decoded.
 *
 * zlib does the inflating; the rest is the filter reconstruction from the
 * specification. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
}

/* Undo the per-row filters, in place, over `raw` (each row prefixed by its
 * filter byte). `bpp` is bytes per pixel, rounded up. */
static void unfilter(unsigned char *raw, int w, int h, int bpp, int stride)
{
    unsigned char *prev = NULL;
    for (int y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * (stride + 1);
        int filter = row[0];
        unsigned char *cur = row + 1;
        for (int i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            switch (filter) {
            case 1: cur[i] = (unsigned char)(cur[i] + a); break;
            case 2: cur[i] = (unsigned char)(cur[i] + b); break;
            case 3: cur[i] = (unsigned char)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (unsigned char)(cur[i] + paeth(a, b, c)); break;
            default: break;                                  /* 0: none */
            }
        }
        prev = cur;
        (void)w;
    }
}

/* Decode `path` into a fresh RGBA buffer. Returns NULL on anything it does
 * not understand. */
unsigned char *w2k_png_load(const char *path, int *out_w, int *out_h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 8 || len > (16 << 20)) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *file = malloc((size_t)len);
    if (!file || fread(file, 1, (size_t)len, f) != (size_t)len) {
        free(file);
        fclose(f);
        return NULL;
    }
    fclose(f);

    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (memcmp(file, sig, 8)) { free(file); return NULL; }

    int w = 0, h = 0, depth = 0, color = -1, interlace = 0;
    unsigned char *idat = NULL;
    size_t idat_len = 0;
    unsigned char pal[256 * 3];
    int npal = 0;
    unsigned char alpha[256];
    int nalpha = 0;

    for (long p = 8; p + 8 <= len; ) {
        unsigned clen = be32(file + p);
        const unsigned char *type = file + p + 4;
        const unsigned char *data = file + p + 8;
        if (clen > (unsigned)(len - p - 12)) break;

        if (!memcmp(type, "IHDR", 4) && clen >= 13) {
            w = (int)be32(data);
            h = (int)be32(data + 4);
            depth = data[8];
            color = data[9];
            interlace = data[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = (int)(clen / 3);
            if (npal > 256) npal = 256;
            memcpy(pal, data, (size_t)npal * 3);
        } else if (!memcmp(type, "tRNS", 4)) {
            nalpha = (int)(clen > 256 ? 256 : clen);
            memcpy(alpha, data, (size_t)nalpha);
        } else if (!memcmp(type, "IDAT", 4)) {
            unsigned char *grown = realloc(idat, idat_len + clen);
            if (!grown) { free(idat); free(file); return NULL; }
            idat = grown;
            memcpy(idat + idat_len, data, clen);
            idat_len += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        p += 12 + clen;
    }

    int channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 :
                   color == 4 ? 2 : color == 6 ? 4 : 0;
    if (!idat || w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
        depth != 8 || !channels || interlace) {
        free(idat);
        free(file);
        return NULL;
    }

    int stride = w * channels;
    size_t rawlen = (size_t)(stride + 1) * h;
    unsigned char *raw = malloc(rawlen);
    unsigned char *rgba = malloc((size_t)w * h * 4);
    if (!raw || !rgba) { free(raw); free(rgba); free(idat); free(file); return NULL; }

    uLongf got = (uLongf)rawlen;
    int rc = uncompress(raw, &got, idat, (uLong)idat_len);
    free(idat);
    free(file);
    if (rc != Z_OK || got != rawlen) { free(raw); free(rgba); return NULL; }

    unfilter(raw, w, h, channels, stride);

    for (int y = 0; y < h; y++) {
        const unsigned char *row = raw + (size_t)y * (stride + 1) + 1;
        for (int x = 0; x < w; x++) {
            unsigned char *o = rgba + ((size_t)y * w + x) * 4;
            const unsigned char *s = row + x * channels;
            switch (color) {
            case 0: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
            case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
            case 3: {
                int idx = s[0] < npal ? s[0] : 0;
                o[0] = pal[idx * 3]; o[1] = pal[idx * 3 + 1]; o[2] = pal[idx * 3 + 2];
                o[3] = idx < nalpha ? alpha[idx] : 255;
                break;
            }
            case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
            default: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
            }
        }
    }
    free(raw);
    *out_w = w;
    *out_h = h;
    return rgba;
}

/* ------------------------------------------------------------------ *
 * Writing: 8-bit RGBA, no filtering, one IDAT. Enough for a screenshot.
 * ------------------------------------------------------------------ */
static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, size_t n)
{
    unsigned char head[8];
    put32(head, (unsigned long)n);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    unsigned long crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const unsigned char *)type, 4);
    if (n) crc = crc32(crc, data, (uInt)n);
    unsigned char tail[4];
    put32(tail, crc);
    fwrite(tail, 1, 4, f);
}

/* The file's bytes for an RGBA picture, malloc'd; the clipboard hands
 * these out as image/png. NULL on failure. */
unsigned char *w2k_png_encode(const unsigned char *rgba, int w, int h, size_t *out_n)
{
    if (!rgba || w <= 0 || h <= 0) return NULL;
    size_t stride = (size_t)w * 4 + 1, raw_n = stride * (size_t)h;
    unsigned char *raw = malloc(raw_n);
    if (!raw) return NULL;
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * stride] = 0;                       /* filter: none */
        memcpy(raw + (size_t)y * stride + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    uLongf zn = compressBound((uLong)raw_n);
    unsigned char *z = malloc(zn);
    if (!z || compress2(z, &zn, raw, (uLong)raw_n, 6) != Z_OK) { free(raw); free(z); return NULL; }
    free(raw);

    FILE *f = tmpfile();
    if (!f) { free(z); return NULL; }
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    put32(ihdr, (unsigned long)w);
    put32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, zn);
    chunk(f, "IEND", NULL, 0);
    free(z);
    long n = ftell(f);
    unsigned char *out = n > 0 ? malloc((size_t)n) : NULL;
    if (out) {
        rewind(f);
        if (fread(out, 1, (size_t)n, f) != (size_t)n) { free(out); out = NULL; }
    }
    fclose(f);
    if (out && out_n) *out_n = (size_t)n;
    return out;
}

int w2k_png_save(const char *path, const unsigned char *rgba, int w, int h)
{
    size_t n = 0;
    unsigned char *bytes = w2k_png_encode(rgba, w, h, &n);
    if (!bytes) return 0;
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(bytes, 1, n, f) == n;
    if (f) fclose(f);
    free(bytes);
    return ok;
}
