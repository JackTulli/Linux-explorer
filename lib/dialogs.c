/* dialogs.c -- combo box, prompt dialog and the file open/save dialog. */
#include "w2kui.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * Combo box
 * ------------------------------------------------------------------ */
W2kCombo *w2k_combo_new(int editable)
{
    W2kCombo *c = w2k_alloc(sizeof *c);
    c->icon = ICO_NONE;
    c->editable = editable;
    c->sel = -1;
    if (editable) c->edit = w2k_edit_new(0);
    return c;
}

void w2k_combo_clear(W2kCombo *c)
{
    for (int i = 0; i < c->n; i++) free(c->items[i]);
    c->n = 0;
    c->sel = -1;
}

void w2k_combo_free(W2kCombo *c)
{
    if (!c) return;
    w2k_combo_clear(c);
    free(c->items);
    w2k_edit_free(c->edit);
    free(c);
}

void w2k_combo_add(W2kCombo *c, const char *text)
{
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->items = realloc(c->items, c->cap * sizeof *c->items);
        if (!c->items) abort();
    }
    c->items[c->n++] = w2k_strdup(text);
    if (c->sel < 0) c->sel = 0;
}

/* The drop-down button's triangle. */
static void down_arrow(Drawable d, int x, int y, int color)
{
    XSetForeground(w2k.dpy, w2k.gc, w2k.col[color]);
    for (int i = 0; i < 4; i++)
        w2k_fill_fg(d, x + i, y + 3 - i, 1, 1 + 2 * i);
}

void w2k_combo_draw(Drawable d, W2kCombo *c)
{
    int bw = 17;
    w2k_edge(d, c->r.x, c->r.y, c->r.w, c->r.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, c->r.x + 2, c->r.y + 2, c->r.w - 4, c->r.h - 4, C_WINDOW);

    /* An editable combo is a text field with a drop-down of past entries
     * beside it -- the Run box, an address bar. The well is drawn here, so
     * the edit inside it must not draw a second one. */
    if (c->editable && c->edit) {
        c->edit->noframe = 1;
        c->edit->r = (W2kRect){ c->r.x + 2, c->r.y + 2,
                                c->r.w - 4 - bw, c->r.h - 4 };
        w2k_edit_draw(d, c->edit);
        int ebx = c->r.x + c->r.w - 2 - bw, eby = c->r.y + 2;
        w2k_button(d, ebx, eby, bw, c->r.h - 4, c->pressed);
        down_arrow(d, ebx + (bw - 7) / 2 + c->pressed,
                   eby + (c->r.h - 4 - 7) / 2 + 2 + c->pressed, C_TEXT);
        return;
    }

    const char *txt = (c->sel >= 0 && c->sel < c->n) ? c->items[c->sel] : "";
    int fh = w2k_font_height(F_UI);
    char buf[200];
    /* The Address bar's combo carries the folder's icon before its name:
     * 16 pixels at 4, the text at 24 (measured off the shell). */
    int tx = c->r.x + 4;
    if (c->icon >= 0) {
        w2k_icon_draw(d, c->r.x + 4, c->r.y + (c->r.h - 16) / 2, c->icon);
        tx = c->r.x + 24;
    }
    w2k_ellipsis(F_UI, txt, c->r.x + c->r.w - bw - 6 - tx, buf, sizeof buf);

    if (c->focused) {
        int tw = w2k_text_width(F_UI, buf, -1);
        w2k_fill(d, tx - 1, c->r.y + 3, tw + 2, c->r.h - 6, C_HIGHLIGHT);
        w2k_text(d, F_UI, tx, c->r.y + (c->r.h - fh) / 2, buf,
                 C_HIGHLIGHTTEXT);
    } else {
        w2k_text(d, F_UI, tx, c->r.y + (c->r.h - fh) / 2, buf, C_WINDOWTEXT);
    }

    int bx = c->r.x + c->r.w - 2 - bw, by = c->r.y + 2;
    w2k_button(d, bx, by, bw, c->r.h - 4, c->pressed);
    down_arrow(d, bx + (bw - 7) / 2 + c->pressed,
               by + (c->r.h - 4 - 7) / 2 + 2 + c->pressed, C_TEXT);
}

/* The basic colours of the Windows colour dialog, in its 8 by 6 order. */
static const unsigned char basic_colors[48][3] = {
    {255,128,128},{255,255,128},{128,255,128},{0,255,128},{128,255,255},{0,128,255},{255,128,192},{255,128,255},
    {255,0,0},{255,255,0},{128,255,0},{0,255,64},{0,255,255},{0,128,192},{128,128,192},{255,0,255},
    {128,64,64},{255,128,64},{0,255,0},{0,128,128},{0,64,128},{128,128,255},{128,0,64},{255,0,128},
    {128,0,0},{255,128,0},{0,128,0},{0,128,64},{0,0,255},{0,0,160},{128,0,128},{128,0,255},
    {64,0,0},{128,64,0},{0,64,0},{0,64,64},{0,0,128},{0,0,64},{64,0,64},{64,0,128},
    {0,0,0},{128,128,0},{128,128,64},{128,128,128},{64,128,128},{192,192,192},{64,0,64},{255,255,255},
};

