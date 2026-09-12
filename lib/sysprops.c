/* sysprops.c -- System Properties, the sheet sysdm.cpl put up.
 *
 * General: the monitor with the Linux 2000 logo on it, and beside it
 * what the system is, who it is registered to and what the computer is --
 * the processor, the machine and the memory, in the words and the K
 * Windows used.
 * Network Identification: the computer's name and workgroup. Hardware:
 * Device Manager. User Profiles: the accounts on this computer. Advanced:
 * Performance Options and the environment. What has no counterpart here
 * (the Hardware wizard, driver signing, profiles, startup and recovery)
 * is shown and greyed, as Windows greyed what a user could not change. */
#include "w2k.h"
#include "w2kui.h"
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef W2K_VERSION
#define W2K_VERSION "?"
#endif

extern char **environ;

#define SP_W 404
#define SP_H 432

enum { B_OK = 1, B_CANCEL, B_APPLY, B_DEVMGR, B_PERF, B_ENV,
       B_WIZARD, B_SIGNING, B_PROFILES, B_STARTUP, B_NETID, B_RENAME,
       B_PDELETE, B_PCHANGE, B_PCOPY, NBTN };

typedef struct {
    W2kWin  *w;
    W2kTabs *tabs;
    W2kList *users;
    W2kRect  b[NBTN];
    int      down;
    W2kSkin *monitor;
    char     distro[128], version[64], kernel[96];
    char     owner[128], host[128];
    char     cpu[3][96];
    int      ncpu;
    char     machine[128], ram[64];
    char     fqdn[256], workgroup[64];
} SysDlg;

/* Run a program without waiting for it, twice forked so it is never a
 * zombie of ours. */
