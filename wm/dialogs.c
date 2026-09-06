/* dialogs.c -- the shell's own dialogs: Run, Shut Down, Log Off and About.
 *
 * These run inside the window manager process. While one is up the WM's own
 * event loop is not running, so w2k_win_foreign_event is pointed back at the
 * WM's handler and window management continues normally. */
#include "wm.h"
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

static void blink(void *v);

/* ------------------------------------------------------------------ *
 * Search
 * ------------------------------------------------------------------ *
 * Opened by typing into the Start menu. The query filters the same list of
 * installed programs the Programs menu is built from; Enter (or a double
 * click) runs the highlighted one. */
typedef struct {
    W2kEdit *edit;
    W2kList *list;
    int      ids[64];
    int      n;
    int      chosen;
} SearchDlg;

static void search_activate(void *user, int idx)
{
    SearchDlg *sd = user;
    if (idx >= 0 && idx < sd->n) sd->chosen = sd->ids[idx];
}

static void search_refresh(SearchDlg *sd)
{
    const char *names[64];
    sd->n = programs_search(w2k_edit_text(sd->edit), sd->ids, names, 64);
    w2k_list_clear(sd->list);
    for (int i = 0; i < sd->n; i++) {
        int r = w2k_list_add(sd->list, programs_icon(sd->ids[i]), NULL);
        w2k_list_set(sd->list, r, 0, names[i]);
    }
    if (sd->n) {
        sd->list->sel = 0;
        sd->list->items[0].selected = 1;
    }
}

static void search_paint(W2kWin *w, Drawable d)
{
    SearchDlg *sd = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_text_mnemonic(d, F_UI, 10, 12, "&Search for programs:", C_TEXT, 1);
    w2k_edit_draw(d, sd->edit);
    w2k_list_draw(d, sd->list);
    const char *hint = sd->n ? "Enter to run, Esc to close"
                             : "No matching programs";
    w2k_text(d, F_UI, 10, w->h - fh - 8, hint, C_GRAYTEXT);
}

static int search_event(W2kWin *w, XEvent *e)
{
    SearchDlg *sd = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_edit_press(sd->edit, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_list_press(sd->list, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        return 1;
    case ButtonRelease:
        w2k_list_release(sd->list, &e->xbutton);
        w2k_edit_release(sd->edit);
        if (sd->chosen) w2k_win_close(w, ID_OK);   /* double-clicked a match */
        return 1;
    case MotionNotify:
        if (w2k_edit_motion(sd->edit, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            if (sd->list->sel >= 0 && sd->list->sel < sd->n)
                sd->chosen = sd->ids[sd->list->sel];
            w2k_win_close(w, ID_OK);
            return 1;
        }
        if (ks == XK_Down || ks == XK_Up || ks == XK_Prior || ks == XK_Next) {
            w2k_list_key(sd->list, &e->xkey);      /* move through matches */
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_edit_key(sd->edit, &e->xkey)) {
            search_refresh(sd);
            w2k_win_dirty(w);
            return 1;
        }
        return 1;
    }
    }
    return 0;
}

/* `first` is the character that was typed at the Start menu, or NULL. */
void wm_search_dialog(const char *first)
{
    SearchDlg sd = { 0 };
    int cw = 320, chh = 280;
    W2kWin *w = w2k_win_new("Search", "w2kshell", cw, chh, 0);

    sd.edit = w2k_edit_new(0);
    w2k_edit_bind(sd.edit, w);
    sd.edit->focused = 1;
    sd.edit->r = (W2kRect){ 10, 28, cw - 20, 21 };
    if (first && *first) {
        /* The caret goes after it: w2k_edit_set() leaves the caret at the
         * start, and the rest of the word then went in ahead of the first
         * letter -- "irefoxf". */
        w2k_edit_set(sd.edit, first);
        sd.edit->caret = sd.edit->sel = (int)strlen(first);
    }

    /* One match per row, reading top to bottom -- the multi-column List view
     * scatters search results across the box. */
    sd.list = w2k_list_new(LV_REPORT);
    sd.list->r = (W2kRect){ 10, 58, cw - 20, chh - 58 - 30 };
    sd.list->focused = 1;
    sd.list->hdr_h = 0;                 /* no column header on a result list */
    w2k_list_add_col(sd.list, "Program", cw - 24 - SCROLL_W, 0);
    sd.list->user = &sd;
    sd.list->on_activate = search_activate;
    w2k_scroll_bind(&sd.list->vsb, w);
    search_refresh(&sd);

    w->user = &sd;
    w->paint = search_paint;
    w->event = search_event;
    w2k_win_center(w, NULL);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);

    w2k_add_timer(w2k_caret_blink, blink, sd.edit);
    int rc = w2k_win_modal(w);
    w2k_del_timer(blink, sd.edit);

    if (rc == ID_OK && sd.chosen) programs_run(sd.chosen, NULL);
    w2k_edit_free(sd.edit);
    w2k_list_free(sd.list);
}

/* ------------------------------------------------------------------ *
 * Start menu settings
 * ------------------------------------------------------------------ */
typedef struct {
    W2kRect  ok, cancel, apply;
    W2kTabs *tabs;                    /* General / Start Menu            */
    int      down, dirty;
    W2kEdit *custom;                  /* the "Custom" banner text        */
    W2kEdit *rgb[2][3];               /* top and bottom gradient colours */
    W2kRect  text_radio[3], icon_radio[3], dither_box, search_box;
    W2kRect  ontop_box, autohide_box, clock_box;
    W2kRect  labels_box, small_box;   /* Windows 7's button options */
    W2kRect  smallicons_box, personalized_box;
    W2kRect  style_radio[2];
    W2kRect  width_radio[2];          /* Windows 2000 / Windows 98 columns */
    int      width;
    W2kRect  preview;                 /* the General page's picture      */
    W2kRect  preview2;                /* the Start Menu page's banner    */
    int      mode, icon, dither, search;   /* edited copies              */
    int      ontop, autohide, showclock, smallicons, personalized;
    int      labels, tsmall;
    int      panel;            /* two-column Start menu */
    int      col[2][3];
} StartDlg;

static void startdlg_sync_edits(StartDlg *sd)
{
    char t[8];
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 3; k++) {
            snprintf(t, sizeof t, "%d", sd->col[i][k]);
            w2k_edit_set(sd->rgb[i][k], t);
        }
}

