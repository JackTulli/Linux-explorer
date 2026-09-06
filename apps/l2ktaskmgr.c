/* l2ktaskmgr -- Linux Task Manager.
 *
 * Applications / Processes / Performance, refreshed once a second.
 * Applications come from the window manager's _NET_CLIENT_LIST; processes
 * and the performance figures come from /proc. */
#include "w2kui.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

enum {
    ID_NEWTASK = 1, ID_EXIT, ID_ENDTASK, ID_SWITCHTO, ID_ENDPROCESS,
    ID_ALWAYSTOP, ID_MINONUSE, ID_HIDEMIN, ID_REFRESH, ID_ABOUT,
    ID_SPEED_HIGH, ID_SPEED_NORMAL, ID_SPEED_LOW, ID_SPEED_PAUSED,
    ID_CPU_ONE, ID_CPU_PER_CORE, ID_KERNEL_TIMES,
    ID_TILE_H, ID_TILE_V, ID_CASCADE, ID_MINIMIZE, ID_MAXIMIZE, ID_BRINGFRONT
};

#define HIST 60          /* seconds of history in the graphs */
#define MAXCPU 64        /* per-core graphs, like the multiprocessor original */
#define MAXPROC 1024

typedef struct {
    int  pid;
    char name[64];
    unsigned long long jiffies;      /* utime + stime  */
    long  rss_kb;
    double cpu;
    int   seen;
} Proc;

typedef struct {
    W2kWin     *win;
    W2kMenubar *mb;
    W2kTabs    *tabs;
    W2kList    *apps;
    W2kList    *procs;
    W2kStatus  *sb;
    W2kRect     b_end, b_switch, b_new, b_endproc;
    int         down;

    Window      appwin[128];
    int         napps;

    Proc        pr[MAXPROC];
    int         npr;
    unsigned long long cpu_total, cpu_idle;

    double      cpu_hist[HIST], mem_hist[HIST];
    int         hist_n;
    long        hist_total;          /* samples ever taken: scrolls the grid */
    double      cpu_now, mem_pct;
    /* Kernel time (system + irq + softirq), the red line under the green. */
    double      kern_hist[HIST], kern_now;
    unsigned long long cpu_kern;
    int         show_kernel;
    /* The page file: Windows' commit charge, from Committed_AS. */
    long        commit_kb, commit_limit_kb, commit_peak_kb;
    double      pf_pct;
    long        slab_kb, sreclaim_kb, sunreclaim_kb, kstack_kb, ptables_kb;
    long        handles;

    /* One set of figures per core, plus the totals above. */
    int         ncpu;
    unsigned long long core_total[MAXCPU], core_idle[MAXCPU], core_kern[MAXCPU];
    double      core_now[MAXCPU], core_know[MAXCPU];
    double      core_hist[MAXCPU][HIST], core_khist[MAXCPU][HIST];
    int         per_core;            /* one graph per CPU */
    long        mem_total_kb, mem_avail_kb, mem_cached_kb;
    long        swap_total_kb, swap_free_kb;
    int         nthreads, nprocs;
    int         interval_ms;
    int         sort_col, sort_dir;
} Tm;

static Tm tm;

/* ------------------------------------------------------------------ *
 * /proc sampling
 * ------------------------------------------------------------------ */
static long read_meminfo_key(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ':' || *p == ' ') p++;
    return atol(p);
}

static void sample_memory(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);

    tm.mem_total_kb  = read_meminfo_key(buf, "MemTotal");
    tm.mem_avail_kb  = read_meminfo_key(buf, "MemAvailable");
    tm.mem_cached_kb = read_meminfo_key(buf, "Cached");
    tm.swap_total_kb = read_meminfo_key(buf, "SwapTotal");
    tm.swap_free_kb  = read_meminfo_key(buf, "SwapFree");
    if (!tm.mem_avail_kb) tm.mem_avail_kb = read_meminfo_key(buf, "MemFree");
    if (tm.mem_total_kb > 0)
        tm.mem_pct = 100.0 * (tm.mem_total_kb - tm.mem_avail_kb) / tm.mem_total_kb;
    /* The Windows figures: the commit charge is what the kernel calls
     * Committed_AS against CommitLimit; the kernel's own memory is the
     * slab, split into what can be paged out (reclaimable) and what
     * cannot, plus stacks and page tables. */
    tm.commit_kb       = read_meminfo_key(buf, "Committed_AS");
    tm.commit_limit_kb = read_meminfo_key(buf, "CommitLimit");
    if (tm.commit_kb > tm.commit_peak_kb) tm.commit_peak_kb = tm.commit_kb;
    tm.slab_kb      = read_meminfo_key(buf, "Slab");
    tm.sreclaim_kb  = read_meminfo_key(buf, "SReclaimable");
    tm.sunreclaim_kb = read_meminfo_key(buf, "SUnreclaim");
    tm.kstack_kb    = read_meminfo_key(buf, "KernelStack");
    tm.ptables_kb   = read_meminfo_key(buf, "PageTables");
    tm.mem_cached_kb += tm.sreclaim_kb;              /* System Cache */
    tm.pf_pct = tm.commit_limit_kb > 0 ? 100.0 * tm.commit_kb / tm.commit_limit_kb : 0;
    if (tm.pf_pct > 100) tm.pf_pct = 100;
    FILE *h = fopen("/proc/sys/fs/file-nr", "r");   /* open handles */
    if (h) {
        long a = 0;
        if (fscanf(h, "%ld", &a) == 1) tm.handles = a;
        fclose(h);
    }
}

