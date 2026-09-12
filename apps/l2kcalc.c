/* l2kcalc.c -- Calculator, Standard and Scientific.
 *
 * The display holds an expression string. Digits and operators are
 * appended as the user types; "=" parses the whole string with
 * precedence  ^  >  * /  >  + -  and unary minus, e.g.
 *     -7+5-2*3/2^3
 * Malformed input such as "2* /2" (operator with no operand) reports an error.
 *
 * Unary functions (sqrt, sin, 1/x, ...) act on the current value.
 * Memory works on the current value. Keyboard + - * / ^ light up the
 * matching button while held.
 *
 * Arithmetic is in double. Results are shown without trailing zeros. */
#include "w2k.h"
#include "w2kui.h"
#include <X11/keysym.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISP_H    26
#define BTN_W     34
#define BTN_H     24
#define GAP        4
#define EXPR_MAX  256

enum { ID_COPY = 1, ID_PASTE, ID_STANDARD, ID_SCIENTIFIC, ID_ABOUT, ID_EXIT };

typedef struct {
    const char *label;
    int   row, col, w;
    int   key;
    int   colour;               /* 0 normal, 1 red, 2 blue */
} Btn;

enum {
    K_BACK = 128, K_CE, K_C, K_MC, K_MR, K_MS, K_MPLUS, K_SIGN, K_SQRT,
    K_PCT, K_INV, K_EQ, K_SIN, K_COS, K_TAN, K_LOG, K_LN, K_EXP, K_POW,
    K_PI, K_FACT
};

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
    char        expr[EXPR_MAX];
    int         len;
    double      memory;
    int         scientific;
    int         error;
    int         just_eq;
    int         down;
    int         glow_key;
    W2kRect     rect[40];
    const Btn  *btn[40];
    int         nbtn;
} cal;

/* ---- expression parser -------------------------------------------
SGFsYW5v
string stream parsing
*/

typedef struct {
    const char *p;
    const char *err;
} Parser;

static void skip_ws(Parser *pr)
{
    while (*pr->p == ' ' || *pr->p == '\t') pr->p++;
}

static int set_err(Parser *pr, const char *msg)
{
    if (!pr->err) pr->err = msg;
    return 0;
}

static int parse_expr(Parser *pr, double *out);

static int parse_number(Parser *pr, double *out)
{
    skip_ws(pr);
    if (!isdigit((unsigned char)*pr->p) && *pr->p != '.')
        return set_err(pr, "Expected a number");
    /* Build a clean digit string, ignoring thousand separators (,). */
    char tmp[96];
    int n = 0;
    const char *s = pr->p;
    while (*s && n + 1 < (int)sizeof tmp) {
        if (isdigit((unsigned char)*s) || *s == '.' || *s == 'e' || *s == 'E' ||
            *s == '+' || *s == '-') {
            /* sign/exponent only after e/E */
            if ((*s == '+' || *s == '-') && n > 0 &&
                tmp[n - 1] != 'e' && tmp[n - 1] != 'E')
                break;
            tmp[n++] = *s++;
        } else if (*s == ',') {
            s++; /* thousand separator — skip */
        } else {
            break;
        }
    }
    tmp[n] = 0;
    if (!n) return set_err(pr, "Invalid number");
    char *end = NULL;
    *out = strtod(tmp, &end);
    if (end == tmp) return set_err(pr, "Invalid number");
    pr->p = s;
    return 1;
}

static int parse_primary(Parser *pr, double *out)
{
    skip_ws(pr);
    if (*pr->p == '(') {
        pr->p++;
        if (!parse_expr(pr, out)) return 0;
        skip_ws(pr);
        if (*pr->p != ')') return set_err(pr, "Missing ')'");
        pr->p++;
        return 1;
    }
    return parse_number(pr, out);
}

static int parse_unary(Parser *pr, double *out)
{
    skip_ws(pr);
    if (*pr->p == '-') {
        pr->p++;
        if (!parse_unary(pr, out)) return 0;
        *out = -*out;
        return 1;
    }
    if (*pr->p == '+') {
        pr->p++;
        return parse_unary(pr, out);
    }
    return parse_primary(pr, out);
}

static int parse_power(Parser *pr, double *out)
{
    if (!parse_unary(pr, out)) return 0;
    skip_ws(pr);
    if (*pr->p == '^') {
        pr->p++;
        double rhs;
        if (!parse_power(pr, &rhs)) return 0;
        *out = pow(*out, rhs);
    }
    return 1;
}