static void spawn(const char *cmd)
{
    pid_t p = fork();
    if (p < 0) return;
    if (p == 0) {
        if (fork() == 0) {
            signal(SIGPIPE, SIG_DFL);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(p, NULL, 0);
}

/* ------------------------------------------------------------------ *
 * The facts
 * ------------------------------------------------------------------ */
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

/* One line of a small file, trimmed; empty when it is missing. */
static void read_line(const char *path, char *out, int n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, n, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
    int l = (int)strlen(out);
    while (l > 0 && out[l - 1] == ' ') out[--l] = 0;
}

/* The firmware's filler for a field nobody set. */
static int dmi_blank(const char *s)
{
    return !*s || !strcasecmp(s, "None") || !strcasecmp(s, "Default string") ||
           !strcasecmp(s, "System Product Name") || !strcasecmp(s, "Not Specified") ||
           strstr(s, "To be filled") || strstr(s, "O.E.M.");
}

/* Wrap a line into at most `max` pieces no wider than `w`. */
static int wrap(const char *text, int w, char out[][96], int max)
{
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        while (*p == ' ') p++;
        int fit = 0, last_space = -1;
        for (int i = 0; p[i] && i < 95; i++) {
            if (p[i] == ' ') last_space = i;
            if (w2k_text_width(F_UI, p, i + 1) > w) break;
            fit = i + 1;
        }
        if (!p[fit]) { snprintf(out[n++], 96, "%s", p); break; }
        int cut = last_space > 0 && last_space < fit ? last_space : fit;
        if (cut <= 0) cut = fit > 0 ? fit : 1;
        snprintf(out[n++], 96, "%.*s", cut, p);
        p += cut;
    }
    return n;
}

static void gather(SysDlg *s, int textw)
{
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) f = fopen("/usr/lib/os-release", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "PRETTY_NAME=", 12)) continue;
            char *v = line + 12;
            v[strcspn(v, "\r\n")] = 0;
            if (*v == '"') { v++; char *q = strrchr(v, '"'); if (q) *q = 0; }
            snprintf(s->distro, sizeof s->distro, "%s", v);
        }
        fclose(f);
    }
    if (!s->distro[0]) snprintf(s->distro, sizeof s->distro, "Linux");

    /* "1.28.0+4755599" is Linux 2000 1.28.0, build 4755599. */
    char ver[64];
    snprintf(ver, sizeof ver, "%s", W2K_VERSION);
    char *plus = strchr(ver, '+');
    if (plus) *plus = 0;
    snprintf(s->version, sizeof s->version, "Linux 2000 %s", ver);
    struct utsname u;
    if (uname(&u) == 0) snprintf(s->kernel, sizeof s->kernel, "Kernel %s", u.release);

    struct passwd *pw = getpwuid(getuid());
    const char *name = pw && pw->pw_gecos && pw->pw_gecos[0] ? pw->pw_gecos :
                       pw && pw->pw_name ? pw->pw_name : "Owner";
    snprintf(s->owner, sizeof s->owner, "%s", name);
    char *comma = strchr(s->owner, ',');
    if (comma) *comma = 0;
    if (gethostname(s->host, sizeof s->host - 1) != 0) snprintf(s->host, sizeof s->host, "localhost");

    /* The processor, as /proc/cpuinfo names it, its spaces tidied. */
    char cpu[256] = "";
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "model name", 10) && strncmp(line, "Model", 5) &&
                strncmp(line, "Hardware", 8)) continue;
            char *c = strchr(line, ':');
            if (!c) continue;
            int o = 0;
            for (char *p = c + 1; *p && *p != '\n' && o < (int)sizeof cpu - 1; p++)
                if (*p != ' ' || (o && cpu[o - 1] != ' ')) cpu[o++] = *p;
            while (o && cpu[o - 1] == ' ') o--;
            cpu[o] = 0;
            if (cpu[0]) break;
        }
        fclose(f);
    }
    if (!cpu[0]) snprintf(cpu, sizeof cpu, "%s", uname(&u) == 0 ? u.machine : "Unknown processor");
    s->ncpu = wrap(cpu, textw, s->cpu, 3);

    /* The machine: its maker and model from the firmware, where Windows
     * named the kind of PC. A laptop's product name is often a part
     * number; its version or family is the name on the lid. */
    char vendor[64], pname[96], pver[96], family[96];
    read_line("/sys/class/dmi/id/sys_vendor", vendor, sizeof vendor);
    read_line("/sys/class/dmi/id/product_name", pname, sizeof pname);
    read_line("/sys/class/dmi/id/product_version", pver, sizeof pver);
    read_line("/sys/class/dmi/id/product_family", family, sizeof family);
    const char *model = !dmi_blank(pver) && strchr(pver, ' ') ? pver :
                        !dmi_blank(family) && strchr(family, ' ') ? family :
                        !dmi_blank(pname) ? pname : "";
    if (model[0] && !dmi_blank(vendor) && strncasecmp(model, vendor, strlen(vendor)))
        snprintf(s->machine, sizeof s->machine, "%s %s", vendor, model);
    else if (model[0]) snprintf(s->machine, sizeof s->machine, "%s", model);
    else snprintf(s->machine, sizeof s->machine, "AT/AT COMPATIBLE");

    unsigned long kb = 0;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) break;
        fclose(f);
    }
    char k[32];
    with_commas(kb, k, sizeof k);
    snprintf(s->ram, sizeof s->ram, "%s KB RAM", k);

    /* Network Identification: the full name, and Samba's workgroup when
     * this computer is in one. */
    char dom[128] = "";
    if (getdomainname(dom, sizeof dom - 1) != 0 || !strcmp(dom, "(none)")) dom[0] = 0;
    snprintf(s->fqdn, sizeof s->fqdn, "%s%s%s", s->host, dom[0] ? "." : "", dom);
    snprintf(s->workgroup, sizeof s->workgroup, "WORKGROUP");
    f = fopen("/etc/samba/smb.conf", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncasecmp(p, "workgroup", 9)) continue;
            char *eq = strchr(p, '=');
            if (!eq) continue;
            for (eq++; *eq == ' ' || *eq == '\t'; eq++) {}
            eq[strcspn(eq, " \t\r\n")] = 0;
            if (*eq) snprintf(s->workgroup, sizeof s->workgroup, "%s", eq);
            break;
        }
        fclose(f);
    }
}

