/* programs.c -- the Programs menu, built from the system's .desktop files.
 *
 * Applications are grouped into Windows-style program groups by their
 * freedesktop category, with Flatpak applications in a group of their own.
 * The menu is rebuilt on every open, so newly installed software shows up
 * without restarting anything. */
#include "wm.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PROG_BASE 1000              /* command ids: PROG_BASE + index */
#define MAXAPPS   1024

typedef struct {
    char *id;                        /* desktop file name, for de-duplication */
    char *name;
    char *exec;
    char *icon;                      /* Icon= from the .desktop file */
    int   icon_id;                   /* resolved lazily, 0 = not yet */
    int   terminal;
    int   flatpak;
    int   group;
} App;

/* The icon for an app, looked up in the icon themes the first time it is
 * asked for -- scanning for every application up front would cost hundreds
 * of stat() calls before the menu even opens. */
static int app_icon(App *a)
{
    if (!a->icon_id) a->icon_id = w2k_icon_by_name(a->icon);
    return a->icon_id;
}

static App  apps[MAXAPPS];
static int  napps;

/* Program groups, in Start-menu order. `match` lists freedesktop main
 * categories that land in each group. */
static const struct { const char *name; const char *match; } groups[] = {
    { "Accessories",   "Utility;TextEditor;Archiving;Calculator;Clock;FileTools;" },
    { "Games",         "Game;" },
    { "Graphics",      "Graphics;2DGraphics;RasterGraphics;VectorGraphics;Photography;" },
    { "Internet",      "Network;WebBrowser;Email;Chat;InstantMessaging;FileTransfer;P2P;" },
    { "Multimedia",    "AudioVideo;Audio;Video;Player;Recorder;Music;" },
    { "Office",        "Office;WordProcessor;Spreadsheet;Presentation;Calendar;" },
    { "Development",   "Development;IDE;Debugger;" },
    { "Education",     "Education;Science;" },
    { "System Tools",  "System;Monitor;Filesystem;TerminalEmulator;PackageManager;" },
    { "Settings",      "Settings;DesktopSettings;HardwareSettings;" },
    { "Other",         "" },
};
#define NGROUPS ((int)(sizeof groups / sizeof *groups))
#define G_OTHER (NGROUPS - 1)

static int group_for(const char *categories)
{
    if (!categories) return G_OTHER;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", categories);
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        char key[128];
        snprintf(key, sizeof key, "%s;", tok);
        for (int g = 0; g < G_OTHER; g++)
            if (strstr(groups[g].match, key)) return g;
    }
    return G_OTHER;
}

/* Strip the field codes (%f, %U, ...) and quotes a shell will not want. */
static char *clean_exec(const char *exec)
{
    char *out = w2k_alloc(strlen(exec) + 1);
    char *o = out;
    for (const char *p = exec; *p; p++) {
        if (*p == '%' && p[1]) {
            if (p[1] == '%') { *o++ = '%'; }
            p++;
            continue;
        }
        *o++ = *p;
    }
    *o = 0;
    while (o > out && isspace((unsigned char)o[-1])) *--o = 0;
    return out;
}

static void add_app(const char *path, const char *id, int flatpak)
{
    if (napps >= MAXAPPS) return;
    for (int i = 0; i < napps; i++)
        if (!strcmp(apps[i].id, id)) return;      /* user copy shadows system */

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    char *name = NULL, *exec = NULL, *cats = NULL, *icon = NULL;
    int nodisplay = 0, terminal = 0, in_entry = 0, is_app = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (line[0] == '[') { in_entry = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in_entry) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *val = eq + 1;
        if (!strcmp(line, "Name") && !name)           name = w2k_strdup(val);
        else if (!strcmp(line, "Exec") && !exec)      exec = w2k_strdup(val);
        else if (!strcmp(line, "Categories"))         { free(cats); cats = w2k_strdup(val); }
        else if (!strcmp(line, "Icon") && !icon)      icon = w2k_strdup(val);
        else if (!strcmp(line, "NoDisplay"))          nodisplay |= !strcasecmp(val, "true");
        else if (!strcmp(line, "Hidden"))             nodisplay |= !strcasecmp(val, "true");
        else if (!strcmp(line, "Terminal"))           terminal = !strcasecmp(val, "true");
        else if (!strcmp(line, "Type"))               is_app = !strcmp(val, "Application");
    }
    fclose(f);

    if (!nodisplay && is_app && name && exec) {
        App *a = &apps[napps++];
        a->id = w2k_strdup(id);
        a->name = name;
        a->exec = clean_exec(exec);
        a->icon = icon;
        a->icon_id = 0;
        a->terminal = terminal;
        icon = NULL;
        a->flatpak = flatpak || strstr(exec, "flatpak run") != NULL;
        a->group = group_for(cats);
        name = NULL;
    }
    free(name);
    free(exec);
    free(cats);
    free(icon);
}