/* Copy the dialog's working values into the live settings. */
static void startdlg_commit(StartDlg *sd)
{
    w2k_start_banner_mode = sd->mode;
    w2k_start_icon = sd->icon;
    w2k_start_banner_dither = sd->dither;
    w2k_start_search = sd->search;
    w2k_taskbar_ontop = sd->ontop;
    w2k_taskbar_autohide = sd->autohide;
    w2k_taskbar_showclock = sd->showclock;
    w2k_taskbar_labels = sd->labels;
    w2k_taskbar_small = sd->tsmall;
    w2k_start_small_icons = sd->smallicons;
    w2k_start_panel = sd->panel;
    w2k_start_personalized = sd->personalized;
    w2k_start_width = sd->width;
    taskbar_init();                    /* re-place and re-stack the bar */
    wm_update_workarea();
    clients_restack();
    snprintf(w2k_start_banner_custom, sizeof w2k_start_banner_custom, "%s",
             w2k_edit_text(sd->custom));
    for (int k = 0; k < 3; k++) {
        int a = atoi(w2k_edit_text(sd->rgb[0][k]));
        int b = atoi(w2k_edit_text(sd->rgb[1][k]));
        w2k_start_banner_top[k]    = a < 0 ? 0 : a > 255 ? 255 : a;
        w2k_start_banner_bottom[k] = b < 0 ? 0 : b > 255 ? 255 : b;
    }
    w2k_start_icon_apply();
    w2k_scheme_save(NULL);
    w2k_scheme_broadcast();
    taskbar_paint();
}

/* The banner strip as the edits describe it, `w` wide and `h` tall, with
 * the name up it. Windows drew the same picture on this page. */
static void startdlg_banner(StartDlg *sd, Drawable d, int x, int y, int w, int h)
{
    int save_dither = w2k_start_banner_dither, save_top[3], save_bot[3];
    for (int k = 0; k < 3; k++) {
        save_top[k] = w2k_start_banner_top[k];
        save_bot[k] = w2k_start_banner_bottom[k];
        w2k_start_banner_top[k] = atoi(w2k_edit_text(sd->rgb[0][k]));
        w2k_start_banner_bottom[k] = atoi(w2k_edit_text(sd->rgb[1][k]));
    }
    w2k_start_banner_dither = sd->dither;
    w2k_menu_banner_fill(d, x, y, w, h);
    int save_mode = w2k_start_banner_mode;
    w2k_start_banner_mode = sd->mode;
    const char *banner = sd->mode == SB_CUSTOM ? w2k_edit_text(sd->custom)
                                               : w2k_start_banner_text();
    w2k_clip_set(x, y, w, h);
    w2k_text_vertical(d, F_UI_BOLD, x + 4, y + h - 6, banner, C_WHITE);
    w2k_clip_clear();
    w2k_start_banner_mode = save_mode;
    w2k_start_banner_dither = save_dither;
    for (int k = 0; k < 3; k++) {
        w2k_start_banner_top[k] = save_top[k];
        w2k_start_banner_bottom[k] = save_bot[k];
    }
}

/* The Start button with the icon chosen, at (x, y); returns its width. */
static int startdlg_button(StartDlg *sd, Drawable d, int x, int y)
{
    int bw = 20 + w2k_mnemonic_width(F_UI_BOLD, "Start") + 8;
    w2k_button(d, x, y, bw, 22, 0);
    /* The flag slot carries whichever icon is applied; the choice shows
     * once it is. */
    w2k_icon_draw(d, x + 3, y + 3, ICO_STARTFLAG);
    w2k_text_mnemonic(d, F_UI_BOLD, x + 21, y + (22 - w2k_font_height(F_UI_BOLD)) / 2,
                      "Start", C_TEXT, 0);
    return bw;
}

/* The General page's picture: a slice of desktop with the Start menu
 * open over the taskbar, drawn from the settings as they stand -- the
 * menu's style and icon size, the bar's clock, the banner. */
static void startdlg_preview(StartDlg *sd, Drawable d)
{
    W2kRect pv = sd->preview;
    w2k_edge(d, pv.x, pv.y, pv.w, pv.h, EDGE_SUNKEN, BF_RECT);
    int x0 = pv.x + 2, y0 = pv.y + 2, w = pv.w - 4, h = pv.h - 4;
    w2k_clip_set(x0, y0, w, h);
    w2k_fill(d, x0, y0, w, h, C_DESKTOP);

    /* The taskbar along the bottom. */
    int tb_h = 28, tby = y0 + h - tb_h;
    w2k_fill(d, x0, tby, w, tb_h, C_FACE);
    w2k_hline(d, x0, tby, w, C_LIGHT);
    w2k_hline(d, x0, tby + 1, w, C_HILIGHT);
    int by = tby + 4;
    int bw = startdlg_button(sd, d, x0 + 2, by);
    /* a task button and the tray */
    int tx = x0 + 2 + bw + 12;
    w2k_button(d, tx, by, 120, 22, 1);
    w2k_icon_draw(d, tx + 5, by + 3, ICO_EXPLORER);
    w2k_text(d, F_UI, tx + 24, by + (22 - w2k_font_height(F_UI)) / 2, "My Computer", C_TEXT);
    if (sd->showclock) {
        int cw = w2k_text_width(F_UI, "12:00 PM", -1) + 12;
        int cx = x0 + w - 2 - cw;
        w2k_edge(d, cx, by, cw, 22, EDGE_SUNKEN_THIN, BF_RECT);
        w2k_text(d, F_UI, cx + 6, by + (22 - w2k_font_height(F_UI)) / 2, "12:00 PM", C_TEXT);
    }

    /* The Start menu above the button. */
    int big = !sd->smallicons;
    int row = big ? 34 : 18, icol = big ? 38 : 20;
    const char *items[] = { "Programs", "Documents", "Settings", "Search", "Help", "Run...", "Shut Down..." };
    const int icons[] = { ICO_PROGRAMS, ICO_DOCUMENTS, ICO_SETTINGS, ICO_SEARCH, ICO_HELP, ICO_RUN, ICO_SHUTDOWN };
    int n = 7;
    int mh = 3 + n * row + 3 + (big ? 0 : 4);
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int tw = w2k_text_width(F_UI, items[i], -1);
        if (tw > maxw) maxw = tw;
    }
    int mw = 21 + icol + maxw + 14 + 6;
    if (sd->panel) {
        /* The two-column panel: a blue-topped box standing in for it. */
        int px = x0 + 2, pw = 200, ph = h - tb_h - 6, py = tby - ph;
        w2k_fill(d, px, py, pw, ph, C_WINDOW);
        w2k_fill_rgb(d, px, py, pw, 34, 0, 82, 195);
        w2k_fill_rgb(d, px + pw / 2, py + 34, pw / 2, ph - 34 - 26, 212, 228, 248);
        w2k_fill_rgb(d, px, py + ph - 26, pw, 26, 0, 82, 195);
        w2k_text_rgb(d, F_UI_BOLD, px + 40, py + 10, "Linux 2000", 255, 255, 255);
        w2k_bigicon_draw(d, px + 6, py + 2, ICO_MYCOMPUTER);
        w2k_frame(d, px, py, pw, ph, C_WINDOWFRAME);
    } else {
        int my = tby - mh, mx = x0 + 2;   /* the top runs off the picture */
        w2k_fill(d, mx, my, mw, mh, C_MENU);
        w2k_edge(d, mx, my, mw, mh, EDGE_RAISED, BF_RECT);
        startdlg_banner(sd, d, mx + 3, my + 3, 21, mh - 6);
        w2k_clip_set(x0, y0, w, h);        /* the banner cleared it */
        int y = my + 3;
        for (int i = 0; i < n; i++) {
            int ix = mx + 3 + 21;
            if (big) w2k_bigicon_draw(d, ix + 3, y + 1, icons[i]);
            else     w2k_icon_draw(d, ix + 2, y + 1, icons[i]);
            w2k_text(d, F_UI, ix + icol, y + (row - w2k_font_height(F_UI)) / 2, items[i],
                     C_MENUTEXT);
            if (i < 4) {
                int ax = mx + mw - 12, ay = y + row / 2 - 3;
                for (int k = 0; k < 4; k++) w2k_fill(d, ax + k, ay + 3 - k, 1, 1 + 2 * k, C_MENUTEXT);
            }
            y += row;
        }
    }
    w2k_clip_clear();
}

