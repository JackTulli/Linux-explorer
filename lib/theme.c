/* theme.c -- the parts of Windows XP and Windows 7 that are painted, not
 * merely coloured.
 *
 * The classic look is edges: two pixels of light and shadow around a flat
 * face, which w2k_edge() draws. Luna threw that away for gradients,
 * rounded corners and glyphs on coloured buttons, and no colour table can
 * express any of it.
 *
 * Where a real screenshot of the element exists, the element is *cropped
 * from it* and shipped as a skin under skins/: the caption is a left cap,
 * a middle column and a right cap; the caption buttons are 21x21 cells;
 * the frame is its two bottom corners; the task button is two caps and a
 * column; the taskbar is one column. The painters below tile those. Each
 * skin was checked by rebuilding the element it came from and diffing:
 * zero pixels differ. That is the only sense in which "pixel-exact" means
 * anything.
 *
 * The stop tables that follow are the fallback for a system without the
 * skins: drawn from the two themes' documented palettes, and they say so.
 */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * Skins, found and cached by name
 * ------------------------------------------------------------------ */
/* Where a skin may live: the user's own, beside the binaries when run
 * from the source tree, and the installed copy. */
int w2k_skin_path(const char *name, char *out, int n)
{
    const char *home = getenv("HOME");
    if (home) {
        snprintf(out, (size_t)n, "%s/.w2k/skins/%s", home, name);
        if (access(out, R_OK) == 0) return 1;
    }
    char exe[768];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;                       /* .../bin */
            snprintf(out, (size_t)n, "%.760s/../skins/%.120s", exe, name);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    snprintf(out, (size_t)n, W2K_PREFIX "/share/w2k/skins/%s", name);
    return access(out, R_OK) == 0;
}

/* `scale` is applied to every colour channel (256 = as is): the hot and
 * pressed states of a button are not in any screenshot to hand, so they
 * are the measured button lightened or darkened. */
static struct { char name[64]; int scale; W2kSkin *s; int tried; } skin_cache[16];
static int nskin_cache;

/* Forget every skin read so far: the files may have changed (a new set of
 * artwork, a theme edited under ~/.w2k/skins); they are read again on the
 * next paint. */
void w2k_skin_cache_flush(void)
{
    for (int i = 0; i < nskin_cache; i++) w2k_skin_free(skin_cache[i].s);
    nskin_cache = 0;
}

static W2kSkin *skin(const char *name, int scale)
{
#define cache skin_cache
#define n nskin_cache
    for (int i = 0; i < n; i++)
        if (cache[i].scale == scale && !strcmp(cache[i].name, name))
            return cache[i].s;
    if (n >= 16) return NULL;
    char path[1024];
    W2kSkin *s = w2k_skin_path(name, path, sizeof path)
               ? w2k_skin_load_scaled(path, scale) : NULL;
    snprintf(cache[n].name, sizeof cache[n].name, "%s", name);
    cache[n].scale = scale;
    cache[n].s = s;
    cache[n].tried = 1;
    n++;
    return s;
#undef cache
#undef n
}

/* ------------------------------------------------------------------ *
 * Fallback gradients (stop tables; positions in thousandths of height)
 * ------------------------------------------------------------------ */
typedef struct { int at; unsigned char r, g, b; } Stop;

