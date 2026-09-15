/* startpanel.c -- the two-column Start menu.
 *
 * Windows XP replaced the single column of Windows 2000 with a panel: a
 * header naming the user, the programs you actually use down the left,
 * the places you keep things down the right, and Log Off / Turn Off in a
 * footer. Windows 7 kept the shape and changed the paint. This is that
 * panel, and the option that turns it on is in Taskbar and Start Menu
 * properties beside the classic one.
 *
 * It is not a W2kMenu: two columns with different backgrounds, a header
 * and a footer are not what that control is for. So this is a window of
 * its own with a pointer grab and its own small event loop, in the same
 * shape as lib/menu.c -- and the items carry the same command ids as the
 * classic menu, so both styles run one dispatch (startmenu_dispatch).
 *
 * Colours come from the theme's table where one fits, and from the two
 * screenshots the panel is modelled on where nothing in the classic
 * palette applies (the pale blue of XP's right column, for instance).
 */
#include "wm.h"
#include "w2kui.h"
#include <X11/extensions/shape.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Windows 7's panel is measured off the 1024x768 screenshot of it open
 * over the taskbar: 411 by 476 at (0,252), with the user's picture in a
 * tile standing 26 rows proud of the top edge. The panel itself is a
 * skin with its text painted out; the rows below say where text goes. */
#define P7_W          411
#define P7_H          480
#define P7_OVER        20     /* the user tile stands this far above the panel */
#define P7_LEFT_X       9     /* the white pane: x 9..258, rows 9..470 */
#define P7_LEFT_W     250
#define P7_LEFT_TOP     9
#define P7_LEFT_BOT   471
#define P7_LEFT_Y      15     /* first row */
#define P7_ROW_H       38     /* 32-pixel icons at 12, text at 51 */
#define P7_SEP_H        7
#define P7_AP_Y       395     /* All Programs bar: rows 395..426 */
#define P7_AP_H        32
#define P7_SEARCH_X    19     /* the search box, inside the white column: */
#define P7_SEARCH_Y   438     /* x 19..242, rows 438..458 */
#define P7_SEARCH_W   224
#define P7_SEARCH_H    21
#define P7_RIGHT_X    261     /* the blue pane: x 261..408 */
#define P7_RIGHT_W    148
#define P7_RIGHT_Y     43
#define P7_RROW_H      35
#define P7_RSEP_H       8
#define P7_SHUT_X     265     /* Shut down, the button proper ... */
#define P7_SHUT_W      73
#define P7_ARROW_W     24     /* ... and the arrow beside it */
#define P7_SHUT_Y     435
#define P7_SHUT_H      21
#define P7_TILE_X     307     /* the tile: 59 by 53, measured */
#define P7_TILE_W      59
#define P7_TILE_H      53

/* Windows Vista's panel, measured off its screenshot: the same 411
 * columns, 474 rows, the white pane in the same place, the tile inside
 * the top edge rather than above it, the right column from row 72 with
 * three-row ruled gaps between its groups, and a strip of three buttons
 * -- power, lock, arrow -- at the bottom right. */
#define PV_H          474
#define PV_LEFT_TOP     7
#define PV_LEFT_BOT   464
#define PV_LEFT_Y       8
#define PV_AP_Y       388
#define PV_AP_H        32
#define PV_SEARCH_Y   432
#define PV_SEARCH_H    22
#define PV_RIGHT_Y     72
#define PV_RROW_H      35
#define PV_RSEP_H       3
#define PV_TILE_X     307
#define PV_TILE_Y       4
#define PV_TILE_W      59
#define PV_TILE_H      59
#define PV_BTN_X      266
#define PV_BTN_Y      431
#define PV_BTN_W      132
#define PV_BTN_H       25

/* What differs between the two panels that share this layout. */
typedef struct {
    int over, h;                 /* rows above the panel; the panel's height */
    int left_top, left_bot, left_y;
    int ap_y, ap_h;
    int search_y, search_h;
    int right_y, rrow_h, rsep_h;
    int btn_x, btn_y, btn_w, btn_h;   /* the button strip at the bottom right */
    int ncell;
    struct { int row, w; } cell[3];   /* its cells, left to right: col-2 row, width */
} PanelMetrics;

static const PanelMetrics metrics7 = {
    P7_OVER, P7_H, P7_LEFT_TOP, P7_LEFT_BOT, P7_LEFT_Y, P7_AP_Y, P7_AP_H,
    P7_SEARCH_Y, P7_SEARCH_H, P7_RIGHT_Y, P7_RROW_H, P7_RSEP_H,
    P7_SHUT_X, P7_SHUT_Y, P7_SHUT_W + P7_ARROW_W, P7_SHUT_H,
    2, { { 1, P7_SHUT_W }, { 0, P7_ARROW_W }, { 0, 0 } }
};
/* Aero: the same panel, its tile taller and standing higher. */
#define PA_OVER        25
#define PA_TILE_W      58
#define PA_TILE_H      57
static const PanelMetrics metricsa = {
    PA_OVER, P7_H, P7_LEFT_TOP, P7_LEFT_BOT, P7_LEFT_Y, P7_AP_Y, P7_AP_H,
    P7_SEARCH_Y, P7_SEARCH_H, P7_RIGHT_Y, P7_RROW_H, P7_RSEP_H,
    P7_SHUT_X, P7_SHUT_Y, P7_SHUT_W + P7_ARROW_W, P7_SHUT_H,
    2, { { 1, P7_SHUT_W }, { 0, P7_ARROW_W }, { 0, 0 } }
};
static const PanelMetrics metricsv = {
    0, PV_H, PV_LEFT_TOP, PV_LEFT_BOT, PV_LEFT_Y, PV_AP_Y, PV_AP_H,
    PV_SEARCH_Y, PV_SEARCH_H, PV_RIGHT_Y, PV_RROW_H, PV_RSEP_H,
    PV_BTN_X, PV_BTN_Y, PV_BTN_W, PV_BTN_H,
    3, { { 1, 58 }, { 2, 52 }, { 0, 22 } }
};

/* Windows XP's panel, measured off its screenshot: 380 by 478, a 68-row
 * header (the last four rows are the orange rule), a 42-row footer, the
 * white column 2..189, the divider at 190 and the pale blue column from
 * 191, all cropped as strips. */
#define XP_W        380
#define XP_HEADER    68
#define XP_FOOTER    42
#define XP_LEFT_W   190
#define XP_BODY_MIN 368      /* the reference body: the panel does not shrink */

#define PANEL_W     (W2K_THEME_IS7(w2k_theme) ? P7_W : 380)
#define HEADER_H     XP_HEADER
#define FOOTER_H     XP_FOOTER
#define LEFT_W       XP_LEFT_W
#define ROW_H        40          /* left column: 32px icons, 40-pixel rows */
#define RROW_H       32          /* right column: 16px icons */
#define SEP_H         8
#define ALLPROG_H    30
#define MAXROWS      24

enum { R_ITEM, R_SEP, R_SUB, R_TITLE };

typedef struct {
    int  kind;
    int  id;
    int  icon;
    int  big;                    /* draw the 32px icon (left column top) */
    int  bold;                   /* XP sets its first group in bold */
    char label[96];
} Row;

static int seven(void) { return W2K_THEME_IS7(w2k_theme); }   /* 7 or Vista */
static int vista(void) { return w2k_theme == THEME_VISTA; }
static int aero(void)  { return w2k_theme == THEME_AERO; }    /* 7 in glass */

static Row  left_rows[MAXROWS], right_rows[MAXROWS];
static int  nleft, nright;
static int  panel_x, panel_y, panel_h;
static int  hot_col = -1, hot_row = -1;   /* what the pointer is over */
static Window panel;
volatile int startpanel_cancel;           /* startmenu_close(), from outside the loop */
static SearchState ss;                    /* typing searches, inside the panel */
static int    searching;
static int    left_skinned;               /* XP's white column, from its skins */
static void   draw_search(Drawable d);

/* ------------------------------------------------------------------ *
 * Building the two columns
 * ------------------------------------------------------------------ */
