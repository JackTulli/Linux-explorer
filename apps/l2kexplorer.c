/* l2kexplorer -- Windows Explorer.
 *
 * Folders pane on the left, contents on the right, four view modes, an
 * address bar with a search filter beside it, a status bar and the usual
 * file operations. Virtual roots (Desktop / My Computer / My Documents)
 * sit above the real filesystem the way the Windows shell namespace does.
 *
 * Search supports wildcards (* ? []); typing a letter focuses the search
 * box, Enter applies the filter and unfocuses. Ctrl+wheel and Alt+1..4
 * cycle the folder view. */
#include "w2kui.h"
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    ID_OPEN = 1, ID_EXPLORE, ID_NEWFOLDER, ID_DELETE, ID_RENAME, ID_PROPS,
    ID_EMPTYBIN, ID_ZIP, ID_UNZIP, ID_RESTORE,
    ID_CLOSE, ID_CUT, ID_COPY, ID_PASTE, ID_SELECTALL, ID_INVERT,
    ID_V_LARGE, ID_V_SMALL, ID_V_LIST, ID_V_DETAILS, ID_REFRESH,
    ID_BACK, ID_FORWARD, ID_UP, ID_SEARCH, ID_FOLDERS, ID_VIEWS,
    ID_SHOW_HIDDEN,
    ID_ABOUT, ID_MYCOMPUTER, ID_MYDOCS, ID_FOLDEROPTS, ID_MAPDRIVE,
    ID_UNDO, ID_SHORTCUT, ID_OPENWITH, ID_NEW_SHORTCUT, ID_NEW_TEXT,
    ID_TB_STANDARD, ID_TB_ADDRESS, ID_STATUSBAR,
    ID_ARR_NAME, ID_ARR_SIZE, ID_ARR_TYPE, ID_ARR_DATE,
    ID_FAV_ADD, ID_FAV_ORG,
    ID_SENDTO_DESKTOP, ID_SENDTO_MYDOCS
};

/* Shell namespace node kinds. */
enum { K_FS = 0, K_DESKTOP, K_MYCOMPUTER, K_NETWORK, K_RECYCLE };

typedef struct { int kind; char path[1024]; } Node;

typedef struct {
    W2kWin     *win;
    W2kMenubar *mb;
    W2kToolbar *tb;
    W2kCombo   *addr;
    W2kEdit    *search;               /* filter box beside the address bar */
    W2kTree    *tree;
    W2kList    *list;
    W2kStatus  *sb;
    W2kRect     addr_label, search_label, tree_r, split_r;

    Node        cur;
    char        history[32][1024];
    int         hist_kind[32];
    int         hist_n, hist_i;

    int         view;                 /* ID_V_* */
    int         show_tree;
    int         show_toolbar, show_address, show_status;
    int         split_x, dragging_split;
    int         sort_col, sort_dir;

    char        clip[64][1024];       /* pending file cut/copy */
    int         nclip, clip_cut;
    char        home[1024];
    char        search_pat[256];      /* active wildcard filter; empty = none */
} Exp;

static Exp ex;

/* ------------------------------------------------------------------ *
 * Paths and file types
 * ------------------------------------------------------------------ */
/* Copy with truncation. Paths longer than a Node can hold simply get cut;
 * that beats overflowing, and is all a file browser can sensibly do. */
static void set_path(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

/* Join a directory and a leaf name; silently truncates rather than
 * overflowing, which is all a browser can sensibly do with a long path. */
static void path_join(char *out, int n, const char *dir, const char *name)
{
    if (n <= 0) return;
    int o = 0;
    for (const char *p = dir; *p && o < n - 1; p++) out[o++] = *p;
    if (strcmp(dir, "/") && o < n - 1) out[o++] = '/';
    for (const char *p = name; *p && o < n - 1; p++) out[o++] = *p;
    out[o] = 0;
}

static void path_parent(char *p)
{
    char *s = strrchr(p, '/');
    if (!s) { strcpy(p, "/"); return; }
    if (s == p) { p[1] = 0; return; }
    *s = 0;
}

/* Lowercase ASCII into out (NUL-terminated). Used so fnmatch can match
 * case-insensitively without depending on the non-POSIX FNM_CASEFOLD. */
static void lower_copy(char *out, int n, const char *in)
{
    int i = 0;
    if (n <= 0) return;
    for (; in[i] && i < n - 1; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = 0;
}

/* Wildcard filter for the search bar. Empty pattern matches everything.
 * A pattern without * or ? is treated as a case-insensitive substring
 * (internally wrapped as *pat*); otherwise it is passed to fnmatch. */
static int entry_matches(const char *name)
{
    const char *pat = ex.search_pat;
    if (!pat || !pat[0]) return 1;
    char lname[512], lpat[280];
    lower_copy(lname, sizeof lname, name);
    lower_copy(lpat, sizeof lpat, pat);
    int wild = 0;
    for (const char *p = lpat; *p; p++)
        if (*p == '*' || *p == '?' || *p == '[') { wild = 1; break; }
    if (!wild) {
        char wrapped[290];
        snprintf(wrapped, sizeof wrapped, "*%s*", lpat);
        return fnmatch(wrapped, lname, 0) == 0;
    }
    return fnmatch(lpat, lname, 0) == 0;
}

/* Windows shows sizes in whole KB, rounded up. */
static void size_text(long long bytes, char *out, int n)
{
    if (bytes < 1024) snprintf(out, n, "%lld KB", bytes ? 1LL : 0LL);
    else {
        long long kb = (bytes + 1023) / 1024;
        if (kb < 1024) snprintf(out, n, "%lld KB", kb);
        else snprintf(out, n, "%lld MB", (kb + 1023) / 1024);
    }
}

static void display_name(const Node *nd, char *out, int n)
{
    switch (nd->kind) {
    case K_DESKTOP:    snprintf(out, n, "Desktop"); return;
    case K_MYCOMPUTER: snprintf(out, n, "My Computer"); return;
    case K_NETWORK:    snprintf(out, n, "My Network Places"); return;
    case K_RECYCLE:    snprintf(out, n, "Recycle Bin"); return;
    }
    if (!strcmp(nd->path, "/"))     { snprintf(out, n, "Local Disk (C:)"); return; }
    if (!strcmp(nd->path, ex.home)) { snprintf(out, n, "My Documents"); return; }
    const char *b = strrchr(nd->path, '/');
    snprintf(out, n, "%s", (b && b[1]) ? b + 1 : nd->path);
}

/* ------------------------------------------------------------------ *
 * Listing
 * ------------------------------------------------------------------ */
typedef struct {
    char name[256];
    int  isdir;
    int  link;                  /* symlink, or a .desktop shortcut */
    long long size;
    time_t mtime;
    int  icon;
    char type[64];
    char target[512];           /* a drive's mount point, for My Computer */
} Entry;

static Entry *entries;
static int    nentries, capentries;

static int cmp_entries(const void *A, const void *B)
{
    const Entry *a = A, *b = B;
    if (a->isdir != b->isdir) return b->isdir - a->isdir;   /* folders first */
    int r = 0;
    switch (ex.sort_col) {
    case 1: r = (a->size > b->size) - (a->size < b->size); break;
    case 2: r = strcmp(a->type, b->type); break;
    case 3: r = (a->mtime > b->mtime) - (a->mtime < b->mtime); break;
    default: r = 0;
    }
    if (r == 0) {
        const char *p = a->name, *q = b->name;
        while (*p && *q) {
            int x = *p, y = *q;
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
            if (x != y) { r = x - y; break; }
            p++; q++;
        }
        if (!r) r = (int)strlen(a->name) - (int)strlen(b->name);
    }
    return ex.sort_dir ? -r : r;
}

static void entries_clear(void) { nentries = 0; }

static Entry *entry_push(void)
{
    if (nentries == capentries) {
        capentries = capentries ? capentries * 2 : 128;
        entries = realloc(entries, capentries * sizeof *entries);
        if (!entries) abort();
    }
    Entry *e = &entries[nentries++];
    memset(e, 0, sizeof *e);
    return e;
}

static void status_update(void)
{
    char buf[80];
    int nsel = 0;
    long long selsize = 0;
    for (int i = 0; i < ex.list->n; i++)
        if (ex.list->items[i].selected) {
            nsel++;
            if (i < nentries) selsize += entries[i].size;
        }
    if (nsel == 1 && ex.list->sel >= 0 && ex.list->sel < nentries &&
        !entries[ex.list->sel].isdir) {
        char sz[32];
        size_text(entries[ex.list->sel].size, sz, sizeof sz);
        snprintf(buf, sizeof buf, "1 object(s) selected");
        w2k_status_set(ex.sb, 0, buf);
        w2k_status_set(ex.sb, 1, sz);
    } else if (nsel > 1) {
        char sz[32];
        size_text(selsize, sz, sizeof sz);
        snprintf(buf, sizeof buf, "%d object(s) selected", nsel);
        w2k_status_set(ex.sb, 0, buf);
        w2k_status_set(ex.sb, 1, sz);
    } else {
        snprintf(buf, sizeof buf, "%d object(s)", ex.list->n);
        w2k_status_set(ex.sb, 0, buf);
        w2k_status_set(ex.sb, 1, "");
    }
    w2k_status_set(ex.sb, 2, ex.cur.kind == K_FS ? "My Computer" : "");
}

static void list_configure(void)
{
    int mode = (ex.view == ID_V_DETAILS) ? LV_REPORT
             : (ex.view == ID_V_LIST)    ? LV_LIST : LV_ICON;
    ex.list->mode = mode;
    ex.list->row_h = (ex.view == ID_V_SMALL || ex.view == ID_V_LIST ||
                      ex.view == ID_V_DETAILS) ? 17 : 17;
    ex.list->fullrow = 0;
}

static void refill_list(void)
{
    w2k_list_clear(ex.list);
    entries_clear();

    if (ex.cur.kind == K_MYCOMPUTER || ex.cur.kind == K_DESKTOP) {
        static const struct { const char *n, *t; int ico; } virt_mc[] = {
            { "Local Disk (C:)", "Local Disk", ICO_DRIVE_HDD },
            { "My Documents",    "System Folder", ICO_MYDOCS },
        };
        static const struct { const char *n, *t; int ico; } virt_dt[] = {
            { "My Documents",      "System Folder", ICO_MYDOCS },
            { "My Computer",       "System Folder", ICO_MYCOMPUTER },
            { "My Network Places", "System Folder", ICO_NETWORK },
            { "Recycle Bin",       "System Folder", ICO_RECYCLE },   /* or full */
        };
        int n = (ex.cur.kind == K_MYCOMPUTER) ? 2 : 4;
        for (int i = 0; i < n; i++) {
            const char *nm = (ex.cur.kind == K_MYCOMPUTER) ? virt_mc[i].n : virt_dt[i].n;
            const char *tp = (ex.cur.kind == K_MYCOMPUTER) ? virt_mc[i].t : virt_dt[i].t;
            int ico = (ex.cur.kind == K_MYCOMPUTER) ? virt_mc[i].ico : virt_dt[i].ico;
            if (ico == ICO_RECYCLE && w2k_trash_count() > 0) ico = ICO_RECYCLE_FULL;
            Entry *e = entry_push();
            snprintf(e->name, sizeof e->name, "%s", nm);
            snprintf(e->type, sizeof e->type, "%s", tp);
            e->isdir = 1;
            e->icon = ico;
            e->target[0] = 0;
        }
        if (ex.cur.kind == K_MYCOMPUTER) {
            /* Mounted media and /mnt as drives from D:, then Control Panel,
             * which Windows 2000 lists here too. */
            W2kDrive dr[16];
            int nd = w2k_fs_drives(dr, 16);
            for (int i = 0; i < nd; i++) {
                Entry *e = entry_push();
                snprintf(e->name, sizeof e->name, "%.200s (%c:)", dr[i].label, dr[i].letter);
                snprintf(e->type, sizeof e->type, "%s", dr[i].optical ? "Compact Disc"
                         : dr[i].removable ? "Removable Disk" : "Local Disk");
                e->isdir = 1;
                e->icon = dr[i].optical ? ICO_DRIVE_CD : dr[i].removable ? ICO_DRIVE_FLOPPY : ICO_DRIVE_HDD;
                snprintf(e->target, sizeof e->target, "%.511s", dr[i].path);
            }
            Entry *e = entry_push();
            snprintf(e->name, sizeof e->name, "Control Panel");
            snprintf(e->type, sizeof e->type, "System Folder");
            e->isdir = 1;
            e->icon = ICO_CONTROLPANEL;
            snprintf(e->target, sizeof e->target, "@controlpanel");
        }
    } else if (ex.cur.kind == K_FS) {
        DIR *dp = opendir(ex.cur.path);
        if (!dp) {
            char msg[1200];
            snprintf(msg, sizeof msg, "%s is not accessible.\n\n%s",
                     ex.cur.path, strerror(errno));
            w2k_msgbox(ex.win, "Windows Explorer", msg, MB_OK | MB_ICONERROR);
        } else {
            struct dirent *de;
            while ((de = readdir(dp))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                /* Unix has no hidden bit; a leading dot is the convention,
                 * and Folder Options decides whether it is honoured. */
                if (de->d_name[0] == '.' && !w2k_folder_hidden) continue;
                char full[2048];
                path_join(full, sizeof full, ex.cur.path, de->d_name);
                struct stat st;
                if (lstat(full, &st) != 0) continue;
                Entry *e = entry_push();
                snprintf(e->name, sizeof e->name, "%s", de->d_name);
                e->link = S_ISLNK(st.st_mode);
                if (e->link) {
                    struct stat ts;
                    if (stat(full, &ts) == 0) st = ts;   /* follow for size */
                }
                e->isdir = S_ISDIR(st.st_mode);
                e->size = e->isdir ? 0 : (long long)st.st_size;
                e->mtime = st.st_mtime;
                e->icon = w2k_file_icon_stat(full, e->name, e->isdir);
                if (!strcasecmp(w2k_file_ext(de->d_name), "desktop") &&
                    access(full, X_OK) == 0) {
                    /* A shortcut wears the icon of what it points at --
                     * but only a shortcut marked executable is trusted to
                     * be one. Anything else that merely ends in .desktop
                     * (a download, something out of an archive) is shown
                     * for what it is, extension and all. */
                    char ico[128];
                    e->link = 1;
                    if (w2k_desktop_entry(full, NULL, 0, NULL, 0,
                                          ico, sizeof ico) && ico[0])
                        e->icon = w2k_icon_by_name(ico);
                }
                w2k_file_type(e->name, e->isdir, e->type, sizeof e->type);
            }
            closedir(dp);
        }
    }
    qsort(entries, nentries, sizeof *entries, cmp_entries);

    for (int i = 0; i < nentries; i++) {
        Entry *e = &entries[i];
        if (!entry_matches(e->name)) continue;
        int r = w2k_list_add(ex.list, e->icon, NULL);
        ex.list->items[r].link = e->link;
        char shown[256];
        if (!e->isdir && !e->link &&
            !strcasecmp(w2k_file_ext(e->name), "desktop"))
            snprintf(shown, sizeof shown, "%s", e->name);   /* untrusted: full name */
        else
            w2k_file_display_name(e->name, e->isdir, shown, sizeof shown);
        w2k_list_set(ex.list, r, 0, shown);
        if (!e->isdir) {
            char sz[32];
            size_text(e->size, sz, sizeof sz);
            w2k_list_set(ex.list, r, 1, sz);
        } else w2k_list_set(ex.list, r, 1, "");
        w2k_list_set(ex.list, r, 2, e->type);
        char when[40];
        struct tm tm;
        localtime_r(&e->mtime, &tm);
        int h12 = tm.tm_hour % 12;
        if (!h12) h12 = 12;
        snprintf(when, sizeof when, "%d/%d/%d %d:%02d %s", tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_year + 1900, h12, tm.tm_min,
                 tm.tm_hour < 12 ? "AM" : "PM");
        w2k_list_set(ex.list, r, 3, e->mtime ? when : "");
    }
    list_configure();
    status_update();
}

/* ------------------------------------------------------------------ *
 * Navigation
 * ------------------------------------------------------------------ */
static void update_caption(void)
{
    char nm[1100], t[1200];
    display_name(&ex.cur, nm, sizeof nm);
    if (w2k_folder_fullpath && ex.cur.kind == K_FS)
        snprintf(t, sizeof t, "%s", ex.cur.path);
    else
        snprintf(t, sizeof t, "%s", nm);
    w2k_win_title(ex.win, t);

    w2k_combo_clear(ex.addr);
    w2k_combo_add(ex.addr, ex.cur.kind == K_FS ? ex.cur.path : nm);
    ex.addr->sel = 0;
    if (ex.addr->editable)
        w2k_combo_set_text(ex.addr, ex.cur.kind == K_FS ? ex.cur.path : nm);

    w2k_toolbar_enable(ex.tb, ID_BACK, ex.hist_i > 0);
    w2k_toolbar_enable(ex.tb, ID_FORWARD, ex.hist_i + 1 < ex.hist_n);
    w2k_toolbar_enable(ex.tb, ID_UP,
                       ex.cur.kind != K_DESKTOP &&
                       !(ex.cur.kind == K_FS && !strcmp(ex.cur.path, "/") && 0));
}

/* ------------------------------------------------------------------ *
 * Per-folder view settings
 *
 * "Remember each folder's view settings" -- a folder opened in Details
 * with a sort on Size comes back that way. Windows keeps these in the
 * registry, a few hundred of them; this keeps the same number in
 * ~/.w2k/folders, oldest dropped first.
 * ------------------------------------------------------------------ */
#define VIEWMEM_MAX 200

static struct { char path[1024]; int view, sort_col, sort_dir; } viewmem[VIEWMEM_MAX];
static int nviewmem, viewmem_loaded, viewmem_dirty;

static void viewmem_file(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/.w2k/folders", home ? home : ".");
}

static void viewmem_load(void)
{
    if (viewmem_loaded) return;
    viewmem_loaded = 1;
    char path[1100];
    viewmem_file(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f) && nviewmem < VIEWMEM_MAX) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        int v = 0, c = 0, d = 0;
        if (sscanf(tab + 1, "%d %d %d", &v, &c, &d) < 1) continue;
        if (v < ID_V_LARGE || v > ID_V_DETAILS) continue;
        snprintf(viewmem[nviewmem].path, sizeof viewmem[0].path, "%.1023s", line);
        viewmem[nviewmem].view = v;
        viewmem[nviewmem].sort_col = c;
        viewmem[nviewmem].sort_dir = d != 0;
        nviewmem++;
    }
    fclose(f);
}

