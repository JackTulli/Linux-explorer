/* l2kpaint -- Paint (Windows XP style). Single-threaded software canvas. */
#include "w2kui.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_FILTERS \
    "PNG Image (*.png)|*.png|JPEG Image (*.jpg;*.jpeg)|*.jpg;*.jpeg|" \
    "Bitmap Image (*.bmp)|*.bmp|All Files (*.*)|*"

enum {
    ID_NEW = 1, ID_OPEN, ID_SAVE, ID_SAVEAS, ID_PROPS, ID_SCALE, ID_EXIT,
    ID_UNDO, ID_REDO,
    ID_ZOOMIN, ID_ZOOMOUT, ID_ZOOM100,
    ID_TOOL_PENCIL, ID_TOOL_BRUSH, ID_TOOL_ERASER, ID_TOOL_LINE,
    ID_TOOL_RECT, ID_TOOL_ELLIPSE, ID_TOOL_FILL, ID_TOOL_PICK, ID_TOOL_TEXT,
    ID_COLOR_FG, ID_COLOR_BG, ID_ABOUT,
    ID_LAYER_NEW, ID_LAYER_DEL, ID_LAYER_UP, ID_LAYER_DOWN,
    ID_LAYER_PREV, ID_LAYER_NEXT, ID_LAYER_TOGGLE
};

enum {
    T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_PICK, T_TEXT
};

#define MAX_UNDO   12
#define MAX_LAYERS 8
#define TOOLBOX_W  52
#define LAYER_W    96
#define PALETTE_H  40
#define COLORBOX_H 52
#define MIN_ZOOM   1
#define MAX_ZOOM   32

/* Classic Paint 28-colour palette (2 rows × 14). */
static const unsigned char k_palette[28][3] = {
    {0,0,0},{128,128,128},{128,0,0},{128,128,0},{0,128,0},{0,128,128},{0,0,128},{128,0,128},
    {128,128,64},{0,64,64},{0,128,255},{0,64,128},{64,0,255},{128,64,0},
    {255,255,255},{192,192,192},{255,0,0},{255,255,0},{0,255,0},{0,255,255},{0,0,255},{255,0,255},
    {255,255,128},{0,255,128},{128,255,255},{128,128,255},{255,0,128},{255,128,64}
};

typedef struct {
    unsigned char *rgba;
    int visible;
    char name[24];
} Layer;

typedef struct {
    unsigned char *rgba;
    int w, h, layer;
} Snap;

typedef struct {
    W2kWin *win;
    W2kMenubar *mb;
    W2kToolbar *tb;
    W2kStatus *sb;
    W2kRect canvas_r, layer_r, palette_r;

    int w, h;
    Layer layer[MAX_LAYERS];
    int nlayers, active;

    char path[1024];
    int untitled, dirty;

    int tool, brush;
    int fg[3], bg[3];
    int zoom, pan_x, pan_y;

    int drawing, x0, y0, x1, y1;
    int panning, pan_mx, pan_my, pan_ox, pan_oy;

    unsigned char *stroke_backup;
    int stroke_x, stroke_y, stroke_w, stroke_h;

    Snap undo[MAX_UNDO], redo[MAX_UNDO];
    int nundo, nredo;

    /* Text tool: click places a prompt, then stamps glyphs. */
    int text_x, text_y;
} Paint;

static Paint pt;

/* ---- layers --------------------------------------------------------- */

static void layer_free(Layer *L)
{
    free(L->rgba);
    memset(L, 0, sizeof *L);
}

static unsigned char *buf_new(int w, int h, int white)
{
    size_t n = (size_t)w * h;
    unsigned char *p = calloc(n, 4);
    if (!p) return NULL;
    if (white)
        for (size_t i = 0; i < n; i++) {
            p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = p[i * 4 + 3] = 255;
        }
    return p;
}

static void layers_clear(void)
{
    for (int i = 0; i < MAX_LAYERS; i++) layer_free(&pt.layer[i]);
    pt.nlayers = pt.active = 0;
    pt.w = pt.h = 0;
}

static int layers_init(int w, int h)
{
    layers_clear();
    if (w < 1 || h < 1 || w > 8192 || h > 8192) return 0;
    pt.layer[0].rgba = buf_new(w, h, 1);
    if (!pt.layer[0].rgba) return 0;
    pt.layer[0].visible = 1;
    snprintf(pt.layer[0].name, sizeof pt.layer[0].name, "Background");
    pt.nlayers = 1;
    pt.active = 0;
    pt.w = w;
    pt.h = h;
    return 1;
}

static int layer_add(void)
{
    if (pt.nlayers >= MAX_LAYERS || pt.w < 1) return 0;
    int i = pt.nlayers;
    pt.layer[i].rgba = buf_new(pt.w, pt.h, 0);
    if (!pt.layer[i].rgba) return 0;
    pt.layer[i].visible = 1;
    snprintf(pt.layer[i].name, sizeof pt.layer[i].name, "Layer %d", i + 1);
    pt.nlayers++;
    pt.active = i;
    return 1;
}

static void layer_delete(int idx)
{
    if (pt.nlayers <= 1 || idx < 0 || idx >= pt.nlayers) return;
    layer_free(&pt.layer[idx]);
    memmove(&pt.layer[idx], &pt.layer[idx + 1],
            (size_t)(pt.nlayers - idx - 1) * sizeof pt.layer[0]);
    memset(&pt.layer[pt.nlayers - 1], 0, sizeof pt.layer[0]);
    pt.nlayers--;
    if (pt.active >= pt.nlayers) pt.active = pt.nlayers - 1;
}

static void layer_swap(int a, int b)
{
    if (a < 0 || b < 0 || a >= pt.nlayers || b >= pt.nlayers || a == b) return;
    Layer t = pt.layer[a];
    pt.layer[a] = pt.layer[b];
    pt.layer[b] = t;
    if (pt.active == a) pt.active = b;
    else if (pt.active == b) pt.active = a;
}

static unsigned char *layers_flatten(void)
{
    if (pt.w < 1 || pt.h < 1) return NULL;
    size_t n = (size_t)pt.w * pt.h;
    unsigned char *out = malloc(n * 4);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = 255;
        out[i * 4 + 3] = 255;
    }
    for (int li = 0; li < pt.nlayers; li++) {
        if (!pt.layer[li].visible || !pt.layer[li].rgba) continue;
        const unsigned char *s = pt.layer[li].rgba;
        for (size_t i = 0; i < n; i++) {
            int a = s[i * 4 + 3];
            if (a == 0) continue;
            if (a == 255) {
                out[i * 4] = s[i * 4];
                out[i * 4 + 1] = s[i * 4 + 1];
                out[i * 4 + 2] = s[i * 4 + 2];
            } else {
                out[i * 4]     = (unsigned char)((s[i * 4]     * a + out[i * 4]     * (255 - a)) / 255);
                out[i * 4 + 1] = (unsigned char)((s[i * 4 + 1] * a + out[i * 4 + 1] * (255 - a)) / 255);
                out[i * 4 + 2] = (unsigned char)((s[i * 4 + 2] * a + out[i * 4 + 2] * (255 - a)) / 255);
            }
        }
    }
    return out;
}

/* ---- undo (active layer only) --------------------------------------- */

static void snap_free(Snap *s)
{
    free(s->rgba);
    memset(s, 0, sizeof *s);
}

static void clear_history(void)
{
    for (int i = 0; i < pt.nundo; i++) snap_free(&pt.undo[i]);
    for (int i = 0; i < pt.nredo; i++) snap_free(&pt.redo[i]);
    pt.nundo = pt.nredo = 0;
}

