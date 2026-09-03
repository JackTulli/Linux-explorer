/* w2kcalc.c -- Calculator, Standard and Scientific.
 *
 * The Windows calculator is a state machine with a display: a pending
 * operand, a pending operator, and a flag saying whether the next digit
 * starts a new number or extends the current one. Everything else --
 * percent, memory, the sign toggle -- acts on those three.
 *
 * Arithmetic is in double, like the original at this size. Numbers are
 * shown with as many significant digits as fit and no trailing zeros,
 * which is what makes 0.1 + 0.2 read as 0.3 rather than 0.30000000000000004. */
#include "w2k.h"
#include "w2kui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISP_H    26
#define BTN_W     34
#define BTN_H     24
#define GAP        4

enum { ID_COPY = 1, ID_PASTE, ID_STANDARD, ID_SCIENTIFIC, ID_ABOUT, ID_EXIT };

typedef struct {
    const char *label;
    int   row, col, w;          /* grid position, in button units */
    int   key;                  /* what it does: an ASCII code or a K_* */
    int   colour;               /* 0 normal, 1 red, 2 blue           */
} Btn;

/* Command codes that are not simple characters. */
enum {
    K_BACK = 128, K_CE, K_C, K_MC, K_MR, K_MS, K_MPLUS, K_SIGN, K_SQRT,
    K_PCT, K_INV, K_EQ, K_SIN, K_COS, K_TAN, K_LOG, K_LN, K_EXP, K_POW,
    K_PI, K_FACT
};

/* The Standard layout, exactly as Windows arranges it. */
static const Btn std_btns[] = {
    { "Backspace", 0, 2, 2, K_BACK, 1 }, { "CE", 0, 4, 1, K_CE, 1 },
    { "C", 0, 5, 1, K_C, 1 },

    { "MC", 1, 0, 1, K_MC, 1 }, { "7", 1, 2, 1, '7', 2 },
    { "8", 1, 3, 1, '8', 2 }, { "9", 1, 4, 1, '9', 2 },
    { "/", 1, 5, 1, '/', 1 }, { "sqrt", 1, 6, 1, K_SQRT, 0 },

    { "MR", 2, 0, 1, K_MR, 1 }, { "4", 2, 2, 1, '4', 2 },
    { "5", 2, 3, 1, '5', 2 }, { "6", 2, 4, 1, '6', 2 },
    { "*", 2, 5, 1, '*', 1 }, { "%", 2, 6, 1, K_PCT, 0 },

    { "MS", 3, 0, 1, K_MS, 1 }, { "1", 3, 2, 1, '1', 2 },
    { "2", 3, 3, 1, '2', 2 }, { "3", 3, 4, 1, '3', 2 },
    { "-", 3, 5, 1, '-', 1 }, { "1/x", 3, 6, 1, K_INV, 0 },

    { "M+", 4, 0, 1, K_MPLUS, 1 }, { "0", 4, 2, 1, '0', 2 },
    { "+/-", 4, 3, 1, K_SIGN, 2 }, { ".", 4, 4, 1, '.', 2 },
    { "+", 4, 5, 1, '+', 1 }, { "=", 4, 6, 1, K_EQ, 1 },
    { NULL, 0, 0, 0, 0, 0 }
};

/* Scientific adds a row of functions above the standard keypad. */
static const Btn sci_btns[] = {
    { "sin", -1, 0, 1, K_SIN, 0 }, { "cos", -1, 1, 1, K_COS, 0 },
    { "tan", -1, 2, 1, K_TAN, 0 }, { "log", -1, 3, 1, K_LOG, 0 },
    { "ln", -1, 4, 1, K_LN, 0 },   { "n!", -1, 5, 1, K_FACT, 0 },
    { "x^y", -1, 6, 1, K_POW, 0 },
    { NULL, 0, 0, 0, 0, 0 }
};

static struct {
    W2kWin     *win;
    W2kMenubar *mb;
    char        display[64];
    double      acc;              /* the value waiting for an operator */
    double      memory;
    int         pending;          /* the operator waiting, or 0        */
    int         fresh;            /* the next digit starts a new number */
    int         scientific;
    int         error;
    int         down;             /* index of the button being pressed  */
    W2kRect     rect[40];
    const Btn  *btn[40];
    int         nbtn;
} cal;