/* Kernel share of the busy time between two samples. */
static double kern_delta(unsigned long long kern, unsigned long long total_now,
                         unsigned long long total_prev, unsigned long long *pkern,
                         double previous)
{
    double pct = previous;
    if (total_prev && total_now > total_prev && kern >= *pkern) {
        pct = 100.0 * (double)(kern - *pkern) / (double)(total_now - total_prev);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    *pkern = kern;
    return pct;
}

/* Busy percentage between two /proc/stat samples of the same counter set. */
static double cpu_delta(unsigned long long total, unsigned long long idle,
                        unsigned long long *ptotal, unsigned long long *pidle,
                        double previous)
{
    double pct = previous;
    if (*ptotal && total > *ptotal) {
        unsigned long long dt = total - *ptotal;
        unsigned long long di = idle - *pidle;
        pct = 100.0 * (double)(dt - di) / (double)dt;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    *ptotal = total;
    *pidle = idle;
    return pct;
}

/* The "cpu" line is the machine; "cpu0", "cpu1", ... are the cores. */
static void sample_cpu(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "cpu", 3)) break;      /* the cpu lines come first */
        unsigned long long v[8] = { 0 };
        char label[16];
        int got = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                         label, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                         &v[6], &v[7]);
        if (got < 5) continue;

        unsigned long long total = 0;
        for (int i = 0; i < 8; i++) total += v[i];
        unsigned long long idle = v[3] + v[4];
        unsigned long long kern = v[2] + v[5] + v[6];   /* system, irq, softirq */

        if (!label[3]) {
            unsigned long long prev = tm.cpu_total;
            tm.cpu_now = cpu_delta(total, idle, &tm.cpu_total, &tm.cpu_idle,
                                   tm.cpu_now);
            tm.kern_now = kern_delta(kern, total, prev, &tm.cpu_kern, tm.kern_now);
        } else {
            int n = atoi(label + 3);
            if (n < 0 || n >= MAXCPU) continue;
            if (n + 1 > tm.ncpu) tm.ncpu = n + 1;
            unsigned long long prev = tm.core_total[n];
            tm.core_now[n] = cpu_delta(total, idle, &tm.core_total[n],
                                       &tm.core_idle[n], tm.core_now[n]);
            tm.core_know[n] = kern_delta(kern, total, prev, &tm.core_kern[n], tm.core_know[n]);
        }
    }
    fclose(f);
}

static Proc *proc_find(int pid)
{
    for (int i = 0; i < tm.npr; i++) if (tm.pr[i].pid == pid) return &tm.pr[i];
    return NULL;
}

static void sample_procs(unsigned long long dtotal)
{
    DIR *dp = opendir("/proc");
    if (!dp) return;
    for (int i = 0; i < tm.npr; i++) tm.pr[i].seen = 0;

    struct dirent *de;
    int threads = 0, count = 0;
    while ((de = readdir(dp))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int pid = atoi(de->d_name);
        char path[64], buf[2048];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        if (!n) continue;
        buf[n] = 0;

        /* The comm field is parenthesised and may itself contain spaces. */
        char *open_p = strchr(buf, '(');
        char *close_p = strrchr(buf, ')');
        if (!open_p || !close_p || close_p < open_p) continue;
        char name[64];
        int len = (int)(close_p - open_p - 1);
        if (len > 63) len = 63;
        memcpy(name, open_p + 1, len);
        name[len] = 0;

        /* The kernel truncates comm to 15 characters, which turns half a
         * browser's processes into "Isolated Web Co". When it looks cut off,
         * take the program name from the command line instead. */
        if (len == 15) {
            char cpath[64], cbuf[256];
            snprintf(cpath, sizeof cpath, "/proc/%d/cmdline", pid);
            FILE *cf = fopen(cpath, "r");
            if (cf) {
                size_t cn = fread(cbuf, 1, sizeof cbuf - 1, cf);
                fclose(cf);
                if (cn) {
                    cbuf[cn] = 0;
                    char *base = strrchr(cbuf, '/');
                    base = base ? base + 1 : cbuf;
                    if (*base && !strncmp(base, name, 8))
                        snprintf(name, sizeof name, "%.63s", base);
                }
            }
        }

        unsigned long long ut = 0, st = 0;
        long rss_pages = 0, nthr = 0;
        /* Fields after ")": state(3) ppid pgrp session tty tpgid flags
         * minflt cminflt majflt cmajflt utime stime ... */
        {
            char *p = close_p + 2;
            int field = 3;
            char *tok = strtok(p, " ");
            while (tok) {
                if (field == 14) ut = strtoull(tok, NULL, 10);
                else if (field == 15) st = strtoull(tok, NULL, 10);
                else if (field == 20) nthr = atol(tok);
                else if (field == 24) rss_pages = atol(tok);
                if (field >= 24) break;
                tok = strtok(NULL, " ");
                field++;
            }
        }
        threads += (int)nthr;
        count++;

        Proc *pr = proc_find(pid);
        if (!pr) {
            if (tm.npr >= MAXPROC) continue;
            pr = &tm.pr[tm.npr++];
            pr->pid = pid;
            pr->jiffies = ut + st;
            pr->cpu = 0;
        } else if (dtotal > 0) {
            unsigned long long now = ut + st;
            pr->cpu = (now >= pr->jiffies)
                    ? 100.0 * (double)(now - pr->jiffies) / (double)dtotal : 0.0;
            pr->jiffies = now;
        }
        snprintf(pr->name, sizeof pr->name, "%s", name);
        pr->rss_kb = rss_pages * (sysconf(_SC_PAGESIZE) / 1024);
        pr->seen = 1;
    }
    closedir(dp);

    /* Drop processes that have exited. */
    int k = 0;
    for (int i = 0; i < tm.npr; i++) if (tm.pr[i].seen) tm.pr[k++] = tm.pr[i];
    tm.npr = k;
    tm.nthreads = threads;
    tm.nprocs = count;
}

/* ------------------------------------------------------------------ *
 * Applications, via EWMH
 * ------------------------------------------------------------------ */
static char *win_name(Window w)
{
    XTextProperty tp;
    if (XGetTextProperty(w2k.dpy, w, &tp, w2k.a_net_wm_name) && tp.nitems) {
        char *s = w2k_strdup((char *)tp.value);
        XFree(tp.value);
        return s;
    }
    if (XGetTextProperty(w2k.dpy, w, &tp, XA_WM_NAME) && tp.nitems) {
        char *s = w2k_strdup((char *)tp.value);
        XFree(tp.value);
        return s;
    }
    return w2k_strdup("(untitled)");
}

static int is_task_window(Window w)
{
    Window tr = None;
    if (XGetTransientForHint(w2k.dpy, w, &tr) && tr != None) return 0;

    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    int ok = 1;
    if (XGetWindowProperty(w2k.dpy, w, w2k.a_net_wm_window_type, 0, 8, False,
                           XA_ATOM, &type, &fmt, &n, &after, &data) == Success
        && data) {
        Atom *a = (Atom *)data;
        for (unsigned long i = 0; i < n; i++)
            if (a[i] == w2k.a_net_wm_wt_dialog || a[i] == w2k.a_net_wm_wt_dock ||
                a[i] == w2k.a_net_wm_wt_utility || a[i] == w2k.a_net_wm_wt_splash)
                ok = 0;
        XFree(data);
    }
    return ok;
}