static void push_undo(void)
{
    Layer *L = &pt.layer[pt.active];
    if (!L->rgba || pt.w < 1) return;
    if (pt.nundo == MAX_UNDO) {
        snap_free(&pt.undo[0]);
        memmove(pt.undo, pt.undo + 1, (MAX_UNDO - 1) * sizeof pt.undo[0]);
        pt.nundo--;
    }
    Snap *s = &pt.undo[pt.nundo];
    snap_free(s);
    s->w = pt.w; s->h = pt.h; s->layer = pt.active;
    s->rgba = malloc((size_t)pt.w * pt.h * 4);
    if (!s->rgba) return;
    memcpy(s->rgba, L->rgba, (size_t)pt.w * pt.h * 4);
    pt.nundo++;
    for (int i = 0; i < pt.nredo; i++) snap_free(&pt.redo[i]);
    pt.nredo = 0;
}

static void apply_snap(const Snap *s)
{
    if (!s || !s->rgba || s->layer < 0 || s->layer >= pt.nlayers) return;
    if (s->w != pt.w || s->h != pt.h) return;
    Layer *L = &pt.layer[s->layer];
    if (!L->rgba) return;
    memcpy(L->rgba, s->rgba, (size_t)pt.w * pt.h * 4);
    pt.active = s->layer;
}

static void do_undo(void)
{
    if (pt.nundo <= 0) return;
    Layer *L = &pt.layer[pt.active];
    if (pt.nredo < MAX_UNDO && L->rgba) {
        Snap *s = &pt.redo[pt.nredo];
        snap_free(s);
        s->w = pt.w; s->h = pt.h; s->layer = pt.active;
        s->rgba = malloc((size_t)pt.w * pt.h * 4);
        if (s->rgba) {
            memcpy(s->rgba, L->rgba, (size_t)pt.w * pt.h * 4);
            pt.nredo++;
        }
    }
    apply_snap(&pt.undo[--pt.nundo]);
    snap_free(&pt.undo[pt.nundo]);
    pt.dirty = 1;
    w2k_win_dirty(pt.win);
}

static void do_redo(void)
{
    if (pt.nredo <= 0) return;
    Layer *L = &pt.layer[pt.active];
    if (pt.nundo < MAX_UNDO && L->rgba) {
        Snap *s = &pt.undo[pt.nundo];
        snap_free(s);
        s->w = pt.w; s->h = pt.h; s->layer = pt.active;
        s->rgba = malloc((size_t)pt.w * pt.h * 4);
        if (s->rgba) {
            memcpy(s->rgba, L->rgba, (size_t)pt.w * pt.h * 4);
            pt.nundo++;
        }
    }
    apply_snap(&pt.redo[--pt.nredo]);
    snap_free(&pt.redo[pt.nredo]);
    pt.dirty = 1;
    w2k_win_dirty(pt.win);
}

/* ---- pixels --------------------------------------------------------- */

static inline int in_doc(int x, int y)
{
    return (unsigned)x < (unsigned)pt.w && (unsigned)y < (unsigned)pt.h;
}

static inline void put_px(int x, int y, int r, int g, int b)
{
    if (!in_doc(x, y)) return;
    Layer *L = &pt.layer[pt.active];
    if (!L->rgba) return;
    unsigned char *p = L->rgba + ((size_t)y * pt.w + x) * 4;
    p[0] = (unsigned char)r;
    p[1] = (unsigned char)g;
    p[2] = (unsigned char)b;
    p[3] = 255;
}

static inline void erase_px(int x, int y)
{
    if (!in_doc(x, y)) return;
    Layer *L = &pt.layer[pt.active];
    if (!L->rgba) return;
    unsigned char *p = L->rgba + ((size_t)y * pt.w + x) * 4;
    if (pt.active == 0) {
        p[0] = (unsigned char)pt.bg[0];
        p[1] = (unsigned char)pt.bg[1];
        p[2] = (unsigned char)pt.bg[2];
        p[3] = 255;
    } else {
        p[0] = p[1] = p[2] = p[3] = 0;
    }
}

static inline void get_px(int x, int y, int *r, int *g, int *b)
{
    *r = *g = *b = 255;
    if (!in_doc(x, y)) return;
    Layer *L = &pt.layer[pt.active];
    if (!L->rgba) return;
    const unsigned char *p = L->rgba + ((size_t)y * pt.w + x) * 4;
    if (p[3] == 0) return;
    *r = p[0]; *g = p[1]; *b = p[2];
}

static void pick_px(int x, int y, int *r, int *g, int *b)
{
    *r = *g = *b = 255;
    if (!in_doc(x, y)) return;
    for (int li = 0; li < pt.nlayers; li++) {
        if (!pt.layer[li].visible || !pt.layer[li].rgba) continue;
        const unsigned char *p =
            pt.layer[li].rgba + ((size_t)y * pt.w + x) * 4;
        if (p[3] == 0) continue;
        *r = p[0]; *g = p[1]; *b = p[2];
        if (p[3] == 255) return;
    }
}

static void stamp(int cx, int cy, int rad, int r, int g, int b, int erase)
{
    if (rad <= 0) {
        if (erase) erase_px(cx, cy);
        else put_px(cx, cy, r, g, b);
        return;
    }
    int rr = rad * rad;
    for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++)
            if (dx * dx + dy * dy <= rr) {
                if (erase) erase_px(cx + dx, cy + dy);
                else put_px(cx + dx, cy + dy, r, g, b);
            }
}

static void draw_line(int x0, int y0, int x1, int y1, int rad,
                      int r, int g, int b, int erase)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        stamp(x0, y0, rad, r, g, b, erase);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_rect(int x0, int y0, int x1, int y1, int rad,
                      int r, int g, int b, int erase)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    draw_line(x0, y0, x1, y0, rad, r, g, b, erase);
    draw_line(x1, y0, x1, y1, rad, r, g, b, erase);
    draw_line(x1, y1, x0, y1, rad, r, g, b, erase);
    draw_line(x0, y1, x0, y0, rad, r, g, b, erase);
}

static void draw_ellipse(int x0, int y0, int x1, int y1, int rad,
                         int r, int g, int b, int erase)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    int rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;
    /* Parametric outline — avoids midpoint edge cases. */
    int steps = (rx + ry) * 4;
    if (steps < 32) steps = 32;
    if (steps > 1024) steps = 1024;
    int px = cx + rx, py = cy;
    for (int i = 1; i <= steps; i++) {
        double a = (6.283185307179586 * i) / steps;
        int x = cx + (int)(rx * cos(a) + 0.5);
        int y = cy + (int)(ry * sin(a) + 0.5);
        draw_line(px, py, x, y, rad, r, g, b, erase);
        px = x; py = y;
    }
}

/* Flood fill with visited bitmask — no stack overflow. */
static void flood_fill(int sx, int sy, int nr, int ng, int nb)
{
    if (!in_doc(sx, sy)) return;
    int tr, tg, tb;
    get_px(sx, sy, &tr, &tg, &tb);
    if (tr == nr && tg == ng && tb == nb) return;

    size_t npx = (size_t)pt.w * pt.h;
    unsigned char *seen = calloc((npx + 7) / 8, 1);
    int *stack = malloc(npx * sizeof *stack);
    if (!seen || !stack) { free(seen); free(stack); return; }

    int top = 0;
    stack[top++] = sy * pt.w + sx;
    seen[sx >> 3] |= (unsigned char)(1 << (sx & 7));

    while (top > 0) {
        int i = stack[--top];
        int x = i % pt.w, y = i / pt.w;
        int r, g, b;
        get_px(x, y, &r, &g, &b);
        if (r != tr || g != tg || b != tb) continue;
        put_px(x, y, nr, ng, nb);
        static const int ox[4] = { -1, 1, 0, 0 };
        static const int oy[4] = { 0, 0, -1, 1 };
        for (int k = 0; k < 4; k++) {
            int nx = x + ox[k], ny = y + oy[k];
            if (!in_doc(nx, ny)) continue;
            size_t ni = (size_t)ny * pt.w + nx;
            if (seen[ni >> 3] & (1u << (ni & 7))) continue;
            seen[ni >> 3] |= (unsigned char)(1u << (ni & 7));
            if (top < (int)npx) stack[top++] = (int)ni;
        }
    }
    free(seen);
    free(stack);
}

