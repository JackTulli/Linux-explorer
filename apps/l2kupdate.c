/* l2kupdate.c -- Linux 2000 Update, in the manner of Windows Update.
 *
 * The page Windows Update put up in Windows 2000: a banner with the
 * globe, a column of links down the left (Welcome, Product Updates,
 * Support Information) and the section on the right. Here Welcome checks
 * for a newer Linux 2000 and installs it when asked; Product Updates
 * does the same for the distribution's packages, Flatpak and Snap, with
 * whichever package manager the machine has. Nothing is ever installed
 * on its own: every check and every install is a click, and the install
 * runs in a terminal in front of you, asking for your password there. */
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>

#ifndef W2K_VERSION
#define W2K_VERSION "?"
#endif

#define WIN_W    640
#define WIN_H    480
#define BANNER_H 58
#define NAV_W    160
#define MAX_LINES 24

enum { SEC_WELCOME, SEC_PRODUCT, SEC_SUPPORT, N_SEC };
enum { BTN_NONE, BTN_CHECK, BTN_INSTALL, BTN_SYS_CHECK, BTN_SYS_INSTALL };

typedef struct { char text[160]; int link; W2kRect r; } Line;

static struct {
    W2kWin  *win;
    W2kFace *big, *head;
    int      section;
    int      down;                      /* button pressed, BTN_* */
    W2kRect  nav[N_SEC];
    W2kRect  check, install, sys_check, sys_install;
    /* Facts */
    char     version[64], build[32];
    char     latest[32];                /* "" until checked */
    int      newer;                     /* a newer release exists */
    char     checked_msg[200];
    char     source[512];               /* the checkout we run from, or "" */
    char     distro[128];
    char     pkgmgr[32];
    int      have_flatpak, have_snap;
    int      counts_known;
    int      n_pkg, n_flatpak, n_snap;
    char     sys_msg[200];
    /* Links on the page, with their rectangles */
    Line     links[8];
    int      nlinks;
} up;

/* ---- Facts ------------------------------------------------------------ */
static int in_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    char *save = NULL;
    for (char *q = strtok_r(copy, ":", &save); q && !found; q = strtok_r(NULL, ":", &save)) {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s/%s", q, prog);
        if (access(tmp, X_OK) == 0) found = 1;
    }
    free(copy);
    return found;
}

static void read_first_line(const char *cmd, char *out, int n)
{
    out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return;
    if (!fgets(out, n, p)) out[0] = 0;
    pclose(p);
    out[strcspn(out, "\r\n")] = 0;
}

static void gather(void)
{
    /* "1.9.1+2aa0798" -> version 1.9.1, build 2aa0798 */
    snprintf(up.version, sizeof up.version, "%s", W2K_VERSION);
    char *plus = strchr(up.version, '+');
    if (plus) { snprintf(up.build, sizeof up.build, "%s", plus + 1); *plus = 0; }

    /* Running from a source checkout (bin/ beside .git)? Then that is
     * what an update pulls and rebuilds. */
    char exe[768];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            char git[900];
            snprintf(git, sizeof git, "%s/../.git", exe);
            if (access(git, F_OK) == 0) {
                char real[512];
                snprintf(git, sizeof git, "%s/..", exe);
                if (realpath(git, real)) snprintf(up.source, sizeof up.source, "%s", real);
            }
        }
    }

    up.distro[0] = 0;
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char line[300];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *v = line + 12;
                v[strcspn(v, "\r\n")] = 0;
                if (*v == '"') { v++; char *q = strrchr(v, '"'); if (q) *q = 0; }
                snprintf(up.distro, sizeof up.distro, "%s", v);
            }
        fclose(f);
    }
    if (!up.distro[0]) snprintf(up.distro, sizeof up.distro, "Linux");

    const char *mgrs[] = { "apt-get", "dnf", "yum", "pacman", "zypper", "apk",
                           "xbps-install", "emerge", NULL };
    up.pkgmgr[0] = 0;
    for (int i = 0; mgrs[i] && !up.pkgmgr[0]; i++)
        if (in_path(mgrs[i])) snprintf(up.pkgmgr, sizeof up.pkgmgr, "%s", mgrs[i]);
    up.have_flatpak = in_path("flatpak");
    up.have_snap = in_path("snap");
}