static void refresh_apps(void)
{
    /* The selection follows the window, not the row: the list is rebuilt
     * every tick and windows come and go above it. */
    Window oldwin = tm.apps->sel >= 0 && tm.apps->sel < tm.napps
                  ? tm.appwin[tm.apps->sel] : None;
    w2k_list_clear(tm.apps);
    tm.napps = 0;

    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(w2k.dpy, w2k.root, w2k.a_net_client_list, 0, 256,
                           False, XA_WINDOW, &type, &fmt, &n, &after,
                           &data) == Success && data) {
        Window *ws = (Window *)data;
        for (unsigned long i = 0; i < n && tm.napps < 128; i++) {
            if (ws[i] == tm.win->win) continue;
            if (!is_task_window(ws[i])) continue;
            char *nm = win_name(ws[i]);
            int r = w2k_list_add(tm.apps, ICO_APP, NULL);
            w2k_list_set(tm.apps, r, 0, nm);
            w2k_list_set(tm.apps, r, 1, "Running");
            free(nm);
            tm.appwin[tm.napps++] = ws[i];
        }
        XFree(data);
    }
    tm.apps->sel = -1;
    for (int i = 0; oldwin && i < tm.napps; i++)
        if (tm.appwin[i] == oldwin) {
            tm.apps->sel = i;
            tm.apps->items[i].selected = 1;
            break;
        }
}

/* ------------------------------------------------------------------ *
 * Process list
 * ------------------------------------------------------------------ */
static int cmp_proc(const void *A, const void *B)
{
    const Proc *a = A, *b = B;
    int r = 0;
    switch (tm.sort_col) {
    case 0: r = strcmp(a->name, b->name); break;
    case 1: r = a->pid - b->pid; break;
    case 2: r = (a->cpu > b->cpu) - (a->cpu < b->cpu); break;
    case 3: r = (a->rss_kb > b->rss_kb) - (a->rss_kb < b->rss_kb); break;
    }
    return tm.sort_dir ? -r : r;
}

/* Sizes in the unit that suits them: the original counted everything in
 * kilobytes, which on a machine with 16 GB of RAM reads as noise. */
static void size_text(long kb, char *out, int n)
{
    if (kb < 0) kb = 0;
    double k = (double)kb;
    if (k >= 1024.0 * 1024.0)      snprintf(out, n, "%.1f GB", k / (1024.0 * 1024.0));
    else if (k >= 1024.0)          snprintf(out, n, "%.0f MB", k / 1024.0);
    else                           snprintf(out, n, "%ld KB", kb);
}

static void refresh_procs(void)
{
    int selpid = -1;
    if (tm.procs->sel >= 0 && tm.procs->sel < tm.procs->n)
        selpid = (int)(long)tm.procs->items[tm.procs->sel].data;

    qsort(tm.pr, tm.npr, sizeof *tm.pr, cmp_proc);
    w2k_list_clear(tm.procs);
    for (int i = 0; i < tm.npr; i++) {
        Proc *p = &tm.pr[i];
        int r = w2k_list_add(tm.procs, ICO_NONE, (void *)(long)p->pid);
        w2k_list_set(tm.procs, r, 0, p->name);
        char b[32];
        snprintf(b, sizeof b, "%d", p->pid);
        w2k_list_set(tm.procs, r, 1, b);
        snprintf(b, sizeof b, "%02d", (int)(p->cpu + 0.5));
        w2k_list_set(tm.procs, r, 2, b);
        size_text(p->rss_kb, b, sizeof b);
        w2k_list_set(tm.procs, r, 3, b);
        if (p->pid == selpid) {
            tm.procs->sel = r;
            tm.procs->items[r].selected = 1;
        }
    }
}

/* The process and thread totals without opening 360 files: the numeric
 * entries in /proc are the processes, and the fourth field of
 * /proc/loadavg is "running/total" tasks, where total is every thread.
 * Full per-process sampling only happens when that list is on screen. */
static void sample_totals(void)
{
    DIR *dp = opendir("/proc");
    int count = 0;
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)))
            if (de->d_name[0] >= '0' && de->d_name[0] <= '9') count++;
        closedir(dp);
        tm.nprocs = count;
    }
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        char buf[128];
        if (fgets(buf, sizeof buf, f)) {
            const char *slash = strchr(buf, '/');
            if (slash) tm.nthreads = atoi(slash + 1);
        }
        fclose(f);
    }
}

static void tick(void *unused)
{
    (void)unused;
    unsigned long long prev = tm.cpu_total;
    sample_cpu();
    sample_memory();
    /* Per-process figures are only needed by the list that shows them. */
    if (tm.tabs->sel == 1) sample_procs(prev ? tm.cpu_total - prev : 0);
    else                   sample_totals();

    tm.hist_total++;
    if (tm.hist_n < HIST) {
        tm.cpu_hist[tm.hist_n] = tm.cpu_now;
        tm.kern_hist[tm.hist_n] = tm.kern_now;
        tm.mem_hist[tm.hist_n] = tm.pf_pct;
        for (int k = 0; k < tm.ncpu; k++) {
            tm.core_hist[k][tm.hist_n] = tm.core_now[k];
            tm.core_khist[k][tm.hist_n] = tm.core_know[k];
        }
        tm.hist_n++;
    } else {
        memmove(tm.cpu_hist, tm.cpu_hist + 1, (HIST - 1) * sizeof(double));
        memmove(tm.kern_hist, tm.kern_hist + 1, (HIST - 1) * sizeof(double));
        memmove(tm.mem_hist, tm.mem_hist + 1, (HIST - 1) * sizeof(double));
        tm.cpu_hist[HIST - 1] = tm.cpu_now;
        tm.kern_hist[HIST - 1] = tm.kern_now;
        tm.mem_hist[HIST - 1] = tm.pf_pct;
        for (int k = 0; k < tm.ncpu; k++) {
            memmove(tm.core_hist[k], tm.core_hist[k] + 1,
                    (HIST - 1) * sizeof(double));
            memmove(tm.core_khist[k], tm.core_khist[k] + 1,
                    (HIST - 1) * sizeof(double));
            tm.core_hist[k][HIST - 1] = tm.core_now[k];
            tm.core_khist[k][HIST - 1] = tm.core_know[k];
        }
    }

    /* Rebuilding a list view nobody is looking at is pure waste: the
     * Processes list is ~380 rows, sorted and re-populated every tick.
     * The samples above still run, so the totals and the graphs stay live
     * whichever tab is showing. */
    if (tm.tabs->sel == 0) refresh_apps();
    if (tm.tabs->sel == 1) refresh_procs();

    char b[80];
    snprintf(b, sizeof b, "Processes: %d", tm.nprocs);
    w2k_status_set(tm.sb, 0, b);
    snprintf(b, sizeof b, "CPU Usage: %d%%", (int)(tm.cpu_now + 0.5));
    w2k_status_set(tm.sb, 1, b);
    /* "Commit Charge: 326M / 1159M", as Windows put it. */
    snprintf(b, sizeof b, "Commit Charge: %ldM / %ldM", tm.commit_kb / 1024,
             tm.commit_limit_kb / 1024);
    w2k_status_set(tm.sb, 2, b);

    w2k_win_dirty(tm.win);
}

