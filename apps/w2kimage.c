/* w2kimage.c -- Imaging: a picture viewer.
 *
 * Opens PNG, JPEG and BMP, fits the picture to the window (or shows it at
 * its own size), and steps through the rest of the folder with the arrow
 * keys, the way the Windows viewer does.
 *
 * The scaled copy is built once per size change into an XImage and cached,
 * so panning and repainting cost one XPutImage rather than a resample. */
#include "w2k.h"
#include "w2kui.h"

#define IMAGE_FILTERS \
    "All Picture Files|*.bmp;*.png;*.jpg;*.jpeg;*.gif;*.ico;*.xpm|" \
    "Bitmap Image (*.bmp)|*.bmp|PNG Image (*.png)|*.png|" \
    "JPEG Image (*.jpg)|*.jpg;*.jpeg|All Files (*.*)|*"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    ID_OPEN = 1, ID_CLOSE, ID_PREV, ID_NEXT, ID_FIT, ID_ACTUAL,
    ID_ZOOMIN, ID_ZOOMOUT, ID_ABOUT
};

#define STATUS_H  20

static struct {
    W2kWin     *win;
    W2kMenubar *mb;
    W2kStatus  *sb;

    char        path[1024];
    unsigned char *rgba;             /* the picture, as loaded */
    int         iw, ih;

    Pixmap      scaled;              /* what is on screen, at view size */
    int         sw, sh;              /* its size                        */
    int         fit;                 /* scale to the window             */
    double      zoom;                /* when not fitting                */

    char        dir[1024];           /* folder, for stepping through    */
    char      **siblings;
    int         nsib, sib_at;
} im;

/* ------------------------------------------------------------------ *
 * The picture
 * ------------------------------------------------------------------ */
static void drop_scaled(void)
{
    if (im.scaled) { w2k_free_pixmap(im.scaled); im.scaled = 0; }
    im.sw = im.sh = 0;
}

static W2kRect view_rect(void)
{
    return (W2kRect){ 2, MENUBAR_H + 2, im.win->w - 4,
                      im.win->h - MENUBAR_H - STATUS_H - 4 };
}

/* Size the picture should be drawn at, for the current view. */
static void target_size(int *tw, int *th)
{
    W2kRect v = view_rect();
    if (!im.iw || !im.ih) { *tw = *th = 0; return; }
    if (!im.fit) {
        /* The X protocol stops at 32767 a side, and a 16x photo would
         * want gigabytes: the zoom is what the picture can take. */
        double z = im.zoom, lim = 8192.0;
        if (im.iw * z > lim) z = lim / im.iw;
        if (im.ih * z > lim) z = lim / im.ih;
        *tw = (int)(im.iw * z);
        *th = (int)(im.ih * z);
    } else {
        double sx = (double)(v.w - 4) / im.iw, sy = (double)(v.h - 4) / im.ih;
        double s = sx < sy ? sx : sy;
        if (s > 1.0) s = 1.0;             /* never blow a small picture up */
        *tw = (int)(im.iw * s);
        *th = (int)(im.ih * s);
    }
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

/* Build the scaled pixmap. Box filter down, nearest up: a photo shrunk with
 * nearest neighbour crawls with artefacts, and pixel art blown up with a box
 * filter turns to mush. */
static void rescale(void)
{
    int tw, th;
    target_size(&tw, &th);
    if (!im.rgba || tw <= 0 || th <= 0) return;
    if (im.scaled && tw == im.sw && th == im.sh) return;
    drop_scaled();

    void *pixels = malloc((size_t)tw * th * 4);
    if (!pixels) return;
    im.scaled = XCreatePixmap(w2k.dpy, w2k.root, tw, th, w2k.depth);
    XImage *xi = XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap, 0,
                              pixels, tw, th, 32, 0);
    if (!xi) { free(pixels); drop_scaled(); return; }

    int shrink = (tw < im.iw || th < im.ih);
    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            unsigned long px;
            if (shrink) {
                int x0 = x * im.iw / tw, x1 = (x + 1) * im.iw / tw;
                int y0 = y * im.ih / th, y1 = (y + 1) * im.ih / th;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                long r = 0, g = 0, b = 0, n = 0;
                for (int sy = y0; sy < y1 && sy < im.ih; sy++)
                    for (int sx = x0; sx < x1 && sx < im.iw; sx++) {
                        const unsigned char *p =
                            im.rgba + ((size_t)sy * im.iw + sx) * 4;
                        r += p[0]; g += p[1]; b += p[2];
                        n++;
                    }
                if (!n) n = 1;
                px = w2k_rgb((int)(r / n), (int)(g / n), (int)(b / n));
            } else {
                int sx = x * im.iw / tw, sy = y * im.ih / th;
                const unsigned char *p =
                    im.rgba + ((size_t)sy * im.iw + sx) * 4;
                px = w2k_rgb(p[0], p[1], p[2]);
            }
            XPutPixel(xi, x, y, px);
        }
    }
    XPutImage(w2k.dpy, im.scaled, w2k.gc, xi, 0, 0, 0, 0, tw, th);
    XDestroyImage(xi);
    im.sw = tw;
    im.sh = th;
}