/* ------------------------------------------------------------------ *
 * The picture: a monitor, drawn, its screen teal, and on the screen the
 * Linux 2000 logo where Windows put its flag -- or, with no logo
 * installed, a waving flag drawn in its place.
 * ------------------------------------------------------------------ */
#define MON_W 112
#define MON_H 96

static void px(unsigned char *img, int x, int y, int r, int g, int b)
{
    if (x < 0 || y < 0 || x >= MON_W || y >= MON_H) return;
    unsigned char *p = img + (y * MON_W + x) * 4;
    p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b; p[3] = 255;
}

static void box(unsigned char *img, int x, int y, int w, int h, int r, int g, int b)
{
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(img, i, j, r, g, b);
}

/* A raised or sunken bevel, as the controls' edges are drawn. */
static void bevel(unsigned char *img, int x, int y, int w, int h, int sunk)
{
    int hi = sunk ? 128 : 255, lo = sunk ? 255 : 128;
    for (int i = x; i < x + w; i++) { px(img, i, y, hi, hi, hi); px(img, i, y + h - 1, lo, lo, lo); }
    for (int j = y; j < y + h; j++) { px(img, x, j, hi, hi, hi); px(img, x + w - 1, j, lo, lo, lo); }
}

static int draw_logo(unsigned char *img);
static void draw_flag(unsigned char *img);

static W2kSkin *monitor_picture(void)
{
    unsigned char *img = calloc(MON_W * MON_H, 4);
    if (!img) return NULL;
    int fr, fg, fb;
    w2k_color_rgb(C_FACE, &fr, &fg, &fb);
    /* The case, the bezel and the screen. */
    box(img, 4, 2, 104, 75, 212, 208, 200);
    bevel(img, 4, 2, 104, 75, 0);
    for (int i = 5; i < 107; i++) px(img, i, 76, 64, 64, 64);
    for (int j = 3; j < 77; j++) px(img, 107, j, 64, 64, 64);
    box(img, 12, 8, 88, 60, 160, 160, 160);
    bevel(img, 12, 8, 88, 60, 1);
    box(img, 14, 10, 84, 56, 0, 128, 128);
    box(img, 92, 70, 4, 2, 0, 200, 0);                      /* the power light */
    /* The neck and the foot. */
    for (int j = 77; j < 84; j++) {
        int inset = (j - 77) / 2;
        box(img, 40 - inset, j, 32 + 2 * inset, 1, 212, 208, 200);
        px(img, 40 - inset, j, 255, 255, 255);
        px(img, 71 + inset, j, 128, 128, 128);
    }
    box(img, 22, 84, 68, 9, 212, 208, 200);
    bevel(img, 22, 84, 68, 9, 0);
    for (int i = 23; i < 89; i++) px(img, i, 93, 64, 64, 64);

    if (!draw_logo(img)) draw_flag(img);
    W2kSkin *s = w2k_skin_from_rgba(img, MON_W, MON_H);
    free(img);
    return s;
}

/* The logo (skins/l2logo.png) as wide as the screen allows, centred on
 * it, its transparent edges blended into the teal. 0 without it. */
static int draw_logo(unsigned char *img)
{
    const int sx = 14, sy = 10, sw = 84, sh = 56;       /* the screen */
    char path[1024];
    int iw = 0, ih = 0;
    unsigned char *rgba = w2k_skin_path("l2logo.png", path, sizeof path) ? w2k_image_load(path, &iw, &ih) : NULL;
    if (!rgba || iw <= 0 || ih <= 0) { free(rgba); return 0; }
    int dw = sw - 6, dh = ih * dw / iw;
    if (dh > sh - 6) { dh = sh - 6; dw = iw * dh / ih; }
    unsigned char *sc = w2k_rgba_resample(rgba, iw, ih, dw, dh, RS_LANCZOS);
    free(rgba);
    if (!sc) return 0;
    int ox = sx + (sw - dw) / 2, oy = sy + (sh - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            const unsigned char *s = sc + ((size_t)y * dw + x) * 4;
            unsigned char *d = img + ((size_t)(oy + y) * MON_W + ox + x) * 4;
            int a = s[3];
            for (int c = 0; c < 3; c++) d[c] = (unsigned char)((s[c] * a + d[c] * (255 - a) + 127) / 255);
        }
    free(sc);
    return 1;
}

