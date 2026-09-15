/* l2kdm.c -- the display manager: the machine boots straight into
 * "Log On to Windows".
 *
 * A root service (config/l2kdm.service) that starts the X server on a
 * virtual terminal, shows the Windows 2000 logon dialog on it with the
 * shell's own toolkit, checks the name and password through PAM, opens a
 * PAM session (which is what registers the login with logind), and runs
 * l2k-session as that user. When the session ends the logon screen comes
 * back; when it ends asking for it, the machine shuts down or restarts.
 * Nothing else stands between the boot and the desktop. */
#include "w2kui.h"
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_PAM
#include <security/pam_appl.h>
#endif

#define DISPLAY_NAME ":0"
#define VT_NUM       "7"
#define RUN_DIR      "/run/l2kdm"
#define AUTH_FILE    RUN_DIR "/Xauthority"

/* Measured off a 800x600 screenshot: the dialog's frame is 415 by 254; the
 * banner fills its top 92 rows under the caption. */
#define DLG_W   415
#define DLG_H   254
#define CAP_H    18
#define BANNER_H 92

static char session_cmd[1024] = "/usr/local/bin/l2k-session";
static char cookie[33];
static pid_t xpid;
static pid_t session_pid;                 /* the session leader, while one runs */
static volatile sig_atomic_t x_ready, x_died;

