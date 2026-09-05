/* desktop.c -- the root surface: wallpaper colour and the desktop icons. */
#include "wm.h"
#include "w2kui.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ICON_CELL_W   75
#define ICON_CELL_H   75
#define ICON_TOP      12
#define ICON_LEFT      8
#define LABEL_PAD      3

static Window dw;
static Pixmap wall;                 /* rendered wallpaper, screen-sized */
static int    sel = -1;             /* the focused icon */
static Time   last_click;
static int    last_click_idx = -1;

/* ------------------------------------------------------------------ *
 * The icons
 * ------------------------------------------------------------------ *
 * Four system icons that are always there, then whatever is in ~/Desktop:
 * .desktop files (which is what a shortcut is here), plus ordinary files
 * and folders. Each icon has a cell position, saved to ~/.w2k/desktop so
 * the arrangement survives a restart.
 *
 * The list is rebuilt from the directory on demand -- creating a shortcut
 * is writing a file, and deleting one is deleting it, so the directory is
 * the model and there is nothing to keep in sync. */
#define MAX_ICONS 128

typedef struct {
    char label[128];
    char cmd[512];              /* what to run; empty for the bin  */
    char path[1024];            /* the file behind it, if any      */
    int  icon;
    int  system;                /* one of the four built-ins       */
    int  col, row;              /* cell, -1 = not placed yet       */
    int  x, y;                  /* pixel position, filled by layout */
} DeskIcon;

static DeskIcon icons[MAX_ICONS];
static int      nicons;
#define NICONS nicons

static char picked[MAX_ICONS];      /* which icons are selected */

static const struct { const char *label; int icon; const char *cmd; }
system_icons[] = {
    { "My Computer",       ICO_MYCOMPUTER, "l2kexplorer /" },
    { "My Documents",      ICO_MYDOCS,     "l2kexplorer ~" },
    { "My Network Places", ICO_NETWORK,    "l2kexplorer /net" },
    { "Recycle Bin",       ICO_RECYCLE,    NULL },
};
#define N_SYSTEM ((int)(sizeof system_icons / sizeof *system_icons))

static void desktop_dir(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/Desktop", home ? home : ".");
}

static void layout_path(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/.w2k/desktop", home ? home : ".");
}

/* An icon's key in the layout file: the file behind it where there is
 * one, so a rename in the shell or a change to "hide extensions" does
 * not lose its position, and the label for the system icons, which are
 * not files. */
static const char *layout_key(const DeskIcon *d)
{
    return d->path[0] ? d->path : d->label;
}

/* Remembered cell for one icon, by key. */
static void layout_load(void)
{
    char path[1024];
    layout_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        int col = 0, row = 0;
        if (sscanf(tab + 1, "%d %d", &col, &row) != 2) continue;
        int placed = 0;
        for (int i = 0; i < nicons && !placed; i++)
            if (!strcmp(layout_key(&icons[i]), line)) {
                icons[i].col = col;
                icons[i].row = row;
                placed = 1;
            }
        /* Files used to be keyed by label; honour those lines too, so an
         * existing desktop keeps its arrangement. */
        for (int i = 0; i < nicons && !placed; i++)
            if (icons[i].col < 0 && !strcmp(icons[i].label, line)) {
                icons[i].col = col;
                icons[i].row = row;
                placed = 1;
            }
    }
    fclose(f);
}

static void layout_save(void)
{
    char path[1024];
    layout_path(path, sizeof path);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }

    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Linux 2000 -- desktop icon positions\n");
    for (int i = 0; i < nicons; i++)
        fprintf(f, "%s\t%d %d\n", layout_key(&icons[i]), icons[i].col,
                icons[i].row);
    fclose(f);
}

/* Read the label and command out of a .desktop file. */
static int read_shortcut(const char *path, DeskIcon *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024], name[128] = "", exec[512] = "", icon[128] = "";
    int in_entry = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { in_entry = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in_entry) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(line, "Name") && !name[0])
            snprintf(name, sizeof name, "%.127s", eq + 1);
        else if (!strcmp(line, "Exec") && !exec[0])
            snprintf(exec, sizeof exec, "%.511s", eq + 1);
        else if (!strcmp(line, "Icon") && !icon[0])
            snprintf(icon, sizeof icon, "%.127s", eq + 1);
    }
    fclose(f);
    if (!name[0] || !exec[0]) return 0;

    /* Strip the field codes a .desktop Exec line may carry. */
    for (char *p = exec; *p; p++)
        if (p[0] == '%' && p[1]) { p[0] = 0; break; }

    snprintf(out->label, sizeof out->label, "%s", name);
    snprintf(out->cmd, sizeof out->cmd, "%s", exec);
    out->icon = icon[0] ? w2k_icon_by_name(icon) : ICO_APP;
    return 1;
}

