/* w2kdisplay -- Display Properties: Background, Appearance, Settings.
 *
 * Appearance edits the shared colour scheme (~/.w2k/scheme) and broadcasts
 * it, so every running w2k program recolours at once. Settings drives
 * XRandR through the xrandr command for resolution, position and primary. */
#include "w2kui.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * Appearance: the editable elements and the classic preset schemes
 * ------------------------------------------------------------------ */
/* `color2` is the second colour of a gradient title bar -- Windows 2000's
 * "Color 2" -- or -1 for the items that have one colour. */
static const struct { const char *label; int color, color2; } elements[] = {
    { "Desktop",                  C_DESKTOP,       -1 },
    { "Active Title Bar",         C_ACTIVETITLE,   C_ACTIVETITLE2 },
    { "Active Title Bar Text",    C_TITLETEXT,     -1 },
    { "Inactive Title Bar",       C_INACTIVETITLE, C_INACTIVETITLE2 },
    { "Inactive Title Bar Text",  C_INACTIVETITLETEXT, -1 },
    { "3D Objects",               C_FACE,          -1 },
    { "3D Objects Highlight",     C_HILIGHT, -1 },
    { "3D Objects Light",         C_LIGHT, -1 },
    { "3D Objects Shadow",        C_SHADOW, -1 },
    { "3D Objects Dark Shadow",   C_DKSHADOW, -1 },
    { "3D Objects Text",          C_TEXT, -1 },
    { "Disabled Text",            C_GRAYTEXT, -1 },
    { "Window",                   C_WINDOW, -1 },
    { "Window Text",              C_WINDOWTEXT, -1 },
    { "Menu",                     C_MENU, -1 },
    { "Menu Text",                C_MENUTEXT, -1 },
    { "Selected Items",           C_HIGHLIGHT, -1 },
    { "Selected Items Text",      C_HIGHLIGHTTEXT, -1 },
    { "Scrollbar",                C_SCROLLBAR, -1 },
    { "ToolTip",                  C_TOOLTIP, -1 },
    { "ToolTip Text",             C_TOOLTIPTEXT, -1 },
    { "Application Background",   C_APPWORKSPACE, -1 },
};
#define NELEM ((int)(sizeof elements / sizeof *elements))

/* Preset schemes. Each lists the colours that differ from Windows Standard.
 * The classic names are Windows 2000's; the values reproduce their look. */
typedef struct { int color; unsigned char r, g, b; } Tint;
/* `theme` names one of the shell's built-in looks -- those bring their own
 * whole colour table, and the taskbar, Start button and Start menu change
 * with them. The rest are the classic Windows 2000 tint schemes, which
 * only recolour the standard look. */
static const struct { const char *name; const Tint *t; int n; int theme; }
presets[] = {
#define S(...) (const Tint[]){ __VA_ARGS__ }
    { "Windows Standard", NULL, 0, THEME_CLASSIC },
    { "Windows XP", NULL, 0, THEME_XP },
    { "Windows 7 Basic", NULL, 0, THEME_BASIC7 },
    { "Windows Classic", S({C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_DKSHADOW,0,0,0},
        {C_ACTIVETITLE,0,0,128},{C_ACTIVETITLE2,16,132,208},{C_INACTIVETITLE,128,128,128},
        {C_INACTIVETITLE2,181,181,181},{C_INACTIVETITLETEXT,192,192,192},{C_MENU,192,192,192},
        {C_HIGHLIGHT,0,0,128},{C_DESKTOP,0,128,128},{C_SCROLLBAR,192,192,192}), 12 , -1 },
    { "Brick", S({C_ACTIVETITLE,128,0,0},{C_ACTIVETITLE2,192,96,96},{C_INACTIVETITLE,128,128,64},
        {C_INACTIVETITLE2,192,192,128},{C_HIGHLIGHT,128,0,0},{C_DESKTOP,0,128,128},
        {C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_MENU,192,192,192},{C_SCROLLBAR,192,192,192}), 10 , -1 },
    { "Desert", S({C_FACE,213,204,170},{C_LIGHT,235,228,200},{C_SHADOW,148,141,112},
        {C_ACTIVETITLE,0,128,128},{C_ACTIVETITLE2,64,192,192},{C_INACTIVETITLE,128,128,64},
        {C_INACTIVETITLE2,192,192,128},{C_MENU,213,204,170},{C_HIGHLIGHT,0,128,128},
        {C_DESKTOP,128,128,0},{C_SCROLLBAR,213,204,170},{C_INACTIVETITLETEXT,213,204,170}), 12 , -1 },
    { "Eggplant", S({C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_ACTIVETITLE,64,0,64},
        {C_ACTIVETITLE2,128,64,128},{C_INACTIVETITLE,128,128,128},{C_INACTIVETITLE2,181,181,181},
        {C_HIGHLIGHT,64,0,64},{C_DESKTOP,0,64,64},{C_MENU,192,192,192},{C_SCROLLBAR,192,192,192}), 10 , -1 },
    { "Lilac", S({C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_ACTIVETITLE,128,0,128},
        {C_ACTIVETITLE2,204,153,204},{C_INACTIVETITLE,128,128,128},{C_INACTIVETITLE2,181,181,181},
        {C_HIGHLIGHT,128,0,128},{C_DESKTOP,128,0,128},{C_MENU,192,192,192},{C_SCROLLBAR,192,192,192}), 10 , -1 },
    { "Maple", S({C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_ACTIVETITLE,128,64,0},
        {C_ACTIVETITLE2,204,153,102},{C_INACTIVETITLE,128,128,128},{C_INACTIVETITLE2,181,181,181},
        {C_HIGHLIGHT,128,64,0},{C_DESKTOP,64,32,0},{C_MENU,192,192,192},{C_SCROLLBAR,192,192,192}), 10 , -1 },
    { "Marine (high color)", S({C_ACTIVETITLE,0,64,128},{C_ACTIVETITLE2,64,160,224},
        {C_INACTIVETITLE,64,96,128},{C_INACTIVETITLE2,128,160,192},{C_HIGHLIGHT,0,64,128},
        {C_DESKTOP,0,64,96}), 6 , -1 },
    { "Plum (high color)", S({C_ACTIVETITLE,96,0,64},{C_ACTIVETITLE2,192,96,160},
        {C_INACTIVETITLE,128,96,128},{C_INACTIVETITLE2,192,160,192},{C_HIGHLIGHT,96,0,64},
        {C_DESKTOP,64,0,64}), 6 , -1 },
    { "Rainy Day", S({C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_ACTIVETITLE,0,64,128},
        {C_ACTIVETITLE2,128,160,192},{C_INACTIVETITLE,128,128,128},{C_INACTIVETITLE2,181,181,181},
        {C_HIGHLIGHT,0,64,128},{C_DESKTOP,64,64,128},{C_MENU,192,192,192},{C_SCROLLBAR,192,192,192}), 10 , -1 },
    { "Rose", S({C_FACE,207,175,183},{C_LIGHT,231,207,213},{C_SHADOW,143,119,127},
        {C_ACTIVETITLE,128,0,64},{C_ACTIVETITLE2,192,96,128},{C_INACTIVETITLE,128,64,96},
        {C_INACTIVETITLE2,192,128,160},{C_MENU,207,175,183},{C_HIGHLIGHT,128,0,64},
        {C_DESKTOP,128,64,96},{C_SCROLLBAR,207,175,183},{C_INACTIVETITLETEXT,207,175,183}), 12 , -1 },
    { "Slate", S({C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_ACTIVETITLE,64,96,128},
        {C_ACTIVETITLE2,128,160,192},{C_INACTIVETITLE,128,128,128},{C_INACTIVETITLE2,181,181,181},
        {C_HIGHLIGHT,64,96,128},{C_DESKTOP,64,96,128},{C_MENU,192,192,192},{C_SCROLLBAR,192,192,192}), 10 , -1 },
    { "Spruce", S({C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_ACTIVETITLE,0,64,32},
        {C_ACTIVETITLE2,64,128,96},{C_INACTIVETITLE,96,128,112},{C_INACTIVETITLE2,160,192,176},
        {C_HIGHLIGHT,0,64,32},{C_DESKTOP,0,64,32},{C_MENU,192,192,192},{C_SCROLLBAR,192,192,192}), 10 , -1 },
    { "Storm (VGA)", S({C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_ACTIVETITLE,128,0,128},
        {C_ACTIVETITLE2,128,0,128},{C_INACTIVETITLE,0,0,128},{C_INACTIVETITLE2,0,0,128},
        {C_HIGHLIGHT,128,0,128},{C_DESKTOP,0,0,128},{C_MENU,192,192,192},{C_SCROLLBAR,192,192,192}), 10 , -1 },
    { "Teal (VGA)", S({C_FACE,192,192,192},{C_LIGHT,223,223,223},{C_ACTIVETITLE,0,128,128},
        {C_ACTIVETITLE2,0,128,128},{C_INACTIVETITLE,128,128,128},{C_INACTIVETITLE2,128,128,128},
        {C_HIGHLIGHT,0,128,128},{C_DESKTOP,0,128,128},{C_MENU,192,192,192},{C_SCROLLBAR,192,192,192}), 10 , -1 },
    { "Wheat", S({C_FACE,213,204,170},{C_LIGHT,235,228,200},{C_SHADOW,148,141,112},
        {C_ACTIVETITLE,128,64,0},{C_ACTIVETITLE2,192,128,64},{C_INACTIVETITLE,128,128,64},
        {C_INACTIVETITLE2,192,192,128},{C_MENU,213,204,170},{C_HIGHLIGHT,128,64,0},
        {C_DESKTOP,128,128,64},{C_SCROLLBAR,213,204,170},{C_INACTIVETITLETEXT,213,204,170}), 12 , -1 },
#undef S
};
#define NPRESET ((int)(sizeof presets / sizeof *presets))
static int matching_preset(void);

