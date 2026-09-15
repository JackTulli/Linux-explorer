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

/* A string value as the file writes it, unescaped in place: the spec's
 * \s \n \t \r and \\. Nothing did this, so an Exec line written by
 * winemenubuilder -- C:\\\\ProgramData\\\\...\\\\Start\\ Menu -- reached
 * the shell doubled up and split at "Start Menu", and no Windows program
 * in the Start menu ever launched. */
void w2k_desktop_unescape(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 's': *o++ = ' '; break;
            case 'n': *o++ = '\n'; break;
            case 't': *o++ = '\t'; break;
            case 'r': *o++ = '\r'; break;
            case '\\': *o++ = '\\'; break;
            default: *o++ = '\\'; *o++ = *p; break;   /* the Exec quoting's own */
            }
            continue;
        }
        *o++ = *p;
    }
    *o = 0;
}

/* An Exec value made into a command for sh -c: escapes undone, the field
 * codes (%f %U %i ...) taken out where they stand -- an argument that was
 * only "%f" goes whole, quotes and all -- and %% made %. The rest of the
 * line stays: "foo %U --new-window" is "foo --new-window", where it used
 * to be cut short at the first %. */
void w2k_desktop_exec_command(const char *exec, char *out, size_t n)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", exec);
    w2k_desktop_unescape(tmp);
    size_t o = 0;
    for (const char *p = tmp; *p && o + 1 < n; p++) {
        if (*p == '%' && p[1]) {
            if (p[1] == '%') { out[o++] = '%'; p++; continue; }
            /* A quoted field code on its own, "%f" or '%f', goes whole. */
            if (o > 0 && (out[o - 1] == '"' || out[o - 1] == '\'') &&
                p[2] == out[o - 1] && (o == 1 || out[o - 2] == ' ') &&
                (p[3] == 0 || p[3] == ' ')) {
                o--;
                p += 2;
                continue;
            }
            p++;
            continue;
        }
        out[o++] = *p;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* A value to write into a .desktop file: the escapes the reader undoes,
 * and for an Exec line % doubled. A file name with a newline in it can
 * then not start a line of its own -- an Exec= of its choosing. */
void w2k_desktop_escape(const char *in, char *out, size_t n, int exec)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 3 < n; p++) {
        switch (*p) {
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
        case '\t': out[o++] = '\\'; out[o++] = 't'; break;
        case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
        case '%':  out[o++] = '%'; if (exec) out[o++] = '%'; break;
        default:
            if ((unsigned char)*p < 0x20) { out[o++] = ' '; break; }
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

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
            if (name && nn) { snprintf(name, (size_t)nn, "%s", val); w2k_desktop_unescape(name); }
            have_name = 1;
        } else if (!have_exec && !strcmp(line, "Exec")) {
            if (exec && en) w2k_desktop_exec_command(val, exec, (size_t)en);
            have_exec = 1;
        } else if (!have_icon && !strcmp(line, "Icon")) {
            if (icon && in) { snprintf(icon, (size_t)in, "%s", val); w2k_desktop_unescape(icon); }
            have_icon = 1;
        } else if (!strcmp(line, "NoDisplay") || !strcmp(line, "Hidden")) {
            if (*val == 't' || *val == 'T' || *val == '1') nodisplay = 1;
        }
    }
    fclose(f);

    if (!have_name || !have_exec) return 0;
    return nodisplay ? -1 : 1;
}
