/* w2kdm.c -- the display manager: the machine boots straight into
 * "Log On to Windows".
 *
 * A root service (config/w2kdm.service) that starts the X server on a
 * virtual terminal, shows the Windows 2000 logon dialog on it with the
 * shell's own toolkit, checks the name and password through PAM, opens a
 * PAM session (which is what registers the login with logind), and runs
 * w2k-session as that user. When the session ends the logon screen comes
 * back; when it ends asking for it, the machine shuts down or restarts.
 * Nothing else stands between the boot and the desktop. */
#include "w2kui.h"
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_PAM
#include <security/pam_appl.h>
#endif

#define DISPLAY_NAME ":0"
#define VT_NUM       "7"
#define RUN_DIR      "/run/w2kdm"
#define AUTH_FILE    RUN_DIR "/Xauthority"

/* Measured off a 800x600 screenshot: the dialog's frame is 415 by 254; the
 * banner fills its top 92 rows under the caption. */
#define DLG_W   415
#define DLG_H   254
#define CAP_H    18
#define BANNER_H 92

static char session_cmd[1024] = "/usr/local/bin/w2k-session";
static char cookie[33];
static pid_t xpid;
static volatile sig_atomic_t x_ready, x_died;

static void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("w2kdm: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ *
 * The X server
 * ------------------------------------------------------------------ */
static void on_usr1(int s) { (void)s; x_ready = 1; }
static void on_chld(int s) { (void)s; x_died = 1; }

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
    char cmd[300];
    snprintf(cmd, sizeof cmd, "xauth -q -f %s add %s . %s", AUTH_FILE, DISPLAY_NAME, cookie);
    if (system(cmd) != 0) { log_line("xauth failed"); return 0; }

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
} Logon;

static Logon lg;

static void paint(W2kWin *w, Drawable d)
{
    int fh = w2k_font_height(F_UI);
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(58, 110, 165));
    XFillRectangle(w2k.dpy, d, w2k.gc, 0, 0, (unsigned)w->w, (unsigned)w->h);

    W2kRect r = lg.dlg;
    w2k_fill(d, r.x, r.y, r.w, r.h, C_FACE);
    w2k_edge(d, r.x, r.y, r.w, r.h, EDGE_RAISED, BF_RECT);
    w2k_gradient(d, r.x + 3, r.y + 3, r.w - 6, CAP_H, C_ACTIVETITLE, C_ACTIVETITLE2);
    w2k_text(d, F_UI_BOLD, r.x + 3 + 5, r.y + 3 + (CAP_H - w2k_font_height(F_UI_BOLD)) / 2,
             "Log On to Windows", C_TITLETEXT);

    int by = r.y + 3 + CAP_H;
    w2k_fill(d, r.x + 3, by, r.w - 6, BANNER_H, C_WINDOW);
    if (lg.banner)
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

/* Name and password through the stack; on success the session is open
 * and pamh stays for run_session(). */
static int authenticate(const char *user)
{
    struct pam_conv conv = { converse, NULL };
    pam_info[0] = 0;
    int rc = pam_start("w2kdm", user, &conv, &pamh);
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
    if (rc == PAM_NEW_AUTHTOK_REQD) rc = pam_chauthtok(pamh, PAM_CHANGE_EXPIRED_AUTHTOK);
    if (rc == PAM_SUCCESS) rc = pam_setcred(pamh, PAM_ESTABLISH_CRED);
    if (rc == PAM_SUCCESS) rc = pam_open_session(pamh, 0);
    if (rc != PAM_SUCCESS) {
        log_line("logon for %s refused: %s", user, pam_strerror(pamh, rc));
        pam_end(pamh, rc);
        pamh = NULL;
        return 0;
    }
    return 1;
}