static void viewmem_save(void)
{
    if (!viewmem_dirty) return;
    char path[1100];
    viewmem_file(path, sizeof path);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Linux 2000 -- per-folder view settings\n");
    for (int i = 0; i < nviewmem; i++)
        fprintf(f, "%s\t%d %d %d\n", viewmem[i].path, viewmem[i].view,
                viewmem[i].sort_col, viewmem[i].sort_dir);
    fclose(f);
    viewmem_dirty = 0;
}

/* Remember how the folder now on screen is being looked at. */
static void viewmem_store(void)
{
    if (ex.cur.kind != K_FS || !ex.cur.path[0]) return;
    viewmem_load();
    for (int i = 0; i < nviewmem; i++)
        if (!strcmp(viewmem[i].path, ex.cur.path)) {
            if (viewmem[i].view == ex.view &&
                viewmem[i].sort_col == ex.sort_col &&
                viewmem[i].sort_dir == ex.sort_dir)
                return;
            viewmem[i].view = ex.view;
            viewmem[i].sort_col = ex.sort_col;
            viewmem[i].sort_dir = ex.sort_dir;
            viewmem_dirty = 1;
            return;
        }
    if (nviewmem == VIEWMEM_MAX) {
        memmove(viewmem, viewmem + 1, sizeof viewmem - sizeof viewmem[0]);
        nviewmem--;
    }
    snprintf(viewmem[nviewmem].path, sizeof viewmem[0].path, "%s", ex.cur.path);
    viewmem[nviewmem].view = ex.view;
    viewmem[nviewmem].sort_col = ex.sort_col;
    viewmem[nviewmem].sort_dir = ex.sort_dir;
    nviewmem++;
    viewmem_dirty = 1;
}

static void viewmem_apply(const Node *nd)
{
    if (nd->kind != K_FS) return;
    viewmem_load();
    for (int i = 0; i < nviewmem; i++)
        if (!strcmp(viewmem[i].path, nd->path)) {
            ex.view = viewmem[i].view;
            ex.sort_col = viewmem[i].sort_col;
            ex.sort_dir = viewmem[i].sort_dir;
            return;
        }
}

static void navigate(const Node *nd, int record)
{
    w2k_sound_play(SND_NAVIGATING);
    viewmem_store();                 /* the folder we are leaving */
    ex.cur = *nd;
    viewmem_apply(nd);
    if (record) {
        if (ex.hist_i + 1 < ex.hist_n) ex.hist_n = ex.hist_i + 1;
        if (ex.hist_n == 32) {
            memmove(ex.history[0], ex.history[1], 31 * sizeof ex.history[0]);
            memmove(ex.hist_kind, ex.hist_kind + 1, 31 * sizeof ex.hist_kind[0]);
            ex.hist_n--;
        }
        set_path(ex.history[ex.hist_n], sizeof ex.history[0], nd->path);
        ex.hist_kind[ex.hist_n] = nd->kind;
        ex.hist_n++;
        ex.hist_i = ex.hist_n - 1;
    }
    /* A new folder clears the search filter so the listing is not stuck
     * on a previous pattern. */
    ex.search_pat[0] = 0;
    if (ex.search) {
        w2k_edit_set(ex.search, "");
        ex.search->focused = 0;
    }
    refill_list();
    update_caption();
    w2k_win_dirty(ex.win);
}

static void navigate_path(const char *p, int record)
{
    Node nd = { K_FS, { 0 } };
    set_path(nd.path, sizeof nd.path, p);
    navigate(&nd, record);
}

/* Go to whatever is typed in the address bar (Enter). Virtual names
 * resolve the same way the tree does. */
static void addr_go(void)
{
    const char *t = w2k_combo_text(ex.addr);
    if (!t || !t[0]) return;
    if (!strcasecmp(t, "My Computer") || !strcmp(t, "C:") || !strcmp(t, "C:\\")) {
        Node nd = { K_MYCOMPUTER, { 0 } };
        navigate(&nd, 1);
        return;
    }
    if (!strcasecmp(t, "Desktop")) {
        Node nd = { K_DESKTOP, { 0 } };
        navigate(&nd, 1);
        return;
    }
    if (!strcasecmp(t, "My Documents") || !strcmp(t, "~")) {
        navigate_path(ex.home, 1);
        return;
    }
    if (!strcasecmp(t, "Recycle Bin")) {
        Node nd = { K_RECYCLE, { 0 } };
        navigate(&nd, 1);
        return;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s", t);
    /* Expand a leading ~ to $HOME. */
    if (path[0] == '~' && (path[1] == '/' || path[1] == 0)) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s%s", ex.home, path[1] ? path + 1 : "");
        snprintf(path, sizeof path, "%s", tmp);
    }
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        navigate_path(path, 1);
    else {
        char msg[1200];
        snprintf(msg, sizeof msg,
                 "Windows cannot find '%s'. Make sure you typed the name correctly.",
                 t);
        w2k_msgbox(ex.win, "Address Bar", msg, MB_OK | MB_ICONWARNING);
    }
}

/* Tab-complete the address bar via the shared w2k_tabcomp(). */
static void addr_complete(void)
{
    if (!ex.addr->editable || !ex.addr->edit) return;
    const char *t = w2k_combo_text(ex.addr);
    if (!t) return;
    const char *cwd = (ex.cur.kind == K_FS && ex.cur.path[0]) ? ex.cur.path : "/";
    char out[1024];
    if (!w2k_tabcomp(t, cwd, out, sizeof out, W2K_TABCOMP_DIRS)) return;
    w2k_combo_set_text(ex.addr, out);
    int n = (int)strlen(out);
    ex.addr->edit->caret = n;
    ex.addr->edit->sel = n;
    ex.addr->edit->caret_on = 1;
}

static void go_up(void)
{
    if (ex.cur.kind == K_FS) {
        if (!strcmp(ex.cur.path, "/")) {
            Node nd = { K_MYCOMPUTER, { 0 } };
            navigate(&nd, 1);
            return;
        }
        char p[1024];
        set_path(p, sizeof p, ex.cur.path);
        path_parent(p);
        navigate_path(p, 1);
    } else if (ex.cur.kind == K_MYCOMPUTER || ex.cur.kind == K_NETWORK ||
               ex.cur.kind == K_RECYCLE) {
        Node nd = { K_DESKTOP, { 0 } };
        navigate(&nd, 1);
    }
}

/* ------------------------------------------------------------------ *
 * Folders pane
 * ------------------------------------------------------------------ */
static int dir_has_subdirs(const char *path)
{
    DIR *dp = opendir(path);
    if (!dp) return 0;
    struct dirent *de;
    int found = 0;
    while (!found && (de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        char full[2048];
        path_join(full, sizeof full, path, de->d_name);
        struct stat st;
        if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) found = 1;
    }
    closedir(dp);
    return found;
}

static void tree_populate(W2kTreeNode *n)
{
    Node *nd = n->data;
    if (!nd || nd->kind != K_FS) return;
    if (n->child) return;                       /* already filled */

    DIR *dp = opendir(nd->path);
    if (!dp) return;
    char names[512][256];
    int cnt = 0;
    struct dirent *de;
    while (cnt < 512 && (de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        char full[2048];
        path_join(full, sizeof full, nd->path, de->d_name);
        struct stat st;
        if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(names[cnt++], 256, "%s", de->d_name);
    }
    closedir(dp);
    for (int i = 0; i < cnt; i++)
        for (int j = i + 1; j < cnt; j++)
            if (strcmp(names[j], names[i]) < 0) {
                char t[256];
                memcpy(t, names[i], 256);
                memcpy(names[i], names[j], 256);
                memcpy(names[j], t, 256);
            }
    for (int i = 0; i < cnt; i++) {
        Node *c = w2k_alloc(sizeof *c);
        c->kind = K_FS;
        path_join(c->path, sizeof c->path, nd->path, names[i]);
        W2kTreeNode *tn = w2k_tree_add(ex.tree, n, names[i], ICO_FOLDER,
                                       ICO_FOLDER_OPEN, c);
        tn->has_kids = dir_has_subdirs(c->path);
    }
}

static void on_tree_expand(void *u, W2kTreeNode *n) { (void)u; tree_populate(n); }

static void on_tree_select(void *u, W2kTreeNode *n)
{
    (void)u;
    if (!n->data) return;
    navigate((Node *)n->data, 1);
}

static Node *mknode(int kind, const char *path)
{
    Node *n = w2k_alloc(sizeof *n);
    n->kind = kind;
    if (path) snprintf(n->path, sizeof n->path, "%.1023s", path);
    return n;
}

static void tree_build(void)
{
    W2kTreeNode *desk = w2k_tree_add(ex.tree, NULL, "Desktop", ICO_DESKTOP,
                                     ICO_DESKTOP, mknode(K_DESKTOP, ""));
    desk->expanded = 1;
    w2k_tree_add(ex.tree, desk, "My Documents", ICO_MYDOCS, ICO_MYDOCS,
                 mknode(K_FS, ex.home))->has_kids = dir_has_subdirs(ex.home);

    W2kTreeNode *mc = w2k_tree_add(ex.tree, desk, "My Computer", ICO_MYCOMPUTER,
                                   ICO_MYCOMPUTER, mknode(K_MYCOMPUTER, ""));
    mc->expanded = 1;
    W2kTreeNode *c = w2k_tree_add(ex.tree, mc, "Local Disk (C:)", ICO_DRIVE_HDD,
                                  ICO_DRIVE_HDD, mknode(K_FS, "/"));
    c->has_kids = 1;
    W2kDrive dr[16];
    int nd = w2k_fs_drives(dr, 16);
    for (int i = 0; i < nd; i++) {
        char label[256];
        snprintf(label, sizeof label, "%.200s (%c:)", dr[i].label, dr[i].letter);
        int ico = dr[i].optical ? ICO_DRIVE_CD : dr[i].removable ? ICO_DRIVE_FLOPPY : ICO_DRIVE_HDD;
        w2k_tree_add(ex.tree, mc, label, ico, ico, mknode(K_FS, dr[i].path))->has_kids =
            dir_has_subdirs(dr[i].path);
    }

    w2k_tree_add(ex.tree, desk, "My Network Places", ICO_NETWORK, ICO_NETWORK,
                 mknode(K_NETWORK, ""));
    w2k_tree_add(ex.tree, desk, "Recycle Bin", ICO_RECYCLE, ICO_RECYCLE,
                 mknode(K_RECYCLE, ""));
}

/* ------------------------------------------------------------------ *
 * File operations
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ *
 * Undo
 *
 * Windows remembers the last few file operations and offers to reverse
 * them ("Undo Delete", Ctrl+Z). Each entry says what was done and where,
 * which is enough: a delete is undone by restoring from the bin, a move
 * by moving back, a rename by renaming back, and a copy by removing the
 * copy. Anything that cannot be reversed is never pushed.
 * ------------------------------------------------------------------ */
enum { U_NONE = 0, U_RENAME, U_MOVE, U_COPY, U_TRASH, U_NEW };

typedef struct { int op; char from[1024], to[1024]; } Undo;

#define UNDO_MAX 16
static Undo undos[UNDO_MAX];
static int  nundo;

static const char *undo_verb(int op)
{
    switch (op) {
    case U_RENAME: return "Rename";
    case U_MOVE:   return "Move";
    case U_COPY:   return "Copy";
    case U_TRASH:  return "Delete";
    case U_NEW:    return "New";
    }
    return NULL;
}

static void undo_push(int op, const char *from, const char *to)
{
    if (nundo == UNDO_MAX) {
        memmove(undos, undos + 1, sizeof undos - sizeof undos[0]);
        nundo--;
    }
    Undo *u = &undos[nundo++];
    u->op = op;
    snprintf(u->from, sizeof u->from, "%s", from ? from : "");
    snprintf(u->to, sizeof u->to, "%s", to ? to : "");
}


/* Copying a folder copies what is in it. Windows does this without
 * comment; doing anything less means dragging a folder in Explorer
 * quietly produces nothing. */
/* Is `path` the Recycle Bin's folder, or inside it? A prefix match alone
 * would also take a sibling such as Trash-old. */
static int under_dir(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return n && !strncmp(path, dir, n) && (path[n] == '/' || path[n] == 0);
}

static int copy_tree(const char *from, const char *to, int depth)
{
    (void)depth;
    return w2k_fs_copy_tree(from, to);
}


/* Delete a whole tree -- the second half of a move that had to fall back
 * to copy-and-delete because the two paths are on different filesystems. */
static int remove_tree(const char *path, int depth)
{
    (void)depth;
    return w2k_fs_remove_tree(path);
}


static void selected_paths(char out[][1024], int max, int *n)
{
    *n = 0;
    for (int i = 0; i < ex.list->n && *n < max; i++)
        if (ex.list->items[i].selected && i < nentries)
            path_join(out[(*n)++], 1024, ex.cur.path, entries[i].name);
}

/* Delete moves to the Recycle Bin; holding Shift destroys instead, exactly
 * as in Windows. Deleting something that is already in the bin is always
 * permanent -- there is nowhere further for it to go. */
static void do_delete_ex(int permanent)
{
    if (ex.cur.kind != K_FS) return;
    char paths[64][1024];
    int n;
    selected_paths(paths, 64, &n);
    if (!n) return;

    if (under_dir(ex.cur.path, w2k_trash_dir()))
        permanent = 1;

    char msg[1200];
    const char *what = permanent ? "permanently delete" : "send";
    const char *where = permanent ? "" : " to the Recycle Bin";
    if (n == 1) snprintf(msg, sizeof msg,
                         "Are you sure you want to %s '%s'%s?",
                         what, strrchr(paths[0], '/') + 1, where);
    else snprintf(msg, sizeof msg,
                  "Are you sure you want to %s these %d items%s?",
                  what, n, where);
    if (w2k_msgbox(ex.win, "Confirm File Delete", msg,
                   MB_YESNO | MB_ICONQUESTION) != ID_YES)
        return;

    for (int i = 0; i < n; i++) {
        struct stat st;
        int rc;
        char binname[512] = "";
        if (!permanent) {
            rc = w2k_trash_move_named(paths[i], binname, sizeof binname);
            if (rc == 0) undo_push(U_TRASH, binname, paths[i]);
        } else {
            rc = (lstat(paths[i], &st) == 0 && S_ISDIR(st.st_mode))
               ? rmdir(paths[i]) : unlink(paths[i]);
        }
        if (rc != 0) {
            char e[1300];
            snprintf(e, sizeof e, "Cannot delete %s.\n\n%s",
                     strrchr(paths[i], '/') + 1, strerror(errno));
            w2k_msgbox(ex.win, "Error Deleting File or Folder", e,
                       MB_OK | MB_ICONERROR);
            break;
        }
    }
    refill_list();
}

static void do_delete(void) { do_delete_ex(0); }

static void do_rename(void)
{
    if (ex.cur.kind != K_FS || ex.list->sel < 0 || ex.list->sel >= nentries) return;
    const char *old = entries[ex.list->sel].name;
    char out[256];
    if (!w2k_prompt(ex.win, "Rename", "&New name:", old, out, sizeof out,
                    ICO_NONE))
        return;
    if (!out[0] || !strcmp(out, old)) return;
    char a[2048], b[2048];
    path_join(a, sizeof a, ex.cur.path, old);
    path_join(b, sizeof b, ex.cur.path, out);
    if (rename(a, b) != 0) {
        char e[1300];
        snprintf(e, sizeof e, "Cannot rename %s.\n\n%s", old, strerror(errno));
        w2k_msgbox(ex.win, "Error Renaming File or Folder", e,
                   MB_OK | MB_ICONERROR);
    } else undo_push(U_RENAME, a, b);
    refill_list();
}

static void do_newfolder(void)
{
    if (ex.cur.kind != K_FS) return;
    char out[256];
    if (!w2k_prompt(ex.win, "New Folder", "&Folder name:", "New Folder", out,
                    sizeof out, ICO_FOLDER))
        return;
    char p[2048];
    path_join(p, sizeof p, ex.cur.path, out);
    if (mkdir(p, 0755) != 0) {
        char e[1300];
        snprintf(e, sizeof e, "Cannot create folder.\n\n%s", strerror(errno));
        w2k_msgbox(ex.win, "Windows Explorer", e, MB_OK | MB_ICONERROR);
    } else undo_push(U_NEW, p, NULL);
    refill_list();
}

/* File > New. Windows fills this submenu from the registry's ShellNew
 * keys; here it is the three entries every installation had. */
static void do_new_shortcut(void)
{
    if (ex.cur.kind != K_FS) return;
    char cmd[512];
    if (!w2k_prompt(ex.win, "Create Shortcut",
                    "Type the &location of the item:", "", cmd, sizeof cmd,
                    ICO_APP))
        return;
    if (!cmd[0]) return;

    char name[256];
    const char *base = strrchr(cmd, '/');
    base = base ? base + 1 : cmd;
    snprintf(name, sizeof name, "%.200s", base);
    char *sp = strchr(name, ' ');
    if (sp) *sp = 0;
    if (!w2k_prompt(ex.win, "Select a Title for the Program",
                    "Type a &name for this shortcut:", name, name, sizeof name,
                    ICO_APP))
        return;
    if (!name[0]) return;

    char link[2048];
    path_join(link, sizeof link, ex.cur.path, name);
    size_t l = strlen(link);
    snprintf(link + l, sizeof link - l, ".desktop");
    FILE *f = fopen(link, "w");
    if (!f) {
        char e[1300];
        snprintf(e, sizeof e, "Cannot create the shortcut.\n\n%s",
                 strerror(errno));
        w2k_msgbox(ex.win, "Windows Explorer", e, MB_OK | MB_ICONERROR);
        return;
    }
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n"
               "Terminal=false\n", name, cmd);
    fclose(f);
    undo_push(U_NEW, link, NULL);
    refill_list();
}

