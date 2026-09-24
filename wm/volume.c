/* volume.c -- the speaker in the notification area.
 *
 * Windows 2000 put a speaker by the clock: click for a slider, right-click
 * for the mixer, double-click for the full volume control. This is that,
 * driven by pactl (PulseAudio or PipeWire's Pulse server, which is what a
 * desktop has) with amixer as the fallback for bare ALSA.
 *
 * The level is subscribed to, not polled: "pactl subscribe" is one
 * long-lived process that says when a sink changes, so the shell asks for
 * the level only when there is news. Polling it every few seconds instead
 * meant forking a shell and two pactl processes around seventeen thousand
 * times a day, and waking the machine to do it. Bare ALSA has nothing to
 * subscribe to, so amixer keeps the slow timer. */
#include "wm.h"
#include "w2kui.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int vol_level = -1;           /* 0..100, -1 = unknown  */
static int vol_muted;
static FILE *sub;                    /* "pactl subscribe", while it lives */
static int   sub_fd = -1;
static int have_pactl = -1;          /* -1 = not yet looked   */
static int have_amixer = -1;
static pid_t set_pid;                /* the level being set, while it runs */
static long  set_at;                 /* ...since when                         */
static int set_pending = -1;         /* the newest level asked for meanwhile */

static int tool_exists(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

static int run_capture(const char *cmd, char *out, int n)
{
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    size_t got = fread(out, 1, (size_t)n - 1, p);
    out[got] = 0;
    pclose(p);
    return got > 0;
}

/* Is there a mixer to talk to at all? A machine with neither pactl nor
 * amixer must not be asked again every few seconds. */
int volume_available(void)
{
    if (have_pactl < 0) have_pactl = tool_exists("pactl");
    if (have_pactl) return 1;
    if (have_amixer < 0) have_amixer = tool_exists("amixer");
    return have_amixer;
}

/* One long-lived "pactl subscribe": its stdout carries a line per change,
 * and the shell asks for the level only when a sink event arrives. The fd
 * joins the window manager's select loop (volume_fd). */
static void sub_open(void)
{
    if (sub || have_pactl != 1) return;
    /* SIGPIPE would kill the shell if the child went away mid-write. */
    signal(SIGPIPE, SIG_IGN);
    sub = popen("pactl subscribe 2>/dev/null", "r");
    if (!sub) return;
    sub_fd = fileno(sub);
    int fl = fcntl(sub_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(sub_fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(sub_fd, F_SETFD, FD_CLOEXEC);
}

/* When the subscription may be started again. With no sound server to
 * talk to, pactl exits the moment it starts; reopening it at every turn
 * of the main loop forked a shell and pactl a few hundred times a second
 * for as long as the server was down. So it waits, longer each time, and
 * the five-second poll stands in meanwhile. */
static long sub_retry_at, sub_backoff, sub_opened_at;

static void sub_close(void)
{
    if (!sub) return;
    pclose(sub);
    sub = NULL;
    sub_fd = -1;
    long now = w2k_now_ms();
    /* One that ran for a while was a real subscription: start over. */
    if (now - sub_opened_at > 60000) sub_backoff = 0;
    sub_backoff = sub_backoff ? sub_backoff * 2 : 5000;
    if (sub_backoff > 60000) sub_backoff = 60000;
    sub_retry_at = now + sub_backoff;
}

/* One level change at a time. A volume key held down asked for a new
 * level at every repeat, each in its own pactl, and they reached the
 * server in any order: the level could end a step or two from where the
 * repeats stopped. Now the newest request waits for the one running and
 * goes when it is done; the ones between are skipped. */
static int set_busy(void)
{
    /* A mixer that hangs is not waited on for ever. */
    if (set_pid > 0 && (kill(set_pid, 0) != 0 || w2k_now_ms() - set_at > 3000))
        set_pid = 0;
    return set_pid > 0;
}

static void set_run(int pct)
{
    char cmd[160];
    if (have_pactl > 0)
        snprintf(cmd, sizeof cmd,
                 "pactl set-sink-volume @DEFAULT_SINK@ %d%% >/dev/null 2>&1", pct);
    else
        snprintf(cmd, sizeof cmd,
                 "amixer -q set Master %d%% >/dev/null 2>&1", pct);
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* As wm_spawn does it. */
        if (w2k.dpy) close(ConnectionNumber(w2k.dpy));
        setsid();
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execlp("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    set_pid = pid;
    set_at = w2k_now_ms();
}

static void set_tick(void *u);

static void set_flush(void)
{
    if (set_pending < 0) return;
    if (set_busy()) {                 /* again shortly, from the main loop */
        w2k_add_timer(20, set_tick, NULL);
        return;
    }
    int pct = set_pending;
    set_pending = -1;
    set_run(pct);
}

static void set_tick(void *u)
{
    (void)u;
    set_flush();
    if (set_pending < 0) w2k_del_timer(set_tick, NULL);
}

/* The fd to watch, or -1 when there is nothing to subscribe to. */
int volume_fd(void)
{
    if (!volume_available()) return -1;
    if (have_pactl == 1 && !sub && w2k_now_ms() >= sub_retry_at) {
        sub_open();
        sub_opened_at = w2k_now_ms();
    }
    return sub_fd;
}

/* Something arrived on the subscription: drain it, and say whether any of
 * it was a sink change worth re-reading the level for. */
int volume_subscribed_event(void)
{
    if (!sub) return 0;
    char buf[1024];
    int want = 0, got = 0;
    for (;;) {
        ssize_t n = read(sub_fd, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            got = 1;
            /* The default sink's level or mute, or the default itself --
             * not "sink-input", which is every stream that starts or
             * stops, the shell's own sounds included. */
            if (strstr(buf, "on sink #") || strstr(buf, "on server")) want = 1;
            if ((size_t)n < sizeof buf - 1) break;
            continue;
        }
        if (n == 0) { sub_close(); break; }        /* the child has gone */
        if (errno == EINTR) continue;
        break;                                      /* EAGAIN: drained */
    }
    (void)got;
    return want;
}

void volume_poll(void)
{
    if (!volume_available()) return;

    char buf[512];
    if (have_pactl) {
        /* One shell, two queries: polling costs a process, so do not pay
         * for it twice. */
        if (run_capture("{ pactl get-sink-volume @DEFAULT_SINK@; "
                        "pactl get-sink-mute @DEFAULT_SINK@; } 2>/dev/null",
                        buf, sizeof buf)) {
            const char *pc = strchr(buf, '%');
            /* Not while a change of ours is on its way: the event from
             * the step before would pull the level back, and the next
             * repeat step from there. */
            if (pc && !set_busy() && set_pending < 0) {
                const char *s = pc;
                while (s > buf && s[-1] >= '0' && s[-1] <= '9') s--;
                vol_level = atoi(s);
                if (vol_level > 100) vol_level = 100;
            }
            vol_muted = strstr(buf, "Mute: yes") != NULL;
        }
        return;
    }
    if (run_capture("amixer get Master 2>/dev/null", buf, sizeof buf)) {
        const char *pc = strchr(buf, '%');
        if (pc && !set_busy() && set_pending < 0) {
            const char *s = pc;
            while (s > buf && s[-1] >= '0' && s[-1] <= '9') s--;
            vol_level = atoi(s);
        }
        vol_muted = strstr(buf, "[off]") != NULL;
    }
}

int volume_level(void) { return vol_level; }
int volume_is_muted(void) { return vol_muted; }

void volume_set(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    /* In the background, the level shown at once: a volume key held down
     * used to stall the whole shell for a pactl round trip per repeat. The
     * sink's own change event then confirms it. */
    vol_level = pct;
    set_pending = pct;
    set_flush();
}

/* Play, pause and stop for whatever is playing: playerctl if it is
 * installed, otherwise the first MPRIS player on the session bus, asked
 * directly -- as a method call: dbus-send sends a signal unless told,
 * which no player answers, so without playerctl the keys did nothing.
 * Runs in the background; a key must not wait on D-Bus. */
void media_control(const char *method)
{
    if (!method || (strcmp(method, "PlayPause") != 0 && strcmp(method, "Pause") != 0 &&
                    strcmp(method, "Stop") != 0)) return;
    /* The players are found with dbus-send too: busctl is systemd's, and
     * not there on Alpine. */
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "if command -v playerctl >/dev/null 2>&1; then playerctl %s; else "
             "p=$(dbus-send --session --print-reply --dest=org.freedesktop.DBus "
             "/org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null | "
             "grep -o 'org\\.mpris\\.MediaPlayer2\\.[^\"]*' | head -n 1); "
             "[ -n \"$p\" ] && dbus-send --session --type=method_call --dest=\"$p\" /org/mpris/MediaPlayer2 "
             "org.mpris.MediaPlayer2.Player.%s; fi >/dev/null 2>&1",
             strcmp(method, "Stop") == 0 ? "stop" :
             strcmp(method, "Pause") == 0 ? "pause" : "play-pause", method);
    wm_spawn(cmd);
}

void volume_toggle_mute(void)
{
    const char *cmd = have_pactl > 0
        ? "pactl set-sink-mute @DEFAULT_SINK@ toggle >/dev/null 2>&1"
        : "amixer -q set Master toggle >/dev/null 2>&1";
    vol_muted = !vol_muted;            /* shown now; done in the background */
    wm_spawn(cmd);
}

/* The speaker, drawn at 16x16: a cone, and either arcs or a cross. On
 * the Windows 7 bar it is Windows 7's own glyph, cut from a capture. */
void volume_draw(Drawable d, int x, int y)
{
    if (w2k_theme == THEME_BASIC7 || w2k_theme == THEME_AERO) {   /* the white speaker */
        static W2kSkin *glyph;
        static int tried;
        if (!tried) {
            tried = 1;
            char path[1024];
            if (w2k_skin_path("w7-volume.png", path, sizeof path)) glyph = w2k_skin_load(path);
        }
        if (glyph) {
            w2k_skin_draw(d, glyph, x, y, 0, 0, 16, 16);
            if (vol_muted || vol_level == 0)
                for (int i = 0; i < 6; i++) {          /* a red cross over the waves */
                    w2k_fill_rgb(d, x + 9 + i, y + 5 + i, 1, 1, 200, 30, 30);
                    w2k_fill_rgb(d, x + 14 - i, y + 5 + i, 1, 1, 200, 30, 30);
                }
            return;
        }
    }
    int c = C_TEXT;
    /* body */
    w2k_fill(d, x + 2, y + 6, 2, 4, c);
    /* cone */
    for (int i = 0; i < 5; i++)
        w2k_fill(d, x + 4 + i, y + 6 - i, 1, 4 + i * 2, c);

    if (vol_muted || vol_level == 0) {
        /* a cross, like the muted speaker */
        for (int i = 0; i < 5; i++) {
            w2k_fill(d, x + 10 + i, y + 5 + i, 1, 1, c);
            w2k_fill(d, x + 14 - i, y + 5 + i, 1, 1, c);
        }
        return;
    }
    /* two arcs, more of them the louder it is */
    int arcs = vol_level > 66 ? 2 : 1;
    for (int a = 0; a < arcs; a++) {
        int ax = x + 10 + a * 2;
        w2k_fill(d, ax, y + 6, 1, 4, c);
        w2k_fill(d, ax - 1 + 1, y + 5, 1, 1, c);
        w2k_fill(d, ax - 1 + 1, y + 10, 1, 1, c);
    }
}

/* ------------------------------------------------------------------ *
 * The slider that drops out of the speaker
 * ------------------------------------------------------------------ *
 * A small override-redirect window with a vertical slider and a mute box,
 * run as its own modal loop with the pointer grabbed -- the same way the
 * menus work, so it closes as soon as the user clicks elsewhere. */
#define VP_W  62
#define VP_H 136
#define SL_X  22
#define SL_Y  12
#define SL_H  86

static void popup_paint(Window win)
{
    Pixmap pm = XCreatePixmap(w2k.dpy, win, (unsigned)w2k_px(VP_W),
                              (unsigned)w2k_px(VP_H), w2k.depth);
    w2k_fill(pm, 0, 0, VP_W, VP_H, C_FACE);
    w2k_edge(pm, 0, 0, VP_W, VP_H, EDGE_RAISED, BF_RECT);

    int lvl = volume_level() < 0 ? 0 : volume_level();
    char b[16];
    snprintf(b, sizeof b, "%d%%", lvl);
    int tw = w2k_text_width(F_UI, b, -1);
    w2k_text(pm, F_UI, (VP_W - tw) / 2, SL_Y + SL_H + 8, b, C_TEXT);

    /* The channel: a sunken groove down the middle. */
    w2k_edge(pm, SL_X + 7, SL_Y, 4, SL_H, EDGE_SUNKEN, BF_RECT);

    /* The thumb, at the level. */
    int ty = SL_Y + (100 - lvl) * (SL_H - 10) / 100;
    w2k_button(pm, SL_X, ty, 18, 10, 0);

    w2k_draw_checkbox(pm, 6, VP_H - 22, "Mute", volume_is_muted(), 0, 0);

    XCopyArea(w2k.dpy, pm, win, w2k.gc, 0, 0, (unsigned)w2k_px(VP_W),
              (unsigned)w2k_px(VP_H), 0, 0);
    w2k_free_pixmap(pm);
}

/* `bx`, `by` are the speaker's position in root coordinates; the popup
 * hangs above it, like the Windows one. */
void volume_popup(int bx, int by)
{
    volume_poll();

    const W2kMonitor *m = w2k_monitor_at(bx, by);
    int pw = w2k_px(VP_W), ph = w2k_px(VP_H);
    int x = bx - pw / 2 + w2k_px(8), y = by - ph;
    if (x < m->x) x = m->x;
    if (x + pw > m->x + m->w) x = m->x + m->w - pw;
    if (y < m->y) y = m->y;

    XSetWindowAttributes a = {
        .override_redirect = True, .save_under = True,
        .background_pixel = w2k.col[C_FACE],
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask
    };
    Window win = XCreateWindow(w2k.dpy, w2k.root, x, y, (unsigned)pw, (unsigned)ph, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWSaveUnder | CWBackPixel |
                               CWEventMask, &a);
    XMapRaised(w2k.dpy, win);
    if (XGrabPointer(w2k.dpy, win, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, w2k.cur_arrow,
                     CurrentTime) != GrabSuccess) {
        XDestroyWindow(w2k.dpy, win);
        return;
    }

    int done = 0, dragging = 0, pending = -1, applied = -1;
    long last_set = 0;
    popup_paint(win);
    while (!done && running) {
        /* The level the slider was let go at may be waiting for the one
         * before it; this loop runs no timers, so see it out here. */
        while (set_pending >= 0 && !XPending(w2k.dpy)) {
            usleep(10000);
            set_flush();
        }
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.window == win) popup_paint(win);
            break;
        case ButtonPress:
        case MotionNotify: {
            /* The grab reports events on our other windows relative to
             * them (owner_events): those are outside by definition. */
            Window on = (e.type == ButtonPress) ? e.xbutton.window : e.xmotion.window;
            int ex = w2k_lp((e.type == ButtonPress) ? e.xbutton.x : e.xmotion.x);
            int ey = w2k_lp((e.type == ButtonPress) ? e.xbutton.y : e.xmotion.y);
            int inside = on == win && ex >= 0 && ex < VP_W && ey >= 0 && ey < VP_H;
            if (e.type == MotionNotify && on != win) break;
            if (e.type == ButtonPress && !inside) { done = 1; break; }
            if (e.type == ButtonPress && ey >= VP_H - 26) {
                volume_toggle_mute();
                popup_paint(win);
                break;
            }
            if (e.type == ButtonPress) dragging = 1;
            if (!dragging) break;
            if (ey < SL_Y) ey = SL_Y;
            if (ey > SL_Y + SL_H) ey = SL_Y + SL_H;
            pending = 100 - (ey - SL_Y) * 100 / SL_H;

            /* Each change runs a mixer command, so rate-limit while
             * dragging: without this a single drag spawns a process per
             * motion event. The final position is applied on release. */
            long now = w2k_now_ms();
            if (now - last_set >= 80) {
                last_set = now;
                volume_set(pending);
                applied = pending;
            } else {
                vol_level = pending;      /* show it moving regardless */
            }
            popup_paint(win);
            break;
        }
        case ButtonRelease:
            if (dragging && pending >= 0 && pending != applied) {
                volume_set(pending);      /* land exactly where it was left */
                applied = pending;
            }
            dragging = 0;
            break;
        case KeyPress:
            done = 1;
            break;
        default:
            if (w2k_menu_foreign_event) w2k_menu_foreign_event(&e);
            break;
        }
    }
    XUngrabPointer(w2k.dpy, CurrentTime);
    XDestroyWindow(w2k.dpy, win);
}