static Row *push(Row *rows, int *n, int kind, int id, const char *label,
                 int icon, int big)
{
    if (*n >= MAXROWS) return NULL;
    Row *r = &rows[(*n)++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->id = id;
    r->icon = icon;
    r->big = big;
    if (label) snprintf(r->label, sizeof r->label, "%.95s", label);
    return r;
}

/* The name Control Panel > User Accounts set, or the passwd entry's. */
static const char *user_display_name(void)
{
    return w2k_account_name();
}

static void build_rows(void)
{
    nleft = nright = 0;

    /* Left: what is pinned, then what is used most -- the two halves of
     * the XP left column, separated by a rule. */
    Pin pins[PIN_MAX];
    int npins = pins_load(PIN_START, pins, PIN_MAX);
    for (int i = 0; i < npins && i < 6; i++)
        push(left_rows, &nleft, R_ITEM, SM_PIN_BASE + i, pins[i].label,
             pin_icon(&pins[i]), 1);

    static const struct { const char *label, *cmd; int id, icon; } own[] = {
        { "Windows Explorer", "l2kexplorer", SM_EXPLORER, ICO_EXPLORER },
        { "Notepad",          "l2knotepad",  SM_NOTEPAD,  ICO_NOTEPAD  },
        { "Calculator",       "l2kcalc",     SM_CALC,     ICO_CALC     },
        { "Task Manager",     "l2ktaskmgr",  SM_TASKMGR,  ICO_TASKMGR  },
        { "Command Prompt",   NULL,          SM_TERMINAL, ICO_TERMINAL },
        { "Snipping Tool",    "l2ksnip",     SM_SNIP,     ICO_SNIP     },
    };
    if (npins) push(left_rows, &nleft, R_SEP, 0, NULL, ICO_NONE, 0);
    for (int i = 0; i < (int)(sizeof own / sizeof *own); i++)
        push(left_rows, &nleft, R_ITEM, own[i].id, own[i].label, own[i].icon, 1);

    /* Right: the places, in the order Windows lists them. Windows 7
     * heads the column with the user's own folder and drops the "My". */
    if (seven()) {
        push(right_rows, &nright, R_ITEM, SM_MYDOCS, user_display_name(),
             ICO_MYDOCS, 0);
        push(right_rows, &nright, R_SUB, SM_RECENTSUB, "Recent Items",
             ICO_DOCUMENTS, 0);
        push(right_rows, &nright, R_ITEM, SM_MYPICS, "Pictures",
             ICO_FILE_BITMAP, 0);
        push(right_rows, &nright, R_ITEM, SM_MYMUSIC, "Music",
             ICO_FILE_MEDIA, 0);
        push(right_rows, &nright, R_SEP, 0, NULL, ICO_NONE, 0);
        push(right_rows, &nright, R_ITEM, SM_MYCOMPUTER, "Computer",
             ICO_MYCOMPUTER, 0);
        push(right_rows, &nright, R_SEP, 0, NULL, ICO_NONE, 0);
        push(right_rows, &nright, R_ITEM, SM_CONTROLPANEL, "Control Panel",
             ICO_CONTROLPANEL, 0);
        push(right_rows, &nright, R_ITEM, SM_DEFAULTS, "Default Programs",
             ICO_PROGRAMS, 0);
        push(right_rows, &nright, R_ITEM, SM_HELP, "Help and Support",
             ICO_HELP, 0);
        push(right_rows, &nright, R_ITEM, SM_SEARCH, "Search", ICO_SEARCH, 0);
        push(right_rows, &nright, R_ITEM, SM_RUN, "Run...", ICO_RUN, 0);
        return;
    }
    Row *r;
    if ((r = push(right_rows, &nright, R_ITEM, SM_MYDOCS, "My Documents", ICO_MYDOCS, 0))) r->bold = 1;
    if ((r = push(right_rows, &nright, R_ITEM, SM_MYPICS, "My Pictures", ICO_FILE_BITMAP, 0))) r->bold = 1;
    if ((r = push(right_rows, &nright, R_ITEM, SM_MYMUSIC, "My Music", ICO_FILE_MEDIA, 0))) r->bold = 1;
    if ((r = push(right_rows, &nright, R_ITEM, SM_MYCOMPUTER, "My Computer", ICO_MYCOMPUTER, 0))) r->bold = 1;
    push(right_rows, &nright, R_SUB, SM_RECENTSUB, "My Recent &Documents",
         ICO_DOCUMENTS, 0);
    push(right_rows, &nright, R_SEP, 0, NULL, ICO_NONE, 0);
    push(right_rows, &nright, R_ITEM, SM_CONTROLPANEL, "&Control Panel",
         ICO_CONTROLPANEL, 0);
    push(right_rows, &nright, R_ITEM, SM_DEFAULTS,
         "Set Program &Access and Defaults", ICO_PROGRAMS, 0);
    push(right_rows, &nright, R_SEP, 0, NULL, ICO_NONE, 0);
    push(right_rows, &nright, R_ITEM, SM_HELP, "&Help and Support", ICO_HELP, 0);
    push(right_rows, &nright, R_ITEM, SM_SEARCH, "&Search", ICO_SEARCH, 0);
    push(right_rows, &nright, R_ITEM, SM_RUN, "&Run...", ICO_RUN, 0);
}

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */
static int rows_height(const Row *rows, int n, int rh)
{
    int h = 0;
    for (int i = 0; i < n; i++) h += rows[i].kind == R_SEP ? SEP_H : rh;
    return h;
}

static int row_at(const Row *rows, int n, int rh, int y0, int y)
{
    int cy = y0;
    for (int i = 0; i < n; i++) {
        int rhh = rows[i].kind == R_SEP ? SEP_H : rh;
        if (y >= cy && y < cy + rhh) return rows[i].kind == R_SEP ? -1 : i;
        cy += rhh;
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */
/* The panel's colours, as RGB. XP's pale blue right column and blue
 * header are its own; over any other look they come from the scheme:
 * the face for the right column, the window colour for the left, and a
 * header and footer in the title colours -- or, in the Modern look (whose
 * shadow is the face colour on purpose, so rules drawn in it vanished),
 * a flat band of its hover grey with rules in its border colour. The
 * text on the header and footer is whatever reads on them: white on a
 * blue title, dark on Modern Light's near-white band (the footer's
 * "Log Off" and "Turn Off Computer" used to be white there, and so
 * invisible). */
typedef struct { int right[3], rule[3], hdr1[3], hdr2[3], text[3]; } PanelColours;

static int lum(const int c[3]) { return (c[0] * 299 + c[1] * 587 + c[2] * 114) / 1000; }
static void set3(int d[3], int r, int g, int b) { d[0] = r; d[1] = g; d[2] = b; }
static void from_scheme(int d[3], int color)
{
    const unsigned char *c = w2k_scheme_rgb(color);
    set3(d, c[0], c[1], c[2]);
}

static void panel_colours(PanelColours *pc)
{
    if (w2k_theme == THEME_XP) {
        set3(pc->right, 211, 229, 250);
        set3(pc->rule, 180, 205, 240);
        set3(pc->hdr1, 0, 83, 225);
        set3(pc->hdr2, 61, 149, 255);
        set3(pc->text, 255, 255, 255);
        return;
    }
    if (w2k_theme == THEME_BASIC7) {
        set3(pc->right, 240, 240, 240);
        set3(pc->rule, 203, 203, 203);
        set3(pc->hdr1, 60, 66, 74);
        set3(pc->hdr2, 32, 36, 42);
        set3(pc->text, 255, 255, 255);
        return;
    }
    from_scheme(pc->right, C_FACE);
    if (w2k_theme == THEME_MODERN) {
        w2k_modern_rgb(MODERN_BORDER, pc->rule);
        w2k_modern_rgb(MODERN_HOT, pc->hdr1);
        memcpy(pc->hdr2, pc->hdr1, sizeof pc->hdr2);
        from_scheme(pc->text, C_TEXT);
    } else {
        from_scheme(pc->rule, C_SHADOW);
        from_scheme(pc->hdr1, C_ACTIVETITLE);
        from_scheme(pc->hdr2, C_ACTIVETITLE2);
        from_scheme(pc->text, C_TITLETEXT);
    }
    /* A title text that does not stand out from its bar (a tint scheme
     * with a pale title) gives way to black or white, whichever reads. */
    int hl = (lum(pc->hdr1) + lum(pc->hdr2)) / 2, tl = lum(pc->text);
    if (tl - hl < 90 && hl - tl < 90) {
        if (hl > 128) set3(pc->text, 0, 0, 0);
        else          set3(pc->text, 255, 255, 255);
    }
}

static unsigned long px3(const int c[3]) { return w2k_rgb(c[0], c[1], c[2]); }

static void fill(Drawable d, int x, int y, int w, int h, unsigned long px)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, px);
    w2k_fill_fg(d, x, y, w, h);
}

/* The panel is measured in logical pixels and drawn through the
 * primitives, which scale it; the few direct X calls map their own. */
static void rect_fg(Drawable d, int x, int y, int w, int h)
{
    w2k_frame_fg(d, x, y, w, h);
}

/* From colour a on the left to b on the right: one fill per run of equal
 * colour (a flat band is one fill). The colours are RGB already -- this
 * used to ask the server for them, twice a gradient, at every hover. */
static void hgradient(Drawable d, int x, int y, int w, int h, const int a[3], const int b[3])
{
    int px = w2k_cx(x), py = w2k_cx(y), ph = w2k_cw(y, h);
    w = w2k_cw(x, w);
    int run = 0;
    unsigned long last = 0;
    for (int i = 0; i <= w; i++) {
        unsigned long c = 0;
        if (i < w) {
            int t = w > 1 ? i * 255 / (w - 1) : 0;
            c = w2k_rgb(a[0] + (b[0] - a[0]) * t / 255, a[1] + (b[1] - a[1]) * t / 255,
                        a[2] + (b[2] - a[2]) * t / 255);
        }
        if (i > 0 && (i == w || c != last)) {
            XSetForeground(w2k.dpy, w2k.gc, last);
            XFillRectangle(w2k.dpy, d, w2k.gc, px + i - run, py, (unsigned)run, (unsigned)ph);
            run = 0;
        }
        last = c;
        run++;
    }
}

static void draw_row(Drawable d, const Row *r, int x, int y, int w, int rh,
                     int hot, unsigned long bg)
{
    if (r->kind == R_SEP) {
        PanelColours pc;
        panel_colours(&pc);
        unsigned long rule = px3(pc.rule);
        fill(d, x, y, w, SEP_H, bg);
        int left = bg == w2k_rgb(255, 255, 255);   /* the white column */
        if (w2k_theme == THEME_XP) {
            /* Measured: (159,183,216) over (214,241,255) on the blue,
             * (213,213,213) over white on the white, 34 in from the left
             * and the width of the column. */
            /* Measured off the panel: the white column's rule runs from
             * 24 to 171, the blue column's from 220 to 352. */
            int sx = left ? 24 : 220, sw = left ? 148 : 133;
            XSetForeground(w2k.dpy, w2k.gc, left ? w2k_rgb(213, 213, 213)
                                                 : w2k_rgb(159, 183, 216));
            w2k_fill_fg(d, sx, y + SEP_H / 2, sw, 1);
            XSetForeground(w2k.dpy, w2k.gc, left ? w2k_rgb(255, 255, 255)
                                                 : w2k_rgb(214, 241, 255));
            w2k_fill_fg(d, sx, y + SEP_H / 2 + 1, sw, 1);
            return;
        }
        XSetForeground(w2k.dpy, w2k.gc, rule);
        w2k_fill_fg(d, x + 8, y + SEP_H / 2, w - 16, 1);
        return;
    }

    int font = r->bold ? F_UI_BOLD : F_UI;
    int fh = w2k_font_height(font);
    fill(d, x, y, w, rh, hot ? w2k.col[C_HIGHLIGHT] : bg);
    int luna = w2k_theme == THEME_XP;
    /* Measured off Luna's panel: pinned items have their 32-pixel icon at
     * 8 and their text at 46; the blue column's icons are 24 pixels at
     * 198 with the text at 226 (icon 7 and text 35 past the column's
     * edge), one pixel higher than the row's centre would put it. The
     * classic rendition keeps 16-pixel icons at the edge. */
    int ix = x + (r->big ? 8 : luna ? 7 : 0);
    int tx = x + (r->big ? (luna ? 44 : 47) : luna ? 35 : 36);
    if (r->icon >= 0) {
        if (r->big)      w2k_bigicon_draw(d, ix, y + (rh - 32) / 2, r->icon);
        else if (luna)   w2k_icon_draw_scaled(d, ix, y + 3, r->icon, 24);
        else             w2k_icon_draw(d, ix, y + (rh - 16) / 2, r->icon);
    }
    /* The text pairs with its ground: the left column is the window
     * colour, the right the face. In a dark scheme they are both light,
     * but a high-contrast one can set them apart. */
    int col = hot ? C_HIGHLIGHTTEXT : bg == w2k.col[C_WINDOW] ? C_WINDOWTEXT : C_TEXT;
    char buf[128];
    w2k_ellipsis(font, r->label, w - (tx - x) - 14, buf, sizeof buf);
    int ty = y + (rh - fh) / 2 - (luna && !r->big ? 1 : 0);
    if (luna && !hot && !r->big && bg != w2k_rgb(255, 255, 255))
        w2k_text_mnemonic_rgb(d, font, tx, ty, buf, 8, 29, 67, 1); /* the blue column's navy */
    else
        w2k_text_mnemonic(d, font, tx, ty, buf, col, 1);
    if (r->kind == R_SUB) {
        /* The submenu arrow, pointing right as the menu control's does. */
        int ax = x + w - 12, ay = y + rh / 2 - 4;
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[col]);
        for (int i = 0; i < 5; i++)
            w2k_fill_fg(d, ax + i, ay + i, 1, 9 - 2 * i);
    }
}