static void do_new_text(void)
{
    if (ex.cur.kind != K_FS) return;
    char p[2048];
    path_join(p, sizeof p, ex.cur.path, "New Text Document.txt");
    struct stat st;
    for (int k = 2; k < 100 && lstat(p, &st) == 0; k++) {
        char name[64];
        snprintf(name, sizeof name, "New Text Document (%d).txt", k);
        path_join(p, sizeof p, ex.cur.path, name);
    }
    FILE *f = fopen(p, "w");
    if (!f) {
        char e[1300];
        snprintf(e, sizeof e, "Cannot create the document.\n\n%s",
                 strerror(errno));
        w2k_msgbox(ex.win, "Windows Explorer", e, MB_OK | MB_ICONERROR);
        return;
    }
    fclose(f);
    undo_push(U_NEW, p, NULL);
    refill_list();
}

static int drop_confirm(const char *dst, void *u);

static void do_paste(void)
{
    if (ex.cur.kind != K_FS || !ex.nclip) return;
    XDefineCursor(w2k.dpy, ex.win->win, w2k.cur_wait);
    XFlush(w2k.dpy);
    for (int i = 0; i < ex.nclip; i++) {
        const char *base = strrchr(ex.clip[i], '/');
        base = base ? base + 1 : ex.clip[i];
        char dst[1024];
        path_join(dst, sizeof dst, ex.cur.path, base);
        if (!strcmp(dst, ex.clip[i])) continue;
        /* One at a time, so each lands in the undo list; a name already
         * here asks first, as in Windows. */
        struct stat st;
        if (lstat(dst, &st) == 0) {
            int c = drop_confirm(dst, NULL);
            if (c < 0) break;
            if (c == 0) continue;
            w2k_fs_remove_tree(dst);
        }
        int ok = ex.clip_cut ? w2k_fs_move(ex.clip[i], dst)
                             : w2k_fs_copy_tree(ex.clip[i], dst);
        if (ok) undo_push(ex.clip_cut ? U_MOVE : U_COPY, ex.clip[i], dst);
        if (!ok) {
            char e[1300];
            snprintf(e, sizeof e, "Cannot %s %s.\n\n%s",
                     ex.clip_cut ? "move" : "copy", base, strerror(errno));
            w2k_msgbox(ex.win, "Error Copying File", e, MB_OK | MB_ICONERROR);
            break;
        }
    }
    XDefineCursor(w2k.dpy, ex.win->win, None);
    if (ex.clip_cut) ex.nclip = 0;
    refill_list();
}

static void spawn(const char *fmt, const char *arg);

/* "Create Shortcut" writes a .desktop beside the file -- this system's
 * .lnk. Send To > Desktop is the same operation aimed at ~/Desktop. */
static void do_create_shortcut(const char *into)
{
    char paths[16][1024];
    int n;
    selected_paths(paths, 16, &n);
    if (!n) return;

    for (int i = 0; i < n; i++) {
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];

        struct stat st;
        int isdir = lstat(paths[i], &st) == 0 && S_ISDIR(st.st_mode);

        char link[2048];
        snprintf(link, sizeof link, "%.900s/%.100s.desktop", into, base);
        for (int k = 2; k < 100 && lstat(link, &st) == 0; k++)
            snprintf(link, sizeof link, "%.900s/%.100s (%d).desktop",
                     into, base, k);

        FILE *f = fopen(link, "w");
        if (!f) {
            char e[1300];
            snprintf(e, sizeof e, "Cannot create a shortcut here.\n\n%s",
                     strerror(errno));
            w2k_msgbox(ex.win, "Windows Explorer", e, MB_OK | MB_ICONERROR);
            return;
        }
        /* A folder opens in Explorer; a file goes to whatever opens it. */
        char cmd[4400], q[4200];
        w2k_shell_quote(paths[i], q, sizeof q);
        if (isdir) snprintf(cmd, sizeof cmd, "l2kexplorer %s", q);
        else       w2k_assoc_command(paths[i], cmd, sizeof cmd);
        fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n"
                   "Terminal=false\n", base, cmd);
        fclose(f);
        chmod(link, 0755);                   /* a shortcut we made is trusted */
        undo_push(U_NEW, link, NULL);
    }
    refill_list();
}

static void do_send_to_mydocs(void)
{
    char paths[16][1024];
    int n;
    selected_paths(paths, 16, &n);
    if (!n) return;
    for (int i = 0; i < n; i++) {
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        char dst[2048];
        snprintf(dst, sizeof dst, "%.900s/%.120s", ex.home, base);
        if (!strcmp(dst, paths[i])) continue;
        if (copy_tree(paths[i], dst, 0)) undo_push(U_COPY, paths[i], dst);
        else {
            char e[1300];
            snprintf(e, sizeof e, "Cannot copy %.120s.\n\n%s", base,
                     strerror(errno));
            w2k_msgbox(ex.win, "Error Copying File", e, MB_OK | MB_ICONERROR);
            break;
        }
    }
    refill_list();
}

/* Open With: the command to run, offered with what this kind of file
 * normally opens in. Setting it as the default writes the association,
 * which is the "Always use this program" check box. */
static void do_open_with(void)
{
    if (ex.list->sel < 0 || ex.list->sel >= nentries) return;
    char full[2048];
    path_join(full, sizeof full, ex.cur.path, entries[ex.list->sel].name);

    char cur[256], out[256];
    w2k_assoc_get(w2k_assoc_class_for(full), cur, sizeof cur);
    char label[128];
    snprintf(label, sizeof label, "&Open %.60s with:",
             entries[ex.list->sel].name);
    if (!w2k_prompt(ex.win, "Open With", label, cur, out, sizeof out,
                    ICO_QUESTION))
        return;
    if (!out[0]) return;

    char cmd[4400], q[4200];
    w2k_shell_quote(full, q, sizeof q);
    if (strstr(out, "%s")) w2k_splice(out, q, cmd, sizeof cmd);
    else                   snprintf(cmd, sizeof cmd, "%s %s", out, q);
    spawn("%s", cmd);
}

/* Reverse the last operation. Failures are reported rather than
 * swallowed: an undo that silently does nothing is worse than none. */
static void do_undo(void)
{
    if (!nundo) return;
    Undo u = undos[--nundo];
    int ok = 1;
    switch (u.op) {
    case U_RENAME:
    case U_MOVE:
        ok = rename(u.to, u.from) == 0;
        if (!ok && copy_tree(u.to, u.from, 0)) {
            remove_tree(u.to, 0);
            ok = 1;
        }
        break;
    case U_COPY:
        /* The copy goes to the Recycle Bin, not away for good: what is
         * there now may not be what was copied. */
        ok = w2k_trash_move(u.to) == 0;
        break;
    case U_TRASH:
        ok = w2k_trash_restore(u.from) == 0;
        break;
    case U_NEW:
        /* Only the empty thing that was made: a folder that has since
         * been filled stays (rmdir refuses it). */
        ok = rmdir(u.from) == 0 || unlink(u.from) == 0;
        break;
    }
    if (!ok) {
        char e[1300];
        snprintf(e, sizeof e, "Cannot undo the %s.\n\n%s",
                 undo_verb(u.op), strerror(errno));
        w2k_msgbox(ex.win, "Windows Explorer", e, MB_OK | MB_ICONERROR);
    }
    refill_list();
}

static void do_properties(void)
{
    if (ex.list->sel < 0 || ex.list->sel >= nentries) return;
    Entry *e = &entries[ex.list->sel];
    if (ex.cur.kind != K_FS) return;         /* virtual folders have none */
    char full[2048];
    path_join(full, sizeof full, ex.cur.path, e->name);
    if (w2k_file_properties(ex.win, full)) refill_list();
}

/* ------------------------------------------------------------------ *
 * Activation
 * ------------------------------------------------------------------ */