/* Dotted versions: 1 when a is newer than b. */
static int version_newer(const char *a, const char *b)
{
    int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
    sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
    sscanf(b, "%d.%d.%d", &b1, &b2, &b3);
    if (a1 != b1) return a1 > b1;
    if (a2 != b2) return a2 > b2;
    return a3 > b3;
}

static void check_release(void)
{
    snprintf(up.checked_msg, sizeof up.checked_msg, "Checking...");
    w2k_win_dirty(up.win);
    w2k_win_repaint_now(up.win);
    char out[8192] = "";
    FILE *p = popen("curl -s -m 15 https://api.github.com/repos/JackTulli/Linux-explorer/releases/latest 2>/dev/null", "r");
    if (p) { size_t n = fread(out, 1, sizeof out - 1, p); out[n] = 0; pclose(p); }
    const char *t = strstr(out, "\"tag_name\"");
    if (!t) {
        up.latest[0] = 0;
        snprintf(up.checked_msg, sizeof up.checked_msg,
                 "Could not reach the release list. Check the network connection and try again.");
        return;
    }
    t = strchr(t + 10, '"');
    if (!t) return;
    t++;
    if (*t == 'v') t++;
    snprintf(up.latest, sizeof up.latest, "%.*s", (int)strcspn(t, "\""), t);
    up.newer = version_newer(up.latest, up.version);
    if (up.newer)
        snprintf(up.checked_msg, sizeof up.checked_msg,
                 "Version %s is available. You have %s.", up.latest, up.version);
    else
        snprintf(up.checked_msg, sizeof up.checked_msg,
                 "The latest release is %s. Your desktop is up to date.", up.latest);
}

static int count_cmd(const char *cmd)
{
    char out[32];
    read_first_line(cmd, out, sizeof out);
    return atoi(out);
}

static void check_system(void)
{
    snprintf(up.sys_msg, sizeof up.sys_msg, "Checking...");
    w2k_win_dirty(up.win);
    w2k_win_repaint_now(up.win);
    up.n_pkg = up.n_flatpak = up.n_snap = 0;
    const char *m = up.pkgmgr;
    if (!strcmp(m, "apt-get"))
        up.n_pkg = count_cmd("apt list --upgradable 2>/dev/null | grep -c 'upgradable from'");
    else if (!strcmp(m, "dnf") || !strcmp(m, "yum"))
        up.n_pkg = count_cmd("dnf -q check-update 2>/dev/null | grep -c '^[A-Za-z0-9]'");
    else if (!strcmp(m, "pacman"))
        up.n_pkg = count_cmd("pacman -Qu 2>/dev/null | wc -l");
    else if (!strcmp(m, "zypper"))
        up.n_pkg = count_cmd("zypper -q lu 2>/dev/null | grep -c '^v '");
    else if (!strcmp(m, "apk"))
        up.n_pkg = count_cmd("apk list -u 2>/dev/null | wc -l");
    else if (!strcmp(m, "xbps-install"))
        up.n_pkg = count_cmd("xbps-install -un 2>/dev/null | wc -l");
    if (up.have_flatpak)
        up.n_flatpak = count_cmd("flatpak remote-ls --updates 2>/dev/null | wc -l");
    if (up.have_snap)
        up.n_snap = count_cmd("snap refresh --list 2>/dev/null | tail -n +2 | wc -l");
    up.counts_known = 1;
    int total = up.n_pkg + up.n_flatpak + up.n_snap;
    if (total)
        snprintf(up.sys_msg, sizeof up.sys_msg,
                 "%d update%s available: %d package%s%s%s.", total, total == 1 ? "" : "s",
                 up.n_pkg, up.n_pkg == 1 ? "" : "s",
                 up.have_flatpak ? "" : "", up.have_snap ? "" : "");
    else
        snprintf(up.sys_msg, sizeof up.sys_msg,
                 "No updates are listed as of the package manager's last refresh.");
}

