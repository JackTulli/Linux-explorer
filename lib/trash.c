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
#include <fcntl.h>
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

/* mkdir -p: create `path` and every directory above it. The bin's own
 * folders are the owner's alone (the spec asks for it): what is in them
 * was deleted, and other users have no business listing its names. */
static void make_path(const char *path)
{
    char buf[1200];
    snprintf(buf, sizeof buf, "%s", path);
    size_t tl = strlen(w2k_trash_dir());
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, (size_t)(p - buf) >= tl ? 0700 : 0755);
        *p = '/';
    }
    mkdir(buf, 0700);
}

/* Path= is percent-encoded, as the spec has it: bytes outside a safe set
 * as %XX. Written raw, a file name with a newline in it could add a Path=
 * line of its own and choose where Restore puts the file; and items that
 * gio or trash-cli put in the bin came back as "My%20File.txt". */
static void path_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            strchr("/-._~!$&'()*+,;=:@", *p))
            out[o++] = (char)*p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void path_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        int a, b;
        if (*p == '%' && (a = hexval(p[1])) >= 0 && (b = hexval(p[2])) >= 0) {
            *o++ = (char)(a * 16 + b);
            p += 2;
        } else *o++ = *p;
    }
    *o = 0;
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

/* A copy made across file systems: created with the file's own mode, as
 * lib/fileops.c's copier does. fopen gave 0644 whatever it had been, so
 * a script came back from the bin not executable and a private file
 * readable by everyone. */
static FILE *create_like(const char *path, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode & 07777);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) { int e = errno; close(fd); unlink(path); errno = e; }
    return f;
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

    /* The name is claimed by creating its .trashinfo exclusively, as the
     * spec asks: two deletions of notes.txt at once cannot both take the
     * same slot. */
    char target[2400], meta[2400], name[512];
    int mfd = -1;
    for (int k = 1; k < 1000 && mfd < 0; k++) {
        if (k == 1) snprintf(name, sizeof name, "%.500s", base);
        else        snprintf(name, sizeof name, "%.480s.%d", base, k);
        snprintf(target, sizeof target, "%s/%s", files, name);
        snprintf(meta, sizeof meta, "%s/%s.trashinfo", info, name);
        struct stat probe;                  /* st stays the item's own */
        if (lstat(target, &probe) == 0) continue;
        mfd = open(meta, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (mfd < 0 && errno != EEXIST) return -1;
    }
    if (mfd < 0) return -1;

    /* An absolute original path, so Restore knows where to put it back. */
    char abs[2048], enc[6200];
    if (path[0] == '/') snprintf(abs, sizeof abs, "%s", path);
    else {
        char cwd[1024];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(abs, sizeof abs, "%s/%s", cwd, path);
    }
    path_encode(abs, enc, sizeof enc);
    {
        time_t now = time(NULL);
        struct tm tm;
        char when[32] = "";
        if (localtime_r(&now, &tm))
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
        char buf[6400];
        int len = snprintf(buf, sizeof buf, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, when);
        int wrote = len > 0 && write(mfd, buf, (size_t)len) == len;
        if (close(mfd) != 0 || !wrote) { unlink(meta); return -1; }
    }
    /* From here on a failure takes the claim back. */
    #define FAIL() do { int e_ = errno; unlink(meta); errno = e_; return -1; } while (0)

    if (rename(path, target) != 0) {
        /* rename() cannot cross a filesystem boundary, and the bin lives on
         * the home one. For a file, copy it over and unlink the original;
         * a directory from another filesystem is refused rather than
         * half-copied. */
        if (errno != EXDEV || !S_ISREG(st.st_mode)) FAIL();

        FILE *in = fopen(path, "rb");
        if (!in) FAIL();
        FILE *out = create_like(target, st.st_mode);
        if (!out) { fclose(in); FAIL(); }
        char buf[65536];
        size_t got;
        int ok = 1;
        while ((got = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, got, out) != got) { ok = 0; break; }
        if (ferror(in)) ok = 0;
        fclose(in);
        if (fclose(out) != 0) ok = 0;
        if (ok) chmod(target, st.st_mode & 07777);
        if (!ok) { unlink(target); FAIL(); }
        if (unlink(path) != 0) { unlink(target); FAIL(); }
    }
    #undef FAIL
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
        /* The first Path= of [Trash Info], decoded. */
        int in_info = 0;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '[') { in_info = !strcmp(line, "[Trash Info]"); continue; }
            if (in_info && !strncmp(line, "Path=", 5)) {
                snprintf(dest, sizeof dest, "%s", line + 5);
                path_decode(dest);
                break;
            }
        }
        fclose(f);
    }
    if (!dest[0] || dest[0] != '/') return -1;
    /* Something newer of the same name at the original place stays. */
    struct stat st;
    if (lstat(dest, &st) == 0) { errno = EEXIST; return -1; }
    if (rename(from, dest) != 0) {
        if (errno != EXDEV) return -1;
        /* Across file systems only a file is copied back; a folder used
         * to be "copied" as an empty file, its record deleted, and the
         * restore reported as done. */
        struct stat sf;
        if (lstat(from, &sf) != 0 || !S_ISREG(sf.st_mode)) { errno = EXDEV; return -1; }
        FILE *in = fopen(from, "rb");
        if (!in) return -1;
        FILE *out = create_like(dest, sf.st_mode);
        if (!out) { fclose(in); return -1; }
        char buf[65536];
        size_t got;
        int ok = 1;
        while ((got = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, got, out) != got) { ok = 0; break; }
        if (ferror(in)) ok = 0;
        fclose(in);
        if (fclose(out) != 0) ok = 0;
        if (ok) chmod(dest, sf.st_mode & 07777);
        if (!ok) { unlink(dest); return -1; }
        /* The bin's copy must go, or it stays there with no record once
         * the .trashinfo is deleted: back out rather than leave both. */
        if (unlink(from) != 0) { int e = errno; unlink(dest); errno = e; return -1; }
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