static void set_status(void)
{
    char b[256];
    const char *name = strrchr(im.path, '/');
    name = name ? name + 1 : im.path;
    if (!im.rgba) {
        w2k_status_set(im.sb, 0, "No picture");
        w2k_status_set(im.sb, 1, "");
        return;
    }
    snprintf(b, sizeof b, "%.200s", name);
    w2k_status_set(im.sb, 0, b);
    snprintf(b, sizeof b, "%d x %d", im.iw, im.ih);
    w2k_status_set(im.sb, 1, b);
    int tw, th;
    target_size(&tw, &th);
    snprintf(b, sizeof b, "%d%%", im.iw ? tw * 100 / im.iw : 100);
    w2k_status_set(im.sb, 2, b);
}

/* The other pictures in the same folder, so the arrow keys can step. */
static void scan_siblings(void)
{
    for (int i = 0; i < im.nsib; i++) free(im.siblings[i]);
    free(im.siblings);
    im.siblings = NULL;
    im.nsib = im.sib_at = 0;

    snprintf(im.dir, sizeof im.dir, "%s", im.path);
    char *slash = strrchr(im.dir, '/');
    if (!slash) { snprintf(im.dir, sizeof im.dir, "."); }
    else *slash = 0;

    DIR *dp = opendir(im.dir);
    if (!dp) return;
    int cap = 64;
    im.siblings = malloc((size_t)cap * sizeof *im.siblings);
    if (!im.siblings) { closedir(dp); return; }
    struct dirent *de;
    while ((de = readdir(dp))) {
        char full[2048];
        if (snprintf(full, sizeof full, "%s/%s", im.dir, de->d_name) >=
            (int)sizeof full) continue;
        if (!w2k_image_is_image(full)) continue;
        if (im.nsib == cap) {
            cap *= 2;
            char **g = realloc(im.siblings, (size_t)cap * sizeof *g);
            if (!g) break;
            im.siblings = g;
        }
        im.siblings[im.nsib++] = w2k_strdup(full);
    }
    closedir(dp);

    for (int i = 1; i < im.nsib; i++) {          /* name order */
        char *v = im.siblings[i];
        int k = i - 1;
        while (k >= 0 && strcmp(im.siblings[k], v) > 0) {
            im.siblings[k + 1] = im.siblings[k];
            k--;
        }
        im.siblings[k + 1] = v;
    }
    for (int i = 0; i < im.nsib; i++)
        if (!strcmp(im.siblings[i], im.path)) { im.sib_at = i; break; }
}

static void load(const char *path, int rescan)
{
    free(im.rgba);
    im.rgba = NULL;
    drop_scaled();
    snprintf(im.path, sizeof im.path, "%s", path);
    im.rgba = w2k_image_load(im.path, &im.iw, &im.ih);

    char title[1100];
    const char *name = strrchr(im.path, '/');
    name = name ? name + 1 : im.path;
    if (im.rgba) snprintf(title, sizeof title, "%s - Imaging", name);
    else         snprintf(title, sizeof title, "Imaging");
    if (im.win) w2k_win_title(im.win, title);

    if (!im.rgba && im.win) {
        char msg[1200];
        snprintf(msg, sizeof msg,
                 "%s\n\nThis file is not a picture Imaging can open.\n"
                 "PNG, JPEG and BMP are supported.", name);
        w2k_msgbox(im.win, "Imaging", msg, MB_OK | MB_ICONERROR);
    }
    if (rescan) scan_siblings();
    im.zoom = 1.0;
    set_status();
    if (im.win) w2k_win_dirty(im.win);
}

static void step(int delta)
{
    if (im.nsib < 2) return;
    im.sib_at = (im.sib_at + delta + im.nsib) % im.nsib;
    load(im.siblings[im.sib_at], 0);
}

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */
static void paint(W2kWin *w, Drawable d)
{
    w2k_menubar_draw(d, im.mb);

    W2kRect v = view_rect();
    w2k_edge(d, v.x, v.y, v.w, v.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, v.x + 2, v.y + 2, v.w - 4, v.h - 4, C_APPWORKSPACE);

    if (im.rgba) {
        rescale();
        if (im.scaled) {
            int x = v.x + 2 + (v.w - 4 - im.sw) / 2;
            int y = v.y + 2 + (v.h - 4 - im.sh) / 2;
            int sx = 0, sy = 0, cw = im.sw, ch = im.sh;
            /* Bigger than the view: show the middle of it. */
            if (cw > v.w - 4) { sx = (cw - (v.w - 4)) / 2; cw = v.w - 4; x = v.x + 2; }
            if (ch > v.h - 4) { sy = (ch - (v.h - 4)) / 2; ch = v.h - 4; y = v.y + 2; }
            XCopyArea(w2k.dpy, im.scaled, d, w2k.gc, sx, sy, cw, ch, x, y);
        }
    } else {
        const char *msg = "No picture open.  File - Open...";
        int tw = w2k_text_width(F_UI, msg, -1);
        w2k_text(d, F_UI, v.x + (v.w - tw) / 2,
                 v.y + v.h / 2 - w2k_font_height(F_UI) / 2, msg, C_WHITE);
    }
    w2k_status_draw(d, im.sb);
    (void)w;
}