/* ------------------------------------------------------------------ *
 * Performance tab
 * ------------------------------------------------------------------ */
#define GREEN   C_GRAYTEXT   /* placeholder; real colours come from w2k_rgb */

static unsigned long g_bright, g_dim, g_black, g_red;

/* The meter: a black well with LED bars -- two rows lit, one dark --
 * climbing from above the reading, which sits inside the well in green,
 * as Task Manager's do. */
static void draw_meter(Drawable d, W2kRect r, double pct, const char *reading)
{
    w2k_edge(d, r.x, r.y, r.w, r.h, EDGE_SUNKEN, BF_RECT);
    w2k_fill_rgb(d, r.x + 2, r.y + 2, r.w - 4, r.h - 4, 0, 0, 0);

    int fh = w2k_font_height(F_UI);
    int text_h = fh + 4;
    int bx = r.x + 6, bw = r.w - 12;
    int top = r.y + 6, bottom = r.y + r.h - 4 - text_h;
    int seg = 3, nseg = (bottom - top) / seg;
    int on = (int)(nseg * pct / 100.0 + 0.5);
    if (pct > 0 && on < 1) on = 1;
    for (int i = 0; i < nseg; i++) {
        int y = bottom - (i + 1) * seg;
        if (i < on) w2k_fill_rgb(d, bx, y, bw, seg - 1, 0, 255, 0);
        else        w2k_fill_rgb(d, bx, y, bw, seg - 1, 0, 48, 0);
    }
    int tw = w2k_text_width(F_UI, reading, -1);
    w2k_text_rgb(d, F_UI, r.x + (r.w - tw) / 2, bottom + 2, reading, 0, 255, 0);
}

/* The history graph: a green grid on black that scrolls with the
 * samples, the reading in bright green and, when asked, the kernel's
 * share in red beneath it. */
static void draw_graph(Drawable d, W2kRect r, const double *hist,
                       const double *kern, int n)
{
    w2k_edge(d, r.x, r.y, r.w, r.h, EDGE_SUNKEN, BF_RECT);
    int gx = r.x + 2, gy = r.y + 2, gw = r.w - 4, gh = r.h - 4;
    w2k_fill_rgb(d, gx, gy, gw, gh, 0, 0, 0);
    if (gw <= 0 || gh <= 0) return;

    /* Twelve-pixel cells; the verticals move left with each sample, the
     * horizontals stand still, as in the original. Small per-core graphs
     * get a coarser mesh so they stay readable. */
    int cell = gw < 100 ? 8 : 12;
    int step = gw / HIST;
    if (step < 1) step = 1;
    int shift = (int)((tm.hist_total * step) % cell);
    w2k_clip_set(gx, gy, gw, gh);
    for (int y = gy + gh - 1 - cell; y > gy; y -= cell)
        w2k_fill_rgb(d, gx, y, gw, 1, 0, 128, 0);
    for (int x = gx + gw - 1 - shift; x > gx; x -= cell)
        w2k_fill_rgb(d, x, gy, 1, gh, 0, 128, 0);

    if (n >= 2) {
        /* Newest sample at the right edge; the lines are polylines in
         * screen pixels so a steep spike is one stroke, not stairs. */
        for (int pass = 0; pass < 2; pass++) {
            const double *h = pass == 0 ? kern : hist;
            if (!h || (pass == 0 && !tm.show_kernel)) continue;
            XSetForeground(w2k.dpy, w2k.gc, pass == 0 ? g_red : g_bright);
            XSetLineAttributes(w2k.dpy, w2k.gc, (unsigned)w2k_th(1), LineSolid, CapButt, JoinMiter);
            XPoint pts[HIST];
            for (int i = 0; i < n; i++) {
                int x = gx + gw - 1 - (n - 1 - i) * step;
                int y = gy + gh - 1 - (int)((gh - 1) * h[i] / 100.0 + 0.5);
                pts[i].x = (short)w2k_cx(x);
                pts[i].y = (short)w2k_cx(y);
            }
            XDrawLines(w2k.dpy, d, w2k.gc, pts, n, CoordModeOrigin);
            XSetLineAttributes(w2k.dpy, w2k.gc, 0, LineSolid, CapButt, JoinMiter);
        }
    }
    w2k_clip_clear();
}

static void label_pair(Drawable d, int x, int y, const char *k, const char *v,
                       int w)
{
    int fh = w2k_font_height(F_UI);
    w2k_text(d, F_UI, x, y, k, C_TEXT);
    int vw = w2k_text_width(F_UI, v, -1);
    w2k_text(d, F_UI, x + w - vw, y, v, C_TEXT);
    (void)fh;
}

/* One small graph per core, tiled to fill the history box. */
static void draw_core_graphs(Drawable d, W2kRect r)
{
    int n = tm.ncpu;
    if (n < 1) { draw_graph(d, r, tm.cpu_hist, tm.kern_hist, tm.hist_n); return; }

    /* Pick the column count that makes the individual graphs squarest for
     * the shape of the box -- sqrt(n) columns would be square only if the
     * box were, and it is a wide, short strip. */
    int cols = 1;
    double best = 1e30;
    for (int k = 1; k <= n; k++) {
        int rows = (n + k - 1) / k;
        double cw = (double)r.w / k, ch = (double)r.h / rows;
        if (cw < 20 || ch < 14) continue;
        double aspect = cw > ch ? cw / ch : ch / cw;
        /* Penalise a ragged last row: 16 cores want 8x2, not 9x2 with a
         * gap in the corner. */
        double score = aspect + 0.15 * (k * rows - n);
        if (score < best) { best = score; cols = k; }
    }
    int rows = (n + cols - 1) / cols;

    int gap = 2;
    int cw = (r.w - (cols - 1) * gap) / cols;
    int ch = (r.h - (rows - 1) * gap) / rows;
    if (cw < 8 || ch < 8) { draw_graph(d, r, tm.cpu_hist, tm.kern_hist, tm.hist_n); return; }

    for (int i = 0; i < n; i++) {
        W2kRect g = { r.x + (i % cols) * (cw + gap),
                      r.y + (i / cols) * (ch + gap), cw, ch };
        draw_graph(d, g, tm.core_hist[i], tm.core_khist[i], tm.hist_n);
    }
}

