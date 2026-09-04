/* taskbar.c -- the bar along the bottom: Start button, Quick Launch,
 * task buttons and the tray clock.
 *
 * The taskbar lives inside the window manager rather than in a separate
 * process: it already knows every client, so there is nothing to
 * synchronise and no IPC to pay for. */
#include "wm.h"
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TB_PAD        2
#define BTN_H        (w2k_theme == THEME_CLASSIC ? 22 : w2k_theme_task_h(w2k_theme))
#define START_GAP     4
#define GRIP_W        7
/* Windows 7's buttons: 60 wide for a 32-pixel icon, 44 for a 16-pixel one. */
#define W7_BTN_W     (w2k_taskbar_small ? 44 : 60)
#define QL_BTN       (w2k_theme == THEME_BASIC7 ? W7_BTN_W : 22)
#define TASK_MAXW   160
#define TASK_MINW    40
#define TASK_GAP      3
/* Where a button row starts: a pixel of bar shows above the classic and
 * XP buttons; Windows 7's fill the row. */
#define BTN_TOP      ((TASKBAR_ROW - BTN_H) / 2 + (BTN_H < TASKBAR_ROW ? 1 : 0))
#define W7_SLIVER    15       /* Windows 7's Show Desktop, at the bar's far end */
#define TRAY_PAD      6

static Window tb;
static int    start_hot;          /* pointer over the Start button */
static Window tb_trigger;          /* the strip that brings it back */
static int    tb_x, tb_y, tb_w, tb_h;

/* The bar occupies one edge of the primary monitor. On the left or right
 * it is a column: the same buttons, laid out down instead of across. */
int taskbar_thickness(void)
{
    /* A column has to be wide enough for a task button's icon and a word
     * or two of its title, so it is measured differently from a row. */
    if (w2k_taskbar_edge == TB_LEFT || w2k_taskbar_edge == TB_RIGHT)
        return 64 + (w2k_taskbar_rows - 1) * 24;
    return TASKBAR_ROW * w2k_taskbar_rows;
}

/* Where the Start menu should hang from: the Start button's outer corner. */
void taskbar_start_origin(int *x, int *y)
{
    *x = tb_x + TB_PAD;
    *y = (w2k_taskbar_edge == TB_TOP) ? tb_y + tb_h : tb_y;
}

static void taskbar_geometry(int *x, int *y, int *w, int *h)
{
    const W2kMonitor *m = w2k_monitor_primary();
    int t = taskbar_thickness();
    switch (w2k_taskbar_edge) {
    case TB_TOP:   *x = m->x; *y = m->y;                 *w = m->w; *h = t; break;
    case TB_LEFT:  *x = m->x; *y = m->y;                 *w = t;    *h = m->h; break;
    case TB_RIGHT: *x = m->x + m->w - t; *y = m->y;      *w = t;    *h = m->h; break;
    default:       *x = m->x; *y = m->y + m->h - t;      *w = m->w; *h = t; break;
    }
}
static int    tb_shown = 1;        /* only meaningful when auto-hiding */
static char   clock_text[32];
static char   clock_date[40];     /* Windows 7 shows the date under the time */

/* Quick Launch: Show Desktop is built in, the rest is whatever the user has
 * pinned (~/.w2k/pinned-taskbar), rebuilt on every layout so a pin or unpin
 * shows up at once. */
#define NQL_MAX (PIN_MAX + 4)
static struct { int icon; char cmd[512]; char tip[128]; } ql[NQL_MAX];
static int nql;

/* ------------------------------------------------------------------ *
 * Themed painting
 *
 * The classic bar is a raised grey panel. Windows XP's is a blue
 * gradient with a bright band at the top and a dark edge at the bottom;
 * the stops below are read off a 1:1 screenshot of the real thing,
 * measured down a column of empty taskbar. Windows 7 Basic's bar is the
 * same idea in graphite.
 * ------------------------------------------------------------------ */
/* The Start button artwork for the current theme, loaded once. Absent, the
 * bar falls back to drawing a classic button. */
/* The Windows 7 orb lights up under the pointer -- not at once, but over
 * a quarter of a second, and it dims the same way when the pointer goes.
 * The strip holds the normal and lit orbs; the frames between are mixed
 * from them once, and a timer steps through them. */
#define ORB_FRAMES 9
#define ORB_STEP_MS 28
static W2kSkin *orb_frames[ORB_FRAMES];
static int orb_frame, orb_target;

static void orb_frames_free(void)
{
    for (int i = 0; i < ORB_FRAMES; i++) { w2k_skin_free(orb_frames[i]); orb_frames[i] = NULL; }
    orb_frame = orb_target = 0;
}

static void orb_frames_build(const char *path)
{
    int w = 0, h = 0;
    unsigned char *rgba = w2k_image_load(path, &w, &h);
    if (!rgba || h < 3) { free(rgba); return; }
    int ch = h / 3;                         /* normal, lit, pressed */
    size_t cell = (size_t)w * ch * 4;
    unsigned char *mix = malloc(cell);
    if (!mix) { free(rgba); return; }
    const unsigned char *a = rgba, *b = rgba + cell;
    for (int f = 0; f < ORB_FRAMES; f++) {
        int t = f * 256 / (ORB_FRAMES - 1);
        for (size_t i = 0; i < cell; i++)
            mix[i] = (unsigned char)((a[i] * (256 - t) + b[i] * t) >> 8);
        orb_frames[f] = w2k_skin_from_rgba(mix, w, ch);
    }
    free(mix);
    free(rgba);
}

static void orb_tick(void *u)
{
    (void)u;
    if (orb_frame < orb_target) orb_frame++;
    else if (orb_frame > orb_target) orb_frame--;
    if (orb_frame == orb_target) w2k_del_timer(orb_tick, NULL);
    taskbar_paint();
}

/* The pointer came on to, or left, the Start button. */
static void start_hot_changed(void)
{
    if (w2k_theme == THEME_BASIC7 && orb_frames[0]) {
        orb_target = start_hot ? ORB_FRAMES - 1 : 0;
        if (orb_frame != orb_target) w2k_add_timer(ORB_STEP_MS, orb_tick, NULL);
    }
    if (w2k_theme != THEME_CLASSIC) taskbar_paint();
}

static W2kSkin *theme_start_skin(void)
{
    static W2kSkin *skin;
    static int tried_theme = -1, tried_small = -1;
    if (tried_theme == w2k_theme && tried_small == w2k_taskbar_small) return skin;
    tried_theme = w2k_theme;
    tried_small = w2k_taskbar_small;
    w2k_skin_free(skin);
    skin = NULL;
    w2k_del_timer(orb_tick, NULL);
    orb_frames_free();
    if (w2k_theme == THEME_CLASSIC) return NULL;

    const char *file = w2k_theme == THEME_XP ? "xp-start.png"
                                             : w2k_taskbar_small ? "w7-orb-small.png"
                                                                : "w7-orb.png";
    char path[1024];
    if (w2k_skin_path(file, path, sizeof path)) {
        skin = w2k_skin_load(path);
        if (skin && w2k_theme == THEME_BASIC7) orb_frames_build(path);
    }
    return skin;
}

