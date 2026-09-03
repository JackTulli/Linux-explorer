/* volume.c -- the speaker in the notification area.
 *
 * Windows 2000 put a speaker by the clock: click for a slider, right-click
 * for the mixer, double-click for the full volume control. This is that,
 * driven by pactl (PulseAudio or PipeWire's Pulse server, which is what a
 * desktop has) with amixer as the fallback for bare ALSA.
 *
 * The level is polled rather than subscribed to: once every few seconds
 * costs nothing and keeps the icon honest when something else changes it. */
#include "wm.h"
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int vol_level = -1;           /* 0..100, -1 = unknown  */
static int vol_muted;
static int have_pactl = -1;          /* -1 = not yet looked   */
static int have_amixer = -1;

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
            if (pc) {
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
        if (pc) {
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
    char cmd[160];
    if (have_pactl > 0)
        snprintf(cmd, sizeof cmd,
                 "pactl set-sink-volume @DEFAULT_SINK@ %d%% >/dev/null 2>&1", pct);
    else
        snprintf(cmd, sizeof cmd,
                 "amixer -q set Master %d%% >/dev/null 2>&1", pct);
    if (system(cmd) == 0) vol_level = pct;
}

void volume_toggle_mute(void)
{
    const char *cmd = have_pactl > 0
        ? "pactl set-sink-mute @DEFAULT_SINK@ toggle >/dev/null 2>&1"
        : "amixer -q set Master toggle >/dev/null 2>&1";
    if (system(cmd) == 0) vol_muted = !vol_muted;
}

/* The speaker, drawn at 16x16: a cone, and either arcs or a cross. */
void volume_draw(Drawable d, int x, int y)
{
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
    Pixmap pm = XCreatePixmap(w2k.dpy, win, VP_W, VP_H, w2k.depth);
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

    XCopyArea(w2k.dpy, pm, win, w2k.gc, 0, 0, VP_W, VP_H, 0, 0);
    w2k_free_pixmap(pm);
}

/* `bx`, `by` are the speaker's position in root coordinates; the popup
 * hangs above it, like the Windows one. */
void volume_popup(int bx, int by)
{
    volume_poll();

    const W2kMonitor *m = w2k_monitor_at(bx, by);
    int x = bx - VP_W / 2 + 8, y = by - VP_H;
    if (x < m->x) x = m->x;
    if (x + VP_W > m->x + m->w) x = m->x + m->w - VP_W;
    if (y < m->y) y = m->y;

    XSetWindowAttributes a = {
        .override_redirect = True, .save_under = True,
        .background_pixel = w2k.col[C_FACE],
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask
    };
    Window win = XCreateWindow(w2k.dpy, w2k.root, x, y, VP_W, VP_H, 0,
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
    while (!done) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.window == win) popup_paint(win);
            break;
        case ButtonPress:
        case MotionNotify: {
            int ex = (e.type == ButtonPress) ? e.xbutton.x : e.xmotion.x;
            int ey = (e.type == ButtonPress) ? e.xbutton.y : e.xmotion.y;
            int inside = ex >= 0 && ex < VP_W && ey >= 0 && ey < VP_H;
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
