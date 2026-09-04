/* font.c -- text, through Xft where it is available.
 *
 * The toolkit used X core bitmap fonts, which meant Adobe Helvetica at 11
 * pixels: the wrong typeface (Windows 2000 draws its shell in Tahoma), no
 * antialiasing to switch on or off, and only Latin-1 -- a file named in
 * Greek or Japanese came out as rubbish. Xft fixes all three.
 *
 * Sizes are given in pixels, never points. Xft would otherwise scale text
 * by the screen's DPI, which on a 4K panel means an enormous shell; the
 * classic metrics are pixel counts and are meant to stay that way.
 *
 * If Xft cannot open a face -- a system with no fonts at all -- everything
 * falls back to the core font path, which is why both are still here. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <X11/Xft/Xft.h>

/* The clip rectangle draw.c keeps, mirrored into Xft. */
extern int w2k_clip_on, w2k_clip_x, w2k_clip_y, w2k_clip_w, w2k_clip_h;

/* Tahoma first, at the size Windows uses (8pt = 11 pixels): someone may
 * have the real thing installed. Otherwise the closest face this system
 * has, at the size that matches Tahoma's metrics -- measured against the
 * buttons of a Windows 2000 screenshot, where "OK" is 13 pixels wide,
 * "Cancel" is 31, and a line is 13 tall:
 *
 *     DejaVu Sans Condensed 10px   OK 13   Cancel 32   height 13
 *     Liberation Sans       11px   OK 15   Cancel 33   height 13
 *     DejaVu Sans           11px   OK 16   Cancel 38   height 14
 *
 * so the condensed cut at 10 pixels it is. Each candidate carries its own
 * size, because the right size depends on the face. */
typedef struct { const char *family; int pixel; } Face;

static const Face ui_faces[] = {
    { "Tahoma", 11 }, { "DejaVu Sans Condensed", 10 },
    { "Liberation Sans", 11 }, { "DejaVu Sans", 10 },
    { "Nimbus Sans", 11 }, { "sans-serif", 11 }, { NULL, 0 }
};
static const Face fixed_faces[] = {
    { "DejaVu Sans Mono", 12 }, { "Liberation Mono", 12 },
    { "Nimbus Mono PS", 12 }, { "monospace", 12 }, { NULL, 0 }
};

static const struct { const Face *faces; int bold; } face[N_FONTS] = {
    [F_UI]      = { ui_faces,    0 },
    [F_UI_BOLD] = { ui_faces,    1 },
    [F_FIXED]   = { fixed_faces, 0 },
    [F_ICON]    = { ui_faces,    0 },
};

static XftFont *xft[N_FONTS];
static int      use_xft;

/* F_ICON is F_UI under another name -- the shell draws icon labels in the
 * same face at the same size. Sharing the object saves a face. */
static int face_alias(int i) { return i == F_ICON ? F_UI : i; }

/* One XftDraw per drawable, kept: a paint draws dozens of strings into the
 * same pixmap, and creating the surface each time is the expensive part. */
#define NDRAW 4
static struct { Drawable d; XftDraw *draw; } draws[NDRAW];
static int draw_next;

static XftColor colours[N_COLORS];
static char     colour_ready[N_COLORS];

static XftDraw *draw_for(Drawable d)
{
    for (int i = 0; i < NDRAW; i++)
        if (draws[i].d == d && draws[i].draw) return draws[i].draw;

    int slot = draw_next++ % NDRAW;
    if (draws[slot].draw) XftDrawDestroy(draws[slot].draw);
    draws[slot].d = d;
    draws[slot].draw = XftDrawCreate(w2k.dpy, d, w2k.visual, w2k.cmap);
    return draws[slot].draw;
}

/* A drawable is about to be freed: forget any surface pointing at it. */
void w2k_font_forget(Drawable d)
{
    for (int i = 0; i < NDRAW; i++)
        if (draws[i].d == d) {
            if (draws[i].draw) XftDrawDestroy(draws[i].draw);
            draws[i].d = 0;
            draws[i].draw = NULL;
        }
}

static XftColor *colour_for(int c)
{
    if (c < 0 || c >= N_COLORS) c = C_TEXT;
    if (colour_ready[c]) return &colours[c];

    int r, g, b;
    w2k_color_rgb(c, &r, &g, &b);
    XRenderColor rc = { (unsigned short)(r * 257), (unsigned short)(g * 257),
                        (unsigned short)(b * 257), 0xffff };
    if (!XftColorAllocValue(w2k.dpy, w2k.visual, w2k.cmap, &rc, &colours[c]))
        return NULL;
    colour_ready[c] = 1;
    return &colours[c];
}