static void spawn(const char *fmt, const char *arg)
{
    char cmd[4400];
    snprintf(cmd, sizeof cmd, fmt, arg);
    /* Forked twice: the grandchild is init's to reap, so a long-lived
     * Explorer does not collect a zombie for everything it ever opened. */
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            close(ConnectionNumber(w2k.dpy));
            setsid();
            execlp("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
}

static void on_activate(void *u, int idx)
{
    (void)u;
    if (idx < 0 || idx >= nentries) return;
    Entry *e = &entries[idx];

    if (ex.cur.kind == K_DESKTOP || ex.cur.kind == K_MYCOMPUTER) {
        Node nd = { K_FS };
        if (!strcmp(e->target, "@controlpanel")) { spawn("%s", "l2kcontrol"); return; }
        if (e->target[0]) { navigate_path(e->target, 1); return; }
        if (!strcmp(e->name, "Local Disk (C:)")) snprintf(nd.path, sizeof nd.path, "/");
        else if (!strcmp(e->name, "My Documents")) snprintf(nd.path, sizeof nd.path, "%s", ex.home);
        else if (!strcmp(e->name, "My Computer")) { nd.kind = K_MYCOMPUTER; }
        else if (!strcmp(e->name, "My Network Places")) { nd.kind = K_NETWORK; }
        else if (!strcmp(e->name, "Recycle Bin")) { nd.kind = K_RECYCLE; }
        else return;
        navigate(&nd, 1);
        return;
    }
    char full[2048];
    path_join(full, sizeof full, ex.cur.path, e->name);
    char q[4200];
    w2k_shell_quote(full, q, sizeof q);
    if (e->isdir) {
        if (w2k_folder_newwindow) spawn("l2kexplorer %s", q);
        else navigate_path(full, 1);
        return;
    }

    /* A .desktop file is a shortcut: run what it points at, the way
     * double-clicking a .lnk does, rather than opening the file itself.
     * Only a shortcut marked executable is trusted that far (the same
     * rule every desktop applies); any other opens as the text it is. */
    if (!strcasecmp(w2k_file_ext(e->name), "desktop") && e->link) {
        char cmd[1024];
        if (w2k_desktop_entry(full, NULL, 0, cmd, sizeof cmd, NULL, 0)) {
            spawn("%s", cmd);
            return;
        }
    }

    /* Whatever the Control Panel says opens this kind of file -- pictures
     * in the viewer, video in VLC, and so on. An executable still runs. */
    if (access(full, X_OK) == 0 && !w2k_image_is_image(full) &&
        strcasecmp(w2k_file_ext(e->name), "desktop") &&
        strcmp(w2k_assoc_class_for(full), "video") &&
        strcmp(w2k_assoc_class_for(full), "audio")) {
        spawn("%s", q);
    } else {
        char cmd[2048];
        w2k_assoc_command(full, cmd, sizeof cmd);
        spawn("%s", cmd);
    }
}

static void on_select(void *u, int idx) { (void)u; (void)idx; status_update(); }

static void on_sort(void *u, int col)
{
    (void)u;
    if (ex.sort_col == col) ex.sort_dir = !ex.sort_dir;
    else { ex.sort_col = col; ex.sort_dir = 0; }
    viewmem_store();
    refill_list();
    w2k_win_dirty(ex.win);
}

/* ------------------------------------------------------------------ *
 * Menus
 * ------------------------------------------------------------------ */
/* Is the Recycle Bin what the user is looking at, or what they just clicked?
 * Either way the Empty command belongs on the menu. */
static int recycle_in_view(void)
{
    if (ex.cur.kind == K_RECYCLE) return 1;
    if (ex.cur.kind == K_FS &&
        under_dir(ex.cur.path, w2k_trash_dir()))
        return 1;
    if (ex.list->sel >= 0 && ex.list->sel < ex.list->n) {
        const char *t = ex.list->items[ex.list->sel].text[0];
        if (t && !strcmp(t, "Recycle Bin")) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Drag and drop
 * ------------------------------------------------------------------ *
 * Dragging out of the list offers the selected files as a URI list, which
 * any application that speaks XDND understands. Dropping onto the list
 * moves files here -- or copies them, when they come from another
 * filesystem or the source only offered a copy.
 *
 * The drag starts on the first motion after a press on a selected row, so
 * an ordinary click still selects and a double click still opens. */
static int drag_armed, drag_from_x, drag_from_y;

/* Where a drop landed: a folder row takes the files, otherwise this folder
 * does. Returns 0 if the drop should be refused. */
static int drop_target_dir(int x, int y, char *out, int n)
{
    if (ex.cur.kind != K_FS) return 0;
    int row = w2k_list_hit(ex.list, x, y);
    if (row >= 0 && row < nentries && entries[row].isdir) {
        path_join(out, (size_t)n, ex.cur.path, entries[row].name);
        return 1;
    }
    snprintf(out, (size_t)n, "%s", ex.cur.path);
    return 1;
}

/* The folder under a point in the Folders bar, walking the rows the tree
 * shows: expanded nodes' children follow them. */
static W2kTreeNode *tree_node_at_row(W2kTreeNode *n, int *row, int want)
{
    for (; n; n = n->sibling) {
        if (*row == want) return n;
        (*row)++;
        if (n->expanded && n->child) {
            W2kTreeNode *hit = tree_node_at_row(n->child, row, want);
            if (hit) return hit;
        }
    }
    return NULL;
}

static int tree_drop_dir(int x, int y, char *out, int n)
{
    if (!ex.show_tree || !w2k_rect_hit(&ex.tree->r, x, y) || ex.tree->row_h <= 0) return 0;
    int want = (y - ex.tree->r.y) / ex.tree->row_h + ex.tree->top, row = 0;
    W2kTreeNode *tn = tree_node_at_row(ex.tree->root ? ex.tree->root->child : NULL, &row, want);
    if (!tn || !tn->data) return 0;
    Node *nd = tn->data;
    if (nd->kind != K_FS) return 0;
    snprintf(out, (size_t)n, "%s", nd->path);
    return 1;
}

static int ex_will_accept(Window w, int x, int y)
{
    (void)w;
    char dir[1024];
    return tree_drop_dir(x, y, dir, sizeof dir) || drop_target_dir(x, y, dir, sizeof dir);
}

/* "This folder already contains a file named X": replace, skip, or stop. */
static int drop_confirm(const char *dst, void *u)
{
    (void)u;
    const char *base = strrchr(dst, '/');
    char msg[600];
    snprintf(msg, sizeof msg, "This folder already contains a file named '%.200s'.\n\n"
             "Would you like to replace the existing file?", base ? base + 1 : dst);
    int r = w2k_msgbox(ex.win, "Confirm File Replace", msg, MB_YESNOCANCEL | MB_ICONQUESTION);
    return r == ID_YES ? 1 : r == ID_NO ? 0 : -1;
}

static void ex_on_drop(Window w, int x, int y, const char *uris, int move)
{
    (void)w;
    char dir[1024];
    if (!tree_drop_dir(x, y, dir, sizeof dir) && !drop_target_dir(x, y, dir, sizeof dir)) return;

    char paths[64][1024];
    int n = w2k_uri_list_paths(uris, paths, 64);
    /* Moving within one folder is a no-op; across folders, files go where
     * they are dropped. Dragging with Ctrl held copies instead, as in
     * Windows. */
    Window rw, cw;
    int rxp, ryp, wx, wy;
    unsigned mask = 0;
    XQueryPointer(w2k.dpy, w2k.root, &rw, &cw, &rxp, &ryp, &wx, &wy, &mask);
    if (mask & ControlMask) move = 0;
    XDefineCursor(w2k.dpy, ex.win->win, w2k.cur_wait);
    XFlush(w2k.dpy);
    int done = w2k_fs_transfer(paths, n, dir, move, drop_confirm, NULL);
    /* A link dragged out of a browser becomes an Internet shortcut. */
    char urls[16][1024];
    int nu = w2k_uri_list_urls(uris, urls, 16);
    for (int i = 0; i < nu; i++) done += w2k_fs_write_url_shortcut(dir, urls[i]);
    XDefineCursor(w2k.dpy, ex.win->win, None);
    if (done) refill_list();
}

/* ------------------------------------------------------------------ *
 * Archives: Add to Zip and Extract
 *
 * zip, unzip, tar and 7z do the work, in a child whose output is read
 * through a pipe as it runs: each file it reports moves the progress
 * bar, so a big archive shows what it is doing instead of freezing the
 * window. Before that, a dialog takes the name and the options. */

static int is_archive(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    static const char *ext[] = { ".zip", ".jar", ".tar", ".tgz", ".gz", ".bz2",
                                 ".xz", ".7z", ".rar", NULL };
    for (int i = 0; ext[i]; i++)
        if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

/* Quote one path for the shell: everything but a single quote goes through
 * as is, and a quote is spliced in as '\''. */
static void shell_quote(const char *in, char *out, int n)
{
    int o = 0;
    if (o < n - 1) out[o++] = '\'';
    for (const char *p = in; *p && o < n - 5; p++) {
        if (*p == '\'') {
            out[o++] = '\''; out[o++] = '\\'; out[o++] = '\''; out[o++] = '\'';
        } else out[o++] = *p;
    }
    if (o < n - 1) out[o++] = '\'';
    out[o] = 0;
}

/* How much is about to be archived: files, folders and bytes. */
static void count_tree(const char *path, int recurse, int *files, int *dirs,
                       long long *bytes, int depth)
{
    struct stat st;
    if (depth > 32 || lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { (*files)++; *bytes += st.st_size; return; }
    (*dirs)++;
    if (!recurse) return;
    DIR *dp = opendir(path);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[2048];
        path_join(sub, sizeof sub, path, de->d_name);
        count_tree(sub, 1, files, dirs, bytes, depth + 1);
    }
    closedir(dp);
}

/* ---- the progress window ---- */
typedef struct {
    W2kWin  *win;
    pid_t    pid;
    int      fd;                 /* the child's output */
    char     line[1024];
    int      linelen;
    char     tail[2048];         /* its last words, for an error box */
    int      done, total, phase, cancelled, exited, status;
    char     current[300];
    char     from[300], to[300];
    W2kRect  bar, cancel;
    int      down;
} Progress;

static void progress_paint(W2kWin *w, Drawable d)
{
    Progress *p = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_bigicon_draw(d, 14, 14, ICO_FILE_ZIP);
    /* An ellipsis on whatever will not fit the line. */
    char buf[400];
    w2k_ellipsis(F_UI, p->current, w->w - 70, buf, sizeof buf);
    w2k_text(d, F_UI, 58, 14, buf, C_TEXT);
    char fr[400], to[400];
    snprintf(fr, sizeof fr, "From:  %s", p->from);
    snprintf(to, sizeof to, "To:  %s", p->to);
    w2k_ellipsis(F_UI, fr, w->w - 70, buf, sizeof buf);
    w2k_text(d, F_UI, 58, 14 + fh + 6, buf, C_TEXT);
    w2k_ellipsis(F_UI, to, w->w - 70, buf, sizeof buf);
    w2k_text(d, F_UI, 58, 14 + 2 * (fh + 6), buf, C_TEXT);
    int pct = p->total > 0 ? (p->done * 100) / p->total : -1;
    if (pct > 100) pct = 100;
    w2k_draw_progress(d, &p->bar, pct, p->phase);
    if (p->total > 0)
        snprintf(buf, sizeof buf, "%d of %d  (%d%%)", p->done < p->total ? p->done : p->total,
                 p->total, pct);
    else
        snprintf(buf, sizeof buf, "%d file(s)", p->done);
    w2k_text(d, F_UI, p->bar.x, p->bar.y + p->bar.h + 6, buf, C_TEXT);
    w2k_draw_pushbutton(d, &p->cancel, "Cancel", p->down ? BS_PRESSED : 0);
}

/* A line of the tool's output: zip says "  adding: name (deflated 12%)",
 * unzip "  inflating: name", tar just "name", 7z "- name". */
static void progress_line(Progress *p, char *line)
{
    while (*line == ' ') line++;
    static const char *const prefixes[] = { "adding:", "inflating:", "extracting:",
                                            "creating:", "updating:", "- ", "+ ", "Extracting", NULL };
    char *name = line;
    for (int i = 0; prefixes[i]; i++)
        if (!strncmp(line, prefixes[i], strlen(prefixes[i]))) {
            name = line + strlen(prefixes[i]);
            break;
        }
    while (*name == ' ') name++;
    char *paren = strstr(name, " (");
    if (paren) *paren = 0;
    if (!*name) return;
    p->done++;
    snprintf(p->current, sizeof p->current, "%.299s", name);
    size_t tl = strlen(p->tail), ll = strlen(line);
    if (tl + ll + 2 > sizeof p->tail) { memmove(p->tail, p->tail + tl / 2, tl - tl / 2 + 1); tl = strlen(p->tail); }
    snprintf(p->tail + tl, sizeof p->tail - tl, "%s\n", line);
}

static void progress_tick(void *u)
{
    Progress *p = u;
    p->phase++;
    if (p->fd >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(p->fd, buf, sizeof buf)) > 0)
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    p->line[p->linelen] = 0;
                    if (p->linelen) progress_line(p, p->line);
                    p->linelen = 0;
                } else if (p->linelen < (int)sizeof p->line - 1)
                    p->line[p->linelen++] = buf[i];
            }
        if (n == 0) { close(p->fd); p->fd = -1; }
    }
    if (!p->exited && p->pid > 0) {
        int st;
        if (waitpid(p->pid, &st, WNOHANG) == p->pid) {
            p->exited = 1;
            p->status = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
        }
    }
    if (p->exited && p->fd < 0) { w2k_win_close(p->win, ID_OK); return; }
    w2k_win_dirty(p->win);
}

static int progress_event(W2kWin *w, XEvent *e)
{
    Progress *p = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_rect_hit(&p->cancel, e->xbutton.x, e->xbutton.y)) p->down = 1;
        w2k_win_dirty(w);
        return 1;
    case ButtonRelease:
        if (p->down && w2k_rect_hit(&p->cancel, e->xbutton.x, e->xbutton.y)) {
            p->cancelled = 1;
            if (p->pid > 0) kill(-p->pid, SIGTERM);
            w2k_win_close(w, ID_CANCEL);
        }
        p->down = 0;
        w2k_win_dirty(w);
        return 1;
    case KeyPress:
        if (XLookupKeysym(&e->xkey, 0) == XK_Escape) {
            p->cancelled = 1;
            if (p->pid > 0) kill(-p->pid, SIGTERM);
            w2k_win_close(w, ID_CANCEL);
        }
        return 1;
    }
    return 0;
}

/* Run `cmd` with the progress window up. Returns 1 on success, 0 when it
 * failed (the error is shown) or was cancelled. */
