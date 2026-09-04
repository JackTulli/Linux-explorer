/* pins.c -- pinned applications, for Quick Launch and the Start menu.
 *
 * Two plain text files, one entry per line as "command<TAB>label", kept in
 * ~/.w2k. They are read on every use rather than cached: the lists are a
 * handful of lines, and rereading means an edit from another process (or by
 * hand in Notepad) shows up immediately. */
#include "wm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *pin_file(int which)
{
    return which == PIN_TASKBAR ? "pinned-taskbar" : "pinned-start";
}

static void pin_path(int which, char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.w2k/%s", home ? home : ".", pin_file(which));
}

int pins_load(int which, Pin *out, int max)
{
    char path[1024];
    pin_path(which, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int n = 0;
    char line[1024];
    while (n < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        /* "command<TAB>label<TAB>icon", the icon optional: a file written
         * by an older build has two fields and still reads. */
        char *tab = strchr(line, '\t');
        char *label = NULL, *icon = NULL;
        if (tab) {
            *tab = 0;
            label = tab + 1;
            char *tab2 = strchr(label, '\t');
            if (tab2) { *tab2 = 0; icon = tab2 + 1; }
        }
        snprintf(out[n].cmd, sizeof out[n].cmd, "%.511s", line);
        snprintf(out[n].label, sizeof out[n].label, "%.127s",
                 label && *label ? label : line);
        snprintf(out[n].icon, sizeof out[n].icon, "%.255s", icon ? icon : "");
        n++;
    }
    fclose(f);
    return n;
}

static void pins_save(int which, Pin *list, int n)
{
    char path[1024];
    pin_path(which, path, sizeof path);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }

    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\t%s\t%s\n", list[i].cmd, list[i].label, list[i].icon);
    fclose(f);
}

int pins_contains(int which, const char *cmd)
{
    Pin list[PIN_MAX];
    int n = pins_load(which, list, PIN_MAX);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].cmd, cmd)) return 1;
    return 0;
}

void pins_add(int which, const char *cmd, const char *label, const char *icon)
{
    if (!cmd || !*cmd) return;
    Pin list[PIN_MAX];
    int n = pins_load(which, list, PIN_MAX);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].cmd, cmd)) return;      /* already pinned */
    if (n >= PIN_MAX) return;

    char name[128] = "", ico[256] = "";
    if (label && *label) snprintf(name, sizeof name, "%s", label);
    if (icon && *icon)   snprintf(ico, sizeof ico, "%s", icon);
    /* Fill in whatever the caller did not know from the system's own
     * record of the application. */
    if (!name[0] || !ico[0]) {
        char dname[128], dicon[256];
        if (programs_lookup(cmd, dname, sizeof dname, dicon, sizeof dicon)) {
            if (!name[0]) snprintf(name, sizeof name, "%s", dname);
            if (!ico[0])  snprintf(ico, sizeof ico, "%s", dicon);
        }
    }
    if (!name[0]) snprintf(name, sizeof name, "%s", cmd);

    snprintf(list[n].cmd, sizeof list[n].cmd, "%s", cmd);
    snprintf(list[n].label, sizeof list[n].label, "%s", name);
    snprintf(list[n].icon, sizeof list[n].icon, "%s", ico);
    pins_save(which, list, n + 1);
}

/* The icon a pin should wear: one of the shell's own by slug, a file, or
 * a freedesktop icon name -- and failing all of those, a guess from the
 * command, so a pin is never a blank page. */
static int pin_icon_resolve(const Pin *p);

int pin_icon(const Pin *p)
{
    if (!p) return ICO_APP;

    /* Resolving can mean a walk of the .desktop database, and the taskbar
     * asks on every layout, so the answer is remembered per pin. The key
     * is both fields: changing the icon must take effect at once. */
    static struct { char key[800]; int id; } cache[PIN_MAX * 2];
    static int ncache;
    char key[800];
    snprintf(key, sizeof key, "%.511s\t%.255s", p->cmd, p->icon);
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].key, key)) return cache[i].id;

    int id = pin_icon_resolve(p);
    if (ncache < (int)(sizeof cache / sizeof *cache)) {
        snprintf(cache[ncache].key, sizeof cache[ncache].key, "%s", key);
        cache[ncache].id = id;
        ncache++;
    }
    return id;
}