/* ------------------------------------------------------------------ *
 * Menus and commands
 * ------------------------------------------------------------------ */
static W2kMenu *build_file(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_OPEN, "&Open...", "Ctrl+O", ICO_FOLDER);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_PREV, "&Previous Picture", "Left", ICO_BACK);
    if (im.nsib < 2) w2k_menu_disable(m);
    w2k_menu_item(m, ID_NEXT, "&Next Picture", "Right", ICO_FORWARD);
    if (im.nsib < 2) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_CLOSE, "E&xit", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_view(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_FIT, "&Fit to Window", NULL, ICO_NONE);
    w2k_menu_radio(m, im.fit);
    w2k_menu_item(m, ID_ACTUAL, "&Actual Size", NULL, ICO_NONE);
    w2k_menu_radio(m, !im.fit);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_ZOOMIN, "Zoom &In", "+", ICO_NONE);
    w2k_menu_item(m, ID_ZOOMOUT, "Zoom &Out", "-", ICO_NONE);
    return m;
}

static W2kMenu *build_help(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_ABOUT, "&About Imaging", NULL, ICO_INFO);
    return m;
}

static void do_open(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s", im.path[0] ? im.path : im.dir);
    if (w2k_file_dialog_filter(im.win, 0, path, sizeof path, IMAGE_FILTERS))
        load(path, 1);
}

static void command(void *u, int id)
{
    (void)u;
    switch (id) {
    case ID_OPEN:  do_open(); break;
    case ID_CLOSE: w2k_win_close(im.win, 0); break;
    case ID_PREV:  step(-1); break;
    case ID_NEXT:  step(1); break;
    case ID_FIT:   im.fit = 1; drop_scaled(); break;
    case ID_ACTUAL: im.fit = 0; im.zoom = 1.0; drop_scaled(); break;
    case ID_ZOOMIN:
        im.fit = 0;
        im.zoom *= 1.25;
        if (im.zoom > 16) im.zoom = 16;
        drop_scaled();
        break;
    case ID_ZOOMOUT:
        im.fit = 0;
        im.zoom /= 1.25;
        if (im.zoom < 0.05) im.zoom = 0.05;
        drop_scaled();
        break;
    case ID_ABOUT:
        w2k_msgbox(im.win, "About Imaging",
                   "Imaging\nWindows 2000 for X11\n\n"
                   "Opens PNG, JPEG and BMP pictures.",
                   MB_OK | MB_ICONINFO);
        break;
    }
    set_status();
    w2k_win_dirty(im.win);
}

static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress:
        if (w2k_menubar_press(im.mb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        return 1;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        int ctrl = (e->xkey.state & ControlMask) != 0;
        if (ctrl && (ks == XK_o || ks == XK_O)) { command(NULL, ID_OPEN); return 1; }
        switch (ks) {
        case XK_Escape:    w2k_win_close(w, 0); return 1;
        case XK_Left:  case XK_Prior:  command(NULL, ID_PREV); return 1;
        case XK_Right: case XK_Next:   command(NULL, ID_NEXT); return 1;
        case XK_plus:  case XK_equal:  case XK_KP_Add:
            command(NULL, ID_ZOOMIN); return 1;
        case XK_minus: case XK_KP_Subtract:
            command(NULL, ID_ZOOMOUT); return 1;
        case XK_f: case XK_F: command(NULL, ID_FIT); return 1;
        case XK_a: case XK_A: command(NULL, ID_ACTUAL); return 1;
        }
        if (w2k_menubar_key(im.mb, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void resized(W2kWin *w)
{
    im.mb->r = (W2kRect){ 0, 0, w->w, MENUBAR_H };
    im.sb->r = (W2kRect){ 0, w->h - STATUS_H, w->w, STATUS_H };
    drop_scaled();                       /* fit changes with the window */
}

int main(int argc, char **argv)
{
    if (w2k_init("w2kimage") < 0) return 1;

    im.fit = 1;
    im.zoom = 1.0;
    snprintf(im.dir, sizeof im.dir, "%s", getenv("HOME") ? getenv("HOME") : ".");

    im.win = w2k_win_new("Imaging", "w2kimage", 640, 480, 1);
    im.win->paint = paint;
    im.win->event = event;
    im.win->resized = resized;

    im.mb = w2k_menubar_new(NULL, command);
    w2k_menubar_add(im.mb, "&File", build_file);
    w2k_menubar_add(im.mb, "&View", build_view);
    w2k_menubar_add(im.mb, "&Help", build_help);

    im.sb = w2k_status_new();
    w2k_status_add(im.sb, 0);
    w2k_status_add(im.sb, 90);
    w2k_status_add(im.sb, 60);
    im.sb->sizegrip = 1;

    resized(im.win);
    if (argc > 1) load(argv[1], 1);
    else          set_status();

    w2k_win_center(im.win, NULL);
    w2k_win_show(im.win);
    w2k_run();
    w2k_fini();
    return 0;
}
