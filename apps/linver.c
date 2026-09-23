/* linver.c -- About Linux 2000, in the manner of winver.
 *
 * The dialog winver put up on Windows 2000: a banner across the top with
 * the logo and the product's name, an etched rule, the product's artwork
 * beside the version and copyright lines -- Windows had its flag there,
 * this has the Linux 2000 logo -- who the desktop is licensed to, another
 * rule, the memory line and OK. Here the banner carries the distribution's
 * own logo next to the name, and the lines tell you the version of Linux
 * 2000, the distribution, the kernel and the window manager.
 *
 * The distribution's logo is the icon /etc/os-release names (LOGO=), from
 * the icon theme first and the largest size there is, and only then from
 * /usr/share/pixmaps, where distributions keep wide wordmarks as well:
 * Fedora's fedora-logo.png is "fedora" in letters, and was squeezed into
 * a square. Whatever is found keeps its shape, fitted into the banner's
 * box, and its soft edges are blended into the banner's white. */
#include "w2kui.h"
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

#ifndef W2K_VERSION
#define W2K_VERSION "?"
#endif

#define DLG_W     460
#define BANNER_H  72
#define LOGO      64
#define ART_W     104               /* the Linux 2000 artwork, beside the lines */

static struct {
    W2kWin  *win;
    W2kRect  ok;
    int      down;
    W2kSkin *logo;                  /* the distribution's, fitted into LOGO square */
    int      logo_w, logo_h;
    W2kSkin *art;                   /* the Linux 2000 logo, ART_W wide */
    int      art_w, art_h;
    W2kFace *big;                   /* the banner's name */
    char     distro[128], distro_id[64], kernel[96], user[128], host[128];
    char     mem[64];
} lv;

/* ---- Facts ------------------------------------------------------------ */
static void os_release(char *pretty, int np, char *id, int ni, char *logo, int nl)
{
    pretty[0] = id[0] = logo[0] = 0;
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) f = fopen("/usr/lib/os-release", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (*v == '"') { v++; char *q = strrchr(v, '"'); if (q) *q = 0; }
        if (!strcmp(line, "PRETTY_NAME")) snprintf(pretty, (size_t)np, "%s", v);
        else if (!strcmp(line, "ID"))     snprintf(id, (size_t)ni, "%s", v);
        else if (!strcmp(line, "LOGO"))   snprintf(logo, (size_t)nl, "%s", v);
    }
    fclose(f);
}

/* Thousands separators, as winver printed the memory. */
static void with_commas(unsigned long n, char *out, int len)
{
    char raw[32];
    snprintf(raw, sizeof raw, "%lu", n);
    int l = (int)strlen(raw), o = 0;
    for (int i = 0; i < l && o < len - 1; i++) {
        if (i && (l - i) % 3 == 0 && o < len - 1) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = 0;
}

/* ---- Pictures ---------------------------------------------------------- */
/* `rgba` fitted into a bw by bh box with its shape kept, and laid over the
 * colour it is drawn on: a skin has only a hard mask, so the soft edge is
 * blended in here rather than cut off. */
static W2kSkin *fitted(const unsigned char *rgba, int iw, int ih, int bw, int bh,
                       int bg, int *ow, int *oh)
{
    int w = bw, h = (int)((long)ih * bw / iw);
    if (h > bh) { h = bh; w = (int)((long)iw * bh / ih); }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    unsigned char *sc = w == iw && h == ih ? NULL : w2k_rgba_resample(rgba, iw, ih, w, h, RS_CUBIC);
    /* No picture rather than the source read as if it were the fitted
     * size: a small logo enlarged would be read past its end. */
    if (!sc && (w != iw || h != ih)) return NULL;
    const unsigned char *src = sc ? sc : rgba;
    int r, g, b;
    w2k_color_rgb(bg, &r, &g, &b);
    unsigned char *out = w2k_alloc((size_t)w * (size_t)h * 4);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        const unsigned char *s = src + i * 4;
        unsigned char *o = out + i * 4;
        int a = s[3];
        o[0] = (unsigned char)((s[0] * a + r * (255 - a) + 127) / 255);
        o[1] = (unsigned char)((s[1] * a + g * (255 - a) + 127) / 255);
        o[2] = (unsigned char)((s[2] * a + b * (255 - a) + 127) / 255);
        o[3] = 255;
    }
    free(sc);
    W2kSkin *sk = w2k_skin_from_rgba(out, w, h);
    free(out);
    *ow = w;
    *oh = h;
    return sk;
}

/* The distribution's logo: the name os-release gives, then the usual
 * others, each looked for in the icon theme from the largest size down --
 * a square icon, shrunk sharp -- and only then in /usr/share/pixmaps. */