static void startdlg_paint(W2kWin *w, Drawable d)
{
    StartDlg *sd = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_tabs_draw(d, sd->tabs);

    if (sd->tabs->sel == 0) {
        startdlg_preview(sd, d);

        W2kRect g3 = { sd->style_radio[0].x - 10, sd->style_radio[0].y - 16, sd->preview.w,
                       (sd->style_radio[1].y + fh + 4 + 8) - (sd->style_radio[0].y - 16) };
        w2k_draw_groupbox(d, &g3, "Start menu");
        w2k_draw_radio(d, sd->style_radio[0].x, sd->style_radio[0].y,
                       "&Start menu (two columns, as Windows XP)", sd->panel, 0, 0);
        w2k_draw_radio(d, sd->style_radio[1].x, sd->style_radio[1].y,
                       "&Classic Start menu (Windows 2000)", !sd->panel, 0, 0);

        W2kRect g4 = { sd->ontop_box.x - 10, sd->ontop_box.y - 16, sd->preview.w,
                       (sd->small_box.y + 16 + 8) - (sd->ontop_box.y - 16) };
        w2k_draw_groupbox(d, &g4, "Taskbar");
        w2k_draw_checkbox(d, sd->ontop_box.x, sd->ontop_box.y, "Always on t&op", sd->ontop, 0, 0);
        w2k_draw_checkbox(d, sd->autohide_box.x, sd->autohide_box.y, "A&uto hide", sd->autohide, 0, 0);
        w2k_draw_checkbox(d, sd->clock_box.x, sd->clock_box.y, "Show cloc&k", sd->showclock, 0, 0);
        w2k_draw_checkbox(d, sd->labels_box.x, sd->labels_box.y,
                          "&Never combine taskbar buttons (Windows 7 look)", sd->labels, 0, 0);
        w2k_draw_checkbox(d, sd->small_box.x, sd->small_box.y,
                          "Use s&mall taskbar icons (Windows 7 look)", sd->tsmall, 0, 0);
    } else {
        /* The banner beside its settings, as the real page showed it. */
        W2kRect pv = sd->preview2;
        w2k_edge(d, pv.x, pv.y, pv.w, pv.h, EDGE_SUNKEN, BF_RECT);
        w2k_fill(d, pv.x + 2, pv.y + 2, pv.w - 4, pv.h - 4, C_MENU);
        startdlg_banner(sd, d, pv.x + 4, pv.y + 4, 21, pv.h - 8);
        w2k_bigicon_draw(d, pv.x + 4 + 21 + 4, pv.y + 6, ICO_PROGRAMS);
        w2k_text(d, F_UI, pv.x + 4 + 21 + 42, pv.y + 6 + (34 - fh) / 2, "Programs", C_MENUTEXT);
        w2k_bigicon_draw(d, pv.x + 4 + 21 + 4, pv.y + 6 + 34, ICO_DOCUMENTS);
        w2k_text(d, F_UI, pv.x + 4 + 21 + 42, pv.y + 6 + 34 + (34 - fh) / 2, "Documents", C_MENUTEXT);

        W2kRect g = { pv.x + pv.w + 8, pv.y, sd->preview.w - pv.w - 8, 3 * (fh + 6) + 22 };
        w2k_draw_groupbox(d, &g, "Banner text");
        const char *labels[3];
        labels[0] = "&Windows 2000 Professional";
        labels[1] = w2k_distro_name();
        labels[2] = "C&ustom:";
        for (int i = 0; i < 3; i++)
            w2k_draw_radio(d, sd->text_radio[i].x, sd->text_radio[i].y, labels[i],
                           sd->mode == i, 0, 0);
        w2k_edit_draw(d, sd->custom);

        W2kRect g2 = { pv.x, sd->rgb[0][0]->r.y - fh - 3 - 18, sd->preview.w, 2 * 26 + 52 + fh };
        w2k_draw_groupbox(d, &g2, "Banner colour");
        const char *rows[2] = { "Top:", "Bottom:" };
        for (int i = 0; i < 2; i++) {
            w2k_text(d, F_UI, g2.x + 10, sd->rgb[i][0]->r.y + (21 - fh) / 2, rows[i], C_TEXT);
            for (int k = 0; k < 3; k++) w2k_edit_draw(d, sd->rgb[i][k]);
            int r = atoi(w2k_edit_text(sd->rgb[i][0]));
            int gg = atoi(w2k_edit_text(sd->rgb[i][1]));
            int b = atoi(w2k_edit_text(sd->rgb[i][2]));
            W2kRect sw = { sd->rgb[i][2]->r.x + 56, sd->rgb[i][0]->r.y, 40, 21 };
            w2k_fill_rgb(d, sw.x, sw.y, sw.w, sw.h, r & 255, gg & 255, b & 255);
            w2k_edge(d, sw.x, sw.y, sw.w, sw.h, EDGE_SUNKEN, BF_RECT);
        }
        const char *rgb_head[3] = { "Red", "Green", "Blue" };
        for (int k = 0; k < 3; k++)
            w2k_text(d, F_UI, sd->rgb[0][k]->r.x + 2, sd->rgb[0][0]->r.y - fh - 3, rgb_head[k], C_TEXT);
        w2k_draw_checkbox(d, sd->dither_box.x, sd->dither_box.y,
                          "Classic &dithered gradient", sd->dither, 0, 0);

        W2kRect g3 = { pv.x, sd->icon_radio[0].y - 16, sd->preview.w,
                       (sd->width_radio[1].y + fh + 4 + 8) - (sd->icon_radio[0].y - 16) };
        w2k_draw_groupbox(d, &g3, "Start button and menu");
        const char *icons[3] = { "Windows &flag", "&Tux", "&Distribution logo" };
        for (int i = 0; i < 3; i++)
            w2k_draw_radio(d, sd->icon_radio[i].x, sd->icon_radio[i].y, icons[i],
                           sd->icon == i, 0, 0);
        w2k_draw_checkbox(d, sd->search_box.x, sd->search_box.y,
                          "&Search for programs when you type in the Start menu", sd->search, 0, 0);
        w2k_draw_checkbox(d, sd->smallicons_box.x, sd->smallicons_box.y,
                          "Show small &icons in Start menu", sd->smallicons, 0, 0);
        w2k_draw_checkbox(d, sd->personalized_box.x, sd->personalized_box.y,
                          "Use &Personalized Menus", sd->personalized, 0, 0);
        w2k_draw_radio(d, sd->width_radio[0].x, sd->width_radio[0].y,
                       "Windows &2000 columns (wider, as measured)", sd->width, 0, 0);
        w2k_draw_radio(d, sd->width_radio[1].x, sd->width_radio[1].y,
                       "Windows &98 columns (narrow)", !sd->width, 0, 0);
    }

    w2k_draw_pushbutton(d, &sd->ok, "OK", BS_DEFAULT | (sd->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &sd->cancel, "Cancel", sd->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &sd->apply, "&Apply",
                        (sd->dirty ? 0 : BS_DISABLED) | (sd->down == 3 ? BS_PRESSED : 0));
}

static int startdlg_event(W2kWin *w, XEvent *e)
{
    StartDlg *sd = w->user;
    int page = sd->tabs->sel;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_tabs_press(sd->tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (page == 1) {
            if (w2k_edit_press(sd->custom, &e->xbutton)) {
                sd->mode = SB_CUSTOM;
                sd->dirty = 1;
                w2k_win_dirty(w);
                return 1;
            }
            for (int i = 0; i < 2; i++)
                for (int k = 0; k < 3; k++)
                    if (w2k_edit_press(sd->rgb[i][k], &e->xbutton)) {
                        sd->custom->focused = 0;
                        for (int a = 0; a < 2; a++)
                            for (int b = 0; b < 3; b++)
                                if (a != i || b != k) sd->rgb[a][b]->focused = 0;
                        sd->dirty = 1;
                        w2k_win_dirty(w);
                        return 1;
                    }
            for (int i = 0; i < 3; i++) {
                if (w2k_rect_hit(&sd->text_radio[i], x, y)) {
                    sd->mode = i; sd->dirty = 1; w2k_win_dirty(w); return 1;
                }
                if (w2k_rect_hit(&sd->icon_radio[i], x, y)) {
                    sd->icon = i; sd->dirty = 1; w2k_win_dirty(w); return 1;
                }
            }
            if (w2k_rect_hit(&sd->dither_box, x, y)) {
                sd->dither = !sd->dither; sd->dirty = 1; w2k_win_dirty(w); return 1;
            }
            if (w2k_rect_hit(&sd->search_box, x, y)) {
                sd->search = !sd->search; sd->dirty = 1; w2k_win_dirty(w); return 1;
            }
            if (w2k_rect_hit(&sd->smallicons_box, x, y)) {
                sd->smallicons = !sd->smallicons; sd->dirty = 1; w2k_win_dirty(w); return 1;
            }
            if (w2k_rect_hit(&sd->personalized_box, x, y)) {
                sd->personalized = !sd->personalized; sd->dirty = 1; w2k_win_dirty(w); return 1;
            }
            for (int i = 0; i < 2; i++)
                if (w2k_rect_hit(&sd->width_radio[i], x, y)) {
                    sd->width = (i == 0); sd->dirty = 1; w2k_win_dirty(w); return 1;
                }
        } else {
            for (int i = 0; i < 2; i++)
                if (w2k_rect_hit(&sd->style_radio[i], x, y)) {
                    sd->panel = (i == 0);
                    sd->dirty = 1;
                    w2k_win_dirty(w);
                    return 1;
                }
            if (w2k_rect_hit(&sd->ontop_box, x, y)) {
                sd->ontop = !sd->ontop; sd->dirty = 1; w2k_win_dirty(w); return 1;
            }
            if (w2k_rect_hit(&sd->autohide_box, x, y)) {
                sd->autohide = !sd->autohide; sd->dirty = 1; w2k_win_dirty(w); return 1;
            }
            if (w2k_rect_hit(&sd->clock_box, x, y)) {
                sd->showclock = !sd->showclock; sd->dirty = 1; w2k_win_dirty(w); return 1;
            }
            if (w2k_rect_hit(&sd->labels_box, x, y)) {
                sd->labels = !sd->labels; sd->dirty = 1; w2k_win_dirty(w); return 1;
            }
            if (w2k_rect_hit(&sd->small_box, x, y)) {
                sd->tsmall = !sd->tsmall; sd->dirty = 1; w2k_win_dirty(w); return 1;
            }
        }
        if (w2k_rect_hit(&sd->ok, x, y)) sd->down = 1;
        else if (w2k_rect_hit(&sd->cancel, x, y)) sd->down = 2;
        else if (w2k_rect_hit(&sd->apply, x, y) && sd->dirty) sd->down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int d = sd->down, x = e->xbutton.x, y = e->xbutton.y;
        sd->down = 0;
        w2k_edit_release(sd->custom);
        for (int i = 0; i < 2; i++)
            for (int k = 0; k < 3; k++) w2k_edit_release(sd->rgb[i][k]);
        if (d == 1 && w2k_rect_hit(&sd->ok, x, y)) {
            startdlg_commit(sd);
            w2k_win_close(w, ID_OK);
        } else if (d == 2 && w2k_rect_hit(&sd->cancel, x, y)) {
            w2k_win_close(w, ID_CANCEL);
        } else if (d == 3 && w2k_rect_hit(&sd->apply, x, y)) {
            startdlg_commit(sd);
            sd->dirty = 0;
        }
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (page == 1 && w2k_edit_motion(sd->custom, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            startdlg_commit(sd);
            w2k_win_close(w, ID_OK);
            return 1;
        }
        if (w2k_tabs_key(sd->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (page == 1 && sd->custom->focused && w2k_edit_key(sd->custom, &e->xkey)) {
            sd->mode = SB_CUSTOM;
            sd->dirty = 1;
            w2k_win_dirty(w);
            return 1;
        }
        if (page == 1)
            for (int i = 0; i < 2; i++)
                for (int k = 0; k < 3; k++)
                    if (sd->rgb[i][k]->focused &&
                        w2k_edit_key(sd->rgb[i][k], &e->xkey)) {
                        sd->dirty = 1;
                        w2k_win_dirty(w);
                        return 1;
                    }
        return 1;
    }
    }
    return 0;
}

void wm_startmenu_dialog(void)
{
    StartDlg sd = { 0 };
    /* Two pages, as Windows 2000's dialog had: the whole thing fits a
     * 600-line screen with room to spare. */
    int cw = 400, chh = 494;
    W2kWin *w = w2k_win_new("Taskbar and Start Menu Properties",
                            "w2kshell", cw, chh, 0);
    int fh = w2k_font_height(F_UI);

    sd.mode = w2k_start_banner_mode;
    sd.icon = w2k_start_icon;
    sd.dither = w2k_start_banner_dither;
    sd.search = w2k_start_search;
    sd.ontop = w2k_taskbar_ontop;
    sd.autohide = w2k_taskbar_autohide;
    sd.showclock = w2k_taskbar_showclock;
    sd.labels = w2k_taskbar_labels;
    sd.tsmall = w2k_taskbar_small;
    sd.smallicons = w2k_start_small_icons;
    sd.panel = w2k_start_panel;
    sd.personalized = w2k_start_personalized;
    sd.width = w2k_start_width;
    for (int k = 0; k < 3; k++) {
        sd.col[0][k] = w2k_start_banner_top[k];
        sd.col[1][k] = w2k_start_banner_bottom[k];
    }

    sd.tabs = w2k_tabs_new(&sd, NULL);
    w2k_tabs_add(sd.tabs, "General");
    w2k_tabs_add(sd.tabs, "Start Menu");
    sd.tabs->r = (W2kRect){ 7, 7, cw - 14, chh - 7 - 41 };
    W2kRect c = w2k_tabs_client(sd.tabs);

    /* General: the picture, the menu style, the bar. */
    sd.preview = (W2kRect){ c.x + 10, c.y + 10, c.w - 20, 176 };
    int y3 = sd.preview.y + sd.preview.h + 10 + 16;
    for (int i = 0; i < 2; i++)
        sd.style_radio[i] = (W2kRect){ c.x + 20, y3 + i * (fh + 6), c.w - 60, fh + 4 };
    int y4 = sd.style_radio[1].y + fh + 4 + 8 + 8 + 16;
    sd.ontop_box    = (W2kRect){ c.x + 20, y4, c.w - 40, 16 };
    sd.autohide_box = (W2kRect){ c.x + 20, y4 + fh + 6, c.w - 40, 16 };
    sd.clock_box    = (W2kRect){ c.x + 20, y4 + 2 * (fh + 6), c.w - 40, 16 };
    sd.labels_box   = (W2kRect){ c.x + 20, y4 + 3 * (fh + 6), c.w - 40, 16 };
    sd.small_box    = (W2kRect){ c.x + 20, y4 + 4 * (fh + 6), c.w - 40, 16 };

    /* Start Menu: the banner beside its text, the colours, the button. */
    sd.preview2 = (W2kRect){ c.x + 10, c.y + 10, 150, 3 * (fh + 6) + 22 };
    int gx = sd.preview2.x + sd.preview2.w + 8;
    int y = sd.preview2.y + 16;
    for (int i = 0; i < 3; i++)
        sd.text_radio[i] = (W2kRect){ gx + 10, y + i * (fh + 6), c.w - gx - 20, fh + 4 };
    sd.custom = w2k_edit_new(0);
    w2k_edit_bind(sd.custom, w);
    sd.custom->r = (W2kRect){ gx + 76, y + 2 * (fh + 6) - 3, c.x + c.w - 20 - (gx + 76), 21 };
    w2k_edit_set(sd.custom, w2k_start_banner_custom);

    int y2 = sd.preview2.y + sd.preview2.h + 10 + 16 + fh + 4;   /* + the Red/Green/Blue row */
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 3; k++) {
            sd.rgb[i][k] = w2k_edit_new(0);
            w2k_edit_bind(sd.rgb[i][k], w);
            sd.rgb[i][k]->r = (W2kRect){ c.x + 74 + k * 46, y2 + i * 26, 40, 21 };
        }
    startdlg_sync_edits(&sd);
    sd.dither_box = (W2kRect){ c.x + 20, y2 + 2 * 26 + 6, c.w - 40, 16 };

    int y5 = sd.dither_box.y + 16 + 8 + 8 + 16;
    for (int i = 0; i < 3; i++)
        sd.icon_radio[i] = (W2kRect){ c.x + 20, y5 + i * (fh + 6), c.w - 60, fh + 4 };
    sd.search_box       = (W2kRect){ c.x + 20, y5 + 3 * (fh + 6) + 2, c.w - 40, 16 };
    sd.smallicons_box   = (W2kRect){ c.x + 20, y5 + 4 * (fh + 6) + 2, c.w - 40, 16 };
    sd.personalized_box = (W2kRect){ c.x + 20, y5 + 5 * (fh + 6) + 2, c.w - 40, 16 };
    sd.width_radio[0] = (W2kRect){ c.x + 20, y5 + 6 * (fh + 6) + 4, c.w - 60, fh + 4 };
    sd.width_radio[1] = (W2kRect){ c.x + 20, y5 + 7 * (fh + 6) + 4, c.w - 60, fh + 4 };

    int by = chh - 12 - 23;
    sd.apply  = (W2kRect){ cw - 12 - 75, by, 75, 23 };
    sd.cancel = (W2kRect){ cw - 12 - 75 * 2 - 6, by, 75, 23 };
    sd.ok     = (W2kRect){ cw - 12 - 75 * 3 - 12, by, 75, 23 };

    w->user = &sd;
    w->paint = startdlg_paint;
    w->event = startdlg_event;
    w2k_win_center(w, NULL);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);

    /* W2K_RENDER_TAB picks the page for the render harness. */
    if (getenv("W2K_RENDER_TAB")) sd.tabs->sel = atoi(getenv("W2K_RENDER_TAB")) ? 1 : 0;

    w2k_add_timer(w2k_caret_blink, blink, sd.custom);
    w2k_win_modal(w);
    w2k_del_timer(blink, sd.custom);

    w2k_tabs_free(sd.tabs);
    w2k_edit_free(sd.custom);
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 3; k++) w2k_edit_free(sd.rgb[i][k]);
}

