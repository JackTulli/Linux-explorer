/* w2kcharmap.c -- Character Map.
 *
 * A grid of every character in a font, a box of the ones you have picked,
 * and a Copy button. The grid is the interesting part: characters are
 * addressed by code point and drawn as UTF-8, which the toolkit can do now
 * that text goes through Xft -- with the old core fonts this program could
 * not have existed above Latin-1.
 *
 * Which code points a font actually has is asked of fontconfig, so the
 * grid shows what is there rather than a wall of missing-glyph boxes. */
#include "w2k.h"
#include "w2kui.h"
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS       20
#define CELL       22
#define STATUS_H   20
#define GLYPH_PX   16    /* the grid draws the font, not the shell font */
#define PREVIEW_PX 60    /* held down, a cell blows up to this */

static struct {
    W2kWin   *win;
    W2kCombo *font;
    W2kEdit  *pick;              /* characters to copy */
    W2kScroll sb;
    W2kStatus *status;
    W2kRect   grid, copy_btn, select_btn;

    char      family[128];
    W2kFace  *face;              /* the font being shown, at grid size */
    W2kFace  *big;               /* the same, enlarged for the preview */
    int       preview;           /* index shown enlarged, -1 for none */
    unsigned  cp[65536];         /* the code points this font has */
    int       ncp;
    int       sel;               /* index into cp[], -1 for none */
    int       down;
} cm;

/* One code point as UTF-8. */
static int utf8(unsigned c, char *out)
{
    if (c < 0x80)    { out[0] = (char)c; out[1] = 0; return 1; }
    if (c < 0x800)   { out[0] = (char)(0xc0 | (c >> 6));
                       out[1] = (char)(0x80 | (c & 0x3f)); out[2] = 0; return 2; }
    if (c < 0x10000) { out[0] = (char)(0xe0 | (c >> 12));
                       out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
                       out[2] = (char)(0x80 | (c & 0x3f)); out[3] = 0; return 3; }
    out[0] = (char)(0xf0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    out[4] = 0;
    return 4;
}

/* Ask fontconfig which characters the chosen family covers, then open the
 * face itself and keep only the characters it can really draw -- the two
 * need not agree, and a mismatch shows up as a grid of empty boxes. */
static void load_coverage(const char *family)
{
    cm.ncp = 0;
    cm.sel = -1;
    cm.preview = -1;

    w2k_face_close(cm.face);
    w2k_face_close(cm.big);
    cm.face = w2k_face_open(family, GLYPH_PX);
    cm.big = w2k_face_open(family, PREVIEW_PX);

    FcPattern *pat = FcNameParse((const FcChar8 *)family);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *match = FcFontMatch(NULL, pat, &res);
    FcPatternDestroy(pat);
    if (!match) return;

    FcCharSet *cs = NULL;
    if (FcPatternGetCharSet(match, FC_CHARSET, 0, &cs) == FcResultMatch && cs) {
        FcChar32 map[FC_CHARSET_MAP_SIZE], next, ucs4;
        ucs4 = FcCharSetFirstPage(cs, map, &next);
        while (ucs4 != FC_CHARSET_DONE && cm.ncp < 65536) {
            for (int i = 0; i < FC_CHARSET_MAP_SIZE && cm.ncp < 65536; i++)
                for (int b = 0; b < 32 && cm.ncp < 65536; b++)
                    if (map[i] & (1u << b)) {
                        unsigned c = ucs4 + (unsigned)(i * 32 + b);
                        /* Control characters have nothing to show. */
                        if (c >= 0x20 && !(c >= 0x7f && c < 0xa0) &&
                            (!cm.face || w2k_face_has(cm.face, c)))
                            cm.cp[cm.ncp++] = c;
                    }
            ucs4 = FcCharSetNextPage(cs, map, &next);
        }
    }
    FcPatternDestroy(match);

    cm.sb.total = (cm.ncp + COLS - 1) / COLS;
    cm.sb.pos = 0;
}

static void fill_fonts(void)
{
    w2k_combo_clear(cm.font);

    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, (char *)NULL);
    FcFontSet *set = FcFontList(NULL, pat, os);
    FcPatternDestroy(pat);
    FcObjectSetDestroy(os);
    if (!set) return;

    /* Sorted and de-duplicated: fontconfig lists a family once per style. */
    char names[512][128];
    int n = 0;
    for (int i = 0; i < set->nfont && n < 512; i++) {
        FcChar8 *fam = NULL;
        if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch)
            continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (!strcmp(names[k], (const char *)fam)) { dup = 1; break; }
        if (!dup) snprintf(names[n++], 128, "%.127s", (const char *)fam);
    }
    FcFontSetDestroy(set);

    for (int i = 1; i < n; i++) {
        char tmp[128];
        snprintf(tmp, sizeof tmp, "%s", names[i]);
        int k = i - 1;
        while (k >= 0 && strcasecmp(names[k], tmp) > 0) {
            snprintf(names[k + 1], 128, "%s", names[k]);
            k--;
        }
        snprintf(names[k + 1], 128, "%s", tmp);
    }
    for (int i = 0; i < n; i++) {
        w2k_combo_add(cm.font, names[i]);
        if (!strcasecmp(names[i], cm.family)) cm.font->sel = i;
    }
    if (cm.font->sel < 0 && n) cm.font->sel = 0;
}