static int run_with_progress(const char *title, const char *cmd, int total,
                             const char *from, const char *to)
{
    Progress p;
    memset(&p, 0, sizeof p);
    p.total = total;
    p.fd = -1;
    snprintf(p.from, sizeof p.from, "%.299s", from);
    snprintf(p.to, sizeof p.to, "%.299s", to);
    snprintf(p.current, sizeof p.current, "Preparing...");

    int pipefd[2];
    if (pipe(pipefd) != 0) return 0;
    p.pid = fork();
    if (p.pid == 0) {
        setpgid(0, 0);           /* Cancel stops the tool, not just the shell */
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (p.pid < 0) { close(pipefd[0]); return 0; }
    p.fd = pipefd[0];
    fcntl(p.fd, F_SETFL, fcntl(p.fd, F_GETFL) | O_NONBLOCK);

    int cw = 420, ch = 176;
    p.win = w2k_win_new(title, "l2kexplorer", cw, ch, 0);
    p.bar = (W2kRect){ 14, 92, cw - 28, 18 };
    p.cancel = (W2kRect){ cw - 12 - 75, ch - 12 - 23, 75, 23 };
    p.win->user = &p;
    p.win->paint = progress_paint;
    p.win->event = progress_event;
    w2k_win_center(p.win, ex.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, p.win->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_add_timer(80, progress_tick, &p);
    w2k_win_modal(p.win);
    w2k_del_timer(progress_tick, &p);
    if (p.fd >= 0) close(p.fd);
    if (p.pid > 0 && !p.exited) { int st; waitpid(p.pid, &st, 0); }

    if (p.cancelled) return 0;
    if (p.status != 0) {
        char msg[2400];
        snprintf(msg, sizeof msg, "The operation did not complete.\n\n%.2000s", p.tail);
        w2k_msgbox(ex.win, title, msg, MB_OK | MB_ICONERROR);
        return 0;
    }
    return 1;
}

/* ---- the Add to Zip / Extract dialog ---- */
/* The archive formats on offer: the tool that writes each, its extension,
 * and how the compression level is spelt for it. */
static const struct { const char *label, *ext, *tool; } formats[] = {
    { "Zip (*.zip)",            ".zip",     "zip" },
    { "7-Zip (*.7z)",           ".7z",      "7z"  },
    { "Tar (*.tar)",            ".tar",     "tar" },
    { "Gzipped tar (*.tar.gz)", ".tar.gz",  "tar" },
    { "Bzip2 tar (*.tar.bz2)",  ".tar.bz2", "tar" },
    { "XZ tar (*.tar.xz)",      ".tar.xz",  "tar" },
};
#define NFORMATS ((int)(sizeof formats / sizeof *formats))

static int tool_installed(const char *name)
{
    static const char *const dirs[] = { "/usr/local/bin", "/usr/bin", "/bin", NULL };
    for (int i = 0; dirs[i]; i++) {
        char p[300];
        snprintf(p, sizeof p, "%s/%s", dirs[i], name);
        if (access(p, X_OK) == 0) return 1;
    }
    return 0;
}

/* Strip the archive extension off a name, whichever format's it is. */
static void strip_archive_ext(char *name)
{
    for (int i = 0; i < NFORMATS; i++) {
        size_t l = strlen(name), e = strlen(formats[i].ext);
        if (l > e && !strcasecmp(name + l - e, formats[i].ext)) { name[l - e] = 0; return; }
    }
}

typedef struct {
    W2kWin   *win;
    W2kEdit  *name;
    W2kCombo *level, *format;
    W2kRect   format_r;
    int       extract;
    int       chk[3];
    const char *chk_label[3];
    W2kRect   chk_r[3], browse, ok, cancel, level_r;
    char      info[200];
    int       down;
} ArcDlg;

static void arc_on_format(void *u, int i)
{
    ArcDlg *a = u;
    if (i < 0 || i >= NFORMATS) return;
    char name[1024];
    snprintf(name, sizeof name, "%s", w2k_edit_text(a->name));
    strip_archive_ext(name);
    strncat(name, formats[i].ext, sizeof name - strlen(name) - 1);
    w2k_edit_set(a->name, name);
    a->name->caret = a->name->sel = (int)strlen(name);
    w2k_win_dirty(a->win);
}

static void arc_paint(W2kWin *w, Drawable d)
{
    ArcDlg *a = w->user;
    int fh = w2k_font_height(F_UI);
    W2kRect g1 = { 12, 10, w->w - 24, 62 };
    w2k_draw_groupbox(d, &g1, a->extract ? "Destination" : "Archive");
    w2k_text_mnemonic(d, F_UI, 24, a->name->r.y + (21 - fh) / 2,
                      a->extract ? "E&xtract to:" : "Archive &name:", C_TEXT, 1);
    w2k_edit_draw(d, a->name);
    w2k_draw_pushbutton(d, &a->browse, "&Browse...", a->down == 3 ? BS_PRESSED : 0);
    W2kRect g2 = { 12, 80, w->w - 24, a->extract ? 96 : 152 };
    w2k_draw_groupbox(d, &g2, "Options");
    if (!a->extract) {
        w2k_text_mnemonic(d, F_UI, 24, a->format_r.y + (21 - fh) / 2, "&Format:", C_TEXT, 1);
        w2k_combo_draw(d, a->format);
        w2k_text_mnemonic(d, F_UI, 24, a->level_r.y + (21 - fh) / 2, "&Compression:", C_TEXT, 1);
        w2k_combo_draw(d, a->level);
    }
    for (int i = 0; i < 3; i++)
        w2k_draw_checkbox(d, a->chk_r[i].x, a->chk_r[i].y, a->chk_label[i], a->chk[i], 0, 0);
    w2k_bigicon_draw(d, 20, g2.y + g2.h + 14, ICO_FILE_ZIP);
    w2k_text(d, F_UI, 62, g2.y + g2.h + 14 + (32 - fh) / 2, a->info, C_TEXT);
    w2k_draw_pushbutton(d, &a->ok, a->extract ? "&Extract" : "&Add",
                        BS_DEFAULT | (a->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &a->cancel, "Cancel", a->down == 2 ? BS_PRESSED : 0);
}

static int arc_event(W2kWin *w, XEvent *e)
{
    ArcDlg *a = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_edit_press(a->name, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (!a->extract && w2k_combo_press(a->format, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (!a->extract && w2k_combo_press(a->level, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        for (int i = 0; i < 3; i++)
            if (w2k_rect_hit(&a->chk_r[i], x, y)) { a->chk[i] = !a->chk[i]; w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&a->ok, x, y)) a->down = 1;
        else if (w2k_rect_hit(&a->cancel, x, y)) a->down = 2;
        else if (w2k_rect_hit(&a->browse, x, y)) a->down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int x = e->xbutton.x, y = e->xbutton.y, b = a->down;
        a->down = 0;
        w2k_edit_release(a->name);
        if (b == 1 && w2k_rect_hit(&a->ok, x, y)) w2k_win_close(w, ID_OK);
        else if (b == 2 && w2k_rect_hit(&a->cancel, x, y)) w2k_win_close(w, ID_CANCEL);
        else if (b == 3 && w2k_rect_hit(&a->browse, x, y)) {
            char path[1024];
            snprintf(path, sizeof path, "%s", w2k_edit_text(a->name));
            if (a->extract) {
                /* Pick anything inside the folder wanted; its folder is taken. */
                if (w2k_file_dialog(w, 0, path, sizeof path)) {
                    struct stat st;
                    if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
                        char *s = strrchr(path, '/');
                        if (s && s != path) *s = 0;
                    }
                    w2k_edit_set(a->name, path);
                }
            } else if (w2k_file_dialog_filter(w, 1, path, sizeof path,
                                              "Archives (*.zip;*.7z;*.tar;*.tar.gz;*.tar.bz2;*.tar.xz)|"
                                              "*.zip;*.7z;*.tar;*.tar.gz;*.tar.bz2;*.tar.xz|All Files (*.*)|*"))
                w2k_edit_set(a->name, path);
        }
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (w2k_edit_motion(a->name, &e->xmotion)) w2k_win_dirty(w);
        return 1;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { w2k_win_close(w, ID_OK); return 1; }
        if (w2k_edit_key(a->name, &e->xkey)) w2k_win_dirty(w);
        return 1;
    }
    }
    return 0;
}

static int arc_dialog(ArcDlg *a, const char *title, const char *initial)
{
    int cw = 440, ch = a->extract ? 296 : 352;
    a->win = w2k_win_new(title, "l2kexplorer", cw, ch, 0);
    a->name = w2k_edit_new(0);
    a->name->r = (W2kRect){ 118, 34, cw - 118 - 12 - 86, 21 };
    w2k_edit_bind(a->name, a->win);
    w2k_edit_set(a->name, initial);
    a->name->caret = a->name->sel = (int)strlen(initial);
    a->browse = (W2kRect){ cw - 12 - 80, 33, 80, 23 };
    int y = 104;
    if (!a->extract) {
        a->format = w2k_combo_new(0);
        for (int i = 0; i < NFORMATS; i++) {
            char lbl[80];
            snprintf(lbl, sizeof lbl, "%s%s", formats[i].label,
                     tool_installed(formats[i].tool) ? "" : "  (not installed)");
            w2k_combo_add(a->format, lbl);
        }
        a->format->sel = 0;
        a->format->user = a;
        a->format->on_change = arc_on_format;
        a->format_r = (W2kRect){ 118, y, 200, 21 };
        a->format->r = a->format_r;
        y += 30;
        a->level = w2k_combo_new(0);
        w2k_combo_add(a->level, "Store (no compression)");
        w2k_combo_add(a->level, "Fast");
        w2k_combo_add(a->level, "Normal");
        w2k_combo_add(a->level, "Maximum");
        a->level->sel = 2;
        a->level_r = (W2kRect){ 118, y, 200, 21 };
        a->level->r = a->level_r;
        y += 30;
    }
    for (int i = 0; i < 3; i++) { a->chk_r[i] = (W2kRect){ 24, y, cw - 48, 18 }; y += 22; }
    a->ok = (W2kRect){ cw - 12 - 75 * 2 - 6, ch - 12 - 23, 75, 23 };
    a->cancel = (W2kRect){ cw - 12 - 75, ch - 12 - 23, 75, 23 };
    a->win->user = a;
    a->win->paint = arc_paint;
    a->win->event = arc_event;
    w2k_win_center(a->win, ex.win);
    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, a->win->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);
    w2k_add_timer(w2k_caret_blink, (void (*)(void *))w2k_edit_blink, a->name);
    int rc = w2k_win_modal(a->win);
    w2k_del_timer((void (*)(void *))w2k_edit_blink, a->name);
    return rc == ID_OK;
}

static void do_zip(void)
{
    if (ex.cur.kind != K_FS) return;
    char paths[64][1024];
    int n;
    selected_paths(paths, 64, &n);
    if (!n) return;

    /* One item: <name>.zip beside it. Several: Archive.zip. */
    const char *base = strrchr(paths[0], '/');
    base = base ? base + 1 : paths[0];
    char zipname[1100], full[2200];
    if (n == 1) snprintf(zipname, sizeof zipname, "%s.zip", base);
    else        snprintf(zipname, sizeof zipname, "Archive.zip");
    snprintf(full, sizeof full, "%s/%s", ex.cur.path, zipname);
    for (int k = 2; access(full, F_OK) == 0 && k < 100; k++) {
        if (n == 1) snprintf(zipname, sizeof zipname, "%s (%d).zip", base, k);
        else        snprintf(zipname, sizeof zipname, "Archive (%d).zip", k);
        snprintf(full, sizeof full, "%s/%s", ex.cur.path, zipname);
    }

    ArcDlg a;
    memset(&a, 0, sizeof a);
    a.chk_label[0] = "&Include subfolders";
    a.chk_label[1] = "&Store relative paths";
    a.chk_label[2] = "&Delete files after adding";
    a.chk[0] = a.chk[1] = 1;
    int files = 0, dirs = 0;
    long long bytes = 0;
    for (int i = 0; i < n; i++) count_tree(paths[i], 1, &files, &dirs, &bytes, 0);
    char sz[32];
    size_text(bytes, sz, sizeof sz);
    snprintf(a.info, sizeof a.info, "%d file(s) in %d folder(s), %s", files, dirs, sz);
    if (!arc_dialog(&a, "Add to Archive", full)) {
        w2k_edit_free(a.name);
        if (a.level) w2k_combo_free(a.level);
        if (a.format) w2k_combo_free(a.format);
        return;
    }

    char target[1024];
    snprintf(target, sizeof target, "%s", w2k_edit_text(a.name));
    if (!strchr(target, '/')) { snprintf(full, sizeof full, "%s/%s", ex.cur.path, target); snprintf(target, sizeof target, "%.1023s", full); }
    int fmt = a.format->sel < 0 || a.format->sel >= NFORMATS ? 0 : a.format->sel;
    {
        /* The name keeps the format's extension whatever was typed. */
        char stem[1024];
        snprintf(stem, sizeof stem, "%s", target);
        strip_archive_ext(stem);
        snprintf(target, sizeof target, "%.1000s%s", stem, formats[fmt].ext);
    }
    int recurse = a.chk[0], relative = a.chk[1], delete = a.chk[2];
    int lv = a.level->sel;                    /* 0 store, 1 fast, 2 normal, 3 maximum */
    w2k_edit_free(a.name);
    w2k_combo_free(a.level);
    w2k_combo_free(a.format);
    if (!tool_installed(formats[fmt].tool)) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s is not installed, so this format cannot be written.",
                 formats[fmt].tool);
        w2k_msgbox(ex.win, "Add to Archive", msg, MB_OK | MB_ICONERROR);
        return;
    }

    files = 0; dirs = 0; bytes = 0;
    for (int i = 0; i < n; i++) count_tree(paths[i], recurse, &files, &dirs, &bytes, 0);

    /* The items, quoted, relative to this folder. */
    char items[6000] = "", q[1200];
    int il = 0;
    for (int i = 0; i < n && il < (int)sizeof items - 1200; i++) {
        const char *nm = strrchr(paths[i], '/');
        /* As ./name: a name beginning with a dash is a file, not an
         * option to the archiver (or to rm). */
        char rel[1100];
        snprintf(rel, sizeof rel, "./%.1000s", nm ? nm + 1 : paths[i]);
        shell_quote(rel, q, sizeof q);
        il += snprintf(items + il, sizeof items - il, " %s", q);
    }
    char cmd[8192], qd[1200], qt[1200];
    shell_quote(ex.cur.path, qd, sizeof qd);
    shell_quote(target, qt, sizeof qt);
    if (fmt == 0) {
        int level = lv == 0 ? 0 : lv == 1 ? 1 : lv == 3 ? 9 : 6;
        snprintf(cmd, sizeof cmd, "cd %s && zip -v %s%s%s-%d %s%s", qd,
                 recurse ? "-r " : "", relative ? "" : "-j ", delete ? "-m " : "", level, qt, items);
    } else if (fmt == 1) {
        int level = lv == 0 ? 0 : lv == 1 ? 1 : lv == 3 ? 9 : 5;
        /* 7z always takes folders whole; -sdel deletes what it took. */
        snprintf(cmd, sizeof cmd, "cd %s && 7z a -bb1 -mx=%d %s%s %s%s", qd, level,
                 relative ? "" : "-spf0 ", delete ? "-sdel " : "", qt, items);
        (void)recurse;
    } else {
        /* tar, through the compressor at the chosen level; "Store" leaves
         * the tar plain when the format is plain tar anyway. */
        const char *comp = fmt == 3 ? "gzip" : fmt == 4 ? "bzip2" : fmt == 5 ? "xz" : NULL;
        int level = lv == 0 ? 1 : lv == 1 ? 1 : lv == 3 ? 9 : 6;
        char filter[80] = "";
        if (comp) snprintf(filter, sizeof filter, "-I '%s -%d' ", comp, level);
        snprintf(cmd, sizeof cmd, "cd %s && tar %s-cvf %s%s%s", qd, filter, qt,
                 recurse ? "" : " --no-recursion", items);
        if (delete) {
            size_t l = strlen(cmd);
            snprintf(cmd + l, sizeof cmd - l, " && rm -rf%s", items);
        }
        (void)relative;
    }
    const char *tb = strrchr(target, '/');
    run_with_progress("Compressing...", cmd, files + dirs, ex.cur.path, tb ? tb + 1 : target);
    refill_list();
}

/* How many entries an archive holds, from its listing. */
static int archive_entries(const char *path)
{
    char q[1200], cmd[1500];
    shell_quote(path, q, sizeof q);
    const char *dot = strrchr(path, '.');
    if (dot && (!strcasecmp(dot, ".zip") || !strcasecmp(dot, ".jar")))
        snprintf(cmd, sizeof cmd, "unzip -Z1 %s 2>/dev/null | wc -l", q);
    else if (dot && (!strcasecmp(dot, ".tar") || !strcasecmp(dot, ".tgz") ||
                     !strcasecmp(dot, ".gz")  || !strcasecmp(dot, ".bz2") ||
                     !strcasecmp(dot, ".xz")))
        snprintf(cmd, sizeof cmd, "tar tf %s 2>/dev/null | wc -l", q);
    else
        snprintf(cmd, sizeof cmd, "7z l -ba %s 2>/dev/null | wc -l", q);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    int n = 0;
    if (fscanf(p, "%d", &n) != 1) n = 0;
    pclose(p);
    return n;
}

static void do_unzip(void)
{
    if (ex.cur.kind != K_FS) return;
    char paths[64][1024];
    int n;
    selected_paths(paths, 64, &n);
    if (!n) return;
    const char *base = strrchr(paths[0], '/');
    base = base ? base + 1 : paths[0];

    ArcDlg a;
    memset(&a, 0, sizeof a);
    a.extract = 1;
    a.chk_label[0] = "Extract into a &folder named after the archive";
    a.chk_label[1] = "&Overwrite existing files";
    a.chk_label[2] = "&Show extracted files when complete";
    a.chk[0] = a.chk[1] = a.chk[2] = 1;
    int entries = archive_entries(paths[0]);
    struct stat st;
    char sz[32] = "";
    if (stat(paths[0], &st) == 0) size_text(st.st_size, sz, sizeof sz);
    snprintf(a.info, sizeof a.info, "%.100s: %d item(s), %s", base, entries, sz);
    if (!arc_dialog(&a, "Extract", ex.cur.path)) { w2k_edit_free(a.name); return; }

    char dest[1200];
    snprintf(dest, sizeof dest, "%.1023s", w2k_edit_text(a.name));
    int into_folder = a.chk[0], overwrite = a.chk[1], show = a.chk[2];
    w2k_edit_free(a.name);
    if (into_folder) {
        char stem[300];
        snprintf(stem, sizeof stem, "%.255s", base);
        char *dot = strrchr(stem, '.');
        if (dot && dot != stem) *dot = 0;
        if (dot && !strcasecmp(dot + 1, "gz")) { char *d2 = strrchr(stem, '.'); if (d2 && !strcasecmp(d2, ".tar")) *d2 = 0; }
        size_t l = strlen(dest);
        snprintf(dest + l, sizeof dest - l, "/%s", stem);
    }
    mkdir(dest, 0755);

    char cmd[4096], qd[1200], qf[1200];
    shell_quote(dest, qd, sizeof qd);
    shell_quote(paths[0], qf, sizeof qf);
    const char *dot = strrchr(paths[0], '.');
    if (dot && (!strcasecmp(dot, ".zip") || !strcasecmp(dot, ".jar")))
        snprintf(cmd, sizeof cmd, "cd %s && unzip %s %s", qd, overwrite ? "-o" : "-n", qf);
    else if (dot && (!strcasecmp(dot, ".tar") || !strcasecmp(dot, ".tgz") ||
                     !strcasecmp(dot, ".gz")  || !strcasecmp(dot, ".bz2") ||
                     !strcasecmp(dot, ".xz")))
        snprintf(cmd, sizeof cmd, "tar xvf %s -C %s%s", qf, qd, overwrite ? "" : " --skip-old-files");
    else
        snprintf(cmd, sizeof cmd, "7z x -y -bb1 %s -o%s %s", overwrite ? "-aoa" : "-aos", qd, qf);
    int ok = run_with_progress("Extracting...", cmd, entries, base, dest);
    refill_list();
    if (ok && show) navigate_path(dest, 1);
}

/* Put selected items back where they were deleted from. */
static void do_restore(void)
{
    char paths[64][1024];
    int n;
    selected_paths(paths, 64, &n);
    if (!n) return;

    int failed = 0;
    for (int i = 0; i < n; i++) {
        const char *base = strrchr(paths[i], '/');
        if (w2k_trash_restore(base ? base + 1 : paths[i]) != 0) failed++;
    }
    if (failed)
        w2k_msgbox(ex.win, "Recycle Bin",
                   failed == 1 ? "That item could not be restored."
                               : "Some items could not be restored.",
                   MB_OK | MB_ICONWARNING);
    refill_list();
}

static void do_empty_bin(void)
{
    int n = w2k_trash_count();
    if (n <= 0) {
        w2k_msgbox(ex.win, "Recycle Bin", "The Recycle Bin is already empty.",
                   MB_OK | MB_ICONINFO);
        return;
    }
    char msg[256];
    snprintf(msg, sizeof msg,
             "Are you sure you want to delete %s?\n\nThis cannot be undone.",
             n == 1 ? "this item" : "these items");
    if (w2k_msgbox(ex.win, "Confirm Delete", msg,
                   MB_YESNO | MB_ICONWARNING) != ID_YES)
        return;
    w2k_trash_empty();
    w2k_sound_play(SND_EMPTYRECYCLE);
    refill_list();
}

static W2kMenu *build_file(void *u)
{
    (void)u;
    int has = ex.list->sel >= 0;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_OPEN, "&Open", NULL, ICO_NONE);
    w2k_menu_default(m);
    if (!has) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_OPENWITH, "Open &With...", NULL, ICO_NONE);
    if (!has || ex.cur.kind != K_FS) w2k_menu_disable(m);
    w2k_menu_sep(m);
    {
        W2kMenu *st = w2k_menu_new();
        w2k_menu_item(st, ID_SENDTO_DESKTOP, "&Desktop (create shortcut)",
                      NULL, ICO_DESKTOP);
        w2k_menu_item(st, ID_SENDTO_MYDOCS, "My &Documents", NULL, ICO_MYDOCS);
        w2k_menu_sub(m, "Se&nd To", ICO_NONE, st);
        if (!has || ex.cur.kind != K_FS) w2k_menu_disable(m);
    }
    w2k_menu_sep(m);
    {
        W2kMenu *nw = w2k_menu_new();
        w2k_menu_item(nw, ID_NEWFOLDER, "&Folder", NULL, ICO_FOLDER);
        w2k_menu_item(nw, ID_NEW_SHORTCUT, "&Shortcut", NULL, ICO_APP);
        w2k_menu_item(nw, ID_NEW_TEXT, "&Text Document", NULL, ICO_FILE_TEXT);
        w2k_menu_sub(m, "Ne&w", ICO_NONE, nw);
        if (ex.cur.kind != K_FS) w2k_menu_disable(m);
    }
    w2k_menu_item(m, ID_SHORTCUT, "Create &Shortcut", NULL, ICO_APP);
    if (!has || ex.cur.kind != K_FS) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_DELETE, "&Delete", "Del", ICO_DELETE);
    if (!has || ex.cur.kind != K_FS) w2k_menu_disable(m);
    w2k_menu_item(m, ID_RENAME, "Rena&me", "F2", ICO_NONE);
    if (!has || ex.cur.kind != K_FS) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_ZIP, "Add to &Archive...", NULL, ICO_FOLDER);
    if (!has || ex.cur.kind != K_FS) w2k_menu_disable(m);
    {
        int arch = 0;
        if (ex.list->sel >= 0 && ex.cur.kind == K_FS) {
            char paths[4][1024];
            int n;
            selected_paths(paths, 4, &n);
            arch = n > 0 && is_archive(paths[0]);
        }
        w2k_menu_item(m, ID_UNZIP, "E&xtract...", NULL, ICO_FOLDER_OPEN);
        if (!arch) w2k_menu_disable(m);
    }
    /* Looking inside the bin: Restore puts things back. */
    if (ex.cur.kind == K_FS &&
        under_dir(ex.cur.path, w2k_trash_files_dir())) {
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_RESTORE, "R&estore", NULL, ICO_BACK);
        if (!has) w2k_menu_disable(m);
    }
    if (recycle_in_view()) {
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_EMPTYBIN, "&Empty Recycle Bin", NULL, ICO_DELETE);
        if (w2k_trash_count() <= 0) w2k_menu_disable(m);
    }
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_PROPS, "P&roperties", NULL, ICO_PROPERTIES);
    if (!has) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_CLOSE, "&Close", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_edit(void *u)
{
    (void)u;
    int has = ex.list->sel >= 0;
    W2kMenu *m = w2k_menu_new();
    {
        char label[64];
        const char *verb = nundo ? undo_verb(undos[nundo - 1].op) : NULL;
        if (verb) snprintf(label, sizeof label, "&Undo %s", verb);
        else      snprintf(label, sizeof label, "&Undo");
        w2k_menu_item(m, ID_UNDO, label, "Ctrl+Z", ICO_NONE);
        if (!nundo) w2k_menu_disable(m);
    }
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_CUT, "Cu&t", "Ctrl+X", ICO_CUT);
    if (!has || ex.cur.kind != K_FS) w2k_menu_disable(m);
    w2k_menu_item(m, ID_COPY, "&Copy", "Ctrl+C", ICO_COPY);
    if (!has || ex.cur.kind != K_FS) w2k_menu_disable(m);
    w2k_menu_item(m, ID_PASTE, "&Paste", "Ctrl+V", ICO_PASTE);
    if (!ex.nclip || ex.cur.kind != K_FS) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_SELECTALL, "Select &All", "Ctrl+A", ICO_NONE);
    w2k_menu_item(m, ID_INVERT, "&Invert Selection", NULL, ICO_NONE);
    return m;
}

