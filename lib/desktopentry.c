/* desktopentry.c -- reading freedesktop .desktop files.
 *
 * These are the closest thing this system has to a .lnk: the Start menu,
 * the desktop, Quick Launch and Explorer all need the name, command and
 * icon out of one, so the parsing lives here rather than in each of them.
 *
 * Only the [Desktop Entry] group is read, the first value of each key
 * wins, and the % field codes are cut off the command -- nothing here can
 * supply a file list to an application that asks for one. */
#include "w2k.h"
#include <stdio.h>
#include <string.h>

int w2k_desktop_entry(const char *path, char *name, int nn,
                      char *exec, int en, char *icon, int in)
{
    if (name && nn) name[0] = 0;
    if (exec && en) exec[0] = 0;
    if (icon && in) icon[0] = 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[1024];
    int in_entry = 0, have_name = 0, have_exec = 0, have_icon = 0, nodisplay = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') {
            /* Groups after [Desktop Entry] are actions, not the entry. */
            if (in_entry) break;
            in_entry = !strcmp(line, "[Desktop Entry]");
            continue;
        }
        if (!in_entry) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *val = eq + 1;
        if (!have_name && !strcmp(line, "Name")) {
            if (name && nn) snprintf(name, (size_t)nn, "%s", val);
            have_name = 1;
        } else if (!have_exec && !strcmp(line, "Exec")) {
            if (exec && en) snprintf(exec, (size_t)en, "%s", val);
            have_exec = 1;
        } else if (!have_icon && !strcmp(line, "Icon")) {
            if (icon && in) snprintf(icon, (size_t)in, "%s", val);
            have_icon = 1;
        } else if (!strcmp(line, "NoDisplay") || !strcmp(line, "Hidden")) {
            if (*val == 't' || *val == 'T' || *val == '1') nodisplay = 1;
        }
    }
    fclose(f);

    if (exec && en) {
        for (char *p = exec; *p; p++)
            if (p[0] == '%' && p[1]) {
                while (p > exec && p[-1] == ' ') p--;
                *p = 0;
                break;
            }
    }
    if (!have_name || !have_exec) return 0;
    return nodisplay ? -1 : 1;
}
