/* fileprops.c -- the file and folder property sheet.
 *
 * Windows 2000's General tab: the icon and an editable name, then the
 * type, where it lives, how big it is, its three dates, and the attribute
 * check boxes. OK and Apply commit, Cancel does nothing -- so a rename
 * and an attribute change made together happen together.
 *
 * A Windows program has a Compatibility tab besides, as on Windows XP:
 * which version of Proton runs it (or Wine), which version of Windows it
 * is told it runs on, and a few switches for games -- kept in ~/.w2k/compat
 * (lib/proton.c) and used by "l2kproton run", which opens the program.
 *
 * Two attributes have no Unix equivalent and are mapped rather than
 * faked. Read-only clears the write bits; Hidden is the leading dot that
 * is this system's actual convention, so ticking it renames the file.
 * Both are what the rest of the shell already believes, so a file marked
 * hidden here disappears from Explorer exactly as it should. */
#include "w2k.h"
#include "w2kui.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    W2kWin  *w;
    W2kTabs *tabs;
    W2kEdit *name;
    W2kEdit *mode_edit;             /* tiny octal chmod box, e.g. "644" */
    W2kRect  ok, cancel, apply, ro_box, hid_box;
    W2kRect  perm_box[9];           /* owner/group/other × r/w/x */
    int      down;

    char     dir[1024];             /* the containing directory */
    char     file[256];             /* the name as it is on disk now */
    int      isdir, islink;
    int      readonly, hidden;      /* working copies */
    int      was_ro, was_hidden;
    mode_t   mode;                  /* working permission bits (07777) */
    mode_t   was_mode;
    char     type[64];
    int      icon;
    long long size, ondisk;
    int      nfiles, nfolders, truncated;
    struct stat st;

    /* The Compatibility tab, for a Windows program. */
    int       compat, exe, is64;    /* exe: an .exe or .com, which can have a Windows version */
    W2kCompat was_cc;
    int       use_runner, use_winver, wined3d, nosync, hud;
    W2kCombo *runner, *winver;
    W2kRect   use_runner_box, use_winver_box, d3d_box, sync_box, hud_box, manager;
    W2kProton pv[32];
    int       npv;
    char      def[128];             /* what runs it without a choice of its own */
    char      missing[128];         /* a build chosen for it that is not installed */
    int       wv_map[16], nwv;      /* the version combo's rows in w2k_win_versions */
} Props;

/* ---- Sizes ---------------------------------------------------------- */

/* Windows' two-part size: "1.44 MB (1,512,345 bytes)". */
static void size_line(long long b, char *out, int n)
{
    char bytes[40];
    /* Thousands separators, built from the right. */
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", b);
    int len = (int)strlen(tmp), k = 0;
    for (int i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0 && k < (int)sizeof bytes - 2) bytes[k++] = ',';
        bytes[k++] = tmp[i];
    }
    bytes[k] = 0;

    const char *unit = "bytes";
    double v = (double)b;
    if (b >= 1024LL * 1024 * 1024) { v = b / (1024.0 * 1024 * 1024); unit = "GB"; }
    else if (b >= 1024 * 1024)     { v = b / (1024.0 * 1024);        unit = "MB"; }
    else if (b >= 1024)            { v = b / 1024.0;                 unit = "KB"; }
    else                           { snprintf(out, (size_t)n, "%s bytes", bytes); return; }
    snprintf(out, (size_t)n, "%.2f %s (%s bytes)", v, unit, bytes);
}

/* Walk a folder. Bounded: a scan of / should not hang the dialog, so it
 * gives up after a while and says so with a "+" on the count. */
static void scan_dir(Props *p, const char *path, int depth, long *budget)
{
    if (depth > 32 || *budget <= 0) { p->truncated = 1; return; }
    DIR *dp = opendir(path);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (--*budget <= 0) { p->truncated = 1; break; }
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            p->nfolders++;
            scan_dir(p, full, depth + 1, budget);
        } else {
            p->nfiles++;
            p->size += (long long)st.st_size;
            p->ondisk += (long long)st.st_blocks * 512;
        }
    }
    closedir(dp);
}

