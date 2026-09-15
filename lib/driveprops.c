/* driveprops.c -- a drive's property sheet, and Disk Cleanup.
 *
 * General, as Windows 2000 drew it: the drive's icon and label, its type
 * and file system, used and free space in bytes and in GB, the capacity,
 * the pie -- blue used, magenta free, tilted, with its rim in the darker
 * shades -- "Drive C" under it, and Disk Cleanup beside. Tools: a
 * read-only check of the file system. Hardware: the disk drives in the
 * computer. The label is changed through udisks, which asks for the
 * administrator where it must.
 *
 * Disk Cleanup offers what can go on that drive -- the Recycle Bin, the
 * picture thumbnails, the browsers' caches, week-old temporary files --
 * with what each would free, and removes what is ticked. Opened on its
 * own it asks which drive first, as cleanmgr did. */
#include "w2k.h"
#include "w2kui.h"
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void spawn(const char *cmd)
{
    pid_t p = fork();
    if (p < 0) return;
    if (p == 0) {
        if (fork() == 0) {
            signal(SIGPIPE, SIG_DFL);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(p, NULL, 0);
}

static int have_tool(const char *name)
{
    static const char *const dirs[] = { "/usr/sbin", "/sbin", "/usr/bin", "/bin", "/usr/local/bin", NULL };
    for (int i = 0; dirs[i]; i++) {
        char p[256];
        snprintf(p, sizeof p, "%s/%s", dirs[i], name);
        if (access(p, X_OK) == 0) return 1;
    }
    return 0;
}

static void with_commas(unsigned long long n, char *out, int len)
{
    char raw[32];
    snprintf(raw, sizeof raw, "%llu", n);
    int l = (int)strlen(raw), o = 0;
    for (int i = 0; i < l && o < len - 1; i++) {
        if (i && (l - i) % 3 == 0 && o < len - 1) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = 0;
}

/* "21.9 GB", "512 MB", as Windows put it beside the bytes. */
static void short_size(unsigned long long b, char *out, int n)
{
    double v = (double)b;
    if (v >= 1024.0 * 1024 * 1024 * 1024) snprintf(out, (size_t)n, "%.2f TB", v / (1024.0 * 1024 * 1024 * 1024));
    else if (v >= 1024.0 * 1024 * 1024) snprintf(out, (size_t)n, "%.2f GB", v / (1024.0 * 1024 * 1024));
    else if (v >= 1024.0 * 1024) snprintf(out, (size_t)n, "%.0f MB", v / (1024.0 * 1024));
    else if (v >= 1024.0) snprintf(out, (size_t)n, "%.0f KB", v / 1024.0);
    else snprintf(out, (size_t)n, "%llu bytes", b);
}

/* ------------------------------------------------------------------ *
 * What the drive is
 * ------------------------------------------------------------------ */
typedef struct {
    char path[512];                 /* where it is mounted */
    char dev[256];                  /* what is mounted there */
    char fstype[32];
    char label[128];
    char name[200];                 /* "Local Disk (C:)" */
    char letter;
    int  removable, optical, network;
    unsigned long long cap, freeb, used;
} DriveInfo;

/* /proc/mounts writes awkward bytes as \ooo. */
static void unoctal(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
            *o++ = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
            p += 3;
        } else *o++ = *p;
    }
    *o = 0;
}

/* The mount that holds `path`: its own, or the longest that contains it. */
static void find_mount(DriveInfo *d)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char line[1600];
    size_t best = 0;
    while (fgets(line, sizeof line, f)) {
        char dev[256], mnt[1024], fs[64];
        if (sscanf(line, "%255s %1023s %63s", dev, mnt, fs) != 3) continue;
        unoctal(mnt);
        size_t ml = strlen(mnt);
        int inside = !strcmp(mnt, "/") ||
                     (!strncmp(d->path, mnt, ml) && (d->path[ml] == '/' || d->path[ml] == 0));
        if (!inside || ml < best) continue;
        best = ml;
        snprintf(d->dev, sizeof d->dev, "%s", dev);
        snprintf(d->fstype, sizeof d->fstype, "%s", fs);
    }
    fclose(f);
}

/* The label, from /dev/disk/by-label (whose names write odd bytes as \xHH). */
static void find_label(DriveInfo *d)
{
    char want[PATH_MAX];
    if (!realpath(d->dev, want)) return;
    DIR *dp = opendir("/dev/disk/by-label");
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        char p[600], r[PATH_MAX];
        snprintf(p, sizeof p, "/dev/disk/by-label/%s", de->d_name);
        if (!realpath(p, r) || strcmp(r, want)) continue;
        int o = 0;
        for (const char *s = de->d_name; *s && o < (int)sizeof d->label - 1; s++) {
            if (s[0] == '\\' && s[1] == 'x' && isxdigit((unsigned char)s[2]) && isxdigit((unsigned char)s[3])) {
                char h[3] = { s[2], s[3], 0 };
                d->label[o++] = (char)strtol(h, NULL, 16);
                s += 3;
            } else d->label[o++] = *s;
        }
        d->label[o] = 0;
        break;
    }
    closedir(dp);
}

/* A stick or a card: the disk says it is removable, or sits on USB. */
static int is_removable(const char *dev)
{
    char real[PATH_MAX];
    if (strncmp(dev, "/dev/", 5) || !realpath(dev, real)) return 0;
    const char *base = strrchr(real, '/');
    base = base ? base + 1 : real;
    char sys[PATH_MAX], link[PATH_MAX];
    snprintf(sys, sizeof sys, "/sys/class/block/%s", base);
    if (!realpath(sys, link)) return 0;
    if (strstr(link, "/usb") || strstr(link, "/mmc")) return 1;
    char rm[PATH_MAX + 32], v[8] = "";
    snprintf(rm, sizeof rm, "%s/removable", link);
    FILE *f = fopen(rm, "r");
    if (!f) {                                   /* a partition: its disk's */
        snprintf(rm, sizeof rm, "%s/../removable", link);
        f = fopen(rm, "r");
    }
    if (f) { if (!fgets(v, sizeof v, f)) v[0] = 0; fclose(f); }
    return v[0] == '1';
}

static const char *fs_label(const char *fs)
{
    static const struct { const char *k, *v; } m[] = {
        { "ntfs", "NTFS" }, { "ntfs3", "NTFS" }, { "fuseblk", "NTFS" }, { "vfat", "FAT32" },
        { "msdos", "FAT" }, { "exfat", "exFAT" }, { "ext2", "ext2" }, { "ext3", "ext3" },
        { "ext4", "ext4" }, { "xfs", "XFS" }, { "btrfs", "Btrfs" }, { "f2fs", "F2FS" },
        { "iso9660", "CDFS" }, { "udf", "UDF" }, { "nfs", "NFS" }, { "nfs4", "NFS" },
        { "cifs", "SMB" }, { "smb3", "SMB" }, { "fuse.sshfs", "SSHFS" }, { "overlay", "OverlayFS" },
        { "zfs", "ZFS" }, { "bcachefs", "bcachefs" } };
    for (size_t i = 0; i < sizeof m / sizeof *m; i++) if (!strcmp(fs, m[i].k)) return m[i].v;
    return fs;
}

