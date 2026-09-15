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

/* Every primitive maps its rectangle through w2k_cx()/w2k_cw() so that
 * adjacent logical rectangles stay adjacent on a scaled screen: a span is
 * the difference of its mapped ends, never a rounded width. */
void w2k_fill_fg(Drawable d, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    int pw = w2k_cw(x, w), ph = w2k_cw(y, h);
    if (pw <= 0 || ph <= 0) return;
    XFillRectangle(w2k.dpy, d, w2k.gc, w2k_cx(x), w2k_cx(y),
                   (unsigned)pw, (unsigned)ph);
}

void w2k_fill(Drawable d, int x, int y, int w, int h, int color)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    w2k_fill_fg(d, x, y, w, h);
}

void w2k_frame_fg(Drawable d, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    int t = w2k_scale_raw ? w2k_th(1) : 1;
    w2k_fill_fg(d, x, y, w, t);
    w2k_fill_fg(d, x, y + h - t, w, t);
    w2k_fill_fg(d, x, y, t, h);
    w2k_fill_fg(d, x + w - t, y, t, h);
}

void w2k_fill_rgb(Drawable d, int x, int y, int w, int h, int r, int g, int b)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
    w2k_fill_fg(d, x, y, w, h);
}

void w2k_hline(Drawable d, int x, int y, int w, int color)
{
    if (w <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    XFillRectangle(w2k.dpy, d, w2k.gc, w2k_cx(x), w2k_cx(y),
                   (unsigned)w2k_cw(x, w), (unsigned)w2k_t1(y));
}

void w2k_vline(Drawable d, int x, int y, int h, int color)
{
    if (h <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    XFillRectangle(w2k.dpy, d, w2k.gc, w2k_cx(x), w2k_cx(y),
                   (unsigned)w2k_t1(x), (unsigned)w2k_cw(y, h));
}

/* The rings of an edge, in screen pixels.
 *
 * Every line is th(1) thick whatever the scale -- one pixel up to 199%,
 * two from 200% -- as Windows draws them at any DPI. In logical mode
 * the rings are anchored to where the control's inside begins (its
 * rectangle inset by the number of rings), not to its outer corner: at a
 * fractional scale two logical pixels can be three on the screen, and
 * anchoring outward would leave a coloured gap between the edge and the
 * white of an edit box. Anchored inward, any spare pixel falls outside,
 * on the parent's background, where it is invisible. In raw mode the
 * rectangle is already screen pixels and the rings sit at its corner. */
static void ring_bounds(int x, int y, int w, int h, int n, int k, int t,
                        int *x0, int *y0, int *x1, int *y1)
{
    if (w2k_scale_raw) {
        *x0 = x + k * t; *y0 = y + k * t;
        *x1 = x + w - k * t; *y1 = y + h - k * t;
    } else {
        *x0 = w2k_cx(x + n) - (n - k) * t;
        *y0 = w2k_cx(y + n) - (n - k) * t;
        *x1 = w2k_cx(x + w - n) + (n - k) * t;
        *y1 = w2k_cx(y + h - n) + (n - k) * t;
    }
}

/* One ring: (x0,y0)-(x1,y1) exclusive, t thick, in screen pixels.
 * Bottom/right win at the corners, as in Windows. */
static void ring_phys(Drawable d, int x0, int y0, int x1, int y1, int t,
                      int lt, int rb, int flags)
{
    if (x1 - x0 < t || y1 - y0 < t) return;
    if (lt >= 0) {
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[lt]);
        if (flags & BF_TOP) {
            int r = x1 - ((flags & BF_RIGHT) ? t : 0);
            if (r > x0) XFillRectangle(w2k.dpy, d, w2k.gc, x0, y0, (unsigned)(r - x0), (unsigned)t);
        }
        if (flags & BF_LEFT) {
            int b = y1 - ((flags & BF_BOTTOM) ? t : 0);
            if (b > y0) XFillRectangle(w2k.dpy, d, w2k.gc, x0, y0, (unsigned)t, (unsigned)(b - y0));
        }
    }
    if (rb >= 0) {
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[rb]);
        if (flags & BF_BOTTOM)
            XFillRectangle(w2k.dpy, d, w2k.gc, x0, y1 - t, (unsigned)(x1 - x0), (unsigned)t);
        if (flags & BF_RIGHT)
            XFillRectangle(w2k.dpy, d, w2k.gc, x1 - t, y0, (unsigned)t, (unsigned)(y1 - y0));
    }
}

