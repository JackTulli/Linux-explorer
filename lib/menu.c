/* menu.c -- classic popup menus: the modal, grabbing, submenu-chaining kind.
 *
 * One nested event loop drives an entire chain of open menus, which is how
 * Windows does it and is far simpler than a retained widget hierarchy. */
#include "w2k.h"
#include <X11/extensions/shape.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define ITEM_H       18      /* a normal text row                    */
#define SEP_H         7      /* separator: 3px air, etched line, 3px */
#define MENU_BORDER   3      /* 2px raised edge + 1px of padding     */
#define ICON_COL     20      /* icon / checkmark gutter              */
#define ARROW_COL    14      /* submenu triangle gutter              */
#define ACCEL_GAP    18      /* minimum air before the accelerator   */
#define BIG_ITEM_H   34      /* Start-menu row with a 32x32 icon     */
#define BIG_ICON_COL 38
#define BANNER_W     21      /* the branding strip down the left     */
#define SUBMENU_LAP   3      /* submenus overlap their parent slightly */

typedef struct {
    int      id;
    char    *text;
    char    *accel;
    int      icon;
    unsigned separator : 1;
    unsigned disabled  : 1;
    unsigned checked   : 1;
    unsigned radio     : 1;
    unsigned isdefault : 1;
    W2kMenu *sub;
} Item;

struct W2kMenu {
    Item *items;
    int   n, cap;
    char *banner;            /* non-NULL => Start-menu styling */
    /* Layout, computed by menu_layout(): items flow top-to-bottom, then
     * into further columns when they would not fit on screen. */
    int  *ix, *iy;           /* per-item offset inside the window */
    int   cols, col_w, body_h;
};

void (*w2k_menu_foreign_event)(XEvent *e);

/* Height of the monitor the menu being laid out will appear on. */
static int menu_max_h;
/* Non-NULL while a menu should treat unclaimed printable keys as type-ahead:
 * the character lands here and the menu closes. */
char *w2k_menu_typeahead;
void (*w2k_menu_closed)(void);

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */
W2kMenu *w2k_menu_new(void)
{
    return w2k_alloc(sizeof(W2kMenu));
}

static Item *push(W2kMenu *m)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->items = realloc(m->items, m->cap * sizeof *m->items);
        if (!m->items) abort();
    }
    Item *it = &m->items[m->n++];
    memset(it, 0, sizeof *it);
    it->icon = ICO_NONE;
    return it;
}

void w2k_menu_item(W2kMenu *m, int id, const char *text, const char *accel,
                   int icon)
{
    Item *it = push(m);
    it->id = id;
    it->text = w2k_strdup(text);
    it->accel = accel ? w2k_strdup(accel) : NULL;
    it->icon = icon;
}

void w2k_menu_sub(W2kMenu *m, const char *text, int icon, W2kMenu *sub)
{
    Item *it = push(m);
    it->text = w2k_strdup(text);
    it->icon = icon;
    it->sub = sub;
}

void w2k_menu_sep(W2kMenu *m)     { push(m)->separator = 1; }
void w2k_menu_disable(W2kMenu *m) { if (m->n) m->items[m->n - 1].disabled = 1; }
void w2k_menu_check(W2kMenu *m, int on)
                                  { if (m->n) m->items[m->n - 1].checked = on ? 1 : 0; }
void w2k_menu_radio(W2kMenu *m, int on)
{
    if (!m->n) return;
    m->items[m->n - 1].checked = on ? 1 : 0;
    m->items[m->n - 1].radio = 1;
}
void w2k_menu_default(W2kMenu *m)  { if (m->n) m->items[m->n - 1].isdefault = 1; }
void w2k_menu_set_banner(W2kMenu *m, const char *text)
                                   { free(m->banner); m->banner = w2k_strdup(text); }
int  w2k_menu_count(W2kMenu *m)    { return m ? m->n : 0; }

void w2k_menu_free(W2kMenu *m)
{
    if (!m) return;
    for (int i = 0; i < m->n; i++) {
        free(m->items[i].text);
        free(m->items[i].accel);
        w2k_menu_free(m->items[i].sub);
    }
    free(m->items);
    free(m->banner);
    free(m->ix);
    free(m->iy);
    free(m);
}

/* ------------------------------------------------------------------ *
 * Metrics
 * ------------------------------------------------------------------ */