static void set_status(void)
{
    char b[96];
    if (cm.sel >= 0 && cm.sel < cm.ncp) {
        char ch[8];
        utf8(cm.cp[cm.sel], ch);
        snprintf(b, sizeof b, "%s    U+%04X", ch, cm.cp[cm.sel]);
    } else {
        snprintf(b, sizeof b, "%d characters", cm.ncp);
    }
    w2k_status_set(cm.status, 0, b);
}

static void paint(W2kWin *w, Drawable d)
{
    int fh = w2k_font_height(F_UI);
    w2k_text_mnemonic(d, F_UI, 10, 12, "&Font:", C_TEXT, 1);
    w2k_combo_draw(d, cm.font);

    /* The grid. */
    w2k_edge(d, cm.grid.x, cm.grid.y, cm.grid.w, cm.grid.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, cm.grid.x + 2, cm.grid.y + 2, cm.grid.w - 4, cm.grid.h - 4,
             C_WINDOW);
    w2k_clip_set(cm.grid.x + 2, cm.grid.y + 2, cm.grid.w - 4, cm.grid.h - 4);

    int rows = (cm.grid.h - 4) / CELL;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < COLS; c++) {
            int idx = (cm.sb.pos + r) * COLS + c;
            if (idx >= cm.ncp) break;
            int x = cm.grid.x + 2 + c * CELL, y = cm.grid.y + 2 + r * CELL;
            if (idx == cm.sel) w2k_fill(d, x, y, CELL, CELL, C_HIGHLIGHT);
            /* The cell grid, drawn as light rules rather than boxes. */
            w2k_vline(d, x + CELL - 1, y, CELL, C_SCROLLBAR);
            w2k_hline(d, x, y + CELL - 1, CELL, C_SCROLLBAR);

            char ch[8];
            utf8(cm.cp[idx], ch);
            int col = idx == cm.sel ? C_HIGHLIGHTTEXT : C_WINDOWTEXT;
            if (cm.face) {
                int tw = w2k_face_width(cm.face, ch, -1);
                int th = w2k_face_height(cm.face);
                w2k_face_text(d, cm.face, x + (CELL - tw) / 2,
                              y + (CELL - th) / 2, ch, col);
            } else {
                int tw = w2k_text_width(F_UI, ch, -1);
                w2k_text(d, F_UI, x + (CELL - tw) / 2, y + (CELL - fh) / 2,
                         ch, col);
            }
        }
    }
    w2k_clip_clear();

    /* Held down, the character is shown enlarged over the grid, as the
     * original does -- at 22 pixels a cell you often cannot tell an
     * acute from a grave otherwise. */
    if (cm.preview >= 0 && cm.preview < cm.ncp && cm.big) {
        int r = cm.preview / COLS - cm.sb.pos, c = cm.preview % COLS;
        if (r >= 0 && r < rows) {
            int pw = PREVIEW_PX + 20, ph = PREVIEW_PX + 20;
            int px = cm.grid.x + 2 + c * CELL + CELL / 2 - pw / 2;
            int py = cm.grid.y + 2 + r * CELL + CELL / 2 - ph / 2;
            if (px < cm.grid.x) px = cm.grid.x;
            if (py < cm.grid.y) py = cm.grid.y;
            if (px + pw > cm.grid.x + cm.grid.w) px = cm.grid.x + cm.grid.w - pw;
            if (py + ph > cm.grid.y + cm.grid.h) py = cm.grid.y + cm.grid.h - ph;
            w2k_fill(d, px, py, pw, ph, C_WINDOW);
            w2k_frame(d, px, py, pw, ph, C_WINDOWFRAME);
            char ch[8];
            utf8(cm.cp[cm.preview], ch);
            int tw = w2k_face_width(cm.big, ch, -1);
            int th = w2k_face_height(cm.big);
            w2k_face_text(d, cm.big, px + (pw - tw) / 2, py + (ph - th) / 2,
                          ch, C_WINDOWTEXT);
        }
    }
    w2k_scroll_draw(d, &cm.sb);

    w2k_text_mnemonic(d, F_UI, 10, cm.pick->r.y - fh - 4,
                      "C&haracters to copy:", C_TEXT, 1);
    w2k_edit_draw(d, cm.pick);
    w2k_draw_pushbutton(d, &cm.select_btn, "&Select",
                        cm.down == 1 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &cm.copy_btn, "&Copy", cm.down == 2 ? BS_PRESSED : 0);
    w2k_status_draw(d, cm.status);
    (void)w;
}

