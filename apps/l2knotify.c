/* l2knotify -- put up a notification balloon from the command line.
 *
 *   l2knotify "Title" "The text of the notice"
 *
 * It goes to the shell through the _W2K_NOTIFY property on the root
 * window, the same way the shell's own programs send theirs, so it needs
 * neither D-Bus nor libnotify. (notify-send works too, through the
 * org.freedesktop.Notifications service the shell provides.) */
#include "w2kui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* As the toolkit's own handler: an error is told, not fatal. */
static int report_xerror(Display *d, XErrorEvent *e)
{
    if (e->error_code == BadWindow || e->error_code == BadDrawable ||
        e->error_code == BadMatch  || e->error_code == BadPixmap)
        return 0;
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "w2k: X error: %s (request %d.%d)\n", buf,
            e->request_code, e->minor_code);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "usage: l2knotify \"Title\" [\"Text\"]\n");
        return 2;
    }
    /* Only the display and two atoms: w2k_init would load the scheme,
     * the fonts, the cursors and the icons for a program that draws
     * nothing, most of the time it took to put a balloon up. */
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "l2knotify: cannot open display \"%s\"\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    XSetErrorHandler(report_xerror);
    char *names[2] = { "_W2K_NOTIFY", "_W2K_COMMAND" };
    Atom got[2];
    if (!XInternAtoms(d, names, 2, False, got)) return 1;
    w2k.dpy = d;
    w2k.root = DefaultRootWindow(d);
    w2k.a_w2k_notify = got[0];
    w2k.a_w2k_command = got[1];
    w2k_notify(argv[1], argc > 2 ? argv[2] : "");
    XSync(d, False);
    return 0;
}