/* The Start menu's big row height, unless small icons are switched on. */
static int menu_big(W2kMenu *m)
{
    return m->banner && !w2k_start_small_icons;
}

static int item_h(W2kMenu *m, Item *it)
{
    if (it->separator) return SEP_H;
    return menu_big(m) ? BIG_ITEM_H : ITEM_H;
}

static int icon_col(W2kMenu *m) { return menu_big(m) ? BIG_ICON_COL : ICON_COL; }

static void menu_layout(W2kMenu *m)
{
    int textw = 0, accelw = 0;
    for (int i = 0; i < m->n; i++) {
        Item *it = &m->items[i];
        if (it->separator) continue;
        int tw = w2k_mnemonic_width(it->isdefault ? F_UI_BOLD : F_UI, it->text);
        if (tw > textw) textw = tw;
        if (it->accel) {
            int aw = w2k_text_width(F_UI, it->accel, -1);
            if (aw > accelw) accelw = aw;
        }
    }
    int cw = icon_col(m) + textw + ARROW_COL;
    if (accelw) cw += ACCEL_GAP + accelw;
    if (cw < 100) cw = 100;
    m->col_w = cw;

    m->ix = realloc(m->ix, (m->n ? m->n : 1) * sizeof *m->ix);
    m->iy = realloc(m->iy, (m->n ? m->n : 1) * sizeof *m->iy);
    if (!m->ix || !m->iy) abort();

    /* Wrap into another column at the bottom of the *monitor*, not of the
     * virtual screen -- otherwise a long menu on a multi-head desktop runs
     * off the bottom of the one it is on. */
    int max_h = menu_max_h ? menu_max_h : w2k.sh;
    max_h -= 2 * MENU_BORDER + 8;
    int left = m->banner ? BANNER_W : 0;
    int col = 0, y = 0, tallest = 0;
    for (int i = 0; i < m->n; i++) {
        int h = item_h(m, &m->items[i]);
        if (y + h > max_h && y > 0) { col++; y = 0; }
        m->ix[i] = MENU_BORDER + left + col * cw;
        m->iy[i] = MENU_BORDER + y;
        y += h;
        if (y > tallest) tallest = y;
    }
    m->cols = col + 1;
    m->body_h = tallest;
}

static void menu_size(W2kMenu *m, int *w, int *h)
{
    menu_layout(m);
    *w = (m->banner ? BANNER_W : 0) + m->cols * m->col_w + 2 * MENU_BORDER;
    *h = m->body_h + 2 * MENU_BORDER;
}

/* y offset of item i inside the menu window */
static int item_y(W2kMenu *m, int idx) { return m->iy[idx]; }