static unsigned char *load_logo(const char *logo_name, int *w, int *h)
{
    static const char *const sizes[] = { "256x256", "192x192", "128x128", "96x96",
                                         "72x72", "64x64", "48x48", "32x32", NULL };
    char names[5][80];
    int nn = 0;
    if (logo_name[0]) snprintf(names[nn++], 80, "%s", logo_name);
    if (lv.distro_id[0]) {
        snprintf(names[nn++], 80, "%s-logo-icon", lv.distro_id);
        snprintf(names[nn++], 80, "%s-logo", lv.distro_id);
        snprintf(names[nn++], 80, "%s", lv.distro_id);
    }
    snprintf(names[nn++], 80, "distributor-logo");
    char path[600];
    for (int i = 0; i < nn; i++)
        for (int s = 0; sizes[s]; s++) {
            snprintf(path, sizeof path, "/usr/share/icons/hicolor/%s/apps/%.80s.png", sizes[s], names[i]);
            unsigned char *rgba = w2k_image_load(path, w, h);
            if (rgba) return rgba;
        }
    for (int i = 0; i < nn; i++) {
        snprintf(path, sizeof path, "/usr/share/pixmaps/%.80s.png", names[i]);
        unsigned char *rgba = w2k_image_load(path, w, h);
        if (rgba) return rgba;
    }
    return NULL;
}

static void find_pictures(const char *logo_name)
{
    int w = 0, h = 0;
    unsigned char *rgba = load_logo(logo_name, &w, &h);
    if (rgba && w > 0 && h > 0) lv.logo = fitted(rgba, w, h, LOGO, LOGO, C_WINDOW, &lv.logo_w, &lv.logo_h);
    free(rgba);

    /* The Linux 2000 artwork (skins/l2logo.png, the README's): beside the
     * lines where winver had the flag, and in the banner when the
     * distribution has no logo of its own. */
    char path[1024];
    rgba = w2k_skin_path("l2logo.png", path, sizeof path) ? w2k_image_load(path, &w, &h) : NULL;
    if (rgba && w > 0 && h > 0) {
        lv.art = fitted(rgba, w, h, ART_W, ART_W, C_FACE, &lv.art_w, &lv.art_h);
        if (!lv.logo) lv.logo = fitted(rgba, w, h, LOGO, LOGO, C_WINDOW, &lv.logo_w, &lv.logo_h);
    }
    free(rgba);
}

static void gather(void)
{
    char logo_name[64];
    os_release(lv.distro, sizeof lv.distro, lv.distro_id, sizeof lv.distro_id,
               logo_name, sizeof logo_name);
    if (!lv.distro[0]) snprintf(lv.distro, sizeof lv.distro, "Linux");

    struct utsname u;
    if (uname(&u) == 0)
        snprintf(lv.kernel, sizeof lv.kernel, "%s %s (%s)", u.sysname, u.release, u.machine);
    else
        snprintf(lv.kernel, sizeof lv.kernel, "Linux");

    struct passwd *pw = getpwuid(getuid());
    const char *name = pw && pw->pw_gecos && pw->pw_gecos[0] ? pw->pw_gecos :
                       pw && pw->pw_name ? pw->pw_name : "you";
    snprintf(lv.user, sizeof lv.user, "%s", name);
    char *comma = strchr(lv.user, ',');
    if (comma) *comma = 0;
    if (gethostname(lv.host, sizeof lv.host - 1) != 0) snprintf(lv.host, sizeof lv.host, "localhost");

    unsigned long kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) break;
        fclose(f);
    }
    with_commas(kb, lv.mem, sizeof lv.mem);

    find_pictures(logo_name);
}

/* ---- The dialog -------------------------------------------------------- */
static void etched(Drawable d, int x, int y, int w)
{
    w2k_hline(d, x, y, w, C_SHADOW);
    w2k_hline(d, x, y + 1, w, C_HILIGHT);
}

