/* capbtn.c -- the glyphs on the caption buttons.
 *
 * Minimise, maximise, restore and close, at the sizes USER32 draws them:
 * measured off a Windows 2000 screenshot rather than eyeballed, since a
 * pixel either way is the difference between "close enough" and right.
 *
 * Within a 16x14 button, and relative to its top-left corner:
 *
 *   minimise   a 6x2 bar at (4, 9)
 *   maximise   a 9x9 outline at (3, 2), its top bar two pixels thick
 *   close      an 8x7 cross at (4, 3)
 *
 * They live here rather than in the window manager so that anything drawing
 * a mock caption -- a settings preview, say -- draws the same pixels. */
#include "w2k.h"

static const char *glyph_close[] = {
    "##    ##",
    " ##  ## ",
    "  ####  ",
    "   ##   ",
    "  ####  ",
    " ##  ## ",
    "##    ##",
    NULL
};

static void draw_glyph(Drawable d, int x, int y, const char *const *rows,
                       int color)
{
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    for (int r = 0; rows[r]; r++)
        for (int c = 0; rows[r][c]; c++)
            if (rows[r][c] == '#')
                XFillRectangle(w2k.dpy, d, w2k.gc, x + c, y + r, 1, 1);
}

/* The title-bar box: an outline whose top bar is two pixels thick, standing
 * in for a caption. */
static void maxbox(Drawable d, int x, int y, int w, int h, int color)
{
    w2k_frame(d, x, y, w, h, color);
    w2k_hline(d, x, y + 1, w, color);
}

void w2k_capglyph_min(Drawable d, int x, int y, int color)
{
    w2k_fill(d, x + 4, y + 9, 6, 2, color);
}

void w2k_capglyph_max(Drawable d, int x, int y, int color)
{
    maxbox(d, x + 3, y + 2, 9, 9, color);
}

/* Restore: a small box behind and above, a second one in front of it. Both
 * sit inside the same 9x9 area the maximise box uses. */
void w2k_capglyph_restore(Drawable d, int x, int y, int color, int face)
{
    maxbox(d, x + 5, y + 2, 7, 7, color);
    w2k_fill(d, x + 3, y + 4, 8, 8, face);      /* punch out the overlap */
    maxbox(d, x + 3, y + 4, 7, 7, color);
}

void w2k_capglyph_close(Drawable d, int x, int y, int color)
{
    draw_glyph(d, x + 4, y + 3, glyph_close, color);
}