static const Stop cap_xp_active[] = {
    {    0,   0,  88, 238 }, {   35,  63, 151, 255 }, {   70,  43, 144, 255 },
    {  105,   3, 114, 255 }, {  140,   3, 101, 241 }, {  175,   0,  92, 233 },
    {  210,   0,  84, 227 }, {  480,   0,  85, 229 }, {  620,   0,  90, 245 },
    {  740,   2, 106, 254 }, {  860,   0, 101, 253 }, {  930,   0,  77, 227 },
    { 1000,   0,  67, 207 },
};
static const Stop cap_xp_inactive[] = {
    {    0, 122, 153, 224 }, {  120, 121, 150, 222 }, {  420, 123, 151, 224 },
    {  620, 125, 155, 227 }, {  760, 129, 167, 232 }, {  860, 130, 169, 233 },
    {  930, 128, 165, 231 }, { 1000, 122, 147, 223 },
};
static const Stop cap_7_active[] = {
    {    0, 232, 241, 250 }, {  120, 215, 228, 242 }, {  700, 190, 212, 236 },
    { 1000, 176, 200, 226 },
};
static const Stop cap_7_inactive[] = {
    {    0, 245, 249, 252 }, {  200, 239, 245, 250 }, { 1000, 219, 232, 244 },
};
static const Stop btn_xp_close[] = {
    {    0, 228,  95,  62 }, {  110, 233, 124,  98 }, {  220, 231, 111,  84 },
    {  380, 227,  92,  58 }, {  610, 231, 101,  61 }, {  780, 230,  93,  50 },
    {  880, 226,  85,  42 }, {  950, 210,  69,  30 }, { 1000, 174,  49,  16 },
};
static const Stop btn_xp_blue[] = {
    {    0,  31,  95, 231 }, {  110,  51, 104, 230 }, {  220,  43,  96, 229 },
    {  390,  28,  86, 231 }, {  620,  28,  93, 236 }, {  780,  28, 100, 240 },
    {  880,  23, 100, 245 }, { 1000,   7,  78, 217 },
};
static const Stop btn_7_close[] = {
    {    0, 232, 108,  92 }, {  400, 214,  70,  56 }, { 1000, 180,  38,  30 },
};
static const Stop btn_7_blue[] = {
    {    0, 246, 249, 252 }, {  400, 226, 235, 245 }, { 1000, 206, 219, 234 },
};
static const Stop task_xp[] = {
    {    0,  72, 146, 247 }, {   60,  77, 139, 241 }, {  130,  66, 134, 244 },
    {  200,  61, 130, 244 }, {  830,  57, 128, 244 }, {  900,  49, 108, 228 },
    { 1000,  38,  83, 184 },
};
static const Stop task_7[] = {
    {    0,  78,  86,  96 }, {  120,  58,  65,  74 }, {  850,  44,  50,  58 },
    { 1000,  30,  34,  40 },
};

static void stop_rgb(const Stop *st, int n, int at, int *r, int *g, int *b)
{
    int i = 0;
    while (i < n - 1 && st[i + 1].at < at) i++;
    const Stop *a = &st[i], *c = &st[i + 1 < n ? i + 1 : i];
    int span = c->at - a->at;
    int t = span > 0 ? (at - a->at) * 255 / span : 0;
    *r = a->r + (c->r - a->r) * t / 255;
    *g = a->g + (c->g - a->g) * t / 255;
    *b = a->b + (c->b - a->b) * t / 255;
}

/* The window manager calls into this file in raw mode (frame chrome, in
 * screen pixels) and the taskbar in logical mode; S() turns a measured
 * constant into whichever the caller is working in. */
static int S(int v) { return w2k_scale_raw ? w2k_px(v) : v; }

static void grad_fill(Drawable d, int x, int y, int w, int h,
                      const Stop *st, int n, const int *inset, int scale)
{
    /* One row per screen pixel; `inset` is indexed by the caller's row. */
    int px = w2k_cx(x), py = w2k_cx(y), pw = w2k_cw(x, w), ph = w2k_cw(y, h);
    for (int i = 0; i < ph; i++) {
        int at = ph > 1 ? i * 1000 / (ph - 1) : 0;
        int r, g, b;
        stop_rgb(st, n, at, &r, &g, &b);
        r = r * scale / 256; g = g * scale / 256; b = b * scale / 256;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        int li = h > 0 ? (int)((long)i * h / ph) : 0;
        int off = inset ? w2k_cw(x, inset[li]) : 0;
        if (pw - 2 * off <= 0) continue;
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
        XFillRectangle(w2k.dpy, d, w2k.gc, px + off, py + i,
                       (unsigned)(pw - 2 * off), 1);
    }
}

static int *corner_insets(int h, int rad, int bottom_too)
{
    int *ins = calloc((size_t)(h > 0 ? h : 1), sizeof *ins);
    if (!ins) return NULL;
    for (int i = 0; i < h && i < rad; i++) {
        int dy = rad - 1 - i, dx = rad - 1;
        while (dx > 0 && dx * dx + dy * dy > (rad - 1) * (rad - 1) + 1) dx--;
        ins[i] = rad - 1 - dx;
    }
    if (bottom_too)
        for (int i = 0; i < h && i < rad; i++)
            if (h - 1 - i >= 0 && ins[h - 1 - i] < ins[i]) ins[h - 1 - i] = ins[i];
    return ins;
}