/* ---- Running an install in a terminal --------------------------------- */
static void spawn(const char *cmd)
{
    pid_t p = fork();
    if (p < 0) return;
    if (p == 0) {
        if (fork() == 0) {
            signal(SIGPIPE, SIG_DFL);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        }
        _exit(0);
    }
    waitpid(p, NULL, 0);
}

/* Write the commands to a script and run it in a terminal that stays
 * open until Enter, so what happened can be read. The script takes root
 * with sudo, doas or su, asking in the terminal. */
static void run_in_terminal(const char *title, const char *body)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    char dir[600], path[700];
    snprintf(dir, sizeof dir, "%s/.w2k", home);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/update-run.sh", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "#!/bin/sh\n"
        "# Written by Linux 2000 Update; runs in a terminal.\n"
        "as_root() {\n"
        "    if [ \"$(id -u)\" = 0 ]; then \"$@\";\n"
        "    elif command -v sudo >/dev/null 2>&1; then sudo \"$@\";\n"
        "    elif command -v doas >/dev/null 2>&1; then doas \"$@\";\n"
        "    else su -c \"$*\"; fi\n"
        "}\n"
        "echo '==> %s'\n"
        "echo\n"
        "%s\n"
        "rc=$?\n"
        "echo\n"
        "if [ $rc = 0 ]; then echo '==> Done.'; else echo \"==> Something failed (exit $rc). The messages above say what.\"; fi\n"
        "echo 'Press Enter to close this window.'\n"
        "read x\n", title, body);
    fclose(f);
    chmod(path, 0755);

    const char *env = getenv("TERMINAL");
    const char *term = env && *env && in_path(env) ? env : NULL;
    const char *cands[] = { "x-terminal-emulator", "xterm", "gnome-terminal", "konsole",
                            "xfce4-terminal", "alacritty", "kitty", "urxvt", NULL };
    for (int i = 0; !term && cands[i]; i++) if (in_path(cands[i])) term = cands[i];
    if (!term) {
        w2k_msgbox(up.win, "Linux 2000 Update",
                   "No terminal program was found to run the update in.\n"
                   "Install xterm, or set TERMINAL.", MB_OK | MB_ICONWARNING);
        return;
    }
    char cmd[1200];
    if (!strcmp(term, "gnome-terminal"))
        snprintf(cmd, sizeof cmd, "%s --title='%s' -- sh '%s'", term, title, path);
    else if (!strcmp(term, "xterm") || !strcmp(term, "urxvt"))
        snprintf(cmd, sizeof cmd, "%s -T '%s' -geometry 100x30 -e sh '%s'", term, title, path);
    else
        snprintf(cmd, sizeof cmd, "%s -e sh '%s'", term, path);
    spawn(cmd);
}

static void install_release(void)
{
    char body[1500];
    if (up.source[0]) {
        /* A source checkout: pull it, build it, install it, and restart
         * the running desktop in place. */
        snprintf(body, sizeof body,
                 "cd '%s' || exit 1\n"
                 "git pull --ff-only || exit 1\n"
                 "make -j\"$(nproc 2>/dev/null || echo 2)\" || exit 1\n"
                 "as_root make install || exit 1\n"
                 "l2kwm --restart 2>/dev/null || true\n", up.source);
    } else {
        /* The installed copy: the same one-line installer that put it
         * there, which updates and restarts a running desktop. */
        snprintf(body, sizeof body,
                 "if [ \"$(id -u)\" = 0 ]; then W2K_USER=\"${SUDO_USER:-$USER}\" sh -c 'curl -sL jacktulli.github.io/w2k | sh';\n"
                 "elif command -v sudo >/dev/null 2>&1; then sudo sh -c 'curl -sL jacktulli.github.io/w2k | sh';\n"
                 "else su -c \"W2K_USER=$USER sh -c 'curl -sL jacktulli.github.io/w2k | sh'\"; fi\n");
    }
    run_in_terminal("Updating Linux 2000", body);
}

