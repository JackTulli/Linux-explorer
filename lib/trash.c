/* trash.c -- the Recycle Bin.
 *
 * The freedesktop trash is ~/.local/share/Trash: deleted files under files/,
 * a matching .trashinfo under info/. Both the desktop and Explorer need to
 * count it and empty it, so it lives here rather than in either of them.
 *
 * Emptying deletes user data, so the recursion is deliberately narrow: it
 * refuses to run anywhere except the two trash subdirectories, never follows
 * a symbolic link out of them, and reports how much it removed. */
#include "w2k.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

const char *w2k_trash_dir(void)
{
    static char dir[1024];
    if (dir[0]) return dir;
    const char *data = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (data && *data) snprintf(dir, sizeof dir, "%s/Trash", data);
    else if (home)     snprintf(dir, sizeof dir, "%s/.local/share/Trash", home);
    else               snprintf(dir, sizeof dir, "/tmp/Trash");
    return dir;
}

const char *w2k_trash_files_dir(void)
{
    static char dir[1100];
    if (!dir[0]) snprintf(dir, sizeof dir, "%s/files", w2k_trash_dir());
    return dir;
}

/* mkdir -p: create `path` and every directory above it. */
static void make_path(const char *path)
{
    char buf[1200];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

int w2k_trash_count(void)
{
    DIR *dp = opendir(w2k_trash_files_dir());
    if (!dp) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dp)))
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) n++;
    closedir(dp);
    return n;
}

/* Delete everything inside `path`, then `path` itself unless it is a root
 * we were asked to keep. Only ever called from w2k_trash_empty(). */
static int purge(const char *path, int keep_self, int depth)
{
    if (depth > 32) return 0;                 /* pathological nesting */
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    int removed = 0;
    if (S_ISDIR(st.st_mode)) {                /* a symlink is never followed */
        DIR *dp = opendir(path);
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char child[4096];
                if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >=
                    (int)sizeof child) continue;
                removed += purge(child, 0, depth + 1);
            }
            closedir(dp);
        }
        if (!keep_self && rmdir(path) == 0) removed++;
    } else if (unlink(path) == 0) {
        removed++;
    }
    return removed;
}

/* Move a file or folder into the Recycle Bin, freedesktop style: the item
 * goes to files/, and a matching .trashinfo in info/ records where it came
 * from and when -- which is what makes Restore possible.
 *
 * Returns 0 on success. A name that is already taken gets a numeric
 * suffix, so deleting two files called notes.txt keeps both. */
/* The plain form; w2k_trash_move_named() also reports the name the item
 * ended up under, which is what Undo needs to put it back. */
int w2k_trash_move(const char *path)
{
    return w2k_trash_move_named(path, NULL, 0);
}

int w2k_trash_move_named(const char *path, char *name_out, int nout)
{
    if (!path || !*path) return -1;
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char files[1100], info[1100];
    snprintf(files, sizeof files, "%s/files", w2k_trash_dir());
    snprintf(info, sizeof info, "%s/info", w2k_trash_dir());
    /* ~/.local/share need not exist yet on a fresh account. */
    make_path(files);
    make_path(info);

    char target[2400], meta[2400], name[512];
    snprintf(name, sizeof name, "%.500s", base);
    snprintf(target, sizeof target, "%s/%s", files, name);
    for (int k = 2; k < 1000 && lstat(target, &st) == 0; k++) {
        snprintf(name, sizeof name, "%.480s.%d", base, k);
        snprintf(target, sizeof target, "%s/%s", files, name);
    }
    snprintf(meta, sizeof meta, "%s/%s.trashinfo", info, name);

    /* An absolute original path, so Restore knows where to put it back. */
    char abs[2048];
    if (path[0] == '/') snprintf(abs, sizeof abs, "%s", path);
    else {
        char cwd[1024];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(abs, sizeof abs, "%s/%s", cwd, path);
    }

    if (rename(path, target) != 0) {
        /* rename() cannot cross a filesystem boundary, and the bin lives on
         * the home one. For a file, copy it over and unlink the original;
         * a directory from another filesystem is refused rather than
         * half-copied. */
        if (errno != EXDEV || !S_ISREG(st.st_mode)) return -1;

        FILE *in = fopen(path, "rb");
        if (!in) return -1;
        FILE *out = fopen(target, "wb");
        if (!out) { fclose(in); return -1; }
        char buf[65536];
        size_t got;
        int ok = 1;
        while ((got = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, got, out) != got) { ok = 0; break; }
        if (ferror(in)) ok = 0;
        fclose(in);
        if (fclose(out) != 0) ok = 0;
        if (!ok) { unlink(target); return -1; }
        if (unlink(path) != 0) { unlink(target); return -1; }
    }

    FILE *f = fopen(meta, "w");
    if (f) {
        time_t now = time(NULL);
        struct tm tm;
        char when[32] = "";
        if (localtime_r(&now, &tm))
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
        fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", abs, when);
        fclose(f);
    }
    if (name_out && nout > 0) snprintf(name_out, (size_t)nout, "%s", name);
    return 0;
}

/* Put a trashed item back where it came from. `name` is its name inside
 * files/. Returns 0 on success. */
int w2k_trash_restore(const char *name)
{
    if (!name || !*name) return -1;
    char meta[2400], from[2400], line[2048], dest[2048] = "";
    snprintf(from, sizeof from, "%s/files/%s", w2k_trash_dir(), name);
    snprintf(meta, sizeof meta, "%s/info/%s.trashinfo", w2k_trash_dir(), name);

    FILE *f = fopen(meta, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "Path=", 5))
                snprintf(dest, sizeof dest, "%s", line + 5);
        }
        fclose(f);
    }
    if (!dest[0]) return -1;
    if (rename(from, dest) != 0) {
        if (errno != EXDEV) return -1;
        FILE *in = fopen(from, "rb");
        if (!in) return -1;
        FILE *out = fopen(dest, "wb");
        if (!out) { fclose(in); return -1; }
        char buf[65536];
        size_t got;
        int ok = 1;
        while ((got = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, got, out) != got) { ok = 0; break; }
        fclose(in);
        if (fclose(out) != 0) ok = 0;
        if (!ok) { unlink(dest); return -1; }
        unlink(from);
    }
    unlink(meta);
    return 0;
}

/* Returns the number of entries removed, or -1 if the trash is not there. */
int w2k_trash_empty(void)
{
    const char *base = w2k_trash_dir();
    struct stat st;
    if (stat(base, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;

    char files[1100], info[1100];
    snprintf(files, sizeof files, "%s/files", base);
    snprintf(info, sizeof info, "%s/info", base);

    int n = purge(files, 1, 0);
    purge(info, 1, 0);                        /* the .trashinfo stubs */
    return n;
}
