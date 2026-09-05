/* scroll.c -- the classic 16-pixel scrollbar: two arrow buttons, a dithered
 * trough and a proportional thumb. */
#include "w2kui.h"
#include <stdlib.h>

#define ARROW  SCROLL_W
#define MINTHUMB 8
#define REPEAT_FIRST 350
#define REPEAT_NEXT   60

void w2k_scroll_bind(W2kScroll *s, W2kWin *w) { s->owner = w; }

void w2k_scroll_clamp(W2kScroll *s)
{
    if (s->line <= 0) s->line = 1;
    int max = s->total - s->page;
    if (max < 0) max = 0;
    if (s->pos > max) s->pos = max;
    if (s->pos < 0) s->pos = 0;
}

int w2k_scroll_needed(W2kScroll *s) { return s->total > s->page; }

/* Geometry of the thumb inside the track. */
static void thumb_geom(W2kScroll *s, int *off, int *size)
{
    int len = (s->vertical ? s->r.h : s->r.w) - 2 * ARROW;
    if (len < MINTHUMB) { *off = 0; *size = len > 0 ? len : 0; return; }
    if (s->total <= s->page || s->total <= 0) { *off = 0; *size = len; return; }

    int t = (int)((long)s->page * len / s->total);
    if (t < MINTHUMB) t = MINTHUMB;
    if (t > len) t = len;
    int max = s->total - s->page;
    int o = max > 0 ? (int)((long)s->pos * (len - t) / max) : 0;
    *off = o;
    *size = t;
}

/* Solid triangle glyphs for the arrow buttons. */
static void arrow(Drawable d, int x, int y, int dir, int color)
{
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    for (int i = 0; i < 4; i++) {
        int n = 1 + 2 * i;
        switch (dir) {
        case 0: w2k_fill_fg(d, x + 3 - i, y + 5 - i + 1, n, 1); break;
        case 1: w2k_fill_fg(d, x + 3 - i, y + 2 + i + 1, n, 1); break;
        case 2: w2k_fill_fg(d, x + 5 - i + 1, y + 3 - i, 1, n); break;
        case 3: w2k_fill_fg(d, x + 2 + i + 1, y + 3 - i, 1, n); break;
        }
    }
}

void w2k_scroll_draw(Drawable d, W2kScroll *s)
{
    int x = s->r.x, y = s->r.y, w = s->r.w, h = s->r.h;
    if (w <= 0 || h <= 0) return;

    int enabled = w2k_scroll_needed(s);
    int glyph = enabled ? C_TEXT : C_GRAYTEXT;

    /* Trough */
    w2k_dither(d, x, y, w, h, C_HILIGHT, C_FACE);

    if (s->vertical) {
        w2k_button(d, x, y, w, ARROW, s->pressed == SB_LINEUP);
        arrow(d, x + (w - 9) / 2, y + (ARROW - 9) / 2,
              1, glyph);
        w2k_button(d, x, y + h - ARROW, w, ARROW, s->pressed == SB_LINEDOWN);
        arrow(d, x + (w - 9) / 2, y + h - ARROW + (ARROW - 9) / 2,
              0, glyph);
        if (enabled) {
            int off, size;
            thumb_geom(s, &off, &size);
            w2k_button(d, x, y + ARROW + off, w, size, 0);
        }
    } else {
        w2k_button(d, x, y, ARROW, h, s->pressed == SB_LINEUP);
        arrow(d, x + (ARROW - 9) / 2, y + (h - 9) / 2, 3, glyph);
        w2k_button(d, x + w - ARROW, y, ARROW, h, s->pressed == SB_LINEDOWN);
        arrow(d, x + w - ARROW + (ARROW - 9) / 2, y + (h - 9) / 2, 2, glyph);
        if (enabled) {
            int off, size;
            thumb_geom(s, &off, &size);
            w2k_button(d, x + ARROW + off, y, size, h, 0);
        }
    }
}

int w2k_scroll_part(W2kScroll *s, int px, int py)
{
    if (!w2k_rect_hit(&s->r, px, py)) return SB_NONE;
    int p = s->vertical ? py - s->r.y : px - s->r.x;
    int len = s->vertical ? s->r.h : s->r.w;

    if (p < ARROW)       return SB_LINEUP;
    if (p >= len - ARROW) return SB_LINEDOWN;
    if (!w2k_scroll_needed(s)) return SB_NONE;

    int off, size;
    thumb_geom(s, &off, &size);
    int t = p - ARROW;
    if (t < off)         return SB_PAGEUP;
    if (t >= off + size) return SB_PAGEDOWN;
    return SB_THUMB;
}

static int apply(W2kScroll *s, int part)
{
    int old = s->pos;
    switch (part) {
    case SB_LINEUP:   s->pos -= s->line; break;
    case SB_LINEDOWN: s->pos += s->line; break;
    case SB_PAGEUP:   s->pos -= s->page; break;
    case SB_PAGEDOWN: s->pos += s->page; break;
    }
    w2k_scroll_clamp(s);
    return s->pos != old;
}

/* Auto-repeat while an arrow or trough is held. */
static void repeat_tick(void *v)
{
    W2kScroll *s = v;
    if (!s->pressed || s->pressed == SB_THUMB) return;
    if (w2k_now_ms() < s->repeat_at) return;
    s->repeat_at = w2k_now_ms() + REPEAT_NEXT;
    if (apply(s, s->pressed) && s->owner) w2k_win_dirty(s->owner);
}

int w2k_scroll_press(W2kScroll *s, int px, int py)
{
    int part = w2k_scroll_part(s, px, py);
    if (part == SB_NONE) return 0;
    s->pressed = part;

    if (part == SB_THUMB) {
        int off, size;
        thumb_geom(s, &off, &size);
        int p = s->vertical ? py - s->r.y : px - s->r.x;
        s->drag_off = p - ARROW - off;
        return 0;
    }
    s->repeat_at = w2k_now_ms() + REPEAT_FIRST;
    if (s->owner) w2k_add_timer(REPEAT_NEXT / 2, repeat_tick, s);
    return apply(s, part);
}

int w2k_scroll_motion(W2kScroll *s, int px, int py)
{
    if (s->pressed != SB_THUMB) return 0;
    int len = (s->vertical ? s->r.h : s->r.w) - 2 * ARROW;
    int off, size;
    thumb_geom(s, &off, &size);
    if (len <= size) return 0;

    int p = (s->vertical ? py - s->r.y : px - s->r.x) - ARROW - s->drag_off;
    int max = s->total - s->page;
    int old = s->pos;
    s->pos = (int)((long)p * max / (len - size));
    w2k_scroll_clamp(s);
    return s->pos != old;
}

void w2k_scroll_release(W2kScroll *s)
{
    if (s->pressed && s->pressed != SB_THUMB) w2k_del_timer(repeat_tick, s);
    s->pressed = SB_NONE;
}

int w2k_scroll_wheel(W2kScroll *s, int dir)
{
    int old = s->pos;
    s->pos += dir * s->line * 3;      /* three lines per notch, as in Windows */
    w2k_scroll_clamp(s);
    return s->pos != old;
}