void w2k_frame(Drawable d, int x, int y, int w, int h, int color)
{
    if (w <= 0 || h <= 0) return;
    if (w2k_ui_scale == 100) {
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
        XDrawRectangle(w2k.dpy, d, w2k.gc, x, y, w - 1, h - 1);
        return;
    }
    int t = w2k_th(1), x0, y0, x1, y1;
    ring_bounds(x, y, w, h, 1, 0, t, &x0, &y0, &x1, &y1);
    ring_phys(d, x0, y0, x1, y1, t, color, color, BF_RECT);
}

static void rect_rgb_sides(Drawable d, int x, int y, int w, int h, int flags, int r, int g, int b);

void w2k_edge(Drawable d, int x, int y, int w, int h, int style, int flags)
{
    if (style == EDGE_FLAT || w <= 0 || h <= 0) return;
    if (W2K_THEME_IS7(w2k_theme)) {
        /* Windows 7 draws its wells, panes and group lines flat: one grey
         * line, with a white one inside a well so what is filled at the
         * old two-pixel inset still meets it. Measured. */
        switch (style) {
        case EDGE_SUNKEN:
            rect_rgb_sides(d, x, y, w, h, flags, 171, 173, 179);
            rect_rgb_sides(d, x + ((flags & BF_LEFT) ? 1 : 0), y + ((flags & BF_TOP) ? 1 : 0),
                           w - ((flags & BF_LEFT) ? 1 : 0) - ((flags & BF_RIGHT) ? 1 : 0),
                           h - ((flags & BF_TOP) ? 1 : 0) - ((flags & BF_BOTTOM) ? 1 : 0),
                           flags, 255, 255, 255);
            return;
        case EDGE_SUNKEN_THIN:
            rect_rgb_sides(d, x, y, w, h, flags, 215, 215, 215);
            return;
        case EDGE_ETCHED:
            rect_rgb_sides(d, x, y, w, h, flags, 213, 223, 229);
            return;
        default:
            break;
        }
    }
    const signed char *e = edge_tab[style];
    int n = e[1] >= 0 ? 2 : 1;
    int t = w2k_ui_scale == 100 ? 1 : w2k_th(1);
    for (int k = 0; k < n; k++) {
        int x0, y0, x1, y1;
        if (w2k_ui_scale == 100) {
            /* Rings that are missing a side do not inset on that side. */
            int dx = (flags & BF_LEFT) ? k : 0, dy = (flags & BF_TOP) ? k : 0;
            x0 = x + dx; y0 = y + dy;
            x1 = x + w - ((flags & BF_RIGHT) ? k : 0);
            y1 = y + h - ((flags & BF_BOTTOM) ? k : 0);
        } else {
            ring_bounds(x, y, w, h, n, k, t, &x0, &y0, &x1, &y1);
            if (!(flags & BF_LEFT))   x0 = w2k_cx(x);
            if (!(flags & BF_TOP))    y0 = w2k_cx(y);
            if (!(flags & BF_RIGHT))  x1 = w2k_cx(x + w);
            if (!(flags & BF_BOTTOM)) y1 = w2k_cx(y + h);
        }
        ring_phys(d, x0, y0, x1, y1, t, k ? e[1] : e[0], k ? e[3] : e[2], flags);
    }
}

/* ------------------------------------------------------------------ *
 * The Modern look
 * ------------------------------------------------------------------ */
