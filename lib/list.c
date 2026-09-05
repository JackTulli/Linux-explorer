/* list.c -- the list view (report / list / icon) and the tree view. */
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define HDR_H      17
#define ROW_H      17
#define ICON_CW    75      /* icon-view cell: the shell's 75 x 75 spacing */
#define ICON_CH    75
#define LIST_CW   150      /* list-view column */
#define TREE_INDENT 19
#define BOX        9       /* the [+] / [-] box */


/* ------------------------------------------------------------------ *
 * List view
 * ------------------------------------------------------------------ */
W2kList *w2k_list_new(int mode)
{
    W2kList *l = w2k_alloc(sizeof *l);
    l->mode = mode;
    l->sel = -1;
    l->anchor = -1;
    l->defer_single = -1;
    l->row_h = ROW_H;
    l->hdr_h = HDR_H;
    l->drag_col = -1;
    l->vsb.vertical = 1;
    l->vsb.line = 1;
    l->hsb.vertical = 0;
    l->hsb.line = 16;
    return l;
}

void w2k_list_clear(W2kList *l)
{
    for (int i = 0; i < l->n; i++)
        for (int c = 0; c < LIST_MAXCOL; c++) free(l->items[i].text[c]);
    l->n = 0;
    l->sel = -1;
    l->top = 0;
    l->vsb.pos = l->hsb.pos = 0;
}

void w2k_list_free(W2kList *l)
{
    if (!l) return;
    w2k_scroll_release(&l->vsb);         /* a held arrow's timer dies with it */
    w2k_list_clear(l);
    for (int c = 0; c < l->ncols; c++) free(l->col[c].title);
    free(l->items);
    free(l);
}

void w2k_list_add_col(W2kList *l, const char *title, int w, int right)
{
    if (l->ncols >= LIST_MAXCOL) return;
    l->col[l->ncols].title = w2k_strdup(title);
    l->col[l->ncols].w = w;
    l->col[l->ncols].right = right;
    l->ncols++;
}

int w2k_list_add(W2kList *l, int icon, void *data)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        W2kListItem *grown = realloc(l->items, (size_t)cap * sizeof *l->items);
        if (!grown) return -1;
        l->items = grown;
        l->cap = cap;
        if (!l->items) abort();
    }
    W2kListItem *it = &l->items[l->n];
    memset(it, 0, sizeof *it);
    it->icon = icon;
    it->data = data;
    return l->n++;
}

void w2k_list_set(W2kList *l, int row, int col, const char *text)
{
    if (row < 0 || row >= l->n || col < 0 || col >= LIST_MAXCOL) return;
    free(l->items[row].text[col]);
    l->items[row].text[col] = w2k_strdup(text ? text : "");
}

/* Interior of the control, excluding edge, header and scrollbars. */
static void view_rect(W2kList *l, W2kRect *v, int *vs, int *hs)
{
    int hdr = (l->mode == LV_REPORT) ? l->hdr_h : 0;
    *vs = 0;
    *hs = 0;

    /* Two passes: each bar can force the other to appear. */
    for (int pass = 0; pass < 2; pass++) {
        int w = l->r.w - 4 - *vs, h = l->r.h - 4 - hdr - *hs;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (l->mode == LV_REPORT) {
            int total = 0;
            for (int c = 0; c < l->ncols; c++) total += l->col[c].w;
            *hs = (total > w) ? SCROLL_W : 0;
            *vs = (l->n * l->row_h > h) ? SCROLL_W : 0;
        } else if (l->mode == LV_LIST) {
            int per = h / l->row_h;
            if (per < 1) per = 1;
            int cols = (l->n + per - 1) / per;
            *hs = (cols * LIST_CW > w) ? SCROLL_W : 0;
            *vs = 0;
        } else {
            int per = w / ICON_CW;
            if (per < 1) per = 1;
            int rows = (l->n + per - 1) / per;
            *vs = (rows * ICON_CH > h) ? SCROLL_W : 0;
            *hs = 0;
        }
    }
    v->x = l->r.x + 2;
    v->y = l->r.y + 2 + hdr;
    v->w = l->r.w - 4 - *vs;
    v->h = l->r.h - 4 - hdr - *hs;
    if (v->w < 1) v->w = 1;
    if (v->h < 1) v->h = 1;
}