/* Rebuild the icon list: the system four, then ~/Desktop. */
void desktop_scan(void)
{
    nicons = 0;
    memset(picked, 0, sizeof picked);

    for (int i = 0; i < N_SYSTEM && nicons < MAX_ICONS; i++) {
        DeskIcon *d = &icons[nicons++];
        memset(d, 0, sizeof *d);
        snprintf(d->label, sizeof d->label, "%s", system_icons[i].label);
        if (system_icons[i].cmd)
            snprintf(d->cmd, sizeof d->cmd, "%s", system_icons[i].cmd);
        d->icon = system_icons[i].icon;
        if (d->icon == ICO_RECYCLE && w2k_trash_count() > 0)
            d->icon = ICO_RECYCLE_FULL;
        d->system = 1;
        d->col = d->row = -1;
    }

    char dir[1024];
    desktop_dir(dir, sizeof dir);
    mkdir(dir, 0755);                       /* first run: make it */
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) && nicons < MAX_ICONS) {
            if (de->d_name[0] == '.' && !w2k_folder_hidden) continue;
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char full[2048];
            snprintf(full, sizeof full, "%s/%s", dir, de->d_name);

            DeskIcon *d = &icons[nicons];
            memset(d, 0, sizeof *d);
            d->col = d->row = -1;
            snprintf(d->path, sizeof d->path, "%.1023s", full);

            size_t len = strlen(de->d_name);
            /* A .desktop file is a shortcut only when marked executable
             * (the rule every desktop applies): anything else with the
             * name is an ordinary file, shown as one. */
            if (len > 8 && !strcmp(de->d_name + len - 8, ".desktop") &&
                access(full, X_OK) == 0) {
                if (!read_shortcut(full, d)) continue;
            } else {
                struct stat st;
                int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
                char shown[256];
                w2k_file_display_name(de->d_name, isdir, shown, sizeof shown);
                snprintf(d->label, sizeof d->label, "%.127s", shown);
                if (isdir) {
                    char q[2200];
                    w2k_shell_quote(full, q, sizeof q);
                    snprintf(d->cmd, sizeof d->cmd, "l2kexplorer %s", q);
                } else w2k_assoc_command(full, d->cmd, sizeof d->cmd);
                d->icon = w2k_file_icon_stat(full, de->d_name, isdir);
            }
            nicons++;
        }
        closedir(dp);
    }
    layout_load();
}

/* Rubber band, dragged across empty desktop. */
static int band_on, band_x0, band_y0, band_x1, band_y1;

/* Dragging an icon to another cell. */
static int drag_icon = -1, drag_dx, drag_dy, drag_moved;

Window desktop_window(void) { return dw; }

/* Icons sit on the primary monitor in a grid of cells. An icon that has
 * never been placed takes the first free cell, filling down then across --
 * the order Windows uses. */
/* The icon grid is laid out in logical pixels over the primary monitor's
 * work area; this is that area in the same units. */
static void workarea_l(int *x, int *y, int *w, int *h)
{
    int wx, wy, ww, wh;
    wm_workarea_of(w2k_monitor_primary(), &wx, &wy, &ww, &wh);
    *x = w2k_lp(wx); *y = w2k_lp(wy); *w = w2k_lp(ww); *h = w2k_lp(wh);
}

static int grid_rows(void)
{
    int wx, wy, ww, wh;
    workarea_l(&wx, &wy, &ww, &wh);
    int rows = (wh - ICON_TOP) / ICON_CELL_H;
    (void)wx; (void)wy; (void)ww;
    return rows < 1 ? 1 : rows;
}

static int cell_taken(int col, int row, int except)
{
    for (int i = 0; i < nicons; i++)
        if (i != except && icons[i].col == col && icons[i].row == row) return 1;
    return 0;
}

static void place_unplaced(void)
{
    int rows = grid_rows();
    for (int i = 0; i < nicons; i++) {
        if (icons[i].col >= 0 && icons[i].row >= 0) continue;
        for (int c = 0; c < 64; c++) {
            for (int r = 0; r < rows; r++)
                if (!cell_taken(c, r, i)) {
                    icons[i].col = c;
                    icons[i].row = r;
                    goto placed;
                }
        }
    placed: ;
    }
}

static void icon_rect(int i, int *x, int *y)
{
    int wx, wy, ww, wh;
    workarea_l(&wx, &wy, &ww, &wh);
    (void)ww; (void)wh;
    *x = wx + ICON_LEFT + icons[i].col * ICON_CELL_W;
    *y = wy + ICON_TOP  + icons[i].row * ICON_CELL_H;
}

/* Nearest free cell to a pixel position, for dropping a dragged icon. */
static void cell_at(int px, int py, int except, int *col, int *row)
{
    int wx, wy, ww, wh;
    workarea_l(&wx, &wy, &ww, &wh);
    (void)ww; (void)wh;
    int c = (px - wx - ICON_LEFT + ICON_CELL_W / 2) / ICON_CELL_W;
    int r = (py - wy - ICON_TOP + ICON_CELL_H / 2) / ICON_CELL_H;
    if (c < 0) c = 0;
    if (r < 0) r = 0;
    if (r >= grid_rows()) r = grid_rows() - 1;

    /* If that cell is occupied, spiral outward for the closest free one. */
    if (!cell_taken(c, r, except)) { *col = c; *row = r; return; }
    for (int d = 1; d < 32; d++)
        for (int dc = -d; dc <= d; dc++)
            for (int dr = -d; dr <= d; dr++) {
                int nc = c + dc, nr = r + dr;
                if (nc < 0 || nr < 0 || nr >= grid_rows()) continue;
                if (!cell_taken(nc, nr, except)) { *col = nc; *row = nr; return; }
            }
    *col = c;
    *row = r;
}

