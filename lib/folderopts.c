/* folderopts.c -- Tools > Folder Options.
 *
 * Three tabs, as in Windows 2000: General (how folders open and how items
 * are clicked), View (the advanced-settings list), and File Types (which
 * program opens what). The settings live in ~/.w2k/scheme with everything
 * else, and every window reads them from there, so a change here reaches
 * Explorer, the desktop and the file dialogs at once.
 *
 * The View tab's list is check boxes rather than the mix of check boxes
 * and radio buttons in a tree that the original uses: the option that
 * matters is whether hidden files are shown, and a two-state answer is
 * what that is. */
#include "w2k.h"
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OPT_FULLPATH, OPT_HIDDEN, OPT_HIDEEXT, OPT_TOOLTIPS, N_OPTS };

static const char *const opt_text[N_OPTS] = {
    "Display the full path in the title bar",
    "Show hidden files and folders",
    "Hide file extensions for known file types",
    "Show pop-up description for folder and desktop items",
};

typedef struct {
    W2kWin  *w;
    W2kTabs *tabs;
    W2kList *adv;            /* View tab */
    W2kList *types;          /* File Types tab */
    W2kRect  ok, cancel, apply, defaults, change;
    int      down;           /* which button is held */

    /* Working copies; nothing is committed until OK or Apply. */
    int newwindow, singleclick;
    int opt[N_OPTS];
} FO;

/* ---- General tab ---------------------------------------------------- */

static W2kRect g_browse_same, g_browse_new, g_click_single, g_click_double;

static void paint_general(Drawable d, FO *f, W2kRect c)
{
    int fh = w2k_font_height(F_UI);
    W2kRect g1 = { c.x + 10, c.y + 10, c.w - 20, 62 };
    w2k_draw_groupbox(d, &g1, "Browse folders");
    g_browse_same = (W2kRect){ g1.x + 14, g1.y + 20, g1.w - 28, fh + 2 };
    g_browse_new  = (W2kRect){ g1.x + 14, g1.y + 40, g1.w - 28, fh + 2 };
    w2k_draw_radio(d, g_browse_same.x, g_browse_same.y,
                   "Open each folder in the same window", !f->newwindow, 0, 0);
    w2k_draw_radio(d, g_browse_new.x, g_browse_new.y,
                   "Open each folder in its own window", f->newwindow, 0, 0);

    W2kRect g2 = { c.x + 10, g1.y + g1.h + 12, c.w - 20, 62 };
    w2k_draw_groupbox(d, &g2, "Click items as follows");
    g_click_single = (W2kRect){ g2.x + 14, g2.y + 20, g2.w - 28, fh + 2 };
    g_click_double = (W2kRect){ g2.x + 14, g2.y + 40, g2.w - 28, fh + 2 };
    w2k_draw_radio(d, g_click_single.x, g_click_single.y,
                   "Single-click to open an item (point to select)",
                   f->singleclick, 0, 0);
    w2k_draw_radio(d, g_click_double.x, g_click_double.y,
                   "Double-click to open an item (single-click to select)",
                   !f->singleclick, 0, 0);
}

/* ---- View tab ------------------------------------------------------- */

static void fill_adv(FO *f)
{
    w2k_list_clear(f->adv);
    for (int i = 0; i < N_OPTS; i++) {
        int r = w2k_list_add(f->adv, ICO_NONE, NULL);
        w2k_list_set(f->adv, r, 0, opt_text[i]);
        f->adv->items[r].checked = f->opt[i];
    }
}

static void on_check(void *user, int idx)
{
    FO *f = user;
    if (idx >= 0 && idx < N_OPTS) f->opt[idx] = f->adv->items[idx].checked;
}

/* ---- File Types tab ------------------------------------------------- */

/* A representative icon for each association class, so the list reads at
 * a glance the way Explorer's File Types tab does. */
static int class_icon(const char *cls)
{
    if (!strcmp(cls, "image")) return ICO_FILE_BITMAP;
    if (!strcmp(cls, "video")) return ICO_FILE_MOVIE;
    if (!strcmp(cls, "audio")) return ICO_FILE_MEDIA;
    if (!strcmp(cls, "text"))  return ICO_FILE_TEXT;
    if (!strcmp(cls, "web"))   return ICO_FILE_HTML;
    return ICO_FILE_UNKNOWN;
}

