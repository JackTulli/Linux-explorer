/* assoc.c -- which program opens which kind of file.
 *
 * A handful of classes rather than one entry per extension: that is what a
 * user actually wants to set ("open videos in VLC"), and it is what the
 * Control Panel presents. ~/.w2k/associations overrides the defaults, one
 * "class=command" per line; %s in a command is where the file goes, and a
 * command without one gets the file appended.
 *
 * Anything unrecognised falls through to xdg-open, so the desktop still
 * opens PDFs and the like with whatever the system has. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct { const char *cls; const char *label; const char *def; }
defaults[] = {
    { "folder","Folders",         "w2kexplorer" },
    { "image", "Pictures",        "w2kimage" },
    { "video", "Video",           "vlc" },
    { "audio", "Music",           "vlc" },
    { "text",  "Text documents",  "w2knotepad" },
    { "web",   "Web pages",       "xdg-open" },
    { "other", "Everything else", "xdg-open" },
};
#define NCLASS ((int)(sizeof defaults / sizeof *defaults))

int w2k_assoc_count(void) { return NCLASS; }
const char *w2k_assoc_class_at(int i)
{
    return (i >= 0 && i < NCLASS) ? defaults[i].cls : NULL;
}
const char *w2k_assoc_label_at(int i)
{
    return (i >= 0 && i < NCLASS) ? defaults[i].label : NULL;
}

static void assoc_path(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/.w2k/associations", home ? home : ".");
}

void w2k_assoc_get(const char *cls, char *out, int n)
{
    out[0] = 0;
    char path[1024];
    assoc_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            if (!strcasecmp(line, cls)) {
                snprintf(out, (size_t)n, "%s", eq + 1);
                break;
            }
        }
        fclose(f);
    }
    if (out[0]) return;
    for (int i = 0; i < NCLASS; i++)
        if (!strcmp(defaults[i].cls, cls)) {
            snprintf(out, (size_t)n, "%s", defaults[i].def);
            return;
        }
    snprintf(out, (size_t)n, "xdg-open");
}

void w2k_assoc_set(const char *cls, const char *cmd)
{
    char path[1024];
    assoc_path(path, sizeof path);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }

    /* Read the file, replace this class, write it back. */
    char kept[NCLASS][1024];
    for (int i = 0; i < NCLASS; i++) kept[i][0] = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            for (int i = 0; i < NCLASS; i++)
                if (!strcasecmp(line, defaults[i].cls))
                    snprintf(kept[i], sizeof kept[i], "%s", eq + 1);
        }
        fclose(f);
    }
    for (int i = 0; i < NCLASS; i++)
        if (!strcmp(defaults[i].cls, cls))
            snprintf(kept[i], sizeof kept[i], "%s", cmd ? cmd : "");

    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Windows 2000 for X11 -- file associations\n"
               "# class=command   (%%s is where the file name goes)\n");
    for (int i = 0; i < NCLASS; i++)
        if (kept[i][0]) fprintf(f, "%s=%s\n", defaults[i].cls, kept[i]);
    fclose(f);
}

/* Which class does this file belong to? */
const char *w2k_assoc_class_for(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return "folder";

    const char *dot = strrchr(path, '.');
    if (!dot) return "other";
    static const struct { const char *ext, *cls; } map[] = {
        { ".png","image" }, { ".jpg","image" }, { ".jpeg","image" },
        { ".jpe","image" }, { ".bmp","image" }, { ".dib","image" },
        { ".gif","image" }, { ".webp","image" }, { ".tif","image" },
        { ".tiff","image" }, { ".ico","image" }, { ".xpm","image" },
        { ".mp4","video" }, { ".mkv","video" }, { ".avi","video" },
        { ".mov","video" }, { ".webm","video" }, { ".wmv","video" },
        { ".mpg","video" }, { ".mpeg","video" }, { ".m4v","video" },
        { ".flv","video" }, { ".ogv","video" }, { ".ts","video" },
        { ".mp3","audio" }, { ".flac","audio" }, { ".ogg","audio" },
        { ".wav","audio" }, { ".m4a","audio" }, { ".opus","audio" },
        { ".wma","audio" }, { ".aac","audio" },
        { ".txt","text" }, { ".log","text" }, { ".md","text" },
        { ".c","text" }, { ".h","text" }, { ".cpp","text" }, { ".py","text" },
        { ".sh","text" }, { ".conf","text" }, { ".cfg","text" },
        { ".ini","text" }, { ".json","text" }, { ".xml","text" },
        { ".html","web" }, { ".htm","web" }, { ".url","web" },
        { NULL, NULL }
    };
    for (int i = 0; map[i].ext; i++)
        if (!strcasecmp(dot, map[i].ext)) return map[i].cls;
    return "other";
}

