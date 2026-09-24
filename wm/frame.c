/* frame.c -- window decorations: the caption bar, its buttons and the
 * sizing border. This is the file that makes a window look like Windows. */
#include "wm.h"
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * Caption geometry
 * ------------------------------------------------------------------ */
typedef struct { int x, y, w, h; } Rect;

/* Fill in the caption rect plus the three button rects, in frame
 * coordinates. Buttons that this window does not get are zero-width. */
/* Frame geometry is in screen pixels: the theme's measurements are
 * multiplied up here, and the chrome is drawn with the primitives in
 * raw mode, where only line thicknesses, fonts, icons and skins scale. */
#define P(v) w2k_px(v)

/* Where the caption icon sits, from the caption's top left corner: the
 * painter draws it here and the system menu is found here. The click
 * used to be looked for at one pixel in whatever the look, and on the
 * themed frames most of the icon then started a move instead. */
static void caption_icon_at(const Rect *cap, int *x, int *y)
{
    int seven = W2K_THEME_IS7(frame_theme());
    int modern = frame_theme() == THEME_MODERN;
    *x = P(frame_theme() == THEME_CLASSIC ? 1 : seven ? 10 : modern ? 9 : 6);
    *y = frame_theme() == THEME_AERO ? P(10) : seven ? P(11) : (cap->h - P(16)) / 2;
}

static void caption_layout(Client *c, Rect *cap, Rect *sys,
                           Rect *mn, Rect *mx, Rect *cl)
{
    int raw = w2k_scale_raw;
    w2k_scale_raw = 1;
    int b = client_border(c);
    int themed = frame_theme() != THEME_CLASSIC;
    /* Luna's caption reaches to the frame's edge and swallows the top
     * border; the classic one sits inside it. Modern's reaches to the
     * visible edge: the invisible margin lies outside. */
    int m = frame_theme() == THEME_MODERN ? w2k_theme_modern_margin() : 0;
    cap->x = themed ? m : b;
    cap->y = themed ? m : b;
    cap->w = client_frame_w(c) - (themed ? 2 * m : 2 * b);
    cap->h = themed ? client_caption_h(c) + b - m : CAPTION_H;

    int bw = themed ? P(w2k_theme_capbtn_size(frame_theme())) : CAPBTN_W;
    int bh = themed ? bw : CAPBTN_H;
    if (themed) {
        /* Where the theme puts them, measured off a screenshot. XP's are
         * square; Windows 7's Close is wider than its neighbours. */
        int by, cx, mxx, mnx;
        w2k_theme_capbtn_place(frame_theme(), cap->w, &by, &cx, &mxx, &mnx);
        cx += cap->x; mxx += cap->x; mnx += cap->x; by += cap->y;
        cl->x = cx;  cl->y = by; cl->w = P(w2k_theme_capbtn_w(frame_theme(), W2K_CAP_CLOSE)); cl->h = bh;
        mx->x = mxx; mx->y = by; mx->w = P(w2k_theme_capbtn_w(frame_theme(), W2K_CAP_MAX));   mx->h = bh;
        mn->x = mnx; mn->y = by; mn->w = P(w2k_theme_capbtn_w(frame_theme(), W2K_CAP_MIN));   mn->h = bh;
    } else {
        int by = cap->y + (cap->h - bh) / 2;
        int right = cap->x + cap->w - P(2);
        cl->x = right - bw; cl->y = by; cl->w = bw; cl->h = bh;
        /* Two pixels of air separate Close from the size buttons. */
        mx->x = cl->x - P(2) - bw; mx->y = by; mx->w = bw; mx->h = bh;
        mn->x = mx->x - bw;     mn->y = by; mn->w = bw; mn->h = bh;
    }

    if (!c->resizable && !c->maximized) {
        /* Fixed-size windows (dialogs) show Close only. */
        mx->w = mn->w = 0;
        mx->x = mn->x = cl->x;
    }
    /* A dialog has no system menu, and so no icon at the left of its
     * caption: the Windows 2000 Run and Open dialogs show their title
     * against the left edge. */
    int ix, iy;
    caption_icon_at(cap, &ix, &iy);
    sys->x = cap->x + ix; sys->y = cap->y + iy; sys->w = c->is_dialog ? 0 : P(16);
    sys->h = P(16);
    if (sys->h > cap->h - P(2)) { sys->y = cap->y; sys->h = cap->h; }
    w2k_scale_raw = raw;
}