static int parse_term(Parser *pr, double *out)
{
    if (!parse_power(pr, out)) return 0;
    for (;;) {
        skip_ws(pr);
        char op = *pr->p;
        if (op != '*' && op != '/') break;
        pr->p++;
        double rhs;
        if (!parse_power(pr, &rhs)) return 0;
        if (op == '*') *out *= rhs;
        else {
            if (rhs == 0.0) return set_err(pr, "Division by zero");
            *out /= rhs;
        }
    }
    return 1;
}

static int parse_expr(Parser *pr, double *out)
{
    if (!parse_term(pr, out)) return 0;
    for (;;) {
        skip_ws(pr);
        char op = *pr->p;
        if (op != '+' && op != '-') break;
        pr->p++;
        double rhs;
        if (!parse_term(pr, &rhs)) return 0;
        if (op == '+') *out += rhs;
        else *out -= rhs;
    }
    return 1;
}

static int evaluate(const char *s, double *out, const char **err)
{
    Parser pr = { .p = s, .err = NULL };
    skip_ws(&pr);
    if (!*pr.p) { *out = 0; return 1; }
    if (!parse_expr(&pr, out)) {
        *err = pr.err ? pr.err : "Syntax error";
        return 0;
    }
    skip_ws(&pr);
    if (*pr.p) {
        *err = "Unexpected characters";
        return 0;
    }
    if (!isfinite(*out)) {
        *err = "Result is not a number";
        return 0;
    }
    return 1;
}

/* ---- display buffer ---------------------------------------------- */

static void format_result(double v, char *buf, int n)
{
    if (!isfinite(v)) {
        snprintf(buf, (size_t)n, "Error");
        return;
    }
    if (fabs(v) < 1e-15) v = 0;

    /* Scientific notation for extremes — no thousands grouping there. */
    if ((fabs(v) >= 1e15 || (fabs(v) > 0 && fabs(v) < 1e-6)) &&
        fabs(v) != 0) {
        snprintf(buf, (size_t)n, "%.15g", v);
        return;
    }

    char raw[96];
    snprintf(raw, sizeof raw, "%.15g", v);
    if (strchr(raw, '.') && !strchr(raw, 'e') && !strchr(raw, 'E')) {
        char *end = raw + strlen(raw) - 1;
        while (end > raw && *end == '0') *end-- = 0;
        if (*end == '.') *end = 0;
    }

    /* Insert commas into the integer part: 3000 -> 3,000 */
    int neg = (raw[0] == '-');
    const char *ip = raw + (neg ? 1 : 0);
    const char *dot = strchr(ip, '.');
    int intlen = dot ? (int)(dot - ip) : (int)strlen(ip);
    const char *frac = dot ? dot : "";

    char out[128];
    int o = 0;
    if (neg && o + 1 < (int)sizeof out) out[o++] = '-';
    for (int i = 0; i < intlen; i++) {
        if (i > 0 && (intlen - i) % 3 == 0 && o + 1 < (int)sizeof out)
            out[o++] = ',';
        if (o + 1 < (int)sizeof out) out[o++] = ip[i];
    }
    while (*frac && o + 1 < (int)sizeof out) out[o++] = *frac++;
    out[o] = 0;
    snprintf(buf, (size_t)n, "%s", out);
}

static void show_value(double v)
{
    format_result(v, cal.expr, sizeof cal.expr);
    cal.len = (int)strlen(cal.expr);
    cal.error = 0;
}

static void show_error(const char *msg)
{
    snprintf(cal.expr, sizeof cal.expr, "%s", msg && *msg ? msg : "Error");
    cal.len = (int)strlen(cal.expr);
    cal.error = 1;
    cal.just_eq = 0;
}

static void expr_clear(void)
{
    snprintf(cal.expr, sizeof cal.expr, "0");
    cal.len = 1;
    cal.error = 0;
    cal.just_eq = 0;
}

static int is_binop(int key)
{
    return key == '+' || key == '-' || key == '*' || key == '/' || key == K_POW;
}

static void expr_append_char(char c)
{
    if (cal.len + 1 >= EXPR_MAX) return;
    if (cal.len == 1 && cal.expr[0] == '0' && c != '.' &&
        c != '+' && c != '-' && c != '*' && c != '/' && c != '^') {
        cal.expr[0] = c;
        cal.expr[1] = 0;
        cal.len = 1;
        return;
    }
    cal.expr[cal.len++] = c;
    cal.expr[cal.len] = 0;
}

