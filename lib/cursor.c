/* cursor.c -- real Windows cursors, at their real size.
 *
 * A cursor set is a directory of .cur files plus a .crs scheme file naming
 * which file plays which role:
 *
 *     [Arrow]
 *     Path=Cursor_1.cur
 *     [IBeam]
 *     Path=Beam.cur
 *
 * That is the layout cursor packs ship in, so a set can be dropped into
 * ~/.w2k/cursors unmodified. Anything the set does not provide falls back to
 * the X cursor font, so a missing or broken pack degrades instead of failing.
 *
 * Size: the bitmap is used exactly as authored -- 32x32 stays 32x32 on a 4K
 * panel. Xcursor's usual behaviour of picking a size from XCURSOR_SIZE or the
 * screen's DPI applies to *themes*; images handed to XcursorImageLoadCursor
 * are never resampled, which is precisely why the loader goes this way round.
 *
 * Animated (.ani) cursors are not read; only the static .cur/.ico form. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <X11/Xcursor/Xcursor.h>

/* Decoded cursor image: straight ARGB, alpha is 0 or 255 only. */
typedef struct {
    int w, h, hx, hy;
    unsigned *px;                 /* w*h, 0xAARRGGBB */
} CurImage;

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n <= 0 || n > (4 << 20)) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *d = malloc((size_t)n);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) *len = n;
    return d;
}

/* Windows packs the two bitmaps bottom-up with rows padded to 4 bytes: the
 * colour ("XOR") image first, then a 1bpp transparency ("AND") mask. */
#define STRIDE(w, bpp) ((((w) * (bpp) + 31) / 32) * 4)

static int mask_bit(const unsigned char *m, int stride, int x, int y)
{
    return (m[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
}

/* Decode image `idx` of a .cur/.ico file. Returns 0 on success. */
static int cur_decode(const unsigned char *d, long n, int idx, CurImage *out)
{
    if (n < 6 + 16) return -1;
    int count = (int)rd16(d + 4);
    if (idx < 0 || idx >= count) return -1;

    const unsigned char *e = d + 6 + 16 * idx;
    int w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
    int hx = (int)rd16(e + 4), hy = (int)rd16(e + 6);
    long off = (long)rd32(e + 12), len = (long)rd32(e + 8);
    if (off < 0 || len < 40 || off + len > n) return -1;

    const unsigned char *b = d + off;
    long hdr = (long)rd32(b);
    if (hdr < 40) return -1;
    int bw = (int)rd32(b + 4), bh = (int)rd32(b + 8);
    int bpp = (int)rd16(b + 14);
    int ncol = (int)rd32(b + 32);
    if (bw != w) w = bw;
    /* An icon DIB claims twice its real height: colour image plus mask. */
    if (bh == 2 * h) bh = h;
    if (bh != h) h = bh;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return -1;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return -1;

    if (!ncol && bpp <= 8) ncol = 1 << bpp;
    /* A corrupt or hostile file can claim any palette size; clamp it before
     * it is used as a pointer offset. */
    if (ncol < 0 || ncol > 256) return -1;

    const unsigned char *pal = b + hdr;
    const unsigned char *xor_ = pal + (long)ncol * 4;
    int xs = STRIDE(w, bpp), ms = STRIDE(w, 1);
    const unsigned char *and_ = xor_ + (long)xs * h;
    /* The colour plane must be inside the file as well as the mask: a
     * truncated icon would otherwise be read off the end. */
    if (xor_ < b || and_ > d + n) return -1;
    int have_mask = (and_ + (long)ms * h) <= (d + n);

    unsigned *px = calloc((size_t)w * h, sizeof *px);
    if (!px) return -1;

    /* Monochrome cursors use a fourth pixel state: mask set *and* colour set
     * means "invert whatever is underneath". X has no such thing, so those
     * pixels are drawn black and given a white halo below -- which is how the
     * I-beam and crosshair stay visible over any background. */
    char *invert = calloc((size_t)w * h, 1);
    if (!invert) { free(px); return -1; }

    for (int y = 0; y < h; y++) {
        const unsigned char *row = xor_ + (long)(h - 1 - y) * xs;
        for (int x = 0; x < w; x++) {
            unsigned r = 0, g = 0, bl = 0, a = 255;
            int idxpx = 0;
            switch (bpp) {
            case 1: idxpx = (row[x >> 3] >> (7 - (x & 7))) & 1; break;
            case 4: idxpx = (row[x >> 1] >> (x & 1 ? 0 : 4)) & 0xf; break;
            case 8: idxpx = row[x]; break;
            case 24: bl = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2]; break;
            case 32: bl = row[x * 4]; g = row[x * 4 + 1];
                     r = row[x * 4 + 2]; a = row[x * 4 + 3]; break;
            }
            if (bpp <= 8) {
                if (idxpx >= ncol) idxpx = 0;
                bl = pal[idxpx * 4]; g = pal[idxpx * 4 + 1]; r = pal[idxpx * 4 + 2];
            }
            int transparent = 0;
            if (have_mask && mask_bit(and_, ms, x, h - 1 - y)) {
                /* Masked out: transparent, unless the colour bit is set too. */
                if (bpp == 1 && idxpx) invert[y * w + x] = 1;
                transparent = 1;
            }
            if (bpp == 32 && !have_mask) transparent = (a == 0);
            px[y * w + x] = transparent ? 0
                                        : (0xffu << 24) | (r << 16) | (g << 8) | bl;
        }
    }

    /* Inverting pixels -> black, with a white outline where they meet nothing. */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (invert[y * w + x]) px[y * w + x] = 0xff000000u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (!invert[y * w + x]) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int ny = y + dy, nx = x + dx;
                    if (ny < 0 || nx < 0 || ny >= h || nx >= w) continue;
                    if (!px[ny * w + nx] && !invert[ny * w + nx])
                        px[ny * w + nx] = 0xffffffffu;
                }
        }
    free(invert);

    if (hx < 0 || hx >= w) hx = 0;
    if (hy < 0 || hy >= h) hy = 0;
    out->w = w; out->h = h; out->hx = hx; out->hy = hy; out->px = px;
    return 0;
}

