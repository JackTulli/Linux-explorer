/* draw.c -- the pixel conventions of the Windows classic 3D look.
 *
 * Every widget in this desktop is ultimately built from w2k_edge() plus a
 * fill, exactly as USER32/COMCTL32 built theirs from DrawEdge().
 */
#include "w2k.h"
#include <string.h>
#include <stdio.h>

/* Edge colour table. Columns: left/top outer, left/top inner,
 * right/bottom outer, right/bottom inner. -1 means "no such ring".
 * These are the DrawEdge() assignments; with the 2000 scheme C_LIGHT is a
 * warm off-white, which is what gives buttons their soft outer highlight. */
static const signed char edge_tab[][4] = {
    /*                    LTouter      LTinner     RBouter      RBinner   */
    /* A window frame and a button are not the same edge, though both are
     * "raised": the frame keeps its white line one pixel in from the
     * outside, a button wears it on the very outside. Measured from a
     * Windows 2000 screenshot -- the Cancel button of Shut Down Windows
     * against the border of the Paint window. */
    [EDGE_RAISED]      = { C_LIGHT,    C_HILIGHT,  C_DKSHADOW,  C_SHADOW  },
    [EDGE_BUTTON]      = { C_HILIGHT,  C_LIGHT,    C_DKSHADOW,  C_SHADOW  },
    [EDGE_SUNKEN]      = { C_SHADOW,   C_DKSHADOW, C_HILIGHT,   C_LIGHT   },
    [EDGE_ETCHED]      = { C_SHADOW,   C_HILIGHT,  C_HILIGHT,   C_SHADOW  },
    [EDGE_BUMP]        = { C_HILIGHT,  C_SHADOW,   C_SHADOW,    C_HILIGHT },
    [EDGE_RAISED_THIN] = { C_HILIGHT,  -1,         C_SHADOW,    -1        },
    [EDGE_SUNKEN_THIN] = { C_SHADOW,   -1,         C_HILIGHT,   -1        },
    [EDGE_FLAT]        = { -1,         -1,         -1,          -1        },
};

int w2k_edge_size(int style)
{
    if (style == EDGE_FLAT) return 0;
    return (edge_tab[style][1] >= 0) ? 2 : 1;
}

void w2k_fill(Drawable d, int x, int y, int w, int h, int color)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    XFillRectangle(w2k.dpy, d, w2k.gc, x, y, w, h);
}