static void scan_dir(const char *dir, int flatpak)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        size_t n = strlen(de->d_name);
        if (n < 9 || strcmp(de->d_name + n - 8, ".desktop")) continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        add_app(path, de->d_name, flatpak);
    }
    closedir(dp);
}

static void scan_all(void)
{
    for (int i = 0; i < napps; i++) {
        free(apps[i].id);
        free(apps[i].name);
        free(apps[i].exec);
        free(apps[i].icon);
    }
    napps = 0;

    const char *home = getenv("HOME");
    char path[2048];

    /* Per-user entries first so they shadow system ones of the same id. */
    const char *xdh = getenv("XDG_DATA_HOME");
    if (xdh && *xdh) {
        snprintf(path, sizeof path, "%s/applications", xdh);
        scan_dir(path, 0);
    } else if (home) {
        snprintf(path, sizeof path, "%s/.local/share/applications", home);
        scan_dir(path, 0);
    }
    if (home) {
        snprintf(path, sizeof path,
                 "%s/.local/share/flatpak/exports/share/applications", home);
        scan_dir(path, 1);
    }
    scan_dir("/var/lib/flatpak/exports/share/applications", 1);

    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    char copy[2048];
    snprintf(copy, sizeof copy, "%s", dirs);
    for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":")) {
        snprintf(path, sizeof path, "%s/applications", tok);
        scan_dir(path, strstr(tok, "flatpak") != NULL);
    }
}

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp(((const App *)a)->name, ((const App *)b)->name);
}

/* ------------------------------------------------------------------ *
 * Personalized Menus
 * ------------------------------------------------------------------ *
 * Windows 2000's signature Start-menu behaviour: programs you have not
 * used fold away, and a chevron at the foot of the group opens the rest.
 * Usage is counted in ~/.w2k/usage, one "count<TAB>name" line per program,
 * bumped whenever something is launched from the menu.
 *
 * A group with only a couple of hidden items is left alone -- folding two
 * things away to save two lines is just annoying. */
#define PERSONAL_MIN_HIDDEN 3
#define CHEVRON_ID 1900

static struct { char name[128]; int count; } usage[512];
static int nusage;

static void usage_path(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/.w2k/usage", home ? home : ".");
}

static void usage_load(void)
{
    static int done;
    if (done) return;
    done = 1;

    char path[1024];
    usage_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (nusage < 512 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        usage[nusage].count = atoi(line);
        snprintf(usage[nusage].name, sizeof usage[nusage].name, "%.127s", tab + 1);
        nusage++;
    }
    fclose(f);
}

static int usage_count(const char *name)
{
    usage_load();
    for (int i = 0; i < nusage; i++)
        if (!strcmp(usage[i].name, name)) return usage[i].count;
    return 0;
}

/* Called when something is launched: remember that it was used. */
void programs_note_use(const char *name)
{
    if (!name || !*name) return;
    usage_load();

    int found = -1;
    for (int i = 0; i < nusage; i++)
        if (!strcmp(usage[i].name, name)) { found = i; break; }
    if (found < 0) {
        if (nusage >= 512) return;
        found = nusage++;
        snprintf(usage[found].name, sizeof usage[found].name, "%.127s", name);
        usage[found].count = 0;
    }
    usage[found].count++;

    char path[1024], dir[1024];
    usage_path(path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < nusage; i++)
        fprintf(f, "%d\t%s\n", usage[i].count, usage[i].name);
    fclose(f);
}