/* ------------------------------------------------------------------ *
 * Run
 * ------------------------------------------------------------------ */
typedef struct {
    W2kCombo *open;                 /* editable, with the run history */
    W2kRect   ok, cancel, browse;
    int       down;
} RunDlg;

/* The Run box remembers what has been typed into it, like its original.
 * One command per line, most recent first. */
#define RUN_MRU_MAX 12

static void run_mru_path(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.w2k/runmru", home ? home : ".");
}

static void run_mru_load(W2kCombo *c)
{
    char path[1024];
    run_mru_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (c->n < RUN_MRU_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (*line) w2k_combo_add(c, line);
    }
    fclose(f);
}

static void run_mru_save(W2kCombo *c, const char *cmd)
{
    char path[1024];
    run_mru_path(path, sizeof path);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }

    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", cmd);
    int written = 1;
    for (int i = 0; i < c->n && written < RUN_MRU_MAX; i++)
        if (strcmp(c->items[i], cmd)) {          /* no duplicates */
            fprintf(f, "%s\n", c->items[i]);
            written++;
        }
    fclose(f);
}

static const char *run_blurb =
    "Type the name of a program, folder, document, or\n"
    "Internet resource, and Windows will open it for you.";

/* Longest common prefix length of a and b. */
static int run_common_prefix(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return i;
}