/* The palette changed: the allocated colours are stale. */
void w2k_font_colours_dirty(void)
{
    for (int i = 0; i < N_COLORS; i++) {
        if (!colour_ready[i]) continue;
        XftColorFree(w2k.dpy, w2k.visual, w2k.cmap, &colours[i]);
        colour_ready[i] = 0;
    }
}

static XftFont *open_face(int i)
{
    /* "Smooth edges of screen fonts" is exactly this flag. Off, glyphs are
     * rendered as crisp bitmaps, which is how Windows 2000 looked. */
    int aa = w2k_effects[FX_SMOOTH_FONTS] ? 1 : 0;
    for (int k = 0; face[i].faces[k].family; k++) {
        XftFont *f = XftFontOpen(w2k.dpy, w2k.screen,
                                 XFT_FAMILY, XftTypeString,
                                     face[i].faces[k].family,
                                 XFT_PIXEL_SIZE, XftTypeDouble,
                                     (double)face[i].faces[k].pixel,
                                 XFT_WEIGHT, XftTypeInteger,
                                     face[i].bold ? XFT_WEIGHT_BOLD : XFT_WEIGHT_MEDIUM,
                                 XFT_ANTIALIAS, XftTypeBool, aa,
                                 FC_HINTING, XftTypeBool, 1,
                                 FC_HINT_STYLE, XftTypeInteger, FC_HINT_FULL,
                                 NULL);
        if (!f) continue;

        /* fontconfig never says "no": asking for Tahoma on a system without
         * it returns whatever it thinks is closest. So check what actually
         * came back, and keep looking unless this is the last candidate,
         * which is a generic family and is meant to substitute. */
        int last = face[i].faces[k + 1].family == NULL;
        char *got = NULL;
        if (!last &&
            XftPatternGetString(f->pattern, XFT_FAMILY, 0, &got) == XftResultMatch &&
            got && strcasecmp(got, face[i].faces[k].family) != 0) {
            XftFontClose(w2k.dpy, f);
            continue;
        }
        if (f->height > 0) return f;
        XftFontClose(w2k.dpy, f);
    }
    return NULL;
}

/* The face for a slot, opened the first time it is asked for. Most
 * programs never draw bold or fixed-pitch text, and a face costs a
 * FreeType instance plus its glyph cache -- a quarter of a megabyte that
 * a calculator has no use for. */
static XftFont *slot_face(int i)
{
    i = face_alias(i);
    if (!use_xft) return NULL;
    if (!xft[i]) xft[i] = open_face(i);
    return xft[i];
}

int w2k_font_init(void)
{
    /* Only the UI face is opened here: it decides whether Xft works at
     * all, and every program draws with it. */
    use_xft = 1;
    if (!slot_face(F_UI)) use_xft = 0;
    return use_xft;
}

/* Reopen the faces -- the antialias setting changed. The new ones are
 * opened before the old are dropped, so a failure leaves the program
 * with the fonts it already had rather than none. */
void w2k_font_reload(void)
{
    if (!use_xft) return;
    XftFont *fresh[N_FONTS] = { NULL };
    int ok = 1;
    for (int i = 0; i < N_FONTS && ok; i++) {
        if (i != face_alias(i) || !xft[i]) continue;   /* never opened */
        fresh[i] = open_face(i);
        if (!fresh[i]) ok = 0;
    }
    if (!ok) {
        for (int i = 0; i < N_FONTS; i++)
            if (fresh[i]) XftFontClose(w2k.dpy, fresh[i]);
        return;
    }
    for (int i = 0; i < N_FONTS; i++)
        if (fresh[i]) {
            XftFontClose(w2k.dpy, xft[i]);
            xft[i] = fresh[i];
        }
}

void w2k_font_fini(void)
{
    for (int i = 0; i < NDRAW; i++)
        if (draws[i].draw) {
            XftDrawDestroy(draws[i].draw);
            draws[i].draw = NULL;
            draws[i].d = 0;
        }
    w2k_font_colours_dirty();
    for (int i = 0; i < N_FONTS; i++) {
        if (xft[i]) XftFontClose(w2k.dpy, xft[i]);
        xft[i] = NULL;
    }
    use_xft = 0;
}

int w2k_font_using_xft(void) { return use_xft; }

int w2k_font_px_height(int font)
{
    XftFont *f = slot_face(font);
    if (f) return f->ascent + f->descent;
    return w2k.font[font]->ascent + w2k.font[font]->descent;
}

