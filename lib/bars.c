/* bars.c -- menu bar, toolbar, status bar and tab control. */
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * Menu bar
 * ------------------------------------------------------------------ */
W2kMenubar *w2k_menubar_new(void *user, void (*on_command)(void *, int))
{
    W2kMenubar *mb = w2k_alloc(sizeof *mb);
    mb->user = user;
    mb->on_command = on_command;
    mb->open = -1;
    mb->r.h = MENUBAR_H;
    return mb;
}

void w2k_menubar_add(W2kMenubar *mb, const char *text,
                     W2kMenu *(*build)(void *))
{
    if (mb->n >= 10) return;
    mb->item[mb->n].text = w2k_strdup(text);
    mb->item[mb->n].build = build;
    mb->n++;
}

/* Drop every menu so the bar can be built again: Task Manager shows its
 * Windows menu only while the Applications tab is up, which is how the
 * original behaves. */
void w2k_menubar_clear(W2kMenubar *mb)
{
    if (!mb) return;
    for (int i = 0; i < mb->n; i++) {
        free(mb->item[i].text);
        mb->item[i].text = NULL;
    }
    mb->n = 0;
    mb->open = -1;
}

void w2k_menubar_free(W2kMenubar *mb)
{
    if (!mb) return;
    for (int i = 0; i < mb->n; i++) free(mb->item[i].text);
    free(mb);
}

static void menubar_layout(W2kMenubar *mb)
{
    int x = mb->r.x + 1;
    for (int i = 0; i < mb->n; i++) {
        /* Measured off the shell: the titles sit eight pixels in and
         * sixteen apart beyond their text. */
        int w = w2k_mnemonic_width(F_UI, mb->item[i].text) + 16;
        mb->item[i].x = x;
        mb->item[i].w = w;
        x += w;
    }
}

void w2k_menubar_draw(Drawable d, W2kMenubar *mb)
{
    menubar_layout(mb);
    w2k_fill(d, mb->r.x, mb->r.y, mb->r.w, MENUBAR_H, C_MENU);
    int fh = w2k_font_height(F_UI);
    for (int i = 0; i < mb->n; i++) {
        int x = mb->item[i].x, w = mb->item[i].w;
        int hot = (i == mb->open);
        if (hot) w2k_fill(d, x, mb->r.y + 1, w, MENUBAR_H - 2, C_HIGHLIGHT);
        w2k_text_mnemonic(d, F_UI, x + 8, mb->r.y + (MENUBAR_H - fh) / 2,
                          mb->item[i].text,
                          hot ? C_HIGHLIGHTTEXT : C_MENUTEXT, 1);
    }
}

static int menubar_index_at(W2kMenubar *mb, int x, int y)
{
    if (y < mb->r.y || y >= mb->r.y + MENUBAR_H) return -1;
    for (int i = 0; i < mb->n; i++)
        if (x >= mb->item[i].x && x < mb->item[i].x + mb->item[i].w) return i;
    return -1;
}

/* Open dropdown `i`, and keep going while the user slides onto a sibling --
 * which is what makes a menu bar feel like a menu bar. */
static void menubar_track(W2kMenubar *mb, int i)
{
    Window root_ret, child;
    int rx, ry, wx, wy;
    unsigned mask;

    while (i >= 0 && i < mb->n) {
        mb->open = i;
        W2kMenu *m = mb->item[i].build ? mb->item[i].build(mb->user) : NULL;
        if (!m) break;

        /* Position in root coordinates, just under the bar. */
        int gx, gy;
        Window dummy;
        XTranslateCoordinates(w2k.dpy, mb->win_ref, w2k.root,
                              mb->item[i].x, mb->r.y + MENUBAR_H,
                              &gx, &gy, &dummy);

        int id = w2k_menu_popup(m, gx, gy, MPOP_LEFT);
        w2k_menu_free(m);
        mb->open = -1;

        if (id) {
            if (mb->on_command) mb->on_command(mb->user, id);
            return;
        }
        /* Dismissed: if the pointer landed on another title, open that one. */
        XQueryPointer(w2k.dpy, w2k.root, &root_ret, &child, &rx, &ry, &wx, &wy,
                      &mask);
        int lx, ly;
        XTranslateCoordinates(w2k.dpy, w2k.root, mb->win_ref, rx, ry, &lx, &ly,
                              &dummy);
        int j = menubar_index_at(mb, lx, ly);
        if (j < 0 || j == i) return;
        i = j;
    }
    mb->open = -1;
}