static void ql_build(void)
{
    nql = 0;
    if (w2k_theme != THEME_BASIC7) {
        /* Windows 7 moved Show Desktop to the sliver at the bar's end. */
        ql[nql].icon = ICO_DESKTOP;
        ql[nql].cmd[0] = 0;                   /* empty = Show Desktop */
        snprintf(ql[nql].tip, sizeof ql[nql].tip, "Show Desktop");
        nql++;
    }

    if (!w2k_taskbar_quicklaunch) { nql = 0; return; }

    Pin pinned[PIN_MAX];
    int n = pins_load(PIN_TASKBAR, pinned, PIN_MAX);
    for (int i = 0; i < n && nql < NQL_MAX; i++) {
        snprintf(ql[nql].cmd, sizeof ql[nql].cmd, "%.511s", pinned[i].cmd);
        snprintf(ql[nql].tip, sizeof ql[nql].tip, "%.127s", pinned[i].label);
        ql[nql].icon = pin_icon(&pinned[i]);
        nql++;
    }
}
#define NQL nql

/* Laid out fresh on every paint; also consulted by the click handler. */
static struct { Client *c; int x, y, w, h, row; } tasks[64];
static int ntasks;
static int start_w, ql_x, task_x, task_w, tray_x, tray_w;
static int vol_x, notify_x, notify_w;      /* speaker and docked icons */
static int ql_y, task_y, tray_y, vol_y, notify_y;  /* the same, down a column */

Window taskbar_window(void) { return tb; }

static int vertical(void);

/* ------------------------------------------------------------------ *
 * Dropping onto the bar
 * ------------------------------------------------------------------ *
 * A file dropped on the Start button becomes a shortcut in the Start
 * menu's Programs folder; dropped on Quick Launch it is pinned there.
 * That is how things get into the Start menu in Windows -- dragging onto
 * the menu itself would mean holding a menu open under a pointer grab
 * that already belongs to the drag, which is a fight not worth having. */
static int start_button_hit(int x, int y)
{
    if (w2k_theme != THEME_CLASSIC && theme_start_skin()) {
        /* The skinned button fills the bar's height. */
        int sw = w2k_skin_w(theme_start_skin());
        return x >= 0 && x < sw && y >= 0 && y < taskbar_thickness();
    }
    int sby = vertical() ? TB_PAD : BTN_TOP;
    return x >= TB_PAD && x < TB_PAD + start_w &&
           y >= sby && y < sby + BTN_H;
}

static int quicklaunch_hit(int x, int y)
{
    if (!NQL) return 0;
    if (vertical())
        return y >= ql_y && y < ql_y + NQL * QL_BTN;
    return x >= ql_x && x < ql_x + NQL * QL_BTN;
}

/* Write a .desktop for `path` into `dir`. */
static void make_shortcut(const char *dir, const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char name[256];
    snprintf(name, sizeof name, "%.255s", base);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;          /* drop the extension */

    char file[2100];
    snprintf(file, sizeof file, "%.1023s/%.255s.desktop", dir, name);
    FILE *f = fopen(file, "w");
    if (!f) return;

    /* A folder or document opens through the shell's associations; an
     * executable runs directly. */
    char cmd[2200];
    if (access(path, X_OK) == 0) snprintf(cmd, sizeof cmd, "%.1023s", path);
    else                         w2k_assoc_command(path, cmd, sizeof cmd);

    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n"
               "Terminal=false\n", name, cmd);
    fclose(f);
    chmod(file, 0755);
}

int taskbar_dnd_accept(int x, int y)
{
    return start_button_hit(x, y) || quicklaunch_hit(x, y);
}

void taskbar_dnd_drop(int x, int y, const char *uris)
{
    char paths[32][1024];
    int n = w2k_uri_list_paths(uris, paths, 32);
    if (!n) return;

    if (start_button_hit(x, y)) {
        char dir[1300];
        startdir_path(dir, sizeof dir);
        strncat(dir, "/Programs", sizeof dir - strlen(dir) - 1);
        for (int i = 0; i < n; i++) make_shortcut(dir, paths[i]);
        balloon_show("Added to the Start menu",
                     n == 1 ? "The shortcut is under Programs."
                            : "The shortcuts are under Programs.");
        return;
    }
    if (quicklaunch_hit(x, y)) {
        for (int i = 0; i < n; i++) {
            char cmd[2200];
            if (access(paths[i], X_OK) == 0)
                snprintf(cmd, sizeof cmd, "%.1023s", paths[i]);
            else
                w2k_assoc_command(paths[i], cmd, sizeof cmd);
            const char *base = strrchr(paths[i], '/');
            pins_add(PIN_TASKBAR, cmd, base ? base + 1 : paths[i], NULL);
        }
        taskbar_paint();
    }
}

/* The screen rectangle of a window's task button, so the minimise
 * animation knows where to fly to. Returns 0 if it has no button. */
int taskbar_button_rect(Client *c, int *x, int *y, int *w, int *h)
{
    for (int i = 0; i < ntasks; i++) {
        if (tasks[i].c != c) continue;
        *x = tb_x + tasks[i].x;
        *y = tb_y + tasks[i].y;
        *w = tasks[i].w;
        *h = tasks[i].h;
        return 1;
    }
    return 0;
}

/* Dragging a task button to another position, and the hover that raises a
 * tooltip. Both are pointer state the bar has to remember between events. */
static int drag_task = -1;
static int hover_task = -1;
static long hover_since;
static int tip_up;

/* The auto-hide trigger strip, or None. It is InputOnly, but stacking still
 * decides who gets the pointer, so the restacking code has to keep it on
 * top -- otherwise a newly mapped window swallows the pointer and the bar
 * never comes back. */
Window taskbar_trigger_window(void) { return tb_trigger; }

/* ------------------------------------------------------------------ *
 * Layout
 * ------------------------------------------------------------------ */
static void update_clock(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int h12 = tm.tm_hour % 12;
    if (!h12) h12 = 12;
    snprintf(clock_text, sizeof clock_text, "%d:%02d %s", h12, tm.tm_min,
             tm.tm_hour < 12 ? "AM" : "PM");
    snprintf(clock_date, sizeof clock_date, "%d/%d/%d", tm.tm_mon + 1,
             tm.tm_mday, tm.tm_year + 1900);
}

static int vertical(void)
{
    return w2k_taskbar_edge == TB_LEFT || w2k_taskbar_edge == TB_RIGHT;
}

/* The column layout, for a bar down the left or right: Start at the top,
 * Quick Launch under it, task buttons filling the middle, and the tray at
 * the bottom. The pieces are the same ones the row layout uses -- only the
 * axis they run along changes. */
static void layout_vertical(void)
{
    ql_build();
    start_w = tb_w - 2 * TB_PAD;

    update_clock();
    tray_w = w2k_taskbar_showclock ? BTN_H : 0;
    tray_x = TB_PAD;

    /* Bottom up: clock, speaker, docked icons. */
    int bottom = tb_h - TB_PAD;
    tray_y = bottom - (w2k_taskbar_showclock ? BTN_H : 0);
    vol_y = tray_y - 4 - 16;
    notify_w = tray_width();
    notify_y = vol_y - (notify_w ? notify_w + 4 : 0);
    tray_layout_column(TB_PAD + (tb_w - 2 * TB_PAD - 16) / 2, notify_y);

    ql_y = TB_PAD + BTN_H + START_GAP + GRIP_W;
    task_y = ql_y + NQL * QL_BTN + START_GAP + GRIP_W;

    ntasks = 0;
    for (Client *c = clients; c && ntasks < 64; c = c->next)
        if (!c->skip_taskbar) tasks[ntasks++].c = c;
    for (int i = 0; i < ntasks / 2; i++) {
        Client *t = tasks[i].c;
        tasks[i].c = tasks[ntasks - 1 - i].c;
        tasks[ntasks - 1 - i].c = t;
    }
    for (int i = 0; i < ntasks; i++)
        if (tasks[i].c->tb_order == 0) tasks[i].c->tb_order = (i + 1) * 16;
    for (int i = 1; i < ntasks; i++) {
        int k = i;
        while (k > 0 && tasks[k - 1].c->tb_order > tasks[k].c->tb_order) {
            Client *t = tasks[k - 1].c;
            tasks[k - 1].c = tasks[k].c;
            tasks[k].c = t;
            k--;
        }
    }

    int avail = notify_y - task_y - TASK_GAP;
    if (avail < 0) avail = 0;
    int th = BTN_H + 2;
    if (ntasks > 0 && ntasks * (th + TASK_GAP) > avail)
        th = avail / ntasks - TASK_GAP;
    if (th < 12) th = 12;
    task_w = tb_w - 2 * TB_PAD;
    for (int i = 0; i < ntasks; i++) {
        tasks[i].x = TB_PAD;
        tasks[i].y = task_y + i * (th + TASK_GAP);
        tasks[i].w = task_w;
        tasks[i].h = th;
        tasks[i].row = 0;
    }
}

