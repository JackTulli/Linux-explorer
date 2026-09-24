/* l2kcontrol.c -- Control Panel.
 *
 * A folder of applets, opened by double-clicking, plus the one applet that
 * lives here rather than in its own program: Default Programs, which sets
 * what opens pictures, video, music and the rest (see lib/assoc.c). */
#include "w2k.h"
#include "w2kui.h"
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <strings.h>
#include <fontconfig/fontconfig.h>

#define STATUS_H 20

enum { ID_OPEN = 1, ID_CLOSE, ID_ABOUT };

/* Which preset the radio buttons name, in the Performance Options dialog. */
enum { PRESET_AUTO, PRESET_APPEARANCE, PRESET_PERFORMANCE, PRESET_CUSTOM };

/* An applet either runs a program or opens a pane of our own. Each row
 * names its pane, so the table can be reordered or added to without
 * anything else knowing its place -- an index list once drifted from it
 * and Folder Options opened Fonts. */
static void open_defaults(void);
static void open_performance(void);
static void open_folder_options(void);
static void open_mouse(void);
static void open_keyboard(void);
static void open_sounds(void);
static void open_fonts(void);
static void open_datetime(void);
static void open_power(void);
static void open_users(void);
static void open_logon(void);
/* System Properties and Disk Cleanup live in the toolkit, so Explorer
 * can open a drive's sheet itself; these open them from here. */
static void open_system(void) { w2k_system_properties(NULL); }
static void open_cleanup(void) { w2k_disk_cleanup(NULL, NULL, NULL); }

typedef struct {
    const char *name;
    const char *desc;
    int         icon;
    const char *cmd;                 /* a program to run, or NULL */
    void      (*open)(void);         /* a pane of our own, or NULL */
} Applet;

static const Applet applets[] = {
    /* Alphabetical, as the shell lists them; the descriptions are the
     * ones Windows 2000 shows in the web-view pane. */
    { "Bluetooth Devices", "Adds, removes and connects Bluetooth devices -- phones, headsets, keyboards and mice -- and sets whether other devices can find this computer.",
      ICO_CP_BLUETOOTH, "l2kbluetooth", NULL },
    { "Date/Time", "Set the date, time and time zone for your computer.",
      ICO_CP_DATETIME, NULL, open_datetime },
    { "Default Programs", "Choose which programs open which kinds of files.",
      ICO_PROGRAMS, NULL, open_defaults },
    { "Device Manager", "Shows the hardware installed in this computer and lets you change its drivers.",
      ICO_MYCOMPUTER, "l2kdevmgmt", NULL },
    { "Disk Management", "Partitions and formats the disks in this computer, and mounts their volumes.",
      ICO_DRIVE_HDD, "l2kdiskmgmt", NULL },
    { "Display", "Customize your desktop display and screen saver.",
      ICO_CP_DISPLAY, "l2kdisplay", NULL },
    { "Folder Options", "Customizes the display of files and folders, changes file associations, and makes network files available offline.",
      ICO_CP_FOLDEROPTS, NULL, open_folder_options },
    { "Fonts", "Displays and manages fonts on your computer.",
      ICO_FONTS_FOLDER, NULL, open_fonts },
    { "Keyboard", "Customizes your keyboard settings.",
      ICO_CP_KEYBOARD, NULL, open_keyboard },
    { "Logon Screen", "Chooses the artwork, the background and the picture the logon screen shows.",
      ICO_LOGOFF, NULL, open_logon },
    { "Mouse", "Customizes your mouse settings.",
      ICO_CP_MOUSE, NULL, open_mouse },
    { "Network and Dial-up Connections", "Connects to other computers, networks, and the Internet.",
      ICO_CP_NETWORK, "l2knetwork", NULL },
    { "Performance Options", "Chooses the visual effects the desktop uses -- smooth icons, menu shadows, animation -- and weighs looks against speed.",
      ICO_SETTINGS, NULL, open_performance },
    { "Power Options", "Configures energy-saving settings for your computer.",
      ICO_CP_POWER, NULL, open_power },
    { "Proton Manager", "Downloads versions of Proton, and chooses which runs your Windows programs.",
      ICO_PROTON, "l2kproton", NULL },
    { "Sounds and Multimedia", "Assigns sounds to events and configures sound devices.",
      ICO_CP_SOUNDS, NULL, open_sounds },
    { "System", "Provides system information and changes environment settings.",
      ICO_CP_SYSTEM, NULL, open_system },
    { "Task Manager", "Shows the programs and processes running on your computer.",
      ICO_TASKMGR, "l2ktaskmgr", NULL },
    { "Taskbar and Start Menu", "Customizes the Start Menu and the taskbar.",
      ICO_TASKBAR, "@startmenu", NULL },      /* @ = ask the shell, not a program */
    { "User Accounts", "Changes the name and picture the Start menu shows for you.",
      ICO_CP_USERS, NULL, open_users },
};
#define NAPPLETS ((int)(sizeof applets / sizeof *applets))

static struct {
    W2kWin       *win;
    W2kFolderWin *fw;
} cp;

/* ------------------------------------------------------------------ *
 * Default Programs
 * ------------------------------------------------------------------ */
typedef struct {
    W2kEdit *edit[8];
    W2kRect  browse[8];
    W2kRect  ok, cancel;
    int      n, down;
} DefDlg;

static void blink(void *v) { w2k_edit_blink(v); }

static void defaults_paint(W2kWin *w, Drawable d)
{
    DefDlg *dd = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_text(d, F_UI, 12, 12,
             "Choose the program that opens each kind of file:", C_TEXT);
    for (int i = 0; i < dd->n; i++) {
        w2k_text(d, F_UI, 12, dd->edit[i]->r.y + (21 - fh) / 2,
                 w2k_assoc_label_at(i), C_TEXT);
        w2k_edit_draw(d, dd->edit[i]);
        w2k_draw_pushbutton(d, &dd->browse[i], "...",
                            dd->down == 100 + i ? BS_PRESSED : 0);
    }
    w2k_text(d, F_UI, 12, w->h - 60,
             "%s in a command is replaced by the file name.", C_GRAYTEXT);
    w2k_draw_pushbutton(d, &dd->ok, "OK", BS_DEFAULT | (dd->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &dd->cancel, "Cancel", dd->down == 2 ? BS_PRESSED : 0);
}

/* OK: every command saved, then the box closes. */
static void defaults_ok(W2kWin *w, DefDlg *dd)
{
    for (int i = 0; i < dd->n; i++)
        w2k_assoc_set(w2k_assoc_class_at(i), w2k_edit_text(dd->edit[i]));
    w2k_assoc_apply_folder_default();
    w2k_win_close(w, ID_OK);
}

static int defaults_event(W2kWin *w, XEvent *e)
{
    DefDlg *dd = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        for (int i = 0; i < dd->n; i++) {
            if (w2k_edit_press(dd->edit[i], &e->xbutton)) {
                for (int k = 0; k < dd->n; k++)
                    if (k != i) dd->edit[k]->focused = 0;
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_rect_hit(&dd->browse[i], x, y)) dd->down = 100 + i;
        }
        if (w2k_rect_hit(&dd->ok, x, y)) dd->down = 1;
        else if (w2k_rect_hit(&dd->cancel, x, y)) dd->down = 2;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int d = dd->down, x = e->xbutton.x, y = e->xbutton.y;
        dd->down = 0;
        for (int i = 0; i < dd->n; i++) w2k_edit_release(dd->edit[i]);
        if (d == 1 && w2k_rect_hit(&dd->ok, x, y)) {
            defaults_ok(w, dd);
        } else if (d == 2 && w2k_rect_hit(&dd->cancel, x, y)) {
            w2k_win_close(w, ID_CANCEL);
        } else if (d >= 100 && d < 100 + dd->n &&
                   w2k_rect_hit(&dd->browse[d - 100], x, y)) {
            char path[1024] = "/usr/bin";
            if (w2k_file_dialog(w, 0, path, sizeof path))
                w2k_edit_set(dd->edit[d - 100], path);
        }
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        for (int i = 0; i < dd->n; i++)
            if (w2k_edit_motion(dd->edit[i], &e->xmotion)) {
                w2k_win_dirty(w);
                return 1;
            }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        /* OK is the default button: Enter pressed it in name only, and a
         * command typed in was saved only by a click on OK. */
        if (ks == XK_Return || ks == XK_KP_Enter) { defaults_ok(w, dd); return 1; }
        if ((ks == XK_Tab || ks == XK_ISO_Left_Tab) && dd->n) {
            int f = -1, back = (e->xkey.state & ShiftMask) || ks == XK_ISO_Left_Tab;
            for (int i = 0; i < dd->n; i++) if (dd->edit[i]->focused) f = i;
            for (int i = 0; i < dd->n; i++) dd->edit[i]->focused = 0;
            if (f < 0) f = back ? 0 : dd->n - 1;
            dd->edit[(f + (back ? dd->n - 1 : 1)) % dd->n]->focused = 1;
            w2k_win_dirty(w);
            return 1;
        }
        for (int i = 0; i < dd->n; i++)
            if (dd->edit[i]->focused && w2k_edit_key(dd->edit[i], &e->xkey)) {
                w2k_win_dirty(w);
                return 1;
            }
        return 1;
    }
    }
    return 0;
}

static void open_defaults(void)
{
    DefDlg dd = { 0 };
    dd.n = w2k_assoc_count();
    if (dd.n > 8) dd.n = 8;

    int cw = 420, chh = 60 + dd.n * 30 + 76;
    W2kWin *w = w2k_win_new("Default Programs", "l2kcontrol", cw, chh, 0);

    for (int i = 0; i < dd.n; i++) {
        dd.edit[i] = w2k_edit_new(0);
        w2k_edit_bind(dd.edit[i], w);
        dd.edit[i]->r = (W2kRect){ 130, 40 + i * 30, cw - 130 - 50, 21 };
        dd.browse[i] = (W2kRect){ cw - 46, 40 + i * 30, 34, 21 };
        char cmd[512];
        w2k_assoc_get(w2k_assoc_class_at(i), cmd, sizeof cmd);
        w2k_edit_set(dd.edit[i], cmd);
        w2k_add_timer(w2k_caret_blink, blink, dd.edit[i]);
    }
    if (dd.n && dd.edit[0]) dd.edit[0]->focused = 1;
    int by = chh - 12 - 23;
    dd.cancel = (W2kRect){ cw - 12 - 75, by, 75, 23 };
    dd.ok     = (W2kRect){ cw - 12 - 75 * 2 - 6, by, 75, 23 };

    w->user = &dd;
    w->paint = defaults_paint;
    w->event = defaults_event;
    w2k_win_center(w, cp.win);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);

    w2k_win_modal(w);
    for (int i = 0; i < dd.n; i++) {
        w2k_del_timer(blink, dd.edit[i]);
        w2k_edit_free(dd.edit[i]);
    }
}

/* ------------------------------------------------------------------ *
 * Performance Options -- the visual effects list
 * ------------------------------------------------------------------ *
 * The dialog Windows puts behind System Properties, Advanced, Performance,
 * Settings: four radio buttons over a checked list of the individual
 * effects. Picking a box switches the radios to Custom, which is what the
 * original does.
 *
 * Effects this desktop cannot do -- font antialiasing needs scalable fonts,
 * and folder backgrounds and visual styles are XP shell furniture -- are
 * listed and greyed rather than hidden, so the dialog is the one people
 * remember and it is honest about what it can deliver. */
typedef struct {
    W2kList *list;
    W2kRect  radio[4];
    W2kRect  ok, cancel, apply;
    int      preset, down, dirty;
    unsigned char fx[N_EFFECTS];
    int      rowfx[N_EFFECTS];       /* which effect each list row shows */
    int      nrows;
} PerfDlg;

static PerfDlg *pd_active;

static void perf_fill(PerfDlg *pd)
{
    w2k_list_clear(pd->list);
    pd->nrows = 0;
    for (int i = 0; i < N_EFFECTS; i++) {
        if (!w2k_effect_listed(i)) continue;      /* withdrawn */
        int r = w2k_list_add(pd->list, ICO_NONE,
                             w2k_effect_supported(i) ? NULL : (void *)-1);
        w2k_list_set(pd->list, r, 0, w2k_effect_label(i));
        pd->list->items[r].checked = pd->fx[i];
        if (r >= 0 && r < N_EFFECTS) pd->rowfx[r] = i;
        pd->nrows = r + 1;
    }
}

static void perf_on_check(void *u, int idx)
{
    PerfDlg *pd = u;
    /* A row is not an effect: withdrawn ones are left out of the list. */
    if (idx < 0 || idx >= pd->nrows) return;
    int fx = pd->rowfx[idx];
    if (fx < 0 || fx >= N_EFFECTS) return;
    pd->fx[fx] = (unsigned char)pd->list->items[idx].checked;
    pd->preset = PRESET_CUSTOM;        /* as the original does */
    pd->dirty = 1;
    /* The dialog is repainted by perf_event, which called us; the folder
     * behind it shows nothing of this until Apply. */
}

static void perf_apply_preset(PerfDlg *pd, int preset)
{
    pd->preset = preset;
    if (preset != PRESET_CUSTOM) {
        unsigned char saved[N_EFFECTS];
        memcpy(saved, w2k_effects, sizeof saved);
        w2k_effects_preset(preset == PRESET_PERFORMANCE ? 1 : 0);
        memcpy(pd->fx, w2k_effects, sizeof pd->fx);
        memcpy(w2k_effects, saved, sizeof saved);   /* not live until Apply */
        perf_fill(pd);
    }
    pd->dirty = 1;
}

static void perf_commit(PerfDlg *pd)
{
    memcpy(w2k_effects, pd->fx, sizeof w2k_effects);
    w2k_scheme_save(NULL);
    w2k_scheme_broadcast();            /* the shell picks them up at once */
    pd->dirty = 0;
}