/* ------------------------------------------------------------------ *
 * Settings: monitors via xrandr
 * ------------------------------------------------------------------ */
typedef struct {
    char name[64];
    int  x, y, w, h;                /* geometry as xrandr reports it now */
    int  primary, connected, enabled;
    char modes[64][16];
    int  nmodes, cur_mode;
    /* pending edits, applied on OK / Apply */
    int  mode_sel, want_primary, want_enabled;
    int  px, py;                    /* pending position, dragged by hand */
} Monitor;

static Monitor mons[8];
static int nmons;

static void read_monitors(void)
{
    nmons = 0;
    FILE *p = popen("xrandr --query 2>/dev/null", "r");
    if (!p) return;
    char line[512];
    Monitor *m = NULL;
    while (fgets(line, sizeof line, p)) {
        if (line[0] != ' ') {
            m = NULL;
            char name[64], state[32];
            if (sscanf(line, "%63s %31s", name, state) < 2) continue;
            if (strcmp(state, "connected")) continue;
            if (nmons >= 8) continue;
            m = &mons[nmons++];
            memset(m, 0, sizeof *m);
            snprintf(m->name, sizeof m->name, "%s", name);
            m->connected = 1;
            m->primary = strstr(line, " primary ") != NULL;
            m->enabled = 1;
            const char *g = strstr(line, "connected");
            int w, h, x, y;
            /* "... connected [primary] WxH+X+Y ..." */
            const char *q = g;
            while (q && *q && sscanf(q, "%dx%d+%d+%d", &w, &h, &x, &y) != 4) q++;
            if (q && *q) { m->w = w; m->h = h; m->x = x; m->y = y; }
            m->cur_mode = -1;
        } else if (m && m->nmodes < 64) {
            char mode[16];
            if (sscanf(line, " %15s", mode) != 1 || !strchr(mode, 'x')) continue;
            snprintf(m->modes[m->nmodes], 16, "%s", mode);
            if (strchr(line, '*')) m->cur_mode = m->nmodes;
            m->nmodes++;
        }
    }
    pclose(p);
    /* Number them the way they are arranged, not the order xrandr happens to
     * enumerate connectors in: "1" should be the leftmost screen. */
    for (int i = 1; i < nmons; i++)
        for (int k = i; k > 0; k--) {
            if (mons[k - 1].x < mons[k].x ||
                (mons[k - 1].x == mons[k].x && mons[k - 1].y <= mons[k].y)) break;
            Monitor t = mons[k - 1];
            mons[k - 1] = mons[k];
            mons[k] = t;
        }

    for (int i = 0; i < nmons; i++) {
        Monitor *m = &mons[i];
        /* A connected output with no geometry is switched off. */
        if (!m->w || !m->h) m->enabled = 0;
        m->mode_sel = m->cur_mode < 0 ? 0 : m->cur_mode;
        m->want_primary = m->primary;
        m->want_enabled = m->enabled;
        m->px = m->x;
        m->py = m->y;
    }
}

/* Size of a monitor as it *would* be with its pending mode selected --
 * the layout has to show the new size before Apply, or dragging to line up
 * two screens is guesswork. */
static void pending_size(const Monitor *m, int *w, int *h)
{
    int mw = 0, mh = 0;
    if (m->nmodes && m->mode_sel >= 0 && m->mode_sel < m->nmodes)
        sscanf(m->modes[m->mode_sel], "%dx%d", &mw, &mh);
    if (mw <= 0 || mh <= 0) { mw = m->w; mh = m->h; }
    if (mw <= 0 || mh <= 0) { mw = 1024; mh = 768; }
    *w = mw;
    *h = mh;
}

/* xrandr will not take negative positions, so slide the whole arrangement
 * back to the origin after a drag. */
static void normalise_positions(void)
{
    int minx = 1 << 30, miny = 1 << 30;
    for (int i = 0; i < nmons; i++) {
        if (!mons[i].want_enabled) continue;
        if (mons[i].px < minx) minx = mons[i].px;
        if (mons[i].py < miny) miny = mons[i].py;
    }
    if (minx == 1 << 30) return;
    for (int i = 0; i < nmons; i++) { mons[i].px -= minx; mons[i].py -= miny; }
}

/* Pull a dragged monitor's edges onto its neighbours', the way the Windows
 * display applet does -- otherwise a one-pixel gap or overlap is almost
 * impossible to avoid by hand, and both make a mess of the desktop. */
#define SNAP 60
static void snap_monitor(int idx)
{
    Monitor *m = &mons[idx];
    int mw, mh;
    pending_size(m, &mw, &mh);
    int bestdx = 1 << 30, bestdy = 1 << 30;

    for (int i = 0; i < nmons; i++) {
        if (i == idx || !mons[i].want_enabled) continue;
        int ow, oh;
        pending_size(&mons[i], &ow, &oh);
        int ox = mons[i].px, oy = mons[i].py;

        /* Horizontal: left-to-right, right-to-left, or aligned edges. */
        int cand_x[4] = { ox + ow - m->px, ox - mw - m->px,
                          ox - m->px, ox + ow - mw - m->px };
        for (int k = 0; k < 4; k++)
            if (abs(cand_x[k]) < abs(bestdx) && abs(cand_x[k]) <= SNAP)
                bestdx = cand_x[k];

        int cand_y[4] = { oy + oh - m->py, oy - mh - m->py,
                          oy - m->py, oy + oh - mh - m->py };
        for (int k = 0; k < 4; k++)
            if (abs(cand_y[k]) < abs(bestdy) && abs(cand_y[k]) <= SNAP)
                bestdy = cand_y[k];
    }
    if (bestdx != 1 << 30) m->px += bestdx;
    if (bestdy != 1 << 30) m->py += bestdy;
    normalise_positions();
}

/* One xrandr invocation for the whole arrangement: mode, absolute position
 * and primary for every output at once. Doing it output by output would put
 * the desktop through invalid intermediate layouts (two screens briefly on
 * top of each other, or no primary at all). */
