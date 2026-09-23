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
#include <limits.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void join(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
}

/* A name beside `path` for something being put there: "dir/.name.tag-pid-k",
 * one that does not exist yet. */
static int beside(const char *path, const char *tag, char *out, size_t n)
{
    const char *slash = strrchr(path, '/');
    int dl = slash ? (int)(slash - path) : 0;
    const char *base = slash ? slash + 1 : path;
    for (int k = 0; k < 1000; k++) {
        snprintf(out, n, "%.*s%s.%.200s.%s-%ld-%d", dl, path, slash ? "/" : "", base, tag,
                 (long)getpid(), k);
        struct stat st;
        if (lstat(out, &st) != 0 && errno == ENOENT) return 1;
    }
    errno = EEXIST;
    return 0;
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
    /* Written under a name of its own and renamed into place when it is
     * whole: a copy that fails half way (a full disk, an unreadable
     * source) used to have already deleted the file it was replacing.
     * Created with the source's own mode from the start (not world-
     * readable while a private file is half copied), and never through a
     * symlink that happens to be sitting at the destination. */
    char tmp[2400];
    if (!beside(to, "copying", tmp, sizeof tmp)) { fclose(a); return 0; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, st.st_mode & 07777);
    if (fd < 0) { fclose(a); return 0; }
    FILE *b = fdopen(fd, "wb");
    if (!b) { close(fd); unlink(tmp); fclose(a); return 0; }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { ok = 0; break; }
    if (ferror(a)) ok = 0;
    fclose(a);
    if (fclose(b) != 0) ok = 0;
    if (ok) chmod(tmp, st.st_mode & 07777);
    if (ok && rename(tmp, to) != 0) ok = 0;
    if (!ok) { int e = errno; unlink(tmp); errno = e; }
    return ok;
}

/* rename() that will not replace what is already at `to`: a rename in
 * Explorer, the Hidden box, an undo -- none of them may silently destroy
 * a file that happens to have the name. The kernel does it atomically
 * where it can; otherwise the name is looked at first. */
