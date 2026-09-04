/* w2knotify -- put up a notification balloon from the command line.
 *
 *   w2knotify "Title" "The text of the notice"
 *
 * It goes to the shell through the _W2K_NOTIFY property on the root
 * window, the same way the shell's own programs send theirs, so it needs
 * neither D-Bus nor libnotify. (notify-send works too, through the
 * org.freedesktop.Notifications service the shell provides.) */
#include "w2kui.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "usage: w2knotify \"Title\" [\"Text\"]\n");
        return 2;
    }
    if (w2k_init("w2knotify") < 0) return 1;
    w2k_notify(argv[1], argc > 2 ? argv[2] : "");
    XSync(w2k.dpy, False);
    return 0;
}
