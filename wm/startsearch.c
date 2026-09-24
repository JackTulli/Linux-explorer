/* startsearch.c -- searching from the Start menu, inside the Start menu.
 *
 * Typing at any of the three Start menus searches: the panels (XP's and
 * 7's) give their left column over to the results and show what was
 * typed in the search box; the classic menu is replaced, where it stood,
 * by a panel of results in its own style. The first key typed is the
 * first letter of the search -- nothing is lost to opening a dialog.
 *
 * What is searched: the programs the Programs menu knows, the names
 * Windows users type (lib/alias.c: taskmgr, calc, winver...) and the
 * recent documents. Enter opens the selected result, Up and Down move
 * the selection, Escape clears the search, Backspace past the first
 * letter ends it. */
#include "wm.h"
#include <X11/keysym.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAXRES ((int)(sizeof ((SearchState *)0)->res / sizeof ((SearchState *)0)->res[0]))

/* "@terminal" is the desktop's terminal; anything else is a command. */
void wm_run_alias(const W2kAlias *a)
{
    if (!a) return;
    if (!strcmp(a->cmd, "@terminal")) { if (wm_terminal_cmd()) wm_spawn(wm_terminal_cmd()); }
    else wm_spawn(a->cmd);
}

static int same_name(const char *a, const char *b) { return !strcasecmp(a, b); }

/* Rebuild the results for the query: programs whose names start with it
 * (then contain it), the Windows names, recent documents. An alias whose
 * program is already listed under its own name is left out. */
static void search_update(SearchState *s)
{
    s->n = 0;
    s->sel = 0;
    if (!s->query[0]) return;

    int ids[MAXRES];
    const char *names[MAXRES];
    int np = programs_search(s->query, ids, names, MAXRES - 4);
    for (int i = 0; i < np && s->n < MAXRES; i++) {
        SResult *r = &s->res[s->n++];
        r->kind = SR_PROG; r->id = ids[i]; r->alias = NULL;
        r->icon = programs_icon(ids[i]);
        snprintf(r->name, sizeof r->name, "%s", names[i]);
        /* Taken now: opening All Programs rescans and re-sorts, and the
         * id would then name a different program. */
        r->cmd[0] = 0;
        r->terminal = 0;
        programs_command(ids[i], r->cmd, sizeof r->cmd, &r->terminal, NULL, 0);
    }
    const W2kAlias *al[16];
    int na = w2k_alias_search(s->query, al, 16);
    for (int i = 0; i < na && s->n < MAXRES; i++) {
        int dup = 0;
        for (int j = 0; j < s->n; j++)
            if (s->res[j].kind == SR_PROG && same_name(s->res[j].name, al[i]->label)) dup = 1;
        if (dup) continue;
        SResult *r = &s->res[s->n++];
        r->kind = SR_ALIAS; r->id = 0; r->alias = al[i]; r->icon = al[i]->icon;
        snprintf(r->name, sizeof r->name, "%s", al[i]->label);
    }
    int nr = recent_load();
    for (int i = 0; i < nr && s->n < MAXRES; i++) {
        const char *l = recent_label(i);
        if (!l || !w2k_strcasestr(l, s->query)) continue;
        SResult *r = &s->res[s->n++];
        r->kind = SR_RECENT; r->id = i; r->alias = NULL;
        r->icon = w2k_file_icon(l, 0);
        snprintf(r->name, sizeof r->name, "%s", l);
        const char *rf = recent_file(i);      /* the list is rebuilt on every load */
        snprintf(r->cmd, sizeof r->cmd, "%s", rf ? rf : "");
    }
}

void startsearch_begin(SearchState *s, const char *first)
{
    memset(s, 0, sizeof *s);
    if (first) snprintf(s->query, sizeof s->query, "%s", first);
    search_update(s);
}

int startsearch_key(SearchState *s, XKeyEvent *k)
{
    KeySym ks = XLookupKeysym(k, 0);
    if (ks == XK_Escape) return SS_ESC;
    if (ks == XK_Return || ks == XK_KP_Enter) return s->n ? SS_RUN : SS_NONE;
    if (ks == XK_Up)   { if (s->sel > 0) s->sel--; return SS_CHANGED; }
    if (ks == XK_Down) { if (s->sel < s->n - 1) s->sel++; return SS_CHANGED; }
    if (ks == XK_BackSpace) {
        size_t n = strlen(s->query);
        if (!n) return SS_EMPTY;
        /* Back over a whole UTF-8 character. */
        do n--; while (n > 0 && (s->query[n] & 0xc0) == 0x80);
        s->query[n] = 0;
        if (!n) return SS_EMPTY;
        search_update(s);
        return SS_CHANGED;
    }
    char buf[16] = "";
    int n = XLookupString(k, buf, sizeof buf - 1, NULL, NULL);
    if (n <= 0 || (unsigned char)buf[0] < ' ' || buf[0] == 127) return SS_NONE;
    buf[n] = 0;
    size_t have = strlen(s->query);
    if (have + (size_t)n >= sizeof s->query) return SS_NONE;
    memcpy(s->query + have, buf, (size_t)n + 1);
    search_update(s);
    return SS_CHANGED;
}