static void append_selected(void)
{
    if (cm.sel < 0 || cm.sel >= cm.ncp) return;
    char ch[8];
    utf8(cm.cp[cm.sel], ch);
    char buf[512];
    snprintf(buf, sizeof buf, "%s%s", w2k_edit_text(cm.pick), ch);
    w2k_edit_set(cm.pick, buf);
}

static void on_font(void *u, int i)
{
    (void)u;
    if (i < 0 || i >= cm.font->n) return;
    snprintf(cm.family, sizeof cm.family, "%s", cm.font->items[i]);
    load_coverage(cm.family);
    set_status();
    w2k_win_dirty(cm.win);
}

static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_combo_press(cm.font, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_edit_press(cm.pick, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_scroll_needed(&cm.sb) && w2k_rect_hit(&cm.sb.r, x, y)) {
            w2k_scroll_press(&cm.sb, x, y);
            w2k_win_dirty(w);
            return 1;
        }
        if (e->xbutton.button == Button4 || e->xbutton.button == Button5) {
            w2k_scroll_wheel(&cm.sb, e->xbutton.button == Button4 ? -1 : 1);
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_rect_hit(&cm.grid, x, y)) {
            int c = (x - cm.grid.x - 2) / CELL, r = (y - cm.grid.y - 2) / CELL;
            int idx = (cm.sb.pos + r) * COLS + c;
            if (c >= 0 && c < COLS && idx >= 0 && idx < cm.ncp) {
                static Time last;
                static int lastidx = -1;
                int dbl = (idx == lastidx &&
                           (int)(e->xbutton.time - last) < w2k_dblclk_ms);
                cm.sel = idx;
                cm.preview = idx;
                if (dbl) { append_selected(); last = 0; lastidx = -1; }
                else     { last = e->xbutton.time; lastidx = idx; }
                set_status();
            }
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_rect_hit(&cm.select_btn, x, y)) cm.down = 1;
        else if (w2k_rect_hit(&cm.copy_btn, x, y)) cm.down = 2;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = cm.down, x = e->xbutton.x, y = e->xbutton.y;
        cm.down = 0;
        cm.preview = -1;
        w2k_scroll_release(&cm.sb);
        w2k_edit_release(cm.pick);
        if (b == 1 && w2k_rect_hit(&cm.select_btn, x, y)) append_selected();
        if (b == 2 && w2k_rect_hit(&cm.copy_btn, x, y))
            w2k_clipboard_set(w2k_edit_text(cm.pick));
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (cm.sb.pressed && w2k_scroll_motion(&cm.sb, e->xmotion.x, e->xmotion.y)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_edit_motion(cm.pick, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        if (cm.preview >= 0 && (e->xmotion.state & Button1Mask) &&
            w2k_rect_hit(&cm.grid, e->xmotion.x, e->xmotion.y)) {
            int c = (e->xmotion.x - cm.grid.x - 2) / CELL;
            int r = (e->xmotion.y - cm.grid.y - 2) / CELL;
            int idx = (cm.sb.pos + r) * COLS + c;
            if (c >= 0 && c < COLS && idx >= 0 && idx < cm.ncp && idx != cm.sel) {
                cm.sel = cm.preview = idx;
                set_status();
                w2k_win_dirty(w);
            }
            return 1;
        }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, 0); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { append_selected(); w2k_win_dirty(w); return 1; }
        if (ks == XK_Prior || ks == XK_Next) {
            cm.sb.pos += (ks == XK_Next ? cm.sb.page : -cm.sb.page);
            w2k_scroll_clamp(&cm.sb);
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_edit_key(cm.pick, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void resized(W2kWin *w)
{
    int fh = w2k_font_height(F_UI);
    cm.font->r = (W2kRect){ 56, 8, w->w - 66, 21 };
    cm.grid = (W2kRect){ 10, 38, COLS * CELL + 4, w->h - 38 - STATUS_H - 74 };
    cm.sb.r = (W2kRect){ cm.grid.x + cm.grid.w + 2, cm.grid.y, SCROLL_W,
                         cm.grid.h };
    cm.sb.vertical = 1;
    cm.sb.page = (cm.grid.h - 4) / CELL;
    cm.sb.line = 1;
    cm.pick->r = (W2kRect){ 10, cm.grid.y + cm.grid.h + 8 + fh + 4,
                            w->w - 20 - 160, 21 };
    cm.select_btn = (W2kRect){ w->w - 155, cm.pick->r.y, 70, 23 };
    cm.copy_btn = (W2kRect){ w->w - 80, cm.pick->r.y, 70, 23 };
    cm.status->r = (W2kRect){ 0, w->h - STATUS_H, w->w, STATUS_H };
}

int main(void)
{
    if (w2k_init("w2kcharmap") < 0) return 1;
    FcInit();

    snprintf(cm.family, sizeof cm.family, "Tahoma");
    cm.sel = -1;

    cm.win = w2k_win_new("Character Map", "w2kcharmap",
                         COLS * CELL + 24 + SCROLL_W, 420, 1);
    cm.win->paint = paint;
    cm.win->event = event;
    cm.win->resized = resized;

    cm.font = w2k_combo_new(0);
    cm.font->on_change = on_font;
    cm.pick = w2k_edit_new(0);
    w2k_edit_bind(cm.pick, cm.win);
    cm.status = w2k_status_new();
    w2k_status_add(cm.status, 0);
    cm.status->sizegrip = 1;
    w2k_scroll_bind(&cm.sb, cm.win);

    fill_fonts();
    if (cm.font->sel >= 0)
        snprintf(cm.family, sizeof cm.family, "%s", cm.font->items[cm.font->sel]);
    load_coverage(cm.family);

    resized(cm.win);
    set_status();
    w2k_win_center(cm.win, NULL);
    w2k_win_show(cm.win);
    w2k_run();
    w2k_face_close(cm.face);
    w2k_face_close(cm.big);
    w2k_fini();
    return 0;
}