static void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("l2kdm: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ *
 * The X server
 * ------------------------------------------------------------------ */
static void on_usr1(int s) { (void)s; x_ready = 1; }
static void on_chld(int s) { (void)s; x_died = 1; }

/* Add the server's cookie to an authority file. The cookie goes down
 * xauth's standard input, never on to a command line where every process
 * on the machine could read it. */
static int xauth_add(const char *file)
{
    setenv("XAUTHORITY", file, 1);
    FILE *p = popen("xauth -q source /dev/stdin", "w");
    if (!p) return 0;
    fprintf(p, "add %s . %s\n", DISPLAY_NAME, cookie);
    return pclose(p) == 0;
}

/* The name that was last logged on, kept across our own restarts. */
#define LAST_FILE RUN_DIR "/last"
static void last_user_save(const char *user)
{
    FILE *f = fopen(LAST_FILE, "w");
    if (!f) return;
    fputs(user, f);
    fclose(f);
}
static void last_user_load(char *buf, size_t n)
{
    buf[0] = 0;
    FILE *f = fopen(LAST_FILE, "r");
    if (!f) return;
    if (!fgets(buf, (int)n, f)) buf[0] = 0;
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
}

static int start_x(void)
{
    mkdir(RUN_DIR, 0700);
    /* A cookie for the server, and for whoever we let in. */
    unsigned char raw[16];
    FILE *r = fopen("/dev/urandom", "rb");
    if (!r || fread(raw, 1, 16, r) != 16) { if (r) fclose(r); return 0; }
    fclose(r);
    for (int i = 0; i < 16; i++) snprintf(cookie + 2 * i, 3, "%02x", raw[i]);
    unlink(AUTH_FILE);
    if (!xauth_add(AUTH_FILE)) { log_line("xauth failed"); return 0; }

    /* The server signals SIGUSR1 to a parent that ignores it once it is
     * ready to take connections. */
    struct sigaction sa = { .sa_handler = on_usr1 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    struct sigaction sc = { .sa_handler = on_chld, .sa_flags = SA_NOCLDSTOP };
    sigemptyset(&sc.sa_mask);
    sigaction(SIGCHLD, &sc, NULL);

    x_ready = x_died = 0;
    xpid = fork();
    if (xpid == 0) {
        signal(SIGUSR1, SIG_IGN);          /* the handshake */
        setsid();
        execlp("Xorg", "Xorg", DISPLAY_NAME, "vt" VT_NUM, "-nolisten", "tcp",
               "-auth", AUTH_FILE, "-background", "none", "-noreset",
               (char *)NULL);
        execlp("X", "X", DISPLAY_NAME, "vt" VT_NUM, "-nolisten", "tcp",
               "-auth", AUTH_FILE, (char *)NULL);
        _exit(127);
    }
    if (xpid < 0) return 0;
    for (int i = 0; i < 300 && !x_ready; i++) {
        int st;
        if (waitpid(xpid, &st, WNOHANG) == xpid) { log_line("the X server exited before it was ready"); return 0; }
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (!x_ready) { log_line("the X server did not come up"); kill(xpid, SIGTERM); return 0; }
    setenv("DISPLAY", DISPLAY_NAME, 1);
    setenv("XAUTHORITY", AUTH_FILE, 1);
    return 1;
}

static int x_alive(void)
{
    int st;
    return xpid > 0 && waitpid(xpid, &st, WNOHANG) == 0;
}

/* ------------------------------------------------------------------ *
 * The logon dialog
 * ------------------------------------------------------------------ */
typedef struct {
    W2kWin  *win;
    W2kEdit *user, *pass;
    W2kRect  dlg, ok, cancel, shutdown, options, dialup;
    int      down, options_open, busy;
    char     message[200];
    W2kSkin *banner;
    int      want;               /* 0 log on, 10 shut down, 11 restart */
    W2kLogonCfg cfg;             /* the look, from the last user's ~/.w2k/logon */
    Pixmap   wall;               /* the wallpaper at the screen's size, or 0 */
    Pixmap   art;                /* the banner's artwork (Linux 2000, the distribution) */
    int      art_w, art_h;
    char     distro[128];
} Logon;

static Logon lg;

/* Whose look the logon screen shows: the user named, or with `fallback`
 * the first ordinary one when there is no such user; for a picture of
 * the screen, whoever runs it. */
typedef struct { char home[1024], name[64]; uid_t uid; gid_t gid; } Who;
static Who shown;                  /* whose picture and wallpaper are up */

static int who_is(const char *user, Who *u, int fallback)
{
    memset(u, 0, sizeof *u);
    if (getenv("W2K_RENDER") && getenv("HOME")) {
        snprintf(u->home, sizeof u->home, "%s", getenv("HOME"));
        u->uid = getuid();
        u->gid = getgid();
        return 1;
    }
    struct passwd *pw = user && *user ? getpwnam(user) : NULL;
    int listed = 0;
    if (!pw && fallback) {
        setpwent();
        listed = 1;
        while ((pw = getpwent()))
            if (pw->pw_uid >= 1000 && pw->pw_uid < 60000 && pw->pw_dir && pw->pw_dir[0] == '/') break;
    }
    int ok = pw && pw->pw_dir && pw->pw_name;
    if (ok) {
        snprintf(u->home, sizeof u->home, "%s", pw->pw_dir);
        snprintf(u->name, sizeof u->name, "%s", pw->pw_name);
        u->uid = pw->pw_uid;
        u->gid = pw->pw_gid;
    }
    if (listed) endpwent();
    return ok;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* All of `n` bytes from `fd`, or 0 at the deadline or the end. */
static int read_until(int fd, void *buf, size_t n, long deadline)
{
    size_t got = 0;
    while (got < n) {
        long left = deadline - now_ms();
        if (left <= 0) return 0;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        ssize_t k = read(fd, (char *)buf + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        got += (size_t)k;
    }
    return 1;
}

static int write_all(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t k = write(fd, (const char *)buf + done, n - done);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        done += (size_t)k;
    }
    return 1;
}

/* A picture a user's settings name, decoded by a child running as that
 * user -- never by root. The logon screen is up before anyone has logged
 * on, and the paths come out of files the user writes: this way the file
 * is only ever one they could open themselves, and root's decoders never
 * parse it. A FIFO, a link to /dev/zero or a picture made to take the
 * decoder's time or memory ends the child, not the logon screen: it has a
 * gigabyte and ten seconds, and is killed at the deadline even if it has
 * been stopped. The child keeps no descriptor but the pipe -- nothing of
 * the X connection, over which the password is typed.
 *
 * What comes back is exactly w x h: the picture stretched to it, or with
 * `square`, cropped to its middle square first. */
static unsigned char *user_picture(const Who *u, const char *path, int w, int h, int square)
{
    if (!path || !path[0] || w <= 0 || h <= 0) return NULL;
    int fd[2];
    if (pipe(fd) != 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return NULL; }
    if (pid == 0) {
        long maxfd = sysconf(_SC_OPEN_MAX);
        for (int i = 3; i < (maxfd > 0 && maxfd < 65536 ? maxfd : 1024); i++)
            if (i != fd[1]) close(i);
        if (lg.pass && lg.pass->text && lg.pass->cap > 0) memset(lg.pass->text, 0, (size_t)lg.pass->cap);
        signal(SIGALRM, SIG_DFL);
        alarm(10);
        struct rlimit mem = { 1UL << 30, 1UL << 30 };
        setrlimit(RLIMIT_AS, &mem);
        /* (Root's own look is root's to decode.) */
        if (geteuid() == 0 && u->uid != 0 &&
            ((u->name[0] && initgroups(u->name, u->gid) != 0 && setgroups(0, NULL) != 0) ||
             (!u->name[0] && setgroups(0, NULL) != 0) ||
             setgid(u->gid) != 0 || setuid(u->uid) != 0 || setuid(0) == 0))
            _exit(1);
        /* A regular file, checked on the descriptor and decoded through
         * it: a FIFO would hold the child to its deadline at every try. */
        int pf = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
        struct stat st, vs;
        char via[64];
        snprintf(via, sizeof via, "/proc/self/fd/%d", pf);
        if (pf < 0 || fstat(pf, &st) != 0 || !S_ISREG(st.st_mode) ||
            stat(via, &vs) != 0 || vs.st_ino != st.st_ino || vs.st_dev != st.st_dev)
            _exit(1);
        w2k_image_max_pixels = 40L * 1000 * 1000;
        int iw = 0, ih = 0;
        unsigned char *rgba = w2k_image_load_scaled(via, w, h, &iw, &ih);
        if (!rgba || iw <= 0 || ih <= 0) _exit(1);
        if (square) {
            int side = iw < ih ? iw : ih, ox = (iw - side) / 2, oy = (ih - side) / 2;
            for (int y = 0; y < side; y++)
                memmove(rgba + (size_t)y * side * 4, rgba + ((size_t)(oy + y) * iw + ox) * 4,
                        (size_t)side * 4);
            iw = ih = side;
        }
        unsigned char *out = iw == w && ih == h ? rgba
                           : w2k_rgba_resample(rgba, iw, ih, w, h, RS_CUBIC);
        _exit(out && write_all(fd[1], out, (size_t)w * h * 4) ? 0 : 1);
    }
    close(fd[1]);
    unsigned char *px = malloc((size_t)w * h * 4);
    if (px && !read_until(fd[0], px, (size_t)w * h * 4, now_ms() + 12000)) { free(px); px = NULL; }
    close(fd[0]);
    /* Done, or stuck: either way it goes, and is reaped here. */
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) ;
    return px;
}

/* The account picture, through the same child, as whoever it is shown for. */
static unsigned char *picture_as_user(const char *path, int size, int *w, int *h)
{
    unsigned char *px = user_picture(&shown, path, size, size, 1);
    if (px) *w = *h = size;
    return px;
}

/* The look: from the last user's ~/.w2k/logon -- the colour or wallpaper,
 * the banner's artwork, whether the user's picture shows. The settings
 * files are read with lib/logon.c's confined open (no FIFO, no symlink,
 * owned by the home's owner, small); pictures go through user_picture(). */
static void look_load(const char *user)
{
    who_is(user, &shown, 1);
    w2k_logon_load(&lg.cfg, shown.home[0] ? shown.home : NULL);
    w2k_account_load_from(shown.home);
    w2k_account_decoder = picture_as_user;
    if (lg.wall) { XFreePixmap(w2k.dpy, lg.wall); lg.wall = 0; }
    if (lg.art)  { XFreePixmap(w2k.dpy, lg.art);  lg.art = 0; }
    if (lg.cfg.wallpaper[0] && shown.home[0]) {
        unsigned char *px = user_picture(&shown, lg.cfg.wallpaper, w2k.sw, w2k.sh, 0);
        if (px) { lg.wall = w2k_pixmap_from_rgba(px, w2k.sw, w2k.sh, NULL); free(px); }
    }
    static const int white[3] = { 255, 255, 255 };
    if (lg.cfg.art == LOGON_ART_LINUX2000) {
        char path[1024];
        int iw = 0, ih = 0;
        unsigned char *rgba = w2k_skin_path("l2logo.png", path, sizeof path)
                            ? w2k_image_load(path, &iw, &ih) : NULL;
        if (rgba && iw > 0 && ih > 0) {
            int h = w2k_px(BANNER_H - 16), w = h * iw / ih;
            unsigned char *sc = w2k_rgba_resample(rgba, iw, ih, w, h, RS_CUBIC);
            if (sc) { lg.art = w2k_pixmap_from_rgba(sc, w, h, white); lg.art_w = w; lg.art_h = h; free(sc); }
        }
        free(rgba);
    } else if (lg.cfg.art == LOGON_ART_DISTRO) {
        char path[1024];
        int iw = 0, ih = 0;
        unsigned char *rgba = w2k_distro_logo_path(path, sizeof path)
                            ? w2k_image_load(path, &iw, &ih) : NULL;
        if (rgba && iw > 0 && ih > 0) {
            int s = w2k_px(56);
            unsigned char *sc = w2k_rgba_resample(rgba, iw, ih, s, s, RS_CUBIC);
            if (sc) { lg.art = w2k_pixmap_from_rgba(sc, s, s, white); lg.art_w = lg.art_h = s; free(sc); }
        }
        free(rgba);
        w2k_distro_pretty_name(lg.distro, sizeof lg.distro);
    }
}

/* The name typed changes whose picture shows. */
static void user_changed(void *u)
{
    (void)u;
    if (!lg.user || getenv("W2K_RENDER")) return;
    Who who;
    if (!who_is(w2k_edit_text(lg.user), &who, 0) || who.uid < 1000 || !who.home[0]) return;
    if (who.uid == shown.uid && !strcmp(who.home, shown.home)) return;
    shown = who;
    w2k_account_load_from(shown.home);
    if (lg.win) w2k_win_dirty(lg.win);
}

static void paint(W2kWin *w, Drawable d)
{
    int fh = w2k_font_height(F_UI);
    if (lg.wall)
        XCopyArea(w2k.dpy, lg.wall, d, w2k.gc, 0, 0, (unsigned)w2k.sw, (unsigned)w2k.sh, 0, 0);
    else {
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(lg.cfg.bg[0], lg.cfg.bg[1], lg.cfg.bg[2]));
        XFillRectangle(w2k.dpy, d, w2k.gc, 0, 0, (unsigned)w->w, (unsigned)w->h);
    }

    W2kRect r = lg.dlg;
    w2k_fill(d, r.x, r.y, r.w, r.h, C_FACE);
    w2k_edge(d, r.x, r.y, r.w, r.h, EDGE_RAISED, BF_RECT);
    w2k_gradient(d, r.x + 3, r.y + 3, r.w - 6, CAP_H, C_ACTIVETITLE, C_ACTIVETITLE2);
    w2k_text(d, F_UI_BOLD, r.x + 3 + 5, r.y + 3 + (CAP_H - w2k_font_height(F_UI_BOLD)) / 2,
             lg.cfg.art == LOGON_ART_LINUX2000 ? "Log On to Linux 2000" :
             lg.cfg.art == LOGON_ART_DISTRO ? "Log On" : "Log On to Windows", C_TITLETEXT);

    int by = r.y + 3 + CAP_H;
    w2k_fill(d, r.x + 3, by, r.w - 6, BANNER_H, C_WINDOW);
    /* The user's picture at the banner's right end, framed, when asked. */
    int pic_w = lg.cfg.show_picture ? 48 + 24 : 0;
    if (lg.cfg.show_picture) {
        int px = r.x + r.w - 3 - 12 - 48, py = by + (BANNER_H - 48) / 2;
        w2k_edge(d, px - 2, py - 2, 52, 52, EDGE_SUNKEN, BF_RECT);
        w2k_account_picture_draw(d, px, py, 48, ICO_MYCOMPUTER);
    }
    if (lg.cfg.art == LOGON_ART_LINUX2000 && lg.art) {
        /* The logo, centred in what the picture leaves of the banner. */
        int aw = w2k_lp(lg.art_w), ah = w2k_lp(lg.art_h);
        int ax = r.x + 3 + (r.w - 6 - pic_w - aw) / 2, ay = by + (BANNER_H - ah) / 2;
        XCopyArea(w2k.dpy, lg.art, d, w2k.gc, 0, 0, (unsigned)lg.art_w, (unsigned)lg.art_h,
                  w2k_cx(ax), w2k_cx(ay));
    } else if (lg.cfg.art == LOGON_ART_DISTRO) {
        /* The distribution's logo, its name beside it. */
        int lx = r.x + 40, ly = by + (BANNER_H - 56) / 2;
        if (lg.art) XCopyArea(w2k.dpy, lg.art, d, w2k.gc, 0, 0, (unsigned)lg.art_w, (unsigned)lg.art_h,
                              w2k_cx(lx), w2k_cx(ly));
        else w2k_bigicon_draw(d, lx + 12, ly + 12, ICO_STARTFLAG);
        char buf[160];
        w2k_ellipsis(F_UI_BOLD, lg.distro, r.w - 6 - pic_w - 110 - 8, buf, sizeof buf);
        w2k_text(d, F_UI_BOLD, r.x + 110, by + 34, buf, C_TEXT);
    } else if (lg.banner)
        w2k_skin_draw(d, lg.banner, r.x + 3, by, 0, 0, w2k_skin_w(lg.banner), w2k_skin_h(lg.banner));
    else {
        w2k_bigicon_draw(d, r.x + 50, by + 30, ICO_STARTFLAG);
        w2k_text(d, F_UI_BOLD, r.x + 110, by + 34, "Windows 2000 Professional", C_TEXT);
    }
    w2k_hline(d, r.x + 3, by + BANNER_H, r.w - 6, C_SHADOW);
    w2k_hline(d, r.x + 3, by + BANNER_H + 1, r.w - 6, C_HILIGHT);

    w2k_text_mnemonic(d, F_UI, lg.user->r.x - 76, lg.user->r.y + (21 - fh) / 2, "&User name:", C_TEXT, 1);
    w2k_edit_draw(d, lg.user);
    w2k_text_mnemonic(d, F_UI, lg.pass->r.x - 76, lg.pass->r.y + (21 - fh) / 2, "&Password:", C_TEXT, 1);
    w2k_edit_draw(d, lg.pass);
    if (lg.options_open)
        w2k_draw_checkbox(d, lg.dialup.x, lg.dialup.y, "&Log on using dial-up connection", 0, 0, 1);
    if (lg.message[0])
        w2k_text(d, F_UI, lg.dialup.x, lg.dialup.y + 22, lg.message, C_TEXT);

    w2k_draw_pushbutton(d, &lg.ok, "OK", BS_DEFAULT | (lg.down == 1 ? BS_PRESSED : 0) | (lg.busy ? BS_DISABLED : 0));
    w2k_draw_pushbutton(d, &lg.cancel, "Cancel", BS_DISABLED);
    w2k_draw_pushbutton(d, &lg.shutdown, "&Shutdown...", lg.down == 3 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &lg.options, lg.options_open ? "&Options <<" : "&Options >>",
                        lg.down == 4 ? BS_PRESSED : 0);
}

static void layout(W2kWin *w)
{
    const W2kMonitor *m = w2k_monitor_primary();
    int mx = m ? m->x : 0, my = m ? m->y : 0, mw = m ? m->w : w->w, mh = m ? m->h : w->h;
    lg.dlg = (W2kRect){ mx + (mw - DLG_W) / 2, my + (mh - DLG_H) / 2 - 50, DLG_W, DLG_H };
    W2kRect r = lg.dlg;
    int body = r.y + 3 + CAP_H + BANNER_H + 2;
    lg.user->r = (W2kRect){ r.x + 87, body + 14, 242, 21 };
    lg.pass->r = (W2kRect){ r.x + 87, body + 42, 242, 21 };
    lg.dialup = (W2kRect){ r.x + 87, body + 72, 242, 18 };
    int bx = r.x + 87, byy = r.y + r.h - 12 - 23;
    lg.ok       = (W2kRect){ bx, byy, 75, 23 };
    lg.cancel   = (W2kRect){ bx + 81, byy, 75, 23 };
    lg.shutdown = (W2kRect){ bx + 162, byy, 75, 23 };
    lg.options  = (W2kRect){ bx + 243, byy, 75, 23 };
}

static void set_message(const char *m)
{
    snprintf(lg.message, sizeof lg.message, "%s", m ? m : "");
    w2k_win_dirty(lg.win);
}

static void logon_failed(const char *why)
{
    lg.busy = 0;
    w2k_edit_set(lg.pass, "");
    lg.pass->focused = 1;
    lg.user->focused = 0;
    w2k_win_dirty(lg.win);
    w2k_msgbox(lg.win, "Logon Message", why && *why ? why :
               "The system could not log you on. Make sure your User name and "
               "domain are correct, then type your password again. Letters in "
               "passwords must be typed using the correct case.",
               MB_OK | MB_ICONWARNING);
}

/* ------------------------------------------------------------------ *
 * PAM
 * ------------------------------------------------------------------ */
#ifdef HAVE_PAM
static pam_handle_t *pamh;
static char pam_info[300];

/* The conversation: the dialog already holds both answers. */
static int converse(int n, const struct pam_message **msg, struct pam_response **resp, void *u)
{
    (void)u;
    struct pam_response *r = calloc((size_t)n, sizeof *r);
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < n; i++) {
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF: r[i].resp = strdup(w2k_edit_text(lg.pass)); break;
        case PAM_PROMPT_ECHO_ON:  r[i].resp = strdup(w2k_edit_text(lg.user)); break;
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            snprintf(pam_info, sizeof pam_info, "%s", msg[i]->msg ? msg[i]->msg : "");
            r[i].resp = NULL;
            break;
        default: free(r); return PAM_CONV_ERR;
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}

/* Name and password through the stack; on success pamh stays for
 * run_session(), which opens the session. */
static int authenticate(const char *user)
{
    struct pam_conv conv = { converse, NULL };
    pam_info[0] = 0;
    int rc = pam_start("l2kdm", user, &conv, &pamh);
    if (rc != PAM_SUCCESS) { log_line("pam_start: %s", pam_strerror(NULL, rc)); return 0; }
    pam_set_item(pamh, PAM_TTY, "tty" VT_NUM);
    pam_set_item(pamh, PAM_XDISPLAY, DISPLAY_NAME);
    /* What logind wants to know to call this a graphical seat session. */
    pam_putenv(pamh, "XDG_SEAT=seat0");
    pam_putenv(pamh, "XDG_VTNR=" VT_NUM);
    pam_putenv(pamh, "XDG_SESSION_TYPE=x11");
    pam_putenv(pamh, "XDG_SESSION_CLASS=user");
    pam_putenv(pamh, "XDG_SESSION_DESKTOP=w2k");
    pam_putenv(pamh, "XDG_CURRENT_DESKTOP=W2K");

    rc = pam_authenticate(pamh, 0);
    if (rc == PAM_SUCCESS) rc = pam_acct_mgmt(pamh, 0);
    if (rc == PAM_NEW_AUTHTOK_REQD) {
        /* The conversation only knows the password that was typed, so it
         * cannot answer a "new password" prompt: say so rather than have
         * the stack reject the old one twice. */
        snprintf(pam_info, sizeof pam_info,
                 "Your password has expired and must be changed before you can "
                 "log on. Change it from a text console (Ctrl+Alt+F2), then try again.");
    }
    if (rc != PAM_SUCCESS) {
        log_line("logon for %s refused: %s", user, pam_strerror(pamh, rc));
        pam_end(pamh, rc);
        pamh = NULL;
        return 0;
    }
    return 1;
}

/* The session, as the user, with what PAM put in the environment. */
static void pam_finish(void)
{
    if (!pamh) return;
    pam_close_session(pamh, 0);
    pam_setcred(pamh, PAM_DELETE_CRED);
    pam_end(pamh, PAM_SUCCESS);
    pamh = NULL;
}


/* The desktop itself, as the user. Runs in a child of the session leader. */
static void exec_desktop(struct passwd *pw)
{
    char **env = pam_getenvlist(pamh);
    char home[1200], usr[300], logname[300], shell[300], path[400], disp[64], xauth[1300];
    /* Tells l2k-session to hand a shutdown or restart back here (its exit
     * status) rather than ask for one itself: root does it without the
     * user's own request being refused while others are logged on. */
    char marker[] = "L2KDM=1";
    snprintf(home, sizeof home, "HOME=%s", pw->pw_dir);
    snprintf(usr, sizeof usr, "USER=%s", pw->pw_name);
    snprintf(logname, sizeof logname, "LOGNAME=%s", pw->pw_name);
    snprintf(shell, sizeof shell, "SHELL=%s", pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh");
    snprintf(path, sizeof path, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/games:/usr/games");
    snprintf(disp, sizeof disp, "DISPLAY=%s", DISPLAY_NAME);
    snprintf(xauth, sizeof xauth, "XAUTHORITY=%s/.Xauthority", pw->pw_dir);
    int n = 0;
    for (char **e = env; e && *e; e++) n++;
    char **envp = calloc((size_t)n + 13, sizeof *envp);
    if (!envp) _exit(126);
    int k = 0;
    for (char **e = env; e && *e; e++) envp[k++] = *e;
    envp[k++] = home; envp[k++] = usr; envp[k++] = logname; envp[k++] = shell;
    envp[k++] = path; envp[k++] = disp; envp[k++] = xauth; envp[k++] = marker;
    envp[k] = NULL;

    /* Nothing of ours -- the display connection, the journal -- goes
     * into the session. */
    long maxfd = sysconf(_SC_OPEN_MAX);
    for (int fd = 3; fd < (maxfd > 0 && maxfd < 65536 ? maxfd : 1024); fd++) close(fd);
    /* The groups were set by the leader, before pam_setcred added any of
     * its own (pam_group): initgroups here threw those away. */
    if (setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0 || setuid(0) == 0) _exit(126);
    if (chdir(pw->pw_dir) != 0) chdir("/");
    /* The server's cookie, in the user's own file. */
    char uauth[1200];
    snprintf(uauth, sizeof uauth, "%s/.Xauthority", pw->pw_dir);
    if (!xauth_add(uauth)) _exit(125);
    char run[1200];
    snprintf(run, sizeof run, "exec '%s'", session_cmd);
    execle("/bin/sh", "sh", "-l", "-c", run, (char *)NULL, envp);
    _exit(127);
}

/* The session leader's answer to SIGTERM (the service stopping, the
 * machine shutting down): pass it to the desktop, and go on waiting so
 * the session is still closed once that has gone. The leader used to
 * inherit the service's own handler, which killed the X server and left
 * at once -- pam_close_session and the rest never ran. */
static volatile pid_t desktop_pid;
static volatile sig_atomic_t leader_stop;
static void leader_forward(int sig)
{
    leader_stop = sig;
    if (desktop_pid > 0) kill(desktop_pid, sig);
}

/* Run the desktop for an authenticated user and wait for it. A session
 * leader is forked first, and it is that process which opens the PAM
 * session: logind attaches the session to whoever calls pam_open_session,
 * and that must be something that ends when the user logs off -- not this
 * service, which lives for the whole boot. */
static int run_session(const char *user)
{
    /* Only ever reached after authenticate() succeeded. */
    if (!pamh) return -1;
    struct passwd *pw = getpwnam(user);
    if (!pw) { pam_end(pamh, PAM_SUCCESS); pamh = NULL; return -1; }
    pid_t leader = fork();
    if (leader == 0) {
        setsid();
        struct sigaction fw = { .sa_handler = leader_forward };
        sigemptyset(&fw.sa_mask);
        sigaction(SIGTERM, &fw, NULL);
        sigaction(SIGINT, &fw, NULL);
        sigaction(SIGHUP, &fw, NULL);
        /* The user's groups before pam_setcred, which may add to them. */
        if (initgroups(pw->pw_name, pw->pw_gid) != 0) {
            log_line("no groups for %s", user);
            pam_end(pamh, PAM_SUCCESS);
            _exit(124);
        }
        int rc = pam_setcred(pamh, PAM_ESTABLISH_CRED);
        if (rc == PAM_SUCCESS) rc = pam_open_session(pamh, 0);
        if (rc != PAM_SUCCESS) {
            log_line("session for %s refused: %s", user, pam_strerror(pamh, rc));
            pam_end(pamh, rc);
            _exit(124);
        }
        /* Held until the desktop's pid is known, so a stop arriving now is
         * passed on rather than lost. */
        sigset_t term, old;
        sigemptyset(&term);
        sigaddset(&term, SIGTERM);
        sigaddset(&term, SIGINT);
        sigaddset(&term, SIGHUP);
        sigprocmask(SIG_BLOCK, &term, &old);
        pid_t pid = leader_stop ? -1 : fork();
        if (pid == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            sigprocmask(SIG_SETMASK, &old, NULL);
            exec_desktop(pw);
        }
        desktop_pid = pid;
        sigprocmask(SIG_SETMASK, &old, NULL);
        int st = 0;
        if (pid > 0) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
        pam_finish();
        _exit(pid < 0 ? 1 : WIFEXITED(st) ? WEXITSTATUS(st) : 1);
    }
    if (leader < 0) { pam_end(pamh, PAM_SUCCESS); pamh = NULL; return -1; }
    /* The leader has its own copy of the handle and closes the session
     * with it. This one goes now, quietly (PAM_DATA_SILENT: the modules
     * leave what they set up alone), and the password PAM kept with it as
     * the authentication token goes too -- it stayed in this process,
     * which lives for the whole boot, until the user logged off. */
    pam_end(pamh, PAM_SUCCESS | PAM_DATA_SILENT);
    pamh = NULL;
    session_pid = leader;
    int st = 0;
    while (waitpid(leader, &st, 0) < 0 && errno == EINTR) ;
    session_pid = 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
#else
static int authenticate(const char *user)
{
    (void)user;
    set_message("Built without PAM: no one can log on.");
    return 0;
}
static int run_session(const char *user) { (void)user; return -1; }
#endif

/* ------------------------------------------------------------------ *
 * Shut Down Windows
 * ------------------------------------------------------------------ */
typedef struct { W2kCombo *what; W2kRect ok, cancel; int down; } ShutDlg;

static void shut_paint(W2kWin *w, Drawable d)
{
    ShutDlg *s = w->user;
    w2k_bigicon_draw(d, 14, 14, ICO_SHUTDOWN);
    w2k_text(d, F_UI, 60, 14, "What do you want the computer to do?", C_TEXT);
    w2k_combo_draw(d, s->what);
    w2k_draw_pushbutton(d, &s->ok, "OK", BS_DEFAULT | (s->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &s->cancel, "Cancel", s->down == 2 ? BS_PRESSED : 0);
}

static int shut_event(W2kWin *w, XEvent *e)
{
    ShutDlg *s = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_combo_press(s->what, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&s->ok, e->xbutton.x, e->xbutton.y)) s->down = 1;
        else if (w2k_rect_hit(&s->cancel, e->xbutton.x, e->xbutton.y)) s->down = 2;
        w2k_win_dirty(w);
        return 1;
    case ButtonRelease: {
        int b = s->down;
        s->down = 0;
        if (b == 1 && w2k_rect_hit(&s->ok, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_OK);
        else if (b == 2 && w2k_rect_hit(&s->cancel, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) w2k_win_close(w, ID_CANCEL);
        if (ks == XK_Return) w2k_win_close(w, ID_OK);
        return 1;
    }
    }
    return 0;
}

static void do_shutdown_dialog(void)
{
    ShutDlg s = { 0 };
    W2kWin *w = w2k_win_new("Shut Down Windows", "l2kdm", 400, 150, 0);
    s.what = w2k_combo_new(0);
    w2k_combo_add(s.what, "Shut down");
    w2k_combo_add(s.what, "Restart");
    s.what->r = (W2kRect){ 60, 40, 300, 21 };
    s.ok = (W2kRect){ 400 - 12 - 75 * 2 - 6, 150 - 12 - 23, 75, 23 };
    s.cancel = (W2kRect){ 400 - 12 - 75, 150 - 12 - 23, 75, 23 };
    w->user = &s;
    w->paint = shut_paint;
    w->event = shut_event;
    w2k_win_center(w, lg.win);
    int rc = w2k_win_modal(w);
    int what = s.what->sel;
    w2k_combo_free(s.what);
    if (rc != ID_OK) return;
    lg.want = what == 1 ? 11 : 10;
    w2k_win_close(lg.win, ID_CANCEL);
}

/* ------------------------------------------------------------------ *
 * Events
 * ------------------------------------------------------------------ */
static void do_logon(void)
{
    const char *user = w2k_edit_text(lg.user);
    if (!*user || lg.busy) return;
    lg.busy = 1;
    set_message("");
    XDefineCursor(w2k.dpy, lg.win->win, w2k.cur_wait);
    w2k_win_repaint_now(lg.win);
    int ok = authenticate(user);
    /* The password has done its work: it does not stay in memory for the
     * rest of the session. */
    w2k_edit_wipe(lg.pass);
    XDefineCursor(w2k.dpy, lg.win->win, w2k.cur_arrow);
    if (ok) { lg.want = 0; w2k_win_close(lg.win, ID_OK); }
    else {
#ifdef HAVE_PAM
        logon_failed(pam_info);
#else
        logon_failed(lg.message);
#endif
    }
}

static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_edit_press(lg.user, &e->xbutton)) { lg.user->focused = 1; lg.pass->focused = 0; w2k_win_dirty(w); return 1; }
        if (w2k_edit_press(lg.pass, &e->xbutton)) { lg.pass->focused = 1; lg.user->focused = 0; w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&lg.ok, x, y) && !lg.busy) lg.down = 1;
        else if (w2k_rect_hit(&lg.shutdown, x, y)) lg.down = 3;
        else if (w2k_rect_hit(&lg.options, x, y)) lg.down = 4;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int x = e->xbutton.x, y = e->xbutton.y, b = lg.down;
        lg.down = 0;
        w2k_edit_release(lg.user);
        w2k_edit_release(lg.pass);
        if (b == 1 && w2k_rect_hit(&lg.ok, x, y)) do_logon();
        else if (b == 3 && w2k_rect_hit(&lg.shutdown, x, y)) do_shutdown_dialog();
        else if (b == 4 && w2k_rect_hit(&lg.options, x, y)) lg.options_open = !lg.options_open;
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (lg.user->focused) w2k_edit_motion(lg.user, &e->xmotion);
        if (lg.pass->focused) w2k_edit_motion(lg.pass, &e->xmotion);
        return 1;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Return || ks == XK_KP_Enter) { do_logon(); return 1; }
        if (ks == XK_Tab) {
            int u = lg.user->focused;
            lg.user->focused = !u;
            lg.pass->focused = u;
            w2k_win_dirty(w);
            return 1;
        }
        if (lg.user->focused && w2k_edit_key(lg.user, &e->xkey)) w2k_win_dirty(w);
        else if (lg.pass->focused && w2k_edit_key(lg.pass, &e->xkey)) w2k_win_dirty(w);
        return 1;
    }
    }
    return 0;
}