static void apply_monitors(void)
{
    char cmd[2048] = "xrandr";
    normalise_positions();
    for (int i = 0; i < nmons; i++) {
        Monitor *m = &mons[i];
        char part[512];
        if (!m->want_enabled) {
            snprintf(part, sizeof part, " --output %.63s --off", m->name);
            strncat(cmd, part, sizeof cmd - strlen(cmd) - 1);
            continue;
        }
        snprintf(part, sizeof part, " --output %.63s --mode %.15s --pos %dx%d%s",
                 m->name, m->nmodes ? m->modes[m->mode_sel] : "auto",
                 m->px, m->py, m->want_primary ? " --primary" : "");
        strncat(cmd, part, sizeof cmd - strlen(cmd) - 1);
    }
    strncat(cmd, " 2>&1", sizeof cmd - strlen(cmd) - 1);
    FILE *p = popen(cmd, "r");
    char out[1024] = "";
    if (p) { size_t n = fread(out, 1, sizeof out - 1, p); out[n] = 0; pclose(p); }
    if (out[0]) {
        char msg[1200];
        snprintf(msg, sizeof msg, "xrandr reported:\n\n%s", out);
        w2k_msgbox(NULL, "Display Properties", msg, MB_OK | MB_ICONWARNING);
    }
    read_monitors();
}

/* ------------------------------------------------------------------ *
 * The dialog
 * ------------------------------------------------------------------ */
typedef struct {
    W2kWin   *win;
    W2kTabs  *tabs;
    W2kRect   ok, cancel, apply;
    int       down;
    int       dirty;                        /* something to Apply */

    /* Background */
    W2kList  *walls;
    W2kCombo *style;
    W2kRect   browse;
    char      wallpath[1024];

    /* Appearance */
    W2kCombo *scheme, *item;
    W2kEdit  *red, *green, *blue;
    W2kRect   swatch, swatch2;              /* Color and Color 2 buttons */
    int       cur_elem;
    int       cur_col;                      /* 0: Color, 1: Color 2 */
    int       suppress;                     /* while filling edits */

    /* Settings */
    W2kCombo *mon, *mode;
    W2kRect   primary_box, enabled_box, layout_box;
    W2kRect   decorate_box;                 /* Appearance page */
    W2kCombo *iconset;                      /* which system's icons */
    char      sets[16][32];
    int       nsets;
    /* Drag state for the monitor arrangement. */
    int       drag_mon;                     /* -1 when not dragging   */
    int       drag_dx, drag_dy;             /* grab offset, screen px */
    double    layout_scale;                 /* screen px -> layout px */
    int       layout_ox, layout_oy;
    int       layout_minx, layout_miny;
} Dlg;

static Dlg dl;

/* The scheme colour the edits and the palette act on. */
static int cur_color(void)
{
    int c2 = elements[dl.cur_elem].color2;
    return dl.cur_col && c2 >= 0 ? c2 : elements[dl.cur_elem].color;
}

static void fill_color_edits(void)
{
    int r, g, b;
    w2k_color_rgb(cur_color(), &r, &g, &b);
    char t[8];
    dl.suppress = 1;
    snprintf(t, sizeof t, "%d", r); w2k_edit_set(dl.red, t);
    snprintf(t, sizeof t, "%d", g); w2k_edit_set(dl.green, t);
    snprintf(t, sizeof t, "%d", b); w2k_edit_set(dl.blue, t);
    dl.suppress = 0;
}

static void color_edited(void *u)
{
    (void)u;
    if (dl.suppress) return;
    int r = atoi(w2k_edit_text(dl.red)), g = atoi(w2k_edit_text(dl.green));
    int b = atoi(w2k_edit_text(dl.blue));
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    w2k_color_set(cur_color(), r, g, b);
    XSetWindowBackground(w2k.dpy, dl.win->win, w2k.col[C_FACE]);
    dl.scheme->sel = matching_preset();
    dl.dirty = 1;
    w2k_win_dirty(dl.win);
}

static void on_item(void *u, int i)
{
    (void)u;
    dl.cur_elem = i;
    dl.cur_col = 0;
    fill_color_edits();
    w2k_win_dirty(dl.win);
}

/* A colour button: the colour in a raised button with a drop arrow, as
 * the Windows dialog draws Color and Color 2. */
static void draw_color_button(Drawable d, const W2kRect *r, int color, int selected)
{
    int cr, cg, cb;
    w2k_color_rgb(color, &cr, &cg, &cb);
    w2k_button(d, r->x, r->y, r->w, r->h, 0);
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(cr, cg, cb));
    XFillRectangle(w2k.dpy, d, w2k.gc, r->x + 4, r->y + 4, (unsigned)(r->w - 24),
                   (unsigned)(r->h - 8));
    w2k_frame(d, r->x + 4, r->y + 4, r->w - 24, r->h - 8, C_DKSHADOW);
    int ax = r->x + r->w - 14, ay = r->y + r->h / 2 - 1;
    w2k_hline(d, ax, ay, 7, C_TEXT);
    w2k_hline(d, ax + 1, ay + 1, 5, C_TEXT);
    w2k_hline(d, ax + 2, ay + 2, 3, C_TEXT);
    w2k_hline(d, ax + 3, ay + 3, 1, C_TEXT);
    if (selected) w2k_focus_rect(d, r->x + 2, r->y + 2, r->w - 4, r->h - 4);
}

/* Color / Color 2 clicked: that colour becomes the one the edits show,
 * and the palette drops down under the button. */
static void pick_color(int which)
{
    dl.cur_col = which;
    fill_color_edits();
    const W2kRect *r = which ? &dl.swatch2 : &dl.swatch;
    int rx, ry;
    Window dummy;
    XTranslateCoordinates(w2k.dpy, dl.win->win, w2k.root, r->x, r->y + r->h,
                          &rx, &ry, &dummy);
    w2k_win_dirty(dl.win);
    w2k_win_repaint_now(dl.win);
    int cr, cg, cb;
    w2k_color_rgb(cur_color(), &cr, &cg, &cb);
    if (w2k_color_popup(rx, ry, &cr, &cg, &cb)) {
        w2k_color_set(cur_color(), cr, cg, cb);
        XSetWindowBackground(w2k.dpy, dl.win->win, w2k.col[C_FACE]);
        fill_color_edits();
        dl.scheme->sel = matching_preset();
        dl.dirty = 1;
    }
    w2k_win_dirty(dl.win);
}

/* The icon set changes at once in this window (the row of samples), and
 * everywhere else when Apply writes the scheme. */
static void on_iconset(void *u, int i)
{
    (void)u;
    if (i < 0 || i >= dl.nsets) return;
    snprintf(w2k_icon_set, sizeof w2k_icon_set, "%s", dl.sets[i]);
    w2k_icon_load_default();
    dl.dirty = 1;
    w2k_win_dirty(dl.win);
}

/* The preset the current colours are, or -1 (the box shows nothing, as
 * the Windows dialog does for colours that are nobody's scheme). A theme
 * preset matches by theme; a tint preset by every colour it sets, with
 * the rest at Windows Standard. */
static int matching_preset(void)
{
    for (int i = 0; i < NPRESET; i++) {
        if (presets[i].theme >= 0) {
            if (w2k_theme == presets[i].theme && presets[i].theme != THEME_CLASSIC)
                return i;
            if (presets[i].theme != THEME_CLASSIC) continue;
        }
        if (w2k_theme != THEME_CLASSIC) continue;
        /* Every colour must be what this preset would set it to. */
        int ok = 1;
        for (int c = 0; c < N_COLORS && ok; c++) {
            if (c == C_BLACK || c == C_WHITE) continue;
            unsigned char want[3];
            w2k_theme_colour(THEME_CLASSIC, c, want);
            for (int k = 0; k < presets[i].n; k++)
                if (presets[i].t[k].color == c) {
                    want[0] = presets[i].t[k].r;
                    want[1] = presets[i].t[k].g;
                    want[2] = presets[i].t[k].b;
                }
            int r, g, b;
            w2k_color_rgb(c, &r, &g, &b);
            if (r != want[0] || g != want[1] || b != want[2]) ok = 0;
        }
        if (ok) return i;
    }
    return -1;
}