static void drive_info(DriveInfo *d, const char *path_in, const char *name_in, char letter)
{
    /* Taken before the memset: a caller re-reading a drive passes its own
     * d->path and d->name, which the memset used to wipe -- after Disk
     * Cleanup the sheet quietly became "/", and OK then relabelled the
     * root file system with the stick's name. */
    char path[sizeof d->path], name[sizeof d->name];
    snprintf(path, sizeof path, "%s", path_in && *path_in ? path_in : "/");
    snprintf(name, sizeof name, "%s", name_in ? name_in : "");
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    find_mount(d);
    find_label(d);
    d->optical = !strcmp(d->fstype, "iso9660") || !strcmp(d->fstype, "udf") || !strncmp(d->dev, "/dev/sr", 7);
    d->network = !strncmp(d->fstype, "nfs", 3) || !strcmp(d->fstype, "cifs") || !strcmp(d->fstype, "smb3") ||
                 !strcmp(d->fstype, "fuse.sshfs") || !strcmp(d->fstype, "9p");
    d->removable = !d->optical && !d->network && is_removable(d->dev);
    d->letter = letter ? letter : !strcmp(d->path, "/") ? 'C' : 0;
    if (name[0]) snprintf(d->name, sizeof d->name, "%s", name);
    else if (!strcmp(d->path, "/")) snprintf(d->name, sizeof d->name, "Local Disk (C:)");
    else {
        const char *b = strrchr(d->path, '/');
        snprintf(d->name, sizeof d->name, "%s", d->label[0] ? d->label : b && b[1] ? b + 1 : d->path);
    }
    struct statvfs sv;
    if (statvfs(d->path, &sv) == 0) {
        d->cap = (unsigned long long)sv.f_blocks * sv.f_frsize;
        d->freeb = (unsigned long long)sv.f_bavail * sv.f_frsize;
        d->used = d->cap > d->freeb ? d->cap - d->freeb : 0;
    }
}

static const char *drive_type(const DriveInfo *d)
{
    return d->network ? "Network Drive" : d->optical ? "CD Drive" :
           d->removable ? "Removable Disk" : "Local Disk";
}

static int drive_icon(const DriveInfo *d)
{
    return d->optical ? ICO_DRIVE_CD : d->removable ? ICO_DRIVE_FLOPPY :
           d->network ? ICO_NETWORK : ICO_DRIVE_HDD;
}

/* ------------------------------------------------------------------ *
 * Running something that takes a while, with the wait box up
 * ------------------------------------------------------------------ */
typedef struct {
    pid_t pid;
    int   fd, done, status, phase, len;
    char  out[6000];
    const char *text;
    W2kWin *w;
} Job;

static void job_read(Job *j)
{
    for (;;) {
        char buf[512];
        ssize_t n = read(j->fd, buf, sizeof buf);
        if (n <= 0) break;
        int room = (int)sizeof j->out - 1 - j->len;
        if (room > 0) { if (n > room) n = room; memcpy(j->out + j->len, buf, (size_t)n); j->len += (int)n; }
    }
    j->out[j->len] = 0;
}

static void job_tick(void *u)
{
    Job *j = u;
    job_read(j);
    int st;
    if (!j->done && waitpid(j->pid, &st, WNOHANG) == j->pid) {
        j->done = 1;
        j->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        job_read(j);
        w2k_win_close(j->w, ID_OK);
        return;
    }
    j->phase++;
    w2k_win_dirty(j->w);
}

static void job_paint(W2kWin *w, Drawable d)
{
    Job *j = w->user;
    w2k_bigicon_draw(d, 14, 14, ICO_DRIVE_HDD);
    w2k_text(d, F_UI, 58, 16, j->text, C_TEXT);
    w2k_text(d, F_UI, 58, 16 + w2k_font_height(F_UI) + 4, "Please wait...", C_GRAYTEXT);
    W2kRect pr = { 14, w->h - 34, w->w - 28, 18 };
    w2k_draw_progress(d, &pr, -1, j->phase);
}

static int job_event(W2kWin *w, XEvent *e) { (void)w; (void)e; return 1; }

/* argv run to its end with the box up; its output and status back. */
static int run_waiting(W2kWin *over, const char *text, char *const argv[], char *out, int n)
{
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[1], 1); dup2(p[1], 2);
        close(p[0]); close(p[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    Job j = { .pid = pid, .fd = p[0], .text = text };
    W2kWin *w = w2k_win_new("Disk", "w2kdialog", 340, 100, 0);
    j.w = w;
    w->user = &j;
    w->paint = job_paint;
    w->event = job_event;
    w2k_win_center(w, over);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);
    w2k_add_timer(120, job_tick, &j);
    w2k_win_modal(w);
    w2k_del_timer(job_tick, &j);
    if (!j.done) { int st; waitpid(pid, &st, 0); j.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128; job_read(&j); }
    close(p[0]);
    snprintf(out, (size_t)n, "%s", j.out);
    return j.status;
}

/* ------------------------------------------------------------------ *
 * Disk Cleanup
 * ------------------------------------------------------------------ */
enum { CL_INTERNET, CL_THUMBS, CL_TEMP, CL_BIN, NCLEAN };

typedef struct {
    const char *name, *about;
    int  on;                             /* ticked */
    int  avail;                          /* on this drive at all */
    unsigned long long bytes;
    char paths[16][1024];                /* what would go */
    int  npaths;
} CleanItem;

/* Bytes under a path, not following links, not leaving its device. */
static unsigned long long tree_bytes(const char *path, dev_t dev, int depth, long *budget)
{
    struct stat st;
    if (depth > 40 || --*budget < 0 || lstat(path, &st) != 0 || st.st_dev != dev) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_blocks * 512;
    unsigned long long sum = 0;
    DIR *dp = opendir(path);
    if (!dp) return 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[4096];
        snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
        sum += tree_bytes(sub, dev, depth + 1, budget);
    }
    closedir(dp);
    return sum;
}

static void add_path(CleanItem *c, const char *p, dev_t dev)
{
    struct stat st;
    if (c->npaths >= 16 || lstat(p, &st) != 0 || st.st_dev != dev) return;
    long budget = 400000;
    c->bytes += tree_bytes(p, dev, 0, &budget);
    snprintf(c->paths[c->npaths++], sizeof c->paths[0], "%s", p);
    c->avail = 1;
}

