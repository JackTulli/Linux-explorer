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

/* Windows XP's panel, measured off its screenshot: 380 by 478, a 68-row
 * header (the last four rows are the orange rule), a 42-row footer, the
 * white column 2..189, the divider at 190 and the pale blue column from
 * 191, all cropped as strips. */
#define XP_W        380
#define XP_HEADER    68
#define XP_FOOTER    42
#define XP_LEFT_W   190
#define XP_BODY_MIN 368      /* the reference body: the panel does not shrink */

#define PANEL_W     (w2k_theme == THEME_BASIC7 ? P7_W : 380)
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

static int seven(void) { return w2k_theme == THEME_BASIC7; }

static Row  left_rows[MAXROWS], right_rows[MAXROWS];
static int  nleft, nright;
static int  panel_x, panel_y, panel_h;
static int  hot_col = -1, hot_row = -1;   /* what the pointer is over */
static Window panel;

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

static const char *user_display_name(void)
{
    static char name[128];
    if (name[0]) return name;
    struct passwd *pw = getpwuid(getuid());
    const char *n = pw && pw->pw_gecos && pw->pw_gecos[0] ? pw->pw_gecos
                  : pw && pw->pw_name ? pw->pw_name : "User";
    snprintf(name, sizeof name, "%.127s", n);
    char *comma = strchr(name, ',');       /* GECOS is a comma-separated list */
    if (comma) *comma = 0;
    if (name[0] >= 'a' && name[0] <= 'z') name[0] -= 32;
    return name;
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
        { "Windows Explorer", "w2kexplorer", SM_EXPLORER, ICO_EXPLORER },
        { "Notepad",          "w2knotepad",  SM_NOTEPAD,  ICO_NOTEPAD  },
        { "Calculator",       "w2kcalc",     SM_CALC,     ICO_CALC     },
        { "Task Manager",     "w2ktaskmgr",  SM_TASKMGR,  ICO_TASKMGR  },
        { "Command Prompt",   NULL,          SM_TERMINAL, ICO_TERMINAL },
        { "Snipping Tool",    "w2ksnip",     SM_SNIP,     ICO_SNIP     },
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
    push(right_rows, &nright, R_ITEM, SM_MYDOCS, "My Documents", ICO_MYDOCS, 0)->bold = 1;
    push(right_rows, &nright, R_ITEM, SM_MYPICS, "My Pictures",
         ICO_FILE_BITMAP, 0)->bold = 1;
    push(right_rows, &nright, R_ITEM, SM_MYMUSIC, "My Music",
         ICO_FILE_MEDIA, 0)->bold = 1;
    push(right_rows, &nright, R_ITEM, SM_MYCOMPUTER, "My Computer",
         ICO_MYCOMPUTER, 0)->bold = 1;
    push(right_rows, &nright, R_SUB, SM_RECENTSUB, "My Recent Documents",
         ICO_DOCUMENTS, 0);
    push(right_rows, &nright, R_SEP, 0, NULL, ICO_NONE, 0);
    push(right_rows, &nright, R_ITEM, SM_CONTROLPANEL, "Control Panel",
         ICO_CONTROLPANEL, 0);
    push(right_rows, &nright, R_ITEM, SM_DEFAULTS,
         "Set Program Access and Defaults", ICO_PROGRAMS, 0);
    push(right_rows, &nright, R_SEP, 0, NULL, ICO_NONE, 0);
    push(right_rows, &nright, R_ITEM, SM_HELP, "Help and Support", ICO_HELP, 0);
    push(right_rows, &nright, R_ITEM, SM_SEARCH, "Search", ICO_SEARCH, 0);
    push(right_rows, &nright, R_ITEM, SM_RUN, "Run...", ICO_RUN, 0);
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
/* The right column's ground: XP's pale blue, Windows 7's white, and the
 * face colour when the panel is worn over the classic scheme. */
static void panel_colours(unsigned long *right_bg, unsigned long *rule,
                          unsigned long *hdr1, unsigned long *hdr2,
                          unsigned long *hdr_text)
{
    if (w2k_theme == THEME_XP) {
        *right_bg  = w2k_rgb(211, 229, 250);
        *rule      = w2k_rgb(180, 205, 240);
        *hdr1      = w2k_rgb(0, 83, 225);
        *hdr2      = w2k_rgb(61, 149, 255);
        *hdr_text  = w2k_rgb(255, 255, 255);
    } else if (w2k_theme == THEME_BASIC7) {
        *right_bg  = w2k_rgb(240, 240, 240);
        *rule      = w2k_rgb(203, 203, 203);
        *hdr1      = w2k_rgb(60, 66, 74);
        *hdr2      = w2k_rgb(32, 36, 42);
        *hdr_text  = w2k_rgb(255, 255, 255);
    } else {
        *right_bg  = w2k.col[C_FACE];
        *rule      = w2k.col[C_SHADOW];
        *hdr1      = w2k.col[C_ACTIVETITLE];
        *hdr2      = w2k.col[C_ACTIVETITLE2];
        *hdr_text  = w2k.col[C_TITLETEXT];
    }
}

static void fill(Drawable d, int x, int y, int w, int h, unsigned long px)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(w2k.dpy, w2k.gc, px);
    XFillRectangle(w2k.dpy, d, w2k.gc, x, y, (unsigned)w, (unsigned)h);
}