/* Tab-complete a path that starts with '/'.  Returns 1 if the text changed. */
static int run_complete_path(W2kEdit *e)
{
    const char *text = w2k_edit_text(e);
    if (!text || text[0] != '/') return 0;

    char dir[1024], prefix[256];
    const char *slash = strrchr(text, '/');
    size_t dlen = (size_t)(slash - text);
    if (dlen >= sizeof dir) return 0;
    if (dlen == 0) {
        dir[0] = '/'; dir[1] = 0;
    } else {
        memcpy(dir, text, dlen);
        dir[dlen] = 0;
    }
    snprintf(prefix, sizeof prefix, "%s", slash + 1);

    DIR *dp = opendir(dir);
    if (!dp) return 0;

    char best[256] = "";
    int  best_len = -1, nmatch = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.' && prefix[0] != '.') continue;
        if (strncmp(de->d_name, prefix, strlen(prefix)) != 0) continue;
        nmatch++;
        if (best_len < 0) {
            snprintf(best, sizeof best, "%s", de->d_name);
            best_len = (int)strlen(best);
        } else {
            int c = run_common_prefix(best, de->d_name);
            if (c < best_len) {
                best[c] = 0;
                best_len = c;
            }
        }
    }
    closedir(dp);
    if (nmatch == 0 || best_len <= (int)strlen(prefix)) return 0;

    char out[1024];
    if (dir[0] == '/' && dir[1] == 0)
        snprintf(out, sizeof out, "/%s", best);
    else
        snprintf(out, sizeof out, "%s/%s", dir, best);

    /* Exact unique match that is a directory: append '/'. */
    if (nmatch == 1) {
        struct stat st;
        if (stat(out, &st) == 0 && S_ISDIR(st.st_mode))
            strncat(out, "/", sizeof out - strlen(out) - 1);
    }

    w2k_edit_set(e, out);
    e->caret = e->sel = (int)strlen(out);
    return 1;
}