void w2k_list_layout(W2kList *l)
{
    W2kRect v;
    int vs, hs;
    view_rect(l, &v, &vs, &hs);

    if (l->mode == LV_REPORT) {
        int total = 0;
        for (int c = 0; c < l->ncols; c++) total += l->col[c].w;
        l->vsb.page = v.h / l->row_h;
        l->vsb.total = l->n;
        l->hsb.page = v.w;
        l->hsb.total = total;
        l->hsb.line = 16;
    } else if (l->mode == LV_LIST) {
        int per = v.h / l->row_h;
        if (per < 1) per = 1;
        int cols = (l->n + per - 1) / per;
        l->hsb.page = v.w;
        l->hsb.total = cols * LIST_CW;
        l->hsb.line = LIST_CW;
        l->vsb.page = l->vsb.total = 1;
        l->vsb.pos = 0;
    } else {
        int per = v.w / ICON_CW;
        if (per < 1) per = 1;
        int rows = (l->n + per - 1) / per;
        l->vsb.page = v.h / ICON_CH;
        if (l->vsb.page < 1) l->vsb.page = 1;
        l->vsb.total = rows;
        l->hsb.page = l->hsb.total = 1;
        l->hsb.pos = 0;
    }
    if (l->vsb.page < 1) l->vsb.page = 1;
    w2k_scroll_clamp(&l->vsb);
    w2k_scroll_clamp(&l->hsb);

    l->vsb.r = (W2kRect){ l->r.x + l->r.w - 2 - SCROLL_W, l->r.y + 2,
                          SCROLL_W, l->r.h - 4 - hs };
    l->hsb.r = (W2kRect){ l->r.x + 2, l->r.y + l->r.h - 2 - SCROLL_W,
                          l->r.w - 4 - vs, SCROLL_W };
    l->top = l->vsb.pos;
    l->scroll_x = l->hsb.pos;
}

void w2k_list_ensure_visible(W2kList *l, int idx)
{
    if (idx < 0 || idx >= l->n) return;
    w2k_list_layout(l);
    if (l->mode == LV_REPORT) {
        if (idx < l->vsb.pos) l->vsb.pos = idx;
        if (idx >= l->vsb.pos + l->vsb.page) l->vsb.pos = idx - l->vsb.page + 1;
    } else if (l->mode == LV_ICON) {
        W2kRect v;
        int vs, hs;
        view_rect(l, &v, &vs, &hs);
        int per = v.w / ICON_CW;
        if (per < 1) per = 1;
        int row = idx / per;
        if (row < l->vsb.pos) l->vsb.pos = row;
        if (row >= l->vsb.pos + l->vsb.page) l->vsb.pos = row - l->vsb.page + 1;
    }
    w2k_scroll_clamp(&l->vsb);
    l->top = l->vsb.pos;
}

static void draw_header(Drawable d, W2kList *l, W2kRect *v)
{
    int x = l->r.x + 2 - l->scroll_x;
    int y = l->r.y + 2;
    int fh = w2k_font_height(F_UI);

    w2k_clip_set(l->r.x + 2, y, v->w, l->hdr_h);

    for (int c = 0; c < l->ncols; c++) {
        int w = l->col[c].w;
        w2k_button(d, x, y, w, l->hdr_h, 0);
        char buf[80];
        w2k_ellipsis(F_UI, l->col[c].title, w - 10, buf, sizeof buf);
        int tw = w2k_text_width(F_UI, buf, -1);
        int tx = l->col[c].right ? x + w - 6 - tw : x + 6;
        w2k_text(d, F_UI, tx, y + (l->hdr_h - fh) / 2, buf, C_TEXT);
        x += w;
    }
    /* Filler past the last column. */
    if (x < l->r.x + 2 + v->w)
        w2k_button(d, x, y, l->r.x + 2 + v->w - x, l->hdr_h, 0);
    w2k_clip_clear();
}

/* Wrap a label into at most two centred lines for icon view. */
static void icon_label(Drawable d, int cx, int y, const char *text, int sel,
                       int focused)
{
    char l1[64] = { 0 }, l2[64] = { 0 };
    int maxw = ICON_CW - 8;
    int n = 1;
    if (w2k_text_width(F_UI, text, -1) > maxw) {
        /* Break at the last space that still fits, measuring at the
         * spaces only: measuring every prefix made a folder of a hundred
         * long names cost thousands of text measurements per repaint. */
        int cut = -1;
        for (int i = 0; text[i]; i++)
            if (text[i] == ' ' && w2k_text_width(F_UI, text, i) <= maxw) cut = i;
        if (cut < 1) {
            /* One long word: binary search for the widest prefix. */
            int lo = 1, hi = (int)strlen(text);
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (w2k_text_width(F_UI, text, mid) <= maxw) lo = mid; else hi = mid - 1;
            }
            cut = lo;
        }
        snprintf(l1, sizeof l1, "%.*s", cut, text);
        char rest[64];
        snprintf(rest, sizeof rest, "%s", text + cut + (text[cut] == ' ' ? 1 : 0));
        w2k_ellipsis(F_UI, rest, maxw, l2, sizeof l2);
        n = 2;
    } else {
        snprintf(l1, sizeof l1, "%s", text);
    }
    const char *lines[2] = { l1, l2 };
    /* Icon labels are set 13 pixels apart, as the shell sets Tahoma 8. */
    int fh = w2k_font_height(F_UI);
    if (fh > 13) fh = 13;
    for (int i = 0; i < n; i++) {
        int tw = w2k_text_width(F_UI, lines[i], -1);
        int tx = cx - tw / 2, ty = y + i * fh;
        if (sel) w2k_fill(d, tx - 2, ty, tw + 4, fh, C_HIGHLIGHT);
        w2k_text(d, F_UI, tx, ty, lines[i], sel ? C_HIGHLIGHTTEXT : C_WINDOWTEXT);
        if (sel && focused && i == n - 1)
            w2k_focus_rect(d, tx - 2, y, tw + 4, fh * n);
    }
}