static void hgradient(Drawable d, int x, int y, int w, int h,
                      unsigned long a, unsigned long b)
{
    XColor ca = { .pixel = a }, cb = { .pixel = b };
    XQueryColor(w2k.dpy, w2k.cmap, &ca);
    XQueryColor(w2k.dpy, w2k.cmap, &cb);
    for (int i = 0; i < w; i++) {
        int t = w > 1 ? i * 255 / (w - 1) : 0;
        int r = (ca.red >> 8) + ((cb.red >> 8) - (ca.red >> 8)) * t / 255;
        int g = (ca.green >> 8) + ((cb.green >> 8) - (ca.green >> 8)) * t / 255;
        int bl = (ca.blue >> 8) + ((cb.blue >> 8) - (ca.blue >> 8)) * t / 255;
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, bl));
        XFillRectangle(w2k.dpy, d, w2k.gc, x + i, y, 1, (unsigned)h);
    }
}

static void draw_row(Drawable d, const Row *r, int x, int y, int w, int rh,
                     int hot, unsigned long bg)
{
    if (r->kind == R_SEP) {
        unsigned long rbg, rule, h1, h2, ht;
        panel_colours(&rbg, &rule, &h1, &h2, &ht);
        fill(d, x, y, w, SEP_H, bg);
        int left = bg == w2k_rgb(255, 255, 255);   /* the white column */
        if (w2k_theme == THEME_XP) {
            /* Measured: (159,183,216) over (214,241,255) on the blue,
             * (213,213,213) over white on the white, 34 in from the left
             * and the width of the column. */
            XSetForeground(w2k.dpy, w2k.gc, left ? w2k_rgb(213, 213, 213)
                                                 : w2k_rgb(159, 183, 216));
            XFillRectangle(w2k.dpy, d, w2k.gc, x + (left ? 34 : 0), y + SEP_H / 2,
                           (unsigned)(left ? w - 60 : w), 1);
            XSetForeground(w2k.dpy, w2k.gc, left ? w2k_rgb(255, 255, 255)
                                                 : w2k_rgb(214, 241, 255));
            XFillRectangle(w2k.dpy, d, w2k.gc, x + (left ? 34 : 0), y + SEP_H / 2 + 1,
                           (unsigned)(left ? w - 60 : w), 1);
            return;
        }
        XSetForeground(w2k.dpy, w2k.gc, rule);
        XFillRectangle(w2k.dpy, d, w2k.gc, x + 8, y + SEP_H / 2,
                       (unsigned)(w - 16), 1);
        return;
    }

    int font = r->bold ? F_UI_BOLD : F_UI;
    int fh = w2k_font_height(font);
    fill(d, x, y, w, rh, hot ? w2k.col[C_HIGHLIGHT] : bg);
    /* Measured: 32-pixel icons at 8 with text at 47; 16-pixel icons at
     * the column's edge with text 36 past it. */
    int ix = x + (r->big ? 8 : 0), tx = x + (r->big ? 47 : 36);
    if (r->icon >= 0) {
        if (r->big) w2k_bigicon_draw(d, ix, y + (rh - 32) / 2, r->icon);
        else        w2k_icon_draw(d, ix, y + (rh - 16) / 2, r->icon);
    }
    int col = hot ? C_HIGHLIGHTTEXT : C_TEXT;
    char buf[128];
    w2k_ellipsis(font, r->label, w - (tx - x) - 14, buf, sizeof buf);
    w2k_text(d, font, tx, y + (rh - fh) / 2, buf, col);
    if (r->kind == R_SUB) {
        /* The submenu arrow, drawn as the menu control draws it. */
        int ax = x + w - 12, ay = y + rh / 2 - 4;
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[col]);
        for (int i = 0; i < 5; i++)
            XFillRectangle(w2k.dpy, d, w2k.gc, ax + i, ay + 4 - i, 1,
                           (unsigned)(2 * i + 1));
    }
}

