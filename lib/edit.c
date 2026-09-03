/* edit.c -- the edit control: caret, selection, word wrap, scrolling.
 *
 * Text is one flat NUL-terminated buffer. A separate array of "visual line"
 * start offsets is rebuilt whenever the text or the wrap width changes, so
 * wrapped and unwrapped modes share a single rendering path. */
#include "w2kui.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define PAD_X 2          /* text inset inside the sunken well */
#define PAD_Y 1
#define TABSTOP 8

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }

/* ------------------------------------------------------------------ *
 * Buffer
 * ------------------------------------------------------------------ */
static void ensure_cap(W2kEdit *e, int need)
{
    if (need + 1 <= e->cap) return;
    int cap = e->cap ? e->cap : 256;
    while (cap < need + 1) cap *= 2;
    e->text = realloc(e->text, cap);
    if (!e->text) abort();
    e->cap = cap;
}

W2kEdit *w2k_edit_new(int multiline)
{
    W2kEdit *e = w2k_alloc(sizeof *e);
    e->multiline = multiline;
    e->font = multiline ? F_FIXED : F_UI;
    e->vsb.vertical = 1;
    e->vsb.line = 1;
    e->hsb.vertical = 0;
    e->hsb.line = 8;
    ensure_cap(e, 0);
    e->text[0] = 0;
    e->layout_w = -1;
    return e;
}

void w2k_edit_bind(W2kEdit *e, W2kWin *w)
{
    e->owner = w;
    w2k_scroll_bind(&e->vsb, w);
    w2k_scroll_bind(&e->hsb, w);
}

void w2k_edit_free(W2kEdit *e)
{
    if (!e) return;
    free(e->text);
    free(e->vls);
    free(e);
}

const char *w2k_edit_text(W2kEdit *e) { return e->text ? e->text : ""; }

/* ------------------------------------------------------------------ *
 * Measuring, with tab expansion
 * ------------------------------------------------------------------ */
static int tab_width(W2kEdit *e)
{
    return TABSTOP * w2k_text_width(e->font, "n", 1);
}

/* Pixel width of text[from .. from+n). */
static int measure(W2kEdit *e, int from, int n)
{
    int x = 0, tw = tab_width(e), run = 0;
    for (int i = 0; i < n; i++) {
        char c = e->text[from + i];
        if (c == '\t') {
            if (run) { x += w2k_text_width(e->font, e->text + from + i - run, run); run = 0; }
            x = ((x / tw) + 1) * tw;
        } else if (c == '\n') {
            break;
        } else run++;
    }
    if (run) x += w2k_text_width(e->font, e->text + from + n - run, run);
    return x;
}