/* ------------------------------------------------------------------ *
 * The Windows 7 panel
 * ------------------------------------------------------------------ */
#define NPANEL_SKINS 7
static struct { const char *name; W2kSkin *s; int tried; } panel_skins[NPANEL_SKINS] = {
    { "w7-usertile.png", NULL, 0 }, { "xp-panel-header.png", NULL, 0 },
    { "xp-panel-footer.png", NULL, 0 }, { "xp-panel-body.png", NULL, 0 },
    { "w7-panel.png", NULL, 0 }, { "vista-usertile.png", NULL, 0 },
    { "vista-power.png", NULL, 0 }
};

static const PanelMetrics *pm7(void) { return vista() ? &metricsv : aero() ? &metricsa : &metrics7; }

void startpanel_skins_reload(void)
{
    for (int i = 0; i < NPANEL_SKINS; i++) {
        w2k_skin_free(panel_skins[i].s);
        panel_skins[i].s = NULL;
        panel_skins[i].tried = 0;
    }
}

static W2kSkin *skin7(const char *name)
{
#define cache panel_skins
    for (int i = 0; i < NPANEL_SKINS; i++) {
        if (strcmp(cache[i].name, name)) continue;
        if (!cache[i].tried) {
            char path[1024];
            if (w2k_skin_path(name, path, sizeof path))
                cache[i].s = w2k_skin_load(path);
            cache[i].tried = 1;
        }
        return cache[i].s;
    }
    return NULL;
#undef cache
}

/* The pointer's row: a pale box on the white pane, a lighter patch of
 * blue on the blue one. Neither is in a screenshot; both are what the
 * real thing does, near enough. */
static void hover7(Drawable d, int x, int y, int w, int h, int light)
{
    fill(d, x, y, w, h, light ? w2k_rgb(229, 240, 252) : w2k_rgb(96, 140, 184));
    XSetForeground(w2k.dpy, w2k.gc,
                   light ? w2k_rgb(153, 188, 230) : w2k_rgb(150, 185, 220));
    rect_fg(d, x, y, w, h);
}

static void arrow7(Drawable d, int x, int cy, int r, int g, int b)
{
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
    for (int i = 0; i < 5; i++)
        w2k_fill_fg(d, x + i, cy - 4 + i, 1, 2 * (4 - i) + 1);
}

/* The panel's ground: one vertical gradient, light at the top and the
 * bottom and dark through the middle, measured every ten rows off the
 * screenshot (which is palettised, so the stops are what it dithers
 * between and the drawing is smoother than the picture). */
static void panel7_ground(Drawable pm, int x0, int y0, int w)
{
    static const struct { int y; unsigned char r, g, b; } st[] = {
        {   1, 174, 197, 218 }, {   2, 149, 178, 205 }, {   6, 149, 178, 205 },
        {   7, 174, 197, 218 }, {   8, 149, 178, 205 }, {  33, 132, 155, 177 },
        {  63, 121, 144, 167 }, {  73, 113, 132, 151 }, {  93, 101, 120, 138 },
        { 103,  93, 112, 129 }, { 143,  86, 102, 118 }, { 163,  74,  88, 101 },
        { 293,  74,  88, 101 }, { 303,  86, 102, 118 }, { 323,  93, 112, 129 },
        { 353, 101, 120, 138 }, { 373, 113, 132, 151 }, { 393, 121, 144, 167 },
        { 413, 132, 155, 177 }, { 423, 140, 164, 185 }, { 433, 149, 178, 205 },
        { 443, 164, 187, 211 }, { 463, 164, 187, 211 }, { 472, 179, 211, 241 },
        { 478, 207, 229, 249 },
    };
    int n = (int)(sizeof st / sizeof *st);
    for (int y = 1; y < P7_H - 1; y++) {
        int k = 0;
        while (k < n - 1 && st[k + 1].y < y) k++;
        int r, g, b;
        if (k >= n - 1 || st[k + 1].y == st[k].y) { r = st[k].r; g = st[k].g; b = st[k].b; }
        else {
            int t = (y - st[k].y) * 256 / (st[k + 1].y - st[k].y);
            if (t < 0) t = 0;
            if (t > 256) t = 256;
            r = st[k].r + (st[k + 1].r - st[k].r) * t / 256;
            g = st[k].g + (st[k + 1].g - st[k].g) * t / 256;
            b = st[k].b + (st[k + 1].b - st[k].b) * t / 256;
        }
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
        w2k_fill_fg(pm, x0 + 1, y0 + y, w - 2, 1);
    }
    /* The outline and the light line inside it. */
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(69, 78, 85));
    rect_fg(pm, x0, y0, w, P7_H);
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(113, 132, 151));
    w2k_fill_fg(pm, x0 + 1, y0 + 1, 1, P7_H - 2);
    w2k_fill_fg(pm, x0 + w - 2, y0 + 1, 1, P7_H - 2);
}

/* Vista's ground: one grey gradient, light at the top, dark through the
 * middle and lighter again at the bottom, measured every ten rows off
 * the screenshot; a dark line outside a lighter one all round, and a
 * light line over the bottom edge. */