int w2k_menubar_press(W2kMenubar *mb, XButtonEvent *b)
{
    if (b->button != Button1) return 0;
    menubar_layout(mb);
    int i = menubar_index_at(mb, b->x, b->y);
    if (i < 0) return 0;
    menubar_track(mb, i);
    return 1;
}

int w2k_menubar_key(W2kMenubar *mb, XKeyEvent *k)
{
    KeySym ks = XLookupKeysym(k, 0);
    if (ks == XK_F10 && !(k->state & ShiftMask)) { menubar_track(mb, 0); return 1; }
    if (!(k->state & Mod1Mask)) return 0;

    int want = (int)ks;
    if (want >= 'A' && want <= 'Z') want += 32;
    for (int i = 0; i < mb->n; i++) {
        const char *t = mb->item[i].text;
        for (const char *p = t; *p; p++)
            if (*p == '&' && p[1]) {
                int ch = p[1];
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                if (ch == want) { menubar_track(mb, i); return 1; }
                break;
            }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Toolbar
 * ------------------------------------------------------------------ */
W2kToolbar *w2k_toolbar_new(void *user, void (*on_command)(void *, int))
{
    W2kToolbar *tb = w2k_alloc(sizeof *tb);
    tb->user = user;
    tb->on_command = on_command;
    tb->hot = tb->pressed = -1;
    tb->r.h = TOOLBAR_H;
    return tb;
}

void w2k_toolbar_add(W2kToolbar *tb, int id, int icon, const char *text)
{
    if (tb->n >= 24) return;
    tb->b[tb->n].id = id;
    tb->b[tb->n].icon = icon;
    tb->b[tb->n].text = text ? w2k_strdup(text) : NULL;
    tb->n++;
}

void w2k_toolbar_sep(W2kToolbar *tb)
{
    if (tb->n >= 24) return;
    tb->b[tb->n].id = TBB_SEP;
    tb->b[tb->n].icon = ICO_NONE;
    tb->n++;
}

void w2k_toolbar_drop(W2kToolbar *tb, int bay)
{
    if (tb->n > 0) tb->b[tb->n - 1].drop = bay > 1 ? bay : 1;
}

void w2k_toolbar_enable(W2kToolbar *tb, int id, int on)
{
    for (int i = 0; i < tb->n; i++)
        if (tb->b[i].id == id) tb->b[i].disabled = !on;
}

void w2k_toolbar_free(W2kToolbar *tb)
{
    if (!tb) return;
    for (int i = 0; i < tb->n; i++) free(tb->b[i].text);
    free(tb);
}

static void toolbar_layout(W2kToolbar *tb)
{
    int x = tb->r.x + 2;
    for (int i = 0; i < tb->n; i++) {
        int w;
        /* Measured off the shell's toolbar: an icon-only button is 24
         * wide; one with text is 31 plus the text; a drop-down arrow
         * adds a 12-pixel bay. */
        int has_text = tb->show_text && tb->b[i].text && tb->b[i].text[0];
        int has_icon = tb->b[i].icon >= 0;
        if (tb->b[i].id == TBB_SEP) w = 8;
        else if (has_text && has_icon)
            w = 31 + w2k_text_width(F_UI, tb->b[i].text, -1);
        else if (has_text)                       /* words alone: 6 in, 6 out */
            w = 12 + w2k_text_width(F_UI, tb->b[i].text, -1);
        else w = 24;
        if (tb->b[i].id != TBB_SEP && tb->b[i].drop)
            w += tb->b[i].drop > 1 ? tb->b[i].drop : tb->b[i].text ? 8 : 13;
        tb->b[i].x = x;
        tb->b[i].w = w;
        x += w;
    }
}

void w2k_toolbar_draw(Drawable d, W2kToolbar *tb)
{
    toolbar_layout(tb);
    w2k_fill(d, tb->r.x, tb->r.y, tb->r.w, tb->r.h, C_FACE);
    /* The toolbar sits under a thin raised lip. */
    w2k_hline(d, tb->r.x, tb->r.y, tb->r.w, C_HILIGHT);
    w2k_hline(d, tb->r.x, tb->r.y + tb->r.h - 1, tb->r.w, C_SHADOW);

    int by = tb->r.y + 2, bh = tb->r.h - 5;
    for (int i = 0; i < tb->n; i++) {
        int x = tb->b[i].x, w = tb->b[i].w;
        if (tb->b[i].id == TBB_SEP) {
            w2k_vline(d, x + 3, by + 1, bh - 2, C_SHADOW);
            w2k_vline(d, x + 4, by + 1, bh - 2, C_HILIGHT);
            continue;
        }
        int down = (i == tb->pressed && i == tb->hot);
        /* Flat until hovered -- the Windows 2000 toolbar style. */
        if (down)                w2k_edge(d, x, by, w, bh, EDGE_SUNKEN_THIN, BF_RECT);
        else if (i == tb->hot && !tb->b[i].disabled)
                                 w2k_edge(d, x, by, w, bh, EDGE_RAISED_THIN, BF_RECT);

        /* An icon-only button centres its icon in the part before the
         * drop-down bay. */
        int has_text = tb->show_text && tb->b[i].text && tb->b[i].text[0];
        int has_icon = tb->b[i].icon >= 0;
        int bay = !tb->b[i].drop ? 0 : tb->b[i].drop > 1 ? tb->b[i].drop
                : has_text ? 8 : 13;
        int body = w - bay;
        int ix = x + (has_text && has_icon ? 4 : (body - 16) / 2) + down;
        int iy = by + (bh - 16) / 2 + down;
        if (has_icon) {
            if (tb->b[i].disabled) w2k_icon_draw_disabled(d, ix, iy, tb->b[i].icon);
            else                   w2k_icon_draw(d, ix, iy, tb->b[i].icon);
        }

        if (has_text) {
            int fh = w2k_font_height(F_UI);
            int tx = (has_icon ? ix + 18 : x + 6 + down), ty = by + (bh - fh) / 2 + down;
            if (tb->b[i].disabled) {
                w2k_text(d, F_UI, tx + 1, ty + 1, tb->b[i].text, C_HILIGHT);
                w2k_text(d, F_UI, tx, ty, tb->b[i].text, C_GRAYTEXT);
            } else {
                w2k_text(d, F_UI, tx, ty, tb->b[i].text, C_TEXT);
            }
        }
        if (tb->b[i].drop) {
            /* A 7-wide, 4-tall triangle, three pixels into the bay. */
            int ax = x + w - 8 + down, ay = by + bh / 2 - 2 + down;
            int col = tb->b[i].disabled ? C_GRAYTEXT : C_TEXT;
            if (tb->b[i].disabled) {
                w2k_hline(d, ax + 1, ay + 1, 7, C_HILIGHT);
                w2k_hline(d, ax + 2, ay + 2, 5, C_HILIGHT);
                w2k_hline(d, ax + 3, ay + 3, 3, C_HILIGHT);
                w2k_hline(d, ax + 4, ay + 4, 1, C_HILIGHT);
            }
            w2k_hline(d, ax, ay, 7, col);
            w2k_hline(d, ax + 1, ay + 1, 5, col);
            w2k_hline(d, ax + 2, ay + 2, 3, col);
            w2k_hline(d, ax + 3, ay + 3, 1, col);
        }
    }
}

static int toolbar_at(W2kToolbar *tb, int x, int y)
{
    if (y < tb->r.y || y >= tb->r.y + tb->r.h) return -1;
    for (int i = 0; i < tb->n; i++)
        if (tb->b[i].id != TBB_SEP && x >= tb->b[i].x &&
            x < tb->b[i].x + tb->b[i].w) return i;
    return -1;
}

int w2k_toolbar_press(W2kToolbar *tb, XButtonEvent *b)
{
    if (b->button != Button1) return 0;
    int i = toolbar_at(tb, b->x, b->y);
    if (i < 0 || tb->b[i].disabled) return 0;
    tb->pressed = tb->hot = i;
    return 1;
}

int w2k_toolbar_motion(W2kToolbar *tb, XMotionEvent *m)
{
    int i = toolbar_at(tb, m->x, m->y);
    if (i == tb->hot) return 0;
    tb->hot = i;
    return 1;
}

void w2k_toolbar_release(W2kToolbar *tb)
{
    int i = tb->pressed;
    tb->pressed = -1;
    if (i >= 0 && i == tb->hot && !tb->b[i].disabled && tb->on_command)
        tb->on_command(tb->user, tb->b[i].id);
}

/* ------------------------------------------------------------------ *
 * Status bar
 * ------------------------------------------------------------------ */
W2kStatus *w2k_status_new(void)
{
    W2kStatus *s = w2k_alloc(sizeof *s);
    s->r.h = STATUS_H;
    s->sizegrip = 1;
    return s;
}

void w2k_status_add(W2kStatus *s, int w)
{
    if (s->n >= 6) return;
    s->pane[s->n].w = w;
    s->pane[s->n].text = w2k_strdup("");
    s->pane[s->n].icon = ICO_NONE;
    s->n++;
}

void w2k_status_icon(W2kStatus *s, int i, int icon)
{
    if (i < 0 || i >= s->n) return;
    s->pane[i].icon = icon;
}

void w2k_status_set(W2kStatus *s, int i, const char *text)
{
    if (i < 0 || i >= s->n) return;
    free(s->pane[i].text);
    s->pane[i].text = w2k_strdup(text ? text : "");
}

void w2k_status_free(W2kStatus *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) free(s->pane[i].text);
    free(s);
}

/* The diagonal ribbing of the resize grip. */
/* The sizing grip: three diagonals in a 12x12 box, each one white pixel
 * with two shadow pixels to its right, spaced four apart and clipped to
 * the box. Traced off the Windows 2000 Task Manager screenshot -- the
 * blocky two-by-two version this replaces was visibly coarser. */
static void sizegrip(Drawable d, int x, int y)
{
    int rx = x + 11, by = y + 11;
    for (int k = 0; k < 3; k++)
        for (int i = 0; x + 4 * k + i <= rx; i++) {
            int px = x + 4 * k + i, py = by - i;
            if (py < y) break;
            w2k_fill(d, px, py, 1, 1, C_HILIGHT);
            if (px + 1 <= rx) w2k_fill(d, px + 1, py, 1, 1, C_SHADOW);
            if (px + 2 <= rx) w2k_fill(d, px + 2, py, 1, 1, C_SHADOW);
        }
}

void w2k_status_draw(Drawable d, W2kStatus *s)
{
    /* No highlight along the top: the Windows status bar is plain face
     * with the panes sunk into it, as the Task Manager screenshot shows. */
    w2k_fill(d, s->r.x, s->r.y, s->r.w, s->r.h, C_FACE);

    int fixed = 0, nstretch = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->pane[i].w > 0) fixed += s->pane[i].w;
        else nstretch++;
    }
    /* The grip is drawn over the last pane rather than beside it -- the
     * pane runs to the end of the bar, and only its text keeps clear. */
    int grip = 2;
    int avail = s->r.w - 4 - fixed - grip - (s->n - 1) * 2;
    int stretch = nstretch ? avail / nstretch : 0;

    int x = s->r.x + 2;
    int fh = w2k_font_height(F_UI);
    for (int i = 0; i < s->n; i++) {
        int w = s->pane[i].w > 0 ? s->pane[i].w : stretch;
        if (i == s->n - 1) w = s->r.x + s->r.w - grip - 2 - x;
        if (w < 4) w = 4;
        w2k_edge(d, x, s->r.y + 2, w, s->r.h - 4, EDGE_SUNKEN_THIN, BF_RECT);
        /* Text two pixels in from the pane's edge, an icon (16 pixels
         * and a gap) before it -- measured off the shell's own bar. */
        int tx = x + 3;
        if (s->pane[i].icon >= 0) {
            w2k_icon_draw(d, x + 4, s->r.y + 2 + (s->r.h - 4 - 16) / 2,
                          s->pane[i].icon);
            tx = x + 22;
        }
        char buf[160];
        int room = x + w - 4 - tx - (s->sizegrip && i == s->n - 1 ? 14 : 0);
        w2k_ellipsis(F_UI, s->pane[i].text, room, buf, sizeof buf);
        w2k_text(d, F_UI, tx, s->r.y + 2 + (s->r.h - 4 - fh) / 2, buf, C_TEXT);
        x += w + 2;
    }
    if (s->sizegrip)
        /* One pixel inside the last pane's bottom-right corner. */
        sizegrip(d, s->r.x + s->r.w - 17, s->r.y + s->r.h - 15);
}