/* Draw a skin strip laid out as [left cap][one column][right cap] across
 * a width, tiling the column. Rows beyond the strip repeat its last row,
 * so an element a pixel taller than the source does not tear. */
static void strip_draw(Drawable d, W2kSkin *s, int sy, int sh, int lcap,
                       int rcap, int x, int y, int w, int h)
{
    /* Source rectangles are in the sheet's own pixels; the destination
     * is in the caller's. */
    int sw = w2k_skin_w(s);
    int hl = w2k_scale_raw ? w2k_lp(h) : h;      /* rows wanted, in sheet pixels */
    int hh = hl < sh ? hl : sh;
    int lc = S(lcap), rc = S(rcap), shp = S(sh);
    int midw = w - lc - rc;
    /* Three requests: the caps copied, the column between them tiled. */
    w2k_skin_draw(d, s, x, y, 0, sy, lcap, hh);
    if (midw > 0) w2k_skin_tile(d, s, x + lc, y, midw, S(hh), lcap, sy, 1, hh);
    w2k_skin_draw(d, s, x + w - rc, y, sw - rcap, sy, rcap, hh);
    if (h > shp) {
        int ly = sy + sh - 1;
        w2k_skin_tile(d, s, x, y + shp, lc, h - shp, 0, ly, lcap, 1);
        if (midw > 0)
            w2k_skin_tile(d, s, x + lc, y + shp, midw, h - shp, lcap, ly, 1, 1);
        w2k_skin_tile(d, s, x + w - rc, y + shp, rc, h - shp, sw - rcap, ly,
                      rcap, 1);
    }
}

/* ------------------------------------------------------------------ *
 * Windows 7 Basic
 *
 * Cropped from the theme's own artwork, supplied as three strips: an
 * active title bar, an inactive one, and the taskbar's texture. The frame
 * is a flat steel blue with a vertical gradient down the caption -- no
 * variation across it -- inside a dark outline and a light line; ten
 * pixels of border, a 31-row caption strip (outline, light line, 29 rows
 * of gradient), buttons 32 by 18 hanging at row 10. So the caption is two
 * caps and a tiled column, and the borders a tiled row and column.
 * ------------------------------------------------------------------ */
#define W7_BORDER     10
#define W7_CAP_H      31
#define W7_CAP_LCAP   40      /* the corner and the icon's place */
#define W7_CAP_RCAP   12
#define W7_BTN_W      32
#define W7_BTN_H      18
#define W7_BTN_Y      10
#define W7_BTN_GAP     2
#define W7_BTN_INSET  10      /* Close's right edge from the frame's */
#define W7_BAR_H      40

/* ------------------------------------------------------------------ *
 * Window captions
 * ------------------------------------------------------------------ */
#define XP_CAP_H 30

void w2k_theme_caption(Drawable d, int x, int y, int w, int h, int active,
                       int theme)
{
    if (theme == THEME_BASIC7) {
        W2kSkin *s = skin("w7-caption.png", 256);
        if (s && w2k_skin_w(s) == W7_CAP_LCAP + 1 + W7_CAP_RCAP &&
            w2k_skin_h(s) == 2 * W7_CAP_H && w >= S(W7_CAP_LCAP + W7_CAP_RCAP)) {
            strip_draw(d, s, active ? 0 : W7_CAP_H, W7_CAP_H, W7_CAP_LCAP,
                       W7_CAP_RCAP, x, y, w, h);
            return;
        }
    }
    if (theme == THEME_XP) {
        W2kSkin *s = skin("xp-caption.png", 256);
        if (s && w2k_skin_w(s) == 59 && w2k_skin_h(s) == 2 * XP_CAP_H && w >= S(58)) {
            /* The caps are 28 wide: the caption's gradient fades over its
             * last 27 columns at each end. Tiling one column at a time is
             * a lot of tiny copies for a wide caption; the server handles
             * it, and it only happens on a focus or title change. */
            strip_draw(d, s, active ? 0 : XP_CAP_H, XP_CAP_H, 29, 29, x, y, w, h);
            return;
        }
    }
    const Stop *st;
    int n;
    if (theme == THEME_BASIC7) {
        st = active ? cap_7_active : cap_7_inactive;
        n = active ? (int)(sizeof cap_7_active / sizeof *cap_7_active)
                   : (int)(sizeof cap_7_inactive / sizeof *cap_7_inactive);
    } else {
        st = active ? cap_xp_active : cap_xp_inactive;
        n = active ? (int)(sizeof cap_xp_active / sizeof *cap_xp_active)
                   : (int)(sizeof cap_xp_inactive / sizeof *cap_xp_inactive);
    }
    int *ins = corner_insets(h, S(theme == THEME_BASIC7 ? 4 : 6), 0);
    grad_fill(d, x, y, w, h, st, n, ins, 256);
    free(ins);
}