static void install_system(void)
{
    char body[1500] = "";
    const char *m = up.pkgmgr;
    if (!strcmp(m, "apt-get"))
        strncat(body, "as_root apt-get update && as_root apt-get -y dist-upgrade\n", sizeof body - 1);
    else if (!strcmp(m, "dnf"))
        strncat(body, "as_root dnf -y upgrade --refresh\n", sizeof body - 1);
    else if (!strcmp(m, "yum"))
        strncat(body, "as_root yum -y update\n", sizeof body - 1);
    else if (!strcmp(m, "pacman"))
        strncat(body, "as_root pacman -Syu --noconfirm\n", sizeof body - 1);
    else if (!strcmp(m, "zypper"))
        strncat(body, "as_root zypper --non-interactive update\n", sizeof body - 1);
    else if (!strcmp(m, "apk"))
        strncat(body, "as_root apk update && as_root apk upgrade\n", sizeof body - 1);
    else if (!strcmp(m, "xbps-install"))
        strncat(body, "as_root xbps-install -Syu\n", sizeof body - 1);
    else if (!strcmp(m, "emerge"))
        strncat(body, "as_root emerge --sync && as_root emerge -uDN @world\n", sizeof body - 1);
    if (up.have_flatpak)
        strncat(body, "flatpak update -y\n", sizeof body - strlen(body) - 1);
    if (up.have_snap)
        strncat(body, "as_root snap refresh\n", sizeof body - strlen(body) - 1);
    if (!body[0]) {
        w2k_msgbox(up.win, "Linux 2000 Update",
                   "No package manager this program knows was found.",
                   MB_OK | MB_ICONINFO);
        return;
    }
    run_in_terminal("Installing system updates", body);
}

/* ---- The page ---------------------------------------------------------- */
static void link_add(const char *text, int x, int y)
{
    if (up.nlinks >= 8) return;
    Line *l = &up.links[up.nlinks++];
    snprintf(l->text, sizeof l->text, "%s", text);
    l->r = (W2kRect){ x, y, w2k_text_width(F_UI, text, -1), 13 };
}

/* Links in the page's blue, lightened when the window colour is dark
 * (the Windows Classic Dark scheme) so they still read. */
static void link_rgb(int *r, int *g, int *b)
{
    const unsigned char *wc = w2k_scheme_rgb(C_WINDOW);
    int dark = (wc[0] * 299 + wc[1] * 587 + wc[2] * 114) / 1000 < 128;
    *r = dark ? 120 : 0; *g = dark ? 170 : 0; *b = dark ? 255 : 204;
}

static void draw_link(Drawable d, const Line *l)
{
    int r, g, b;
    link_rgb(&r, &g, &b);
    w2k_text_rgb(d, F_UI, l->r.x, l->r.y, l->text, r, g, b);
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
    XFillRectangle(w2k.dpy, d, w2k.gc, l->r.x, l->r.y + w2k_font_px_ascent(F_UI) + 1,
                   (unsigned)l->r.w, 1);
}

static int heading(Drawable d, int x, int y, const char *s)
{
    if (up.head) {
        w2k_face_text(d, up.head, x, y, s, C_WINDOWTEXT);
        return w2k_face_height(up.head) + 6;
    }
    w2k_text(d, F_UI_BOLD, x, y, s, C_WINDOWTEXT);
    return 18;
}