int w2k_modern_dark(void)
{
    const unsigned char *f = w2k_scheme_rgb(C_FACE);
    return (f[0] * 299 + f[1] * 587 + f[2] * 114) / 1000 < 128;
}

/* The handful of tones the look is drawn in, beside the scheme's own
 * colours: a button is a shade lighter than the face in Light and a
 * shade lighter than it in Dark too, pressed goes the other way, and
 * the outline is the scheme's frame grey. */
void w2k_modern_rgb(int what, int rgb[3])
{
    static const unsigned char light[][3] = {
        [MODERN_BUTTON]  = { 251, 251, 251 }, [MODERN_PRESSED] = { 225, 225, 225 },
        [MODERN_HOT]     = { 235, 235, 235 }, [MODERN_BORDER]  = { 209, 209, 209 },
        [MODERN_ACCENT]  = {   0, 103, 192 },
    };
    static const unsigned char dark[][3] = {
        [MODERN_BUTTON]  = {  45,  45,  45 }, [MODERN_PRESSED] = {  38,  38,  38 },
        [MODERN_HOT]     = {  54,  54,  54 }, [MODERN_BORDER]  = {  62,  62,  62 },
        [MODERN_ACCENT]  = {   0, 120, 212 },
    };
    const unsigned char *t = (w2k_modern_dark() ? dark : light)[what];
    rgb[0] = t[0]; rgb[1] = t[1]; rgb[2] = t[2];
}

/* How far in from the corner's edge row i (0 at the outer edge) of a
 * quarter circle of radius r begins: the largest run of pixels whose
 * centres fall inside the circle. Integer arithmetic in half-pixels. */
int w2k_round_inset(int r, int i)
{
    if (r <= 0 || i >= r) return 0;
    int dy = 2 * (r - i) - 1;                   /* twice the row centre's height */
    for (int ins = 0; ins < r; ins++) {
        int dx = 2 * (r - ins) - 1;
        if (dx * dx + dy * dy <= 4 * r * r) return ins;
    }
    return r;
}

/* Fill a rounded rectangle. The corners are laid out in device pixels,
 * whatever the scale, so the curve is smooth at 200% rather than a copy
 * of the 100% staircase. */
void w2k_round_fill_rgb(Drawable d, int x, int y, int w, int h, int r,
                        int R, int G, int B)
{
    if (w <= 0 || h <= 0) return;
    int x0 = w2k_cx(x), y0 = w2k_cx(y), x1 = w2k_cx(x + w), y1 = w2k_cx(y + h);
    int pw = x1 - x0, ph = y1 - y0;
    if (pw <= 0 || ph <= 0) return;
    int pr = w2k_scale_raw ? r : w2k_px(r);
    if (2 * pr > pw) pr = pw / 2;
    if (2 * pr > ph) pr = ph / 2;
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(R, G, B));
    if (ph > 2 * pr)
        XFillRectangle(w2k.dpy, d, w2k.gc, x0, y0 + pr, (unsigned)pw,
                       (unsigned)(ph - 2 * pr));
    for (int i = 0; i < pr; i++) {
        int ins = w2k_round_inset(pr, i);
        if (2 * ins >= pw) continue;
        XFillRectangle(w2k.dpy, d, w2k.gc, x0 + ins, y0 + i, (unsigned)(pw - 2 * ins), 1);
        XFillRectangle(w2k.dpy, d, w2k.gc, x0 + ins, y1 - 1 - i, (unsigned)(pw - 2 * ins), 1);
    }
}

/* A rounded box: the outline colour under a fill inset by one line. */
void w2k_round_rect_rgb(Drawable d, int x, int y, int w, int h, int r,
                        const int fill[3], const int line[3])
{
    if (w <= 0 || h <= 0) return;
    int t = w2k_scale_raw ? w2k_th(1) : 1;
    w2k_round_fill_rgb(d, x, y, w, h, r, line[0], line[1], line[2]);
    int ri = w2k_scale_raw ? r - t : r - 1;
    w2k_round_fill_rgb(d, x + t, y + t, w - 2 * t, h - 2 * t, ri < 0 ? 0 : ri,
                       fill[0], fill[1], fill[2]);
}