/* ------------------------------------------------------------------ *
 * The Windows 7 panel
 * ------------------------------------------------------------------ */
static W2kSkin *skin7(const char *name)
{
    static struct { const char *name; W2kSkin *s; int tried; } cache[5] = {
        { "w7-usertile.png", NULL, 0 }, { "xp-panel-header.png", NULL, 0 },
        { "xp-panel-footer.png", NULL, 0 }, { "xp-panel-body.png", NULL, 0 },
        { "w7-panel.png", NULL, 0 }
    };
    for (int i = 0; i < 5; i++) {
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
}

/* The pointer's row: a pale box on the white pane, a lighter patch of
 * blue on the blue one. Neither is in a screenshot; both are what the
 * real thing does, near enough. */
static void hover7(Drawable d, int x, int y, int w, int h, int light)
{
    fill(d, x, y, w, h, light ? w2k_rgb(229, 240, 252) : w2k_rgb(96, 140, 184));
    XSetForeground(w2k.dpy, w2k.gc,
                   light ? w2k_rgb(153, 188, 230) : w2k_rgb(150, 185, 220));
    XDrawRectangle(w2k.dpy, d, w2k.gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
}

static void arrow7(Drawable d, int x, int cy, int r, int g, int b)
{
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
    for (int i = 0; i < 5; i++)
        XFillRectangle(w2k.dpy, d, w2k.gc, x + i, cy - 4 + i, 1,
                       (unsigned)(2 * (4 - i) + 1));
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
        XFillRectangle(w2k.dpy, pm, w2k.gc, x0 + 1, y0 + y, (unsigned)(w - 2), 1);
    }
    /* The outline and the light line inside it. */
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(69, 78, 85));
    XDrawRectangle(w2k.dpy, pm, w2k.gc, x0, y0, (unsigned)(w - 1), P7_H - 1);
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(113, 132, 151));
    XDrawLine(w2k.dpy, pm, w2k.gc, x0 + 1, y0 + 1, x0 + 1, y0 + P7_H - 2);
    XDrawLine(w2k.dpy, pm, w2k.gc, x0 + w - 2, y0 + 1, x0 + w - 2, y0 + P7_H - 2);
}