/* Tab-complete the first word against $PATH binaries. */
static int run_complete_cmd(W2kEdit *e)
{
    const char *text = w2k_edit_text(e);
    if (!text || !*text || text[0] == '/' || text[0] == '.') return 0;

    char word[256];
    size_t wlen = strcspn(text, " \t");
    if (wlen == 0 || wlen >= sizeof word) return 0;
    memcpy(word, text, wlen);
    word[wlen] = 0;

    const char *pathenv = getenv("PATH");
    if (!pathenv) return 0;
    char *path = strdup(pathenv);
    if (!path) return 0;

    char best[256] = "";
    int  best_len = -1, nmatch = 0;
    for (char *dir = strtok(path, ":"); dir; dir = strtok(NULL, ":")) {
        DIR *dp = opendir(dir);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (strncmp(de->d_name, word, wlen) != 0) continue;
            nmatch++;
            if (best_len < 0) {
                snprintf(best, sizeof best, "%s", de->d_name);
                best_len = (int)strlen(best);
            } else {
                int c = run_common_prefix(best, de->d_name);
                if (c < best_len) {
                    best[c] = 0;
                    best_len = c;
                }
            }
        }
        closedir(dp);
    }
    free(path);
    if (nmatch == 0 || best_len <= (int)wlen) return 0;

    char out[1024];
    if (text[wlen])
        snprintf(out, sizeof out, "%s%s", best, text + wlen);
    else
        snprintf(out, sizeof out, "%s", best);

    w2k_edit_set(e, out);
    e->caret = e->sel = (int)strlen(best);
    return 1;
}

static int run_tab_complete(W2kCombo *c)
{
    const char *t = w2k_edit_text(c->edit);
    if (!t || !*t) return 0;
    if (t[0] == '/') return run_complete_path(c->edit);
    return run_complete_cmd(c->edit);
}

static void run_paint(W2kWin *w, Drawable d)
{
    RunDlg *r = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_bigicon_draw(d, 14, 16, ICO_RUN);

    const char *p = run_blurb;
    for (int i = 0; i < 2; i++) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        w2k_textn(d, F_UI, 58, 16 + i * (fh + 3), p, n, C_TEXT);
        if (!nl) break;
        p = nl + 1;
    }
    w2k_text_mnemonic(d, F_UI, 14, r->open->r.y + (21 - fh) / 2, "&Open:",
                      C_TEXT, 1);
    w2k_combo_draw(d, r->open);
    w2k_draw_pushbutton(d, &r->ok, "OK",
                        BS_DEFAULT | (r->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &r->cancel, "Cancel", r->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &r->browse, "&Browse...",
                        r->down == 3 ? BS_PRESSED : 0);
}