static void blink(void *u) { w2k_edit_blink(u); }
static void take_focus(void *u)
{
    (void)u;
    XSetInputFocus(w2k.dpy, lg.win->win, RevertToPointerRoot, CurrentTime);
    w2k_del_timer(take_focus, NULL);
}

static int quiet_xerror(Display *d, XErrorEvent *e)
{
    char msg[128];
    XGetErrorText(d, e->error_code, msg, sizeof msg);
    log_line("X error ignored: %s (request %d)", msg, e->request_code);
    return 0;
}

/* systemctl stop, or the shutdown: the session and the server go too. */
static void on_term(int sig)
{
    (void)sig;
    if (session_pid > 0) kill(session_pid, SIGTERM);
    if (xpid > 0) kill(xpid, SIGTERM);
    _exit(0);
}

static int quiet_xio(Display *d)
{
    (void)d;
    log_line("lost the X server");
    _exit(1);                        /* systemd starts us again */
}

/* Put the dialog up and wait for a logon (ID_OK) or a shutdown request. */
static int logon_screen(const char *last_user, char *user_out, int n)
{
    memset(&lg, 0, sizeof lg);
    lg.win = w2k_win_new("Log On to Windows", "l2kdm", w2k_lp(w2k.sw), w2k_lp(w2k.sh), 0);
    lg.win->paint = paint;
    lg.win->event = event;
    lg.win->resized = layout;
    XSetWindowAttributes a = { .override_redirect = True };
    XChangeWindowAttributes(w2k.dpy, lg.win->win, CWOverrideRedirect, &a);
    XMoveWindow(w2k.dpy, lg.win->win, 0, 0);
    lg.user = w2k_edit_new(0);
    w2k_edit_bind(lg.user, lg.win);
    lg.pass = w2k_edit_new(0);
    lg.pass->password = 1;
    /* For a picture of the screen only: a password to show masked. */
    if (getenv("W2K_RENDER") && getenv("W2K_RENDER_PASSWORD")) w2k_edit_set(lg.pass, getenv("W2K_RENDER_PASSWORD"));
    w2k_edit_bind(lg.pass, lg.win);
    if (last_user && *last_user) w2k_edit_set(lg.user, last_user);
    else {
        struct passwd *pw;
        setpwent();
        while ((pw = getpwent()))
            if (pw->pw_uid >= 1000 && pw->pw_uid < 60000 && pw->pw_shell &&
                !strstr(pw->pw_shell, "nologin") && !strstr(pw->pw_shell, "false")) {
                w2k_edit_set(lg.user, pw->pw_name);
                break;
            }
        endpwent();
    }
    lg.pass->focused = 1;
    lg.options_open = 1;
    layout(lg.win);
    look_load(w2k_edit_text(lg.user));
    lg.user->on_change = user_changed;
    char path[1024];
    if (lg.cfg.art == LOGON_ART_WINDOWS &&
        w2k_skin_path("logon-banner.png", path, sizeof path)) lg.banner = w2k_skin_load(path);

    w2k_add_timer(w2k_caret_blink, blink, lg.user);
    w2k_add_timer(w2k_caret_blink, blink, lg.pass);
    w2k_add_timer(200, take_focus, NULL);
    int rc = w2k_win_modal(lg.win);
    w2k_del_timer(blink, lg.user);
    w2k_del_timer(blink, lg.pass);
    snprintf(user_out, (size_t)n, "%s", w2k_edit_text(lg.user));
    w2k_edit_free(lg.user);
    w2k_edit_free(lg.pass);
    if (lg.banner) w2k_skin_free(lg.banner);
    if (lg.wall) XFreePixmap(w2k.dpy, lg.wall);
    if (lg.art)  XFreePixmap(w2k.dpy, lg.art);
    XFlush(w2k.dpy);
    /* Log on only when the dialog was dismissed by a successful logon:
     * anything else (a shutdown request, or a close forced on the window
     * from outside) is never an authentication. */
    if (rc == ID_OK) return 0;
    return lg.want == 10 || lg.want == 11 ? lg.want : -1;
}

