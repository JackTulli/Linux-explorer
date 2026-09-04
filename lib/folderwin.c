/* folderwin.c -- the Windows 2000 shell's folder window.
 *
 * Every metric in here was measured off a screenshot of the real Control
 * Panel: three 24-pixel bands (an etched line pair over a 22-pixel bar)
 * for the menu, the standard toolbar and the Address bar; the list view
 * flush against the client edge below them; a 200-pixel web-view pane on
 * the left with the coloured-frames banner, the folder's icon over it,
 * a bold Verdana heading, a two-pixel blue rule and the text; the status
 * bar in three panes with My Computer's icon in the last. Control Panel
 * and Network and Dial-up Connections draw themselves through this. */
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define BAND       24
#define BAR_H      22
#define BANNER_H   48
#define PANE_TEXT_X 12

/* The shell's own toolbar, in its order. */
static const struct { int id, icon; const char *text; int drop, off; } std_buttons[] = {
    { FW_BACK,    ICO_TB_BACK,    "Back",    1, 1 },
    { FW_FORWARD, ICO_TB_FORWARD, NULL,      1, 1 },
    { FW_UP,      ICO_TB_UP,      NULL,      0, 0 },
    { TBB_SEP,    ICO_NONE,       NULL,      0, 0 },
    { FW_SEARCH,  ICO_TB_SEARCH,  "Search",  0, 0 },
    { FW_FOLDERS, ICO_TB_FOLDERS, "Folders", 0, 0 },
    { FW_HISTORY, ICO_TB_HISTORY, NULL,      0, 0 },
    { TBB_SEP,    ICO_NONE,       NULL,      0, 0 },
    { FW_MOVETO,  ICO_TB_MOVETO,  NULL,      0, 1 },
    { FW_COPYTO,  ICO_TB_COPYTO,  NULL,      0, 1 },
    { FW_DELETE,  ICO_TB_DELETE,  NULL,      0, 0 },
    { FW_UNDO,    ICO_TB_UNDO,    NULL,      0, 0 },
    { TBB_SEP,    ICO_NONE,       NULL,      0, 0 },
    { FW_VIEWS,   ICO_TB_VIEWS,   NULL,      7, 0 },    /* a narrower bay */
};

/* Run a program without waiting for it, twice forked so it is never a
 * zombie of ours. */
static void spawn(const char *cmd)
{
    pid_t p = fork();
    if (p < 0) return;
    if (p == 0) {
        if (fork() == 0) {
            signal(SIGPIPE, SIG_DFL);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        }
        _exit(0);
    }
    waitpid(p, NULL, 0);
}

/* ------------------------------------------------------------------ *
 * Menus
 * ------------------------------------------------------------------ */