static void figure(Drawable d, int x, int y, int w, const char *k, long v)
{
    char b[32];
    snprintf(b, sizeof b, "%ld", v);
    label_pair(d, x, y, k, b, w);
}

static void paint_perf(Drawable d, W2kRect c)
{
    int fh = w2k_font_height(F_UI);
    char b[64];

    /* The Windows 2000 page: two rows of a meter beside its history, the
     * meter's well 60 wide, then four boxes of figures in kilobytes. */
    int meter_w = 62;
    int grp_h = ((c.h - 12) * 55 / 100 - 6) / 2;
    int meter_h = grp_h - 22;
    if (meter_h < 50) { meter_h = 50; grp_h = meter_h + 22; }

    /* --- CPU ---------------------------------------------------------- */
    W2kRect grp = { c.x + 4, c.y + 2, meter_w + 16, grp_h };
    w2k_draw_groupbox(d, &grp, "CPU Usage");
    W2kRect g1 = { grp.x + 8, grp.y + 16, meter_w, meter_h };
    snprintf(b, sizeof b, "%d %%", (int)(tm.cpu_now + 0.5));
    draw_meter(d, g1, tm.cpu_now, b);

    int hx = grp.x + grp.w + 6;
    W2kRect hgrp = { hx, c.y + 2, c.x + c.w - hx - 4, grp_h };
    if (tm.per_core && tm.ncpu > 1)
        snprintf(b, sizeof b, "CPU Usage History (%d CPUs)", tm.ncpu);
    else
        snprintf(b, sizeof b, "CPU Usage History");
    w2k_draw_groupbox(d, &hgrp, b);
    W2kRect hg = { hgrp.x + 8, hgrp.y + 16, hgrp.w - 16, meter_h };
    if (tm.per_core) draw_core_graphs(d, hg);
    else             draw_graph(d, hg, tm.cpu_hist, tm.kern_hist, tm.hist_n);

    /* --- Page file ---------------------------------------------------- */
    int y2 = grp.y + grp_h + 6;
    W2kRect mgrp = { c.x + 4, y2, meter_w + 16, grp_h };
    w2k_draw_groupbox(d, &mgrp, "PF Usage");
    W2kRect m1 = { mgrp.x + 8, mgrp.y + 16, meter_w, meter_h };
    if (tm.commit_kb >= 1024 * 1024)
        snprintf(b, sizeof b, "%.1f GB", tm.commit_kb / (1024.0 * 1024.0));
    else
        snprintf(b, sizeof b, "%ld MB", tm.commit_kb / 1024);
    draw_meter(d, m1, tm.pf_pct, b);

    W2kRect mhgrp = { hx, y2, c.x + c.w - hx - 4, grp_h };
    w2k_draw_groupbox(d, &mhgrp, "Page File Usage History");
    W2kRect mh = { mhgrp.x + 8, mhgrp.y + 16, mhgrp.w - 16, meter_h };
    draw_graph(d, mh, tm.mem_hist, NULL, tm.hist_n);

    /* The four figure boxes along the bottom, in K as Windows shows them. */
    int y3 = mgrp.y + grp_h + 6;
    int bw = (c.w - 16) / 2, bh = c.y + c.h - y3 - 4;
    if (bh < 40) return;
    const char *titles[4] = { "Totals", "Physical Memory (K)",
                              "Commit Charge (K)", "Kernel Memory (K)" };
    for (int i = 0; i < 4; i++) {
        W2kRect g = { c.x + 4 + (i % 2) * (bw + 8), y3 + (i / 2) * (bh / 2 + 2),
                      bw, bh / 2 - 2 };
        w2k_draw_groupbox(d, &g, titles[i]);
        int lx = g.x + 10, ly = g.y + 16, lw = g.w - 20, dy = fh + 2;
        switch (i) {
        case 0:
            figure(d, lx, ly, lw, "Handles", tm.handles);
            figure(d, lx, ly + dy, lw, "Threads", tm.nthreads);
            figure(d, lx, ly + 2 * dy, lw, "Processes", tm.nprocs);
            break;
        case 1:
            figure(d, lx, ly, lw, "Total", tm.mem_total_kb);
            figure(d, lx, ly + dy, lw, "Available", tm.mem_avail_kb);
            figure(d, lx, ly + 2 * dy, lw, "System Cache", tm.mem_cached_kb);
            break;
        case 2:
            figure(d, lx, ly, lw, "Total", tm.commit_kb);
            figure(d, lx, ly + dy, lw, "Limit", tm.commit_limit_kb);
            figure(d, lx, ly + 2 * dy, lw, "Peak", tm.commit_peak_kb);
            break;
        case 3:
            figure(d, lx, ly, lw, "Total", tm.slab_kb + tm.kstack_kb + tm.ptables_kb);
            figure(d, lx, ly + dy, lw, "Paged", tm.sreclaim_kb);
            figure(d, lx, ly + 2 * dy, lw, "Nonpaged", tm.sunreclaim_kb + tm.kstack_kb + tm.ptables_kb);
            break;
        }
    }
}

/* ------------------------------------------------------------------ *
 * Commands
 * ------------------------------------------------------------------ */
static void send_close(Window w)
{
    XClientMessageEvent ev = {
        .type = ClientMessage, .window = w,
        .message_type = w2k.a_net_close_window, .format = 32
    };
    ev.data.l[0] = CurrentTime;
    XSendEvent(w2k.dpy, w2k.root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&ev);
    XFlush(w2k.dpy);
}

static void send_activate(Window w)
{
    XClientMessageEvent ev = {
        .type = ClientMessage, .window = w,
        .message_type = w2k.a_net_active_window, .format = 32
    };
    ev.data.l[0] = 2;
    ev.data.l[1] = CurrentTime;
    XSendEvent(w2k.dpy, w2k.root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&ev);
    XFlush(w2k.dpy);
}

/* ------------------------------------------------------------------ *
 * The Windows menu
 *
 * Windows 2000's Task Manager arranges windows from here. The work is
 * done by asking the window manager: _NET_MOVERESIZE_WINDOW to place a
 * window, WM_CHANGE_STATE to minimize, _NET_WM_STATE to maximize. Any
 * EWMH window manager understands them, not only ours.
 *
 * The items act on the selected tasks, or on all of them when nothing is
 * selected -- which is what tiling with no selection means.
 * ------------------------------------------------------------------ */
