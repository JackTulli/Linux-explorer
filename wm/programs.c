/* programs.c -- the Programs menu, built from the system's .desktop files.
 *
 * Applications are grouped into Windows-style program groups by their
 * freedesktop category, with Flatpak applications in a group of their own
 * and Windows programs -- what Wine installed, from the .desktop entries
 * its menu builder writes under applications/wine -- in another.
 * The menu is rebuilt on every open, so newly installed software shows up
 * without restarting anything. */
#include "wm.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Clear of every other Start menu range (chevrons 1900, Start folder
 * 2000, recent documents 2500): 1000 met them past the 900th program. */
#define PROG_BASE 10000             /* command ids: PROG_BASE + index */
#define MAXAPPS   1024

typedef struct {
    char *id;                        /* desktop file name, for de-duplication */
    char *name;
    char *exec;
    char *icon;                      /* Icon= from the .desktop file */
    int   icon_id;                   /* resolved lazily, 0 = not yet */
    int   terminal;
    int   flatpak;
    int   wine;                      /* from Wine's own menu tree */
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
    char buf[512], *sp = NULL;
    snprintf(buf, sizeof buf, "%s", categories);
    /* strtok_r: the caller may be in the middle of tokenising a list of
     * directories with strtok, and a plain strtok here would hijack it. */
    for (char *tok = strtok_r(buf, ";", &sp); tok; tok = strtok_r(NULL, ";", &sp)) {
        char key[128];
        snprintf(key, sizeof key, "%s;", tok);
        for (int g = 0; g < G_OTHER; g++)
            if (strstr(groups[g].match, key)) return g;
    }
    return G_OTHER;
}

/* The command for sh: the file's escapes undone and its field codes
 * (%f, %U, ...) taken out (lib/desktopentry.c). */
static char *clean_exec(const char *exec)
{
    char out[4096];
    w2k_desktop_exec_command(exec, out, sizeof out);
    return w2k_strdup(out);
}

static int adding_wine;              /* set while scanning Wine's tree */

/* Every id seen in this scan, listed or not: a user's copy marked
 * NoDisplay or Hidden -- how menu editors hide an application -- shadows
 * the system's copy of the same id, which used to be listed anyway
 * because only listed entries were remembered. */
static char **seen_ids;
static int nseen, capseen;

static int seen(const char *id)
{
    for (int i = 0; i < nseen; i++) if (!strcmp(seen_ids[i], id)) return 1;
    if (nseen == capseen) {
        int cap = capseen ? capseen * 2 : 256;
        char **g = realloc(seen_ids, (size_t)cap * sizeof *g);
        if (!g) return 0;
        seen_ids = g;
        capseen = cap;
    }
    seen_ids[nseen] = strdup(id);
    if (seen_ids[nseen]) nseen++;
    return 0;
}

static void add_app(const char *path, const char *id, int flatpak)
{
    if (napps >= MAXAPPS) return;
    if (seen(id)) return;                      /* the user's copy shadows the system's */

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
        if (!strcmp(line, "Name") && !name)           { name = w2k_strdup(val); w2k_desktop_unescape(name); }
        else if (!strcmp(line, "Exec") && !exec)      exec = w2k_strdup(val);
        else if (!strcmp(line, "Categories"))         { free(cats); cats = w2k_strdup(val); }
        else if (!strcmp(line, "Icon") && !icon)      { icon = w2k_strdup(val); w2k_desktop_unescape(icon); }
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
        a->wine = adding_wine;
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

/* Wine's menu tree: applications/wine/Programs/<vendor>/<program>.desktop,
 * folders within folders. The ids carry the folder so two vendors' "Help"
 * entries stay apart. */
static void scan_wine(const char *dir, const char *rel, int depth)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        char path[2048], id[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        snprintf(id, sizeof id, "wine/%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) scan_wine(path, id + 5, depth + 1);
            continue;
        }
        size_t n = strlen(de->d_name);
        if (n < 9 || strcmp(de->d_name + n - 8, ".desktop")) continue;
        adding_wine = 1;
        add_app(path, id, 0);
        adding_wine = 0;
    }
    closedir(dp);
}

static int cmp_name(const void *a, const void *b);

/* What the application folders look like now: their modification times,
 * Wine's sub-folders included, folded into one number. Installing or
 * removing a program changes a folder's time. */
