/* l2kproton.c -- Proton Manager, and what Windows programs are opened with.
 *
 * Proton is Valve's Wine, with DXVK and the rest of what games need;
 * GE-Proton is a build of it made to be used outside Steam as well. Proton
 * Manager is a property sheet after Windows 2000's: the versions installed
 * and which is the default, the versions there are to download, the
 * programs with settings of their own, and the options they all run with.
 *
 *   l2kproton                    Proton Manager
 *   l2kproton download           ... open at its Download page
 *   l2kproton run FILE [ARG...]  run a Windows program, installer, shortcut
 *                                or batch file as its Compatibility tab says:
 *                                what Explorer opens them with
 *   l2kproton winecfg [NAME]     Wine's configuration for the Windows the
 *                                build NAME runs programs in (Wine's own
 *                                without one)
 *
 * The settings are ~/.w2k/compat (lib/proton.c). A build Proton Manager
 * installs lives in $XDG_DATA_HOME/l2k/proton/<name>, and runs programs in
 * a Windows of its own, $XDG_DATA_HOME/l2k/prefixes/<name>; a program
 * installed into one of those runs there whatever runs it, so that it finds
 * what it installed. What Proton says goes to ~/.w2k/proton.log. */
#include "w2kui.h"
#include <X11/keysym.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GE_API  "https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases?per_page=40"
#define GE_DL   "https://github.com/GloriousEggroll/proton-ge-custom/releases/download/"
#define GE_SITE "github.com/GloriousEggroll/proton-ge-custom"
#define UMU_API "https://api.github.com/repos/Open-Wine-Components/umu-launcher/releases/latest"

static char data_dir[PATH_MAX];         /* $XDG_DATA_HOME/l2k */
static char self_exe[PATH_MAX];         /* to run this program again */
static int  ui_state;                   /* 1 connected, -1 no display, 0 not tried */

/* ------------------------------------------------------------------ *
 * Small things
 * ------------------------------------------------------------------ */
static int ui(void)
{
    if (!ui_state) ui_state = w2k_init("l2kproton") < 0 ? -1 : 1;
    return ui_state > 0;
}

/* A message box, or standard error when there is no display to put one
 * on (and then the answer is no). */
static int say(int flags, const char *fmt, ...)
{
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (ui()) return w2k_msgbox(NULL, "Proton Manager", msg, flags);
    fprintf(stderr, "l2kproton: %s\n", msg);
    return (flags & 0x0f) ? ID_NO : ID_OK;
}

static void mkdirs(const char *path)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(buf, 0755); *s = '/'; }
    mkdir(buf, 0755);
}

static void home_path(char *buf, size_t n, const char *rel)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/%s", home ? home : ".", rel);
}

static void stdin_null(void)
{
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) { dup2(nul, 0); if (nul > 2) close(nul); }
}

/* Started apart from this process, which may be about to exit. */
static void spawn(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            stdin_null();
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

static int run_wait(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        stdin_null();
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* As Explorer puts it: "538 MB". */
static void size_text(long long b, char *out, int n)
{
    if (b >= 1024LL * 1024 * 1024) snprintf(out, (size_t)n, "%.1f GB", (double)b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)     snprintf(out, (size_t)n, "%lld MB", b / (1024 * 1024));
    else                           snprintf(out, (size_t)n, "%lld KB", (b + 1023) / 1024);
}

static void log_path(char *buf, size_t n) { home_path(buf, n, ".w2k/proton.log"); }

/* A folder as it is spoken of: ~ for the home folder. */
static void place(const char *path, char *out, size_t n)
{
    const char *home = getenv("HOME");
    size_t l = home ? strlen(home) : 0;
    if (l > 1 && !strncmp(path, home, l) && (path[l] == '/' || !path[l]))
        snprintf(out, n, "~%s", path + l);
    else
        snprintf(out, n, "%s", path);
}

/* Standard output and error into the log, under a line saying what is
 * being run. A log grown past a megabyte starts again. */
static void to_log(const char *what)
{
    char path[PATH_MAX], dir[PATH_MAX];
    log_path(path, sizeof path);
    home_path(dir, sizeof dir, ".w2k");
    mkdir(dir, 0755);
    struct stat st;
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    if (stat(path, &st) == 0 && st.st_size > 1024 * 1024) flags |= O_TRUNC;
    int fd = open(path, flags, 0644);
    if (fd < 0) return;
    char when[64];
    time_t t = time(NULL);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t));
    dprintf(fd, "\n==== %s  %s\n", when, what);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
}

/* ------------------------------------------------------------------ *
 * What runs a program, and where
 * ------------------------------------------------------------------ */
typedef struct {
    int       wine;                     /* Wine, not a build of Proton */
    W2kProton build;
    char      root[PATH_MAX];           /* Proton's compatdata; its Windows is root/pfx.
                                         * For Wine, the prefix itself. */
    char      umu_run[PATH_MAX];        /* umu-launcher, when programs run through it */
} Runner;

/* The runner called `name`, for `exe` (NULL: in the build's own Windows).
 * 0 when there is no such build. */
static int runner_for(Runner *r, const char *name, const char *exe)
{
    memset(r, 0, sizeof *r);
    if (!strcmp(name, "wine")) {
        r->wine = 1;
        w2k_wine_prefix(r->root, sizeof r->root);
        return 1;
    }
    if (!w2k_proton_find(name, &r->build)) return 0;
    char pfx[128];
    if (!exe || !w2k_proton_prefix_of(exe, pfx, sizeof pfx)) snprintf(pfx, sizeof pfx, "%s", name);
    snprintf(r->root, sizeof r->root, "%.3900s/prefixes/%s", data_dir, pfx);
    W2kCompatOptions o;
    w2k_compat_options(&o);
    char umu[PATH_MAX];
    snprintf(umu, sizeof umu, "%.4000s/umu/umu/umu-run", data_dir);
    if (o.umu && access(umu, X_OK) == 0) snprintf(r->umu_run, sizeof r->umu_run, "%s", umu);
    return 1;
}

/* The Steam Runtime umu-launcher runs programs in: downloaded the first
 * time it is wanted, a good few hundred megabytes. */
static int umu_runtime_present(void)
{
    char dir[PATH_MAX];
    const char *x = getenv("XDG_DATA_HOME");
    if (x && x[0] == '/') snprintf(dir, sizeof dir, "%.4000s/umu", x);
    else home_path(dir, sizeof dir, ".local/share/umu");
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while (!found && (de = readdir(d))) found = !strncmp(de->d_name, "steamrt", 7);
    closedir(d);
    return found;
}