/* Desktop labels wrap to two lines and centre under the icon. */
static void draw_label(Drawable d, int cx, int y, const char *text, int selected)
{
    char line[2][64] = { { 0 }, { 0 } };
    int nlines = 1;
    int maxw = ICON_CELL_W - 6;

    if (w2k_text_width(F_ICON, text, -1) > maxw) {
        const char *sp = strrchr(text, ' ');
        /* Prefer the last space that still lets the first line fit. */
        for (const char *p = text; p; p = strchr(p + 1, ' ')) {
            if (*p != ' ' && p != text) continue;
            if (w2k_text_width(F_ICON, text, (int)(p - text)) <= maxw) sp = p;
        }
        if (sp && sp > text) {
            int n = (int)(sp - text);
            if (n > 63) n = 63;
            memcpy(line[0], text, n);
            snprintf(line[1], sizeof line[1], "%s", sp + 1);
            nlines = 2;
        } else {
            snprintf(line[0], sizeof line[0], "%.63s", text);
        }
    } else {
        snprintf(line[0], sizeof line[0], "%.63s", text);
    }

    int fh = w2k_font_height(F_ICON);
    for (int i = 0; i < nlines; i++) {
        int tw = w2k_text_width(F_ICON, line[i], -1);
        int tx = cx - tw / 2, ty = y + i * fh;
        /* With drop shadows on, the label sits on the wallpaper with a dark
         * copy behind it instead of on a filled plate. */
        if (!selected && w2k_effects[FX_ICON_SHADOW]) {
            w2k_text(d, F_ICON, tx + 1, ty + 1, line[i], C_BLACK);
        } else {
            w2k_fill(d, tx - LABEL_PAD, ty - 1, tw + 2 * LABEL_PAD, fh + 2,
                     selected ? C_HIGHLIGHT : C_DESKTOP);
        }
        w2k_text(d, F_ICON, tx, ty, line[i],
                 selected ? C_HIGHLIGHTTEXT : C_WHITE);
        if (selected && i == nlines - 1)
            w2k_focus_rect(d, tx - LABEL_PAD, y - 1,
                           tw + 2 * LABEL_PAD, fh * nlines + 2);
    }
}

/* Render the wallpaper once into a screen-sized pixmap (centre / tile /
 * stretch, nearest neighbour -- what the 2000 shell did too). */
static void build_wallpaper(void)
{
    if (wall) { w2k_free_pixmap(wall); wall = 0; }
    if (!w2k_wallpaper[0]) return;
    int iw, ih;
    unsigned char *rgba = w2k_image_load(w2k_wallpaper, &iw, &ih);   /* BMP, PNG or JPEG */
    if (!rgba || iw <= 0 || ih <= 0) { free(rgba); return; }

    wall = XCreatePixmap(w2k.dpy, w2k.root, w2k.sw, w2k.sh, w2k.depth);
    w2k_fill(wall, 0, 0, w2k.sw, w2k.sh, C_DESKTOP);

    /* Centre and stretch are resolved per monitor: a picture centred on the
     * union of three screens would sit across two bezels, and one stretched
     * to it would be unrecognisable. Tiling stays continuous across the lot,
     * which is what makes a tiled pattern look seamless. */
    for (int k = 0; k < w2k_monitor_count(); k++) {
        const W2kMonitor *m = w2k_monitor(k);
        int W = m->w, H = m->h;
        char *pixels = malloc((size_t)W * H * 4);
        XImage *im = pixels ? XCreateImage(w2k.dpy, w2k.visual, w2k.depth,
                                           ZPixmap, 0, pixels, W, H, 32, 0)
                            : NULL;
        if (!im) { free(pixels); break; }
        /* Fit keeps the picture's shape inside the monitor, Fill fills the
         * monitor with it and crops, Span stretches it over every monitor
         * at once. All in 16.16 fixed point, nearest neighbour. */
        int st = w2k_wallpaper_style;
        long fx = 0, fy = 0, ox = 0, oy = 0;    /* scale (source per dest) and offset */
        if (st == 3 || st == 4) {
            long sw = ((long)iw << 16) / W, shh = ((long)ih << 16) / H;
            long f = st == 3 ? (sw > shh ? sw : shh) : (sw < shh ? sw : shh);
            fx = fy = f;
            ox = (((long)iw << 16) - f * W) / 2;
            oy = (((long)ih << 16) - f * H) / 2;
        }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int sx, sy;
                if (st == 2) { sx = x * iw / W; sy = y * ih / H; }
                else if (st == 5) {
                    sx = (int)((long)(m->x + x) * iw / w2k.sw);
                    sy = (int)((long)(m->y + y) * ih / w2k.sh);
                }
                else if (st == 3 || st == 4) {
                    sx = (int)((fx * x + ox) >> 16);
                    sy = (int)((fy * y + oy) >> 16);
                }
                else if (st == 1) {
                    sx = (m->x + x) % iw; sy = (m->y + y) % ih;
                } else { sx = x - (W - iw) / 2; sy = y - (H - ih) / 2; }
                unsigned long px;
                if (sx < 0 || sy < 0 || sx >= iw || sy >= ih) px = w2k.col[C_DESKTOP];
                else {
                    const unsigned char *p = rgba + ((size_t)sy * iw + sx) * 4;
                    px = w2k_rgb(p[0], p[1], p[2]);
                }
                XPutPixel(im, x, y, px);
            }
        XPutImage(w2k.dpy, wall, w2k.gc, im, 0, 0, m->x, m->y, W, H);
        XDestroyImage(im);
    }
    free(rgba);
}

/* The wallpaper is the window's background, so the server paints the parts
 * no icon touches -- the other monitors included -- without us building a
 * pixmap the size of the whole virtual screen on every repaint. */
static void apply_background(void)
{
    if (wall) XSetWindowBackgroundPixmap(w2k.dpy, dw, wall);
    else      XSetWindowBackground(w2k.dpy, dw, w2k.col[C_DESKTOP]);
    XClearWindow(w2k.dpy, dw);
}

void desktop_reload(void)
{
    build_wallpaper();
    XSetWindowBackground(w2k.dpy, w2k.root, w2k.col[C_DESKTOP]);
    XClearWindow(w2k.dpy, w2k.root);
    if (dw) apply_background();
    desktop_paint();
}