static void layout(void)
{
    if (vertical()) { layout_vertical(); return; }
    ql_build();
    /* A themed Start button is a bitmap, so the strip's width is the
     * button's width; the classic one is sized to its label. */
    W2kSkin *sk = theme_start_skin();
    start_w = sk ? w2k_skin_w(sk) - 2
                 : 20 + w2k_mnemonic_width(F_UI_BOLD, "Start") + 8;
    ql_x    = TB_PAD + start_w + START_GAP + GRIP_W;
    task_x  = ql_x + NQL * QL_BTN + START_GAP + GRIP_W;
    int seven = w2k_theme == THEME_BASIC7;
    if (seven) {
        /* Measured: the first pinned button starts 58 pixels in, and the
         * buttons, pinned and running alike, are 60 wide and adjacent. */
        ql_x   = 58;
        task_x = ql_x + NQL * QL_BTN;
    }

    update_clock();
    /* The tray, right to left: clock, then the speaker, then whatever
     * applications have docked. */
    int clock_w = w2k_text_width(F_UI, clock_text, -1);
    if (seven && !w2k_taskbar_small) {
        /* Two lines, the date under the time; and the Show Desktop
         * sliver plus a gap stays clear at the bar's end. */
        int dw = w2k_text_width(F_UI, clock_date, -1);
        if (dw > clock_w) clock_w = dw;
    }
    tray_w  = w2k_taskbar_showclock ? clock_w + 2 * TRAY_PAD : 0;
    tray_x  = tb_w - (seven ? W7_SLIVER + 6 : TB_PAD) - tray_w;

    vol_x = tray_x - 4 - 16;
    notify_w = tray_width();
    notify_x = vol_x - (notify_w ? notify_w + 4 : 0);
    tray_layout(notify_x, (TASKBAR_H - BTN_H) / 2 + (BTN_H < TASKBAR_ROW ? 1 : 0) + (BTN_H - 16) / 2, 16);

    ntasks = 0;
    for (Client *c = clients; c && ntasks < 64; c = c->next)
        if (!c->skip_taskbar) tasks[ntasks++].c = c;

    /* Oldest window leftmost: our client list is newest-first, so reverse. */
    for (int i = 0; i < ntasks / 2; i++) {
        Client *t = tasks[i].c;
        tasks[i].c = tasks[ntasks - 1 - i].c;
        tasks[ntasks - 1 - i].c = t;
    }

    /* Then apply whatever order the user has dragged them into. A window
     * with no key yet keeps its arrival position. */
    for (int i = 0; i < ntasks; i++)
        if (tasks[i].c->tb_order == 0) tasks[i].c->tb_order = (i + 1) * 16;
    for (int i = 1; i < ntasks; i++) {
        int k = i;
        while (k > 0 && tasks[k - 1].c->tb_order > tasks[k].c->tb_order) {
            Client *t = tasks[k - 1].c;
            tasks[k - 1].c = tasks[k].c;
            tasks[k].c = t;
            k--;
        }
    }

    /* Task buttons fill the strip between Quick Launch and the tray, and
     * wrap onto further rows when the bar is more than one row tall --
     * which is what dragging the top edge of the Windows taskbar does. */
    int rows = w2k_taskbar_rows;
    int gap = seven ? 0 : TASK_GAP;
    int avail = notify_x - task_x - gap;
    if (avail < 0) avail = 0;
    int per_row = ntasks ? (ntasks + rows - 1) / rows : 0;
    if (per_row < 1) per_row = 1;

    if (ntasks > 0) {
        task_w = (avail - (per_row - 1) * gap) / per_row;
        /* Windows 7 combines to an icon-wide button unless told to show
         * labels, when its buttons are as wide as XP's. */
        int maxw = seven && !w2k_taskbar_labels ? W7_BTN_W : TASK_MAXW;
        if (task_w > maxw) task_w = maxw;
        if (task_w < TASK_MINW) task_w = TASK_MINW;
    } else task_w = 0;

    for (int i = 0; i < ntasks; i++) {
        tasks[i].x = task_x + (i % per_row) * (task_w + gap);
        tasks[i].row = i / per_row;
        tasks[i].w = task_w;
        tasks[i].y = BTN_TOP + tasks[i].row * TASKBAR_ROW;
        tasks[i].h = BTN_H;
    }
}

/* The two-line vertical gripper that separates taskbar sections. */
static void draw_grip(Drawable d, int x, int y, int h)
{
    w2k_vline(d, x + 1, y, h, C_HILIGHT);
    w2k_vline(d, x + 2, y, h, C_SHADOW);
    w2k_vline(d, x + 4, y, h, C_HILIGHT);
    w2k_vline(d, x + 5, y, h, C_SHADOW);
}

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */
/* Everything on the bar, into `pm`, which is `h` rows tall and tb_w
 * wide. taskbar_paint() copies it to the window; taskbar_render() to a
 * file. */