/* ------------------------------------------------------------------ *
 * Tab control
 * ------------------------------------------------------------------ */
W2kTabs *w2k_tabs_new(void *user, void (*on_change)(void *, int))
{
    W2kTabs *t = w2k_alloc(sizeof *t);
    t->user = user;
    t->on_change = on_change;
    /* Lets W2K_RENDER (see w2k_win_show) capture a page other than the first. */
    const char *sel = getenv("W2K_RENDER_TAB");
    if (sel) t->sel = atoi(sel);
    return t;
}

void w2k_tabs_add(W2kTabs *t, const char *text)
{
    if (t->n >= 8) return;
    t->tab[t->n++].text = w2k_strdup(text);
}

void w2k_tabs_free(W2kTabs *t)
{
    if (!t) return;
    for (int i = 0; i < t->n; i++) free(t->tab[i].text);
    free(t);
}

/* Geometry, measured off a Windows 2000 screenshot of Task Manager
 * (the tab strip at 1:1, three tabs, the first selected):
 *
 *   strip top T = 47      selected tab top
 *   base tabs         T+2 .. B-1      (18 rows)
 *   body top      B = T+20            the raised edge of the page
 *   selected tab      T   .. B        two pixels prouder on every side,
 *                                     and one row further down, so that it
 *                                     interrupts the body's top edge
 *
 * and per tab, with the rect [l, l+w) and rows top..bottom:
 *
 *   white  (l+2 .. l+w-3, top)        the top edge, chamfered at both ends
 *   white  (l+1, top+1)               the chamfer pixel
 *   white  (l,   top+2 .. bottom)     the left edge
 *   dark   (l+w-2, top+1)             the right chamfer, which is dark
 *   shadow (l+w-2, top+2 .. bottom)   the right edge, two pixels
 *   dark   (l+w-1, top+2 .. bottom)
 *
 * Tabs abut exactly: the next tab's highlight sits one pixel right of the
 * previous tab's dark edge. The selected tab, being two pixels wider,
 * covers its neighbours' highlights, which is what makes it look raised.
 */