static void panelv_ground(Drawable pm)
{
    static const struct { short y; unsigned char g; } st[] = {
        {   0, 109 }, {  10, 106 }, {  20, 103 }, {  30,  99 }, {  40,  94 },
        {  50,  89 }, {  60,  84 }, {  70,  79 }, {  80,  75 }, {  90,  72 },
        { 100,  70 }, { 110,  67 }, { 120,  65 }, { 130,  63 }, { 140,  61 },
        { 150,  59 }, { 160,  57 }, { 170,  56 }, { 180,  54 }, { 190,  53 },
        { 200,  52 }, { 220,  51 }, { 240,  51 }, { 250,  52 }, { 260,  53 },
        { 270,  54 }, { 280,  55 }, { 290,  57 }, { 300,  59 }, { 310,  61 },
        { 320,  63 }, { 330,  66 }, { 340,  68 }, { 350,  71 }, { 360,  74 },
        { 370,  77 }, { 380,  80 }, { 390,  83 }, { 400,  87 }, { 410,  93 },
        { 420, 100 }, { 430, 106 }, { 440, 113 }, { 450, 118 }, { 460, 123 },
        { 470, 127 }, { 473, 127 },
    };
    int n = (int)(sizeof st / sizeof *st);
    for (int y = 0; y < PV_H; y++) {
        int k = 0;
        while (k < n - 1 && st[k + 1].y < y) k++;
        int g = st[k].g;
        if (k < n - 1 && st[k + 1].y > st[k].y)
            g += (st[k + 1].g - st[k].g) * (y - st[k].y) / (st[k + 1].y - st[k].y);
        w2k_fill_rgb(pm, 1, y, P7_W - 2, 1, g, g, g);
    }
    w2k_fill_rgb(pm, 0, 0, 1, PV_H, 45, 45, 45);
    w2k_fill_rgb(pm, P7_W - 1, 0, 1, PV_H, 45, 45, 45);
    w2k_fill_rgb(pm, 1, 0, 1, PV_H, 103, 103, 103);
    w2k_fill_rgb(pm, P7_W - 2, 0, 1, PV_H, 103, 103, 103);
    w2k_fill_rgb(pm, 1, PV_H - 2, P7_W - 2, 1, 159, 159, 159);
    w2k_fill_rgb(pm, 0, PV_H - 1, P7_W, 1, 45, 45, 45);
}

/* The pointer's row on Vista's dark pane: a lighter grey box. */
static void hoverv(Drawable d, int x, int y, int w, int h)
{
    fill(d, x, y, w, h, w2k_rgb(92, 92, 92));
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(150, 150, 150));
    rect_fg(d, x, y, w, h);
}

/* The Windows 7 panel, and Vista's, which lays its rows out the same way
 * on its own ground. */
static void panel7_draw(Drawable pm)
{
    const PanelMetrics *m = pm7();
    int v = vista();
    int oy = m->over;
    int lh = m->left_bot - m->left_top + 1;
    if (v) {
        panelv_ground(pm);
    } else if (aero()) {
        /* Aero: a slab of dark glass over the wallpaper, the white pane
         * cut into it with the search band across its foot. In screen
         * pixels, from where the panel stands. */
        w2k_glass_above = panel;
        w2k_aero_panel(pm, 0, 0, panel_x, panel_y, w2k_px(P7_W), w2k_px(P7_H), w2k_px(oy),
                       w2k_px(P7_LEFT_X), w2k_px(m->left_top), w2k_px(P7_LEFT_W), w2k_px(lh),
                       w2k_px(m->ap_y + m->ap_h + 2), w2k_px(P7_TILE_X));
    } else {
        /* Above the panel only the tile is window (the rest is shaped
         * away); the desktop colour is for the W2K_RENDER picture of it. */
        fill(pm, 0, 0, P7_W, oy, w2k.col[C_DESKTOP]);
        panel7_ground(pm, 0, oy, P7_W);
    }
    /* The white column, in a dark line; Vista's has a lighter line over
     * it and another under it. Aero's came with the glass. */
    if (!aero()) {
        fill(pm, P7_LEFT_X, oy + m->left_top, P7_LEFT_W, lh, w2k_rgb(255, 255, 255));
        XSetForeground(w2k.dpy, w2k.gc, v ? w2k_rgb(61, 61, 61) : w2k_rgb(101, 120, 138));
        rect_fg(pm, P7_LEFT_X - 1, oy + m->left_top - 1, P7_LEFT_W + 2, lh + 2);
    }
    if (v) {
        w2k_fill_rgb(pm, P7_LEFT_X - 1, oy + m->left_top - 2, P7_LEFT_W + 2, 1, 144, 144, 144);
        w2k_fill_rgb(pm, P7_LEFT_X - 1, oy + m->left_bot + 2, P7_LEFT_W + 2, 1, 158, 158, 158);
    }
    /* The user's tile: Windows 7 stands it above the panel, Vista keeps
     * it inside the top edge. */
    if (v) {
        W2kSkin *tile = skin7("vista-usertile.png");
        if (tile) w2k_skin_draw(pm, tile, PV_TILE_X, PV_TILE_Y, 0, 0, PV_TILE_W, PV_TILE_H);
        w2k_account_picture_draw(pm, PV_TILE_X + (PV_TILE_W - 46) / 2,
                                 PV_TILE_Y + (PV_TILE_H - 46) / 2, 46, ICO_MYCOMPUTER);
    } else if (aero()) {
        /* The frame came with the glass; the picture fills it. */
        w2k_account_picture_draw(pm, P7_TILE_X + 5, 6, 48, ICO_MYCOMPUTER);
    } else {
        W2kSkin *tile = skin7("w7-usertile.png");
        if (tile) w2k_skin_draw(pm, tile, P7_TILE_X, 0, 0, 0, P7_TILE_W, P7_TILE_H);
        w2k_account_picture_draw(pm, P7_TILE_X + (P7_TILE_W - 44) / 2, (P7_TILE_H - 44) / 2,
                                 44, ICO_MYCOMPUTER);
    }

    int fh = w2k_font_height(F_UI);
    char buf[128];
    unsigned long sepc = v ? w2k_rgb(224, 224, 224) : w2k_rgb(207, 229, 249);

    /* The white column: 32-pixel icons at 12 on 38-pixel rows, text at 51. */
    int y = oy + m->left_y;
    for (int i = 0; i < nleft; i++) {
        const Row *r = &left_rows[i];
        if (r->kind == R_SEP) {
            fill(pm, P7_LEFT_X + 12, y + P7_SEP_H / 2, P7_LEFT_W - 24, 1, sepc);
            y += P7_SEP_H;
            continue;
        }
        if (y + P7_ROW_H > oy + m->ap_y - 4) break;
        if (hot_col == 0 && hot_row == i)
            hover7(pm, P7_LEFT_X + 2, y, P7_LEFT_W - 4, P7_ROW_H, 1);
        if (r->icon >= 0) w2k_bigicon_draw(pm, P7_LEFT_X + 3, y + 3, r->icon);
        w2k_ellipsis(F_UI, r->label, P7_LEFT_W - 60, buf, sizeof buf);
        w2k_text_rgb(pm, F_UI, P7_LEFT_X + 42, y + (P7_ROW_H - fh) / 2, buf,
                     0, 0, 0);
        y += P7_ROW_H;
    }

    int ay = oy + m->ap_y;
    fill(pm, P7_LEFT_X + 12, ay - 2, P7_LEFT_W - 24, 1, sepc);
    if (hot_col == 0 && hot_row == nleft)
        hover7(pm, P7_LEFT_X + 2, ay, P7_LEFT_W - 4, m->ap_h, 1);
    arrow7(pm, P7_LEFT_X + 11, ay + m->ap_h / 2, 0, 0, 0);
    w2k_text_rgb(pm, F_UI, P7_LEFT_X + 42, ay + (m->ap_h - fh) / 2,
                 "All Programs", 0, 0, 0);

    /* The search box: white in a dark line, the magnifier at its end. */
    int sy = oy + m->search_y, sh = m->search_h;
    fill(pm, P7_SEARCH_X, sy, P7_SEARCH_W, sh, w2k_rgb(255, 255, 255));
    XSetForeground(w2k.dpy, w2k.gc, v ? w2k_rgb(63, 63, 63) : w2k_rgb(74, 88, 101));
    rect_fg(pm, P7_SEARCH_X, sy, P7_SEARCH_W, sh);
    w2k_text_rgb(pm, F_UI, P7_SEARCH_X + 6, sy + (sh - fh) / 2,
                 "Search programs and files", 109, 109, 109);
    int mx = P7_SEARCH_X + P7_SEARCH_W - 14, my = sy + sh / 2 - 2;
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(58, 96, 140));
    XSetLineAttributes(w2k.dpy, w2k.gc, (unsigned)w2k_th(1), LineSolid, CapButt, JoinMiter);
    XDrawArc(w2k.dpy, pm, w2k.gc, w2k_cx(mx - 4), w2k_cx(my - 4),
             (unsigned)w2k_cw(mx - 4, 7), (unsigned)w2k_cw(my - 4, 7), 0, 360 * 64);
    XSetLineAttributes(w2k.dpy, w2k.gc, 0, LineSolid, CapButt, JoinMiter);
    for (int i = 0; i < 5; i++) {                 /* the handle */
        w2k_fill_fg(pm, mx + 2 + i, my + 2 + i, 1, 1);
        w2k_fill_fg(pm, mx + 3 + i, my + 2 + i, 1, 1);
    }

    /* The right column: white text, 35 pixels a row. Windows 7 leaves a
     * gap between groups; Vista rules it, a dark line over a light one. */
    y = oy + m->right_y;
    for (int i = 0; i < nright; i++) {
        const Row *r = &right_rows[i];
        if (r->kind == R_SEP) {
            if (v) {
                w2k_fill_rgb(pm, P7_RIGHT_X - 3, y + 1, P7_RIGHT_W + 3, 1, 44, 44, 44);
                w2k_fill_rgb(pm, P7_RIGHT_X - 3, y + 2, P7_RIGHT_W + 3, 1, 72, 72, 72);
            }
            y += m->rsep_h;
            continue;
        }
        if (y + m->rrow_h > oy + m->btn_y - 4) break;
        if (hot_col == 1 && hot_row == i) {
            if (v) hoverv(pm, P7_RIGHT_X + 2, y, P7_RIGHT_W - 4, m->rrow_h);
            else if (aero())
                w2k_aero_button(pm, w2k_px(P7_RIGHT_X + 2), w2k_px(y),
                                panel_x + w2k_px(P7_RIGHT_X + 2), panel_y + w2k_px(y),
                                w2k_px(P7_RIGHT_W - 4), w2k_px(m->rrow_h), -1, 1);
            else   hover7(pm, P7_RIGHT_X + 2, y, P7_RIGHT_W - 4, m->rrow_h, 0);
        }
        w2k_ellipsis(F_UI, r->label, P7_RIGHT_W - 30, buf, sizeof buf);
        w2k_text_rgb(pm, F_UI, P7_RIGHT_X + 13, y + (m->rrow_h - fh) / 2, buf,
                     255, 255, 255);
        if (r->kind == R_SUB)
            arrow7(pm, P7_RIGHT_X + P7_RIGHT_W - 14, y + m->rrow_h / 2,
                   255, 255, 255);
        y += m->rrow_h;
    }

    if (v) {
        /* Power, lock and the arrow, cropped whole from the reference;
         * the pointer's cell gets a light ring. */
        W2kSkin *pb = skin7("vista-power.png");
        int bx = m->btn_x, by = oy + m->btn_y;
        if (pb) w2k_skin_draw(pm, pb, bx, by, 0, 0, PV_BTN_W, PV_BTN_H);
        else {
            fill(pm, bx, by, PV_BTN_W, PV_BTN_H, w2k_rgb(60, 60, 60));
            XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(120, 120, 120));
            rect_fg(pm, bx, by, PV_BTN_W, PV_BTN_H);
        }
        int cx = bx;
        for (int k = 0; k < m->ncell; k++) {
            if (hot_col == 2 && hot_row == m->cell[k].row) {
                XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(210, 210, 210));
                rect_fg(pm, cx + 1, by + 1, m->cell[k].w - 2, PV_BTN_H - 2);
            }
            cx += m->cell[k].w;
        }
        return;
    }

    /* Shut down, and the arrow beside it that stands in for Log Off. */
    int shy = oy + P7_SHUT_Y;
    if (aero()) {
        w2k_aero_button(pm, w2k_px(P7_SHUT_X), w2k_px(shy), panel_x + w2k_px(P7_SHUT_X),
                        panel_y + w2k_px(shy), w2k_px(P7_SHUT_W + P7_ARROW_W), w2k_px(P7_SHUT_H),
                        w2k_px(P7_SHUT_W), hot_col == 2);
    } else {
        fill(pm, P7_SHUT_X, shy, P7_SHUT_W + P7_ARROW_W, P7_SHUT_H,
             hot_col == 2 ? w2k_rgb(190, 210, 232) : w2k_rgb(164, 187, 211));
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(101, 120, 138));
        rect_fg(pm, P7_SHUT_X, shy, P7_SHUT_W + P7_ARROW_W, P7_SHUT_H);
        w2k_fill_fg(pm, P7_SHUT_X + P7_SHUT_W, shy, 1, P7_SHUT_H);
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(214, 226, 240));
        rect_fg(pm, P7_SHUT_X + 1, shy + 1, P7_SHUT_W - 2, P7_SHUT_H - 2);
    }
    w2k_text_rgb(pm, F_UI, P7_SHUT_X + 7, shy + (P7_SHUT_H - fh) / 2,
                 "Shut down", 255, 255, 255);
    arrow7(pm, P7_SHUT_X + P7_SHUT_W + 10, shy + P7_SHUT_H / 2, 255, 255, 255);
    if (searching) draw_search(pm);
}