static unsigned long long sig_mix(unsigned long long h, const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) return h * 1099511628211ULL;
    h = (h ^ (unsigned long long)st.st_mtim.tv_sec) * 1099511628211ULL;
    h = (h ^ (unsigned long long)st.st_mtim.tv_nsec) * 1099511628211ULL;
    for (const char *q = p; *q; q++) h = (h ^ (unsigned char)*q) * 1099511628211ULL;
    return h;
}

static unsigned long long sig_tree(unsigned long long h, const char *dir, int depth)
{
    h = sig_mix(h, dir);
    if (depth > 4) return h;
    DIR *dp = opendir(dir);
    if (!dp) return h;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.' || de->d_type != DT_DIR) continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        h = sig_tree(h, p, depth + 1);
    }
    closedir(dp);
    return h;
}

static unsigned long long folders_signature(void)
{
    unsigned long long h = 1469598103934665603ULL;
    const char *home = getenv("HOME"), *xdh = getenv("XDG_DATA_HOME");
    char path[2048];
    if (xdh && *xdh) snprintf(path, sizeof path, "%s/applications", xdh);
    else if (home)   snprintf(path, sizeof path, "%s/.local/share/applications", home);
    else path[0] = 0;
    if (path[0]) {
        h = sig_mix(h, path);
        /* Appended in place, a very long XDG_DATA_HOME ran off the buffer. */
        char wine[2048];
        snprintf(wine, sizeof wine, "%.2042s/wine", path);
        h = sig_tree(h, wine, 0);
    }
    if (home) {
        snprintf(path, sizeof path, "%s/.local/share/flatpak/exports/share/applications", home);
        h = sig_mix(h, path);
    }
    h = sig_mix(h, "/var/lib/flatpak/exports/share/applications");
    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    char copy[2048], *sp = NULL;
    snprintf(copy, sizeof copy, "%s", dirs);
    for (char *tok = strtok_r(copy, ":", &sp); tok; tok = strtok_r(NULL, ":", &sp)) {
        snprintf(path, sizeof path, "%s/applications", tok);
        h = sig_mix(h, path);
    }
    return h;
}

/* The programs, read from their .desktop files and sorted by name. Every
 * opening of the classic Start menu (and every All Programs, every search,
 * every taskbar right-click) used to read them all again; now only a
 * change to the folders, or a minute gone by, does. And always sorted:
 * a rescan left unsorted used to give a menu's ids to other programs. */