static void on_scheme(void *u, int i)
{
    (void)u;
    char wp[1024];
    snprintf(wp, sizeof wp, "%s", w2k_wallpaper);
    int st = w2k_wallpaper_style;
    /* A theme preset replaces the whole look; a tint preset recolours the
     * classic one, so pick that theme back up first. Only the colour table
     * is reset: w2k_scheme_reset() would take the effects, the taskbar,
     * the folder options and the monitor arrangement back to their
     * defaults too, and Apply would then write those defaults over what
     * Performance Options and the other applets had saved. */
    w2k_theme = presets[i].theme >= 0 ? presets[i].theme : THEME_CLASSIC;
    w2k_theme_colours(w2k_theme);
    snprintf(w2k_wallpaper, sizeof w2k_wallpaper, "%s", wp);
    w2k_wallpaper_style = st;
    for (int k = 0; k < presets[i].n; k++)
        w2k_color_set(presets[i].t[k].color, presets[i].t[k].r,
                      presets[i].t[k].g, presets[i].t[k].b);
    XSetWindowBackground(w2k.dpy, dl.win->win, w2k.col[C_FACE]);
    fill_color_edits();
    dl.dirty = 1;
    w2k_win_dirty(dl.win);
}

static void on_style(void *u, int i) { (void)u; w2k_wallpaper_style = i; dl.dirty = 1; w2k_win_dirty(dl.win); }

static void on_wall(void *u, int i)
{
    (void)u;
    if (i < 0) return;
    const char *t = dl.walls->items[i].text[0];
    if (!strcmp(t, "(None)")) w2k_wallpaper[0] = 0;
    else snprintf(w2k_wallpaper, sizeof w2k_wallpaper, "%s",
                  (const char *)dl.walls->items[i].data);
    dl.dirty = 1;
    w2k_win_dirty(dl.win);
}

/* The user's Pictures folder: $XDG_PICTURES_DIR, the entry for it in
 * ~/.config/user-dirs.dirs, or ~/Pictures. */
static void pictures_dir(char *out, int n)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/";
    const char *env = getenv("XDG_PICTURES_DIR");
    if (env && env[0]) { snprintf(out, (size_t)n, "%s", env); return; }
    snprintf(out, (size_t)n, "%s/Pictures", home);
    char path[1200];
    snprintf(path, sizeof path, "%s/.config/user-dirs.dirs", home);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "XDG_PICTURES_DIR=", 17)) continue;
        char *v = line + 17;
        v[strcspn(v, "\r\n")] = 0;
        if (*v == '"') { v++; char *q = strrchr(v, '"'); if (q) *q = 0; }
        if (!strncmp(v, "$HOME/", 6)) snprintf(out, (size_t)n, "%.500s/%.500s", home, v + 6);
        else if (*v == '/')           snprintf(out, (size_t)n, "%.1000s", v);
        break;
    }
    fclose(f);
}

static int is_picture(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".png") ||
           !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg");
}

/* The picture itself, scaled into the little monitor the way the desktop
 * will show it -- centred, tiled, stretched, fitted, filled or spanned --
 * as the real dialog does. The monitor stands in for the primary screen
 * (or, for Span, the whole desktop) at the same shape. Built once per
 * picture and style and kept. */
static void wallpaper_preview(Drawable d, int x, int y, int w, int h)
{
    static Pixmap cache;
    static char cache_path[1024];
    static int cache_style = -1, cache_w, cache_h;
    if (cache && !strcmp(cache_path, w2k_wallpaper) && cache_style == w2k_wallpaper_style &&
        cache_w == w && cache_h == h) {
        XCopyArea(w2k.dpy, cache, d, w2k_copy_gc(), 0, 0, (unsigned)w, (unsigned)h, x, y);
        return;
    }
    int iw, ih;
    unsigned char *rgba = w2k_image_load(w2k_wallpaper, &iw, &ih);
    if (!rgba || iw <= 0 || ih <= 0) { free(rgba); w2k_fill(d, x, y, w, h, C_DESKTOP); return; }
    if (cache) w2k_free_pixmap(cache);
    cache = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)w, (unsigned)h, w2k.depth);
    snprintf(cache_path, sizeof cache_path, "%s", w2k_wallpaper);
    cache_style = w2k_wallpaper_style; cache_w = w; cache_h = h;

    /* The screen the preview stands for, so the picture keeps its scale. */
    const W2kMonitor *m = w2k_monitor_primary();
    int SW = w2k_wallpaper_style == 5 ? w2k.sw : m->w;
    int SH = w2k_wallpaper_style == 5 ? w2k.sh : m->h;
    if (SW <= 0 || SH <= 0) { SW = 1024; SH = 768; }
    int st = w2k_wallpaper_style;
    long fx = 0, ox = 0, oy = 0;
    if (st == 3 || st == 4) {
        long sw = ((long)iw << 16) / SW, shh = ((long)ih << 16) / SH;
        fx = st == 3 ? (sw > shh ? sw : shh) : (sw < shh ? sw : shh);
        ox = (((long)iw << 16) - fx * SW) / 2;
        oy = (((long)ih << 16) - fx * SH) / 2;
    }
    char *pixels = malloc((size_t)w * h * 4);
    XImage *im = pixels ? XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap, 0,
                                       pixels, (unsigned)w, (unsigned)h, 32, 0) : NULL;
    if (!im) { free(pixels); free(rgba); return; }
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            int X = px * SW / w, Y = py * SH / h, sx, sy;
            if (st == 2 || st == 5) { sx = X * iw / SW; sy = Y * ih / SH; }
            else if (st == 3 || st == 4) { sx = (int)((fx * X + ox) >> 16); sy = (int)((fx * Y + oy) >> 16); }
            else if (st == 1) { sx = X % iw; sy = Y % ih; }
            else { sx = X - (SW - iw) / 2; sy = Y - (SH - ih) / 2; }
            unsigned long c;
            if (sx < 0 || sy < 0 || sx >= iw || sy >= ih) c = w2k.col[C_DESKTOP];
            else {
                const unsigned char *p = rgba + ((size_t)sy * iw + sx) * 4;
                c = w2k_rgb(p[0], p[1], p[2]);
            }
            XPutPixel(im, px, py, c);
        }
    XPutImage(w2k.dpy, cache, w2k_copy_gc(), im, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    XDestroyImage(im);
    free(rgba);
    XCopyArea(w2k.dpy, cache, d, w2k_copy_gc(), 0, 0, (unsigned)w, (unsigned)h, x, y);
}

static void fill_walls(void)
{
    w2k_list_clear(dl.walls);
    int r = w2k_list_add(dl.walls, ICO_NONE, NULL);
    w2k_list_set(dl.walls, r, 0, "(None)");
    if (!w2k_wallpaper[0]) { dl.walls->sel = r; dl.walls->items[r].selected = 1; }

    /* The Pictures folder first -- that is where wallpapers live -- then
     * the shell's own. */
    const char *home = getenv("HOME");
    char dirs[3][1024];
    int nd = 0;
    pictures_dir(dirs[nd++], 1024);
    if (home) snprintf(dirs[nd++], 1024, "%s/.w2k/wallpapers", home);
    snprintf(dirs[nd++], 1024, W2K_PREFIX "/share/w2k/wallpapers");
    for (int d = 0; d < nd; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (de->d_name[0] == '.' || !is_picture(de->d_name)) continue;
            char *full = w2k_alloc(2048);
            int len = snprintf(full, 2048, "%s/%s", dirs[d], de->d_name);
            if (len >= 2048) { free(full); continue; }
            int k = w2k_list_add(dl.walls, ICO_PAINT, full);
            char label[300];
            const char *dot = strrchr(de->d_name, '.');
            snprintf(label, sizeof label, "%.*s", (int)(dot - de->d_name), de->d_name);
            w2k_list_set(dl.walls, k, 0, label);
            if (!strcmp(full, w2k_wallpaper)) { dl.walls->sel = k; dl.walls->items[k].selected = 1; }
        }
        closedir(dp);
    }
    if (w2k_wallpaper[0] && dl.walls->sel <= 0) {
        char *full = w2k_strdup(w2k_wallpaper);
        int k = w2k_list_add(dl.walls, ICO_PAINT, full);
        const char *b = strrchr(full, '/');
        w2k_list_set(dl.walls, k, 0, b ? b + 1 : full);
        dl.walls->sel = k;
        dl.walls->items[k].selected = 1;
    }
}