static int in_rect(const Rect *r, int x, int y)
{
    return r->w > 0 && x >= r->x && x < r->x + r->w &&
           y >= r->y && y < r->y + r->h;
}

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */
/* All of the frame's drawing goes to `d`, which is normally the frame
 * window -- and a pixmap when frame_render() is looking at it. */
static void frame_draw_raw(Client *c, Drawable d);

static void frame_draw(Client *c, Drawable d)
{
    int raw = w2k_scale_raw;
    w2k_scale_raw = 1;
    frame_draw_raw(c, d);
    w2k_scale_raw = raw;
}

static void frame_draw_raw(Client *c, Drawable d)
{
    int fw = client_frame_w(c), fh = client_frame_h(c);
    int b = client_border(c);
    int active = (focused == c);

    if (frame_theme() == THEME_CLASSIC) {
        /* Sizing border: a raised edge with a face-coloured grab margin. */
        int e2 = 2 * w2k_th(1);                 /* the edge is two lines */
        w2k_edge(d, 0, 0, fw, fh, EDGE_RAISED, BF_RECT);
        if (b > e2) {
            w2k_fill(d, e2, e2, fw - 2 * e2, b - e2, C_FACE);        /* top    */
            w2k_fill(d, e2, fh - b, fw - 2 * e2, b - e2, C_FACE);    /* bottom */
            w2k_fill(d, e2, e2, b - e2, fh - 2 * e2, C_FACE);        /* left   */
            w2k_fill(d, fw - b, e2, b - e2, fh - 2 * e2, C_FACE);    /* right  */
        }
    } else if (frame_theme() == THEME_AERO) {
        /* Glass: the borders and the bottom here, the caption band with
         * the rest of the caption below. The wallpaper under the frame is
         * what shows through, so the frame's place on the screen matters. */
        int rx = c->x - b, ry = c->y - b - client_caption_h(c);
        w2k_glass_above = c->frame;
        w2k_aero_frame(d, 0, 0, rx, ry, fw, fh, P(AERO_TOP), fh, active, -1, -1, 0, 0);
    } else {
        /* Luna's frame is the caption colour carried down both sides and
         * along the bottom, with a hairline where the client begins. */
        /* The border comes from the frame skin: four pixels each side
         * and along the bottom, corners included. */
        w2k_theme_frame_edges(d, fw, fh, b, active, frame_theme());
    }

    Rect cap, sys, mn, mx, cl;
    caption_layout(c, &cap, &sys, &mn, &mx, &cl);
    if (cap.w <= 0) return;

    /* Double-buffer just the caption strip: the gradient is drawn a column
     * at a time and would otherwise tear on focus changes. */
    /* One buffer per window, kept until the caption changes size: a
     * caption repaints on every focus change and every title change. */
    /* Widened in steps of 256: dragging a window's edge changes cap.w on
     * every motion event, and an exact fit meant freeing and remaking a
     * pixmap the width of the window at up to 120 Hz. */
    int want_w = (cap.w + 255) & ~255;
    if (c->capbuf && (c->capbuf_w < cap.w || c->capbuf_w > want_w + 256 ||
                      c->capbuf_h != cap.h)) {
        w2k_free_pixmap(c->capbuf);
        c->capbuf = 0;
    }
    if (!c->capbuf) {
        c->capbuf = XCreatePixmap(w2k.dpy, w2k.root, want_w, cap.h, w2k.depth);
        c->capbuf_w = want_w;
        c->capbuf_h = cap.h;
    }
    Pixmap pm = c->capbuf;
    if (frame_theme() == THEME_CLASSIC)
        w2k_gradient(pm, 0, 0, cap.w, cap.h,
                     active ? C_ACTIVETITLE  : C_INACTIVETITLE,
                     active ? C_ACTIVETITLE2 : C_INACTIVETITLE2);
    else if (frame_theme() == THEME_AERO) {
        /* The caption band of glass, buttons included: lit under the
         * pointer, pressed under the button, grey when inactive. */
        int rx = c->x - b, ry = c->y - b - client_caption_h(c);
        int hot = c->btn_hot == HT_MINBUTTON ? 0 : c->btn_hot == HT_MAXBUTTON ? 1
                : c->btn_hot == HT_CLOSE ? 2 : -1;
        int down = c->btn_down && c->btn_down == c->btn_hot ? hot : -1;
        w2k_glass_above = c->frame;
        w2k_aero_frame(pm, 0, 0, rx, ry, fw, fh, 0, cap.h, active, hot, down, mn.w == 0, 1);
    } else
        w2k_theme_caption(pm, 0, 0, cap.w, cap.h, active, frame_theme());

    /* Windows 7 sets its icon two pixels in from the eight-pixel border
     * and the title, in the regular UI face, six past it. */
    int seven = W2K_THEME_IS7(frame_theme());
    int modern = frame_theme() == THEME_MODERN;
    int inset, iy;
    caption_icon_at(&cap, &inset, &iy);
    int tx = inset + P(1);
    if (c->icon >= 0 && !c->is_dialog) {
        /* Measured off the artwork: the icon at (10,11), the title at 30.
         * Windows 11 sets the icon eight pixels in and the title eight
         * past it. */
        w2k_icon_draw(pm, inset, iy, c->icon);
        tx = frame_theme() == THEME_CLASSIC ? inset + P(16 + 3)
           : modern ? inset + P(16 + 8) : P(seven ? 30 : 27);
    }
    int tfont = (seven || modern) ? F_UI : F_UI_BOLD;

    int avail = mn.w ? mn.x - cap.x - tx - P(2) : cl.x - cap.x - tx - P(2);
    if (avail > P(8) && c->name) {
        char buf[256];
        w2k_ellipsis(tfont, c->name, avail, buf, sizeof buf);
        int ty = (cap.h - w2k_font_height(tfont)) / 2 + P(1);
        if (seven) ty = P(10) + (P(21) - w2k_font_height(tfont)) / 2;   /* centred below the outline */
        if (frame_theme() == THEME_AERO) ty = P(2) + (P(32) - w2k_font_height(tfont)) / 2;   /* the glass rows */
        if (frame_theme() == THEME_XP) {
            /* Luna sets the title in white over a soft shadow. */
            w2k_text_rgb(pm, F_UI_BOLD, tx + P(1), ty + P(1), buf,
                         active ? 0 : 90, active ? 40 : 110,
                         active ? 120 : 160);
        }
        if (frame_theme() == THEME_AERO) {
            /* Aero: black, active or not, as Windows 7 sets it on the
             * glass (it was white here, which the owner asked to change). */
            w2k_text_rgb(pm, tfont, tx, ty, buf, 0, 0, 0);
        } else if (seven) {    /* black when active, grey when not */
            int g = active ? 0 : 153;
            w2k_text_rgb(pm, tfont, tx, ty, buf, g, g, g);
        }
        else
            w2k_text(pm, tfont, tx, ty, buf,
                     active ? C_TITLETEXT : C_INACTIVETITLETEXT);
    }

    if (frame_theme() == THEME_AERO) {      /* the buttons came with the glass */
        XCopyArea(w2k.dpy, pm, d, w2k.gc, 0, 0, cap.w, cap.h, cap.x, cap.y);
        return;
    }
    /* Buttons live on the caption pixmap, so shift into its coordinates. */
    int dx = -cap.x, dy = -cap.y;
    int themed = frame_theme() != THEME_CLASSIC;
    if (mn.w) {
        int p = (c->btn_down == HT_MINBUTTON && c->btn_hot == HT_MINBUTTON);
        if (themed) {
            w2k_theme_capbtn(pm, mn.x + dx, mn.y + dy, mn.w, mn.h,
                             W2K_CAP_MIN, active, p, frame_theme());
        } else {
            w2k_button(pm, mn.x + dx, mn.y + dy, mn.w, mn.h, p);
            w2k_capglyph_min(pm, mn.x + dx + p, mn.y + dy + p, C_TEXT);
        }

        p = (c->btn_down == HT_MAXBUTTON && c->btn_hot == HT_MAXBUTTON);
        if (themed) {
            w2k_theme_capbtn(pm, mx.x + dx, mx.y + dy, mx.w, mx.h,
                             c->maximized ? W2K_CAP_RESTORE : W2K_CAP_MAX,
                             active, p, frame_theme());
        } else {
            w2k_button(pm, mx.x + dx, mx.y + dy, mx.w, mx.h, p);
            if (c->maximized) w2k_capglyph_restore(pm, mx.x + dx + p, mx.y + dy + p, C_TEXT, C_FACE);
            else              w2k_capglyph_max(pm, mx.x + dx + p, mx.y + dy + p, C_TEXT);
        }
    }
    int p = (c->btn_down == HT_CLOSE && c->btn_hot == HT_CLOSE);
    if (themed) {
        w2k_theme_capbtn(pm, cl.x + dx, cl.y + dy, cl.w, cl.h, W2K_CAP_CLOSE,
                         active, p, frame_theme());
    } else {
        w2k_button(pm, cl.x + dx, cl.y + dy, cl.w, cl.h, p);
        w2k_capglyph_close(pm, cl.x + dx + p, cl.y + dy + p, C_TEXT);
    }

    XCopyArea(w2k.dpy, pm, d, w2k.gc, 0, 0, cap.w, cap.h, cap.x, cap.y);
}