static void panel7_draw(Drawable pm)
{
    W2kSkin *tile = skin7("w7-usertile.png");
    int oy = P7_OVER;
    /* Above the panel only the tile is window (the rest is shaped away);
     * the desktop colour is for the W2K_RENDER picture of it. */
    fill(pm, 0, 0, P7_W, oy, w2k.col[C_DESKTOP]);
    panel7_ground(pm, 0, oy, P7_W);
    /* The white column, in a dark line. */
    fill(pm, P7_LEFT_X, oy + P7_LEFT_TOP, P7_LEFT_W, P7_LEFT_BOT - P7_LEFT_TOP + 1,
         w2k_rgb(255, 255, 255));
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(101, 120, 138));
    XDrawRectangle(w2k.dpy, pm, w2k.gc, P7_LEFT_X - 1, oy + P7_LEFT_TOP - 1,
                   P7_LEFT_W + 1, P7_LEFT_BOT - P7_LEFT_TOP + 2);
    if (tile) w2k_skin_draw(pm, tile, P7_TILE_X, 0, 0, 0, P7_TILE_W, P7_TILE_H);
    w2k_bigicon_draw(pm, P7_TILE_X + (P7_TILE_W - 32) / 2, (P7_TILE_H - 32) / 2,
                     ICO_MYCOMPUTER);

    int fh = w2k_font_height(F_UI);
    char buf[128];

    /* The white column: 32-pixel icons at 12 on 38-pixel rows, text at 51. */
    int y = oy + P7_LEFT_Y;
    for (int i = 0; i < nleft; i++) {
        const Row *r = &left_rows[i];
        if (r->kind == R_SEP) {
            fill(pm, P7_LEFT_X + 12, y + P7_SEP_H / 2, P7_LEFT_W - 24, 1,
                 w2k_rgb(207, 229, 249));
            y += P7_SEP_H;
            continue;
        }
        if (y + P7_ROW_H > oy + P7_AP_Y - 4) break;
        if (hot_col == 0 && hot_row == i)
            hover7(pm, P7_LEFT_X + 2, y, P7_LEFT_W - 4, P7_ROW_H, 1);
        if (r->icon >= 0) w2k_bigicon_draw(pm, P7_LEFT_X + 3, y + 3, r->icon);
        w2k_ellipsis(F_UI, r->label, P7_LEFT_W - 60, buf, sizeof buf);
        w2k_text_rgb(pm, F_UI, P7_LEFT_X + 42, y + (P7_ROW_H - fh) / 2, buf,
                     0, 0, 0);
        y += P7_ROW_H;
    }

    int ay = oy + P7_AP_Y;
    fill(pm, P7_LEFT_X + 12, ay - 2, P7_LEFT_W - 24, 1, w2k_rgb(207, 229, 249));
    if (hot_col == 0 && hot_row == nleft)
        hover7(pm, P7_LEFT_X + 2, ay, P7_LEFT_W - 4, P7_AP_H, 1);
    arrow7(pm, P7_LEFT_X + 11, ay + P7_AP_H / 2, 0, 0, 0);
    w2k_text_rgb(pm, F_UI, P7_LEFT_X + 42, ay + (P7_AP_H - fh) / 2,
                 "All Programs", 0, 0, 0);

    /* The search box: white in a dark line, the magnifier at its end. */
    int sy = oy + P7_SEARCH_Y;
    fill(pm, P7_SEARCH_X, sy, P7_SEARCH_W, P7_SEARCH_H, w2k_rgb(255, 255, 255));
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(74, 88, 101));
    XDrawRectangle(w2k.dpy, pm, w2k.gc, P7_SEARCH_X, sy, P7_SEARCH_W - 1,
                   P7_SEARCH_H - 1);
    w2k_text_rgb(pm, F_UI, P7_SEARCH_X + 6, sy + (P7_SEARCH_H - fh) / 2,
                 "Search programs and files", 109, 109, 109);
    int mx = P7_SEARCH_X + P7_SEARCH_W - 14, my = sy + P7_SEARCH_H / 2 - 2;
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(58, 96, 140));
    XDrawArc(w2k.dpy, pm, w2k.gc, mx - 4, my - 4, 7, 7, 0, 360 * 64);
    XDrawLine(w2k.dpy, pm, w2k.gc, mx + 2, my + 2, mx + 6, my + 6);
    XDrawLine(w2k.dpy, pm, w2k.gc, mx + 3, my + 2, mx + 7, my + 6);

    /* The blue column: white text, 35 pixels a row. */
    y = oy + P7_RIGHT_Y;
    for (int i = 0; i < nright; i++) {
        const Row *r = &right_rows[i];
        if (r->kind == R_SEP) {          /* a gap; Windows 7 draws no rule */
            y += P7_RSEP_H;
            continue;
        }
        if (y + P7_RROW_H > oy + P7_SHUT_Y - 4) break;
        if (hot_col == 1 && hot_row == i)
            hover7(pm, P7_RIGHT_X + 2, y, P7_RIGHT_W - 4, P7_RROW_H, 0);
        w2k_ellipsis(F_UI, r->label, P7_RIGHT_W - 30, buf, sizeof buf);
        w2k_text_rgb(pm, F_UI, P7_RIGHT_X + 13, y + (P7_RROW_H - fh) / 2, buf,
                     255, 255, 255);
        if (r->kind == R_SUB)
            arrow7(pm, P7_RIGHT_X + P7_RIGHT_W - 14, y + P7_RROW_H / 2,
                   255, 255, 255);
        y += P7_RROW_H;
    }

    /* Shut down, and the arrow beside it that stands in for Log Off. */
    int shy = oy + P7_SHUT_Y;
    fill(pm, P7_SHUT_X, shy, P7_SHUT_W + P7_ARROW_W, P7_SHUT_H,
         hot_col == 2 ? w2k_rgb(190, 210, 232) : w2k_rgb(164, 187, 211));
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(101, 120, 138));
    XDrawRectangle(w2k.dpy, pm, w2k.gc, P7_SHUT_X, shy,
                   P7_SHUT_W + P7_ARROW_W - 1, P7_SHUT_H - 1);
    XDrawLine(w2k.dpy, pm, w2k.gc, P7_SHUT_X + P7_SHUT_W, shy,
              P7_SHUT_X + P7_SHUT_W, shy + P7_SHUT_H - 1);
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(214, 226, 240));
    XDrawRectangle(w2k.dpy, pm, w2k.gc, P7_SHUT_X + 1, shy + 1,
                   P7_SHUT_W - 3, P7_SHUT_H - 3);
    w2k_text_rgb(pm, F_UI, P7_SHUT_X + 7, shy + (P7_SHUT_H - fh) / 2,
                 "Shut down", 255, 255, 255);
    arrow7(pm, P7_SHUT_X + P7_SHUT_W + 10, shy + P7_SHUT_H / 2, 255, 255, 255);
}