/* Every browser profile's cache folder under ~/.cache. */
static void add_browser_caches(CleanItem *c, const char *home, dev_t dev)
{
    char base[1024];
    snprintf(base, sizeof base, "%s/.cache/mozilla/firefox", home);
    DIR *dp = opendir(base);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (de->d_name[0] == '.') continue;
            char p[2200];
            snprintf(p, sizeof p, "%s/%s/cache2", base, de->d_name);
            add_path(c, p, dev);
        }
        closedir(dp);
    }
    static const char *const chromes[] = { "chromium", "google-chrome", "BraveSoftware/Brave-Browser",
                                           "microsoft-edge", "vivaldi", NULL };
    for (int i = 0; chromes[i]; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/.cache/%s/Default/Cache", home, chromes[i]);
        add_path(c, p, dev);
        snprintf(p, sizeof p, "%s/.cache/%s/Default/Code Cache", home, chromes[i]);
        add_path(c, p, dev);
    }
}

/* The user's own temporary files that nothing has touched for a week:
 * each file judged by itself -- read, written and changed a week ago --
 * and a folder only removed once that has left it empty. A folder's own
 * time only changes when entries come and go, so judging it by that
 * removed live ones whole: a tmux server's socket directory, an ssh
 * agent's, a build that had been running for days. Sockets, pipes and
 * links are never touched. `dry` only counts. */
static long long temp_prune(const char *path, time_t cutoff, dev_t dev, int depth, int dry)
{
    struct stat st;
    if (depth > 32 || lstat(path, &st) != 0 || st.st_dev != dev || st.st_uid != getuid())
        return 0;
    if (S_ISREG(st.st_mode)) {
        if (st.st_mtime >= cutoff || st.st_atime >= cutoff || st.st_ctime >= cutoff) return 0;
        if (!dry && unlink(path) != 0) return 0;
        return (long long)st.st_size;
    }
    if (!S_ISDIR(st.st_mode)) return 0;
    long long sum = 0;
    DIR *dp = opendir(path);
    if (!dp) return 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[4096];
        snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
        sum += temp_prune(sub, cutoff, dev, depth + 1, dry);
    }
    closedir(dp);
    if (!dry && depth > 0 && st.st_mtime < cutoff) rmdir(path);   /* only if empty now */
    return sum;
}

static void add_old_temp(CleanItem *c, dev_t dev)
{
    static const char *const dirs[] = { "/var/tmp", "/tmp", NULL };
    time_t week = time(NULL) - 7 * 24 * 3600;
    for (int i = 0; dirs[i] && c->npaths < 16; i++) {
        DIR *dp = opendir(dirs[i]);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp)) && c->npaths < 16) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char p[1200];
            snprintf(p, sizeof p, "%s/%s", dirs[i], de->d_name);
            struct stat st;
            if (lstat(p, &st) != 0 || st.st_uid != getuid() || st.st_dev != dev) continue;
            if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) continue;
            long long b = temp_prune(p, week, dev, 1, 1);
            if (b <= 0) continue;
            c->bytes += b;
            snprintf(c->paths[c->npaths++], sizeof c->paths[0], "%s", p);
            c->avail = 1;
        }
        closedir(dp);
    }
}

static void clean_measure(CleanItem *it, const char *drive_path)
{
    static const char *const names[NCLEAN] = { "Temporary Internet Files", "Thumbnails",
                                               "Temporary files", "Recycle Bin" };
    static const char *const about[NCLEAN] = {
        "The web browsers keep copies of the pages you have looked at, to show them faster the "
        "next time. The pages themselves and your settings stay.",
        "Pictures of your pictures, which Explorer and other programs keep to show folders "
        "quickly. They are made again when needed.",
        "Programs sometimes leave temporary files behind. Only yours that nothing has touched "
        "for a week are removed.",
        "The Recycle Bin holds files you have deleted. They are not removed from the disk "
        "until you empty it." };
    struct stat ds;
    memset(it, 0, sizeof(CleanItem) * NCLEAN);
    for (int i = 0; i < NCLEAN; i++) { it[i].name = names[i]; it[i].about = about[i]; }
    it[CL_INTERNET].on = it[CL_THUMBS].on = 1;          /* the Recycle Bin, as on Windows, not */
    if (stat(drive_path, &ds) != 0) return;
    const char *home = getenv("HOME");
    if (!home) return;
    add_browser_caches(&it[CL_INTERNET], home, ds.st_dev);
    char p[1100];
    snprintf(p, sizeof p, "%s/.cache/thumbnails", home);
    add_path(&it[CL_THUMBS], p, ds.st_dev);
    add_old_temp(&it[CL_TEMP], ds.st_dev);
    const char *bin = w2k_trash_files_dir();
    if (bin) add_path(&it[CL_BIN], bin, ds.st_dev);
}

typedef struct {
    W2kWin    *w;
    CleanItem  it[NCLEAN];
    int        rows[NCLEAN], nrows, sel;
    DriveInfo  d;
    W2kRect    list, ok, cancel;
    int        down;
} CleanDlg;

#define ROW_H 18

static unsigned long long clean_total(CleanDlg *c)
{
    unsigned long long t = 0;
    for (int i = 0; i < c->nrows; i++) if (c->it[c->rows[i]].on) t += c->it[c->rows[i]].bytes;
    return t;
}