static int selected_windows(Window *out, int max)
{
    int n = 0;
    for (int i = 0; i < tm.napps && i < tm.apps->n && n < max; i++)
        if (tm.apps->items[i].selected) out[n++] = tm.appwin[i];
    if (n) return n;
    for (int i = 0; i < tm.napps && n < max; i++) out[n++] = tm.appwin[i];
    return n;
}

static void send_place(Window win, int x, int y, int w, int h)
{
    XEvent e = { 0 };
    e.xclient.type = ClientMessage;
    e.xclient.window = win;
    e.xclient.message_type = w2k.a_net_moveresize_window;
    e.xclient.format = 32;
    /* Gravity 0 (the window's own) with all four values supplied. */
    e.xclient.data.l[0] = (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11);
    e.xclient.data.l[1] = x;
    e.xclient.data.l[2] = y;
    e.xclient.data.l[3] = w;
    e.xclient.data.l[4] = h;
    XSendEvent(w2k.dpy, w2k.root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &e);
}

static void send_state(Window win, int add, Atom a, Atom b)
{
    XEvent e = { 0 };
    e.xclient.type = ClientMessage;
    e.xclient.window = win;
    e.xclient.message_type = w2k.a_net_wm_state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = add ? 1 : 0;
    e.xclient.data.l[1] = (long)a;
    e.xclient.data.l[2] = (long)b;
    XSendEvent(w2k.dpy, w2k.root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &e);
}

/* The area windows may occupy: what the window manager published, minus
 * nothing -- it already excludes the taskbar. */
static void work_area(int *x, int *y, int *w, int *h)
{
    *x = 0; *y = 0; *w = w2k.sw; *h = w2k.sh;
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(w2k.dpy, w2k.root, w2k.a_net_workarea, 0, 4, False,
                           XA_CARDINAL, &type, &fmt, &n, &after,
                           &data) == Success && data) {
        if (n >= 4) {
            long *v = (long *)data;
            *x = (int)v[0]; *y = (int)v[1];
            *w = (int)v[2]; *h = (int)v[3];
        }
        XFree(data);
    }
}

static void arrange(int how)
{
    Window ws[128];
    int n = selected_windows(ws, 128);
    if (n <= 0) return;
    int ax, ay, aw, ah;
    work_area(&ax, &ay, &aw, &ah);

    if (how == ID_CASCADE) {
        int step = 24, cw = aw * 2 / 3, ch = ah * 2 / 3;
        for (int i = 0; i < n; i++) {
            int off = step * i;
            /* Start again from the top left once the pile runs off. */
            if (off + cw > aw || off + ch > ah) off = 0;
            send_place(ws[i], ax + off, ay + off, cw, ch);
        }
        return;
    }
    if (how == ID_TILE_H) {                 /* stacked, full width */
        int hgt = ah / n;
        for (int i = 0; i < n; i++)
            send_place(ws[i], ax, ay + i * hgt, aw, hgt);
        return;
    }
    int wid = aw / n;                       /* side by side */
    for (int i = 0; i < n; i++)
        send_place(ws[i], ax + i * wid, ay, wid, ah);
}

static W2kMenu *build_windows(void *u)
{
    (void)u;
    int has = tm.napps > 0;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_TILE_H, "Tile &Horizontally", NULL, ICO_NONE);
    if (!has) w2k_menu_disable(m);
    w2k_menu_item(m, ID_TILE_V, "Tile &Vertically", NULL, ICO_NONE);
    if (!has) w2k_menu_disable(m);
    w2k_menu_item(m, ID_MINIMIZE, "Mi&nimize", NULL, ICO_NONE);
    if (tm.apps->sel < 0) w2k_menu_disable(m);
    w2k_menu_item(m, ID_MAXIMIZE, "Ma&ximize", NULL, ICO_NONE);
    if (tm.apps->sel < 0) w2k_menu_disable(m);
    w2k_menu_item(m, ID_CASCADE, "&Cascade", NULL, ICO_NONE);
    if (!has) w2k_menu_disable(m);
    w2k_menu_item(m, ID_BRINGFRONT, "&Bring To Front", NULL, ICO_NONE);
    if (tm.apps->sel < 0) w2k_menu_disable(m);
    return m;
}

static void command(void *user, int id)
{
    (void)user;
    switch (id) {
    case ID_NEWTASK: {
        char out[512];
        if (w2k_prompt(tm.win, "Create New Task",
                       "Type the name of a program to open:", "", out,
                       sizeof out, ICO_RUN) && out[0]) {
            pid_t pid = fork();
            if (pid == 0) {
                if (fork() == 0) {
                    close(ConnectionNumber(w2k.dpy));
                    setsid();
                    execlp("/bin/sh", "sh", "-c", out, (char *)NULL);
                }
                _exit(127);
            }
            if (pid > 0) { int st; waitpid(pid, &st, 0); }
        }
        break;
    }
    case ID_ENDTASK:
        if (tm.apps->sel >= 0 && tm.apps->sel < tm.napps)
            send_close(tm.appwin[tm.apps->sel]);
        break;
    case ID_SWITCHTO:
        if (tm.apps->sel >= 0 && tm.apps->sel < tm.napps)
            send_activate(tm.appwin[tm.apps->sel]);
        break;
    case ID_ENDPROCESS: {
        if (tm.procs->sel < 0 || tm.procs->sel >= tm.procs->n) break;
        int pid = (int)(long)tm.procs->items[tm.procs->sel].data;
        if (w2k_msgbox(tm.win, "Task Manager Warning",
                       "WARNING: Terminating a process can cause undesired\n"
                       "results including loss of data and system instability.\n"
                       "\n"
                       "Are you sure you want to terminate the process?",
                       MB_YESNO | MB_ICONWARNING) == ID_YES)
            kill(pid, SIGTERM);
        break;
    }
    case ID_TILE_H: case ID_TILE_V: case ID_CASCADE:
        arrange(id);
        break;
    case ID_MINIMIZE:
        if (tm.apps->sel >= 0 && tm.apps->sel < tm.napps)
            XIconifyWindow(w2k.dpy, tm.appwin[tm.apps->sel], w2k.screen);
        break;
    case ID_MAXIMIZE:
        if (tm.apps->sel >= 0 && tm.apps->sel < tm.napps)
            send_state(tm.appwin[tm.apps->sel], 1, w2k.a_net_wm_state_maxv,
                       w2k.a_net_wm_state_maxh);
        break;
    case ID_BRINGFRONT:
        if (tm.apps->sel >= 0 && tm.apps->sel < tm.napps)
            send_activate(tm.appwin[tm.apps->sel]);
        break;
    case ID_REFRESH: tick(NULL); break;
    case ID_EXIT:    w2k_win_close(tm.win, 0); break;
    case ID_SPEED_HIGH:   tm.interval_ms = 500; break;
    case ID_SPEED_NORMAL: tm.interval_ms = 1000; break;
    case ID_SPEED_LOW:    tm.interval_ms = 4000; break;
    case ID_SPEED_PAUSED: tm.interval_ms = 0; break;
    case ID_CPU_ONE:      tm.per_core = 0; break;
    case ID_CPU_PER_CORE: tm.per_core = 1; break;
    case ID_KERNEL_TIMES: tm.show_kernel = !tm.show_kernel; break;
    case ID_ABOUT:
        w2k_msgbox(tm.win, "About Task Manager",
                   "Linux Task Manager\nLinux 2000\nA Windows 2000-style desktop for X11\n\n"
                   "Applications from the window manager, processes and\n"
                   "performance figures from /proc.\n\nLinux 2000 is not affiliated with, endorsed by or sponsored by Microsoft.\nWindows is a trademark of Microsoft Corporation.",
                   MB_OK | MB_ICONINFO);
        break;
    }
    if (id >= ID_SPEED_HIGH) {
        w2k_del_timer(tick, NULL);
        if (tm.interval_ms) w2k_add_timer(tm.interval_ms, tick, NULL);
    }
    w2k_win_dirty(tm.win);
}