/* Column 0 left, 1 right, 2 the Shut down button (row 1) and its arrow
 * (row 0, Log Off), 3 the search box. */
static int hit7(int x, int y, int *col, int *row)
{
    int oy = P7_OVER;
    *col = *row = -1;
    if (x < 0 || x >= P7_W || y < oy || y >= oy + P7_H) return 0;
    if (y >= oy + P7_SHUT_Y && y < oy + P7_SHUT_Y + P7_SHUT_H) {
        if (x >= P7_SHUT_X && x < P7_SHUT_X + P7_SHUT_W) {
            *col = 2; *row = 1; return 1;
        }
        if (x >= P7_SHUT_X + P7_SHUT_W &&
            x < P7_SHUT_X + P7_SHUT_W + P7_ARROW_W) {
            *col = 2; *row = 0; return 1;
        }
    }
    if (x >= P7_SEARCH_X && x < P7_SEARCH_X + P7_SEARCH_W &&
        y >= oy + P7_SEARCH_Y && y < oy + P7_SEARCH_Y + P7_SEARCH_H) {
        *col = 3; *row = 0; return 1;
    }
    if (x >= P7_LEFT_X && x < P7_LEFT_X + P7_LEFT_W) {
        if (y >= oy + P7_AP_Y && y < oy + P7_AP_Y + P7_AP_H) {
            *col = 0; *row = nleft; return 1;
        }
        int cy = oy + P7_LEFT_Y;
        for (int i = 0; i < nleft; i++) {
            int rh = left_rows[i].kind == R_SEP ? P7_SEP_H : P7_ROW_H;
            if (cy + rh > oy + P7_AP_Y - 4) break;
            if (y >= cy && y < cy + rh) {
                if (left_rows[i].kind == R_SEP) return 0;
                *col = 0; *row = i; return 1;
            }
            cy += rh;
        }
        return 0;
    }
    if (x >= P7_RIGHT_X && x < P7_RIGHT_X + P7_RIGHT_W) {
        int cy = oy + P7_RIGHT_Y;
        for (int i = 0; i < nright; i++) {
            int rh = right_rows[i].kind == R_SEP ? P7_RSEP_H : P7_RROW_H;
            if (cy + rh > oy + P7_SHUT_Y - 4) break;
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

    unsigned long right_bg, rule, hdr1, hdr2, hdr_text;
    panel_colours(&right_bg, &rule, &hdr1, &hdr2, &hdr_text);

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
        /* The user's picture: 48 pixels inside a frame at 7; ours is the
         * 32-pixel icon, centred. */
        w2k_bigicon_draw(pm, 17, 17, ICO_MYCOMPUTER);
    } else {
        hgradient(pm, 0, 0, PANEL_W, HEADER_H, hdr1, hdr2);
        hgradient(pm, 0, panel_h - FOOTER_H, PANEL_W, FOOTER_H, hdr2, hdr1);
        w2k_bigicon_draw(pm, 10, (HEADER_H - 32) / 2, ICO_MYCOMPUTER);
        fill(pm, 0, body_y, LEFT_W, body_h, w2k.col[C_WINDOW]);
        fill(pm, LEFT_W, body_y, PANEL_W - LEFT_W, body_h, right_bg);
        XSetForeground(w2k.dpy, w2k.gc, rule);
        XFillRectangle(w2k.dpy, pm, w2k.gc, LEFT_W, body_y, 1, (unsigned)body_h);
    }
    int fhb = w2k_font_height(F_UI_BOLD);
    int name_x = skinned ? 68 : 52;
    w2k_text_rgb(pm, F_UI_BOLD, name_x + 1, (64 - fhb) / 2 + 1, user_display_name(),
                 (int)((hdr1 >> 16) & 0xff) / 2, (int)((hdr1 >> 8) & 0xff) / 2,
                 (int)(hdr1 & 0xff) / 2);
    w2k_text_rgb(pm, F_UI_BOLD, name_x, (64 - fhb) / 2, user_display_name(),
                 (int)((hdr_text >> 16) & 0xff), (int)((hdr_text >> 8) & 0xff),
                 (int)(hdr_text & 0xff));

    int lx = skinned ? 2 : 0, lw = skinned ? LEFT_W - 2 : LEFT_W;
    int rx = LEFT_W + 1, rw = PANEL_W - LEFT_W - 1 - (skinned ? 7 : 0);
    unsigned long lbg = skinned ? w2k_rgb(255, 255, 255) : w2k.col[C_WINDOW];
    unsigned long rbg = skinned ? w2k_rgb(208, 230, 250) : right_bg;
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
    XSetForeground(w2k.dpy, w2k.gc, skinned ? w2k_rgb(210, 210, 210) : rule);
    XFillRectangle(w2k.dpy, pm, w2k.gc, lx + 34, ap_y - 5, (unsigned)(lw - 60), 1);
    Row ap = { .kind = R_SUB, .id = SM_ALLPROGRAMS, .icon = ICO_NONE, .bold = 1 };
    snprintf(ap.label, sizeof ap.label, "All Programs");
    draw_row(pm, &ap, lx, ap_y, lw, ALLPROG_H,
             hot_col == 0 && hot_row == nleft, lbg);

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
        { "Log Off", SM_LOGOFF, ICO_LOGOFF },
        { "Turn Off Computer", SM_SHUTDOWN, ICO_SHUTDOWN },
    };
    int bx = PANEL_W - 8;
    for (int i = 1; i >= 0; i--) {
        int tw = w2k_text_width(F_UI, fb[i].label, -1);
        int bw = 16 + 6 + tw + 12;
        bx -= bw;
        int hot = hot_col == 2 && hot_row == i;
        if (hot) fill(pm, bx, fy + 6, bw, FOOTER_H - 12, w2k.col[C_HIGHLIGHT]);
        w2k_icon_draw(pm, bx + 6, fy + (FOOTER_H - 16) / 2, fb[i].icon);
        w2k_text_rgb(pm, F_UI, bx + 6 + 16 + 6, fy + (FOOTER_H - fh) / 2,
                     fb[i].label, 255, 255, 255);
    }

    if (!skinned) w2k_frame(pm, 0, 0, PANEL_W, panel_h, C_WINDOWFRAME);
}