/* The right-click menu over a file: Windows' order, which is not the
 * File menu's -- the verbs come first, the clipboard next, Properties
 * last. */
static W2kMenu *build_item_context(void)
{
    int fs = ex.cur.kind == K_FS;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_OPEN, "&Open", NULL, ICO_NONE);
    w2k_menu_default(m);
    w2k_menu_item(m, ID_OPENWITH, "Open &With...", NULL, ICO_NONE);
    if (!fs) w2k_menu_disable(m);

    if (fs) {
        int arch = 0;
        char paths[4][1024];
        int n;
        selected_paths(paths, 4, &n);
        arch = n > 0 && is_archive(paths[0]);
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_ZIP, "Add to &Archive...", NULL, ICO_FILE_ZIP);
        w2k_menu_item(m, ID_UNZIP, "E&xtract...", NULL, ICO_FOLDER_OPEN);
        if (!arch) w2k_menu_disable(m);

        W2kMenu *st = w2k_menu_new();
        w2k_menu_item(st, ID_SENDTO_DESKTOP, "&Desktop (create shortcut)",
                      NULL, ICO_DESKTOP);
        w2k_menu_item(st, ID_SENDTO_MYDOCS, "My &Documents", NULL, ICO_MYDOCS);
        w2k_menu_sub(m, "Se&nd To", ICO_NONE, st);
    }

    w2k_menu_sep(m);
    w2k_menu_item(m, ID_CUT, "Cu&t", NULL, ICO_CUT);
    if (!fs) w2k_menu_disable(m);
    w2k_menu_item(m, ID_COPY, "&Copy", NULL, ICO_COPY);
    if (!fs) w2k_menu_disable(m);

    if (fs) {
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_SHORTCUT, "Create &Shortcut", NULL, ICO_APP);
        w2k_menu_item(m, ID_DELETE, "&Delete", NULL, ICO_DELETE);
        w2k_menu_item(m, ID_RENAME, "Rena&me", NULL, ICO_NONE);
        if (under_dir(ex.cur.path, w2k_trash_files_dir())) {
            w2k_menu_sep(m);
            w2k_menu_item(m, ID_RESTORE, "R&estore", NULL, ICO_BACK);
        }
    }
    if (recycle_in_view()) {
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_EMPTYBIN, "&Empty Recycle Bin", NULL, ICO_DELETE);
        if (w2k_trash_count() <= 0) w2k_menu_disable(m);
    }
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_PROPS, "P&roperties", NULL, ICO_PROPERTIES);
    if (!fs) w2k_menu_disable(m);
    return m;
}

/* The right-click menu over empty space in the folder. */
static W2kMenu *build_folder_context(void)
{
    int fs = ex.cur.kind == K_FS;
    W2kMenu *m = w2k_menu_new();

    W2kMenu *v = w2k_menu_new();
    w2k_menu_item(v, ID_V_LARGE, "Lar&ge Icons", NULL, ICO_NONE);
    w2k_menu_radio(v, ex.view == ID_V_LARGE);
    w2k_menu_item(v, ID_V_SMALL, "S&mall Icons", NULL, ICO_NONE);
    w2k_menu_radio(v, ex.view == ID_V_SMALL);
    w2k_menu_item(v, ID_V_LIST, "&List", NULL, ICO_NONE);
    w2k_menu_radio(v, ex.view == ID_V_LIST);
    w2k_menu_item(v, ID_V_DETAILS, "&Details", NULL, ICO_NONE);
    w2k_menu_radio(v, ex.view == ID_V_DETAILS);
    w2k_menu_sub(m, "Vie&w", ICO_VIEWS, v);
    w2k_menu_item(m, ID_REFRESH, "Re&fresh", NULL, ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_PASTE, "&Paste", NULL, ICO_PASTE);
    if (!fs || !ex.nclip) w2k_menu_disable(m);
    w2k_menu_sep(m);
    {
        W2kMenu *nw = w2k_menu_new();
        w2k_menu_item(nw, ID_NEWFOLDER, "&Folder", NULL, ICO_FOLDER);
        w2k_menu_item(nw, ID_NEW_SHORTCUT, "&Shortcut", NULL, ICO_APP);
        w2k_menu_item(nw, ID_NEW_TEXT, "&Text Document", NULL, ICO_FILE_TEXT);
        w2k_menu_sub(m, "Ne&w", ICO_NONE, nw);
        if (!fs) w2k_menu_disable(m);
    }
    if (recycle_in_view()) {
        w2k_menu_sep(m);
        w2k_menu_item(m, ID_EMPTYBIN, "&Empty Recycle Bin", NULL, ICO_DELETE);
        if (w2k_trash_count() <= 0) w2k_menu_disable(m);
    }
    return m;
}

static W2kMenu *build_view(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();

    W2kMenu *bars = w2k_menu_new();
    w2k_menu_item(bars, ID_TB_STANDARD, "&Standard Buttons", NULL, ICO_NONE);
    w2k_menu_check(bars, ex.show_toolbar);
    w2k_menu_item(bars, ID_TB_ADDRESS, "&Address Bar", NULL, ICO_NONE);
    w2k_menu_check(bars, ex.show_address);
    w2k_menu_sub(m, "&Toolbars", ICO_NONE, bars);

    w2k_menu_item(m, ID_STATUSBAR, "Status &Bar", NULL, ICO_NONE);
    w2k_menu_check(m, ex.show_status);

    W2kMenu *ebar = w2k_menu_new();
    w2k_menu_item(ebar, ID_FOLDERS, "&Folders", NULL, ICO_FOLDER);
    w2k_menu_check(ebar, ex.show_tree);
    w2k_menu_sub(m, "E&xplorer Bar", ICO_NONE, ebar);
    w2k_menu_sep(m);

    w2k_menu_item(m, ID_V_LARGE, "Lar&ge Icons", NULL, ICO_NONE);
    w2k_menu_radio(m, ex.view == ID_V_LARGE);
    w2k_menu_item(m, ID_V_SMALL, "S&mall Icons", NULL, ICO_NONE);
    w2k_menu_radio(m, ex.view == ID_V_SMALL);
    w2k_menu_item(m, ID_V_LIST, "&List", NULL, ICO_NONE);
    w2k_menu_radio(m, ex.view == ID_V_LIST);
    w2k_menu_item(m, ID_V_DETAILS, "&Details", NULL, ICO_NONE);
    w2k_menu_radio(m, ex.view == ID_V_DETAILS);
    w2k_menu_sep(m);

    w2k_menu_item(m, ID_SHOW_HIDDEN, "Show &Hidden Files", "Ctrl+H", ICO_NONE);
    w2k_menu_check(m, w2k_folder_hidden);
    w2k_menu_sep(m);

    W2kMenu *arr = w2k_menu_new();
    w2k_menu_item(arr, ID_ARR_NAME, "by &Name", NULL, ICO_NONE);
    w2k_menu_radio(arr, ex.sort_col == 0);
    w2k_menu_item(arr, ID_ARR_TYPE, "by &Type", NULL, ICO_NONE);
    w2k_menu_radio(arr, ex.sort_col == 2);
    w2k_menu_item(arr, ID_ARR_SIZE, "by Si&ze", NULL, ICO_NONE);
    w2k_menu_radio(arr, ex.sort_col == 1);
    w2k_menu_item(arr, ID_ARR_DATE, "by &Date", NULL, ICO_NONE);
    w2k_menu_radio(arr, ex.sort_col == 3);
    w2k_menu_sub(m, "Arrange &Icons", ICO_NONE, arr);
    w2k_menu_sep(m);

    /* Windows 2000 has no Go menu: Back, Forward and Up live here. */
    W2kMenu *go = w2k_menu_new();
    w2k_menu_item(go, ID_BACK, "&Back", "Alt+Left", ICO_BACK);
    if (ex.hist_i <= 0) w2k_menu_disable(go);
    w2k_menu_item(go, ID_FORWARD, "&Forward", "Alt+Right", ICO_FORWARD);
    if (ex.hist_i + 1 >= ex.hist_n) w2k_menu_disable(go);
    w2k_menu_item(go, ID_UP, "&Up One Level", NULL, ICO_UP);
    w2k_menu_sep(go);
    w2k_menu_item(go, ID_MYDOCS, "My &Documents", NULL, ICO_MYDOCS);
    w2k_menu_item(go, ID_MYCOMPUTER, "My &Computer", NULL, ICO_MYCOMPUTER);
    w2k_menu_sub(m, "&Go To", ICO_NONE, go);

    w2k_menu_item(m, ID_REFRESH, "R&efresh", "F5", ICO_NONE);
    return m;
}

/* ------------------------------------------------------------------ *
 * Favorites
 *
 * A folder of shortcuts, like the Start menu's -- Windows keeps it in
 * the profile, this keeps it in ~/.w2k/Favorites. Adding one writes a
 * .desktop pointing at the folder being viewed.
 * ------------------------------------------------------------------ */
#define FAV_BASE 600
#define FAV_MAX  40

static struct { char label[128], target[1024]; } favs[FAV_MAX];
static int nfavs;

static void favorites_dir(char *buf, int n)
{
    snprintf(buf, (size_t)n, "%s/.w2k/Favorites", ex.home);
}

static int fav_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void favorites_load(void)
{
    nfavs = 0;
    char dir[1100];
    favorites_dir(dir, sizeof dir);
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    char names[FAV_MAX][256];
    int n = 0;
    while ((de = readdir(dp)) && n < FAV_MAX) {
        if (de->d_name[0] == '.') continue;
        snprintf(names[n++], 256, "%s", de->d_name);
    }
    closedir(dp);
    qsort(names, (size_t)n, sizeof names[0], fav_cmp);

    for (int i = 0; i < n; i++) {
        char full[1400];
        snprintf(full, sizeof full, "%.1000s/%.255s", dir, names[i]);
        char label[128], exec[1024];
        if (!strcasecmp(w2k_file_ext(names[i]), "desktop") &&
            w2k_desktop_entry(full, label, sizeof label, exec, sizeof exec,
                              NULL, 0)) {
            snprintf(favs[nfavs].label, sizeof favs[0].label, "%s", label);
            snprintf(favs[nfavs].target, sizeof favs[0].target, "%s", exec);
            nfavs++;
        }
    }
}

static void favorites_add(void)
{
    if (ex.cur.kind != K_FS) return;
    char dir[1100];
    favorites_dir(dir, sizeof dir);
    char parent[1100];
    snprintf(parent, sizeof parent, "%s/.w2k", ex.home);
    mkdir(parent, 0755);
    mkdir(dir, 0755);

    const char *base = strrchr(ex.cur.path, '/');
    base = (base && base[1]) ? base + 1 : ex.cur.path;
    char name[256];
    if (!w2k_prompt(ex.win, "Add Favorite", "&Name:", base, name, sizeof name,
                    ICO_FOLDER))
        return;
    if (!name[0]) return;

    char link[1500];
    snprintf(link, sizeof link, "%.1000s/%.200s.desktop", dir, name);
    FILE *f = fopen(link, "w");
    if (!f) {
        w2k_msgbox(ex.win, "Windows Explorer",
                   "Cannot add this favorite.", MB_OK | MB_ICONERROR);
        return;
    }
    char q[4200];
    w2k_shell_quote(ex.cur.path, q, sizeof q);
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\n"
               "Exec=l2kexplorer %s\nTerminal=false\n", name, q);
    fclose(f);
    chmod(link, 0755);                       /* a shortcut we made is trusted */
    favorites_load();
}