void w2k_hline(Drawable d, int x, int y, int w, int color)
{
    if (w <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    XFillRectangle(w2k.dpy, d, w2k.gc, x, y, w, 1);
}

void w2k_vline(Drawable d, int x, int y, int h, int color)
{
    if (h <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    XFillRectangle(w2k.dpy, d, w2k.gc, x, y, 1, h);
}

void w2k_frame(Drawable d, int x, int y, int w, int h, int color)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    XDrawRectangle(w2k.dpy, d, w2k.gc, x, y, w - 1, h - 1);
}

/* One ring of an edge. Bottom/right win at the corners, as in Windows. */
static void ring(Drawable d, int x, int y, int w, int h,
                 int lt, int rb, int flags)
{
    if (w <= 0 || h <= 0) return;
    if (lt >= 0) {
        if (flags & BF_TOP)  w2k_hline(d, x, y, w - ((flags & BF_RIGHT) ? 1 : 0), lt);
        if (flags & BF_LEFT) w2k_vline(d, x, y, h - ((flags & BF_BOTTOM) ? 1 : 0), lt);
    }
    if (rb >= 0) {
        if (flags & BF_BOTTOM) w2k_hline(d, x, y + h - 1, w, rb);
        if (flags & BF_RIGHT)  w2k_vline(d, x + w - 1, y, h, rb);
    }
}

void w2k_edge(Drawable d, int x, int y, int w, int h, int style, int flags)
{
    if (style == EDGE_FLAT || w <= 0 || h <= 0) return;
    const signed char *e = edge_tab[style];
    ring(d, x, y, w, h, e[0], e[2], flags);
    if (e[1] >= 0) {
        int dx = (flags & BF_LEFT) ? 1 : 0, dy = (flags & BF_TOP) ? 1 : 0;
        int dw = dx + ((flags & BF_RIGHT) ? 1 : 0);
        int dh = dy + ((flags & BF_BOTTOM) ? 1 : 0);
        ring(d, x + dx, y + dy, w - dw, h - dh, e[1], e[3], flags);
    }
}

void w2k_button(Drawable d, int x, int y, int w, int h, int pressed)
{
    if (w <= 0 || h <= 0) return;
    if (pressed) {
        /* A depressed pushbutton loses the highlight entirely: a single
         * shadow ring, with the face pushed in by one pixel. */
        w2k_frame(d, x, y, w, h, C_SHADOW);
        w2k_fill(d, x + 1, y + 1, w - 2, h - 2, C_FACE);
    } else {
        w2k_edge(d, x, y, w, h, EDGE_BUTTON, BF_RECT);
        w2k_fill(d, x + 2, y + 2, w - 4, h - 4, C_FACE);
    }
}

/* The taskbar's own background, which after Windows 2000 stopped being a
 * grey panel. The stops are read off 1:1 screenshots -- a column of empty
 * taskbar in Windows XP, and the same in Windows 7 Basic -- so the bright
 * band near the top and the dark edge at the bottom land where they do in
 * the original. Positions are thousandths of the bar's height, so the
 * gradient survives a bar of any thickness. */
typedef struct { int at; unsigned char r, g, b; } BarStop;

static const BarStop bar_xp[] = {
    {    0,  49, 104, 213 }, {   35,  56, 136, 233 }, {   80,  73, 147, 230 },
    {  120,  48, 123, 229 }, {  160,  38, 106, 236 }, {  200,  48, 103, 221 },
    {  850,  48, 105, 224 }, {  900,  33,  92, 228 },
    {  950,  30,  80, 196 }, { 1000,  25,  65, 165 },
};
static const BarStop bar_basic7[] = {
    {    0, 108, 116, 126 }, {   25,  76,  84,  94 }, {   60,  46,  52,  60 },
    {  200,  32,  36,  42 }, {  850,  26,  29,  34 }, { 1000,  16,  18,  21 },
};

void w2k_bar_gradient(Drawable d, int x, int y, int w, int h, int theme)
{
    if (w <= 0 || h <= 0) return;
    const BarStop *st = bar_xp;
    int n = (int)(sizeof bar_xp / sizeof *bar_xp);
    if (theme == THEME_BASIC7) {
        st = bar_basic7;
        n = (int)(sizeof bar_basic7 / sizeof *bar_basic7);
    }
    for (int i = 0; i < h; i++) {
        int at = h > 1 ? i * 1000 / (h - 1) : 0;
        int k = 0;
        while (k < n - 1 && st[k + 1].at < at) k++;
        const BarStop *a = &st[k], *b = &st[k + 1 < n ? k + 1 : k];
        int span = b->at - a->at;
        int t = span > 0 ? (at - a->at) * 255 / span : 0;
        XSetForeground(w2k.dpy, w2k.gc,
                       w2k_rgb(a->r + (b->r - a->r) * t / 255,
                               a->g + (b->g - a->g) * t / 255,
                               a->b + (b->b - a->b) * t / 255));
        XFillRectangle(w2k.dpy, d, w2k.gc, x, y + i, (unsigned)w, 1);
    }
}

void w2k_gradient(Drawable d, int x, int y, int w, int h, int c1, int c2)
{
    if (w <= 0 || h <= 0) return;
    int r1, g1, b1, r2, g2, b2;
    w2k_color_rgb(c1, &r1, &g1, &b1);
    w2k_color_rgb(c2, &r2, &g2, &b2);
    if (w == 1) { w2k_fill(d, x, y, w, h, c1); return; }
    for (int i = 0; i < w; i++) {
        int r = r1 + (r2 - r1) * i / (w - 1);
        int g = g1 + (g2 - g1) * i / (w - 1);
        int b = b1 + (b2 - b1) * i / (w - 1);
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
        XFillRectangle(w2k.dpy, d, w2k.gc, x + i, y, 1, h);
    }
}

/* The current clip rectangle, mirrored so masked icon blits (which use
 * their own GC) can honour it by intersecting. */
int w2k_clip_on, w2k_clip_x, w2k_clip_y, w2k_clip_w, w2k_clip_h;

void w2k_clip_set(int x, int y, int w, int h)
{
    XRectangle r = { x, y, w > 0 ? w : 0, h > 0 ? h : 0 };
    XSetClipRectangles(w2k.dpy, w2k.gc, 0, 0, &r, 1, Unsorted);
    w2k_clip_on = 1;
    w2k_clip_x = x; w2k_clip_y = y; w2k_clip_w = w; w2k_clip_h = h;
}

void w2k_clip_clear(void)
{
    XSetClipMask(w2k.dpy, w2k.gc, None);
    w2k_clip_on = 0;
}

void w2k_focus_rect(Drawable d, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    XSetTSOrigin(w2k.dpy, w2k.gc_focus, 0, 0);
    XFillRectangle(w2k.dpy, d, w2k.gc_focus, x, y, w, 1);
    XFillRectangle(w2k.dpy, d, w2k.gc_focus, x, y + h - 1, w, 1);
    XFillRectangle(w2k.dpy, d, w2k.gc_focus, x, y + 1, 1, h - 2);
    XFillRectangle(w2k.dpy, d, w2k.gc_focus, x + w - 1, y + 1, 1, h - 2);
}

void w2k_dither(Drawable d, int x, int y, int w, int h, int fg, int bg)
{
    if (w <= 0 || h <= 0) return;
    w2k_fill(d, x, y, w, h, bg);
    XSetForeground(w2k.dpy, w2k.gc_dither, w2k.col[fg]);
    XSetTSOrigin(w2k.dpy, w2k.gc_dither, 0, 0);
    XFillRectangle(w2k.dpy, d, w2k.gc_dither, x, y, w, h);
}

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */
int w2k_text_width(int font, const char *s, int len)
{
    if (!s) return 0;
    if (len < 0) len = strlen(s);
    return w2k_font_px_width(font, s, len);
}

int w2k_font_height(int font) { return w2k_font_px_height(font); }

int w2k_font_ascent(int font) { return w2k_font_px_ascent(font); }

/* y is the top of the line, as every caller expects; the backend wants a
 * baseline. */
void w2k_textn(Drawable d, int font, int x, int y, const char *s, int len,
               int color)
{
    if (!s || len <= 0) return;
    w2k_font_draw(d, font, x, y + w2k_font_px_ascent(font), s, len, color);
}

void w2k_text(Drawable d, int font, int x, int y, const char *s, int color)
{
    if (s) w2k_textn(d, font, x, y, s, strlen(s), color);
}

void w2k_text_disabled(Drawable d, int font, int x, int y, const char *s)
{
    /* White ghost down-right, then grey on top -- the engraved look. */
    w2k_text(d, font, x + 1, y + 1, s, C_HILIGHT);
    w2k_text(d, font, x, y, s, C_GRAYTEXT);
}

/* Strip '&' markers into `out`, reporting which output character carried the
 * mnemonic. '&&' collapses to a literal '&'. */
static int strip_mnemonic(const char *s, char *out, int outsz, int *ul)
{
    int n = 0;
    *ul = -1;
    for (const char *p = s; *p && n < outsz - 1; p++) {
        if (*p == '&') {
            if (p[1] == '&') { out[n++] = '&'; p++; }
            else if (p[1])   { if (*ul < 0) *ul = n; }
            continue;
        }
        out[n++] = *p;
    }
    out[n] = '\0';
    return n;
}

int w2k_mnemonic_width(int font, const char *s)
{
    char buf[512];
    int ul;
    int n = strip_mnemonic(s, buf, sizeof buf, &ul);
    return w2k_text_width(font, buf, n);
}

int w2k_text_mnemonic(Drawable d, int font, int x, int y, const char *s,
                      int color, int show_underline)
{
    char buf[512];
    int ul;
    int n = strip_mnemonic(s, buf, sizeof buf, &ul);
    w2k_textn(d, font, x, y, buf, n, color);
    if (show_underline && ul >= 0 && w2k_accel_shown) {
        int x0 = x + w2k_text_width(font, buf, ul);
        int cw = w2k_text_width(font, buf + ul, 1);
        w2k_hline(d, x0, y + w2k_font_px_ascent(font) + 1, cw, color);
    }
    return w2k_text_width(font, buf, n);
}

int w2k_text_mnemonic_rgb(Drawable d, int font, int x, int y, const char *s,
                          int r, int g, int b, int show_underline)
{
    char buf[512];
    int ul;
    int n = strip_mnemonic(s, buf, sizeof buf, &ul);
    buf[n] = '\0';
    w2k_text_rgb(d, font, x, y, buf, r, g, b);
    if (show_underline && ul >= 0 && w2k_accel_shown) {
        int x0 = x + w2k_text_width(font, buf, ul);
        int cw = w2k_text_width(font, buf + ul, 1);
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
        XFillRectangle(w2k.dpy, d, w2k.gc, x0, y + w2k_font_px_ascent(font) + 1,
                       (unsigned)cw, 1);
    }
    return w2k_text_width(font, buf, n);
}

void w2k_ellipsis(int font, const char *s, int maxw, char *buf, int bufsz)
{
    int n = s ? (int)strlen(s) : 0;
    if (n > bufsz - 1) n = bufsz - 1;
    memcpy(buf, s ? s : "", n);
    buf[n] = '\0';
    if (w2k_text_width(font, buf, n) <= maxw) return;

    int dots = w2k_text_width(font, "...", 3);
    while (n > 0 && w2k_text_width(font, buf, n) + dots > maxw) n--;
    if (n + 3 < bufsz) { memcpy(buf + n, "...", 4); }
    else buf[n > 0 ? n : 0] = '\0';
}

/* Rotate text 90 degrees counter-clockwise by transposing a scratch bitmap.
 * Core X fonts cannot be rotated, and this is drawn once per menu, so the
 * per-pixel cost is irrelevant. */
void w2k_text_vertical(Drawable d, int font, int x, int y, const char *s,
                       int color)
{
    if (!s || !*s) return;
    int tw = w2k_text_width(font, s, -1);
    int th = w2k_font_height(font);
    if (tw <= 0 || th <= 0) return;

    /* Render horizontally into a scratch pixmap of the normal depth --
     * Xft cannot draw into a 1-bit drawable -- then read it back and plot
     * the dark pixels rotated. Drawn once per menu, so the per-pixel cost
     * does not matter. */
    /* The caller's clip rectangle is in its own coordinates; applying it to
     * the scratch pixmap would cut the text to ribbons. Suspend it for the
     * render and put it back for the plotting, which does need clipping. */
    int had_clip = w2k_clip_on;
    int cx = w2k_clip_x, cy = w2k_clip_y, cw = w2k_clip_w, ch = w2k_clip_h;
    if (had_clip) w2k_clip_clear();

    Pixmap tmp = XCreatePixmap(w2k.dpy, w2k.root, tw, th, w2k.depth);
    w2k_fill(tmp, 0, 0, tw, th, C_WHITE);
    w2k_font_draw(tmp, font, 0, w2k_font_px_ascent(font), s, (int)strlen(s),
                  C_BLACK);
    if (had_clip) w2k_clip_set(cx, cy, cw, ch);

    XImage *im = XGetImage(w2k.dpy, tmp, 0, 0, tw, th, AllPlanes, ZPixmap);
    if (im) {
        /* Mask off the padding bits: on a 24-bit visual XGetImage hands
         * back 32-bit pixels whose top byte is not the colour we filled. */
        unsigned long white = w2k.col[C_WHITE] & 0xffffffu;
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
        for (int ty = 0; ty < th; ty++)
            for (int tx = 0; tx < tw; tx++)
                if ((XGetPixel(im, tx, ty) & 0xffffffu) != white)
                    XFillRectangle(w2k.dpy, d, w2k.gc, x + ty, y - tx, 1, 1);
        XDestroyImage(im);
    }
    w2k_free_pixmap(tmp);
}