static void measure(Props *p)
{
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", p->dir, p->file);
    if (lstat(full, &p->st) != 0) return;
    p->islink = S_ISLNK(p->st.st_mode);
    p->isdir = S_ISDIR(p->st.st_mode);
    mode_t shown_mode = p->st.st_mode;
    if (p->islink) {
        /* A link's own mode is always 0777 and means nothing; what it
         * points at is shown. Nothing is changed through it -- chmod()
         * follows the link, and ticking Read-only on a link to a private
         * key used to leave the key readable by everyone. */
        struct stat ts;
        if (stat(full, &ts) == 0) { p->isdir = S_ISDIR(ts.st_mode); shown_mode = ts.st_mode; }
    }
    p->st.st_mode = (p->st.st_mode & ~(mode_t)07777) | (shown_mode & 07777);
    p->readonly = p->was_ro = !(p->st.st_mode & S_IWUSR);
    p->hidden = p->was_hidden = p->file[0] == '.';
    p->mode = p->was_mode = p->st.st_mode & 07777;
    p->icon = w2k_file_icon_stat(full, p->file, p->isdir);
    w2k_file_type(p->file, p->isdir, p->type, sizeof p->type);

    if (p->isdir) {
        long budget = 200000;
        scan_dir(p, full, 0, &budget);
    } else {
        p->size = (long long)p->st.st_size;
        p->ondisk = (long long)p->st.st_blocks * 512;
    }
}

/* ---- Mode helpers --------------------------------------------------- */

static void mode_to_edit(Props *p)
{
    if (!p->mode_edit) return;
    char buf[8];
    snprintf(buf, sizeof buf, "%03o", (unsigned)(p->mode & 0777));
    w2k_edit_set(p->mode_edit, buf);
}

/* Read the octal box into p->mode (low 9 bits). Returns 1 if valid. */
static int mode_from_edit(Props *p)
{
    if (!p->mode_edit) return 0;
    const char *t = w2k_edit_text(p->mode_edit);
    if (!t || !t[0]) return 0;
    char *end = NULL;
    unsigned long v = strtoul(t, &end, 8);
    if (end == t || v > 0777) return 0;
    p->mode = (p->mode & ~0777) | (mode_t)v;
    p->readonly = !(p->mode & S_IWUSR);
    return 1;
}

/* ---- Painting ------------------------------------------------------- */

static void when_text(time_t t, char *out, int n)
{
    if (!t) { snprintf(out, (size_t)n, "(unknown)"); return; }
    struct tm tm;
    localtime_r(&t, &tm);
    /* "Tuesday, September 02, 2026 3:04:11 PM" */
    strftime(out, (size_t)n, "%A, %B %d, %Y %I:%M:%S %p", &tm);
}

static int row(Drawable d, int x, int vx, int y, const char *label,
               const char *value)
{
    int fh = w2k_font_height(F_UI);
    w2k_text(d, F_UI, x, y, label, C_TEXT);
    w2k_text(d, F_UI, vx, y, value, C_TEXT);
    return y + fh + 5;
}

static void sep(Drawable d, int x, int y, int w)
{
    w2k_hline(d, x, y, w, C_SHADOW);
    w2k_hline(d, x, y + 1, w, C_HILIGHT);
}

/* ---- The Compatibility tab ------------------------------------------ */

/* The runner combo's rows by the names ~/.w2k/compat uses: Wine, the
 * builds newest first, and last a build this program was set to use that
 * has since been removed -- kept, so that looking at the tab does not
 * quietly change what the program runs with. */
static const char *runner_at(const Props *p, int row)
{
    if (row >= 1 && row <= p->npv) return p->pv[row - 1].name;
    if (row == p->npv + 1 && p->missing[0]) return p->missing;
    return "wine";
}

static int runner_row(const Props *p, const char *name)
{
    for (int i = 0; i < p->npv; i++)
        if (!strcmp(p->pv[i].name, name)) return i + 1;
    return p->missing[0] && !strcmp(p->missing, name) ? p->npv + 1 : 0;
}

/* What will run it, as the tab stands. */
static const char *chosen_runner(const Props *p)
{
    return p->use_runner ? runner_at(p, p->runner->sel) : p->def;
}

static void runner_name(const char *runner, char *out, int n)
{
    snprintf(out, (size_t)n, "%s", strcmp(runner, "wine") ? runner : "Wine");
}

/* (Re)fill the runner combo from the builds there are now. `choice` is
 * the program's own ("" for none). */