static int para(Drawable d, int x, int y, int maxw, const char *s)
{
    /* Word-wrapped Tahoma 8 at 13 pixels a line. */
    const char *p = s;
    int lines = 0;
    while (*p) {
        const char *end = p, *sp = NULL;
        while (*end) {
            if (*end == ' ') sp = end;
            if (w2k_text_width(F_UI, p, (int)(end - p) + 1) > maxw) break;
            end++;
        }
        if (*end && sp && sp > p) end = sp;
        if (end == p) end = p + 1;
        w2k_textn(d, F_UI, x, y + lines * 13, p, (int)(end - p), C_WINDOWTEXT);
        lines++;
        p = end;
        while (*p == ' ') p++;
    }
    return lines * 13;
}

static void paint(W2kWin *w, Drawable d)
{
    up.nlinks = 0;
    w2k_fill(d, 0, 0, w->w, w->h, C_WINDOW);

    /* The banner: the globe and the name, over a navy rule. */
    w2k_bigicon_draw(d, 14, (BANNER_H - 32) / 2 - 1, ICO_WINUPDATE);
    if (up.big) {
        int asc = w2k_face_ascent(up.big);
        w2k_face_text(d, up.big, 58, 10 + (26 - asc), "Linux 2000 Update", C_WINDOWTEXT);
    } else {
        w2k_text(d, F_UI_BOLD, 58, 20, "Linux 2000 Update", C_WINDOWTEXT);
    }
    w2k_text(d, F_UI, w->w - 12 - w2k_text_width(F_UI, "Updates only when you ask", -1),
             BANNER_H - 20, "Updates only when you ask", C_GRAYTEXT);
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(0, 0, 128));
    XFillRectangle(w2k.dpy, d, w2k.gc, 0, BANNER_H - 3, (unsigned)w->w, 3);

    /* The column of links, the current one in bold. */
    static const char *const names[N_SEC] = { "Welcome", "Product Updates", "Support Information" };
    int ny = BANNER_H + 18;
    for (int i = 0; i < N_SEC; i++) {
        up.nav[i] = (W2kRect){ 16, ny, NAV_W - 24, 16 };
        if (i == up.section) {
            w2k_fill(d, 10, ny - 2, NAV_W - 14, 18, C_FACE);
            w2k_text(d, F_UI_BOLD, 16, ny, names[i], C_WINDOWTEXT);
        } else {
            Line l;
            snprintf(l.text, sizeof l.text, "%s", names[i]);
            l.r = (W2kRect){ 16, ny, w2k_text_width(F_UI, names[i], -1), 13 };
            draw_link(d, &l);
        }
        ny += 22;
    }
    w2k_vline(d, NAV_W, BANNER_H, w->h - BANNER_H, C_SHADOW);

    int x = NAV_W + 20, maxw = w->w - x - 20, y = BANNER_H + 16;
    char buf[300];
    switch (up.section) {
    case SEC_WELCOME:
        y += heading(d, x, y, "Welcome to Linux 2000 Update");
        y += para(d, x, y, maxw,
                  "Linux 2000 Update keeps this desktop current. It checks the project's "
                  "releases when you ask it to and installs a newer version when you tell "
                  "it to, in a terminal window in front of you. It never checks or installs "
                  "on its own.") + 10;
        snprintf(buf, sizeof buf, "Installed: Linux 2000 %s%s%s", up.version,
                 up.build[0] ? ", build " : "", up.build);
        w2k_text(d, F_UI, x, y, buf, C_WINDOWTEXT); y += 13;
        if (up.source[0]) {
            snprintf(buf, sizeof buf, "Running from the source checkout at %s", up.source);
            y += para(d, x, y, maxw, buf);
        }
        y += 10;
        up.check = (W2kRect){ x, y, 120, 23 };
        w2k_draw_pushbutton(d, &up.check, "&Check for updates", up.down == BTN_CHECK ? BS_PRESSED : 0);
        y += 32;
        if (up.checked_msg[0]) y += para(d, x, y, maxw, up.checked_msg) + 8;
        if (up.newer) {
            up.install = (W2kRect){ x, y, 120, 23 };
            snprintf(buf, sizeof buf, "&Install %s", up.latest);
            w2k_draw_pushbutton(d, &up.install, buf, up.down == BTN_INSTALL ? BS_PRESSED : 0);
            y += 32;
            y += para(d, x, y, maxw,
                      up.source[0] ?
                      "The update pulls the checkout, rebuilds it, installs it (asking for "
                      "your password) and restarts the desktop in place with every window kept." :
                      "The update runs the same installer that set the desktop up, as root "
                      "(asking for your password), and restarts the desktop in place with "
                      "every window kept.");
        } else {
            up.install = (W2kRect){ 0, 0, 0, 0 };
        }
        y += 12;
        link_add("What's new in each release", x, y);
        break;

    case SEC_PRODUCT:
        y += heading(d, x, y, "Product Updates");
        y += para(d, x, y, maxw,
                  "The rest of the computer: the packages the distribution installed, "
                  "Flatpak applications and Snaps. Checking asks the package manager what "
                  "it knows; installing runs its own upgrade in a terminal, as root, "
                  "asking for your password there.") + 10;
        snprintf(buf, sizeof buf, "System: %s", up.distro);
        w2k_text(d, F_UI, x, y, buf, C_WINDOWTEXT); y += 13;
        snprintf(buf, sizeof buf, "Package manager: %s", up.pkgmgr[0] ? up.pkgmgr : "none found");
        w2k_text(d, F_UI, x, y, buf, C_WINDOWTEXT); y += 13;
        snprintf(buf, sizeof buf, "Flatpak: %s      Snap: %s",
                 up.have_flatpak ? "installed" : "not installed",
                 up.have_snap ? "installed" : "not installed");
        w2k_text(d, F_UI, x, y, buf, C_WINDOWTEXT); y += 13 + 10;
        up.sys_check = (W2kRect){ x, y, 120, 23 };
        w2k_draw_pushbutton(d, &up.sys_check, "&Check for updates", up.down == BTN_SYS_CHECK ? BS_PRESSED : 0);
        up.sys_install = (W2kRect){ x + 128, y, 120, 23 };
        w2k_draw_pushbutton(d, &up.sys_install, "&Install updates",
                            (up.pkgmgr[0] || up.have_flatpak || up.have_snap ? 0 : BS_DISABLED) |
                            (up.down == BTN_SYS_INSTALL ? BS_PRESSED : 0));
        y += 32;
        if (up.sys_msg[0]) y += para(d, x, y, maxw, up.sys_msg) + 4;
        if (up.counts_known) {
            snprintf(buf, sizeof buf, "Packages: %d", up.n_pkg);
            w2k_text(d, F_UI, x, y, buf, C_WINDOWTEXT); y += 13;
            if (up.have_flatpak) { snprintf(buf, sizeof buf, "Flatpak: %d", up.n_flatpak); w2k_text(d, F_UI, x, y, buf, C_WINDOWTEXT); y += 13; }
            if (up.have_snap) { snprintf(buf, sizeof buf, "Snap: %d", up.n_snap); w2k_text(d, F_UI, x, y, buf, C_WINDOWTEXT); y += 13; }
        }
        break;

    case SEC_SUPPORT:
        y += heading(d, x, y, "Support Information");
        y += para(d, x, y, maxw,
                  "Linux 2000 is an independent project, not affiliated with, endorsed by "
                  "or sponsored by Microsoft. Windows is a trademark of Microsoft "
                  "Corporation.") + 10;
        snprintf(buf, sizeof buf, "Installed: Linux 2000 %s on %s", up.version, up.distro);
        y += para(d, x, y, maxw, buf) + 10;
        link_add("The project on GitHub", x, y); y += 17;
        link_add("Releases and what changed", x, y); y += 17;
        link_add("The Discord server", x, y); y += 17;
        link_add("Read the manual (README)", x, y); y += 17;
        y += 10;
        y += para(d, x, y, maxw,
                  "To update by hand: as root, curl -sL jacktulli.github.io/w2k | sh. "
                  "From a source checkout: git pull, make, sudo make install. "
                  "l2kwm --version says what is installed.");
        break;
    }
    for (int i = 0; i < up.nlinks; i++) draw_link(d, &up.links[i]);
}