int w2k_theme_caption_h(int theme)
{
    /* Windows XP: 30 rows from the frame's top edge to the client, of
     * which 4 are the frame border. Windows 7 Basic: 31, of which 10. */
    if (theme == THEME_BASIC7) return W7_CAP_H - W7_BORDER;
    return XP_CAP_H - 4;
}

/* The frame's border: four pixels each side and along the bottom, with
 * rounded bottom corners, from the corner blocks in the frame skin. The
 * client window covers everything inside, so the corner blocks can be
 * blitted whole. */
void w2k_theme_frame_edges(Drawable d, int fw, int fh, int b, int active,
                           int theme)
{
    if (theme == THEME_BASIC7 && b == S(W7_BORDER)) {
        W2kSkin *fr = skin("w7-frame.png", 256), *bt = skin("w7-bottom.png", 256);
        if (fr && bt && w2k_skin_w(fr) == 20 && w2k_skin_h(fr) == 2 &&
            w2k_skin_w(bt) == 1 && w2k_skin_h(bt) == 20) {
            /* The bottom first, then the sides over its ends, so the
             * outline turns the corner squarely. */
            int row = active ? 0 : 1;
            int bd = S(W7_BORDER), ch = S(W7_CAP_H);
            w2k_skin_tile(d, bt, 0, fh - bd, fw, bd, 0,
                          active ? 0 : W7_BORDER, 1, W7_BORDER);
            int sh = fh - ch;
            if (sh > 0) {
                w2k_skin_tile(d, fr, 0, ch, bd, sh, 0, row, W7_BORDER, 1);
                w2k_skin_tile(d, fr, fw - bd, ch, bd, sh,
                              W7_BORDER, row, W7_BORDER, 1);
            }
            return;
        }
    }
    W2kSkin *s = theme == THEME_XP ? skin("xp-frame.png", 256) : NULL;
    W2kSkin *bt = theme == THEME_XP ? skin("xp-bottom.png", 256) : NULL;
    W2kSkin *rs = theme == THEME_XP ? skin("xp-rightshade.png", 256) : NULL;
    if (s && bt && w2k_skin_w(s) == 36 && w2k_skin_h(s) == 16 && b == S(4)) {
        int sy = active ? 0 : 8;
        /* Sides: the corner block's top row, repeated down. The right side
         * carries seventeen rows of shading just under the caption -- the
         * corner's shadow -- which come from their own strip. */
        int side_h = fh - S(8);
        if (side_h > 0) {
            w2k_skin_tile(d, s, 0, 0, S(4), side_h, 0, sy, 4, 1);
            w2k_skin_tile(d, s, fw - S(4), 0, S(4), side_h, 32, sy, 4, 1);
            int n = w2k_scale_raw ? w2k_lp(side_h) - XP_CAP_H : side_h - XP_CAP_H;
            if (n > 17) n = 17;
            if (rs && n > 0)
                w2k_skin_draw(d, rs, fw - S(4), S(XP_CAP_H), 0, active ? 0 : 17, 4, n);
        }
        /* Bottom: four rows sampled well away from the corners. */
        if (fw - S(36) > 0)
            w2k_skin_tile(d, bt, S(28), fh - S(4), fw - S(36), S(4), 0, active ? 0 : 4, 1, 4);
        /* The bottom-left curve is a long one -- 26 columns -- so that
         * corner block is 28 wide; the right one is 8. */
        w2k_skin_draw(d, s, 0, fh - S(8), 0, sy, 28, 8);
        w2k_skin_draw(d, s, fw - S(8), fh - S(8), 28, sy, 8, 8);
        return;
    }
    int side = active ? C_ACTIVETITLE : C_INACTIVETITLE;
    w2k_fill(d, 0, 0, b, fh, side);
    w2k_fill(d, fw - b, 0, b, fh, side);
    w2k_fill(d, 0, fh - b, fw, b, side);
}