static int run_event(W2kWin *w, XEvent *e)
{
    RunDlg *r = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_combo_press(r->open, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&r->ok, e->xbutton.x, e->xbutton.y))          r->down = 1;
        else if (w2k_rect_hit(&r->cancel, e->xbutton.x, e->xbutton.y)) r->down = 2;
        else if (w2k_rect_hit(&r->browse, e->xbutton.x, e->xbutton.y)) r->down = 3;
        w2k_win_dirty(w);
        return 1;
    case MotionNotify:
        if (w2k_edit_motion(r->open->edit, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        w2k_edit_release(r->open->edit);
        int d = r->down;
        r->down = 0;
        if (d == 1 && w2k_rect_hit(&r->ok, e->xbutton.x, e->xbutton.y))
            w2k_win_close(w, ID_OK);
        else if (d == 2 && w2k_rect_hit(&r->cancel, e->xbutton.x, e->xbutton.y))
            w2k_win_close(w, ID_CANCEL);
        else if (d == 3 && w2k_rect_hit(&r->browse, e->xbutton.x, e->xbutton.y)) {
            char path[1024];
            snprintf(path, sizeof path, "%s", w2k_combo_text(r->open));
            if (w2k_file_dialog(w, 0, path, sizeof path))
                w2k_combo_set_text(r->open, path);
        }
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { w2k_win_close(w, ID_OK); return 1; }
        if (ks == XK_Tab) {
            if (run_tab_complete(r->open))
                w2k_win_dirty(w);
            return 1;
        }
        if (w2k_combo_key(r->open, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void blink(void *v) { w2k_edit_blink(v); }

/* Point the framework's stray-event hook back at the WM for the duration of
 * a dialog, then put it back. */

void wm_run_dialog(void)
{
    RunDlg r = { 0 };
    /* Proportions of the original: icon and blurb across the top, the Open
     * combo on its own row, then the three buttons hard against the bottom
     * right. The old layout left a band of empty dialog in the middle. */
    int cw = 358, chh = 134;

    W2kWin *w = w2k_win_new("Run", "w2kshell", cw, chh, 0);
    r.open = w2k_combo_new(1);
    run_mru_load(r.open);
    w2k_edit_bind(r.open->edit, w);
    r.open->edit->focused = 1;
    r.open->focused = 1;

    r.open->r  = (W2kRect){ 58, 62, cw - 72, 21 };
    int by = chh - 12 - 23;
    r.browse   = (W2kRect){ cw - 12 - 75, by, 75, 23 };
    r.cancel   = (W2kRect){ cw - 12 - 75 * 2 - 6, by, 75, 23 };
    r.ok       = (W2kRect){ cw - 12 - 75 * 3 - 12, by, 75, 23 };

    w->user = &r;
    w->paint = run_paint;
    w->event = run_event;
    w2k_win_center(w, NULL);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);

    w2k_add_timer(w2k_caret_blink, blink, r.open->edit);
    int rc = w2k_win_modal(w);
    w2k_del_timer(blink, r.open->edit);

    if (rc == ID_OK) {
        const char *cmd = w2k_combo_text(r.open);
        if (*cmd) {
            run_mru_save(r.open, cmd);
            /* A path to a program runs, as it does in Windows; a path to
             * a document or folder, or a URL, opens through the desktop
             * handler. The name is quoted: spaces and shell characters
             * in a path must not be parsed by the shell. */
            struct stat st;
            int is_url = !strncasecmp(cmd, "https://", 8) ||
                         !strncasecmp(cmd, "http://", 7) ||
                         !strncasecmp(cmd, "file://", 7);
            int is_doc = cmd[0] == '/' && stat(cmd, &st) == 0 &&
                         !(S_ISREG(st.st_mode) && access(cmd, X_OK) == 0);
            if (is_url || is_doc) {
                char q[2200], open[2300];
                w2k_shell_quote(cmd, q, sizeof q);
                snprintf(open, sizeof open, "xdg-open %s", q);
                wm_spawn(open);
            } else {
                /* "cmd" and "command" open the terminal, as they open the
                 * command prompt in Windows. */
                char word[64];
                snprintf(word, sizeof word, "%.63s", cmd);
                word[strcspn(word, " \t")] = 0;
                if ((!strcasecmp(word, "cmd") || !strcasecmp(word, "cmd.exe") ||
                     !strcasecmp(word, "command") || !strcasecmp(word, "command.com")) &&
                    wm_terminal_cmd())
                    wm_spawn(wm_terminal_cmd());
                else
                    wm_spawn(cmd);
            }
        }
    }
    w2k_combo_free(r.open);
}

/* ------------------------------------------------------------------ *
 * Shut Down Windows
 * ------------------------------------------------------------------ */
typedef struct {
    W2kCombo *what;
    W2kRect   ok, cancel, help;
    int       down;
} ShutDlg;

static const char *shut_desc[] = {
    "Ends your session and closes every program, so that\n"
    "another user can log on.",
    "Closes all programs and ends the X session.",
    "Closes all programs and ends the session so it can be\n"
    "started again.",
    "Maintains your session, keeping the computer running on\n"
    "low power with data still in memory. To return to full\n"
    "power, press the power button or open the lid.",
    "Saves your session to disk and turns the computer off.\n"
    "When you turn it on again, your session is restored.",
};

static void shut_paint(W2kWin *w, Drawable d)
{
    ShutDlg *s = w->user;
    int fh = w2k_font_height(F_UI);

    /* The banner strip: the Start-menu gradient turned on its side. */
    int bw = 66, bh = 96;
    for (int i = 0; i < bh; i++) {
        int t = i * 255 / (bh - 1);
        XSetForeground(w2k.dpy, w2k.gc,
                       w2k_rgb(10 + (0 - 10) * t / 255,
                               36 + (0 - 36) * t / 255,
                               106 + (48 - 106) * t / 255));
        w2k_fill_fg(d, 0, 0 + i, bw, 1);
    }
    w2k_icon_draw(d, (bw - 16) / 2, 12, ICO_STARTFLAG);
    w2k_text(d, F_UI_BOLD, 6, 40, "Windows", C_WHITE);
    w2k_text(d, F_UI_BOLD, 12, 40 + fh, "2000", C_WHITE);

    w2k_text(d, F_UI, bw + 14, 16, "What do you want the computer to do?", C_TEXT);
    w2k_combo_draw(d, s->what);

    int di = s->what->sel < 0 ? 0 : s->what->sel;
    if (di >= 3 && di < s->what->n) {
        di = !strcmp(s->what->items[di], "Hibernate") ? 4 : 3;
    }
    const char *p = shut_desc[di];
    for (int i = 0; i < 3 && p; i++) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        w2k_textn(d, F_UI, bw + 14, 74 + i * (fh + 2), p, n, C_TEXT);
        p = nl ? nl + 1 : NULL;
    }
    w2k_draw_pushbutton(d, &s->ok, "OK",
                        BS_DEFAULT | (s->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &s->cancel, "Cancel", s->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &s->help, "&Help",
                        BS_DISABLED | (s->down == 3 ? BS_PRESSED : 0));
}

static int shut_event(W2kWin *w, XEvent *e)
{
    ShutDlg *s = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_combo_press(s->what, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&s->ok, e->xbutton.x, e->xbutton.y))          s->down = 1;
        else if (w2k_rect_hit(&s->cancel, e->xbutton.x, e->xbutton.y)) s->down = 2;
        w2k_win_dirty(w);
        return 1;
    case ButtonRelease: {
        int d = s->down;
        s->down = 0;
        if (d == 1 && w2k_rect_hit(&s->ok, e->xbutton.x, e->xbutton.y))
            w2k_win_close(w, ID_OK);
        else if (d == 2 && w2k_rect_hit(&s->cancel, e->xbutton.x, e->xbutton.y))
            w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { w2k_win_close(w, ID_OK); return 1; }
        if (ks == XK_Down && s->what->sel + 1 < s->what->n) {
            s->what->sel++;
            w2k_win_dirty(w);
            return 1;
        }
        if (ks == XK_Up && s->what->sel > 0) {
            s->what->sel--;
            w2k_win_dirty(w);
            return 1;
        }
        return 1;
    }
    }
    return 0;
}

void wm_shutdown_dialog(void)
{
    ShutDlg s = { 0 };
    int cw = 400, chh = 160;

    W2kWin *w = w2k_win_new("Shut Down Windows", "w2kshell", cw, chh, 0);
    s.what = w2k_combo_new(0);
    w2k_combo_add(s.what, "Log off");
    w2k_combo_add(s.what, "Shut down");
    w2k_combo_add(s.what, "Restart");
    if (w2k_power_can("suspend"))   w2k_combo_add(s.what, "Stand by");
    if (w2k_power_can("hibernate")) w2k_combo_add(s.what, "Hibernate");
    s.what->sel = 1;
    s.what->r = (W2kRect){ 80, 38, cw - 92, 21 };

    int by = chh - 12 - 23;
    s.help   = (W2kRect){ cw - 12 - 75, by, 75, 23 };
    s.cancel = (W2kRect){ cw - 12 - 75 * 2 - 6, by, 75, 23 };
    s.ok     = (W2kRect){ cw - 12 - 75 * 3 - 12, by, 75, 23 };

    w->user = &s;
    w->paint = shut_paint;
    w->event = shut_event;
    w2k_win_center(w, NULL);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);

    int rc = w2k_win_modal(w);
    int what = s.what->sel;
    char chosen[32] = "";
    if (what >= 0 && what < s.what->n) snprintf(chosen, sizeof chosen, "%s", s.what->items[what]);
    w2k_combo_free(s.what);

    /* 0 = log off, 1 = shut down, 2 = restart; l2k-session acts on it.
     * Standing by and hibernating keep the session: the machine is asked
     * directly, and comes back where it was. */
    if (rc != ID_OK) return;
    if (!strcmp(chosen, "Stand by"))       w2k_power_action("suspend");
    else if (!strcmp(chosen, "Hibernate")) w2k_power_action("hibernate");
    else wm_logout(what);
}