/* Column 0 left, 1 right, 2 the Shut down button (row 1) and its arrow
 * (row 0, Log Off), 3 the search box. */
static int hit7(int x, int y, int *col, int *row)
{
    const PanelMetrics *m = pm7();
    int oy = m->over;
    *col = *row = -1;
    if (x < 0 || x >= P7_W || y < oy || y >= oy + m->h) return 0;
    if (y >= oy + m->btn_y && y < oy + m->btn_y + m->btn_h) {
        int cx = m->btn_x;
        for (int k = 0; k < m->ncell; k++) {
            if (x >= cx && x < cx + m->cell[k].w) {
                *col = 2; *row = m->cell[k].row; return 1;
            }
            cx += m->cell[k].w;
        }
    }
    if (x >= P7_SEARCH_X && x < P7_SEARCH_X + P7_SEARCH_W &&
        y >= oy + m->search_y && y < oy + m->search_y + m->search_h) {
        *col = 3; *row = 0; return 1;
    }
    if (x >= P7_LEFT_X && x < P7_LEFT_X + P7_LEFT_W) {
        if (y >= oy + m->ap_y && y < oy + m->ap_y + m->ap_h) {
            *col = 0; *row = nleft; return 1;
        }
        int cy = oy + m->left_y;
        for (int i = 0; i < nleft; i++) {
            int rh = left_rows[i].kind == R_SEP ? P7_SEP_H : P7_ROW_H;
            if (cy + rh > oy + m->ap_y - 4) break;
            if (y >= cy && y < cy + rh) {
                if (left_rows[i].kind == R_SEP) return 0;
                *col = 0; *row = i; return 1;
            }
            cy += rh;
        }
        return 0;
    }
    if (x >= P7_RIGHT_X && x < P7_RIGHT_X + P7_RIGHT_W) {
        int cy = oy + m->right_y;
        for (int i = 0; i < nright; i++) {
            int rh = right_rows[i].kind == R_SEP ? m->rsep_h : m->rrow_h;
            if (cy + rh > oy + m->btn_y - 4) break;
            if (y >= cy && y < cy + rh) {
                if (right_rows[i].kind == R_SEP) return 0;
                *col = 1; *row = i; return 1;
            }
            cy += rh;
        }
    }
    return 0;
}