/* ------------------------------------------------------------------ *
 * Caption buttons
 * ------------------------------------------------------------------ */
int w2k_theme_capbtn_size(int theme)
{
    return theme == THEME_BASIC7 ? W7_BTN_H : 21;
}

int w2k_theme_capbtn_w(int theme, int kind)
{
    (void)kind;
    return theme == THEME_BASIC7 ? W7_BTN_W : 21;   /* all three alike */
}

/* Where the buttons sit on an XP caption, measured: 21 pixels square at
 * row 6, their right edges 27, 50 and 73 pixels in from the frame's. */
void w2k_theme_capbtn_place(int theme, int fw, int *y, int *close_x,
                            int *max_x, int *min_x)
{
    if (theme == THEME_BASIC7) {
        /* Measured: 32 wide, two apart, Close ten pixels in from the edge. */
        *y = S(W7_BTN_Y);
        *close_x = fw - S(W7_BTN_INSET + W7_BTN_W);
        *max_x = *close_x - S(W7_BTN_GAP + W7_BTN_W);
        *min_x = *max_x - S(W7_BTN_GAP + W7_BTN_W);
        return;
    }
    *y = S(6);
    *close_x = fw - S(27);
    *max_x = fw - S(50);
    *min_x = fw - S(73);
}

void w2k_theme_capbtn(Drawable d, int x, int y, int w, int h, int kind,
                      int active, int pressed, int theme)
{
    if (theme == THEME_BASIC7) {
        W2kSkin *s = skin("w7-capbtn.png", pressed ? 200 : 256);
        if (s && w2k_skin_w(s) == 134 && w2k_skin_h(s) == 2 * W7_BTN_H &&
            w == S(W7_BTN_W) && h == S(W7_BTN_H)) {
            /* Four cells: Minimise, Maximise, Close from the artwork, and
             * Restore built from the Maximise cell -- its frame glyph
             * drawn twice, one behind the other. */
            int cell = kind == W2K_CAP_CLOSE ? 2 : kind == W2K_CAP_MIN ? 0
                     : kind == W2K_CAP_RESTORE ? 3 : 1;
            w2k_skin_draw(d, s, x, y, cell * (W7_BTN_W + W7_BTN_GAP),
                          active ? 0 : W7_BTN_H, W7_BTN_W, W7_BTN_H);
            return;
        }
    }
    if (theme == THEME_XP) {
        W2kSkin *s = skin("xp-capbtn.png", pressed ? 200 : 256);
        if (s && w2k_skin_w(s) == 63 && w2k_skin_h(s) == 44 && w == S(21) && h == S(21)) {
            /* Cells are 21 by 22: the last row is the button's shadow. */
            int cell = kind == W2K_CAP_CLOSE ? 2 : kind == W2K_CAP_MIN ? 0 : 1;
            w2k_skin_draw(d, s, x, y, cell * 21, active ? 0 : 22, 21, 22);
            if (kind == W2K_CAP_RESTORE) {
                /* No restore button in the screenshots: the maximise
                 * button with the two-frames glyph over its own. */
                int cx = x + S(10), cy = y + S(10);
                w2k_fill(d, cx - S(4), cy - S(5), S(11), S(11), C_HIGHLIGHT);
                w2k_fill_rgb(d, cx - S(3), cy - S(4), S(9), S(9), 28, 93, 236);
                XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(255, 255, 255));
                w2k_fill_fg(d, cx - S(1), cy - S(5), S(7), S(2));
                w2k_fill_fg(d, cx + S(5), cy - S(5), S(1), S(5));
                w2k_fill_fg(d, cx - S(5), cy - S(1), S(7), S(2));
                w2k_fill_fg(d, cx - S(5), cy - S(1), S(1), S(6));
                w2k_fill_fg(d, cx - S(5), cy + S(4), S(7), S(1));
                w2k_fill_fg(d, cx + S(1), cy - S(1), S(1), S(6));
            }
            return;
        }
    }

    int seven = theme == THEME_BASIC7;
    const Stop *st = kind == W2K_CAP_CLOSE ? (seven ? btn_7_close : btn_xp_close)
                                           : (seven ? btn_7_blue : btn_xp_blue);
    int n;
    if (kind == W2K_CAP_CLOSE)
        n = seven ? (int)(sizeof btn_7_close / sizeof *btn_7_close)
                  : (int)(sizeof btn_xp_close / sizeof *btn_xp_close);
    else
        n = seven ? (int)(sizeof btn_7_blue / sizeof *btn_7_blue)
                  : (int)(sizeof btn_xp_blue / sizeof *btn_xp_blue);
    int *ins = corner_insets(h, S(3), 1);
    grad_fill(d, x, y, w, h, st, n, ins, active ? 256 : 200);
    free(ins);

    unsigned long edge;
    if (seven) edge = kind == W2K_CAP_CLOSE ? w2k_rgb(160, 30, 24)
                                            : w2k_rgb(140, 152, 166);
    else       edge = kind == W2K_CAP_CLOSE ? w2k_rgb(255, 236, 230)
                                            : w2k_rgb(160, 195, 252);
    XSetForeground(w2k.dpy, w2k.gc, edge);
    int *eins = corner_insets(h, S(3), 1);
    int t = S(1);
    for (int i = 0; i < h; i++) {
        int off = eins ? eins[i] : 0;
        if (i == 0 || i == h - 1 || (eins && i > 0 && eins[i] != eins[i - 1]))
            w2k_fill_fg(d, x + off, y + i, w - 2 * off, 1);
        else {
            w2k_fill_fg(d, x + off, y + i, t, 1);
            w2k_fill_fg(d, x + w - t - off, y + i, t, 1);
        }
    }
    free(eins);

    int o = pressed ? S(1) : 0;
    int cx = x + w / 2 + o, cy = y + h / 2 + o;
    if (seven && kind != W2K_CAP_CLOSE)
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(45, 55, 70));
    else
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(255, 255, 255));
    switch (kind) {
    case W2K_CAP_CLOSE:
        for (int i = 0; i < 7; i++) {
            w2k_fill_fg(d, cx - S(4) + i * t, cy - S(3) + i * t, S(2), t);
            w2k_fill_fg(d, cx + S(3) - i * t, cy - S(3) + i * t, S(2), t);
        }
        break;
    case W2K_CAP_MIN:
        w2k_fill_fg(d, cx - S(3), cy + S(2), S(7), S(2));
        break;
    case W2K_CAP_MAX:
        w2k_fill_fg(d, cx - S(4), cy - S(4), S(9), S(2));
        w2k_fill_fg(d, cx - S(4), cy - S(4), S(1), S(8));
        w2k_fill_fg(d, cx + S(4), cy - S(4), S(1), S(8));
        w2k_fill_fg(d, cx - S(4), cy + S(3), S(9), S(1));
        break;
    default:
        w2k_fill_fg(d, cx - S(1), cy - S(5), S(7), S(2));
        w2k_fill_fg(d, cx + S(5), cy - S(5), S(1), S(5));
        w2k_fill_fg(d, cx - S(5), cy - S(1), S(7), S(2));
        w2k_fill_fg(d, cx - S(5), cy - S(1), S(1), S(6));
        w2k_fill_fg(d, cx - S(5), cy + S(4), S(7), S(1));
        w2k_fill_fg(d, cx + S(1), cy - S(1), S(1), S(6));
        break;
    }
}