static void fill_runners(Props *p, const char *choice)
{
    char want[128], full[2048], label[200];
    snprintf(want, sizeof want, "%s", choice);      /* may point into p->pv */
    snprintf(full, sizeof full, "%s/%s", p->dir, p->file);
    w2k_compat_default_runner(full, p->def, sizeof p->def);
    p->npv = w2k_proton_list(p->pv, (int)(sizeof p->pv / sizeof *p->pv));
    p->missing[0] = 0;
    if (want[0] && strcmp(want, "wine")) {
        int found = 0;
        for (int i = 0; i < p->npv && !found; i++) found = !strcmp(p->pv[i].name, want);
        if (!found) snprintf(p->missing, sizeof p->missing, "%s", want);
    }
    w2k_combo_clear(p->runner);
    w2k_combo_add(p->runner, w2k_wine_available() ? "Wine" : "Wine (not installed)");
    for (int i = 0; i < p->npv; i++) {
        snprintf(label, sizeof label, "%s%s", p->pv[i].name, p->pv[i].steam ? " (Steam)" : "");
        w2k_combo_add(p->runner, label);
    }
    if (p->missing[0]) {
        snprintf(label, sizeof label, "%s (not installed)", p->missing);
        w2k_combo_add(p->runner, label);
    }
    p->runner->sel = runner_row(p, p->use_runner ? want : p->def);
}

static void fill_versions(Props *p, const char *want)
{
    w2k_combo_clear(p->winver);
    p->nwv = 0;
    int sel = -1, xp = 0;
    for (int i = 0; i < w2k_n_win_versions && p->nwv < (int)(sizeof p->wv_map / sizeof *p->wv_map); i++) {
        const W2kWinVersion *v = &w2k_win_versions[i];
        if (p->is64 && !v->id64) continue;      /* no 64-bit program ran on 95 */
        if (!strcmp(v->id, "winxp")) xp = p->nwv;
        if (!strcmp(v->id, want)) sel = p->nwv;
        p->wv_map[p->nwv++] = i;
        w2k_combo_add(p->winver, v->label);
    }
    /* Windows 7's tab offers XP (Service Pack 3) first; so does this. */
    p->winver->sel = sel >= 0 ? sel : xp;
}

/* The tab, as the settings to keep. */
static void compat_state(const Props *p, W2kCompat *c)
{
    memset(c, 0, sizeof *c);
    if (p->use_runner)
        snprintf(c->runner, sizeof c->runner, "%s", runner_at(p, p->runner->sel));
    if (p->use_winver && p->exe && p->winver->sel >= 0 && p->winver->sel < p->nwv)
        snprintf(c->winver, sizeof c->winver, "%s", w2k_win_versions[p->wv_map[p->winver->sel]].id);
    c->wined3d = p->wined3d;
    c->nosync = p->nosync;
    c->hud = p->hud;
}

static int check_w(const char *text)
{
    return 13 + 5 + w2k_mnemonic_width(F_UI, text) + 2;
}