int w2k_color_popup(int rx, int ry, int *r, int *g, int *b)
{
    const int cell = 18, gap = 2, cols = 8, rows = 6;
    int w = cols * (cell + gap) + gap + 4, h = rows * (cell + gap) + gap + 4;
    int pw = w2k_px(w), ph = w2k_px(h);          /* on the screen */
    const W2kMonitor *m = w2k_monitor_at(rx, ry);
    int x = rx, y = ry;
    if (x + pw > m->x + m->w) x = m->x + m->w - pw;
    if (y + ph > m->y + m->h) y = ry - ph - w2k_px(22);
    if (y < m->y) y = m->y;

    XSetWindowAttributes a = {
        .override_redirect = True, .save_under = True,
        .background_pixel = w2k.col[C_FACE],
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | KeyPressMask
    };
    Window win = XCreateWindow(w2k.dpy, w2k.root, x, y, (unsigned)pw, (unsigned)ph, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWSaveUnder | CWBackPixel |
                               CWEventMask, &a);
    XMapRaised(w2k.dpy, win);
    if (XGrabPointer(w2k.dpy, win, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, w2k.cur_arrow,
                     CurrentTime) != GrabSuccess) {
        XDestroyWindow(w2k.dpy, win);
        return 0;
    }
    XGrabKeyboard(w2k.dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    int hot = -1, picked = 0, done = 0, repaint = 1;
    for (int i = 0; i < 48; i++)
        if (basic_colors[i][0] == *r && basic_colors[i][1] == *g && basic_colors[i][2] == *b)
            hot = i;
    long opened = w2k_now_ms();
    while (!done) {
        if (repaint) {
            Pixmap pm = XCreatePixmap(w2k.dpy, win, (unsigned)pw, (unsigned)ph, w2k.depth);
            w2k_fill(pm, 0, 0, w, h, C_FACE);
            w2k_edge(pm, 0, 0, w, h, EDGE_RAISED, BF_RECT);
            for (int i = 0; i < 48; i++) {
                int cx = 2 + gap + (i % cols) * (cell + gap);
                int cy = 2 + gap + (i / cols) * (cell + gap);
                w2k_fill_rgb(pm, cx + 1, cy + 1, cell - 2, cell - 2,
                             basic_colors[i][0], basic_colors[i][1], basic_colors[i][2]);
                w2k_edge(pm, cx, cy, cell, cell, EDGE_SUNKEN_THIN, BF_RECT);
                if (i == hot) w2k_focus_rect(pm, cx - 1, cy - 1, cell + 2, cell + 2);
            }
            XCopyArea(w2k.dpy, pm, win, w2k.gc, 0, 0, (unsigned)pw, (unsigned)ph, 0, 0);
            w2k_free_pixmap(pm);
            repaint = 0;
        }
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        if (e.type == MotionNotify || e.type == ButtonRelease || e.type == ButtonPress) {
            int lx = w2k_lp((e.type == MotionNotify ? e.xmotion.x_root : e.xbutton.x_root) - x);
            int ly = w2k_lp((e.type == MotionNotify ? e.xmotion.y_root : e.xbutton.y_root) - y);
            int i = -1;
            if (lx >= 2 + gap && ly >= 2 + gap && lx < w - 2 && ly < h - 2) {
                int c = (lx - 2 - gap) / (cell + gap), rr = (ly - 2 - gap) / (cell + gap);
                if (c < cols && rr < rows) i = rr * cols + c;
            }
            if (e.type == MotionNotify) {
                if (i >= 0 && i != hot) { hot = i; repaint = 1; }
                continue;
            }
            if (e.type == ButtonRelease && w2k_now_ms() - opened < 250) continue;
            if (lx < 0 || lx >= w || ly < 0 || ly >= h) { done = 1; continue; }
            if (e.type == ButtonRelease && i >= 0) { hot = i; picked = 1; done = 1; }
        } else if (e.type == KeyPress) {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Escape) done = 1;
            else if ((ks == XK_Return || ks == XK_KP_Enter) && hot >= 0) { picked = 1; done = 1; }
            else if (ks == XK_Right && hot < 47) { hot++; repaint = 1; }
            else if (ks == XK_Left && hot > 0) { hot--; repaint = 1; }
            else if (ks == XK_Down && hot + cols < 48) { hot += cols; repaint = 1; }
            else if (ks == XK_Up && hot - cols >= 0) { hot -= cols; repaint = 1; }
            else if (hot < 0 && (ks == XK_Right || ks == XK_Down)) { hot = 0; repaint = 1; }
        } else if (e.type == Expose && e.xexpose.window == win) repaint = 1;
    }
    XUngrabKeyboard(w2k.dpy, CurrentTime);
    XUngrabPointer(w2k.dpy, CurrentTime);
    XDestroyWindow(w2k.dpy, win);
    XFlush(w2k.dpy);
    if (picked && hot >= 0) {
        *r = basic_colors[hot][0];
        *g = basic_colors[hot][1];
        *b = basic_colors[hot][2];
    }
    return picked;
}