static void first_line(const char *path, char *out, int n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (!fgets(out, n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

/* Its Windows has been made, and by this version of Proton: otherwise
 * Proton has work to do before the program starts. */
static int prefix_ready(const Runner *r)
{
    if (r->wine) return 1;
    if (r->umu_run[0] && !umu_runtime_present()) return 0;
    char path[PATH_MAX + 64], have[256], want[256];
    snprintf(path, sizeof path, "%s/pfx/system.reg", r->root);
    if (access(path, F_OK) != 0) return 0;
    /* The build's file says "<build time> <name>"; the Windows it made
     * says the name -- in the root, or through umu one folder down. */
    snprintf(path, sizeof path, "%s/%sversion", r->root, r->umu_run[0] ? "pfx/" : "");
    first_line(path, have, sizeof have);
    snprintf(path, sizeof path, "%s/version", r->build.dir);
    first_line(path, want, sizeof want);
    const char *name = strchr(want, ' ');
    name = name ? name + 1 : want;
    return !name[0] || !strcmp(have, name);
}

static void runner_env(const Runner *r, const W2kCompat *c, const W2kCompatOptions *o)
{
    if (r->wine) {
        setenv("WINEPREFIX", r->root, 1);
        return;
    }
    char steam[PATH_MAX + 16], pfx[PATH_MAX + 16];
    snprintf(steam, sizeof steam, "%.4000s/steam", data_dir);
    snprintf(pfx, sizeof pfx, "%s/pfx", r->root);
    mkdirs(steam);
    if (r->umu_run[0]) {
        mkdirs(pfx);
        setenv("WINEPREFIX", pfx, 1);
        setenv("PROTONPATH", r->build.dir, 1);
        if (!getenv("GAMEID")) setenv("GAMEID", "umu-default", 1);
        unsetenv("STEAM_COMPAT_DATA_PATH");
    } else {
        /* Proton outside Steam wants the client's folder named, if only
         * to find nothing in it. */
        mkdirs(r->root);
        unsetenv("WINEPREFIX");
        setenv("STEAM_COMPAT_DATA_PATH", r->root, 1);
        setenv("STEAM_COMPAT_CLIENT_INSTALL_PATH", steam, 1);
    }
    if (!o) return;
    if (!o->dxvk || (c && c->wined3d)) setenv("PROTON_USE_WINED3D", "1", 1);
    if (!o->esync || (c && c->nosync)) setenv("PROTON_NO_ESYNC", "1", 1);
    if (!o->fsync || (c && c->nosync)) setenv("PROTON_NO_FSYNC", "1", 1);
    if (o->nvapi) setenv("PROTON_ENABLE_NVAPI", "1", 1);
    if (o->hud || (c && c->hud)) setenv("DXVK_HUD", "fps", 1);
    /* Wine's new WoW64 keeps Linux's own libraries, their heaps and the
     * sound server's buffers out of a 32-bit program's 4 GB, and lets the
     * graphics driver map into all of it. A big 32-bit game otherwise runs
     * out of addresses, and DXVK fails where it next asks for a buffer. */
    if (c && c->wow64) setenv("PROTON_USE_WOW64", "1", 1);
}

/* The command for `args` in the runner's Windows, with Proton's `verb`:
 * run for a program, runinprefix for something quick like reg, and
 * getcompatpath -- which answers a question -- to have the Windows set
 * up and nothing else. Wine takes no verb. Freed by the caller. */
static char **runner_argv(const Runner *r, const char *verb, char *const args[], int nargs)
{
    char **v = w2k_alloc((size_t)(nargs + 4) * sizeof *v);
    int n = 0;
    if (r->wine) {
        v[n++] = "wine";
    } else if (r->umu_run[0]) {
        /* umu's own way of starting a program is the one it waits on. */
        setenv("PROTON_VERB", strcmp(verb, "run") ? verb : "waitforexitandrun", 1);
        v[n++] = (char *)r->umu_run;
    } else {
        static char script[PATH_MAX + 16];
        snprintf(script, sizeof script, "%s/proton", r->build.dir);
        v[n++] = script;
        v[n++] = (char *)verb;
    }
    for (int i = 0; i < nargs; i++) v[n++] = args[i];
    v[n] = NULL;
    return v;
}

/* ------------------------------------------------------------------ *
 * The version of Windows a program is told it runs on
 *
 * Wine's AppDefaults key for the program's file name, in the Windows it
 * runs in. What was last set there is noted beside that Windows, so the
 * registry -- a second or two -- is only touched when it changes.
 * ------------------------------------------------------------------ */
static const char *winver_value(const char *exe, const char *winver)
{
    if (!winver[0]) return "";
    int is64 = w2k_exe_is_64bit(exe);
    for (int i = 0; i < w2k_n_win_versions; i++)
        if (!strcmp(w2k_win_versions[i].id, winver)) {
            const char *v = is64 ? w2k_win_versions[i].id64 : w2k_win_versions[i].id;
            return v ? v : "";
        }
    return "";
}

static void marker_get(const char *marker, const char *base, char *out, int n)
{
    out[0] = 0;
    FILE *f = fopen(marker, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (!strcasecmp(line, base)) snprintf(out, (size_t)n, "%s", tab + 1);
    }
    fclose(f);
}

static void marker_set(const char *marker, const char *base, const char *value)
{
    char tmp[PATH_MAX + 64];
    snprintf(tmp, sizeof tmp, "%s.%ld", marker, (long)getpid());
    FILE *in = fopen(marker, "r"), *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    char line[600];
    while (in && fgets(line, sizeof line, in)) {
        size_t cut = strcspn(line, "\t");
        if (line[cut] != '\t' || cut != strlen(base) || strncasecmp(line, base, cut)) fputs(line, out);
    }
    if (in) fclose(in);
    if (value[0]) fprintf(out, "%s\t%s\n", base, value);
    if (fclose(out) != 0 || rename(tmp, marker) != 0) unlink(tmp);
}

static void apply_winver(const Runner *r, const char *exe, const char *winver)
{
    const char *dot = strrchr(exe, '.'), *slash = strrchr(exe, '/');
    if (!dot || (strcasecmp(dot, ".exe") && strcasecmp(dot, ".com"))) return;
    const char *base = slash ? slash + 1 : exe;
    if (strlen(base) > 200 || strpbrk(base, "\\\t\n")) return;
    const char *want = winver_value(exe, winver);
    char marker[PATH_MAX + 32], have[32];
    snprintf(marker, sizeof marker, "%s/l2k-appdefaults", r->root);
    marker_get(marker, base, have, sizeof have);
    if (!strcmp(have, want)) return;

    char key[300];
    snprintf(key, sizeof key, "HKCU\\Software\\Wine\\AppDefaults\\%s", base);
    char *args[8];
    int n = 0;
    args[n++] = "reg";
    args[n++] = want[0] ? "add" : "delete";
    args[n++] = key;
    args[n++] = "/v";
    args[n++] = "Version";
    if (want[0]) { args[n++] = "/d"; args[n++] = (char *)want; }
    args[n++] = "/f";
    char **v = runner_argv(r, "runinprefix", args, n);
    int rc = run_wait(v);
    free(v);
    unsetenv("PROTON_VERB");
    /* Deleting what was never there fails, and is done all the same. */
    if (rc == 0 || !want[0]) marker_set(marker, base, want);
}

/* ------------------------------------------------------------------ *
 * Setting a Windows up, with something to look at meanwhile
 * ------------------------------------------------------------------ */
typedef struct {
    W2kWin *w;
    pid_t   pid;
    int     rc, finished, phase, down;
    char    text[400];
    W2kRect cancel;
} Setup;

static void setup_paint(W2kWin *w, Drawable d)
{
    Setup *s = w->user;
    w2k_fill(d, 0, 0, w->w, w->h, C_FACE);
    w2k_bigicon_draw(d, 14, 14, ICO_PROTON);
    int tx = 58, tw = w->w - tx - 14;
    int th = w2k_text_wrapped(d, F_UI, tx, 16, tw, s->text, C_TEXT);
    W2kRect bar = { tx, 16 + th + 10, tw, 16 };
    w2k_draw_progress(d, &bar, -1, s->phase);
    s->cancel = (W2kRect){ w->w - 14 - 75, w->h - 12 - 23, 75, 23 };
    w2k_draw_pushbutton(d, &s->cancel, "Cancel", s->down ? BS_PRESSED : 0);
}

static int setup_event(W2kWin *w, XEvent *e)
{
    Setup *s = w->user;
    if (e->type == ButtonPress && w2k_rect_hit(&s->cancel, e->xbutton.x, e->xbutton.y)) {
        s->down = 1;
        w2k_win_dirty(w);
        return 1;
    }
    if (e->type == ButtonRelease && s->down) {
        s->down = 0;
        if (w2k_rect_hit(&s->cancel, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    if (e->type == KeyPress && XLookupKeysym(&e->xkey, 0) == XK_Escape) {
        w2k_win_close(w, ID_CANCEL);
        return 1;
    }
    return 0;
}

static void setup_tick(void *u)
{
    Setup *s = u;
    int st;
    if (waitpid(s->pid, &st, WNOHANG) == s->pid) {
        s->finished = 1;
        s->rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        w2k_win_close(s->w, ID_OK);
        return;
    }
    s->phase++;
    w2k_win_dirty(s->w);
}

/* argv run beside a box saying `text`, with a Cancel button: its exit
 * status, or -1 when it was cancelled. Its output goes to the log, but
 * for standard output when `quiet`. */
static int work_dialog(const char *text, char *const argv[], int quiet)
{
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        setpgid(0, 0);
        stdin_null();
        if (quiet) {
            int out = open("/dev/null", O_WRONLY);
            if (out >= 0) { dup2(out, 1); if (out > 2) close(out); }
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid);

    Setup s;
    memset(&s, 0, sizeof s);
    s.pid = pid;
    s.rc = -1;
    snprintf(s.text, sizeof s.text, "%s", text);
    if (!ui()) {
        int st;
        while (waitpid(pid, &st, 0) < 0)
            if (errno != EINTR) return 1;
        return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    s.w = w2k_win_new("Proton Manager", "l2kproton", 390, 132, 0);
    s.w->user = &s;
    s.w->paint = setup_paint;
    s.w->event = setup_event;
    w2k_win_center(s.w, NULL);
    w2k_add_timer(100, setup_tick, &s);
    w2k_win_modal(s.w);
    w2k_del_timer(setup_tick, &s);
    if (!s.finished) {
        kill(-pid, SIGTERM);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }
    return s.rc;
}

/* 0 when the Windows is ready; -1 when it was cancelled. */
static int prepare(const Runner *r, const char *name)
{
    char *args[] = { "/" };
    char **v = runner_argv(r, "getcompatpath", args, 1);
    char text[400];
    if (r->umu_run[0] && !umu_runtime_present())
        snprintf(text, sizeof text, "Downloading the Steam Runtime, and setting up Windows for %s. "
                 "This is done once, and can take several minutes.", name);
    else
        snprintf(text, sizeof text, "Setting up Windows for %s. This is done once for each "
                 "version of Proton, and takes a minute or so.", name);
    int rc = work_dialog(text, v, 1);       /* its answer, a path, is not wanted */
    unsetenv("PROTON_VERB");
    free(v);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Windows Media Player 9 for a program
 *
 * Proton decodes Windows Media sound and video with its own winedmo,
 * which some games cannot live with: Halo 2, whose every sound is WMA,
 * spins in it for ever at its main menu. Such a program can be given
 * Microsoft's decoders instead -- Windows Media Player 9, put into the
 * build's Windows by winetricks, as Lutris's Halo 2 installer does --
 * with Proton's own switched off for that program alone, along with what
 * is built on them and would call into nothing.
 * ------------------------------------------------------------------ */
static const char *const wmp_disabled[] = { "winedmo", "mfasfsrcsnk", "mfsrcsnk", "mfmp4srcsnk", "iyuv_32" };
static const char *const wmp_native[] = { "wmadmod", "wmvcore" };

/* winetricks -- the latest release, fetched the first time it is wanted --
 * and its wmp9. Everything it is given is an argument. */
static const char wmp9_sh[] =
    "set -u\n"
    "data=$1 bin=$2 pfx=$3\n"
    "wt=$data/winetricks/winetricks\n"
    "if [ ! -x \"$wt\" ]; then\n"
    "  mkdir -p \"$data/winetricks\" || exit 1\n"
    "  tag=$(curl -fsSL --max-time 30 https://api.github.com/repos/Winetricks/winetricks/releases/latest |"
    " sed -n 's/.*\"tag_name\": *\"\\([0-9A-Za-z._-]*\\)\".*/\\1/p' | head -n 1)\n"
    "  curl -fsSL --retry 3 -o \"$wt.part\" \"https://raw.githubusercontent.com/Winetricks/winetricks/${tag:-master}/src/winetricks\" &&"
    " chmod +x \"$wt.part\" && mv -f \"$wt.part\" \"$wt\" || { rm -f \"$wt.part\"; echo 'winetricks could not be downloaded'; exit 1; }\n"
    "fi\n"
    /* Proton's own WMA decoder is a link to a read-only file with a newer
     * version than Microsoft's, and the installer only replaces older ones. */
    "sys=$pfx/drive_c/windows/syswow64\n"
    "if [ -L \"$sys/wmadmod.dll\" ] || head -c 128 \"$sys/wmadmod.dll\" 2>/dev/null | grep -q -a 'Wine builtin DLL'; then rm -f \"$sys/wmadmod.dll\"; fi\n"
    "export WINEPREFIX=\"$pfx\" WINE=\"$bin/wine\" WINESERVER=\"$bin/wineserver\" WINEDEBUG=-all\n"
    "export WINETRICKS_CACHE=\"$data/winetricks/cache\" WINETRICKS_LATEST_VERSION_CHECK=disabled\n"
    "exec \"$wt\" -q --force wmp9\n";

/* Is Microsoft's WMA decoder in the build's Windows -- not Proton's own,
 * which says it is a builtin in its header? */
static int wmp9_present(const Runner *r)
{
    char path[PATH_MAX + 64], head[128];
    snprintf(path, sizeof path, "%s/pfx/drive_c/windows/syswow64/wmadmod.dll", r->root);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    static const char mark[] = "Wine builtin DLL";
    for (size_t i = 0; i + sizeof mark - 1 <= n; i++)
        if (!memcmp(head + i, mark, sizeof mark - 1)) return 0;
    return n >= 64 && head[0] == 'M' && head[1] == 'Z';
}

/* 0 to stop: the program is not to be started. */
static int wmp9_setup(const Runner *r, const char *exe, int on, const char *name)
{
    const char *slash = strrchr(exe, '/');
    const char *base = slash ? slash + 1 : exe;
    if (strlen(base) > 200 || strpbrk(base, "\\\t\n[]\"")) return 1;
    char marker[PATH_MAX + 32], have[8];
    snprintf(marker, sizeof marker, "%s/l2k-wmp9", r->root);
    marker_get(marker, base, have, sizeof have);
    int was = !strcmp(have, "1");

    if (on && !wmp9_present(r)) {
        char bin[PATH_MAX + 16], pfx[PATH_MAX + 16], text[400];
        snprintf(bin, sizeof bin, "%s/files/bin", r->build.dir);
        snprintf(pfx, sizeof pfx, "%s/pfx", r->root);
        snprintf(text, sizeof text, "Installing Windows Media Player 9 in the Windows %s runs "
                 "programs in, for %s's sound and video. This is done once.", name, base);
        char *argv[] = { "sh", "-c", (char *)wmp9_sh, "sh", data_dir, bin, pfx, NULL };
        int rc = work_dialog(text, argv, 0);
        if (rc < 0) return 0;                               /* cancelled */
        if (rc != 0 || !wmp9_present(r)) {
            char lp[PATH_MAX], shown[PATH_MAX];
            log_path(lp, sizeof lp);
            place(lp, shown, sizeof shown);
            return say(MB_YESNO | MB_ICONWARNING, "Windows Media Player 9 could not be installed, so "
                       "%s may have no sound or video.\n\nWhat went wrong is written in %s.\n\n"
                       "Start it anyway?", base, shown) == ID_YES;
        }
    }
    if (on == was) return 1;

    /* The program's own DLL overrides, in one go: a .reg file imported. */
    char reg[PATH_MAX + 32], winpath[PATH_MAX + 40];
    snprintf(reg, sizeof reg, "%s/l2k-wmp9.reg", r->root);
    FILE *f = fopen(reg, "w");
    if (!f) return 1;
    fprintf(f, "REGEDIT4\n\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\%s\\DllOverrides]\n", base);
    for (size_t i = 0; i < sizeof wmp_disabled / sizeof *wmp_disabled; i++)
        fprintf(f, on ? "\"%s\"=\"\"\n" : "\"%s\"=-\n", wmp_disabled[i]);
    for (size_t i = 0; i < sizeof wmp_native / sizeof *wmp_native; i++)
        fprintf(f, on ? "\"%s\"=\"native\"\n" : "\"%s\"=-\n", wmp_native[i]);
    if (fclose(f) != 0) return 1;
    snprintf(winpath, sizeof winpath, "Z:%s", reg);
    for (char *s = winpath; *s; s++)
        if (*s == '/') *s = '\\';
    char *args[] = { "reg", "import", winpath };
    char **v = runner_argv(r, "runinprefix", args, 3);
    int rc = run_wait(v);
    free(v);
    unsetenv("PROTON_VERB");
    unlink(reg);
    if (rc == 0) marker_set(marker, base, on ? "1" : "");
    return 1;
}

static int ready(const Runner *r, const char *name)
{
    if (prefix_ready(r)) return 1;
    int rc = prepare(r, name);
    if (rc == 0) return 1;
    if (rc > 0) {
        char lp[PATH_MAX], shown[PATH_MAX];
        log_path(lp, sizeof lp);
        place(lp, shown, sizeof shown);
        say(MB_OK | MB_ICONERROR, "%s could not set up Windows to run programs.\n\n"
            "What went wrong is written in %s.", name, shown);
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * l2kproton run, winecfg, setup
 * ------------------------------------------------------------------ */
static int no_runner(void)
{
    if (say(MB_YESNO | MB_ICONQUESTION, "Windows programs cannot run: neither Wine nor Proton is "
            "installed.\n\nProton Manager can download Proton. Open it now?") == ID_YES) {
        char *argv[] = { self_exe, "download", NULL };
        spawn(argv);
    }
    return 1;
}

static int cmd_run(const char *file, char **extra, int nextra)
{
    char exe[PATH_MAX];
    if (!realpath(file, exe)) {
        say(MB_OK | MB_ICONERROR, "Windows cannot find '%s'. Make sure you typed the name "
            "correctly, and then try again.", file);
        return 1;
    }
    const char *base = strrchr(exe, '/') + 1;
    W2kCompat c;
    W2kCompatOptions o;
    w2k_compat_get(exe, &c);
    w2k_compat_options(&o);

    char name[128];
    w2k_compat_default_runner(exe, name, sizeof name);
    if (c.runner[0] && (!strcmp(c.runner, "wine") || w2k_proton_find(c.runner, NULL))) {
        snprintf(name, sizeof name, "%s", c.runner);
    } else if (c.runner[0]) {
        if (say(MB_YESNO | MB_ICONWARNING, "%s is set to run with %s, which is no longer "
                "installed.\n\nRun it with %s instead?", base, c.runner,
                strcmp(name, "wine") ? name : "Wine") != ID_YES)
            return 1;
    }

    Runner r;
    if (!runner_for(&r, name, exe)) return no_runner();
    if (r.wine && !w2k_wine_available()) return no_runner();
    runner_env(&r, &c, &o);

    char what[PATH_MAX + 200];
    snprintf(what, sizeof what, "%s with %s", exe, r.wine ? "Wine" : name);
    to_log(what);
    if (!ready(&r, name)) return 1;
    if (!r.wine && !wmp9_setup(&r, exe, c.wmp, name)) return 1;
    apply_winver(&r, exe, c.winver);

    /* Started in its own folder, as Explorer starts a program. Proton
     * reads $PWD. */
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s", (int)(base - 1 - exe) > 0 ? (int)(base - 1 - exe) : 1, exe);
    if (chdir(dir) == 0) setenv("PWD", dir, 1);

    /* A program runs itself. An installer, shortcut or batch file goes to
     * start, which hands it to what Windows opens it with. */
    const char *dot = strrchr(base, '.');
    int program = dot && (!strcasecmp(dot, ".exe") || !strcasecmp(dot, ".com"));
    char **args = w2k_alloc((size_t)(nextra + 4) * sizeof *args);
    int n = 0;
    if (!program || r.wine) {
        args[n++] = r.wine ? "start" : "start.exe";
        args[n++] = "/unix";
    }
    args[n++] = exe;
    for (int i = 0; i < nextra; i++) args[n++] = extra[i];
    char **v = runner_argv(&r, "run", args, n);
    execvp(v[0], v);
    say(MB_OK | MB_ICONERROR, "%s could not be started: %s.", r.wine ? "Wine" : name, strerror(errno));
    return 1;
}

static int cmd_winecfg(const char *name, const char *prefix)
{
    Runner r;
    if (!runner_for(&r, name, NULL)) {
        say(MB_OK | MB_ICONERROR, "%s is not installed.", name);
        return 1;
    }
    if (r.wine && !w2k_wine_available()) return no_runner();
    /* Another build's prefix -- one whose own build has been removed. */
    if (!r.wine && prefix && prefix[0]) {
        if (strchr(prefix, '/') || prefix[0] == '.') return 2;
        snprintf(r.root, sizeof r.root, "%.3800s/prefixes/%.200s", data_dir, prefix);
    }
    W2kCompatOptions o;
    w2k_compat_options(&o);
    runner_env(&r, NULL, &o);
    to_log(r.wine ? "Wine configuration" : name);
    if (!ready(&r, name)) return 1;
    char *args[] = { "winecfg" };
    char **v = runner_argv(&r, "run", args, 1);
    execvp(v[0], v);
    say(MB_OK | MB_ICONERROR, "Wine configuration could not be started: %s.", strerror(errno));
    return 1;
}

/* The last step of installing a build: its Windows made. Quietly, for
 * Proton Manager's download to report on. */
static int cmd_setup(const char *name)
{
    Runner r;
    if (!name || !runner_for(&r, name, NULL) || r.wine) return 2;
    W2kCompatOptions o;
    w2k_compat_options(&o);
    runner_env(&r, NULL, &o);
    char *args[] = { "/" };
    char **v = runner_argv(&r, "getcompatpath", args, 1);
    return run_wait(v) == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ *
 * GitHub's list of releases: just enough JSON
 * ------------------------------------------------------------------ */
typedef struct { const char *p, *e; int depth; } Json;

static void j_ws(Json *j)
{
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int j_lit(Json *j, char c)
{
    j_ws(j);
    if (j->p < j->e && *j->p == c) { j->p++; return 1; }
    return 0;
}

/* A string into `out` -- \u escapes as UTF-8 -- or passed over when
 * `out` is NULL. */
static int j_str(Json *j, char *out, int n)
{
    j_ws(j);
    if (j->p >= j->e || *j->p != '"') return 0;
    j->p++;
    int k = 0;
    while (j->p < j->e && *j->p != '"') {
        unsigned c = (unsigned char)*j->p++;
        if (c == '\\') {
            if (j->p >= j->e) return 0;
            c = (unsigned char)*j->p++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'b' || c == 'f') c = ' ';
            else if (c == 'u') {
                if (j->e - j->p < 4) return 0;
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    char h = *j->p++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else return 0;
                }
                if (v >= 0xd800 && v < 0xe000) v = '?';    /* nothing read here is outside the BMP */
                unsigned char u[3];
                int ul;
                if (v < 0x80) { u[0] = (unsigned char)v; ul = 1; }
                else if (v < 0x800) { u[0] = (unsigned char)(0xc0 | v >> 6); u[1] = (unsigned char)(0x80 | (v & 0x3f)); ul = 2; }
                else { u[0] = (unsigned char)(0xe0 | v >> 12); u[1] = (unsigned char)(0x80 | ((v >> 6) & 0x3f));
                       u[2] = (unsigned char)(0x80 | (v & 0x3f)); ul = 3; }
                for (int i = 0; i < ul; i++)
                    if (out && k < n - 1) out[k++] = (char)u[i];
                continue;
            }
        }
        if (out && k < n - 1) out[k++] = (char)c;
    }
    if (j->p >= j->e) return 0;
    j->p++;
    if (out && n > 0) out[k] = 0;
    return 1;
}

static int j_skip(Json *j)
{
    j_ws(j);
    if (j->p >= j->e) return 0;
    char c = *j->p;
    if (c == '"') return j_str(j, NULL, 0);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        if (++j->depth > 64) return 0;
        j->p++;
        if (!j_lit(j, close))
            for (;;) {
                if (c == '{' && (!j_str(j, NULL, 0) || !j_lit(j, ':'))) return 0;
                if (!j_skip(j)) return 0;
                if (j_lit(j, ',')) continue;
                if (j_lit(j, close)) break;
                return 0;
            }
        j->depth--;
        return 1;
    }
    const char *s = j->p;                     /* a number, true, false or null */
    while (j->p < j->e && !strchr(",}] \t\r\n", *j->p)) j->p++;
    return j->p > s;
}

/* A value as text: a string's contents, or a number or word as it is. */
static int j_text(Json *j, char *out, int n)
{
    j_ws(j);
    if (j->p < j->e && *j->p == '"') return j_str(j, out, n);
    const char *s = j->p;
    if (!j_skip(j)) return 0;
    snprintf(out, (size_t)n, "%.*s", (int)(j->p - s), s);
    return 1;
}

typedef struct {
    char      tag[64];
    char      date[16];                 /* 8/30/2026 */
    char      url[400], sum[400];       /* the tarball, and its checksum */
    long long size;
} Release;

typedef struct { char name[128], url[400]; long long size; } Asset;

static int tag_ok(const char *t)
{
    if (strncmp(t, "GE-Proton", 9) || strlen(t) >= 64) return 0;
    for (const char *s = t; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
              *s == '.' || *s == '-' || *s == '_'))
            return 0;
    return 1;
}

static int url_is(const char *url, const char *prefix, const char *name)
{
    size_t pl = strlen(prefix);
    return !strncmp(url, prefix, pl) && !strcmp(url + pl, name);
}

/* The release's tarball for this machine: GE-ProtonX-Y-x86_64.tar.gz and
 * its .sha512sum, or before there were ARM builds GE-ProtonX-Y.tar.gz. */
static void pick_assets(Release *r, const Asset *as, int n)
{
    struct utsname u;
    const char *arch = uname(&u) == 0 && !strcmp(u.machine, "aarch64") ? "aarch64" : "x86_64";
    char prefix[200];
    snprintf(prefix, sizeof prefix, GE_DL "%s/", r->tag);
    for (int pass = 0; pass < 2 && !r->url[0]; pass++) {
        char tar[128], sum[128];
        if (pass == 0) {
            snprintf(tar, sizeof tar, "%s-%s.tar.gz", r->tag, arch);
            snprintf(sum, sizeof sum, "%s-%s.sha512sum", r->tag, arch);
        } else {
            if (strcmp(arch, "x86_64")) break;
            snprintf(tar, sizeof tar, "%s.tar.gz", r->tag);
            snprintf(sum, sizeof sum, "%s.sha512sum", r->tag);
        }
        const Asset *t = NULL, *s = NULL;
        for (int i = 0; i < n; i++) {
            if (!strcmp(as[i].name, tar)) t = &as[i];
            if (!strcmp(as[i].name, sum)) s = &as[i];
        }
        /* Only from where GE-Proton is published, whatever the list says,
         * and by the name expected: the files are saved under it. */
        if (!t || !s || !url_is(t->url, prefix, tar) || !url_is(s->url, prefix, sum))
            continue;
        snprintf(r->url, sizeof r->url, "%s", t->url);
        snprintf(r->sum, sizeof r->sum, "%s", s->url);
        r->size = t->size;
    }
}

/* The releases GitHub lists, newest first as it lists them; -1 when the
 * text is not what was expected. */
static int parse_releases(const char *text, size_t len, Release *out, int max)
{
    Json j = { text, text + len, 0 };
    int n = 0;
    if (!j_lit(&j, '[')) return -1;
    if (j_lit(&j, ']')) return 0;
    static Asset as[16];
    for (;;) {
        Release r;
        memset(&r, 0, sizeof r);
        int nas = 0, skip = 0;
        if (!j_lit(&j, '{')) return -1;
        if (!j_lit(&j, '}'))
            for (;;) {
                char key[64], val[64];
                if (!j_str(&j, key, sizeof key) || !j_lit(&j, ':')) return -1;
                if (!strcmp(key, "tag_name")) {
                    if (!j_text(&j, r.tag, sizeof r.tag)) return -1;
                } else if (!strcmp(key, "published_at")) {
                    /* 2026-08-30T17:22:05Z, written as Explorer writes a date. */
                    if (!j_text(&j, val, sizeof val)) return -1;
                    int yy, mm, dd;
                    if (sscanf(val, "%4d-%2d-%2d", &yy, &mm, &dd) == 3)
                        snprintf(r.date, sizeof r.date, "%d/%d/%d", mm, dd, yy);
                } else if (!strcmp(key, "draft") || !strcmp(key, "prerelease")) {
                    if (!j_text(&j, val, sizeof val)) return -1;
                    if (!strcmp(val, "true")) skip = 1;
                } else if (!strcmp(key, "assets")) {
                    if (!j_lit(&j, '[')) return -1;
                    if (!j_lit(&j, ']'))
                        for (;;) {
                            Asset a;
                            memset(&a, 0, sizeof a);
                            if (!j_lit(&j, '{')) return -1;
                            if (!j_lit(&j, '}'))
                                for (;;) {
                                    char k2[64];
                                    if (!j_str(&j, k2, sizeof k2) || !j_lit(&j, ':')) return -1;
                                    if (!strcmp(k2, "name")) {
                                        if (!j_text(&j, a.name, sizeof a.name)) return -1;
                                    } else if (!strcmp(k2, "browser_download_url")) {
                                        if (!j_text(&j, a.url, sizeof a.url)) return -1;
                                    } else if (!strcmp(k2, "size")) {
                                        if (!j_text(&j, val, sizeof val)) return -1;
                                        a.size = atoll(val);
                                    } else if (!j_skip(&j)) {
                                        return -1;
                                    }
                                    if (j_lit(&j, ',')) continue;
                                    if (j_lit(&j, '}')) break;
                                    return -1;
                                }
                            if (nas < (int)(sizeof as / sizeof *as)) as[nas++] = a;
                            if (j_lit(&j, ',')) continue;
                            if (j_lit(&j, ']')) break;
                            return -1;
                        }
                } else if (!j_skip(&j)) {
                    return -1;
                }
                if (j_lit(&j, ',')) continue;
                if (j_lit(&j, '}')) break;
                return -1;
            }
        if (!skip && n < max && tag_ok(r.tag)) {
            pick_assets(&r, as, nas);
            if (r.url[0]) out[n++] = r;
        }
        if (j_lit(&j, ',')) continue;
        if (j_lit(&j, ']')) break;
        return -1;
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * Work beside the window: a download, the list of releases
 * ------------------------------------------------------------------ */
typedef struct {
    pid_t pid;
    int   fd;
    char  tail[4096];                   /* the last of what it said */
    int   len;
    char  part[600];                    /* a line not yet ended */
    int   plen;
    void (*line)(const char *);
    void (*done)(int rc, const char *tail);
} Job;
enum { J_LIST, J_WORK };
static Job jobs[2] = { { .pid = -1, .fd = -1 }, { .pid = -1, .fd = -1 } };

static W2kWin *main_win;

static void job_input(void *u)
{
    Job *j = u;
    for (;;) {
        char b[2048];
        ssize_t n = read(j->fd, b, sizeof b);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = b[i];
                if (c == '\n' || c == '\r') {
                    j->part[j->plen] = 0;
                    if (j->plen && j->line) j->line(j->part);
                    j->plen = 0;
                } else if (j->plen < (int)sizeof j->part - 1) {
                    j->part[j->plen++] = c;
                }
                if (j->len == (int)sizeof j->tail - 1) {
                    memmove(j->tail, j->tail + 1024, (size_t)(j->len - 1024));
                    j->len -= 1024;
                }
                j->tail[j->len++] = c;
            }
            j->tail[j->len] = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        break;
    }
    w2k_del_fd(j->fd);
    close(j->fd);
    j->fd = -1;
    if (j->plen && j->line) { j->part[j->plen] = 0; j->line(j->part); }
    j->plen = 0;
    int st = 0, rc = -1;
    if (waitpid(j->pid, &st, 0) == j->pid) rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    j->pid = -1;
    if (j->done) j->done(rc, j->tail);
    if (main_win) w2k_win_dirty(main_win);
}

static int job_start(int slot, char *const argv[], void (*line)(const char *),
                     void (*done)(int, const char *))
{
    Job *j = &jobs[slot];
    if (j->pid > 0) return 0;
    int fds[2];
    if (pipe(fds) < 0) return 0;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
    if (pid == 0) {
        setpgid(0, 0);                  /* so that Stop reaches curl and tar too */
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        stdin_null();
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    memset(j, 0, sizeof *j);
    j->pid = pid;
    j->fd = fds[0];
    j->line = line;
    j->done = done;
    fcntl(j->fd, F_SETFL, O_NONBLOCK);
    fcntl(j->fd, F_SETFD, FD_CLOEXEC);
    w2k_add_fd(j->fd, job_input, j);
    return 1;
}

/* Download, check, unpack and set up a build. Everything it is given
 * is an argument, never part of the script. */
static const char install_sh[] =
    "set -u\n"
    "tag=$1 url=$2 sum=$3 data=$4 self=$5\n"
    "file=${url##*/} sumfile=${sum##*/}\n"
    "mkdir -p \"$data/downloads\" \"$data/proton\" || { echo \"@error Could not make the folder $data.\"; exit 1; }\n"
    "cd \"$data/downloads\" || exit 1\n"
    "echo @stage download\n"
    "curl -fsSL --retry 3 --connect-timeout 30 -o \"$sumfile\" \"$sum\" ||"
    " { echo \"@error Could not download $sumfile. Check your Internet connection, and then try again.\"; exit 1; }\n"
    "if ! sha512sum -c \"$sumfile\" >/dev/null 2>&1; then\n"
    "  rm -f \"$file\"\n"
    "  curl -fsSL --retry 3 --connect-timeout 30 -o \"$file.part\" \"$url\" ||"
    " { rm -f \"$file.part\"; echo \"@error The download of $file failed. Check your Internet connection, and then try again.\"; exit 1; }\n"
    "  mv -f \"$file.part\" \"$file\"\n"
    "fi\n"
    "echo @stage verify\n"
    "sha512sum -c \"$sumfile\" >/dev/null 2>&1 ||"
    " { rm -f \"$file\"; echo \"@error $file was damaged on the way: its checksum is wrong. Try again.\"; exit 1; }\n"
    "echo @stage unpack\n"
    "tmp=$data/proton/.unpack-$tag old=$data/proton/.old-$tag\n"
    "rm -rf \"$tmp\" \"$old\" && mkdir -p \"$tmp\" || exit 1\n"
    "tar -xzf \"$file\" -C \"$tmp\" || { rm -rf \"$tmp\"; echo \"@error $file could not be unpacked. Is the disk full?\"; exit 1; }\n"
    "set -- \"$tmp\"/*\n"
    "if [ $# -ne 1 ] || [ ! -x \"$1/proton\" ]; then rm -rf \"$tmp\"; echo \"@error $file does not hold a version of Proton.\"; exit 1; fi\n"
    "if [ -e \"$data/proton/$tag\" ]; then mv \"$data/proton/$tag\" \"$old\" || exit 1; fi\n"
    "mv \"$1\" \"$data/proton/$tag\" || { rm -rf \"$tmp\"; echo \"@error Could not install $tag.\"; exit 1; }\n"
    "rm -rf \"$tmp\" \"$old\"; rm -f \"$file\" \"$sumfile\"\n"
    "echo @stage setup\n"
    "\"$self\" setup \"$tag\" || { echo \"@error $tag is installed, but could not set up Windows. See the log.\"; exit 1; }\n"
    "echo @done\n";

/* umu-launcher: the latest release's zipapp, unpacked where it is looked
 * for. The address is taken from GitHub's answer, and only from there. */
static const char umu_sh[] =
    "set -u\n"
    "data=$1 api=$2\n"
    "json=$(curl -fsSL --max-time 60 \"$api\") || { echo \"@error GitHub could not be reached. Check your Internet connection.\"; exit 1; }\n"
    "url=$(printf '%s\\n' \"$json\" | grep -o '\"browser_download_url\": *\"[^\"]*-zipapp\\.tar\"' | head -n 1 | sed 's/.*\"\\(https:[^\"]*\\)\"$/\\1/')\n"
    "case $url in https://github.com/Open-Wine-Components/umu-launcher/releases/download/*) ;;"
    " *) echo \"@error umu-launcher's download was not found.\"; exit 1;; esac\n"
    "mkdir -p \"$data/umu\" && cd \"$data/umu\" || exit 1\n"
    "curl -fsSL --retry 3 -o umu.tar.part \"$url\" || { rm -f umu.tar.part; echo \"@error The download of umu-launcher failed.\"; exit 1; }\n"
    "rm -rf umu.new && mkdir umu.new && tar -xf umu.tar.part -C umu.new && rm -f umu.tar.part ||"
    " { rm -rf umu.new umu.tar.part; echo \"@error umu-launcher could not be unpacked.\"; exit 1; }\n"
    "[ -x umu.new/umu/umu-run ] || { rm -rf umu.new; echo \"@error umu-launcher was not where it was expected.\"; exit 1; }\n"
    "rm -rf umu && mv umu.new/umu umu && rm -rf umu.new\n"
    "echo @done\n";

/* ------------------------------------------------------------------ *
 * Proton Manager
 * ------------------------------------------------------------------ */
enum { PG_VERSIONS, PG_DOWNLOAD, PG_PROGRAMS, PG_PREFIXES, PG_OPTIONS };

enum {
    B_NONE, B_OK, B_CANCEL, B_APPLY,
    B_DEFAULT, B_REMOVE, B_DRIVE, B_WINECFG,
    B_REFRESH, B_INSTALL, B_STOP,
    B_ADD, B_CHANGE, B_FORGET, B_RUN,
    B_PDRIVE, B_PFOLDER, B_PWINECFG, B_PCOPY,
    B_LOG,
    NB
};

static const struct { int page; const char *label; } btn[NB] = {
    [B_OK]      = { -1, "OK" },
    [B_CANCEL]  = { -1, "Cancel" },
    [B_APPLY]   = { -1, "&Apply" },
    [B_DEFAULT] = { PG_VERSIONS, "Set as &Default" },
    [B_REMOVE]  = { PG_VERSIONS, "&Remove" },
    [B_DRIVE]   = { PG_VERSIONS, "Open &C: Drive" },
    [B_WINECFG] = { PG_VERSIONS, "Wine &Settings..." },
    [B_REFRESH] = { PG_DOWNLOAD, "&Refresh" },
    [B_INSTALL] = { PG_DOWNLOAD, "&Install" },
    [B_STOP]    = { PG_DOWNLOAD, "&Stop" },
    [B_ADD]     = { PG_PROGRAMS, "A&dd..." },
    [B_CHANGE]  = { PG_PROGRAMS, "&Change..." },
    [B_FORGET]  = { PG_PROGRAMS, "&Remove" },
    [B_RUN]     = { PG_PROGRAMS, "R&un" },
    [B_PDRIVE]  = { PG_PREFIXES, "Open &C: Drive" },
    [B_PFOLDER] = { PG_PREFIXES, "Open &Folder" },
    [B_PWINECFG]= { PG_PREFIXES, "Wine &Settings..." },
    [B_PCOPY]   = { PG_PREFIXES, "Copy &Path" },
    [B_LOG]     = { PG_OPTIONS,  "View &Log" },
};

enum { WORK_NONE, WORK_INSTALL, WORK_UMU };
enum { ST_DOWNLOAD, ST_VERIFY, ST_UNPACK, ST_SETUP };
enum { OPT_DXVK, OPT_ESYNC, OPT_FSYNC, OPT_NVAPI, OPT_HUD, OPT_UMU, NOPT };

#define MAX_REL  64
#define MAX_PROG 256
#define MAX_PFX  64

/* A prefix: the Windows programs run in, with its own C: drive. */
typedef struct {
    char name[128];                     /* its folder: the build that made it, or "wine" */
    char path[PATH_MAX];                /* WINEPREFIX */
    char last[128];                     /* the version of Proton that set it up last */
    char progs[400];                    /* the programs with settings that run in it */
    int  wine;
} PfxRow;

static struct {
    W2kWin   *win;
    W2kTabs  *tabs;
    W2kRect   b[NB];
    int       down;

    W2kList  *vlist;                    /* Versions */
    W2kProton pv[64];
    int       npv;

    W2kList  *rlist;                    /* Download */
    Release   rel[MAX_REL];
    int       nrel, listed, listing;
    char      list_msg[300];

    int       work, stage, phase, stopped;
    int       work_first;               /* no build was installed when it began */
    char      work_tag[64];
    char      work_part[PATH_MAX + 64]; /* the download as it grows */
    long long work_size, have, last_bytes;
    long      last_ms;
    double    rate;
    char      work_msg[400];            /* how the last of it went */

    W2kList  *plist;                    /* Programs */
    char    (*prog)[4096];
    W2kCompat cs[MAX_PROG];
    int       nprog;

    W2kList  *xlist;                    /* Prefixes */
    PfxRow   *px;
    int       npx;
    char      px_msg[400];

    W2kCompatOptions o, saved;          /* Options */
    W2kCombo *def;
    W2kRect   opt[NOPT];
    char      umu_msg[300];
} pm;

static const char *vrow_name(int row)
{
    return row >= 1 && row <= pm.npv ? pm.pv[row - 1].name : "wine";
}

static void list_select(W2kList *l, int row)
{
    for (int i = 0; i < l->n; i++) l->items[i].selected = i == row;
    l->sel = row;
    /* Not before the page has been laid out: a list of no height scrolled
     * the selection to its top, and the rows were one off from then on. */
    if (row >= 0 && l->r.h > 0) w2k_list_ensure_visible(l, row);
}

static int options_changed(void)
{
    const W2kCompatOptions *a = &pm.o, *b = &pm.saved;
    return strcmp(a->def, b->def) || a->umu != b->umu || a->dxvk != b->dxvk ||
           a->esync != b->esync || a->fsync != b->fsync || a->nvapi != b->nvapi || a->hud != b->hud;
}

static void fill_default_combo(void)
{
    w2k_combo_clear(pm.def);
    w2k_combo_add(pm.def, w2k_wine_available() ? "Wine" : "Wine (not installed)");
    int sel = 0;
    for (int i = 0; i < pm.npv; i++) {
        w2k_combo_add(pm.def, pm.pv[i].name);
        if (!strcmp(pm.pv[i].name, pm.o.def)) sel = i + 1;
    }
    pm.def->sel = sel;
}

static void fill_versions(void)
{
    char keep[128];
    snprintf(keep, sizeof keep, "%s", pm.vlist->sel >= 0 ? vrow_name(pm.vlist->sel) : pm.o.def);
    pm.npv = w2k_proton_list(pm.pv, (int)(sizeof pm.pv / sizeof *pm.pv));
    /* A default that has gone -- removed by hand -- is no default. */
    int found = !strcmp(pm.o.def, "wine");
    for (int i = 0; i < pm.npv && !found; i++) found = !strcmp(pm.pv[i].name, pm.o.def);
    w2k_list_clear(pm.vlist);
    int sel = 0;
    for (int i = 0; i <= pm.npv; i++) {
        const char *name = vrow_name(i);
        int row = w2k_list_add(pm.vlist, i ? ICO_PROTON : ICO_APP, NULL);
        w2k_list_set(pm.vlist, row, 0, i ? name : "Wine");
        w2k_list_set(pm.vlist, row, 1, i ? (pm.pv[i - 1].steam ? "Steam" : "Proton Manager")
                                         : (w2k_wine_available() ? "This computer" : "Not installed"));
        w2k_list_set(pm.vlist, row, 2, !strcmp(name, pm.o.def) || (!found && !i) ? "Default" : "");
        if (!strcmp(name, keep)) sel = row;
    }
    list_select(pm.vlist, sel);
    fill_default_combo();
}

static int installed(const char *tag)
{
    for (int i = 0; i < pm.npv; i++)
        if (!strcmp(pm.pv[i].name, tag)) return 1;
    return 0;
}

static void fill_releases(void)
{
    char keep[64] = "";
    if (pm.rlist->sel >= 0 && pm.rlist->sel < pm.nrel) snprintf(keep, sizeof keep, "%s", pm.rel[pm.rlist->sel].tag);
    w2k_list_clear(pm.rlist);
    int sel = -1;
    for (int i = 0; i < pm.nrel; i++) {
        const Release *r = &pm.rel[i];
        char size[32];
        size_text(r->size, size, sizeof size);
        int row = w2k_list_add(pm.rlist, ICO_PROTON, NULL);
        w2k_list_set(pm.rlist, row, 0, r->tag);
        w2k_list_set(pm.rlist, row, 1, r->date);
        w2k_list_set(pm.rlist, row, 2, size);
        w2k_list_set(pm.rlist, row, 3, !strcmp(r->tag, pm.work_tag) ? "Installing..." :
                                        installed(r->tag) ? "Installed" : "");
        if (!strcmp(r->tag, keep)) sel = row;
    }
    /* The newest not yet installed is the likeliest choice. */
    for (int i = 0; sel < 0 && i < pm.nrel; i++)
        if (!installed(pm.rel[i].tag)) sel = i;
    list_select(pm.rlist, sel >= 0 ? sel : (pm.nrel ? 0 : -1));
}

static const char *winver_label(const char *id)
{
    for (int i = 0; i < w2k_n_win_versions; i++)
        if (!strcmp(w2k_win_versions[i].id, id)) return w2k_win_versions[i].label;
    return id;
}

static void fill_programs(void)
{
    char keep[4096] = "";
    if (pm.plist->sel >= 0 && pm.plist->sel < pm.nprog) snprintf(keep, sizeof keep, "%s", pm.prog[pm.plist->sel]);
    pm.nprog = w2k_compat_list(pm.prog, pm.cs, MAX_PROG);
    w2k_list_clear(pm.plist);
    int sel = -1;
    for (int i = 0; i < pm.nprog; i++) {
        const char *path = pm.prog[i], *slash = strrchr(path, '/');
        char name[300], folder[4096];
        snprintf(name, sizeof name, "%.250s%s", slash ? slash + 1 : path,
                 access(path, F_OK) == 0 ? "" : " (missing)");
        snprintf(folder, sizeof folder, "%.*s", slash && slash > path ? (int)(slash - path) : 1, path);
        const W2kCompat *c = &pm.cs[i];
        char runs[200];
        if (!c->runner[0])                snprintf(runs, sizeof runs, "Default");
        else if (!strcmp(c->runner, "wine")) snprintf(runs, sizeof runs, "Wine");
        else snprintf(runs, sizeof runs, "%s%s", c->runner, installed(c->runner) ? "" : " (not installed)");
        int row = w2k_list_add(pm.plist, ICO_APP, NULL);
        w2k_list_set(pm.plist, row, 0, name);
        w2k_list_set(pm.plist, row, 1, runs);
        w2k_list_set(pm.plist, row, 2, c->winver[0] ? winver_label(c->winver) : "");
        w2k_list_set(pm.plist, row, 3, folder);
        if (!strcmp(path, keep)) sel = row;
    }
    list_select(pm.plist, sel >= 0 ? sel : (pm.nprog ? 0 : -1));
}

static int by_prefix_name(const void *a, const void *b)
{
    return w2k_natural_cmp(((const PfxRow *)b)->name, ((const PfxRow *)a)->name);
}

/* Every prefix there is -- one for each build that has run a program,
 * kept when the build is removed -- and Wine's own, with the programs
 * that have settings of their own and run in each. */
static void fill_prefixes(void)
{
    char keep[128] = "";
    if (pm.xlist->sel >= 0 && pm.xlist->sel < pm.npx) snprintf(keep, sizeof keep, "%s", pm.px[pm.xlist->sel].name);
    pm.npx = 0;
    pm.px_msg[0] = 0;

    char dir[PATH_MAX + 16];
    snprintf(dir, sizeof dir, "%.4000s/prefixes", data_dir);
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d)) && pm.npx < MAX_PFX - 1) {
        if (de->d_name[0] == '.') continue;
        PfxRow *x = &pm.px[pm.npx];
        char root[PATH_MAX], probe[PATH_MAX + 32];
        snprintf(root, sizeof root, "%.3900s/%.150s", dir, de->d_name);
        snprintf(probe, sizeof probe, "%s/pfx", root);
        struct stat st;
        if (stat(probe, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%.127s", de->d_name);
        snprintf(x->path, sizeof x->path, "%.4000s", probe);
        /* Proton writes the version that set it up in the root; through
         * umu, one folder down. */
        snprintf(probe, sizeof probe, "%s/version", root);
        first_line(probe, x->last, sizeof x->last);
        if (!x->last[0]) {
            snprintf(probe, sizeof probe, "%s/pfx/version", root);
            first_line(probe, x->last, sizeof x->last);
        }
        pm.npx++;
    }
    if (d) closedir(d);
    qsort(pm.px, (size_t)pm.npx, sizeof *pm.px, by_prefix_name);

    char wp[PATH_MAX];
    w2k_wine_prefix(wp, sizeof wp);
    struct stat st;
    if (stat(wp, &st) == 0 && S_ISDIR(st.st_mode)) {
        PfxRow *x = &pm.px[pm.npx++];
        memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "wine");
        snprintf(x->path, sizeof x->path, "%s", wp);
        snprintf(x->last, sizeof x->last, "Wine");
        x->wine = 1;
    }

    /* Where each program runs: the prefix it was installed into, else its
     * version's, or the default's -- as "l2kproton run" decides. */
    char (*paths)[4096] = malloc(MAX_PROG * sizeof *paths);
    W2kCompat *cs = malloc(MAX_PROG * sizeof *cs);
    int n = paths && cs ? w2k_compat_list(paths, cs, MAX_PROG) : 0;
    for (int i = 0; i < n; i++) {
        char where[128];
        if (!w2k_proton_prefix_of(paths[i], where, sizeof where)) {
            if (cs[i].runner[0] && (!strcmp(cs[i].runner, "wine") || installed(cs[i].runner)))
                snprintf(where, sizeof where, "%s", cs[i].runner);
            else
                w2k_compat_default_runner(paths[i], where, sizeof where);
        }
        for (int k = 0; k < pm.npx; k++) {
            PfxRow *x = &pm.px[k];
            if (strcmp(x->name, where)) continue;
            const char *base = strrchr(paths[i], '/');
            size_t l = strlen(x->progs);
            snprintf(x->progs + l, sizeof x->progs - l, "%s%s", l ? ", " : "", base ? base + 1 : paths[i]);
            break;
        }
    }
    free(paths);
    free(cs);

    w2k_list_clear(pm.xlist);
    int sel = pm.npx ? 0 : -1;
    for (int k = 0; k < pm.npx; k++) {
        PfxRow *x = &pm.px[k];
        int row = w2k_list_add(pm.xlist, x->wine ? ICO_APP : ICO_FOLDER, NULL);
        w2k_list_set(pm.xlist, row, 0, x->wine ? "Wine" : x->name);
        w2k_list_set(pm.xlist, row, 1, x->last[0] ? x->last : "(not set up)");
        w2k_list_set(pm.xlist, row, 2, x->progs);
        if (!strcmp(x->name, keep)) sel = row;
    }
    list_select(pm.xlist, sel);
}

static void on_prefix_select(void *u, int row)
{
    pm.px_msg[0] = 0;                   /* "Copied": about the row that was */
}

static void cache_path(char *buf, size_t n)
{
    const char *x = getenv("XDG_CACHE_HOME");
    if (x && x[0] == '/') snprintf(buf, n, "%.4000s/l2k/proton-releases.json", x);
    else home_path(buf, n, ".cache/l2k/proton-releases.json");
}

static int load_releases(void)
{
    char path[PATH_MAX];
    cache_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t len = 0, cap = 1 << 16;
    char *text = malloc(cap);
    while (text) {
        if (len == cap) {
            if (cap >= (size_t)32 << 20) break;
            char *t = realloc(text, cap * 2);
            if (!t) break;
            text = t;
            cap *= 2;
        }
        size_t got = fread(text + len, 1, cap - len, f);
        if (!got) break;
        len += got;
    }
    fclose(f);
    int n = text ? parse_releases(text, len, pm.rel, MAX_REL) : -1;
    free(text);
    if (n < 0) return 0;
    pm.nrel = n;
    fill_releases();
    return 1;
}

static void listing_done(int rc, const char *tail)
{
    pm.listing = 0;
    char path[PATH_MAX], part[PATH_MAX + 16];
    cache_path(path, sizeof path);
    snprintf(part, sizeof part, "%s.part", path);
    if (rc == 0 && rename(part, path) == 0 && load_releases()) {
        pm.list_msg[0] = 0;
        return;
    }
    unlink(part);
    snprintf(pm.list_msg, sizeof pm.list_msg, rc == 127
             ? "Proton Manager needs curl to download Proton. Install curl, and then click Refresh."
             : "The list of versions could not be downloaded from GitHub. Check your Internet "
               "connection, and then click Refresh.");
}

static void start_listing(void)
{
    if (pm.listing) return;
    char path[PATH_MAX], dir[PATH_MAX], part[PATH_MAX + 16];
    cache_path(path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdirs(dir); }
    snprintf(part, sizeof part, "%s.part", path);
    char *argv[] = { "curl", "-fsSL", "--max-time", "60", "-H", "Accept: application/vnd.github+json",
                     "-o", part, GE_API, NULL };
    if (job_start(J_LIST, argv, NULL, listing_done)) {
        pm.listing = 1;
        pm.list_msg[0] = 0;
    }
}

/* The Download page's list: what was fetched last, fetched again when that
 * is more than an hour old. GitHub allows sixty questions an hour. */
static void enter_download(void)
{
    if (pm.listed) return;
    pm.listed = 1;
    char path[PATH_MAX];
    cache_path(path, sizeof path);
    struct stat st;
    int have = load_releases();
    if (!have || stat(path, &st) != 0 || time(NULL) - st.st_mtime > 3600) start_listing();
}

static void on_tab(void *u, int page)
{
    if (page == PG_DOWNLOAD) enter_download();
    if (page == PG_PROGRAMS) fill_programs();
    if (page == PG_PREFIXES) fill_prefixes();
}

static void work_tick(void *u)
{
    pm.phase++;
    if (pm.work == WORK_INSTALL && pm.stage == ST_DOWNLOAD) {
        struct stat st;
        pm.have = stat(pm.work_part, &st) == 0 ? (long long)st.st_size : pm.have;
        long now = w2k_now_ms();
        if (!pm.last_ms) {
            pm.last_ms = now;
            pm.last_bytes = pm.have;
        } else if (now - pm.last_ms >= 1000) {
            double r = (double)(pm.have - pm.last_bytes) * 1000.0 / (double)(now - pm.last_ms);
            pm.rate = pm.rate > 0 ? pm.rate * 0.6 + r * 0.4 : r;
            pm.last_ms = now;
            pm.last_bytes = pm.have;
        }
    }
    if (pm.tabs->sel == PG_DOWNLOAD || pm.tabs->sel == PG_OPTIONS) w2k_win_dirty(pm.win);
}

static void install_line(const char *s)
{
    if (!strncmp(s, "@stage ", 7)) {
        s += 7;
        pm.stage = !strcmp(s, "verify") ? ST_VERIFY : !strcmp(s, "unpack") ? ST_UNPACK :
                   !strcmp(s, "setup") ? ST_SETUP : ST_DOWNLOAD;
        if (pm.stage == ST_SETUP) { fill_versions(); fill_releases(); }
    } else if (!strncmp(s, "@error ", 7)) {
        snprintf(pm.work_msg, sizeof pm.work_msg, "%s", s + 7);
    }
}

/* What a stopped installation leaves: the partial download and its
 * checksum, and the folder it was being unpacked into. A whole download
 * is kept, to be checked and used next time. */
static void clean_partial(const char *tag, const char *part)
{
    char tmp[PATH_MAX + 128], sum[PATH_MAX + 64];
    snprintf(tmp, sizeof tmp, "%.3900s/proton/.unpack-%s", data_dir, tag);
    snprintf(sum, sizeof sum, "%s", part);
    char *ext = strstr(sum, ".tar.gz.part");
    if (ext) { strcpy(ext, ".sha512sum"); unlink(sum); }
    unlink(part);
    char *argv[] = { "rm", "-rf", "--", tmp, NULL };
    spawn(argv);
}

static void install_done(int rc, const char *tail)
{
    w2k_del_timer(work_tick, NULL);
    char tag[64];
    snprintf(tag, sizeof tag, "%s", pm.work_tag);
    int first = pm.work_first;
    pm.work = WORK_NONE;
    pm.work_tag[0] = 0;
    fill_versions();
    if (rc == 0) {
        /* The first version of Proton installed becomes the default:
         * it is what it was downloaded for. */
        if (first && installed(tag) && !strcmp(pm.saved.def, "wine")) {
            snprintf(pm.saved.def, sizeof pm.saved.def, "%s", tag);
            w2k_compat_save_options(&pm.saved);
            if (!strcmp(pm.o.def, "wine")) snprintf(pm.o.def, sizeof pm.o.def, "%s", tag);
            fill_versions();
            snprintf(pm.work_msg, sizeof pm.work_msg, "%s is installed. Windows programs now run with it.", tag);
        } else {
            snprintf(pm.work_msg, sizeof pm.work_msg, "%s is installed. To run programs with it, make it "
                     "the default on the Versions tab, or choose it on a program's Compatibility tab.", tag);
        }
    } else if (pm.stopped) {
        clean_partial(tag, pm.work_part);
        snprintf(pm.work_msg, sizeof pm.work_msg, "The installation of %s was stopped.", tag);
    } else if (!pm.work_msg[0]) {
        snprintf(pm.work_msg, sizeof pm.work_msg, "%s could not be installed.", tag);
    }
    pm.stopped = 0;
    fill_releases();
}

static void start_install(void)
{
    int i = pm.rlist->sel;
    if (pm.work || i < 0 || i >= pm.nrel) return;
    const Release *r = &pm.rel[i];
    const char *file = strrchr(r->url, '/');
    snprintf(pm.work_part, sizeof pm.work_part, "%.3900s/downloads/%s.part", data_dir, file ? file + 1 : r->url);
    char *argv[] = { "sh", "-c", (char *)install_sh, "sh", (char *)r->tag, (char *)r->url, (char *)r->sum,
                     data_dir, self_exe, NULL };
    if (!job_start(J_WORK, argv, install_line, install_done)) return;
    pm.work = WORK_INSTALL;
    pm.work_first = pm.npv == 0;
    pm.stage = ST_DOWNLOAD;
    pm.stopped = 0;
    snprintf(pm.work_tag, sizeof pm.work_tag, "%s", r->tag);
    pm.work_size = r->size;
    pm.have = pm.last_bytes = 0;
    pm.last_ms = 0;
    pm.rate = 0;
    pm.work_msg[0] = 0;
    w2k_add_timer(250, work_tick, NULL);
    fill_releases();
}

static void stop_work(void)
{
    if (!pm.work || jobs[J_WORK].pid <= 0) return;
    pm.stopped = 1;
    kill(-jobs[J_WORK].pid, SIGTERM);
}

static void umu_line(const char *s)
{
    if (!strncmp(s, "@error ", 7)) snprintf(pm.umu_msg, sizeof pm.umu_msg, "%s", s + 7);
}

static void umu_done(int rc, const char *tail)
{
    w2k_del_timer(work_tick, NULL);
    pm.work = WORK_NONE;
    if (rc == 0) {
        pm.umu_msg[0] = 0;
    } else {
        pm.o.umu = 0;
        if (!pm.umu_msg[0]) snprintf(pm.umu_msg, sizeof pm.umu_msg, "umu-launcher could not be downloaded.");
    }
}

static int umu_installed(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%.4000s/umu/umu/umu-run", data_dir);
    return access(path, X_OK) == 0;
}

static void get_umu(void)
{
    if (pm.work || umu_installed()) return;
    char *argv[] = { "sh", "-c", (char *)umu_sh, "sh", data_dir, UMU_API, NULL };
    if (!job_start(J_WORK, argv, umu_line, umu_done)) { pm.o.umu = 0; return; }
    pm.work = WORK_UMU;
    pm.umu_msg[0] = 0;
    w2k_add_timer(250, work_tick, NULL);
}

/* ---- The pages ------------------------------------------------------- */

static void button_row(int y, int x, const int *ids, const int *widths, int n)
{
    for (int i = 0; i < n; i++) {
        pm.b[ids[i]] = (W2kRect){ x, y, widths[i], 23 };
        x += widths[i] + 6;
    }
}

static int check_w(const char *text) { return 13 + 5 + w2k_mnemonic_width(F_UI, text) + 2; }

static void paint_versions(Drawable d, W2kRect c)
{
    int x = c.x + 10, w = c.w - 20, y = c.y + 10;
    y += w2k_text_wrapped(d, F_UI, x, y, w, "Windows programs run with the default version, unless the "
                          "Compatibility tab of a program's Properties chooses another.", C_TEXT) + 8;
    pm.vlist->r = (W2kRect){ x, y, w, 196 };
    w2k_list_layout(pm.vlist);
    w2k_list_draw(d, pm.vlist);
    y += pm.vlist->r.h + 8;
    static const int ids[] = { B_DEFAULT, B_REMOVE, B_DRIVE, B_WINECFG };
    static const int widths[] = { 96, 75, 96, 104 };
    button_row(y, x, ids, widths, 4);
    y += 23 + 12;

    int sel = pm.vlist->sel;
    char text[3 * PATH_MAX], a[PATH_MAX], b[PATH_MAX + 64];
    if (sel <= 0) {
        char prefix[PATH_MAX];
        w2k_wine_prefix(prefix, sizeof prefix);
        place(prefix, a, sizeof a);
        if (w2k_wine_available())
            snprintf(text, sizeof text, "The Wine installed on this computer runs programs in %s.", a);
        else
            snprintf(text, sizeof text, "Wine is not installed on this computer. Install it with your "
                     "distribution's software, or download Proton, which needs no Wine.");
    } else {
        const W2kProton *p = &pm.pv[sel - 1];
        char where[PATH_MAX + 64];
        snprintf(where, sizeof where, "%.3900s/prefixes/%s", data_dir, p->name);
        place(p->dir, a, sizeof a);
        place(where, b, sizeof b);
        snprintf(text, sizeof text, "%s is in %s. It runs programs in a Windows of its own, in %s.",
                 p->name, a, b);
    }
    w2k_text_wrapped(d, F_UI, x, y, w, text, C_GRAYTEXT);
}

static void paint_download(Drawable d, W2kRect c)
{
    int fh = w2k_font_height(F_UI);
    int x = c.x + 10, w = c.w - 20, y = c.y + 10;
    y += w2k_text_wrapped(d, F_UI, x, y, w, "GE-Proton is a version of Proton made to run games and "
                          "programs outside Steam. Choose a version, and click Install to download it "
                          "and set it up.", C_TEXT) + 8;
    pm.rlist->r = (W2kRect){ x, y, w, 170 };
    w2k_list_layout(pm.rlist);
    w2k_list_draw(d, pm.rlist);
    y += pm.rlist->r.h + 8;
    pm.b[B_REFRESH] = (W2kRect){ x, y, 75, 23 };
    pm.b[B_INSTALL] = (W2kRect){ x + w - 75, y, 75, 23 };
    y += 23 + 14;

    /* What is going on: a download, or how the last one went. */
    char text[600], a[32], b[32];
    int pct = -2;
    text[0] = 0;
    if (pm.work == WORK_INSTALL) {
        switch (pm.stage) {
        case ST_DOWNLOAD:
            size_text(pm.have, a, sizeof a);
            size_text(pm.work_size, b, sizeof b);
            if (pm.rate > 0) {
                char r[32];
                size_text((long long)pm.rate, r, sizeof r);
                snprintf(text, sizeof text, "Downloading %s: %s of %s (%s/sec)", pm.work_tag, a, b, r);
            } else {
                snprintf(text, sizeof text, "Downloading %s: %s of %s", pm.work_tag, a, b);
            }
            pct = pm.work_size > 0 ? (int)(pm.have * 100 / pm.work_size) : -1;
            if (pct > 100) pct = 100;
            break;
        case ST_VERIFY: snprintf(text, sizeof text, "Checking the download of %s...", pm.work_tag); pct = -1; break;
        case ST_UNPACK: snprintf(text, sizeof text, "Unpacking %s...", pm.work_tag); pct = -1; break;
        default:        snprintf(text, sizeof text, "Setting up Windows for %s...", pm.work_tag); pct = -1; break;
        }
    } else if (pm.listing) {
        snprintf(text, sizeof text, "Getting the list of versions from GitHub...");
        pct = -1;
    } else if (pm.work_msg[0]) {
        snprintf(text, sizeof text, "%s", pm.work_msg);
    } else if (pm.list_msg[0]) {
        snprintf(text, sizeof text, "%s", pm.list_msg);
    }
    int th = w2k_text_wrapped(d, F_UI, x, y, w, text, C_TEXT);
    y += (th > fh ? th : fh) + 6;
    pm.b[B_STOP] = (W2kRect){ x + w - 75, y - 3, 75, 23 };
    if (pct >= -1) {
        W2kRect bar = { x, y, w - 75 - 10, 17 };
        w2k_draw_progress(d, &bar, pct, pm.phase);
    }
    w2k_text(d, F_UI, x, c.y + c.h - 8 - fh, "From " GE_SITE, C_GRAYTEXT);
}

static void paint_programs(Drawable d, W2kRect c)
{
    int x = c.x + 10, w = c.w - 20, y = c.y + 10;
    y += w2k_text_wrapped(d, F_UI, x, y, w, "These programs have settings of their own, made on the "
                          "Compatibility tab of their Properties.", C_TEXT) + 8;
    pm.plist->r = (W2kRect){ x, y, w, 226 };
    w2k_list_layout(pm.plist);
    w2k_list_draw(d, pm.plist);
    if (!pm.nprog) {
        static const char *const none = "No program has settings of its own.";
        int tw = w2k_text_width(F_UI, none, -1);
        w2k_text(d, F_UI, x + (w - tw) / 2, y + 40, none, C_GRAYTEXT);
    }
    y += pm.plist->r.h + 8;
    static const int ids[] = { B_ADD, B_CHANGE, B_FORGET, B_RUN };
    static const int widths[] = { 75, 75, 75, 75 };
    button_row(y, x, ids, widths, 4);
    y += 23 + 12;
    w2k_text_wrapped(d, F_UI, x, y, w, "Add a program to give it settings: which version of Proton "
                     "runs it, and which version of Windows it is told it runs on.", C_GRAYTEXT);
}

static void paint_prefixes(Drawable d, W2kRect c)
{
    int x = c.x + 10, w = c.w - 20, y = c.y + 10;
    y += w2k_text_wrapped(d, F_UI, x, y, w, "Each version of Proton runs programs in a Windows of its "
                          "own, a prefix, with its own C: drive: the programs installed there are in "
                          "it, with their saved games and settings.", C_TEXT) + 8;
    pm.xlist->r = (W2kRect){ x, y, w, 180 };
    w2k_list_layout(pm.xlist);
    w2k_list_draw(d, pm.xlist);
    if (!pm.npx) {
        static const char *const none = "No program has run in a prefix yet.";
        int tw = w2k_text_width(F_UI, none, -1);
        w2k_text(d, F_UI, x + (w - tw) / 2, y + 40, none, C_GRAYTEXT);
    }
    y += pm.xlist->r.h + 8;
    static const int ids[] = { B_PDRIVE, B_PFOLDER, B_PWINECFG, B_PCOPY };
    static const int widths[] = { 96, 84, 104, 80 };
    button_row(y, x, ids, widths, 4);
    y += 23 + 12;

    int xs = pm.xlist->sel;
    if (xs < 0 || xs >= pm.npx) return;
    const PfxRow *p = &pm.px[xs];
    char text[3 * PATH_MAX], where[PATH_MAX];
    place(p->path, where, sizeof where);
    if (pm.px_msg[0])
        snprintf(text, sizeof text, "%s", pm.px_msg);
    else if (p->wine)
        snprintf(text, sizeof text, "Wine's own prefix is %s, and its C: drive %s/drive_c.", where, where);
    else if (!installed(p->name))
        snprintf(text, sizeof text, "This prefix is %s. %s is no longer installed: its programs run "
                 "here with another version.", where, p->name);
    else
        snprintf(text, sizeof text, "This prefix is %s, and its C: drive %s/drive_c.", where, where);
    w2k_text_wrapped(d, F_UI, x, y, w, text, C_GRAYTEXT);
}

static void paint_options(Drawable d, W2kRect c)
{
    int fh = w2k_font_height(F_UI);
    int x = c.x + 10, w = c.w - 20, y = c.y + 10;

    W2kRect g = { x, y, w, 20 + fh + 6 + 21 + 12 };
    w2k_draw_groupbox(d, &g, "Default");
    w2k_text(d, F_UI, g.x + 12, g.y + 20, "Run Windows programs with:", C_TEXT);
    pm.def->r = (W2kRect){ g.x + 12, g.y + 20 + fh + 6, 220, 21 };
    w2k_combo_draw(d, pm.def);

    static const char *const label[NOPT] = {
        [OPT_DXVK]  = "Use Vulkan for Direct3D (DXVK)",
        [OPT_ESYNC] = "Use esync",
        [OPT_FSYNC] = "Use fsync",
        [OPT_NVAPI] = "Turn on NVIDIA DLSS (NVAPI)",
        [OPT_HUD]   = "Show the frame rate in every program",
        [OPT_UMU]   = "Run programs in the Steam Runtime (umu-launcher)",
    };
    int val[NOPT] = { pm.o.dxvk, pm.o.esync, pm.o.fsync, pm.o.nvapi, pm.o.hud, pm.o.umu };

    g = (W2kRect){ x, g.y + g.h + 8, w, 20 + 5 * (fh + 8) + 2 };
    w2k_draw_groupbox(d, &g, "Games");
    for (int i = OPT_DXVK; i <= OPT_HUD; i++) {
        int by = g.y + 20 + i * (fh + 8);
        pm.opt[i] = (W2kRect){ g.x + 12, by, check_w(label[i]), fh + 2 };
        w2k_draw_checkbox(d, g.x + 12, by, label[i], val[i], 0, 0);
    }

    g = (W2kRect){ x, g.y + g.h + 8, w, 20 + fh + 8 + 3 * fh + 8 };
    w2k_draw_groupbox(d, &g, "Steam Runtime");
    pm.opt[OPT_UMU] = (W2kRect){ g.x + 12, g.y + 20, check_w(label[OPT_UMU]), fh + 2 };
    w2k_draw_checkbox(d, g.x + 12, g.y + 20, label[OPT_UMU], val[OPT_UMU], 0, pm.work == WORK_UMU);
    char text[400];
    if (pm.work == WORK_UMU) snprintf(text, sizeof text, "Downloading umu-launcher...");
    else if (pm.umu_msg[0]) snprintf(text, sizeof text, "%s", pm.umu_msg);
    else if (umu_installed() && umu_runtime_present())
        snprintf(text, sizeof text, "umu-launcher and the Steam Runtime are installed.");
    else if (umu_installed())
        snprintf(text, sizeof text, "umu-launcher is installed. The Steam Runtime, some 700 MB, is "
                 "downloaded the first time a program runs in it.");
    else
        snprintf(text, sizeof text, "Programs run in the runtime Steam gives games. umu-launcher is "
                 "downloaded when this is turned on.");
    w2k_text_wrapped(d, F_UI, g.x + 12 + 18, g.y + 20 + fh + 8, g.w - 24 - 18, text, C_GRAYTEXT);

    pm.b[B_LOG] = (W2kRect){ x, c.y + c.h - 10 - 23, 80, 23 };
}

/* The C: drive of the Windows a version runs programs in. */
static int drive_of(int row, char *path, size_t n)
{
    if (row < 0 || row > pm.npv) return 0;
    if (row == 0) {
        char prefix[PATH_MAX];
        w2k_wine_prefix(prefix, sizeof prefix);
        snprintf(path, n, "%.4000s/drive_c", prefix);
    } else {
        snprintf(path, n, "%.3900s/prefixes/%s/pfx/drive_c", data_dir, pm.pv[row - 1].name);
    }
    return 1;
}

static int closing(W2kWin *w);

static int enabled(int b)
{
    int vs = pm.vlist->sel, rs = pm.rlist->sel, ps = pm.plist->sel, xs = pm.xlist->sel;
    char path[PATH_MAX + 64];
    switch (b) {
    case B_APPLY:   return options_changed();
    case B_DEFAULT: return vs >= 0 && strcmp(vrow_name(vs), pm.o.def) && (vs > 0 || w2k_wine_available());
    case B_REMOVE:  return vs > 0 && !pm.pv[vs - 1].steam && strcmp(pm.pv[vs - 1].name, pm.work_tag);
    case B_DRIVE:   return drive_of(vs, path, sizeof path) && access(path, F_OK) == 0;
    case B_WINECFG: return vs > 0 || (vs == 0 && w2k_wine_available());
    case B_REFRESH: return !pm.listing;
    case B_INSTALL: return !pm.work && rs >= 0 && rs < pm.nrel && !installed(pm.rel[rs].tag);
    case B_STOP:    return pm.work == WORK_INSTALL && !pm.stopped;
    case B_CHANGE:  return ps >= 0 && ps < pm.nprog && access(pm.prog[ps], F_OK) == 0;
    case B_FORGET:  return ps >= 0 && ps < pm.nprog;
    case B_RUN:     return ps >= 0 && ps < pm.nprog && access(pm.prog[ps], F_OK) == 0;
    case B_LOG:     log_path(path, sizeof path); return access(path, F_OK) == 0;
    case B_PDRIVE:
        if (xs < 0 || xs >= pm.npx) return 0;
        snprintf(path, sizeof path, "%.4000s/drive_c", pm.px[xs].path);
        return access(path, F_OK) == 0;
    case B_PFOLDER: return xs >= 0 && xs < pm.npx && access(pm.px[xs].path, F_OK) == 0;
    case B_PWINECFG:
        if (xs < 0 || xs >= pm.npx) return 0;
        return pm.px[xs].wine ? w2k_wine_available() : pm.npv > 0;
    case B_PCOPY:   return xs >= 0 && xs < pm.npx;
    default:        return 1;
    }
}

static int visible(int b)
{
    if (b == B_STOP && pm.work != WORK_INSTALL) return 0;   /* only while there is something to stop */
    return btn[b].page < 0 || btn[b].page == pm.tabs->sel;
}

static void paint(W2kWin *w, Drawable d)
{
    w2k_fill(d, 0, 0, w->w, w->h, C_FACE);
    w2k_tabs_draw(d, pm.tabs);
    W2kRect c = w2k_tabs_client(pm.tabs);
    switch (pm.tabs->sel) {
    case PG_VERSIONS: paint_versions(d, c); break;
    case PG_DOWNLOAD: paint_download(d, c); break;
    case PG_PROGRAMS: paint_programs(d, c); break;
    case PG_PREFIXES: paint_prefixes(d, c); break;
    default:          paint_options(d, c); break;
    }
    pm.b[B_OK]     = (W2kRect){ w->w - 12 - 75 * 3 - 12, w->h - 12 - 23, 75, 23 };
    pm.b[B_CANCEL] = (W2kRect){ w->w - 12 - 75 * 2 - 6, w->h - 12 - 23, 75, 23 };
    pm.b[B_APPLY]  = (W2kRect){ w->w - 12 - 75, w->h - 12 - 23, 75, 23 };
    for (int i = 1; i < NB; i++) {
        if (!visible(i)) continue;
        int st = (i == B_OK ? BS_DEFAULT : 0) | (pm.down == i ? BS_PRESSED : 0) | (enabled(i) ? 0 : BS_DISABLED);
        w2k_draw_pushbutton(d, &pm.b[i], btn[i].label, st);
    }
}

/* ---- Doing things --------------------------------------------------- */

static int save_options(void)
{
    if (!options_changed()) return 1;
    if (w2k_compat_save_options(&pm.o) != 0) {
        w2k_msgbox(pm.win, "Proton Manager", "The settings could not be saved.", MB_OK | MB_ICONERROR);
        return 0;
    }
    pm.saved = pm.o;
    return 1;
}

static void set_default(const char *name)
{
    snprintf(pm.o.def, sizeof pm.o.def, "%s", name);
    fill_versions();
}

static void remove_build(void)
{
    int vs = pm.vlist->sel;
    if (vs <= 0 || pm.pv[vs - 1].steam) return;
    W2kProton p = pm.pv[vs - 1];
    char msg[600];
    snprintf(msg, sizeof msg, "Are you sure you want to remove %s?\n\nThe programs installed with it "
             "are kept, and run with another version.", p.name);
    if (w2k_msgbox(pm.win, "Confirm Version Remove", msg, MB_YESNO | MB_ICONQUESTION) != ID_YES) return;

    /* Out of the list at once -- a folder whose name starts with a dot
     * is not a build -- and deleted beside the window. */
    char gone[PATH_MAX + 64];
    snprintf(gone, sizeof gone, "%.3900s/proton/.removed-%s-%ld", data_dir, p.name, (long)getpid());
    if (rename(p.dir, gone) != 0) {
        snprintf(msg, sizeof msg, "%s could not be removed: %s.", p.name, strerror(errno));
        w2k_msgbox(pm.win, "Proton Manager", msg, MB_OK | MB_ICONERROR);
        return;
    }
    char *argv[] = { "rm", "-rf", "--", gone, NULL };
    spawn(argv);

    fill_versions();
    /* The default, when it was this, is the newest there is now. */
    const char *next = pm.npv ? pm.pv[0].name : "wine";
    if (!strcmp(pm.saved.def, p.name)) {
        snprintf(pm.saved.def, sizeof pm.saved.def, "%s", next);
        w2k_compat_save_options(&pm.saved);
    }
    if (!strcmp(pm.o.def, p.name)) snprintf(pm.o.def, sizeof pm.o.def, "%s", next);
    fill_versions();
    if (pm.listed) fill_releases();
}

static void open_drive(void)
{
    char path[PATH_MAX + 64];
    if (!drive_of(pm.vlist->sel, path, sizeof path)) return;
    char *argv[] = { "l2kexplorer", path, NULL };
    spawn(argv);
}

static void add_program(void)
{
    char path[PATH_MAX];
    home_path(path, sizeof path, "");
    if (!w2k_file_dialog_filter(pm.win, 0, path, sizeof path,
                                "Windows Programs (*.exe;*.msi;*.bat;*.lnk)|*.exe;*.msi;*.bat;*.lnk;*.com;*.cmd|"
                                "All Files (*.*)|*"))
        return;
    if (!w2k_wine_file(path)) {
        char msg[PATH_MAX + 100];
        snprintf(msg, sizeof msg, "%s is not a Windows program.", path);
        w2k_msgbox(pm.win, "Proton Manager", msg, MB_OK | MB_ICONERROR);
        return;
    }
    w2k_file_properties_page(pm.win, path, 1);
    fill_programs();
    char real[PATH_MAX];
    if (realpath(path, real))
        for (int i = 0; i < pm.nprog; i++)
            if (!strcmp(pm.prog[i], real)) list_select(pm.plist, i);
}

static void command(int b)
{
    int ps = pm.plist->sel, xs = pm.xlist->sel;
    switch (b) {
    case B_OK:
        if (save_options() && closing(pm.win)) w2k_win_close(pm.win, ID_OK);
        break;
    case B_CANCEL:
        if (closing(pm.win)) w2k_win_close(pm.win, ID_CANCEL);
        break;
    case B_APPLY:
        save_options();
        break;
    case B_DEFAULT:
        if (pm.vlist->sel >= 0) set_default(vrow_name(pm.vlist->sel));
        break;
    case B_REMOVE:
        remove_build();
        break;
    case B_DRIVE:
        open_drive();
        break;
    case B_WINECFG: {
        char *argv[] = { self_exe, "winecfg", (char *)vrow_name(pm.vlist->sel), NULL };
        spawn(argv);
        break;
    }
    case B_REFRESH:
        start_listing();
        break;
    case B_INSTALL:
        start_install();
        break;
    case B_STOP:
        stop_work();
        break;
    case B_ADD:
        add_program();
        break;
    case B_CHANGE:
        if (ps >= 0 && ps < pm.nprog) {
            char path[4096];
            snprintf(path, sizeof path, "%s", pm.prog[ps]);
            w2k_file_properties_page(pm.win, path, 1);
            fill_programs();
        }
        break;
    case B_FORGET:
        if (ps >= 0 && ps < pm.nprog) {
            char msg[4200];
            snprintf(msg, sizeof msg, "Remove the settings of %s? It will run as Windows programs "
                     "do by default.", strrchr(pm.prog[ps], '/') ? strrchr(pm.prog[ps], '/') + 1 : pm.prog[ps]);
            if (w2k_msgbox(pm.win, "Proton Manager", msg, MB_YESNO | MB_ICONQUESTION) == ID_YES) {
                w2k_compat_set(pm.prog[ps], NULL);
                fill_programs();
            }
        }
        break;
    case B_RUN:
        if (ps >= 0 && ps < pm.nprog) {
            char *argv[] = { self_exe, "run", pm.prog[ps], NULL };
            spawn(argv);
        }
        break;
    case B_PDRIVE:
    case B_PFOLDER:
        if (xs >= 0 && xs < pm.npx) {
            char path[PATH_MAX + 16];
            snprintf(path, sizeof path, "%.4000s%s", pm.px[xs].path, b == B_PDRIVE ? "/drive_c" : "");
            char *argv[] = { "l2kexplorer", path, NULL };
            spawn(argv);
        }
        break;
    case B_PWINECFG:
        if (xs >= 0 && xs < pm.npx) {
            const PfxRow *p = &pm.px[xs];
            if (p->wine) {
                char *argv[] = { self_exe, "winecfg", "wine", NULL };
                spawn(argv);
            } else {
                /* Its own build, or -- that removed -- the default, or the newest. */
                const char *with = installed(p->name) ? p->name : installed(pm.o.def) ? pm.o.def : pm.pv[0].name;
                char *argv[] = { self_exe, "winecfg", (char *)with, (char *)p->name, NULL };
                spawn(argv);
            }
        }
        break;
    case B_PCOPY:
        if (xs >= 0 && xs < pm.npx) {
            char where[PATH_MAX];
            w2k_clipboard_set(pm.px[xs].path);
            place(pm.px[xs].path, where, sizeof where);
            snprintf(pm.px_msg, sizeof pm.px_msg, "Copied %.360s to the clipboard.", where);
        }
        break;
    case B_LOG: {
        char path[PATH_MAX];
        log_path(path, sizeof path);
        char *argv[] = { "l2knotepad", path, NULL };
        spawn(argv);
        break;
    }
    }
    w2k_win_dirty(pm.win);
}

static void on_activate(void *u, int row)
{
    W2kList *l = u;
    if (l == pm.vlist && enabled(B_DEFAULT)) command(B_DEFAULT);
    else if (l == pm.rlist && enabled(B_INSTALL)) command(B_INSTALL);
    else if (l == pm.plist && enabled(B_CHANGE)) command(B_CHANGE);
    else if (l == pm.xlist && enabled(B_PDRIVE)) command(B_PDRIVE);
}

static void on_default(void *u, int idx)
{
    set_default(idx <= 0 ? "wine" : pm.pv[idx - 1].name);
}

static W2kList *page_list(void)
{
    switch (pm.tabs->sel) {
    case PG_VERSIONS: return pm.vlist;
    case PG_DOWNLOAD: return pm.rlist;
    case PG_PROGRAMS: return pm.plist;
    case PG_PREFIXES: return pm.xlist;
    default:          return NULL;
    }
}

static int event(W2kWin *w, XEvent *e)
{
    W2kList *list = page_list();
    switch (e->type) {
    case ButtonPress: {
        XButtonEvent *b = &e->xbutton;
        if (w2k_tabs_press(pm.tabs, b)) { w2k_win_dirty(w); return 1; }
        for (int i = 1; i < NB; i++)
            if (visible(i) && w2k_rect_hit(&pm.b[i], b->x, b->y)) {
                if (b->button == Button1 && enabled(i)) pm.down = i;
                w2k_win_dirty(w);
                return 1;
            }
        if (list && w2k_rect_hit(&list->r, b->x, b->y)) {
            w2k_list_press(list, b);
        } else if (pm.tabs->sel == PG_OPTIONS) {
            if (w2k_combo_press(pm.def, b)) { w2k_win_dirty(w); return 1; }
            pm.def->focused = 0;
            int *val[NOPT] = { &pm.o.dxvk, &pm.o.esync, &pm.o.fsync, &pm.o.nvapi, &pm.o.hud, &pm.o.umu };
            for (int i = 0; i < NOPT; i++)
                if (b->button == Button1 && w2k_rect_hit(&pm.opt[i], b->x, b->y) &&
                    !(i == OPT_UMU && pm.work == WORK_UMU)) {
                    *val[i] = !*val[i];
                    if (i == OPT_UMU) {
                        pm.umu_msg[0] = 0;
                        if (pm.o.umu) get_umu();
                    }
                    break;
                }
        }
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int down = pm.down;
        pm.down = 0;
        if (list) w2k_list_release(list, &e->xbutton);
        if (down && w2k_rect_hit(&pm.b[down], e->xbutton.x, e->xbutton.y) && enabled(down)) command(down);
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (list && w2k_list_motion(list, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case FocusIn:
        /* Back from somewhere a build may have come or gone. */
        if (e->xfocus.mode == NotifyNormal && e->xfocus.detail != NotifyPointer && !pm.work) {
            fill_versions();
            if (pm.listed) fill_releases();
            if (pm.tabs->sel == PG_PROGRAMS) fill_programs();
            if (pm.tabs->sel == PG_PREFIXES) fill_prefixes();
            w2k_win_dirty(w);
        }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (w2k_tabs_key(pm.tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (ks == XK_Escape) { command(B_CANCEL); return 1; }
        if (ks == XK_F5 && pm.tabs->sel == PG_DOWNLOAD) { command(B_REFRESH); return 1; }
        if (list && list->focused && list->sel >= 0 && (ks == XK_Return || ks == XK_KP_Enter)) {
            on_activate(list, list->sel);
            return 1;
        }
        if (ks == XK_Return || ks == XK_KP_Enter) { command(B_OK); return 1; }
        if (list && w2k_list_key(list, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 0;
    }
    }
    return 0;
}

/* Closing with an installation going: stop it, or stay. */
static int closing(W2kWin *w)
{
    if (pm.work != WORK_INSTALL) return 1;
    char msg[300];
    snprintf(msg, sizeof msg, "Proton Manager is still installing %s. Stop the installation and close?", pm.work_tag);
    if (w2k_msgbox(w, "Proton Manager", msg, MB_YESNO | MB_ICONQUESTION) != ID_YES) return 0;
    return 1;
}

static int manager(int page)
{
    if (!ui()) return 1;
    signal(SIGPIPE, SIG_IGN);
    pm.prog = w2k_alloc(MAX_PROG * sizeof *pm.prog);
    w2k_compat_options(&pm.o);
    pm.saved = pm.o;

    int W = 440, H = 480;
    pm.win = main_win = w2k_win_new("Proton Manager", "l2kproton", W, H, 0);
    pm.win->paint = paint;
    pm.win->event = event;
    pm.win->closing = closing;

    pm.tabs = w2k_tabs_new(NULL, on_tab);
    w2k_tabs_add(pm.tabs, "Versions");
    w2k_tabs_add(pm.tabs, "Download");
    w2k_tabs_add(pm.tabs, "Programs");
    w2k_tabs_add(pm.tabs, "Prefixes");
    w2k_tabs_add(pm.tabs, "Options");
    pm.tabs->r = (W2kRect){ 8, 8, W - 16, H - 8 - 40 };

    pm.vlist = w2k_list_new(LV_REPORT);
    /* Columns that fit beside the scroll bar, with no scrolling sideways. */
    w2k_list_add_col(pm.vlist, "Name", 170, 0);
    w2k_list_add_col(pm.vlist, "Installed by", 116, 0);
    w2k_list_add_col(pm.vlist, "Status", 84, 0);
    pm.rlist = w2k_list_new(LV_REPORT);
    w2k_list_add_col(pm.rlist, "Version", 124, 0);
    w2k_list_add_col(pm.rlist, "Released", 84, 0);
    w2k_list_add_col(pm.rlist, "Size", 70, 1);
    w2k_list_add_col(pm.rlist, "Status", 92, 0);
    pm.plist = w2k_list_new(LV_REPORT);
    w2k_list_add_col(pm.plist, "Program", 110, 0);
    w2k_list_add_col(pm.plist, "Runs with", 150, 0);
    w2k_list_add_col(pm.plist, "Compatibility mode", 130, 0);
    w2k_list_add_col(pm.plist, "Folder", 240, 0);
    pm.xlist = w2k_list_new(LV_REPORT);
    w2k_list_add_col(pm.xlist, "Prefix", 118, 0);
    w2k_list_add_col(pm.xlist, "Set up by", 118, 0);
    w2k_list_add_col(pm.xlist, "Programs", 136, 0);
    pm.xlist->on_select = on_prefix_select;
    pm.px = w2k_alloc(MAX_PFX * sizeof *pm.px);
    W2kList *lists[4] = { pm.vlist, pm.rlist, pm.plist, pm.xlist };
    for (int i = 0; i < 4; i++) {
        lists[i]->fullrow = 1;
        lists[i]->user = lists[i];
        lists[i]->on_activate = on_activate;
    }
    pm.def = w2k_combo_new(0);
    pm.def->on_change = on_default;

    fill_versions();
    /* Nothing installed yet: what there is to get is the place to start. */
    if (page == PG_VERSIONS && pm.npv == 0 && !w2k_wine_available()) page = PG_DOWNLOAD;
    pm.tabs->sel = page;
    on_tab(NULL, page);

    w2k_win_center(pm.win, NULL);
    w2k_win_show(pm.win);
    w2k_run();

    if (pm.work == WORK_INSTALL) {
        stop_work();
        pid_t pid = jobs[J_WORK].pid;
        if (pid > 0) while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
        clean_partial(pm.work_tag, pm.work_part);
    }
    main_win = NULL;
    w2k_list_free(pm.vlist);
    w2k_list_free(pm.rlist);
    w2k_list_free(pm.plist);
    w2k_list_free(pm.xlist);
    free(pm.px);
    w2k_combo_free(pm.def);
    w2k_tabs_free(pm.tabs);
    free(pm.prog);
    w2k_fini();
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: l2kproton                     Proton Manager\n"
                    "       l2kproton download            ... at its Download page\n"
                    "       l2kproton run FILE [ARG...]   run a Windows program as its Compatibility tab says\n"
                    "       l2kproton winecfg [NAME [PREFIX]]  Wine configuration for a version's Windows,\n"
                    "                                     or another prefix run with that version\n");
    return 2;
}

int main(int argc, char **argv)
{
    w2k_proton_data_dir(data_dir, sizeof data_dir);
    ssize_t n = readlink("/proc/self/exe", self_exe, sizeof self_exe - 1);
    if (n > 0) {
        self_exe[n] = 0;
        char *del = strstr(self_exe, " (deleted)");
        if (del) *del = 0;
    } else {
        snprintf(self_exe, sizeof self_exe, "l2kproton");
    }

    if (argc >= 2 && !strcmp(argv[1], "run")) {
        if (argc < 3) return usage();
        return cmd_run(argv[2], argv + 3, argc - 3);
    }
    if (argc >= 2 && !strcmp(argv[1], "winecfg"))
        return cmd_winecfg(argc > 2 ? argv[2] : "wine", argc > 3 ? argv[3] : NULL);
    if (argc >= 2 && !strcmp(argv[1], "setup")) return cmd_setup(argc > 2 ? argv[2] : NULL);
    if (argc >= 2 && !strcmp(argv[1], "download")) return manager(PG_DOWNLOAD);
    if (argc >= 2) return usage();
    return manager(PG_VERSIONS);
}
