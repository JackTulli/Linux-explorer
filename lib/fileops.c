/* fileops.c -- copying, moving and deleting trees, shared by everything
 * that takes a drop: Explorer, the desktop, the taskbar.
 *
 * A move is a rename when the two paths share a filesystem, and a copy
 * followed by a delete when they do not. A copy of a folder copies what is
 * in it. Symlinks are copied as links. The caller decides what happens
 * when the destination exists, through `confirm`: 1 replaces it, 0 skips
 * it, -1 stops the whole operation. */
#include "w2k.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void join(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
}

static int copy_one(const char *from, const char *to)
{
    struct stat st;
    if (stat(from, &st) != 0) return 0;
    /* Only regular files are copied as data: a FIFO would block for ever
     * and a device node would never end. */
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; return 0; }
    FILE *a = fopen(from, "rb");
    if (!a) return 0;
    /* Created with the source's own mode from the start (not world-
     * readable while a private file is half copied), and never through a
     * symlink that happens to be sitting at the destination. */
    unlink(to);
    int fd = open(to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, st.st_mode & 07777);
    if (fd < 0) { fclose(a); return 0; }
    FILE *b = fdopen(fd, "wb");
    if (!b) { close(fd); fclose(a); return 0; }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { ok = 0; break; }
    if (ferror(a)) ok = 0;
    fclose(a);
    if (fclose(b) != 0) ok = 0;
    if (ok) {
        struct stat st;
        if (stat(from, &st) == 0) chmod(to, st.st_mode & 07777);
    } else unlink(to);
    return ok;
}

int w2k_fs_copy_tree(const char *from, const char *to)
{
    static int depth;
    if (depth > 40) return 0;
    struct stat st;
    if (lstat(from, &st) != 0) return 0;
    if (S_ISLNK(st.st_mode)) {
        char target[2048];
        ssize_t n = readlink(from, target, sizeof target - 1);
        if (n <= 0) return 0;
        target[n] = 0;
        unlink(to);
        return symlink(target, to) == 0;
    }
    if (!S_ISDIR(st.st_mode)) return copy_one(from, to);

    /* A folder into itself would never end. */
    size_t fl = strlen(from);
    if (!strncmp(from, to, fl) && (to[fl] == '/' || to[fl] == 0)) { errno = EINVAL; return 0; }
    if (mkdir(to, st.st_mode & 07777) != 0 && errno != EEXIST) return 0;
    DIR *dp = opendir(from);
    if (!dp) return 0;
    int ok = 1;
    struct dirent *de;
    depth++;
    while (ok && (de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char a[2048], b[2048];
        join(a, sizeof a, from, de->d_name);
        join(b, sizeof b, to, de->d_name);
        if (!w2k_fs_copy_tree(a, b)) ok = 0;
    }
    depth--;
    closedir(dp);
    return ok;
}

int w2k_fs_remove_tree(const char *path)
{
    static int depth;
    if (depth > 40) return 0;
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    DIR *dp = opendir(path);
    if (!dp) return 0;
    struct dirent *de;
    depth++;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[2048];
        join(sub, sizeof sub, path, de->d_name);
        w2k_fs_remove_tree(sub);
    }
    depth--;
    closedir(dp);
    return rmdir(path) == 0;
}

int w2k_fs_move(const char *from, const char *to)
{
    if (rename(from, to) == 0) return 1;
    if (errno != EXDEV) return 0;
    if (!w2k_fs_copy_tree(from, to)) { w2k_fs_remove_tree(to); return 0; }
    return w2k_fs_remove_tree(from);
}

/* Put `n` paths into `dir`, moving or copying. Returns how many landed;
 * `confirm` (may be NULL: always replace) is asked about existing names. */
int w2k_fs_transfer(char paths[][1024], int n, const char *dir, int move,
                    int (*confirm)(const char *dst, void *user), void *user)
{
    int done = 0;
    for (int i = 0; i < n; i++) {
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        if (!*base) continue;
        char to[2100];
        join(to, sizeof to, dir, base);
        if (!strcmp(paths[i], to)) continue;          /* onto itself */
        struct stat st;
        if (lstat(to, &st) == 0) {
            /* Replacing a folder that the source lives inside would delete
             * the source along with it: never that. */
            size_t tl = strlen(to);
            if (!strncmp(paths[i], to, tl) && paths[i][tl] == '/') { errno = EINVAL; continue; }
            int c = confirm ? confirm(to, user) : 1;
            if (c < 0) break;
            if (c == 0) continue;
            if (!w2k_fs_remove_tree(to)) continue;
        }
        int ok = move ? w2k_fs_move(paths[i], to) : w2k_fs_copy_tree(paths[i], to);
        if (ok) done++;
    }
    return done;
}

/* ------------------------------------------------------------------ *
 * Drives: what is mounted where a user would think of it as a drive --
 * removable media under /media and /run/media, anything under /mnt --
 * lettered from D: the way Windows would.
 * ------------------------------------------------------------------ */