/* A modal drop-down list. Returns the chosen index, or -1. */
static int combo_dropdown(W2kCombo *c, int rx, int ry)
{
    int row = w2k_font_height(F_UI) + 3;
    int rows = c->n < 12 ? c->n : 12;
    if (rows < 1) return -1;
    int h = rows * row + 4, w = c->r.w;
    int pw = w2k_px(w), ph = w2k_px(h);          /* on the screen */
    int y = ry;
    /* Flip above the box when there is no room below -- measured against the
     * monitor the combo is on, not the whole desktop. */
    const W2kMonitor *m = w2k_monitor_at(rx, ry);
    if (y + ph > m->y + m->h) y = ry - w2k_px(c->r.h) - ph;
    if (y < m->y) y = m->y;

    XSetWindowAttributes a = {
        .override_redirect = True, .save_under = True,
        .background_pixel = w2k.col[C_WINDOW],
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | KeyPressMask
    };
    Window win = XCreateWindow(w2k.dpy, w2k.root, rx, y, (unsigned)pw, (unsigned)ph, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWSaveUnder | CWBackPixel |
                               CWEventMask, &a);

    /* "Slide open combo boxes": grow the list down out of the box. */
    if (w2k_effects[FX_SLIDE_COMBO] && h > 20) {
        XMapRaised(w2k.dpy, win);
        for (int step = 1; step < 4; step++) {
            int sh = ph * step / 4;
            XResizeWindow(w2k.dpy, win, (unsigned)pw, sh < 4 ? 4 : sh);
            XFlush(w2k.dpy);
            usleep(8000);
        }
        XResizeWindow(w2k.dpy, win, (unsigned)pw, (unsigned)ph);
    }
    XMapRaised(w2k.dpy, win);
    /* Without the pointer grab a click outside would never arrive and the
     * list could not be dismissed: then there is no list. */
    if (XGrabPointer(w2k.dpy, win, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, w2k.cur_arrow,
                     CurrentTime) != GrabSuccess) {
        XDestroyWindow(w2k.dpy, win);
        return -1;
    }
    XGrabKeyboard(w2k.dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    int top = 0, hot = c->sel;
    if (hot >= rows) top = hot - rows + 1;
    int result = -1, done = 0, repaint = 1;
    long opened = w2k_now_ms();

    while (!done) {
        if (repaint) {
            Pixmap pm = XCreatePixmap(w2k.dpy, win, (unsigned)pw, (unsigned)ph, w2k.depth);
            w2k_fill(pm, 0, 0, w, h, C_WINDOW);
            w2k_frame(pm, 0, 0, w, h, C_WINDOWFRAME);
            for (int i = 0; i < rows && top + i < c->n; i++) {
                int iy = 2 + i * row;
                int sel = (top + i == hot);
                if (sel) w2k_fill(pm, 2, iy, w - 4, row, C_HIGHLIGHT);
                char b2[200];
                w2k_ellipsis(F_UI, c->items[top + i], w - 10, b2, sizeof b2);
                w2k_text(pm, F_UI, 4, iy + 1, b2,
                         sel ? C_HIGHLIGHTTEXT : C_WINDOWTEXT);
            }
            XCopyArea(w2k.dpy, pm, win, w2k.gc, 0, 0, (unsigned)pw, (unsigned)ph, 0, 0);
            w2k_free_pixmap(pm);
            repaint = 0;
        }
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        if (e.type == MotionNotify) {
            int lx = w2k_lp(e.xmotion.x_root - rx), ly = w2k_lp(e.xmotion.y_root - y);
            if (lx >= 0 && lx < w && ly >= 2 && ly < h - 2) {
                int i = top + (ly - 2) / row;
                if (i < c->n && i != hot) { hot = i; repaint = 1; }
            }
        } else if (e.type == ButtonRelease || e.type == ButtonPress) {
            if (e.type == ButtonRelease && w2k_now_ms() - opened < 250) continue;
            int lx = w2k_lp(e.xbutton.x_root - rx), ly = w2k_lp(e.xbutton.y_root - y);
            if (lx < 0 || lx >= w || ly < 0 || ly >= h) { done = 1; continue; }
            if (e.type == ButtonRelease) {
                int i = top + (ly - 2) / row;
                if (i >= 0 && i < c->n) result = i;
                done = 1;
            }
        } else if (e.type == KeyPress) {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Escape) done = 1;
            else if (ks == XK_Return || ks == XK_KP_Enter) { result = hot; done = 1; }
            else if (ks == XK_Down && hot + 1 < c->n) { hot++; repaint = 1; }
            else if (ks == XK_Up && hot > 0) { hot--; repaint = 1; }
            if (hot < top) { top = hot; repaint = 1; }
            if (hot >= top + rows) { top = hot - rows + 1; repaint = 1; }
        } else if (e.type == Expose && e.xexpose.window == win) repaint = 1;
        else if (w2k_win_foreign_event) {
            /* Not ours: the window manager (when this runs inside it)
             * must keep managing while the list is open, and other
             * dialogs keep painting. */
            w2k_win_foreign_event(&e);
        }
    }
    XUngrabKeyboard(w2k.dpy, CurrentTime);
    XUngrabPointer(w2k.dpy, CurrentTime);
    XDestroyWindow(w2k.dpy, win);
    return result;
}