static void paint_compat(Props *p, Drawable d, W2kRect c)
{
    int fh = w2k_font_height(F_UI);
    int x = c.x + 10, wid = c.w - 20, y = c.y + 10;
    y += w2k_text_wrapped(d, F_UI, x, y, wid,
                          "If this program does not work correctly, try running it with "
                          "another version of Proton, or in compatibility mode for the "
                          "version of Windows it was made for.", C_TEXT);
    y += 10;

    int gh = 20 + fh + 8 + 21 + 12;
    W2kRect g = { x, y, wid, gh };
    w2k_draw_groupbox(d, &g, "Proton");
    static const char *const run_with = "Run this program with:";
    p->use_runner_box = (W2kRect){ g.x + 12, g.y + 20, check_w(run_with), fh + 2 };
    w2k_draw_checkbox(d, g.x + 12, g.y + 20, run_with, p->use_runner, 0, 0);
    int cy = g.y + 20 + fh + 8;
    p->manager = (W2kRect){ g.x + g.w - 12 - 112, cy - 1, 112, 23 };
    p->runner->r = (W2kRect){ g.x + 12, cy, p->manager.x - 8 - (g.x + 12), 21 };
    p->runner->disabled = !p->use_runner;
    w2k_combo_draw(d, p->runner);
    w2k_draw_pushbutton(d, &p->manager, "Proton Manager...", p->down == 4 ? BS_PRESSED : 0);

    g = (W2kRect){ x, g.y + g.h + 8, wid, gh };
    w2k_draw_groupbox(d, &g, "Compatibility mode");
    static const char *const mode_for = "Run this program in compatibility mode for:";
    p->use_winver_box = (W2kRect){ g.x + 12, g.y + 20, check_w(mode_for), fh + 2 };
    w2k_draw_checkbox(d, g.x + 12, g.y + 20, mode_for, p->use_winver && p->exe, 0, !p->exe);
    p->winver->r = (W2kRect){ g.x + 12, g.y + 20 + fh + 8, 220, 21 };
    p->winver->disabled = !p->use_winver || !p->exe;
    w2k_combo_draw(d, p->winver);

    /* The switches are Proton's: Wine goes without them. */
    int off = !strcmp(chosen_runner(p), "wine");
    g = (W2kRect){ x, g.y + g.h + 8, wid, 20 + 3 * (fh + 8) + 4 };
    w2k_draw_groupbox(d, &g, "Settings");
    static const char *const label[3] = {
        "Use OpenGL (WineD3D) in place of Vulkan (DXVK)",
        "Turn off esync and fsync",
        "Show the frame rate",
    };
    int *on[3] = { &p->wined3d, &p->nosync, &p->hud };
    W2kRect *box[3] = { &p->d3d_box, &p->sync_box, &p->hud_box };
    for (int i = 0; i < 3; i++) {
        int by = g.y + 20 + i * (fh + 8);
        *box[i] = (W2kRect){ g.x + 12, by, check_w(label[i]), fh + 2 };
        w2k_draw_checkbox(d, g.x + 12, by, label[i], *on[i], 0, off);
    }
    y = g.y + g.h + 10;

    if (!p->npv)
        y += w2k_text_wrapped(d, F_UI, x, y, wid, "No version of Proton is installed. Click "
                              "Proton Manager to download one.", C_TEXT) + 4;
    if (!p->exe)
        w2k_text_wrapped(d, F_UI, x, y, wid, "Compatibility mode is set on a program itself "
                         "(an .exe file), not on an installer, shortcut or batch file.", C_TEXT);
}

