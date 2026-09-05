/* monitors.c -- where the physical screens actually are.
 *
 * X gives out a single coordinate space covering every monitor at once, so
 * DisplayWidth()/DisplayHeight() describe the bounding box of the whole
 * arrangement and nothing else. Anything that has to sit "on the screen" --
 * the taskbar, the Start menu, a maximised window, a drop-down that must not
 * fall off the edge -- needs the rectangle of one monitor instead, which is
 * what RandR is asked for here.
 *
 * The list is cached and refreshed on RandR notifications rather than
 * queried per use: menu placement hits it on every popup. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/extensions/Xrandr.h>

#define MAX_MON 16

static W2kMonitor mon[MAX_MON];
static int nmon;
static int rr_base = -1;          /* RandR event base; -1 = no usable RandR */

/* Fallback for a server without RandR 1.5: the whole screen is "monitor 1". */
static void one_big_screen(void)
{
    memset(&mon[0], 0, sizeof mon[0]);
    snprintf(mon[0].name, sizeof mon[0].name, "screen");
    mon[0].w = w2k.sw;
    mon[0].h = w2k.sh;
    mon[0].primary = 1;
    nmon = 1;
}

void w2k_monitors_refresh(void)
{
    nmon = 0;

    if (rr_base >= 0) {
        int n = 0;
        XRRMonitorInfo *mi = XRRGetMonitors(w2k.dpy, w2k.root, True, &n);
        if (mi) {
            for (int i = 0; i < n && nmon < MAX_MON; i++) {
                if (mi[i].width <= 0 || mi[i].height <= 0) continue;
                W2kMonitor *m = &mon[nmon++];
                memset(m, 0, sizeof *m);
                char *nm = mi[i].name ? XGetAtomName(w2k.dpy, mi[i].name) : NULL;
                snprintf(m->name, sizeof m->name, "%s", nm ? nm : "?");
                if (nm) XFree(nm);
                m->x = mi[i].x;
                m->y = mi[i].y;
                m->w = mi[i].width;
                m->h = mi[i].height;
                m->primary = mi[i].primary != 0;
            }
            XRRFreeMonitors(mi);
        }
    }
    if (!nmon) one_big_screen();

    /* Exactly one primary. X is happy to report none (or, across a layout
     * change, briefly more than one); the taskbar needs a single answer. */
    int p = -1;
    for (int i = 0; i < nmon; i++) {
        if (!mon[i].primary) continue;
        if (p < 0) p = i;
        else mon[i].primary = 0;
    }
    if (p < 0) mon[0].primary = 1;
}

void w2k_monitors_init(void)
{
    int err_base;
    rr_base = -1;
    if (XRRQueryExtension(w2k.dpy, &rr_base, &err_base)) {
        int maj = 0, min = 0;
        /* XRRGetMonitors -- and with it any notion of "primary" -- is 1.5. */
        if (XRRQueryVersion(w2k.dpy, &maj, &min) &&
            (maj > 1 || (maj == 1 && min >= 5))) {
            XRRSelectInput(w2k.dpy, w2k.root,
                           RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                           RROutputChangeNotifyMask);
        } else {
            rr_base = -1;
        }
    }
    w2k_monitors_refresh();
}

int w2k_monitors_event(XEvent *e)
{
    if (rr_base < 0 || e->type < rr_base) return 0;
    int t = e->type - rr_base;
    if (t != RRScreenChangeNotify && t != RRNotify) return 0;

    /* Xlib caches the screen size inside Display; only this updates it. */
    XRRUpdateConfiguration(e);
    w2k.sw = DisplayWidth(w2k.dpy, w2k.screen);
    w2k.sh = DisplayHeight(w2k.dpy, w2k.screen);
    w2k_monitors_refresh();
    return 1;
}

int w2k_monitor_count(void)
{
    if (!nmon) w2k_monitors_refresh();
    return nmon;
}

const W2kMonitor *w2k_monitor(int i)
{
    if (!nmon) w2k_monitors_refresh();
    if (i < 0 || i >= nmon) i = 0;
    return &mon[i];
}

const W2kMonitor *w2k_monitor_primary(void)
{
    if (!nmon) w2k_monitors_refresh();
    for (int i = 0; i < nmon; i++)
        if (mon[i].primary) return &mon[i];
    return &mon[0];
}

/* The monitor containing (x,y); the nearest one if the point is in a gap
 * between monitors or off the end of the layout. */