void w2k_list_draw(Drawable d, W2kList *l)
{
    w2k_list_layout(l);
    W2kRect v;
    int vs, hs;
    view_rect(l, &v, &vs, &hs);

    w2k_edge(d, l->r.x, l->r.y, l->r.w, l->r.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, l->r.x + 2, l->r.y + 2, l->r.w - 4, l->r.h - 4, C_WINDOW);

    if (l->mode == LV_REPORT) draw_header(d, l, &v);

    w2k_clip_set(v.x, v.y, v.w, v.h);

    int fh = w2k_font_height(F_UI);

    if (l->mode == LV_REPORT) {
        for (int i = l->top; i < l->n && (i - l->top) * l->row_h < v.h; i++) {
            int y = v.y + (i - l->top) * l->row_h;
            int sel = l->items[i].selected || i == l->sel;
            int x = v.x - l->scroll_x;

            if (sel && l->fullrow)
                w2k_fill(d, v.x, y, v.w, l->row_h, C_HIGHLIGHT);

            for (int c = 0; c < l->ncols; c++) {
                int w = l->col[c].w;
                const char *txt = l->items[i].text[c] ? l->items[i].text[c] : "";
                int tx = x + 4;
                if (c == 0 && l->checkboxes) {
                    /* A check box in front of the first column, as a list
                     * view with LVS_EX_CHECKBOXES draws it. */
                    w2k_draw_checkbox(d, x + 3, y + (l->row_h - 13) / 2, "",
                                      l->items[i].checked, 0,
                                      l->items[i].data == (void *)-1);
                    tx = x + 21;
                }
                if (c == 0 && l->items[i].icon >= 0) {
                    if (l->items[i].link)
                        w2k_icon_draw_link(d, tx - 2, y + (l->row_h - 16) / 2,
                                           l->items[i].icon);
                    else
                        w2k_icon_draw(d, tx - 2, y + (l->row_h - 16) / 2,
                                      l->items[i].icon);
                    tx += 18;
                }
                char buf[200];
                w2k_ellipsis(F_UI, txt, w - (tx - x) - 6, buf, sizeof buf);
                int tw = w2k_text_width(F_UI, buf, -1);
                if (l->col[c].right) tx = x + w - 6 - tw;

                if (sel && !l->fullrow && c == 0) {
                    w2k_fill(d, tx - 2, y, tw + 4, l->row_h, C_HIGHLIGHT);
                    if (l->focused)
                        w2k_focus_rect(d, tx - 2, y, tw + 4, l->row_h);
                }
                int greyed = l->checkboxes && l->items[i].data == (void *)-1;
                w2k_text(d, F_UI, tx, y + (l->row_h - fh) / 2, buf,
                         greyed ? C_GRAYTEXT :
                         sel && (l->fullrow || c == 0) ? C_HIGHLIGHTTEXT
                                                       : C_WINDOWTEXT);
                x += w;
            }
            if (sel && l->fullrow && l->focused)
                w2k_focus_rect(d, v.x, y, v.w, l->row_h);
        }
    } else if (l->mode == LV_LIST) {
        int per = v.h / l->row_h;
        if (per < 1) per = 1;
        for (int i = 0; i < l->n; i++) {
            int cx = v.x + (i / per) * LIST_CW - l->scroll_x;
            int y = v.y + (i % per) * l->row_h;
            if (cx > v.x + v.w || cx + LIST_CW < v.x) continue;
            int sel = l->items[i].selected || i == l->sel;
            if (l->items[i].icon >= 0) {
                int iy = y + (l->row_h - 16) / 2;
                if (l->items[i].link)
                    w2k_icon_draw_link(d, cx + 2, iy, l->items[i].icon);
                else
                    w2k_icon_draw(d, cx + 2, iy, l->items[i].icon);
            }
            const char *txt = l->items[i].text[0] ? l->items[i].text[0] : "";
            char buf[200];
            w2k_ellipsis(F_UI, txt, LIST_CW - 26, buf, sizeof buf);
            int tw = w2k_text_width(F_UI, buf, -1);
            if (sel) w2k_fill(d, cx + 19, y, tw + 4, l->row_h, C_HIGHLIGHT);
            w2k_text(d, F_UI, cx + 21, y + (l->row_h - fh) / 2, buf,
                     sel ? C_HIGHLIGHTTEXT : C_WINDOWTEXT);
            if (sel && l->focused) w2k_focus_rect(d, cx + 19, y, tw + 4, l->row_h);
        }
    } else {
        int per = v.w / ICON_CW;
        if (per < 1) per = 1;
        for (int i = 0; i < l->n; i++) {
            int row = i / per;
            int y = v.y + (row - l->top) * ICON_CH;
            if (y + ICON_CH < v.y || y > v.y + v.h) continue;
            int cx = v.x + (i % per) * ICON_CW + ICON_CW / 2;
            int sel = l->items[i].selected || i == l->sel;
            /* Measured off Windows 2000: the icon four pixels below the
             * cell's top, the label's first line 40 below it. */
            if (l->items[i].icon >= 0) {
                if (l->items[i].link)
                    w2k_bigicon_draw_link(d, cx - 16, y + 4, l->items[i].icon);
                else
                    w2k_bigicon_draw(d, cx - 16, y + 4, l->items[i].icon);
            }
            icon_label(d, cx, y + 40, l->items[i].text[0] ?
                       l->items[i].text[0] : "", sel, l->focused);
        }
    }
    /* The rubber band goes on top of the items, as a dotted outline. */
    if (l->band_on) {
        int x0 = l->band_x0 < l->band_x1 ? l->band_x0 : l->band_x1;
        int y0 = l->band_y0 < l->band_y1 ? l->band_y0 : l->band_y1;
        int bw = l->band_x0 < l->band_x1 ? l->band_x1 - l->band_x0
                                         : l->band_x0 - l->band_x1;
        int bh = l->band_y0 < l->band_y1 ? l->band_y1 - l->band_y0
                                         : l->band_y0 - l->band_y1;
        if (bw > 1 && bh > 1) {
            if (w2k_effects[FX_TRANSLUCENT_SEL]) {
                XSetForeground(w2k.dpy, w2k.gc_dither, w2k.col[C_HIGHLIGHT]);
                XSetTSOrigin(w2k.dpy, w2k.gc_dither, 0, 0);
                XFillRectangle(w2k.dpy, d, w2k.gc_dither, x0, y0, bw, bh);
                w2k_frame(d, x0, y0, bw, bh, C_HIGHLIGHT);
            } else {
                w2k_focus_rect(d, x0, y0, bw, bh);
            }
        }
    }
    w2k_clip_clear();

    if (vs) w2k_scroll_draw(d, &l->vsb);
    if (hs) w2k_scroll_draw(d, &l->hsb);
    if (vs && hs)
        w2k_fill(d, l->hsb.r.x + l->hsb.r.w, l->vsb.r.y + l->vsb.r.h,
                 SCROLL_W, SCROLL_W, C_FACE);
}