static void paint_general(Props *p, Drawable d, W2kRect c)
{
    int fh = w2k_font_height(F_UI);
    int x = c.x + 12, vx = c.x + 110, wid = c.w - 24;

    if (p->islink) w2k_bigicon_draw_link(d, x, c.y + 10, p->icon);
    else           w2k_bigicon_draw(d, x, c.y + 10, p->icon);
    w2k_edit_draw(d, p->name);

    int y = c.y + 52;
    sep(d, x, y, wid);
    y += 8;

    char buf[512];
    y = row(d, x, vx, y, "Type of file:", p->type);
    if (!p->isdir) {
        char cmd[256];
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", p->dir, p->file);
        w2k_assoc_command(full, cmd, sizeof cmd);
        char *sp = strchr(cmd, ' ');
        if (sp) *sp = 0;
        /* A Windows program is opened by what its Compatibility tab says. */
        const char *base = strrchr(cmd, '/');
        if (p->compat && !strcmp(base ? base + 1 : cmd, "l2kproton"))
            runner_name(chosen_runner(p), cmd, sizeof cmd);
        y = row(d, x, vx, y, "Opens with:", cmd);
    }
    y += 3;
    sep(d, x, y, wid);
    y += 8;

    y = row(d, x, vx, y, "Location:", p->dir);
    size_line(p->size, buf, sizeof buf);
    if (p->truncated) {
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, "  or more");
    }
    y = row(d, x, vx, y, "Size:", buf);
    size_line(p->ondisk, buf, sizeof buf);
    y = row(d, x, vx, y, "Size on disk:", buf);
    if (p->isdir) {
        snprintf(buf, sizeof buf, "%d Files, %d Folders%s", p->nfiles,
                 p->nfolders, p->truncated ? " (partial)" : "");
        y = row(d, x, vx, y, "Contains:", buf);
    }
    y += 3;
    sep(d, x, y, wid);
    y += 8;

    when_text(p->st.st_ctime, buf, sizeof buf);
    y = row(d, x, vx, y, "Created:", buf);
    if (!p->isdir) {
        when_text(p->st.st_mtime, buf, sizeof buf);
        y = row(d, x, vx, y, "Modified:", buf);
        when_text(p->st.st_atime, buf, sizeof buf);
        y = row(d, x, vx, y, "Accessed:", buf);
    }
    y += 3;
    sep(d, x, y, wid);
    y += 10;

    w2k_text(d, F_UI, x, y, "Attributes:", C_TEXT);
    p->ro_box  = (W2kRect){ vx, y - 1, 100, fh + 4 };
    p->hid_box = (W2kRect){ vx + 110, y - 1, 100, fh + 4 };
    w2k_draw_checkbox(d, p->ro_box.x, p->ro_box.y, "&Read-only",
                      p->readonly, 0, 0);
    w2k_draw_checkbox(d, p->hid_box.x, p->hid_box.y, "&Hidden",
                      p->hidden, 0, 0);
    y += fh + 8;
    sep(d, x, y, wid);
    y += 6;

    /* Unix mode: tiny octal edit plus owner/group/other rwx checkboxes. */
    {
        w2k_text(d, F_UI, x, y, "Permissions:", C_TEXT);
        if (p->mode_edit) {
            p->mode_edit->r = (W2kRect){ vx, y - 2, 44, fh + 6 };
            w2k_edit_draw(d, p->mode_edit);
            w2k_text(d, F_UI, vx + 50, y, "(octal)", C_GRAYTEXT);
        }
        y += fh + 8;
        static const mode_t bits[9] = {
            S_IRUSR, S_IWUSR, S_IXUSR,
            S_IRGRP, S_IWGRP, S_IXGRP,
            S_IROTH, S_IWOTH, S_IXOTH
        };
        static const char *const labels[9] = {
            "Owner read", "write", "exec",
            "Group read", "write", "exec",
            "Other read", "write", "exec"
        };
        int py = y;
        for (int rowi = 0; rowi < 3; rowi++) {
            int px = vx;
            for (int coli = 0; coli < 3; coli++) {
                int i = rowi * 3 + coli;
                int bw = (coli == 0) ? 90 : 52;
                p->perm_box[i] = (W2kRect){ px, py - 1, bw, fh + 2 };
                w2k_draw_checkbox(d, px, py - 1, labels[i],
                                  (p->mode & bits[i]) != 0, 0, 0);
                px += bw + 2;
            }
            py += fh + 3;
        }
    }
}