/* The list of modes belongs to whichever monitor is selected. */
static void fill_mode_combo(void)
{
    w2k_combo_clear(dl.mode);
    int cur = dl.mon->sel;
    if (cur < 0 || cur >= nmons) return;
    for (int k = 0; k < mons[cur].nmodes; k++)
        w2k_combo_add(dl.mode, mons[cur].modes[k]);
    dl.mode->sel = mons[cur].mode_sel;
}

/* Rebuilding a combo clears its selection, so the caller's choice has to be
 * carried across -- and this list is rebuilt whenever a label changes, which
 * includes the moment the user picks a different monitor. */
static void fill_monitor_combos(void)
{
    int keep = dl.mon->sel;
    w2k_combo_clear(dl.mon);
    for (int i = 0; i < nmons; i++) {
        char t[96];
        snprintf(t, sizeof t, "%d. %.63s%s", i + 1, mons[i].name,
                 mons[i].want_primary ? " (primary)" : "");
        w2k_combo_add(dl.mon, t);
    }
    if (!nmons) w2k_combo_add(dl.mon, "(no monitors found - is xrandr installed?)");
    if (keep < 0 || keep >= nmons) keep = nmons ? 0 : -1;
    dl.mon->sel = keep;
    fill_mode_combo();
}

static void on_mon(void *u, int i)
{
    (void)u;
    if (i >= 0 && i < nmons) dl.mon->sel = i;
    fill_mode_combo();
    w2k_win_dirty(dl.win);
}

static void on_mode(void *u, int i)
{
    (void)u;
    if (dl.mon->sel < 0 || dl.mon->sel >= nmons) return;
    mons[dl.mon->sel].mode_sel = i;
    /* The box in the layout changes size with the mode; keep the screens
     * touching rather than leaving a hole where the old size was. */
    snap_monitor(dl.mon->sel);
    dl.dirty = 1;
    w2k_win_dirty(dl.win);
}

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */
/* A little monitor with the desktop inside it, for the Background tab. */
static void draw_monitor_preview(Drawable d, int x, int y, int w, int h)
{
    w2k_fill(d, x, y, w, h, C_FACE);
    w2k_edge(d, x, y, w, h, EDGE_RAISED, BF_RECT);
    w2k_edge(d, x + 8, y + 8, w - 16, h - 30, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, x + 10, y + 10, w - 20, h - 34, C_DESKTOP);
    if (w2k_wallpaper[0])
        wallpaper_preview(d, x + 10, y + 10, w - 20, h - 34);
    w2k_fill(d, x + w / 2 - 12, y + h - 22, 24, 8, C_SHADOW);
    w2k_fill(d, x + w / 2 - 30, y + h - 14, 60, 6, C_SHADOW);
}

/* Minimize, Maximize and Close, right-aligned at `right`, with their
 * glyphs -- the shell's own, as the frames draw them. */
static void caption_buttons(Drawable d, int right, int y)
{
    int cx = right - 16, mx = cx - 2 - 16, mn = mx - 16;
    w2k_button(d, mn, y, 16, 14, 0);
    w2k_capglyph_min(d, mn, y, C_TEXT);
    w2k_button(d, mx, y, 16, 14, 0);
    w2k_capglyph_max(d, mx, y, C_TEXT);
    w2k_button(d, cx, y, 16, 14, 0);
    w2k_capglyph_close(d, cx, y, C_TEXT);
}

/* The Appearance preview: inactive window, active window, message box. */
static void draw_appearance_preview(Drawable d, W2kRect r)
{
    w2k_fill(d, r.x, r.y, r.w, r.h, C_DESKTOP);
    w2k_edge(d, r.x, r.y, r.w, r.h, EDGE_SUNKEN, BF_RECT);
    int fh = w2k_font_height(F_UI);

    /* inactive */
    int ix = r.x + 12, iy = r.y + 10, iw = r.w - 70, ih = 60;
    w2k_fill(d, ix, iy, iw, ih, C_FACE);
    w2k_edge(d, ix, iy, iw, ih, EDGE_RAISED, BF_RECT);
    w2k_gradient(d, ix + 4, iy + 4, iw - 8, 18, C_INACTIVETITLE, C_INACTIVETITLE2);
    w2k_text(d, F_UI_BOLD, ix + 8, iy + 6, "Inactive Window", C_INACTIVETITLETEXT);
    caption_buttons(d, ix + iw - 6, iy + 6);

    /* active */
    int ax = r.x + 30, ay = r.y + 36, aw = r.w - 60, ah = r.h - 60;
    w2k_fill(d, ax, ay, aw, ah, C_FACE);
    w2k_edge(d, ax, ay, aw, ah, EDGE_RAISED, BF_RECT);
    w2k_gradient(d, ax + 4, ay + 4, aw - 8, 18, C_ACTIVETITLE, C_ACTIVETITLE2);
    w2k_text(d, F_UI_BOLD, ax + 8, ay + 6, "Active Window", C_TITLETEXT);
    caption_buttons(d, ax + aw - 6, ay + 6);
    /* menu bar */
    w2k_fill(d, ax + 4, ay + 22, aw - 8, 19, C_MENU);
    w2k_text(d, F_UI, ax + 10, ay + 25, "Normal", C_MENUTEXT);
    w2k_text_disabled(d, F_UI, ax + 60, ay + 25, "Disabled");
    w2k_fill(d, ax + 118, ay + 23, 60, 17, C_HIGHLIGHT);
    w2k_text(d, F_UI, ax + 124, ay + 25, "Selected", C_HIGHLIGHTTEXT);
    /* client */
    int cx = ax + 4, cy = ay + 42, cw = aw - 8, ch = ah - 46;
    w2k_edge(d, cx, cy, cw, ch, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, cx + 2, cy + 2, cw - 4, ch - 4, C_WINDOW);
    w2k_text(d, F_UI, cx + 6, cy + 6, "Window Text", C_WINDOWTEXT);
    w2k_dither(d, cx + cw - 18, cy + 2, 16, ch - 4, C_HILIGHT, C_SCROLLBAR);
    w2k_button(d, cx + cw - 18, cy + 2, 16, 16, 0);
    w2k_button(d, cx + cw - 18, cy + ch - 18, 16, 16, 0);

    /* message box */
    int mx = r.x + r.w / 2 - 10, my = r.y + r.h - 78, mw = r.w / 2 - 6, mh = 66;
    w2k_fill(d, mx, my, mw, mh, C_FACE);
    w2k_edge(d, mx, my, mw, mh, EDGE_RAISED, BF_RECT);
    w2k_gradient(d, mx + 3, my + 3, mw - 6, 18, C_ACTIVETITLE, C_ACTIVETITLE2);
    w2k_text(d, F_UI_BOLD, mx + 7, my + 5, "Message Box", C_TITLETEXT);
    w2k_button(d, mx + mw - 5 - 16, my + 5, 16, 14, 0);
    w2k_capglyph_close(d, mx + mw - 5 - 16, my + 5, C_TEXT);
    w2k_text(d, F_UI, mx + 10, my + 26, "Message Text", C_TEXT);
    W2kRect ok = { mx + mw / 2 - 30, my + mh - 26, 60, 20 };
    w2k_draw_pushbutton(d, &ok, "OK", BS_DEFAULT);
    (void)fh;
}

/* ------------------------------------------------------------------ *
 * The monitor arrangement: numbered boxes the user drags around
 * ------------------------------------------------------------------ */

/* Recompute the screen-pixels-to-layout-pixels mapping. Kept in dl so that
 * hit-testing and dragging use exactly the mapping that was drawn. */