/* How many characters of the line starting at `from` fit in `px` pixels. */
static int chars_for_width(W2kEdit *e, int from, int maxn, int px)
{
    int lo = 0, hi = maxn;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (measure(e, from, mid) <= px) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* ------------------------------------------------------------------ *
 * Visual lines
 * ------------------------------------------------------------------ */
static void vl_push(W2kEdit *e, int off)
{
    if (e->nvl == e->vlcap) {
        e->vlcap = e->vlcap ? e->vlcap * 2 : 128;
        e->vls = realloc(e->vls, e->vlcap * sizeof *e->vls);
        if (!e->vls) abort();
    }
    e->vls[e->nvl++] = off;
}

static int text_area_w(W2kEdit *e)
{
    int w = e->r.w - 4 - 2 * PAD_X;
    if (e->multiline && w2k_scroll_needed(&e->vsb)) w -= SCROLL_W;
    return w > 8 ? w : 8;
}

static void rebuild_lines(W2kEdit *e)
{
    e->nvl = 0;
    if (!e->multiline) { vl_push(e, 0); return; }

    int wrapw = text_area_w(e);
    int i = 0;
    while (i <= e->len) {
        vl_push(e, i);
        int eol = i;
        while (eol < e->len && e->text[eol] != '\n') eol++;

        if (!e->wrap) {
            i = (eol < e->len) ? eol + 1 : e->len + 1;
            continue;
        }
        int seg = eol - i;
        if (measure(e, i, seg) <= wrapw) {
            i = (eol < e->len) ? eol + 1 : e->len + 1;
            continue;
        }
        int fit = chars_for_width(e, i, seg, wrapw);
        if (fit < 1) fit = 1;
        /* Break at the last space that still fits, as Notepad does. */
        int brk = fit;
        for (int k = fit; k > 0; k--)
            if (e->text[i + k - 1] == ' ' || e->text[i + k - 1] == '\t') { brk = k; break; }
        i += brk;
    }
    if (e->nvl == 0) vl_push(e, 0);
}

static int vl_end(W2kEdit *e, int row)
{
    if (row + 1 < e->nvl) {
        int end = e->vls[row + 1];
        if (end > 0 && end <= e->len && e->text[end - 1] == '\n') end--;
        return end;
    }
    return e->len;
}

static int row_for_offset(W2kEdit *e, int off)
{
    int lo = 0, hi = e->nvl - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (e->vls[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

int w2k_edit_line_count(W2kEdit *e)
{
    int n = 1;
    for (int i = 0; i < e->len; i++) if (e->text[i] == '\n') n++;
    return n;
}

void w2k_edit_caret_rowcol(W2kEdit *e, int *row, int *col)
{
    int r = 1, last = 0;
    for (int i = 0; i < e->caret && i < e->len; i++)
        if (e->text[i] == '\n') { r++; last = i + 1; }
    *row = r;
    *col = e->caret - last + 1;
}

/* ------------------------------------------------------------------ *
 * Layout
 * ------------------------------------------------------------------ */
static int line_h(W2kEdit *e) { return w2k_font_height(e->font); }

void w2k_edit_layout(W2kEdit *e)
{
    int lh = line_h(e);

    if (!e->multiline) {
        e->nvl = 0;
        vl_push(e, 0);
        return;
    }
    /* Two passes: the vertical bar's presence changes the wrap width. */
    for (int pass = 0; pass < 2; pass++) {
        rebuild_lines(e);
        e->vsb.total = e->nvl;
        e->vsb.page  = (e->r.h - 4) / lh;
        if (e->vsb.page < 1) e->vsb.page = 1;
        w2k_scroll_clamp(&e->vsb);
    }
    int vs = w2k_scroll_needed(&e->vsb) ? SCROLL_W : 0;
    int hs = 0;

    if (!e->wrap) {
        int widest = 0;
        for (int i = 0; i < e->nvl; i++) {
            int w = measure(e, e->vls[i], vl_end(e, i) - e->vls[i]);
            if (w > widest) widest = w;
        }
        e->hsb.total = widest + 4;
        e->hsb.page  = e->r.w - 4 - vs;
        if (e->hsb.page < 1) e->hsb.page = 1;
        hs = w2k_scroll_needed(&e->hsb) ? SCROLL_W : 0;
        w2k_scroll_clamp(&e->hsb);
    } else {
        e->hsb.total = e->hsb.page = 1;
        e->hsb.pos = 0;
    }

    e->vsb.r = (W2kRect){ e->r.x + e->r.w - 2 - SCROLL_W, e->r.y + 2,
                          SCROLL_W, e->r.h - 4 - hs };
    e->hsb.r = (W2kRect){ e->r.x + 2, e->r.y + e->r.h - 2 - SCROLL_W,
                          e->r.w - 4 - vs, SCROLL_W };
    e->vsb.page = (e->r.h - 4 - hs) / lh;
    if (e->vsb.page < 1) e->vsb.page = 1;
    w2k_scroll_clamp(&e->vsb);
    e->layout_w = e->r.w;
}

static void changed(W2kEdit *e)
{
    w2k_edit_layout(e);
    if (e->on_change) e->on_change(e->user);
    if (e->owner) w2k_win_dirty(e->owner);
}

/* Scroll so the caret is on screen. */
static void ensure_caret_visible(W2kEdit *e)
{
    int row = row_for_offset(e, e->caret);

    if (e->multiline) {
        if (row < e->vsb.pos) e->vsb.pos = row;
        if (row >= e->vsb.pos + e->vsb.page) e->vsb.pos = row - e->vsb.page + 1;
        w2k_scroll_clamp(&e->vsb);
    }
    int cx = measure(e, e->vls[row], e->caret - e->vls[row]);
    W2kScroll *h = e->multiline ? &e->hsb : NULL;
    int view = text_area_w(e);
    if (h && !e->wrap) {
        if (cx < h->pos) h->pos = cx;
        if (cx > h->pos + view - 4) h->pos = cx - view + 4;
        w2k_scroll_clamp(h);
    } else if (!e->multiline) {
        if (cx < e->scroll_x) e->scroll_x = cx;
        if (cx > e->scroll_x + view - 4) e->scroll_x = cx - view + 4;
        if (e->scroll_x < 0) e->scroll_x = 0;
    }
}

/* ------------------------------------------------------------------ *
 * Editing
 * ------------------------------------------------------------------ */
void w2k_edit_set(W2kEdit *e, const char *text)
{
    int n = text ? (int)strlen(text) : 0;
    ensure_cap(e, n);
    memcpy(e->text, text ? text : "", n);
    e->text[n] = 0;
    e->len = n;
    e->caret = e->sel = 0;
    e->vsb.pos = e->hsb.pos = e->scroll_x = 0;
    changed(e);
}

int w2k_edit_has_sel(W2kEdit *e) { return e->sel != e->caret; }

static void sel_range(W2kEdit *e, int *a, int *b)
{
    *a = min_i(e->sel, e->caret);
    *b = max_i(e->sel, e->caret);
}

void w2k_edit_delete_sel(W2kEdit *e)
{
    if (!w2k_edit_has_sel(e) || e->readonly) return;
    int a, b;
    sel_range(e, &a, &b);
    memmove(e->text + a, e->text + b, e->len - b + 1);
    e->len -= (b - a);
    e->caret = e->sel = a;
    changed(e);
}

void w2k_edit_insert(W2kEdit *e, const char *s)
{
    if (e->readonly || !s) return;
    w2k_edit_delete_sel(e);

    int n = strlen(s);
    if (!e->multiline) {                 /* single-line: flatten newlines */
        char *copy = w2k_strdup(s);
        int k = 0;
        for (int i = 0; i < n; i++)
            if (copy[i] != '\n' && copy[i] != '\r') copy[k++] = copy[i];
        copy[k] = 0;
        ensure_cap(e, e->len + k);
        memmove(e->text + e->caret + k, e->text + e->caret, e->len - e->caret + 1);
        memcpy(e->text + e->caret, copy, k);
        e->len += k;
        e->caret += k;
        free(copy);
    } else {
        ensure_cap(e, e->len + n);
        memmove(e->text + e->caret + n, e->text + e->caret, e->len - e->caret + 1);
        memcpy(e->text + e->caret, s, n);
        e->len += n;
        e->caret += n;
    }
    e->sel = e->caret;
    changed(e);
    ensure_caret_visible(e);
}

void w2k_edit_select_all(W2kEdit *e)
{
    e->sel = 0;
    e->caret = e->len;
    if (e->owner) w2k_win_dirty(e->owner);
}

void w2k_edit_copy(W2kEdit *e)
{
    if (!w2k_edit_has_sel(e)) return;
    int a, b;
    sel_range(e, &a, &b);
    char *s = w2k_alloc(b - a + 1);
    memcpy(s, e->text + a, b - a);
    s[b - a] = 0;
    w2k_clipboard_set(s);
    free(s);
}

void w2k_edit_cut(W2kEdit *e)
{
    if (e->readonly) return;
    w2k_edit_copy(e);
    w2k_edit_delete_sel(e);
}

void w2k_edit_paste(W2kEdit *e)
{
    char *s = w2k_clipboard_get();
    if (!s) return;
    w2k_edit_insert(e, s);
    free(s);
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */
void w2k_edit_draw(Drawable d, W2kEdit *e)
{
    if (e->layout_w != e->r.w) w2k_edit_layout(e);

    int lh = line_h(e);
    int vs = (e->multiline && w2k_scroll_needed(&e->vsb)) ? SCROLL_W : 0;
    int hs = (e->multiline && !e->wrap && w2k_scroll_needed(&e->hsb)) ? SCROLL_W : 0;

    int inset = e->noframe ? 0 : 2;
    if (!e->noframe)
        w2k_edge(d, e->r.x, e->r.y, e->r.w, e->r.h, EDGE_SUNKEN, BF_RECT);
    int tx0 = e->r.x + inset, ty0 = e->r.y + inset;
    int tw = e->r.w - 2 * inset - vs, th = e->r.h - 2 * inset - hs;
    w2k_fill(d, tx0, ty0, tw, th, e->readonly ? C_FACE : C_WINDOW);

    /* Clip text to the field. */
    w2k_clip_set(tx0, ty0, tw, th);

    int a, b;
    sel_range(e, &a, &b);
    int first = e->multiline ? e->vsb.pos : 0;
    int nvis = e->multiline ? e->vsb.page + 1 : 1;
    int xoff = e->multiline ? (e->wrap ? 0 : e->hsb.pos) : e->scroll_x;

    for (int i = first; i < e->nvl && i < first + nvis; i++) {
        int ls = e->vls[i], le = vl_end(e, i);
        int y = ty0 + PAD_Y + (i - first) * lh;
        int x = tx0 + PAD_X - xoff;

        /* Selection band for this row. */
        if (b > a && b > ls && a < le + 1) {
            int s0 = max_i(a, ls), s1 = min_i(b, le);
            if (b > le && i + 1 < e->nvl) s1 = le;        /* includes the newline */
            if (s1 >= s0) {
                int sx = x + measure(e, ls, s0 - ls);
                int sw = measure(e, ls, s1 - ls) - measure(e, ls, s0 - ls);
                if (b > le) sw += w2k_text_width(e->font, " ", 1);
                if (sw > 0) w2k_fill(d, sx, y, sw, lh, C_HIGHLIGHT);
            }
        }

        /* Draw the line in runs so tabs advance correctly and the selected
         * span picks up the highlight colour. */
        int run = 0, px = x, tabw = tab_width(e);
        for (int k = 0; k <= le - ls; k++) {
            int at = ls + k;
            int ch = (at < le) ? e->text[at] : 0;
            int is_tab = (ch == '\t');
            if (k == le - ls || is_tab) {
                if (run) {
                    /* Split the run at the selection boundaries. */
                    int rs = at - run;
                    while (run > 0) {
                        int sel_here = (rs >= a && rs < b);
                        int n = 0;
                        while (n < run) {
                            int p = rs + n;
                            int s2 = (p >= a && p < b);
                            if (s2 != sel_here) break;
                            n++;
                        }
                        w2k_textn(d, e->font, px, y, e->text + rs, n,
                                  sel_here ? C_HIGHLIGHTTEXT : C_WINDOWTEXT);
                        px += w2k_text_width(e->font, e->text + rs, n);
                        rs += n;
                        run -= n;
                    }
                }
                if (is_tab) px = x + ((px - x) / tabw + 1) * tabw;
            } else run++;
        }

        /* Caret */
        if (e->focused && e->caret_on && e->caret >= ls && e->caret <= le &&
            !w2k_edit_has_sel(e)) {
            int cx = x + measure(e, ls, e->caret - ls);
            w2k_fill(d, cx, y, 1, lh, C_WINDOWTEXT);
        }
    }
    w2k_clip_clear();

    if (vs) w2k_scroll_draw(d, &e->vsb);
    if (hs) w2k_scroll_draw(d, &e->hsb);
    if (vs && hs)   /* the dead square where the two bars meet */
        w2k_fill(d, e->hsb.r.x + e->hsb.r.w, e->vsb.r.y + e->vsb.r.h,
                 SCROLL_W, SCROLL_W, C_FACE);
}

void w2k_edit_scroll_to_caret(W2kEdit *e)
{
    if (e->layout_w != e->r.w) w2k_edit_layout(e);
    ensure_caret_visible(e);
    if (e->owner) w2k_win_dirty(e->owner);
}

void w2k_edit_blink(W2kEdit *e)
{
    if (!e->focused) { e->caret_on = 0; return; }
    e->caret_on = !e->caret_on;
    if (e->owner) w2k_win_dirty(e->owner);
}

/* ------------------------------------------------------------------ *
 * Mouse
 * ------------------------------------------------------------------ */
static int offset_at(W2kEdit *e, int px, int py)
{
    int lh = line_h(e);
    int ty0 = e->r.y + 2 + PAD_Y;
    int first = e->multiline ? e->vsb.pos : 0;
    int row = first + (py - ty0) / lh;
    if ((py - ty0) < 0) row = first;
    if (row < 0) row = 0;
    if (row >= e->nvl) row = e->nvl - 1;

    int xoff = e->multiline ? (e->wrap ? 0 : e->hsb.pos) : e->scroll_x;
    int rel = px - (e->r.x + 2 + PAD_X) + xoff;
    int ls = e->vls[row], le = vl_end(e, row);

    int best = ls, bestd = 1 << 30;
    for (int k = 0; k <= le - ls; k++) {
        int w = measure(e, ls, k);
        int dd = w - rel;
        if (dd < 0) dd = -dd;
        if (dd < bestd) { bestd = dd; best = ls + k; }
    }
    return best;
}

static int is_word(int c) { return isalnum((unsigned char)c) || c == '_'; }

int w2k_edit_press(W2kEdit *e, XButtonEvent *b)
{
    if (e->multiline) {
        if (w2k_scroll_needed(&e->vsb) && w2k_rect_hit(&e->vsb.r, b->x, b->y)) {
            if (w2k_scroll_press(&e->vsb, b->x, b->y) && e->owner)
                w2k_win_dirty(e->owner);
            return 1;
        }
        if (!e->wrap && w2k_scroll_needed(&e->hsb) &&
            w2k_rect_hit(&e->hsb.r, b->x, b->y)) {
            if (w2k_scroll_press(&e->hsb, b->x, b->y) && e->owner)
                w2k_win_dirty(e->owner);
            return 1;
        }
    }
    if (b->button == Button4 || b->button == Button5) {
        if (e->multiline && w2k_scroll_wheel(&e->vsb, b->button == Button4 ? -1 : 1)
            && e->owner) w2k_win_dirty(e->owner);
        return 1;
    }
    if (!w2k_rect_hit(&e->r, b->x, b->y)) return 0;
    if (b->button != Button1) return 1;

    e->focused = 1;
    e->caret_on = 1;
    int off = offset_at(e, b->x, b->y);

    /* Double-click selects a word. */
    static Time last;
    static int lastoff = -1;
    if (off == lastoff && (int)(b->time - last) < w2k_dblclk_ms) {
        int a = off, z = off;
        while (a > 0 && is_word(e->text[a - 1])) a--;
        while (z < e->len && is_word(e->text[z])) z++;
        e->sel = a;
        e->caret = z;
        last = 0;
    } else {
        e->caret = off;
        if (!(b->state & ShiftMask)) e->sel = off;
        last = b->time;
        lastoff = off;
    }
    if (e->owner) w2k_win_dirty(e->owner);
    return 1;
}

int w2k_edit_motion(W2kEdit *e, XMotionEvent *m)
{
    if (e->vsb.pressed) {
        if (w2k_scroll_motion(&e->vsb, m->x, m->y) && e->owner)
            w2k_win_dirty(e->owner);
        return 1;
    }
    if (e->hsb.pressed) {
        if (w2k_scroll_motion(&e->hsb, m->x, m->y) && e->owner)
            w2k_win_dirty(e->owner);
        return 1;
    }
    if (!(m->state & Button1Mask) || !e->focused) return 0;
    e->caret = offset_at(e, m->x, m->y);
    ensure_caret_visible(e);
    if (e->owner) w2k_win_dirty(e->owner);
    return 1;
}

void w2k_edit_release(W2kEdit *e)
{
    w2k_scroll_release(&e->vsb);
    w2k_scroll_release(&e->hsb);
}

/* ------------------------------------------------------------------ *
 * Keyboard
 * ------------------------------------------------------------------ */
int w2k_edit_key(W2kEdit *e, XKeyEvent *k)
{
    char buf[32];
    KeySym ks;
    int n = XLookupString(k, buf, sizeof buf - 1, &ks, NULL);
    buf[n > 0 ? n : 0] = 0;

    int ctrl = (k->state & ControlMask) != 0;
    int shift = (k->state & ShiftMask) != 0;
    int old = e->caret;
    int handled = 1;

    if (ctrl) {
        switch (ks) {
        case XK_a: case XK_A: w2k_edit_select_all(e); return 1;
        case XK_c: case XK_C: w2k_edit_copy(e); return 1;
        case XK_x: case XK_X: w2k_edit_cut(e); return 1;
        case XK_v: case XK_V: w2k_edit_paste(e); return 1;
        case XK_Home: e->caret = 0; goto moved;
        case XK_End:  e->caret = e->len; goto moved;
        case XK_Left: {
            while (e->caret > 0 && !is_word(e->text[e->caret - 1])) e->caret--;
            while (e->caret > 0 && is_word(e->text[e->caret - 1])) e->caret--;
            goto moved;
        }
        case XK_Right: {
            while (e->caret < e->len && is_word(e->text[e->caret])) e->caret++;
            while (e->caret < e->len && !is_word(e->text[e->caret])) e->caret++;
            goto moved;
        }
        }
    }

    switch (ks) {
    case XK_Left:  if (e->caret > 0) e->caret--; goto moved;
    case XK_Right: if (e->caret < e->len) e->caret++; goto moved;

    case XK_Up:
    case XK_Down: {
        if (!e->multiline) { handled = 0; break; }
        int row = row_for_offset(e, e->caret);
        int col = measure(e, e->vls[row], e->caret - e->vls[row]);
        int nr = row + (ks == XK_Down ? 1 : -1);
        if (nr < 0 || nr >= e->nvl) goto moved;
        int ls = e->vls[nr], le = vl_end(e, nr);
        e->caret = ls + chars_for_width(e, ls, le - ls, col);
        goto moved;
    }
    case XK_Prior:
    case XK_Next: {
        if (!e->multiline) { handled = 0; break; }
        int row = row_for_offset(e, e->caret);
        int nr = row + (ks == XK_Next ? e->vsb.page : -e->vsb.page);
        if (nr < 0) nr = 0;
        if (nr >= e->nvl) nr = e->nvl - 1;
        e->vsb.pos += (nr - row);
        w2k_scroll_clamp(&e->vsb);
        e->caret = e->vls[nr];
        goto moved;
    }
    case XK_Home: {
        int row = row_for_offset(e, e->caret);
        e->caret = e->vls[row];
        goto moved;
    }
    case XK_End: {
        int row = row_for_offset(e, e->caret);
        e->caret = vl_end(e, row);
        goto moved;
    }

    case XK_BackSpace:
        if (e->readonly) return 1;
        if (w2k_edit_has_sel(e)) { w2k_edit_delete_sel(e); return 1; }
        if (e->caret > 0) {
            memmove(e->text + e->caret - 1, e->text + e->caret,
                    e->len - e->caret + 1);
            e->len--;
            e->caret--;
            e->sel = e->caret;
            changed(e);
            ensure_caret_visible(e);
        }
        return 1;

    case XK_Delete:
        if (e->readonly) return 1;
        if (w2k_edit_has_sel(e)) { w2k_edit_delete_sel(e); return 1; }
        if (e->caret < e->len) {
            memmove(e->text + e->caret, e->text + e->caret + 1,
                    e->len - e->caret);
            e->len--;
            changed(e);
        }
        return 1;

    case XK_Return:
    case XK_KP_Enter:
        if (!e->multiline) return 0;         /* let the dialog take it */
        w2k_edit_insert(e, "\n");
        return 1;

    case XK_Tab:
        if (!e->multiline) return 0;         /* focus traversal */
        w2k_edit_insert(e, "\t");
        return 1;

    case XK_Escape:
        return 0;

    default:
        if (n > 0 && (unsigned char)buf[0] >= 32 && !ctrl) {
            w2k_edit_insert(e, buf);
            return 1;
        }
        handled = 0;
    }
    if (!handled) return 0;
    return 1;

moved:
    if (!shift) e->sel = e->caret;
    if (e->caret != old || shift) {
        e->caret_on = 1;
        ensure_caret_visible(e);
        if (e->owner) w2k_win_dirty(e->owner);
    }
    return 1;
}