static void taskbar_draw(Pixmap pm, int h)
{
    if (w2k_theme == THEME_CLASSIC) {
        w2k_fill(pm, 0, 0, tb_w, h, C_FACE);
        /* The bar's own raised top edge. */
        w2k_hline(pm, 0, 0, tb_w, C_LIGHT);
        w2k_hline(pm, 0, 1, tb_w, C_HILIGHT);
    } else {
        w2k_theme_bar(pm, 0, 0, tb_w, h, w2k_theme);
    }

    int vert = vertical();
    int by = vert ? TB_PAD : BTN_TOP;

    /* --- Start button ------------------------------------------------ */
    int start_pressed = startmenu_is_open();
    W2kSkin *skin = theme_start_skin();
    if (skin) {
        /* Three states stacked in one strip: normal, hot, pressed. */
        int sw = w2k_skin_w(skin), sh = w2k_skin_h(skin) / 3;
        int state = start_pressed ? 2 : (start_hot ? 1 : 0);
        int sy = vert ? TB_PAD : (h - sh) / 2;
        if (state != 2 && orb_frames[0] && w2k_theme == THEME_BASIC7)
            /* Part way through the glow: the mixed frame. */
            w2k_skin_draw(pm, orb_frames[orb_frame], TB_PAD - 2, sy, 0, 0, sw, sh);
        else
            w2k_skin_draw(pm, skin, TB_PAD - 2, sy, 0, state * sh, sw, sh);
    } else {
        w2k_button(pm, TB_PAD, by, start_w, BTN_H, start_pressed);
        int o = start_pressed ? 1 : 0;
        w2k_icon_draw(pm, TB_PAD + 3 + o, by + (BTN_H - 16) / 2 + o,
                      ICO_STARTFLAG);
        w2k_text_mnemonic(pm, F_UI_BOLD, TB_PAD + 21 + o,
                          by + (BTN_H - w2k_font_height(F_UI_BOLD)) / 2 + o,
                          "Start", C_TEXT, 0);
    }
    if (!vert && w2k_theme == THEME_CLASSIC)
        draw_grip(pm, TB_PAD + start_w + 1, by + 2, BTN_H - 4);

    /* --- Quick Launch ------------------------------------------------ */
    for (int i = 0; i < NQL; i++) {
        if (vert)
            w2k_icon_draw(pm, TB_PAD + (start_w - 16) / 2,
                          ql_y + i * QL_BTN + (QL_BTN - 16) / 2, ql[i].icon);
        else if (w2k_theme == THEME_BASIC7) {
            /* A framed box with its icon, like a running window's. */
            w2k_theme_taskbutton(pm, ql_x + i * QL_BTN, 0, QL_BTN, BTN_H,
                                 W2K_TB_NORMAL, w2k_theme);
            if (w2k_taskbar_small)
                w2k_icon_draw(pm, ql_x + i * QL_BTN + (QL_BTN - 16) / 2,
                              (BTN_H - 16) / 2, ql[i].icon);
            else
                w2k_bigicon_draw(pm, ql_x + i * QL_BTN + 14, 4, ql[i].icon);
        }
        else
            w2k_icon_draw(pm, ql_x + i * QL_BTN + (QL_BTN - 16) / 2,
                          by + (BTN_H - 16) / 2, ql[i].icon);
    }
    if (!vert && w2k_theme == THEME_CLASSIC)
        draw_grip(pm, task_x - GRIP_W + 1, by + 2, BTN_H - 4);

    /* --- Task buttons ------------------------------------------------ */
    for (int i = 0; i < ntasks; i++) {
        Client *c = tasks[i].c;
        int x = tasks[i].x, w = tasks[i].w;
        int by = tasks[i].y, bh = tasks[i].h;   /* a column's buttons differ */
        int active = (c == focused && !c->minimized);

        if (w2k_theme != THEME_CLASSIC) {
            /* Luna's task buttons are rounded gradients, and the label on
             * them is white -- there is no raised edge anywhere. */
            int state = active ? W2K_TB_DOWN
                      : (i == hover_task ? W2K_TB_HOT : W2K_TB_NORMAL);
            w2k_theme_taskbutton(pm, x, by, w, bh, state, w2k_theme);
        } else if (active) {
            /* A depressed task button gets the classic 50% dither. */
            w2k_edge(pm, x, by, w, bh, EDGE_SUNKEN, BF_RECT);
            w2k_dither(pm, x + 2, by + 2, w - 4, bh - 4, C_HILIGHT, C_FACE);
        } else {
            w2k_button(pm, x, by, w, bh, 0);
        }
        if (w2k_theme == THEME_BASIC7) {
            int isz = w2k_taskbar_small ? 16 : 32;
            if (!w2k_taskbar_labels) {
                /* Combined: the icon alone, centred. */
                if (w >= isz) {
                    int ix = x + (w - isz) / 2, iy = by + (bh - isz) / 2;
                    if (isz == 32) w2k_bigicon_draw(pm, ix, iy, c->icon);
                    else           w2k_icon_draw(pm, ix, iy, c->icon);
                }
                continue;
            }
            /* Never combine: icon at the left, the title beside it. */
            int ix = x + 6, iy = by + (bh - isz) / 2;
            if (isz == 32) w2k_bigicon_draw(pm, ix, iy, c->icon);
            else           w2k_icon_draw(pm, ix, iy, c->icon);
            int tx = ix + isz + 6, avail = x + w - 6 - tx;
            if (avail > 6) {
                char buf[160];
                w2k_ellipsis(F_UI, c->name, avail, buf, sizeof buf);
                w2k_text_rgb(pm, F_UI, tx, by + (bh - w2k_font_height(F_UI)) / 2,
                             buf, 0, 0, 0);
            }
            continue;
        }
        int o = active ? 1 : 0;
        int ix = x + 4 + o, tx = ix + 20;
        if (w > 26) w2k_icon_draw(pm, ix, by + (bh - 16) / 2 + o, c->icon);
        int avail = x + w - 4 - tx;
        if (avail > 6) {
            char buf[160];
            w2k_ellipsis(F_UI, c->name, avail, buf, sizeof buf);
            int ty = by + (bh - w2k_font_height(F_UI)) / 2 + o;
            if (w2k_theme == THEME_CLASSIC)
                w2k_text(pm, F_UI, tx, ty, buf, C_TEXT);
            else
                w2k_text_rgb(pm, F_UI, tx, ty, buf, 255, 255, 255);
        }
    }

    /* --- Tray: docked icons, the speaker, then the clock -------------- */
    if (vert) {
        int wx = TB_PAD, ww = tb_w - 2 * TB_PAD;
        int wy = notify_y - 4, wh = tb_h - TB_PAD - wy;
        if (w2k_theme == THEME_CLASSIC)
            w2k_edge(pm, wx, wy, ww, wh, EDGE_SUNKEN_THIN, BF_RECT);
        else if (w2k_theme != THEME_BASIC7)
            w2k_theme_taskbutton(pm, wx, wy, ww, wh, W2K_TB_DOWN, w2k_theme);
        volume_draw(pm, TB_PAD + (ww - 16) / 2, vol_y);
        if (w2k_taskbar_showclock) {
            int tw = w2k_text_width(F_UI, clock_text, -1);
            int cx = TB_PAD + (ww - tw) / 2, cy = tray_y + (BTN_H - w2k_font_height(F_UI)) / 2;
            if (w2k_theme == THEME_CLASSIC) w2k_text(pm, F_UI, cx, cy, clock_text, C_TEXT);
            else if (w2k_theme == THEME_BASIC7) w2k_text_rgb(pm, F_UI, cx, cy, clock_text, 0, 0, 0);
            else w2k_text_rgb(pm, F_UI, cx, cy, clock_text, 255, 255, 255);
        }
        return;
    }
    int well_x = notify_x - 4;
    if (w2k_theme == THEME_CLASSIC) {
        w2k_edge(pm, well_x, by, tb_w - TB_PAD - well_x, BTN_H,
                 EDGE_SUNKEN_THIN, BF_RECT);
    } else if (w2k_theme != THEME_BASIC7) {   /* Windows 7's is in the bar skin */
        /* Luna's notification area is a darker inset panel with a light
         * line down its left edge. */
        int tw = tb_w - TB_PAD - well_x;
        w2k_theme_taskbutton(pm, well_x, by, tw, BTN_H, W2K_TB_DOWN, w2k_theme);
    }
    volume_draw(pm, vol_x, by + (BTN_H - 16) / 2);
    if (w2k_taskbar_showclock && w2k_theme == THEME_BASIC7 && !w2k_taskbar_small) {
        /* Time over date, white and centred: the tops of the two lines
         * are at rows 6 and 21 of the bar in the screenshot. A small bar
         * has room for the time alone, drawn as the other themes do. */
        int cw = tray_w - 2 * TRAY_PAD;
        int tw = w2k_text_width(F_UI, clock_text, -1);
        int dw = w2k_text_width(F_UI, clock_date, -1);
        w2k_text_rgb(pm, F_UI, tray_x + TRAY_PAD + (cw - tw) / 2, 6,
                     clock_text, 0, 0, 0);
        w2k_text_rgb(pm, F_UI, tray_x + TRAY_PAD + (cw - dw) / 2, 21,
                     clock_date, 0, 0, 0);
    } else if (w2k_taskbar_showclock) {
        int cy = by + (BTN_H - w2k_font_height(F_UI)) / 2;
        if (w2k_theme == THEME_CLASSIC)
            w2k_text(pm, F_UI, tray_x + TRAY_PAD, cy, clock_text, C_TEXT);
        else if (w2k_theme == THEME_BASIC7)
            w2k_text_rgb(pm, F_UI, tray_x + TRAY_PAD, cy, clock_text, 0, 0, 0);
        else
            w2k_text_rgb(pm, F_UI, tray_x + TRAY_PAD, cy, clock_text,
                         255, 255, 255);
    }

}

