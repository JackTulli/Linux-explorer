/* xdo -- minimal XTest driver used to exercise the desktop during
 * development. Not part of the installed desktop.
 *   xdo :9 move X Y | click X Y [button] | key KEYSYM | type TEXT | sleep MS
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Display *d;

static void nap(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void press_keysym(KeySym ks, int shift)
{
    KeyCode kc = XKeysymToKeycode(d, ks);
    if (!kc) { fprintf(stderr, "xdo: no keycode for %ld\n", (long)ks); return; }
    KeyCode sh = XKeysymToKeycode(d, XK_Shift_L);
    if (shift) XTestFakeKeyEvent(d, sh, True, 0);
    XTestFakeKeyEvent(d, kc, True, 0);
    XTestFakeKeyEvent(d, kc, False, 0);
    if (shift) XTestFakeKeyEvent(d, sh, False, 0);
    XFlush(d);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: xdo DISPLAY cmd ...\n"); return 2; }
    d = XOpenDisplay(argv[1]);
    if (!d) { fprintf(stderr, "xdo: cannot open %s\n", argv[1]); return 1; }

    for (int i = 2; i < argc; ) {
        const char *cmd = argv[i++];
        if (!strcmp(cmd, "move") && i + 1 < argc) {
            XTestFakeMotionEvent(d, -1, atoi(argv[i]), atoi(argv[i + 1]), 0);
            i += 2;
        } else if (!strcmp(cmd, "click") && i + 1 < argc) {
            int b = (i + 2 < argc && argv[i + 2][0] >= '1' && argv[i + 2][0] <= '5' &&
                     !argv[i + 2][1]) ? atoi(argv[i + 2]) : 1;
            XTestFakeMotionEvent(d, -1, atoi(argv[i]), atoi(argv[i + 1]), 0);
            XFlush(d);
            nap(60);
            XTestFakeButtonEvent(d, b, True, 0);
            XFlush(d);
            nap(50);
            XTestFakeButtonEvent(d, b, False, 0);
            i += (b != 1 || (i + 2 < argc && !argv[i + 2][1] &&
                             argv[i + 2][0] >= '1' && argv[i + 2][0] <= '5')) ? 3 : 2;
        } else if (!strcmp(cmd, "drag") && i + 3 < argc) {
            int x0 = atoi(argv[i]), y0 = atoi(argv[i+1]);
            int x1 = atoi(argv[i+2]), y1 = atoi(argv[i+3]);
            XTestFakeMotionEvent(d, -1, x0, y0, 0); XFlush(d); nap(60);
            XTestFakeButtonEvent(d, 1, True, 0); XFlush(d); nap(60);
            for (int s = 1; s <= 10; s++) {
                XTestFakeMotionEvent(d, -1, x0 + (x1 - x0) * s / 10,
                                     y0 + (y1 - y0) * s / 10, 0);
                XFlush(d);
                nap(20);
            }
            XTestFakeButtonEvent(d, 1, False, 0);
            i += 4;
        } else if (!strcmp(cmd, "key") && i < argc) {
            KeySym ks = XStringToKeysym(argv[i]);
            if (ks == NoSymbol) { fprintf(stderr, "xdo: bad keysym %s\n", argv[i]); return 1; }
            press_keysym(ks, 0);
            i++;
        } else if (!strcmp(cmd, "type") && i < argc) {
            /* ASCII text, one key at a time; a shifted key where needed. */
            for (const char *c = argv[i]; *c; c++) {
                char name[2] = { *c, 0 };
                KeySym ks = XStringToKeysym(name);
                if (*c == ' ') ks = XK_space;
                else if (*c == '/') ks = XK_slash;
                else if (*c == '.') ks = XK_period;
                else if (*c == '-') ks = XK_minus;
                else if (*c == '_') ks = XK_underscore;
                else if (*c == ':') ks = XK_colon;
                if (ks == NoSymbol) continue;
                int shift = (*c >= 'A' && *c <= 'Z') || *c == '_' || *c == ':';
                press_keysym(ks, shift);
                nap(15);
            }
            i++;
        } else if (!strcmp(cmd, "sleep") && i < argc) {
            nap(atoi(argv[i]));
            i++;
        } else if (!strcmp(cmd, "keydown") && i < argc) {
            XTestFakeKeyEvent(d, XKeysymToKeycode(d, XStringToKeysym(argv[i])), True, 0);
            i++;
        } else if (!strcmp(cmd, "keyup") && i < argc) {
            XTestFakeKeyEvent(d, XKeysymToKeycode(d, XStringToKeysym(argv[i])), False, 0);
            i++;
        } else if (!strcmp(cmd, "type") && i < argc) {
            for (const char *p = argv[i]; *p; p++) {
                char buf[2] = { *p, 0 };
                KeySym ks = XStringToKeysym(buf);
                int shift = 0;
                if (*p == ' ') ks = XK_space;
                else if (*p >= 'A' && *p <= 'Z') { shift = 1; }
                else if (ks == NoSymbol) {
                    static const struct { char c; const char *n; int sh; } sp[] = {
                        {'/', "slash", 0}, {'.', "period", 0}, {'-', "minus", 0},
                        {'_', "underscore", 1}, {':', "colon", 1}, {',', "comma", 0},
                        {'!', "exclam", 1}, {'?', "question", 1}, {'*', "asterisk", 1},
                        {'\n', "Return", 0}, {'=', "equal", 0}, {'+', "plus", 1},
                        {'(', "parenleft", 1}, {')', "parenright", 1}, {0, 0, 0}
                    };
                    for (int k = 0; sp[k].n; k++)
                        if (sp[k].c == *p) { ks = XStringToKeysym(sp[k].n); shift = sp[k].sh; }
                }
                if (ks == NoSymbol) continue;
                press_keysym(ks, shift);
                nap(12);
            }
            i++;
        } else if (!strcmp(cmd, "sleep") && i < argc) {
            XFlush(d);
            nap(atoi(argv[i]));
            i++;
        } else {
            fprintf(stderr, "xdo: unknown command %s\n", cmd);
            return 2;
        }
        XFlush(d);
        nap(40);
    }
    XSync(d, False);
    XCloseDisplay(d);
    return 0;
}
