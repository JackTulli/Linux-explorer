/* iconload.c -- runtime icon overrides from .ico files.
 *
 * Any built-in icon can be replaced by dropping <slug>.ico into
 * ~/.w2k/icons (or $W2K_ICON_DIR). This is a small reader for the classic
 * ICO container: 1/4/8/24/32-bit DIB images with an AND mask. PNG-encoded
 * entries (Vista and later) are skipped. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char *w2k_icon_user16[N_ICONS];
unsigned char *w2k_icon_user32[N_ICONS];
void w2k_icon_cache_drop(int id);                /* icon.c */

static const char *const slugs[N_ICONS] = {
    [ICO_APP] = "app", [ICO_FOLDER] = "folder", [ICO_FOLDER_OPEN] = "folder_open",
    [ICO_FILE] = "file", [ICO_FILE_TEXT] = "file_text",
    [ICO_MYCOMPUTER] = "mycomputer", [ICO_DRIVE_HDD] = "drive_hdd",
    [ICO_DRIVE_FLOPPY] = "drive_floppy", [ICO_DRIVE_CD] = "drive_cd",
    [ICO_NOTEPAD] = "notepad", [ICO_EXPLORER] = "explorer",
    [ICO_TASKMGR] = "taskmgr", [ICO_MYDOCS] = "mydocs", [ICO_RECYCLE] = "recycle",
    [ICO_NETWORK] = "network", [ICO_PROGRAMS] = "programs",
    [ICO_DOCUMENTS] = "documents", [ICO_SETTINGS] = "settings",
    [ICO_SEARCH] = "search", [ICO_HELP] = "help", [ICO_RUN] = "run",
    [ICO_SHUTDOWN] = "shutdown", [ICO_LOGOFF] = "logoff", [ICO_UP] = "up",
    [ICO_BACK] = "back", [ICO_FORWARD] = "forward", [ICO_CUT] = "cut",
    [ICO_COPY] = "copy", [ICO_PASTE] = "paste", [ICO_DELETE] = "delete",
    [ICO_PROPERTIES] = "properties", [ICO_VIEWS] = "views",
    [ICO_CONTROLPANEL] = "controlpanel", [ICO_TERMINAL] = "terminal",
    [ICO_CALC] = "calc", [ICO_PAINT] = "paint", [ICO_SNIP] = "snip",
    [ICO_CHARMAP] = "charmap", [ICO_INFO] = "info",
    [ICO_WARNING] = "warning", [ICO_QUESTION] = "question",
    [ICO_ERROR] = "error", [ICO_ACCESSORIES] = "accessories",
    [ICO_STARTFLAG] = "startflag", [ICO_DESKTOP] = "desktop",
    [ICO_WINUPDATE] = "winupdate",
    [ICO_FILE_BITMAP] = "file_bitmap", [ICO_FILE_JPEG] = "file_jpeg",
    [ICO_FILE_GIF] = "file_gif", [ICO_FILE_HTML] = "file_html",
    [ICO_FILE_WAVE] = "file_wave", [ICO_FILE_MIDI] = "file_midi",
    [ICO_FILE_MOVIE] = "file_movie", [ICO_FILE_MEDIA] = "file_media",
    [ICO_FILE_ZIP] = "file_zip", [ICO_FILE_INI] = "file_ini",
    [ICO_FILE_BAT] = "file_bat", [ICO_FILE_SYS] = "file_sys",
    [ICO_FILE_RTF] = "file_rtf", [ICO_FILE_FONT] = "file_font",
    [ICO_FILE_UNKNOWN] = "file_unknown",
    [ICO_LINK_OVERLAY] = "link_overlay",
    [ICO_SPEAKER] = "speaker", [ICO_CURSORFILE] = "cursorfile",
    [ICO_FAVORITES] = "favorites", [ICO_FONTS_FOLDER] = "fonts_folder",
    [ICO_RECYCLE_FULL] = "recycle_full",
};

