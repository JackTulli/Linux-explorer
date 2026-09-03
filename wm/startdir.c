/* startdir.c -- the Start menu as a folder tree.
 *
 * Windows builds the Programs menu by walking a directory: every folder is
 * a submenu, every shortcut an item, and rearranging the menu means moving
 * files around. That is what this does, over ~/.w2k/Start Menu:
 *
 *     Start Menu/Programs/...        the tree under Programs
 *     Start Menu/Programs/Startup/   run once when the session begins
 *
 * Entries are .desktop files, the same as the desktop's shortcuts, so one
 * can be dragged from one to the other later without conversion.
 *
 * The tree is merged with the installed-application groups rather than
 * replacing them: a fresh account has an empty tree and would otherwise
 * see an empty Programs menu. */
#include "wm.h"
#include "w2kui.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ENTRIES 256

static struct { char cmd[512]; } entries[MAX_ENTRIES];
static int nentries;

const char *startdir_command(int id)
{
    int i = id - STARTDIR_BASE;
    return (i >= 0 && i < nentries) ? entries[i].cmd : NULL;
}

void startdir_path(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/.w2k/Start Menu", home ? home : ".");
}

/* Make the tree on first use, so there is somewhere to drop things. */
void startdir_ensure(void)
{
    char base[1024], sub[1200];
    startdir_path(base, sizeof base);

    char parent[1024];
    snprintf(parent, sizeof parent, "%s", base);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; mkdir(parent, 0755); }

    mkdir(base, 0755);
    snprintf(sub, sizeof sub, "%s/Programs", base);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/Programs/Startup", base);
    mkdir(sub, 0755);
}

/* Name and command out of a .desktop file. */
static int read_entry(const char *path, char *name, int nn, char *cmd, int cn,
                      char *icon, int in)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int in_entry = 0;
    name[0] = cmd[0] = icon[0] = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { in_entry = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in_entry) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(line, "Name") && !name[0]) snprintf(name, (size_t)nn, "%s", eq + 1);
        else if (!strcmp(line, "Exec") && !cmd[0]) snprintf(cmd, (size_t)cn, "%s", eq + 1);
        else if (!strcmp(line, "Icon") && !icon[0]) snprintf(icon, (size_t)in, "%s", eq + 1);
    }
    fclose(f);
    for (char *p = cmd; *p; p++)
        if (p[0] == '%' && p[1]) { p[0] = 0; break; }
    return name[0] && cmd[0];
}

static int cmp_str(const void *a, const void *b)
{
    return strcasecmp(*(const char **)a, *(const char **)b);
}

/* Add one directory's contents to `m`: folders first as submenus, then
 * shortcuts, each sorted by name -- the order Windows shows. */
static int add_dir(W2kMenu *m, const char *dir, int depth, int skip_startup)
{
    if (depth > 6) return 0;
    DIR *dp = opendir(dir);
    if (!dp) return 0;

    char *dirs[128], *files[128];
    int nd = 0, nf = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (skip_startup && !strcasecmp(de->d_name, "Startup")) continue;
            if (nd < 128) dirs[nd++] = w2k_strdup(de->d_name);
        } else {
            size_t len = strlen(de->d_name);
            if (len > 8 && !strcasecmp(de->d_name + len - 8, ".desktop") &&
                nf < 128)
                files[nf++] = w2k_strdup(de->d_name);
        }
    }
    closedir(dp);
    qsort(dirs, (size_t)nd, sizeof *dirs, cmp_str);
    qsort(files, (size_t)nf, sizeof *files, cmp_str);

    int added = 0;
    for (int i = 0; i < nd; i++) {
        char sub[2048];
        snprintf(sub, sizeof sub, "%s/%s", dir, dirs[i]);
        W2kMenu *child = w2k_menu_new();
        if (add_dir(child, sub, depth + 1, 0)) {
            w2k_menu_sub(m, dirs[i], ICO_FOLDER, child);
            added = 1;
        } else {
            w2k_menu_free(child);
        }
        free(dirs[i]);
    }
    for (int i = 0; i < nf; i++) {
        char full[2048], name[128], cmd[512], icon[128];
        snprintf(full, sizeof full, "%s/%s", dir, files[i]);
        if (read_entry(full, name, sizeof name, cmd, sizeof cmd,
                       icon, sizeof icon) && nentries < MAX_ENTRIES) {
            snprintf(entries[nentries].cmd, sizeof entries[nentries].cmd,
                     "%s", cmd);
            w2k_menu_item(m, STARTDIR_BASE + nentries, name, NULL,
                          icon[0] ? w2k_icon_by_name(icon) : ICO_APP);
            nentries++;
            added = 1;
        }
        free(files[i]);
    }
    return added;
}

/* Everything under Start Menu/Programs, minus Startup. Returns how many
 * top-level things it added. */
int startdir_add_programs(W2kMenu *m)
{
    nentries = 0;
    startdir_ensure();
    char dir[1200];
    startdir_path(dir, sizeof dir);
    strncat(dir, "/Programs", sizeof dir - strlen(dir) - 1);
    return add_dir(m, dir, 0, 1);
}

/* Run everything in Startup, once, when the session begins. */
void startdir_run_startup(void)
{
    char dir[1300];
    startdir_path(dir, sizeof dir);
    strncat(dir, "/Programs/Startup", sizeof dir - strlen(dir) - 1);

    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        char full[2048], name[128], cmd[512], icon[128];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        size_t len = strlen(de->d_name);
        if (len > 8 && !strcasecmp(de->d_name + len - 8, ".desktop")) {
            if (read_entry(full, name, sizeof name, cmd, sizeof cmd,
                           icon, sizeof icon))
                wm_spawn(cmd);
        } else if (access(full, X_OK) == 0) {
            wm_spawn(full);          /* a plain executable or script */
        }
    }
    closedir(dp);
}