static void paint(W2kWin *w, Drawable d)
{
    Props *p = w->user;
    w2k_fill(d, 0, 0, w->w, w->h, C_FACE);
    w2k_tabs_draw(d, p->tabs);
    W2kRect c = w2k_tabs_client(p->tabs);
    if (p->compat && p->tabs->sel == 1) paint_compat(p, d, c);
    else                                paint_general(p, d, c);

    w2k_draw_pushbutton(d, &p->ok, "OK",
                        BS_DEFAULT | (p->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &p->cancel, "Cancel", p->down == 2 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &p->apply, "&Apply", p->down == 3 ? BS_PRESSED : 0);
}

/* ---- Applying ------------------------------------------------------- */

static void fail(Props *p, const char *what)
{
    char msg[512];
    snprintf(msg, sizeof msg, "Unable to %s.\n\n%s", what, strerror(errno));
    w2k_msgbox(p->w, "Properties", msg, MB_OK | MB_ICONERROR);
}

static int apply(Props *p)
{
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", p->dir, p->file);

    /* Prefer the octal box when it holds a valid value; otherwise keep the
     * mode the checkboxes last set. */
    mode_from_edit(p);

    if (p->readonly != p->was_ro && (p->mode & 0777) == (p->was_mode & 0777)) {
        mode_t m = p->mode;
        if (p->readonly) m &= (mode_t)~(S_IWUSR | S_IWGRP | S_IWOTH);
        else             m |= S_IWUSR;
        p->mode = m;
        mode_to_edit(p);
    }
    if (p->islink) {                 /* see measure(): not through a link */
        p->mode = p->was_mode;
        p->readonly = p->was_ro;
        mode_to_edit(p);
    } else if ((p->mode & 07777) != (p->was_mode & 07777) || p->readonly != p->was_ro) {
        if (chmod(full, p->mode & 07777) != 0) {
            fail(p, "set permissions");
            return 0;
        }
        p->was_mode = p->mode & 07777;
        p->was_ro = !(p->mode & S_IWUSR);
        p->readonly = p->was_ro;
        p->st.st_mode = (p->st.st_mode & ~07777) | p->was_mode;
        mode_to_edit(p);
    }

    /* The name and the hidden dot are the same operation: one rename. */
    const char *typed = w2k_edit_text(p->name);
    char want[256];
    snprintf(want, sizeof want, "%s", typed && *typed ? typed : p->file);
    if (want[0] == '.') memmove(want, want + 1, strlen(want));   /* dot is the flag */
    char target[256];
    if (p->hidden) snprintf(target, sizeof target, ".%.254s", want);
    else           snprintf(target, sizeof target, "%.255s", want);

    if (strcmp(target, p->file)) {
        if (strchr(target, '/')) {
            w2k_msgbox(p->w, "Properties",
                       "A file name cannot contain a slash.",
                       MB_OK | MB_ICONERROR);
            return 0;
        }
        char to[2048];
        snprintf(to, sizeof to, "%s/%s", p->dir, target);
        /* A Windows program's settings go with it to its new name. */
        W2kCompat moved;
        int had = p->compat && w2k_compat_get(full, &moved);
        if (had) w2k_compat_set(full, NULL);
        /* Never over something else with that name: ticking Hidden on
         * "profile" used to replace an existing ".profile". */
        if (w2k_fs_rename_noreplace(full, to) != 0) {
            int err = errno;
            if (had) w2k_compat_set(full, &moved);
            errno = err;
            if (errno == EEXIST) {
                char m[400];
                snprintf(m, sizeof m, "Cannot rename: there is already a file named "
                         "'%.200s' in this folder.", target);
                w2k_msgbox(p->w, "Properties", m, MB_OK | MB_ICONERROR);
                return 0;
            }
            fail(p, "rename this item");
            return 0;
        }
        if (had) w2k_compat_set(to, &moved);
        snprintf(p->file, sizeof p->file, "%s", target);
        p->was_hidden = p->hidden;
        w2k_edit_set(p->name, want);
        char title[300];
        snprintf(title, sizeof title, "%s Properties", p->file);
        w2k_win_title(p->w, title);
        /* The name may have changed; re-read the item under its new one,
         * so a second Apply works from the right file. */
        snprintf(full, sizeof full, "%s/%s", p->dir, p->file);
    }
    if (lstat(full, &p->st) == 0) {
        p->was_ro = !(p->st.st_mode & S_IWUSR);
        p->was_mode = p->st.st_mode & 07777;
        p->mode = p->was_mode;
        p->readonly = p->was_ro;
        mode_to_edit(p);
    }

    if (p->compat) {
        W2kCompat now;
        compat_state(p, &now);
        if (strcmp(now.runner, p->was_cc.runner) || strcmp(now.winver, p->was_cc.winver) ||
            now.wined3d != p->was_cc.wined3d || now.nosync != p->was_cc.nosync ||
            now.hud != p->was_cc.hud) {
            if (w2k_compat_set(full, &now) != 0) {
                w2k_msgbox(p->w, "Properties", "Unable to save the compatibility settings.",
                           MB_OK | MB_ICONERROR);
                return 0;
            }
            p->was_cc = now;
        }
    }
    return 1;
}

/* Opened apart from this process, so that closing the sheet leaves it. */
static void launch(const char *prog)
{
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            signal(SIGPIPE, SIG_DFL);
            execlp(prog, prog, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);
}

static void general_press(Props *p, XButtonEvent *b)
{
    int x = b->x, y = b->y;
    if (w2k_edit_press(p->name, b)) {
        if (p->mode_edit) p->mode_edit->focused = 0;
    } else if (p->mode_edit && w2k_edit_press(p->mode_edit, b)) {
        p->name->focused = 0;
    } else if (w2k_rect_hit(&p->ro_box, x, y)) {
        p->readonly = !p->readonly;
        if (p->readonly) p->mode &= (mode_t)~(S_IWUSR | S_IWGRP | S_IWOTH);
        else             p->mode |= S_IWUSR;
        mode_to_edit(p);
    } else if (w2k_rect_hit(&p->hid_box, x, y)) {
        p->hidden = !p->hidden;
    } else {
        static const mode_t bits[9] = {
            S_IRUSR, S_IWUSR, S_IXUSR,
            S_IRGRP, S_IWGRP, S_IXGRP,
            S_IROTH, S_IWOTH, S_IXOTH
        };
        for (int i = 0; i < 9; i++)
            if (w2k_rect_hit(&p->perm_box[i], x, y)) {
                p->mode ^= bits[i];
                p->readonly = !(p->mode & S_IWUSR);
                mode_to_edit(p);
                break;
            }
    }
}

static void compat_press(Props *p, XButtonEvent *b)
{
    int x = b->x, y = b->y;
    W2kCombo *combo[2] = { p->runner, p->winver };
    for (int i = 0; i < 2; i++)
        if (w2k_rect_hit(&combo[i]->r, x, y)) {
            combo[1 - i]->focused = 0;
            w2k_combo_press(combo[i], b);
            if (combo[i]->disabled) combo[i]->focused = 0;
            return;
        }
    p->runner->focused = p->winver->focused = 0;
    int proton = strcmp(chosen_runner(p), "wine") != 0;
    if (w2k_rect_hit(&p->use_runner_box, x, y)) {
        p->use_runner = !p->use_runner;
        /* Unticked, the list shows what runs the program instead. */
        if (!p->use_runner) p->runner->sel = runner_row(p, p->def);
    } else if (w2k_rect_hit(&p->use_winver_box, x, y)) {
        if (p->exe) p->use_winver = !p->use_winver;
    } else if (proton && w2k_rect_hit(&p->d3d_box, x, y)) {
        p->wined3d = !p->wined3d;
    } else if (proton && w2k_rect_hit(&p->sync_box, x, y)) {
        p->nosync = !p->nosync;
    } else if (proton && w2k_rect_hit(&p->hud_box, x, y)) {
        p->hud = !p->hud;
    } else if (w2k_rect_hit(&p->manager, x, y)) {
        p->down = 4;
    }
}

static int event(W2kWin *w, XEvent *e)
{
    Props *p = w->user;
    int general = !(p->compat && p->tabs->sel == 1);
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_tabs_press(p->tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&p->ok, x, y)) p->down = 1;
        else if (w2k_rect_hit(&p->cancel, x, y)) p->down = 2;
        else if (w2k_rect_hit(&p->apply, x, y)) p->down = 3;
        else if (general) general_press(p, &e->xbutton);
        else compat_press(p, &e->xbutton);
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int b = p->down, x = e->xbutton.x, y = e->xbutton.y;
        p->down = 0;
        w2k_edit_release(p->name);
        if (p->mode_edit) w2k_edit_release(p->mode_edit);
        if (b == 1 && w2k_rect_hit(&p->ok, x, y)) {
            if (apply(p)) w2k_win_close(w, ID_OK);
            return 1;
        }
        if (b == 2 && w2k_rect_hit(&p->cancel, x, y)) {
            w2k_win_close(w, ID_CANCEL);
            return 1;
        }
        if (b == 3 && w2k_rect_hit(&p->apply, x, y)) apply(p);
        if (b == 4 && w2k_rect_hit(&p->manager, x, y)) launch("l2kproton");
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (!general) return 0;
        if (w2k_edit_motion(p->name, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        if (p->mode_edit && w2k_edit_motion(p->mode_edit, &e->xmotion)) {
            w2k_win_dirty(w);
            return 1;
        }
        return 0;
    case FocusIn:
        /* Back from Proton Manager, perhaps with another build. */
        if (p->compat && e->xfocus.mode == NotifyNormal && e->xfocus.detail != NotifyPointer) {
            fill_runners(p, p->use_runner ? runner_at(p, p->runner->sel) : "");
            w2k_win_dirty(w);
        }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            if (apply(p)) w2k_win_close(w, ID_OK);
            return 1;
        }
        if (w2k_tabs_key(p->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (!general) return 1;
        if (p->mode_edit && p->mode_edit->focused) {
            if (w2k_edit_key(p->mode_edit, &e->xkey)) {
                /* Live-sync checkboxes while typing a valid octal value. */
                mode_from_edit(p);
                w2k_win_dirty(w);
                return 1;
            }
        }
        if (w2k_edit_key(p->name, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void blink_cb(void *u) { w2k_edit_blink(u); }

int w2k_file_properties(W2kWin *over, const char *path)
{
    return w2k_file_properties_page(over, path, 0);
}

int w2k_file_properties_page(W2kWin *over, const char *path, int page)
{
    Props p;
    memset(&p, 0, sizeof p);

    snprintf(p.dir, sizeof p.dir, "%s", path);
    char *slash = strrchr(p.dir, '/');
    if (slash && slash != p.dir) { *slash = 0; snprintf(p.file, sizeof p.file, "%s", slash + 1); }
    else if (slash)              { p.dir[1] = 0; snprintf(p.file, sizeof p.file, "%s", slash + 1); }
    else                         { snprintf(p.file, sizeof p.file, "%s", path); snprintf(p.dir, sizeof p.dir, "."); }
    if (!p.file[0]) return 0;
    measure(&p);

    int W = 400, H = p.isdir ? 440 : 500;
    char title[300];
    snprintf(title, sizeof title, "%s Properties", p.file);
    W2kWin *w = w2k_win_new(title, "w2kdialog", W, H, 0);
    p.w = w;
    w->user = &p;
    w->paint = paint;
    w->event = event;

    p.tabs = w2k_tabs_new(NULL, NULL);
    w2k_tabs_add(p.tabs, "General");
    /* A Windows program's Compatibility tab: what runs it, and how. */
    p.compat = !p.isdir && w2k_wine_file(p.file);
    if (p.compat) {
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", p.dir, p.file);
        const char *dot = strrchr(p.file, '.');
        p.exe = dot && (!strcasecmp(dot, ".exe") || !strcasecmp(dot, ".com"));
        p.is64 = p.exe && w2k_exe_is_64bit(full);
        w2k_compat_get(full, &p.was_cc);
        p.use_runner = p.was_cc.runner[0] != 0;
        p.use_winver = p.was_cc.winver[0] != 0;
        p.wined3d = p.was_cc.wined3d;
        p.nosync = p.was_cc.nosync;
        p.hud = p.was_cc.hud;
        p.runner = w2k_combo_new(0);
        p.winver = w2k_combo_new(0);
        fill_runners(&p, p.was_cc.runner);
        fill_versions(&p, p.was_cc.winver);
        w2k_tabs_add(p.tabs, "Compatibility");
        if (page == 1) p.tabs->sel = 1;
    }
    p.tabs->r = (W2kRect){ 8, 8, W - 16, H - 8 - 40 };
    W2kRect c = w2k_tabs_client(p.tabs);

    p.name = w2k_edit_new(0);
    w2k_edit_bind(p.name, w);
    {   /* The name without the hidden dot: the dot is the check box. */
        const char *shown = p.file[0] == '.' ? p.file + 1 : p.file;
        w2k_edit_set(p.name, shown);
    }
    p.name->r = (W2kRect){ c.x + 56, c.y + 16, c.w - 68, 21 };
    p.name->focused = 1;
    w2k_edit_select_all(p.name);

    p.mode_edit = w2k_edit_new(0);
    w2k_edit_bind(p.mode_edit, w);
    mode_to_edit(&p);

    p.ok     = (W2kRect){ W - 12 - 75 * 3 - 12, H - 12 - 23, 75, 23 };
    p.cancel = (W2kRect){ W - 12 - 75 * 2 - 6, H - 12 - 23, 75, 23 };
    p.apply  = (W2kRect){ W - 12 - 75, H - 12 - 23, 75, 23 };

    w2k_win_center(w, over);
    if (over) XSetTransientForHint(w2k.dpy, w->win, over->win);
    w2k_add_timer(w2k_caret_blink, blink_cb, p.name);
    w2k_add_timer(w2k_caret_blink, blink_cb, p.mode_edit);
    int r = w2k_win_modal(w);
    w2k_del_timer(blink_cb, p.name);
    w2k_del_timer(blink_cb, p.mode_edit);
    w2k_edit_free(p.name);
    w2k_edit_free(p.mode_edit);
    if (p.runner) w2k_combo_free(p.runner);
    if (p.winver) w2k_combo_free(p.winver);
    w2k_tabs_free(p.tabs);
    return r == ID_OK;
}