void wm_logoff_dialog(void)
{
    if (w2k_msgbox(NULL, "Log Off Windows",
                   "Are you sure you want to log off?",
                   MB_YESNO | MB_ICONQUESTION) == ID_YES)
        wm_logout(0);
}

void wm_help_dialog(void)
{
    w2k_msgbox(NULL, "About Windows",
               "Linux 2000\n"
               "A Windows 2000-style desktop for X11\n"
               "\n"
               "A window manager and desktop environment in the style of the\n"
               "Windows 2000 shell, using Xlib and nothing else.\n"
               "\n"
               "Press Ctrl+Esc for the Start menu, Alt+Tab to switch windows,\n"
               "and Ctrl+Alt+Del for Task Manager.\n"
               "\n"
               "Linux 2000 is not affiliated with, endorsed by or sponsored by Microsoft.\nWindows is a trademark of Microsoft Corporation.",
               MB_OK | MB_ICONINFO);
}

/* ------------------------------------------------------------------ *
 * Change Icon
 *
 * Windows shows the icons inside a chosen file and lets you pick one.
 * There are no icon resources in an ELF binary to show, so this offers
 * the shell's own artwork -- which is what a pinned shortcut usually
 * wants -- plus Browse for an .ico or .png of the user's own.
 *
 * The answer is written back as text: "w2k:<slug>" for a built-in, or the
 * path of the file that was browsed to (see pin_icon()).
 * ------------------------------------------------------------------ */
typedef struct {
    W2kList *list;
    W2kRect  ok, cancel, browse;
    int      down;
    char     chosen[256];
    W2kWin  *w;
} IconDlg;

static void icd_fill(IconDlg *d)
{
    w2k_list_clear(d->list);
    for (int id = 0; id < N_ICONS; id++) {
        const char *slug = w2k_icon_slug(id);
        if (!slug) continue;
        int r = w2k_list_add(d->list, id, (void *)(long)(id + 1));
        w2k_list_set(d->list, r, 0, slug);
    }
}

static void icd_paint(W2kWin *w, Drawable d)
{
    IconDlg *dd = w->user;
    w2k_text(d, F_UI, 10, 10, "Select an icon for this shortcut:", C_TEXT);
    w2k_list_draw(d, dd->list);
    w2k_draw_pushbutton(d, &dd->browse, "&Browse...",
                        dd->down == 3 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &dd->ok, "OK",
                        BS_DEFAULT | (dd->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &dd->cancel, "Cancel", dd->down == 2 ? BS_PRESSED : 0);
}

static void icd_accept(IconDlg *dd)
{
    int i = dd->list->sel;
    if (i < 0 || i >= dd->list->n) return;
    int id = (int)(long)dd->list->items[i].data - 1;
    const char *slug = w2k_icon_slug(id);
    if (!slug) return;
    snprintf(dd->chosen, sizeof dd->chosen, "w2k:%s", slug);
    w2k_win_close(dd->w, ID_OK);
}

static int icd_event(W2kWin *w, XEvent *e)
{
    IconDlg *dd = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_list_press(dd->list, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&dd->ok, x, y)) dd->down = 1;
        else if (w2k_rect_hit(&dd->cancel, x, y)) dd->down = 2;
        else if (w2k_rect_hit(&dd->browse, x, y)) dd->down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = dd->down, x = e->xbutton.x, y = e->xbutton.y;
        dd->down = 0;
        w2k_list_release(dd->list, &e->xbutton);
        if (b == 1 && w2k_rect_hit(&dd->ok, x, y)) { icd_accept(dd); return 1; }
        if (b == 2 && w2k_rect_hit(&dd->cancel, x, y)) {
            w2k_win_close(w, ID_CANCEL);
            return 1;
        }
        if (b == 3 && w2k_rect_hit(&dd->browse, x, y)) {
            char path[1024] = "";
            const char *home = getenv("HOME");
            if (home) snprintf(path, sizeof path, "%s", home);
            if (w2k_file_dialog_filter(w, 0, path, sizeof path,
                                       "Icons (*.ico;*.png)|*.ico;*.png|"
                                       "All Files (*.*)|*")) {
                snprintf(dd->chosen, sizeof dd->chosen, "%s", path);
                w2k_win_close(w, ID_OK);
                return 1;
            }
        }
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (w2k_list_motion(dd->list, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return) { icd_accept(dd); return 1; }
        if (w2k_list_key(dd->list, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void icd_resized(W2kWin *w)
{
    IconDlg *dd = w->user;
    dd->list->r = (W2kRect){ 10, 30, w->w - 20, w->h - 30 - 46 };
    w2k_list_layout(dd->list);
    int by = w->h - 12 - 23;
    dd->browse = (W2kRect){ 10, by, 85, 23 };
    dd->cancel = (W2kRect){ w->w - 12 - 75, by, 75, 23 };
    dd->ok     = (W2kRect){ w->w - 12 - 75 * 2 - 6, by, 75, 23 };
}

int wm_change_icon_dialog(char *out, int outsz)
{
    IconDlg dd;
    memset(&dd, 0, sizeof dd);

    W2kWin *w = w2k_win_new("Change Icon", "w2kdialog", 420, 330, 1);
    dd.w = w;
    w->user = &dd;
    w->paint = icd_paint;
    w->event = icd_event;
    w->resized = icd_resized;

    dd.list = w2k_list_new(LV_ICON);
    dd.list->focused = 1;
    dd.list->on_activate = NULL;
    w2k_scroll_bind(&dd.list->vsb, w);
    w2k_list_add_col(dd.list, NULL, 100, 0);
    icd_fill(&dd);
    icd_resized(w);
    w2k_win_center(w, NULL);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);

    int r = w2k_win_modal(w);
    w2k_list_free(dd.list);
    if (r == ID_OK && dd.chosen[0]) {
        snprintf(out, (size_t)outsz, "%s", dd.chosen);
        return 1;
    }
    return 0;
}