/* The session, as the user, with what PAM put in the environment. */
static int run_session(const char *user)
{
    struct passwd *pw = getpwnam(user);
    if (!pw) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        char **env = pam_getenvlist(pamh);
        char home[1200], usr[300], logname[300], shell[300], path[400], disp[64], xauth[1300];
        snprintf(home, sizeof home, "HOME=%s", pw->pw_dir);
        snprintf(usr, sizeof usr, "USER=%s", pw->pw_name);
        snprintf(logname, sizeof logname, "LOGNAME=%s", pw->pw_name);
        snprintf(shell, sizeof shell, "SHELL=%s", pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh");
        snprintf(path, sizeof path, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/games:/usr/games");
        snprintf(disp, sizeof disp, "DISPLAY=%s", DISPLAY_NAME);
        snprintf(xauth, sizeof xauth, "XAUTHORITY=%s/.Xauthority", pw->pw_dir);
        int n = 0;
        for (char **e = env; e && *e; e++) n++;
        char **envp = calloc((size_t)n + 12, sizeof *envp);
        int k = 0;
        for (char **e = env; e && *e; e++) envp[k++] = *e;
        envp[k++] = home; envp[k++] = usr; envp[k++] = logname; envp[k++] = shell;
        envp[k++] = path; envp[k++] = disp; envp[k++] = xauth;
        envp[k] = NULL;

        setsid();
        if (initgroups(pw->pw_name, pw->pw_gid) != 0 || setgid(pw->pw_gid) != 0 ||
            setuid(pw->pw_uid) != 0) _exit(126);
        if (chdir(pw->pw_dir) != 0) chdir("/");
        /* The server's cookie, in the user's own file. */
        char cmd[400];
        snprintf(cmd, sizeof cmd, "XAUTHORITY=%s/.Xauthority xauth -q add %s . %s", pw->pw_dir,
                 DISPLAY_NAME, cookie);
        system(cmd);
        char run[1200];
        snprintf(run, sizeof run, "exec '%s'", session_cmd);
        execle("/bin/sh", "sh", "-l", "-c", run, (char *)NULL, envp);
        _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    pam_close_session(pamh, 0);
    pam_setcred(pamh, PAM_DELETE_CRED);
    pam_end(pamh, PAM_SUCCESS);
    pamh = NULL;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
#else
static int authenticate(const char *user)
{
    (void)user;
    set_message("Built without PAM: no one can log on.");
    return 0;
}
static int run_session(const char *user) { (void)user; return 1; }
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
    W2kWin *w = w2k_win_new("Shut Down Windows", "w2kdm", 400, 150, 0);
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
    lg.win = w2k_win_new("Log On to Windows", "w2kdm", w2k.sw, w2k.sh, 0);
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
    char path[1024];
    if (w2k_skin_path("logon-banner.png", path, sizeof path)) lg.banner = w2k_skin_load(path);

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
    XFlush(w2k.dpy);
    return rc == ID_OK ? 0 : lg.want;
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
        fprintf(stderr, "w2kdm: must run as root (it is a service: systemctl start w2kdm)\n");
        return 1;
    }
    /* The session lives beside us. */
    char self[1024];
    ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
    if (len > 0) {
        self[len] = 0;
        char *slash = strrchr(self, '/');
        if (slash) { *slash = 0; snprintf(session_cmd, sizeof session_cmd, "%.900s/w2k-session", self); }
    }

    int own_x = !getenv("DISPLAY");
    if (own_x && !start_x()) return 1;
    if (w2k_init("w2kdm") < 0) { log_line("cannot open the display"); return 1; }
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
    for (;;) {
        if (own_x && !x_alive()) { log_line("the X server has gone"); return 1; }
        char user[64];
        int want = logon_screen(last, user, sizeof user);
        if (want == 10 || want == 11) {
            log_line("%s requested from the logon screen", want == 10 ? "shutdown" : "restart");
            if (own_x) kill(xpid, SIGTERM);
            execlp("systemctl", "systemctl", want == 10 ? "poweroff" : "reboot", (char *)NULL);
            return 0;
        }
        snprintf(last, sizeof last, "%s", user);
        log_line("%s logged on", user);
        int rc = run_session(user);
        log_line("session for %s ended with %d", user, rc);
        if (rc == 10 || rc == 11) {
            if (own_x) kill(xpid, SIGTERM);
            execlp("systemctl", "systemctl", rc == 10 ? "poweroff" : "reboot", (char *)NULL);
            return 0;
        }
    }
}