/* Where item `i` is drawn, in window coordinates. The inverse of
 * w2k_list_hit(), and what the rubber band tests against. */
static int item_rect(W2kList *l, int i, W2kRect *out)
{
    W2kRect v;
    int vs, hs;
    view_rect(l, &v, &vs, &hs);
    if (i < 0 || i >= l->n) return 0;

    if (l->mode == LV_REPORT) {
        int total = 0;
        for (int c = 0; c < l->ncols; c++) total += l->col[c].w;
        *out = (W2kRect){ v.x - l->scroll_x, v.y + (i - l->top) * l->row_h,
                          total ? total : v.w, l->row_h };
        return 1;
    }
    if (l->mode == LV_LIST) {
        int per = v.h / l->row_h;
        if (per < 1) per = 1;
        *out = (W2kRect){ v.x + (i / per) * LIST_CW - l->scroll_x,
                          v.y + (i % per) * l->row_h, LIST_CW, l->row_h };
        return 1;
    }
    int per = v.w / ICON_CW;
    if (per < 1) per = 1;
    *out = (W2kRect){ v.x + (i % per) * ICON_CW,
                      v.y + (i / per - l->top) * ICON_CH, ICON_CW, ICON_CH };
    return 1;
}

/* Do two rectangles overlap? */
static int rect_overlap(const W2kRect *a, const W2kRect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

/* Select everything the band covers. */
static void band_select(W2kList *l)
{
    W2kRect band = {
        l->band_x0 < l->band_x1 ? l->band_x0 : l->band_x1,
        l->band_y0 < l->band_y1 ? l->band_y0 : l->band_y1,
        l->band_x0 < l->band_x1 ? l->band_x1 - l->band_x0 : l->band_x0 - l->band_x1,
        l->band_y0 < l->band_y1 ? l->band_y1 - l->band_y0 : l->band_y0 - l->band_y1
    };
    if (band.w < 1) band.w = 1;
    if (band.h < 1) band.h = 1;

    int first = -1;
    for (int i = 0; i < l->n; i++) {
        W2kRect r;
        if (!item_rect(l, i, &r)) continue;
        int in = rect_overlap(&r, &band);
        if (in) {
            l->items[i].selected = 1;
            if (first < 0) first = i;
        } else if (!l->band_add) {
            l->items[i].selected = 0;
        }
    }
    if (first >= 0) l->sel = first;
}

int w2k_list_hit(W2kList *l, int px, int py)
{
    W2kRect v;
    int vs, hs;
    view_rect(l, &v, &vs, &hs);
    if (!w2k_rect_hit(&v, px, py)) return -1;

    if (l->mode == LV_REPORT) {
        int i = l->top + (py - v.y) / l->row_h;
        return (i >= 0 && i < l->n) ? i : -1;
    }
    if (l->mode == LV_LIST) {
        int per = v.h / l->row_h;
        if (per < 1) per = 1;
        int col = (px - v.x + l->scroll_x) / LIST_CW;
        int row = (py - v.y) / l->row_h;
        int i = col * per + row;
        return (i >= 0 && i < l->n) ? i : -1;
    }
    int per = v.w / ICON_CW;
    if (per < 1) per = 1;
    int col = (px - v.x) / ICON_CW;
    int row = l->top + (py - v.y) / ICON_CH;
    if (col >= per) return -1;
    int i = row * per + col;
    return (i >= 0 && i < l->n) ? i : -1;
}

/* Column dividers in the header, for resizing. */
static int divider_at(W2kList *l, int px, int py)
{
    if (l->mode != LV_REPORT) return -1;
    if (py < l->r.y + 2 || py >= l->r.y + 2 + l->hdr_h) return -1;
    int x = l->r.x + 2 - l->scroll_x;
    for (int c = 0; c < l->ncols; c++) {
        x += l->col[c].w;
        if (px >= x - 4 && px <= x + 2) return c;
    }
    return -1;
}

int w2k_list_press(W2kList *l, XButtonEvent *b)
{
    w2k_list_layout(l);

    if (b->button == Button4 || b->button == Button5) {
        W2kScroll *s = (l->mode == LV_LIST) ? &l->hsb : &l->vsb;
        int before = s->pos;
        int r = w2k_scroll_wheel(s, b->button == Button4 ? -1 : 1);

        /* "Smooth-scroll list boxes": walk to the new position over a few
         * frames instead of jumping, repainting the owning window as we
         * go. Only worth it when the jump is more than a line. */
        if (r && w2k_effects[FX_SMOOTH_SCROLL] && s->owner &&
            abs(s->pos - before) > 1) {
            int target = s->pos;
            for (int step = 1; step < 4; step++) {
                s->pos = before + (target - before) * step / 4;
                w2k_win_repaint_now(s->owner);
                usleep(7000);
            }
            s->pos = target;
        }
        return r;
    }
    if (w2k_scroll_needed(&l->vsb) && w2k_rect_hit(&l->vsb.r, b->x, b->y))
        return w2k_scroll_press(&l->vsb, b->x, b->y) | 1;
    if (w2k_scroll_needed(&l->hsb) && w2k_rect_hit(&l->hsb.r, b->x, b->y))
        return w2k_scroll_press(&l->hsb, b->x, b->y) | 1;
    if (!w2k_rect_hit(&l->r, b->x, b->y)) return 0;

    if (b->button == Button1) {
        int dc = divider_at(l, b->x, b->y);
        if (dc >= 0) {
            l->drag_col = dc;
            l->drag_x = b->x;
            return 1;
        }
        if (l->mode == LV_REPORT && b->y < l->r.y + 2 + l->hdr_h) {
            int x = l->r.x + 2 - l->scroll_x;
            for (int c = 0; c < l->ncols; c++) {
                if (b->x >= x && b->x < x + l->col[c].w) {
                    if (l->on_sort) l->on_sort(l->user, c);
                    return 1;
                }
                x += l->col[c].w;
            }
            return 1;
        }
    }
    l->focused = 1;
    l->click_pending = 0;
    int i = w2k_list_hit(l, b->x, b->y);

    /* The check box is its own hit target: clicking it toggles without
     * disturbing the selection, exactly as in Windows. */
    if (l->checkboxes && i >= 0 && b->button == Button1) {
        W2kRect v;
        int vs, hs;
        view_rect(l, &v, &vs, &hs);
        int bx = v.x - l->scroll_x + 3;
        if (b->x >= bx && b->x < bx + 15 &&
            l->items[i].data != (void *)-1) {
            l->items[i].checked = !l->items[i].checked;
            if (l->on_check) l->on_check(l->user, i);
            return 1;
        }
    }

    if (b->button == Button1 || b->button == Button3) {
        if (i >= 0) {
            static Time last;
            static int lasti = -1;
            int dbl = (i == lasti && (int)(b->time - last) < w2k_dblclk_ms &&
                       b->button == Button1);

            l->defer_single = -1;
            if (l->multisel && (b->state & ShiftMask)) {
                /* Shift: everything from the anchor to here. With Ctrl as
                 * well the range is added to what is selected. */
                int a = l->anchor >= 0 && l->anchor < l->n ? l->anchor : i;
                if (!(b->state & ControlMask))
                    for (int k = 0; k < l->n; k++) l->items[k].selected = 0;
                for (int k = a < i ? a : i; k <= (a < i ? i : a); k++)
                    l->items[k].selected = 1;
            } else if (l->multisel && (b->state & ControlMask)) {
                l->items[i].selected = !l->items[i].selected;
                l->anchor = i;
            } else if (l->items[i].selected && b->button == Button3) {
                /* Right-clicking one of several selected items keeps the
                 * lot, so the menu applies to all of them. */
            } else if (l->items[i].selected && l->multisel && !dbl) {
                /* A press on something already selected may be the start
                 * of dragging the whole selection; it becomes a plain
                 * select-this-alone only if the button comes up in place. */
                l->defer_single = i;
                l->anchor = i;
            } else {
                for (int k = 0; k < l->n; k++) l->items[k].selected = 0;
                l->items[i].selected = 1;
                l->anchor = i;
            }
            l->sel = i;
            if (l->on_select) l->on_select(l->user, i);

            if (dbl) {
                last = 0;
                lasti = -1;
                if (l->on_activate) l->on_activate(l->user, i);
            } else {
                last = b->time;
                lasti = i;
                /* Folder Options can put the view into single-click mode.
                 * The item opens when the button comes back up over it --
                 * pressing alone must stay a selection, or dragging a file
                 * would open it on the way out. */
                if (l->singleclick && b->button == Button1 &&
                    !(b->state & (ControlMask | ShiftMask))) {
                    l->click_pending = 1;
                    l->click_item = i;
                    l->click_x = b->x;
                    l->click_y = b->y;
                }
            }
        } else if (b->button == Button1) {
            /* Empty space: start a rubber band. Holding Ctrl adds to what is
             * already selected instead of replacing it. */
            l->band_add = l->multisel && (b->state & ControlMask);
            if (!l->band_add)
                for (int k = 0; k < l->n; k++) l->items[k].selected = 0;
            l->band_on = 1;
            l->band_x0 = l->band_x1 = b->x;
            l->band_y0 = l->band_y1 = b->y;
            if (!l->band_add) {
                l->sel = -1;
                if (l->on_select) l->on_select(l->user, -1);
            }
        }
        return 1;
    }
    return 1;
}

int w2k_list_motion(W2kList *l, XMotionEvent *m)
{
    if (l->click_pending &&
        (abs(m->x - l->click_x) > 4 || abs(m->y - l->click_y) > 4))
        l->click_pending = 0;          /* this is a drag, not a click */

    if (l->band_on) {
        l->band_x1 = m->x;
        l->band_y1 = m->y;
        if (l->multisel) band_select(l);
        return 1;
    }
    if (l->drag_col >= 0) {
        int nw = l->col[l->drag_col].w + (m->x - l->drag_x);
        if (nw < 24) nw = 24;
        l->col[l->drag_col].w = nw;
        l->drag_x = m->x;
        return 1;
    }
    if (l->vsb.pressed) return w2k_scroll_motion(&l->vsb, m->x, m->y);
    if (l->hsb.pressed) return w2k_scroll_motion(&l->hsb, m->x, m->y);
    return 0;
}

void w2k_list_release(W2kList *l, XButtonEvent *b)
{
    /* The press on a selected item that did not turn into a drag: now it
     * selects that item alone. A NULL event means a drag happened. */
    if (l->defer_single >= 0) {
        int i = l->defer_single;
        l->defer_single = -1;
        if (b && i < l->n) {
            for (int k = 0; k < l->n; k++) l->items[k].selected = (k == i);
            l->sel = i;
            if (l->on_select) l->on_select(l->user, i);
        }
    }
    if (l->click_pending) {
        int i = l->click_item;
        l->click_pending = 0;
        if (b && w2k_list_hit(l, b->x, b->y) == i &&
            abs(b->x - l->click_x) <= 4 && abs(b->y - l->click_y) <= 4 &&
            l->on_activate)
            l->on_activate(l->user, i);
    }
    l->band_on = 0;
    l->drag_col = -1;
    w2k_scroll_release(&l->vsb);
    w2k_scroll_release(&l->hsb);
}

int w2k_list_key(W2kList *l, XKeyEvent *k)
{
    if (l->n == 0) return 0;
    KeySym ks = XLookupKeysym(k, 0);

    /* Space toggles the focused row's check box, as in Windows. */
    if (l->checkboxes && ks == XK_space && l->sel >= 0 && l->sel < l->n &&
        l->items[l->sel].data != (void *)-1) {
        l->items[l->sel].checked = !l->items[l->sel].checked;
        if (l->on_check) l->on_check(l->user, l->sel);
        return 1;
    }
    int old = l->sel, i = l->sel;
    W2kRect v;
    int vs, hs;
    view_rect(l, &v, &vs, &hs);

    int stride = 1;
    if (l->mode == LV_ICON) {
        stride = v.w / ICON_CW;
        if (stride < 1) stride = 1;
    } else if (l->mode == LV_LIST) {
        stride = v.h / l->row_h;
        if (stride < 1) stride = 1;
    }

    switch (ks) {
    case XK_Down:  i = (l->mode == LV_ICON) ? i + stride : i + 1; break;
    case XK_Up:    i = (l->mode == LV_ICON) ? i - stride : i - 1; break;
    case XK_Right: i = (l->mode == LV_ICON) ? i + 1 : i + stride; break;
    case XK_Left:  i = (l->mode == LV_ICON) ? i - 1 : i - stride; break;
    case XK_Home:  i = 0; break;
    case XK_End:   i = l->n - 1; break;
    case XK_Next:  i += l->vsb.page; break;
    case XK_Prior: i -= l->vsb.page; break;
    case XK_Return: case XK_KP_Enter:
        if (l->sel >= 0 && l->on_activate) l->on_activate(l->user, l->sel);
        return 1;
    default:
        return 0;
    }
    if (i < 0) i = 0;
    if (i >= l->n) i = l->n - 1;
    if (i != old) {
        for (int c = 0; c < l->n; c++) l->items[c].selected = 0;
        l->items[i].selected = 1;
        l->sel = i;
        w2k_list_ensure_visible(l, i);
        if (l->on_select) l->on_select(l->user, i);
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 * Tree view
 * ------------------------------------------------------------------ */
W2kTree *w2k_tree_new(void)
{
    W2kTree *t = w2k_alloc(sizeof *t);
    t->root = w2k_alloc(sizeof *t->root);
    t->root->expanded = 1;
    t->root->depth = -1;
    t->row_h = ROW_H;
    t->vsb.vertical = 1;
    t->vsb.line = 1;
    return t;
}

static void free_node(W2kTreeNode *n)
{
    while (n) {
        W2kTreeNode *next = n->sibling;
        free_node(n->child);
        free(n->text);
        free(n);
        n = next;
    }
}

void w2k_tree_free(W2kTree *t)
{
    if (!t) return;
    w2k_scroll_release(&t->vsb);
    free_node(t->root->child);
    free(t->root);
    free(t);
}

void w2k_tree_clear_children(W2kTree *t, W2kTreeNode *n)
{
    if (!n) n = t->root;
    if (t->sel) {
        /* Do not leave the selection dangling inside the freed subtree. */
        for (W2kTreeNode *p = t->sel; p; p = p->parent)
            if (p == n) { t->sel = n; break; }
    }
    free_node(n->child);
    n->child = NULL;
}

W2kTreeNode *w2k_tree_add(W2kTree *t, W2kTreeNode *parent, const char *text,
                          int icon, int icon_open, void *data)
{
    if (!parent) parent = t->root;
    W2kTreeNode *n = w2k_alloc(sizeof *n);
    n->text = w2k_strdup(text);
    n->icon = icon;
    n->icon_open = icon_open >= 0 ? icon_open : icon;
    n->data = data;
    n->parent = parent;
    n->depth = parent->depth + 1;

    W2kTreeNode **p = &parent->child;
    while (*p) p = &(*p)->sibling;
    *p = n;
    return n;
}

void w2k_tree_select(W2kTree *t, W2kTreeNode *n) { t->sel = n; }

/* Depth-first walk over the currently visible rows. */
static W2kTreeNode *tree_next_visible(W2kTreeNode *n)
{
    if (!n) return NULL;
    if (n->expanded && n->child) return n->child;
    while (n) {
        if (n->sibling) return n->sibling;
        n = n->parent;
    }
    return NULL;
}

static int tree_count(W2kTree *t)
{
    int n = 0;
    for (W2kTreeNode *p = t->root->child; p; p = tree_next_visible(p)) n++;
    return n;
}

void w2k_tree_layout(W2kTree *t)
{
    int hs = 0;
    t->vsb.total = tree_count(t);
    t->vsb.page = (t->r.h - 4 - hs) / t->row_h;
    if (t->vsb.page < 1) t->vsb.page = 1;
    w2k_scroll_clamp(&t->vsb);
    t->vsb.r = (W2kRect){ t->r.x + t->r.w - 2 - SCROLL_W, t->r.y + 2,
                          SCROLL_W, t->r.h - 4 };
    t->top = t->vsb.pos;
}

/* The [+] / [-] box, and the dotted lines that join siblings. */
static void draw_box(Drawable d, int x, int y, int expanded)
{
    w2k_fill(d, x, y, BOX, BOX, C_WINDOW);
    w2k_frame(d, x, y, BOX, BOX, C_SHADOW);
    w2k_hline(d, x + 2, y + BOX / 2, BOX - 4, C_WINDOWTEXT);
    if (!expanded) w2k_vline(d, x + BOX / 2, y + 2, BOX - 4, C_WINDOWTEXT);
}

static void dotted_h(Drawable d, int x, int y, int w, int parity)
{
    for (int i = 0; i < w; i++)
        if (((x + i + parity) & 1) == 0)
            w2k_fill(d, x + i, y, 1, 1, C_SHADOW);
}

static void dotted_v(Drawable d, int x, int y, int h, int parity)
{
    for (int i = 0; i < h; i++)
        if (((y + i + parity) & 1) == 0)
            w2k_fill(d, x, y + i, 1, 1, C_SHADOW);
}

void w2k_tree_draw(Drawable d, W2kTree *t)
{
    w2k_tree_layout(t);
    int vs = w2k_scroll_needed(&t->vsb) ? SCROLL_W : 0;

    w2k_edge(d, t->r.x, t->r.y, t->r.w, t->r.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, t->r.x + 2, t->r.y + 2, t->r.w - 4, t->r.h - 4, C_WINDOW);

    W2kRect v = { t->r.x + 2, t->r.y + 2, t->r.w - 4 - vs, t->r.h - 4 };
    w2k_clip_set(v.x, v.y, v.w, v.h);

    int fh = w2k_font_height(F_UI);
    int idx = 0;
    for (W2kTreeNode *n = t->root->child; n; n = tree_next_visible(n), idx++) {
        if (idx < t->top) continue;
        int y = v.y + (idx - t->top) * t->row_h;
        if (y > v.y + v.h) break;

        int ind = v.x + 2 + n->depth * TREE_INDENT;
        int cy = y + t->row_h / 2;

        /* Connector from the parent's vertical rail to this row. */
        dotted_h(d, ind + BOX / 2, cy, TREE_INDENT - BOX / 2 - 1, 0);
        int has_next = n->sibling != NULL;
        dotted_v(d, ind + BOX / 2, y, has_next ? t->row_h : cy - y + 1, 0);
        /* Rails for the ancestors that still have siblings below. */
        for (W2kTreeNode *a = n->parent; a && a->depth >= 0; a = a->parent)
            if (a->sibling)
                dotted_v(d, v.x + 2 + a->depth * TREE_INDENT + BOX / 2, y,
                         t->row_h, 0);

        if (n->child || n->has_kids) draw_box(d, ind, cy - BOX / 2, n->expanded);

        int ix = ind + TREE_INDENT;
        int icon = (n->expanded && n->child) ? n->icon_open : n->icon;
        if (icon >= 0) w2k_icon_draw(d, ix, y + (t->row_h - 16) / 2, icon);

        int tx = ix + 20;
        int tw = w2k_text_width(F_UI, n->text, -1);
        int sel = (n == t->sel);
        if (sel) {
            w2k_fill(d, tx - 2, y + 1, tw + 4, t->row_h - 2, C_HIGHLIGHT);
            if (t->focused) w2k_focus_rect(d, tx - 2, y + 1, tw + 4, t->row_h - 2);
        }
        w2k_text(d, F_UI, tx, y + (t->row_h - fh) / 2, n->text,
                 sel ? C_HIGHLIGHTTEXT : C_WINDOWTEXT);
    }
    w2k_clip_clear();
    if (vs) w2k_scroll_draw(d, &t->vsb);
}

int w2k_tree_press(W2kTree *t, XButtonEvent *b)
{
    w2k_tree_layout(t);
    if (b->button == Button4 || b->button == Button5)
        return w2k_scroll_wheel(&t->vsb, b->button == Button4 ? -1 : 1);
    if (w2k_scroll_needed(&t->vsb) && w2k_rect_hit(&t->vsb.r, b->x, b->y))
        return w2k_scroll_press(&t->vsb, b->x, b->y) | 1;
    if (!w2k_rect_hit(&t->r, b->x, b->y)) return 0;

    t->focused = 1;
    int row = t->top + (b->y - (t->r.y + 2)) / t->row_h;
    int idx = 0;
    for (W2kTreeNode *n = t->root->child; n; n = tree_next_visible(n), idx++) {
        if (idx != row) continue;
        int ind = t->r.x + 4 + n->depth * TREE_INDENT;
        if ((n->child || n->has_kids) && b->x >= ind && b->x < ind + BOX) {
            n->expanded = !n->expanded;
            if (n->expanded && t->on_expand) t->on_expand(t->user, n);
            return 1;
        }
        if (b->x >= ind + TREE_INDENT) {
            t->sel = n;
            if (t->on_select) t->on_select(t->user, n);
            static Time last;
            static W2kTreeNode *lastn;
            if (n == lastn && (int)(b->time - last) < w2k_dblclk_ms) {
                n->expanded = !n->expanded;
                if (n->expanded && t->on_expand) t->on_expand(t->user, n);
                last = 0;
            } else { last = b->time; lastn = n; }
            return 1;
        }
        return 1;
    }
    return 1;
}

int w2k_tree_key(W2kTree *t, XKeyEvent *k)
{
    KeySym ks = XLookupKeysym(k, 0);
    if (!t->sel) {
        if (ks == XK_Down) { t->sel = t->root->child; return t->sel != NULL; }
        return 0;
    }
    W2kTreeNode *n = t->sel;
    switch (ks) {
    case XK_Down: {
        W2kTreeNode *x = tree_next_visible(n);
        if (x) { t->sel = x; if (t->on_select) t->on_select(t->user, x); }
        break;
    }
    case XK_Up: {
        W2kTreeNode *prev = NULL;
        for (W2kTreeNode *p = t->root->child; p && p != n; p = tree_next_visible(p))
            prev = p;
        if (prev) { t->sel = prev; if (t->on_select) t->on_select(t->user, prev); }
        break;
    }
    case XK_Right:
        if ((n->child || n->has_kids) && !n->expanded) {
            n->expanded = 1;
            if (t->on_expand) t->on_expand(t->user, n);
        } else if (n->child) {
            t->sel = n->child;
            if (t->on_select) t->on_select(t->user, t->sel);
        }
        break;
    case XK_Left:
        if (n->expanded && n->child) n->expanded = 0;
        else if (n->parent && n->parent->depth >= 0) {
            t->sel = n->parent;
            if (t->on_select) t->on_select(t->user, t->sel);
        }
        break;
    default:
        return 0;
    }
    /* Keep the selection on screen. */
    int idx = 0;
    for (W2kTreeNode *p = t->root->child; p; p = tree_next_visible(p), idx++)
        if (p == t->sel) break;
    if (idx < t->vsb.pos) t->vsb.pos = idx;
    if (idx >= t->vsb.pos + t->vsb.page) t->vsb.pos = idx - t->vsb.page + 1;
    w2k_scroll_clamp(&t->vsb);
    return 1;
}