int w2k_fs_rename_noreplace(const char *from, const char *to)
{
#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 1 /* RENAME_NOREPLACE */) == 0)
        return 0;
    if (errno != ENOSYS && errno != EINVAL && errno != EEXIST) return -1;
#endif
    struct stat a, b;
    if (lstat(to, &b) == 0) {
        /* The same file under another spelling: a change of case on a
         * file system that ignores case. That is a rename, not a clash. */
        if (lstat(from, &a) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
            return rename(from, to);
        errno = EEXIST;
        return -1;
    }
    return rename(from, to);
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
    /* Made writable, and given the folder's own mode once its contents
     * are in: a read-only folder -- anything copied off a CD -- was made
     * read-only first, and nothing could then go into it. */
    int made = mkdir(to, (st.st_mode & 07777) | S_IRWXU) == 0;
    if (!made && errno != EEXIST) return 0;
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
    if (made && chmod(to, st.st_mode & 07777) != 0) ok = 0;
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

/* Where an entry lives, resolved: its folder through realpath (a doubled
 * slash, a symlinked folder, "..") and its own name as it is, not
 * followed -- a link being moved is the link. */
static int resolve_entry(const char *path, char *out, size_t n)
{
    char dir[PATH_MAX], rd[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    size_t l = strlen(dir);
    while (l > 1 && dir[l - 1] == '/') dir[--l] = 0;
    char *slash = strrchr(dir, '/');
    const char *base;
    if (!slash) { base = dir; if (!realpath(".", rd)) return 0; }
    else {
        *slash = 0;
        base = slash + 1;
        if (!realpath(dir[0] ? dir : "/", rd)) return 0;
    }
    snprintf(out, n, "%s%s%s", rd, strcmp(rd, "/") ? "/" : "", base);
    return 1;
}

/* Is the entry `path` the folder `dir`, or somewhere inside it? */
static int lives_in(const char *path, const char *dir)
{
    char rp[PATH_MAX + 256], rd[PATH_MAX + 256];
    if (!resolve_entry(path, rp, sizeof rp) || !resolve_entry(dir, rd, sizeof rd)) return 0;
    size_t n = strlen(rd);
    return !strncmp(rp, rd, n) && (rp[n] == '/' || rp[n] == 0);
}

/* A folder moved onto a folder of the same name: what is in it goes in,
 * as Windows does it, rather than one folder replacing the other and
 * whatever only the old one held going with it. */
static int move_merge(const char *from, const char *to)
{
    static int depth;
    if (depth > 40) return 0;
    DIR *dp = opendir(from);
    if (!dp) return 0;
    int ok = 1;
    struct dirent *de;
    depth++;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char a[2048], b[2048];
        join(a, sizeof a, from, de->d_name);
        join(b, sizeof b, to, de->d_name);
        struct stat sa, sb;
        if (lstat(b, &sb) != 0) { if (!w2k_fs_move(a, b)) ok = 0; continue; }
        if (lstat(a, &sa) != 0) { ok = 0; continue; }
        if (S_ISDIR(sa.st_mode) && S_ISDIR(sb.st_mode)) { if (!move_merge(a, b)) ok = 0; continue; }
        if (S_ISDIR(sa.st_mode) != S_ISDIR(sb.st_mode)) { ok = 0; continue; }   /* file vs folder */
        /* A file over a file: the old one aside until the new one is in. */
        char old[2400];
        if (!beside(b, "replaced", old, sizeof old) || rename(b, old) != 0) { ok = 0; continue; }
        if (w2k_fs_move(a, b)) w2k_fs_remove_tree(old);
        else { w2k_fs_remove_tree(b); rename(old, b); ok = 0; }
    }
    depth--;
    closedir(dp);
    if (ok) rmdir(from);             /* empty now; a failure leaves what did not move */
    return ok;
}

/* Put one item at `to` by moving or copying it. When `to` exists,
 * `confirm` (NULL: yes) is asked; 1 replaces, 0 skips, -1 stops.
 *
 * What is replaced is only let go once its replacement is in: it is
 * renamed aside first and renamed back if the copy fails, where it used
 * to be deleted before anything was known to have worked. A folder onto
 * a folder merges. And "the same file" and "the folder the source lives
 * in" are decided by where the paths lead, not by how they are spelt.
 *
 * Returns 1 done, 0 skipped, -1 stopped, -2 failed (errno says why). */
int w2k_fs_put(const char *from, const char *to, int move,
               int (*confirm)(const char *dst, void *user), void *user)
{
    struct stat sf, st;
    if (lstat(from, &sf) != 0) return -2;
    if (lstat(to, &st) != 0) {
        if (errno != ENOENT) return -2;
        int ok = move ? w2k_fs_move(from, to) : w2k_fs_copy_tree(from, to);
        return ok ? 1 : -2;
    }
    if (sf.st_dev == st.st_dev && sf.st_ino == st.st_ino) return 0;   /* onto itself */
    /* Replacing a folder that the source lives inside would delete the
     * source along with it: never that. */
    if (lives_in(from, to)) { errno = EINVAL; return -2; }
    int c = confirm ? confirm(to, user) : 1;
    if (c < 0) return -1;
    if (c == 0) return 0;
    if (S_ISDIR(sf.st_mode) && S_ISDIR(st.st_mode))
        return (move ? move_merge(from, to) : w2k_fs_copy_tree(from, to)) ? 1 : -2;
    if (S_ISDIR(sf.st_mode) != S_ISDIR(st.st_mode)) {
        errno = S_ISDIR(st.st_mode) ? EISDIR : ENOTDIR;    /* a file and a folder */
        return -2;
    }
    char old[2400];
    if (!beside(to, "replaced", old, sizeof old) || rename(to, old) != 0) return -2;
    int ok = move ? w2k_fs_move(from, to) : w2k_fs_copy_tree(from, to);
    if (ok) { w2k_fs_remove_tree(old); return 1; }
    int e = errno;
    w2k_fs_remove_tree(to);
    rename(old, to);
    errno = e;
    return -2;
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
        int r = w2k_fs_put(paths[i], to, move, confirm, user);
        if (r < 0 && r != -2) break;
        if (r == 1) done++;
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
        snprintf(d->dev, sizeof d->dev, "%s", dev);
        d->mounted = 1;
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

/* KEY="value" out of a line of lsblk -P, which writes odd bytes as \xHH. */
static void lsblk_value(const char *line, const char *key, char *out, size_t n)
{
    char pat[32];
    snprintf(pat, sizeof pat, "%s=\"", key);
    size_t kl = strlen(pat);
    out[0] = 0;
    for (const char *p = line; (p = strstr(p, pat)); p += kl) {
        if (p != line && p[-1] != ' ') continue;
        size_t o = 0;
        for (p += kl; *p && *p != '"' && o + 1 < n; p++) {
            if (p[0] == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
                char h[3] = { p[2], p[3], 0 };
                out[o++] = (char)strtol(h, NULL, 16);
                p += 3;
            } else out[o++] = *p;
        }
        out[o] = 0;
        return;
    }
}

/* File systems a desktop mounts, and the partitions it leaves alone: the
 * EFI system partition, Microsoft's reserved and recovery partitions, BIOS
 * and extended boot. */
static int mountable_fs(const char *fs)
{
    static const char *const ok[] = { "vfat", "msdos", "exfat", "ntfs", "ntfs3", "ext2", "ext3",
        "ext4", "xfs", "btrfs", "f2fs", "iso9660", "udf", "hfsplus", "hfs", "jfs", "nilfs2",
        "reiserfs", "bcachefs", NULL };
    for (int i = 0; ok[i]; i++) if (!strcmp(fs, ok[i])) return 1;
    return 0;
}

static int system_parttype(const char *t)
{
    static const char *const sys[] = { "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "0xef",
        "e3c9e316-0b5c-4db8-817d-f92df00215ae", "de94bba4-06d1-4d40-a16a-bfd50179d6ac", "0x27",
        "21686148-6449-6e6f-744e-656564454649", "bc13c2ff-59e6-4262-a352-b275fd6f7172", NULL };
    for (int i = 0; sys[i]; i++) if (!strcasecmp(t, sys[i])) return 1;
    return 0;
}

int w2k_fs_drives_all(W2kDrive *out, int max)
{
    int n = w2k_fs_drives(out, max);
    FILE *f = popen("lsblk -P -o PATH,TYPE,FSTYPE,LABEL,MOUNTPOINT,PARTTYPE,RM,HOTPLUG 2>/dev/null", "r");
    if (!f) return n;
    int mounted = n;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char path[128], type[16], fs[32], label[128], mnt[512], pt[48], rm[4], hp[4];
        lsblk_value(line, "PATH", path, sizeof path);
        lsblk_value(line, "TYPE", type, sizeof type);
        lsblk_value(line, "FSTYPE", fs, sizeof fs);
        lsblk_value(line, "LABEL", label, sizeof label);
        lsblk_value(line, "MOUNTPOINT", mnt, sizeof mnt);
        lsblk_value(line, "PARTTYPE", pt, sizeof pt);
        lsblk_value(line, "RM", rm, sizeof rm);
        lsblk_value(line, "HOTPLUG", hp, sizeof hp);
        int rom = !strcmp(type, "rom");
        int eject = rom || !strcmp(rm, "1") || !strcmp(hp, "1");
        if (mnt[0]) {
            for (int i = 0; i < mounted; i++) if (!strcmp(out[i].dev, path)) out[i].ejectable = eject;
            continue;
        }
        if (n >= max || !path[0] || !mountable_fs(fs) || system_parttype(pt)) continue;
        W2kDrive *d = &out[n];
        memset(d, 0, sizeof *d);
        snprintf(d->dev, sizeof d->dev, "%s", path);
        d->optical = rom || !strcmp(fs, "iso9660") || !strcmp(fs, "udf");
        d->removable = eject;
        d->ejectable = eject;
        snprintf(d->label, sizeof d->label, "%s", label[0] ? label : d->optical ? "CD Drive" :
                 d->removable ? "Removable Disk" : "Local Disk");
        d->letter = (char)('D' + n);
        n++;
    }
    pclose(f);
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