/* ------------------------------------------------------------------ *
 * The number in the display
 * ------------------------------------------------------------------ */
static void show(double v)
{
    if (!isfinite(v)) {
        snprintf(cal.display, sizeof cal.display, "Cannot divide by zero.");
        cal.error = 1;
        return;
    }
    /* Enough digits to be useful, none of the noise: print at high
     * precision, then trim the trailing zeros and any bare point. */
    snprintf(cal.display, sizeof cal.display, "%.15g", v);
    if (strchr(cal.display, '.') && !strchr(cal.display, 'e')) {
        char *end = cal.display + strlen(cal.display) - 1;
        while (end > cal.display && *end == '0') *end-- = 0;
        if (*end == '.') *end = 0;
    }
}

static double current(void) { return atof(cal.display); }

static void apply_pending(void)
{
    double b = current();
    switch (cal.pending) {
    case '+': cal.acc += b; break;
    case '-': cal.acc -= b; break;
    case '*': cal.acc *= b; break;
    case '/': cal.acc = (b == 0) ? INFINITY : cal.acc / b; break;
    case K_POW: cal.acc = pow(cal.acc, b); break;
    default:  cal.acc = b; break;
    }
    show(cal.acc);
}

static double factorial(double v)
{
    if (v < 0 || v > 170 || v != floor(v)) return NAN;
    double r = 1;
    for (int i = 2; i <= (int)v; i++) r *= i;
    return r;
}

static void press(int key)
{
    if (cal.error && key != K_C && key != K_CE) return;

    if (key >= '0' && key <= '9') {
        if (cal.fresh || !strcmp(cal.display, "0")) {
            snprintf(cal.display, sizeof cal.display, "%c", (char)key);
            cal.fresh = 0;
        } else if (strlen(cal.display) < 24) {
            size_t n = strlen(cal.display);
            cal.display[n] = (char)key;
            cal.display[n + 1] = 0;
        }
        return;
    }
    switch (key) {
    case '.':
        if (cal.fresh) { snprintf(cal.display, sizeof cal.display, "0."); cal.fresh = 0; }
        else if (!strchr(cal.display, '.'))
            strncat(cal.display, ".", sizeof cal.display - strlen(cal.display) - 1);
        return;
    case K_BACK:
        if (cal.fresh) return;
        if (strlen(cal.display) > 1) cal.display[strlen(cal.display) - 1] = 0;
        else snprintf(cal.display, sizeof cal.display, "0");
        return;
    case K_CE:
        snprintf(cal.display, sizeof cal.display, "0");
        cal.fresh = 1;
        cal.error = 0;
        return;
    case K_C:
        snprintf(cal.display, sizeof cal.display, "0");
        cal.acc = 0;
        cal.pending = 0;
        cal.fresh = 1;
        cal.error = 0;
        return;
    case K_SIGN: {
        double v = -current();
        show(v);
        return;
    }
    case K_SQRT: show(sqrt(current())); cal.fresh = 1; return;
    case K_INV:  show(current() == 0 ? INFINITY : 1.0 / current()); cal.fresh = 1; return;
    case K_PCT:  show(cal.acc * current() / 100.0); cal.fresh = 1; return;
    case K_SIN:  show(sin(current())); cal.fresh = 1; return;
    case K_COS:  show(cos(current())); cal.fresh = 1; return;
    case K_TAN:  show(tan(current())); cal.fresh = 1; return;
    case K_LOG:  show(log10(current())); cal.fresh = 1; return;
    case K_LN:   show(log(current())); cal.fresh = 1; return;
    case K_FACT: show(factorial(current())); cal.fresh = 1; return;
    case K_MC:   cal.memory = 0; return;
    case K_MR:   show(cal.memory); cal.fresh = 1; return;
    case K_MS:   cal.memory = current(); cal.fresh = 1; return;
    case K_MPLUS: cal.memory += current(); cal.fresh = 1; return;

    case '+': case '-': case '*': case '/': case K_POW:
        if (cal.pending && !cal.fresh) apply_pending();
        else cal.acc = current();
        cal.pending = key;
        cal.fresh = 1;
        return;

    case K_EQ:
        if (cal.pending) apply_pending();
        else cal.acc = current();
        cal.pending = 0;
        cal.fresh = 1;
        return;
    }
}