int w2k_combo_press(W2kCombo *c, XButtonEvent *b)
{
    if (!w2k_rect_hit(&c->r, b->x, b->y)) return 0;
    if (b->button != Button1) return 1;

    /* On an editable combo only the arrow drops the list down; the rest of
     * the control is the text field. */
    if (c->editable && c->edit) {
        int bx = c->r.x + c->r.w - 2 - 17;
        if (b->x < bx) {
            c->focused = 1;
            w2k_edit_press(c->edit, b);
            return 1;
        }
    }

    Window child;
    int rx, ry;
    XTranslateCoordinates(w2k.dpy, b->window, w2k.root, c->r.x,
                          c->r.y + c->r.h, &rx, &ry, &child);
    c->pressed = 1;
    c->focused = 1;
    int i = combo_dropdown(c, rx, ry);
    c->pressed = 0;
    if (i >= 0 && i != c->sel) {
        c->sel = i;
        if (c->editable && c->edit) w2k_edit_set(c->edit, c->items[i]);
        if (c->on_change) c->on_change(c->user, i);
    }
    return 1;
}

int w2k_combo_key(W2kCombo *c, XKeyEvent *k)
{
    if (!c->editable || !c->edit) return 0;
    return w2k_edit_key(c->edit, k);
}

const char *w2k_combo_text(W2kCombo *c)
{
    if (c->editable && c->edit) return w2k_edit_text(c->edit);
    return (c->sel >= 0 && c->sel < c->n) ? c->items[c->sel] : "";
}

void w2k_combo_set_text(W2kCombo *c, const char *text)
{
    if (c->editable && c->edit) w2k_edit_set(c->edit, text);
}

/* ------------------------------------------------------------------ *
 * Prompt dialog (one label, one edit, OK / Cancel)
 * ------------------------------------------------------------------ */
typedef struct {
    const char *label;
    int      icon;
    W2kEdit *edit;
    W2kRect  ok, cancel;
    int      down, focus;
} Prompt;

static void prompt_paint(W2kWin *w, Drawable d)
{
    Prompt *p = w->user;
    int x = 12;
    if (p->icon >= 0) { w2k_bigicon_draw(d, 12, 14, p->icon); x = 12 + 32 + 12; }
    w2k_text(d, F_UI, x, 16, p->label, C_TEXT);
    w2k_edit_draw(d, p->edit);
    w2k_draw_pushbutton(d, &p->ok, "OK",
                        BS_DEFAULT | (p->focus == 1 ? BS_FOCUS : 0) |
                        (p->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &p->cancel, "Cancel",
                        (p->focus == 2 ? BS_FOCUS : 0) |
                        (p->down == 2 ? BS_PRESSED : 0));
}

static int prompt_event(W2kWin *w, XEvent *e)
{
    Prompt *p = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_edit_press(p->edit, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&p->ok, e->xbutton.x, e->xbutton.y))     p->down = 1;
        else if (w2k_rect_hit(&p->cancel, e->xbutton.x, e->xbutton.y)) p->down = 2;
        w2k_win_dirty(w);
        return 1;
    case MotionNotify:
        if (w2k_edit_motion(p->edit, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        w2k_edit_release(p->edit);
        int d = p->down;
        p->down = 0;
        if (d == 1 && w2k_rect_hit(&p->ok, e->xbutton.x, e->xbutton.y))
            w2k_win_close(w, ID_OK);
        else if (d == 2 && w2k_rect_hit(&p->cancel, e->xbutton.x, e->xbutton.y))
            w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            w2k_win_close(w, p->focus == 2 ? ID_CANCEL : ID_OK);
            return 1;
        }
        if (ks == XK_Tab) {
            p->focus = (p->focus + 1) % 3;
            p->edit->focused = (p->focus == 0);
            w2k_win_dirty(w);
            return 1;
        }
        if (p->focus == 0 && w2k_edit_key(p->edit, &e->xkey)) {
            w2k_win_dirty(w);
            return 1;
        }
        return 1;
    }
    }
    return 0;
}