const char *w2k_icon_slug(int id)
{
    return (id >= 0 && id < N_ICONS) ? slugs[id] : NULL;
}

static unsigned le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* Decode one DIB entry into a fresh RGBA buffer. */
static unsigned char *decode_dib(const unsigned char *p, unsigned long len,
                                 int w, int h)
{
    if (len < 40) return NULL;
    unsigned long hdr = le32(p);
    int bpp = le16(p + 14);
    unsigned long ncol = le32(p + 32);
    if (bpp <= 8 && ncol == 0) ncol = 1UL << bpp;
    if (bpp > 8) ncol = 0;
    if (hdr < 40 || hdr > len) return NULL;

    const unsigned char *pal = p + hdr;
    const unsigned char *xor_ = pal + ncol * 4;
    unsigned long xor_stride = ((unsigned long)w * bpp + 31) / 32 * 4;
    unsigned long and_stride = ((unsigned long)w + 31) / 32 * 4;
    const unsigned char *and_ = xor_ + xor_stride * h;
    if ((unsigned long)(xor_ - p) + xor_stride * h > len) return NULL;
    int have_and = (unsigned long)(and_ - p) + and_stride * h <= len;

    unsigned char *out = w2k_alloc((size_t)w * h * 4);
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = xor_ + xor_stride * (h - 1 - y);  /* bottom-up */
        for (int x = 0; x < w; x++) {
            unsigned char *o = out + ((size_t)y * w + x) * 4;
            unsigned r = 0, g = 0, b = 0, a = 255, idx = 0;
            switch (bpp) {
            case 1:  idx = (row[x >> 3] >> (7 - (x & 7))) & 1; break;
            case 4:  idx = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15; break;
            case 8:  idx = row[x]; break;
            case 24: b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2]; break;
            case 32: b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2];
                     a = row[x * 4 + 3]; if (a) any_alpha = 1; break;
            default: free(out); return NULL;
            }
            if (bpp <= 8) {
                if (idx >= ncol) idx = 0;
                b = pal[idx * 4]; g = pal[idx * 4 + 1]; r = pal[idx * 4 + 2];
            }
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
    }
    /* The AND mask decides transparency unless the image had real alpha. */
    if (have_and && !(bpp == 32 && any_alpha))
        for (int y = 0; y < h; y++) {
            const unsigned char *row = and_ + and_stride * (h - 1 - y);
            for (int x = 0; x < w; x++)
                if ((row[x >> 3] >> (7 - (x & 7))) & 1)
                    out[((size_t)y * w + x) * 4 + 3] = 0;
        }
    return out;
}