static void scan_all(void)
{
    static unsigned long long last_sig;
    static long last_scan;
    long now = w2k_now_ms();
    unsigned long long sig = folders_signature();
    if (napps && sig == last_sig && now - last_scan < 60000) return;
    last_sig = sig;
    last_scan = now;

    for (int i = 0; i < napps; i++) {
        free(apps[i].id);
        free(apps[i].name);
        free(apps[i].exec);
        free(apps[i].icon);
    }
    napps = 0;
    for (int i = 0; i < nseen; i++) free(seen_ids[i]);
    nseen = 0;

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
    /* What Wine installed. */
    if (xdh && *xdh) snprintf(path, sizeof path, "%s/applications/wine", xdh);
    else if (home)   snprintf(path, sizeof path, "%s/.local/share/applications/wine", home);
    else path[0] = 0;
    if (path[0]) scan_wine(path, "", 0);
    scan_dir("/var/lib/flatpak/exports/share/applications", 1);

    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    char copy[2048], *sp = NULL;
    snprintf(copy, sizeof copy, "%s", dirs);
    for (char *tok = strtok_r(copy, ":", &sp); tok; tok = strtok_r(NULL, ":", &sp)) {
        snprintf(path, sizeof path, "%s/applications", tok);
        scan_dir(path, strstr(tok, "flatpak") != NULL);
    }
    qsort(apps, (size_t)napps, sizeof *apps, cmp_name);
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

/* Set while a group is showing everything, after the chevron was clicked.
 * Groups are counted as slots: the categories, then Flatpak (NGROUPS) and
 * Windows Programs (NGROUPS + 1), which have no category of their own --
 * numbered by category alone, their chevrons were the first category's. */
static int expand_group = -1;

void programs_expand(int group) { expand_group = group; }
void programs_collapse_all(void) { expand_group = -1; }

/* Build one group's submenu; NULL when the group is empty. */
static W2kMenu *group_menu(int group, int flatpak, int wine)
{
    W2kMenu *m = NULL;
    int shown = 0, hidden = 0;
    int slot = flatpak ? NGROUPS : wine ? NGROUPS + 1 : group;
    int expand = !w2k_start_personalized || expand_group == slot;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < napps; i++) {
            if (apps[i].flatpak != flatpak || apps[i].wine != wine) continue;
            if (!flatpak && !wine && apps[i].group != group) continue;
            int used = usage_count(apps[i].name) > 0;
            /* First pass: what has been used. Second: the rest, unless
             * the group is folded away behind a chevron -- which only a
             * group with a few unused items gets, so one or two are never
             * hidden with nothing to bring them back. */
            if (pass == 0 && !used) { hidden++; continue; }
            if (pass == 1 && (used || (!expand && hidden >= PERSONAL_MIN_HIDDEN))) continue;
            if (!m) m = w2k_menu_new();
            /* A name is data: "Sound & Video" is not a mnemonic. */
            char label[256];
            w2k_menu_escape(apps[i].name, label, sizeof label);
            w2k_menu_item(m, PROG_BASE + i, label, NULL,
                          app_icon(&apps[i]));
            shown++;
        }
    }
    /* Nothing used yet: show the lot rather than an empty group. */
    if (!m && hidden) {
        m = w2k_menu_new();
        for (int i = 0; i < napps; i++) {
            if (apps[i].flatpak != flatpak || apps[i].wine != wine) continue;
            if (!flatpak && !wine && apps[i].group != group) continue;
            char label[256];
            w2k_menu_escape(apps[i].name, label, sizeof label);
            w2k_menu_item(m, PROG_BASE + i, label, NULL,
                          app_icon(&apps[i]));
        }
        return m;
    }
    if (m && !expand && hidden >= PERSONAL_MIN_HIDDEN) {
        w2k_menu_sep(m);
        w2k_menu_item(m, CHEVRON_ID + slot, "\xc2\xbb", NULL, ICO_NONE);
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
    if (id < CHEVRON_ID || id > CHEVRON_ID + NGROUPS + 1) return 0;
    if (group) *group = id - CHEVRON_ID;
    return 1;
}

/* For w2k_menu_on_expand: a chevron's group again, showing everything,
 * for the menu to put where the folded one was. */
W2kMenu *programs_expand_menu(int id)
{
    int slot;
    if (!programs_is_chevron(id, &slot)) return NULL;
    expand_group = slot;
    return group_menu(slot < NGROUPS ? slot : 0, slot == NGROUPS, slot == NGROUPS + 1);
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
    scan_all();                              /* cached: only a change rescans */
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
    scan_all();                              /* sorted by name */

    int any = 0;
    for (int g = 0; g < NGROUPS; g++) {
        W2kMenu *sub = group_menu(g, 0, 0);
        if (!sub) continue;
        w2k_menu_sub(m, groups[g].name, ICO_PROGRAMS, sub);
        any = 1;
    }
    W2kMenu *fp = group_menu(0, 1, 0);
    if (fp) {
        if (any) w2k_menu_sep(m);
        w2k_menu_sub(m, "Flatpak", ICO_PROGRAMS, fp);
        any = 1;
    }
    W2kMenu *wn = group_menu(0, 0, 1);
    if (wn) {
        if (any && !fp) w2k_menu_sep(m);
        w2k_menu_sub(m, "Windows Programs", ICO_PROGRAMS, wn);
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
/* The command line an id stands for, and whether it wants a terminal.
 * A caller that keeps a result across a rescan must keep this, not the
 * id: scan_all() re-sorts apps[] and the id would then name another
 * program. */
int programs_command(int id, char *cmd, int cn, int *terminal, char *name, int nn)
{
    int i = id - PROG_BASE;
    if (i < 0 || i >= napps) return 0;
    if (cmd && cn) snprintf(cmd, (size_t)cn, "%s", apps[i].exec);
    if (name && nn) snprintf(name, (size_t)nn, "%s", apps[i].name);
    if (terminal) *terminal = apps[i].terminal;
    return 1;
}

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