static void blink_cb(void *v) { w2k_edit_blink(v); }

int w2k_prompt(W2kWin *over, const char *title, const char *label,
               const char *initial, char *out, int outsz, int icon)
{
    Prompt p = { .label = label, .icon = icon, .focus = 0 };
    int cw = 350, chh = 130;

    W2kWin *w = w2k_win_new(title, "w2kdialog", cw, chh, 0);
    p.edit = w2k_edit_new(0);
    w2k_edit_bind(p.edit, w);
    w2k_edit_set(p.edit, initial ? initial : "");
    w2k_edit_select_all(p.edit);
    p.edit->focused = 1;

    int ex = (icon >= 0) ? 56 : 12;
    p.edit->r = (W2kRect){ ex, 44, cw - ex - 12, 21 };
    p.ok     = (W2kRect){ cw - 12 - 75 * 2 - 6, chh - 12 - 23, 75, 23 };
    p.cancel = (W2kRect){ cw - 12 - 75, chh - 12 - 23, 75, 23 };

    w->user = &p;
    w->paint = prompt_paint;
    w->event = prompt_event;
    w2k_win_center(w, over);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);

    w2k_add_timer(w2k_caret_blink, blink_cb, p.edit);
    int r = w2k_win_modal(w);
    w2k_del_timer(blink_cb, p.edit);

    if (r == ID_OK) snprintf(out, outsz, "%s", w2k_edit_text(p.edit));
    w2k_edit_free(p.edit);
    return r == ID_OK;
}

/* ------------------------------------------------------------------ *
 * File open / save
 * ------------------------------------------------------------------ */
/* The places bar down the left, as Windows 2000 added it: a column of
 * shortcuts on a grey ground. Ours names folders this shell actually
 * has. */
static const struct { const char *label; int icon; const char *sub; }
places[] = {
    { "Desktop",      ICO_DESKTOP,    "/Desktop"        },
    { "My Documents", ICO_MYDOCS,     ""                },
    { "Favorites",    ICO_FAVORITES,  "/.w2k/Favorites" },
    { "My Computer",  ICO_MYCOMPUTER, NULL              },   /* root */
};
#define NPLACES ((int)(sizeof places / sizeof *places))

#define FD_MAXFILTER 8

typedef struct {
    char      dir[1024];
    int       save;
    W2kList  *list;
    W2kEdit  *name;
    W2kCombo *look;
    W2kCombo *type;                 /* Files of type */
    W2kRect   ok, cancel, up, newfolder, bar;
    int       down;
    int       place_hot;            /* the place being pressed, or -1 */
    W2kWin   *w;
    int       accepted;
    struct { char label[64], pattern[64]; } filter[FD_MAXFILTER];
    int       nfilters;
} FileDlg;

/* Does a name match the chosen filter? Patterns are the simple "*.txt"
 * kind -- that is all a shell file dialog has ever needed. */
static int fd_matches(FileDlg *f, const char *name)
{
    if (f->nfilters <= 0) return 1;
    int i = f->type && f->type->sel >= 0 ? f->type->sel : 0;
    if (i >= f->nfilters) return 1;
    const char *pat = f->filter[i].pattern;
    if (!pat[0] || !strcmp(pat, "*") || !strcmp(pat, "*.*")) return 1;

    /* Several extensions may be separated by semicolons. */
    const char *p = pat;
    while (*p) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > 1 && p[0] == '*') {
            size_t ext = len - 1;                  /* ".txt" */
            size_t nl = strlen(name);
            if (nl > ext && !strncasecmp(name + nl - ext, p + 1, ext)) return 1;
        }
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static void fd_fill(FileDlg *f)
{
    w2k_list_clear(f->list);
    DIR *dp = opendir(f->dir);
    if (!dp) return;

    char *dirs[2048], *files[2048];
    int nd = 0, nf = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.' && strcmp(de->d_name, "..") &&
            !w2k_folder_hidden) continue;
        if (!strcmp(de->d_name, "..") && !strcmp(f->dir, "/")) continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", f->dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (nd < 2048) dirs[nd++] = w2k_strdup(de->d_name); }
        else if (fd_matches(f, de->d_name)) {
            if (nf < 2048) files[nf++] = w2k_strdup(de->d_name);
        }
    }
    closedir(dp);
    qsort(dirs, nd, sizeof *dirs, name_cmp);
    qsort(files, nf, sizeof *files, name_cmp);

    for (int i = 0; i < nd; i++) {
        int r = w2k_list_add(f->list, ICO_FOLDER, (void *)(long)1);
        w2k_list_set(f->list, r, 0, dirs[i]);
        free(dirs[i]);
    }
    for (int i = 0; i < nf; i++) {
        int r = w2k_list_add(f->list, w2k_file_icon(files[i], 0), NULL);
        w2k_list_set(f->list, r, 0, files[i]);
        free(files[i]);
    }
    w2k_combo_clear(f->look);
    w2k_combo_add(f->look, f->dir);
}