static double current_value(void)
{
    if (cal.error) return 0;
    double v = 0;
    const char *err = NULL;
    if (evaluate(cal.expr, &v, &err)) return v;
    /* Fall back: trailing token, commas ignored. */
    int i = cal.len - 1;
    while (i >= 0 && (isdigit((unsigned char)cal.expr[i]) ||
                      cal.expr[i] == '.' || cal.expr[i] == ','))
        i--;
    if (i >= 0 && cal.expr[i] == '-' &&
        (i == 0 || is_binop((unsigned char)cal.expr[i - 1]) || cal.expr[i - 1] == '('))
        i--;
    char tmp[96];
    int n = 0;
    for (int j = i + 1; j < cal.len && n + 1 < (int)sizeof tmp; j++) {
        if (cal.expr[j] != ',') tmp[n++] = cal.expr[j];
    }
    tmp[n] = 0;
    return atof(tmp);
}

static void replace_trailing_number(double v)
{
    int i = cal.len - 1;
    while (i >= 0 && (isdigit((unsigned char)cal.expr[i]) ||
                      cal.expr[i] == '.' || cal.expr[i] == ','))
        i--;
    if (i >= 0 && cal.expr[i] == '-' &&
        (i == 0 || is_binop((unsigned char)cal.expr[i - 1]) || cal.expr[i - 1] == '('))
        i--;
    int start = i + 1;
    char num[64];
    format_result(v, num, sizeof num);
    if (!isfinite(v)) { show_error("Error"); return; }
    cal.expr[start] = 0;
    cal.len = start;
    size_t nl = strlen(num);
    if (cal.len + (int)nl >= EXPR_MAX) {
        show_value(v);
        cal.just_eq = 1;
        return;
    }
    memcpy(cal.expr + cal.len, num, nl + 1);
    cal.len += (int)nl;
    cal.error = 0;
}

static double factorial(double v)
{
    if (v < 0 || v > 170 || v != floor(v)) return NAN;
    double r = 1;
    for (int i = 2; i <= (int)v; i++) r *= i;
    return r;
}

/* ---- key handling ------------------------------------------------ */

static void press(int key)
{
    if (cal.error && key != K_C && key != K_CE && key != K_BACK) return;

    if (cal.just_eq) {
        if ((key >= '0' && key <= '9') || key == '.') {
            expr_clear();
            cal.just_eq = 0;
        } else if (is_binop(key)) {
            cal.just_eq = 0;
            cal.error = 0;
        } else if (key == K_EQ) {
            return;
        } else {
            cal.just_eq = 0;
        }
    }

    if (key >= '0' && key <= '9') {
        expr_append_char((char)key);
        return;
    }

    switch (key) {
    case '.':
        expr_append_char('.');
        return;

    case ',':
        /* Thousand separator in the number being typed. */
        expr_append_char(',');
        return;

    case K_BACK:
        if (cal.len > 1) cal.expr[--cal.len] = 0;
        else expr_clear();
        return;

    case K_CE:
        if (cal.error) { expr_clear(); return; }
        {
            int i = cal.len - 1;
            while (i >= 0 && (isdigit((unsigned char)cal.expr[i]) ||
                              cal.expr[i] == '.' || cal.expr[i] == ','))
                i--;
            if (i >= 0 && cal.expr[i] == '-' &&
                (i == 0 || is_binop((unsigned char)cal.expr[i - 1]) || cal.expr[i - 1] == '('))
                i--;
            int start = i + 1;
            if (start <= 0) expr_clear();
            else { cal.expr[start] = 0; cal.len = start; }
        }
        return;

    case K_C:
        expr_clear();
        return;

    case K_SIGN: {
        if (cal.just_eq || (cal.len > 0 && !strpbrk(cal.expr, "+-*/^"))) {
            show_value(-current_value());
            return;
        }
        int i = cal.len - 1;
        while (i >= 0 && (isdigit((unsigned char)cal.expr[i]) ||
                          cal.expr[i] == '.' || cal.expr[i] == ','))
            i--;
        if (i >= 0 && cal.expr[i] == '-' &&
            (i == 0 || is_binop((unsigned char)cal.expr[i - 1]) || cal.expr[i - 1] == '(')) {
            memmove(cal.expr + i, cal.expr + i + 1, (size_t)(cal.len - i));
            cal.len--;
        } else {
            int pos = i + 1;
            if (cal.len + 1 >= EXPR_MAX) return;
            memmove(cal.expr + pos + 1, cal.expr + pos, (size_t)(cal.len - pos + 1));
            cal.expr[pos] = '-';
            cal.len++;
        }
        return;
    }

    case '+': case '-': case '*': case '/':
        expr_append_char((char)key);
        return;

    case K_POW:
        expr_append_char('^');
        return;

    case K_EQ: {
        double v = 0;
        const char *err = NULL;
        if (!evaluate(cal.expr, &v, &err)) {
            show_error(err);
            return;
        }
        show_value(v);
        cal.just_eq = 1;
        return;
    }

    case K_SQRT: {
        double v = current_value();
        if (v < 0) { show_error("Invalid input"); return; }
        if (cal.just_eq || !strpbrk(cal.expr, "+-*/^"))
            show_value(sqrt(v));
        else
            replace_trailing_number(sqrt(v));
        cal.just_eq = 1;
        return;
    }
    case K_INV: {
        double v = current_value();
        if (v == 0) { show_error("Cannot divide by zero."); return; }
        if (cal.just_eq || !strpbrk(cal.expr, "+-*/^"))
            show_value(1.0 / v);
        else
            replace_trailing_number(1.0 / v);
        cal.just_eq = 1;
        return;
    }
    case K_PCT:
        show_value(current_value() / 100.0);
        cal.just_eq = 1;
        return;
    case K_SIN:
        show_value(sin(current_value())); cal.just_eq = 1; return;
    case K_COS:
        show_value(cos(current_value())); cal.just_eq = 1; return;
    case K_TAN:
        show_value(tan(current_value())); cal.just_eq = 1; return;
    case K_LOG: {
        double v = current_value();
        if (v <= 0) { show_error("Invalid input"); return; }
        show_value(log10(v)); cal.just_eq = 1; return;
    }
    case K_LN: {
        double v = current_value();
        if (v <= 0) { show_error("Invalid input"); return; }
        show_value(log(v)); cal.just_eq = 1; return;
    }
    case K_FACT: {
        double v = factorial(current_value());
        if (!isfinite(v)) { show_error("Invalid input"); return; }
        show_value(v); cal.just_eq = 1; return;
    }

    case K_MC:    cal.memory = 0; return;
    case K_MR:    show_value(cal.memory); cal.just_eq = 1; return;
    case K_MS:    cal.memory = current_value(); cal.just_eq = 1; return;
    case K_MPLUS: cal.memory += current_value(); cal.just_eq = 1; return;
    }
}