static int pin_icon_resolve(const Pin *p)
{
    if (!strncmp(p->icon, "w2k:", 4)) {
        int id = w2k_icon_by_slug(p->icon + 4);
        if (id >= 0) return id;
    }
    if (p->icon[0]) {
        int id = w2k_icon_by_name(p->icon);
        if (id != ICO_APP) return id;
    }
    /* Nothing recorded: ask the system, then fall back to the shell's own
     * programs by name. */
    char dicon[256];
    if (programs_lookup(p->cmd, NULL, 0, dicon, sizeof dicon) && dicon[0]) {
        int id = w2k_icon_by_name(dicon);
        if (id != ICO_APP) return id;
    }
    if (strstr(p->cmd, "w2kexplorer")) return ICO_EXPLORER;
    if (strstr(p->cmd, "w2knotepad"))  return ICO_NOTEPAD;
    if (strstr(p->cmd, "w2ktaskmgr"))  return ICO_TASKMGR;
    if (strstr(p->cmd, "w2ksnip"))    return ICO_SNIP;
    if (strstr(p->cmd, "w2kcalc"))     return ICO_CALC;
    if (strstr(p->cmd, "w2kcharmap"))  return ICO_CHARMAP;
    if (strstr(p->cmd, "w2kdevmgmt"))  return ICO_MYCOMPUTER;
    if (strstr(p->cmd, "w2kcontrol"))  return ICO_CONTROLPANEL;
    if (strstr(p->cmd, "w2kdisplay"))  return ICO_SETTINGS;
    if (strstr(p->cmd, "w2kimage"))    return ICO_PAINT;
    if (strstr(p->cmd, "term"))        return ICO_TERMINAL;
    return ICO_APP;
}

/* Edit a pin that is already in the list. */
static void pins_edit(int which, const char *cmd, const char *label,
                      const char *icon)
{
    Pin list[PIN_MAX];
    int n = pins_load(which, list, PIN_MAX), touched = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].cmd, cmd)) continue;
        if (label) snprintf(list[i].label, sizeof list[i].label, "%s", label);
        if (icon)  snprintf(list[i].icon, sizeof list[i].icon, "%s", icon);
        touched = 1;
        break;
    }
    if (touched) pins_save(which, list, n);
}

void pins_rename(int which, const char *cmd, const char *label)
{
    pins_edit(which, cmd, label, NULL);
}

void pins_set_icon(int which, const char *cmd, const char *icon)
{
    pins_edit(which, cmd, NULL, icon);
}

void pins_remove(int which, const char *cmd)
{
    Pin list[PIN_MAX];
    int n = pins_load(which, list, PIN_MAX), out = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].cmd, cmd)) list[out++] = list[i];
    if (out != n) pins_save(which, list, out);
}

/* What command would start this window again?
 *
 * _NET_WM_PID plus /proc gives the real command line, which is what the user
 * actually launched -- WM_CLASS is only a fallback for clients that do not
 * publish a pid (and for those, the instance name is usually the binary). */
void pin_command_for_client(Client *c, char *cmd, int cn, char *label, int ln,
                            char *icon, int in)
{
    cmd[0] = 0;
    if (icon && in) icon[0] = 0;

    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(w2k.dpy, c->win, w2k.a_net_wm_pid, 0, 1, False,
                           XA_CARDINAL, &type, &fmt, &n, &after,
                           &data) == Success && data) {
        if (fmt == 32 && n >= 1) {
            long pid = *(long *)data;
            char path[64], buf[1024];
            snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
            FILE *f = fopen(path, "r");
            if (f) {
                size_t got = fread(buf, 1, sizeof buf - 1, f);
                fclose(f);
                if (got) {
                    buf[got] = 0;
                    /* argv is NUL-separated; join the parts with spaces. */
                    for (size_t i = 0; i + 1 < got; i++)
                        if (!buf[i]) buf[i] = ' ';
                    snprintf(cmd, cn, "%s", buf);
                }
            }
        }
        XFree(data);
    }
    if (!cmd[0] && c->cls) snprintf(cmd, cn, "%s", c->cls);
    if (!cmd[0]) snprintf(cmd, cn, "%s", c->name ? c->name : "");

    /* The name and icon come from the system's record of the application
     * where there is one: WM_CLASS's class ("Firefox") is a fair name but
     * its instance ("Navigator") is not, and neither is an icon. */
    label[0] = 0;
    if (programs_lookup(cmd, label, ln, icon, in) ||
        (c->cls_name &&
         programs_lookup(c->cls_name, label, ln, icon, in)) ||
        (c->cls && programs_lookup(c->cls, label, ln, icon, in)))
        return;

    const char *base = c->cls_name && *c->cls_name ? c->cls_name :
                       c->cls && *c->cls ? c->cls : c->name;
    snprintf(label, ln, "%s", base ? base : cmd);
    if (label[0] >= 'a' && label[0] <= 'z') label[0] -= 32;
}