static void fd_chdir(FileDlg *f, const char *sub)
{
    char next[2048];
    if (!strcmp(sub, "..")) {
        snprintf(next, sizeof next, "%s", f->dir);
        char *slash = strrchr(next, '/');
        if (slash && slash != next) *slash = 0;
        else strcpy(next, "/");
    } else if (sub[0] == '/') {
        snprintf(next, sizeof next, "%s", sub);
    } else {
        snprintf(next, sizeof next, "%s%s%s", f->dir,
                 strcmp(f->dir, "/") ? "/" : "", sub);
    }
    snprintf(f->dir, sizeof f->dir, "%s", next);
    fd_fill(f);
    f->list->vsb.pos = 0;
}

static void fd_activate(void *user, int idx)
{
    FileDlg *f = user;
    if (idx < 0) return;
    const char *nm = f->list->items[idx].text[0];
    if (f->list->items[idx].data) { fd_chdir(f, nm); }
    else {
        w2k_edit_set(f->name, nm);
        f->accepted = 1;
        w2k_win_close(f->w, ID_OK);
    }
    w2k_win_dirty(f->w);
}

static void fd_select(void *user, int idx)
{
    FileDlg *f = user;
    if (idx >= 0 && !f->list->items[idx].data)
        w2k_edit_set(f->name, f->list->items[idx].text[0]);
    w2k_win_dirty(f->w);
}

/* Jump to one of the places down the left. */
static void fd_goto_place(FileDlg *f, int i)
{
    const char *home = getenv("HOME");
    char path[1100];
    if (!places[i].sub) snprintf(path, sizeof path, "/");
    else snprintf(path, sizeof path, "%.900s%s", home ? home : "/",
                  places[i].sub);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (!places[i].sub) return;
        /* Favorites need not exist yet; the rest we leave alone. */
        if (strstr(places[i].sub, "Favorites")) {
            char parent[1100];
            snprintf(parent, sizeof parent, "%.900s/.w2k", home ? home : "/");
            mkdir(parent, 0755);
            if (mkdir(path, 0755) != 0) return;
        } else return;
    }
    fd_chdir(f, path);
}

/* The toolbar's Create New Folder, which Windows has here too. */
static void fd_new_folder(FileDlg *f)
{
    char name[256];
    if (!w2k_prompt(f->w, "New Folder", "&Folder name:", "New Folder", name,
                    sizeof name, ICO_FOLDER))
        return;
    if (!name[0] || strchr(name, '/')) return;
    char path[1400];
    snprintf(path, sizeof path, "%.1000s/%.200s", f->dir, name);
    if (mkdir(path, 0755) != 0) {
        char msg[1500];
        snprintf(msg, sizeof msg, "Cannot create the folder.\n\n%s",
                 strerror(errno));
        w2k_msgbox(f->w, "New Folder", msg, MB_OK | MB_ICONERROR);
        return;
    }
    fd_fill(f);
}

/* One place: a big icon over a centred label, on the grey ground. The
 * pressed one is drawn sunk, which is how Windows shows the folder the
 * dialog is currently in. */
static void fd_place_draw(Drawable d, FileDlg *f, int i, int y, int h)
{
    int x = f->bar.x + 2, w = f->bar.w - 4;
    int pressed = (f->place_hot == i);
    if (pressed) w2k_edge(d, x, y, w, h, EDGE_SUNKEN, BF_RECT);
    w2k_bigicon_draw(d, x + (w - 32) / 2 + pressed, y + 4 + pressed,
                     places[i].icon);
    int fh = w2k_font_height(F_UI);
    char buf[64];
    w2k_ellipsis(F_UI, places[i].label, w - 6, buf, sizeof buf);
    int tw = w2k_text_width(F_UI, buf, -1);
    w2k_text(d, F_UI, x + (w - tw) / 2 + pressed, y + h - fh - 4 + pressed,
             buf, C_HILIGHT);
}