/* Prefer 32x32 -- the size every classic cursor was drawn at. */
static int best_image(const unsigned char *d, long n)
{
    if (n < 6) return -1;
    int count = (int)rd16(d + 4), best = -1, best_score = -1;
    for (int i = 0; i < count && 6 + 16 * i + 16 <= n; i++) {
        const unsigned char *e = d + 6 + 16 * i;
        int w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
        int score = (w == 32 && h == 32) ? 1000 : 500 - (w > 32 ? w - 32 : 32 - w);
        if (score > best_score) { best_score = score; best = i; }
    }
    return best;
}

/* Composite a soft shadow under a cursor: the shape again, offset down and
 * right, blurred a little and at a third opacity. Real alpha, because
 * XcursorImageLoadCursor takes ARGB -- this is the one effect the X core
 * cursor could never do. */
static void add_shadow(CurImage *im)
{
    int w = im->w, h = im->h;
    unsigned *out = calloc((size_t)w * h, sizeof *out);
    if (!out) return;

    static const int off = 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            /* Coverage of the shape around the offset source pixel. */
            int cover = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int sx = x - off + dx, sy = y - off + dy;
                    n++;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    if (im->px[sy * w + sx] >> 24) cover++;
                }
            int a = n ? cover * 90 / n : 0;         /* ~35% at full cover */
            out[y * w + x] = a ? ((unsigned)a << 24) : 0;
        }

    /* The cursor itself goes over the shadow. */
    for (int i = 0; i < w * h; i++) {
        unsigned p = im->px[i];
        if (p >> 24) out[i] = p;
    }
    free(im->px);
    im->px = out;
}