/* ------------------------------------------------------------------ *
 * The taskbar and its buttons
 * ------------------------------------------------------------------ */
int w2k_theme_task_h(int theme)
{
    /* XP's task buttons: rows 573..597 of a 570..599 bar. Windows 7's
     * fill the bar, top line and all: forty rows, or thirty with small
     * icons. */
    return theme == THEME_BASIC7 ? (w2k_taskbar_small ? 30 : W7_BAR_H) : 25;
}

void w2k_theme_taskbutton(Drawable d, int x, int y, int w, int h, int state,
                          int theme)
{
    if (theme == THEME_BASIC7) {
        /* Basic's buttons are pale framed boxes on the bar: a dark line,
         * a light one inside it, and a fill that lightens under the
         * pointer and more for the active window. The reference for these
         * is a palettised screenshot, so they are drawn, not cropped. */
        unsigned long fill = state == W2K_TB_DOWN ? w2k_rgb(207, 229, 249)
                           : state == W2K_TB_HOT  ? w2k_rgb(225, 235, 250)
                                                  : w2k_rgb(179, 211, 241);
        int bx = x, by = y + 1, bw = w, bh = h - 2;   /* under the bar's top line */
        XSetForeground(w2k.dpy, w2k.gc, fill);
        w2k_fill_fg(d, bx + 1, by + 1, bw - 2, bh - 2);
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(200, 218, 238));
        w2k_frame_fg(d, bx + 1, by + 1, bw - 2, bh - 2);
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(51, 79, 109));
        w2k_fill_fg(d, bx + 1, by, bw - 2, 1);
        w2k_fill_fg(d, bx + 1, by + bh - 1, bw - 2, 1);
        w2k_fill_fg(d, bx, by + 1, 1, bh - 2);
        w2k_fill_fg(d, bx + bw - 1, by + 1, 1, bh - 2);
        return;
    }
    if (theme == THEME_XP) {
        /* The pressed button is cropped from a screenshot too: the active
         * window's button in it is drawn that way. Hot is the normal one
         * lightened -- no screenshot shows the pointer over a button. */
        W2kSkin *s = state == W2K_TB_DOWN ? skin("xp-task-down.png", 256)
                   : skin("xp-task.png", state == W2K_TB_HOT ? 292 : 256);
        if (s && w2k_skin_w(s) == 11 && w2k_skin_h(s) == 25) {
            strip_draw(d, s, 0, 25, 5, 5, x, y, w, h);
            return;
        }
    }
    const Stop *st = theme == THEME_BASIC7 ? task_7 : task_xp;
    int n = theme == THEME_BASIC7 ? (int)(sizeof task_7 / sizeof *task_7)
                                  : (int)(sizeof task_xp / sizeof *task_xp);
    int scale = state == W2K_TB_DOWN ? 200 : state == W2K_TB_HOT ? 292 : 256;
    int *ins = corner_insets(h, S(3), 1);
    grad_fill(d, x, y, w, h, st, n, ins, scale);
    free(ins);
}