/* A vertical gradient, row by row, in the caller's coordinates. */
static void vgrad_rgb(Drawable d, int x, int y, int w, int h,
                      const int c0[3], const int c1[3])
{
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++) {
        int t = h > 1 ? i * 256 / (h - 1) : 0;
        w2k_fill_rgb(d, x, y + i, w, 1,
                     c0[0] + (c1[0] - c0[0]) * t / 256,
                     c0[1] + (c1[1] - c0[1]) * t / 256,
                     c0[2] + (c1[2] - c0[2]) * t / 256);
    }
}

/* Windows 7's push button: a rounded outline, a white line inside it and
 * a two-tone gradient -- light over the upper half, darker over the
 * lower -- from its theme; pressed, the same in blue. */
static void button7(Drawable d, int x, int y, int w, int h, int pressed)
{
    static const int line_n[3] = { 112, 112, 112 }, line_p[3] = { 44, 98, 139 };
    static const int top0_n[3] = { 242, 242, 242 }, top1_n[3] = { 235, 235, 235 };
    static const int bot0_n[3] = { 221, 221, 221 }, bot1_n[3] = { 207, 207, 207 };
    static const int top0_p[3] = { 229, 244, 252 }, top1_p[3] = { 196, 229, 246 };
    static const int bot0_p[3] = { 152, 209, 239 }, bot1_p[3] = { 104, 179, 219 };
    static const int inner_n[3] = { 252, 252, 252 }, inner_p[3] = { 198, 226, 241 };
    w2k_round_rect_rgb(d, x, y, w, h, 3, pressed ? inner_p : inner_n, pressed ? line_p : line_n);
    int ih = h - 4, half = ih / 2;
    if (ih <= 0) return;
    vgrad_rgb(d, x + 2, y + 2, w - 4, half, pressed ? top0_p : top0_n, pressed ? top1_p : top1_n);
    vgrad_rgb(d, x + 2, y + 2 + half, w - 4, ih - half, pressed ? bot0_p : bot0_n, pressed ? bot1_p : bot1_n);
}

/* A one-pixel rectangle in a colour, the sides `flags` asks for. */
static void rect_rgb_sides(Drawable d, int x, int y, int w, int h, int flags, int r, int g, int b)
{
    if (flags & BF_TOP)    w2k_fill_rgb(d, x, y, w, 1, r, g, b);
    if (flags & BF_BOTTOM) w2k_fill_rgb(d, x, y + h - 1, w, 1, r, g, b);
    if (flags & BF_LEFT)   w2k_fill_rgb(d, x, y, 1, h, r, g, b);
    if (flags & BF_RIGHT)  w2k_fill_rgb(d, x + w - 1, y, 1, h, r, g, b);
}

void w2k_button(Drawable d, int x, int y, int w, int h, int pressed)
{
    if (w <= 0 || h <= 0) return;
    if (W2K_THEME_IS7(w2k_theme)) { button7(d, x, y, w, h, pressed); return; }
    if (w2k_theme == THEME_MODERN) {
        /* A flat rounded box; the corners keep whatever is behind. */
        int f[3], l[3];
        w2k_modern_rgb(pressed ? MODERN_PRESSED : MODERN_BUTTON, f);
        w2k_modern_rgb(MODERN_BORDER, l);
        int r = w2k_scale_raw ? w2k_px(4) : 4;
        int lim = (w < h ? w : h) / 4;
        if (r > lim) r = lim;
        w2k_round_rect_rgb(d, x, y, w, h, r, f, l);
        return;
    }
    /* The face first, over the whole rectangle, then the edge on top: at
     * a fractional scale an inset fill and the rings would not meet. */
    w2k_fill(d, x, y, w, h, C_FACE);
    if (pressed) {
        /* A depressed pushbutton loses the highlight entirely: a single
         * shadow ring, with the face pushed in by one pixel. */
        w2k_frame(d, x, y, w, h, C_SHADOW);
    } else {
        w2k_edge(d, x, y, w, h, EDGE_BUTTON, BF_RECT);
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
    int px = w2k_cx(x), py = w2k_cx(y), pw = w2k_cw(x, w);
    h = w2k_cw(y, h);
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
        XFillRectangle(w2k.dpy, d, w2k.gc, px, py + i, (unsigned)pw, 1);
    }
}

