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
#include <sys/wait.h>
#include <strings.h>
#include <fontconfig/fontconfig.h>

#define STATUS_H 20

enum { ID_OPEN = 1, ID_CLOSE, ID_ABOUT };

/* Which preset the radio buttons name, in the Performance Options dialog. */
enum { PRESET_AUTO, PRESET_APPEARANCE, PRESET_PERFORMANCE, PRESET_CUSTOM };

/* An applet either runs a program or opens a pane of our own. */
typedef struct {
    const char *name;
    const char *desc;
    int         icon;
    const char *cmd;                 /* NULL: handled in here */
} Applet;

static const Applet applets[] = {
    /* Alphabetical, as the shell lists them; the descriptions are the
     * ones Windows 2000 shows in the web-view pane. */
    { "Date/Time", "Set the date, time and time zone for your computer.",
      ICO_CP_DATETIME, NULL },
    { "Default Programs", "Choose which programs open which kinds of files.",
      ICO_PROGRAMS, NULL },
    { "Device Manager", "Shows the hardware installed in this computer and lets you change its drivers.",
      ICO_MYCOMPUTER, "l2kdevmgmt" },
    { "Display", "Customize your desktop display and screen saver.",
      ICO_CP_DISPLAY, "l2kdisplay" },
    { "Folder Options", "Customizes the display of files and folders, changes file associations, and makes network files available offline.",
      ICO_CP_FOLDEROPTS, NULL },
    { "Fonts", "Displays and manages fonts on your computer.",
      ICO_FONTS_FOLDER, NULL },
    { "Keyboard", "Customizes your keyboard settings.",
      ICO_CP_KEYBOARD, NULL },
    { "Mouse", "Customizes your mouse settings.",
      ICO_CP_MOUSE, NULL },
    { "Network and Dial-up Connections", "Connects to other computers, networks, and the Internet.",
      ICO_CP_NETWORK, "l2knetwork" },
    { "Power Options", "Configures energy-saving settings for your computer.",
      ICO_CP_POWER, NULL },                /* laptops only: see main() */
    { "Sounds and Multimedia", "Assigns sounds to events and configures sound devices.",
      ICO_CP_SOUNDS, NULL },
    { "System", "Provides system information and changes environment settings.",
      ICO_CP_SYSTEM, NULL },
    { "Task Manager", "Shows the programs and processes running on your computer.",
      ICO_TASKMGR, "l2ktaskmgr" },
    { "Taskbar and Start Menu", "Customizes the Start Menu and the taskbar.",
      ICO_TASKBAR, "@startmenu" },      /* @ = ask the shell, not a program */
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
            for (int i = 0; i < dd->n; i++)
                w2k_assoc_set(w2k_assoc_class_at(i), w2k_edit_text(dd->edit[i]));
            w2k_assoc_apply_folder_default();
            w2k_win_close(w, ID_OK);
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
        if (ks == XK_Tab) {
            int f = -1;
            for (int i = 0; i < dd->n; i++) if (dd->edit[i]->focused) f = i;
            for (int i = 0; i < dd->n; i++) dd->edit[i]->focused = 0;
            dd->edit[(f + 1) % dd->n]->focused = 1;
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
    if (dd.n) dd.edit[0]->focused = 1;
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
} PerfDlg;

static PerfDlg *pd_active;

static void perf_fill(PerfDlg *pd)
{
    w2k_list_clear(pd->list);
    for (int i = 0; i < N_EFFECTS; i++) {
        int r = w2k_list_add(pd->list, ICO_NONE,
                             w2k_effect_supported(i) ? NULL : (void *)-1);
        w2k_list_set(pd->list, r, 0, w2k_effect_label(i));
        pd->list->items[r].checked = pd->fx[i];
    }
}

static void perf_on_check(void *u, int idx)
{
    PerfDlg *pd = u;
    if (idx < 0 || idx >= N_EFFECTS) return;
    pd->fx[idx] = (unsigned char)pd->list->items[idx].checked;
    pd->preset = PRESET_CUSTOM;        /* as the original does */
    pd->dirty = 1;
    w2k_win_dirty(cp.win);
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
enum { AP_DATETIME = 0, AP_DEFAULTS, AP_DEVMGR, AP_DISPLAY, AP_FOLDER,
       AP_FONTS, AP_KEYBOARD, AP_MOUSE, AP_NETWORK, AP_POWER, AP_SOUNDS,
       AP_SYSTEM, AP_TASKMGR, AP_STARTMENU };

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

    w->user = id;
    w->paint = input_paint;
    w->event = input_event;
    w2k_win_center(w, cp.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_win_modal(w);
}

static void mouse_commit(InputDlg *id)
{
    w2k_mouse_swap = id->sel_radio == 1;
    /* The slider counts up as "faster", which is a shorter interval. */
    w2k_dblclk_ms = 900 - id->sl[0].s.pos * 70;
    w2k_mouse_speed = id->sl[1].s.pos;
    w2k_effects[FX_CURSOR_SHADOW] = id->chk[0].on &&
                                    w2k_effect_supported(FX_CURSOR_SHADOW);
    w2k_cursors_windows = id->chk[1].on;
}

static void open_mouse(void)
{
    InputDlg id;
    memset(&id, 0, sizeof id);
    id.title = "Mouse Properties";
    id.commit = mouse_commit;
    id.nradio = 2;
    id.radio[0].label = "&Right-handed";
    id.radio[1].label = "&Left-handed";
    id.sel_radio = w2k_mouse_swap ? 1 : 0;
    id.nsl = 2;
    id.sl[0].label = "&Double-click speed:";
    id.sl[0].lo = "Slow"; id.sl[0].hi = "Fast";
    id.sl[0].s = (W2kSlider){ .lo = 0, .hi = 10, .ticks = 10,
                              .pos = (900 - w2k_dblclk_ms) / 70 };
    id.sl[1].label = "&Pointer speed:";
    id.sl[1].lo = "Slow"; id.sl[1].hi = "Fast";
    id.sl[1].s = (W2kSlider){ .lo = 1, .hi = 10, .ticks = 9,
                              .pos = w2k_mouse_speed };
    id.ncheck = 2;
    id.chk[0].label = "Show &shadow under pointer";
    id.chk[0].on = w2k_effects[FX_CURSOR_SHADOW];
    id.chk[1].label = "Use the &Windows 2000 pointers (off: the X server's own)";
    id.chk[1].on = w2k_cursors_windows;
    input_run(&id, 322);
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
    if (!dt->live) return;
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

/* Read the fields back, then hand the clock and the zone to timedatectl. */
static void dt_apply(DtDlg *dt)
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
            if (w2k_edit_press(dt->year, &e->xbutton)) { dt->focus = 1; w2k_win_dirty(w); return 1; }
            if (w2k_edit_press(dt->timef, &e->xbutton)) { dt->focus = 2; w2k_win_dirty(w); return 1; }
            int bump = 0;
            if (w2k_rect_hit(&dt->yup, x, y)) { dt->down = 4; bump = 1; }
            if (w2k_rect_hit(&dt->ydn, x, y)) { dt->down = 5; bump = 2; }
            if (w2k_rect_hit(&dt->tup, x, y)) { dt->down = 6; bump = 3; }
            if (w2k_rect_hit(&dt->tdn, x, y)) { dt->down = 7; bump = 4; }
            if (bump) {
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
 * ------------------------------------------------------------------ */
typedef struct {
    W2kWin    *win;
    W2kTabs   *tabs;
    W2kRect    ok, cancel, apply;
    W2kSlider  bright;
    W2kPower   pw;
    int        have_backlight, cur_bright, want_bright;
    int        down, dirty;
} PowerDlg;

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
    pd->dirty = pd->want_bright != pd->cur_bright;
    w2k_win_dirty(pd->win);
}

static void pw_commit(PowerDlg *pd)
{
    if (!pd->dirty) return;
    if (w2k_backlight_set(pd->want_bright) == 0) {
        pd->cur_bright = pd->want_bright;
        pd->dirty = 0;
    } else {
        w2k_msgbox(pd->win, "Power Options",
                   "The brightness could not be changed. Install brightnessctl, or\n"
                   "give your user write access to /sys/class/backlight.",
                   MB_OK | MB_ICONERROR);
    }
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
        if (pd->tabs->sel == 1 && pd->have_backlight &&
            w2k_slider_press(&pd->bright, &e->xbutton)) { w2k_win_dirty(w); return 1; }
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
        if (pd->tabs->sel == 1 && pd->have_backlight &&
            w2k_slider_motion(&pd->bright, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { pw_commit(pd); w2k_win_close(w, ID_OK); return 1; }
        if (w2k_tabs_key(pd->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (pd->tabs->sel == 1 && pd->have_backlight &&
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
    int cw = 398, chh = 372;
    W2kWin *w = w2k_win_new("Power Options Properties", "l2kcontrol", cw, chh, 0);
    pd.win = w;
    pd.tabs = w2k_tabs_new(&pd, NULL);
    w2k_tabs_add(pd.tabs, "Power Meter");
    w2k_tabs_add(pd.tabs, "Brightness");
    pd.tabs->r = (W2kRect){ 7, 7, cw - 14, chh - 7 - 41 };
    W2kRect c = w2k_tabs_client(pd.tabs);

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
    w2k_tabs_free(pd.tabs);
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
    switch (i) {
    case AP_DEFAULTS: open_defaults(); break;
    case AP_SYSTEM:   open_performance(); break;
    case AP_FOLDER:   w2k_folder_options(cp.win); break;
    case AP_MOUSE:    open_mouse(); break;
    case AP_KEYBOARD: open_keyboard(); break;
    case AP_SOUNDS:   open_sounds(); break;
    case AP_FONTS:    open_fonts(); break;
    case AP_DATETIME: open_datetime(); break;
    case AP_POWER:    open_power(); break;
    }
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
        open_applet(cp.fw->list->sel);
        break;
    case FW_REFRESH:
        pane_fill(cp.fw->list->sel);
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
            open_applet(cp.fw->list->sel);
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
        };
        for (int i = 0; i < (int)(sizeof direct / sizeof *direct); i++)
            if (!strcasecmp(argv[1], direct[i].word)) {
                direct[i].fn();
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
    int laptop = w2k_is_laptop();
    for (int i = 0; i < NAPPLETS; i++) {
        if (i == AP_POWER && !laptop) continue;   /* nothing to meter or dim */
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