/* Build the command line that opens `path`. */
void w2k_assoc_command(const char *path, char *out, int n)
{
    const char *cls = w2k_assoc_class_for(path);
    char cmd[512];
    w2k_assoc_get(cls, cmd, sizeof cmd);

    if (strstr(cmd, "%s")) {
        char quoted[1200];
        snprintf(quoted, sizeof quoted, "'%s'", path);
        snprintf(out, (size_t)n, cmd, quoted);
    } else {
        snprintf(out, (size_t)n, "%s '%s'", cmd, path);
    }
}

/* Tell the rest of the desktop which file manager opens folders: the
 * XDG default for inode/directory, which is what xdg-open and "show in
 * folder" in other programs consult. Explorer gets a .desktop file of its
 * own in ~/.local/share/applications for the purpose; another program
 * is named by its .desktop, if it has one. 1 if the default was set. */
int w2k_assoc_apply_folder_default(void)
{
    char cmd[512];
    w2k_assoc_get("folder", cmd, sizeof cmd);
    if (!cmd[0]) return 0;
    char word[256];
    snprintf(word, sizeof word, "%.255s", cmd);
    word[strcspn(word, " \t")] = 0;
    const char *base = strrchr(word, '/');
    base = base ? base + 1 : word;
    const char *home = getenv("HOME");
    if (!home) return 0;

    char desktop[300];
    if (!strcmp(base, "w2kexplorer")) {
        /* Our own: written fresh, pointing at the binary that is running. */
        char dir[1200], path[1500], exe[1024] = "w2kexplorer";
        char self[1024];
        ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
        if (len > 0) {
            self[len] = 0;
            char *slash = strrchr(self, '/');
            if (slash) { *slash = 0; snprintf(exe, sizeof exe, "%.900s/w2kexplorer", self); }
        }
        if (access(exe, X_OK) != 0) snprintf(exe, sizeof exe, "w2kexplorer");
        snprintf(dir, sizeof dir, "%s/.local/share/applications", home);
        char parent[1200];
        snprintf(parent, sizeof parent, "%s/.local/share", home);
        mkdir(parent, 0755);
        mkdir(dir, 0755);
        snprintf(path, sizeof path, "%s/w2kexplorer.desktop", dir);
        FILE *f = fopen(path, "w");
        if (!f) return 0;
        fprintf(f, "[Desktop Entry]\nType=Application\nName=Windows Explorer\n"
                   "Comment=Browse files and folders\nExec=%s %%f\nIcon=system-file-manager\n"
                   "Terminal=false\nCategories=System;FileTools;FileManager;\n"
                   "MimeType=inode/directory;x-directory/normal;\nNoDisplay=false\n", exe);
        fclose(f);
        snprintf(desktop, sizeof desktop, "w2kexplorer.desktop");
    } else {
        snprintf(desktop, sizeof desktop, "%.200s.desktop", base);
    }
    char run[1200];
    snprintf(run, sizeof run,
             "xdg-mime default '%s' inode/directory x-directory/normal >/dev/null 2>&1", desktop);
    return system(run) == 0;
}