void taskbar_paint(void)
{
    if (!tb) return;
    layout();

    int h = tb_h;
    /* Kept between repaints: the bar redraws on every focus change, every
     * clock tick and every task appearing or going away. */
    static Pixmap pm;
    static int pm_w;
    static int pm_h;
    if (pm && (pm_w != tb_w || pm_h != h)) { w2k_free_pixmap(pm); pm = 0; }
    if (!pm) {
        pm = XCreatePixmap(w2k.dpy, tb, tb_w, h, w2k.depth);
        pm_w = tb_w;
        pm_h = h;
    }
    taskbar_draw(pm, h);
    XCopyArea(w2k.dpy, pm, tb, w2k.gc, 0, 0, tb_w, h, 0, 0);
}

/* W2K_RENDER aid: the bar as it would be painted `w` wide, to a PPM. */
int taskbar_render(const char *path, int w)
{
    tb_w = w;
    tb_h = TASKBAR_H;
    layout();
    /* W2K_RENDER_ORB=n paints the orb part way through its glow. */
    if (getenv("W2K_RENDER_ORB") && theme_start_skin() && orb_frames[0]) {
        orb_frame = atoi(getenv("W2K_RENDER_ORB"));
        if (orb_frame < 0) orb_frame = 0;
        if (orb_frame >= ORB_FRAMES) orb_frame = ORB_FRAMES - 1;
    }
    Pixmap pm = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)w, (unsigned)tb_h,
                              w2k.depth);
    taskbar_draw(pm, tb_h);
    XImage *im = XGetImage(w2k.dpy, pm, 0, 0, (unsigned)w, (unsigned)tb_h,
                           AllPlanes, ZPixmap);
    FILE *f = fopen(path, "wb");
    if (f && im) {
        fprintf(f, "P6\n%d %d\n255\n", w, tb_h);
        for (int y = 0; y < tb_h; y++)
            for (int x = 0; x < w; x++) {
                unsigned long v = XGetPixel(im, x, y);
                unsigned char rgb[3] = { (v >> 16) & 0xff, (v >> 8) & 0xff,
                                         v & 0xff };
                fwrite(rgb, 1, 3, f);
            }
    }
    if (f) fclose(f);
    if (im) XDestroyImage(im);
    w2k_free_pixmap(pm);
    return 1;
}

void taskbar_sync(void) { taskbar_paint(); }

/* Polling the mixer means running pactl, which means forking a shell.
 * Five seconds is often enough to notice that something else changed the
 * level -- once a second, which is what this used to do, is sixty
 * processes a minute for an idle desktop. */
#define VOLUME_POLL_MS 5000

static long next_volume_poll;

void taskbar_tick(void)
{
    char old[32];
    memcpy(old, clock_text, sizeof old);
    update_clock();

    long now = w2k_now_ms();
    int vol_before = volume_level(), mute_before = volume_is_muted();
    if (now >= next_volume_poll && volume_available()) {
        next_volume_poll = now + VOLUME_POLL_MS;
        volume_poll();
    }
    if (strcmp(old, clock_text) || vol_before != volume_level() ||
        mute_before != volume_is_muted())
        taskbar_paint();
}

/* How long the shell can sleep before something on the bar needs
 * attention: the clock at the next minute, the mixer at its next poll,
 * and a tooltip half a second after the pointer stopped on a button. */
int taskbar_next_tick_ms(void)
{
    long now = w2k_now_ms();
    int wait = 60000;
    if (volume_available())
        wait = (int)(next_volume_poll > now ? next_volume_poll - now : 0);

    if (w2k_taskbar_showclock) {
        /* The clock shows minutes, so it only has to wake for one. */
        int secs = (int)(time(NULL) % 60);
        int to_minute = (60 - secs) * 1000 + 50;
        if (to_minute < wait) wait = to_minute;
    }
    if (hover_task >= 0 && hover_since && !tip_up) {
        int left = 500 - (int)(now - hover_since);
        if (left < 0) left = 0;
        if (left < wait) wait = left;
    }
    return wait;
}

/* ------------------------------------------------------------------ *
 * Setup
 * ------------------------------------------------------------------ */
/* Auto-hide: the bar sits just off the bottom of its monitor with a couple
 * of pixels showing, and a one-pixel strip along the very bottom edge
 * catches the pointer and brings it back. */
#define TB_PEEK 2

/* A one-pixel window along the bottom edge, purely to notice the pointer. */
static void taskbar_trigger_place(void)
{
    const W2kMonitor *m = w2k_monitor_primary();
    if (!w2k_taskbar_autohide) {
        if (tb_trigger) { XDestroyWindow(w2k.dpy, tb_trigger); tb_trigger = 0; }
        return;
    }
    /* A one-pixel line along whichever edge the bar hides against. */
    int x = m->x, y = m->y, w = m->w, h = 1;
    switch (w2k_taskbar_edge) {
    case TB_TOP:   break;
    case TB_LEFT:  w = 1; h = m->h; break;
    case TB_RIGHT: x = m->x + m->w - 1; w = 1; h = m->h; break;
    default:       y = m->y + m->h - 1; break;
    }

    if (!tb_trigger) {
        XSetWindowAttributes a = {
            .override_redirect = True,
            .event_mask = EnterWindowMask
        };
        tb_trigger = XCreateWindow(w2k.dpy, w2k.root, x, y, w, h, 0,
                                   CopyFromParent, InputOnly, CopyFromParent,
                                   CWOverrideRedirect | CWEventMask, &a);
        XMapWindow(w2k.dpy, tb_trigger);
    } else {
        XMoveResizeWindow(w2k.dpy, tb_trigger, x, y, w, h);
    }
    XRaiseWindow(w2k.dpy, tb_trigger);
}

/* Where the bar sits when hidden: just off its own edge, with a sliver
 * showing so it can still be clicked. */
static void taskbar_place(void)
{
    const W2kMonitor *m = w2k_monitor_primary();
    int hidden = w2k_taskbar_autohide && !tb_shown;
    int t = taskbar_thickness();
    int x = tb_x, y = tb_y;

    switch (w2k_taskbar_edge) {
    case TB_TOP:   y = hidden ? m->y - t + TB_PEEK : m->y; break;
    case TB_LEFT:  x = hidden ? m->x - t + TB_PEEK : m->x; break;
    case TB_RIGHT: x = hidden ? m->x + m->w - TB_PEEK : m->x + m->w - t; break;
    default:       y = hidden ? m->y + m->h - TB_PEEK : m->y + m->h - t; break;
    }
    if (tb) XMoveWindow(w2k.dpy, tb, x, y);
    tb_x = x;
    tb_y = y;
}

void taskbar_reveal(int show)
{
    if (!w2k_taskbar_autohide) { tb_shown = 1; taskbar_place(); return; }
    if (tb_shown == show) return;
    tb_shown = show;
    taskbar_place();
    if (show) XRaiseWindow(w2k.dpy, tb);
}