void w2k_gradient(Drawable d, int x, int y, int w, int h, int c1, int c2)
{
    if (w <= 0 || h <= 0) return;
    int r1, g1, b1, r2, g2, b2;
    w2k_color_rgb(c1, &r1, &g1, &b1);
    w2k_color_rgb(c2, &r2, &g2, &b2);
    int px = w2k_cx(x), py = w2k_cx(y), ph = w2k_cw(y, h), lw = w;
    w = w2k_cw(x, w);
    if (w == 1 || (r1 == r2 && g1 == g2 && b1 == b2)) { w2k_fill(d, x, y, lw, h, c1); return; }
    /* A band per colour, not a column per pixel: a 2560-pixel caption was
     * 5,120 requests at every repaint of a frame -- every step of a drag,
     * every change of focus -- though it holds at most 256 shades each way. */
    unsigned long last = 0;
    int start = 0;
    for (int i = 0; i <= w; i++) {
        unsigned long px_c = 0;
        if (i < w) {
            int r = r1 + (r2 - r1) * i / (w - 1);
            int g = g1 + (g2 - g1) * i / (w - 1);
            int b = b1 + (b2 - b1) * i / (w - 1);
            px_c = w2k_rgb(r, g, b);
            if (i == 0) { last = px_c; continue; }
            if (px_c == last) continue;
        }
        XSetForeground(w2k.dpy, w2k.gc, last);
        XFillRectangle(w2k.dpy, d, w2k.gc, px + start, py, (unsigned)(i - start), (unsigned)ph);
        start = i;
        last = px_c;
    }
}

/* The current clip rectangle, mirrored so masked icon blits (which use
 * their own GC) can honour it by intersecting. */
int w2k_clip_on, w2k_clip_x, w2k_clip_y, w2k_clip_w, w2k_clip_h;

void w2k_clip_set(int x, int y, int w, int h)
{
    /* Kept in physical pixels: the icon and skin blits intersect with it
     * after mapping their own rectangles. */
    int pw = w2k_cw(x, w), ph = w2k_cw(y, h);
    x = w2k_cx(x); y = w2k_cx(y); w = pw; h = ph;
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
    int px = w2k_cx(x), py = w2k_cx(y), pw = w2k_cw(x, w), ph = w2k_cw(y, h);
    int t = w2k_th(1);
    XSetTSOrigin(w2k.dpy, w2k.gc_focus, 0, 0);
    XFillRectangle(w2k.dpy, d, w2k.gc_focus, px, py, pw, t);
    XFillRectangle(w2k.dpy, d, w2k.gc_focus, px, py + ph - t, pw, t);
    XFillRectangle(w2k.dpy, d, w2k.gc_focus, px, py + t, t, ph - 2 * t);
    XFillRectangle(w2k.dpy, d, w2k.gc_focus, px + pw - t, py + t, t, ph - 2 * t);
}

void w2k_dither(Drawable d, int x, int y, int w, int h, int fg, int bg)
{
    if (w <= 0 || h <= 0) return;
    w2k_fill(d, x, y, w, h, bg);
    XSetForeground(w2k.dpy, w2k.gc_dither, w2k.col[fg]);
    XSetTSOrigin(w2k.dpy, w2k.gc_dither, 0, 0);
    XFillRectangle(w2k.dpy, d, w2k.gc_dither, w2k_cx(x), w2k_cx(y),
                   (unsigned)w2k_cw(x, w), (unsigned)w2k_cw(y, h));
}

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */
int w2k_text_width(int font, const char *s, int len)
{
    if (!s) return 0;
    if (len < 0) len = strlen(s);
    int px = w2k_font_px_width(font, s, len);
    return w2k_scale_raw ? px : w2k_lp(px);
}