static void perf_paint(W2kWin *w, Drawable d)
{
    PerfDlg *pd = w->user;
    int fh = w2k_font_height(F_UI);

    w2k_text(d, F_UI, 12, 10,
             "Select the settings you want to use for the appearance and",
             C_TEXT);
    w2k_text(d, F_UI, 12, 10 + fh + 2, "performance of this desktop.", C_TEXT);

    static const char *labels[4] = {
        "&Let the desktop choose what's best",
        "Adjust for best &appearance",
        "Adjust for best &performance",
        "&Custom:"
    };
    for (int i = 0; i < 4; i++)
        w2k_draw_radio(d, pd->radio[i].x, pd->radio[i].y, labels[i],
                       pd->preset == i, 0, 0);

    w2k_list_draw(d, pd->list);

    w2k_draw_pushbutton(d, &pd->ok, "OK",
                        BS_DEFAULT | (pd->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &pd->cancel, "Cancel", pd->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &pd->apply, "&Apply",
                        (pd->dirty ? 0 : BS_DISABLED) |
                        (pd->down == 3 ? BS_PRESSED : 0));
}

static int perf_event(W2kWin *w, XEvent *e)
{
    PerfDlg *pd = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_list_press(pd->list, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        for (int i = 0; i < 4; i++)
            if (w2k_rect_hit(&pd->radio[i], x, y)) {
                perf_apply_preset(pd, i);
                w2k_win_dirty(w);
                return 1;
            }
        if (w2k_rect_hit(&pd->ok, x, y)) pd->down = 1;
        else if (w2k_rect_hit(&pd->cancel, x, y)) pd->down = 2;
        else if (w2k_rect_hit(&pd->apply, x, y) && pd->dirty) pd->down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = pd->down, x = e->xbutton.x, y = e->xbutton.y;
        pd->down = 0;
        w2k_list_release(pd->list, &e->xbutton);
        if (b == 1 && w2k_rect_hit(&pd->ok, x, y)) {
            perf_commit(pd);
            w2k_win_close(w, ID_OK);
        } else if (b == 2 && w2k_rect_hit(&pd->cancel, x, y)) {
            w2k_win_close(w, ID_CANCEL);
        } else if (b == 3 && w2k_rect_hit(&pd->apply, x, y)) {
            perf_commit(pd);
        }
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (w2k_list_motion(pd->list, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            perf_commit(pd);
            w2k_win_close(w, ID_OK);
            return 1;
        }
        if (w2k_list_key(pd->list, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void open_performance(void)
{
    PerfDlg pd = { 0 };
    memcpy(pd.fx, w2k_effects, sizeof pd.fx);
    pd.preset = PRESET_CUSTOM;

    int cw = 400, chh = 420;
    W2kWin *w = w2k_win_new("Performance Options", "l2kcontrol", cw, chh, 0);
    int fh = w2k_font_height(F_UI);

    int ry = 12 + 2 * (fh + 2) + 10;
    for (int i = 0; i < 4; i++)
        pd.radio[i] = (W2kRect){ 16, ry + i * (fh + 7), cw - 32, fh + 4 };

    pd.list = w2k_list_new(LV_REPORT);
    pd.list->checkboxes = 1;
    pd.list->hdr_h = 0;
    pd.list->fullrow = 1;
    pd.list->user = &pd;
    pd.list->on_check = perf_on_check;
    pd.list->focused = 1;
    w2k_list_add_col(pd.list, "Effect", cw - 40 - SCROLL_W, 0);
    pd.list->r = (W2kRect){ 16, ry + 4 * (fh + 7) + 6, cw - 32,
                            chh - (ry + 4 * (fh + 7) + 6) - 48 };
    w2k_scroll_bind(&pd.list->vsb, w);
    perf_fill(&pd);

    int by = chh - 12 - 23;
    pd.apply  = (W2kRect){ cw - 12 - 75, by, 75, 23 };
    pd.cancel = (W2kRect){ cw - 12 - 75 * 2 - 6, by, 75, 23 };
    pd.ok     = (W2kRect){ cw - 12 - 75 * 3 - 12, by, 75, 23 };

    w->user = &pd;
    w->paint = perf_paint;
    w->event = perf_event;
    w2k_win_center(w, cp.win);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);

    pd_active = &pd;
    w2k_win_modal(w);
    pd_active = NULL;
    w2k_list_free(pd.list);
}

/* ------------------------------------------------------------------ *
 * Mouse, Keyboard and Sounds
 *
 * Three small applets that share one dialog shell: a handful of
 * trackbars and check boxes over the input settings, applied to the X
 * server on OK (see lib/input.c) and remembered in ~/.w2k/scheme.
 * ------------------------------------------------------------------ */
static void open_folder_options(void) { w2k_folder_options(cp.win); }

#define MAX_SLIDERS 4

typedef struct InputDlg InputDlg;
struct InputDlg {
    const char *title;
    int      nsl;
    struct { const char *label, *lo, *hi; W2kSlider s; } sl[MAX_SLIDERS];
    int      ncheck;
    struct { const char *label; int on; W2kRect r; } chk[3];
    int      nradio;
    struct { const char *label; W2kRect r; } radio[2];
    int      sel_radio;
    int      test;                  /* offer a Test button (Sounds) */
    W2kRect  test_r, ok, cancel, apply;
    int      down;
    void   (*commit)(InputDlg *);
};

static void input_paint(W2kWin *w, Drawable d)
{
    InputDlg *id = w->user;
    int fh = w2k_font_height(F_UI);
    int y = 12;

    for (int i = 0; i < id->nradio; i++) {
        w2k_draw_radio(d, 16, id->radio[i].r.y, id->radio[i].label,
                       id->sel_radio == i, 0, 0);
        y = id->radio[i].r.y + fh + 8;
    }
    for (int i = 0; i < id->nsl; i++) {
        W2kRect r = id->sl[i].s.r;
        w2k_text_mnemonic(d, F_UI, 16, r.y - fh - 6, id->sl[i].label,
                          C_TEXT, 1);
        w2k_slider_draw(d, &id->sl[i].s);
        w2k_text(d, F_UI, r.x, r.y + SLIDER_THICK + 2, id->sl[i].lo, C_TEXT);
        int tw = w2k_text_width(F_UI, id->sl[i].hi, -1);
        w2k_text(d, F_UI, r.x + r.w - tw, r.y + SLIDER_THICK + 2,
                 id->sl[i].hi, C_TEXT);
    }
    for (int i = 0; i < id->ncheck; i++)
        w2k_draw_checkbox(d, id->chk[i].r.x, id->chk[i].r.y, id->chk[i].label,
                          id->chk[i].on, 0, 0);
    if (id->test)
        w2k_draw_pushbutton(d, &id->test_r, "&Test",
                            id->down == 4 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &id->ok, "OK",
                        BS_DEFAULT | (id->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &id->cancel, "Cancel", id->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &id->apply, "&Apply", id->down == 3 ? BS_PRESSED : 0);
    (void)y;
}

static void input_commit(InputDlg *id)
{
    if (id->commit) id->commit(id);
    w2k_input_apply();
    w2k_scheme_save(NULL);
    w2k_scheme_broadcast();
}

static int input_event(W2kWin *w, XEvent *e)
{
    InputDlg *id = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        for (int i = 0; i < id->nsl; i++)
            if (w2k_slider_press(&id->sl[i].s, &e->xbutton)) {
                /* Only one control has the focus, so only one answers the
                 * arrow keys. */
                for (int k = 0; k < id->nsl; k++)
                    if (k != i) id->sl[k].s.focused = 0;
                w2k_win_dirty(w);
                return 1;
            }
        for (int i = 0; i < id->nradio; i++)
            if (w2k_rect_hit(&id->radio[i].r, x, y)) id->sel_radio = i;
        for (int i = 0; i < id->ncheck; i++)
            if (w2k_rect_hit(&id->chk[i].r, x, y)) id->chk[i].on = !id->chk[i].on;
        if (id->test && w2k_rect_hit(&id->test_r, x, y)) id->down = 4;
        else if (w2k_rect_hit(&id->ok, x, y)) id->down = 1;
        else if (w2k_rect_hit(&id->cancel, x, y)) id->down = 2;
        else if (w2k_rect_hit(&id->apply, x, y)) id->down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = id->down, x = e->xbutton.x, y = e->xbutton.y;
        id->down = 0;
        for (int i = 0; i < id->nsl; i++) w2k_slider_release(&id->sl[i].s);
        if (b == 1 && w2k_rect_hit(&id->ok, x, y)) {
            input_commit(id);
            w2k_win_close(w, ID_OK);
            return 1;
        }
        if (b == 2 && w2k_rect_hit(&id->cancel, x, y)) {
            w2k_win_close(w, ID_CANCEL);
            return 1;
        }
        if (b == 3 && w2k_rect_hit(&id->apply, x, y)) input_commit(id);
        if (b == 4 && id->test && w2k_rect_hit(&id->test_r, x, y)) {
            /* Ring with what the sliders currently say, not what is
             * saved -- that is the point of a test button. */
            input_commit(id);
            XBell(w2k.dpy, 0);
        }
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        for (int i = 0; i < id->nsl; i++)
            if (w2k_slider_motion(&id->sl[i].s, &e->xmotion)) {
                w2k_win_dirty(w);
                return 1;
            }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return) { input_commit(id); w2k_win_close(w, ID_OK); return 1; }
        /* Tab walks the sliders: only a click used to give one the focus,
         * so without a mouse none of them could be moved. */
        if ((ks == XK_Tab || ks == XK_ISO_Left_Tab) && id->nsl) {
            int f = 0, back = (e->xkey.state & ShiftMask) || ks == XK_ISO_Left_Tab;
            for (int i = 0; i < id->nsl; i++) if (id->sl[i].s.focused) f = i;
            for (int i = 0; i < id->nsl; i++) id->sl[i].s.focused = 0;
            id->sl[(f + (back ? id->nsl - 1 : 1)) % id->nsl].s.focused = 1;
            w2k_win_dirty(w);
            return 1;
        }
        for (int i = 0; i < id->nsl; i++)
            if (id->sl[i].s.focused && w2k_slider_key(&id->sl[i].s, &e->xkey)) {
                w2k_win_dirty(w);
                return 1;
            }
        return 1;
    }
    }
    return 0;
}

static void input_run(InputDlg *id, int height)
{
    int cw = 360, chh = height;
    W2kWin *w = w2k_win_new(id->title, "l2kcontrol", cw, chh, 0);
    int fh = w2k_font_height(F_UI);

    int y = 14;
    for (int i = 0; i < id->nradio; i++) {
        id->radio[i].r = (W2kRect){ 16, y, cw - 32, fh + 4 };
        y += fh + 8;
    }
    if (id->nradio) y += 6;
    for (int i = 0; i < id->nsl; i++) {
        y += fh + 6;
        id->sl[i].s.r = (W2kRect){ 24, y, cw - 48, SLIDER_THICK };
        id->sl[i].s.owner = w;
        y += SLIDER_THICK + fh + 12;
    }
    for (int i = 0; i < id->ncheck; i++) {
        id->chk[i].r = (W2kRect){ 16, y, cw - 32, fh + 4 };
        y += fh + 8;
    }
    int by = chh - 12 - 23;
    id->test_r = (W2kRect){ 16, by, 75, 23 };
    id->apply  = (W2kRect){ cw - 12 - 75, by, 75, 23 };
    id->cancel = (W2kRect){ cw - 12 - 75 * 2 - 6, by, 75, 23 };
    id->ok     = (W2kRect){ cw - 12 - 75 * 3 - 12, by, 75, 23 };

    if (id->nsl) id->sl[0].s.focused = 1;   /* the first control, as in Windows */
    w->user = id;
    w->paint = input_paint;
    w->event = input_event;
    w2k_win_center(w, cp.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_win_modal(w);
}

/* ---- Mouse Properties ----------------------------------------------- *
 * Windows 2000's four tabs, in its own order: Buttons, Pointers, Motion
 * and Hardware. What each one sets is real -- the button order and the
 * pointer speed go into the X server (lib/input.c), the double-click
 * time and the click-to-open rule into the shell's settings, and the
 * pointers into the cursor set in ~/.w2k/cursors. */
enum { MT_BUTTONS, MT_POINTERS, MT_MOTION, MT_HARDWARE };
enum { MP_NONE, MP_OK, MP_CANCEL, MP_APPLY, MP_SAVEAS, MP_DELETE,
       MP_USEDEF, MP_BROWSE };

typedef struct {
    W2kWin   *win;
    W2kTabs  *tabs;
    int       down;
    W2kRect   ok, cancel, apply;

    /* Buttons */
    int       swap, single;
    W2kSlider dbl;
    W2kRect   r_right, r_left, r_single, r_double, test;
    int       test_open;

    /* Pointers */
    W2kCombo *scheme;
    char      set_name[8][64];      /* the pointer sets, by folder name */
    int       nsets;
    int       was_windows;          /* the set in use when the box opened, */
    char      was_scheme[64];       /* or when Apply last took */
    W2kList  *roles;
    int       shadow;
    W2kRect   r_shadow, saveas, del, usedef, browse, preview;
    int       icon_of[16];
    /* The user.crs files Browse... and Use Default have written, as they
     * were before the first write; Cancel puts them back. */
    struct { char path[700]; char *was; int existed; } crs[12];
    int       ncrs;

    /* Motion */
    W2kSlider speed;
    int       accel, snap;
    W2kRect   r_accel[4], r_snap;

    /* Hardware */
    W2kList  *devs;
} MouseDlg;

/* The pointer of one role as a picture the list can draw: the .cur file
 * is an icon file in all but name, so the icon loader reads it. */
static int role_icon(int r)
{
    int id = w2k_cursor_role_icon(r);
    return id < 0 ? ICO_NONE : id;
}

static void mouse_fill_roles(MouseDlg *m)
{
    w2k_list_clear(m->roles);
    int n = w2k_cursor_roles();
    for (int r = 0; r < n && r < 16; r++) {
        m->icon_of[r] = role_icon(r);
        int row = w2k_list_add(m->roles, ICO_NONE, (void *)(intptr_t)r);
        w2k_list_set(m->roles, row, 0, w2k_cursor_role_label(r));
    }
}

/* The pointing devices the kernel knows about, for the Hardware tab. */
static void mouse_fill_devices(MouseDlg *m)
{
    w2k_list_clear(m->devs);
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return;
    char line[512], name[128] = "";
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "N: Name=", 8)) {
            char *q = strchr(line + 8, '"');
            if (q) {
                char *e = strchr(q + 1, '"');
                if (e) *e = 0;
                snprintf(name, sizeof name, "%.127s", q + 1);
            }
        } else if (!strncmp(line, "H: Handlers=", 12) && name[0]) {
            if (strstr(line, "mouse") || strstr(line, "event") == NULL) {
                if (strstr(line, "mouse")) {
                    int row = w2k_list_add(m->devs, ICO_CP_MOUSE, NULL);
                    w2k_list_set(m->devs, row, 0, name);
                    w2k_list_set(m->devs, row, 1, "Mice and other pointing devices");
                }
            }
            name[0] = 0;
        }
    }
    fclose(f);
    if (!m->devs->n) {
        int row = w2k_list_add(m->devs, ICO_CP_MOUSE, NULL);
        w2k_list_set(m->devs, row, 0, "Pointing device");
        w2k_list_set(m->devs, row, 1, "Mice and other pointing devices");
    }
}

/* The mouse on the Buttons tab, with the button that selects picked out:
 * Windows drew a bitmap; this draws the same shape. */
static void draw_mouse(Drawable d, int x, int y, int left_handed)
{
    /* A mouse seen from above: the body is an oval, the two buttons sit
     * in its top half with the wheel between them, and the one that
     * selects is filled in, as the bitmap in Windows 2000 shows it. */
    const int w = 44, h = 58;
    int cx = x + w / 2;
    for (int row = 0; row < h; row++) {
        double t = (row - h / 2.0) / (h / 2.0);
        double k = 1.0 - t * t * (row < h / 2 ? 0.55 : 0.30);  /* narrower at the top */
        if (k < 0) k = 0;
        int half = (int)((w / 2.0) * (k > 1 ? 1 : k));
        if (half <= 0) continue;
        int fy = y + row;
        w2k_fill_rgb(d, cx - half, fy, half * 2, 1, 255, 255, 255);
        w2k_fill_rgb(d, cx - half, fy, 1, 1, 0, 0, 0);          /* outline */
        w2k_fill_rgb(d, cx + half - 1, fy, 1, 1, 0, 0, 0);
        if (row == 0 || row == h - 1)
            w2k_fill_rgb(d, cx - half, fy, half * 2, 1, 0, 0, 0);
        /* The button half that selects, shaded down to the split. */
        if (row > 1 && row < 24) {
            int bx = left_handed ? cx + 1 : cx - half + 1;
            int bw = half - 2;
            if (bw > 0) w2k_fill_rgb(d, bx, fy, bw, 1, 0, 0, 128);
        }
    }
    w2k_hline(d, cx - w / 2 + 2, y + 24, w - 4, 0);              /* the split */
    w2k_vline(d, cx, y + 1, 23, 0);
    w2k_fill_rgb(d, cx - 3, y + 6, 6, 12, 224, 224, 224);        /* the wheel */
    w2k_edge(d, cx - 4, y + 5, 8, 14, EDGE_SUNKEN_THIN, BF_RECT);
    w2k_vline(d, cx, y - 8, 8, 0);                               /* the cable */
}

/* The Test area's jack-in-the-box: shut, then open on a double-click. */
static void draw_jack(Drawable d, const W2kRect *r, int open)
{
    int bx = r->x + r->w / 2 - 12, by = r->y + r->h - 26;
    if (open) {
        w2k_fill_rgb(d, bx + 4, by - 20, 16, 20, 192, 0, 192);   /* the jack */
        w2k_fill_rgb(d, bx + 8, by - 26, 8, 8, 255, 255, 0);
    }
    w2k_fill_rgb(d, bx, by, 24, 22, 214, 170, 0);                 /* the box */
    w2k_edge(d, bx, by, 24, 22, EDGE_RAISED, BF_RECT);
    if (!open) w2k_hline(d, bx + 2, by + 4, 20, C_SHADOW);
}

static void mouse_paint(W2kWin *w, Drawable d)
{
    MouseDlg *m = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_tabs_draw(d, m->tabs);
    W2kRect c = w2k_tabs_client(m->tabs);
    int x = c.x + 10, gw = c.w - 20;

    if (m->tabs->sel == MT_BUTTONS) {
        W2kRect g = { x, c.y + 10, gw, 136 };
        w2k_draw_groupbox(d, &g, "Button configuration");
        m->r_right = (W2kRect){ x + 16, c.y + 30, 110, fh + 4 };
        m->r_left  = (W2kRect){ x + 140, c.y + 30, 110, fh + 4 };
        w2k_draw_radio(d, m->r_right.x, m->r_right.y, "&Right-handed", !m->swap, 0, 0);
        w2k_draw_radio(d, m->r_left.x, m->r_left.y, "&Left-handed", m->swap, 0, 0);
        draw_mouse(d, c.x + c.w / 2 - 23, c.y + 56, m->swap);
        int ly = c.y + 62;
        w2k_text(d, F_UI, x + 16, ly, "Left Button:", C_TEXT);
        w2k_text(d, F_UI, x + 22, ly + fh + 4,
                 m->swap ? "- Context Menu" : "- Normal Select", C_TEXT);
        w2k_text(d, F_UI, x + 22, ly + 2 * fh + 6,
                 m->swap ? "- Special Drag" : "- Normal Drag", C_TEXT);
        int rx = x + gw - 120;
        w2k_text(d, F_UI, rx, ly, "Right Button:", C_TEXT);
        w2k_text(d, F_UI, rx + 6, ly + fh + 4,
                 m->swap ? "- Normal Select" : "- Context Menu", C_TEXT);
        w2k_text(d, F_UI, rx + 6, ly + 2 * fh + 6,
                 m->swap ? "- Normal Drag" : "- Special Drag", C_TEXT);

        W2kRect g2 = { x, c.y + 156, gw, 74 };
        w2k_draw_groupbox(d, &g2, "Files and Folders");
        w2k_bigicon_draw(d, x + 14, c.y + 178, ICO_FOLDER);
        m->r_single = (W2kRect){ x + 56, c.y + 176, gw - 70, fh + 4 };
        m->r_double = (W2kRect){ x + 56, c.y + 200, gw - 70, fh + 4 };
        w2k_draw_radio(d, m->r_single.x, m->r_single.y,
                       "&Single-click to open an item (point to select)",
                       m->single, 0, 0);
        w2k_draw_radio(d, m->r_double.x, m->r_double.y,
                       "&Double-click to open an item (single-click to select)",
                       !m->single, 0, 0);

        W2kRect g3 = { x, c.y + 240, gw, 92 };
        w2k_draw_groupbox(d, &g3, "Double-click speed");
        w2k_text(d, F_UI, x + 16, c.y + 296, "Slow", C_TEXT);
        w2k_slider_draw(d, &m->dbl);
        w2k_text(d, F_UI, m->dbl.r.x + m->dbl.r.w + 8, c.y + 296, "Fast", C_TEXT);
        w2k_text(d, F_UI, x + gw - 104, c.y + 262, "Test area:", C_TEXT);
        w2k_draw_well(d, &m->test);
        draw_jack(d, &m->test, m->test_open);

    } else if (m->tabs->sel == MT_POINTERS) {
        W2kRect g = { x, c.y + 10, gw, 92 };
        w2k_draw_groupbox(d, &g, "Scheme");
        w2k_combo_draw(d, m->scheme);
        w2k_draw_pushbutton(d, &m->saveas, "Save &As...",
                            m->down == MP_SAVEAS ? BS_PRESSED : 0);
        w2k_draw_pushbutton(d, &m->del, "&Delete",
                            BS_DISABLED | (m->down == MP_DELETE ? BS_PRESSED : 0));
        w2k_draw_well(d, &m->preview);
        {   /* the pointer the list is on, at its own size */
            int row = m->roles->sel >= 0 ? m->roles->sel : 0;
            int ic = (row >= 0 && row < 16) ? m->icon_of[row] : ICO_NONE;
            if (ic != ICO_NONE)
                w2k_bigicon_draw(d, m->preview.x + m->preview.w / 2 - 16,
                                 m->preview.y + m->preview.h / 2 - 16, ic);
        }
        w2k_text(d, F_UI, x, c.y + 112, "Customize:", C_TEXT);
        w2k_list_draw(d, m->roles);
        /* Each pointer at the right of its own row, where Windows shows
         * it; the list itself draws icons on the left. */
        for (int i = m->roles->top; i < m->roles->n; i++) {
            int ry = m->roles->r.y + 2 + m->roles->hdr_h +
                     (i - m->roles->top) * m->roles->row_h;
            if (ry + m->roles->row_h > m->roles->r.y + m->roles->r.h - 2) break;
            if (i < 16 && m->icon_of[i] != ICO_NONE)
                w2k_icon_draw_scaled(d, m->roles->r.x + m->roles->r.w - 44,
                                     ry + (m->roles->row_h - 24) / 2, m->icon_of[i], 24);
        }
        w2k_draw_checkbox(d, m->r_shadow.x, m->r_shadow.y,
                          "&Enable pointer shadow", m->shadow, 0,
                          !w2k_effect_supported(FX_CURSOR_SHADOW));
        w2k_draw_pushbutton(d, &m->usedef, "&Use Default",
                            m->down == MP_USEDEF ? BS_PRESSED : 0);
        w2k_draw_pushbutton(d, &m->browse, "&Browse...",
                            m->down == MP_BROWSE ? BS_PRESSED : 0);

    } else if (m->tabs->sel == MT_MOTION) {
        W2kRect g = { x, c.y + 10, gw, 92 };
        w2k_draw_groupbox(d, &g, "Speed");
        w2k_bigicon_draw(d, x + 14, c.y + 30, ICO_CP_MOUSE);
        w2k_text(d, F_UI, x + 56, c.y + 38, "Adjust how fast your pointer moves", C_TEXT);
        w2k_text(d, F_UI, x + 56, c.y + 74, "Slow", C_TEXT);
        w2k_slider_draw(d, &m->speed);
        w2k_text(d, F_UI, m->speed.r.x + m->speed.r.w + 8, c.y + 74, "Fast", C_TEXT);

        W2kRect g2 = { x, c.y + 112, gw, 100 };
        w2k_draw_groupbox(d, &g2, "Acceleration");
        w2k_bigicon_draw(d, x + 14, c.y + 132, ICO_CP_MOUSE);
        w2k_text(d, F_UI, x + 56, c.y + 136, "Adjust how much your pointer accelerates as", C_TEXT);
        w2k_text(d, F_UI, x + 56, c.y + 136 + fh + 2, "you move it faster", C_TEXT);
        static const char *const an[4] = { "&None", "&Low", "&Medium", "&High" };
        for (int i = 0; i < 4; i++)
            w2k_draw_radio(d, m->r_accel[i].x, m->r_accel[i].y, an[i],
                           m->accel == i, 0, 0);

        W2kRect g3 = { x, c.y + 222, gw, 74 };
        w2k_draw_groupbox(d, &g3, "Snap to default");
        W2kRect okb = { x + 16, c.y + 244, 46, 23 };
        w2k_draw_pushbutton(d, &okb, "OK", BS_DEFAULT);
        w2k_draw_checkbox(d, m->r_snap.x, m->r_snap.y,
                          "&Move pointer to the default button in dialog boxes",
                          m->snap, 0, 0);

    } else {
        w2k_text(d, F_UI, x, c.y + 12, "Devices:", C_TEXT);
        w2k_list_draw(d, m->devs);
        w2k_text_wrapped(d, F_UI, x, c.y + 206, gw,
                         "The pointer is the X server's; its speed and button order are "
                         "set on the Motion and Buttons tabs. A device that is not "
                         "listed here is one the kernel does not call a mouse.", C_TEXT);
    }

    w2k_draw_pushbutton(d, &m->ok, "OK", BS_DEFAULT | (m->down == MP_OK ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &m->cancel, "Cancel", m->down == MP_CANCEL ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &m->apply, "&Apply", m->down == MP_APPLY ? BS_PRESSED : 0);
}

/* The Scheme list shows each set as it is picked, which loads it; Cancel
 * puts back the one the desktop was using. */
static void mouse_unpick(MouseDlg *m)
{
    if (w2k_cursors_windows == m->was_windows &&
        !strcmp(w2k_cursor_scheme, m->was_scheme)) return;
    w2k_cursors_windows = m->was_windows;
    snprintf(w2k_cursor_scheme, sizeof w2k_cursor_scheme, "%.63s", m->was_scheme);
    w2k_cursors_init();
}

/* Browse... and Use Default write the set's user.crs at once, so the list
 * shows the new pointer; Cancel used to leave it there, in use from the
 * next logon. Each file is kept as it was before its first write here. 0
 * when it could not be kept: then nothing is written. */
static int mouse_keep_crs(MouseDlg *m)
{
    const char *home = getenv("HOME");
    if (!home) return 0;
    char path[700];
    /* Where w2k_cursor_role_set writes: the chosen set's own folder, or
     * the cursors folder itself for the Windows 2000 set. */
    if (w2k_cursor_scheme[0] && strcasecmp(w2k_cursor_scheme, "win2k"))
        snprintf(path, sizeof path, "%.500s/.w2k/cursors/%.63s/user.crs", home, w2k_cursor_scheme);
    else
        snprintf(path, sizeof path, "%.500s/.w2k/cursors/user.crs", home);
    for (int i = 0; i < m->ncrs; i++) if (!strcmp(m->crs[i].path, path)) return 1;
    if (m->ncrs >= (int)(sizeof m->crs / sizeof m->crs[0])) return 0;
    char *was = NULL;
    int existed = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t cap = 65536, n;
        was = malloc(cap + 1);
        n = was ? fread(was, 1, cap + 1, f) : 0;
        int bad = !was || ferror(f) || n > cap;
        fclose(f);
        if (bad) { free(was); return 0; }
        was[n] = 0;
        existed = 1;
    } else if (errno != ENOENT) return 0;
    snprintf(m->crs[m->ncrs].path, sizeof m->crs[m->ncrs].path, "%s", path);
    m->crs[m->ncrs].was = was;
    m->crs[m->ncrs].existed = existed;
    m->ncrs++;
    return 1;
}

/* OK or Apply: what is in the files now stays. */
static void mouse_forget_crs(MouseDlg *m)
{
    for (int i = 0; i < m->ncrs; i++) free(m->crs[i].was);
    m->ncrs = 0;
}

/* Cancel: each file as it was, or gone again when it was not there. */
static void mouse_restore_crs(MouseDlg *m)
{
    if (!m->ncrs) return;
    for (int i = 0; i < m->ncrs; i++) {
        if (!m->crs[i].existed) { unlink(m->crs[i].path); continue; }
        FILE *f = fopen(m->crs[i].path, "w");
        if (!f) continue;
        fputs(m->crs[i].was, f);
        fclose(f);
    }
    mouse_forget_crs(m);
    w2k_cursors_init();
}

static void mouse_commit(MouseDlg *m)
{
    w2k_mouse_swap = m->swap;
    w2k_folder_singleclick = m->single;
    w2k_dblclk_ms = 900 - m->dbl.pos * 70;
    w2k_mouse_speed = m->speed.pos;
    w2k_mouse_accel = m->accel;
    w2k_snap_default = m->snap;
    w2k_effects[FX_CURSOR_SHADOW] = m->shadow && w2k_effect_supported(FX_CURSOR_SHADOW);
    {   /* Whichever pointer set the Scheme list is on. */
        int sel = m->scheme->sel;
        if (sel == 0) { w2k_cursors_windows = 1; snprintf(w2k_cursor_scheme, sizeof w2k_cursor_scheme, "win2k"); }
        else if (sel <= m->nsets) {
            w2k_cursors_windows = 1;
            snprintf(w2k_cursor_scheme, sizeof w2k_cursor_scheme, "%.63s",
                     m->set_name[sel - 1]);
        } else w2k_cursors_windows = 0;
        m->was_windows = w2k_cursors_windows;
        snprintf(m->was_scheme, sizeof m->was_scheme, "%.63s", w2k_cursor_scheme);
    }
    mouse_forget_crs(m);
    w2k_scheme_save(NULL);
    w2k_input_apply();
    w2k_cursors_init();
    w2k_scheme_broadcast();
}

static int mouse_event(W2kWin *w, XEvent *e)
{
    MouseDlg *m = w->user;
    if ((e->type == KeyPress && w2k_tabs_key(m->tabs, &e->xkey)) ||
        (e->type == ButtonPress && w2k_tabs_press(m->tabs, &e->xbutton))) {
        w2k_win_dirty(w);
        return 1;
    }
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (m->tabs->sel == MT_BUTTONS) {
            if (w2k_slider_press(&m->dbl, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_rect_hit(&m->r_right, x, y)) m->swap = 0;
            else if (w2k_rect_hit(&m->r_left, x, y)) m->swap = 1;
            else if (w2k_rect_hit(&m->r_single, x, y)) m->single = 1;
            else if (w2k_rect_hit(&m->r_double, x, y)) m->single = 0;
            else if (w2k_rect_hit(&m->test, x, y)) {
                /* The box opens on the second click of a double-click, as
                 * it does in Windows, and shuts on the next one. */
                static Time last;
                if (e->xbutton.time - last <= (Time)w2k_dblclk_ms) m->test_open = !m->test_open;
                last = e->xbutton.time;
            }
        } else if (m->tabs->sel == MT_POINTERS) {
            if (w2k_combo_press(m->scheme, &e->xbutton)) {
                /* The list and the preview follow the scheme at once; the
                 * desktop itself waits for OK or Apply. */
                int sel = m->scheme->sel;
                if (sel == 0) { w2k_cursors_windows = 1; snprintf(w2k_cursor_scheme, sizeof w2k_cursor_scheme, "win2k"); }
                else if (sel <= m->nsets) {
                    w2k_cursors_windows = 1;
                    snprintf(w2k_cursor_scheme, sizeof w2k_cursor_scheme,
                             "%.63s", m->set_name[sel - 1]);
                } else w2k_cursors_windows = 0;
                w2k_cursors_init();
                mouse_fill_roles(m);
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_list_press(m->roles, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_rect_hit(&m->r_shadow, x, y) && w2k_effect_supported(FX_CURSOR_SHADOW))
                m->shadow = !m->shadow;
            else if (w2k_rect_hit(&m->saveas, x, y)) m->down = MP_SAVEAS;
            else if (w2k_rect_hit(&m->usedef, x, y)) m->down = MP_USEDEF;
            else if (w2k_rect_hit(&m->browse, x, y)) m->down = MP_BROWSE;
        } else if (m->tabs->sel == MT_MOTION) {
            if (w2k_slider_press(&m->speed, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            for (int i = 0; i < 4; i++)
                if (w2k_rect_hit(&m->r_accel[i], x, y)) m->accel = i;
            if (w2k_rect_hit(&m->r_snap, x, y)) m->snap = !m->snap;
        } else {
            if (w2k_list_press(m->devs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        }
        if (w2k_rect_hit(&m->ok, x, y)) m->down = MP_OK;
        else if (w2k_rect_hit(&m->cancel, x, y)) m->down = MP_CANCEL;
        else if (w2k_rect_hit(&m->apply, x, y)) m->down = MP_APPLY;
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (w2k_slider_motion(&m->dbl, &e->xmotion) ||
            w2k_slider_motion(&m->speed, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        int b = m->down, x = e->xbutton.x, y = e->xbutton.y;
        m->down = MP_NONE;
        w2k_slider_release(&m->dbl);
        w2k_slider_release(&m->speed);
        w2k_list_release(m->roles, &e->xbutton);
        if (b == MP_OK && w2k_rect_hit(&m->ok, x, y)) {
            mouse_commit(m);
            w2k_win_close(w, ID_OK);
            return 1;
        }
        if (b == MP_CANCEL && w2k_rect_hit(&m->cancel, x, y)) {
            w2k_win_close(w, ID_CANCEL);
            return 1;
        }
        if (b == MP_APPLY && w2k_rect_hit(&m->apply, x, y)) mouse_commit(m);
        if (b == MP_BROWSE && w2k_rect_hit(&m->browse, x, y)) {
            /* Another pointer for the role the list is on. */
            int row = m->roles->sel;
            if (row >= 0) {
                char path[1024] = "";
                const char *cur = w2k_cursor_role_file(row);
                if (cur) snprintf(path, sizeof path, "%s", cur);
                if (w2k_file_dialog_filter(w, 0, path, sizeof path,
                                           "Cursors (*.cur;*.ico)|*.cur;*.ico|All Files (*.*)|*")) {
                    if (!mouse_keep_crs(m))     /* Cancel could not undo it */
                        w2k_msgbox(w, "Mouse Properties",
                                   "The pointer scheme in ~/.w2k/cursors could not be read, "
                                   "so it was left as it is.", MB_OK | MB_ICONERROR);
                    else if (w2k_cursor_role_set(row, path)) mouse_fill_roles(m);
                    else w2k_msgbox(w, "Mouse Properties",
                                    "That file is not a cursor this desktop can read.",
                                    MB_OK | MB_ICONERROR);
                }
            }
        }
        if (b == MP_USEDEF && w2k_rect_hit(&m->usedef, x, y)) {
            /* The pointer the list is on goes back to the one its set
             * came with, as in Windows; the rest stay as they are. */
            int row = m->roles->sel;
            char def[1024];
            if (row >= 0 && w2k_cursor_role_default(row, def, sizeof def) &&
                mouse_keep_crs(m) && w2k_cursor_role_set(row, def)) mouse_fill_roles(m);
        }
        if (b == MP_SAVEAS && w2k_rect_hit(&m->saveas, x, y))
            w2k_msgbox(w, "Mouse Properties",
                       "The pointers in use are already the scheme this desktop "
                       "keeps, in ~/.w2k/cursors.", MB_OK | MB_ICONINFO);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return) { mouse_commit(m); w2k_win_close(w, ID_OK); return 1; }
        if (m->tabs->sel == MT_POINTERS && w2k_list_key(m->roles, &e->xkey)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (m->tabs->sel == MT_MOTION && w2k_slider_key(&m->speed, &e->xkey)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (m->tabs->sel == MT_BUTTONS && w2k_slider_key(&m->dbl, &e->xkey)) {
            w2k_win_dirty(w);
            return 1;
        }
        return 1;
    }
    }
    return 0;
}

static void open_mouse(void)
{
    MouseDlg m;
    memset(&m, 0, sizeof m);
    const int W = 396, H = 422;
    W2kWin *w = w2k_win_new("Mouse Properties", "l2kcontrol", W, H, 0);
    m.win = w;
    w->user = &m;
    w->paint = mouse_paint;
    w->event = mouse_event;

    m.tabs = w2k_tabs_new(NULL, NULL);
    w2k_tabs_add(m.tabs, "Buttons");
    w2k_tabs_add(m.tabs, "Pointers");
    w2k_tabs_add(m.tabs, "Motion");
    w2k_tabs_add(m.tabs, "Hardware");
    m.tabs->r = (W2kRect){ 8, 8, W - 16, H - 8 - 44 };
    W2kRect c = w2k_tabs_client(m.tabs);
    int x = c.x + 10, gw = c.w - 20, fh = w2k_font_height(F_UI);

    /* Buttons */
    m.swap = w2k_mouse_swap;
    m.single = w2k_folder_singleclick;
    m.dbl = (W2kSlider){ .r = { x + 50, c.y + 288, 150, SLIDER_THICK },
                         .lo = 0, .hi = 10, .ticks = 10,
                         .pos = (900 - w2k_dblclk_ms) / 70, .owner = w };
    m.test = (W2kRect){ x + gw - 76, c.y + 276, 56, 50 };

    /* Pointers */
    /* Scheme: the pointers this desktop ships, every set beside them --
     * ReactOS is one -- and the X server's own. */
    m.scheme = w2k_combo_new(0);
    m.nsets = w2k_cursor_schemes(m.set_name, 8);
    w2k_combo_add(m.scheme, "Windows 2000 (system scheme)");
    for (int i = 0; i < m.nsets; i++) {
        char label[80];
        /* "reactos" reads as "ReactOS", the way its own people write it. */
        if (!strcasecmp(m.set_name[i], "reactos")) snprintf(label, sizeof label, "ReactOS");
        else snprintf(label, sizeof label, "%.63s", m.set_name[i]);
        w2k_combo_add(m.scheme, label);
    }
    w2k_combo_add(m.scheme, "(None) -- the X server's own pointers");
    m.was_windows = w2k_cursors_windows;
    snprintf(m.was_scheme, sizeof m.was_scheme, "%.63s", w2k_cursor_scheme);
    m.scheme->sel = 0;                  /* "win2k": the set in the folder itself */
    if (!w2k_cursors_windows) m.scheme->sel = 1 + m.nsets;
    else for (int i = 0; i < m.nsets; i++)
        if (!strcasecmp(m.set_name[i], w2k_cursor_scheme)) m.scheme->sel = 1 + i;
    m.scheme->r = (W2kRect){ x + 16, c.y + 32, gw - 120, 21 };
    m.saveas = (W2kRect){ x + 90, c.y + 62, 80, 23 };
    m.del    = (W2kRect){ x + 178, c.y + 62, 75, 23 };
    m.preview = (W2kRect){ x + gw - 76, c.y + 24, 56, 56 };
    m.roles = w2k_list_new(LV_REPORT);
    w2k_list_add_col(m.roles, "", gw - 60, 0);
    m.roles->r = (W2kRect){ x, c.y + 128, gw, 150 };
    m.roles->hdr_h = 0;                 /* the Customize list has no header */
    mouse_fill_roles(&m);
    m.shadow = w2k_effects[FX_CURSOR_SHADOW];
    m.r_shadow = (W2kRect){ x, c.y + 290, 200, fh + 4 };
    m.usedef = (W2kRect){ x + gw - 166, c.y + 286, 80, 23 };
    m.browse = (W2kRect){ x + gw - 80, c.y + 286, 80, 23 };

    /* Motion */
    m.speed = (W2kSlider){ .r = { x + 90, c.y + 66, 160, SLIDER_THICK },
                           .lo = 1, .hi = 10, .ticks = 9,
                           .pos = w2k_mouse_speed, .owner = w };
    m.accel = w2k_mouse_accel;
    m.snap = w2k_snap_default;
    for (int i = 0; i < 4; i++)
        m.r_accel[i] = (W2kRect){ x + 16 + i * 84, c.y + 176, 80, fh + 4 };
    m.r_snap = (W2kRect){ x + 76, c.y + 248, gw - 90, fh + 4 };

    /* Hardware */
    m.devs = w2k_list_new(LV_REPORT);
    w2k_list_add_col(m.devs, "Name", 220, 0);
    w2k_list_add_col(m.devs, "Type", 150, 0);
    m.devs->r = (W2kRect){ x, c.y + 30, gw, 160 };
    mouse_fill_devices(&m);

    int by = H - 12 - 23;
    m.ok     = (W2kRect){ W - 12 - 75 * 3 - 12, by, 75, 23 };
    m.cancel = (W2kRect){ W - 12 - 75 * 2 - 6, by, 75, 23 };
    m.apply  = (W2kRect){ W - 12 - 75, by, 75, 23 };

    w2k_win_center(w, cp.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_win_modal(w);
    mouse_restore_crs(&m);              /* Cancel, Escape or the close box */
    mouse_unpick(&m);
    w2k_combo_free(m.scheme);
    w2k_list_free(m.roles);
    w2k_list_free(m.devs);
    w2k_tabs_free(m.tabs);
}

static void keyboard_commit(InputDlg *id)
{
    /* Windows' sliders are "repeat delay" (long to short) and "repeat
     * rate" (slow to fast); both are stored as what X wants. */
    w2k_key_delay = 1000 - id->sl[0].s.pos * 175;
    w2k_key_rate = 2 + id->sl[1].s.pos * 3;
    w2k_caret_blink = 1100 - id->sl[2].s.pos * 100;
}

static void open_keyboard(void)
{
    InputDlg id;
    memset(&id, 0, sizeof id);
    id.title = "Keyboard Properties";
    id.commit = keyboard_commit;
    id.nsl = 3;
    id.sl[0].label = "Repeat &delay:";
    id.sl[0].lo = "Long"; id.sl[0].hi = "Short";
    id.sl[0].s = (W2kSlider){ .lo = 0, .hi = 4, .ticks = 4,
                              .pos = (1000 - w2k_key_delay) / 175 };
    id.sl[1].label = "Repeat &rate:";
    id.sl[1].lo = "Slow"; id.sl[1].hi = "Fast";
    id.sl[1].s = (W2kSlider){ .lo = 0, .hi = 10, .ticks = 10,
                              .pos = (w2k_key_rate - 2) / 3 };
    id.sl[2].label = "&Cursor blink rate:";
    id.sl[2].lo = "None"; id.sl[2].hi = "Fast";
    id.sl[2].s = (W2kSlider){ .lo = 0, .hi = 10, .ticks = 10,
                              .pos = (1100 - w2k_caret_blink) / 100 };
    input_run(&id, 320);
}

/* ------------------------------------------------------------------ *
 * Sounds and Multimedia
 *
 * The Windows 2000 applet: the sound events in their two groups, each
 * with the file it plays and a speaker beside it when it plays one; the
 * Name box with a play button and Browse; the Scheme box, which here
 * lists the sound packs (Windows 98, 2000, XP, 7 and the 7 themes); the
 * volume slider. A second page keeps the system beep's settings.
 * ------------------------------------------------------------------ */
typedef struct {
    W2kWin   *win;
    W2kTabs  *tabs;
    W2kList  *list;
    W2kCombo *name, *scheme;
    W2kRect   play, browse, saveas, del, ok, cancel, apply;
    W2kSlider vol;
    char      files[64][128];
    int       nfiles;
    char      pack_ids[24][32], pack_labels[24][48];
    int       npacks;
    int       cur;                  /* the event picked, -1 */
    int       down, dirty, fill;
    /* the beep page */
    W2kSlider bvol, bpitch, bdur;
    W2kRect   beep_box;
    int       beep_on;
} SndDlg;

static SndDlg *sd_active;

/* Which event a list row is: the group rows carry -1. */
static int snd_row_event(SndDlg *sd, int row)
{
    if (row < 0 || row >= sd->list->n) return -1;
    return (int)(intptr_t)sd->list->items[row].data - 1;
}

static void snd_fill_list(SndDlg *sd)
{
    int keep = sd->list->sel;
    w2k_list_clear(sd->list);
    for (int g = 0; g < 2; g++) {
        int r = w2k_list_add(sd->list, ICO_NONE, (void *)(intptr_t)0);
        w2k_list_set(sd->list, r, 0, g ? "Windows Explorer" : "Windows");
        for (int ev = 0; ev < N_SOUNDS; ev++) {
            if (w2k_sound_group(ev) != g) continue;
            char path[1200], label[80];
            int has = w2k_sound_file(ev, path, sizeof path);
            snprintf(label, sizeof label, "    %s", w2k_sound_label(ev));
            r = w2k_list_add(sd->list, has ? ICO_SPEAKER : ICO_NONE, (void *)(intptr_t)(ev + 1));
            w2k_list_set(sd->list, r, 0, label);
        }
    }
    sd->list->sel = keep;
}

/* The Name box: (None), the pack's files, and the event's own file when
 * it is one from elsewhere. */
static void snd_fill_name(SndDlg *sd)
{
    sd->fill = 1;
    w2k_combo_clear(sd->name);
    sd->nfiles = w2k_sound_pack_files(w2k_sound_pack, sd->files, 63);
    w2k_combo_add(sd->name, "(None)");
    for (int i = 0; i < sd->nfiles; i++) w2k_combo_add(sd->name, sd->files[i]);
    int sel = 0;
    if (sd->cur >= 0) {
        char path[1200];
        if (w2k_sound_file(sd->cur, path, sizeof path)) {
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            const char *ov = w2k_sound_override[sd->cur];
            if (ov[0] == '/') {
                snprintf(sd->files[sd->nfiles], 128, "%s", ov);
                w2k_combo_add(sd->name, base);
                sel = ++sd->nfiles;
            } else {
                for (int i = 0; i < sd->nfiles; i++)
                    if (!strcmp(sd->files[i], base)) sel = i + 1;
            }
        }
    }
    sd->name->sel = sel;
    sd->fill = 0;
}

static void snd_on_select(void *u, int idx)
{
    SndDlg *sd = u;
    sd->cur = snd_row_event(sd, idx);
    snd_fill_name(sd);
    w2k_win_dirty(sd->win);
}

static void snd_on_name(void *u, int i)
{
    SndDlg *sd = u;
    if (sd->fill || sd->cur < 0) return;
    char *ov = w2k_sound_override[sd->cur];
    if (i <= 0) {
        snprintf(ov, 256, "none");
    } else if (i - 1 < sd->nfiles) {
        const char *f = sd->files[i - 1];
        /* The pack's own choice is recorded as nothing, so a change of
         * pack changes it too. */
        if (!strcmp(f, w2k_sound_default(sd->cur, w2k_sound_pack))) ov[0] = 0;
        else snprintf(ov, 256, "%s", f);
    }
    sd->dirty = 1;
    snd_fill_list(sd);
    w2k_win_dirty(sd->win);
}

static void snd_on_scheme(void *u, int i)
{
    SndDlg *sd = u;
    if (i < 0 || i >= sd->npacks) return;
    snprintf(w2k_sound_pack, sizeof w2k_sound_pack, "%s", sd->pack_ids[i]);
    memset(w2k_sound_override, 0, sizeof w2k_sound_override);
    sd->dirty = 1;
    snd_fill_list(sd);
    snd_fill_name(sd);
    w2k_win_dirty(sd->win);
}

static void snd_on_volume(void *u, int pos)
{
    SndDlg *sd = u;
    w2k_sound_volume = pos * 10;
    sd->dirty = 1;
    w2k_win_dirty(sd->win);
}

static void snd_on_beep(void *u, int pos)
{
    SndDlg *sd = u;
    (void)pos;
    sd->dirty = 1;
    w2k_win_dirty(sd->win);
}

static void snd_commit(SndDlg *sd)
{
    w2k_bell_on = sd->beep_on;
    w2k_bell_volume = sd->bvol.pos * 10;
    w2k_bell_pitch = 100 + sd->bpitch.pos * 100;
    w2k_bell_duration = 20 + sd->bdur.pos * 40;
    w2k_input_apply();
    w2k_scheme_save(NULL);
    w2k_scheme_broadcast();
    sd->dirty = 0;
}

static void snd_paint(W2kWin *w, Drawable d)
{
    SndDlg *sd = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_tabs_draw(d, sd->tabs);
    W2kRect c = w2k_tabs_client(sd->tabs);

    if (sd->tabs->sel == 0) {
        w2k_text_mnemonic(d, F_UI, c.x + 9, c.y + 8, "Sound &Events:", C_TEXT, 1);
        w2k_list_draw(d, sd->list);

        W2kRect g = { c.x + 9, sd->list->r.y + sd->list->r.h + 8, c.w - 18, 64 };
        w2k_draw_groupbox(d, &g, "Sound");
        w2k_text_mnemonic(d, F_UI, g.x + 10, sd->name->r.y - fh - 3, "&Name:", C_TEXT, 1);
        w2k_combo_draw(d, sd->name);
        /* The play button: a small triangle, as the applet's. */
        w2k_button(d, sd->play.x, sd->play.y, sd->play.w, sd->play.h, sd->down == 5);
        int px = sd->play.x + sd->play.w / 2 - 3 + (sd->down == 5), py = sd->play.y + sd->play.h / 2 - 5 + (sd->down == 5);
        for (int i = 0; i < 6; i++) w2k_vline(d, px + i, py + i, 11 - 2 * i, sd->cur >= 0 ? C_TEXT : C_GRAYTEXT);
        w2k_draw_pushbutton(d, &sd->browse, "&Browse...",
                            (sd->cur < 0 ? BS_DISABLED : 0) | (sd->down == 6 ? BS_PRESSED : 0));

        int sy = g.y + g.h + 10;
        w2k_text_mnemonic(d, F_UI, c.x + 9, sy, "&Scheme:", C_TEXT, 1);
        w2k_combo_draw(d, sd->scheme);
        w2k_draw_pushbutton(d, &sd->saveas, "Sa&ve As...", BS_DISABLED);
        w2k_draw_pushbutton(d, &sd->del, "&Delete", BS_DISABLED);

        W2kRect v = { c.x + 9, sd->del.y + sd->del.h + 12, c.w - 18, 60 };
        w2k_draw_groupbox(d, &v, "Sound Volume");
        w2k_text(d, F_UI, v.x + 10, sd->vol.r.y + 4, "Low", C_TEXT);
        w2k_slider_draw(d, &sd->vol);
        w2k_text(d, F_UI, v.x + v.w - 10 - w2k_text_width(F_UI, "High", -1), sd->vol.r.y + 4, "High", C_TEXT);
    } else {
        int y = c.y + 14;
        w2k_draw_checkbox(d, sd->beep_box.x, sd->beep_box.y, "Play the system &beep",
                          sd->beep_on, 0, 0);
        y = sd->bvol.r.y - fh - 4;
        w2k_text_mnemonic(d, F_UI, c.x + 9, y, "&Volume:", C_TEXT, 1);
        w2k_slider_draw(d, &sd->bvol);
        w2k_text_mnemonic(d, F_UI, c.x + 9, sd->bpitch.r.y - fh - 4, "&Pitch:", C_TEXT, 1);
        w2k_slider_draw(d, &sd->bpitch);
        w2k_text_mnemonic(d, F_UI, c.x + 9, sd->bdur.r.y - fh - 4, "&Duration:", C_TEXT, 1);
        w2k_slider_draw(d, &sd->bdur);
        w2k_text(d, F_UI, c.x + 9, c.y + c.h - fh * 2 - 12,
                 "The beep is the X server's; the events on the Sounds page", C_GRAYTEXT);
        w2k_text(d, F_UI, c.x + 9, c.y + c.h - fh - 8,
                 "play through the sound card.", C_GRAYTEXT);
    }
    w2k_draw_pushbutton(d, &sd->ok, "OK", BS_DEFAULT | (sd->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &sd->cancel, "Cancel", sd->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &sd->apply, "&Apply",
                        (sd->dirty ? 0 : BS_DISABLED) | (sd->down == 3 ? BS_PRESSED : 0));
}

static void snd_play_current(SndDlg *sd)
{
    char path[1200];
    if (sd->cur >= 0 && w2k_sound_file(sd->cur, path, sizeof path))
        w2k_sound_play_file(path);
}

static void snd_browse(SndDlg *sd)
{
    if (sd->cur < 0) return;
    char path[1024];
    char dir[1024];
    if (w2k_sound_pack_dir(w2k_sound_pack, dir, sizeof dir))
        snprintf(path, sizeof path, "%s/", dir);
    else
        snprintf(path, sizeof path, "%s/", getenv("HOME") ? getenv("HOME") : "/");
    if (!w2k_file_dialog(sd->win, 0, path, sizeof path)) return;
    snprintf(w2k_sound_override[sd->cur], 256, "%s", path);
    sd->dirty = 1;
    snd_fill_list(sd);
    snd_fill_name(sd);
}

static int snd_event(W2kWin *w, XEvent *e)
{
    SndDlg *sd = w->user;
    int tab = sd->tabs->sel;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_tabs_press(sd->tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (tab == 0) {
            if (w2k_list_press(sd->list, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_combo_press(sd->name, &e->xbutton) ||
                w2k_combo_press(sd->scheme, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_slider_press(&sd->vol, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_rect_hit(&sd->play, x, y) && sd->cur >= 0) sd->down = 5;
            else if (w2k_rect_hit(&sd->browse, x, y) && sd->cur >= 0) sd->down = 6;
        } else {
            if (w2k_rect_hit(&sd->beep_box, x, y)) {
                sd->beep_on = !sd->beep_on;
                sd->dirty = 1;
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_slider_press(&sd->bvol, &e->xbutton) ||
                w2k_slider_press(&sd->bpitch, &e->xbutton) ||
                w2k_slider_press(&sd->bdur, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        }
        if (w2k_rect_hit(&sd->ok, x, y)) sd->down = 1;
        else if (w2k_rect_hit(&sd->cancel, x, y)) sd->down = 2;
        else if (w2k_rect_hit(&sd->apply, x, y) && sd->dirty) sd->down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = sd->down, x = e->xbutton.x, y = e->xbutton.y;
        sd->down = 0;
        w2k_list_release(sd->list, &e->xbutton);
        w2k_slider_release(&sd->vol);
        w2k_slider_release(&sd->bvol);
        w2k_slider_release(&sd->bpitch);
        w2k_slider_release(&sd->bdur);
        if (b == 1 && w2k_rect_hit(&sd->ok, x, y)) { snd_commit(sd); w2k_win_close(w, ID_OK); }
        else if (b == 2 && w2k_rect_hit(&sd->cancel, x, y)) w2k_win_close(w, ID_CANCEL);
        else if (b == 3 && w2k_rect_hit(&sd->apply, x, y)) snd_commit(sd);
        else if (b == 5 && w2k_rect_hit(&sd->play, x, y)) snd_play_current(sd);
        else if (b == 6 && w2k_rect_hit(&sd->browse, x, y)) snd_browse(sd);
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (tab == 0) {
            if (w2k_slider_motion(&sd->vol, &e->xmotion) ||
                w2k_list_motion(sd->list, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        } else if (w2k_slider_motion(&sd->bvol, &e->xmotion) ||
                   w2k_slider_motion(&sd->bpitch, &e->xmotion) ||
                   w2k_slider_motion(&sd->bdur, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { snd_commit(sd); w2k_win_close(w, ID_OK); return 1; }
        if (w2k_tabs_key(sd->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (tab == 0 && w2k_list_key(sd->list, &e->xkey)) {
            sd->cur = snd_row_event(sd, sd->list->sel);
            snd_fill_name(sd);
            w2k_win_dirty(w);
            return 1;
        }
        return 1;
    }
    }
    return 0;
}

static void open_sounds(void)
{
    SndDlg sd;
    memset(&sd, 0, sizeof sd);
    sd.cur = -1;
    int cw = 400, chh = 470;
    W2kWin *w = w2k_win_new("Sounds and Multimedia Properties", "l2kcontrol", cw, chh, 0);
    sd.win = w;
    sd.tabs = w2k_tabs_new(&sd, NULL);
    w2k_tabs_add(sd.tabs, "Sounds");
    w2k_tabs_add(sd.tabs, "System Beep");
    sd.tabs->r = (W2kRect){ 7, 7, cw - 14, chh - 7 - 41 };
    W2kRect c = w2k_tabs_client(sd.tabs);

    sd.list = w2k_list_new(LV_REPORT);
    sd.list->hdr_h = 0;
    sd.list->fullrow = 1;
    sd.list->focused = 1;
    sd.list->user = &sd;
    sd.list->on_select = snd_on_select;
    w2k_list_add_col(sd.list, "Event", c.w - 18 - SCROLL_W - 6, 0);
    sd.list->r = (W2kRect){ c.x + 9, c.y + 24, c.w - 18, 150 };
    w2k_scroll_bind(&sd.list->vsb, w);

    int gy = sd.list->r.y + sd.list->r.h + 8;
    sd.name = w2k_combo_new(0);
    sd.name->user = &sd;
    sd.name->on_change = snd_on_name;
    sd.name->r = (W2kRect){ c.x + 19, gy + 34, c.w - 38 - 30 - 75 - 12, 21 };
    sd.play = (W2kRect){ sd.name->r.x + sd.name->r.w + 6, gy + 34, 24, 21 };
    sd.browse = (W2kRect){ sd.play.x + sd.play.w + 6, gy + 34, 75, 21 };

    int sy = gy + 64 + 10;
    sd.scheme = w2k_combo_new(0);
    sd.scheme->user = &sd;
    sd.scheme->on_change = snd_on_scheme;
    sd.scheme->r = (W2kRect){ c.x + 9, sy + 16, c.w - 18 - 75 * 2 - 12, 21 };
    sd.saveas = (W2kRect){ sd.scheme->r.x + sd.scheme->r.w + 6, sy + 16, 75, 21 };
    sd.del = (W2kRect){ sd.saveas.x + 75 + 6, sy + 16, 75, 21 };
    sd.npacks = w2k_sound_packs(sd.pack_ids, sd.pack_labels, 24);
    for (int i = 0; i < sd.npacks; i++) {
        w2k_combo_add(sd.scheme, sd.pack_labels[i]);
        if (!strcmp(sd.pack_ids[i], w2k_sound_pack)) sd.scheme->sel = i;
    }
    if (!sd.npacks) w2k_combo_add(sd.scheme, "(no sound packs installed)");

    int vy = sd.del.y + sd.del.h + 12;
    sd.vol = (W2kSlider){ .r = { c.x + 9 + 40, vy + 22, c.w - 18 - 80, 24 },
                          .lo = 0, .hi = 10, .ticks = 10, .pos = w2k_sound_volume / 10,
                          .owner = w, .user = &sd, .on_change = snd_on_volume };

    /* The beep page. */
    sd.beep_on = w2k_bell_on;
    sd.beep_box = (W2kRect){ c.x + 9, c.y + 14, c.w - 18, 16 };
    int by = c.y + 60;
    sd.bvol = (W2kSlider){ .r = { c.x + 9, by, c.w - 18, 24 }, .lo = 0, .hi = 10, .ticks = 10,
                           .pos = w2k_bell_volume / 10, .owner = w, .user = &sd, .on_change = snd_on_beep };
    sd.bpitch = (W2kSlider){ .r = { c.x + 9, by + 60, c.w - 18, 24 }, .lo = 0, .hi = 10, .ticks = 10,
                             .pos = (w2k_bell_pitch - 100) / 100, .owner = w, .user = &sd, .on_change = snd_on_beep };
    sd.bdur = (W2kSlider){ .r = { c.x + 9, by + 120, c.w - 18, 24 }, .lo = 0, .hi = 10, .ticks = 10,
                           .pos = (w2k_bell_duration - 20) / 40, .owner = w, .user = &sd, .on_change = snd_on_beep };

    int bby = chh - 12 - 23;
    sd.apply  = (W2kRect){ cw - 12 - 75, bby, 75, 23 };
    sd.cancel = (W2kRect){ cw - 12 - 75 * 2 - 6, bby, 75, 23 };
    sd.ok     = (W2kRect){ cw - 12 - 75 * 3 - 12, bby, 75, 23 };

    snd_fill_list(&sd);
    snd_fill_name(&sd);

    w->user = &sd;
    w->paint = snd_paint;
    w->event = snd_event;
    w2k_win_center(w, cp.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    sd_active = &sd;
    int rc = w2k_win_modal(w);
    sd_active = NULL;
    if (rc != ID_OK) w2k_scheme_load(NULL);        /* discard what was not applied */
    w2k_list_free(sd.list);
    w2k_combo_free(sd.name);
    w2k_combo_free(sd.scheme);
    w2k_tabs_free(sd.tabs);
}

/* ------------------------------------------------------------------ *
 * Fonts
 *
 * The Fonts folder and the font viewer behind it: the family name, the
 * alphabet, and the pangram at the sizes Windows shows. The faces come
 * from fontconfig and are opened by name (see w2k_face_open).
 * ------------------------------------------------------------------ */
#define FONT_MAX 512

static struct {
    W2kList *list;
    char     name[FONT_MAX][96];
    int      n;
    W2kWin  *win;
} fonts;

static const int preview_sizes[] = { 12, 18, 24, 36, 48 };
#define N_PREVIEW ((int)(sizeof preview_sizes / sizeof *preview_sizes))

typedef struct { char family[96]; W2kFace *face[N_PREVIEW]; W2kRect done; int down; }
FontView;

static void fontview_paint(W2kWin *w, Drawable d)
{
    FontView *fv = w->user;
    int fh = w2k_font_height(F_UI);
    int y = 12;
    w2k_text(d, F_UI_BOLD, 12, y, fv->family, C_TEXT);
    y += fh + 6;
    w2k_hline(d, 12, y, w->w - 24, C_SHADOW);
    w2k_hline(d, 12, y + 1, w->w - 24, C_HILIGHT);
    y += 8;

    /* The alphabet, then the pangram at each size -- the layout of the
     * Windows font viewer. */
    static const char *const rows[] = {
        "abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "1234567890.:,;'\"(!?)+-*/=",
    };
    for (int i = 0; i < 3; i++) {
        if (fv->face[0]) {
            w2k_face_text(d, fv->face[0], 12, y, rows[i], C_TEXT);
            y += w2k_face_height(fv->face[0]) + 2;
        }
    }
    y += 6;
    w2k_hline(d, 12, y, w->w - 24, C_SHADOW);
    w2k_hline(d, 12, y + 1, w->w - 24, C_HILIGHT);
    y += 8;

    for (int i = 0; i < N_PREVIEW; i++) {
        if (!fv->face[i]) continue;
        char line[128];
        snprintf(line, sizeof line,
                 "%d  The quick brown fox jumps over the lazy dog.",
                 preview_sizes[i]);
        w2k_clip_set(0, y, w->w, w->h - y - 44);
        w2k_face_text(d, fv->face[i], 12, y, line, C_TEXT);
        w2k_clip_clear();
        y += w2k_face_height(fv->face[i]) + 8;
    }
    w2k_draw_pushbutton(d, &fv->done, "&Done",
                        BS_DEFAULT | (fv->down ? BS_PRESSED : 0));
}

static int fontview_event(W2kWin *w, XEvent *e)
{
    FontView *fv = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_rect_hit(&fv->done, e->xbutton.x, e->xbutton.y)) fv->down = 1;
        w2k_win_dirty(w);
        return 1;
    case ButtonRelease:
        if (fv->down && w2k_rect_hit(&fv->done, e->xbutton.x, e->xbutton.y)) {
            w2k_win_close(w, ID_OK);
            return 1;
        }
        fv->down = 0;
        w2k_win_dirty(w);
        return 1;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape || ks == XK_Return) { w2k_win_close(w, ID_OK); return 1; }
        return 1;
    }
    }
    return 0;
}

static void open_fontview(const char *family)
{
    FontView fv;
    memset(&fv, 0, sizeof fv);
    snprintf(fv.family, sizeof fv.family, "%s", family);
    for (int i = 0; i < N_PREVIEW; i++)
        fv.face[i] = w2k_face_open(family, preview_sizes[i]);

    int cw = 560, chh = 420;
    W2kWin *w = w2k_win_new(family, "l2kcontrol", cw, chh, 1);
    fv.done = (W2kRect){ cw - 12 - 75, chh - 12 - 23, 75, 23 };
    w->user = &fv;
    w->paint = fontview_paint;
    w->event = fontview_event;
    w2k_win_center(w, cp.win);
    w2k_win_modal(w);
    for (int i = 0; i < N_PREVIEW; i++) w2k_face_close(fv.face[i]);
}

static int font_name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void fonts_scan(void)
{
    fonts.n = 0;
    FcInit();
    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, (char *)NULL);
    FcFontSet *set = FcFontList(NULL, pat, os);
    FcPatternDestroy(pat);
    FcObjectSetDestroy(os);
    if (!set) return;
    for (int i = 0; i < set->nfont && fonts.n < FONT_MAX; i++) {
        FcChar8 *fam = NULL;
        if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch)
            continue;
        int dup = 0;
        for (int k = 0; k < fonts.n; k++)
            if (!strcasecmp(fonts.name[k], (const char *)fam)) { dup = 1; break; }
        if (!dup) snprintf(fonts.name[fonts.n++], 96, "%s", (const char *)fam);
    }
    FcFontSetDestroy(set);
    qsort(fonts.name, (size_t)fonts.n, sizeof fonts.name[0], font_name_cmp);
}

static void fonts_activate(void *u, int idx)
{
    (void)u;
    if (idx >= 0 && idx < fonts.n) open_fontview(fonts.name[idx]);
}

static void fonts_paint(W2kWin *w, Drawable d)
{
    w2k_list_draw(d, fonts.list);
    (void)w;
}

static int fonts_event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress:
        if (w2k_list_press(fonts.list, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        return 1;
    case ButtonRelease:
        w2k_list_release(fonts.list, &e->xbutton);
        return 1;
    case MotionNotify:
        if (w2k_list_motion(fonts.list, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (w2k_list_key(fonts.list, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void fonts_resized(W2kWin *w)
{
    fonts.list->r = (W2kRect){ 0, 0, w->w, w->h };
    w2k_list_layout(fonts.list);
}

static void open_fonts(void)
{
    fonts_scan();

    W2kWin *w = w2k_win_new("Fonts", "l2kcontrol", 520, 400, 1);
    fonts.win = w;
    fonts.list = w2k_list_new(LV_ICON);
    fonts.list->on_activate = fonts_activate;
    fonts.list->multisel = 0;
    fonts.list->singleclick = w2k_folder_singleclick;
    w2k_scroll_bind(&fonts.list->vsb, w);
    for (int i = 0; i < fonts.n; i++) {
        int r = w2k_list_add(fonts.list, ICO_FILE_FONT, NULL);
        w2k_list_set(fonts.list, r, 0, fonts.name[i]);
    }
    w->paint = fonts_paint;
    w->event = fonts_event;
    w->resized = fonts_resized;
    fonts_resized(w);
    w2k_win_center(w, cp.win);
    w2k_win_modal(w);
    w2k_list_free(fonts.list);
    fonts.list = NULL;
}

/* ------------------------------------------------------------------ *
 * The folder of applets
 * ------------------------------------------------------------------ */
static void spawn(const char *cmd)
{
    if (!cmd) return;
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        }
        _exit(127);
    }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
}

/* The Start menu's own settings belong to the window manager -- it draws
 * the menu -- so the applet asks it to put the dialog up. */
static void wm_command(long code)
{
    XEvent e = { 0 };
    e.xclient.type = ClientMessage;
    e.xclient.window = w2k.root;
    e.xclient.message_type = w2k.a_w2k_command;
    e.xclient.format = 32;
    e.xclient.data.l[0] = code;
    XSendEvent(w2k.dpy, w2k.root, False, SubstructureNotifyMask, &e);
    XFlush(w2k.dpy);
}


/* ------------------------------------------------------------------ *
 * Date/Time Properties
 *
 * The Windows 2000 dialog: a month calendar and an analogue clock on the
 * Date & Time tab, the zone on the other. Laid out from a screenshot of
 * the original, 404 by 341 with its frame: the Date group at (16,38),
 * 182 by 196, the Time group beside it, the month list and year spinner
 * on row 58, the calendar well 158 by 128 at (29,89), the clock 62 in
 * radius about (291,121), the time field on row 200, and OK, Cancel and
 * Apply along row 286. Setting the clock is timedatectl's job, and it
 * asks for authorisation through polkit; when it refuses, the dialog
 * shows what it said.
 * ------------------------------------------------------------------ */
typedef struct {
    W2kWin   *win;
    W2kTabs  *tabs;
    W2kCombo *month, *zone;
    W2kEdit  *year, *timef;
    struct tm t;                 /* the date and time shown */
    int       live;              /* still following the system clock */
    int       held;              /* a field clicked into: the clock stops */
    int       focus;             /* 1 the year, 2 the time */
    int       down;              /* 1 OK, 2 Cancel, 3 Apply, 4..7 the spinners */
    int       dst, zone_sel, zone_was;
    char      zone_name[64];
    W2kRect   ok, cancel, apply, yup, ydn, tup, tdn, dst_r, cal;
} DtDlg;

static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"
};

static int days_in_month(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m];
}

static void dt_sync_fields(DtDlg *dt)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%d", dt->t.tm_year + 1900);
    w2k_edit_set(dt->year, buf);
    int h12 = dt->t.tm_hour % 12;
    if (!h12) h12 = 12;
    snprintf(buf, sizeof buf, "%d:%02d:%02d %s", h12, dt->t.tm_min, dt->t.tm_sec,
             dt->t.tm_hour < 12 ? "AM" : "PM");
    w2k_edit_set(dt->timef, buf);
    dt->month->sel = dt->t.tm_mon;
}

static void dt_tick(void *u)
{
    DtDlg *dt = u;
    if (!dt->live || dt->held) return;
    time_t now = time(NULL);
    localtime_r(&now, &dt->t);
    dt_sync_fields(dt);
    w2k_win_dirty(dt->win);
}

static void dt_on_tab(void *u, int i) { (void)i; w2k_win_dirty(((DtDlg *)u)->win); }

static void dt_on_month(void *u, int i)
{
    DtDlg *dt = u;
    dt->t.tm_mon = i;
    int n = days_in_month(dt->t.tm_year + 1900, i);
    if (dt->t.tm_mday > n) dt->t.tm_mday = n;
    dt->live = 0;
    w2k_win_dirty(dt->win);
}

static void dt_on_zone(void *u, int i)
{
    DtDlg *dt = u;
    dt->zone_sel = i;
    w2k_win_dirty(dt->win);
}

/* The current zone, as /etc/localtime names it: "America/New_York". */
static void dt_current_zone(char *out, int n)
{
    char link[512];
    ssize_t len = readlink("/etc/localtime", link, sizeof link - 1);
    out[0] = 0;
    if (len > 0) {
        link[len] = 0;
        const char *p = strstr(link, "zoneinfo/");
        if (p) snprintf(out, (size_t)n, "%s", p + 9);
    }
    if (!out[0]) {
        FILE *f = fopen("/etc/timezone", "r");
        if (f) {
            if (fgets(out, n, f)) out[strcspn(out, "\r\n")] = 0;
            fclose(f);
        }
    }
}

static void dt_draw_spin(Drawable d, const W2kRect *up, const W2kRect *dn, int pressed)
{
    const W2kRect *r[2] = { up, dn };
    for (int i = 0; i < 2; i++) {
        w2k_fill(d, r[i]->x, r[i]->y, r[i]->w, r[i]->h, C_FACE);
        w2k_edge(d, r[i]->x, r[i]->y, r[i]->w, r[i]->h,
                 pressed == i + 1 ? EDGE_SUNKEN : EDGE_BUTTON, BF_RECT);
        int cx = r[i]->x + r[i]->w / 2 + (pressed == i + 1), cy = r[i]->y + r[i]->h / 2 + (pressed == i + 1);
        XSetForeground(w2k.dpy, w2k.gc, w2k.col[C_TEXT]);
        for (int k = 0; k < 3; k++) {
            int yy = i == 0 ? cy - 1 + k : cy + 1 - k;
            XFillRectangle(w2k.dpy, d, w2k.gc, cx - k, yy, (unsigned)(2 * k + 1), 1);
        }
    }
}

static void dt_draw_calendar(Drawable d, DtDlg *dt)
{
    W2kRect c = dt->cal;
    w2k_draw_well(d, &c);
    int x0 = c.x + 2, y0 = c.y + 2, w = c.w - 4;
    w2k_fill(d, x0, y0, w, 17, C_FACE);
    static const char *const dn[7] = { "S", "M", "T", "W", "T", "F", "S" };
    int fh = w2k_font_height(F_UI);
    for (int i = 0; i < 7; i++) {
        int cx0 = x0 + i * w / 7, cw = (i + 1) * w / 7 - i * w / 7;
        w2k_text_rgb(d, F_UI, cx0 + (cw - w2k_text_width(F_UI, dn[i], -1)) / 2,
                     y0 + (17 - fh) / 2, dn[i], 255, 255, 255);
    }
    struct tm first = dt->t;
    first.tm_mday = 1;
    first.tm_hour = 12;
    mktime(&first);
    int wd = first.tm_wday, n = days_in_month(dt->t.tm_year + 1900, dt->t.tm_mon);
    for (int day = 1; day <= n; day++) {
        int idx = wd + day - 1, row = idx / 7, col = idx % 7;
        int cx0 = x0 + col * w / 7, cw = (col + 1) * w / 7 - col * w / 7;
        int cy0 = y0 + 17 + row * 18;
        char b[16];
        snprintf(b, sizeof b, "%d", day % 100);
        int tw = w2k_text_width(F_UI, b, -1);
        int tx = cx0 + (cw - tw) / 2, ty = cy0 + (18 - fh) / 2;
        if (day == dt->t.tm_mday) {
            w2k_fill(d, tx - 2, ty, tw + 4, fh, C_HIGHLIGHT);
            w2k_text(d, F_UI, tx, ty, b, C_HIGHLIGHTTEXT);
        } else
            w2k_text(d, F_UI, tx, ty, b, C_TEXT);
    }
}

static void dt_hand(Drawable d, int cx, int cy, double a, int len, int wide)
{
    double sx = sin(a), cy_ = -cos(a);
    XPoint p[4] = {
        { (short)(cx + lround(sx * len)), (short)(cy + lround(cy_ * len)) },
        { (short)(cx + lround(-cy_ * wide)), (short)(cy + lround(sx * wide)) },
        { (short)(cx - lround(sx * 6)), (short)(cy - lround(cy_ * 6)) },
        { (short)(cx - lround(-cy_ * wide)), (short)(cy - lround(sx * wide)) },
    };
    XFillPolygon(w2k.dpy, d, w2k.gc, p, 4, Convex, CoordModeOrigin);
}

static void dt_draw_clock(Drawable d, DtDlg *dt, int cx, int cy, int r)
{
    /* The ring: a dot a minute, a square on the hour. */
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(0, 128, 128));
    for (int i = 0; i < 60; i++) {
        double a = i * M_PI / 30;
        int x = cx + (int)lround(sin(a) * (r - 4)), y = cy - (int)lround(cos(a) * (r - 4));
        int s = i % 5 ? 1 : 3;
        XFillRectangle(w2k.dpy, d, w2k.gc, x - s / 2, y - s / 2, (unsigned)s, (unsigned)s);
    }
    double h = ((dt->t.tm_hour % 12) + dt->t.tm_min / 60.0) * M_PI / 6;
    double m = (dt->t.tm_min + dt->t.tm_sec / 60.0) * M_PI / 30;
    double sec = dt->t.tm_sec * M_PI / 30;
    dt_hand(d, cx, cy, h, r * 5 / 10, 4);
    dt_hand(d, cx, cy, m, r * 3 / 4, 3);
    XDrawLine(w2k.dpy, d, w2k.gc, cx, cy, cx + (int)lround(sin(sec) * r * 0.8),
              cy - (int)lround(cos(sec) * r * 0.8));
    XFillArc(w2k.dpy, d, w2k.gc, cx - 3, cy - 3, 7, 7, 0, 360 * 64);
}

static void dt_paint(W2kWin *w, Drawable d)
{
    DtDlg *dt = w->user;
    w2k_tabs_draw(d, dt->tabs);
    int fh = w2k_font_height(F_UI);
    if (dt->tabs->sel == 0) {
        W2kRect g1 = { 16, 38, 182, 196 }, g2 = { 208, 38, 183, 196 };
        w2k_draw_groupbox(d, &g1, "&Date");
        w2k_draw_groupbox(d, &g2, "&Time");
        w2k_combo_draw(d, dt->month);
        w2k_edit_draw(d, dt->year);
        dt_draw_spin(d, &dt->yup, &dt->ydn, dt->down == 4 ? 1 : dt->down == 5 ? 2 : 0);
        dt_draw_calendar(d, dt);
        dt_draw_clock(d, dt, 291, 121, 62);
        w2k_edit_draw(d, dt->timef);
        dt_draw_spin(d, &dt->tup, &dt->tdn, dt->down == 6 ? 1 : dt->down == 7 ? 2 : 0);
        char z[200];
        snprintf(z, sizeof z, "Current time zone:  %s", dt->zone_name[0] ? dt->zone_name : "(unknown)");
        w2k_text(d, F_UI, 17, 254, z, C_TEXT);
    } else {
        w2k_combo_draw(d, dt->zone);
        w2k_draw_checkbox(d, dt->dst_r.x, dt->dst_r.y,
                          "&Automatically adjust clock for daylight saving changes",
                          dt->dst, 0, 0);
        char info[160];
        strftime(info, sizeof info, "Current time:  %A, %d %B %Y, %H:%M:%S %Z (UTC%z)", &dt->t);
        w2k_text(d, F_UI, 16, 78 + fh + 16, info, C_TEXT);
    }
    w2k_draw_pushbutton(d, &dt->ok, "OK", BS_DEFAULT | (dt->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &dt->cancel, "Cancel", dt->down == 2 ? BS_PRESSED : 0);
    /* Apply comes alive once something has been changed, as in Windows. */
    int changed = !dt->live || dt->zone_sel != dt->zone_was;
    w2k_draw_pushbutton(d, &dt->apply, "&Apply",
                        (dt->down == 3 ? BS_PRESSED : 0) | (changed ? 0 : BS_DISABLED));
}

/* What was typed into the year and time fields, into dt->t. */
static void dt_read_fields(DtDlg *dt)
{
    int y = atoi(w2k_edit_text(dt->year));
    if (y >= 1970 && y <= 2099 && y != dt->t.tm_year + 1900) {
        dt->t.tm_year = y - 1900;
        dt->live = 0;
    }
    int h, m, s;
    char ap[3] = "";
    if (sscanf(w2k_edit_text(dt->timef), "%d:%d:%d %2s", &h, &m, &s, ap) >= 3 &&
        h >= 0 && h <= 23 && m >= 0 && m < 60 && s >= 0 && s < 60) {
        if (!strcasecmp(ap, "PM") && h < 12) h += 12;
        if (!strcasecmp(ap, "AM") && h == 12) h = 0;
        if (h != dt->t.tm_hour || m != dt->t.tm_min || s != dt->t.tm_sec) {
            dt->t.tm_hour = h; dt->t.tm_min = m; dt->t.tm_sec = s;
            dt->live = 0;
        }
    }
}

/* Read the fields back, then hand the clock and the zone to timedatectl. */
static void dt_apply(DtDlg *dt)
{
    dt_read_fields(dt);
    char out[1024] = "";
    if (dt->zone_sel != dt->zone_was && dt->zone_sel >= 0 && dt->zone_sel < dt->zone->n) {
        char cmd[400];
        snprintf(cmd, sizeof cmd, "timedatectl set-timezone '%.200s' 2>&1",
                 dt->zone->items[dt->zone_sel]);
        FILE *p = popen(cmd, "r");
        if (p) { size_t n = fread(out, 1, sizeof out - 1, p); out[n] = 0; pclose(p); }
        if (!out[0]) {
            dt->zone_was = dt->zone_sel;
            snprintf(dt->zone_name, sizeof dt->zone_name, "%.63s", dt->zone->items[dt->zone_sel]);
            tzset();
        }
    }
    if (!dt->live && !out[0]) {
        char cmd[200];
        snprintf(cmd, sizeof cmd, "timedatectl set-time '%04d-%02d-%02d %02d:%02d:%02d' 2>&1",
                 dt->t.tm_year + 1900, dt->t.tm_mon + 1, dt->t.tm_mday,
                 dt->t.tm_hour, dt->t.tm_min, dt->t.tm_sec);
        FILE *p = popen(cmd, "r");
        if (p) { size_t n = fread(out, 1, sizeof out - 1, p); out[n] = 0; pclose(p); }
    }
    if (out[0]) {
        char msg[1200];
        snprintf(msg, sizeof msg, "The date and time could not be set.\n\n%s", out);
        w2k_msgbox(dt->win, "Date/Time Properties", msg, MB_OK | MB_ICONWARNING);
    }
    dt->live = 1;                      /* follow the clock again, set or not */
    dt->held = 0;
    dt_tick(dt);
}

static int dt_event(W2kWin *w, XEvent *e)
{
    DtDlg *dt = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_tabs_press(dt->tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (dt->tabs->sel == 0) {
            if (w2k_combo_press(dt->month, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            /* A click into a field stops the clock, as in Windows: the tick
             * used to write over the field every second while it was being
             * selected or typed into. Nothing is set until a change. */
            if (w2k_edit_press(dt->year, &e->xbutton)) { dt->focus = 1; dt->held = 1; w2k_win_dirty(w); return 1; }
            if (w2k_edit_press(dt->timef, &e->xbutton)) { dt->focus = 2; dt->held = 1; w2k_win_dirty(w); return 1; }
            int bump = 0;
            if (w2k_rect_hit(&dt->yup, x, y)) { dt->down = 4; bump = 1; }
            if (w2k_rect_hit(&dt->ydn, x, y)) { dt->down = 5; bump = 2; }
            if (w2k_rect_hit(&dt->tup, x, y)) { dt->down = 6; bump = 3; }
            if (w2k_rect_hit(&dt->tdn, x, y)) { dt->down = 7; bump = 4; }
            if (bump) {
                /* Typed and not yet applied: the spinner steps from it,
                 * where it used to put the old time back. */
                dt_read_fields(dt);
                dt->live = 0;
                if (bump == 1 && dt->t.tm_year < 199) dt->t.tm_year++;
                if (bump == 2 && dt->t.tm_year > 70) dt->t.tm_year--;
                if (bump == 3) dt->t.tm_hour = (dt->t.tm_hour + 1) % 24;
                if (bump == 4) dt->t.tm_hour = (dt->t.tm_hour + 23) % 24;
                int n = days_in_month(dt->t.tm_year + 1900, dt->t.tm_mon);
                if (dt->t.tm_mday > n) dt->t.tm_mday = n;
                dt_sync_fields(dt);
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_rect_hit(&dt->cal, x, y) && y >= dt->cal.y + 19) {
                int cw = dt->cal.w - 4;
                int col = (x - dt->cal.x - 2) * 7 / cw, row = (y - dt->cal.y - 19) / 18;
                struct tm first = dt->t;
                first.tm_mday = 1; first.tm_hour = 12;
                mktime(&first);
                int day = row * 7 + col - first.tm_wday + 1;
                if (col >= 0 && col < 7 && day >= 1 &&
                    day <= days_in_month(dt->t.tm_year + 1900, dt->t.tm_mon)) {
                    dt->t.tm_mday = day;
                    dt->live = 0;
                    w2k_win_dirty(w);
                }
                return 1;
            }
        } else {
            if (w2k_combo_press(dt->zone, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_rect_hit(&dt->dst_r, x, y)) { dt->dst = !dt->dst; w2k_win_dirty(w); return 1; }
        }
        if (w2k_rect_hit(&dt->ok, x, y)) dt->down = 1;
        else if (w2k_rect_hit(&dt->cancel, x, y)) dt->down = 2;
        else if (w2k_rect_hit(&dt->apply, x, y)) dt->down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int x = e->xbutton.x, y = e->xbutton.y, b = dt->down;
        dt->down = 0;
        w2k_edit_release(dt->year);
        w2k_edit_release(dt->timef);
        if (b == 1 && w2k_rect_hit(&dt->ok, x, y)) { dt_apply(dt); w2k_win_close(w, ID_OK); }
        else if (b == 2 && w2k_rect_hit(&dt->cancel, x, y)) w2k_win_close(w, ID_CANCEL);
        else if (b == 3 && w2k_rect_hit(&dt->apply, x, y)) dt_apply(dt);
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (dt->focus == 1) w2k_edit_motion(dt->year, &e->xmotion);
        if (dt->focus == 2) w2k_edit_motion(dt->timef, &e->xmotion);
        return 1;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { dt_apply(dt); w2k_win_close(w, ID_OK); return 1; }
        if (w2k_tabs_key(dt->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (dt->focus == 1 && w2k_edit_key(dt->year, &e->xkey)) { dt->live = 0; w2k_win_dirty(w); }
        if (dt->focus == 2 && w2k_edit_key(dt->timef, &e->xkey)) { dt->live = 0; w2k_win_dirty(w); }
        return 1;
    }
    }
    return 0;
}

static void open_datetime(void)
{
    DtDlg dt;
    memset(&dt, 0, sizeof dt);
    dt.win = w2k_win_new("Date/Time Properties", "l2kcontrol", 398, 316, 0);
    dt.tabs = w2k_tabs_new(&dt, dt_on_tab);
    w2k_tabs_add(dt.tabs, "Date && Time");
    w2k_tabs_add(dt.tabs, "Time Zone");
    dt.tabs->r = (W2kRect){ 7, 7, 384, 267 };

    dt.month = w2k_combo_new(0);
    for (int i = 0; i < 12; i++) w2k_combo_add(dt.month, month_names[i]);
    dt.month->r = (W2kRect){ 27, 58, 82, 21 };
    dt.month->user = &dt;
    dt.month->on_change = dt_on_month;
    dt.year = w2k_edit_new(0);
    dt.year->r = (W2kRect){ 114, 58, 60, 21 };
    w2k_edit_bind(dt.year, dt.win);
    dt.timef = w2k_edit_new(0);
    dt.timef->r = (W2kRect){ 255, 200, 78, 21 };
    w2k_edit_bind(dt.timef, dt.win);
    dt.yup = (W2kRect){ 174, 58, 15, 11 };  dt.ydn = (W2kRect){ 174, 69, 15, 10 };
    dt.tup = (W2kRect){ 333, 200, 15, 11 }; dt.tdn = (W2kRect){ 333, 211, 15, 10 };
    dt.cal = (W2kRect){ 29, 89, 158, 128 };
    dt.ok = (W2kRect){ 155, 286, 72, 23 };
    dt.cancel = (W2kRect){ 227, 286, 72, 23 };
    dt.apply = (W2kRect){ 309, 286, 74, 23 };

    /* The zones, as the tz database lists them; the current one selected. */
    dt.zone = w2k_combo_new(0);
    dt.zone->r = (W2kRect){ 16, 45, 375, 21 };
    dt.zone->user = &dt;
    dt.zone->on_change = dt_on_zone;
    dt_current_zone(dt.zone_name, sizeof dt.zone_name);
    dt.zone_sel = dt.zone_was = -1;
    FILE *f = fopen("/usr/share/zoneinfo/zone1970.tab", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            char *tab = strchr(line, '\t');
            tab = tab ? strchr(tab + 1, '\t') : NULL;
            if (!tab) continue;
            char *tz = tab + 1;
            tz[strcspn(tz, "\t\r\n")] = 0;
            if (!strcmp(tz, dt.zone_name)) dt.zone_sel = dt.zone_was = dt.zone->n;
            w2k_combo_add(dt.zone, tz);
        }
        fclose(f);
    }
    if (dt.zone_sel < 0 && dt.zone_name[0]) {
        w2k_combo_add(dt.zone, dt.zone_name);
        dt.zone_sel = dt.zone_was = dt.zone->n - 1;
    }
    dt.zone->sel = dt.zone_sel;
    int fh = w2k_font_height(F_UI);
    dt.dst_r = (W2kRect){ 16, 78, 375, fh + 4 };

    time_t now = time(NULL);
    localtime_r(&now, &dt.t);
    dt.dst = dt.t.tm_isdst > 0;
    dt.live = 1;
    dt_sync_fields(&dt);

    dt.win->user = &dt;
    dt.win->paint = dt_paint;
    dt.win->event = dt_event;
    w2k_win_center(dt.win, cp.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, dt.win->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_add_timer(1000, dt_tick, &dt);
    w2k_win_modal(dt.win);
    w2k_del_timer(dt_tick, &dt);
    w2k_combo_free(dt.month);
    w2k_combo_free(dt.zone);
    w2k_edit_free(dt.year);
    w2k_edit_free(dt.timef);
    w2k_tabs_free(dt.tabs);
}

/* ------------------------------------------------------------------ *
 * Power Options
 *
 * The Windows 2000 applet's Power Meter page -- the battery, what is left
 * in it and where the power is coming from -- and a Brightness page for
 * the screen's backlight, which the original kept in the laptop maker's
 * own software. Shown in Control Panel on machines that have either.
 * Graphics picks the GPU the desktop's programs render on (lib/gpu.c),
 * what NVIDIA's control panel did for a laptop with two.
 * ------------------------------------------------------------------ */
typedef struct {
    W2kWin    *win;
    W2kTabs   *tabs;
    W2kRect    ok, cancel, apply;
    W2kSlider  bright;
    W2kPower   pw;
    int        have_backlight, cur_bright, want_bright;
    int        down, dirty;
    /* Power Schemes: minutes of idleness; Advanced: the lid and the
     * power button, from logind. */
    W2kCombo  *mon_off, *standby, *hibern, *lid, *pbtn;
    char       lid_was[32], pbtn_was[32];
    /* Where each list started: one left alone keeps the machine's own
     * value, even one the list does not offer ("lock", 7 minutes). */
    int        lid_idx, pbtn_idx, mo_idx, sb_idx, hb_idx;
    int        can_suspend, can_hibernate;
    /* Graphics: the processor the desktop's programs draw with; the
     * first is the default one. */
    W2kGpu     gpu[W2K_GPU_MAX];
    W2kRect    gpu_radio[W2K_GPU_MAX];
    int        ngpu, gpu_sel;
} PowerDlg;

/* The times on offer, in minutes; 0 is Never. */
static const int pw_minutes[] = { 0, 1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180 };
#define N_PW_MINUTES ((int)(sizeof pw_minutes / sizeof *pw_minutes))
static const char *const pw_actions[] = { "ignore", "suspend", "hibernate", "poweroff" };
static const char *const pw_action_labels[] = { "Do nothing", "Stand by", "Hibernate", "Shut down" };

static void pw_fill_minutes(W2kCombo *c, int minutes)
{
    for (int i = 0; i < N_PW_MINUTES; i++) {
        char t[32];
        int m = pw_minutes[i];
        if (m == 0)           snprintf(t, sizeof t, "Never");
        else if (m < 60)      snprintf(t, sizeof t, "After %d min%s", m, m == 1 ? "" : "s");
        else                  snprintf(t, sizeof t, "After %d hour%s", m / 60, m == 60 ? "" : "s");
        w2k_combo_add(c, t);
        if (m == minutes) c->sel = i;
    }
    if (c->sel < 0) c->sel = 0;
}

static void pw_fill_actions(W2kCombo *c, const char *current)
{
    for (int i = 0; i < 4; i++) {
        w2k_combo_add(c, pw_action_labels[i]);
        if (!strcmp(pw_actions[i], current)) c->sel = i;
    }
    if (c->sel < 0) c->sel = !strcmp(current, "lock") ? 1 : 0;
}

static void pw_on_change(void *u, int i)
{
    PowerDlg *pd = u;
    (void)i;
    pd->dirty = 1;
    w2k_win_dirty(pd->win);
}

static void pw_refresh(PowerDlg *pd)
{
    w2k_power_read(&pd->pw);
    if (pd->have_backlight && !pd->dirty) {
        int b;
        if (w2k_backlight_get(&b)) { pd->cur_bright = b; pd->bright.pos = b / 5; }
    }
}

static void pw_tick(void *u)
{
    PowerDlg *pd = u;
    pw_refresh(pd);
    w2k_win_dirty(pd->win);
}

static void pw_on_bright(void *u, int pos)
{
    PowerDlg *pd = u;
    pd->want_bright = pos * 5;
    /* Only ever sets it: a slider moved and moved back must not clear
     * what the other tabs have waiting. */
    if (pd->want_bright != pd->cur_bright) pd->dirty = 1;
    w2k_win_dirty(pd->win);
}

static void pw_commit(PowerDlg *pd)
{
    if (!pd->dirty) return;
    int ok = 1;
    /* The scheme's minutes: saved and broadcast, the shell applies them. */
    int mo = pd->mon_off->sel == pd->mo_idx ? w2k_monitor_off_min
           : pw_minutes[pd->mon_off->sel < 0 ? 0 : pd->mon_off->sel];
    int sb = pd->standby->sel == pd->sb_idx ? w2k_standby_min
           : pw_minutes[pd->standby->sel < 0 ? 0 : pd->standby->sel];
    int hb = pd->hibern->sel == pd->hb_idx ? w2k_hibernate_min
           : pw_minutes[pd->hibern->sel < 0 ? 0 : pd->hibern->sel];
    if (mo != w2k_monitor_off_min || sb != w2k_standby_min || hb != w2k_hibernate_min) {
        w2k_monitor_off_min = mo;
        w2k_standby_min = sb;
        w2k_hibernate_min = hb;
        w2k_input_apply();
        w2k_scheme_save(NULL);
        w2k_scheme_broadcast();
    }
    pd->mo_idx = pd->mon_off->sel;
    pd->sb_idx = pd->standby->sel;
    pd->hb_idx = pd->hibern->sel;
    /* The graphics processor: saved and broadcast, and every shell
     * process hands it on to what it starts next. */
    const char *gpu = pd->gpu_sel > 0 ? pd->gpu[pd->gpu_sel].addr : "";
    if (pd->ngpu && strcmp(gpu, w2k_gpu_pref)) {
        snprintf(w2k_gpu_pref, sizeof w2k_gpu_pref, "%s", gpu);
        w2k_scheme_save(NULL);
        w2k_scheme_broadcast();
    }
    /* The lid and the button belong to logind: one administrator prompt
     * writes both. */
    /* Only when one of them was changed here: an untouched list used to
     * turn a value it does not offer into "Do nothing", and ask for the
     * administrator's password to write it, on any Apply. */
    if (pd->lid->sel != pd->lid_idx || pd->pbtn->sel != pd->pbtn_idx) {
        char lid[32], btn[32];
        snprintf(lid, sizeof lid, "%s", pd->lid->sel == pd->lid_idx ? pd->lid_was
                 : pw_actions[pd->lid->sel < 0 ? 0 : pd->lid->sel]);
        snprintf(btn, sizeof btn, "%s", pd->pbtn->sel == pd->pbtn_idx ? pd->pbtn_was
                 : pw_actions[pd->pbtn->sel < 0 ? 0 : pd->pbtn->sel]);
        if (w2k_power_lid_set(lid, btn) == 0) {
            snprintf(pd->lid_was, sizeof pd->lid_was, "%s", lid);
            snprintf(pd->pbtn_was, sizeof pd->pbtn_was, "%s", btn);
            pd->lid_idx = pd->lid->sel;
            pd->pbtn_idx = pd->pbtn->sel;
        } else {
            ok = 0;
            w2k_msgbox(pd->win, "Power Options",
                       "The lid and power button settings could not be written.\n"
                       "They live in /etc/systemd/logind.conf.d and need an\n"
                       "administrator (pkexec).", MB_OK | MB_ICONERROR);
        }
    }
    if (pd->want_bright != pd->cur_bright) {
        if (w2k_backlight_set(pd->want_bright) == 0) pd->cur_bright = pd->want_bright;
        else {
            ok = 0;
            w2k_msgbox(pd->win, "Power Options",
                       "The brightness could not be changed.\n\n"
                       "The installer gives your account the video group and a udev rule\n"
                       "for the backlight: log off and on again for them to take effect\n"
                       "(or run install.sh once more). Otherwise install brightnessctl, or\n"
                       "log on with a PolicyKit agent running so that pkexec can ask.",
                       MB_OK | MB_ICONERROR);
        }
    }
    if (ok) pd->dirty = 0;
}

/* The meter's big battery: a case standing up, filled from the bottom
 * to the charge, the percentage over it. */
static void pw_draw_meter(Drawable d, int x, int y, const W2kPower *p)
{
    int w = 40, h = 64;
    w2k_fill(d, x + 14, y, 12, 4, C_TEXT);                 /* the terminal */
    w2k_edge(d, x, y + 4, w, h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, x + 2, y + 6, w - 4, h - 4, C_WINDOW);
    if (p->present && p->percent >= 0) {
        int fh = (h - 4) * p->percent / 100;
        if (p->percent <= 10 && p->charging == 0)
            w2k_fill_rgb(d, x + 2, y + 6 + (h - 4 - fh), w - 4, fh, 255, 0, 0);
        else
            w2k_fill_rgb(d, x + 2, y + 6 + (h - 4 - fh), w - 4, fh, 0, 128, 0);
        char t[8];
        snprintf(t, sizeof t, "%d%%", p->percent);
        int tw = w2k_text_width(F_UI_BOLD, t, -1);
        w2k_text(d, F_UI_BOLD, x + (w - tw) / 2, y + 4 + (h - w2k_font_height(F_UI_BOLD)) / 2,
                 t, C_TEXT);
    }
}

static void pw_paint(W2kWin *w, Drawable d)
{
    PowerDlg *pd = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_tabs_draw(d, pd->tabs);
    W2kRect c = w2k_tabs_client(pd->tabs);

    if (pd->tabs->sel == 0) {
        /* Power Schemes: the Windows 2000 page's settings, one column. */
        w2k_icon_draw(d, c.x + 12, c.y + 14, ICO_CP_POWER);
        w2k_text(d, F_UI, c.x + 36, c.y + 12, "Select the power settings that suit the way you use", C_TEXT);
        w2k_text(d, F_UI, c.x + 36, c.y + 12 + fh, "this computer. Each is measured from the last key or", C_TEXT);
        w2k_text(d, F_UI, c.x + 36, c.y + 12 + 2 * fh, "mouse movement.", C_TEXT);
        W2kRect g = { c.x + 9, c.y + 66, c.w - 18, 128 };
        w2k_draw_groupbox(d, &g, "Settings for the power scheme");
        w2k_text_mnemonic(d, F_UI, g.x + 10, pd->mon_off->r.y + (21 - fh) / 2, "Turn off &monitor:", C_TEXT, 1);
        w2k_combo_draw(d, pd->mon_off);
        w2k_text_mnemonic(d, F_UI, g.x + 10, pd->standby->r.y + (21 - fh) / 2, "System &stand by:", C_TEXT, 1);
        w2k_combo_draw(d, pd->standby);
        w2k_text_mnemonic(d, F_UI, g.x + 10, pd->hibern->r.y + (21 - fh) / 2, "System &hibernates:", C_TEXT, 1);
        w2k_combo_draw(d, pd->hibern);
        int y = g.y + g.h + 10;
        if (!pd->can_suspend || !pd->can_hibernate) {
            w2k_text(d, F_UI, c.x + 9, y, !pd->can_suspend
                     ? "This computer cannot stand by (systemd-logind says no)."
                     : "This computer cannot hibernate (no swap, or logind says no).", C_GRAYTEXT);
            y += fh + 2;
        }
        w2k_text(d, F_UI, c.x + 9, y, "The monitor is turned off by DPMS; standing by and hibernating", C_GRAYTEXT); y += fh;
        w2k_text(d, F_UI, c.x + 9, y, "go through systemd-logind. Programs playing video may keep the", C_GRAYTEXT); y += fh;
        w2k_text(d, F_UI, c.x + 9, y, "computer awake by inhibiting them.", C_GRAYTEXT);
    } else if (pd->tabs->sel == 1) {
        /* Advanced: what the lid and the power button do. */
        W2kRect g = { c.x + 9, c.y + 10, c.w - 18, 112 };
        w2k_draw_groupbox(d, &g, "Power buttons");
        w2k_text_mnemonic(d, F_UI, g.x + 10, g.y + 20, "When I close the &lid of my portable computer:", C_TEXT, 1);
        w2k_combo_draw(d, pd->lid);
        w2k_text_mnemonic(d, F_UI, g.x + 10, pd->pbtn->r.y - fh - 4, "When I press the &power button on my computer:", C_TEXT, 1);
        w2k_combo_draw(d, pd->pbtn);
        int y = g.y + g.h + 12;
        w2k_text(d, F_UI, c.x + 9, y, "These are systemd-logind's HandleLidSwitch and HandlePowerKey;", C_GRAYTEXT); y += fh;
        w2k_text(d, F_UI, c.x + 9, y, "changing them asks for an administrator's password and writes", C_GRAYTEXT); y += fh;
        w2k_text(d, F_UI, c.x + 9, y, "/etc/systemd/logind.conf.d/50-linux2000.conf.", C_GRAYTEXT);
    } else if (pd->tabs->sel == 2) {
        W2kRect g = { c.x + 9, c.y + 10, c.w - 18, 74 };
        w2k_draw_groupbox(d, &g, "Power status");
        w2k_bigicon_draw(d, g.x + 12, g.y + 22, ICO_CP_POWER);
        const W2kPower *p = &pd->pw;
        const char *src = !p->present ? "AC power" : p->ac_online == 1 ? "AC power" : "Batteries";
        char line[160];
        snprintf(line, sizeof line, "Current power source:  %s", src);
        w2k_text(d, F_UI, g.x + 56, g.y + 22, line, C_TEXT);
        if (p->present) snprintf(line, sizeof line, "Total battery power remaining:  %d%%", p->percent);
        else            snprintf(line, sizeof line, "No battery is detected in this computer.");
        w2k_text(d, F_UI, g.x + 56, g.y + 22 + fh + 6, line, C_TEXT);

        W2kRect g2 = { c.x + 9, g.y + g.h + 10, c.w - 18, 120 };
        w2k_draw_groupbox(d, &g2, "Battery");
        pw_draw_meter(d, g2.x + 16, g2.y + 24, p);
        int tx = g2.x + 76, ty = g2.y + 26;
        if (p->present) {
            char desc[96];
            w2k_power_describe(p, desc, sizeof desc);
            snprintf(line, sizeof line, "%s", p->name);
            w2k_text(d, F_UI_BOLD, tx, ty, line, C_TEXT); ty += fh + 4;
            w2k_text(d, F_UI, tx, ty, p->charging == 1 ? "Status:  Charging" :
                     p->charging == 2 ? "Status:  Charged" : "Status:  Discharging", C_TEXT);
            ty += fh + 2;
            w2k_text(d, F_UI, tx, ty, desc, C_TEXT); ty += fh + 2;
            snprintf(line, sizeof line, "Power source:  %s",
                     p->ac_online == 1 ? "AC power" : "Battery");
            w2k_text(d, F_UI, tx, ty, line, C_TEXT);
        } else {
            w2k_text(d, F_UI, tx, ty, "This computer runs on AC power.", C_GRAYTEXT);
        }
        w2k_text(d, F_UI, c.x + 9, c.y + c.h - fh - 8,
                 "The meter refreshes every few seconds; the battery is read from sysfs.", C_GRAYTEXT);
    } else if (pd->tabs->sel == 4) {
        /* Graphics: Windows 10's "graphics preference", for the whole
         * desktop rather than a program at a time. */
        w2k_icon_draw(d, c.x + 12, c.y + 14, ICO_CP_DISPLAY);
        w2k_text(d, F_UI, c.x + 36, c.y + 12, "Choose the graphics processor that programs you start", C_TEXT);
        w2k_text(d, F_UI, c.x + 36, c.y + 12 + fh, "from this desktop draw with. Games run best on the", C_TEXT);
        w2k_text(d, F_UI, c.x + 36, c.y + 12 + 2 * fh, "fastest one.", C_TEXT);
        W2kRect g = { c.x + 9, c.y + 66, c.w - 18, 20 + (pd->ngpu ? pd->ngpu : 1) * (2 * fh + 12) + 2 };
        w2k_draw_groupbox(d, &g, "Graphics processor");
        if (!pd->ngpu)
            w2k_text(d, F_UI, g.x + 10, g.y + 22, "No graphics processor was found in sysfs.", C_GRAYTEXT);
        int nouveau = 0;
        for (int i = 0; i < pd->ngpu; i++) {
            const W2kGpu *gp = &pd->gpu[i];
            const W2kRect *r = &pd->gpu_radio[i];
            char line[200];
            snprintf(line, sizeof line, "%s%s", gp->name, i == 0 ? "  (default)" : "");
            w2k_draw_radio(d, r->x, r->y, line, pd->gpu_sel == i, 0, i > 0 && !gp->driver[0]);
            if (gp->driver[0]) snprintf(line, sizeof line, "Driver: %s", gp->driver);
            else               snprintf(line, sizeof line, "No driver is loaded for it.");
            w2k_text(d, F_UI, r->x + 19, r->y + fh + 4, line, C_GRAYTEXT);
            if (pd->gpu_sel == i && !strcmp(gp->driver, "nouveau")) nouveau = 1;
        }
        int y = g.y + g.h + 10;
        if (pd->ngpu > 1) {
            w2k_text(d, F_UI, c.x + 9, y, "Programs started after you click Apply use it. Close and", C_GRAYTEXT); y += fh;
            w2k_text(d, F_UI, c.x + 9, y, "reopen those already running, such as Steam, to move them.", C_GRAYTEXT); y += fh;
            w2k_text(d, F_UI, c.x + 9, y, "A second processor draws more power while it is in use.", C_GRAYTEXT); y += fh;
        } else if (pd->ngpu == 1) {
            w2k_text(d, F_UI, c.x + 9, y, "This computer has one graphics processor; every program", C_GRAYTEXT); y += fh;
            w2k_text(d, F_UI, c.x + 9, y, "draws with it.", C_GRAYTEXT); y += fh;
        }
        if (nouveau) {
            y += 8;
            w2k_icon_draw(d, c.x + 9, y, ICO_WARNING);
            w2k_text(d, F_UI, c.x + 33, y, "With nouveau, NVIDIA cards older than the GeForce 16", C_TEXT); y += fh;
            w2k_text(d, F_UI, c.x + 33, y, "series run far below full speed. NVIDIA's own driver is", C_TEXT); y += fh;
            w2k_text(d, F_UI, c.x + 33, y, "much faster for games.", C_TEXT);
        }
    } else {
        W2kRect g = { c.x + 9, c.y + 10, c.w - 18, 90 };
        w2k_draw_groupbox(d, &g, "Screen brightness");
        if (pd->have_backlight) {
            w2k_text_mnemonic(d, F_UI, g.x + 10, g.y + 20, "&Brightness:", C_TEXT, 1);
            w2k_text(d, F_UI, g.x + 10, pd->bright.r.y + 4, "Dark", C_TEXT);
            w2k_slider_draw(d, &pd->bright);
            w2k_text(d, F_UI, g.x + g.w - 10 - w2k_text_width(F_UI, "Bright", -1),
                     pd->bright.r.y + 4, "Bright", C_TEXT);
            char line[64];
            snprintf(line, sizeof line, "%d%%", pd->dirty ? pd->want_bright : pd->cur_bright);
            w2k_text(d, F_UI_BOLD, g.x + g.w - 10 - w2k_text_width(F_UI_BOLD, line, -1),
                     g.y + 20, line, C_TEXT);
        } else {
            w2k_text(d, F_UI, g.x + 10, g.y + 24, "No adjustable backlight was found on this computer.",
                     C_GRAYTEXT);
            w2k_text(d, F_UI, g.x + 10, g.y + 24 + fh + 2, "(/sys/class/backlight is empty.)",
                     C_GRAYTEXT);
        }
        int y = g.y + g.h + 12;
        w2k_text(d, F_UI, c.x + 9, y, "The brightness is written to the backlight directly when", C_GRAYTEXT); y += fh;
        w2k_text(d, F_UI, c.x + 9, y, "your user may, and through brightnessctl or an", C_GRAYTEXT); y += fh;
        w2k_text(d, F_UI, c.x + 9, y, "administrator prompt otherwise.", C_GRAYTEXT);
    }
    w2k_draw_pushbutton(d, &pd->ok, "OK", BS_DEFAULT | (pd->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &pd->cancel, "Cancel", pd->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &pd->apply, "&Apply",
                        (pd->dirty ? 0 : BS_DISABLED) | (pd->down == 3 ? BS_PRESSED : 0));
}

static int pw_event(W2kWin *w, XEvent *e)
{
    PowerDlg *pd = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_tabs_press(pd->tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (pd->tabs->sel == 3 && pd->have_backlight &&
            w2k_slider_press(&pd->bright, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (pd->tabs->sel == 0 && (w2k_combo_press(pd->mon_off, &e->xbutton) ||
                                   w2k_combo_press(pd->standby, &e->xbutton) ||
                                   w2k_combo_press(pd->hibern, &e->xbutton))) { w2k_win_dirty(w); return 1; }
        if (pd->tabs->sel == 1 && (w2k_combo_press(pd->lid, &e->xbutton) ||
                                   w2k_combo_press(pd->pbtn, &e->xbutton))) { w2k_win_dirty(w); return 1; }
        if (pd->tabs->sel == 4)
            for (int i = 0; i < pd->ngpu; i++)
                if (w2k_rect_hit(&pd->gpu_radio[i], x, y) && (i == 0 || pd->gpu[i].driver[0])) {
                    if (pd->gpu_sel != i) { pd->gpu_sel = i; pd->dirty = 1; }
                    w2k_win_dirty(w);
                    return 1;
                }
        if (w2k_rect_hit(&pd->ok, x, y)) pd->down = 1;
        else if (w2k_rect_hit(&pd->cancel, x, y)) pd->down = 2;
        else if (w2k_rect_hit(&pd->apply, x, y) && pd->dirty) pd->down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = pd->down, x = e->xbutton.x, y = e->xbutton.y;
        pd->down = 0;
        w2k_slider_release(&pd->bright);
        if (b == 1 && w2k_rect_hit(&pd->ok, x, y)) { pw_commit(pd); w2k_win_close(w, ID_OK); }
        else if (b == 2 && w2k_rect_hit(&pd->cancel, x, y)) w2k_win_close(w, ID_CANCEL);
        else if (b == 3 && w2k_rect_hit(&pd->apply, x, y)) pw_commit(pd);
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (pd->tabs->sel == 3 && pd->have_backlight &&
            w2k_slider_motion(&pd->bright, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { pw_commit(pd); w2k_win_close(w, ID_OK); return 1; }
        if (w2k_tabs_key(pd->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (pd->tabs->sel == 3 && pd->have_backlight &&
            w2k_slider_key(&pd->bright, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void open_power(void)
{
    PowerDlg pd;
    memset(&pd, 0, sizeof pd);
    int cw = 398, chh = 372, fh0 = w2k_font_height(F_UI);
    W2kWin *w = w2k_win_new("Power Options Properties", "l2kcontrol", cw, chh, 0);
    pd.win = w;
    pd.tabs = w2k_tabs_new(&pd, NULL);
    w2k_tabs_add(pd.tabs, "Power Schemes");
    w2k_tabs_add(pd.tabs, "Advanced");
    w2k_tabs_add(pd.tabs, "Power Meter");
    w2k_tabs_add(pd.tabs, "Brightness");
    w2k_tabs_add(pd.tabs, "Graphics");
    pd.tabs->r = (W2kRect){ 7, 7, cw - 14, chh - 7 - 41 };
    W2kRect c = w2k_tabs_client(pd.tabs);

    pd.ngpu = w2k_gpus(pd.gpu, W2K_GPU_MAX, 1);
    for (int i = 0; i < pd.ngpu; i++) {
        pd.gpu_radio[i] = (W2kRect){ c.x + 19, c.y + 66 + 20 + i * (2 * fh0 + 12), c.w - 38, fh0 + 4 };
        if (i > 0 && !strcmp(pd.gpu[i].addr, w2k_gpu_pref)) pd.gpu_sel = i;
    }

    pd.can_suspend = w2k_power_can("suspend");
    pd.can_hibernate = w2k_power_can("hibernate");
    pd.mon_off = w2k_combo_new(0); pd.mon_off->user = &pd; pd.mon_off->on_change = pw_on_change;
    pd.standby = w2k_combo_new(0); pd.standby->user = &pd; pd.standby->on_change = pw_on_change;
    pd.hibern  = w2k_combo_new(0); pd.hibern->user = &pd;  pd.hibern->on_change = pw_on_change;
    pw_fill_minutes(pd.mon_off, w2k_monitor_off_min);
    pw_fill_minutes(pd.standby, w2k_standby_min);
    pw_fill_minutes(pd.hibern, w2k_hibernate_min);
    pd.mo_idx = pd.mon_off->sel;
    pd.sb_idx = pd.standby->sel;
    pd.hb_idx = pd.hibern->sel;
    pd.mon_off->r = (W2kRect){ c.x + 160, c.y + 66 + 22, c.w - 18 - 160, 21 };
    pd.standby->r = (W2kRect){ c.x + 160, c.y + 66 + 52, c.w - 18 - 160, 21 };
    pd.hibern->r  = (W2kRect){ c.x + 160, c.y + 66 + 82, c.w - 18 - 160, 21 };
    w2k_power_lid_get(pd.lid_was, sizeof pd.lid_was, pd.pbtn_was, sizeof pd.pbtn_was);
    pd.lid  = w2k_combo_new(0); pd.lid->user = &pd;  pd.lid->on_change = pw_on_change;
    pd.pbtn = w2k_combo_new(0); pd.pbtn->user = &pd; pd.pbtn->on_change = pw_on_change;
    pw_fill_actions(pd.lid, pd.lid_was);
    pw_fill_actions(pd.pbtn, pd.pbtn_was);
    pd.lid_idx = pd.lid->sel;
    pd.pbtn_idx = pd.pbtn->sel;
    pd.lid->r  = (W2kRect){ c.x + 19, c.y + 10 + 20 + fh0 + 4, c.w - 38, 21 };
    pd.pbtn->r = (W2kRect){ c.x + 19, c.y + 10 + 20 + fh0 + 4 + 21 + 10 + fh0 + 4, c.w - 38, 21 };

    pd.have_backlight = w2k_backlight_available();
    pd.bright = (W2kSlider){ .r = { c.x + 9 + 40, c.y + 10 + 44, c.w - 18 - 80 - 10, 24 },
                             .lo = 0, .hi = 20, .ticks = 10, .pos = 10,
                             .owner = w, .user = &pd, .on_change = pw_on_bright };
    pw_refresh(&pd);
    pd.want_bright = pd.cur_bright;

    int bby = chh - 12 - 23;
    pd.apply  = (W2kRect){ cw - 12 - 75, bby, 75, 23 };
    pd.cancel = (W2kRect){ cw - 12 - 75 * 2 - 6, bby, 75, 23 };
    pd.ok     = (W2kRect){ cw - 12 - 75 * 3 - 12, bby, 75, 23 };

    w->user = &pd;
    w->paint = pw_paint;
    w->event = pw_event;
    w2k_win_center(w, cp.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_add_timer(5000, pw_tick, &pd);
    w2k_win_modal(w);
    w2k_del_timer(pw_tick, &pd);
    w2k_combo_free(pd.mon_off); w2k_combo_free(pd.standby); w2k_combo_free(pd.hibern);
    w2k_combo_free(pd.lid); w2k_combo_free(pd.pbtn);
    w2k_tabs_free(pd.tabs);
}

/* ------------------------------------------------------------------ *
 * User Accounts
 *
 * The name and the picture the Start menu shows for you: Windows XP's
 * "Change my picture", on a Windows 2000 sheet. Kept in ~/.w2k/account;
 * the logon name stays what the system calls you.
 * ------------------------------------------------------------------ */
typedef struct {
    W2kWin  *win;
    W2kEdit *name;
    W2kRect  pic, change, def, ok, cancel, apply;
    char     picture[1024];
    int      down, dirty;
} UsersDlg;

static void ua_commit(UsersDlg *ud)
{
    const char *n = w2k_edit_text(ud->name);
    /* A name that is only the passwd entry's is not kept: leave the file
     * saying nothing and it follows the account. */
    w2k_account_save(n && strcmp(n, w2k_account_default_name()) ? n : "", ud->picture);
    w2k_scheme_broadcast();
    ud->dirty = 0;
}

static void ua_paint(W2kWin *w, Drawable d)
{
    UsersDlg *ud = w->user;
    int fh = w2k_font_height(F_UI);
    W2kRect g = { 12, 10, w->w - 24, 96 };
    w2k_draw_groupbox(d, &g, "Picture");
    /* The picture in a sunken frame, as the Start menu shows it. */
    w2k_edge(d, ud->pic.x - 2, ud->pic.y - 2, ud->pic.w + 4, ud->pic.h + 4, EDGE_SUNKEN, BF_RECT);
    /* The picture as chosen here, saved or not. */
    w2k_account_preview(ud->picture);
    w2k_account_picture_draw(d, ud->pic.x, ud->pic.y, ud->pic.w, ICO_MYCOMPUTER);
    w2k_draw_pushbutton(d, &ud->change, "&Change Picture...", ud->down == 4 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &ud->def, "&Default Picture",
                        (ud->picture[0] ? 0 : BS_DISABLED) | (ud->down == 5 ? BS_PRESSED : 0));
    w2k_text(d, F_UI, ud->change.x, ud->def.y + ud->def.h + 6,
             ud->picture[0] ? "A picture of your own; it is cropped to a square." : "The shell's own tile.", C_GRAYTEXT);

    W2kRect g2 = { 12, g.y + g.h + 8, w->w - 24, 40 + fh + 21 };
    w2k_draw_groupbox(d, &g2, "Name");
    w2k_text_mnemonic(d, F_UI, g2.x + 10, ud->name->r.y - fh - 4,
                      "The &name the Start menu shows:", C_TEXT, 1);
    w2k_edit_draw(d, ud->name);
    char note[200];
    snprintf(note, sizeof note, "Your logon name stays \"%s\"; this is only how the Start menu greets you.",
             getenv("USER") ? getenv("USER") : "you");
    w2k_text(d, F_UI, 12, g2.y + g2.h + 8, note, C_GRAYTEXT);

    w2k_draw_pushbutton(d, &ud->ok, "OK", BS_DEFAULT | (ud->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &ud->cancel, "Cancel", ud->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &ud->apply, "&Apply",
                        (ud->dirty ? 0 : BS_DISABLED) | (ud->down == 3 ? BS_PRESSED : 0));
}

static void ua_pick_picture(UsersDlg *ud)
{
    char path[1024];
    snprintf(path, sizeof path, "%s", ud->picture[0] ? ud->picture : "");
    if (!path[0]) {
        const char *h = getenv("HOME");
        snprintf(path, sizeof path, "%s/", h ? h : "/");
    }
    if (w2k_file_dialog_filter(ud->win, 0, path, sizeof path,
                               "Pictures (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp|All Files (*.*)|*")) {
        /* Decoded small (a photograph by 8), only to see that it can be. */
        int pw = 0, ph = 0;
        unsigned char *probe = w2k_image_load_scaled(path, 96, 96, &pw, &ph);
        if (!probe) {
            w2k_msgbox(ud->win, "User Accounts",
                       "That file is not a picture Linux 2000 can read (PNG, JPEG or BMP).",
                       MB_OK | MB_ICONWARNING);
            return;
        }
        free(probe);
        snprintf(ud->picture, sizeof ud->picture, "%s", path);
        ud->dirty = 1;
    }
}

static int ua_event(W2kWin *w, XEvent *e)
{
    UsersDlg *ud = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_edit_press(ud->name, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&ud->ok, x, y)) ud->down = 1;
        else if (w2k_rect_hit(&ud->cancel, x, y)) ud->down = 2;
        else if (w2k_rect_hit(&ud->apply, x, y) && ud->dirty) ud->down = 3;
        else if (w2k_rect_hit(&ud->change, x, y)) ud->down = 4;
        else if (w2k_rect_hit(&ud->def, x, y) && ud->picture[0]) ud->down = 5;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = ud->down, x = e->xbutton.x, y = e->xbutton.y;
        ud->down = 0;
        if (b == 1 && w2k_rect_hit(&ud->ok, x, y)) { ua_commit(ud); w2k_win_close(w, ID_OK); }
        else if (b == 2 && w2k_rect_hit(&ud->cancel, x, y)) w2k_win_close(w, ID_CANCEL);
        else if (b == 3 && w2k_rect_hit(&ud->apply, x, y)) ua_commit(ud);
        else if (b == 4 && w2k_rect_hit(&ud->change, x, y)) ua_pick_picture(ud);
        else if (b == 5 && w2k_rect_hit(&ud->def, x, y)) { ud->picture[0] = 0; ud->dirty = 1; }
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { ua_commit(ud); w2k_win_close(w, ID_OK); return 1; }
        if (w2k_edit_key(ud->name, &e->xkey)) { ud->dirty = 1; w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void open_users(void)
{
    UsersDlg ud;
    memset(&ud, 0, sizeof ud);
    int cw = 372, chh = 292, fh = w2k_font_height(F_UI);
    W2kWin *w = w2k_win_new("User Accounts", "l2kcontrol", cw, chh, 0);
    ud.win = w;
    w->user = &ud;
    w->paint = ua_paint;
    w->event = ua_event;
    snprintf(ud.picture, sizeof ud.picture, "%s", w2k_account_picture());
    ud.pic    = (W2kRect){ 12 + 14, 10 + 22, 48, 48 };
    ud.change = (W2kRect){ 12 + 14 + 48 + 16, 10 + 22, 120, 23 };
    ud.def    = (W2kRect){ 12 + 14 + 48 + 16, 10 + 22 + 27, 120, 23 };
    ud.name = w2k_edit_new(0);
    w2k_edit_bind(ud.name, w);
    ud.name->r = (W2kRect){ 12 + 10, 10 + 96 + 8 + 20 + fh + 4, cw - 24 - 20, 21 };
    w2k_edit_set(ud.name, w2k_account_name());
    ud.name->focused = 1;
    w2k_add_timer(w2k_caret_blink, blink, ud.name);
    int bby = chh - 12 - 23;
    ud.apply  = (W2kRect){ cw - 12 - 75, bby, 75, 23 };
    ud.cancel = (W2kRect){ cw - 12 - 75 * 2 - 6, bby, 75, 23 };
    ud.ok     = (W2kRect){ cw - 12 - 75 * 3 - 12, bby, 75, 23 };
    w2k_win_center(w, cp.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_win_modal(w);
    w2k_del_timer(blink, ud.name);
    w2k_edit_free(ud.name);
}

/* ------------------------------------------------------------------ *
 * Logon Screen
 *
 * What the logon screen shows: the banner's artwork (Windows 2000,
 * Linux 2000, the distribution's own logo), the colour or wallpaper
 * behind the dialog, and whether the user's picture is on it. Kept in
 * ~/.w2k/logon; the logon screen reads the last user's.
 * ------------------------------------------------------------------ */
typedef struct {
    W2kWin  *win;
    W2kLogonCfg cfg;
    W2kEdit *rgb[3], *wall;
    W2kRect  art[3], swatch, browse, none, pic, ok, cancel, apply;
    char     distro[128];
    int      down, dirty;
} LogonDlg;

static void lo_edit_focus(LogonDlg *ld, W2kEdit *e)
{
    for (int i = 0; i < 3; i++) ld->rgb[i]->focused = ld->rgb[i] == e;
    ld->wall->focused = ld->wall == e;
}

static void lo_read_colour(LogonDlg *ld)
{
    for (int i = 0; i < 3; i++) {
        int v = atoi(w2k_edit_text(ld->rgb[i]));
        ld->cfg.bg[i] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
}

static void lo_changed(void *u)
{
    LogonDlg *ld = u;
    lo_read_colour(ld);
    snprintf(ld->cfg.wallpaper, sizeof ld->cfg.wallpaper, "%s", w2k_edit_text(ld->wall));
    ld->dirty = 1;
    w2k_win_dirty(ld->win);
}

static void lo_paint(W2kWin *w, Drawable d)
{
    LogonDlg *ld = w->user;
    int fh = w2k_font_height(F_UI);
    W2kRect g = { 12, 10, w->w - 24, 88 };
    w2k_draw_groupbox(d, &g, "Artwork");
    static const char *const names[3] = { "Windows 2000 Professional", "Linux 2000", NULL };
    char dl[160];
    snprintf(dl, sizeof dl, "%s (the distribution's logo)", ld->distro);
    for (int i = 0; i < 3; i++)
        w2k_draw_radio(d, ld->art[i].x, ld->art[i].y, i == 2 ? dl : names[i], ld->cfg.art == i, 0, 0);

    W2kRect g2 = { 12, g.y + g.h + 8, w->w - 24, 118 };
    w2k_draw_groupbox(d, &g2, "Background");
    w2k_text_mnemonic(d, F_UI, g2.x + 14, ld->swatch.y + (ld->swatch.h - fh) / 2, "&Color:", C_TEXT, 1);
    w2k_edge(d, ld->swatch.x, ld->swatch.y, ld->swatch.w, ld->swatch.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill_rgb(d, ld->swatch.x + 2, ld->swatch.y + 2, ld->swatch.w - 4, ld->swatch.h - 4,
                 ld->cfg.bg[0], ld->cfg.bg[1], ld->cfg.bg[2]);
    for (int i = 0; i < 3; i++) w2k_edit_draw(d, ld->rgb[i]);
    w2k_text(d, F_UI, ld->rgb[2]->r.x + ld->rgb[2]->r.w + 8, ld->swatch.y + (ld->swatch.h - fh) / 2,
             "red, green, blue", C_GRAYTEXT);
    w2k_text_mnemonic(d, F_UI, g2.x + 14, ld->wall->r.y + (21 - fh) / 2, "&Wallpaper:", C_TEXT, 1);
    w2k_edit_draw(d, ld->wall);
    w2k_draw_pushbutton(d, &ld->browse, "&Browse...", ld->down == 4 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &ld->none, "&None",
                        (ld->cfg.wallpaper[0] ? 0 : BS_DISABLED) | (ld->down == 5 ? BS_PRESSED : 0));
    w2k_text(d, F_UI, g2.x + 14, ld->none.y + 4, "A picture is stretched over the colour.", C_GRAYTEXT);

    w2k_draw_checkbox(d, ld->pic.x, ld->pic.y, "&Show the user's picture on the logon screen",
                      ld->cfg.show_picture, 0, 0);
    w2k_text(d, F_UI, 12, ld->pic.y + 26, "The logon screen takes its look from the last user who logged on.", C_GRAYTEXT);

    w2k_draw_pushbutton(d, &ld->ok, "OK", BS_DEFAULT | (ld->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &ld->cancel, "Cancel", ld->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &ld->apply, "&Apply",
                        (ld->dirty ? 0 : BS_DISABLED) | (ld->down == 3 ? BS_PRESSED : 0));
}

static void lo_commit(LogonDlg *ld)
{
    lo_read_colour(ld);
    snprintf(ld->cfg.wallpaper, sizeof ld->cfg.wallpaper, "%s", w2k_edit_text(ld->wall));
    if (w2k_logon_save(&ld->cfg) < 0)
        w2k_msgbox(ld->win, "Logon Screen", "The settings could not be saved.", MB_OK | MB_ICONWARNING);
    ld->dirty = 0;
}

static int lo_event(W2kWin *w, XEvent *e)
{
    LogonDlg *ld = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        for (int i = 0; i < 3; i++)
            if (w2k_edit_press(ld->rgb[i], &e->xbutton)) { lo_edit_focus(ld, ld->rgb[i]); w2k_win_dirty(w); return 1; }
        if (w2k_edit_press(ld->wall, &e->xbutton)) { lo_edit_focus(ld, ld->wall); w2k_win_dirty(w); return 1; }
        for (int i = 0; i < 3; i++)
            if (w2k_rect_hit(&ld->art[i], x, y)) { ld->cfg.art = i; ld->dirty = 1; w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&ld->pic, x, y)) { ld->cfg.show_picture = !ld->cfg.show_picture; ld->dirty = 1; w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&ld->ok, x, y)) ld->down = 1;
        else if (w2k_rect_hit(&ld->cancel, x, y)) ld->down = 2;
        else if (w2k_rect_hit(&ld->apply, x, y) && ld->dirty) ld->down = 3;
        else if (w2k_rect_hit(&ld->browse, x, y)) ld->down = 4;
        else if (w2k_rect_hit(&ld->none, x, y) && ld->cfg.wallpaper[0]) ld->down = 5;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = ld->down, x = e->xbutton.x, y = e->xbutton.y;
        ld->down = 0;
        if (b == 1 && w2k_rect_hit(&ld->ok, x, y)) { lo_commit(ld); w2k_win_close(w, ID_OK); }
        else if (b == 2 && w2k_rect_hit(&ld->cancel, x, y)) w2k_win_close(w, ID_CANCEL);
        else if (b == 3 && w2k_rect_hit(&ld->apply, x, y)) lo_commit(ld);
        else if (b == 4 && w2k_rect_hit(&ld->browse, x, y)) {
            char path[1024];
            snprintf(path, sizeof path, "%s", ld->cfg.wallpaper[0] ? ld->cfg.wallpaper : "/usr/share/backgrounds/");
            if (w2k_file_dialog_filter(w, 0, path, sizeof path,
                                       "Pictures (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp|All Files (*.*)|*")) {
                w2k_edit_set(ld->wall, path);
                snprintf(ld->cfg.wallpaper, sizeof ld->cfg.wallpaper, "%s", path);
                ld->dirty = 1;
            }
        }
        else if (b == 5 && w2k_rect_hit(&ld->none, x, y)) { w2k_edit_set(ld->wall, ""); ld->cfg.wallpaper[0] = 0; ld->dirty = 1; }
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { lo_commit(ld); w2k_win_close(w, ID_OK); return 1; }
        for (int i = 0; i < 3; i++)
            if (ld->rgb[i]->focused && w2k_edit_key(ld->rgb[i], &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (ld->wall->focused && w2k_edit_key(ld->wall, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void open_logon(void)
{
    LogonDlg ld;
    memset(&ld, 0, sizeof ld);
    int cw = 392, chh = 330;
    W2kWin *w = w2k_win_new("Logon Screen", "l2kcontrol", cw, chh, 0);
    ld.win = w;
    w->user = &ld;
    w->paint = lo_paint;
    w->event = lo_event;
    w2k_logon_load(&ld.cfg, NULL);
    w2k_distro_pretty_name(ld.distro, sizeof ld.distro);
    for (int i = 0; i < 3; i++) ld.art[i] = (W2kRect){ 26, 30 + i * 20, 330, 16 };
    ld.swatch = (W2kRect){ 26 + 70, 106 + 22, 40, 21 };
    for (int i = 0; i < 3; i++) {
        ld.rgb[i] = w2k_edit_new(0);
        w2k_edit_bind(ld.rgb[i], w);
        ld.rgb[i]->r = (W2kRect){ ld.swatch.x + ld.swatch.w + 8 + i * 40, ld.swatch.y, 36, 21 };
        char v[8];
        snprintf(v, sizeof v, "%d", ld.cfg.bg[i]);
        w2k_edit_set(ld.rgb[i], v);
        ld.rgb[i]->on_change = lo_changed;
        ld.rgb[i]->user = &ld;
    }
    ld.wall = w2k_edit_new(0);
    w2k_edit_bind(ld.wall, w);
    ld.wall->r = (W2kRect){ 26 + 70, ld.swatch.y + 30, 190, 21 };
    w2k_edit_set(ld.wall, ld.cfg.wallpaper);
    ld.wall->on_change = lo_changed;
    ld.wall->user = &ld;
    ld.browse = (W2kRect){ ld.wall->r.x + ld.wall->r.w + 8, ld.wall->r.y - 1, 75, 23 };
    ld.none   = (W2kRect){ ld.browse.x, ld.browse.y + 27, 75, 23 };
    ld.pic    = (W2kRect){ 26, 106 + 118 + 12, 340, 16 };
    ld.dirty = 0;
    int bby = chh - 12 - 23;
    ld.apply  = (W2kRect){ cw - 12 - 75, bby, 75, 23 };
    ld.cancel = (W2kRect){ cw - 12 - 75 * 2 - 6, bby, 75, 23 };
    ld.ok     = (W2kRect){ cw - 12 - 75 * 3 - 12, bby, 75, 23 };
    lo_edit_focus(&ld, ld.rgb[0]);
    for (int i = 0; i < 3; i++) w2k_add_timer(w2k_caret_blink, blink, ld.rgb[i]);
    w2k_add_timer(w2k_caret_blink, blink, ld.wall);
    w2k_win_center(w, cp.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_win_modal(w);
    for (int i = 0; i < 3; i++) { w2k_del_timer(blink, ld.rgb[i]); w2k_edit_free(ld.rgb[i]); }
    w2k_del_timer(blink, ld.wall);
    w2k_edit_free(ld.wall);
}

/* An applet whose program this setup did not install is not listed:
 * install.sh can leave out Bluetooth and Wi-Fi, or every program past the
 * basic set, and an icon that does nothing is worse than no icon. */
static int applet_installed(const Applet *a)
{
    if (!a->cmd || !strcmp(a->cmd, "@startmenu")) return 1;
    size_t n = strcspn(a->cmd, " ");
    char prog[128];
    if (n >= sizeof prog) return 1;
    memcpy(prog, a->cmd, n);
    prog[n] = 0;
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin:/usr/local/bin";
    char buf[512];
    for (const char *p = path; *p; ) {
        const char *q = strchr(p, ':');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len && len + n + 2 <= sizeof buf) {
            memcpy(buf, p, len);
            buf[len] = '/';
            memcpy(buf + len + 1, prog, n + 1);
            if (access(buf, X_OK) == 0) return 1;
        }
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

/* Rows of the Control Panel list carry their applet's index: on a desktop
 * machine Power Options is left out, so the row and the index differ. */
static int applet_of_row(int row)
{
    if (!cp.fw || row < 0 || row >= cp.fw->list->n) return -1;
    return (int)(intptr_t)cp.fw->list->items[row].data;
}

static void open_applet(int i)
{
    if (i < 0 || i >= NAPPLETS) return;
    if (applets[i].cmd) {
        if (!strcmp(applets[i].cmd, "@startmenu")) wm_command(3);
        else                                      spawn(applets[i].cmd);
        return;
    }
    if (applets[i].open) applets[i].open();
}

/* The web-view pane: the folder's own words until an item is picked,
 * then that item's name and description, as the shell does. */
static void pane_fill(int idx)
{
    W2kFolderWin *f = cp.fw;
    w2k_folderwin_pane_clear(f);
    if (idx >= 0 && idx < NAPPLETS) {
        w2k_folderwin_pane_add(f, FW_BOLD, applets[idx].name);
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        w2k_folderwin_pane_add(f, FW_PLAIN, applets[idx].desc);
        w2k_folderwin_status(f, applets[idx].desc);
    } else {
        w2k_folderwin_pane_add(f, FW_PLAIN,
            "Use the settings in Control Panel to personalize your computer.");
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        w2k_folderwin_pane_add(f, FW_PLAIN, "Select an item to view its description.");
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        w2k_folderwin_pane_add(f, FW_LINK, "Windows Update");
        w2k_folderwin_pane_add(f, FW_LINK, "Windows 2000 Support");
        char buf[40];
        snprintf(buf, sizeof buf, "%d object(s)", f->list->n);
        w2k_folderwin_status(f, buf);
    }
}

static void on_activate(void *u, int idx) { (void)u; open_applet(applet_of_row(idx)); }

static void on_select(void *u, int idx)
{
    (void)u;
    pane_fill(applet_of_row(idx));
    w2k_win_dirty(cp.win);
}

static void command(void *u, int id)
{
    (void)u;
    switch (id) {
    case FW_LAST + 0:   /* Windows Update */
        spawn("l2kupdate");
        break;
    case FW_LAST + 1:   /* Windows 2000 Support */
        spawn("xdg-open https://discord.gg/KPQBnSqcK");
        break;
    case FW_OPEN:
        open_applet(applet_of_row(cp.fw->list->sel));
        break;
    case FW_REFRESH:
        pane_fill(applet_of_row(cp.fw->list->sel));
        break;
    }
}

static W2kMenu *build_file(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, FW_OPEN, "&Open", NULL, ICO_NONE);
    w2k_menu_default(m);
    return m;
}

static void paint(W2kWin *w, Drawable d)
{
    (void)w;
    w2k_folderwin_paint(cp.fw, d);
}

static int event(W2kWin *w, XEvent *e)
{
    if (e->type == KeyPress) {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, 0); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            open_applet(applet_of_row(cp.fw->list->sel));
            return 1;
        }
    }
    if (w2k_folderwin_event(cp.fw, e)) return 1;
    return e->type == ButtonPress || e->type == ButtonRelease || e->type == KeyPress;
}

static void resized(W2kWin *w)
{
    (void)w;
    w2k_folderwin_layout(cp.fw);
}

int main(int argc, char **argv)
{
    if (w2k_init("l2kcontrol") < 0) return 1;

    /* Development aid: W2K_RENDER_APPLET=n renders row n's pane. */
    if (getenv("W2K_RENDER_APPLET") && getenv("W2K_RENDER")) {
        int i = atoi(getenv("W2K_RENDER_APPLET"));
        if (i >= 0 && i < NAPPLETS && applets[i].open) applets[i].open();
        w2k_fini();
        return 0;
    }
    /* "l2kcontrol mouse" opens that applet straight away, the way
     * "control mouse" does in Windows -- the Start menu uses it, and so
     * can anything else. */
    if (argc > 1) {
        static const struct { const char *word; void (*fn)(void); } direct[] = {
            { "performance", open_performance },
            { "defaults",    open_defaults    },
            { "mouse",       open_mouse       },
            { "keyboard",    open_keyboard    },
            { "sounds",      open_sounds      },
            { "fonts",       open_fonts       },
            { "datetime",    open_datetime    },
            { "power",       open_power       },
            { "users",       open_users       },
            { "logon",       open_logon       },
            { "system",      open_system      },
            { "cleanup",     open_cleanup     },
        };
        for (int i = 0; i < (int)(sizeof direct / sizeof *direct); i++)
            if (!strcasecmp(argv[1], direct[i].word)) {
                direct[i].fn();
                w2k_fini();
                return 0;
            }
        /* "l2kcontrol drive <mount point> [name]": that drive's sheet. */
        if (!strcasecmp(argv[1], "drive") && argc > 2) {
            w2k_drive_properties(NULL, argv[2], argc > 3 ? argv[3] : NULL, 0);
            w2k_fini();
            return 0;
        }
        if (!strcasecmp(argv[1], "folders")) {
            w2k_folder_options(NULL);
            w2k_fini();
            return 0;
        }
    }

    /* The folder window at the size of the reference screenshot. */
    cp.fw = w2k_folderwin_new("Control Panel", "l2kcontrol", ICO_CONTROLPANEL,
                              870, 682, NULL, command);
    cp.win = cp.fw->win;
    cp.win->paint = paint;
    cp.win->event = event;
    cp.win->resized = resized;
    cp.fw->build_file = build_file;

    W2kList *l = cp.fw->list;
    l->on_activate = on_activate;
    l->on_select = on_select;
    for (int i = 0; i < NAPPLETS; i++) {
        if (!applet_installed(&applets[i])) continue;
        int r = w2k_list_add(l, applets[i].icon, (void *)(intptr_t)i);
        w2k_list_set(l, r, 0, applets[i].name);
    }
    pane_fill(-1);

    w2k_folderwin_layout(cp.fw);
    w2k_win_center(cp.win, NULL);
    w2k_win_show(cp.win);
    w2k_run();
    w2k_folderwin_free(cp.fw);
    w2k_fini();
    return 0;
}