static void panel_paint(void)
{
    Pixmap pm = XCreatePixmap(w2k.dpy, panel, PANEL_W, panel_h, w2k.depth);
    panel_draw(pm);
    XCopyArea(w2k.dpy, pm, panel, w2k.gc, 0, 0, PANEL_W, panel_h, 0, 0);
    w2k_free_pixmap(pm);
}

/* Work out how tall the panel is for the rows it holds. */
static void panel_measure(void)
{
    if (seven()) { panel_h = P7_H + P7_OVER; return; }
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
    Pixmap pm = XCreatePixmap(w2k.dpy, w2k.root, PANEL_W, panel_h, w2k.depth);
    hot_col = hot_row = -1;
    /* W2K_RENDER_HOT=col,row paints that item as the pointer's. */
    if (getenv("W2K_RENDER_HOT"))
        sscanf(getenv("W2K_RENDER_HOT"), "%d,%d", &hot_col, &hot_row);
    panel_draw(pm);

    XImage *im = XGetImage(w2k.dpy, pm, 0, 0, PANEL_W, panel_h, AllPlanes,
                           ZPixmap);
    FILE *f = fopen(path, "wb");
    if (f && im) {
        fprintf(f, "P6\n%d %d\n255\n", PANEL_W, panel_h);
        for (int y = 0; y < panel_h; y++)
            for (int x = 0; x < PANEL_W; x++) {
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
        int bw = 16 + 6 + w2k_text_width(F_UI, fb[i].label, -1) + 12;
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
    if (col == 2) return row == 0 ? SM_LOGOFF : SM_SHUTDOWN;
    if (col == 3) return SM_SEARCH;
    return 0;
}

/* All Programs and My Recent Documents open the classic menus, to the
 * right of the panel where Windows puts them. */
static int open_submenu(int id, int x, int y)
{
    W2kMenu *m = w2k_menu_new();
    if (id == SM_ALLPROGRAMS) {
        if (startdir_add_programs(m)) w2k_menu_sep(m);
        programs_add_groups(m);
    } else {
        int n = recent_load();
        if (!n) {
            w2k_menu_item(m, 0, "(Empty)", NULL, ICO_NONE);
            w2k_menu_disable(m);
        }
        for (int i = 0; i < n; i++)
            w2k_menu_item(m, RECENT_BASE + i, recent_label(i), NULL,
                          w2k_icon_by_name(recent_file(i)));
    }
    /* The panel holds the pointer grab; the menu takes it and gives it
     * back, in the same way the Start menu's context menus do. */
    int chosen = w2k_menu_popup(m, x, y, MPOP_BOTTOMUP);
    w2k_menu_free(m);
    return chosen;
}

static void panel_regrab(void)
{
    XGrabPointer(w2k.dpy, panel, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, w2k.cur_arrow, CurrentTime);
    XGrabKeyboard(w2k.dpy, panel, True, GrabModeAsync, GrabModeAsync,
                  CurrentTime);
}

int startpanel_run(int bx, int by)
{
    build_rows();
    panel_measure();

    const W2kMonitor *m = w2k_monitor_at(bx, by);
    panel_x = m->x + 0;
    panel_y = by - panel_h;
    if (w2k_taskbar_edge == TB_TOP) panel_y = by;
    if (panel_y < m->y) panel_y = m->y;
    if (panel_x + PANEL_W > m->x + m->w) panel_x = m->x + m->w - PANEL_W;

    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel = w2k.col[C_WINDOW],
        .save_under = True,
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | KeyPressMask | LeaveWindowMask
    };
    panel = XCreateWindow(w2k.dpy, w2k.root, panel_x, panel_y, PANEL_W,
                          panel_h, 0, CopyFromParent, InputOutput,
                          CopyFromParent,
                          CWOverrideRedirect | CWBackPixel | CWSaveUnder |
                          CWEventMask, &a);
    if (w2k_theme == THEME_XP) {
        /* Rounded top corners, measured: five, three, two, one, one. */
        static const int ins[5] = { 5, 3, 2, 1, 1 };
        XRectangle rs[6];
        for (int i = 0; i < 5; i++)
            rs[i] = (XRectangle){ (short)ins[i], (short)i,
                                  (unsigned short)(PANEL_W - 2 * ins[i]), 1 };
        rs[5] = (XRectangle){ 0, 5, (unsigned short)PANEL_W, (unsigned short)(panel_h - 5) };
        XShapeCombineRectangles(w2k.dpy, panel, ShapeBounding, 0, 0, rs, 6,
                                ShapeSet, Unsorted);
    }
    if (seven()) {
        /* Only the panel and the tile above it are window; the desktop
         * shows either side of the tile. */
        static const int ins[4] = { 3, 2, 1, 1 };   /* the corners' curve */
        XRectangle rs[14];
        int n = 0;
        for (int i = 0; i < 4; i++) {
            rs[n++] = (XRectangle){ (short)ins[i], (short)(P7_OVER + i),
                                    (unsigned short)(P7_W - 2 * ins[i]), 1 };
            rs[n++] = (XRectangle){ (short)ins[i],
                                    (short)(P7_OVER + P7_H - 1 - i),
                                    (unsigned short)(P7_W - 2 * ins[i]), 1 };
        }
        rs[n++] = (XRectangle){ 0, P7_OVER + 4, P7_W, P7_H - 8 };
        /* The tile above the panel, its top corners cut like the skin's. */
        for (int i = 0; i < 3; i++)
            rs[n++] = (XRectangle){ (short)(P7_TILE_X + ins[i]), (short)i,
                                    (unsigned short)(P7_TILE_W - 2 * ins[i]), 1 };
        rs[n++] = (XRectangle){ P7_TILE_X, 3, P7_TILE_W, P7_OVER - 3 };
        XShapeCombineRectangles(w2k.dpy, panel, ShapeBounding, 0, 0, rs, n,
                                ShapeSet, Unsorted);
    }
    XMapRaised(w2k.dpy, panel);
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
    char typed[8] = "";              /* a key typed: the panel searches for it */
    while (!done && running) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.window == panel) panel_paint();
            else wm_handle_event(&e);
            break;
        case MotionNotify: {
            int col, row;
            int x = e.xmotion.x_root - panel_x, y = e.xmotion.y_root - panel_y;
            hit_test(x, y, &col, &row);
            if (col != hot_col || row != hot_row) {
                hot_col = col;
                hot_row = row;
                panel_paint();
            }
            break;
        }
        case ButtonPress: {
            if (e.xbutton.x_root < panel_x || e.xbutton.x_root >= panel_x + PANEL_W ||
                e.xbutton.y_root < panel_y || e.xbutton.y_root >= panel_y + panel_h) {
                done = 1;                      /* click-away dismisses */
                break;
            }
            /* Right-clicking a pinned program offers to unpin or rename it,
             * with the same menu the classic Start menu shows. The menu
             * takes the grab; it comes back afterwards, and the rows are
             * rebuilt in case a pin went. */
            if (e.xbutton.button == Button3) {
                int col, row;
                int x = e.xbutton.x_root - panel_x, y = e.xbutton.y_root - panel_y;
                if (hit_test(x, y, &col, &row) && col == 0) {
                    int id = row_id(col, row);
                    if (id >= SM_PIN_BASE && id < SM_PIN_BASE + PIN_MAX) {
                        startmenu_context(id, e.xbutton.x_root, e.xbutton.y_root);
                        panel_regrab();
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
            int x = e.xbutton.x_root - panel_x, y = e.xbutton.y_root - panel_y;
            /* Letting go over nothing -- a rule, the margin, the picture --
             * leaves the panel up, as Windows does. */
            if (!hit_test(x, y, &col, &row)) break;
            int id = row_id(col, row);
            if (!id) break;
            if (id == SM_ALLPROGRAMS || id == SM_RECENTSUB) {
                /* The submenu takes the grab; take it back afterwards. */
                int chosen = open_submenu(id, panel_x + PANEL_W,
                                          seven() ? panel_y + P7_OVER + P7_AP_Y + P7_AP_H
                                                  : panel_y + panel_h - FOOTER_H);
                if (chosen) { result = chosen; done = 1; break; }
                panel_regrab();
                opened = w2k_now_ms();
                panel_paint();
                break;
            }
            result = id;
            done = 1;
            break;
        }
        case KeyPress: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Escape) { done = 1; break; }
            /* Typing into the panel searches, as typing into Windows 7's
             * search box does: the panel closes and the Search dialog
             * opens with the character typed. */
            char buf[8] = "";
            int n = XLookupString(&e.xkey, buf, sizeof buf - 1, NULL, NULL);
            if (n > 0 && buf[0] >= ' ' && buf[0] < 127) {
                buf[n] = 0;
                snprintf(typed, sizeof typed, "%s", buf);
                done = 1;
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
    if (typed[0]) wm_search_dialog(typed);
    return result;
}