/* Luna's windows have rounded top corners, and a corner drawn in the
 * caption colour is just a square with a curve painted on it -- the
 * desktop has to show through. That means shaping the frame window, so
 * the two corner pixels genuinely are not part of it.
 *
 * Called on every resize; cheap enough (two small pixmaps) and the only
 * way to get the shape right. */
/* The frame's shape as rows of rectangles, handed to the server as they
 * are. Rebuilt only when what it depends on changed: every step of a
 * drag used to build it again, pure moves included -- a frame-sized mask
 * filled, sent, and turned back into a region by the server, up to 120
 * times a second. */
#define SHAPE_ROWS 96
static XRectangle shape_r[SHAPE_ROWS];
static int shape_n;

static void shape_row(int x, int y, int w, int h)
{
    if (w > 0 && h > 0 && shape_n < SHAPE_ROWS)
        shape_r[shape_n++] = (XRectangle){ (short)x, (short)y, (unsigned short)w, (unsigned short)h };
}

static void shape_set(Client *c)
{
    XShapeCombineRectangles(w2k.dpy, c->frame, ShapeBounding, 0, 0, shape_r, shape_n,
                            ShapeSet, Unsorted);
}

void frame_shape(Client *c)
{
    if (!c->frame) return;
    int fw = client_frame_w(c), fh = client_frame_h(c);
    if (fw <= 0 || fh <= 0) return;
    int key = ((frame_theme() * 2 + !!c->maximized) * 2 + !!c->fullscreen) * 2 + !!c->decorate;
    key = key * 1000 + w2k_ui_scale;
    if (c->shape_w == fw && c->shape_h == fh && c->shape_key == key) return;
    c->shape_w = fw;
    c->shape_h = fh;
    c->shape_key = key;
    shape_n = 0;

    if (frame_theme() == THEME_MODERN && c->decorate && !c->fullscreen) {
        /* The invisible margin is cut away all round, and the visible
         * window gets Windows 11's rounded corners -- unless it is
         * maximised, when the margin is off the screen and the corners
         * are square. */
        int m = w2k_theme_modern_margin();
        int rad = c->maximized ? 0 : P(8);
        int vw = fw - 2 * m, vh = fh - 2 * m;
        if (vw <= 0 || vh <= 0) return;
        if (2 * rad > vw) rad = vw / 2;
        if (2 * rad > vh) rad = vh / 2;
        if (vh > 2 * rad) shape_row(m, m + rad, vw, vh - 2 * rad);
        for (int i = 0; i < rad; i++) {
            int ins = w2k_round_inset(rad, i);
            if (2 * ins >= vw) continue;
            shape_row(m + ins, m + i, vw - 2 * ins, 1);
            shape_row(m + ins, m + vh - 1 - i, vw - 2 * ins, 1);
        }
        shape_set(c);
        return;
    }
    if (frame_theme() == THEME_CLASSIC || !c->decorate || c->maximized ||
        c->fullscreen) {
        XShapeCombineMask(w2k.dpy, c->frame, ShapeBounding, 0, 0, None,
                          ShapeSet);
        return;
    }

    /* The corner cut per row. Luna's is measured off its caption skin --
     * the pixels there that are the screenshot's white, not the frame:
     * five, three, two, one, one -- so the shape and the artwork agree to
     * the pixel; a circle a pixel too tight left white specks outside the
     * curve. Basic's caption has no such corner, and keeps a small arc. */
    if (frame_theme() == THEME_AERO && w2k_aero_corner_rows() > 0) {
        /* Aero's corners, top and bottom, follow its corner art: the shape
         * turns opaque where the art does. */
        int n = w2k_aero_corner_rows();
        if (fh > 2 * n) shape_row(0, n, fw, fh - 2 * n);
        for (int i = 0; i < n && i < fh; i++) {
            int t = w2k_aero_corner_inset(i, 0), u = w2k_aero_corner_inset(i, 1);
            if (2 * t < fw) shape_row(t, i, fw - 2 * t, 1);
            if (2 * u < fw) shape_row(u, fh - 1 - i, fw - 2 * u, 1);
        }
        shape_set(c);
        return;
    }
    static const int luna[5] = { 5, 3, 2, 1, 1 };
    static const int basic[5] = { 3, 2, 1, 1, 0 };
    const int *ins = W2K_THEME_IS7(frame_theme()) ? basic : luna;
    /* On a scaled desktop each measured row stands for a band of rows. */
    int rad = P(5);
    shape_row(0, rad, fw, fh - rad);
    for (int i = 0; i < rad; i++) {
        int off = P(ins[i * 5 / rad]);
        if (2 * off < fw) shape_row(off, i, fw - 2 * off, 1);
    }
    shape_set(c);
}