static W2kMenu *build_file(void *u)
{
    W2kFolderWin *f = u;
    W2kMenu *m = f->build_file ? f->build_file(f->user) : NULL;
    if (!m) m = w2k_menu_new();
    else    w2k_menu_sep(m);
    w2k_menu_item(m, FW_CLOSE, "&Close", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_edit(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, FW_SELECTALL, "Select &All", "Ctrl+A", ICO_NONE);
    return m;
}

static W2kMenu *build_view(void *u)
{
    W2kFolderWin *f = u;
    W2kMenu *m = w2k_menu_new();
    W2kMenu *bars = w2k_menu_new();
    w2k_menu_item(bars, FW_TB_STANDARD, "&Standard Buttons", NULL, ICO_NONE);
    w2k_menu_check(bars, f->show_toolbar);
    w2k_menu_item(bars, FW_TB_ADDRESS, "&Address Bar", NULL, ICO_NONE);
    w2k_menu_check(bars, f->show_address);
    w2k_menu_sub(m, "&Toolbars", ICO_NONE, bars);
    w2k_menu_item(m, FW_STATUSBAR, "Status &Bar", NULL, ICO_NONE);
    w2k_menu_check(m, f->show_status);
    w2k_menu_sep(m);
    w2k_menu_item(m, FW_V_LARGE, "Lar&ge Icons", NULL, ICO_NONE);
    w2k_menu_radio(m, f->list->mode == LV_ICON);
    w2k_menu_item(m, FW_V_LIST, "&List", NULL, ICO_NONE);
    w2k_menu_radio(m, f->list->mode == LV_LIST);
    w2k_menu_sep(m);
    w2k_menu_item(m, FW_REFRESH, "&Refresh", "F5", ICO_NONE);
    return m;
}

static W2kMenu *build_favorites(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, 0, "&Add to Favorites...", NULL, ICO_NONE);
    w2k_menu_item(m, 0, "&Organize Favorites...", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_tools(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, FW_FOLDEROPTS, "Folder &Options...", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_extra(void *u)
{
    W2kFolderWin *f = u;
    return f->build_extra ? f->build_extra(f->user) : w2k_menu_new();
}

static W2kMenu *build_help(void *u)
{
    W2kFolderWin *f = u;
    W2kMenu *m = f->build_help ? f->build_help(f->user) : NULL;
    if (!m) {
        m = w2k_menu_new();
        w2k_menu_item(m, FW_HELPTOPICS, "&Help Topics", NULL, ICO_NONE);
        w2k_menu_sep(m);
    }
    w2k_menu_item(m, FW_ABOUT, "&About Windows", NULL, ICO_NONE);
    return m;
}

/* ------------------------------------------------------------------ *
 * Commands the chrome handles itself
 * ------------------------------------------------------------------ */
static void command(void *u, int id)
{
    W2kFolderWin *f = u;
    switch (id) {
    case 0: return;
    case FW_CLOSE:       w2k_win_close(f->win, 0); return;
    case FW_TB_STANDARD: f->show_toolbar = !f->show_toolbar; break;
    case FW_TB_ADDRESS:  f->show_address = !f->show_address; break;
    case FW_STATUSBAR:   f->show_status  = !f->show_status;  break;
    case FW_V_LARGE:     f->list->mode = LV_ICON; break;
    case FW_V_LIST:      f->list->mode = LV_LIST; break;
    case FW_VIEWS:
        f->list->mode = f->list->mode == LV_ICON ? LV_LIST : LV_ICON;
        break;
    case FW_SELECTALL:
        for (int i = 0; i < f->list->n; i++) f->list->items[i].selected = 1;
        if (f->list->n) f->list->sel = 0;
        break;
    case FW_UP:
    case FW_FOLDERS:     spawn("l2kexplorer"); return;
    case FW_SEARCH: {
        /* The shell's Search dialog, asked for through _W2K_COMMAND. */
        XEvent ev = { 0 };
        ev.xclient.type = ClientMessage;
        ev.xclient.window = w2k.root;
        ev.xclient.message_type = w2k.a_w2k_command;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 5;
        XSendEvent(w2k.dpy, w2k.root, False, SubstructureNotifyMask, &ev);
        XFlush(w2k.dpy);
        return;
    }
    case FW_FOLDEROPTS:
        w2k_folder_options(f->win);
        f->list->singleclick = w2k_folder_singleclick;
        break;
    case FW_HELPTOPICS:
    case FW_ABOUT:
        w2k_msgbox(f->win, "About Windows",
                   "Linux 2000\n\n"
                   "A Windows 2000-like shell, written from scratch against Xlib.\n\n"
                   "Linux 2000 is not affiliated with, endorsed by or sponsored by Microsoft.\nWindows is a trademark of Microsoft Corporation.",
                   MB_OK | MB_ICONINFO);
        return;
    case FW_HISTORY: case FW_GO: case FW_BACK: case FW_FORWARD:
        return;
    default:
        if (f->on_command) f->on_command(f->user, id);
        w2k_win_dirty(f->win);
        return;
    }
    w2k_folderwin_layout(f);
    w2k_win_dirty(f->win);
}

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */
W2kFolderWin *w2k_folderwin_new(const char *title, const char *cls, int icon,
                                int w, int h, void *user,
                                void (*on_command)(void *, int))
{
    W2kFolderWin *f = w2k_alloc(sizeof *f);
    f->user = user;
    f->on_command = on_command;
    f->icon = icon;
    f->pane_w = 200;
    f->hot_link = -1;
    f->show_toolbar = f->show_address = f->show_status = 1;
    snprintf(f->title, sizeof f->title, "%s", title);

    /* The window opens at the reference's size, or what the screen has. */
    if (w > w2k.sw - 40) w = w2k.sw - 40;
    if (h > w2k.sh - 80) h = w2k.sh - 80;
    f->win = w2k_win_new(title, cls, w, h, 1);
    f->win->min_w = 420;
    f->win->min_h = 260;

    f->mb = w2k_menubar_new(f, command);
    f->mb->win_ref = f->win->win;
    w2k_menubar_add(f->mb, "&File", build_file);
    w2k_menubar_add(f->mb, "&Edit", build_edit);
    w2k_menubar_add(f->mb, "&View", build_view);
    w2k_menubar_add(f->mb, "F&avorites", build_favorites);
    w2k_menubar_add(f->mb, "&Tools", build_tools);
    w2k_menubar_add(f->mb, "&Help", build_help);

    f->tb = w2k_toolbar_new(f, command);
    f->tb->show_text = 1;
    for (int i = 0; i < (int)(sizeof std_buttons / sizeof *std_buttons); i++) {
        if (std_buttons[i].id == TBB_SEP) { w2k_toolbar_sep(f->tb); continue; }
        w2k_toolbar_add(f->tb, std_buttons[i].id, std_buttons[i].icon,
                        std_buttons[i].text);
        if (std_buttons[i].drop) w2k_toolbar_drop(f->tb, std_buttons[i].drop);
        if (std_buttons[i].off) w2k_toolbar_enable(f->tb, std_buttons[i].id, 0);
    }

    f->addr = w2k_combo_new(0);
    f->addr->icon = icon;
    w2k_combo_add(f->addr, title);
    f->addr->sel = 0;

    f->list = w2k_list_new(LV_ICON);
    f->list->focused = 1;
    f->list->singleclick = w2k_folder_singleclick;
    w2k_scroll_bind(&f->list->vsb, f->win);

    f->sb = w2k_status_new();
    w2k_status_add(f->sb, 0);
    w2k_status_add(f->sb, 76);
    w2k_status_add(f->sb, 149);
    w2k_status_icon(f->sb, 2, ICO_MYCOMPUTER);
    w2k_status_set(f->sb, 2, "My Computer");

    /* The web view's heading is Tahoma Bold at 17 pixels ("Control Panel"
     * is 113 wide on the reference); fontconfig substitutes DejaVu Sans
     * Bold where Tahoma is not installed, and the heading wraps. */
    f->title_face = w2k_face_open_bold("Tahoma", 17);
    char path[1024];
    if (w2k_skin_path("webview-banner.png", path, sizeof path))
        f->banner = w2k_skin_load(path);

    w2k_folderwin_layout(f);
    return f;
}

void w2k_folderwin_free(W2kFolderWin *f)
{
    if (!f) return;
    w2k_folderwin_pane_clear(f);
    w2k_menubar_free(f->mb);
    w2k_toolbar_free(f->tb);
    w2k_combo_free(f->addr);
    w2k_list_free(f->list);
    w2k_status_free(f->sb);
    w2k_face_close(f->title_face);
    w2k_skin_free(f->banner);
    free(f);
}

/* Optional extra menu between Tools and Help (the network folder's
 * Advanced). Rebuilds the bar. */
static void rebuild_menubar(W2kFolderWin *f)
{
    w2k_menubar_clear(f->mb);
    w2k_menubar_add(f->mb, "&File", build_file);
    w2k_menubar_add(f->mb, "&Edit", build_edit);
    w2k_menubar_add(f->mb, "&View", build_view);
    w2k_menubar_add(f->mb, "F&avorites", build_favorites);
    w2k_menubar_add(f->mb, "&Tools", build_tools);
    if (f->extra_title[0]) w2k_menubar_add(f->mb, f->extra_title, build_extra);
    w2k_menubar_add(f->mb, "&Help", build_help);
}

void w2k_folderwin_extra_menu(W2kFolderWin *f, const char *title,
                              W2kMenu *(*build)(void *user))
{
    snprintf(f->extra_title, sizeof f->extra_title, "%s", title ? title : "");
    f->build_extra = build;
    rebuild_menubar(f);
}

/* ------------------------------------------------------------------ *
 * Layout
 * ------------------------------------------------------------------ */
void w2k_folderwin_layout(W2kFolderWin *f)
{
    W2kWin *w = f->win;
    int y = 0;
    /* The menu bar's text sits three pixels under the band's bar. */
    f->mb->r = (W2kRect){ 1, y + 3, w->w, MENUBAR_H };
    y += BAND;
    if (f->show_toolbar) {
        /* The toolbar draws two pixels of lip above its buttons and three
         * below; the buttons themselves fill the 22-pixel bar. */
        f->tb->r = (W2kRect){ 1, y, w->w, BAR_H + 5 };
        y += BAND;
    } else {
        f->tb->r = (W2kRect){ 0, 0, 0, 0 };
    }
    if (f->show_address) {
        f->addr_label = (W2kRect){ 4, y + 2, 44, BAR_H };
        f->addr->r = (W2kRect){ 49, y + 2, w->w - 49 - 50, BAR_H };
        f->go_r = (W2kRect){ w->w - 45, y + 2, 41, BAR_H };
        y += BAND;
    } else {
        f->addr_label = f->addr->r = f->go_r = (W2kRect){ 0, 0, 0, 0 };
    }
    int bottom = w->h - (f->show_status ? STATUS_H : 0);
    f->sb->r = (W2kRect){ -2, bottom, w->w + 4, f->show_status ? STATUS_H : 0 };

    f->list_r = (W2kRect){ 0, y, w->w, bottom - y };
    int pw = f->pane_w;
    if (pw > w->w - 120) pw = 0;
    f->pane_r = (W2kRect){ 2, y + 2, pw, f->list_r.h - 4 };
    /* The list's own sunken border lands under the pane; the frame
     * around both is drawn afterwards. */
    f->list->r = (W2kRect){ pw, y, w->w - pw, f->list_r.h };
}

/* ------------------------------------------------------------------ *
 * The web-view pane
 * ------------------------------------------------------------------ */
void w2k_folderwin_pane_clear(W2kFolderWin *f)
{
    for (int i = 0; i < f->nlines; i++) free(f->line[i].text);
    f->nlines = 0;
    f->hot_link = -1;
}

void w2k_folderwin_pane_add(W2kFolderWin *f, int style, const char *text)
{
    if (f->nlines >= FW_PANE_LINES) return;
    f->line[f->nlines].style = style;
    f->line[f->nlines].text = w2k_strdup(text ? text : "");
    f->line[f->nlines].r = (W2kRect){ 0, 0, 0, 0 };
    f->nlines++;
}

void w2k_folderwin_status(W2kFolderWin *f, const char *text)
{
    w2k_status_set(f->sb, 0, text);
}

/* Word-wrap `s` into lines no wider than `maxw` in `font`; calls `emit`
 * for each. Returns the number of lines. */
static int wrap(int font, const char *s, int maxw,
                void (*emit)(void *, const char *, int), void *u)
{
    int n = 0;
    const char *p = s;
    while (*p) {
        const char *end = p, *last_space = NULL;
        while (*end) {
            if (*end == ' ') last_space = end;
            if (w2k_text_width(font, p, (int)(end - p) + 1) > maxw) break;
            end++;
        }
        if (*end && last_space && last_space > p) end = last_space;
        if (end == p) end = p + 1;
        emit(u, p, (int)(end - p));
        n++;
        p = end;
        while (*p == ' ') p++;
    }
    if (n == 0) { emit(u, "", 0); n = 1; }
    return n;
}

typedef struct { Drawable d; int x, *y, font, lineh, color; } WrapCtx;

static void emit_line(void *u, const char *s, int n)
{
    WrapCtx *c = u;
    w2k_textn(c->d, c->font, c->x, *c->y, s, n, c->color);
    *c->y += c->lineh;
}

typedef struct { Drawable d; W2kFace *face; int x, *y, lineh; } FaceCtx;

static void emit_face(void *u, const char *s, int n)
{
    FaceCtx *c = u;
    char buf[200];
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    w2k_face_text(c->d, c->face, c->x, *c->y, buf, C_WINDOWTEXT);
    *c->y += c->lineh;
}

/* Wrapping in a W2kFace: measured the same way, with its own widths. */
static int wrap_face(W2kFace *face, const char *s, int maxw, FaceCtx *c)
{
    int n = 0;
    const char *p = s;
    while (*p) {
        const char *end = p, *last_space = NULL;
        while (*end) {
            if (*end == ' ') last_space = end;
            if (w2k_face_width(face, p, (int)(end - p) + 1) > maxw) break;
            end++;
        }
        if (*end && last_space && last_space > p) end = last_space;
        if (end == p) end = p + 1;
        emit_face(c, p, (int)(end - p));
        n++;
        p = end;
        while (*p == ' ') p++;
    }
    return n;
}

static void draw_pane(W2kFolderWin *f, Drawable d)
{
    W2kRect *p = &f->pane_r;
    if (p->w <= 0) return;
    w2k_clip_set(p->x, p->y, p->w, p->h);
    w2k_fill(d, p->x, p->y, p->w, p->h, C_WINDOW);

    /* The banner, and the folder's icon over it at (12, 8). */
    if (f->banner)
        w2k_skin_draw(d, f->banner, p->x, p->y, 0, 0,
                      w2k_skin_w(f->banner), w2k_skin_h(f->banner));
    if (f->icon >= 0) w2k_bigicon_draw(d, p->x + 12, p->y + 8, f->icon);

    /* The heading: capitals 53 below the pane's top on the reference. */
    int tx = p->x + PANE_TEXT_X;
    /* The reference's text runs to 176 of the pane's 200; Xft's advances
     * round up, so four pixels of slack keep the same line breaks. */
    int maxw = p->w - 2 * PANE_TEXT_X + 4;
    int y;
    if (f->title_face) {
        int asc = w2k_face_ascent(f->title_face);
        int lh = w2k_face_height(f->title_face);
        y = p->y + 53 - asc + 13;
        FaceCtx c = { d, f->title_face, tx, &y, lh };
        wrap_face(f->title_face, f->title, maxw, &c);
        y += 3;
    } else {
        y = p->y + 52;
        WrapCtx c = { d, tx, &y, F_UI_BOLD, w2k_font_height(F_UI_BOLD), C_WINDOWTEXT };
        wrap(F_UI_BOLD, f->title, maxw, emit_line, &c);
        y += 4;
    }
    /* The rule: two rows of (102,153,204) across the pane. */
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(102, 153, 204));
    XFillRectangle(w2k.dpy, d, w2k.gc, p->x, y, (unsigned)p->w, 2);
    y += 2 + 15;

    /* Tahoma 8 is set 13 pixels apart in the pane, as in the shell. */
    int fh = w2k_font_height(F_UI);
    if (fh > 13) fh = 13;
    for (int i = 0; i < f->nlines; i++) {
        int st = f->line[i].style;
        const char *s = f->line[i].text;
        if (st == FW_BLANK) {
            /* A blank before a link is a pixel shorter on the reference. */
            int next_link = i + 1 < f->nlines && f->line[i + 1].style == FW_LINK;
            y += fh - (next_link ? 1 : 0);
            continue;
        }
        if (st == FW_LINK) {
            int tw = w2k_text_width(F_UI, s, -1);
            w2k_text_rgb(d, F_UI, tx, y, s, 0, 0, 255);
            XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(0, 0, 255));
            XFillRectangle(w2k.dpy, d, w2k.gc, tx, y + w2k_font_px_ascent(F_UI) + 1,
                           (unsigned)tw, 1);
            f->line[i].r = (W2kRect){ tx, y, tw, fh };
            y += fh + 4;
            continue;
        }
        int font = st == FW_BOLD ? F_UI_BOLD : F_UI;
        WrapCtx c = { d, tx, &y, font, fh, C_WINDOWTEXT };
        wrap(font, s, maxw, emit_line, &c);
    }
    w2k_clip_clear();
}

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */
static void etched(Drawable d, int y, int w)
{
    w2k_hline(d, 0, y, w, C_SHADOW);
    w2k_hline(d, 0, y + 1, w, C_HILIGHT);
}

void w2k_folderwin_paint(W2kFolderWin *f, Drawable d)
{
    W2kWin *w = f->win;
    int fh = w2k_font_height(F_UI);

    w2k_fill(d, 0, 0, w->w, f->list_r.y, C_FACE);
    w2k_menubar_draw(d, f->mb);
    etched(d, 0, w->w);
    int y = BAND;
    if (f->show_toolbar) {
        w2k_toolbar_draw(d, f->tb);
        etched(d, y, w->w);
        y += BAND;
    }
    if (f->show_address) {
        w2k_fill(d, 0, y, w->w, BAND, C_FACE);
        etched(d, y, w->w);
        w2k_text_mnemonic(d, F_UI, f->addr_label.x,
                          f->addr->r.y + (BAR_H - fh) / 2, "A&ddress", C_TEXT, 1);
        w2k_combo_draw(d, f->addr);
        /* Go: a flat button, its arrow and the word. */
        W2kRect *g = &f->go_r;
        if (f->go_down) w2k_edge(d, g->x, g->y, g->w, g->h, EDGE_SUNKEN_THIN, BF_RECT);
        w2k_icon_draw(d, g->x + 5 + f->go_down, g->y + 3 + f->go_down, ICO_TB_GO);
        w2k_text_mnemonic(d, F_UI, g->x + 23 + f->go_down,
                          g->y + (BAR_H - fh) / 2 + f->go_down, "&Go", C_TEXT, 1);
        y += BAND;
    }

    w2k_list_draw(d, f->list);
    draw_pane(f, d);
    w2k_edge(d, f->list_r.x, f->list_r.y, f->list_r.w, f->list_r.h,
             EDGE_SUNKEN, BF_RECT);
    if (f->show_status) w2k_status_draw(d, f->sb);
}

/* ------------------------------------------------------------------ *
 * Events
 * ------------------------------------------------------------------ */
static int link_at(W2kFolderWin *f, int x, int y)
{
    for (int i = 0; i < f->nlines; i++)
        if (f->line[i].style == FW_LINK && w2k_rect_hit(&f->line[i].r, x, y))
            return i;
    return -1;
}

int w2k_folderwin_event(W2kFolderWin *f, XEvent *e)
{
    W2kWin *w = f->win;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_menubar_press(f->mb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (f->show_toolbar && w2k_toolbar_press(f->tb, &e->xbutton)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (f->show_address && w2k_rect_hit(&f->go_r, x, y)) {
            f->go_down = 1;
            w2k_win_dirty(w);
            return 1;
        }
        if (e->xbutton.button == 1 && link_at(f, x, y) >= 0) {
            f->hot_link = link_at(f, x, y);
            return 1;
        }
        if (w2k_list_press(f->list, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        return 0;
    }
    case ButtonRelease: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (f->go_down) {
            f->go_down = 0;
            w2k_win_dirty(w);
            if (w2k_rect_hit(&f->go_r, x, y)) command(f, FW_GO);
            return 1;
        }
        if (f->hot_link >= 0) {
            int i = f->hot_link;
            f->hot_link = -1;
            if (link_at(f, x, y) == i) command(f, FW_LAST + i);
            return 1;
        }
        if (f->show_toolbar) w2k_toolbar_release(f->tb);
        w2k_list_release(f->list, &e->xbutton);
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (f->show_toolbar && w2k_toolbar_motion(f->tb, &e->xmotion)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_list_motion(f->list, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case KeyPress: {
        if (w2k_menubar_key(f->mb, &e->xkey)) { w2k_win_dirty(w); return 1; }
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_F5) { command(f, FW_REFRESH); return 1; }
        if ((e->xkey.state & ControlMask) && (ks == XK_a || ks == XK_A)) {
            command(f, FW_SELECTALL);
            return 1;
        }
        if (w2k_list_key(f->list, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 0;
    }
    }
    return 0;
}
