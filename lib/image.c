/* image.c -- load a picture, whatever it happens to be.
 *
 * The format is decided by what is in the file, not by the extension: a
 * .jpg that is really a PNG opens anyway, which is the behaviour anyone
 * double-clicking a file expects. PNG is decoded here in the toolkit
 * (png.c), BMP too (bmp.c), and JPEG through libjpeg. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>

/* libjpeg's default error handler exits the process. Longjmp out instead. */
struct jpeg_bail {
    struct jpeg_error_mgr mgr;
    jmp_buf back;
};

static void jpeg_bail_out(j_common_ptr cinfo)
{
    struct jpeg_bail *e = (struct jpeg_bail *)cinfo->err;
    longjmp(e->back, 1);
}

unsigned char *w2k_jpeg_load(const char *path, int *out_w, int *out_h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_bail err;
    /* Both buffers must be visible to the error path: libjpeg longjmps out
     * of the middle of the decode on a corrupt file. */
    unsigned char *volatile rgba = NULL, *volatile line = NULL;

    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_bail_out;
    if (setjmp(err.back)) {
        free(rgba);
        free(line);
        jpeg_destroy_decompress(&cinfo);
        free(rgba);
        free(line);
        fclose(f);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return NULL;
    }
    rgba = malloc((size_t)w * h * 4);
    line = malloc((size_t)w * 3);
    if (!rgba || !line) {
        free(rgba); free(line);
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return NULL;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        int y = (int)cinfo.output_scanline;
        JSAMPROW lp = line;
        jpeg_read_scanlines(&cinfo, &lp, 1);
        for (int x = 0; x < w; x++) {
            unsigned char *o = rgba + ((size_t)y * w + x) * 4;
            o[0] = line[x * 3];
            o[1] = line[x * 3 + 1];
            o[2] = line[x * 3 + 2];
            o[3] = 255;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(line);
    fclose(f);

    *out_w = w;
    *out_h = h;
    return rgba;
}

unsigned char *w2k_image_load(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char magic[8] = { 0 };
    size_t n = fread(magic, 1, sizeof magic, f);
    fclose(f);
    if (n < 4) return NULL;

    static const unsigned char png_sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (n >= 8 && !memcmp(magic, png_sig, 8))       return w2k_png_load(path, w, h);
    if (magic[0] == 0xff && magic[1] == 0xd8)       return w2k_jpeg_load(path, w, h);
    if (magic[0] == 'B' && magic[1] == 'M')         return w2k_bmp_load(path, w, h);
    return NULL;
}

/* Does this look like something l2kimage can open? Used for associations
 * and for stepping through a folder. */
int w2k_image_is_image(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    static const char *ext[] = { ".png", ".jpg", ".jpeg", ".jpe", ".bmp",
                                 ".dib", NULL };
    for (int i = 0; ext[i]; i++)
        if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Writing JPEG (quality 90) and 24-bit BMP, for Save As.
 * ------------------------------------------------------------------ */
int w2k_jpeg_save_quality(const char *path, const unsigned char *rgba,
                          int w, int h, int quality)
{
    if (!rgba || w <= 0 || h <= 0) return 0;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    struct jpeg_compress_struct c;
    struct jpeg_bail err;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_bail_out;
    unsigned char *volatile row = malloc((size_t)w * 3);
    if (!row) { fclose(f); return 0; }
    jpeg_create_compress(&c);
    if (setjmp(err.back)) { jpeg_destroy_compress(&c); free(row); fclose(f); return 0; }
    jpeg_stdio_dest(&c, f);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    for (int y = 0; y < h; y++) {
        const unsigned char *p = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            row[x * 3] = p[x * 4];
            row[x * 3 + 1] = p[x * 4 + 1];
            row[x * 3 + 2] = p[x * 4 + 2];
        }
        JSAMPROW rp = row;
        jpeg_write_scanlines(&c, &rp, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    fclose(f);
    return 1;
}

int w2k_jpeg_save(const char *path, const unsigned char *rgba, int w, int h)
{
    return w2k_jpeg_save_quality(path, rgba, w, h, 90);
}

int w2k_bmp_save(const char *path, const unsigned char *rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int stride = (w * 3 + 3) & ~3;
    unsigned long size = 54 + (unsigned long)stride * h;
    unsigned char hd[54] = { 'B', 'M' };
    hd[2] = size & 0xff; hd[3] = (size >> 8) & 0xff; hd[4] = (size >> 16) & 0xff; hd[5] = (size >> 24) & 0xff;
    hd[10] = 54; hd[14] = 40;
    hd[18] = w & 0xff; hd[19] = (w >> 8) & 0xff; hd[20] = (w >> 16) & 0xff; hd[21] = (w >> 24) & 0xff;
    hd[22] = h & 0xff; hd[23] = (h >> 8) & 0xff; hd[24] = (h >> 16) & 0xff; hd[25] = (h >> 24) & 0xff;
    hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, f);
    unsigned char *row = calloc(1, (size_t)stride);
    for (int y = h - 1; y >= 0 && row; y--) {
        const unsigned char *p = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) { row[x * 3] = p[x * 4 + 2]; row[x * 3 + 1] = p[x * 4 + 1]; row[x * 3 + 2] = p[x * 4]; }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    fclose(f);
    return 1;
}