void desktop_paint(void)
{
    if (!dw) return;
    /* Only the primary monitor carries icons, so only it needs an off-screen
     * buffer; everything else is already on screen as the window background. */
    const W2kMonitor *m = w2k_monitor_primary();
    int wx, wy, w, h;
    wm_workarea_of(m, &wx, &wy, &w, &h);

    /* The back buffer is kept between repaints. At 1680x1050 it is seven
     * megabytes; allocating and freeing that on every selection change --
     * and on every motion event while a selection rectangle is dragged --
     * was the most expensive thing the desktop did. */
    static Pixmap pm;
    static int pm_w, pm_h;
    if (pm && (pm_w != w || pm_h != h)) { w2k_free_pixmap(pm); pm = 0; }
    if (!pm) {
        pm = XCreatePixmap(w2k.dpy, dw, w, h, w2k.depth);
        pm_w = w;
        pm_h = h;
    }
    if (wall) XCopyArea(w2k.dpy, wall, pm, w2k.gc, wx, wy, w, h, 0, 0);
    else {
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_DESKTOP]);
        XFillRectangle(w2k.dpy, pm, w2k.gc, 0, 0, (unsigned)w, (unsigned)h);
    }

    /* From here on in logical pixels, relative to the work area. */
    int lx, ly, lw, lh;
    workarea_l(&lx, &ly, &lw, &lh);
    place_unplaced();
    for (int i = 0; i < NICONS; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        x -= lx; y -= ly;                 /* into the buffer's coordinates */
        int ix = x + (ICON_CELL_W - 32) / 2;
        if (picked[i]) {
            /* Selected desktop icons are tinted, not boxed. */
            w2k_bigicon_draw(pm, ix, y, icons[i].icon);
            XSetForeground(w2k.dpy, w2k.gc_dither, w2k.col[C_HIGHLIGHT]);
            XSetTSOrigin(w2k.dpy, w2k.gc_dither, 0, 0);
            XFillRectangle(w2k.dpy, pm, w2k.gc_dither, w2k_cx(ix), w2k_cx(y),
                           (unsigned)w2k_cw(ix, 32), (unsigned)w2k_cw(y, 32));
        } else {
            w2k_bigicon_draw(pm, ix, y, icons[i].icon);
        }
        draw_label(pm, x + ICON_CELL_W / 2, y + 36, icons[i].label, picked[i]);
    }

    /* The selection rectangle. Translucent means a 50% dither of the
     * highlight colour -- the closest an 8-bit-era desktop gets to alpha,
     * and what the effect meant before compositing. */
    if (band_on) {
        int x0 = (band_x0 < band_x1 ? band_x0 : band_x1) - lx;
        int y0 = (band_y0 < band_y1 ? band_y0 : band_y1) - ly;
        int bw = band_x0 < band_x1 ? band_x1 - band_x0 : band_x0 - band_x1;
        int bh = band_y0 < band_y1 ? band_y1 - band_y0 : band_y0 - band_y1;
        if (bw > 1 && bh > 1) {
            if (w2k_effects[FX_TRANSLUCENT_SEL]) {
                XSetForeground(w2k.dpy, w2k.gc_dither, w2k.col[C_HIGHLIGHT]);
                XSetTSOrigin(w2k.dpy, w2k.gc_dither, 0, 0);
                XFillRectangle(w2k.dpy, pm, w2k.gc_dither, w2k_cx(x0), w2k_cx(y0),
                               (unsigned)w2k_cw(x0, bw), (unsigned)w2k_cw(y0, bh));
                w2k_frame(pm, x0, y0, bw, bh, C_HIGHLIGHT);
            } else {
                w2k_focus_rect(pm, x0, y0, bw, bh);
            }
        }
    }

    XCopyArea(w2k.dpy, pm, dw, w2k.gc, 0, 0, w, h, wx, wy);
}

/* Select every icon the band touches. */
static void band_select(void)
{
    int bx = band_x0 < band_x1 ? band_x0 : band_x1;
    int by = band_y0 < band_y1 ? band_y0 : band_y1;
    int bw = band_x0 < band_x1 ? band_x1 - band_x0 : band_x0 - band_x1;
    int bh = band_y0 < band_y1 ? band_y1 - band_y0 : band_y0 - band_y1;

    sel = -1;
    for (int i = 0; i < NICONS; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        int hit = x < bx + bw && bx < x + ICON_CELL_W &&
                  y < by + bh && by < y + ICON_CELL_H;
        picked[i] = (char)hit;
        if (hit && sel < 0) sel = i;
    }
}

void desktop_init(void)
{
    if (!nicons) desktop_scan();       /* icons from ~/Desktop, plus the four */
    /* The desktop covers every monitor; the taskbar is a separate window
     * stacked above it, so no height needs subtracting here. */
    int w = w2k.sw, h = w2k.sh;
    if (dw) {
        XMoveResizeWindow(w2k.dpy, dw, 0, 0, w, h);
        build_wallpaper();
        apply_background();
        desktop_paint();
        return;
    }
    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel  = w2k.col[C_DESKTOP],
        /* PointerMotion is for the icon tooltips; it only arrives while
         * the pointer is actually over the desktop. */
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      ButtonMotionMask | PointerMotionMask | LeaveWindowMask |
                      KeyPressMask
    };
    dw = XCreateWindow(w2k.dpy, w2k.root, 0, 0, w, h, 0, CopyFromParent,
                       InputOutput, CopyFromParent,
                       CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    XLowerWindow(w2k.dpy, dw);
    XMapWindow(w2k.dpy, dw);
    desktop_dnd_init();          /* files can be dropped here */
    apply_background();
    /* The root itself shows through at the very edges; match the colour. */
    XSetWindowBackground(w2k.dpy, w2k.root, w2k.col[C_DESKTOP]);
    XClearWindow(w2k.dpy, w2k.root);
    desktop_paint();
}