static void open_url(const char *url)
{
    char cmd[300];
    snprintf(cmd, sizeof cmd, "xdg-open '%s'", url);
    spawn(cmd);
}

static void follow_link(int i)
{
    const char *t = up.links[i].text;
    if (!strcmp(t, "What's new in each release") || !strcmp(t, "Releases and what changed"))
        open_url("https://github.com/JackTulli/Linux-explorer/releases");
    else if (!strcmp(t, "The project on GitHub"))
        open_url("https://github.com/JackTulli/Linux-explorer");
    else if (!strcmp(t, "The Discord server"))
        open_url("https://discord.gg/KPQBnSqcK");
    else if (!strcmp(t, "Read the manual (README)"))
        open_url("https://github.com/JackTulli/Linux-explorer#readme");
}

static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        for (int i = 0; i < N_SEC; i++)
            if (w2k_rect_hit(&up.nav[i], x, y)) {
                up.section = i;
                w2k_win_dirty(w);
                return 1;
            }
        for (int i = 0; i < up.nlinks; i++)
            if (w2k_rect_hit(&up.links[i].r, x, y)) { follow_link(i); return 1; }
        if (up.section == SEC_WELCOME) {
            if (w2k_rect_hit(&up.check, x, y)) up.down = BTN_CHECK;
            else if (up.newer && w2k_rect_hit(&up.install, x, y)) up.down = BTN_INSTALL;
        } else if (up.section == SEC_PRODUCT) {
            if (w2k_rect_hit(&up.sys_check, x, y)) up.down = BTN_SYS_CHECK;
            else if (w2k_rect_hit(&up.sys_install, x, y)) up.down = BTN_SYS_INSTALL;
        }
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = up.down, x = e->xbutton.x, y = e->xbutton.y;
        up.down = BTN_NONE;
        w2k_win_dirty(w);
        if (b == BTN_CHECK && w2k_rect_hit(&up.check, x, y)) check_release();
        else if (b == BTN_INSTALL && w2k_rect_hit(&up.install, x, y)) install_release();
        else if (b == BTN_SYS_CHECK && w2k_rect_hit(&up.sys_check, x, y)) check_system();
        else if (b == BTN_SYS_INSTALL && w2k_rect_hit(&up.sys_install, x, y)) install_system();
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, 0); return 1; }
        if (ks == XK_F5) { if (up.section == SEC_PRODUCT) check_system(); else check_release(); return 1; }
        return 1;
    }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (w2k_init("l2kupdate") < 0) return 1;
    gather();
    up.big = w2k_face_open_bold("Tahoma", 26);
    up.head = w2k_face_open_bold("Tahoma", 17);
    /* "l2kupdate system" opens on Product Updates; the render harness's
     * W2K_RENDER_TAB picks a section too. */
    if (argc > 1 && !strcasecmp(argv[1], "system")) up.section = SEC_PRODUCT;
    if (getenv("W2K_RENDER_TAB")) up.section = atoi(getenv("W2K_RENDER_TAB")) % N_SEC;

    up.win = w2k_win_new("Linux 2000 Update", "l2kupdate", WIN_W, WIN_H, 1);
    up.win->min_w = 480;
    up.win->min_h = 360;
    up.win->paint = paint;
    up.win->event = event;
    w2k_win_center(up.win, NULL);
    w2k_win_show(up.win);
    w2k_run();
    w2k_face_close(up.big);
    w2k_face_close(up.head);
    w2k_fini();
    return 0;
}
