/* l2kswatch -- scratch harness for eyeballing primitives and icon art. */
#include "w2k.h"
#include <stdio.h>

static const char *names[] = {
 "app","folder","fold.open","file","file.txt","mycomp","hdd","floppy","cd",
 "notepad","explorer","taskmgr","mydocs","recycle","network","programs",
 "documents","settings","search","help","run","shutdwn","logoff","up","back",
 "fwd","cut","copy","paste","delete","props","views","cpanel","term","calc",
 "paint","info","warn","quest","error","access","flag","desktop","winupd" };

static void draw(Window win, int W, int H)
{
    Pixmap pm = XCreatePixmap(w2k.dpy, win, W, H, w2k.depth);
    w2k_fill(pm, 0, 0, W, H, C_FACE);

    w2k_text(pm, F_UI_BOLD, 8, 6, "16x16", C_TEXT);
    for (int i = 0; i < N_ICONS; i++) {
        int col = i % 14, row = i / 14;
        int x = 8 + col * 52, y = 22 + row * 34;
        w2k_icon_draw(pm, x, y, i);
        w2k_text(pm, F_UI, x, y + 17, names[i], C_TEXT);
    }
    int y0 = 22 + 3 * 34 + 8;
    w2k_text(pm, F_UI_BOLD, 8, y0, "32x32", C_TEXT);
    int big[] = { ICO_MYCOMPUTER, ICO_MYDOCS, ICO_DOCUMENTS, ICO_PROGRAMS,
                  ICO_SETTINGS, ICO_SEARCH, ICO_HELP, ICO_RUN, ICO_SHUTDOWN,
                  ICO_LOGOFF, ICO_RECYCLE, ICO_NETWORK, ICO_EXPLORER,
                  ICO_NOTEPAD, ICO_TASKMGR, ICO_TERMINAL, ICO_CONTROLPANEL,
                  ICO_FILE, ICO_FOLDER, ICO_CALC };
    for (int i = 0; i < (int)(sizeof big / sizeof *big); i++) {
        int x = 8 + (i % 10) * 62, y = y0 + 16 + (i / 10) * 50;
        w2k_bigicon_draw(pm, x, y, big[i]);
    }
    XCopyArea(w2k.dpy, pm, win, w2k.gc, 0, 0, W, H, 0, 0);
    w2k_free_pixmap(pm);
}

int main(void)
{
    if (w2k_init("l2kswatch") < 0) return 1;
    int W = 740, H = 340;
    Window win = XCreateSimpleWindow(w2k.dpy, w2k.root, 20, 20, W, H, 0, 0,
                                     w2k.col[C_FACE]);
    XSelectInput(w2k.dpy, win, ExposureMask | KeyPressMask);
    w2k_set_wm_name(win, "w2k swatch");
    XMapWindow(w2k.dpy, win);
    for (;;) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        if (e.type == Expose && e.xexpose.count == 0) draw(win, W, H);
        else if (e.type == KeyPress) break;
    }
    w2k_fini();
    return 0;
}