static int icon_at(int px, int py)
{
    for (int i = 0; i < NICONS; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        if (px >= x && px < x + ICON_CELL_W && py >= y && py < y + ICON_CELL_H)
            return i;
    }
    return -1;
}

enum { DM_ARRANGE = 1, DM_REFRESH, DM_NEWFOLDER, DM_PROPS, DM_EXPLORE,
       DM_OPEN, DM_TASKMGR, DM_EMPTYBIN,
       DM_NEW_SHORTCUT, DM_NEW_TEXT, DM_DELETE, DM_RENAME,
       DM_ARR_NAME, DM_ARR_TYPE, DM_LINEUP };

/* Desktop icons mostly launch a command; the Recycle Bin opens the trash. */
static void desktop_open(int i)
{
    if (icons[i].cmd[0]) { wm_spawn(icons[i].cmd); return; }
    char cmd[2400], q[2200];
    w2k_shell_quote(w2k_trash_files_dir(), q, sizeof q);
    snprintf(cmd, sizeof cmd, "l2kexplorer %s", q);
    wm_spawn(cmd);
}

/* The bin icon is full or empty, and nothing tells us when that changes:
 * a file trashed from Explorer is another process's doing. The desktop
 * therefore checks the count on its tick, which is a stat of one
 * directory and only repaints when the answer changes. */
/* The bin is looked at every couple of seconds rather than on every turn
 * of the loop: a stat is cheap, but nothing here changes that fast. */
#define BIN_POLL_MS 2000

static long next_bin_poll;

void desktop_bin_tick(void)
{
    static int last = -1;
    static time_t last_mtime;

    long now = w2k_now_ms();
    if (now < next_bin_poll) return;
    next_bin_poll = now + BIN_POLL_MS;
    /* Counting means a readdir; the directory's mtime says whether one is
     * worth doing, and four times a second that difference matters. */
    struct stat st;
    if (stat(w2k_trash_files_dir(), &st) != 0) return;
    if (last >= 0 && st.st_mtime == last_mtime) return;
    last_mtime = st.st_mtime;

    int full = w2k_trash_count() > 0;
    if (full == last) return;
    last = full;
    for (int i = 0; i < nicons; i++)
        if (icons[i].system &&
            (icons[i].icon == ICO_RECYCLE || icons[i].icon == ICO_RECYCLE_FULL)) {
            icons[i].icon = full ? ICO_RECYCLE_FULL : ICO_RECYCLE;
            desktop_paint();
            break;
        }
}

static void empty_recycle_bin(void)
{
    int n = w2k_trash_count();
    if (n <= 0) {
        w2k_msgbox(NULL, "Recycle Bin", "The Recycle Bin is already empty.",
                   MB_OK | MB_ICONINFO);
        return;
    }
    char msg[256];
    snprintf(msg, sizeof msg,
             "Are you sure you want to delete %s?\n\nThis cannot be undone.",
             n == 1 ? "this item" : "these items");
    char title[64];
    snprintf(title, sizeof title, "Confirm Delete");
    if (w2k_msgbox(NULL, title, msg, MB_YESNO | MB_ICONWARNING) != ID_YES)
        return;
    w2k_trash_empty();
    desktop_bin_tick();
}

/* ------------------------------------------------------------------ *
 * Drag and drop
 * ------------------------------------------------------------------ *
 * Files dropped on the desktop move into ~/Desktop; icons dragged off it
 * are offered to whatever is under the pointer. Both go through XDND, so
 * dragging works with Explorer and with anything else on the system.
 *
 * An icon drag only leaves the desktop once the pointer does -- inside it,
 * dragging moves the icon between cells, which is the more common thing to
 * want. */
static int dnd_out;        /* an icon drag has left the desktop */

void desktop_dnd_drop(int x, int y, const char *uris, int move)
{
    (void)x; (void)y;
    char dir[1024];
    desktop_dir(dir, sizeof dir);

    char paths[64][1024];
    int n = w2k_uri_list_paths(uris, paths, 64);
    /* Ctrl held copies, as in Windows; otherwise the source's choice. */
    Window rw, cw;
    int rxp, ryp, wx, wy;
    unsigned mask = 0;
    XQueryPointer(w2k.dpy, w2k.root, &rw, &cw, &rxp, &ryp, &wx, &wy, &mask);
    if (mask & ControlMask) move = 0;
    int done = w2k_fs_transfer(paths, n, dir, move, NULL, NULL);
    char urls[16][1024];
    int nu = w2k_uri_list_urls(uris, urls, 16);
    for (int i = 0; i < nu; i++) done += w2k_fs_write_url_shortcut(dir, urls[i]);
    if (!done) return;
    desktop_scan();
    /* The first thing dropped lands in the cell under the pointer. */
    for (int i = 0; i < n; i++) {
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        char to[2100];
        snprintf(to, sizeof to, "%.1023s/%.512s", dir, base);
        for (int k = 0; k < nicons; k++)
            if (!strcmp(icons[k].path, to)) {
                int col, row;
                cell_at(x, y, k, &col, &row);
                icons[k].col = col;
                icons[k].row = row;
                layout_save();
                i = n;
                break;
            }
    }
    desktop_paint();
}

void desktop_dnd_init(void) { w2k_dnd_accept(dw); }

/* ------------------------------------------------------------------ *
 * Desktop commands
 * ------------------------------------------------------------------ */
static void refresh_now(void)
{
    desktop_scan();
    desktop_paint();
}

static void new_folder(void)
{
    char dir[1024], name[256] = "New Folder", full[1400];
    desktop_dir(dir, sizeof dir);
    if (!w2k_prompt(NULL, "New Folder", "&Folder name:", name, name,
                    sizeof name, ICO_FOLDER))
        return;
    snprintf(full, sizeof full, "%s/%s", dir, name);
    if (mkdir(full, 0755) != 0)
        w2k_msgbox(NULL, "New Folder", "That folder could not be created.",
                   MB_OK | MB_ICONERROR);
    refresh_now();
}