/* A favourite made here runs "l2kexplorer <path>"; going there in this
 * window is better than opening another one, so the path is pulled back
 * out of the command when it has that shape. */
static void favorites_open(int i)
{
    if (i < 0 || i >= nfavs) return;
    const char *cmd = favs[i].target;
    const char *p = strstr(cmd, "l2kexplorer ");
    if (p) {
        char path[1024];
        snprintf(path, sizeof path, "%s", p + 12);
        size_t len = strlen(path);
        if (len > 1 && path[0] == '\'' && path[len - 1] == '\'') {
            path[len - 1] = 0;
            memmove(path, path + 1, len - 1);
        }
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            navigate_path(path, 1);
            return;
        }
    }
    spawn("%s", cmd);
}

static W2kMenu *build_favorites(void *u)
{
    (void)u;
    favorites_load();
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_FAV_ADD, "&Add to Favorites...", NULL, ICO_FAVORITES);
    if (ex.cur.kind != K_FS) w2k_menu_disable(m);
    w2k_menu_item(m, ID_FAV_ORG, "&Organize Favorites...", NULL,
                  ICO_FOLDER_OPEN);
    if (nfavs) {
        w2k_menu_sep(m);
        for (int i = 0; i < nfavs; i++)
            w2k_menu_item(m, FAV_BASE + i, favs[i].label, NULL, ICO_FOLDER);
    }
    return m;
}

static W2kMenu *build_tools(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_MAPDRIVE, "&Map Network Drive...", NULL, ICO_NETWORK);
    w2k_menu_disable(m);              /* nothing to map to */
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_FOLDEROPTS, "F&older Options...", NULL, ICO_SETTINGS);
    return m;
}

static W2kMenu *build_help(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_ABOUT, "&About Windows", NULL, ICO_NONE);
    return m;
}

static void layout(W2kWin *w);

static void command(void *user, int id)
{
    if (id >= FAV_BASE && id < FAV_BASE + FAV_MAX) {
        favorites_open(id - FAV_BASE);
        return;
    }
    (void)user;
    switch (id) {
    case ID_OPEN:      on_activate(NULL, ex.list->sel); break;
    case ID_NEWFOLDER: do_newfolder(); break;
    case ID_DELETE:    do_delete(); break;
    case ID_RENAME:    do_rename(); break;
    /* The bars a user hides stay hidden next time, as in Windows. */
    case ID_TB_STANDARD:
    case ID_TB_ADDRESS:
    case ID_STATUSBAR:
        if (id == ID_TB_STANDARD)     ex.show_toolbar = !ex.show_toolbar;
        else if (id == ID_TB_ADDRESS) ex.show_address = !ex.show_address;
        else                          ex.show_status = !ex.show_status;
        w2k_view_toolbar = ex.show_toolbar;
        w2k_view_address = ex.show_address;
        w2k_view_status = ex.show_status;
        w2k_scheme_save(NULL);
        layout(ex.win);
        break;
    case ID_ARR_NAME: on_sort(NULL, 0); break;
    case ID_ARR_SIZE: on_sort(NULL, 1); break;
    case ID_ARR_TYPE: on_sort(NULL, 2); break;
    case ID_ARR_DATE: on_sort(NULL, 3); break;
    case ID_FAV_ADD:  favorites_add(); break;
    case ID_FAV_ORG: {
        char dir[1100];
        favorites_dir(dir, sizeof dir);
        char parent[1100];
        snprintf(parent, sizeof parent, "%s/.w2k", ex.home);
        mkdir(parent, 0755);
        mkdir(dir, 0755);
        navigate_path(dir, 1);
        break;
    }
    case ID_FOLDEROPTS:
        if (w2k_folder_options(ex.win)) {
            ex.list->singleclick = w2k_folder_singleclick;
            update_caption();
            refill_list();
        }
        break;
    case ID_PROPS:     do_properties(); break;
    case ID_EMPTYBIN:  do_empty_bin(); break;
    case ID_RESTORE:   do_restore(); break;
    case ID_ZIP:       do_zip(); break;
    case ID_UNZIP:     do_unzip(); break;
    case ID_CLOSE:     w2k_win_close(ex.win, 0); break;

    case ID_CUT:
    case ID_COPY:
        selected_paths(ex.clip, 64, &ex.nclip);
        ex.clip_cut = (id == ID_CUT);
        break;
    case ID_PASTE: do_paste(); break;
    case ID_UNDO:  do_undo(); break;
    case ID_NEW_SHORTCUT: do_new_shortcut(); break;
    case ID_NEW_TEXT:     do_new_text(); break;
    case ID_SHORTCUT:  do_create_shortcut(ex.cur.path); break;
    case ID_SENDTO_DESKTOP: {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/Desktop", ex.home);
        mkdir(dir, 0755);
        do_create_shortcut(dir);
        break;
    }
    case ID_SENDTO_MYDOCS: do_send_to_mydocs(); break;
    case ID_OPENWITH:      do_open_with(); break;
    case ID_SELECTALL:
        for (int i = 0; i < ex.list->n; i++) ex.list->items[i].selected = 1;
        if (ex.list->n) ex.list->sel = 0;
        status_update();
        break;
    case ID_INVERT:
        for (int i = 0; i < ex.list->n; i++)
            ex.list->items[i].selected = !ex.list->items[i].selected;
        status_update();
        break;

    case ID_V_LARGE: case ID_V_SMALL: case ID_V_LIST: case ID_V_DETAILS:
        ex.view = id;
        list_configure();
        viewmem_store();
        break;
    case ID_SHOW_HIDDEN:
        w2k_folder_hidden = !w2k_folder_hidden;
        w2k_scheme_save(NULL);
        refill_list();
        break;
    case ID_REFRESH: refill_list(); break;
    case ID_FOLDERS: ex.show_tree = !ex.show_tree;
                     if (ex.win->resized) ex.win->resized(ex.win);
                     break;

    case ID_BACK:
        if (ex.hist_i > 0) {
            ex.hist_i--;
            Node nd = { ex.hist_kind[ex.hist_i], { 0 } };
            set_path(nd.path, sizeof nd.path, ex.history[ex.hist_i]);
            navigate(&nd, 0);
        }
        break;
    case ID_FORWARD:
        if (ex.hist_i + 1 < ex.hist_n) {
            ex.hist_i++;
            Node nd = { ex.hist_kind[ex.hist_i], { 0 } };
            set_path(nd.path, sizeof nd.path, ex.history[ex.hist_i]);
            navigate(&nd, 0);
        }
        break;
    case ID_UP:     go_up(); break;
    case ID_SEARCH: {
        /* The shell's Search dialog, asked for through _W2K_COMMAND. */
        XEvent ev = { 0 };
        ev.xclient.type = ClientMessage;
        ev.xclient.window = w2k.root;
        ev.xclient.message_type = w2k.a_w2k_command;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 5;
        XSendEvent(w2k.dpy, w2k.root, False, SubstructureNotifyMask, &ev);
        XFlush(w2k.dpy);
        break;
    }
    case ID_MYDOCS: navigate_path(ex.home, 1); break;
    case ID_MYCOMPUTER: { Node nd = { K_MYCOMPUTER, { 0 } }; navigate(&nd, 1); break; }
    case ID_VIEWS:
        ex.view = (ex.view == ID_V_DETAILS) ? ID_V_LARGE : ex.view + 1;
        list_configure();
        viewmem_store();
        break;
    case ID_ABOUT:
        w2k_msgbox(ex.win, "About Windows",
                   "Windows Explorer\nLinux 2000\nA Windows 2000-style desktop for X11\n\n"
                   "Browsing the filesystem the way the shell used to.\n\n"
                   "Linux 2000 is not affiliated with, endorsed by or sponsored by Microsoft.\nWindows is a trademark of Microsoft Corporation.",
                   MB_OK | MB_ICONINFO);
        break;
    }
    update_caption();
    w2k_win_dirty(ex.win);
}

/* ------------------------------------------------------------------ *
 * Layout / painting
 * ------------------------------------------------------------------ */
#define ADDR_H 24

static void layout(W2kWin *w)
{
    int y = 0;
    ex.mb->r = (W2kRect){ 0, y, w->w, MENUBAR_H };
    y += MENUBAR_H;
    if (ex.show_toolbar) {
        ex.tb->r = (W2kRect){ 0, y, w->w, TOOLBAR_H };
        y += TOOLBAR_H;
    } else {
        ex.tb->r = (W2kRect){ 0, 0, 0, 0 };
    }

    if (ex.show_address) {
        /* Address on the left, search filter on the right of the same row. */
        int search_w = 160;
        if (w->w < 420) search_w = 100;
        int gap = 8;
        int search_label_w = 42;
        int right = w->w - 6;
        ex.search->r = (W2kRect){ right - search_w, y + 2, search_w, 20 };
        ex.search_label = (W2kRect){ ex.search->r.x - search_label_w - 2, y + 4,
                                     search_label_w, 16 };
        ex.addr_label = (W2kRect){ 6, y + 4, 46, 16 };
        int addr_x = 54;
        int addr_w = ex.search_label.x - gap - addr_x;
        if (addr_w < 80) addr_w = 80;
        ex.addr->r = (W2kRect){ addr_x, y + 2, addr_w, 20 };
        y += ADDR_H;
    } else {
        ex.addr_label = (W2kRect){ 0, 0, 0, 0 };
        ex.addr->r = (W2kRect){ 0, 0, 0, 0 };
        ex.search_label = (W2kRect){ 0, 0, 0, 0 };
        ex.search->r = (W2kRect){ 0, 0, 0, 0 };
    }

    int bottom = w->h - (ex.show_status ? STATUS_H : 0);
    ex.sb->r = (W2kRect){ 0, bottom, w->w, ex.show_status ? STATUS_H : 0 };

    if (ex.show_tree) {
        if (ex.split_x < 80) ex.split_x = 190;
        if (ex.split_x > w->w - 120) ex.split_x = w->w - 120;
        ex.tree->r = (W2kRect){ 2, y, ex.split_x - 2, bottom - y - 2 };
        ex.split_r = (W2kRect){ ex.split_x, y, 4, bottom - y - 2 };
        ex.list->r = (W2kRect){ ex.split_x + 4, y, w->w - ex.split_x - 6,
                                bottom - y - 2 };
    } else {
        ex.tree->r = (W2kRect){ 0, 0, 0, 0 };
        ex.split_r = (W2kRect){ 0, 0, 0, 0 };
        ex.list->r = (W2kRect){ 2, y, w->w - 4, bottom - y - 2 };
    }
}

static void paint(W2kWin *w, Drawable d)
{
    int fh = w2k_font_height(F_UI);
    w2k_menubar_draw(d, ex.mb);
    if (ex.show_toolbar) w2k_toolbar_draw(d, ex.tb);

    if (ex.show_address) {
        w2k_fill(d, 0, ex.addr->r.y - 2, w->w, ADDR_H, C_FACE);
        w2k_text_mnemonic(d, F_UI, ex.addr_label.x,
                          ex.addr->r.y + (20 - fh) / 2, "A&ddress", C_TEXT, 1);
        w2k_combo_draw(d, ex.addr);
        w2k_text_mnemonic(d, F_UI, ex.search_label.x,
                          ex.search->r.y + (20 - fh) / 2, "Se&arch", C_TEXT, 1);
        w2k_edit_draw(d, ex.search);
    }

    if (ex.show_tree) {
        w2k_tree_draw(d, ex.tree);
        /* Splitter: plain face, like the shell's. */
        w2k_fill(d, ex.split_r.x, ex.split_r.y, ex.split_r.w, ex.split_r.h,
                 C_FACE);
    }
    w2k_list_draw(d, ex.list);
    if (ex.show_status) w2k_status_draw(d, ex.sb);
}