static void panel_draw(Drawable d)
{
    if (seven()) { panel7_draw(d); return; }

    PanelColours pc;
    panel_colours(&pc);
    unsigned long right_bg = px3(pc.right), rule = px3(pc.rule);

    Pixmap pm = d;
    int body_y = HEADER_H, body_h = panel_h - HEADER_H - FOOTER_H;

    /* Header and footer: XP's are cropped from its screenshot -- the
     * header with the picture frame, the footer with the buttons painted
     * out -- and the body is one row of it, tiled. Over the classic
     * scheme, or without the skins, gradients in the title colours. */
    W2kSkin *hd = w2k_theme == THEME_XP ? skin7("xp-panel-header.png") : NULL;
    W2kSkin *ft = w2k_theme == THEME_XP ? skin7("xp-panel-footer.png") : NULL;
    W2kSkin *bd = w2k_theme == THEME_XP ? skin7("xp-panel-body.png") : NULL;
    int skinned = hd && ft && bd && w2k_skin_w(hd) == XP_W &&
                  w2k_skin_h(hd) == XP_HEADER && w2k_skin_h(ft) == XP_FOOTER;
    if (skinned) {
        w2k_skin_draw(pm, hd, 0, 0, 0, 0, XP_W, XP_HEADER);
        w2k_skin_draw(pm, ft, 0, panel_h - FOOTER_H, 0, 0, XP_W, XP_FOOTER);
        w2k_skin_tile(pm, bd, 0, body_y, XP_W, body_h, 0, 0, XP_W, 1);
        /* The user's picture: 48 pixels inside a frame at 7 -- theirs, or
         * the 32-pixel icon centred in its place. */
        w2k_account_picture_draw(pm, 9, 9, 48, ICO_MYCOMPUTER);
    } else {
        hgradient(pm, 0, 0, PANEL_W, HEADER_H, pc.hdr1, pc.hdr2);
        hgradient(pm, 0, panel_h - FOOTER_H, PANEL_W, FOOTER_H, pc.hdr2, pc.hdr1);
        w2k_account_picture_draw(pm, 8, (HEADER_H - 48) / 2, 48, ICO_MYCOMPUTER);
        fill(pm, 0, body_y, LEFT_W, body_h, w2k.col[C_WINDOW]);
        fill(pm, LEFT_W, body_y, PANEL_W - LEFT_W, body_h, right_bg);
        XSetForeground(w2k.dpy, w2k.gc, rule);
        w2k_fill_fg(pm, LEFT_W, body_y, 1, body_h);
    }
    int fhb = w2k_font_height(F_UI_BOLD);
    /* Clear of the 48-pixel picture at 8 (it ends at 56). */
    int name_x = skinned ? 68 : 64;
    /* XP's white name has a dark shadow under it. Only a light name on a
     * dark bar gets one: under dark text on a light bar (Modern Light) it
     * doubled the name into a smear. */
    if (lum(pc.text) > lum(pc.hdr1) + 60)
        w2k_text_rgb(pm, F_UI_BOLD, name_x + 1, (64 - fhb) / 2 + 1, user_display_name(),
                     pc.hdr1[0] / 2, pc.hdr1[1] / 2, pc.hdr1[2] / 2);
    w2k_text_rgb(pm, F_UI_BOLD, name_x, (64 - fhb) / 2, user_display_name(),
                 pc.text[0], pc.text[1], pc.text[2]);

    left_skinned = skinned;
    int lx = skinned ? 2 : 0, lw = skinned ? LEFT_W - 2 : LEFT_W;
    int rx = LEFT_W + 1, rw = PANEL_W - LEFT_W - 1 - (skinned ? 7 : 0);
    unsigned long lbg = skinned ? w2k_rgb(255, 255, 255) : w2k.col[C_WINDOW];
    unsigned long rbg = skinned ? w2k_rgb(210, 229, 250) : right_bg;
    int y = body_y + 4;
    for (int i = 0; i < nleft; i++) {
        int rh = left_rows[i].kind == R_SEP ? SEP_H : ROW_H;
        draw_row(pm, &left_rows[i], lx, y, lw, ROW_H,
                 hot_col == 0 && hot_row == i, lbg);
        y += rh;
    }

    /* "All Programs" sits at the foot of the left column, above the
     * footer, a rule over it. */
    int ap_y = body_y + body_h - ALLPROG_H;
    if (skinned) {
        /* Measured: the rule 35 above the footer from 24 to 171; the bold
         * label at 46 with its top 21 above the footer; the green arrow,
         * 16 by 22, at 129. */
        int fb = panel_h - FOOTER_H;
        int hot = hot_col == 0 && hot_row == nleft;
        if (hot) fill(pm, lx, ap_y, lw, ALLPROG_H, w2k.col[C_HIGHLIGHT]);
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(213, 213, 213));
        w2k_fill_fg(pm, 24, fb - 35, 148, 1);
        w2k_text_mnemonic(pm, F_UI_BOLD, 46, fb - 23 - (w2k_font_height(F_UI_BOLD) - 13) / 2,
                          "All &Programs", hot ? C_HIGHLIGHTTEXT : C_TEXT, 1);
        /* The arrow: a green triangle 16 by 22 with a dark rim; its face
         * is lit from the left, the columns sampled off the reference. */
        static const unsigned char face[16][3] = {
            { 97, 203, 99 }, { 128, 214, 128 }, { 158, 224, 156 }, { 153, 222, 154 },
            { 148, 220, 153 }, { 136, 218, 137 }, { 123, 216, 121 }, { 103, 211, 101 },
            { 83, 206, 82 }, { 70, 191, 68 }, { 56, 176, 53 }, { 55, 154, 53 },
            { 54, 131, 53 }, { 52, 124, 50 }, { 48, 118, 46 }, { 41, 112, 42 },
        };
        int ax = 129, ay = fb - 27;
        for (int i = 0; i < 16; i++) {
            int half = (11 * (16 - i) + 15) / 16;   /* 22 tall at the base, to a point */
            int top = ay + 11 - half, h = 2 * half;
            if (h < 1) h = 1;
            XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(41, 112, 42));
            w2k_fill_fg(pm, ax + i, top, 1, h);
            if (h > 2) {
                XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(face[i][0], face[i][1], face[i][2]));
                w2k_fill_fg(pm, ax + i, top + 1, 1, h - 2);
            }
        }
    } else {
        XSetForeground(w2k.dpy, w2k.gc, rule);
        w2k_fill_fg(pm, lx + 34, ap_y - 5, lw - 60, 1);
        Row ap = { .kind = R_SUB, .id = SM_ALLPROGRAMS, .icon = ICO_NONE, .bold = 1 };
        snprintf(ap.label, sizeof ap.label, "All Programs");
        draw_row(pm, &ap, lx, ap_y, lw, ALLPROG_H,
                 hot_col == 0 && hot_row == nleft, lbg);
    }

    y = body_y + 4;
    for (int i = 0; i < nright; i++) {
        int rh = right_rows[i].kind == R_SEP ? SEP_H : RROW_H;
        draw_row(pm, &right_rows[i], rx, y, rw, RROW_H,
                 hot_col == 1 && hot_row == i, rbg);
        y += rh;
    }

    /* Footer buttons: Log Off and Turn Off Computer, right-aligned. */
    int fy = panel_h - FOOTER_H;
    int fh = w2k_font_height(F_UI);
    struct { const char *label; int id, icon; } fb[2] = {
        { "&Log Off", SM_LOGOFF, ICO_LOGOFF },
        { "T&urn Off Computer", SM_SHUTDOWN, ICO_SHUTDOWN },
    };
    if (skinned) {
        /* Measured: 24-pixel icons at 176 and 251, 8 below the footer's
         * top; the labels at 204 and 279 with their tops 16 below it. */
        static const int fx[2] = { 176, 251 };
        for (int i = 0; i < 2; i++) {
            int hot = hot_col == 2 && hot_row == i;
            int tw = w2k_mnemonic_width(F_UI, fb[i].label);
            if (hot) fill(pm, fx[i] - 4, fy + 5, 24 + 4 + tw + 8, FOOTER_H - 10, w2k.col[C_HIGHLIGHT]);
            w2k_icon_draw_scaled(pm, fx[i], fy + 8, fb[i].icon, 24);
            w2k_text_mnemonic_rgb(pm, F_UI, fx[i] + 28, fy + 14 - (fh - 13) / 2, fb[i].label,
                                  255, 255, 255, 1);
        }
    } else {
        int bx = PANEL_W - 8;
        for (int i = 1; i >= 0; i--) {
            int tw = w2k_mnemonic_width(F_UI, fb[i].label);
            int bw = 16 + 6 + tw + 12;
            bx -= bw;
            int hot = hot_col == 2 && hot_row == i;
            if (hot) fill(pm, bx, fy + 6, bw, FOOTER_H - 12, w2k.col[C_HIGHLIGHT]);
            w2k_icon_draw(pm, bx + 6, fy + (FOOTER_H - 16) / 2, fb[i].icon);
            if (hot)
                w2k_text_mnemonic(pm, F_UI, bx + 6 + 16 + 6, fy + (FOOTER_H - fh) / 2,
                                  fb[i].label, C_HIGHLIGHTTEXT, 1);
            else
                w2k_text_mnemonic_rgb(pm, F_UI, bx + 6 + 16 + 6, fy + (FOOTER_H - fh) / 2,
                                      fb[i].label, pc.text[0], pc.text[1], pc.text[2], 1);
        }
    }

    if (searching) draw_search(pm);
    if (!skinned) w2k_frame(pm, 0, 0, PANEL_W, panel_h, C_WINDOWFRAME);
}

/* Where the search draws when it has the left column: the results and
 * the box the query shows in. Windows 7's box is its own; XP's stands at
 * the top of the column. All in the panel's coordinates. */
static void search_area(int *x, int *y, int *w, int *h, int *rowh,
                        int *bx, int *by, int *bw, int *bh)
{
    *rowh = 24;
    if (seven()) {
        const PanelMetrics *m = pm7();
        int oy = m->over;
        *x = P7_LEFT_X + 2; *y = oy + m->left_top + 4; *w = P7_LEFT_W - 4;
        *h = oy + m->ap_y - 4 - *y;
        *bx = P7_SEARCH_X; *by = oy + m->search_y; *bw = P7_SEARCH_W; *bh = m->search_h;
        return;
    }
    int body_y = HEADER_H, body_h = panel_h - HEADER_H - FOOTER_H;
    int ap_y = body_y + body_h - ALLPROG_H;
    *bx = 6; *by = body_y + 6; *bw = LEFT_W - 12; *bh = 22;
    *x = 2; *y = *by + *bh + 6; *w = LEFT_W - 4;
    *h = ap_y - 6 - *y;
}