/* ------------------------------------------------------------------ *
 * Layout and painting
 * ------------------------------------------------------------------ */
static void build_buttons(void)
{
    cal.nbtn = 0;
    int top = MENUBAR_H + 6 + DISP_H + 8;
    if (cal.scientific) {
        for (const Btn *b = sci_btns; b->label; b++) {
            cal.btn[cal.nbtn] = b;
            cal.rect[cal.nbtn++] = (W2kRect){
                8 + b->col * (BTN_W + GAP), top, BTN_W, BTN_H };
        }
        top += BTN_H + GAP + 4;
    }
    for (const Btn *b = std_btns; b->label; b++) {
        cal.btn[cal.nbtn] = b;
        cal.rect[cal.nbtn++] = (W2kRect){
            8 + b->col * (BTN_W + GAP), top + b->row * (BTN_H + GAP),
            b->w * BTN_W + (b->w - 1) * GAP, BTN_H };
    }
}

static void paint(W2kWin *w, Drawable d)
{
    w2k_menubar_draw(d, cal.mb);

    /* The display: a sunken well with the number right-aligned. */
    W2kRect disp = { 8, MENUBAR_H + 6, w->w - 16, DISP_H };
    w2k_edge(d, disp.x, disp.y, disp.w, disp.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, disp.x + 2, disp.y + 2, disp.w - 4, disp.h - 4, C_WINDOW);
    int tw = w2k_text_width(F_UI, cal.display, -1);
    w2k_text(d, F_UI, disp.x + disp.w - 8 - tw,
             disp.y + (disp.h - w2k_font_height(F_UI)) / 2, cal.display,
             cal.error ? C_GRAYTEXT : C_WINDOWTEXT);

    /* Memory indicator, as the original shows it. */
    if (cal.memory != 0) {
        W2kRect mem = { 8, disp.y + disp.h + 6, 34, 20 };
        w2k_edge(d, mem.x, mem.y, mem.w, mem.h, EDGE_SUNKEN, BF_RECT);
        w2k_text(d, F_UI, mem.x + 12, mem.y + 3, "M", C_TEXT);
    }

    for (int i = 0; i < cal.nbtn; i++) {
        const Btn *b = cal.btn[i];
        W2kRect r = cal.rect[i];
        w2k_draw_pushbutton(d, &r, "", cal.down == i ? BS_PRESSED : 0);
        int tw2 = w2k_text_width(F_UI, b->label, -1);
        int o = cal.down == i ? 1 : 0;
        int tx = r.x + (r.w - tw2) / 2 + o;
        int ty = r.y + (r.h - w2k_font_height(F_UI)) / 2 + o;

        /* The original's key colours: digits blue, operators and the
         * memory and clear keys red, functions black. */
        if (b->colour == 1)      w2k_text_rgb(d, F_UI, tx, ty, b->label, 200, 0, 0);
        else if (b->colour == 2) w2k_text_rgb(d, F_UI, tx, ty, b->label, 0, 0, 190);
        else                     w2k_text(d, F_UI, tx, ty, b->label, C_TEXT);
    }
}

/* ------------------------------------------------------------------ *
 * Menus, input
 * ------------------------------------------------------------------ */
static W2kMenu *build_edit(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_COPY, "&Copy", "Ctrl+C", ICO_COPY);
    w2k_menu_item(m, ID_PASTE, "&Paste", "Ctrl+V", ICO_PASTE);
    return m;
}

static W2kMenu *build_view(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_STANDARD, "&Standard", NULL, ICO_NONE);
    w2k_menu_radio(m, !cal.scientific);
    w2k_menu_item(m, ID_SCIENTIFIC, "S&cientific", NULL, ICO_NONE);
    w2k_menu_radio(m, cal.scientific);
    return m;
}

static W2kMenu *build_help(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_ABOUT, "&About Calculator", NULL, ICO_INFO);
    return m;
}

