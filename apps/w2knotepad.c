/* w2knotepad -- Notepad, as it shipped with Windows 2000.
 *
 * File / Edit / Format / Help, one edit control filling the client area,
 * word wrap off by default, single-level undo. */
#include "w2kui.h"

/* What Notepad's Open and Save As offer, as Windows words it. */
#define TEXT_FILTERS "Text Documents (*.txt)|*.txt|All Files (*.*)|*"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    ID_NEW = 1, ID_OPEN, ID_SAVE, ID_SAVEAS, ID_PAGESETUP, ID_PRINT, ID_EXIT,
    ID_UNDO, ID_CUT, ID_COPY, ID_PASTE, ID_CLEAR, ID_FIND, ID_FINDNEXT,
    ID_GOTO, ID_SELECTALL, ID_TIMEDATE,
    ID_WORDWRAP, ID_FONT,
    ID_HELPTOPICS, ID_ABOUT
};

typedef struct {
    W2kWin     *win;
    W2kMenubar *mb;
    W2kEdit    *ed;
    char        path[1024];
    int         untitled;
    int         dirty;

    /* Single-level undo, exactly what Notepad offers. */
    char       *undo_text;
    int         undo_caret, undo_valid, typing_run;

    char        find_what[256];
    int         find_case, find_up;
} Pad;

static Pad pad;

/* ------------------------------------------------------------------ *
 * Title / document state
 * ------------------------------------------------------------------ */
static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void update_title(void)
{
    /* Windows never marks the caption dirty; it asks when you close. */
    char t[1400];
    snprintf(t, sizeof t, "%s - Notepad",
             pad.untitled ? "Untitled" : base_name(pad.path));
    w2k_win_title(pad.win, t);
}

static void checkpoint(void)
{
    free(pad.undo_text);
    pad.undo_text = w2k_strdup(w2k_edit_text(pad.ed));
    pad.undo_caret = pad.ed->caret;
    pad.undo_valid = 1;
}

static void on_change(void *user)
{
    (void)user;
    pad.dirty = 1;
}

int do_save(int saveas);

static int confirm_discard(void)
{
    if (!pad.dirty) return 1;
    char msg[1400];
    snprintf(msg, sizeof msg, "The text in the %s file has changed.\n\n"
             "Do you want to save the changes?",
             pad.untitled ? "Untitled" : base_name(pad.path));
    int r = w2k_msgbox(pad.win, "Notepad", msg, MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == ID_CANCEL) return 0;
    if (r == ID_NO) return 1;
    return do_save(0);
}

/* ------------------------------------------------------------------ *
 * File I/O
 * ------------------------------------------------------------------ */
static int load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[1200];
        snprintf(msg, sizeof msg, "Cannot find the file %s.\n\n%s",
                 path, strerror(errno));
        w2k_msgbox(pad.win, "Notepad", msg, MB_OK | MB_ICONWARNING);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) n = 0;
    char *buf = w2k_alloc(n + 1);
    size_t got = fread(buf, 1, n, f);
    buf[got] = 0;
    fclose(f);

    /* Strip CRs so DOS files do not sprout stray glyphs. */
    char *w = buf;
    for (char *r = buf; *r; r++) if (*r != '\r') *w++ = *r;
    *w = 0;

    w2k_edit_set(pad.ed, buf);
    free(buf);
    snprintf(pad.path, sizeof pad.path, "%s", path);
    pad.untitled = 0;
    pad.dirty = 0;
    pad.undo_valid = 0;
    update_title();
    return 1;
}

int do_save(int saveas)
{
    if (saveas || pad.untitled) {
        char p[1024];
        snprintf(p, sizeof p, "%s", pad.untitled ? "" : pad.path);
        if (!w2k_file_dialog_filter(pad.win, 1, p, sizeof p, TEXT_FILTERS))
            return 0;
        snprintf(pad.path, sizeof pad.path, "%s", p);
        pad.untitled = 0;
    }
    FILE *f = fopen(pad.path, "wb");
    if (!f) {
        char msg[1200];
        snprintf(msg, sizeof msg, "Cannot create the file %s.\n\n%s",
                 pad.path, strerror(errno));
        w2k_msgbox(pad.win, "Notepad", msg, MB_OK | MB_ICONERROR);
        return 0;
    }
    const char *t = w2k_edit_text(pad.ed);
    fwrite(t, 1, strlen(t), f);
    fclose(f);
    pad.dirty = 0;
    update_title();
    return 1;
}