static void new_text_document(void)
{
    char dir[1024], name[256] = "New Text Document.txt", full[1400];
    desktop_dir(dir, sizeof dir);
    if (!w2k_prompt(NULL, "New Text Document", "&File name:", name, name,
                    sizeof name, ICO_FILE_TEXT))
        return;
    snprintf(full, sizeof full, "%s/%s", dir, name);
    FILE *f = fopen(full, "wx");
    if (f) fclose(f);
    else w2k_msgbox(NULL, "New Text Document",
                    "That file already exists, or could not be created.",
                    MB_OK | MB_ICONERROR);
    refresh_now();
}

/* A shortcut here is a .desktop file, which is what everything else on the
 * system already understands. */
static void new_shortcut(void)
{
    char cmd[512] = "";
    if (!w2k_prompt(NULL, "Create Shortcut",
                    "Type the &location of the item:", "", cmd, sizeof cmd,
                    ICO_APP))
        return;
    if (!cmd[0]) return;

    /* Name it after the program, without its path or arguments. */
    char name[256];
    snprintf(name, sizeof name, "%s", cmd);
    char *sp = strchr(name, ' ');
    if (sp) *sp = 0;
    const char *base = strrchr(name, '/');
    if (base) memmove(name, base + 1, strlen(base));
    if (name[0] >= 'a' && name[0] <= 'z') name[0] -= 32;
    if (!w2k_prompt(NULL, "Create Shortcut", "Type a &name for the shortcut:",
                    name, name, sizeof name, ICO_APP))
        return;

    char dir[1024], full[1400];
    desktop_dir(dir, sizeof dir);
    snprintf(full, sizeof full, "%s/%s.desktop", dir, name);
    FILE *f = fopen(full, "w");
    if (!f) {
        w2k_msgbox(NULL, "Create Shortcut", "The shortcut could not be created.",
                   MB_OK | MB_ICONERROR);
        return;
    }
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n"
               "Terminal=false\n", name, cmd);
    fclose(f);
    chmod(full, 0755);
    refresh_now();
}

static void delete_icon(int i)
{
    if (i < 0 || i >= nicons || icons[i].system || !icons[i].path[0]) return;
    char msg[1200];
    snprintf(msg, sizeof msg,
             "Are you sure you want to send '%s' to the Recycle Bin?",
             icons[i].label);
    if (w2k_msgbox(NULL, "Confirm Delete", msg,
                   MB_YESNO | MB_ICONQUESTION) != ID_YES)
        return;
    if (w2k_trash_move(icons[i].path) != 0)
        w2k_msgbox(NULL, "Delete", "That item could not be deleted.",
                   MB_OK | MB_ICONERROR);
    refresh_now();
}

static void rename_icon(int i)
{
    if (i < 0 || i >= nicons || icons[i].system || !icons[i].path[0]) return;
    const char *base = strrchr(icons[i].path, '/');
    base = base ? base + 1 : icons[i].path;
    char name[256];
    snprintf(name, sizeof name, "%.255s", base);
    if (!w2k_prompt(NULL, "Rename", "&New name:", name, name, sizeof name,
                    ICO_NONE))
        return;

    char dir[1024], to[1400];
    desktop_dir(dir, sizeof dir);
    snprintf(to, sizeof to, "%s/%s", dir, name);
    if (rename(icons[i].path, to) != 0)
        w2k_msgbox(NULL, "Rename", "That item could not be renamed.",
                   MB_OK | MB_ICONERROR);
    refresh_now();
}

/* Sort into the grid: by name, or by type then name. */
static int cmp_name(const void *a, const void *b)
{
    const DeskIcon *x = a, *y = b;
    if (x->system != y->system) return y->system - x->system;
    return strcasecmp(x->label, y->label);
}

static int cmp_type(const void *a, const void *b)
{
    const DeskIcon *x = a, *y = b;
    if (x->system != y->system) return y->system - x->system;
    if (x->icon != y->icon) return x->icon - y->icon;
    return strcasecmp(x->label, y->label);
}

static void arrange_icons(int by_type)
{
    qsort(icons, (size_t)nicons, sizeof *icons, by_type ? cmp_type : cmp_name);
    int rows = grid_rows();
    for (int i = 0; i < nicons; i++) {
        icons[i].col = i / rows;
        icons[i].row = i % rows;
    }
    layout_save();
    desktop_paint();
}

/* Snap what is there to the grid without reordering anything. */
static void line_up_icons(void)
{
    for (int i = 0; i < nicons; i++) {
        int col = icons[i].col, row = icons[i].row;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        icons[i].col = col;
        icons[i].row = row;
    }
    layout_save();
    desktop_paint();
}