static void draw_search(Drawable d)
{
    int x, y, w, h, rowh, bx, by, bw, bh;
    search_area(&x, &y, &w, &h, &rowh, &bx, &by, &bw, &bh);
    /* The results sit on the left column's own ground: 7's pane and XP's
     * column are white, and over any other look the column is the window
     * colour. They were always painted white, and in a dark scheme the
     * results (in its light text) could not be seen. */
    unsigned long ground = seven() || left_skinned ? w2k_rgb(255, 255, 255) : w2k.col[C_WINDOW];
    if (seven()) fill(d, x, y, w, h, ground);
    else {
        /* The whole column above All Programs, from the top of the body:
         * a strip between the box and the first result used to let the
         * pinned row's icon show through. */
        fill(d, x, HEADER_H, w, y + h - HEADER_H, ground);
    }
    startsearch_draw_box(d, &ss, bx, by, bw, bh);
    startsearch_draw_rows(d, &ss, x, y, w, h, rowh, ground, 0);
}

static void panel_paint(void)
{
    int pw = w2k_px(PANEL_W), ph = w2k_px(panel_h);
    Pixmap pm = XCreatePixmap(w2k.dpy, panel, (unsigned)pw, (unsigned)ph, w2k.depth);
    panel_draw(pm);
    XCopyArea(w2k.dpy, pm, panel, w2k.gc, 0, 0, (unsigned)pw, (unsigned)ph, 0, 0);
    w2k_free_pixmap(pm);
}

/* Work out how tall the panel is for the rows it holds. */
static void panel_measure(void)
{
    if (seven()) { panel_h = pm7()->h + pm7()->over; return; }
    int body_h = 8 + rows_height(left_rows, nleft, ROW_H) + 8 + ALLPROG_H;
    int right_h = 8 + rows_height(right_rows, nright, RROW_H);
    if (right_h > body_h) body_h = right_h;
    if (w2k_theme == THEME_XP && body_h < XP_BODY_MIN) body_h = XP_BODY_MIN;
    panel_h = HEADER_H + body_h + FOOTER_H;
}

/* Development aid, like W2K_RENDER for the dialogs: paint the panel into
 * a pixmap and write it out as a PPM, so it can be looked at without a
 * desktop running. */
int startpanel_render(const char *path)
{
    build_rows();
    panel_measure();
    int pw = w2k_px(PANEL_W), ph = w2k_px(panel_h);
    Pixmap pm = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)pw, (unsigned)ph, w2k.depth);
    hot_col = hot_row = -1;
    /* W2K_RENDER_HOT=col,row paints that item as the pointer's. */
    if (getenv("W2K_RENDER_HOT"))
        sscanf(getenv("W2K_RENDER_HOT"), "%d,%d", &hot_col, &hot_row);
    panel_draw(pm);

    XImage *im = XGetImage(w2k.dpy, pm, 0, 0, (unsigned)pw, (unsigned)ph, AllPlanes,
                           ZPixmap);
    FILE *f = fopen(path, "wb");
    if (f && im) {
        fprintf(f, "P6\n%d %d\n255\n", pw, ph);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                unsigned long v = XGetPixel(im, x, y);
                unsigned char rgb[3] = { (v >> 16) & 0xff, (v >> 8) & 0xff,
                                         v & 0xff };
                fwrite(rgb, 1, 3, f);
            }
    }
    if (f) fclose(f);
    if (im) XDestroyImage(im);
    w2k_free_pixmap(pm);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Hit testing and the loop
 * ------------------------------------------------------------------ */
static int footer_hit(int x, int y, int *idx)
{
    if (y < panel_h - FOOTER_H) return 0;
    struct { const char *label; } fb[2] = { { "Log Off" },
                                            { "Turn Off Computer" } };
    int bx = PANEL_W - 8;
    for (int i = 1; i >= 0; i--) {
        int bw = 16 + 6 + w2k_mnemonic_width(F_UI, fb[i].label) + 12;
        bx -= bw;
        if (x >= bx && x < bx + bw) { *idx = i; return 1; }
    }
    return 0;
}

/* What is under the pointer: column 0 left, 1 right, 2 footer. */
static int hit_test(int x, int y, int *col, int *row)
{
    int body_y = HEADER_H, body_h = panel_h - HEADER_H - FOOTER_H;
    int ap_y = body_y + body_h - ALLPROG_H;
    if (seven()) return hit7(x, y, col, row);
    *col = *row = -1;
    if (x < 0 || x >= PANEL_W || y < 0 || y >= panel_h) return 0;

    int idx;
    if (footer_hit(x, y, &idx)) { *col = 2; *row = idx; return 1; }
    if (y < body_y || y >= body_y + body_h) return 0;

    if (x < LEFT_W) {
        if (y >= ap_y) { *col = 0; *row = nleft; return 1; }  /* All Programs */
        int i = row_at(left_rows, nleft, ROW_H, body_y + 4, y);
        if (i < 0) return 0;
        *col = 0; *row = i;
        return 1;
    }
    int i = row_at(right_rows, nright, RROW_H, body_y + 4, y);
    if (i < 0) return 0;
    *col = 1; *row = i;
    return 1;
}

static int row_id(int col, int row)
{
    if (col == 0) return row == nleft ? SM_ALLPROGRAMS :
                         (row >= 0 && row < nleft ? left_rows[row].id : 0);
    if (col == 1) return row >= 0 && row < nright ? right_rows[row].id : 0;
    if (col == 2) return row == 1 ? SM_SHUTDOWN : SM_LOGOFF;   /* the arrow, and Vista's lock, log off */
    if (col == 3) return SM_SEARCH;
    return 0;
}

/* All Programs and My Recent Documents open the classic menus, to the
 * right of the panel where Windows puts them. */
static int open_submenu(int id, int x, int y)
{
    W2kMenu *m = w2k_menu_new();
    if (id == SM_ALLPROGRAMS) {
        programs_collapse_all();               /* every opening starts folded */
        if (startdir_add_programs(m)) w2k_menu_sep(m);
        programs_add_groups(m);
    } else {
        int n = recent_load();
        if (!n) {
            w2k_menu_item(m, 0, "(Empty)", NULL, ICO_NONE);
            w2k_menu_disable(m);
        }
        for (int i = 0; i < n; i++) {
            /* Its kind of icon, not the file decoded as one (startmenu.c). */
            char label[260];
            w2k_menu_escape(recent_label(i), label, sizeof label);
            w2k_menu_item(m, RECENT_BASE + i, label, NULL,
                          w2k_file_icon(recent_label(i), 0));
        }
    }
    /* The panel holds the pointer grab; the menu takes it and gives it
     * back, in the same way the Start menu's context menus do. */
    w2k_menu_on_expand = programs_expand_menu;
    int chosen = w2k_menu_popup(m, x, y, MPOP_BOTTOMUP);
    w2k_menu_on_expand = NULL;
    w2k_menu_free(m);
    return chosen;
}

/* 0 when the pointer could not be taken back: without it the click-away
 * dismissal never fires and the panel sits on top of everything. */