static void layout_metrics(W2kRect r)
{
    /* Recomputing the origin and scale mid-drag would move every box under
     * the pointer as the bounding rectangle grows -- the arrangement has to
     * hold still while one monitor is being dragged across it. */
    if (dl.drag_mon >= 0 && dl.layout_scale > 0) return;

    int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
    for (int i = 0; i < nmons; i++) {
        if (!mons[i].want_enabled) continue;
        int w, h;
        pending_size(&mons[i], &w, &h);
        if (mons[i].px < minx) minx = mons[i].px;
        if (mons[i].py < miny) miny = mons[i].py;
        if (mons[i].px + w > maxx) maxx = mons[i].px + w;
        if (mons[i].py + h > maxy) maxy = mons[i].py + h;
    }
    if (minx == 1 << 30) { dl.layout_scale = 0; return; }

    /* Leave room around the arrangement so a monitor can be dragged clear
     * of its neighbours instead of being pinned against the frame. */
    int tw = maxx - minx, th = maxy - miny;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    double sc = (double)(r.w - 40) / tw;
    if ((double)(r.h - 30) / th < sc) sc = (double)(r.h - 30) / th;
    if (sc <= 0) sc = 0.01;

    dl.layout_scale = sc;
    dl.layout_minx = minx;
    dl.layout_miny = miny;
    dl.layout_ox = r.x + (r.w - (int)(tw * sc)) / 2;
    dl.layout_oy = r.y + (r.h - (int)(th * sc)) / 2;
}

/* Box for monitor i in window coordinates. */
static W2kRect mon_box(int i)
{
    int w, h;
    pending_size(&mons[i], &w, &h);
    return (W2kRect){
        dl.layout_ox + (int)((mons[i].px - dl.layout_minx) * dl.layout_scale),
        dl.layout_oy + (int)((mons[i].py - dl.layout_miny) * dl.layout_scale),
        (int)(w * dl.layout_scale), (int)(h * dl.layout_scale)
    };
}

static void draw_monitor_layout(Drawable d, W2kRect r)
{
    dl.layout_box = r;
    w2k_fill(d, r.x, r.y, r.w, r.h, C_APPWORKSPACE);
    w2k_edge(d, r.x, r.y, r.w, r.h, EDGE_SUNKEN, BF_RECT);
    if (!nmons) return;
    layout_metrics(r);
    if (dl.layout_scale <= 0) return;

    for (int i = 0; i < nmons; i++) {
        if (!mons[i].want_enabled) continue;
        W2kRect b = mon_box(i);
        int sel = (i == dl.mon->sel);

        /* A monitor is drawn as a screen: raised bezel, coloured glass. */
        w2k_fill(d, b.x, b.y, b.w, b.h, C_FACE);
        w2k_edge(d, b.x, b.y, b.w, b.h, EDGE_RAISED, BF_RECT);
        if (b.w > 8 && b.h > 8)
            w2k_fill(d, b.x + 3, b.y + 3, b.w - 6, b.h - 6,
                     sel ? C_HIGHLIGHT : C_DESKTOP);

        /* The primary monitor wears the taskbar, so it is obvious which
         * screen the Start button will end up on. */
        if (mons[i].want_primary && b.w > 10 && b.h > 14) {
            w2k_fill(d, b.x + 3, b.y + b.h - 8, b.w - 6, 5, C_FACE);
            w2k_hline(d, b.x + 3, b.y + b.h - 8, b.w - 6, C_HILIGHT);
        }

        char n[8];
        snprintf(n, sizeof n, "%d", i + 1);
        int tw = w2k_text_width(F_UI_BOLD, n, -1);
        w2k_text(d, F_UI_BOLD, b.x + (b.w - tw) / 2,
                 b.y + (b.h - w2k_font_height(F_UI_BOLD)) / 2 - 2, n,
                 C_HIGHLIGHTTEXT);
    }
}

/* Which monitor is under this window-relative point? Topmost first, so a
 * box drawn over another can still be grabbed. */
static int mon_at(int x, int y)
{
    if (dl.layout_scale <= 0) return -1;
    for (int i = nmons - 1; i >= 0; i--) {
        if (!mons[i].want_enabled) continue;
        W2kRect b = mon_box(i);
        if (w2k_rect_hit(&b, x, y)) return i;
    }
    return -1;
}

/* Layout pixels back to screen pixels. */
static int to_screen(int layout_px) { return (int)(layout_px / dl.layout_scale); }