static void context_menu(int x, int y, int over)
{
    W2kMenu *m = w2k_menu_new();
    if (over >= 0) {
        w2k_menu_item(m, DM_OPEN, "&Open", NULL, ICO_NONE);
        w2k_menu_default(m);
        if (icons[over].icon == ICO_RECYCLE ||
            icons[over].icon == ICO_RECYCLE_FULL) {
            w2k_menu_sep(m);
            w2k_menu_item(m, DM_EMPTYBIN, "&Empty Recycle Bin", NULL, ICO_DELETE);
            if (w2k_trash_count() <= 0) w2k_menu_disable(m);
        }
        /* Only what came out of ~/Desktop can be renamed or deleted; the
         * four system icons are not files. */
        if (!icons[over].system) {
            w2k_menu_sep(m);
            w2k_menu_item(m, DM_DELETE, "&Delete", "Del", ICO_DELETE);
            w2k_menu_item(m, DM_RENAME, "Rena&me", "F2", ICO_NONE);
        }
        w2k_menu_sep(m);
        w2k_menu_item(m, DM_PROPS, "P&roperties", NULL, ICO_NONE);
        /* A shortcut on the desktop is a file and has a property sheet;
         * the four system icons are not files. My Computer's sheet is the
         * display settings, which is where Windows sends it too. */
        if (icons[over].system && icons[over].icon != ICO_MYCOMPUTER)
            w2k_menu_disable(m);
    } else {
        W2kMenu *arr = w2k_menu_new();
        w2k_menu_item(arr, DM_ARR_NAME, "by &Name", NULL, ICO_NONE);
        w2k_menu_item(arr, DM_ARR_TYPE, "by &Type", NULL, ICO_NONE);
        w2k_menu_sep(arr);
        w2k_menu_item(arr, DM_LINEUP, "&Line up Icons", NULL, ICO_NONE);
        w2k_menu_sub(m, "Arra&nge Icons", ICO_NONE, arr);
        w2k_menu_item(m, DM_REFRESH, "Re&fresh", NULL, ICO_NONE);
        w2k_menu_sep(m);

        W2kMenu *nw = w2k_menu_new();
        w2k_menu_item(nw, DM_NEWFOLDER, "&Folder", NULL, ICO_FOLDER);
        w2k_menu_item(nw, DM_NEW_SHORTCUT, "&Shortcut", NULL, ICO_APP);
        w2k_menu_item(nw, DM_NEW_TEXT, "&Text Document", NULL, ICO_FILE_TEXT);
        w2k_menu_sub(m, "Ne&w", ICO_NONE, nw);

        w2k_menu_sep(m);
        w2k_menu_item(m, DM_TASKMGR, "&Task Manager", NULL, ICO_TASKMGR);
        w2k_menu_sep(m);
        w2k_menu_item(m, DM_PROPS, "P&roperties", NULL, ICO_SETTINGS);
    }
    int id = w2k_menu_popup(m, x, y, MPOP_LEFT);
    w2k_menu_free(m);

    switch (id) {
    case DM_REFRESH:   desktop_reload(); break;
    case DM_PROPS:
        if (over >= 0 && !icons[over].system && icons[over].path[0]) {
            if (w2k_file_properties(NULL, icons[over].path)) desktop_reload();
        } else {
            wm_spawn("l2kdisplay");
        }
        break;
    case DM_TASKMGR:   wm_spawn("l2ktaskmgr"); break;
    case DM_OPEN:
    case DM_EXPLORE:   if (over >= 0) desktop_open(over); break;
    case DM_EMPTYBIN:  empty_recycle_bin(); break;
    case DM_NEWFOLDER: new_folder(); break;
    case DM_NEW_SHORTCUT: new_shortcut(); break;
    case DM_NEW_TEXT:  new_text_document(); break;
    case DM_DELETE:    delete_icon(over); break;
    case DM_RENAME:    rename_icon(over); break;
    case DM_ARR_NAME:  arrange_icons(0); break;
    case DM_ARR_TYPE:  arrange_icons(1); break;
    case DM_LINEUP:    line_up_icons(); break;
    }
}

/* Pointing at a desktop icon shows its name after half a second, when
 * "show pop-up description for folder and desktop items" is on. A file
 * whose name is elided in the label is the case that needs it. */
static int hover_icon = -1, tip_up;
static long hover_since;

static void desktop_hover_clear(void)
{
    hover_icon = -1;
    hover_since = 0;
    if (tip_up) { w2k_tooltip_hide(); tip_up = 0; }
}

/* The next moment the desktop needs waking: its own tooltip, or the bin
 * poll. */
int desktop_next_tick_ms(void)
{
    long now = w2k_now_ms();
    int wait = (int)(next_bin_poll > now ? next_bin_poll - now : 0);
    if (w2k_folder_tooltips && hover_icon >= 0 && hover_since && !tip_up) {
        int left = 500 - (int)(now - hover_since);
        if (left < 0) left = 0;
        if (left < wait) wait = left;
    }
    return wait;
}

void desktop_hover_tick(void)
{
    if (!w2k_folder_tooltips || hover_icon < 0 || tip_up || !hover_since) return;
    if (w2k_now_ms() - hover_since < 500) return;
    if (hover_icon >= nicons) { desktop_hover_clear(); return; }

    Window r, ch;
    int rx, ry, wx, wy;
    unsigned mask;
    if (!XQueryPointer(w2k.dpy, w2k.root, &r, &ch, &rx, &ry, &wx, &wy, &mask))
        return;
    /* The file name, not the elided label -- that is the point. */
    const char *text = icons[hover_icon].label;
    const char *slash = strrchr(icons[hover_icon].path, '/');
    if (slash && slash[1]) text = slash + 1;
    w2k_tooltip_show(text, rx + 12, ry + 20);
    tip_up = 1;
}