/* Set while a group is showing everything, after the chevron was clicked. */
static int expand_group = -1;

void programs_expand(int group) { expand_group = group; }
void programs_collapse_all(void) { expand_group = -1; }


/* Build one group's submenu; NULL when the group is empty. */
static W2kMenu *group_menu(int group, int flatpak)
{
    W2kMenu *m = NULL;
    int shown = 0, hidden = 0;
    int expand = !w2k_start_personalized || expand_group == group;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < napps; i++) {
            if (apps[i].flatpak != flatpak) continue;
            if (!flatpak && apps[i].group != group) continue;
            int used = usage_count(apps[i].name) > 0;
            /* First pass: what has been used. Second: the rest, and only
             * when the group is expanded. */
            if (pass == 0 && !used) { hidden++; continue; }
            if (pass == 1 && (used || !expand)) continue;
            if (!m) m = w2k_menu_new();
            w2k_menu_item(m, PROG_BASE + i, apps[i].name, NULL,
                          app_icon(&apps[i]));
            shown++;
        }
    }
    /* Nothing used yet: show the lot rather than an empty group. */
    if (m && !shown) return m;
    if (!m && hidden) {
        m = w2k_menu_new();
        for (int i = 0; i < napps; i++) {
            if (apps[i].flatpak != flatpak) continue;
            if (!flatpak && apps[i].group != group) continue;
            w2k_menu_item(m, PROG_BASE + i, apps[i].name, NULL,
                          app_icon(&apps[i]));
        }
        return m;
    }
    if (m && !expand && hidden >= PERSONAL_MIN_HIDDEN) {
        w2k_menu_sep(m);
        w2k_menu_item(m, CHEVRON_ID + group, "\xc2\xbb", NULL, ICO_NONE);
    }
    return m;
}

/* The basename of the program a command runs: "/usr/bin/firefox -P x"
 * gives "firefox". Pins are matched on this. */
static void command_binary(const char *cmd, char *out, int n)
{
    out[0] = 0;
    if (!cmd) return;
    while (*cmd == ' ') cmd++;
    /* Skip a leading environment assignment or wrapper, which .desktop
     * files use often enough to matter: "env FOO=1 firefox". */
    if (!strncmp(cmd, "env ", 4)) {
        cmd += 4;
        while (*cmd == ' ') cmd++;
        while (*cmd && strchr(cmd, '=') && *cmd != ' ') {
            const char *sp = strchr(cmd, ' ');
            const char *eq = strchr(cmd, '=');
            if (!sp || !eq || eq > sp) break;
            cmd = sp;
            while (*cmd == ' ') cmd++;
        }
    }
    const char *end = cmd;
    while (*end && *end != ' ') end++;
    const char *base = end;
    while (base > cmd && base[-1] != '/') base--;
    int len = (int)(end - base);
    if (len >= n) len = n - 1;
    memcpy(out, base, (size_t)len);
    out[len] = 0;
}

/* What the system says an application is called and which icon it wears,
 * looked up by the command that starts it (or by its WM_CLASS, which is
 * usually the same word). This is how a pin gets "Firefox" and the
 * Firefox icon instead of "Navigator" and a blank page. */
int programs_lookup(const char *key, char *name, int nn, char *icon, int in)
{
    if (!key || !*key) return 0;
    scan_all();

    char want[128];
    command_binary(key, want, sizeof want);
    if (!want[0]) return 0;

    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < napps; i++) {
            char have[128];
            command_binary(apps[i].exec, have, sizeof have);
            int hit = pass == 0 ? !strcasecmp(have, want)
                                : !strcasecmp(apps[i].name, want);
            if (!hit) continue;
            if (name && nn) snprintf(name, (size_t)nn, "%s", apps[i].name);
            if (icon && in) snprintf(icon, (size_t)in, "%s",
                                     apps[i].icon ? apps[i].icon : "");
            return 1;
        }
    return 0;
}