int startsearch_run(SearchState *s, int i)
{
    if (i < 0 || i >= s->n) return 0;
    SResult *r = &s->res[i];
    switch (r->kind) {
    case SR_PROG: {
        if (!r->cmd[0]) return 0;
        programs_note_use(r->name);
        const char *term = wm_terminal_cmd();
        char line[1200];
        if (r->terminal && term) snprintf(line, sizeof line, "%s -e %s", term, r->cmd);
        else                     snprintf(line, sizeof line, "%s", r->cmd);
        wm_spawn(line);
        return 1;
    }
    case SR_ALIAS:  wm_run_alias(r->alias); return 1;
    case SR_RECENT: {
        const char *f = r->cmd[0] ? r->cmd : NULL;
        if (!f) return 0;
        /* The desktop's own associations, as Start > Documents uses: with
         * xdg-open the same file opened in another program from here. */
        char open[2300];
        w2k_assoc_command(f, open, sizeof open);
        wm_spawn(open);
        return 1;
    }
    }
    return 0;
}

/* The rows: a 16-pixel icon and the name, the selected one in the
 * highlight colour. `menu_look` draws them as the classic menu draws its
 * items; otherwise as the panels do, on `bg`. Below the last, "No items
 * match your search." when nothing did. */
void startsearch_draw_rows(Drawable d, SearchState *s, int x, int y, int w, int maxh,
                           int rowh, unsigned long bg, int menu_look)
{
    int fh = w2k_font_height(F_UI);
    int shown = 0;
    for (int i = 0; i < s->n && (shown + 1) * rowh <= maxh; i++, shown++) {
        SResult *r = &s->res[i];
        int ry = y + i * rowh;
        int sel = i == s->sel;
        XSetForeground(w2k.dpy, w2k.gc, sel ? w2k.col[C_HIGHLIGHT] : bg);
        w2k_fill_fg(d, x, ry, w, rowh);
        if (r->icon >= 0) w2k_icon_draw(d, x + (menu_look ? 4 : 8), ry + (rowh - 16) / 2, r->icon);
        char buf[160];
        int tx = x + (menu_look ? 26 : 32);
        w2k_ellipsis(F_UI, r->name, w - (tx - x) - 8, buf, sizeof buf);
        /* Panel rows sit on a window-coloured ground (7's and XP's white
         * pane, or the column over other looks): the text that goes with it. */
        w2k_text(d, F_UI, tx, ry + (rowh - fh) / 2, buf,
                 sel ? C_HIGHLIGHTTEXT : (menu_look ? C_MENUTEXT : C_WINDOWTEXT));
    }
    if (!s->n && s->query[0]) {
        XSetForeground(w2k.dpy, w2k.gc, bg);
        w2k_fill_fg(d, x, y, w, rowh);
        w2k_text(d, F_UI, x + 8, y + (rowh - fh) / 2, "No items match your search.", C_GRAYTEXT);
    }
}

int startsearch_row_at(SearchState *s, int y0, int rowh, int maxh, int y)
{
    if (y < y0 || rowh <= 0) return -1;
    int i = (y - y0) / rowh;
    if (i >= s->n || (i + 1) * rowh > maxh) return -1;
    return i;
}

/* The query in a white box, a caret after it; the magnifier at the end. */
void startsearch_draw_box(Drawable d, SearchState *s, int x, int y, int w, int h)
{
    int fh = w2k_font_height(F_UI);
    w2k_fill(d, x, y, w, h, C_WINDOW);
    if (W2K_THEME_IS7(w2k_theme)) w2k_fill_rgb(d, x, y, w, 1, 171, 173, 179),
                                  w2k_fill_rgb(d, x, y + h - 1, w, 1, 171, 173, 179),
                                  w2k_fill_rgb(d, x, y, 1, h, 171, 173, 179),
                                  w2k_fill_rgb(d, x + w - 1, y, 1, h, 171, 173, 179);
    else w2k_edge(d, x, y, w, h, EDGE_SUNKEN, BF_RECT);
    int tx = x + 6, ty = y + (h - fh) / 2;
    char buf[160];
    w2k_ellipsis(F_UI, s->query, w - 30, buf, sizeof buf);
    w2k_text(d, F_UI, tx, ty, buf, C_WINDOWTEXT);
    int cx = tx + w2k_text_width(F_UI, buf, -1) + 1;
    w2k_fill(d, cx, ty, 1, fh, C_WINDOWTEXT);
    /* The magnifier, as the Windows 7 panel draws it. */
    int mx = x + w - 14, my = y + h / 2 - 2;
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(58, 96, 140));
    XSetLineAttributes(w2k.dpy, w2k.gc, (unsigned)w2k_th(1), LineSolid, CapButt, JoinMiter);
    XDrawArc(w2k.dpy, d, w2k.gc, w2k_cx(mx - 4), w2k_cx(my - 4),
             (unsigned)w2k_cw(mx - 4, 7), (unsigned)w2k_cw(my - 4, 7), 0, 360 * 64);
    XSetLineAttributes(w2k.dpy, w2k.gc, 0, LineSolid, CapButt, JoinMiter);
    for (int i = 0; i < 5; i++) {
        w2k_fill_fg(d, mx + 2 + i, my + 2 + i, 1, 1);
        w2k_fill_fg(d, mx + 3 + i, my + 2 + i, 1, 1);
    }
}