int w2k_font_px_ascent(int font)
{
    XftFont *f = slot_face(font);
    if (f) return f->ascent;
    return w2k.font[font]->ascent;
}

int w2k_font_px_width(int font, const char *s, int len)
{
    if (!s || len <= 0) return 0;
    XftFont *f = slot_face(font);
    if (f) {
        XGlyphInfo gi;
        XftTextExtentsUtf8(w2k.dpy, f, (const FcChar8 *)s, len, &gi);
        return gi.xOff;
    }
    return XTextWidth(w2k.font[font], s, len);
}

/* A colour that is not in the palette. Windows' calculator keys are a
 * fixed red and blue, not scheme colours, so there has to be a way to ask
 * for one. Cached, since the callers use a handful. */
static struct { unsigned rgb; XftColor c; int ready; } rgb_cache[8];

static XftColor *colour_rgb(int r, int g, int b)
{
    unsigned key = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
    for (int i = 0; i < 8; i++)
        if (rgb_cache[i].ready && rgb_cache[i].rgb == key)
            return &rgb_cache[i].c;
    for (int i = 0; i < 8; i++) {
        if (rgb_cache[i].ready) continue;
        XRenderColor rc = { (unsigned short)(r * 257), (unsigned short)(g * 257),
                            (unsigned short)(b * 257), 0xffff };
        if (!XftColorAllocValue(w2k.dpy, w2k.visual, w2k.cmap, &rc,
                                &rgb_cache[i].c))
            return NULL;
        rgb_cache[i].rgb = key;
        rgb_cache[i].ready = 1;
        return &rgb_cache[i].c;
    }
    return NULL;
}

void w2k_text_rgb(Drawable d, int font, int x, int y, const char *s,
                  int r, int g, int b)
{
    if (!s || !*s) return;
    int len = (int)strlen(s);
    int baseline = y + w2k_font_px_ascent(font);

    XftFont *fc = slot_face(font);
    if (fc) {
        XftDraw *dr = draw_for(d);
        XftColor *c = colour_rgb(r, g, b);
        if (!dr || !c) return;
        if (w2k_clip_on) {
            XRectangle rc = { (short)w2k_clip_x, (short)w2k_clip_y,
                              (unsigned short)w2k_clip_w,
                              (unsigned short)w2k_clip_h };
            XftDrawSetClipRectangles(dr, 0, 0, &rc, 1);
        } else {
            XftDrawSetClip(dr, NULL);
        }
        XftDrawStringUtf8(dr, c, fc, x, baseline, (const FcChar8 *)s, len);
        return;
    }
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(r, g, b));
    XSetFont(w2k.dpy, w2k.gc, w2k.font[font]->fid);
    XDrawString(w2k.dpy, d, w2k.gc, x, baseline, s, len);
}

/* Draw at a baseline, which is what both backends want. */
void w2k_font_draw(Drawable d, int font, int x, int baseline,
                   const char *s, int len, int color)
{
    if (!s || len <= 0) return;
    XftFont *f = slot_face(font);
    if (f) {
        XftDraw *dr = draw_for(d);
        XftColor *c = colour_for(color);
        if (!dr || !c) return;
        if (w2k_clip_on) {
            XRectangle r = { (short)w2k_clip_x, (short)w2k_clip_y,
                             (unsigned short)w2k_clip_w,
                             (unsigned short)w2k_clip_h };
            XftDrawSetClipRectangles(dr, 0, 0, &r, 1);
        } else {
            XftDrawSetClip(dr, NULL);
        }
        XftDrawStringUtf8(dr, c, f, x, baseline, (const FcChar8 *)s, len);
        return;
    }
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    XSetFont(w2k.dpy, w2k.gc, w2k.font[font]->fid);
    XDrawString(w2k.dpy, d, w2k.gc, x, baseline, s, len);
}

/* Into a 1-bit pixmap, for the rotated Start-menu banner. */
void w2k_font_draw_mono(Pixmap p, GC g, int font, int x, int baseline,
                        const char *s, int len)
{
    XftFont *f = slot_face(font);
    if (f) {
        /* Xft cannot draw into a depth-1 drawable, so the glyphs are
         * rendered as a mask through the same path a shaped window uses. */
        XftDraw *dr = XftDrawCreateBitmap(w2k.dpy, p);
        if (dr) {
            XftColor white;
            XRenderColor rc = { 0xffff, 0xffff, 0xffff, 0xffff };
            if (XftColorAllocValue(w2k.dpy, w2k.visual, w2k.cmap, &rc, &white)) {
                XftDrawStringUtf8(dr, &white, f, x, baseline,
                                  (const FcChar8 *)s, len);
                XftColorFree(w2k.dpy, w2k.visual, w2k.cmap, &white);
            }
            XftDrawDestroy(dr);
            return;
        }
    }
    XSetFont(w2k.dpy, g, w2k.font[font]->fid);
    XDrawString(w2k.dpy, p, g, x, baseline, s, len);
}