static Cursor cursor_from_file(const char *path)
{
    long n = 0;
    unsigned char *d = slurp(path, &n);
    if (!d) return None;

    CurImage im = { 0 };
    int idx = best_image(d, n);
    if (idx < 0 || cur_decode(d, n, idx, &im) != 0) { free(d); return None; }
    free(d);

    if (w2k_effects[FX_CURSOR_SHADOW]) add_shadow(&im);

    Cursor c = None;
    if (XcursorSupportsARGB(w2k.dpy)) {
        XcursorImage *xi = XcursorImageCreate(im.w, im.h);
        if (xi) {
            xi->xhot = (unsigned)im.hx;
            xi->yhot = (unsigned)im.hy;
            /* Xcursor wants premultiplied alpha. The artwork itself is
             * opaque or clear, but a shadow is neither. */
            for (int i = 0; i < im.w * im.h; i++) {
                unsigned p = im.px[i], a = p >> 24;
                unsigned r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
                if (a != 255) { r = r * a / 255; g = g * a / 255; b = b * a / 255; }
                ((XcursorPixel *)xi->pixels)[i] =
                    (a << 24) | (r << 16) | (g << 8) | b;
            }
            c = XcursorImageLoadCursor(w2k.dpy, xi);
            XcursorImageDestroy(xi);
        }
    } else {
        /* No ARGB cursors: fold down to the two 1bpp planes X has always had. */
        int stride = (im.w + 7) / 8;
        char *src = calloc((size_t)stride * im.h, 1);
        char *msk = calloc((size_t)stride * im.h, 1);
        if (src && msk) {
            for (int y = 0; y < im.h; y++)
                for (int x = 0; x < im.w; x++) {
                    unsigned p = im.px[y * im.w + x];
                    if (!(p >> 24)) continue;
                    msk[y * stride + (x >> 3)] |= (char)(1 << (x & 7));
                    int lum = ((p >> 16 & 0xff) * 30 + (p >> 8 & 0xff) * 59 +
                               (p & 0xff) * 11) / 100;
                    if (lum < 128) src[y * stride + (x >> 3)] |= (char)(1 << (x & 7));
                }
            Pixmap ps = XCreateBitmapFromData(w2k.dpy, w2k.root, src, im.w, im.h);
            Pixmap pm = XCreateBitmapFromData(w2k.dpy, w2k.root, msk, im.w, im.h);
            XColor black = { .red = 0, .green = 0, .blue = 0 };
            XColor white = { .red = 65535, .green = 65535, .blue = 65535 };
            c = XCreatePixmapCursor(w2k.dpy, ps, pm, &black, &white,
                                    (unsigned)im.hx, (unsigned)im.hy);
            w2k_free_pixmap(ps);
            w2k_free_pixmap(pm);
        }
        free(src);
        free(msk);
    }
    free(im.px);
    return c;
}

/* ------------------------------------------------------------------ *
 * Roles
 * ------------------------------------------------------------------ */
enum { R_ARROW, R_IBEAM, R_WAIT, R_HAND, R_SIZEALL,
       R_SIZENS, R_SIZEWE, R_SIZENWSE, R_SIZENESW, R_NO, N_ROLES };

static const struct {
    const char *scheme_name;      /* section name in the .crs file    */
    const char *file_name;        /* bare <role>.cur, if there is no scheme */
    unsigned    font_shape;       /* X cursor font fallback           */
} role_info[N_ROLES] = {
    [R_ARROW]     = { "Arrow",     "Arrow",     XC_left_ptr           },
    [R_IBEAM]     = { "IBeam",     "IBeam",     XC_xterm              },
    [R_WAIT]      = { "Wait",      "Wait",      XC_watch              },
    [R_HAND]      = { "Hand",      "Hand",      XC_hand2              },
    [R_SIZEALL]   = { "SizeAll",   "SizeAll",   XC_fleur              },
    [R_SIZENS]    = { "SizeNS",    "SizeNS",    XC_sb_v_double_arrow  },
    [R_SIZEWE]    = { "SizeWE",    "SizeWE",    XC_sb_h_double_arrow  },
    [R_SIZENWSE]  = { "SizeNWSE",  "SizeNWSE",  XC_top_left_corner    },
    [R_SIZENESW]  = { "SizeNESW",  "SizeNESW",  XC_top_right_corner   },
    [R_NO]        = { "No",        "No",        XC_circle             },
};

static Cursor *role_slot(int r)
{
    switch (r) {
    case R_ARROW:    return &w2k.cur_arrow;
    case R_IBEAM:    return &w2k.cur_text;
    case R_WAIT:     return &w2k.cur_wait;
    case R_HAND:     return &w2k.cur_hand;
    case R_SIZEALL:  return &w2k.cur_move;
    case R_SIZENS:   return &w2k.cur_size_ns;
    case R_SIZEWE:   return &w2k.cur_size_we;
    case R_SIZENWSE: return &w2k.cur_size_nwse;
    case R_SIZENESW: return &w2k.cur_size_nesw;
    case R_NO:       return &w2k.cur_no;
    }
    return NULL;
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' ||
                     e[-1] == '\t')) *--e = 0;
    /* Strip a UTF-8 BOM, which cursor packs frequently carry. */
    if ((unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb &&
        (unsigned char)s[2] == 0xbf) memmove(s, s + 3, strlen(s + 3) + 1);
}