/* 10 shuts down, 11 restarts; whichever tool the system has. */
static void power(int what)
{
    const char *verb = what == 10 ? "poweroff" : "reboot";
    execlp("systemctl", "systemctl", verb, (char *)NULL);
    execlp("loginctl", "loginctl", verb, (char *)NULL);
    execlp(verb, verb, (char *)NULL);
    log_line("cannot %s: no systemctl, loginctl or %s", verb, verb);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--check")) {
#ifdef HAVE_PAM
        return 0;
#else
        return 1;
#endif
    }
    if (geteuid() != 0 && !getenv("W2K_RENDER")) {
        fprintf(stderr, "l2kdm: must run as root (it is a service: systemctl start l2kdm)\n");
        return 1;
    }
    /* The session lives beside us. */
    char self[1024];
    ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
    if (len > 0) {
        self[len] = 0;
        char *slash = strrchr(self, '/');
        if (slash) { *slash = 0; snprintf(session_cmd, sizeof session_cmd, "%.900s/l2k-session", self); }
    }

    struct sigaction st = { .sa_handler = on_term };
    sigemptyset(&st.sa_mask);
    sigaction(SIGTERM, &st, NULL);
    sigaction(SIGINT, &st, NULL);

    int own_x = !getenv("DISPLAY");
    if (own_x && !start_x()) {
        log_line("no X server: see the lines above (journalctl -u l2kdm)");
        return 1;
    }
    if (w2k_init("l2kdm") < 0) { log_line("cannot open the display"); return 1; }
    XSetErrorHandler(quiet_xerror);
    XSetIOErrorHandler(quiet_xio);
    /* The Windows arrow on the root as well, from the first moment. */
    XDefineCursor(w2k.dpy, w2k.root, w2k.cur_arrow);

    if (getenv("W2K_RENDER")) {          /* a picture of the screen */
        char u[64];
        logon_screen(NULL, u, sizeof u);
        return 0;
    }

    char last[64] = "";
    last_user_load(last, sizeof last);
    for (;;) {
        if (own_x && !x_alive()) { log_line("the X server has gone"); return 1; }
        char user[64];
        int want = logon_screen(last, user, sizeof user);
        if (want == 10 || want == 11) {
            log_line("%s requested from the logon screen", want == 10 ? "shutdown" : "restart");
            if (own_x) kill(xpid, SIGTERM);
            power(want);
        }
        if (want != 0) continue;             /* the dialog closed without a logon */
        snprintf(last, sizeof last, "%s", user);
        last_user_save(last);
        log_line("%s logged on", user);
        /* Nothing on the root for this connection while the session runs:
         * it is not read until the session is over, and the server would
         * queue every property change the desktop makes for it. */
        XSelectInput(w2k.dpy, w2k.root, NoEventMask);
        XSync(w2k.dpy, False);
        int rc = run_session(user);
        XSelectInput(w2k.dpy, w2k.root, PropertyChangeMask);
        log_line("session for %s ended with %d", user, rc);
        if (rc == 10 || rc == 11) {
            if (own_x) kill(xpid, SIGTERM);
            power(rc);
        }
        if (own_x) {
            /* The cookie that session was given must not open the next
             * one's server, so the server goes with the session: stop it,
             * wait for the terminal to come free, and start over with a
             * new server and a new cookie. */
            kill(xpid, SIGTERM);
            int st;
            while (waitpid(xpid, &st, 0) < 0 && errno == EINTR) ;
            /* start_x() set these for itself: left in, they made the new
             * instance think it ran under someone else's server, start
             * none, fail to connect and wait for systemd's restart. */
            unsetenv("DISPLAY");
            unsetenv("XAUTHORITY");
            execv("/proc/self/exe", argv);
            return 0;                        /* systemd starts us again */
        }
    }
}