static void tabs_layout(W2kTabs *t)
{
    int x = t->r.x + 2;
    for (int i = 0; i < t->n; i++) {
        int w = w2k_mnemonic_width(F_UI, t->tab[i].text) + 2 * TAB_PAD;
        t->tab[i].x = x;
        t->tab[i].w = w;
        x += w;
    }
}

W2kRect w2k_tabs_client(W2kTabs *t)
{
    return (W2kRect){ t->r.x + 3, t->r.y + TABS_H + 3,
                      t->r.w - 6, t->r.h - TABS_H - 6 };
}

static void tab_shape(Drawable d, int l, int top, int w, int bottom)
{
    int h = bottom - top;
    w2k_fill(d, l + 1, top + 1, w - 2, h, C_FACE);
    w2k_hline(d, l + 2, top, w - 4, C_HILIGHT);
    w2k_fill(d, l + 1, top + 1, 1, 1, C_HILIGHT);
    w2k_vline(d, l, top + 2, h - 1, C_HILIGHT);
    w2k_fill(d, l + w - 2, top + 1, 1, 1, C_DKSHADOW);
    w2k_vline(d, l + w - 2, top + 2, h - 1, C_SHADOW);
    w2k_vline(d, l + w - 1, top + 2, h - 1, C_DKSHADOW);
}