/* Free a pixmap that may have had text drawn into it.
 *
 * The XftDraw cache is keyed by drawable id, and X reuses ids: a surface
 * left pointing at a freed pixmap would eventually be handed a different
 * pixmap with the same number. Everything that frees a drawable goes
 * through here. */
void w2k_free_pixmap(Pixmap p)
{
    if (!p) return;
    w2k_font_forget(p);
    XFreePixmap(w2k.dpy, p);
}

/* --- Arbitrary faces ------------------------------------------------- *
 *
 * The shell fonts above are fixed, which is right for a shell: everything
 * is drawn in one typeface at one size. Character Map is the exception --
 * showing a font is the whole point of it -- so it can ask for a face by
 * name. Nothing else should. */
struct W2kFace { XftFont *f; };

static W2kFace *face_open(const char *family, int pixel, int bold)
{
    if (!use_xft || !family || pixel <= 0) return NULL;
    XftFont *f = XftFontOpen(w2k.dpy, w2k.screen,
                             XFT_FAMILY, XftTypeString, family,
                             XFT_PIXEL_SIZE, XftTypeDouble, (double)pixel,
                             XFT_WEIGHT, XftTypeInteger,
                                 bold ? XFT_WEIGHT_BOLD : XFT_WEIGHT_MEDIUM,
                             XFT_ANTIALIAS, XftTypeBool,
                                 w2k_effects[FX_SMOOTH_FONTS] ? 1 : 0,
                             FC_HINTING, XftTypeBool, 1,
                             NULL);
    if (!f) return NULL;
    W2kFace *fa = calloc(1, sizeof *fa);
    if (!fa) { XftFontClose(w2k.dpy, f); return NULL; }
    fa->f = f;
    return fa;
}

W2kFace *w2k_face_open(const char *family, int pixel)
{
    return face_open(family, pixel, 0);
}

W2kFace *w2k_face_open_bold(const char *family, int pixel)
{
    return face_open(family, pixel, 1);
}

void w2k_face_close(W2kFace *fa)
{
    if (!fa) return;
    if (fa->f) XftFontClose(w2k.dpy, fa->f);
    free(fa);
}

int w2k_face_height(W2kFace *fa)
{
    return fa && fa->f ? fa->f->ascent + fa->f->descent : 0;
}

int w2k_face_ascent(W2kFace *fa) { return fa && fa->f ? fa->f->ascent : 0; }

int w2k_face_width(W2kFace *fa, const char *s, int len)
{
    if (!fa || !fa->f || !s) return 0;
    if (len < 0) len = (int)strlen(s);
    XGlyphInfo gi;
    XftTextExtentsUtf8(w2k.dpy, fa->f, (const FcChar8 *)s, len, &gi);
    return gi.xOff;
}

/* Does this face have a glyph for the character, rather than the box that
 * fontconfig substitutes? Character Map's grid would be full of boxes
 * otherwise, since the coverage it lists is the matched face's, and the
 * face opened by name need not be the same one. */
int w2k_face_has(W2kFace *fa, unsigned cp)
{
    return fa && fa->f && XftCharExists(w2k.dpy, fa->f, (FcChar32)cp);
}

void w2k_face_text(Drawable d, W2kFace *fa, int x, int y, const char *s,
                   int color)
{
    if (!fa || !fa->f || !s || !*s) return;
    XftDraw *dr = draw_for(d);
    XftColor *c = colour_for(color);
    if (!dr || !c) return;
    if (w2k_clip_on) {
        XRectangle rc = { (short)w2k_clip_x, (short)w2k_clip_y,
                          (unsigned short)w2k_clip_w,
                          (unsigned short)w2k_clip_h };
        XftDrawSetClipRectangles(dr, 0, 0, &rc, 1);
    } else {
        XftDrawSetClip(dr, NULL);
    }
    XftDrawStringUtf8(dr, c, fa->f, x, y + fa->f->ascent,
                      (const FcChar8 *)s, (int)strlen(s));
}