static void resize_window(void)
{
    int rows = 5 + (cal.scientific ? 1 : 0);
    int w = 8 * 2 + 7 * BTN_W + 6 * GAP;
    int h = MENUBAR_H + 6 + DISP_H + 8 + rows * (BTN_H + GAP) + 8;
    if (cal.scientific) h += 4;
    /* The window is fixed-size and changes with the mode, so ask the
     * manager for the new size and tell it this is still not resizable. */
    XSizeHints sh = { 0 };
    sh.flags = PMinSize | PMaxSize;
    sh.min_width = sh.max_width = w;
    sh.min_height = sh.max_height = h;
    XSetWMNormalHints(w2k.dpy, cal.win->win, &sh);
    XResizeWindow(w2k.dpy, cal.win->win, (unsigned)w, (unsigned)h);
    cal.win->w = w;
    cal.win->h = h;
    if (cal.win->buf) { w2k_free_pixmap(cal.win->buf); cal.win->buf = 0; }
    cal.mb->r = (W2kRect){ 0, 0, w, MENUBAR_H };
    build_buttons();
}

static void command(void *u, int id)
{
    (void)u;
    switch (id) {
    case ID_COPY:  w2k_clipboard_set(cal.display); break;
    case ID_PASTE: {
        char *t = w2k_clipboard_get();
        if (t) {
            show(atof(t));
            cal.fresh = 1;
            free(t);
        }
        break;
    }
    case ID_STANDARD:   cal.scientific = 0; resize_window(); break;
    case ID_SCIENTIFIC: cal.scientific = 1; resize_window(); break;
    case ID_ABOUT:
        w2k_msgbox(cal.win, "About Calculator",
                   "Calculator\nWindows 2000 for X11", MB_OK | MB_ICONINFO);
        break;
    case ID_EXIT: w2k_win_close(cal.win, 0); break;
    }
    w2k_win_dirty(cal.win);
}

static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        if (w2k_menubar_press(cal.mb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        for (int i = 0; i < cal.nbtn; i++)
            if (w2k_rect_hit(&cal.rect[i], e->xbutton.x, e->xbutton.y)) {
                cal.down = i;
                w2k_win_dirty(w);
                return 1;
            }
        return 1;
    }
    case ButtonRelease: {
        int i = cal.down;
        cal.down = -1;
        if (i >= 0 && i < cal.nbtn &&
            w2k_rect_hit(&cal.rect[i], e->xbutton.x, e->xbutton.y))
            press(cal.btn[i]->key);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        char buf[8];
        KeySym ks;
        int n = XLookupString(&e->xkey, buf, sizeof buf - 1, &ks, NULL);
        int ctrl = (e->xkey.state & ControlMask) != 0;
        if (ctrl && (ks == XK_c || ks == XK_C)) { command(NULL, ID_COPY); return 1; }
        if (ctrl && (ks == XK_v || ks == XK_V)) { command(NULL, ID_PASTE); return 1; }
        switch (ks) {
        case XK_Escape:    press(K_C); break;
        case XK_Delete:    press(K_CE); break;
        case XK_BackSpace: press(K_BACK); break;
        case XK_Return: case XK_KP_Enter: press(K_EQ); break;
        default:
            if (n == 1) {
                char c = buf[0];
                if ((c >= '0' && c <= '9') || c == '.' || c == '+' ||
                    c == '-' || c == '*' || c == '/')
                    press(c);
                else if (c == '=') press(K_EQ);
                else if (c == '%') press(K_PCT);
                else if (c == 'r') press(K_INV);
                else if (c == '@') press(K_SQRT);
            }
            break;
        }
        w2k_win_dirty(w);
        return 1;
    }
    }
    return 0;
}

int main(void)
{
    if (w2k_init("w2kcalc") < 0) return 1;

    snprintf(cal.display, sizeof cal.display, "0");
    cal.fresh = 1;
    cal.down = -1;

    cal.win = w2k_win_new("Calculator", "w2kcalc", 260, 220, 0);
    cal.win->paint = paint;
    cal.win->event = event;

    cal.mb = w2k_menubar_new(NULL, command);
    w2k_menubar_add(cal.mb, "&Edit", build_edit);
    w2k_menubar_add(cal.mb, "&View", build_view);
    w2k_menubar_add(cal.mb, "&Help", build_help);

    resize_window();
    w2k_win_center(cal.win, NULL);
    w2k_win_show(cal.win);
    w2k_run();
    w2k_fini();
    return 0;
}