void taskbar_relayout(void)
{
    taskbar_init();
    wm_update_workarea();
    clients_restack();
    desktop_init();
    w2k_scheme_save(NULL);
    w2k_scheme_broadcast();
}


void taskbar_init(void)
{
    /* The bar belongs to the primary monitor and spans exactly that monitor:
     * stretching it across the whole virtual screen is what made it useless
     * on a multi-head desktop -- the clock ended up on the far right-hand
     * panel and the task buttons were kilometres wide. */
    const W2kMonitor *m = w2k_monitor_primary();
    taskbar_geometry(&tb_x, &tb_y, &tb_w, &tb_h);
    (void)m;

    if (tb) {
        XMoveResizeWindow(w2k.dpy, tb, tb_x, tb_y, tb_w, tb_h);
        if (w2k_taskbar_ontop) XRaiseWindow(w2k.dpy, tb);
        taskbar_place();
        taskbar_trigger_place();
        taskbar_paint();
        return;
    }
    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel  = w2k.col[C_FACE],
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | EnterWindowMask | LeaveWindowMask
    };
    tb = XCreateWindow(w2k.dpy, w2k.root, tb_x, tb_y, tb_w, tb_h, 0,
                       CopyFromParent, InputOutput, CopyFromParent,
                       CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    XMapRaised(w2k.dpy, tb);
    /* Files can be dropped on the Start button and Quick Launch. */
    w2k_dnd_accept(tb);
    taskbar_trigger_place();
    tray_init(tb);              /* applications can dock from now on */
    volume_poll();
    taskbar_paint();
}

/* ------------------------------------------------------------------ *
 * Interaction
 * ------------------------------------------------------------------ */
enum { TM_RESTORE = 1, TM_MOVE, TM_SIZE, TM_MIN, TM_MAX, TM_CLOSE,
       TM_PIN_TB, TM_UNPIN_TB, TM_PIN_SM, TM_UNPIN_SM,
       TB_CASCADE = 20, TB_TILEH, TB_TILEV, TB_MINALL, TB_TASKMGR, TB_PROPS,
       QL_UNPIN = 40, QL_OPEN, QL_RENAME, QL_ICON,
       TB_EDGE_BOTTOM = 50, TB_EDGE_TOP, TB_EDGE_LEFT, TB_EDGE_RIGHT,
       TB_ROWS1 = 60, TB_ROWS2, TB_ROWS3, TB_ROWS4,
       TB_TB_QUICKLAUNCH = 70, TB_TB_DESKTOP, TB_TB_ADDRESS };

static void task_context_menu(Client *c, int x, int y)
{
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, TM_RESTORE, "&Restore", NULL, ICO_NONE);
    if (!c->maximized && !c->minimized) w2k_menu_disable(m);
    w2k_menu_item(m, TM_MOVE, "&Move", NULL, ICO_NONE);
    if (c->maximized || c->minimized) w2k_menu_disable(m);
    w2k_menu_item(m, TM_SIZE, "&Size", NULL, ICO_NONE);
    if (c->maximized || c->minimized || !c->resizable) w2k_menu_disable(m);
    w2k_menu_item(m, TM_MIN, "Mi&nimize", NULL, ICO_NONE);
    if (c->minimized) w2k_menu_disable(m);
    w2k_menu_item(m, TM_MAX, "Ma&ximize", NULL, ICO_NONE);
    if (c->maximized || !c->resizable) w2k_menu_disable(m);
    w2k_menu_sep(m);

    /* Pinning needs a command that would start this program again. */
    char cmd[512], label[128], icon[256];
    pin_command_for_client(c, cmd, sizeof cmd, label, sizeof label,
                           icon, sizeof icon);
    int on_tb = pins_contains(PIN_TASKBAR, cmd);
    int on_sm = pins_contains(PIN_START, cmd);
    w2k_menu_item(m, on_tb ? TM_UNPIN_TB : TM_PIN_TB,
                  on_tb ? "Unpin from Tas&kbar" : "Pin to Tas&kbar",
                  NULL, ICO_NONE);
    if (!cmd[0]) w2k_menu_disable(m);
    w2k_menu_item(m, on_sm ? TM_UNPIN_SM : TM_PIN_SM,
                  on_sm ? "Unpin from Start Men&u" : "Pin to Start Men&u",
                  NULL, ICO_STARTFLAG);
    if (!cmd[0]) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, TM_CLOSE, "&Close", "Alt+F4", ICO_NONE);

    Window cw = c->win;
    int id = w2k_menu_popup(m, x, y, MPOP_BOTTOMUP);
    w2k_menu_free(m);
    if (client_find(cw) != c) return;      /* closed while the menu was up */

    switch (id) {
    case TM_RESTORE: client_restore(c); client_maximize(c, 0); break;
    case TM_MIN:     client_minimize(c); break;
    case TM_MAX:     client_restore(c); client_maximize(c, 1); break;
    case TM_CLOSE:   client_close(c); break;
    case TM_MOVE: case TM_SIZE:
        client_restore(c);
        sysmenu_popup(c, c->x, c->y);
        break;
    case TM_PIN_TB:
        pins_add(PIN_TASKBAR, cmd, label, icon);
        taskbar_paint();
        break;
    case TM_UNPIN_TB: pins_remove(PIN_TASKBAR, cmd); taskbar_paint(); break;
    case TM_PIN_SM:   pins_add(PIN_START, cmd, label, icon); break;
    case TM_UNPIN_SM: pins_remove(PIN_START, cmd);           break;
    }
}

/* Cascade / tile, straight off the Windows taskbar context menu. */
/* Cascade and tile work one monitor at a time: windows stay on the screen
 * the user put them on, and each screen gets a full arrangement of its own. */
static void arrange_on(int how, const W2kMonitor *mon)
{
    int ax, ay, aw, ah;
    wm_workarea_of(mon, &ax, &ay, &aw, &ah);

    Client *list[64];
    int n = 0;
    for (Client *c = clients; c && n < 64; c = c->next) {
        if (c->skip_taskbar || c->minimized) continue;
        if (w2k_monitor_at(c->x + c->w / 2, c->y + c->h / 2) != mon) continue;
        list[n++] = c;
    }
    if (!n) return;

    if (how == TB_CASCADE) {
        int step = CAPTION_H + FRAME_SIZE;
        for (int i = n - 1, k = 0; i >= 0; i--, k++) {
            Client *c = list[i];
            client_maximize(c, 0);
            int b = client_border(c), cap = client_caption_h(c);
            int fx = ax + 4 + k * step, fy = ay + 4 + k * step;
            int fw = aw * 2 / 3, fh = ah * 2 / 3;
            if (fy + fh > ay + ah) { fy = ay + 4; fx = ax + 4; k = 0; }
            client_move_resize(c, fx + b, fy + b + cap,
                               fw - 2 * b, fh - 2 * b - cap);
            client_raise(c);
        }
    } else if (how == TB_TILEH || how == TB_TILEV) {
        int cols = (how == TB_TILEV) ? n : 1;
        int rows = (how == TB_TILEV) ? 1 : n;
        if (n > 3) {                       /* fall back to a grid */
            cols = 1;
            while (cols * cols < n) cols++;
            rows = (n + cols - 1) / cols;
        }
        int cw = aw / cols, chh = ah / rows;
        for (int i = 0; i < n; i++) {
            Client *c = list[i];
            client_maximize(c, 0);
            int b = client_border(c), cap = client_caption_h(c);
            int fx = ax + (i % cols) * cw, fy = ay + (i / cols) * chh;
            client_move_resize(c, fx + b, fy + b + cap,
                               cw - 2 * b, chh - 2 * b - cap);
        }
    } else if (how == TB_MINALL) {
        for (int i = 0; i < n; i++) client_minimize(list[i]);
    }
}