/* ---- layout / paint ---------------------------------------------- */

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

    W2kRect disp = { 8, MENUBAR_H + 6, w->w - 16, DISP_H };
    w2k_edge(d, disp.x, disp.y, disp.w, disp.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill(d, disp.x + 2, disp.y + 2, disp.w - 4, disp.h - 4, C_WINDOW);

    const char *show = cal.expr[0] ? cal.expr : "0";
    char clipped[EXPR_MAX];
    w2k_ellipsis(F_UI, show, disp.w - 16, clipped, sizeof clipped);
    int tw = w2k_text_width(F_UI, clipped, -1);
    w2k_text(d, F_UI, disp.x + disp.w - 8 - tw,
             disp.y + (disp.h - w2k_font_height(F_UI)) / 2, clipped,
             cal.error ? C_GRAYTEXT : C_WINDOWTEXT);

    if (cal.memory != 0) {
        W2kRect mem = { 8, disp.y + disp.h + 6, 34, 20 };
        w2k_edge(d, mem.x, mem.y, mem.w, mem.h, EDGE_SUNKEN, BF_RECT);
        w2k_text(d, F_UI, mem.x + 12, mem.y + 3, "M", C_TEXT);
    }

    for (int i = 0; i < cal.nbtn; i++) {
        const Btn *b = cal.btn[i];
        W2kRect r = cal.rect[i];
        int pressed = (cal.down == i) || (cal.glow_key && cal.glow_key == b->key);
        w2k_draw_pushbutton(d, &r, "", pressed ? BS_PRESSED : 0);
        int tw2 = w2k_text_width(F_UI, b->label, -1);
        int o = pressed ? 1 : 0;
        int tx = r.x + (r.w - tw2) / 2 + o;
        int ty = r.y + (r.h - w2k_font_height(F_UI)) / 2 + o;

        int fr, fg, fb;
        w2k_color_rgb(C_FACE, &fr, &fg, &fb);
        int dark = (fr * 299 + fg * 587 + fb * 114) / 1000 < 128;
        if (b->colour == 1)
            w2k_text_rgb(d, F_UI, tx, ty, b->label,
                         dark ? 255 : 200, dark ? 120 : 0, dark ? 120 : 0);
        else if (b->colour == 2)
            w2k_text_rgb(d, F_UI, tx, ty, b->label,
                         dark ? 120 : 0, dark ? 170 : 0, dark ? 255 : 190);
        else
            w2k_text(d, F_UI, tx, ty, b->label, C_TEXT);
    }
}

