/* slider.c -- the trackbar.
 *
 * The control Windows uses for volume, double-click speed and pointer
 * speed: a sunken channel, a pointed thumb, and a row of tick marks. The
 * thumb points at the ticks, which is why it is not a plain rectangle --
 * on a horizontal trackbar with ticks below, its bottom five rows narrow
 * to a point.
 *
 * Positions are integers between `lo` and `hi` inclusive; the caller
 * decides what they mean. */
#include "w2k.h"
#include "w2kui.h"

#define THUMB_W   11
#define THUMB_H   21
#define BODY_H    16          /* the square part; the rest is the point */
#define CHANNEL   4

static int span(const W2kSlider *s)
{
    return (s->vertical ? s->r.h : s->r.w) - THUMB_W;
}

/* Where the middle of the thumb sits, in pixels along the track. */
static int thumb_at(const W2kSlider *s)
{
    int range = s->hi - s->lo;
    if (range <= 0) return THUMB_W / 2;
    int p = s->pos < s->lo ? s->lo : s->pos > s->hi ? s->hi : s->pos;
    return THUMB_W / 2 + span(s) * (p - s->lo) / range;
}

static int pos_from(const W2kSlider *s, int along)
{
    int range = s->hi - s->lo, w = span(s);
    if (w <= 0) return s->lo;
    along -= THUMB_W / 2;
    if (along < 0) along = 0;
    if (along > w) along = w;
    return s->lo + (along * range + w / 2) / w;
}

/* The thumb, drawn as spans so the point comes out right. */
static void draw_thumb(Drawable d, int x, int y, int vertical, int focused)
{
    for (int i = 0; i < THUMB_H; i++) {
        int inset = i < BODY_H ? 0 : i - BODY_H + 1;
        int w = THUMB_W - inset * 2;
        if (w <= 0) break;
        int lx = x + inset, ly = y + i;
        if (vertical) {
            /* Rotated: the point faces right. */
            w2k_fill(d, x + i, y + inset, 1, w, C_FACE);
            w2k_vline(d, x + i, y + inset, w, i == 0 ? C_HILIGHT : C_FACE);
        } else {
            w2k_fill(d, lx, ly, w, 1, C_FACE);
        }
    }
    if (vertical) {
        w2k_hline(d, x, y, BODY_H, C_HILIGHT);
        w2k_hline(d, x, y + THUMB_W - 1, BODY_H, C_DKSHADOW);
        w2k_hline(d, x + 1, y + THUMB_W - 2, BODY_H - 2, C_SHADOW);
        w2k_vline(d, x, y + 1, THUMB_W - 2, C_HILIGHT);
        for (int i = BODY_H; i < THUMB_H; i++) {
            int inset = i - BODY_H + 1;
            w2k_fill(d, x + i, y + inset, 1, 1, C_HILIGHT);
            w2k_fill(d, x + i, y + THUMB_W - 1 - inset, 1, 1, C_DKSHADOW);
        }
    } else {
        w2k_hline(d, x, y, THUMB_W, C_HILIGHT);
        w2k_vline(d, x, y, BODY_H, C_HILIGHT);
        w2k_vline(d, x + THUMB_W - 1, y, BODY_H, C_DKSHADOW);
        w2k_vline(d, x + THUMB_W - 2, y + 1, BODY_H - 2, C_SHADOW);
        for (int i = BODY_H; i < THUMB_H; i++) {
            int inset = i - BODY_H + 1;
            w2k_fill(d, x + inset - 1, y + i, 1, 1, C_HILIGHT);
            w2k_fill(d, x + THUMB_W - inset, y + i, 1, 1, C_DKSHADOW);
            if (THUMB_W - inset - 1 > inset)
                w2k_fill(d, x + THUMB_W - inset - 1, y + i, 1, 1, C_SHADOW);
        }
    }
    if (focused) w2k_focus_rect(d, x + 2, y + 2, THUMB_W - 4, BODY_H - 4);
}