static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        if (w2k_dnd_active()) return 1;
        int x = e->xbutton.x, y = e->xbutton.y;
        /* Mouse wheel: Ctrl+wheel cycles view modes; plain wheel scrolls the
         * pane under the pointer (list or folders tree). */
        if (e->xbutton.button == Button4 || e->xbutton.button == Button5) {
            int up = (e->xbutton.button == Button4);
            if (e->xbutton.state & ControlMask) {
                if (up)
                    ex.view = (ex.view == ID_V_LARGE) ? ID_V_DETAILS : ex.view - 1;
                else
                    ex.view = (ex.view == ID_V_DETAILS) ? ID_V_LARGE : ex.view + 1;
                list_configure();
                viewmem_store();
                w2k_win_dirty(w);
                return 1;
            }
            if (ex.show_tree && w2k_rect_hit(&ex.tree->r, x, y)) {
                w2k_list_layout(ex.list); /* keep geometry current */
                w2k_tree_layout(ex.tree);
                if (w2k_scroll_wheel(&ex.tree->vsb, up ? -1 : 1))
                    w2k_win_dirty(w);
                return 1;
            }
            /* Default: scroll the file list (vertical, or horizontal in List). */
            {
                w2k_list_layout(ex.list);
                W2kScroll *s = (ex.list->mode == LV_LIST) ? &ex.list->hsb
                                                          : &ex.list->vsb;
                if (w2k_scroll_wheel(s, up ? -1 : 1)) {
                    ex.list->top = ex.list->vsb.pos;
                    ex.list->scroll_x = ex.list->hsb.pos;
                    w2k_win_dirty(w);
                }
            }
            return 1;
        }
        if (w2k_menubar_press(ex.mb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_toolbar_press(ex.tb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (ex.show_address && w2k_combo_press(ex.addr, &e->xbutton)) {
            if (ex.search) ex.search->focused = 0;
            if (ex.addr->edit && ex.addr->edit->focused) {
                ex.list->focused = 0;
                if (ex.tree) ex.tree->focused = 0;
            }
            w2k_win_dirty(w);
            return 1;
        }
        if (ex.show_address && w2k_edit_press(ex.search, &e->xbutton)) {
            ex.list->focused = 0;
            if (ex.tree) ex.tree->focused = 0;
            w2k_win_dirty(w);
            return 1;
        }
        if (ex.show_tree && w2k_rect_hit(&ex.split_r, x, y)) {
            ex.dragging_split = 1;
            return 1;
        }
        if (ex.show_tree && w2k_tree_press(ex.tree, &e->xbutton)) {
            ex.list->focused = 0;
            ex.tree->focused = 1;
            if (ex.search) ex.search->focused = 0;
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_list_press(ex.list, &e->xbutton)) {
            ex.tree->focused = 0;
            if (ex.search) ex.search->focused = 0;
            /* A press on a selected item may become a drag; a press on empty
             * space is the rubber band's, never a drag of what was selected
             * before. */
            int hit = w2k_list_hit(ex.list, x, y);
            if (e->xbutton.button == Button1 && hit >= 0 && hit < ex.list->n &&
                ex.list->items[hit].selected && ex.cur.kind == K_FS) {
                drag_armed = 1;                 /* becomes a drag on motion */
                drag_from_x = x;
                drag_from_y = y;
            }
            if (e->xbutton.button == Button3) {
                W2kMenu *m = ex.list->sel >= 0 ? build_item_context()
                                               : build_folder_context();
                Window ch;
                int rx, ry;
                XTranslateCoordinates(w2k.dpy, w->win, w2k.root, x, y, &rx, &ry, &ch);
                int id = w2k_menu_popup(m, rx, ry, MPOP_LEFT);
                w2k_menu_free(m);
                if (id) command(NULL, id);
            }
            w2k_win_dirty(w);
            return 1;
        }
        return 1;
    }
    case MotionNotify:
        /* A few pixels of movement with the button down turns a press on a
         * selected row into a drag. */
        if (drag_armed && !w2k_dnd_active() &&
            (abs(e->xmotion.x - drag_from_x) > 4 ||
             abs(e->xmotion.y - drag_from_y) > 4)) {
            drag_armed = 0;
            char paths[64][1024];
            int n;
            selected_paths(paths, 64, &n);
            if (n) {
                char *uris = w2k_uri_list_build(paths, n);
                if (uris) {
                    w2k_dnd_begin(w->win, uris, 1);
                    free(uris);
                    XGrabPointer(w2k.dpy, w->win, False,
                                 ButtonReleaseMask | PointerMotionMask,
                                 GrabModeAsync, GrabModeAsync, None,
                                 w2k.cur_arrow, CurrentTime);
                }
            }
        }
        if (w2k_dnd_active()) {
            w2k_dnd_set_time(e->xmotion.time);
            w2k_dnd_motion(e->xmotion.x_root, e->xmotion.y_root);
            return 1;
        }
        if (ex.dragging_split) {
            ex.split_x = e->xmotion.x;
            layout(w);
            w2k_win_dirty(w);
            return 1;
        }
        if (ex.show_address && ex.addr && ex.addr->edit &&
            w2k_edit_motion(ex.addr->edit, &e->xmotion)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (ex.show_address && ex.search &&
            w2k_edit_motion(ex.search, &e->xmotion)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_toolbar_motion(ex.tb, &e->xmotion) ||
            w2k_list_motion(ex.list, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease:
        drag_armed = 0;
        if (w2k_dnd_active()) {
            w2k_dnd_set_time(e->xbutton.time);
            if (!w2k_dnd_drop()) w2k_dnd_cancel();
            XUngrabPointer(w2k.dpy, CurrentTime);
            w2k_list_release(ex.list, NULL);     /* the drag, not a click */
            return 1;
        }
        ex.dragging_split = 0;
        w2k_toolbar_release(ex.tb);
        if (ex.addr && ex.addr->edit) w2k_edit_release(ex.addr->edit);
        if (ex.search) w2k_edit_release(ex.search);
        w2k_list_release(ex.list, &e->xbutton);
        w2k_win_dirty(w);
        return 1;
    case KeyPress: {
        if (w2k_menubar_key(ex.mb, &e->xkey)) { w2k_win_dirty(w); return 1; }
        KeySym ks = XLookupKeysym(&e->xkey, 0);

        /* Editable address bar: Enter navigates, Tab completes a path. */
        if (ex.show_address && ex.addr && ex.addr->editable &&
            ex.addr->edit && ex.addr->edit->focused) {
            if (ks == XK_Return || ks == XK_KP_Enter) {
                addr_go();
                if (ex.addr->edit) ex.addr->edit->focused = 0;
                ex.list->focused = 1;
                w2k_win_dirty(w);
                return 1;
            }
            if (ks == XK_Tab || ks == XK_ISO_Left_Tab) {
                addr_complete();
                w2k_win_dirty(w);
                return 1;
            }
            if (ks == XK_Escape) {
                if (ex.addr->edit) {
                    ex.addr->edit->focused = 0;
                    w2k_combo_set_text(ex.addr,
                        ex.cur.kind == K_FS ? ex.cur.path : "");
                }
                ex.list->focused = 1;
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_combo_key(ex.addr, &e->xkey)) {
                w2k_win_dirty(w);
                return 1;
            }
        }

        /* Search bar focused: Enter applies the filter and unfocuses;
         * Escape clears focus (and the filter if empty). */
        if (ex.show_address && ex.search && ex.search->focused) {
            if (ks == XK_Return || ks == XK_KP_Enter) {
                const char *t = w2k_edit_text(ex.search);
                snprintf(ex.search_pat, sizeof ex.search_pat, "%s", t ? t : "");
                ex.search->focused = 0;
                ex.list->focused = 1;
                refill_list();
                w2k_win_dirty(w);
                return 1;
            }
            if (ks == XK_Escape) {
                ex.search->focused = 0;
                ex.list->focused = 1;
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_edit_key(ex.search, &e->xkey)) {
                w2k_win_dirty(w);
                return 1;
            }
        }

        if (e->xkey.state & Mod1Mask) {
            if (ks == XK_Left)  { command(NULL, ID_BACK); return 1; }
            if (ks == XK_Right) { command(NULL, ID_FORWARD); return 1; }
            if (ks == XK_Up)    { command(NULL, ID_UP); return 1; }
            if (ks == XK_Return) { command(NULL, ID_PROPS); return 1; }
            /* Alt+1..4 select folder view: Large, Small, List, Details. */
            if (ks == XK_1 || ks == XK_KP_1) {
                ex.view = ID_V_LARGE; list_configure(); viewmem_store();
                w2k_win_dirty(w); return 1;
            }
            if (ks == XK_2 || ks == XK_KP_2) {
                ex.view = ID_V_SMALL; list_configure(); viewmem_store();
                w2k_win_dirty(w); return 1;
            }
            if (ks == XK_3 || ks == XK_KP_3) {
                ex.view = ID_V_LIST; list_configure(); viewmem_store();
                w2k_win_dirty(w); return 1;
            }
            if (ks == XK_4 || ks == XK_KP_4) {
                ex.view = ID_V_DETAILS; list_configure(); viewmem_store();
                w2k_win_dirty(w); return 1;
            }
        }
        if ((e->xkey.state & ControlMask) && (ks == XK_n || ks == XK_N)) {
            /* A new window on this folder. */
            if (ex.cur.kind == K_FS) {
                char q[4200];
                w2k_shell_quote(ex.cur.path, q, sizeof q);
                spawn("l2kexplorer %s", q);
            } else spawn("%s", "l2kexplorer");
            return 1;
        }
        if (e->xkey.state & ControlMask) {
            switch (ks) {
            case XK_x: case XK_X: command(NULL, ID_CUT); return 1;
            case XK_c: case XK_C: command(NULL, ID_COPY); return 1;
            case XK_v: case XK_V: command(NULL, ID_PASTE); return 1;
            case XK_a: case XK_A: command(NULL, ID_SELECTALL); return 1;
            case XK_z: case XK_Z: command(NULL, ID_UNDO); return 1;
            case XK_h: case XK_H: command(NULL, ID_SHOW_HIDDEN); return 1;
            case XK_f: case XK_F:
                /* Ctrl+F focuses the search bar; caret at the start. */
                if (ex.show_address && ex.search) {
                    if (ex.addr && ex.addr->edit) ex.addr->edit->focused = 0;
                    ex.search->focused = 1;
                    ex.list->focused = 0;
                    if (ex.tree) ex.tree->focused = 0;
                    ex.search->caret = 0;
                    ex.search->sel = 0;
                    ex.search->caret_on = 1;
                    w2k_win_dirty(w);
                    return 1;
                }
                break;
            }
        }

        /* `/` focuses the address bar (path), caret at the end. */
        if (ex.show_address && ex.addr && ex.addr->editable &&
            ex.addr->edit &&
            !(e->xkey.state & (ControlMask | Mod1Mask | Mod4Mask)) &&
            (ks == XK_slash || ks == XK_KP_Divide)) {
            if (ex.search) ex.search->focused = 0;
            ex.addr->edit->focused = 1;
            ex.list->focused = 0;
            if (ex.tree) ex.tree->focused = 0;
            {
                const char *t = w2k_combo_text(ex.addr);
                int n = t ? (int)strlen(t) : 0;
                ex.addr->edit->caret = n;
                ex.addr->edit->sel = n;
                ex.addr->edit->caret_on = 1;
            }
            w2k_win_dirty(w);
            return 1;
        }
        if (ks == XK_F5)     { command(NULL, ID_REFRESH); return 1; }
        if (ks == XK_F2)     { command(NULL, ID_RENAME); return 1; }
        if (ks == XK_Delete) {
            /* Shift+Delete skips the Recycle Bin, as in Windows. */
            do_delete_ex((e->xkey.state & ShiftMask) != 0);
            return 1;
        }
        if (ks == XK_BackSpace) { command(NULL, ID_UP); return 1; }

        /* Type any letter a–z to focus the search bar and start filtering.
         * Skip when the address bar already has the caret. */
        if (ex.show_address && ex.search &&
            !(ex.addr && ex.addr->edit && ex.addr->edit->focused) &&
            !(e->xkey.state & (ControlMask | Mod1Mask | Mod4Mask)) &&
            ((ks >= XK_a && ks <= XK_z) || (ks >= XK_A && ks <= XK_Z))) {
            char buf[8] = { 0 };
            XLookupString(&e->xkey, buf, sizeof buf, NULL, NULL);
            if (buf[0]) {
                ex.search->focused = 1;
                ex.list->focused = 0;
                if (ex.tree) ex.tree->focused = 0;
                w2k_edit_set(ex.search, "");
                w2k_edit_insert(ex.search, buf);
                w2k_win_dirty(w);
                return 1;
            }
        }

        if (ex.tree->focused && w2k_tree_key(ex.tree, &e->xkey)) {
            w2k_win_dirty(w);
            return 1;
        }
        if (w2k_list_key(ex.list, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (w2k_init("l2kexplorer") < 0) return 1;

    const char *h = getenv("HOME");
    if (!h) {
        struct passwd *pw = getpwuid(getuid());
        h = (pw && pw->pw_dir) ? pw->pw_dir : "/";
    }
    snprintf(ex.home, sizeof ex.home, "%s", h);

    ex.view = ID_V_DETAILS;
    ex.show_toolbar = w2k_view_toolbar;
    ex.show_address = w2k_view_address;
    ex.show_status = w2k_view_status;
    ex.show_tree = 1;
    ex.split_x = 190;

    ex.win = w2k_win_new("My Computer", "l2kexplorer", 720, 480, 1);
    ex.win->paint = paint;
    ex.win->event = event;
    ex.win->resized = layout;
    ex.win->min_w = 420;
    ex.win->min_h = 260;

    ex.mb = w2k_menubar_new(NULL, command);
    ex.mb->win_ref = ex.win->win;
    w2k_menubar_add(ex.mb, "&File", build_file);
    w2k_menubar_add(ex.mb, "&Edit", build_edit);
    w2k_menubar_add(ex.mb, "&View", build_view);
    w2k_menubar_add(ex.mb, "F&avorites", build_favorites);
    w2k_menubar_add(ex.mb, "&Tools", build_tools);
    w2k_menubar_add(ex.mb, "&Help", build_help);

    ex.tb = w2k_toolbar_new(NULL, command);
    ex.tb->show_text = 1;
    w2k_toolbar_add(ex.tb, ID_BACK, ICO_BACK, "Back");
    ex.tb->show_text = 1;
    w2k_toolbar_add(ex.tb, ID_FORWARD, ICO_FORWARD, "Forward");
    w2k_toolbar_add(ex.tb, ID_UP, ICO_UP, NULL);
    w2k_toolbar_sep(ex.tb);
    w2k_toolbar_add(ex.tb, ID_SEARCH, ICO_SEARCH, NULL);
    w2k_toolbar_add(ex.tb, ID_FOLDERS, ICO_FOLDER, NULL);
    w2k_toolbar_sep(ex.tb);
    w2k_toolbar_add(ex.tb, ID_CUT, ICO_CUT, NULL);
    w2k_toolbar_add(ex.tb, ID_COPY, ICO_COPY, NULL);
    w2k_toolbar_add(ex.tb, ID_PASTE, ICO_PASTE, NULL);
    w2k_toolbar_add(ex.tb, ID_DELETE, ICO_DELETE, NULL);
    w2k_toolbar_sep(ex.tb);
    w2k_toolbar_add(ex.tb, ID_PROPS, ICO_PROPERTIES, NULL);
    w2k_toolbar_add(ex.tb, ID_VIEWS, ICO_VIEWS, NULL);

    ex.addr = w2k_combo_new(1);           /* editable path with Tab complete */
    if (ex.addr->edit) w2k_edit_bind(ex.addr->edit, ex.win);
    ex.search = w2k_edit_new(0);
    w2k_edit_bind(ex.search, ex.win);
    ex.search_pat[0] = 0;

    ex.tree = w2k_tree_new();
    ex.tree->on_select = on_tree_select;
    ex.tree->on_expand = on_tree_expand;
    w2k_scroll_bind(&ex.tree->vsb, ex.win);
    tree_build();

    ex.list = w2k_list_new(LV_REPORT);
    ex.list->multisel = 1;
    ex.list->focused = 1;
    ex.list->on_activate = on_activate;
    ex.list->singleclick = w2k_folder_singleclick;
    ex.list->on_select = on_select;
    ex.list->on_sort = on_sort;
    w2k_scroll_bind(&ex.list->vsb, ex.win);
    w2k_scroll_bind(&ex.list->hsb, ex.win);
    w2k_list_add_col(ex.list, "Name", 190, 0);
    w2k_list_add_col(ex.list, "Size", 70, 1);
    w2k_list_add_col(ex.list, "Type", 120, 0);
    w2k_list_add_col(ex.list, "Modified", 130, 0);

    ex.sb = w2k_status_new();
    w2k_status_add(ex.sb, 0);
    w2k_status_add(ex.sb, 90);
    w2k_status_add(ex.sb, 110);

    layout(ex.win);

    if (argc > 1) {
        struct stat st;
        if (!strcmp(argv[1], "~")) navigate_path(ex.home, 1);
        else if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode))
            navigate_path(argv[1], 1);
        else { Node nd = { K_MYCOMPUTER, "" }; navigate(&nd, 1); }
    } else {
        Node nd = { K_MYCOMPUTER, { 0 } };
        navigate(&nd, 1);
    }

    /* Reaps a finished zip/unzip and refreshes the listing. */

    /* Files can be dragged in from anywhere that speaks XDND. */
    w2k_dnd_accept(ex.win->win);
    w2k_dnd_on_drop = ex_on_drop;
    w2k_dnd_will_accept = ex_will_accept;

    /* Development aid: W2K_EXPLORER_DEMO=zip|unzip|progress puts that
     * dialog up first, so W2K_RENDER can picture it. */
    const char *demo = getenv("W2K_EXPLORER_DEMO");
    if (demo && !strcmp(demo, "zip")) {
        ArcDlg a; memset(&a, 0, sizeof a);
        a.chk_label[0] = "&Include subfolders"; a.chk_label[1] = "&Store relative paths";
        a.chk_label[2] = "&Delete files after adding"; a.chk[0] = a.chk[1] = 1;
        snprintf(a.info, sizeof a.info, "14 file(s) in 3 folder(s), 2,310 KB");
        char p[1200];
        snprintf(p, sizeof p, "%s/Documents/Archive.zip", ex.home);
        arc_dialog(&a, "Add to Archive", p);
    } else if (demo && !strcmp(demo, "unzip")) {
        ArcDlg a; memset(&a, 0, sizeof a); a.extract = 1;
        a.chk_label[0] = "Extract into a &folder named after the archive";
        a.chk_label[1] = "&Overwrite existing files"; a.chk_label[2] = "&Show extracted files when complete";
        a.chk[0] = a.chk[1] = a.chk[2] = 1;
        snprintf(a.info, sizeof a.info, "Archive.zip: 14 item(s), 2,310 KB");
        char p[1200];
        snprintf(p, sizeof p, "%s/Documents", ex.home);
        arc_dialog(&a, "Extract", p);
    } else if (demo && !strcmp(demo, "progress")) {
        run_with_progress("Compressing...", "for i in 1 2 3 4; do echo \"  adding: photos/holiday $i.jpg (deflated 3%)\"; sleep 1; done",
                          10, ex.home, "Archive.zip");
    }
    w2k_win_show(ex.win);
    w2k_run();

    viewmem_store();                 /* the folder we were looking at */
    viewmem_save();
    w2k_menubar_free(ex.mb);
    w2k_toolbar_free(ex.tb);
    w2k_combo_free(ex.addr);
    w2k_edit_free(ex.search);
    w2k_tree_free(ex.tree);
    w2k_list_free(ex.list);
    w2k_status_free(ex.sb);
    w2k_fini();
    return 0;
}