/* The flag: four panes, bowed as it flies, split by black, and its
 * trail of squares. */
static void draw_flag(unsigned char *img)
{
    const int fx = 42, fy = 22, fw = 42, fh = 30;
    for (int x = fx; x < fx + fw; x++) {
        double u = (double)(x - fx) / fw;
        double top = fy - 3.0 * sin(M_PI * u) + 2.0 * u;
        for (int y = (int)top; y < (int)(top + fh) + 1; y++) {
            double v = (y - top) / fh;
            if (v < 0 || v >= 1) continue;
            int mid_x = fabs(x - (fx + fw / 2.0)) < 1.0;
            int mid_y = fabs(y - (top + fh / 2.0)) < 1.0;
            if (mid_x || mid_y) { px(img, x, y, 0, 0, 0); continue; }
            int pane = (u >= 0.5) + 2 * (v >= 0.5);
            static const unsigned char col[4][3] = {
                { 240, 40, 30 }, { 40, 180, 50 }, { 30, 80, 230 }, { 250, 210, 20 } };
            px(img, x, y, col[pane][0], col[pane][1], col[pane][2]);
        }
    }
    /* The trail, squares shrinking away to the left: red above, blue
     * below, a few missing, as the logo scatters them. */
    for (int k = 1; k <= 4; k++) {
        int sz = 5 - k, gap = 6;
        if (sz < 1) sz = 1;
        int x = fx - k * gap;
        for (int j = 0; j < 6; j++) {
            if (k > 1 && (j + k) % 3 == 0) continue;
            int y = fy + 1 + j * 5 + (k & 1);
            int blue = j >= 3;
            box(img, x, y, sz, sz, blue ? 30 : 240, blue ? 80 : 40, blue ? 230 : 30);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Environment Variables
 * ------------------------------------------------------------------ */
typedef struct { W2kList *l; W2kRect ok; int down; } EnvDlg;

static void env_paint(W2kWin *w, Drawable d)
{
    EnvDlg *e = w->user;
    w2k_text(d, F_UI, 12, 12, "Variables of this session (as programs started from the desktop see them):", C_TEXT);
    w2k_list_draw(d, e->l);
    w2k_text(d, F_UI, 12, w->h - 40, "They are set at logon; ~/.profile and the session file are where to change them.", C_GRAYTEXT);
    w2k_draw_pushbutton(d, &e->ok, "OK", BS_DEFAULT | (e->down ? BS_PRESSED : 0));
}

static int env_event(W2kWin *w, XEvent *ev)
{
    EnvDlg *e = w->user;
    switch (ev->type) {
    case ButtonPress:
        if (w2k_list_press(e->l, &ev->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&e->ok, ev->xbutton.x, ev->xbutton.y)) e->down = 1;
        w2k_win_dirty(w);
        return 1;
    case MotionNotify:
        if (w2k_list_motion(e->l, &ev->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease:
        w2k_list_release(e->l, &ev->xbutton);
        if (e->down && w2k_rect_hit(&e->ok, ev->xbutton.x, ev->xbutton.y)) w2k_win_close(w, ID_OK);
        e->down = 0;
        w2k_win_dirty(w);
        return 1;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&ev->xkey, 0);
        if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter) { w2k_win_close(w, ID_OK); return 1; }
        if (w2k_list_key(e->l, &ev->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static int env_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void environment_dialog(W2kWin *over)
{
    EnvDlg e = { 0 };
    int W = 420, H = 380;
    W2kWin *w = w2k_win_new("Environment Variables", "w2kdialog", W, H, 0);
    w->user = &e;
    w->paint = env_paint;
    w->event = env_event;
    e.l = w2k_list_new(LV_REPORT);
    e.l->fullrow = 1;
    w2k_scroll_bind(&e.l->vsb, w);
    w2k_scroll_bind(&e.l->hsb, w);
    w2k_list_add_col(e.l, "Variable", 130, 0);
    w2k_list_add_col(e.l, "Value", 600, 0);
    e.l->r = (W2kRect){ 12, 32, W - 24, H - 32 - 52 };
    int n = 0;
    while (environ[n]) n++;
    char **v = malloc(sizeof *v * (size_t)(n ? n : 1));
    if (v) {
        for (int i = 0; i < n; i++) v[i] = environ[i];
        qsort(v, (size_t)n, sizeof *v, env_cmp);
        for (int i = 0; i < n; i++) {
            const char *eq = strchr(v[i], '=');
            if (!eq) continue;
            char name[128];
            snprintf(name, sizeof name, "%.*s", (int)(eq - v[i]), v[i]);
            int r = w2k_list_add(e.l, ICO_NONE, NULL);
            w2k_list_set(e.l, r, 0, name);
            w2k_list_set(e.l, r, 1, eq + 1);
        }
        free(v);
    }
    w2k_list_layout(e.l);
    e.ok = (W2kRect){ W - 12 - 75, H - 12 - 23, 75, 23 };
    w2k_win_center(w, over);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);
    w2k_win_modal(w);
    w2k_list_free(e.l);
}

/* ------------------------------------------------------------------ *
 * The sheet
 * ------------------------------------------------------------------ */
static int enabled(int b)
{
    return b == B_OK || b == B_CANCEL || b == B_DEVMGR || b == B_PERF || b == B_ENV;
}

/* Which buttons belong to which page. */
static int on_page(int b, int page)
{
    switch (b) {
    case B_OK: case B_CANCEL: case B_APPLY: return 1;
    case B_NETID: case B_RENAME:            return page == 1;
    case B_WIZARD: case B_SIGNING: case B_DEVMGR: case B_PROFILES: return page == 2;
    case B_PDELETE: case B_PCHANGE: case B_PCOPY: return page == 3;
    case B_PERF: case B_ENV: case B_STARTUP: return page == 4;
    }
    return 0;
}

static const char *label(int b)
{
    static const char *const t[NBTN] = {
        [B_OK] = "OK", [B_CANCEL] = "Cancel", [B_APPLY] = "&Apply",
        [B_DEVMGR] = "&Device Manager...", [B_PERF] = "&Performance Options...",
        [B_ENV] = "&Environment Variables...", [B_WIZARD] = "&Hardware Wizard...",
        [B_SIGNING] = "Driver &Signing...", [B_PROFILES] = "Hardware &Profiles...",
        [B_STARTUP] = "&Startup and Recovery...", [B_NETID] = "&Network ID...",
        [B_RENAME] = "P&roperties", [B_PDELETE] = "&Delete",
        [B_PCHANGE] = "&Change Type...", [B_PCOPY] = "C&opy To..." };
    return t[b];
}

/* Lines of text, one after another, from (x, y); returns where they end. */
static int lines(Drawable d, int x, int y, int color, const char *const *t)
{
    int fh = w2k_font_height(F_UI);
    for (int i = 0; t[i]; i++, y += fh) w2k_text(d, F_UI, x, y, t[i], color);
    return y;
}

static void sp_paint(W2kWin *w, Drawable d)
{
    SysDlg *s = w->user;
    int fh = w2k_font_height(F_UI);
    int lh = fh > 13 ? fh : 13;
    w2k_tabs_draw(d, s->tabs);
    W2kRect c = w2k_tabs_client(s->tabs);
    int page = s->tabs->sel;

    if (page == 0) {
        w2k_skin_draw(d, s->monitor, c.x + 36, c.y + 42, 0, 0, MON_W, MON_H);
        int x = c.x + 190, y = c.y + 30, ix = x + 14;
        w2k_text(d, F_UI, x, y, "System:", C_TEXT);            y += lh;
        w2k_text(d, F_UI, ix, y, s->distro, C_TEXT);           y += lh;
        w2k_text(d, F_UI, ix, y, s->version, C_TEXT);          y += lh;
        w2k_text(d, F_UI, ix, y, s->kernel, C_TEXT);           y += lh * 2;
        w2k_text(d, F_UI, x, y, "Registered to:", C_TEXT);     y += lh;
        w2k_text(d, F_UI, ix, y, s->owner, C_TEXT);            y += lh;
        w2k_text(d, F_UI, ix, y, s->host, C_TEXT);             y += lh * 3;
        w2k_text(d, F_UI, x, y, "Computer:", C_TEXT);          y += lh;
        for (int i = 0; i < s->ncpu; i++, y += lh) w2k_text(d, F_UI, ix, y, s->cpu[i], C_TEXT);
        w2k_text(d, F_UI, ix, y, s->machine, C_TEXT);          y += lh;
        w2k_text(d, F_UI, ix, y, s->ram, C_TEXT);
    } else if (page == 1) {
        w2k_bigicon_draw(d, c.x + 14, c.y + 14, ICO_MYCOMPUTER);
        static const char *const intro[] = { "Linux 2000 uses the following information to",
                                             "identify your computer on the network.", NULL };
        lines(d, c.x + 60, c.y + 16, C_TEXT, intro);
        int y = c.y + 66;
        w2k_text(d, F_UI, c.x + 14, y, "Full computer name:", C_TEXT);
        w2k_text(d, F_UI, c.x + 130, y, s->fqdn, C_TEXT);      y += lh + 8;
        w2k_text(d, F_UI, c.x + 14, y, "Workgroup:", C_TEXT);
        w2k_text(d, F_UI, c.x + 130, y, s->workgroup, C_TEXT);
        static const char *const how[] = { "The computer's name is its host name. Renaming it",
            "under a running desktop can lock new windows out of", "the display, so it is not done from here:",
            "use hostnamectl, then log off and on again.", NULL };
        lines(d, c.x + 14, s->b[B_NETID].y - 4 * fh - 14, C_GRAYTEXT, how);
    } else if (page == 2) {
        W2kRect g1 = { c.x + 8, c.y + 10, c.w - 16, 84 };
        w2k_draw_groupbox(d, &g1, "Hardware Wizard");
        w2k_bigicon_draw(d, g1.x + 12, g1.y + 22, ICO_MYCOMPUTER);
        static const char *const t1[] = { "The Hardware wizard helps you install hardware.", NULL };
        lines(d, g1.x + 58, g1.y + 22, C_TEXT, t1);
        W2kRect g2 = { c.x + 8, g1.y + g1.h + 8, c.w - 16, 104 };
        w2k_draw_groupbox(d, &g2, "Device Manager");
        w2k_bigicon_draw(d, g2.x + 12, g2.y + 22, ICO_CP_SYSTEM);
        static const char *const t2[] = { "The Device Manager lists all the hardware devices",
            "installed on your computer. Use the Device Manager", "to change the properties of any device.", NULL };
        lines(d, g2.x + 58, g2.y + 22, C_TEXT, t2);
        W2kRect g3 = { c.x + 8, g2.y + g2.h + 8, c.w - 16, 84 };
        w2k_draw_groupbox(d, &g3, "Hardware Profiles");
        w2k_bigicon_draw(d, g3.x + 12, g3.y + 22, ICO_SETTINGS);
        static const char *const t3[] = { "Hardware profiles provide a way for you to set up",
                                          "and store different hardware configurations.", NULL };
        lines(d, g3.x + 58, g3.y + 22, C_TEXT, t3);
    } else if (page == 3) {
        w2k_bigicon_draw(d, c.x + 12, c.y + 12, ICO_CP_USERS);
        static const char *const t[] = { "User profiles contain desktop settings and other",
            "information related to your logon. Here, each is the", "account's home folder.", NULL };
        lines(d, c.x + 56, c.y + 12, C_TEXT, t);
        w2k_text(d, F_UI, c.x + 12, c.y + 64, "Profiles stored on this computer:", C_TEXT);
        w2k_list_draw(d, s->users);
    } else {
        static const char *const t[3][3] = {
            { "Performance options control how the desktop draws", "itself, which affects the speed of your computer.", NULL },
            { "Environment variables tell your computer where to", "find certain types of information.", NULL },
            { "Startup and recovery options tell your computer how", "to start and what to do if an error stops it.", NULL } };
        const char *title[3] = { "Performance", "Environment Variables", "Startup and Recovery" };
        int y = c.y + 10;
        for (int i = 0; i < 3; i++) {
            W2kRect g = { c.x + 8, y, c.w - 16, 96 };
            w2k_draw_groupbox(d, &g, title[i]);
            lines(d, g.x + 12, g.y + 20, C_TEXT, t[i]);
            y += g.h + 8;
        }
    }
    for (int b = 1; b < NBTN; b++) {
        if (!on_page(b, page)) continue;
        int st = (b == B_OK ? BS_DEFAULT : 0) | (s->down == b ? BS_PRESSED : 0) |
                 (enabled(b) ? 0 : BS_DISABLED);
        w2k_draw_pushbutton(d, &s->b[b], label(b), st);
    }
}

static void press_button(SysDlg *s, int b)
{
    switch (b) {
    case B_OK:     w2k_win_close(s->w, ID_OK); break;
    case B_CANCEL: w2k_win_close(s->w, ID_CANCEL); break;
    case B_DEVMGR: spawn("l2kdevmgmt"); break;
    case B_PERF:   spawn("l2kcontrol performance"); break;
    case B_ENV:    environment_dialog(s->w); break;
    }
}

static int sp_event(W2kWin *w, XEvent *e)
{
    SysDlg *s = w->user;
    switch (e->type) {
    case ButtonPress: {
        if (w2k_tabs_press(s->tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (s->tabs->sel == 3 && w2k_list_press(s->users, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        for (int b = 1; b < NBTN; b++)
            if (on_page(b, s->tabs->sel) && enabled(b) &&
                w2k_rect_hit(&s->b[b], e->xbutton.x, e->xbutton.y)) s->down = b;
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (s->tabs->sel == 3 && w2k_list_motion(s->users, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        w2k_list_release(s->users, &e->xbutton);
        int b = s->down;
        s->down = 0;
        if (b && w2k_rect_hit(&s->b[b], e->xbutton.x, e->xbutton.y)) press_button(s, b);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { w2k_win_close(w, ID_OK); return 1; }
        if (w2k_tabs_key(s->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (s->tabs->sel == 3 && w2k_list_key(s->users, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

/* The accounts with a home of their own: people, not services. */
static void fill_users(SysDlg *s)
{
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent())) {
        if (pw->pw_uid < 1000 || pw->pw_uid >= 60000 || !pw->pw_dir) continue;
        struct stat st;
        if (stat(pw->pw_dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char name[200], when[40];
        snprintf(name, sizeof name, "%s\\%s", s->host, pw->pw_name);
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        snprintf(when, sizeof when, "%d/%d/%d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
        int r = w2k_list_add(s->users, ICO_CP_USERS, NULL);
        w2k_list_set(s->users, r, 0, name);
        w2k_list_set(s->users, r, 1, pw->pw_uid == getuid() ? "Local (you)" : "Local");
        w2k_list_set(s->users, r, 2, when);
    }
    endpwent();
}

int w2k_system_properties(W2kWin *over)
{
    SysDlg s;
    memset(&s, 0, sizeof s);
    W2kWin *w = w2k_win_new("System Properties", "w2kdialog", SP_W, SP_H, 0);
    s.w = w;
    w->user = &s;
    w->paint = sp_paint;
    w->event = sp_event;
    s.tabs = w2k_tabs_new(&s, NULL);
    w2k_tabs_add(s.tabs, "General");
    w2k_tabs_add(s.tabs, "Network Identification");
    w2k_tabs_add(s.tabs, "Hardware");
    w2k_tabs_add(s.tabs, "User Profiles");
    w2k_tabs_add(s.tabs, "Advanced");
    s.tabs->r = (W2kRect){ 6, 6, SP_W - 12, SP_H - 6 - 40 };
    W2kRect c = w2k_tabs_client(s.tabs);
    gather(&s, c.x + c.w - 12 - (c.x + 190 + 14));
    s.monitor = monitor_picture();

    s.b[B_OK]     = (W2kRect){ SP_W - 8 - 75 * 3 - 12, SP_H - 8 - 23, 75, 23 };
    s.b[B_CANCEL] = (W2kRect){ SP_W - 8 - 75 * 2 - 6, SP_H - 8 - 23, 75, 23 };
    s.b[B_APPLY]  = (W2kRect){ SP_W - 8 - 75, SP_H - 8 - 23, 75, 23 };
    /* Network Identification: its two buttons under the words. */
    s.b[B_NETID]  = (W2kRect){ c.x + c.w - 14 - 90 * 2 - 8, c.y + c.h - 14 - 23, 90, 23 };
    s.b[B_RENAME] = (W2kRect){ c.x + c.w - 14 - 90, c.y + c.h - 14 - 23, 90, 23 };
    /* Hardware: each group's buttons at its foot, right-aligned. */
    int g1 = c.y + 10, g2 = g1 + 84 + 8, g3 = g2 + 104 + 8;
    s.b[B_WIZARD]   = (W2kRect){ c.x + c.w - 20 - 128, g1 + 84 - 32, 128, 23 };
    s.b[B_SIGNING]  = (W2kRect){ c.x + c.w - 20 - 128 * 2 - 8, g2 + 104 - 32, 128, 23 };
    s.b[B_DEVMGR]   = (W2kRect){ c.x + c.w - 20 - 128, g2 + 104 - 32, 128, 23 };
    s.b[B_PROFILES] = (W2kRect){ c.x + c.w - 20 - 128, g3 + 84 - 32, 128, 23 };
    /* User Profiles: the list, and its three buttons under it. */
    s.users = w2k_list_new(LV_REPORT);
    s.users->fullrow = 1;
    w2k_scroll_bind(&s.users->vsb, w);
    w2k_scroll_bind(&s.users->hsb, w);
    w2k_list_add_col(s.users, "Name", 170, 0);
    w2k_list_add_col(s.users, "Type", 80, 0);
    w2k_list_add_col(s.users, "Modified", 90, 0);
    s.users->r = (W2kRect){ c.x + 12, c.y + 84, c.w - 24, c.h - 84 - 50 };
    fill_users(&s);
    w2k_list_layout(s.users);
    int by = c.y + c.h - 14 - 23;
    s.b[B_PDELETE] = (W2kRect){ c.x + c.w - 12 - 90, by, 90, 23 };
    s.b[B_PCHANGE] = (W2kRect){ c.x + c.w - 12 - 90 * 2 - 6, by, 90, 23 };
    s.b[B_PCOPY]   = (W2kRect){ c.x + c.w - 12 - 90 * 3 - 12, by, 90, 23 };
    /* Advanced: one button at the foot of each group. */
    static const int adv[3] = { B_PERF, B_ENV, B_STARTUP };
    for (int i = 0; i < 3; i++) {
        int gy = c.y + 10 + i * (96 + 8);
        s.b[adv[i]] = (W2kRect){ c.x + c.w - 20 - 150, gy + 96 - 32, 150, 23 };
    }

    w2k_win_center(w, over);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);
    int r = w2k_win_modal(w);
    w2k_list_free(s.users);
    w2k_tabs_free(s.tabs);
    w2k_skin_free(s.monitor);
    return r == ID_OK;
}