static unsigned char *scale_rgba(const unsigned char *src, int sw, int sh,
                                 int dw, int dh)
{
    unsigned char *out = w2k_alloc((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            memcpy(out + ((size_t)y * dw + x) * 4,
                   src + ((size_t)(y * sh / dh) * sw + x * sw / dw) * 4, 4);
    return out;
}

/* Load a size x size image from an ICO, preferring exact size then depth. */
static unsigned char *load_ico(const char *path, int size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 6 || n > (4 << 20)) { fclose(f); return NULL; }
    unsigned char *buf = w2k_alloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    if (le16(buf) != 0 || le16(buf + 2) != 1) { free(buf); return NULL; }

    int count = le16(buf + 4), best = -1, best_bpp = -1, bw = 0, bh = 0;
    for (int i = 0; i < count && 6 + 16 * (i + 1) <= n; i++) {
        const unsigned char *e = buf + 6 + 16 * i;
        int w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
        unsigned long off = le32(e + 12), len = le32(e + 8);
        if (off + len > (unsigned long)n || len < 40) continue;
        if (!memcmp(buf + off, "\x89PNG", 4)) continue;
        int bpp = le16(e + 6);
        if (!bpp) bpp = le16(buf + off + 14);
        int better = best < 0 ||
                     abs(w - size) < abs(bw - size) ||
                     (w == bw && bpp > best_bpp);
        if (better) { best = i; best_bpp = bpp; bw = w; bh = h; }
    }
    unsigned char *out = NULL;
    if (best >= 0) {
        const unsigned char *e = buf + 6 + 16 * best;
        unsigned char *img = decode_dib(buf + le32(e + 12), le32(e + 8), bw, bh);
        if (img && (bw != size || bh != size)) {
            out = scale_rgba(img, bw, bh, size, size);
            free(img);
        } else out = img;
    }
    free(buf);
    return out;
}

int w2k_icon_load_file(int id, const char *path)
{
    if (id < 0 || id >= N_ICONS) return 0;
    if (!path) {                       /* back to the built-in artwork */
        free(w2k_icon_user16[id]);
        free(w2k_icon_user32[id]);
        w2k_icon_user16[id] = NULL;
        w2k_icon_user32[id] = NULL;
        w2k_icon_cache_drop(id);
        return 1;
    }
    unsigned char *i16 = load_ico(path, 16);
    if (!i16) return 0;
    free(w2k_icon_user16[id]);
    free(w2k_icon_user32[id]);
    w2k_icon_user16[id] = i16;
    w2k_icon_user32[id] = load_ico(path, 32);
    w2k_icon_cache_drop(id);
    return 1;
}

/* An icon file registered as a new icon id, whatever format it is in:
 * .ico through the reader above, anything else through the image loader.
 * Cached by path, so a pinned icon costs one decode per session. */
int w2k_icon_from_file(const char *path)
{
    if (!path || !*path) return ICO_APP;

    static struct { char path[512]; int id; } cache[32];
    static int ncache;
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].path, path)) return cache[i].id;

    unsigned char *i16 = load_ico(path, 16), *i32 = NULL;
    if (i16) {
        i32 = load_ico(path, 32);
    } else {
        int w = 0, h = 0;
        unsigned char *rgba = w2k_image_load(path, &w, &h);
        if (rgba && w > 0 && h > 0) {
            i16 = w2k_rgba_scale(rgba, w, h, 16);
            i32 = w2k_rgba_scale(rgba, w, h, 32);
        }
        free(rgba);
    }
    int id = ICO_APP;
    if (i16 && i32) id = w2k_icon_register(i16, i32);
    else { free(i16); free(i32); }

    if (ncache < (int)(sizeof cache / sizeof *cache)) {
        snprintf(cache[ncache].path, sizeof cache[ncache].path, "%.511s", path);
        cache[ncache].id = id;
        ncache++;
    }
    return id;
}

/* The built-in icon a slug names -- the reverse of w2k_icon_slug(), so a
 * chosen icon can be written down as "w2k:notepad" and read back. */
int w2k_icon_by_slug(const char *slug)
{
    if (!slug || !*slug) return -1;
    for (int i = 0; i < N_ICONS; i++)
        if (slugs[i] && !strcmp(slugs[i], slug)) return i;
    return -1;
}

int w2k_icon_load_dir(const char *dir)
{
    if (!dir || !*dir) return 0;
    int loaded = 0;
    for (int id = 0; id < N_ICONS; id++) {
        if (!slugs[id]) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.ico", dir, slugs[id]);
        unsigned char *i16 = load_ico(path, 16);
        if (!i16) continue;
        free(w2k_icon_user16[id]);
        free(w2k_icon_user32[id]);
        w2k_icon_user16[id] = i16;
        w2k_icon_user32[id] = load_ico(path, 32);
        w2k_icon_cache_drop(id);
        loaded++;
    }
    return loaded;
}

int w2k_icon_load_default(void)
{
    int n = w2k_icon_load_dir(W2K_PREFIX "/share/w2k/icons");
    const char *home = getenv("HOME");
    if (home) {
        char path[1024];
        snprintf(path, sizeof path, "%s/.w2k/icons", home);
        n += w2k_icon_load_dir(path);
    }
    const char *env = getenv("W2K_ICON_DIR");
    if (env) n += w2k_icon_load_dir(env);
    return n;
}