/* Was this id the chevron at the foot of a group? If so, remember which
 * group to expand and say so, and the menu is rebuilt showing everything. */
int programs_is_chevron(int id, int *group)
{
    if (id < CHEVRON_ID || id >= CHEVRON_ID + 64) return 0;
    if (group) *group = id - CHEVRON_ID;
    return 1;
}

/* Matches for a search box: substring of the program name, case-insensitive,
 * with names that start with the query first. Returns how many were filled
 * in; ids are the same command ids the Programs menu uses. */
/* Icon id for one of the ids programs_search() hands back. */
int programs_icon(int id)
{
    int i = id - PROG_BASE;
    if (i < 0 || i >= napps) return ICO_APP;
    return app_icon(&apps[i]);
}

int programs_search(const char *query, int *ids, const char **names, int max)
{
    if (!napps) scan_all();
    if (!query || !*query) return 0;

    char q[128];
    int qn = 0;
    for (const char *p = query; *p && qn < (int)sizeof q - 1; p++)
        q[qn++] = (char)tolower((unsigned char)*p);
    q[qn] = 0;

    int n = 0;
    for (int pass = 0; pass < 2 && n < max; pass++)
        for (int i = 0; i < napps && n < max; i++) {
            char low[256];
            int k = 0;
            for (const char *p = apps[i].name; *p && k < (int)sizeof low - 1; p++)
                low[k++] = (char)tolower((unsigned char)*p);
            low[k] = 0;
            int starts = !strncmp(low, q, (size_t)qn);
            if (pass == 0 ? !starts : (starts || !strstr(low, q))) continue;
            ids[n] = PROG_BASE + i;
            names[n] = apps[i].name;
            n++;
        }
    return n;
}

/* Adds the program groups to `m` -- Accessories, Games, Internet and so on,
 * each a submenu. They belong directly under Programs: burying them one
 * level further down under "Installed Programs" is a level nobody needs. */
void programs_add_groups(W2kMenu *m)
{
    scan_all();
    qsort(apps, napps, sizeof *apps, cmp_name);

    int any = 0;
    for (int g = 0; g < NGROUPS; g++) {
        W2kMenu *sub = group_menu(g, 0);
        if (!sub) continue;
        w2k_menu_sub(m, groups[g].name, ICO_PROGRAMS, sub);
        any = 1;
    }
    W2kMenu *fp = group_menu(0, 1);
    if (fp) {
        if (any) w2k_menu_sep(m);
        w2k_menu_sub(m, "Flatpak", ICO_PROGRAMS, fp);
        any = 1;
    }
    if (!any) {
        w2k_menu_item(m, 0, "(No programs found)", NULL, ICO_NONE);
        w2k_menu_disable(m);
    }
}

/* The pieces behind a Programs menu id, for pinning it. */
int programs_entry(int id, char *cmd, int cn, char *name, int nn,
                   char *icon, int in)
{
    int i = id - PROG_BASE;
    if (i < 0 || i >= napps) return 0;
    if (cmd && cn)  snprintf(cmd, (size_t)cn, "%s", apps[i].exec);
    if (name && nn) snprintf(name, (size_t)nn, "%s", apps[i].name);
    if (icon && in) snprintf(icon, (size_t)in, "%s",
                             apps[i].icon ? apps[i].icon : "");
    return 1;
}

/* Called with the id the Start menu got back; returns 1 if it was ours. */
int programs_run(int id, const char *terminal)
{
    int i = id - PROG_BASE;
    if (i < 0 || i >= napps) return 0;
    programs_note_use(apps[i].name);      /* personalized menus count this */
    char cmd[4096];
    if (apps[i].terminal && terminal)
        snprintf(cmd, sizeof cmd, "%s -e %s", terminal, apps[i].exec);
    else
        snprintf(cmd, sizeof cmd, "%s", apps[i].exec);
    wm_spawn(cmd);
    return 1;
}