/* Physical metrics are what the fonts report; the logical ones the
 * programs lay out with are those divided by the scale. In raw mode the
 * two coincide. */
int w2k_font_height(int font)
{
    int px = w2k_font_px_height(font);
    return w2k_scale_raw ? px : w2k_lp(px);
}

int w2k_font_ascent(int font)
{
    int px = w2k_font_px_ascent(font);
    return w2k_scale_raw ? px : w2k_lp(px);
}

/* y is the top of the line, as every caller expects; the backend wants a
 * baseline. */
void w2k_textn(Drawable d, int font, int x, int y, const char *s, int len,
               int color)
{
    if (!s || len <= 0) return;
    w2k_font_draw(d, font, w2k_cx(x), w2k_cx(y) + w2k_font_px_ascent(font),
                  s, len, color);
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

/* A name that comes from data -- a file, a program -- made safe to use as
 * a label: "&" doubled, so "Tom & Jerry.mp4" shows as it is rather than
 * with an underlined space where the ampersand was. */
void w2k_menu_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 2 < n; p++) {
        if (*p == '&') out[o++] = '&';
        out[o++] = *p;
    }
    if (n) out[o < n ? o : n - 1] = 0;
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
        w2k_hline(d, x0, y + w2k_font_ascent(font) + 1, cw, color);
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
        w2k_fill_rgb(d, x0, y + w2k_font_ascent(font) + 1, cw, 1, r, g, b);
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
    /* The longest prefix that fits with the dots, by bisection: a report
     * view of long names measured each one a character at a time. */
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (w2k_text_width(font, buf, mid) + dots <= maxw) lo = mid; else hi = mid - 1;
    }
    n = lo;
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
    int tw = w2k_font_px_width(font, s, (int)strlen(s));
    int th = w2k_font_px_height(font);
    if (tw <= 0 || th <= 0) return;
    x = w2k_cx(x); y = w2k_cx(y);

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
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_WHITE]);
    XFillRectangle(w2k.dpy, tmp, w2k.gc, 0, 0, (unsigned)tw, (unsigned)th);
    w2k_font_draw(tmp, font, 0, w2k_font_px_ascent(font), s, (int)strlen(s),
                  C_BLACK);
    if (had_clip) w2k_clip_set(cx, cy, cw, ch);

    XImage *im = XGetImage(w2k.dpy, tmp, 0, 0, tw, th, AllPlanes, ZPixmap);
    if (im) {
        /* Mask off the padding bits: on a 24-bit visual XGetImage hands
         * back 32-bit pixels whose top byte is not the colour we filled. */
        unsigned long white = w2k.col[C_WHITE] & 0xffffffu;
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
        /* Batched: a dark pixel used to be a request of its own, and the
         * Start menu repaints its banner every time the highlighted item
         * changes -- some five hundred and sixty requests per hover. */
        XRectangle run[256];
        int k = 0;
        for (int ty = 0; ty < th; ty++)
            for (int tx = 0; tx < tw; tx++) {
                if ((XGetPixel(im, tx, ty) & 0xffffffu) == white) continue;
                run[k].x = (short)(x + ty);
                run[k].y = (short)(y - tx);
                run[k].width = run[k].height = 1;
                if (++k == (int)(sizeof run / sizeof *run)) {
                    XFillRectangles(w2k.dpy, d, w2k.gc, run, k);
                    k = 0;
                }
            }
        if (k) XFillRectangles(w2k.dpy, d, w2k.gc, run, k);
        XDestroyImage(im);
    }
    w2k_free_pixmap(tmp);
}