static W2kMenu *build_file(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_NEWTASK, "&New Task (Run...)", NULL, ICO_RUN);
    w2k_menu_sep(m);
    w2k_menu_item(m, ID_EXIT, "E&xit Task Manager", NULL, ICO_NONE);
    return m;
}

static W2kMenu *build_options(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_ALWAYSTOP, "&Always On Top", NULL, ICO_NONE);
    w2k_menu_disable(m);
    w2k_menu_item(m, ID_MINONUSE, "&Minimize On Use", NULL, ICO_NONE);
    w2k_menu_disable(m);
    w2k_menu_item(m, ID_HIDEMIN, "&Hide When Minimized", NULL, ICO_NONE);
    w2k_menu_disable(m);
    return m;
}

static W2kMenu *build_view(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_REFRESH, "&Refresh Now", "F5", ICO_NONE);
    W2kMenu *sp = w2k_menu_new();
    w2k_menu_item(sp, ID_SPEED_HIGH, "&High", NULL, ICO_NONE);
    w2k_menu_radio(sp, tm.interval_ms == 500);
    w2k_menu_item(sp, ID_SPEED_NORMAL, "&Normal", NULL, ICO_NONE);
    w2k_menu_radio(sp, tm.interval_ms == 1000);
    w2k_menu_item(sp, ID_SPEED_LOW, "&Low", NULL, ICO_NONE);
    w2k_menu_radio(sp, tm.interval_ms == 4000);
    w2k_menu_item(sp, ID_SPEED_PAUSED, "&Paused", NULL, ICO_NONE);
    w2k_menu_radio(sp, tm.interval_ms == 0);
    w2k_menu_sub(m, "&Update Speed", ICO_NONE, sp);

    W2kMenu *cp = w2k_menu_new();
    w2k_menu_item(cp, ID_CPU_ONE, "&One Graph, All CPUs", NULL, ICO_NONE);
    w2k_menu_radio(cp, !tm.per_core);
    w2k_menu_item(cp, ID_CPU_PER_CORE, "One Graph &Per CPU", NULL, ICO_NONE);
    w2k_menu_radio(cp, tm.per_core);
    w2k_menu_sub(m, "&CPU History", ICO_NONE, cp);
    w2k_menu_item(m, ID_KERNEL_TIMES, "Show &Kernel Times", NULL, ICO_NONE);
    w2k_menu_check(m, tm.show_kernel);
    return m;
}

static W2kMenu *build_help(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, ID_ABOUT, "&About Task Manager", NULL, ICO_NONE);
    return m;
}

/* ------------------------------------------------------------------ *
 * Layout and paint
 * ------------------------------------------------------------------ */
/* Measured off the Windows 2000 screenshot: the page contents sit 14
 * pixels inside the tab body, the buttons are 80x23 six apart with their
 * right edge on the list's, and the list stops seven pixels above them. */
#define PAGE_PAD  14
#define BTN_W     80
#define BTN_H     23
#define BTN_GAP    6

static void layout(W2kWin *w)
{
    tm.mb->r = (W2kRect){ 0, 0, w->w, MENUBAR_H };
    int bottom = w->h - STATUS_H;
    tm.sb->r = (W2kRect){ 0, bottom, w->w, STATUS_H };
    tm.tabs->r = (W2kRect){ 8, MENUBAR_H + 2, w->w - 16, bottom - MENUBAR_H - 6 };

    W2kRect c = w2k_tabs_client(tm.tabs);
    int by = c.y + c.h - 13 - BTN_H;
    int right = c.x + c.w - PAGE_PAD;

    tm.apps->r = (W2kRect){ c.x + PAGE_PAD, c.y + PAGE_PAD,
                            c.w - 2 * PAGE_PAD, by - 7 - (c.y + PAGE_PAD) };
    tm.b_new    = (W2kRect){ right - BTN_W, by, BTN_W, BTN_H };
    tm.b_switch = (W2kRect){ right - BTN_W * 2 - BTN_GAP, by, BTN_W, BTN_H };
    tm.b_end    = (W2kRect){ right - BTN_W * 3 - BTN_GAP * 2, by, BTN_W, BTN_H };

    tm.procs->r = tm.apps->r;
    tm.b_endproc = (W2kRect){ right - BTN_W, by, BTN_W, BTN_H };
}

static void paint(W2kWin *w, Drawable d)
{
    w2k_menubar_draw(d, tm.mb);
    w2k_tabs_draw(d, tm.tabs);
    W2kRect c = w2k_tabs_client(tm.tabs);

    switch (tm.tabs->sel) {
    case 0:
        w2k_list_draw(d, tm.apps);
        w2k_draw_pushbutton(d, &tm.b_end, "&End Task",
                            (tm.apps->sel < 0 ? BS_DISABLED : 0) |
                            (tm.down == 1 ? BS_PRESSED : 0));
        w2k_draw_pushbutton(d, &tm.b_switch, "&Switch To",
                            (tm.apps->sel < 0 ? BS_DISABLED : 0) |
                            (tm.down == 2 ? BS_PRESSED : 0));
        w2k_draw_pushbutton(d, &tm.b_new, "&New Task...",
                            tm.down == 3 ? BS_PRESSED : 0);
        break;
    case 1:
        w2k_list_draw(d, tm.procs);
        w2k_draw_pushbutton(d, &tm.b_endproc, "End &Process",
                            (tm.procs->sel < 0 ? BS_DISABLED : 0) |
                            (tm.down == 4 ? BS_PRESSED : 0));
        break;
    case 2:
        paint_perf(d, c);
        break;
    }
    w2k_status_draw(d, tm.sb);
}