static void paint(W2kWin *w, Drawable d)
{
    w2k_tabs_draw(d, dl.tabs);
    W2kRect c = w2k_tabs_client(dl.tabs);
    int fh = w2k_font_height(F_UI);

    switch (dl.tabs->sel) {
    case 0:
        draw_monitor_preview(d, c.x + (c.w - 180) / 2, c.y + 10, 180, 150);
        w2k_text_mnemonic(d, F_UI, c.x + 10, c.y + 172,
                          "Select a background picture or HTML document as &Wallpaper:",
                          C_TEXT, 1);
        w2k_list_draw(d, dl.walls);
        w2k_draw_pushbutton(d, &dl.browse, "&Browse...", dl.down == 4 ? BS_PRESSED : 0);
        w2k_text_mnemonic(d, F_UI, dl.style->r.x - 110, dl.style->r.y + (21 - fh) / 2,
                          "&Picture Display:", C_TEXT, 1);
        w2k_combo_draw(d, dl.style);
        break;
    case 1: {
        W2kRect pv = { c.x + 10, c.y + 10, c.w - 20, 170 };
        draw_appearance_preview(d, pv);
        w2k_text_mnemonic(d, F_UI, c.x + 10, dl.scheme->r.y - fh - 3, "&Scheme:", C_TEXT, 1);
        w2k_combo_draw(d, dl.scheme);
        w2k_text_mnemonic(d, F_UI, c.x + 10, dl.item->r.y - fh - 3, "&Item:", C_TEXT, 1);
        w2k_combo_draw(d, dl.item);
        w2k_text_mnemonic(d, F_UI, c.x + 10, dl.iconset->r.y - fh - 3, "Ic&ons:", C_TEXT, 1);
        w2k_combo_draw(d, dl.iconset);
        /* A few of the set's icons beside the box, so the choice can be seen. */
        int ix = dl.iconset->r.x + dl.iconset->r.w + 10, iy = dl.iconset->r.y + 2;
        static const int show[] = { ICO_MYCOMPUTER, ICO_FOLDER, ICO_RECYCLE, ICO_FILE_TEXT, ICO_CONTROLPANEL };
        for (int k = 0; k < 5; k++) w2k_icon_draw(d, ix + k * 22, iy, show[k]);
        w2k_text(d, F_UI, dl.red->r.x, dl.red->r.y - fh - 3, "Red:", C_TEXT);
        w2k_text(d, F_UI, dl.green->r.x, dl.green->r.y - fh - 3, "Green:", C_TEXT);
        w2k_text(d, F_UI, dl.blue->r.x, dl.blue->r.y - fh - 3, "Blue:", C_TEXT);
        w2k_edit_draw(d, dl.red);
        w2k_edit_draw(d, dl.green);
        w2k_edit_draw(d, dl.blue);
        w2k_text_mnemonic(d, F_UI, dl.swatch.x, dl.swatch.y - fh - 3, "&Color:", C_TEXT, 1);
        draw_color_button(d, &dl.swatch, elements[dl.cur_elem].color, dl.cur_col == 0);
        if (elements[dl.cur_elem].color2 >= 0) {
            w2k_text_mnemonic(d, F_UI, dl.swatch2.x, dl.swatch2.y - fh - 3, "Color &2:", C_TEXT, 1);
            draw_color_button(d, &dl.swatch2, elements[dl.cur_elem].color2, dl.cur_col == 1);
        }
        w2k_edge(d, dl.swatch.x, dl.swatch.y, dl.swatch.w, dl.swatch.h, EDGE_SUNKEN, BF_RECT);
        w2k_draw_checkbox(d, dl.decorate_box.x, dl.decorate_box.y,
                          "&Title bar and border on windows that ask for none",
                          w2k_force_decorations, 0, 0);
        w2k_text(d, F_UI, c.x + 10, c.y + c.h - fh - 6,
                 "Colours apply to every open window when you click Apply.", C_GRAYTEXT);
        break;
    }
    case 2: {
        W2kRect lay = { c.x + 10, c.y + 10, c.w - 20, 150 };
        draw_monitor_layout(d, lay);
        w2k_text(d, F_UI, c.x + 10, c.y + 166,
                 "Drag the monitor icons to match the physical arrangement.",
                 C_GRAYTEXT);
        w2k_text_mnemonic(d, F_UI, c.x + 10, dl.mon->r.y + (21 - fh) / 2, "&Display:", C_TEXT, 1);
        w2k_combo_draw(d, dl.mon);
        w2k_text_mnemonic(d, F_UI, c.x + 10, dl.mode->r.y + (21 - fh) / 2, "Screen &area:", C_TEXT, 1);
        w2k_combo_draw(d, dl.mode);

        int cur = dl.mon->sel;
        int valid = cur >= 0 && cur < nmons;
        w2k_draw_checkbox(d, dl.enabled_box.x, dl.enabled_box.y,
                          "&Extend my Windows desktop onto this monitor",
                          valid && mons[cur].want_enabled, 0, !valid);
        /* The last screen standing cannot be switched off, and the primary
         * flag has nowhere to go on a disabled output. */
        w2k_draw_checkbox(d, dl.primary_box.x, dl.primary_box.y,
                          "Use this device as the &primary monitor",
                          valid && mons[cur].want_primary, 0,
                          !valid || !mons[cur].want_enabled);
        if (valid) {
            char info[160];
            int mw, mh;
            pending_size(&mons[cur], &mw, &mh);
            snprintf(info, sizeof info, "%s -- %d x %d at %d, %d",
                     mons[cur].name, mw, mh, mons[cur].px, mons[cur].py);
            w2k_text(d, F_UI, c.x + 10, c.y + c.h - fh - 6, info, C_GRAYTEXT);
        }
        break;
    }
    }
    w2k_draw_pushbutton(d, &dl.ok, "OK", BS_DEFAULT | (dl.down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &dl.cancel, "Cancel", dl.down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &dl.apply, "&Apply",
                        (dl.dirty ? 0 : BS_DISABLED) | (dl.down == 3 ? BS_PRESSED : 0));
}

/* ------------------------------------------------------------------ *
 * Actions
 * ------------------------------------------------------------------ */
/* What the Settings tab is about to apply, into the scheme's record of the
 * arrangement, so the next session starts with it. */
static void record_monitors(void)
{
    normalise_positions();
    w2k_monitor_cfg_n = 0;
    for (int i = 0; i < nmons && w2k_monitor_cfg_n < 8; i++) {
        const Monitor *m = &mons[i];
        W2kMonitorCfg *c = &w2k_monitor_cfg[w2k_monitor_cfg_n++];
        memset(c, 0, sizeof *c);
        snprintf(c->name, sizeof c->name, "%.63s", m->name);
        if (m->nmodes) snprintf(c->mode, sizeof c->mode, "%s", m->modes[m->mode_sel]);
        c->x = m->px;
        c->y = m->py;
        c->primary = m->want_primary;
        c->enabled = m->want_enabled;
    }
}

static void do_apply(void)
{
    int monitors = dl.tabs->sel == 2 && nmons;
    if (monitors) record_monitors();
    w2k_scheme_save(NULL);
    w2k_scheme_broadcast();
    if (monitors) apply_monitors();
    dl.dirty = 0;
    w2k_win_dirty(dl.win);
}

static void do_cancel(void)
{
    w2k_scheme_load(NULL);          /* discard unapplied edits locally */
    w2k_win_close(dl.win, ID_CANCEL);
}

static void blink_cb(void *v) { w2k_edit_blink(v); }

static int event(W2kWin *w, XEvent *e)
{
    int tab = dl.tabs->sel;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_tabs_press(dl.tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (tab == 0) {
            if (w2k_list_press(dl.walls, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_combo_press(dl.style, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_rect_hit(&dl.browse, x, y)) dl.down = 4;
        } else if (tab == 1) {
            if (w2k_combo_press(dl.scheme, &e->xbutton) ||
                w2k_combo_press(dl.item, &e->xbutton) ||
                w2k_combo_press(dl.iconset, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_rect_hit(&dl.decorate_box, x, y)) {
                w2k_force_decorations = !w2k_force_decorations;
                dl.dirty = 1;
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_rect_hit(&dl.swatch, x, y)) { pick_color(0); return 1; }
            if (elements[dl.cur_elem].color2 >= 0 && w2k_rect_hit(&dl.swatch2, x, y)) {
                pick_color(1);
                return 1;
            }
            W2kEdit *eds[] = { dl.red, dl.green, dl.blue };
            for (int i = 0; i < 3; i++)
                if (w2k_edit_press(eds[i], &e->xbutton)) {
                    for (int k = 0; k < 3; k++) if (k != i) eds[k]->focused = 0;
                    w2k_win_dirty(w);
                    return 1;
                }
        } else {
            if (w2k_combo_press(dl.mon, &e->xbutton) ||
                w2k_combo_press(dl.mode, &e->xbutton)) {
                w2k_win_dirty(w);
                return 1;
            }
            int cur = dl.mon->sel;
            if (w2k_rect_hit(&dl.primary_box, x, y) && cur >= 0 && cur < nmons &&
                mons[cur].want_enabled) {
                /* Exactly one primary, always. */
                for (int i = 0; i < nmons; i++) mons[i].want_primary = (i == cur);
                fill_monitor_combos();      /* the "(primary)" label moved */
                dl.dirty = 1;
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_rect_hit(&dl.enabled_box, x, y) && cur >= 0 && cur < nmons) {
                int on = !mons[cur].want_enabled;
                int others = 0;
                for (int i = 0; i < nmons; i++)
                    if (i != cur && mons[i].want_enabled) others++;
                if (on || others) {          /* never switch off the last one */
                    mons[cur].want_enabled = on;
                    if (!on && mons[cur].want_primary) {
                        mons[cur].want_primary = 0;
                        for (int i = 0; i < nmons; i++)
                            if (mons[i].want_enabled) { mons[i].want_primary = 1; break; }
                    }
                    if (on) snap_monitor(cur);
                    dl.dirty = 1;
                }
                w2k_win_dirty(w);
                return 1;
            }
            /* Grab a monitor out of the arrangement. */
            if (w2k_rect_hit(&dl.layout_box, x, y)) {
                int i = mon_at(x, y);
                if (i >= 0) {
                    dl.mon->sel = i;
                    fill_mode_combo();
                    W2kRect b = mon_box(i);
                    dl.drag_mon = i;
                    dl.drag_dx = x - b.x;
                    dl.drag_dy = y - b.y;
                }
                w2k_win_dirty(w);
                return 1;
            }
        }
        if (w2k_rect_hit(&dl.ok, x, y)) dl.down = 1;
        else if (w2k_rect_hit(&dl.cancel, x, y)) dl.down = 2;
        else if (w2k_rect_hit(&dl.apply, x, y) && dl.dirty) dl.down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (tab == 2 && dl.drag_mon >= 0) {
            /* Follow the pointer in screen pixels, so a drag across the box
             * moves the monitor by the distance it looks like it moved. */
            int nx = e->xmotion.x - dl.drag_dx, ny = e->xmotion.y - dl.drag_dy;
            int bw, bh;
            pending_size(&mons[dl.drag_mon], &bw, &bh);
            int pw = (int)(bw * dl.layout_scale), ph = (int)(bh * dl.layout_scale);
            int lo_x = dl.layout_box.x + 3, lo_y = dl.layout_box.y + 3;
            int hi_x = dl.layout_box.x + dl.layout_box.w - 3 - pw;
            int hi_y = dl.layout_box.y + dl.layout_box.h - 3 - ph;
            if (nx < lo_x) nx = lo_x;
            if (nx > hi_x) nx = hi_x;
            if (ny < lo_y) ny = lo_y;
            if (ny > hi_y) ny = hi_y;
            mons[dl.drag_mon].px = dl.layout_minx +
                to_screen(nx - dl.layout_ox);
            mons[dl.drag_mon].py = dl.layout_miny +
                to_screen(ny - dl.layout_oy);
            dl.dirty = 1;
            w2k_win_dirty(w);
            return 1;
        }
        if (tab == 0 && w2k_list_motion(dl.walls, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        if (tab == 1 && (w2k_edit_motion(dl.red, &e->xmotion) || w2k_edit_motion(dl.green, &e->xmotion) ||
                         w2k_edit_motion(dl.blue, &e->xmotion))) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        if (dl.drag_mon >= 0) {
            snap_monitor(dl.drag_mon);
            dl.drag_mon = -1;
            dl.dirty = 1;
            dl.down = 0;
            w2k_win_dirty(w);
            return 1;
        }
        w2k_list_release(dl.walls, &e->xbutton);
        w2k_edit_release(dl.red); w2k_edit_release(dl.green); w2k_edit_release(dl.blue);
        int d = dl.down, x = e->xbutton.x, y = e->xbutton.y;
        dl.down = 0;
        if (d == 1 && w2k_rect_hit(&dl.ok, x, y)) { do_apply(); w2k_win_close(w, ID_OK); }
        else if (d == 2 && w2k_rect_hit(&dl.cancel, x, y)) do_cancel();
        else if (d == 3 && w2k_rect_hit(&dl.apply, x, y)) do_apply();
        else if (d == 4 && w2k_rect_hit(&dl.browse, x, y)) {
            char p[1024];
            if (w2k_wallpaper[0]) snprintf(p, sizeof p, "%s", w2k_wallpaper);
            else                  pictures_dir(p, sizeof p);
            if (w2k_file_dialog_filter(w, 0, p, sizeof p,
                                       "Pictures (*.bmp;*.png;*.jpg)|*.bmp;*.png;*.jpg;*.jpeg;*.gif|All Files (*.*)|*")) {
                snprintf(w2k_wallpaper, sizeof w2k_wallpaper, "%s", p);
                fill_walls();
                dl.dirty = 1;
            }
        }
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { do_cancel(); return 1; }
        if (w2k_tabs_key(dl.tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { do_apply(); w2k_win_close(w, ID_OK); return 1; }
        if (tab == 1) {
            W2kEdit *eds[] = { dl.red, dl.green, dl.blue };
            if (ks == XK_Tab) {
                int f = dl.red->focused ? 0 : dl.green->focused ? 1 : dl.blue->focused ? 2 : -1;
                for (int k = 0; k < 3; k++) eds[k]->focused = 0;
                eds[(f + 1 + 3) % 3]->focused = 1;
                w2k_win_dirty(w);
                return 1;
            }
            for (int k = 0; k < 3; k++)
                if (eds[k]->focused && w2k_edit_key(eds[k], &e->xkey)) { w2k_win_dirty(w); return 1; }
        }
        if (tab == 0 && w2k_list_key(dl.walls, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void on_tab(void *u, int i) { (void)u; (void)i; w2k_win_dirty(dl.win); }

int main(void)
{
    if (w2k_init("w2kdisplay") < 0) return 1;
    int W = 420, H = 486;
    dl.win = w2k_win_new("Display Properties", "w2kdisplay", W, H, 0);
    dl.win->paint = paint;
    dl.win->event = event;

    dl.tabs = w2k_tabs_new(NULL, on_tab);
    w2k_tabs_add(dl.tabs, "Background");
    w2k_tabs_add(dl.tabs, "Appearance");
    w2k_tabs_add(dl.tabs, "Settings");
    dl.tabs->r = (W2kRect){ 6, 6, W - 12, H - 6 - 12 - 23 - 12 };
    W2kRect c = w2k_tabs_client(dl.tabs);
    int by = H - 12 - 23;
    dl.apply  = (W2kRect){ W - 12 - 75, by, 75, 23 };
    dl.cancel = (W2kRect){ W - 12 - 75 * 2 - 6, by, 75, 23 };
    dl.ok     = (W2kRect){ W - 12 - 75 * 3 - 12, by, 75, 23 };

    /* Background */
    dl.walls = w2k_list_new(LV_LIST);
    dl.walls->r = (W2kRect){ c.x + 10, c.y + 190, c.w - 20 - 90, 110 };
    dl.walls->on_select = on_wall;
    dl.walls->focused = 1;
    w2k_scroll_bind(&dl.walls->hsb, dl.win);
    dl.browse = (W2kRect){ c.x + c.w - 10 - 80, c.y + 190, 80, 23 };
    dl.style = w2k_combo_new(0);
    w2k_combo_add(dl.style, "Center");
    w2k_combo_add(dl.style, "Tile");
    w2k_combo_add(dl.style, "Stretch");
    w2k_combo_add(dl.style, "Fit");
    w2k_combo_add(dl.style, "Fill");
    w2k_combo_add(dl.style, "Span");
    dl.style->sel = w2k_wallpaper_style;
    dl.style->on_change = on_style;
    dl.style->r = (W2kRect){ c.x + c.w - 10 - 130, c.y + 310, 130, 21 };
    fill_walls();

    /* Appearance */
    dl.scheme = w2k_combo_new(0);
    for (int i = 0; i < NPRESET; i++) w2k_combo_add(dl.scheme, presets[i].name);
    dl.scheme->on_change = on_scheme;
    dl.scheme->sel = matching_preset();

    dl.scheme->r = (W2kRect){ c.x + 10, c.y + 206, 190, 21 };
    dl.item = w2k_combo_new(0);
    for (int i = 0; i < NELEM; i++) w2k_combo_add(dl.item, elements[i].label);
    dl.item->on_change = on_item;
    dl.item->r = (W2kRect){ c.x + 10, c.y + 252, 190, 21 };
    /* Development aid: W2K_RENDER_ITEM=n renders with that item selected. */
    if (getenv("W2K_RENDER_ITEM")) {
        int it = atoi(getenv("W2K_RENDER_ITEM"));
        if (it >= 0 && it < NELEM) dl.cur_elem = it;
    }
    dl.item->sel = dl.cur_elem;
    int ex = c.x + 214;
    dl.red   = w2k_edit_new(0); dl.red->r   = (W2kRect){ ex, c.y + 252, 44, 21 };
    dl.green = w2k_edit_new(0); dl.green->r = (W2kRect){ ex + 50, c.y + 252, 44, 21 };
    dl.blue  = w2k_edit_new(0); dl.blue->r  = (W2kRect){ ex + 100, c.y + 252, 44, 21 };
    W2kEdit *eds[] = { dl.red, dl.green, dl.blue };
    for (int k = 0; k < 3; k++) {
        w2k_edit_bind(eds[k], dl.win);
        eds[k]->on_change = color_edited;
        w2k_add_timer(w2k_caret_blink, blink_cb, eds[k]);
    }
    dl.swatch  = (W2kRect){ ex, c.y + 206, 66, 21 };
    dl.swatch2 = (W2kRect){ ex + 78, c.y + 206, 66, 21 };
    dl.iconset = w2k_combo_new(0);
    dl.nsets = w2k_icon_sets(dl.sets, 16);
    for (int i = 0; i < dl.nsets; i++) {
        w2k_combo_add(dl.iconset, w2k_icon_set_label(dl.sets[i]));
        if (!strcmp(dl.sets[i], w2k_icon_set)) dl.iconset->sel = i;
    }
    dl.iconset->on_change = on_iconset;
    dl.iconset->r = (W2kRect){ c.x + 10, c.y + 298, 190, 21 };
    dl.decorate_box = (W2kRect){ c.x + 10, c.y + 330, c.w - 20, 16 };
    fill_color_edits();

    /* Settings */
    read_monitors();
    dl.drag_mon = -1;
    dl.mon = w2k_combo_new(0);  dl.mon->on_change = on_mon;
    dl.mode = w2k_combo_new(0); dl.mode->on_change = on_mode;
    dl.mon->r  = (W2kRect){ c.x + 100, c.y + 186, c.w - 110, 21 };
    dl.mode->r = (W2kRect){ c.x + 100, c.y + 216, c.w - 110, 21 };
    dl.enabled_box = (W2kRect){ c.x + 10, c.y + 250, c.w - 20, 16 };
    dl.primary_box = (W2kRect){ c.x + 10, c.y + 272, c.w - 20, 16 };
    fill_monitor_combos();

    w2k_win_center(dl.win, NULL);
    w2k_win_show(dl.win);
    w2k_run();
    w2k_fini();
    return 0;
}