void w2k_tabs_draw(Drawable d, W2kTabs *t)
{
    tabs_layout(t);
    int by = t->r.y + TABS_H;              /* top edge row of the body */

    /* The body's white edge is on the outside, one pixel, with the two
     * dark pixels on the other two sides -- DrawEdge(EDGE_RAISED|BF_SOFT),
     * which is the same edge a button wears. */
    w2k_edge(d, t->r.x, by, t->r.w, t->r.h - TABS_H, EDGE_BUTTON, BF_RECT);
    w2k_fill(d, t->r.x + 2, by + 2, t->r.w - 4, t->r.h - TABS_H - 4, C_FACE);

    /* The label sits a fixed distance below the tab's own top edge, so
     * the selected tab -- which starts two pixels higher -- carries its
     * label up with it. That is what the screenshot shows, and it is why
     * a selected tab reads as lifted rather than merely fatter. */
    int fh = w2k_font_height(F_UI);
    int label_dy = (TABS_H - 2 - fh) / 2 + 1;

    /* Unselected first, then the selected one over the top of them. */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < t->n; i++) {
            int sel = (i == t->sel);
            if (sel != pass) continue;
            int l = t->tab[i].x - (sel ? 2 : 0);
            int w = t->tab[i].w + (sel ? 4 : 0);
            int top = t->r.y + (sel ? 0 : 2);
            tab_shape(d, l, top, w, sel ? by : by - 1);
            w2k_text_mnemonic(d, F_UI,
                              t->tab[i].x +
                                  (t->tab[i].w -
                                   w2k_mnemonic_width(F_UI, t->tab[i].text)) / 2,
                              top + label_dy, t->tab[i].text, C_TEXT, 1);
        }
}