void w2k_slider_draw(Drawable d, W2kSlider *s)
{
    int t = thumb_at(s);
    if (s->vertical) {
        int cx = s->r.x + (THUMB_H - CHANNEL) / 2;
        w2k_edge(d, cx, s->r.y + THUMB_W / 2 - 2, CHANNEL,
                 s->r.h - THUMB_W + 4, EDGE_SUNKEN, BF_RECT);
        for (int i = 0; i <= s->ticks && s->ticks > 0; i++) {
            int y = s->r.y + THUMB_W / 2 + span(s) * i / s->ticks;
            w2k_hline(d, s->r.x + THUMB_H + 2, y, 4, C_TEXT);
        }
        draw_thumb(d, s->r.x, s->r.y + t - THUMB_W / 2, 1, s->focused);
    } else {
        int cy = s->r.y + (THUMB_H - CHANNEL) / 2;
        w2k_edge(d, s->r.x + THUMB_W / 2 - 2, cy, s->r.w - THUMB_W + 4,
                 CHANNEL, EDGE_SUNKEN, BF_RECT);
        for (int i = 0; i <= s->ticks && s->ticks > 0; i++) {
            int x = s->r.x + THUMB_W / 2 + span(s) * i / s->ticks;
            w2k_vline(d, x, s->r.y + THUMB_H + 2, 4, C_TEXT);
        }
        draw_thumb(d, s->r.x + t - THUMB_W / 2, s->r.y, 0, s->focused);
    }
}

static void changed(W2kSlider *s, int old)
{
    if (s->pos == old) return;
    if (s->on_change) s->on_change(s->user, s->pos);
    if (s->owner) w2k_win_dirty(s->owner);
}

int w2k_slider_press(W2kSlider *s, XButtonEvent *b)
{
    W2kRect hit = s->r;
    if (s->vertical) hit.w = THUMB_H; else hit.h = THUMB_H;
    if (!w2k_rect_hit(&hit, b->x, b->y)) return 0;

    int old = s->pos;
    int along = s->vertical ? b->y - s->r.y : b->x - s->r.x;
    int t = thumb_at(s);
    if (along >= t - THUMB_W / 2 && along <= t + THUMB_W / 2) {
        s->dragging = 1;
        s->grab = along - t;
    } else {
        /* A click beside the thumb pages towards the pointer. */
        int step = (s->hi - s->lo) / 5;
        if (step < 1) step = 1;
        s->pos += along > t ? step : -step;
        if (s->pos < s->lo) s->pos = s->lo;
        if (s->pos > s->hi) s->pos = s->hi;
        s->dragging = 1;
        s->grab = 0;
    }
    s->focused = 1;
    changed(s, old);
    return 1;
}

int w2k_slider_motion(W2kSlider *s, XMotionEvent *m)
{
    if (!s->dragging) return 0;
    int old = s->pos;
    int along = (s->vertical ? m->y - s->r.y : m->x - s->r.x) - s->grab;
    s->pos = pos_from(s, along);
    changed(s, old);
    return 1;
}

void w2k_slider_release(W2kSlider *s) { s->dragging = 0; }

int w2k_slider_key(W2kSlider *s, XKeyEvent *k)
{
    KeySym ks = XLookupKeysym(k, 0);
    int old = s->pos, page = (s->hi - s->lo) / 5;
    if (page < 1) page = 1;
    switch (ks) {
    case XK_Left: case XK_Up:    s->pos--; break;
    case XK_Right: case XK_Down: s->pos++; break;
    case XK_Prior:               s->pos -= page; break;
    case XK_Next:                s->pos += page; break;
    case XK_Home:                s->pos = s->lo; break;
    case XK_End:                 s->pos = s->hi; break;
    default: return 0;
    }
    if (s->pos < s->lo) s->pos = s->lo;
    if (s->pos > s->hi) s->pos = s->hi;
    changed(s, old);
    return 1;
}