void frame_paint(Client *c)
{
    if (!c->decorate || !c->mapped) return;
    frame_draw(c, c->frame);
}

/* Development aid, in the shape of the other W2K_RENDER hooks: draw a
 * frame of the given size into a pixmap and write it out as a PPM, so the
 * themed chrome can be looked at without a desktop running. */
int frame_render(const char *path, int cw, int ch, int active, int maximized)
{
    Client c;
    memset(&c, 0, sizeof c);
    c.w = cw;
    c.h = ch;
    c.decorate = 1;
    c.mapped = 1;
    c.resizable = 1;
    c.maximized = maximized;
    c.icon = ICO_EXPLORER;
    c.name = (char *)"My Computer";
    /* W2K_RENDER_TITLE / W2K_RENDER_ICON dress the frame for a picture
     * of a particular program. */
    if (getenv("W2K_RENDER_TITLE")) c.name = getenv("W2K_RENDER_TITLE");
    if (getenv("W2K_RENDER_ICON")) c.icon = atoi(getenv("W2K_RENDER_ICON"));
    if (getenv("W2K_RENDER_DIALOG_FRAME")) c.resizable = 0;
    Client *saved = focused;
    focused = active ? &c : NULL;

    int fw = client_frame_w(&c), fh = client_frame_h(&c);
    Pixmap pm = XCreatePixmap(w2k.dpy, w2k.root, (unsigned)fw, (unsigned)fh,
                              w2k.depth);
    int raw = w2k_scale_raw;
    w2k_scale_raw = 1;                 /* everything here is screen pixels */
    w2k_fill(pm, 0, 0, fw, fh, C_DESKTOP);
    frame_draw(&c, pm);
    /* A patch of client area, so the frame is seen around something. */
    int b = client_border(&c), cap = client_caption_h(&c);
    w2k_fill(pm, b, b + cap, cw, ch, C_WINDOW);
    w2k_scale_raw = raw;
    focused = saved;

    XImage *im = XGetImage(w2k.dpy, pm, 0, 0, (unsigned)fw, (unsigned)fh,
                           AllPlanes, ZPixmap);
    FILE *f = fopen(path, "wb");
    if (f && im) {
        fprintf(f, "P6\n%d %d\n255\n", fw, fh);
        for (int y = 0; y < fh; y++)
            for (int x = 0; x < fw; x++) {
                unsigned long v = XGetPixel(im, x, y);
                unsigned char rgb[3] = { (v >> 16) & 0xff, (v >> 8) & 0xff,
                                         v & 0xff };
                fwrite(rgb, 1, 3, f);
            }
    }
    if (f) fclose(f);
    if (im) XDestroyImage(im);
    if (c.capbuf) w2k_free_pixmap(c.capbuf);
    w2k_free_pixmap(pm);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Hit testing
 * ------------------------------------------------------------------ */
int frame_hittest(Client *c, int fx, int fy)
{
    if (!c->decorate) return HT_CLIENT;

    int fw = client_frame_w(c), fh = client_frame_h(c);
    int b = client_border(c);

    Rect cap, sys, mn, mx, cl;
    caption_layout(c, &cap, &sys, &mn, &mx, &cl);

    if (in_rect(&cl, fx, fy)) return HT_CLOSE;
    if (in_rect(&mx, fx, fy)) return HT_MAXBUTTON;
    if (in_rect(&mn, fx, fy)) return HT_MINBUTTON;

    /* The sizing border before the caption: the themed captions reach
     * the frame's edge, over the top border and the sides beside them,
     * and XP, Vista, 7 and Aero windows could not be sized from the top
     * or its corners -- a drag there moved them. */
    if (c->resizable && !c->maximized && !c->fullscreen) {
        int L = fx < b, R = fx >= fw - b, T = fy < b, B = fy >= fh - b;
        int nearL = fx < CORNER_GRAB, nearR = fx >= fw - CORNER_GRAB;
        int nearT = fy < CORNER_GRAB, nearB = fy >= fh - CORNER_GRAB;

        if ((L || T) && nearL && nearT) return HT_TOPLEFT;
        if ((R || T) && nearR && nearT) return HT_TOPRIGHT;
        if ((L || B) && nearL && nearB) return HT_BOTTOMLEFT;
        if ((R || B) && nearR && nearB) return HT_BOTTOMRIGHT;
        if (L) return HT_LEFT;
        if (R) return HT_RIGHT;
        if (T) return HT_TOP;
        if (B) return HT_BOTTOM;
    }
    if (in_rect(&sys, fx, fy)) return HT_SYSMENU;
    if (in_rect(&cap, fx, fy)) return HT_CAPTION;
    if (fx < b || fy < b || fx >= fw - b || fy >= fh - b) return HT_NOWHERE;
    return HT_CLIENT;
}

Cursor frame_cursor(int ht)
{
    switch (ht) {
    case HT_LEFT: case HT_RIGHT:            return w2k.cur_size_we;
    case HT_TOP: case HT_BOTTOM:            return w2k.cur_size_ns;
    case HT_TOPLEFT: case HT_BOTTOMRIGHT:   return w2k.cur_size_nwse;
    case HT_TOPRIGHT: case HT_BOTTOMLEFT:   return w2k.cur_size_nesw;
    default:                                return w2k.cur_arrow;
    }
}

/* ------------------------------------------------------------------ *
 * Interaction
 * ------------------------------------------------------------------ */
void frame_button_press(Client *c, XButtonEvent *e)
{
    int ht = frame_hittest(c, e->x, e->y);

    if (e->button == Button1) {
        client_raise(c);
        client_focus(c);

        switch (ht) {
        case HT_CLOSE:
        case HT_MINBUTTON:
        case HT_MAXBUTTON:
            c->btn_down = ht;
            c->btn_hot  = ht;
            frame_paint(c);
            return;
        case HT_SYSMENU:
            /* The system menu drops from the left edge, under the caption. */
            sysmenu_popup(c, c->x - client_border(c), c->y);
            return;
        case HT_CAPTION:
            /* Double-click the caption toggles maximise. */
            {
                static Time last;
                static Window lastw;
                if (lastw == c->frame && (int)(e->time - last) < w2k_dblclk_ms) {
                    last = 0;
                    client_maximize(c, !c->maximized);
                    return;
                }
                last = e->time;
                lastw = c->frame;
            }
            if (!c->maximized) do_move(c, e);
            return;
        case HT_LEFT: case HT_RIGHT: case HT_TOP: case HT_BOTTOM:
        case HT_TOPLEFT: case HT_TOPRIGHT:
        case HT_BOTTOMLEFT: case HT_BOTTOMRIGHT:
            do_resize(c, e, ht);
            return;
        }
    } else if (e->button == Button3 && (ht == HT_CAPTION || ht == HT_SYSMENU)) {
        sysmenu_popup(c, e->x_root, e->y_root);
    }
}

/* What the Minimize button puts down: a dialog has no task button to
 * come back from, so it goes down with the window that owns it and comes
 * back with it. The button did nothing on a resizable one -- a file
 * chooser's, say. */
static Client *minimize_target(Client *c)
{
    for (int i = 0; c && c->skip_taskbar && i < 8; i++)
        c = c->transient_for ? client_find(c->transient_for) : NULL;
    return c;
}

void frame_button_release(Client *c, XButtonEvent *e)
{
    if (!c->btn_down) return;
    int ht = frame_hittest(c, e->x, e->y);
    int which = c->btn_down;
    c->btn_down = 0;
    frame_paint(c);

    if (ht != which) return;         /* released off the button: cancelled */
    switch (which) {
    case HT_CLOSE:     client_close(c); break;
    case HT_MINBUTTON: client_minimize(minimize_target(c)); break;
    case HT_MAXBUTTON: client_maximize(c, !c->maximized); break;
    }
}

void frame_motion(Client *c, XMotionEvent *e)
{
    int ht = frame_hittest(c, e->x, e->y);

    if (c->btn_down) {
        int hot = (ht == c->btn_down) ? ht : 0;
        if (hot != c->btn_hot) { c->btn_hot = hot; frame_paint(c); }
        return;
    }
    /* With no button held the caption buttons still light under the
     * pointer, which is what the themed painters draw a hot state for. */
    int hot = (ht == HT_MINBUTTON || ht == HT_MAXBUTTON || ht == HT_CLOSE) ? ht : 0;
    if (hot != c->btn_hot) { c->btn_hot = hot; frame_paint(c); }
    /* One request per change, not one per pointer position. */
    Cursor cur = frame_cursor(ht);
    if (cur != c->cursor) { XDefineCursor(w2k.dpy, c->frame, cur); c->cursor = cur; }
}

void frame_leave(Client *c)
{
    if (c->btn_hot) { c->btn_hot = 0; frame_paint(c); }
    /* The pointer has left the frame -- or gone into the client, which
     * inherits the frame's cursor unless it sets its own. Either way the
     * sizing arrow must not stay behind -- and the frame must know it is
     * gone, or coming back to the border kept the arrow (frame_motion sets
     * a cursor only when it differs from the one it last set). */
    XDefineCursor(w2k.dpy, c->frame, w2k.cur_arrow);
    c->cursor = w2k.cur_arrow;
}