static void fill_types(FO *f)
{
    w2k_list_clear(f->types);
    for (int i = 0; i < w2k_assoc_count(); i++) {
        char cmd[256];
        w2k_assoc_get(w2k_assoc_class_at(i), cmd, sizeof cmd);
        int r = w2k_list_add(f->types, class_icon(w2k_assoc_class_at(i)), NULL);
        w2k_list_set(f->types, r, 0, w2k_assoc_label_at(i));
        w2k_list_set(f->types, r, 1, cmd[0] ? cmd : "(none)");
    }
}

static void do_change(FO *f)
{
    int i = f->types->sel;
    if (i < 0 || i >= w2k_assoc_count()) return;
    const char *cls = w2k_assoc_class_at(i);
    char cmd[256], out[256], title[128];
    w2k_assoc_get(cls, cmd, sizeof cmd);
    snprintf(title, sizeof title, "Edit File Type");
    if (!w2k_prompt(f->w, title, "Opens with:", cmd, out, sizeof out,
                    ICO_QUESTION))
        return;
    w2k_assoc_set(cls, out);
    fill_types(f);
    w2k_win_dirty(f->w);
}

/* ---- The sheet ------------------------------------------------------ */

static void commit(FO *f)
{
    w2k_folder_fullpath    = f->opt[OPT_FULLPATH];
    w2k_folder_hidden      = f->opt[OPT_HIDDEN];
    w2k_folder_hide_ext    = f->opt[OPT_HIDEEXT];
    w2k_folder_tooltips    = f->opt[OPT_TOOLTIPS];
    w2k_folder_newwindow   = f->newwindow;
    w2k_folder_singleclick = f->singleclick;
    w2k_scheme_save(NULL);
    w2k_scheme_broadcast();
}

static void restore_defaults(FO *f)
{
    f->opt[OPT_FULLPATH] = 0;
    f->opt[OPT_HIDDEN]   = 0;
    f->opt[OPT_HIDEEXT]  = 1;
    f->opt[OPT_TOOLTIPS] = 1;
    f->newwindow = 0;
    f->singleclick = 0;
    fill_adv(f);
}