void w2k_theme_bar(Drawable d, int x, int y, int w, int h, int theme)
{
    if (theme == THEME_BASIC7) {
        /* The theme's own taskbar texture, both sizes of it, is one flat
         * colour: (167,192,220), every pixel. The Show Desktop sliver at
         * the far end is marked off with a line. */
        w2k_fill_rgb(d, x, y, w, h, 167, 192, 220);
        w2k_fill_rgb(d, x + w - 16, y + 3, 1, h - 6, 126, 152, 182);
        w2k_fill_rgb(d, x + w - 15, y + 3, 1, h - 6, 214, 228, 242);
        return;
    }
    if (theme == THEME_XP) {
        W2kSkin *s = skin("xp-taskbar.png", 256);
        if (s && w2k_skin_w(s) == 1) {
            int sh = w2k_skin_h(s);
            if (h == sh) { w2k_skin_tile(d, s, x, y, w, h, 0, 0, 1, sh); return; }
            for (int row = 0; row < h; row++) {
                int srow = row * sh / (h > 1 ? h : 1);
                if (srow >= sh) srow = sh - 1;
                w2k_skin_tile(d, s, x, y + row, w, 1, 0, srow, 1, 1);
            }
            return;
        }
    }
    w2k_bar_gradient(d, x, y, w, h, theme);
}