int w2k_tabs_press(W2kTabs *t, XButtonEvent *b)
{
    if (b->button != Button1) return 0;
    tabs_layout(t);
    if (b->y < t->r.y || b->y >= t->r.y + TABS_H) return 0;
    for (int i = 0; i < t->n; i++) {
        int over = (i == t->sel) ? 2 : 0;   /* the selected tab's overhang */
        if (b->x >= t->tab[i].x - over && b->x < t->tab[i].x + t->tab[i].w + over) {
            if (i != t->sel) {
                t->sel = i;
                if (t->on_change) t->on_change(t->user, i);
            }
            return 1;
        }
    }
    return 0;
}

int w2k_tabs_key(W2kTabs *t, XKeyEvent *k)
{
    if (!(k->state & ControlMask)) return 0;
    KeySym ks = XLookupKeysym(k, 0);
    if (ks != XK_Tab && ks != XK_Next && ks != XK_Prior) return 0;
    int dir = (ks == XK_Prior || (k->state & ShiftMask)) ? -1 : 1;
    t->sel = (t->sel + dir + t->n) % t->n;
    if (t->on_change) t->on_change(t->user, t->sel);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Progress bar: the Windows 2000 control -- a sunken well with blocks of
 * highlight colour, each two thirds as wide as the well is tall, two
 * pixels apart. `percent` below zero draws a marquee: three blocks going
 * round, `phase` steps along.
 * ------------------------------------------------------------------ */
void w2k_draw_progress(Drawable d, const W2kRect *r, int percent, int phase)
{
    w2k_fill(d, r->x, r->y, r->w, r->h, C_FACE);
    w2k_edge(d, r->x, r->y, r->w, r->h, EDGE_SUNKEN_THIN, BF_RECT);
    int ix = r->x + 2, iy = r->y + 2, iw = r->w - 4, ih = r->h - 4;
    if (iw <= 0 || ih <= 0) return;
    int bw = ih * 2 / 3;
    if (bw < 4) bw = 4;
    int step = bw + 2, nblocks = (iw + 2) / step;
    if (nblocks < 1) nblocks = 1;
    int lit_from = 0, lit_to;
    if (percent < 0) {
        lit_from = phase % (nblocks + 3) - 3;
        lit_to = lit_from + 3;
    } else {
        if (percent > 100) percent = 100;
        lit_to = (nblocks * percent + 50) / 100;
    }
    for (int i = 0; i < nblocks; i++) {
        if (i < lit_from || i >= lit_to) continue;
        int x = ix + i * step, w = bw;
        if (x + w > ix + iw) w = ix + iw - x;
        if (w > 0) w2k_fill(d, x, iy, w, ih, C_HIGHLIGHT);
    }
}