static int item_at(W2kMenu *m, int x, int y)
{
    for (int i = 0; i < m->n; i++) {
        int h = item_h(m, &m->items[i]);
        if (x >= m->ix[i] && x < m->ix[i] + m->col_w &&
            y >= m->iy[i] && y < m->iy[i] + h)
            return m->items[i].separator ? -1 : i;
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */
static void draw_check(Drawable d, int x, int y, int color)
{
    /* The classic 7x7 tick, one pixel at a time. */
    static const char *rows[] = {
        "     #", "    ##", "#  ###", "## ###",
        "#####", " ###", "  #", NULL };
    static const signed char ox[] = { 0, 0, 0, 0, 0, 1, 2 };
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    for (int r = 0; rows[r]; r++)
        for (int c = 0; rows[r][c]; c++)
            if (rows[r][c] == '#')
                w2k_fill_fg(d, x + c + ox[r], y + r, 1, 1);
}

static void draw_bullet(Drawable d, int x, int y, int color)
{
    w2k_fill(d, x + 1, y, 4, 6, color);
    w2k_fill(d, x, y + 1, 6, 4, color);
}

/* Right-pointing solid triangle for submenu items. */
static void draw_arrow(Drawable d, int x, int y, int color)
{
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    for (int i = 0; i < 4; i++)
        w2k_fill_fg(d, x + i, y + 3 - i, 1, 1 + 2 * i);
}

/* The Start menu banner gradient.
 *
 * The shape is measured off a Windows 2000 screenshot rather than assumed:
 * the strip is solid at the top colour for its first third, ramps to the
 * bottom colour across the middle, and is solid again for the last eighth.
 * A straight top-to-bottom ramp puts far too much blue in the upper half.
 *
 * Smooth is one row per shade. Dithered is how a 256-colour desktop had to
 * do it: solid bands with a 50% checkerboard of the next shade across each
 * join, which is what gives the original its faintly striped look on period
 * hardware. */
#define BANNER_RAMP_START 33        /* percent of the strip */
#define BANNER_RAMP_END   88

/* Position down the strip (0..h) to position along the ramp (0..255). */
static int banner_t(int i, int h)
{
    if (h < 2) return 0;
    int pct = i * 100 / (h - 1);
    if (pct <= BANNER_RAMP_START) return 0;
    if (pct >= BANNER_RAMP_END) return 255;
    return (pct - BANNER_RAMP_START) * 255 /
           (BANNER_RAMP_END - BANNER_RAMP_START);
}

static unsigned long banner_shade(int t)
{
    const int *a = w2k_start_banner_top, *b = w2k_start_banner_bottom;
    return w2k_rgb(a[0] + (b[0] - a[0]) * t / 255,
                   a[1] + (b[1] - a[1]) * t / 255,
                   a[2] + (b[2] - a[2]) * t / 255);
}

void w2k_menu_banner_fill(Drawable d, int x, int y, int w, int h)
{
    if (h < 1 || w < 1) return;
    /* Worked in screen pixels: the ramp is one row per shade. */
    int pw = w2k_cw(x, w), ph = w2k_cw(y, h);
    x = w2k_cx(x); y = w2k_cx(y); w = pw; h = ph;
    if (h < 1 || w < 1) return;

    if (!w2k_start_banner_dither) {
        for (int i = 0; i < h; i++) {
            XSetForeground(w2k.dpy, w2k.gc, banner_shade(banner_t(i, h)));
            XFillRectangle(w2k.dpy, d, w2k.gc, x, y + i, w, 1);
        }
        return;
    }

    int bands = 8;
    for (int k = 0; k < bands; k++) {
        int y0 = y + h * k / bands, y1 = y + h * (k + 1) / bands;
        unsigned long c0 = banner_shade(banner_t(y0 - y, h));
        unsigned long c1 = banner_shade(banner_t(y1 - y, h));
        XSetForeground(w2k.dpy, w2k.gc, c0);
        XFillRectangle(w2k.dpy, d, w2k.gc, x, y0, w, y1 - y0);
        /* Lower half of the band: checkerboard towards the next shade. */
        int mid = y0 + (y1 - y0) / 2;
        XSetForeground(w2k.dpy, w2k.gc_dither, c1);
        XSetTSOrigin(w2k.dpy, w2k.gc_dither, 0, 0);
        XFillRectangle(w2k.dpy, d, w2k.gc_dither, x, mid, w, y1 - mid);
    }
}

/* One buffer per menu window, kept while the menu is up: moving the pointer
 * down a menu repaints it once per item. */
static struct { Window win; Pixmap pm; int w, h; } menu_buf[8];

static Pixmap menu_buffer(Window win, int w, int h)
{
    int free_slot = -1;
    for (int i = 0; i < 8; i++) {
        if (menu_buf[i].win == win) {
            if (menu_buf[i].w == w && menu_buf[i].h == h) return menu_buf[i].pm;
            w2k_free_pixmap(menu_buf[i].pm);
            menu_buf[i].pm = XCreatePixmap(w2k.dpy, win, w, h, w2k.depth);
            menu_buf[i].w = w;
            menu_buf[i].h = h;
            return menu_buf[i].pm;
        }
        if (!menu_buf[i].win && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {                     /* deeper than we cache */
        return XCreatePixmap(w2k.dpy, win, w, h, w2k.depth);
    }
    menu_buf[free_slot].win = win;
    menu_buf[free_slot].pm = XCreatePixmap(w2k.dpy, win, w, h, w2k.depth);
    menu_buf[free_slot].w = w;
    menu_buf[free_slot].h = h;
    return menu_buf[free_slot].pm;
}

static int menu_buffer_cached(Window win)
{
    for (int i = 0; i < 8; i++) if (menu_buf[i].win == win) return 1;
    return 0;
}

/* Called when a menu window is destroyed. */
static void menu_buffer_drop(Window win)
{
    for (int i = 0; i < 8; i++)
        if (menu_buf[i].win == win) {
            w2k_free_pixmap(menu_buf[i].pm);
            menu_buf[i].win = 0;
            menu_buf[i].pm = 0;
            return;
        }
}

static void menu_paint(W2kMenu *m, Window win, int w, int h, int sel)
{
    Pixmap pm = menu_buffer(win, w2k_px(w), w2k_px(h));
    int owned = 1;
    for (int i = 0; i < 8; i++) if (menu_buf[i].pm == pm) owned = 0;

    w2k_fill(pm, 0, 0, w, h, C_MENU);
    w2k_edge(pm, 0, 0, w, h, EDGE_RAISED, BF_RECT);

    int left = MENU_BORDER;
    if (m->banner) {
        /* The vertical strip down the left of the Start menu, with the
         * product name reading upward. Colours and dithering come from the
         * settings; see w2k_menu_banner_fill(). */
        int bx = MENU_BORDER, by = MENU_BORDER;
        int bh = h - 2 * MENU_BORDER;
        w2k_menu_banner_fill(pm, bx, by, BANNER_W, bh);
        w2k_text_vertical(pm, F_UI_BOLD, bx + 4, by + bh - 6, m->banner,
                          C_WHITE);
    }
    (void)left;

    int icol = icon_col(m);
    int fh = w2k_font_height(F_UI);

    for (int i = 0; i < m->n; i++) {
        Item *it = &m->items[i];
        int iy = item_y(m, i), ih = item_h(m, it);
        int left = m->ix[i], w = m->ix[i] + m->col_w;   /* this column */

        if (it->separator) {
            int sx = left, sw = w - left;
            w2k_hline(pm, sx, iy + SEP_H / 2 - 1, sw, C_SHADOW);
            w2k_hline(pm, sx, iy + SEP_H / 2,     sw, C_HILIGHT);
            continue;
        }

        int hot = (i == sel) && !it->disabled;
        int tcol = it->disabled ? C_GRAYTEXT : (hot ? C_HIGHLIGHTTEXT : C_MENUTEXT);
        if (hot)
            w2k_fill(pm, left, iy, w - left, ih, C_HIGHLIGHT);

        int isz = menu_big(m) ? 32 : 16;
        int ix = left + (icol - isz) / 2, icy = iy + (ih - isz) / 2;
        if (it->checked && it->icon < 0) {
            /* A checked item without an icon gets a sunken, dithered well. */
            w2k_dither(pm, left + 1, iy + 1, icol - 2, ih - 2, C_HILIGHT, C_FACE);
            w2k_edge(pm, left + 1, iy + 1, icol - 2, ih - 2, EDGE_SUNKEN_THIN, BF_RECT);
            if (it->radio) draw_bullet(pm, left + icol / 2 - 3, iy + ih / 2 - 3,
                                       it->disabled ? C_GRAYTEXT : C_MENUTEXT);
            else           draw_check(pm, left + icol / 2 - 3, iy + ih / 2 - 4,
                                      it->disabled ? C_GRAYTEXT : C_MENUTEXT);
        } else if (it->icon >= 0) {
            if (it->disabled)      w2k_icon_draw_disabled(pm, ix, icy, it->icon);
            else if (menu_big(m))  w2k_bigicon_draw(pm, ix, icy, it->icon);
            else                   w2k_icon_draw(pm, ix, icy, it->icon);
        }

        int tf = it->isdefault ? F_UI_BOLD : F_UI;
        int tx = left + icol;
        int ty = iy + (ih - fh) / 2;
        if (it->disabled && !hot) {
            char buf[256];
            /* Reuse the mnemonic stripper by drawing twice, engraved. */
            w2k_ellipsis(tf, "", 0, buf, sizeof buf);
            w2k_text_mnemonic(pm, tf, tx + 1, ty + 1, it->text, C_HILIGHT, 1);
            w2k_text_mnemonic(pm, tf, tx, ty, it->text, C_GRAYTEXT, 1);
        } else {
            w2k_text_mnemonic(pm, tf, tx, ty, it->text, tcol, 1);
        }

        if (it->accel)
            w2k_text(pm, F_UI, w - ARROW_COL / 2 -
                     w2k_text_width(F_UI, it->accel, -1), ty, it->accel, tcol);
        if (it->sub)
            draw_arrow(pm, w - 9, iy + ih / 2 - 3, tcol);
    }

    XCopyArea(w2k.dpy, pm, win, w2k.gc, 0, 0, (unsigned)w2k_px(w),
              (unsigned)w2k_px(h), 0, 0);
    if (owned) w2k_free_pixmap(pm);     /* an uncached, deep level */
}

/* ------------------------------------------------------------------ *
 * The modal chain
 * ------------------------------------------------------------------ */
#define MAXLEVEL 8

typedef struct {
    W2kMenu *m;
    Window   win;
    Window   shadow;             /* 0 when the effect is off */
    int      x, y, w, h;         /* x, y on the screen; w, h logical */
    int      pw, ph;             /* the window's size on the screen */
    int      sel;
} Level;

/* Destroy a level's windows, shadow included. */
static void level_destroy(Level *lv)
{
    menu_buffer_drop(lv->win);
    XDestroyWindow(w2k.dpy, lv->win);
    if (lv->shadow) {
        XDestroyWindow(w2k.dpy, lv->shadow);
        lv->shadow = 0;
    }
}


/* The shadow is its own window, offset down and right: black, but shaped
 * to a 50% checkerboard, so every other pixel is not window at all and
 * whatever is underneath shows through at half strength. That is how the
 * effect looked without a compositor -- and shaping it, rather than
 * painting a stipple on a solid window, means nothing to repaint: an
 * exposed solid window came back plain black. */
static Window make_shadow_window(int x, int y, int w, int h)
{
    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel = w2k.col[C_BLACK],
    };
    Window s = XCreateWindow(w2k.dpy, w2k.root, x, y, (unsigned)w, (unsigned)h,
                             0, CopyFromParent, InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel, &a);
    Pixmap mask = XCreatePixmap(w2k.dpy, s, (unsigned)w, (unsigned)h, 1);
    XGCValues gv = { .foreground = 0, .background = 0 };
    GC g = XCreateGC(w2k.dpy, mask, GCForeground | GCBackground, &gv);
    XFillRectangle(w2k.dpy, mask, g, 0, 0, (unsigned)w, (unsigned)h);
    XSetForeground(w2k.dpy, g, 1);
    XSetFillStyle(w2k.dpy, g, FillStippled);
    XSetStipple(w2k.dpy, g, w2k.pm_dither);
    XSetTSOrigin(w2k.dpy, g, 0, 0);
    XFillRectangle(w2k.dpy, mask, g, 0, 0, (unsigned)w, (unsigned)h);
    XFreeGC(w2k.dpy, g);
    XShapeCombineMask(w2k.dpy, s, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreePixmap(w2k.dpy, mask);
    XMapWindow(w2k.dpy, s);
    return s;
}

static Window make_menu_window(int x, int y, int w, int h)
{
    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel  = w2k.col[C_MENU],
        .save_under        = True,
        .event_mask        = ExposureMask | ButtonPressMask |
                             ButtonReleaseMask | PointerMotionMask
    };
    Window win = XCreateWindow(w2k.dpy, w2k.root, x, y, w, h, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWSaveUnder |
                               CWEventMask, &a);
    return win;                  /* mapped by the caller, slid in or not */
}

static void open_level(Level *lv, W2kMenu *m, int px, int py, int flags,
                       int parent_right, int parent_left)
{
    const W2kMonitor *mon0 =
        w2k_monitor_at(parent_right >= 0 ? parent_left : px, py);
    menu_max_h = w2k_lp(mon0->h);

    int w, h;
    menu_size(m, &w, &h);
    int pw = w2k_px(w), ph = w2k_px(h);      /* on the screen */
    int x = px, y = py;

    if (flags & MPOP_RIGHTALIGN) x -= pw;
    if (flags & MPOP_BOTTOMUP)   y -= ph;

    /* Keep the menu on the monitor it was invoked from. Clamping to the
     * virtual screen instead lets a menu near the right edge of one panel
     * spill onto the next one.
     *
     * Submenus are opened with px = 0 -- their x comes from the parent's
     * edges, not from the anchor -- so asking about px would always name the
     * leftmost monitor, and a submenu on any other screen would think it had
     * run out of room and flip to the left, onto its neighbour. The parent's
     * own edge is a real coordinate on the right monitor. */
    const W2kMonitor *mon = w2k_monitor_at(parent_right >= 0 ? parent_left : px,
                                           py);
    int mx = mon->x, my = mon->y, mw = mon->w, mh = mon->h;

    int lap = w2k_px(SUBMENU_LAP), sh_off = w2k_px(4);
    if (parent_right >= 0) {                       /* submenu placement */
        x = parent_right - lap;
        if (x + pw > mx + mw) {
            x = parent_left - pw + lap;
            if (x < mx) x = mx + mw - pw;
        }
    } else {
        if (x + pw > mx + mw) x = mx + mw - pw;
        if (x < mx) x = mx;
    }
    if (y + ph > my + mh) y = my + mh - ph;
    if (y < my) y = my;

    lv->m = m;
    lv->x = x; lv->y = y; lv->w = w; lv->h = h;
    lv->pw = pw; lv->ph = ph;
    lv->sel = -1;
    /* The shadow is created first so it stacks underneath. */
    lv->shadow = w2k_effects[FX_MENU_SHADOW]
               ? make_shadow_window(x + sh_off, y + sh_off, pw, ph) : 0;
    lv->win = make_menu_window(x, y, pw, ph);

    /* "Fade or slide menus into view": the menu slides out from the edge
     * it is anchored to. Nothing fades -- without a compositor a fade is
     * a lie -- so this is the slide half, which is what the setting did
     * on hardware without alpha. The menu is painted into its buffer
     * first and the window grows over it, showing more of the picture at
     * each step: the earlier version grew an unpainted window and let
     * the picture arrive afterwards, which looked like a glitch. */
    /* (Only with a cached buffer to copy from: deeper than the cache the
     * menu just appears, rather than growing over an unpainted pixmap.) */
    int slide = w2k_effects[FX_FADE_MENUS] && h > 24;
    if (slide) {
        menu_paint(m, lv->win, w, h, -1);
        slide = menu_buffer_cached(lv->win);
    }
    if (slide) {
        int up = (flags & MPOP_BOTTOMUP) != 0;
        Pixmap pm = menu_buffer(lv->win, pw, ph);
        for (int step = 1; step <= 6; step++) {
            int sh = ph * step / 6;
            if (sh < 4) sh = 4;
            int wy = up ? y + ph - sh : y;
            XMoveResizeWindow(w2k.dpy, lv->win, x, wy, (unsigned)pw, (unsigned)sh);
            if (lv->shadow)
                XMoveResizeWindow(w2k.dpy, lv->shadow, x + sh_off, wy + sh_off,
                                  (unsigned)pw, (unsigned)sh);
            if (step == 1) XMapRaised(w2k.dpy, lv->win);
            XCopyArea(w2k.dpy, pm, lv->win, w2k_copy_gc(), 0, up ? ph - sh : 0,
                      (unsigned)pw, (unsigned)sh, 0, 0);
            XFlush(w2k.dpy);
            usleep(12000);
        }
        XMoveResizeWindow(w2k.dpy, lv->win, x, y, (unsigned)pw, (unsigned)ph);
    } else
        XMapRaised(w2k.dpy, lv->win);
}

static int level_at(Level *lv, int n, int rx, int ry)
{
    for (int i = n - 1; i >= 0; i--)
        if (rx >= lv[i].x && rx < lv[i].x + lv[i].pw &&
            ry >= lv[i].y && ry < lv[i].y + lv[i].ph)
            return i;
    return -1;
}

/* Skip separators when arrow-keying. */
static int next_selectable(W2kMenu *m, int from, int dir)
{
    if (m->n == 0) return -1;
    int i = from;
    for (int k = 0; k < m->n; k++) {
        i += dir;
        if (i < 0) i = m->n - 1;
        if (i >= m->n) i = 0;
        if (!m->items[i].separator) return i;
    }
    return -1;
}

static int match_mnemonic(W2kMenu *m, KeySym ks)
{
    if (ks < 32 || ks > 126) return -1;
    int want = (ks >= 'A' && ks <= 'Z') ? ks + 32 : ks;
    for (int i = 0; i < m->n; i++) {
        const char *t = m->items[i].text;
        if (!t || m->items[i].separator) continue;
        for (const char *p = t; *p; p++)
            if (*p == '&' && p[1]) {
                int ch = p[1];
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                if (ch == want) return i;
                break;
            }
    }
    return -1;
}

/* Set by a caller that wants to offer a context menu on its own items --
 * the Start menu does, for pinned entries and program entries. */
int (*w2k_menu_on_context)(int id, int root_x, int root_y);

static int menu_popup(W2kMenu *m, int x, int y, int flags);

/* Menus are laid out in logical pixels even when the window manager,
 * which draws its chrome raw, opens them. */
int w2k_menu_popup(W2kMenu *m, int x, int y, int flags)
{
    int raw = w2k_scale_raw;
    w2k_scale_raw = 0;
    int r = menu_popup(m, x, y, flags);
    w2k_scale_raw = raw;
    return r;
}

static int menu_popup(W2kMenu *m, int x, int y, int flags)
{
    if (!m || m->n == 0) return 0;

    Level lv[MAXLEVEL];
    int n = 1;
    open_level(&lv[0], m, x, y, flags, -1, -1);
    w2k_sound_play(SND_MENUPOPUP);

    if (XGrabPointer(w2k.dpy, lv[0].win, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, w2k.cur_arrow,
                     CurrentTime) != GrabSuccess) {
        level_destroy(&lv[0]);
        return 0;
    }
    XGrabKeyboard(w2k.dpy, lv[0].win, True, GrabModeAsync, GrabModeAsync,
                  CurrentTime);

    long opened = w2k_now_ms();
    int result = 0, done = 0;
    int repaint = 1;

    while (!done) {
        if (repaint) {
            for (int i = 0; i < n; i++)
                menu_paint(lv[i].m, lv[i].win, lv[i].w, lv[i].h, lv[i].sel);
            repaint = 0;
        }
        XEvent e;
        XNextEvent(w2k.dpy, &e);

        switch (e.type) {
        case Expose:
            for (int i = 0; i < n; i++)
                if (e.xexpose.window == lv[i].win) repaint = 1;
            if (!repaint && w2k_menu_foreign_event) w2k_menu_foreign_event(&e);
            break;

        case MotionNotify: {
            int rx = e.xmotion.x_root, ry = e.xmotion.y_root;
            int li = level_at(lv, n, rx, ry);
            if (li < 0) break;
            int idx = item_at(lv[li].m, w2k_lp(rx - lv[li].x), w2k_lp(ry - lv[li].y));
            /* Moving back into a shallower menu closes everything below it. */
            if (li < n - 1 && (idx < 0 || idx != lv[li].sel)) {
                for (int k = li + 1; k < n; k++) level_destroy(&lv[k]);
                n = li + 1;
                repaint = 1;
            }
            if (idx != lv[li].sel) {
                lv[li].sel = idx;
                repaint = 1;
                Item *it = (idx >= 0) ? &lv[li].m->items[idx] : NULL;
                if (it && it->sub && !it->disabled && n < MAXLEVEL) {
                    int iy = lv[li].y + w2k_px(item_y(lv[li].m, idx));
                    open_level(&lv[n], it->sub, 0, iy, 0,
                               lv[li].x + w2k_px(lv[li].m->ix[idx] + lv[li].m->col_w),
                               lv[li].x + w2k_px(lv[li].m->ix[idx]));
                    n++;
                }
            }
            break;
        }

        case ButtonPress: {
            int li = level_at(lv, n, e.xbutton.x_root, e.xbutton.y_root);
            if (li < 0) { done = 1; break; } /* click-away dismisses */

            /* Right-clicking an item hands it to the caller, which may put
             * a context menu of its own up. That menu takes the pointer
             * grab, so this one takes it back afterwards. */
            if (e.xbutton.button == Button3 && w2k_menu_on_context) {
                int idx = item_at(lv[li].m, w2k_lp(e.xbutton.x_root - lv[li].x),
                                  w2k_lp(e.xbutton.y_root - lv[li].y));
                if (idx < 0) break;
                Item *it = &lv[li].m->items[idx];
                if (it->disabled || it->sub || !it->id) break;
                int acted = w2k_menu_on_context(it->id, e.xbutton.x_root,
                                                e.xbutton.y_root);
                if (acted) { done = 1; break; }
                XGrabPointer(w2k.dpy, lv[0].win, True,
                             ButtonPressMask | ButtonReleaseMask |
                             PointerMotionMask, GrabModeAsync, GrabModeAsync,
                             None, w2k.cur_arrow, CurrentTime);
                XGrabKeyboard(w2k.dpy, lv[0].win, True, GrabModeAsync,
                              GrabModeAsync, CurrentTime);
                opened = w2k_now_ms();     /* ignore the stray release */
                repaint = 1;
            }
            break;
        }

        case ButtonRelease: {
            /* Ignore the release that belongs to the click which opened us. */
            if (w2k_now_ms() - opened < 250) break;
            int li = level_at(lv, n, e.xbutton.x_root, e.xbutton.y_root);
            if (li < 0) { done = 1; break; }
            int idx = item_at(lv[li].m, w2k_lp(e.xbutton.x_root - lv[li].x),
                              w2k_lp(e.xbutton.y_root - lv[li].y));
            if (idx < 0) break;
            Item *it = &lv[li].m->items[idx];
            if (it->disabled || it->sub) break;
            result = it->id;
            done = 1;
            break;
        }

        case KeyPress: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            Level *top = &lv[n - 1];
            if (ks == XK_Escape) {
                if (n > 1) { level_destroy(top); n--; repaint = 1; }
                else done = 1;
            } else if (ks == XK_Down || ks == XK_Up) {
                top->sel = next_selectable(top->m, top->sel < 0 ?
                                           (ks == XK_Down ? -1 : 0) : top->sel,
                                           ks == XK_Down ? 1 : -1);
                repaint = 1;
            } else if (ks == XK_Right) {
                Item *it = (top->sel >= 0) ? &top->m->items[top->sel] : NULL;
                if (it && it->sub && !it->disabled && n < MAXLEVEL) {
                    int iy = top->y + w2k_px(item_y(top->m, top->sel));
                    open_level(&lv[n], it->sub, 0, iy, 0, top->x + top->pw, top->x);
                    lv[n].sel = next_selectable(it->sub, -1, 1);
                    n++;
                    repaint = 1;
                }
            } else if (ks == XK_Left) {
                if (n > 1) { level_destroy(top); n--; repaint = 1; }
            } else if (ks == XK_Return || ks == XK_KP_Enter) {
                Item *it = (top->sel >= 0) ? &top->m->items[top->sel] : NULL;
                if (it && !it->disabled) {
                    if (it->sub && n < MAXLEVEL) {
                        int iy = top->y + w2k_px(item_y(top->m, top->sel));
                        open_level(&lv[n], it->sub, 0, iy, 0, top->x + top->pw, top->x);
                        lv[n].sel = next_selectable(it->sub, -1, 1);
                        n++;
                        repaint = 1;
                    } else { result = it->id; done = 1; }
                }
            } else if (w2k_menu_typeahead &&
                       match_mnemonic(top->m, ks) < 0 &&
                       XLookupString(&e.xkey, w2k_menu_typeahead, 2, NULL, NULL) == 1 &&
                       (unsigned char)w2k_menu_typeahead[0] >= ' ') {
                /* A printable key that is nobody's mnemonic: the caller
                 * wanted it (the Start menu turns it into a search), so
                 * close up and hand it over. */
                w2k_menu_typeahead[1] = 0;
                done = 1;
            } else {
                int idx = match_mnemonic(top->m, ks);
                if (idx >= 0) {
                    Item *it = &top->m->items[idx];
                    if (it->disabled) break;
                    top->sel = idx;
                    if (it->sub && n < MAXLEVEL) {
                        int iy = top->y + item_y(top->m, idx);
                        open_level(&lv[n], it->sub, 0, iy, 0, top->x + top->w, top->x);
                        lv[n].sel = next_selectable(it->sub, -1, 1);
                        n++;
                        repaint = 1;
                    } else { result = it->id; done = 1; }
                }
            }
            break;
        }

        default:
            if (w2k_menu_foreign_event) w2k_menu_foreign_event(&e);
            break;
        }
    }

    XUngrabKeyboard(w2k.dpy, CurrentTime);
    XUngrabPointer(w2k.dpy, CurrentTime);
    for (int i = 0; i < n; i++) level_destroy(&lv[i]);
    XFlush(w2k.dpy);
    if (w2k_menu_closed) w2k_menu_closed();
    if (result) w2k_sound_play(SND_MENUCOMMAND);
    return result;
}