/* Read "[Role] / Path=file.cur" pairs into paths[]. Returns roles filled. */
static int read_scheme(const char *crs, const char *dir, char paths[][512])
{
    FILE *f = fopen(crs, "r");
    if (!f) return 0;
    char line[512], role[64] = "";
    int filled = 0;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            char *close = strchr(p, ']');
            if (!close) continue;
            *close = 0;
            snprintf(role, sizeof role, "%s", p + 1);
        } else if (!strncasecmp(p, "Path=", 5) && role[0]) {
            for (int r = 0; r < N_ROLES; r++) {
                if (strcasecmp(role, role_info[r].scheme_name)) continue;
                if (paths[r][0]) break;          /* first wins */
                snprintf(paths[r], 512, "%s/%s", dir, p + 5);
                filled++;
                break;
            }
            role[0] = 0;
        }
    }
    fclose(f);
    return filled;
}

/* Find the .crs scheme in `dir`, if there is one. */
static int find_scheme(const char *dir, char *out, int n)
{
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    struct dirent *de;
    int found = 0;
    while (!found && (de = readdir(dp))) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcasecmp(de->d_name + len - 4, ".crs")) continue;
        if (snprintf(out, (size_t)n, "%s/%s", dir, de->d_name) < n) found = 1;
    }
    closedir(dp);
    return found;
}

static int load_from_dir(const char *dir)
{
    char paths[N_ROLES][512];
    memset(paths, 0, sizeof paths);

    char crs[512];
    if (find_scheme(dir, crs, sizeof crs)) read_scheme(crs, dir, paths);

    /* A set without a scheme file can still name its files by role. */
    for (int r = 0; r < N_ROLES; r++)
        if (!paths[r][0])
            snprintf(paths[r], sizeof paths[r], "%s/%s.cur", dir,
                     role_info[r].file_name);

    int loaded = 0;
    for (int r = 0; r < N_ROLES; r++) {
        Cursor c = cursor_from_file(paths[r]);
        if (c == None) continue;
        Cursor *slot = role_slot(r);
        if (*slot) XFreeCursor(w2k.dpy, *slot);
        *slot = c;
        loaded++;
    }
    return loaded;
}

void w2k_cursors_init(void)
{
    /* Called again when the cursor-shadow setting changes, so release what
     * is there before replacing it. */
    for (int r = 0; r < N_ROLES; r++) {
        Cursor *slot = role_slot(r);
        if (*slot) {
            XFreeCursor(w2k.dpy, *slot);
            *slot = None;
        }
    }
    /* X cursor font first, so every slot is valid whatever happens next. */
    for (int r = 0; r < N_ROLES; r++)
        *role_slot(r) = XCreateFontCursor(w2k.dpy, role_info[r].font_shape);

    const char *dirs[3];
    char home_dir[512];
    int nd = 0;
    const char *home = getenv("HOME");
    if (home) {
        snprintf(home_dir, sizeof home_dir, "%s/.w2k/cursors", home);
        dirs[nd++] = home_dir;
    }
    dirs[nd++] = "/usr/local/share/w2k/cursors";
    dirs[nd++] = "/usr/share/w2k/cursors";

    for (int i = 0; i < nd; i++) {
        int n = load_from_dir(dirs[i]);
        if (n) {
            /* One line in the session log says where the pointer came from,
             * so a machine showing the wrong one can be told apart from a
             * machine that never found the set. */
            if (getenv("W2K_DEBUG") || !getenv("W2K_QUIET"))
                fprintf(stderr, "w2k: cursors: %d/%d roles from %s\n", n, N_ROLES, dirs[i]);
            return;
        }
    }
    fprintf(stderr, "w2k: cursors: no cursor set found (looked in %s%s%s); "
            "using the X server's own\n", home ? home_dir : "", home ? ", " : "",
            "/usr/local/share/w2k/cursors, /usr/share/w2k/cursors");
}