/* ------------------------------------------------------------------ *
 * The classic menu's search: a panel in the menu's style, exactly where
 * the menu was and as big as it was, with its banner down the left.
 * ------------------------------------------------------------------ */
#define CS_ROW    20

void startsearch_classic(const char *first, int bx, int by, int mw, int mh,
                         int banner_w, const char *banner)
{
    SearchState s;
    startsearch_begin(&s, first);

    int bd = w2k_menu_border();
    int pw = w2k_px(mw), ph = w2k_px(mh);
    const W2kMonitor *m = w2k_monitor_primary();
    int px = bx, py = w2k_taskbar_edge == TB_TOP ? by : by - ph;
    if (py < m->y) py = m->y;
    if (px + pw > m->x + m->w) px = m->x + m->w - pw;

    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel = w2k.col[C_MENU],
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | KeyPressMask
    };
    Window win = XCreateWindow(w2k.dpy, w2k.root, px, py, (unsigned)pw, (unsigned)ph, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    XMapRaised(w2k.dpy, win);
    if (XGrabPointer(w2k.dpy, win, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, w2k.cur_arrow, CurrentTime) != GrabSuccess) {
        XDestroyWindow(w2k.dpy, win);
        return;
    }
    if (XGrabKeyboard(w2k.dpy, win, True, GrabModeAsync, GrabModeAsync,
                      CurrentTime) != GrabSuccess) {
        /* Without the keys there is nothing to type into: give up rather
         * than leave a panel that only a click can dismiss. */
        XUngrabPointer(w2k.dpy, CurrentTime);
        XDestroyWindow(w2k.dpy, win);
        return;
    }

    int left = bd + banner_w;
    int box_x = left + 4, box_y = bd + 4, box_w = mw - left - bd - 8, box_h = 22;
    int rows_y = box_y + box_h + 4, rows_h = mh - bd - 2 - rows_y;
    int done = 0, run = -1;
    Pixmap pm = XCreatePixmap(w2k.dpy, win, (unsigned)pw, (unsigned)ph, w2k.depth);
    startpanel_cancel = 0;
    while (!done && running && !startpanel_cancel) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        int paint = 0;
        switch (e.type) {
        case Expose: paint = e.xexpose.window == win; if (!paint) wm_handle_event(&e); break;
        case MotionNotify: {
            int i = startsearch_row_at(&s, rows_y, CS_ROW, rows_h, w2k_lp(e.xmotion.y_root - py));
            if (i >= 0 && i != s.sel) { s.sel = i; paint = 1; }
            break;
        }
        case ButtonPress:
            if (e.xbutton.x_root < px || e.xbutton.x_root >= px + pw ||
                e.xbutton.y_root < py || e.xbutton.y_root >= py + ph) done = 1;
            break;
        case ButtonRelease:
            if (e.xbutton.button == Button1) {
                int i = startsearch_row_at(&s, rows_y, CS_ROW, rows_h, w2k_lp(e.xbutton.y_root - py));
                if (i >= 0) { run = i; done = 1; }
            }
            break;
        case KeyPress: {
            KeySym sk = XLookupKeysym(&e.xkey, 0);
            if (sk == XK_Super_L || sk == XK_Super_R) { done = 1; break; }
            int r = startsearch_key(&s, &e.xkey);
            if (r == SS_RUN) { run = s.sel; done = 1; }
            else if (r == SS_ESC || r == SS_EMPTY) done = 1;
            else if (r == SS_CHANGED) paint = 1;
            break;
        }
        default: wm_handle_event(&e); break;
        }
        if (paint) {
            w2k_fill(pm, 0, 0, mw, mh, C_MENU);
            w2k_edge(pm, 0, 0, mw, mh, EDGE_RAISED, BF_RECT);
            if (banner_w > 0) {
                /* As the menu draws it: the gradient, the name reading up. */
                int bh = mh - 2 * bd;
                w2k_menu_banner_fill(pm, bd, bd, banner_w, bh);
                w2k_text_vertical(pm, F_UI_BOLD, bd + 4, bd + bh - 6, banner, C_WHITE);
            }
            startsearch_draw_box(pm, &s, box_x, box_y, box_w, box_h);
            startsearch_draw_rows(pm, &s, left + 2, rows_y, mw - left - bd - 4, rows_h,
                                  CS_ROW, w2k.col[C_MENU], 1);
            XCopyArea(w2k.dpy, pm, win, w2k.gc, 0, 0, (unsigned)pw, (unsigned)ph, 0, 0);
        }
    }
    w2k_free_pixmap(pm);
    XUngrabKeyboard(w2k.dpy, CurrentTime);
    XUngrabPointer(w2k.dpy, CurrentTime);
    XDestroyWindow(w2k.dpy, win);
    XFlush(w2k.dpy);
    if (run >= 0) startsearch_run(&s, run);
}