static void arrange(int how)
{
    for (int i = 0; i < w2k_monitor_count(); i++)
        arrange_on(how, w2k_monitor(i));
}

static void taskbar_context_menu(int x, int y)
{
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, TB_CASCADE, "Casca&de Windows", NULL, ICO_NONE);
    w2k_menu_item(m, TB_TILEH, "Tile Windows &Horizontally", NULL, ICO_NONE);
    w2k_menu_item(m, TB_TILEV, "Tile Windows V&ertically", NULL, ICO_NONE);
    w2k_menu_item(m, TB_MINALL, "&Minimize All Windows", NULL, ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_item(m, TB_TASKMGR, "&Task Manager", NULL, ICO_TASKMGR);
    w2k_menu_sep(m);
    /* Toolbars, as in Windows: what appears alongside the task buttons. */
    W2kMenu *bars = w2k_menu_new();
    w2k_menu_item(bars, TB_TB_QUICKLAUNCH, "&Quick Launch", NULL, ICO_NONE);
    w2k_menu_check(bars, w2k_taskbar_quicklaunch);
    w2k_menu_item(bars, TB_TB_DESKTOP, "&Desktop", NULL, ICO_DESKTOP);
    w2k_menu_disable(bars);
    w2k_menu_item(bars, TB_TB_ADDRESS, "&Address", NULL, ICO_NONE);
    w2k_menu_disable(bars);
    w2k_menu_sub(m, "Tool&bars", ICO_NONE, bars);

    /* Which edge, and how tall. Windows does both by dragging; a menu is
     * how you do it when the bar is locked, and is easier to hit. */
    W2kMenu *edge = w2k_menu_new();
    w2k_menu_item(edge, TB_EDGE_BOTTOM, "&Bottom", NULL, ICO_NONE);
    w2k_menu_radio(edge, w2k_taskbar_edge == TB_BOTTOM);
    w2k_menu_item(edge, TB_EDGE_TOP, "&Top", NULL, ICO_NONE);
    w2k_menu_radio(edge, w2k_taskbar_edge == TB_TOP);
    w2k_menu_item(edge, TB_EDGE_LEFT, "&Left", NULL, ICO_NONE);
    w2k_menu_radio(edge, w2k_taskbar_edge == TB_LEFT);
    w2k_menu_item(edge, TB_EDGE_RIGHT, "&Right", NULL, ICO_NONE);
    w2k_menu_radio(edge, w2k_taskbar_edge == TB_RIGHT);
    w2k_menu_sub(m, "&Position", ICO_NONE, edge);

    W2kMenu *rows = w2k_menu_new();
    static const char *row_labels[4] = { "&1 row", "&2 rows", "&3 rows", "&4 rows" };
    for (int r = 0; r < 4; r++) {
        w2k_menu_item(rows, TB_ROWS1 + r, row_labels[r], NULL, ICO_NONE);
        w2k_menu_radio(rows, w2k_taskbar_rows == r + 1);
    }
    w2k_menu_sub(m, "&Size", ICO_NONE, rows);
    w2k_menu_sep(m);

    w2k_menu_item(m, TB_PROPS, "P&roperties", NULL, ICO_NONE);

    int id = w2k_menu_popup(m, x, y, MPOP_BOTTOMUP);
    w2k_menu_free(m);

    if (id == TB_TASKMGR)    wm_spawn("w2ktaskmgr");
    else if (id == TB_PROPS) wm_startmenu_dialog();
    else if (id >= TB_EDGE_BOTTOM && id <= TB_EDGE_RIGHT) {
        w2k_taskbar_edge = id - TB_EDGE_BOTTOM;
        taskbar_relayout();
    } else if (id >= TB_ROWS1 && id <= TB_ROWS4) {
        w2k_taskbar_rows = id - TB_ROWS1 + 1;
        taskbar_relayout();
    } else if (id == TB_TB_QUICKLAUNCH) {
        w2k_taskbar_quicklaunch = !w2k_taskbar_quicklaunch;
        taskbar_relayout();
    } else if (id) arrange(id);
}

static void show_desktop_toggle(void)
{
    int any = 0;
    for (Client *c = clients; c; c = c->next)
        if (!c->minimized && !c->skip_taskbar) { any = 1; break; }
    for (Client *c = clients; c; c = c->next) {
        if (c->skip_taskbar) continue;
        if (any) client_minimize(c);
        else if (c->minimized) client_restore(c);
    }
}

/* Called from the main loop: put a tooltip up once the pointer has been
 * still over a button for long enough, and take it down again. */
void taskbar_hover_tick(void)
{
    if (hover_task < 0 || tip_up || !hover_since) return;
    if (w2k_now_ms() - hover_since < 500) return;
    if (hover_task >= ntasks || !tasks[hover_task].c) return;

    Window r, ch;
    int rx, ry, wx, wy;
    unsigned mask;
    if (!XQueryPointer(w2k.dpy, w2k.root, &r, &ch, &rx, &ry, &wx, &wy, &mask))
        return;
    w2k_tooltip_show(tasks[hover_task].c->name, rx + 12, ry + 20);
    tip_up = 1;
}

static void hover_clear(void)
{
    hover_task = -1;
    hover_since = 0;
    if (tip_up) { w2k_tooltip_hide(); tip_up = 0; }
}