static int panel_regrab(void)
{
    if (XGrabPointer(w2k.dpy, panel, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, w2k.cur_arrow,
                     CurrentTime) != GrabSuccess)
        return 0;
    XGrabKeyboard(w2k.dpy, panel, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    return 1;
}

int startpanel_run(int bx, int by)
{
    build_rows();
    panel_measure();

    const W2kMonitor *m = w2k_monitor_at(bx, by);
    int pw = w2k_px(PANEL_W), ph = w2k_px(panel_h);   /* on the screen */
    panel_x = m->x + 0;
    panel_y = by - ph;
    if (w2k_taskbar_edge == TB_TOP) panel_y = by;
    if (panel_y < m->y) panel_y = m->y;
    if (panel_x + pw > m->x + m->w) panel_x = m->x + m->w - pw;

    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel = w2k.col[C_WINDOW],
        .save_under = True,
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | KeyPressMask | LeaveWindowMask
    };
    panel = XCreateWindow(w2k.dpy, w2k.root, panel_x, panel_y, (unsigned)pw,
                          (unsigned)ph, 0, CopyFromParent, InputOutput,
                          CopyFromParent,
                          CWOverrideRedirect | CWBackPixel | CWSaveUnder |
                          CWEventMask, &a);
    if (w2k_theme == THEME_XP) {
        /* Rounded top corners, measured: five, three, two, one, one. */
        static const int ins[5] = { 5, 3, 2, 1, 1 };
        XRectangle rs[6];
        for (int i = 0; i < 5; i++) {
            int in = w2k_px(ins[i]), y0 = w2k_px(i), y1 = w2k_px(i + 1);
            rs[i] = (XRectangle){ (short)in, (short)y0,
                                  (unsigned short)(pw - 2 * in), (unsigned short)(y1 - y0) };
        }
        rs[5] = (XRectangle){ 0, (short)w2k_px(5), (unsigned short)pw,
                              (unsigned short)(ph - w2k_px(5)) };
        XShapeCombineRectangles(w2k.dpy, panel, ShapeBounding, 0, 0, rs, 6,
                                ShapeSet, Unsorted);
    }
    if (vista()) {
        /* A rounded rectangle, the corners' curve three, two, one, one. */
        static const int ins[4] = { 3, 2, 1, 1 };
        XRectangle rs[9];
        int n = 0;
        #define RV(x, y, w, h) (XRectangle){ (short)w2k_px(x), (short)w2k_px(y), \
                                (unsigned short)(w2k_px((x) + (w)) - w2k_px(x)), \
                                (unsigned short)(w2k_px((y) + (h)) - w2k_px(y)) }
        for (int i = 0; i < 4; i++) {
            rs[n++] = RV(ins[i], i, P7_W - 2 * ins[i], 1);
            rs[n++] = RV(ins[i], PV_H - 1 - i, P7_W - 2 * ins[i], 1);
        }
        rs[n++] = RV(0, 4, P7_W, PV_H - 8);
        #undef RV
        XShapeCombineRectangles(w2k.dpy, panel, ShapeBounding, 0, 0, rs, n,
                                ShapeSet, Unsorted);
    } else if (seven()) {
        /* Only the panel and the tile above it are window; the desktop
         * shows either side of the tile. */
        static const int ins[4] = { 3, 2, 1, 1 };   /* the corners' curve */
        XRectangle rs[14];
        int n = 0;
        #define R(x, y, w, h) (XRectangle){ (short)w2k_px(x), (short)w2k_px(y), \
                                (unsigned short)(w2k_px((x) + (w)) - w2k_px(x)), \
                                (unsigned short)(w2k_px((y) + (h)) - w2k_px(y)) }
        int tw = aero() ? PA_TILE_W : P7_TILE_W, over = aero() ? PA_OVER : P7_OVER;
        if (aero()) {
            /* Aero's slab: top corners five, three, two, one, one; the
             * bottom ones square on the bar. Measured. */
            static const int insa[5] = { 5, 3, 2, 1, 1 };
            for (int i = 0; i < 5; i++) rs[n++] = R(insa[i], over + i, P7_W - 2 * insa[i], 1);
            rs[n++] = R(0, over + 5, P7_W, P7_H - 5);
        } else {
            for (int i = 0; i < 4; i++) {
                rs[n++] = R(ins[i], over + i, P7_W - 2 * ins[i], 1);
                rs[n++] = R(ins[i], over + P7_H - 1 - i, P7_W - 2 * ins[i], 1);
            }
            rs[n++] = R(0, over + 4, P7_W, P7_H - 8);
        }
        /* The tile above the panel, its top corners cut like the skin's;
         * Aero's stands higher and is wider. */
        for (int i = 0; i < 3; i++)
            rs[n++] = R(P7_TILE_X + ins[i], i, tw - 2 * ins[i], 1);
        rs[n++] = R(P7_TILE_X, 3, tw, over - 3);
        #undef R
        XShapeCombineRectangles(w2k.dpy, panel, ShapeBounding, 0, 0, rs, n,
                                ShapeSet, Unsorted);
    }
    XMapRaised(w2k.dpy, panel);
    taskbar_orb_raise();               /* the menu opens behind the orb */
    if (XGrabPointer(w2k.dpy, panel, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, w2k.cur_arrow,
                     CurrentTime) != GrabSuccess) {
        XDestroyWindow(w2k.dpy, panel);
        return 0;
    }
    XGrabKeyboard(w2k.dpy, panel, True, GrabModeAsync, GrabModeAsync,
                  CurrentTime);

    hot_col = hot_row = -1;
    panel_paint();

    long opened = w2k_now_ms();
    int result = 0, done = 0;
    searching = 0;
    startpanel_cancel = 0;
    while (!done && running && !startpanel_cancel) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.window == panel) panel_paint();
            else wm_handle_event(&e);
            break;
        case MotionNotify: {
            int col, row;
            int x = w2k_lp(e.xmotion.x_root - panel_x), y = w2k_lp(e.xmotion.y_root - panel_y);
            if (searching) {
                int sx, sy, sw, sh, rowh, bx2, by2, bw2, bh2;
                search_area(&sx, &sy, &sw, &sh, &rowh, &bx2, &by2, &bw2, &bh2);
                if (x >= sx && x < sx + sw) {
                    int i = startsearch_row_at(&ss, sy, rowh, sh, y);
                    if (i >= 0 && i != ss.sel) { ss.sel = i; panel_paint(); }
                    break;
                }
            }
            hit_test(x, y, &col, &row);
            if (col != hot_col || row != hot_row) {
                hot_col = col;
                hot_row = row;
                panel_paint();
            }
            break;
        }
        case ButtonPress: {
            if (e.xbutton.x_root < panel_x || e.xbutton.x_root >= panel_x + pw ||
                e.xbutton.y_root < panel_y || e.xbutton.y_root >= panel_y + ph) {
                done = 1;                      /* click-away dismisses */
                break;
            }
            /* Right-clicking a pinned program offers to unpin or rename it,
             * with the same menu the classic Start menu shows. The menu
             * takes the grab; it comes back afterwards, and the rows are
             * rebuilt in case a pin went. */
            if (e.xbutton.button == Button3) {
                int col, row;
                int x = w2k_lp(e.xbutton.x_root - panel_x), y = w2k_lp(e.xbutton.y_root - panel_y);
                if (hit_test(x, y, &col, &row) && col == 0) {
                    int id = row_id(col, row);
                    if (id >= SM_PIN_BASE && id < SM_PIN_BASE + PIN_MAX) {
                        startmenu_context(id, e.xbutton.x_root, e.xbutton.y_root);
                        if (!panel_regrab()) { done = 1; break; }
                        build_rows();
                        hot_col = hot_row = -1;
                        opened = w2k_now_ms();
                        panel_paint();
                    }
                }
            }
            break;
        }
        case ButtonRelease: {
            if (w2k_now_ms() - opened < 250) break;   /* the opening click */
            if (e.xbutton.button != Button1) break;
            int col, row;
            int x = w2k_lp(e.xbutton.x_root - panel_x), y = w2k_lp(e.xbutton.y_root - panel_y);
            if (searching) {
                /* A result opens; the rest of the panel goes on working. */
                int sx, sy, sw, sh, rowh, bx2, by2, bw2, bh2;
                search_area(&sx, &sy, &sw, &sh, &rowh, &bx2, &by2, &bw2, &bh2);
                if (x >= sx && x < sx + sw && y >= sy && y < sy + sh) {
                    int i = startsearch_row_at(&ss, sy, rowh, sh, y);
                    if (i >= 0) { startsearch_run(&ss, i); done = 1; }
                    break;
                }
                if (x >= bx2 && x < bx2 + bw2 && y >= by2 && y < by2 + bh2) break;
            }
            /* Letting go over nothing -- a rule, the margin, the picture --
             * leaves the panel up, as Windows does. */
            if (!hit_test(x, y, &col, &row)) break;
            int id = row_id(col, row);
            if (!id) break;
            if (id == SM_SEARCH) {
                /* The search box, or Search on the right: the search
                 * opens in the panel, empty, waiting to be typed into. */
                searching = 1;
                startsearch_begin(&ss, "");
                hot_col = hot_row = -1;
                panel_paint();
                break;
            }
            if (id == SM_ALLPROGRAMS || id == SM_RECENTSUB) {
                /* The submenu takes the grab; take it back afterwards. */
                int chosen = open_submenu(id, panel_x + pw,
                                          seven() ? panel_y + w2k_px(P7_OVER + P7_AP_Y + P7_AP_H)
                                                  : panel_y + w2k_px(panel_h - FOOTER_H));
                if (chosen) { result = chosen; done = 1; break; }
                if (!panel_regrab()) { done = 1; break; }
                opened = w2k_now_ms();
                panel_paint();
                break;
            }
            result = id;
            done = 1;
            break;
        }
        case KeyPress: {
            /* The Windows key again closes the panel. */
            {
                KeySym sk = XLookupKeysym(&e.xkey, 0);
                if (sk == XK_Super_L || sk == XK_Super_R) { done = 1; break; }
            }
            if (searching) {
                int r = startsearch_key(&ss, &e.xkey);
                if (r == SS_RUN) { startsearch_run(&ss, ss.sel); done = 1; }
                else if (r == SS_ESC || r == SS_EMPTY) { searching = 0; panel_paint(); }
                else if (r == SS_CHANGED) panel_paint();
                break;
            }
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Escape) { done = 1; break; }
            /* Typing into the panel searches, inside the panel: the left
             * column gives way to the results and the box shows the
             * text, the first letter included. */
            char buf[8] = "";
            int n = XLookupString(&e.xkey, buf, sizeof buf - 1, NULL, NULL);
            if (n > 0 && (unsigned char)buf[0] >= ' ' && buf[0] != 127) {
                buf[n] = 0;
                searching = 1;
                startsearch_begin(&ss, buf);
                hot_col = hot_row = -1;
                panel_paint();
            }
            break;
        }
        default:
            wm_handle_event(&e);
            break;
        }
    }

    XUngrabKeyboard(w2k.dpy, CurrentTime);
    XUngrabPointer(w2k.dpy, CurrentTime);
    XDestroyWindow(w2k.dpy, panel);
    XFlush(w2k.dpy);
    return result;
}