/* ---- menus / input ----------------------------------------------- */

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
    XSizeHints sh = { 0 };
    sh.flags = PMinSize | PMaxSize;
    sh.min_width = sh.max_width = w;
    sh.min_height = sh.max_height = h;
    XSetWMNormalHints(w2k.dpy, cal.win->win, &sh);
    w2k_win_resize(cal.win, w, h);
    if (cal.win->buf) { w2k_free_pixmap(cal.win->buf); cal.win->buf = 0; }
    cal.mb->r = (W2kRect){ 0, 0, w, MENUBAR_H };
    build_buttons();
}

static void command(void *u, int id)
{
    (void)u;
    switch (id) {
    case ID_COPY:  w2k_clipboard_set(cal.expr); break;
    case ID_PASTE: {
        char *t = w2k_clipboard_get();
        if (t) {
            size_t n = strlen(t);
            if (n >= EXPR_MAX) n = EXPR_MAX - 1;
            memcpy(cal.expr, t, n);
            cal.expr[n] = 0;
            cal.len = (int)n;
            cal.error = 0;
            cal.just_eq = 0;
            free(t);
        }
        break;
    }
    case ID_STANDARD:   cal.scientific = 0; resize_window(); break;
    case ID_SCIENTIFIC: cal.scientific = 1; resize_window(); break;
    case ID_ABOUT:
        w2k_msgbox(cal.win, "About Calculator",
                   "Calculator\nLinux 2000\nA Windows 2000-style desktop for X11\n\n"
                   "Expression mode: type -7+5*2^3 and press =\n"
                   "Operators: + - * / ^   (keyboard glows the key)\n\n"
                   "Linux 2000 is not affiliated with, endorsed by or sponsored by Microsoft.\n"
                   "Windows is a trademark of Microsoft Corporation.",
                   MB_OK | MB_ICONINFO);
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

        int key = 0;
        switch (ks) {
        case XK_Escape:    key = K_C; break;
        case XK_Delete:    key = K_CE; break;
        case XK_BackSpace: key = K_BACK; break;
        case XK_Return: case XK_KP_Enter: case XK_equal: case XK_KP_Equal:
            key = K_EQ; break;
        case XK_plus: case XK_KP_Add:          key = '+'; break;
        case XK_minus: case XK_KP_Subtract:    key = '-'; break;
        case XK_asterisk: case XK_KP_Multiply: key = '*'; break;
        case XK_slash: case XK_KP_Divide:      key = '/'; break;
        case XK_asciicircum:                   key = K_POW; break;
        case XK_period: case XK_KP_Decimal:    key = '.'; break;
        case XK_comma:                         key = ','; break;
        default:
            if (ks >= XK_0 && ks <= XK_9) key = (int)('0' + (ks - XK_0));
            else if (ks >= XK_KP_0 && ks <= XK_KP_9)
                key = (int)('0' + (ks - XK_KP_0));
            else if (n == 1) {
                char c = buf[0];
                if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == '+' ||
                    c == '-' || c == '*' || c == '/')
                    key = (unsigned char)c;
                else if (c == '=') key = K_EQ;
                else if (c == '%') key = K_PCT;
                else if (c == '^') key = K_POW;
                else if (c == 'r') key = K_INV;
                else if (c == '@') key = K_SQRT;
            }
            break;
        }
        if (key) {
            cal.glow_key = key;
            press(key);
            w2k_win_dirty(w);
            return 1;
        }
        return 1;
    }
    case KeyRelease:
        if (cal.glow_key) {
            cal.glow_key = 0;
            w2k_win_dirty(w);
            return 1;
        }
        break;
    }
    return 0;
}

int main(void)
{
    if (w2k_init("l2kcalc") < 0) return 1;

    memset(&cal, 0, sizeof cal);
    expr_clear();
    cal.down = -1;

    cal.win = w2k_win_new("Calculator", "l2kcalc", 260, 220, 0);
    cal.win->paint = paint;
    cal.win->event = event;

    XSelectInput(w2k.dpy, cal.win->win,
                 ExposureMask | KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask | StructureNotifyMask |
                 FocusChangeMask | EnterWindowMask | LeaveWindowMask);

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