int taskbar_event(XEvent *e)
{
    if (!tb) return 0;
    if (w2k_tooltip_event(e)) return 1;

    /* The clock's tooltip is the full date, as in Windows. */
    if (e->type == MotionNotify && e->xmotion.window == tb &&
        w2k_taskbar_showclock && drag_task < 0 &&
        (vertical() ? e->xmotion.y >= tray_y : e->xmotion.x >= tray_x)) {
        if (hover_task >= 0) hover_clear();      /* off a button, on to the clock */
        if (!tip_up) {
            time_t t = time(NULL);
            struct tm tm;
            char date[96];
            localtime_r(&t, &tm);
            strftime(date, sizeof date, "%A, %d %B %Y", &tm);
            w2k_tooltip_show(date, e->xmotion.x_root + 12, e->xmotion.y_root - 34);
            tip_up = 1;
        }
        return 1;
    }

    /* Hovering a task button, and dragging one to a new position. */
    if (e->type == MotionNotify && e->xmotion.window == tb) {
        /* The themed Start button has a hot state of its own. */
        int over_start = start_button_hit(e->xmotion.x, e->xmotion.y);
        if (over_start != start_hot) {
            start_hot = over_start;
            start_hot_changed();
        }
        int mx = e->xmotion.x, my = e->xmotion.y;
        int over = -1;
        for (int i = 0; i < ntasks; i++)
            if (mx >= tasks[i].x && mx < tasks[i].x + tasks[i].w &&
                my >= tasks[i].y && my < tasks[i].y + tasks[i].h) {
                over = i;
                break;
            }
        if (drag_task >= 0 && over >= 0 && over != drag_task) {
            /* Swap the sort keys: the buttons change places under the
             * pointer and stay that way. */
            long a = tasks[drag_task].c->tb_order;
            tasks[drag_task].c->tb_order = tasks[over].c->tb_order;
            tasks[over].c->tb_order = a;
            drag_task = over;
            taskbar_paint();
            return 1;
        }
        if (over != hover_task) {
            hover_clear();
            hover_task = over;
            hover_since = over >= 0 ? w2k_now_ms() : 0;
        }
        return 1;
    }
    if (e->type == LeaveNotify && e->xcrossing.window == tb) {
        hover_clear();
        if (start_hot) { start_hot = 0; start_hot_changed(); }
    }
    if (e->type == ButtonRelease && drag_task >= 0) { drag_task = -1; return 1; }
    if (e->type == ButtonPress) hover_clear();

    if (w2k_taskbar_autohide) {
        if (e->type == EnterNotify && e->xcrossing.window == tb_trigger) {
            taskbar_reveal(1);
            return 1;
        }
        if (e->type == LeaveNotify && e->xcrossing.window == tb &&
            e->xcrossing.detail != NotifyInferior) {
            taskbar_reveal(0);
            return 1;
        }
    }

    if (e->type == Expose && e->xexpose.window == tb) {
        if (e->xexpose.count == 0) taskbar_paint();
        return 1;
    }
    if (e->type != ButtonPress || e->xbutton.window != tb) return 0;

    int x = e->xbutton.x, y = e->xbutton.y;
    int by = BTN_TOP;

    if (e->xbutton.button == Button3) {
        if (vertical() ? (y >= vol_y - 2 && y < vol_y + 18)
                       : (x >= vol_x - 2 && x < vol_x + 18)) {
            W2kMenu *m = w2k_menu_new();
            w2k_menu_item(m, 1, volume_is_muted() ? "&Unmute" : "&Mute",
                          NULL, ICO_NONE);
            w2k_menu_item(m, 2, "Open Volume &Control", NULL, ICO_NONE);
            int id = w2k_menu_popup(m, e->xbutton.x_root, tb_y, MPOP_BOTTOMUP);
            w2k_menu_free(m);
            if (id == 1) { volume_toggle_mute(); taskbar_paint(); }
            else if (id == 2) {
                char cmd[400];
                snprintf(cmd, sizeof cmd, "pavucontrol || %s -e alsamixer",
                         wm_terminal_cmd() ? wm_terminal_cmd() : "xterm");
                wm_spawn(cmd);
            }
            return 1;
        }
        /* Right-clicking the Start button goes straight to its settings. */
        if (x >= TB_PAD && x < TB_PAD + start_w && y >= by && y < by + BTN_H) {
            wm_startmenu_dialog();
            return 1;
        }
        for (int i = 0; i < NQL; i++) {
            if (!ql[i].cmd[0]) continue;         /* Show Desktop has no menu */
            int qx = vertical() ? TB_PAD : ql_x + i * QL_BTN;
            int qy = vertical() ? ql_y + i * QL_BTN : 0;
            if (vertical()) { if (y < qy || y >= qy + QL_BTN) continue; }
            else if (x < qx || x >= qx + QL_BTN) continue;
            W2kMenu *m = w2k_menu_new();
            w2k_menu_item(m, QL_OPEN, "&Open", NULL, ICO_NONE);
            w2k_menu_default(m);
            w2k_menu_sep(m);
            w2k_menu_item(m, QL_RENAME, "Rena&me...", NULL, ICO_NONE);
            w2k_menu_item(m, QL_ICON, "Change &Icon...", NULL, ICO_NONE);
            w2k_menu_sep(m);
            char item[160];
            snprintf(item, sizeof item, "&Unpin \"%.100s\"", ql[i].tip);
            w2k_menu_item(m, QL_UNPIN, item, NULL, ICO_DELETE);
            int id = w2k_menu_popup(m, e->xbutton.x_root, tb_y, MPOP_BOTTOMUP);
            w2k_menu_free(m);
            /* ql[] is rebuilt by taskbar_paint(), so the command and label
             * are copied out before anything can move them. */
            char cmd[512], label[128];
            snprintf(cmd, sizeof cmd, "%s", ql[i].cmd);
            snprintf(label, sizeof label, "%s", ql[i].tip);
            switch (id) {
            case QL_OPEN:
                if (cmd[0]) wm_spawn(cmd);
                break;
            case QL_RENAME: {
                char name[128];
                if (w2k_prompt(NULL, "Rename", "&Name:", label, name,
                               sizeof name, ICO_NONE) && name[0]) {
                    pins_rename(PIN_TASKBAR, cmd, name);
                    taskbar_paint();
                }
                break;
            }
            case QL_ICON: {
                char icon[256];
                if (wm_change_icon_dialog(icon, sizeof icon)) {
                    pins_set_icon(PIN_TASKBAR, cmd, icon);
                    taskbar_paint();
                }
                break;
            }
            case QL_UNPIN:
                pins_remove(PIN_TASKBAR, cmd);
                taskbar_paint();
                break;
            }
            return 1;
        }
        for (int i = 0; i < ntasks; i++)
            if (x >= tasks[i].x && x < tasks[i].x + tasks[i].w &&
                y >= tasks[i].y && y < tasks[i].y + tasks[i].h) {
                task_context_menu(tasks[i].c, e->xbutton.x_root, tb_y);
                return 1;
            }
        taskbar_context_menu(e->xbutton.x_root, tb_y);
        return 1;
    }
    if (e->xbutton.button != Button1) return 1;

    if (!vertical() && w2k_taskbar_showclock && x >= tray_x &&
        !(w2k_theme == THEME_BASIC7 && x >= tb_w - W7_SLIVER)) {
        /* Double-clicking the clock opens Date/Time Properties. */
        static long last_clock_click;
        long now = w2k_now_ms();
        if (last_clock_click && now - last_clock_click < w2k_dblclk_ms) {
            wm_spawn("w2kcontrol datetime");
            last_clock_click = 0;
        } else last_clock_click = now;
        return 1;
    }
    if (w2k_theme == THEME_BASIC7 && !vertical() && x >= tb_w - W7_SLIVER) {
        show_desktop_toggle();            /* the sliver at the bar's end */
        return 1;
    }
    int sby = vertical() ? TB_PAD : by;
    if (x >= TB_PAD && x < TB_PAD + start_w && y >= sby && y < sby + BTN_H) {
        if (startmenu_is_open()) startmenu_close();
        else                     startmenu_open();
        return 1;
    }
    for (int i = 0; i < NQL; i++) {
        int qx = vertical() ? TB_PAD : ql_x + i * QL_BTN;
        int qy = vertical() ? ql_y + i * QL_BTN : by;
        if (x >= qx && x < qx + (vertical() ? start_w : QL_BTN) &&
            y >= qy && y < qy + (vertical() ? QL_BTN : BTN_H)) {
            if (ql[i].cmd[0]) wm_spawn(ql[i].cmd);
            else              show_desktop_toggle();
            return 1;
        }
    }
    if (vertical() ? (y >= vol_y - 2 && y < vol_y + 18)
                   : (x >= vol_x - 2 && x < vol_x + 18)) {
        volume_popup(vertical() ? tb_x + tb_w : tb_x + vol_x,
                     vertical() ? tb_y + vol_y : tb_y);
        return 1;
    }
    for (int i = 0; i < ntasks; i++) {
        if (x < tasks[i].x || x >= tasks[i].x + tasks[i].w) continue;
        if (y < tasks[i].y || y >= tasks[i].y + tasks[i].h) continue;
        Client *c = tasks[i].c;
        drag_task = i;              /* may turn into a reorder drag */
        /* Clicking the active window's button minimises it, as in Windows. */
        if (c == focused && !c->minimized) client_minimize(c);
        else                               client_restore(c);
        return 1;
    }
    return 1;
}