static void fd_paint(W2kWin *w, Drawable d)
{
    FileDlg *f = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_text_mnemonic(d, F_UI, 10, 14, "Look &in:", C_TEXT, 1);
    w2k_combo_draw(d, f->look);
    w2k_draw_pushbutton(d, &f->up, NULL, f->down == 3 ? BS_PRESSED : 0);
    w2k_icon_draw(d, f->up.x + (f->up.w - 16) / 2, f->up.y + (f->up.h - 16) / 2,
                  ICO_UP);
    w2k_draw_pushbutton(d, &f->newfolder, NULL, f->down == 4 ? BS_PRESSED : 0);
    w2k_icon_draw(d, f->newfolder.x + (f->newfolder.w - 16) / 2,
                  f->newfolder.y + (f->newfolder.h - 16) / 2, ICO_FOLDER);

    /* The places bar: a sunken well filled with the shadow colour. */
    w2k_edge(d, f->bar.x, f->bar.y, f->bar.w, f->bar.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, f->bar.x + 2, f->bar.y + 2, f->bar.w - 4, f->bar.h - 4,
             C_SHADOW);
    int ph = (f->bar.h - 4) / NPLACES;
    for (int i = 0; i < NPLACES; i++)
        fd_place_draw(d, f, i, f->bar.y + 2 + i * ph, ph);

    w2k_list_draw(d, f->list);

    w2k_text_mnemonic(d, F_UI, f->bar.x, f->name->r.y + (21 - fh) / 2,
                      "File &name:", C_TEXT, 1);
    w2k_edit_draw(d, f->name);
    if (f->nfilters > 0) {
        w2k_text_mnemonic(d, F_UI, f->bar.x, f->type->r.y + (21 - fh) / 2,
                          "Files of &type:", C_TEXT, 1);
        w2k_combo_draw(d, f->type);
    }
    w2k_draw_pushbutton(d, &f->ok, f->save ? "&Save" : "&Open",
                        BS_DEFAULT | (f->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &f->cancel, "Cancel", f->down == 2 ? BS_PRESSED : 0);
}

static int fd_event(W2kWin *w, XEvent *e)
{
    FileDlg *f = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_combo_press(f->look, &e->xbutton)) {
            if (f->look->sel >= 0) fd_chdir(f, f->look->items[f->look->sel]);
            w2k_win_dirty(w);
            return 1;
        }
        if (f->nfilters > 0 && w2k_combo_press(f->type, &e->xbutton)) {
            fd_fill(f);                       /* a different set of files */
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_rect_hit(&f->bar, e->xbutton.x, e->xbutton.y)) {
            int ph = (f->bar.h - 4) / NPLACES;
            int i = (e->xbutton.y - f->bar.y - 2) / (ph > 0 ? ph : 1);
            if (i >= 0 && i < NPLACES) {
                f->place_hot = i;
                fd_goto_place(f, i);
            }
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_rect_hit(&f->up, e->xbutton.x, e->xbutton.y)) {
            f->down = 3;
            fd_chdir(f, "..");
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_rect_hit(&f->newfolder, e->xbutton.x, e->xbutton.y)) {
            f->down = 4;
            fd_new_folder(f);
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_list_press(f->list, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_edit_press(f->name, &e->xbutton)) {
            f->list->focused = 0;
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_rect_hit(&f->ok, e->xbutton.x, e->xbutton.y))     f->down = 1;
        else if (w2k_rect_hit(&f->cancel, e->xbutton.x, e->xbutton.y)) f->down = 2;
        w2k_win_dirty(w);
        return 1;

    case MotionNotify:
        if (w2k_list_motion(f->list, &e->xmotion) ||
            w2k_edit_motion(f->name, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;

    case ButtonRelease: {
        w2k_list_release(f->list, &e->xbutton);
        w2k_edit_release(f->name);
        int d = f->down;
        f->down = 0;
        f->place_hot = -1;
        if (d == 1 && w2k_rect_hit(&f->ok, e->xbutton.x, e->xbutton.y)) {
            f->accepted = 1;
            w2k_win_close(w, ID_OK);
        } else if (d == 2 && w2k_rect_hit(&f->cancel, e->xbutton.x, e->xbutton.y))
            w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            const char *nm = w2k_edit_text(f->name);
            struct stat st;
            char full[2048];
            snprintf(full, sizeof full, "%s%s%s", f->dir,
                     strcmp(f->dir, "/") ? "/" : "", nm);
            if (!f->save && stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                fd_chdir(f, nm);
            } else {
                f->accepted = 1;
                w2k_win_close(w, ID_OK);
            }
            w2k_win_dirty(w);
            return 1;
        }
        if (f->list->focused && w2k_list_key(f->list, &e->xkey)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_edit_key(f->name, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

#define FD_BAR_W 87            /* the places bar, measured off Windows */

static void fd_resized(W2kWin *w)
{
    FileDlg *f = w->user;
    int rows = f->nfilters > 0 ? 2 : 1;
    int bottom = w->h - (rows == 2 ? 66 : 38);

    f->look->r    = (W2kRect){ 62, 10, w->w - 62 - 70, 21 };
    f->up         = (W2kRect){ w->w - 64, 10, 24, 21 };
    f->newfolder  = (W2kRect){ w->w - 34, 10, 24, 21 };
    f->bar        = (W2kRect){ 10, 38, FD_BAR_W, bottom - 48 };
    f->list->r    = (W2kRect){ 10 + FD_BAR_W + 8, 38,
                               w->w - 20 - FD_BAR_W - 8, bottom - 48 };
    f->name->r    = (W2kRect){ 90, bottom - 4, w->w - 90 - 95, 21 };
    f->ok         = (W2kRect){ w->w - 85, bottom - 4, 75, 23 };
    if (f->type)
        f->type->r = (W2kRect){ 90, bottom + 24, w->w - 90 - 95, 21 };
    f->cancel     = (W2kRect){ w->w - 85, bottom + 24, 75, 23 };
}

int w2k_file_dialog(W2kWin *over, int save, char *path, int pathsz)
{
    return w2k_file_dialog_filter(over, save, path, pathsz, NULL);
}

/* `filters` is "Label|pattern|Label|pattern|..." -- the same idea as the
 * Windows filter string, with a bar instead of its embedded NULs. */
static void fd_parse_filters(FileDlg *f, const char *spec)
{
    if (!spec || !*spec) return;
    const char *p = spec;
    while (*p && f->nfilters < FD_MAXFILTER) {
        const char *bar = strchr(p, '|');
        if (!bar) break;
        size_t n = (size_t)(bar - p);
        if (n >= sizeof f->filter[0].label) n = sizeof f->filter[0].label - 1;
        memcpy(f->filter[f->nfilters].label, p, n);
        f->filter[f->nfilters].label[n] = 0;

        p = bar + 1;
        bar = strchr(p, '|');
        n = bar ? (size_t)(bar - p) : strlen(p);
        if (n >= sizeof f->filter[0].pattern) n = sizeof f->filter[0].pattern - 1;
        memcpy(f->filter[f->nfilters].pattern, p, n);
        f->filter[f->nfilters].pattern[n] = 0;
        f->nfilters++;
        if (!bar) break;
        p = bar + 1;
    }
}

int w2k_file_dialog_filter(W2kWin *over, int save, char *path, int pathsz,
                           const char *filters)
{
    FileDlg f = { .save = save };
    f.place_hot = -1;
    fd_parse_filters(&f, filters);
    W2kWin *w = w2k_win_new(save ? "Save As" : "Open", "w2kdialog",
                            560, f.nfilters ? 350 : 330, 1);
    f.w = w;
    f.list = w2k_list_new(LV_LIST);
    f.list->user = &f;
    f.list->on_activate = fd_activate;
    f.list->on_select = fd_select;
    f.list->focused = 0;                 /* typing goes to the name box */
    w2k_scroll_bind(&f.list->vsb, w);
    w2k_scroll_bind(&f.list->hsb, w);
    f.name = w2k_edit_new(0);
    w2k_edit_bind(f.name, w);
    f.look = w2k_combo_new(0);
    if (f.nfilters > 0) {
        f.type = w2k_combo_new(0);
        for (int i = 0; i < f.nfilters; i++)
            w2k_combo_add(f.type, f.filter[i].label);
        f.type->sel = 0;
    }

    /* Split the incoming path into a directory and a file name. */
    snprintf(f.dir, sizeof f.dir, "%s", path && *path ? path : ".");
    struct stat st;
    if (stat(f.dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char *slash = strrchr(f.dir, '/');
        if (slash) {
            w2k_edit_set(f.name, slash + 1);
            if (slash == f.dir) f.dir[1] = 0;
            else *slash = 0;
        } else {
            w2k_edit_set(f.name, f.dir);
            snprintf(f.dir, sizeof f.dir, ".");
        }
    }
    if (!strcmp(f.dir, ".")) {
        if (!getcwd(f.dir, sizeof f.dir)) snprintf(f.dir, sizeof f.dir, "/");
    }
    fd_fill(&f);
    f.name->focused = 1;
    w2k_edit_select_all(f.name);

    w->user = &f;
    w->paint = fd_paint;
    w->event = fd_event;
    w->resized = fd_resized;
    w->min_w = 380;
    w->min_h = 260;
    fd_resized(w);
    w2k_win_center(w, over);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);

    w2k_add_timer(w2k_caret_blink, blink_cb, f.name);
    w2k_win_modal(w);
    w2k_del_timer(blink_cb, f.name);

    if (f.type) w2k_combo_free(f.type);
    int ok = f.accepted;
    if (ok) {
        const char *nm = w2k_edit_text(f.name);
        if (nm[0] == '/') snprintf(path, pathsz, "%s", nm);
        else snprintf(path, pathsz, "%s%s%s", f.dir,
                      strcmp(f.dir, "/") ? "/" : "", nm);
        if (!nm[0]) ok = 0;
    }
    w2k_list_free(f.list);
    w2k_edit_free(f.name);
    w2k_combo_free(f.look);
    return ok;
}