int desktop_event(XEvent *e)
{
    if (w2k_tooltip_event(e)) return 1;

    /* The desktop window sits at the root's origin, so its positions are
     * screen positions: the grid wants them logical. */
    if (w2k_ui_scale != 100 && dw) {
        if (e->type == MotionNotify && e->xmotion.window == dw) {
            e->xmotion.x = w2k_lp(e->xmotion.x);
            e->xmotion.y = w2k_lp(e->xmotion.y);
        } else if ((e->type == ButtonPress || e->type == ButtonRelease) &&
                   e->xbutton.window == dw) {
            e->xbutton.x = w2k_lp(e->xbutton.x);
            e->xbutton.y = w2k_lp(e->xbutton.y);
        }
    }

    if (e->type == MotionNotify && e->xmotion.window == dw &&
        drag_icon < 0 && !band_on) {
        int over = icon_at(e->xmotion.x, e->xmotion.y);
        if (over != hover_icon) {
            desktop_hover_clear();
            hover_icon = over;
            if (over >= 0) hover_since = w2k_now_ms();
        }
    }
    if (e->type == LeaveNotify && e->xcrossing.window == dw)
        desktop_hover_clear();

    if (!dw) return 0;
    if (w2k_dnd_event(e)) return 1;

    if (e->type == Expose && e->xexpose.window == dw) {
        if (e->xexpose.count == 0) desktop_paint();
        return 1;
    }
    /* Dragging an icon to a new cell. */
    if (e->type == MotionNotify && e->xmotion.window == dw && drag_icon >= 0) {
        XEvent tmp;
        while (XCheckTypedWindowEvent(w2k.dpy, dw, MotionNotify, &tmp))
            e = &tmp;

        /* Once the pointer leaves the icon area of the desktop, hand the
         * drag over to XDND so it can land in another window. */
        if (!dnd_out && icons[drag_icon].path[0]) {
            int wx, wy, ww, wh;
            wm_workarea_of(w2k_monitor_primary(), &wx, &wy, &ww, &wh);
            if (e->xmotion.x_root < wx || e->xmotion.x_root >= wx + ww ||
                e->xmotion.y_root < wy || e->xmotion.y_root >= wy + wh) {
                char paths[1][1024];
                snprintf(paths[0], sizeof paths[0], "%s", icons[drag_icon].path);
                char *uris = w2k_uri_list_build(paths, 1);
                if (uris) {
                    w2k_dnd_begin(dw, uris, 1);
                    free(uris);
                    dnd_out = 1;
                }
            }
        }
        if (dnd_out) {
            w2k_dnd_set_time(e->xmotion.time);
            w2k_dnd_motion(e->xmotion.x_root, e->xmotion.y_root);
            return 1;
        }

        int col, row;
        cell_at(e->xmotion.x - drag_dx, e->xmotion.y - drag_dy, drag_icon,
                &col, &row);
        if (col != icons[drag_icon].col || row != icons[drag_icon].row) {
            icons[drag_icon].col = col;
            icons[drag_icon].row = row;
            drag_moved = 1;
            desktop_paint();
        }
        return 1;
    }
    if (e->type == ButtonRelease && drag_icon >= 0) {
        if (dnd_out) {
            w2k_dnd_set_time(e->xbutton.time);
            if (!w2k_dnd_drop()) w2k_dnd_cancel();
            dnd_out = 0;
            desktop_scan();
            desktop_paint();
        } else if (drag_moved) {
            layout_save();
        }
        drag_icon = -1;
        drag_moved = 0;
        return 1;
    }
    if (e->type == MotionNotify && e->xmotion.window == dw && band_on) {
        /* Coalesce: only the newest position matters. */
        XEvent tmp;
        while (XCheckTypedWindowEvent(w2k.dpy, dw, MotionNotify, &tmp))
            e = &tmp;
        band_x1 = e->xmotion.x;
        band_y1 = e->xmotion.y;
        band_select();
        desktop_paint();
        return 1;
    }
    if (e->type == ButtonRelease && band_on) {
        band_on = 0;
        desktop_paint();
        return 1;
    }
    if (e->type == KeyPress && e->xkey.window == dw) {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Delete && sel >= 0) {
            delete_icon(sel);
            return 1;
        }
        if (ks == XK_F2 && sel >= 0) { rename_icon(sel); return 1; }
        if (ks == XK_F5) { refresh_now(); return 1; }
        if ((ks == XK_Return || ks == XK_KP_Enter) && sel >= 0) {
            desktop_open(sel);
            return 1;
        }
        return 0;
    }
    if (e->type != ButtonPress || e->xbutton.window != dw) return 0;

    int idx = icon_at(e->xbutton.x, e->xbutton.y);

    if (e->xbutton.button == Button3) {
        if (idx >= 0 && !picked[idx]) {
            memset(picked, 0, sizeof picked);
            picked[idx] = 1;
            sel = idx;
            desktop_paint();
        }
        context_menu(e->xbutton.x_root, e->xbutton.y_root, idx);
        return 1;
    }
    if (e->xbutton.button != Button1) return 1;

    /* Clicking the desktop takes focus away from any window -- and gives it
     * to the desktop, so Delete and F2 land here. */
    client_focus(NULL);
    XSetInputFocus(w2k.dpy, dw, RevertToPointerRoot, e->xbutton.time);

    if (idx < 0) {
        /* Empty desktop: start a selection rectangle. */
        memset(picked, 0, sizeof picked);
        sel = -1;
        band_on = 1;
        band_x0 = band_x1 = e->xbutton.x;
        band_y0 = band_y1 = e->xbutton.y;
        desktop_paint();
    } else if (!picked[idx] || idx != sel) {
        memset(picked, 0, sizeof picked);
        picked[idx] = 1;
        sel = idx;
        desktop_paint();
    }

    if (idx >= 0) {
        /* Pick it up: the drag only counts once the pointer actually moves,
         * so a plain click still selects and double-click still opens. */
        int ix, iy;
        icon_rect(idx, &ix, &iy);
        drag_icon = idx;
        drag_dx = e->xbutton.x - ix;
        drag_dy = e->xbutton.y - iy;
        drag_moved = 0;
    }

    if (idx >= 0 && idx == last_click_idx &&
        e->xbutton.time - last_click < 400) {
        last_click = 0;
        last_click_idx = -1;
        desktop_open(idx);
    } else {
        last_click = e->xbutton.time;
        last_click_idx = idx;
    }
    return 1;
}