const W2kMonitor *w2k_monitor_at(int x, int y)
{
    if (!nmon) w2k_monitors_refresh();
    for (int i = 0; i < nmon; i++)
        if (x >= mon[i].x && x < mon[i].x + mon[i].w &&
            y >= mon[i].y && y < mon[i].y + mon[i].h)
            return &mon[i];

    const W2kMonitor *best = &mon[0];
    long bestd = -1;
    for (int i = 0; i < nmon; i++) {
        int cx = mon[i].x + mon[i].w / 2, cy = mon[i].y + mon[i].h / 2;
        long dx = x - cx, dy = y - cy, d = dx * dx + dy * dy;
        if (bestd < 0 || d < bestd) { bestd = d; best = &mon[i]; }
    }
    return best;
}

/* The monitor a window is "on" -- the one under its centre, which is what
 * Windows uses to decide where a maximised window goes. */
const W2kMonitor *w2k_monitor_of_window(Window w)
{
    Window root;
    int x, y;
    unsigned int ww, wh, bw, dep;
    if (w && XGetGeometry(w2k.dpy, w, &root, &x, &y, &ww, &wh, &bw, &dep)) {
        Window child;
        int rx, ry;
        if (XTranslateCoordinates(w2k.dpy, w, w2k.root, 0, 0, &rx, &ry, &child))
            return w2k_monitor_at(rx + (int)ww / 2, ry + (int)wh / 2);
        return w2k_monitor_at(x + (int)ww / 2, y + (int)wh / 2);
    }
    return w2k_monitor_primary();
}

const W2kMonitor *w2k_monitor_of_pointer(void)
{
    Window r, c;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (XQueryPointer(w2k.dpy, w2k.root, &r, &c, &rx, &ry, &wx, &wy, &mask))
        return w2k_monitor_at(rx, ry);
    return w2k_monitor_primary();
}

/* ------------------------------------------------------------------ *
 * The saved arrangement
 * ------------------------------------------------------------------ */
/* Display Properties applies its Settings tab with one xrandr command, and
 * that is what is played back here at the start of a session -- but only
 * for outputs xrandr reports connected now: a monitor that has gone away
 * since is skipped rather than have the whole command refused. */
int w2k_monitors_apply_saved(void)
{
    if (!w2k_monitor_cfg_n) return 0;

    char connected[8][64];
    int nconn = 0;
    FILE *q = popen("xrandr --query 2>/dev/null", "r");
    if (!q) return 0;
    char line[512];
    while (fgets(line, sizeof line, q) && nconn < 8) {
        char name[64], state[32];
        if (line[0] == ' ' || sscanf(line, "%63s %31s", name, state) < 2) continue;
        if (!strcmp(state, "connected"))
            snprintf(connected[nconn++], 64, "%s", name);
    }
    pclose(q);

    char cmd[2048] = "xrandr";
    int n = 0;
    for (int i = 0; i < w2k_monitor_cfg_n; i++) {
        const W2kMonitorCfg *c = &w2k_monitor_cfg[i];
        int present = 0;
        for (int k = 0; k < nconn; k++)
            if (!strcmp(connected[k], c->name)) present = 1;
        if (!present) continue;
        char part[360], extra[128] = "";
        /* The rate goes with an explicit mode; the scale is xrandr's
         * transform, 125 per cent being a 0.8 scale (a smaller virtual
         * screen stretched over the panel). 100 resets an earlier one. */
        if (c->rate[0] && c->mode[0])
            snprintf(extra, sizeof extra, " --rate %s", c->rate);
        if (c->scale && c->scale != 100) {
            /* Nearest-neighbour, not bilinear: the panel shows the virtual
             * screen's pixels whole (200 per cent is an exact doubling)
             * instead of smearing every edge across its neighbours. */
            char sc[64];
            snprintf(sc, sizeof sc, " --scale %.4fx%.4f --filter nearest",
                     100.0 / c->scale, 100.0 / c->scale);
            strncat(extra, sc, sizeof extra - strlen(extra) - 1);
        } else {
            strncat(extra, " --scale 1x1 --filter bilinear", sizeof extra - strlen(extra) - 1);
        }
        if (!c->enabled)
            snprintf(part, sizeof part, " --output %s --off", c->name);
        else if (c->mode[0])
            snprintf(part, sizeof part, " --output %s --mode %s%s --pos %dx%d%s",
                     c->name, c->mode, extra, c->x, c->y, c->primary ? " --primary" : "");
        else
            snprintf(part, sizeof part, " --output %s --auto%s --pos %dx%d%s",
                     c->name, extra, c->x, c->y, c->primary ? " --primary" : "");
        strncat(cmd, part, sizeof cmd - strlen(cmd) - 1);
        n++;
    }
    if (!n) return 0;
    /* Development aid: W2K_XRANDR_DRY=1 prints the command instead. */
    if (getenv("W2K_XRANDR_DRY")) { fprintf(stderr, "%s\n", cmd); return 1; }
    strncat(cmd, " >/dev/null 2>&1", sizeof cmd - strlen(cmd) - 1);
    return system(cmd) == 0;
}