static void clean_paint(W2kWin *w, Drawable d)
{
    CleanDlg *c = w->user;
    int fh = w2k_font_height(F_UI);
    char buf[400], sz[40];
    w2k_bigicon_draw(d, 14, 14, drive_icon(&c->d));
    unsigned long long all = 0;
    for (int i = 0; i < c->nrows; i++) all += c->it[c->rows[i]].bytes;
    short_size(all, sz, sizeof sz);
    snprintf(buf, sizeof buf, "You can use Disk Cleanup to free up to %s of disk space", sz);
    w2k_text(d, F_UI, 58, 16, buf, C_TEXT);
    snprintf(buf, sizeof buf, "on %s.", c->d.name);
    w2k_text(d, F_UI, 58, 16 + fh, buf, C_TEXT);
    w2k_text_mnemonic(d, F_UI, 14, c->list.y - fh - 4, "&Files to delete:", C_TEXT, 1);
    w2k_draw_well(d, &c->list);
    for (int i = 0; i < c->nrows; i++) {
        CleanItem *it = &c->it[c->rows[i]];
        int y = c->list.y + 2 + i * ROW_H;
        if (i == c->sel) w2k_fill(d, c->list.x + 22, y, c->list.w - 24, ROW_H, C_HIGHLIGHT);
        w2k_draw_checkbox(d, c->list.x + 5, y + 2, "", it->on, 0, 0);
        w2k_text(d, F_UI, c->list.x + 26, y + (ROW_H - fh) / 2, it->name,
                 i == c->sel ? C_HIGHLIGHTTEXT : C_WINDOWTEXT);
        short_size(it->bytes, sz, sizeof sz);
        w2k_text(d, F_UI, c->list.x + c->list.w - 8 - w2k_text_width(F_UI, sz, -1), y + (ROW_H - fh) / 2,
                 sz, i == c->sel ? C_HIGHLIGHTTEXT : C_WINDOWTEXT);
    }
    if (!c->nrows) w2k_text(d, F_UI, c->list.x + 8, c->list.y + 6, "Nothing on this drive can be cleaned up.", C_GRAYTEXT);
    short_size(clean_total(c), sz, sizeof sz);
    snprintf(buf, sizeof buf, "Total amount of disk space you gain:  %s", sz);
    int y = c->list.y + c->list.h + 8;
    w2k_text(d, F_UI, 14, y, buf, C_TEXT);
    W2kRect g = { 14, y + fh + 8, w->w - 28, c->ok.y - (y + fh + 8) - 12 };
    w2k_draw_groupbox(d, &g, "Description");
    if (c->sel >= 0 && c->sel < c->nrows) {
        char lines[5][96];
        const char *t = c->it[c->rows[c->sel]].about;
        int nl = 0, gw = g.w - 24;
        while (*t && nl < 5) {                 /* words, wrapped to the box */
            int fit = 0, sp = -1;
            for (int i = 0; t[i] && i < 95; i++) {
                if (t[i] == ' ') sp = i;
                if (w2k_text_width(F_UI, t, i + 1) > gw) break;
                fit = i + 1;
            }
            int cut = t[fit] && sp > 0 ? sp : fit;
            if (cut <= 0) break;
            snprintf(lines[nl++], 96, "%.*s", cut, t);
            t += cut;
            while (*t == ' ') t++;
        }
        for (int i = 0; i < nl; i++) w2k_text(d, F_UI, g.x + 12, g.y + 20 + i * fh, lines[i], C_TEXT);
    }
    w2k_draw_pushbutton(d, &c->ok, "OK", BS_DEFAULT | (c->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &c->cancel, "Cancel", c->down == 2 ? BS_PRESSED : 0);
}

static int clean_event(W2kWin *w, XEvent *e)
{
    CleanDlg *c = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_rect_hit(&c->list, x, y)) {
            int i = (y - c->list.y - 2) / ROW_H;
            if (i >= 0 && i < c->nrows) {
                c->sel = i;
                if (x < c->list.x + 22) c->it[c->rows[i]].on = !c->it[c->rows[i]].on;
            }
        } else if (w2k_rect_hit(&c->ok, x, y)) c->down = 1;
        else if (w2k_rect_hit(&c->cancel, x, y)) c->down = 2;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = c->down;
        c->down = 0;
        if (b == 1 && w2k_rect_hit(&c->ok, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_OK);
        if (b == 2 && w2k_rect_hit(&c->cancel, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { w2k_win_close(w, ID_OK); return 1; }
        if (ks == XK_Up && c->sel > 0) c->sel--;
        else if (ks == XK_Down && c->sel + 1 < c->nrows) c->sel++;
        else if (ks == XK_space && c->sel >= 0 && c->sel < c->nrows)
            c->it[c->rows[c->sel]].on = !c->it[c->rows[c->sel]].on;
        w2k_win_dirty(w);
        return 1;
    }
    }
    return 0;
}

/* "Disk Cleanup is calculating...": up while the caches and the bin are
 * measured, which can take a while on a cold disk or a full bin -- the
 * window used to appear only after it all, with nothing meanwhile. */
static void calc_paint(W2kWin *w, Drawable d)
{
    const char *name = w->user;
    w2k_fill(d, 0, 0, w->w, w->h, C_FACE);
    w2k_bigicon_draw(d, 14, 14, ICO_DRIVE_HDD);
    char t[300];
    snprintf(t, sizeof t, "Disk Cleanup is calculating how much space you will be able "
             "to free on %s. This may take a few minutes to complete.", name ? name : "the drive");
    w2k_text_wrapped(d, F_UI, 58, 16, w->w - 72, t, C_TEXT);
}

static void clean_drive(W2kWin *over, const DriveInfo *d)
{
    static CleanDlg c;
    memset(&c, 0, sizeof c);
    c.d = *d;
    W2kWin *calc = w2k_win_new("Disk Cleanup", "w2kdialog", 360, 76, 0);
    calc->user = (void *)d->name;
    calc->paint = calc_paint;
    w2k_win_center(calc, over);
    if (over) XSetTransientForHint(w2k.dpy, calc->win, over->win);
    w2k_win_show_now(calc);
    clean_measure(c.it, d->path);
    w2k_win_destroy(calc);
    for (int i = 0; i < NCLEAN; i++) if (c.it[i].avail) c.rows[c.nrows++] = i;
    int W = 400, H = 400;
    char title[260];
    snprintf(title, sizeof title, "Disk Cleanup for %s", d->name);
    W2kWin *w = w2k_win_new(title, "w2kdialog", W, H, 0);
    c.w = w;
    w->user = &c;
    w->paint = clean_paint;
    w->event = clean_event;
    c.list = (W2kRect){ 14, 80, W - 28, NCLEAN * ROW_H + 4 };
    c.ok = (W2kRect){ W - 14 - 75 * 2 - 6, H - 12 - 23, 75, 23 };
    c.cancel = (W2kRect){ W - 14 - 75, H - 12 - 23, 75, 23 };
    w2k_win_center(w, over);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);
    if (w2k_win_modal(w) != ID_OK || !clean_total(&c)) return;
    if (w2k_msgbox(over, "Disk Cleanup", "Are you sure you want to perform these actions?",
                   MB_YESNO | MB_ICONQUESTION) != ID_YES) return;
    for (int i = 0; i < c.nrows; i++) {
        CleanItem *it = &c.it[c.rows[i]];
        if (!it->on) continue;
        if (c.rows[i] == CL_BIN) { w2k_trash_empty(); continue; }
        if (c.rows[i] == CL_TEMP) {
            /* File by file, by the same rule the count used. */
            time_t week = time(NULL) - 7 * 24 * 3600;
            struct stat ds;
            for (int k = 0; k < it->npaths; k++)
                if (lstat(it->paths[k], &ds) == 0)
                    temp_prune(it->paths[k], week, ds.st_dev, 1, 0);
            continue;
        }
        for (int k = 0; k < it->npaths; k++) {
            struct stat st;
            if (lstat(it->paths[k], &st) != 0) continue;
            if (S_ISDIR(st.st_mode) && c.rows[i] != CL_TEMP) {
                /* A cache folder is emptied, not removed: its program
                 * expects to find it. */
                DIR *dp = opendir(it->paths[k]);
                if (!dp) continue;
                struct dirent *de;
                while ((de = readdir(dp))) {
                    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                    char sub[2200];
                    snprintf(sub, sizeof sub, "%s/%s", it->paths[k], de->d_name);
                    w2k_fs_remove_tree(sub);
                }
                closedir(dp);
            } else w2k_fs_remove_tree(it->paths[k]);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Select Drive: cleanmgr's first question
 * ------------------------------------------------------------------ */
typedef struct { W2kCombo *combo; W2kRect ok, cancel; int down; } PickDlg;

static void pick_paint(W2kWin *w, Drawable d)
{
    PickDlg *p = w->user;
    (void)w;
    w2k_bigicon_draw(d, 14, 14, ICO_DRIVE_HDD);
    w2k_text(d, F_UI, 58, 18, "Select the drive you want to clean up.", C_TEXT);
    w2k_text_mnemonic(d, F_UI, 58, 46, "&Drives:", C_TEXT, 1);
    w2k_combo_draw(d, p->combo);
    w2k_draw_pushbutton(d, &p->ok, "OK", BS_DEFAULT | (p->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &p->cancel, "E&xit", p->down == 2 ? BS_PRESSED : 0);
}

static int pick_event(W2kWin *w, XEvent *e)
{
    PickDlg *p = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_combo_press(p->combo, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&p->ok, e->xbutton.x, e->xbutton.y)) p->down = 1;
        else if (w2k_rect_hit(&p->cancel, e->xbutton.x, e->xbutton.y)) p->down = 2;
        w2k_win_dirty(w);
        return 1;
    case ButtonRelease: {
        int b = p->down;
        p->down = 0;
        if (b == 1 && w2k_rect_hit(&p->ok, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_OK);
        if (b == 2 && w2k_rect_hit(&p->cancel, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (w2k_combo_key(p->combo, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { w2k_win_close(w, ID_OK); return 1; }
        return 1;
    }
    }
    return 0;
}

void w2k_disk_cleanup(W2kWin *over, const char *path, const char *name)
{
    DriveInfo d;
    if (path && *path) { drive_info(&d, path, name, 0); clean_drive(over, &d); return; }
    /* No drive given: C: and whatever is mounted, to choose from. */
    W2kDrive dr[16];
    int nd = w2k_fs_drives(dr, 16);
    PickDlg p = { 0 };
    p.combo = w2k_combo_new(0);
    w2k_combo_add(p.combo, "Local Disk (C:)");
    char names[17][160];
    for (int i = 0; i < nd; i++) {
        snprintf(names[i + 1], sizeof names[0], "%.120s (%c:)", dr[i].label, dr[i].letter);
        w2k_combo_add(p.combo, names[i + 1]);
    }
    p.combo->sel = 0;
    int W = 330, H = 130;
    W2kWin *w = w2k_win_new("Disk Cleanup : Drive Selection", "w2kdialog", W, H, 0);
    w->user = &p;
    w->paint = pick_paint;
    w->event = pick_event;
    p.combo->r = (W2kRect){ 58, 62, W - 58 - 14, 21 };
    p.ok = (W2kRect){ W - 14 - 75 * 2 - 6, H - 12 - 23, 75, 23 };
    p.cancel = (W2kRect){ W - 14 - 75, H - 12 - 23, 75, 23 };
    w2k_win_center(w, over);
    int r = w2k_win_modal(w);
    int sel = p.combo->sel;
    w2k_combo_free(p.combo);
    if (r != ID_OK) return;
    if (sel <= 0) drive_info(&d, "/", "Local Disk (C:)", 'C');
    else drive_info(&d, dr[sel - 1].path, names[sel], dr[sel - 1].letter);
    clean_drive(over, &d);
}

/* ------------------------------------------------------------------ *
 * The property sheet
 * ------------------------------------------------------------------ */
enum { DB_OK = 1, DB_CANCEL, DB_APPLY, DB_CLEANUP, DB_CHECK, DB_BACKUP, DB_DEFRAG,
       DB_HWPROPS, DB_TROUBLE, NDB };

typedef struct {
    W2kWin   *w;
    W2kTabs  *tabs;
    W2kEdit  *label;
    W2kList  *disks;
    W2kRect   b[NDB];
    int       down;
    DriveInfo d;
    int       can_label;
    const char *checker;             /* the read-only check for this file system */
} DriveDlg;

/* The pie: an ellipse `w` by `h`, `depth` thick, the used share in blue
 * from nine o'clock going down, as Windows drew it, the rest magenta;
 * the rim is each colour's darker shade, seen where the pie faces us. */
static void draw_pie(Drawable d, int cx, int cy, int w, int h, int depth, double used)
{
    double a0 = 180.0, a1 = 180.0 + 360.0 * used;             /* the blue arc, in degrees */
    double rx = w / 2.0, ry = h / 2.0;
    for (int x = (int)(cx - rx); x <= (int)(cx + rx); x++) {
        double dx = (x - cx) / rx;
        if (dx < -1 || dx > 1) continue;
        double dy = sqrt(1 - dx * dx);
        int yb = (int)(cy + ry * dy);
        double ang = atan2(-dy, dx) * 180.0 / M_PI;           /* below the middle: 180..360 */
        if (ang < 0) ang += 360.0;
        int blue = used > 0 && ((ang >= a0 && ang <= a1) || (a1 > 360.0 && ang <= a1 - 360.0));
        if (blue) w2k_fill_rgb(d, x, yb, 1, depth, 0, 0, 128);
        else      w2k_fill_rgb(d, x, yb, 1, depth, 128, 0, 128);
    }
    int X = w2k_cx(cx - w / 2), Y = w2k_cx(cy - h / 2);
    unsigned W = (unsigned)w2k_cw(cx - w / 2, w), H = (unsigned)w2k_cw(cy - h / 2, h);
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(255, 0, 255));
    XFillArc(w2k.dpy, d, w2k.gc, X, Y, W, H, 0, 360 * 64);
    if (used > 0) {
        /* X measures angles anticlockwise from three o'clock: from nine
         * o'clock that is downwards, the way the rim above was coloured. */
        int ext = (int)(360.0 * 64 * used);
        if (ext < 64) ext = 64;                               /* a sliver still shows */
        XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(0, 0, 255));
        XFillArc(w2k.dpy, d, w2k.gc, X, Y, W, H, 180 * 64, ext);
    }
}

static void etched(Drawable d, int x, int y, int w)
{
    w2k_hline(d, x, y, w, C_SHADOW);
    w2k_hline(d, x, y + 1, w, C_HILIGHT);
}

static void swatch(Drawable d, int x, int y, int r, int g, int b)
{
    w2k_fill_rgb(d, x, y, 14, 14, r, g, b);
    w2k_frame(d, x, y, 14, 14, C_BLACK);
}

static void right_text(Drawable d, int xr, int y, const char *t)
{
    w2k_text(d, F_UI, xr - w2k_text_width(F_UI, t, -1), y, t, C_TEXT);
}

static int db_enabled(DriveDlg *s, int b)
{
    switch (b) {
    case DB_OK: case DB_CANCEL: case DB_CLEANUP: case DB_HWPROPS: return 1;
    case DB_APPLY: return s->can_label && strcmp(w2k_edit_text(s->label), s->d.label);
    case DB_CHECK: return s->checker != NULL;
    }
    return 0;
}

static int db_page(int b)
{
    switch (b) {
    case DB_CLEANUP: return 0;
    case DB_CHECK: case DB_BACKUP: case DB_DEFRAG: return 1;
    case DB_HWPROPS: case DB_TROUBLE: return 2;
    }
    return -1;                                            /* every page */
}

static void dp_paint(W2kWin *w, Drawable d)
{
    DriveDlg *s = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_tabs_draw(d, s->tabs);
    W2kRect c = w2k_tabs_client(s->tabs);
    int page = s->tabs->sel;
    char buf[160], sz[40];

    if (page == 0) {
        int x = c.x + 12, y = c.y + 14, vx = c.x + 136, rx = c.x + c.w - 14;
        w2k_bigicon_draw(d, x, y, drive_icon(&s->d));
        w2k_text(d, F_UI, c.x + 60, s->label->r.y + (21 - fh) / 2, "Label:", C_TEXT);
        w2k_edit_draw(d, s->label);
        y = c.y + 50;
        etched(d, x, y, c.w - 24);                          y += 10;
        w2k_text(d, F_UI, c.x + 60, y, "Type:", C_TEXT);
        w2k_text(d, F_UI, vx, y, drive_type(&s->d), C_TEXT);                  y += fh + 6;
        w2k_text(d, F_UI, c.x + 60, y, "File system:", C_TEXT);
        w2k_text(d, F_UI, vx, y, fs_label(s->d.fstype), C_TEXT);              y += fh + 8;
        etched(d, x, y, c.w - 24);                          y += 10;
        swatch(d, x + 2, y, 0, 0, 255);
        w2k_text(d, F_UI, c.x + 36, y, "Used space:", C_TEXT);
        with_commas(s->d.used, sz, sizeof sz);
        snprintf(buf, sizeof buf, "%s bytes", sz);
        right_text(d, rx - 70, y, buf);
        short_size(s->d.used, sz, sizeof sz);
        right_text(d, rx, y, sz);                          y += fh + 10;
        swatch(d, x + 2, y, 255, 0, 255);
        w2k_text(d, F_UI, c.x + 36, y, "Free space:", C_TEXT);
        with_commas(s->d.freeb, sz, sizeof sz);
        snprintf(buf, sizeof buf, "%s bytes", sz);
        right_text(d, rx - 70, y, buf);
        short_size(s->d.freeb, sz, sizeof sz);
        right_text(d, rx, y, sz);                          y += fh + 8;
        etched(d, x, y, c.w - 24);                          y += 10;
        w2k_text(d, F_UI, c.x + 36, y, "Capacity:", C_TEXT);
        with_commas(s->d.cap, sz, sizeof sz);
        snprintf(buf, sizeof buf, "%s bytes", sz);
        right_text(d, rx - 70, y, buf);
        short_size(s->d.cap, sz, sizeof sz);
        right_text(d, rx, y, sz);                          y += fh + 12;
        /* The pie, and under it the drive's letter. */
        int pw = 120, ph = 58, depth = 12, pcx = c.x + c.w / 2 - 20, pcy = y + ph / 2;
        draw_pie(d, pcx, pcy, pw, ph, depth, s->d.cap ? (double)s->d.used / (double)s->d.cap : 0);
        if (s->d.letter) snprintf(buf, sizeof buf, "Drive %c", s->d.letter);
        else snprintf(buf, sizeof buf, "%s", s->d.name);
        w2k_text(d, F_UI, pcx - w2k_text_width(F_UI, buf, -1) / 2, pcy + ph / 2 + depth + 6, buf, C_TEXT);
        y = s->b[DB_CLEANUP].y + 23 + 10;
        etched(d, x, y, c.w - 24);                          y += 10;
        w2k_draw_checkbox(d, x, y, "Compress drive to save disk space", 0, 0, 1);            y += 22;
        w2k_draw_checkbox(d, x, y, "Allow Indexing Service to index this disk for fast file searching", 0, 0, 1);
    } else if (page == 1) {
        static const char *const title[3] = { "Error-checking", "Backup", "Defragmentation" };
        static const char *const text[3] = { "This option will check the volume for errors.",
            "This option will back up files on the volume.", "This option will defragment files on the volume." };
        static const int icon[3] = { ICO_DRIVE_HDD, ICO_DOCUMENTS, ICO_SETTINGS };
        for (int i = 0; i < 3; i++) {
            W2kRect g = { c.x + 8, c.y + 10 + i * 96, c.w - 16, 88 };
            w2k_draw_groupbox(d, &g, title[i]);
            w2k_bigicon_draw(d, g.x + 12, g.y + 20, icon[i]);
            w2k_text(d, F_UI, g.x + 56, g.y + 24, text[i], C_TEXT);
        }
        const char *why = s->checker ? "The check only reads the disk: it reports what is wrong and changes nothing."
                                     : "This file system cannot be checked from here.";
        w2k_text(d, F_UI, c.x + 10, c.y + 10 + 3 * 96 + 4, why, C_GRAYTEXT);
        w2k_text(d, F_UI, c.x + 10, c.y + 10 + 3 * 96 + 4 + fh,
                 "Linux file systems do not need defragmenting.", C_GRAYTEXT);
    } else {
        w2k_text(d, F_UI, c.x + 10, c.y + 10, "All disk drives:", C_TEXT);
        w2k_list_draw(d, s->disks);
        W2kRect g = { c.x + 8, s->disks->r.y + s->disks->r.h + 10, c.w - 16, 84 };
        w2k_draw_groupbox(d, &g, "Device Properties");
        const char *nm = s->disks->sel >= 0 && s->disks->sel < s->disks->n ? s->disks->items[s->disks->sel].text[0] : "";
        const char *loc = s->disks->sel >= 0 && s->disks->sel < s->disks->n ? s->disks->items[s->disks->sel].text[1] : "";
        snprintf(buf, sizeof buf, "Name:  %s", nm);
        w2k_text(d, F_UI, g.x + 12, g.y + 20, buf, C_TEXT);
        snprintf(buf, sizeof buf, "Type:  %s", loc);
        w2k_text(d, F_UI, g.x + 12, g.y + 20 + fh + 2, buf, C_TEXT);
        w2k_text(d, F_UI, g.x + 12, g.y + 20 + 2 * (fh + 2), "Device status:  This device is working properly.", C_TEXT);
    }
    for (int b = 1; b < NDB; b++) {
        int pg = db_page(b);
        if (pg >= 0 && pg != page) continue;
        static const char *const lbl[NDB] = { [DB_OK] = "OK", [DB_CANCEL] = "Cancel", [DB_APPLY] = "&Apply",
            [DB_CLEANUP] = "Disk &Cleanup...", [DB_CHECK] = "&Check Now...", [DB_BACKUP] = "&Backup Now...",
            [DB_DEFRAG] = "&Defragment Now...", [DB_HWPROPS] = "P&roperties", [DB_TROUBLE] = "&Troubleshoot..." };
        int st = (b == DB_OK ? BS_DEFAULT : 0) | (s->down == b ? BS_PRESSED : 0) | (db_enabled(s, b) ? 0 : BS_DISABLED);
        w2k_draw_pushbutton(d, &s->b[b], lbl[b], st);
    }
}

/* udisks' object for a block device: its kernel name, with anything but
 * letters and digits written as _xx. */
static void udisks_path(const char *dev, char *out, int n)
{
    char real[PATH_MAX];
    const char *base = realpath(dev, real) ? strrchr(real, '/') : NULL;
    base = base ? base + 1 : dev;
    int o = snprintf(out, (size_t)n, "/org/freedesktop/UDisks2/block_devices/");
    for (const char *p = base; *p && o < n - 4; p++) {
        if (isalnum((unsigned char)*p)) out[o++] = *p;
        else o += snprintf(out + o, (size_t)(n - o), "_%02x", (unsigned char)*p);
    }
    out[o] = 0;
}

/* The new label, through udisks: 0 when it took, else its words in err. */
static int set_label(DriveDlg *s, const char *label, char *err, int n)
{
    char obj[300], lit[300];
    udisks_path(s->d.dev, obj, sizeof obj);
    int o = 0;
    lit[o++] = '\'';
    for (const char *p = label; *p && o < (int)sizeof lit - 4; p++) {
        if (*p == '\'' || *p == '\\') lit[o++] = '\\';
        lit[o++] = *p;
    }
    lit[o++] = '\'';
    lit[o] = 0;
    /* Time enough for the password prompt: gdbus's own 25 seconds used to
     * report a failure while the label went on to be changed anyway. */
    char *const av[] = { "gdbus", "call", "--system", "--timeout", "300",
                         "--dest", "org.freedesktop.UDisks2",
                         "--object-path", obj, "--method", "org.freedesktop.UDisks2.Filesystem.SetLabel",
                         lit, "{}", NULL };
    int st = run_waiting(s->w, "Changing the label...", av, err, n);
    return st;
}

static int dp_commit(DriveDlg *s)
{
    const char *want = w2k_edit_text(s->label);
    if (!s->can_label || !strcmp(want, s->d.label)) return 1;
    char err[2000];
    if (set_label(s, want, err, sizeof err) != 0) {
        char msg[2300];
        const char *why = strstr(err, "NotAuthorized") ? "You are not allowed to do this, or the password was not given." : err;
        snprintf(msg, sizeof msg, "The label could not be changed.\n\n%.2000s", why);
        w2k_msgbox(s->w, "Local Disk", msg, MB_OK | MB_ICONERROR);
        return 0;
    }
    snprintf(s->d.label, sizeof s->d.label, "%s", want);
    return 1;
}

static void dp_check(DriveDlg *s)
{
    char *av[8];
    int n = 0;
    av[n++] = "pkexec";
    if (!strcmp(s->checker, "e2fsck")) { av[n++] = "e2fsck"; av[n++] = "-f"; av[n++] = "-n"; }
    else if (!strcmp(s->checker, "ntfsfix")) { av[n++] = "ntfsfix"; av[n++] = "-n"; }
    else { av[n++] = (char *)s->checker; av[n++] = "-n"; }
    av[n++] = s->d.dev;
    av[n] = NULL;
    if (geteuid() == 0) { memmove(av, av + 1, sizeof *av * (size_t)n); }
    static char out[6000];
    int st = run_waiting(s->w, "Checking the disk...", av, out, sizeof out);
    char msg[6300];
    if (st == 126 || st == 127)
        snprintf(msg, sizeof msg, "The disk was not checked: it was not authorized, or pkexec is not installed.");
    else {
        /* e2fsck and fsck.fat say 0 for clean and 4 for errors left
         * unfixed -- which with -n is any error at all. */
        int len = (int)strlen(out);
        const char *tail = len > 1500 ? out + len - 1500 : out;
        snprintf(msg, sizeof msg, "%s\n\n%s", st == 0 ? "The disk check is complete. No errors were found."
                 : "The disk check found problems. Nothing was changed; Disk Management or fsck as the "
                   "administrator, with the drive unmounted, can repair them. (The drive is in use: "
                   "a check made while it is being written to can report changes in progress as "
                   "problems. Check it again unmounted to be sure.)", tail);
    }
    w2k_msgbox(s->w, "Checking Disk", msg, MB_OK | (st == 0 ? MB_ICONINFO : MB_ICONWARNING));
}

static void dp_button(DriveDlg *s, int b)
{
    switch (b) {
    case DB_OK:      if (dp_commit(s)) w2k_win_close(s->w, ID_OK); break;
    case DB_CANCEL:  w2k_win_close(s->w, ID_CANCEL); break;
    case DB_APPLY:   dp_commit(s); break;
    case DB_CLEANUP:
        clean_drive(s->w, &s->d);
        drive_info(&s->d, s->d.path, s->d.name, s->d.letter);    /* the figures after */
        break;
    case DB_CHECK:   dp_check(s); break;
    case DB_HWPROPS: spawn("l2kdevmgmt"); break;
    }
}

static void blink_cb(void *v) { w2k_edit_blink(v); }

static int dp_event(W2kWin *w, XEvent *e)
{
    DriveDlg *s = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_tabs_press(s->tabs, &e->xbutton)) { s->label->focused = s->tabs->sel == 0; w2k_win_dirty(w); return 1; }
        if (s->tabs->sel == 0 && s->can_label && w2k_edit_press(s->label, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (s->tabs->sel == 2 && w2k_list_press(s->disks, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        for (int b = 1; b < NDB; b++) {
            int pg = db_page(b);
            if ((pg < 0 || pg == s->tabs->sel) && db_enabled(s, b) && w2k_rect_hit(&s->b[b], x, y)) s->down = b;
        }
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (w2k_edit_motion(s->label, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        w2k_edit_release(s->label);
        w2k_list_release(s->disks, &e->xbutton);
        int b = s->down;
        s->down = 0;
        if (b && w2k_rect_hit(&s->b[b], e->xbutton.x, e->xbutton.y)) dp_button(s, b);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { dp_button(s, DB_OK); return 1; }
        if (w2k_tabs_key(s->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (s->tabs->sel == 0 && s->can_label && w2k_edit_key(s->label, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (s->tabs->sel == 2 && w2k_list_key(s->disks, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

/* The disks in the computer, from sysfs: model and maker, and the kind. */
static void fill_disks(W2kList *l)
{
    DIR *dp = opendir("/sys/block");
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        const char *n = de->d_name;
        if (n[0] == '.' || !strncmp(n, "loop", 4) || !strncmp(n, "ram", 3) ||
            !strncmp(n, "zram", 4) || !strncmp(n, "dm-", 3) || !strncmp(n, "md", 2)) continue;
        char p[512], model[128] = "", vendor[64] = "";
        snprintf(p, sizeof p, "/sys/block/%s/device/model", n);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(model, sizeof model, f)) model[strcspn(model, "\r\n")] = 0; fclose(f); }
        snprintf(p, sizeof p, "/sys/block/%s/device/vendor", n);
        f = fopen(p, "r");
        if (f) { if (fgets(vendor, sizeof vendor, f)) vendor[strcspn(vendor, "\r\n")] = 0; fclose(f); }
        for (char *s = model + strlen(model); s > model && s[-1] == ' '; ) *--s = 0;
        for (char *s = vendor + strlen(vendor); s > vendor && s[-1] == ' '; ) *--s = 0;
        char name[200];
        snprintf(name, sizeof name, "%s%s%s", vendor, vendor[0] && model[0] ? " " : "", model[0] ? model : n);
        int r = w2k_list_add(l, !strncmp(n, "sr", 2) ? ICO_DRIVE_CD : ICO_DRIVE_HDD, NULL);
        w2k_list_set(l, r, 0, name);
        w2k_list_set(l, r, 1, !strncmp(n, "sr", 2) ? "DVD/CD-ROM drives" : "Disk drives");
    }
    closedir(dp);
    if (l->n) { l->sel = 0; l->items[0].selected = 1; }
}

int w2k_drive_properties(W2kWin *over, const char *path, const char *name, char letter)
{
    static DriveDlg s;
    memset(&s, 0, sizeof s);
    drive_info(&s.d, path, name, letter);
    /* A label goes through udisks, to a block device's file system. */
    s.can_label = !s.d.network && !s.d.optical && !strncmp(s.d.dev, "/dev/", 5) && have_tool("gdbus");
    const char *fs = s.d.fstype;
    s.checker = !strncmp(fs, "ext", 3) && have_tool("e2fsck") ? "e2fsck" :
                !strcmp(fs, "vfat") && have_tool("fsck.vfat") ? "fsck.vfat" :
                !strcmp(fs, "exfat") && have_tool("fsck.exfat") ? "fsck.exfat" :
                (!strcmp(fs, "ntfs") || !strcmp(fs, "ntfs3") || !strcmp(fs, "fuseblk")) && have_tool("ntfsfix") ? "ntfsfix" : NULL;

    int W = 360, H = 452;
    char title[260];
    snprintf(title, sizeof title, "%s Properties", s.d.name);
    W2kWin *w = w2k_win_new(title, "w2kdialog", W, H, 0);
    s.w = w;
    w->user = &s;
    w->paint = dp_paint;
    w->event = dp_event;
    s.tabs = w2k_tabs_new(&s, NULL);
    w2k_tabs_add(s.tabs, "General");
    w2k_tabs_add(s.tabs, "Tools");
    w2k_tabs_add(s.tabs, "Hardware");
    s.tabs->r = (W2kRect){ 6, 6, W - 12, H - 6 - 40 };
    W2kRect c = w2k_tabs_client(s.tabs);

    s.label = w2k_edit_new(0);
    w2k_edit_bind(s.label, w);
    w2k_edit_set(s.label, s.d.label);
    s.label->r = (W2kRect){ c.x + 136, c.y + 16, 140, 21 };
    s.label->focused = s.can_label;
    s.label->readonly = !s.can_label;

    s.b[DB_OK]     = (W2kRect){ W - 8 - 75 * 3 - 12, H - 8 - 23, 75, 23 };
    s.b[DB_CANCEL] = (W2kRect){ W - 8 - 75 * 2 - 6, H - 8 - 23, 75, 23 };
    s.b[DB_APPLY]  = (W2kRect){ W - 8 - 75, H - 8 - 23, 75, 23 };
    /* General: Disk Cleanup to the right of the pie's foot. */
    int fh = w2k_font_height(F_UI);
    int pie_top = c.y + 50 + 10 + (fh + 6) + (fh + 8) + 10 + (fh + 10) + (fh + 8) + 10 + (fh + 12);
    s.b[DB_CLEANUP] = (W2kRect){ c.x + c.w - 14 - 96, pie_top + 58 + 12 - 23 + 16, 96, 23 };
    for (int i = 0; i < 3; i++)
        s.b[DB_CHECK + i] = (W2kRect){ c.x + c.w - 20 - 110, c.y + 10 + i * 96 + 88 - 32, 110, 23 };
    s.disks = w2k_list_new(LV_REPORT);
    s.disks->fullrow = 1;
    w2k_scroll_bind(&s.disks->vsb, w);
    w2k_scroll_bind(&s.disks->hsb, w);
    w2k_list_add_col(s.disks, "Name", 200, 0);
    w2k_list_add_col(s.disks, "Type", 110, 0);
    s.disks->r = (W2kRect){ c.x + 10, c.y + 28, c.w - 20, 130 };
    fill_disks(s.disks);
    w2k_list_layout(s.disks);
    int gb = s.disks->r.y + s.disks->r.h + 10 + 84 + 8;
    s.b[DB_TROUBLE] = (W2kRect){ c.x + c.w - 10 - 90 * 2 - 6, gb, 90, 23 };
    s.b[DB_HWPROPS] = (W2kRect){ c.x + c.w - 10 - 90, gb, 90, 23 };

    w2k_win_center(w, over);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);
    w2k_add_timer(w2k_caret_blink, blink_cb, s.label);
    int r = w2k_win_modal(w);
    w2k_del_timer(blink_cb, s.label);
    w2k_edit_free(s.label);
    w2k_list_free(s.disks);
    w2k_tabs_free(s.tabs);
    return r == ID_OK;
}