/* ------------------------------------------------------------------ *
 * Find
 * ------------------------------------------------------------------ */
static int ci_chr(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int find_from(const char *hay, const char *needle, int start, int back,
                     int matchcase)
{
    int hn = strlen(hay), nn = strlen(needle);
    if (!nn) return -1;
    if (back) {
        for (int i = start; i >= 0; i--) {
            if (i + nn > hn) continue;
            int k = 0;
            while (k < nn) {
                int a = hay[i + k], b = needle[k];
                if (!matchcase) { a = ci_chr(a); b = ci_chr(b); }
                if (a != b) break;
                k++;
            }
            if (k == nn) return i;
        }
        return -1;
    }
    for (int i = start; i + nn <= hn; i++) {
        int k = 0;
        while (k < nn) {
            int a = hay[i + k], b = needle[k];
            if (!matchcase) { a = ci_chr(a); b = ci_chr(b); }
            if (a != b) break;
            k++;
        }
        if (k == nn) return i;
    }
    return -1;
}

static void do_find_next(int announce)
{
    if (!pad.find_what[0]) return;
    const char *t = w2k_edit_text(pad.ed);
    int from = pad.find_up ? pad.ed->caret - (int)strlen(pad.find_what) - 1
                           : (pad.ed->caret > pad.ed->sel ? pad.ed->caret
                                                          : pad.ed->sel);
    if (pad.find_up && from < 0) from = 0;
    int at = find_from(t, pad.find_what, from, pad.find_up, pad.find_case);
    if (at < 0) {
        if (announce) {
            char msg[300];
            snprintf(msg, sizeof msg, "Cannot find \"%s\"", pad.find_what);
            w2k_msgbox(pad.win, "Notepad", msg, MB_OK | MB_ICONINFO);
        }
        return;
    }
    pad.ed->sel = at;
    pad.ed->caret = at + strlen(pad.find_what);
    w2k_edit_scroll_to_caret(pad.ed);
    w2k_win_dirty(pad.win);
}

/* The Find dialog: text box, Match case, direction group, Find Next. */
typedef struct {
    W2kEdit *what;
    W2kRect  next, cancel, mcase, up, down;
    int      down_btn;
} FindDlg;

static void find_paint(W2kWin *w, Drawable d)
{
    FindDlg *f = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_text_mnemonic(d, F_UI, 10, f->what->r.y + (21 - fh) / 2,
                      "Fi&nd what:", C_TEXT, 1);
    w2k_edit_draw(d, f->what);
    w2k_draw_checkbox(d, 12, 52, "Match &case", pad.find_case, 0, 0);

    W2kRect grp = { 118, 42, 132, 40 };
    w2k_draw_groupbox(d, &grp, "Direction");
    w2k_draw_radio(d, 128, 58, "&Up", pad.find_up, 0, 0);
    w2k_draw_radio(d, 186, 58, "&Down", !pad.find_up, 0, 0);

    w2k_draw_pushbutton(d, &f->next, "&Find Next",
                        BS_DEFAULT | (f->down_btn == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &f->cancel, "Cancel",
                        f->down_btn == 2 ? BS_PRESSED : 0);
}

static int find_event(W2kWin *w, XEvent *e)
{
    FindDlg *f = w->user;
    switch (e->type) {
    case ButtonPress: {
        if (w2k_edit_press(f->what, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_rect_hit(&f->mcase, x, y)) pad.find_case = !pad.find_case;
        else if (w2k_rect_hit(&f->up, x, y))   pad.find_up = 1;
        else if (w2k_rect_hit(&f->down, x, y)) pad.find_up = 0;
        else if (w2k_rect_hit(&f->next, x, y))   f->down_btn = 1;
        else if (w2k_rect_hit(&f->cancel, x, y)) f->down_btn = 2;
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (w2k_edit_motion(f->what, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        w2k_edit_release(f->what);
        int d = f->down_btn;
        f->down_btn = 0;
        if (d == 1 && w2k_rect_hit(&f->next, e->xbutton.x, e->xbutton.y)) {
            snprintf(pad.find_what, sizeof pad.find_what, "%s",
                     w2k_edit_text(f->what));
            do_find_next(1);
        } else if (d == 2) w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            snprintf(pad.find_what, sizeof pad.find_what, "%s",
                     w2k_edit_text(f->what));
            do_find_next(1);
            return 1;
        }
        if (w2k_edit_key(f->what, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void blink_cb(void *v) { w2k_edit_blink(v); }

static void find_dialog(void)
{
    FindDlg f = { 0 };
    int cw = 360, chh = 100;
    W2kWin *w = w2k_win_new("Find", "w2knotepad", cw, chh, 0);
    f.what = w2k_edit_new(0);
    w2k_edit_bind(f.what, w);
    f.what->focused = 1;
    f.what->r = (W2kRect){ 74, 12, 190, 21 };
    if (pad.find_what[0]) {
        w2k_edit_set(f.what, pad.find_what);
        w2k_edit_select_all(f.what);
    }
    f.next   = (W2kRect){ cw - 12 - 78, 12, 78, 23 };
    f.cancel = (W2kRect){ cw - 12 - 78, 40, 78, 23 };
    f.mcase  = (W2kRect){ 12, 52, 100, 16 };
    f.up     = (W2kRect){ 128, 58, 50, 14 };
    f.down   = (W2kRect){ 186, 58, 58, 14 };

    w->user = &f;
    w->paint = find_paint;
    w->event = find_event;
    w2k_win_center(w, pad.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    XSetTransientForHint(w2k.dpy, w->win, pad.win->win);

    w2k_add_timer(w2k_caret_blink, blink_cb, f.what);
    w2k_win_modal(w);
    w2k_del_timer(blink_cb, f.what);
    w2k_edit_free(f.what);
    w2k_win_dirty(pad.win);
}

/* ------------------------------------------------------------------ *
 * Menus
 * ------------------------------------------------------------------ */
static W2kMenu *build_file(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_NEW,    "&New",        "Ctrl+N", ICO_NONE);
    w2k_menu_item(m, ID_OPEN,   "&Open...",    "Ctrl+O", ICO_NONE);
    w2k_menu_item(m, ID_SAVE,   "&Save",       "Ctrl+S", ICO_NONE);
    w2k_menu_item(m, ID_SAVEAS, "Save &As...", NULL,     ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_PAGESETUP, "Page Se&tup...", NULL, ICO_NONE);
    w2k_menu_disable(m);
    w2k_menu_item(m, ID_PRINT, "&Print...", "Ctrl+P", ICO_NONE);
    w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_EXIT, "E&xit", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_edit(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_UNDO, "&Undo", "Ctrl+Z", ICO_NONE);
    if (!pad.undo_valid) w2k_menu_disable(m);
    w2k_menu_sep(m);
    int hassel = w2k_edit_has_sel(pad.ed);
    w2k_menu_item(m, ID_CUT, "Cu&t", "Ctrl+X", ICO_NONE);
    if (!hassel) w2k_menu_disable(m);
    w2k_menu_item(m, ID_COPY, "&Copy", "Ctrl+C", ICO_NONE);
    if (!hassel) w2k_menu_disable(m);
    w2k_menu_item(m, ID_PASTE, "&Paste", "Ctrl+V", ICO_NONE);
    w2k_menu_item(m, ID_CLEAR, "De&lete", "Del", ICO_NONE);
    if (!hassel) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_FIND, "&Find...", "Ctrl+F", ICO_NONE);
    w2k_menu_item(m, ID_FINDNEXT, "Find &Next", "F3", ICO_NONE);
    if (!pad.find_what[0]) w2k_menu_disable(m);
    w2k_menu_item(m, ID_GOTO, "&Go To...", "Ctrl+G", ICO_NONE);
    if (pad.ed->wrap) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_SELECTALL, "Select &All", "Ctrl+A", ICO_NONE);
    w2k_menu_item(m, ID_TIMEDATE, "Time/&Date", "F5", ICO_NONE);
    return m;
}

static W2kMenu *build_format(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_WORDWRAP, "&Word Wrap", NULL, ICO_NONE);
    w2k_menu_check(m, pad.ed->wrap);
    w2k_menu_item(m, ID_FONT, "&Font...", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_help(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_HELPTOPICS, "&Help Topics", NULL, ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_ABOUT, "&About Notepad", NULL, ICO_NONE);
    return m;
}

/* A minimal Font dialog: this build has two faces to choose between. */
static void font_dialog(void)
{
    static const char *names[] = { "Fixedsys", "MS Sans Serif" };
    int cur = (pad.ed->font == F_FIXED) ? 0 : 1;
    char msg[300];
    snprintf(msg, sizeof msg,
             "Font: %s\n\nSwitch to %s?", names[cur], names[!cur]);
    if (w2k_msgbox(pad.win, "Font", msg, MB_OKCANCEL | MB_ICONQUESTION) == ID_OK) {
        pad.ed->font = cur ? F_FIXED : F_UI;
        pad.ed->layout_w = -1;
        w2k_win_dirty(pad.win);
    }
}

static void goto_dialog(void)
{
    char buf[32];
    int row, col;
    w2k_edit_caret_rowcol(pad.ed, &row, &col);
    snprintf(buf, sizeof buf, "%d", row);
    char out[32];
    if (!w2k_prompt(pad.win, "Go To Line", "&Line number:", buf, out,
                    sizeof out, ICO_NONE))
        return;
    int want = atoi(out);
    if (want < 1) want = 1;
    const char *t = w2k_edit_text(pad.ed);
    int line = 1, off = 0;
    while (t[off] && line < want) if (t[off++] == '\n') line++;
    if (line < want) {
        w2k_msgbox(pad.win, "Notepad",
                   "The line number is beyond the total number of lines.",
                   MB_OK | MB_ICONWARNING);
        return;
    }
    pad.ed->caret = pad.ed->sel = off;
    w2k_edit_layout(pad.ed);
    w2k_win_dirty(pad.win);
}

static void insert_timedate(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    int h12 = tm.tm_hour % 12;
    if (!h12) h12 = 12;
    snprintf(buf, sizeof buf, "%d:%02d %s %d/%d/%d", h12, tm.tm_min,
             tm.tm_hour < 12 ? "AM" : "PM", tm.tm_mon + 1, tm.tm_mday,
             tm.tm_year + 1900);
    checkpoint();
    w2k_edit_insert(pad.ed, buf);
}

static void command(void *user, int id)
{
    (void)user;
    pad.typing_run = 0;
    switch (id) {
    case ID_NEW:
        if (!confirm_discard()) break;
        w2k_edit_set(pad.ed, "");
        pad.untitled = 1;
        pad.dirty = 0;
        pad.undo_valid = 0;
        pad.path[0] = 0;
        update_title();
        break;
    case ID_OPEN: {
        if (!confirm_discard()) break;
        char p[1024];
        snprintf(p, sizeof p, "%s", pad.untitled ? "" : pad.path);
        if (w2k_file_dialog_filter(pad.win, 0, p, sizeof p, TEXT_FILTERS))
            load_file(p);
        break;
    }
    case ID_SAVE:   do_save(0); break;
    case ID_SAVEAS: do_save(1); break;
    case ID_EXIT:
        if (confirm_discard()) w2k_win_close(pad.win, 0);
        break;

    case ID_UNDO:
        if (pad.undo_valid) {
            char *cur = w2k_strdup(w2k_edit_text(pad.ed));
            int curcaret = pad.ed->caret;
            w2k_edit_set(pad.ed, pad.undo_text);
            pad.ed->caret = pad.ed->sel =
                pad.undo_caret <= pad.ed->len ? pad.undo_caret : pad.ed->len;
            free(pad.undo_text);
            pad.undo_text = cur;             /* undo is its own redo */
            pad.undo_caret = curcaret;
        }
        break;
    case ID_CUT:   checkpoint(); w2k_edit_cut(pad.ed); break;
    case ID_COPY:  w2k_edit_copy(pad.ed); break;
    case ID_PASTE: checkpoint(); w2k_edit_paste(pad.ed); break;
    case ID_CLEAR: checkpoint(); w2k_edit_delete_sel(pad.ed); break;
    case ID_SELECTALL: w2k_edit_select_all(pad.ed); break;
    case ID_TIMEDATE:  insert_timedate(); break;
    case ID_FIND:      find_dialog(); break;
    case ID_FINDNEXT:  do_find_next(1); break;
    case ID_GOTO:      goto_dialog(); break;

    case ID_WORDWRAP:
        pad.ed->wrap = !pad.ed->wrap;
        pad.ed->hsb.pos = 0;
        pad.ed->layout_w = -1;
        w2k_edit_layout(pad.ed);
        break;
    case ID_FONT: font_dialog(); break;

    case ID_HELPTOPICS:
    case ID_ABOUT:
        w2k_msgbox(pad.win, "About Notepad",
                   "Notepad\n"
                   "Windows 2000 for X11\n"
                   "\n"
                   "A faithful rebuild of the classic text editor.",
                   MB_OK | MB_ICONINFO);
        break;
    }
    w2k_win_dirty(pad.win);
}

/* ------------------------------------------------------------------ *
 * Window plumbing
 * ------------------------------------------------------------------ */
static void layout(W2kWin *w)
{
    pad.mb->r = (W2kRect){ 0, 0, w->w, MENUBAR_H };
    pad.ed->r = (W2kRect){ 0, MENUBAR_H, w->w, w->h - MENUBAR_H };
    pad.ed->layout_w = -1;
}

static void paint(W2kWin *w, Drawable d)
{
    w2k_menubar_draw(d, pad.mb);
    w2k_edit_draw(d, pad.ed);
}

static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress:
        if (w2k_menubar_press(pad.mb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_edit_press(pad.ed, &e->xbutton)) {
            pad.typing_run = 0;
            w2k_win_dirty(w);
            return 1;
        }
        return 1;
    case MotionNotify:
        if (w2k_edit_motion(pad.ed, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease:
        w2k_edit_release(pad.ed);
        return 1;
    case FocusIn:  pad.ed->focused = 1; w2k_win_dirty(w); return 1;
    case FocusOut: pad.ed->focused = 0; pad.ed->caret_on = 0;
                   w2k_win_dirty(w); return 1;
    case KeyPress: {
        if (w2k_menubar_key(pad.mb, &e->xkey)) { w2k_win_dirty(w); return 1; }
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (e->xkey.state & ControlMask) {
            switch (ks) {
            case XK_n: case XK_N: command(NULL, ID_NEW); return 1;
            case XK_o: case XK_O: command(NULL, ID_OPEN); return 1;
            case XK_s: case XK_S: command(NULL, ID_SAVE); return 1;
            case XK_f: case XK_F: command(NULL, ID_FIND); return 1;
            case XK_g: case XK_G: command(NULL, ID_GOTO); return 1;
            case XK_z: case XK_Z: command(NULL, ID_UNDO); return 1;
            }
        }
        if (ks == XK_F3) { command(NULL, ID_FINDNEXT); return 1; }
        if (ks == XK_F5) { command(NULL, ID_TIMEDATE); return 1; }

        /* One undo checkpoint per run of typing, as Notepad does. */
        int mods = (ks == XK_BackSpace || ks == XK_Delete ||
                    ks == XK_Return || ks == XK_KP_Enter || ks == XK_Tab);
        char tmp[8];
        KeySym dummy;
        int n = XLookupString(&e->xkey, tmp, sizeof tmp, &dummy, NULL);
        if (n > 0 && (unsigned char)tmp[0] >= 32 &&
            !(e->xkey.state & ControlMask)) mods = 1;
        if (mods && !pad.typing_run) { checkpoint(); pad.typing_run = 1; }
        if (!mods) pad.typing_run = 0;

        if (w2k_edit_key(pad.ed, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static int closing(W2kWin *w)
{
    (void)w;
    return confirm_discard();
}

int main(int argc, char **argv)
{
    if (w2k_init("w2knotepad") < 0) return 1;

    pad.untitled = 1;
    pad.win = w2k_win_new("Untitled - Notepad", "w2knotepad", 560, 420, 1);
    pad.win->paint = paint;
    pad.win->event = event;
    pad.win->resized = layout;
    pad.win->closing = closing;

    pad.ed = w2k_edit_new(1);
    w2k_edit_bind(pad.ed, pad.win);
    pad.ed->on_change = on_change;
    pad.ed->focused = 1;

    pad.mb = w2k_menubar_new(NULL, command);
    pad.mb->win_ref = pad.win->win;
    w2k_menubar_add(pad.mb, "&File", build_file);
    w2k_menubar_add(pad.mb, "&Edit", build_edit);
    w2k_menubar_add(pad.mb, "F&ormat", build_format);
    w2k_menubar_add(pad.mb, "&Help", build_help);

    layout(pad.win);
    if (argc > 1) load_file(argv[1]);

    w2k_add_timer(w2k_caret_blink, blink_cb, pad.ed);
    w2k_win_show(pad.win);
    w2k_run();

    w2k_menubar_free(pad.mb);
    w2k_edit_free(pad.ed);
    free(pad.undo_text);
    w2k_fini();
    return 0;
}