static void on_sort_procs(void *u, int col)
{
    (void)u;
    if (tm.sort_col == col) tm.sort_dir = !tm.sort_dir;
    else { tm.sort_col = col; tm.sort_dir = (col >= 2); }
    refresh_procs();
}

/* The Windows menu belongs to the Applications tab, and appears only
 * while that tab is up -- as in the original. */
static void build_menubar(void)
{
    if (!tm.mb || !tm.tabs) return;
    w2k_menubar_clear(tm.mb);
    w2k_menubar_add(tm.mb, "&File", build_file);
    w2k_menubar_add(tm.mb, "&Options", build_options);
    w2k_menubar_add(tm.mb, "&View", build_view);
    if (tm.tabs->sel == 0)
        w2k_menubar_add(tm.mb, "&Windows", build_windows);
    w2k_menubar_add(tm.mb, "&Help", build_help);
}

static void on_tab_fill(void)
{
    if (tm.tabs->sel == 0) refresh_apps();
    if (tm.tabs->sel == 1) {
        /* Coming back to the process list: it has not been sampled while
         * it was hidden, so take a reading now rather than showing a
         * stale one for a second. */
        sample_procs(0);
        refresh_procs();
    }
}

static void on_tab(void *u, int i)
{
    (void)u;
    (void)i;
    on_tab_fill();                  /* the page being shown fills at once */
    build_menubar();
    w2k_win_dirty(tm.win);
}

static int event(W2kWin *w, XEvent *e)
{
    W2kList *lv = tm.tabs->sel == 0 ? tm.apps
                : tm.tabs->sel == 1 ? tm.procs : NULL;
    switch (e->type) {
    case ButtonPress: {
        if (w2k_menubar_press(tm.mb, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_tabs_press(tm.tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (lv && w2k_list_press(lv, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        int x = e->xbutton.x, y = e->xbutton.y;
        if (tm.tabs->sel == 0) {
            if (w2k_rect_hit(&tm.b_end, x, y))         tm.down = 1;
            else if (w2k_rect_hit(&tm.b_switch, x, y)) tm.down = 2;
            else if (w2k_rect_hit(&tm.b_new, x, y))    tm.down = 3;
        } else if (tm.tabs->sel == 1) {
            if (w2k_rect_hit(&tm.b_endproc, x, y))     tm.down = 4;
        }
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (lv && w2k_list_motion(lv, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        if (lv) w2k_list_release(lv, &e->xbutton);
        int d = tm.down;
        tm.down = 0;
        int x = e->xbutton.x, y = e->xbutton.y;
        if (d == 1 && w2k_rect_hit(&tm.b_end, x, y))         command(NULL, ID_ENDTASK);
        else if (d == 2 && w2k_rect_hit(&tm.b_switch, x, y)) command(NULL, ID_SWITCHTO);
        else if (d == 3 && w2k_rect_hit(&tm.b_new, x, y))    command(NULL, ID_NEWTASK);
        else if (d == 4 && w2k_rect_hit(&tm.b_endproc, x, y))command(NULL, ID_ENDPROCESS);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        if (w2k_menubar_key(tm.mb, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (w2k_tabs_key(tm.tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_F5) { command(NULL, ID_REFRESH); return 1; }
        if (lv && w2k_list_key(lv, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

int main(void)
{
    if (w2k_init("l2ktaskmgr") < 0) return 1;

    g_bright = w2k_rgb(0, 255, 0);
    g_red = w2k_rgb(255, 0, 0);
    tm.show_kernel = 1;
    g_dim    = w2k_rgb(0, 130, 0);
    g_black  = w2k_rgb(0, 0, 0);
    tm.interval_ms = 1000;
    tm.per_core = 1;

    tm.win = w2k_win_new("Linux Task Manager", "l2ktaskmgr", 520, 520, 1);
    tm.win->paint = paint;
    tm.win->event = event;
    tm.win->resized = layout;
    tm.win->min_w = 400;
    tm.win->min_h = 340;

    tm.mb = w2k_menubar_new(NULL, command);
    tm.mb->win_ref = tm.win->win;

    tm.tabs = w2k_tabs_new(NULL, on_tab);
    w2k_tabs_add(tm.tabs, "Applications");
    w2k_tabs_add(tm.tabs, "Processes");
    w2k_tabs_add(tm.tabs, "Performance");
    build_menubar();               /* needs to know which tab is up */

    tm.apps = w2k_list_new(LV_REPORT);
    tm.apps->focused = 1;
    w2k_scroll_bind(&tm.apps->vsb, tm.win);
    w2k_scroll_bind(&tm.apps->hsb, tm.win);
    w2k_list_add_col(tm.apps, "Task", 330, 0);
    w2k_list_add_col(tm.apps, "Status", 110, 0);

    tm.procs = w2k_list_new(LV_REPORT);
    tm.procs->fullrow = 1;
    tm.procs->focused = 1;
    tm.procs->on_sort = on_sort_procs;
    w2k_scroll_bind(&tm.procs->vsb, tm.win);
    w2k_scroll_bind(&tm.procs->hsb, tm.win);
    w2k_list_add_col(tm.procs, "Image Name", 170, 0);
    w2k_list_add_col(tm.procs, "PID", 55, 1);
    w2k_list_add_col(tm.procs, "CPU", 45, 1);
    w2k_list_add_col(tm.procs, "Mem Usage", 90, 1);
    tm.sort_col = 3;
    tm.sort_dir = 1;

    tm.sb = w2k_status_new();
    w2k_status_add(tm.sb, 110);
    w2k_status_add(tm.sb, 110);
    w2k_status_add(tm.sb, 0);

    layout(tm.win);
    tick(NULL);
    w2k_add_timer(tm.interval_ms, tick, NULL);

    w2k_win_show(tm.win);
    w2k_run();

    w2k_menubar_free(tm.mb);
    w2k_tabs_free(tm.tabs);
    w2k_list_free(tm.apps);
    w2k_list_free(tm.procs);
    w2k_status_free(tm.sb);
    w2k_fini();
    return 0;
}