int w2k_fs_drives(W2kDrive *out, int max)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    int n = 0;
    char line[1024];
    const char *user = getenv("USER");
    while (n < max && fgets(line, sizeof line, f)) {
        char dev[256], mnt[512], fs[64];
        if (sscanf(line, "%255s %511s %63s", dev, mnt, fs) != 3) continue;
        int ok = !strncmp(mnt, "/media/", 7) || !strncmp(mnt, "/run/media/", 11) ||
                 !strncmp(mnt, "/mnt/", 5);
        if (!ok) continue;
        /* /media/<user> itself is a folder of mounts, not a mount. */
        if (user && (!strcmp(mnt + 7, user) || (!strncmp(mnt, "/run/media/", 11) && !strcmp(mnt + 11, user)))) continue;
        /* \\040 for a space, from the kernel */
        char *p = mnt, *q = mnt;
        while (*p) {
            if (!strncmp(p, "\\040", 4)) { *q++ = ' '; p += 4; }
            else *q++ = *p++;
        }
        *q = 0;
        W2kDrive *d = &out[n];
        memset(d, 0, sizeof *d);
        snprintf(d->path, sizeof d->path, "%s", mnt);
        const char *base = strrchr(mnt, '/');
        snprintf(d->label, sizeof d->label, "%.127s", base && base[1] ? base + 1 : mnt);
        d->letter = (char)('D' + n);
        d->optical = !strcmp(fs, "iso9660") || !strcmp(fs, "udf");
        d->removable = !strncmp(mnt, "/media/", 7) || !strncmp(mnt, "/run/media/", 11);
        n++;
    }
    fclose(f);
    return n;
}

/* An Internet shortcut, the way Windows makes one when a link is dropped
 * on a folder: a .desktop of Type=Link named after the site. */
int w2k_fs_write_url_shortcut(const char *dir, const char *url)
{
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    char name[128];
    int o = 0;
    for (const char *p = host; *p && *p != '/' && *p != '?' && o < 100; p++)
        name[o++] = (*p == ':' ) ? '_' : *p;
    name[o] = 0;
    if (!o) snprintf(name, sizeof name, "Internet Shortcut");
    char path[1500];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s.desktop", dir, name);
    for (int k = 2; k < 100 && lstat(path, &st) == 0; k++)
        snprintf(path, sizeof path, "%s/%s (%d).desktop", dir, name, k);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "[Desktop Entry]\nType=Link\nName=%s\nURL=%s\nIcon=text-html\n", name, url);
    fclose(f);
    return 1;
}

/* The http(s) lines of a dropped URI list, for the above. */
int w2k_uri_list_urls(const char *uris, char urls[][1024], int max)
{
    int n = 0;
    const char *p = uris;
    while (*p && n < max) {
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != '\r' && *end != '\n') end++;
        if ((!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) && end - p < 1023) {
            memcpy(urls[n], p, (size_t)(end - p));
            urls[n][end - p] = 0;
            n++;
        }
        p = end;
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * Path tab-completion
 * ------------------------------------------------------------------ */
int w2k_tabcomp(const char *text, const char *cwd, char *out, int n, int flags)
{
    if (!text || !out || n <= 0) return 0;
    out[0] = 0;

    char path[1024];
    snprintf(path, sizeof path, "%s", text);

    /* Expand a leading ~ to $HOME. */
    if (path[0] == '~' && (path[1] == '/' || path[1] == 0)) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s%s", home, path[1] ? path + 1 : "");
            snprintf(path, sizeof path, "%s", tmp);
        }
    }

    char dir[1024], prefix[256];
    int had_slash = 0;
    const char *slash = strrchr(path, '/');
    if (slash) {
        had_slash = 1;
        size_t dl = (size_t)(slash - path);
        if (dl == 0) {
            snprintf(dir, sizeof dir, "/");
        } else {
            if (dl >= sizeof dir) dl = sizeof dir - 1;
            memcpy(dir, path, dl);
            dir[dl] = 0;
        }
        snprintf(prefix, sizeof prefix, "%s", slash + 1);
    } else {
        snprintf(dir, sizeof dir, "%s",
                 (cwd && cwd[0]) ? cwd : "/");
        snprintf(prefix, sizeof prefix, "%s", path);
    }

    DIR *dp = opendir(dir[0] ? dir : "/");
    if (!dp) return 0;

    char best[256] = "";
    int nmatch = 0;
    size_t plen = strlen(prefix);
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (de->d_name[0] == '.' && (!prefix[0] || prefix[0] != '.'))
            continue;
        if (plen && strncasecmp(de->d_name, prefix, plen) != 0) continue;
        if (flags & W2K_TABCOMP_DIRS) {
            char full[1200];
            snprintf(full, sizeof full, "%s/%s",
                     strcmp(dir, "/") ? dir : "", de->d_name);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        }
        nmatch++;
        if (!best[0]) {
            snprintf(best, sizeof best, "%s", de->d_name);
        } else {
            size_t i = 0;
            while (best[i] && de->d_name[i] &&
                   tolower((unsigned char)best[i]) ==
                   tolower((unsigned char)de->d_name[i]))
                i++;
            best[i] = 0;
        }
    }
    closedir(dp);
    if (!nmatch || !best[0]) return 0;

    char built[1024];
    if (had_slash) {
        if (dir[0] == '/' && dir[1] == 0)
            snprintf(built, sizeof built, "/%s", best);
        else
            snprintf(built, sizeof built, "%s/%s", dir, best);
    } else {
        /* Relative completion: return only the leaf (caller was typing
         * a name in the current folder). */
        snprintf(built, sizeof built, "%s", best);
    }

    /* Unique directory match: append a trailing slash. */
    if (nmatch == 1) {
        char full[1200];
        if (built[0] == '/')
            snprintf(full, sizeof full, "%s", built);
        else
            snprintf(full, sizeof full, "%s/%s",
                     strcmp(dir, "/") ? dir : "", best);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t L = strlen(built);
            if (L + 1 < sizeof built && built[L - 1] != '/') {
                built[L] = '/';
                built[L + 1] = 0;
            }
        }
    }

    if (!strcmp(built, text)) return 0;
    snprintf(out, (size_t)n, "%s", built);
    return 1;
}