static void paint(W2kWin *w, Drawable d)
{
    FO *f = w->user;
    w2k_fill(d, 0, 0, w->w, w->h, C_FACE);
    w2k_tabs_draw(d, f->tabs);
    W2kRect c = w2k_tabs_client(f->tabs);

    if (f->tabs->sel == 0) {
        paint_general(d, f, c);
    } else if (f->tabs->sel == 1) {
        w2k_text(d, F_UI, c.x + 10, c.y + 8, "Advanced settings:", C_TEXT);
        w2k_list_draw(d, f->adv);
    } else {
        w2k_text(d, F_UI, c.x + 10, c.y + 8, "Registered file types:", C_TEXT);
        w2k_list_draw(d, f->types);
        w2k_draw_pushbutton(d, &f->change, "&Change...",
                            (f->down == 5 ? BS_PRESSED : 0) |
                            (f->types->sel < 0 ? BS_DISABLED : 0));
    }
    if (f->tabs->sel != 2)
        w2k_draw_pushbutton(d, &f->defaults, "&Restore Defaults",
                            f->down == 4 ? BS_PRESSED : 0);

    w2k_draw_pushbutton(d, &f->ok, "OK",
                        BS_DEFAULT | (f->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &f->cancel, "Cancel", f->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &f->apply, "&Apply", f->down == 3 ? BS_PRESSED : 0);
}

static int event(W2kWin *w, XEvent *e)
{
    FO *f = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_tabs_press(f->tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (f->tabs->sel == 0) {
            if (w2k_rect_hit(&g_browse_same, x, y)) f->newwindow = 0;
            else if (w2k_rect_hit(&g_browse_new, x, y)) f->newwindow = 1;
            else if (w2k_rect_hit(&g_click_single, x, y)) f->singleclick = 1;
            else if (w2k_rect_hit(&g_click_double, x, y)) f->singleclick = 0;
        } else if (f->tabs->sel == 1) {
            if (w2k_list_press(f->adv, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        } else {
            if (w2k_list_press(f->types, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_rect_hit(&f->change, x, y)) f->down = 5;
        }
        if (w2k_rect_hit(&f->ok, x, y)) f->down = 1;
        else if (w2k_rect_hit(&f->cancel, x, y)) f->down = 2;
        else if (w2k_rect_hit(&f->apply, x, y)) f->down = 3;
        else if (f->tabs->sel != 2 && w2k_rect_hit(&f->defaults, x, y)) f->down = 4;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = f->down, x = e->xbutton.x, y = e->xbutton.y;
        f->down = 0;
        w2k_list_release(f->adv, &e->xbutton);
        w2k_list_release(f->types, &e->xbutton);
        if (b == 1 && w2k_rect_hit(&f->ok, x, y)) {
            commit(f);
            w2k_win_close(w, ID_OK);
            return 1;
        }
        if (b == 2 && w2k_rect_hit(&f->cancel, x, y)) {
            w2k_win_close(w, ID_CANCEL);
            return 1;
        }
        if (b == 3 && w2k_rect_hit(&f->apply, x, y)) commit(f);
        if (b == 4 && w2k_rect_hit(&f->defaults, x, y)) restore_defaults(f);
        if (b == 5 && w2k_rect_hit(&f->change, x, y)) do_change(f);
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (f->tabs->sel == 1 && w2k_list_motion(f->adv, &e->xmotion)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (f->tabs->sel == 2 && w2k_list_motion(f->types, &e->xmotion)) {
            w2k_win_dirty(w);
            return 1;
        }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return) { commit(f); w2k_win_close(w, ID_OK); return 1; }
        if (w2k_tabs_key(f->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (f->tabs->sel == 1 && w2k_list_key(f->adv, &e->xkey)) {
            for (int i = 0; i < N_OPTS; i++) f->opt[i] = f->adv->items[i].checked;
            w2k_win_dirty(w);
            return 1;
        }
        if (f->tabs->sel == 2 && w2k_list_key(f->types, &e->xkey)) {
            w2k_win_dirty(w);
            return 1;
        }
        return 1;
    }
    }
    return 0;
}

int w2k_folder_options(W2kWin *over)
{
    FO f;
    memset(&f, 0, sizeof f);
    f.opt[OPT_FULLPATH] = w2k_folder_fullpath;
    f.opt[OPT_HIDDEN]   = w2k_folder_hidden;
    f.opt[OPT_HIDEEXT]  = w2k_folder_hide_ext;
    f.opt[OPT_TOOLTIPS] = w2k_folder_tooltips;
    f.newwindow   = w2k_folder_newwindow;
    f.singleclick = w2k_folder_singleclick;

    int W = 380, H = 330;
    W2kWin *w = w2k_win_new("Folder Options", "w2kdialog", W, H, 0);
    f.w = w;
    w->user = &f;
    w->paint = paint;
    w->event = event;

    f.tabs = w2k_tabs_new(&f, NULL);
    w2k_tabs_add(f.tabs, "General");
    w2k_tabs_add(f.tabs, "View");
    w2k_tabs_add(f.tabs, "File Types");
    f.tabs->r = (W2kRect){ 8, 8, W - 16, H - 8 - 40 };
    W2kRect c = w2k_tabs_client(f.tabs);

    f.adv = w2k_list_new(LV_REPORT);
    f.adv->checkboxes = 1;
    f.adv->fullrow = 1;
    f.adv->on_check = on_check;
    f.adv->user = &f;
    w2k_list_add_col(f.adv, NULL, c.w - 20 - SCROLL_W, 0);
    f.adv->hdr_h = 0;              /* no column header on this one */
    f.adv->r = (W2kRect){ c.x + 10, c.y + 26, c.w - 20, c.h - 26 - 42 };
    fill_adv(&f);
    w2k_list_layout(f.adv);

    f.types = w2k_list_new(LV_REPORT);
    f.types->fullrow = 1;
    w2k_list_add_col(f.types, "File Type", 130, 0);
    w2k_list_add_col(f.types, "Opens with", c.w - 150 - SCROLL_W, 0);
    f.types->r = (W2kRect){ c.x + 10, c.y + 26, c.w - 20, c.h - 26 - 42 };
    fill_types(&f);
    w2k_list_layout(f.types);

    f.defaults = (W2kRect){ c.x + c.w - 10 - 110, c.y + c.h - 32, 110, 23 };
    f.change   = (W2kRect){ c.x + c.w - 10 - 80, c.y + c.h - 32, 80, 23 };
    f.ok     = (W2kRect){ W - 12 - 75 * 3 - 12, H - 12 - 23, 75, 23 };
    f.cancel = (W2kRect){ W - 12 - 75 * 2 - 6, H - 12 - 23, 75, 23 };
    f.apply  = (W2kRect){ W - 12 - 75, H - 12 - 23, 75, 23 };

    w2k_win_center(w, over);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);
    int r = w2k_win_modal(w);
    w2k_list_free(f.adv);
    w2k_list_free(f.types);
    w2k_tabs_free(f.tabs);
    return r == ID_OK;
}