static void paint(W2kWin *w, Drawable d)
{
    int fh = w2k_font_height(F_UI);
    int lh = fh > 13 ? fh : 13;

    /* The banner: white, the logo, the name in large type and the line
     * under it, the way "Built on NT Technology" sat under the name. */
    w2k_fill(d, 0, 0, w->w, BANNER_H, C_WINDOW);
    if (lv.logo)
        w2k_skin_draw(d, lv.logo, 16 + (LOGO - lv.logo_w) / 2,
                      (BANNER_H - lv.logo_h) / 2, 0, 0, lv.logo_w, lv.logo_h);
    else         w2k_bigicon_draw(d, 32, (BANNER_H - 32) / 2, ICO_STARTFLAG);
    int tx = 16 + LOGO + 18;
    if (lv.big) {
        int asc = w2k_face_ascent(lv.big);
        w2k_face_text(d, lv.big, tx, 14 + (26 - asc), "Linux 2000", C_WINDOWTEXT);
    } else {
        w2k_text(d, F_UI_BOLD, tx, 18, "Linux 2000", C_WINDOWTEXT);
    }
    w2k_text(d, F_UI, tx + 1, 48, "A Windows 2000-style desktop for X11", C_WINDOWTEXT);
    etched(d, 0, BANNER_H, w->w);

    /* The Linux 2000 logo, and the lines beside it. */
    int y = BANNER_H + 14;
    int x = 64;
    if (lv.art) {
        w2k_skin_draw(d, lv.art, 16, y + 2, 0, 0, lv.art_w, lv.art_h);
        x = 16 + ART_W + 16;
    } else {
        w2k_bigicon_draw(d, 16, y, ICO_STARTFLAG);
    }
    char buf[256];
    w2k_text(d, F_UI, x, y, "Linux 2000", C_TEXT);                       y += lh;
    snprintf(buf, sizeof buf, "Version %s", W2K_VERSION);
    /* "1.7.0+6b51866" reads as "Version 1.7.0 (Build 6b51866)". */
    char *plus = strchr(buf, '+');
    if (plus) { char build[32]; snprintf(build, sizeof build, "%s", plus + 1);
                snprintf(plus, sizeof buf - (size_t)(plus - buf), " (Build %s)", build); }
    w2k_text(d, F_UI, x, y, buf, C_TEXT);                                y += lh;
    w2k_text(d, F_UI, x, y, "Copyright (C) 2026 the Linux 2000 contributors", C_TEXT); y += lh;
    y += lh / 2;
    snprintf(buf, sizeof buf, "Running on %s", lv.distro);
    w2k_text(d, F_UI, x, y, buf, C_TEXT);                                y += lh;
    w2k_text(d, F_UI, x, y, lv.kernel, C_TEXT);                          y += lh;
    w2k_text(d, F_UI, x, y, "Window manager: l2kwm, drawn with Xlib and nothing else", C_TEXT); y += lh;
    y += lh / 2;
    w2k_text(d, F_UI, x, y, "This desktop is licensed to:", C_TEXT);    y += lh;
    w2k_text(d, F_UI, x + 16, y, lv.user, C_TEXT);                       y += lh;
    w2k_text(d, F_UI, x + 16, y, lv.host, C_TEXT);                       y += lh;
    y += 6;
    etched(d, 16, y, w->w - 32);
    y += 10;
    snprintf(buf, sizeof buf, "Physical memory available to Linux 2000:");
    w2k_text(d, F_UI, x, y, buf, C_TEXT);
    snprintf(buf, sizeof buf, "%s KB", lv.mem);
    w2k_text(d, F_UI, w->w - 16 - w2k_text_width(F_UI, buf, -1), y, buf, C_TEXT);
    y += lh + 6;
    etched(d, 16, y, w->w - 32);
    y += 8;
    w2k_text(d, F_UI, 16, y, "Not affiliated with, endorsed by or sponsored by Microsoft.", C_GRAYTEXT);
    y += lh;
    w2k_text(d, F_UI, 16, y, "Windows is a trademark of Microsoft Corporation.", C_GRAYTEXT);

    w2k_draw_pushbutton(d, &lv.ok, "OK", BS_DEFAULT | (lv.down ? BS_PRESSED : 0));
}

static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress:
        if (w2k_rect_hit(&lv.ok, e->xbutton.x, e->xbutton.y)) lv.down = 1;
        w2k_win_dirty(w);
        return 1;
    case ButtonRelease:
        if (lv.down && w2k_rect_hit(&lv.ok, e->xbutton.x, e->xbutton.y))
            w2k_win_close(w, ID_OK);
        lv.down = 0;
        w2k_win_dirty(w);
        return 1;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter || ks == XK_space)
            w2k_win_close(w, ID_OK);
        return 1;
    }
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (w2k_init("linver") < 0) return 1;
    gather();
    lv.big = w2k_face_open_bold("Tahoma", 26);

    /* Tall enough for every line drawn in paint(). */
    int lh = w2k_font_height(F_UI) > 13 ? w2k_font_height(F_UI) : 13;
    int h = BANNER_H + 14 + lh * 10 + lh + 6 + 10 + lh + 6 + 8 + lh * 2 + 16 + 23 + 12;
    lv.win = w2k_win_new("About Linux 2000", "linver", DLG_W, h, 0);
    lv.win->paint = paint;
    lv.win->event = event;
    lv.ok = (W2kRect){ DLG_W - 12 - 75, h - 12 - 23, 75, 23 };

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, lv.win->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_win_center(lv.win, NULL);
    w2k_win_show(lv.win);
    w2k_run();
    w2k_face_close(lv.big);
    w2k_skin_free(lv.logo);
    w2k_skin_free(lv.art);
    w2k_fini();
    return 0;
}