static void stamp_char(int x, int y, char ch, int r, int g, int b)
{
    /* 5x7 column bitmaps, LSB = top row. Index = ASCII - 32. */
    static const unsigned char glyphs[95][5] = {
        {0,0,0,0,0},
        {0,0,0x5f,0,0},{0,7,0,7,0},{0x14,0x7f,0x14,0x7f,0x14},
        {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},
        {0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},
        {0x1c,0x22,0x41,0,0},{0,0x41,0x22,0x1c,0},
        {0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
        {0,0x50,0x30,0,0},{0x08,0x08,0x08,0x08,0x08},
        {0,0x60,0x60,0,0},{0x20,0x10,0x08,0x04,0x02},
        {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},
        {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
        {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
        {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},
        {0,0x36,0x36,0,0},{0,0x56,0x36,0,0},
        {0x08,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},
        {0,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
        {0x32,0x49,0x79,0x41,0x3e},
        {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
        {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
        {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
        {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
        {0,0x41,0x7f,0x41,0},{0x20,0x40,0x41,0x3f,0x01},
        {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
        {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
        {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
        {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
        {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
        {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
        {0,0x7f,0x41,0x41,0},{0x02,0x04,0x08,0x10,0x20},
        {0,0x41,0x41,0x7f,0},{0x04,0x02,0x01,0x02,0x04},
        {0x40,0x40,0x40,0x40,0x40},{0,1,2,4,0},
        {0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},
        {0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},
        {0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},
        {0x0c,0x52,0x52,0x52,0x3e},{0x7f,0x08,0x04,0x04,0x78},
        {0,0x44,0x7d,0x40,0},{0x20,0x40,0x44,0x3d,0},
        {0x7f,0x10,0x28,0x44,0},{0,0x41,0x7f,0x40,0},
        {0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},
        {0x38,0x44,0x44,0x44,0x38},{0x7c,0x14,0x14,0x14,0x08},
        {0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},
        {0x48,0x54,0x54,0x54,0x24},{0x04,0x3f,0x44,0x40,0x20},
        {0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},
        {0x3c,0x40,0x30,0x40,0x3c},{0x44,0x28,0x10,0x28,0x44},
        {0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},
        {0,0x08,0x36,0x41,0},{0,0,0x7f,0,0},
        {0,0x41,0x36,0x08,0},{0x10,0x08,0x08,0x10,0x08}
    };
    int idx = (unsigned char)ch - 32;
    if (idx < 0 || idx > 94) idx = '?' - 32;
    for (int col = 0; col < 5; col++) {
        unsigned char bits = glyphs[idx][col];
        for (int row = 0; row < 7; row++)
            if (bits & (1u << row))
                put_px(x + col, y + row, r, g, b);
    }
}

static void draw_text_string(int x, int y, const char *s, int r, int g, int b)
{
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += 9; continue; }
        stamp_char(cx, y, *s, r, g, b);
        cx += 6;
    }
}

/* ---- stroke backup -------------------------------------------------- */

static void stroke_free(void)
{
    free(pt.stroke_backup);
    pt.stroke_backup = NULL;
    pt.stroke_w = pt.stroke_h = 0;
}

static void stroke_save_full(void)
{
    stroke_free();
    Layer *L = &pt.layer[pt.active];
    if (!L->rgba) return;
    pt.stroke_x = 0;
    pt.stroke_y = 0;
    pt.stroke_w = pt.w;
    pt.stroke_h = pt.h;
    size_t n = (size_t)pt.w * pt.h * 4;
    pt.stroke_backup = malloc(n);
    if (pt.stroke_backup) memcpy(pt.stroke_backup, L->rgba, n);
}

static void stroke_restore(void)
{
    if (!pt.stroke_backup) return;
    Layer *L = &pt.layer[pt.active];
    if (!L->rgba) return;
    memcpy(L->rgba, pt.stroke_backup, (size_t)pt.w * pt.h * 4);
}

/* ---- UI helpers ----------------------------------------------------- */

static void screen_to_doc(int sx, int sy, int *dx, int *dy)
{
    *dx = pt.pan_x + (sx - pt.canvas_r.x) / pt.zoom;
    *dy = pt.pan_y + (sy - pt.canvas_r.y) / pt.zoom;
}

static void update_title(void)
{
    char t[1200];
    const char *name = pt.untitled ? "Untitled" : strrchr(pt.path, '/');
    if (!pt.untitled && name) name++;
    else if (!pt.untitled) name = pt.path;
    snprintf(t, sizeof t, "%s%s - Paint", pt.dirty ? "*" : "", name);
    w2k_win_title(pt.win, t);
}

static void update_status(void)
{
    char b[160];
    snprintf(b, sizeof b, "%d x %d", pt.w, pt.h);
    w2k_status_set(pt.sb, 0, b);
    snprintf(b, sizeof b, "%d%%", pt.zoom * 100);
    w2k_status_set(pt.sb, 1, b);
    static const char *tools[] = {
        "Pencil", "Brush", "Eraser", "Line", "Rectangle", "Ellipse",
        "Fill", "Pick", "Text"
    };
    snprintf(b, sizeof b, "%s · %s (%d/%d)",
             tools[pt.tool], pt.layer[pt.active].name,
             pt.active + 1, pt.nlayers);
    w2k_status_set(pt.sb, 2, b);
}

static int confirm_discard(void)
{
    if (!pt.dirty) return 1;
    int r = w2k_msgbox(pt.win, "Paint", "Save changes to the picture?",
                       MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == ID_CANCEL || r == 0) return 0;
    if (r == ID_NO) return 1;
    return -1;
}

static int save_to(const char *path)
{
    unsigned char *flat = layers_flatten();
    if (!flat) return 0;
    const char *ext = strrchr(path, '.');
    int ok = 0;
    if (ext && !strcasecmp(ext, ".png"))
        ok = w2k_png_save(path, flat, pt.w, pt.h);
    else if (ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))) {
        char buf[16] = "90";
        if (w2k_prompt(pt.win, "JPEG Options", "Quality (1-100):",
                       "90", buf, sizeof buf, ICO_NONE))
            ok = w2k_jpeg_save_quality(path, flat, pt.w, pt.h, atoi(buf));
        else { free(flat); return 0; }
    } else if (ext && !strcasecmp(ext, ".bmp"))
        ok = w2k_bmp_save(path, flat, pt.w, pt.h);
    else
        ok = w2k_png_save(path, flat, pt.w, pt.h);
    free(flat);
    if (!ok) {
        w2k_msgbox(pt.win, "Paint", "The picture could not be saved.",
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    snprintf(pt.path, sizeof pt.path, "%s", path);
    pt.untitled = 0;
    pt.dirty = 0;
    update_title();
    return 1;
}

static int do_save(int saveas)
{
    if (!saveas && !pt.untitled && pt.path[0]) return save_to(pt.path);
    char path[1024];
    snprintf(path, sizeof path, "%s", pt.untitled ? "Untitled.png" : pt.path);
    if (!w2k_file_dialog_filter(pt.win, 1, path, sizeof path, IMAGE_FILTERS))
        return 0;
    return save_to(path);
}

static void do_new(void)
{
    int c = confirm_discard();
    if (c == 0) return;
    if (c < 0 && !do_save(0)) return;
    layers_init(640, 480);
    pt.path[0] = 0;
    pt.untitled = 1;
    pt.dirty = 0;
    pt.zoom = 1;
    pt.pan_x = pt.pan_y = 0;
    clear_history();
    update_title();
    update_status();
    w2k_win_dirty(pt.win);
}

static void do_open(void)
{
    int c = confirm_discard();
    if (c == 0) return;
    if (c < 0 && !do_save(0)) return;
    char path[1024] = "";
    if (!w2k_file_dialog_filter(pt.win, 0, path, sizeof path, IMAGE_FILTERS))
        return;
    int w = 0, h = 0;
    unsigned char *rgba = w2k_image_load(path, &w, &h);
    if (!rgba || w < 1 || h < 1) {
        free(rgba);
        w2k_msgbox(pt.win, "Paint", "The picture could not be opened.",
                   MB_OK | MB_ICONERROR);
        return;
    }
    layers_init(w, h);
    memcpy(pt.layer[0].rgba, rgba, (size_t)w * h * 4);
    free(rgba);
    snprintf(pt.path, sizeof pt.path, "%s", path);
    pt.untitled = 0;
    pt.dirty = 0;
    pt.zoom = 1;
    pt.pan_x = pt.pan_y = 0;
    clear_history();
    update_title();
    update_status();
    w2k_win_dirty(pt.win);
}

/* Resize document (attributes / properties). */
static void do_properties(void)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%d", pt.w);
    char wb[32], hb[32];
    snprintf(wb, sizeof wb, "%d", pt.w);
    snprintf(hb, sizeof hb, "%d", pt.h);
    if (!w2k_prompt(pt.win, "Attributes", "Width (pixels):", wb, wb, sizeof wb, ICO_NONE))
        return;
    if (!w2k_prompt(pt.win, "Attributes", "Height (pixels):", hb, hb, sizeof hb, ICO_NONE))
        return;
    int nw = atoi(wb), nh = atoi(hb);
    if (nw < 1 || nh < 1 || nw > 8192 || nh > 8192) {
        w2k_msgbox(pt.win, "Paint", "Size must be between 1 and 8192.",
                   MB_OK | MB_ICONERROR);
        return;
    }
    if (nw == pt.w && nh == pt.h) return;
    /* Reallocate each layer, copy the overlapping region. */
    for (int li = 0; li < pt.nlayers; li++) {
        unsigned char *nbuf = buf_new(nw, nh, li == 0);
        if (!nbuf) return;
        int cw = nw < pt.w ? nw : pt.w;
        int ch = nh < pt.h ? nh : pt.h;
        for (int y = 0; y < ch; y++)
            memcpy(nbuf + (size_t)y * nw * 4,
                   pt.layer[li].rgba + (size_t)y * pt.w * 4,
                   (size_t)cw * 4);
        free(pt.layer[li].rgba);
        pt.layer[li].rgba = nbuf;
    }
    pt.w = nw;
    pt.h = nh;
    clear_history();
    pt.dirty = 1;
    update_title();
    update_status();
    w2k_win_dirty(pt.win);
}

/* Scale image (nearest-neighbour). */
static void do_scale(void)
{
    char buf[32] = "100";
    if (!w2k_prompt(pt.win, "Stretch/Skew", "Scale percent:", buf, buf, sizeof buf, ICO_NONE))
        return;
    int pct = atoi(buf);
    if (pct < 10 || pct > 500) {
        w2k_msgbox(pt.win, "Paint", "Scale must be between 10% and 500%.",
                   MB_OK | MB_ICONERROR);
        return;
    }
    int nw = pt.w * pct / 100;
    int nh = pt.h * pct / 100;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > 8192 || nh > 8192) {
        w2k_msgbox(pt.win, "Paint", "Resulting size is too large.",
                   MB_OK | MB_ICONERROR);
        return;
    }
    for (int li = 0; li < pt.nlayers; li++) {
        unsigned char *src = pt.layer[li].rgba;
        unsigned char *dst = buf_new(nw, nh, 0);
        if (!dst) return;
        for (int y = 0; y < nh; y++) {
            int sy = y * pt.h / nh;
            for (int x = 0; x < nw; x++) {
                int sx = x * pt.w / nw;
                const unsigned char *p = src + ((size_t)sy * pt.w + sx) * 4;
                unsigned char *o = dst + ((size_t)y * nw + x) * 4;
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
            }
        }
        free(pt.layer[li].rgba);
        pt.layer[li].rgba = dst;
    }
    pt.w = nw;
    pt.h = nh;
    clear_history();
    pt.dirty = 1;
    update_title();
    update_status();
    w2k_win_dirty(pt.win);
}

static void do_text_at(int dx, int dy)
{
    char buf[256] = "";
    if (!w2k_prompt(pt.win, "Text", "Text:", "", buf, sizeof buf, ICO_NONE))
        return;
    if (!buf[0]) return;
    push_undo();
    draw_text_string(dx, dy, buf, pt.fg[0], pt.fg[1], pt.fg[2]);
    pt.dirty = 1;
    update_title();
    w2k_win_dirty(pt.win);
}

/* ---- layout / paint ------------------------------------------------- */

static void layout(W2kWin *w)
{
    int y = 0;
    if (pt.mb) { pt.mb->r = (W2kRect){ 0, 0, w->w, MENUBAR_H }; y += MENUBAR_H; }
    if (pt.tb) { pt.tb->r = (W2kRect){ 0, y, w->w, TOOLBAR_H }; y += TOOLBAR_H; }
    int bot = (pt.sb ? STATUS_H : 0) + PALETTE_H + COLORBOX_H;
    pt.layer_r = (W2kRect){ w->w - LAYER_W, y, LAYER_W, w->h - y - bot };
    if (pt.layer_r.h < 1) pt.layer_r.h = 1;
    pt.canvas_r = (W2kRect){
        TOOLBOX_W, y,
        w->w - TOOLBOX_W - LAYER_W,
        w->h - y - bot
    };
    if (pt.canvas_r.w < 1) pt.canvas_r.w = 1;
    if (pt.canvas_r.h < 1) pt.canvas_r.h = 1;
    pt.palette_r = (W2kRect){
        0, w->h - (pt.sb ? STATUS_H : 0) - PALETTE_H,
        w->w, PALETTE_H
    };
    if (pt.sb)
        pt.sb->r = (W2kRect){ 0, w->h - STATUS_H, w->w, STATUS_H };
}

static void composite_px(int dx, int dy, int *r, int *g, int *b)
{
    *r = *g = *b = 255;
    for (int li = 0; li < pt.nlayers; li++) {
        if (!pt.layer[li].visible || !pt.layer[li].rgba) continue;
        const unsigned char *p =
            pt.layer[li].rgba + ((size_t)dy * pt.w + dx) * 4;
        int a = p[3];
        if (a == 0) continue;
        if (a == 255) { *r = p[0]; *g = p[1]; *b = p[2]; }
        else {
            *r = (p[0] * a + *r * (255 - a)) / 255;
            *g = (p[1] * a + *g * (255 - a)) / 255;
            *b = (p[2] * a + *b * (255 - a)) / 255;
        }
    }
}

static void blit_visible(Drawable d)
{
    W2kRect r = pt.canvas_r;
    w2k_fill(d, r.x, r.y, r.w, r.h, C_APPWORKSPACE);
    if (pt.w < 1 || pt.h < 1 || pt.zoom < 1) return;

    int doc_x0 = pt.pan_x, doc_y0 = pt.pan_y;
    int vis_w = (r.w + pt.zoom - 1) / pt.zoom;
    int vis_h = (r.h + pt.zoom - 1) / pt.zoom;
    if (doc_x0 < 0) { r.x += (-doc_x0) * pt.zoom; vis_w += doc_x0; doc_x0 = 0; }
    if (doc_y0 < 0) { r.y += (-doc_y0) * pt.zoom; vis_h += doc_y0; doc_y0 = 0; }
    if (doc_x0 >= pt.w || doc_y0 >= pt.h || vis_w <= 0 || vis_h <= 0) return;
    if (doc_x0 + vis_w > pt.w) vis_w = pt.w - doc_x0;
    if (doc_y0 + vis_h > pt.h) vis_h = pt.h - doc_y0;

    int out_w = vis_w * pt.zoom;
    int out_h = vis_h * pt.zoom;
    if (out_w > pt.canvas_r.w - (r.x - pt.canvas_r.x))
        out_w = pt.canvas_r.w - (r.x - pt.canvas_r.x);
    if (out_h > pt.canvas_r.h - (r.y - pt.canvas_r.y))
        out_h = pt.canvas_r.h - (r.y - pt.canvas_r.y);
    if (out_w <= 0 || out_h <= 0) return;

    /* The picture is put up in screen pixels: on a scaled desktop each
     * screen pixel maps back to a logical one, then to the document. */
    int px0 = w2k_cx(r.x), py0 = w2k_cx(r.y);
    int pw = w2k_cw(r.x, out_w), ph = w2k_cw(r.y, out_h);
    if (pw <= 0 || ph <= 0) return;
    char *pixels = malloc((size_t)pw * ph * 4);
    if (!pixels) return;
    XImage *xi = XCreateImage(w2k.dpy, w2k.visual, w2k.depth, ZPixmap, 0,
                              pixels, (unsigned)pw, (unsigned)ph, 32, 0);
    if (!xi) { free(pixels); return; }

    int sc = w2k_ui_scale;
    for (int y = 0; y < ph; y++) {
        int ly = (int)((long)y * 100 / sc);
        int dy = doc_y0 + ly / pt.zoom;
        if (dy >= pt.h) dy = pt.h - 1;
        for (int x = 0; x < pw; x++) {
            int lx = (int)((long)x * 100 / sc);
            int dx = doc_x0 + lx / pt.zoom;
            if (dx >= pt.w) dx = pt.w - 1;
            int cr, cg, cb;
            composite_px(dx, dy, &cr, &cg, &cb);
            XPutPixel(xi, x, y, w2k_rgb(cr, cg, cb));
        }
    }
    XPutImage(w2k.dpy, d, w2k.gc, xi, 0, 0, px0, py0, (unsigned)pw, (unsigned)ph);
    XDestroyImage(xi);
    w2k_edge(d, r.x - 1, r.y - 1, out_w + 2, out_h + 2, EDGE_SUNKEN_THIN, BF_RECT);
}

static void paint_toolbox(Drawable d, int y0)
{
    static const char *lab[] = { "P", "B", "E", "/", "R", "O", "F", "I", "A" };
    static const int tools[] = {
        T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_PICK, T_TEXT
    };
    int cell = 22, fh = w2k_font_height(F_UI);
    for (int i = 0; i < 9; i++) {
        int col = i % 2, row = i / 2;
        int x = 4 + col * (cell + 2);
        int y = y0 + 4 + row * (cell + 2);
        int sel = (pt.tool == tools[i]);
        w2k_button(d, x, y, cell, cell, sel);
        int tw = w2k_text_width(F_UI, lab[i], -1);
        w2k_text(d, F_UI, x + (cell - tw) / 2, y + (cell - fh) / 2, lab[i], C_TEXT);
    }
    int by = y0 + 4 + 5 * (cell + 2) + 4;
    w2k_text(d, F_UI, 6, by, "Size", C_TEXT);
    by += fh + 2;
    for (int i = 0; i < 4; i++) {
        int s = i + 1;
        int x = 6 + i * 11;
        int sel = (pt.brush == i);
        w2k_fill(d, x, by, 10, 10, sel ? C_HIGHLIGHT : C_WINDOW);
        w2k_edge(d, x, by, 10, 10, EDGE_SUNKEN_THIN, BF_RECT);
        w2k_fill(d, x + 5 - s / 2, by + 5 - s / 2, s, s, C_TEXT);
    }
}

static void paint_colorbox(Drawable d, int y0)
{
    int x = 8;
    w2k_fill_rgb(d, x + 10, y0 + 16, 26, 26, pt.bg[0], pt.bg[1], pt.bg[2]);
    w2k_edge(d, x + 10, y0 + 16, 26, 26, EDGE_SUNKEN_THIN, BF_RECT);
    w2k_fill_rgb(d, x, y0 + 6, 26, 26, pt.fg[0], pt.fg[1], pt.fg[2]);
    w2k_edge(d, x, y0 + 6, 26, 26, EDGE_RAISED, BF_RECT);
}

static void paint_palette(Drawable d)
{
    W2kRect r = pt.palette_r;
    w2k_fill(d, r.x, r.y, r.w, r.h, C_FACE);
    w2k_edge(d, r.x, r.y, r.w, 1, EDGE_SUNKEN_THIN, BF_TOP);
    int cell = 14, gap = 2;
    int x0 = TOOLBOX_W + 4;
    int y0 = r.y + (r.h - 2 * (cell + gap)) / 2;
    for (int i = 0; i < 28; i++) {
        int col = i % 14, row = i / 14;
        int x = x0 + col * (cell + gap);
        int y = y0 + row * (cell + gap);
        w2k_fill_rgb(d, x, y, cell, cell,
                     k_palette[i][0], k_palette[i][1], k_palette[i][2]);
        w2k_edge(d, x, y, cell, cell, EDGE_SUNKEN_THIN, BF_RECT);
    }
}

static void paint_layers(Drawable d)
{
    W2kRect r = pt.layer_r;
    w2k_fill(d, r.x, r.y, r.w, r.h, C_FACE);
    w2k_edge(d, r.x, r.y, 1, r.h, EDGE_SUNKEN_THIN, BF_LEFT);
    int fh = w2k_font_height(F_UI);
    w2k_text(d, F_UI, r.x + 6, r.y + 4, "Layers", C_TEXT);

    int row_h = fh + 10;
    int y = r.y + fh + 10;
    /* Draw top layer first (highest index at top of list). */
    for (int i = pt.nlayers - 1; i >= 0; i--) {
        int sel = (i == pt.active);
        if (sel) w2k_fill(d, r.x + 2, y - 2, r.w - 4, row_h, C_HIGHLIGHT);
        /* Eye (visibility). */
        w2k_edge(d, r.x + 6, y + 2, 12, 12, EDGE_SUNKEN_THIN, BF_RECT);
        if (pt.layer[i].visible)
            w2k_fill(d, r.x + 9, y + 5, 6, 6, C_TEXT);
        int tc = sel ? C_HILIGHT : C_TEXT;
        w2k_text(d, F_UI, r.x + 22, y + 2, pt.layer[i].name, tc);
        y += row_h;
    }
    /* Buttons */
    int by = r.y + r.h - 48;
    if (by < y + 4) by = y + 4;
    w2k_draw_pushbutton(d, &(W2kRect){ r.x + 4, by, 28, 20 }, "+", 0);
    w2k_draw_pushbutton(d, &(W2kRect){ r.x + 34, by, 28, 20 }, "-", 0);
    w2k_draw_pushbutton(d, &(W2kRect){ r.x + 4, by + 22, 28, 20 }, "↑", 0);
    w2k_draw_pushbutton(d, &(W2kRect){ r.x + 34, by + 22, 28, 20 }, "↓", 0);
}

static void on_paint(W2kWin *w, Drawable d)
{
    w2k_fill(d, 0, 0, w->w, w->h, C_FACE);
    if (pt.mb) w2k_menubar_draw(d, pt.mb);
    if (pt.tb) w2k_toolbar_draw(d, pt.tb);

    int y0 = (pt.mb ? MENUBAR_H : 0) + (pt.tb ? TOOLBAR_H : 0);
    w2k_fill(d, 0, y0, TOOLBOX_W, pt.canvas_r.h + COLORBOX_H, C_FACE);
    w2k_edge(d, TOOLBOX_W - 1, y0, 1, pt.canvas_r.h + COLORBOX_H,
             EDGE_SUNKEN_THIN, BF_LEFT);
    paint_toolbox(d, y0);

    int cy = pt.palette_r.y - COLORBOX_H;
    w2k_fill(d, 0, cy, TOOLBOX_W + 60, COLORBOX_H, C_FACE);
    paint_colorbox(d, cy);

    blit_visible(d);
    paint_layers(d);
    paint_palette(d);
    if (pt.sb) w2k_status_draw(d, pt.sb);
}

/* ---- hit testing ---------------------------------------------------- */

static int tool_at(int x, int y)
{
    int y0 = (pt.mb ? MENUBAR_H : 0) + (pt.tb ? TOOLBAR_H : 0);
    int cell = 22;
    for (int i = 0; i < 9; i++) {
        int col = i % 2, row = i / 2;
        int tx = 4 + col * (cell + 2);
        int ty = y0 + 4 + row * (cell + 2);
        if (x >= tx && x < tx + cell && y >= ty && y < ty + cell) return i;
    }
    return -1;
}

static int size_at(int x, int y)
{
    int y0 = (pt.mb ? MENUBAR_H : 0) + (pt.tb ? TOOLBAR_H : 0);
    int cell = 22, fh = w2k_font_height(F_UI);
    int by = y0 + 4 + 5 * (cell + 2) + 4 + fh + 2;
    for (int i = 0; i < 4; i++) {
        int sx = 6 + i * 11;
        if (x >= sx && x < sx + 10 && y >= by && y < by + 10) return i;
    }
    return -1;
}

static int palette_at(int x, int y)
{
    int cell = 14, gap = 2;
    int x0 = TOOLBOX_W + 4;
    int y0 = pt.palette_r.y + (pt.palette_r.h - 2 * (cell + gap)) / 2;
    for (int i = 0; i < 28; i++) {
        int col = i % 14, row = i / 14;
        int px = x0 + col * (cell + gap);
        int py = y0 + row * (cell + gap);
        if (x >= px && x < px + cell && y >= py && y < py + cell) return i;
    }
    return -1;
}

/* Layer panel: returns 0=none, 1=row(idx), 2=+, 3=-, 4=up, 5=down, 6=eye */
static int layer_hit(int x, int y, int *idx)
{
    W2kRect r = pt.layer_r;
    if (!w2k_rect_hit(&r, x, y)) return 0;
    int fh = w2k_font_height(F_UI);
    int row_h = fh + 10;
    int ly = r.y + fh + 10;
    for (int i = pt.nlayers - 1; i >= 0; i--) {
        if (y >= ly - 2 && y < ly - 2 + row_h) {
            *idx = i;
            if (x >= r.x + 6 && x < r.x + 18) return 6; /* eye */
            return 1;
        }
        ly += row_h;
    }
    int by = r.y + r.h - 48;
    if (by < ly + 4) by = ly + 4;
    if (y >= by && y < by + 20) {
        if (x >= r.x + 4 && x < r.x + 32) return 2;
        if (x >= r.x + 34 && x < r.x + 62) return 3;
    }
    if (y >= by + 22 && y < by + 42) {
        if (x >= r.x + 4 && x < r.x + 32) return 4;
        if (x >= r.x + 34 && x < r.x + 62) return 5;
    }
    return 0;
}

static void apply_tool_drag(int finalize)
{
    int rad = pt.brush;
    int erase = (pt.tool == T_ERASER);
    int *c = pt.fg;
    if (pt.tool == T_LINE || pt.tool == T_RECT || pt.tool == T_ELLIPSE) {
        stroke_restore();
        if (pt.tool == T_LINE)
            draw_line(pt.x0, pt.y0, pt.x1, pt.y1, rad, c[0], c[1], c[2], erase);
        else if (pt.tool == T_RECT)
            draw_rect(pt.x0, pt.y0, pt.x1, pt.y1, rad, c[0], c[1], c[2], erase);
        else
            draw_ellipse(pt.x0, pt.y0, pt.x1, pt.y1, rad, c[0], c[1], c[2], erase);
        if (finalize) stroke_free();
    }
}

/* ---- commands / menus ----------------------------------------------- */

static void command(void *user, int id)
{
    (void)user;
    switch (id) {
    case ID_NEW: do_new(); break;
    case ID_OPEN: do_open(); break;
    case ID_SAVE: do_save(0); break;
    case ID_SAVEAS: do_save(1); break;
    case ID_PROPS: do_properties(); break;
    case ID_SCALE: do_scale(); break;
    case ID_EXIT: w2k_win_close(pt.win, 0); break;
    case ID_UNDO: do_undo(); break;
    case ID_REDO: do_redo(); break;
    case ID_ZOOMIN:
        if (pt.zoom < MAX_ZOOM) { pt.zoom *= 2; update_status(); w2k_win_dirty(pt.win); }
        break;
    case ID_ZOOMOUT:
        if (pt.zoom > MIN_ZOOM) { pt.zoom /= 2; update_status(); w2k_win_dirty(pt.win); }
        break;
    case ID_ZOOM100:
        pt.zoom = 1; pt.pan_x = pt.pan_y = 0;
        update_status(); w2k_win_dirty(pt.win);
        break;
    case ID_TOOL_PENCIL:  pt.tool = T_PENCIL;  update_status(); w2k_win_dirty(pt.win); break;
    case ID_TOOL_BRUSH:   pt.tool = T_BRUSH;   update_status(); w2k_win_dirty(pt.win); break;
    case ID_TOOL_ERASER:  pt.tool = T_ERASER;  update_status(); w2k_win_dirty(pt.win); break;
    case ID_TOOL_LINE:    pt.tool = T_LINE;    update_status(); w2k_win_dirty(pt.win); break;
    case ID_TOOL_RECT:    pt.tool = T_RECT;    update_status(); w2k_win_dirty(pt.win); break;
    case ID_TOOL_ELLIPSE: pt.tool = T_ELLIPSE; update_status(); w2k_win_dirty(pt.win); break;
    case ID_TOOL_FILL:    pt.tool = T_FILL;    update_status(); w2k_win_dirty(pt.win); break;
    case ID_TOOL_PICK:    pt.tool = T_PICK;    update_status(); w2k_win_dirty(pt.win); break;
    case ID_TOOL_TEXT:    pt.tool = T_TEXT;    update_status(); w2k_win_dirty(pt.win); break;
    case ID_COLOR_FG:
        if (w2k_color_picker(pt.win, &pt.fg[0], &pt.fg[1], &pt.fg[2]))
            w2k_win_dirty(pt.win);
        break;
    case ID_COLOR_BG:
        if (w2k_color_picker(pt.win, &pt.bg[0], &pt.bg[1], &pt.bg[2]))
            w2k_win_dirty(pt.win);
        break;
    case ID_ABOUT:
        w2k_msgbox(pt.win, "About Paint",
                   "Paint\n\nA simple image editor for this desktop.",
                   MB_OK | MB_ICONINFO);
        break;
    case ID_LAYER_NEW:
        if (layer_add()) { pt.dirty = 1; update_title(); update_status(); w2k_win_dirty(pt.win); }
        break;
    case ID_LAYER_DEL:
        if (pt.nlayers > 1) {
            layer_delete(pt.active);
            pt.dirty = 1; update_title(); update_status(); w2k_win_dirty(pt.win);
        }
        break;
    case ID_LAYER_UP:
        if (pt.active + 1 < pt.nlayers) {
            layer_swap(pt.active, pt.active + 1);
            update_status(); w2k_win_dirty(pt.win);
        }
        break;
    case ID_LAYER_DOWN:
        if (pt.active > 0) {
            layer_swap(pt.active, pt.active - 1);
            update_status(); w2k_win_dirty(pt.win);
        }
        break;
    case ID_LAYER_PREV:
        if (pt.active > 0) { pt.active--; update_status(); w2k_win_dirty(pt.win); }
        break;
    case ID_LAYER_NEXT:
        if (pt.active + 1 < pt.nlayers) { pt.active++; update_status(); w2k_win_dirty(pt.win); }
        break;
    case ID_LAYER_TOGGLE:
        pt.layer[pt.active].visible = !pt.layer[pt.active].visible;
        update_status(); w2k_win_dirty(pt.win);
        break;
    }
}

static W2kMenu *build_file(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_NEW, "&New", "Ctrl+N", ICO_NONE);
    w2k_menu_item(m, ID_OPEN, "&Open...", "Ctrl+O", ICO_FOLDER_OPEN);
    w2k_menu_item(m, ID_SAVE, "&Save", "Ctrl+S", ICO_FILE_BITMAP);
    w2k_menu_item(m, ID_SAVEAS, "Save &As...", NULL, ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_PROPS, "A&ttributes...", "Ctrl+E", ICO_NONE);
    w2k_menu_item(m, ID_SCALE, "S&tretch/Skew...", NULL, ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_EXIT, "E&xit", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_edit(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_UNDO, "&Undo", "Ctrl+Z", ICO_NONE);
    w2k_menu_item(m, ID_REDO, "&Redo", "Ctrl+Y", ICO_NONE);
    return m;
}

static W2kMenu *build_view(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_ZOOMIN, "Zoom &In", "Ctrl++", ICO_NONE);
    w2k_menu_item(m, ID_ZOOMOUT, "Zoom &Out", "Ctrl+-", ICO_NONE);
    w2k_menu_item(m, ID_ZOOM100, "&Actual Size", "Ctrl+0", ICO_NONE);
    return m;
}

static W2kMenu *build_image(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_LAYER_NEW, "&New Layer", "Ctrl+Shift+N", ICO_NONE);
    w2k_menu_item(m, ID_LAYER_DEL, "&Delete Layer", NULL, ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_LAYER_UP, "Move Layer &Up", NULL, ICO_NONE);
    w2k_menu_item(m, ID_LAYER_DOWN, "Move Layer &Down", NULL, ICO_NONE);
    w2k_menu_item(m, ID_LAYER_TOGGLE, "&Show/Hide Layer", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_colors(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_COLOR_FG, "&Foreground Color...", NULL, ICO_NONE);
    w2k_menu_item(m, ID_COLOR_BG, "&Background Color...", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_help(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_ABOUT, "&About Paint", NULL, ICO_NONE);
    return m;
}

/* ---- events --------------------------------------------------------- */

static int on_event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_menubar_press(pt.mb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_toolbar_press(pt.tb, &e->xbutton)) { w2k_win_dirty(w); return 1; }

        /* Shift+LMB or MMB = pan */
        if ((e->xbutton.button == Button2 ||
             (e->xbutton.button == Button1 && (e->xbutton.state & ShiftMask))) &&
            w2k_rect_hit(&pt.canvas_r, x, y)) {
            pt.panning = 1;
            pt.pan_mx = x; pt.pan_my = y;
            pt.pan_ox = pt.pan_x; pt.pan_oy = pt.pan_y;
            return 1;
        }

        if (e->xbutton.button == Button4 || e->xbutton.button == Button5) {
            if (w2k_rect_hit(&pt.canvas_r, x, y)) {
                int dx, dy;
                screen_to_doc(x, y, &dx, &dy);
                int old = pt.zoom;
                if (e->xbutton.button == Button4 && pt.zoom < MAX_ZOOM) pt.zoom *= 2;
                if (e->xbutton.button == Button5 && pt.zoom > MIN_ZOOM) pt.zoom /= 2;
                if (pt.zoom != old) {
                    pt.pan_x = dx - (x - pt.canvas_r.x) / pt.zoom;
                    pt.pan_y = dy - (y - pt.canvas_r.y) / pt.zoom;
                    update_status();
                    w2k_win_dirty(w);
                }
            }
            return 1;
        }

        int ti = tool_at(x, y);
        if (ti >= 0) {
            static const int tools[] = {
                T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT,
                T_ELLIPSE, T_FILL, T_PICK, T_TEXT
            };
            pt.tool = tools[ti];
            update_status();
            w2k_win_dirty(w);
            return 1;
        }
        int si = size_at(x, y);
        if (si >= 0) { pt.brush = si; w2k_win_dirty(w); return 1; }

        int pi = palette_at(x, y);
        if (pi >= 0) {
            if (e->xbutton.button == Button3) {
                pt.bg[0] = k_palette[pi][0];
                pt.bg[1] = k_palette[pi][1];
                pt.bg[2] = k_palette[pi][2];
            } else {
                pt.fg[0] = k_palette[pi][0];
                pt.fg[1] = k_palette[pi][1];
                pt.fg[2] = k_palette[pi][2];
            }
            w2k_win_dirty(w);
            return 1;
        }

        /* FG/BG boxes open the HSV picker. */
        {
            int cy = pt.palette_r.y - COLORBOX_H;
            if (y >= cy && y < pt.palette_r.y && x < TOOLBOX_W + 50) {
                if (e->xbutton.button == Button3) command(NULL, ID_COLOR_BG);
                else command(NULL, ID_COLOR_FG);
                return 1;
            }
        }

        int lidx = 0;
        int lh = layer_hit(x, y, &lidx);
        if (lh) {
            if (lh == 1) { pt.active = lidx; update_status(); }
            else if (lh == 2) command(NULL, ID_LAYER_NEW);
            else if (lh == 3) command(NULL, ID_LAYER_DEL);
            else if (lh == 4) command(NULL, ID_LAYER_UP);
            else if (lh == 5) command(NULL, ID_LAYER_DOWN);
            else if (lh == 6) {
                pt.layer[lidx].visible = !pt.layer[lidx].visible;
                update_status();
            }
            w2k_win_dirty(w);
            return 1;
        }

        if (!w2k_rect_hit(&pt.canvas_r, x, y) || pt.w < 1) return 1;

        int dx, dy;
        screen_to_doc(x, y, &dx, &dy);

        if (pt.tool == T_PICK) {
            pick_px(dx, dy, &pt.fg[0], &pt.fg[1], &pt.fg[2]);
            w2k_win_dirty(w);
            return 1;
        }
        if (pt.tool == T_TEXT) {
            do_text_at(dx, dy);
            return 1;
        }
        if (pt.tool == T_FILL) {
            push_undo();
            int *c = (e->xbutton.button == Button3) ? pt.bg : pt.fg;
            flood_fill(dx, dy, c[0], c[1], c[2]);
            pt.dirty = 1;
            update_title();
            w2k_win_dirty(w);
            return 1;
        }

        push_undo();
        pt.drawing = 1;
        pt.x0 = pt.x1 = dx;
        pt.y0 = pt.y1 = dy;
        if (pt.tool == T_LINE || pt.tool == T_RECT || pt.tool == T_ELLIPSE) {
            stroke_save_full();
        } else {
            int erase = (pt.tool == T_ERASER);
            int *c = (e->xbutton.button == Button3) ? pt.bg : pt.fg;
            int rad = (pt.tool == T_PENCIL) ? 0 : pt.brush;
            stamp(dx, dy, rad, c[0], c[1], c[2], erase);
        }
        pt.dirty = 1;
        update_title();
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (pt.panning) {
            int dx = e->xmotion.x - pt.pan_mx;
            int dy = e->xmotion.y - pt.pan_my;
            pt.pan_x = pt.pan_ox - dx / (pt.zoom > 0 ? pt.zoom : 1);
            pt.pan_y = pt.pan_oy - dy / (pt.zoom > 0 ? pt.zoom : 1);
            w2k_win_dirty(w);
            return 1;
        }
        if (pt.drawing) {
            int dx, dy;
            screen_to_doc(e->xmotion.x, e->xmotion.y, &dx, &dy);
            if (pt.tool == T_PENCIL || pt.tool == T_BRUSH || pt.tool == T_ERASER) {
                int erase = (pt.tool == T_ERASER);
                int rad = (pt.tool == T_PENCIL) ? 0 : pt.brush;
                draw_line(pt.x1, pt.y1, dx, dy, rad,
                          pt.fg[0], pt.fg[1], pt.fg[2], erase);
                pt.x1 = dx; pt.y1 = dy;
            } else {
                pt.x1 = dx; pt.y1 = dy;
                apply_tool_drag(0);
            }
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_toolbar_motion(pt.tb, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease:
        if (pt.panning) { pt.panning = 0; return 1; }
        w2k_toolbar_release(pt.tb);
        if (pt.drawing) {
            if (pt.tool == T_LINE || pt.tool == T_RECT || pt.tool == T_ELLIPSE)
                apply_tool_drag(1);
            pt.drawing = 0;
            w2k_win_dirty(w);
        }
        return 1;
    case KeyPress: {
        if (w2k_menubar_key(pt.mb, &e->xkey)) { w2k_win_dirty(w); return 1; }
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (e->xkey.state & ControlMask) {
            if (e->xkey.state & ShiftMask) {
                if (ks == XK_n || ks == XK_N) { command(NULL, ID_LAYER_NEW); return 1; }
            }
            switch (ks) {
            case XK_n: case XK_N: command(NULL, ID_NEW); return 1;
            case XK_o: case XK_O: command(NULL, ID_OPEN); return 1;
            case XK_s: case XK_S: command(NULL, ID_SAVE); return 1;
            case XK_e: case XK_E: command(NULL, ID_PROPS); return 1;
            case XK_z: case XK_Z: command(NULL, ID_UNDO); return 1;
            case XK_y: case XK_Y: command(NULL, ID_REDO); return 1;
            case XK_equal: case XK_plus: case XK_KP_Add:
                command(NULL, ID_ZOOMIN); return 1;
            case XK_minus: case XK_KP_Subtract:
                command(NULL, ID_ZOOMOUT); return 1;
            case XK_0: case XK_KP_0:
                command(NULL, ID_ZOOM100); return 1;
            }
        }
        if (ks == XK_Page_Up)   { command(NULL, ID_LAYER_PREV); return 1; }
        if (ks == XK_Page_Down) { command(NULL, ID_LAYER_NEXT); return 1; }
        int step = 32 / (pt.zoom > 0 ? pt.zoom : 1);
        if (step < 1) step = 1;
        if (ks == XK_Left)  { pt.pan_x -= step; w2k_win_dirty(w); return 1; }
        if (ks == XK_Right) { pt.pan_x += step; w2k_win_dirty(w); return 1; }
        if (ks == XK_Up)    { pt.pan_y -= step; w2k_win_dirty(w); return 1; }
        if (ks == XK_Down)  { pt.pan_y += step; w2k_win_dirty(w); return 1; }
        return 0;
    }
    case ClientMessage:
        if ((Atom)e->xclient.data.l[0] == w2k.a_wm_delete) {
            int c = confirm_discard();
            if (c == 0) return 1;
            if (c < 0 && !do_save(0)) return 1;
            w2k_win_close(w, 0);
            return 1;
        }
        break;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (w2k_init("l2kpaint") < 0) return 1;
    memset(&pt, 0, sizeof pt);
    pt.tool = T_PENCIL;
    pt.brush = 1;
    pt.zoom = 1;
    pt.fg[0] = pt.fg[1] = pt.fg[2] = 0;
    pt.bg[0] = pt.bg[1] = pt.bg[2] = 255;
    pt.untitled = 1;
    if (!layers_init(640, 480)) return 1;

    if (argc > 1) {
        int w = 0, h = 0;
        unsigned char *rgba = w2k_image_load(argv[1], &w, &h);
        if (rgba && w > 0 && h > 0) {
            layers_init(w, h);
            memcpy(pt.layer[0].rgba, rgba, (size_t)w * h * 4);
            free(rgba);
            snprintf(pt.path, sizeof pt.path, "%s", argv[1]);
            pt.untitled = 0;
        } else {
            free(rgba);
        }
    }

    pt.win = w2k_win_new("Paint", "l2kpaint", 900, 600, 0);
    pt.win->paint = on_paint;
    pt.win->event = on_event;
    pt.win->resized = layout;
    pt.win->min_w = 480;
    pt.win->min_h = 360;

    pt.mb = w2k_menubar_new(NULL, command);
    pt.mb->win_ref = pt.win->win;
    w2k_menubar_add(pt.mb, "&File", build_file);
    w2k_menubar_add(pt.mb, "&Edit", build_edit);
    w2k_menubar_add(pt.mb, "&View", build_view);
    w2k_menubar_add(pt.mb, "&Image", build_image);
    w2k_menubar_add(pt.mb, "&Colors", build_colors);
    w2k_menubar_add(pt.mb, "&Help", build_help);

    pt.tb = w2k_toolbar_new(NULL, command);
    w2k_toolbar_add(pt.tb, ID_NEW, ICO_NONE, "New");
    w2k_toolbar_add(pt.tb, ID_OPEN, ICO_FOLDER_OPEN, "Open");
    w2k_toolbar_add(pt.tb, ID_SAVE, ICO_FILE_BITMAP, "Save");
    w2k_toolbar_sep(pt.tb);
    w2k_toolbar_add(pt.tb, ID_UNDO, ICO_NONE, "Undo");
    w2k_toolbar_add(pt.tb, ID_REDO, ICO_NONE, "Redo");
    w2k_toolbar_sep(pt.tb);
    w2k_toolbar_add(pt.tb, ID_ZOOMIN, ICO_NONE, "+");
    w2k_toolbar_add(pt.tb, ID_ZOOMOUT, ICO_NONE, "-");
    w2k_toolbar_sep(pt.tb);
    w2k_toolbar_add(pt.tb, ID_TOOL_PENCIL, ICO_NONE, "P");
    w2k_toolbar_add(pt.tb, ID_TOOL_BRUSH, ICO_NONE, "B");
    w2k_toolbar_add(pt.tb, ID_TOOL_ERASER, ICO_NONE, "E");
    w2k_toolbar_add(pt.tb, ID_TOOL_FILL, ICO_NONE, "F");
    w2k_toolbar_add(pt.tb, ID_TOOL_TEXT, ICO_NONE, "A");

    pt.sb = w2k_status_new();
    w2k_status_add(pt.sb, 120);
    w2k_status_add(pt.sb, 50);
    w2k_status_add(pt.sb, 220);

    layout(pt.win);
    update_title();
    update_status();
    w2k_win_show(pt.win);
    w2k_run();

    layers_clear();
    stroke_free();
    clear_history();
    return 0;
}
