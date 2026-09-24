/* l2knetwork.c -- Network and Dial-up Connections.
 *
 * The Windows 2000 folder: Make New Connection and one icon per adapter,
 * the web-view pane describing the selected one (type, status, the
 * adapter's name), and the Local Area Connection Status dialog with its
 * Connection and Activity groups. Wireless adapters get a Wireless
 * Network Connection in the same style, whose status dialog carries a
 * signal-strength row and a Wireless Networks page listing what is in
 * range, with Connect and Disconnect.
 *
 * Adapters come from /sys/class/net; their names from the PCI id
 * database or the USB product string; packet counts from the kernel's
 * statistics. Wireless scanning and connecting go through NetworkManager's
 * nmcli, which is what desktop distributions ship; without it the
 * wireless page says so and the rest still works. */
#include "w2kui.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <net/if.h>
#include <signal.h>

#define MAX_CONN 16
#define MAX_NETS 64

typedef struct {
    char ifname[32];
    char label[64];             /* "Local Area Connection 2" */
    char desc[160];             /* the adapter, as Windows names it */
    int  wireless;
    int  up, carrier;
    int  nm;                    /* NetworkManager's state for it (100 connected), -1 */
    long speed_mbps;            /* -1 when the driver does not say */
    unsigned long rx, tx;       /* packets */
    char ssid[80];              /* wireless: the network joined */
    int  signal;                /* wireless: 0..100 */
    char rate[32];              /* wireless: "270 Mbit/s" */
    time_t since;               /* when the connection came up */
} Conn;

typedef struct {
    char ssid[80];
    int  signal;
    char security[40];
    int  in_use;
} Net;

static struct {
    W2kFolderWin *fw;
    W2kWin       *win;
    Conn          conn[MAX_CONN];
    int           nconn;
    int           have_nmcli;
    int           ident_link;   /* the pane's Network Identification line, -1 */
} nw;

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */
static int read_line(const char *path, char *out, int n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, n, f)) { fclose(f); out[0] = 0; return 0; }
    fclose(f);
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    return 1;
}

static int in_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    char *save = NULL;
    for (char *q = strtok_r(copy, ":", &save); q && !found;
         q = strtok_r(NULL, ":", &save)) {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s/%s", q, prog);
        if (access(tmp, X_OK) == 0) found = 1;
    }
    free(copy);
    return found;
}

/* Run argv, capture its output (stdout and stderr) into out. Returns the
 * exit status, or -1. */
static int run_capture(char *const argv[], char *out, int n)
{
    int fd[2];
    if (out && n > 0) out[0] = 0;
    if (pipe(fd) < 0) return -1;
    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (p == 0) {
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[0]);
        close(fd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    int len = 0;
    for (;;) {
        char buf[512];
        ssize_t r = read(fd[0], buf, sizeof buf);
        if (r <= 0) break;
        if (out && len < n - 1) {
            int take = (int)r;
            if (take > n - 1 - len) take = n - 1 - len;
            memcpy(out + len, buf, (size_t)take);
            len += take;
            out[len] = 0;
        }
    }
    close(fd[0]);
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void spawn(const char *cmd)
{
    pid_t p = fork();
    if (p < 0) return;
    if (p == 0) {
        if (fork() == 0) {
            signal(SIGPIPE, SIG_DFL);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        }
        _exit(0);
    }
    waitpid(p, NULL, 0);
}

/* nmcli's terse output escapes ':' in values as '\:'. Split one line into
 * fields, unescaping as it goes. */
static int split_terse(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line, *w = line;
    fields[n++] = w;
    while (*p) {
        if (*p == '\\' && p[1]) { *w++ = p[1]; p += 2; continue; }
        if (*p == ':' && n < max) { *w++ = 0; fields[n++] = w; p++; continue; }
        *w++ = *p++;
    }
    *w = 0;
    return n;
}

/* ------------------------------------------------------------------ *
 * The adapters
 * ------------------------------------------------------------------ */
/* The PCI id database, for the adapter's real name. */
static int pci_name(const char *vendor, const char *device, char *out, int n)
{
    static const char *const dbs[] = {
        "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
        "/usr/share/pci.ids", "/var/lib/pciutils/pci.ids", NULL
    };
    unsigned v = (unsigned)strtoul(vendor, NULL, 16);
    unsigned d = (unsigned)strtoul(device, NULL, 16);
    for (int i = 0; dbs[i]; i++) {
        FILE *f = fopen(dbs[i], "r");
        if (!f) continue;
        char line[512], vname[200] = "";
        int in_vendor = 0;
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            if (line[0] != '\t') {
                unsigned id = (unsigned)strtoul(line, NULL, 16);
                in_vendor = (id == v && strlen(line) > 6);
                if (in_vendor) {
                    snprintf(vname, sizeof vname, "%s", line + 6);
                    char *nl = strchr(vname, '\n');
                    if (nl) *nl = 0;
                }
                continue;
            }
            if (!in_vendor || line[1] == '\t') continue;
            unsigned id = (unsigned)strtoul(line + 1, NULL, 16);
            if (id == d && strlen(line) > 7) {
                char *nm = line + 7;
                char *nl = strchr(nm, '\n');
                if (nl) *nl = 0;
                /* "Intel Corporation" -> "Intel", as Windows abbreviates. */
                char *corp = strstr(vname, " Corporation");
                if (corp) *corp = 0;
                corp = strstr(vname, " Corp.");
                if (corp) *corp = 0;
                snprintf(out, (size_t)n, "%s %s", vname, nm);
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }
    return 0;
}

static void describe(Conn *c)
{
    char p[300], v[64] = "", d[64] = "";
    snprintf(p, sizeof p, "/sys/class/net/%s/device/vendor", c->ifname);
    read_line(p, v, sizeof v);
    snprintf(p, sizeof p, "/sys/class/net/%s/device/device", c->ifname);
    read_line(p, d, sizeof d);
    if (v[0] && d[0] && pci_name(v, d, c->desc, sizeof c->desc)) return;

    /* USB adapters carry their product string. */
    char prod[160] = "", manu[160] = "";
    snprintf(p, sizeof p, "/sys/class/net/%s/device/../product", c->ifname);
    read_line(p, prod, sizeof prod);
    snprintf(p, sizeof p, "/sys/class/net/%s/device/../manufacturer", c->ifname);
    read_line(p, manu, sizeof manu);
    if (prod[0]) {
        if (manu[0] && !strstr(prod, manu))
            snprintf(c->desc, sizeof c->desc, "%s %s", manu, prod);
        else
            snprintf(c->desc, sizeof c->desc, "%s", prod);
        return;
    }
    /* Failing that, the driver's name. */
    char link[300], drv[300];
    snprintf(link, sizeof link, "/sys/class/net/%s/device/driver", c->ifname);
    ssize_t len = readlink(link, drv, sizeof drv - 1);
    if (len > 0) {
        drv[len] = 0;
        const char *base = strrchr(drv, '/');
        snprintf(c->desc, sizeof c->desc, "%s %s Adapter",
                 base ? base + 1 : drv,
                 c->wireless ? "Wireless" : "Ethernet");
        return;
    }
    snprintf(c->desc, sizeof c->desc, "%s (%s)",
             c->wireless ? "Wireless Network Adapter" : "Network Adapter",
             c->ifname);
}

/* What NetworkManager knows about a wireless adapter: the network it is
 * on, the signal and the rate. */
static void wireless_state(Conn *c)
{
    c->ssid[0] = 0;
    c->signal = 0;
    c->rate[0] = 0;
    if (!nw.have_nmcli) return;
    char *argv[] = { "nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,RATE", "dev",
                     "wifi", "list", "ifname", c->ifname, "--rescan", "no", NULL };
    char out[8192];
    if (run_capture(argv, out, sizeof out) != 0) return;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *f[6];
        int n = split_terse(line, f, 6);
        if (n < 4 || f[0][0] != '*') continue;
        snprintf(c->ssid, sizeof c->ssid, "%s", f[1]);
        c->signal = atoi(f[2]);
        snprintf(c->rate, sizeof c->rate, "%s", f[3]);
        break;
    }
}

/* When the connection came up: NetworkManager's activation time when it
 * manages the adapter, otherwise the boot. */
static time_t connected_since(Conn *c)
{
    if (nw.have_nmcli) {
        char *argv[] = { "nmcli", "-t", "-f", "DEVICE,CONNECTION", "dev",
                         "status", NULL };
        char out[4096];
        if (run_capture(argv, out, sizeof out) == 0) {
            char con[200] = "";
            char *save = NULL;
            for (char *line = strtok_r(out, "\n", &save); line;
                 line = strtok_r(NULL, "\n", &save)) {
                char *f[3];
                int n = split_terse(line, f, 3);
                if (n >= 2 && !strcmp(f[0], c->ifname)) {
                    snprintf(con, sizeof con, "%s", f[1]);
                    break;
                }
            }
            if (con[0] && strcmp(con, "--")) {
                char *argv2[] = { "nmcli", "-g", "connection.timestamp", "con",
                                  "show", con, NULL };
                char ts[64];
                if (run_capture(argv2, ts, sizeof ts) == 0 && atol(ts) > 0)
                    return (time_t)atol(ts);
            }
        }
    }
    char up[64];
    if (read_line("/proc/uptime", up, sizeof up))
        return time(NULL) - (time_t)atof(up);
    return time(NULL);
}

/* NetworkManager's state for the adapter: 20 unavailable (no cable),
 * 30 disconnected, 100 connected, 120 failed... -1 when it does not say. */
static int nm_state(const Conn *c)
{
    if (!nw.have_nmcli) return -1;
    char *argv[] = { "nmcli", "-g", "GENERAL.STATE", "dev", "show", (char *)c->ifname, NULL };
    char out[128];
    if (run_capture(argv, out, sizeof out) != 0 || out[0] < '0' || out[0] > '9') return -1;
    return atoi(out);
}

static void refresh_stats(Conn *c)
{
    char p[300], v[64];
    /* Enabled is the adapter being up, cable or not: operstate says "down"
     * for an unplugged cable, which showed as Disabled and offered an
     * Enable that failed. NetworkManager's own disconnect leaves the link
     * up, so its word counts too: disconnected is what Disable does. */
    snprintf(p, sizeof p, "/sys/class/net/%s/flags", c->ifname);
    c->up = read_line(p, v, sizeof v) && (strtoul(v, NULL, 16) & IFF_UP) &&
            c->nm != 30 && c->nm != 110 && c->nm != 120;
    snprintf(p, sizeof p, "/sys/class/net/%s/carrier", c->ifname);
    c->carrier = read_line(p, v, sizeof v) && v[0] == '1';
    snprintf(p, sizeof p, "/sys/class/net/%s/speed", c->ifname);
    c->speed_mbps = read_line(p, v, sizeof v) ? atol(v) : -1;
    if (c->speed_mbps <= 0) c->speed_mbps = -1;
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/rx_packets", c->ifname);
    c->rx = read_line(p, v, sizeof v) ? strtoul(v, NULL, 10) : 0;
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/tx_packets", c->ifname);
    c->tx = read_line(p, v, sizeof v) ? strtoul(v, NULL, 10) : 0;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const Conn *)a)->ifname, ((const Conn *)b)->ifname);
}

static void scan_adapters(void)
{
    nw.nconn = 0;
    DIR *dp = opendir("/sys/class/net");
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp)) && nw.nconn < MAX_CONN) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
        char p[300];
        struct stat st;
        /* Virtual adapters (bridges, tunnels, docker) have no device. */
        snprintf(p, sizeof p, "/sys/class/net/%s/device", e->d_name);
        if (stat(p, &st) != 0) continue;
        Conn *c = &nw.conn[nw.nconn];
        memset(c, 0, sizeof *c);
        snprintf(c->ifname, sizeof c->ifname, "%s", e->d_name);
        snprintf(p, sizeof p, "/sys/class/net/%s/wireless", e->d_name);
        c->wireless = stat(p, &st) == 0;
        if (!c->wireless) {
            snprintf(p, sizeof p, "/sys/class/net/%s/phy80211", e->d_name);
            c->wireless = stat(p, &st) == 0;
        }
        nw.nconn++;
    }
    closedir(dp);
    qsort(nw.conn, (size_t)nw.nconn, sizeof *nw.conn, by_name);

    int nlan = 0, nwifi = 0;
    for (int i = 0; i < nw.nconn; i++) {
        Conn *c = &nw.conn[i];
        int k = c->wireless ? ++nwifi : ++nlan;
        const char *base = c->wireless ? "Wireless Network Connection"
                                       : "Local Area Connection";
        if (k == 1) snprintf(c->label, sizeof c->label, "%s", base);
        else        snprintf(c->label, sizeof c->label, "%s %d", base, k);
        describe(c);
        c->nm = nm_state(c);
        refresh_stats(c);
        if (c->wireless) wireless_state(c);
    }
}

static const char *status_word(const Conn *c)
{
    if (!c->up) return "Disabled";
    if (c->wireless) return c->ssid[0] ? "Connected" : "Not connected";
    if (!c->carrier) return "Network cable unplugged";
    return "Enabled";
}

static void speed_text(const Conn *c, char *out, int n)
{
    if (c->wireless && c->rate[0]) {
        /* "270 Mbit/s" -> "270.0 Mbps" */
        double mb = atof(c->rate);
        snprintf(out, (size_t)n, "%.1f Mbps", mb);
        return;
    }
    if (c->speed_mbps < 0) { snprintf(out, (size_t)n, "Unknown"); return; }
    if (c->speed_mbps >= 1000)
        snprintf(out, (size_t)n, "%.1f Gbps", c->speed_mbps / 1000.0);
    else
        snprintf(out, (size_t)n, "%.1f Mbps", (double)c->speed_mbps);
}

/* ------------------------------------------------------------------ *
 * The folder
 * ------------------------------------------------------------------ */
static void pane_fill(int idx)
{
    W2kFolderWin *f = nw.fw;
    w2k_folderwin_pane_clear(f);
    nw.ident_link = -1;
    if (idx == 0) {
        w2k_folderwin_pane_add(f, FW_BOLD, "Make New Connection");
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        w2k_folderwin_pane_add(f, FW_PLAIN,
            "The Network Connection Wizard helps you create a new connection "
            "so that your computer can have access to other computers and "
            "networks.");
        w2k_folderwin_status(f, "Make New Connection");
    } else if (idx > 0 && idx <= nw.nconn) {
        Conn *c = &nw.conn[idx - 1];
        char buf[200];
        w2k_folderwin_pane_add(f, FW_BOLD, c->label);
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        snprintf(buf, sizeof buf, "Type: %s",
                 c->wireless ? "Wireless Connection" : "LAN Connection");
        w2k_folderwin_pane_add(f, FW_PLAIN, buf);
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        if (c->wireless && c->up && c->ssid[0])
            snprintf(buf, sizeof buf, "Status: Connected to %s", c->ssid);
        else
            snprintf(buf, sizeof buf, "Status: %s", status_word(c));
        w2k_folderwin_pane_add(f, FW_PLAIN, buf);
        if (c->wireless && c->up && c->ssid[0]) {
            const char *q = c->signal >= 80 ? "Excellent" : c->signal >= 60 ?
                            "Very Good" : c->signal >= 40 ? "Good" :
                            c->signal >= 20 ? "Low" : "Very Low";
            snprintf(buf, sizeof buf, "Signal Strength: %s", q);
            w2k_folderwin_pane_add(f, FW_PLAIN, buf);
        }
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        w2k_folderwin_pane_add(f, FW_PLAIN, c->desc);
        w2k_folderwin_status(f, c->desc);
    } else {
        w2k_folderwin_pane_add(f, FW_PLAIN,
            "This folder contains network connections for this computer, and "
            "a wizard to help you create a new connection.");
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        w2k_folderwin_pane_add(f, FW_PLAIN,
            "To create a new connection, click Make New Connection.");
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        w2k_folderwin_pane_add(f, FW_PLAIN,
            "To open a connection, click its icon.");
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        w2k_folderwin_pane_add(f, FW_PLAIN,
            "To see the status of a wireless connection and the networks in "
            "range, open its icon.");
        w2k_folderwin_pane_add(f, FW_BLANK, NULL);
        nw.ident_link = f->nlines;
        w2k_folderwin_pane_add(f, FW_LINK, "Network Identification");
        char buf[40];
        snprintf(buf, sizeof buf, "%d object(s)", nw.nconn + 1);
        w2k_folderwin_status(f, buf);
    }
}

static void fill_list(void)
{
    W2kList *l = nw.fw->list;
    w2k_list_clear(l);
    int r = w2k_list_add(l, ICO_NET_NEW, NULL);
    w2k_list_set(l, r, 0, "Make New Connection");
    for (int i = 0; i < nw.nconn; i++) {
        r = w2k_list_add(l, nw.conn[i].wireless ? ICO_NET_WIRELESS : ICO_NET_LAN,
                         NULL);
        w2k_list_set(l, r, 0, nw.conn[i].label);
    }
}

/* ------------------------------------------------------------------ *
 * The Status dialog
 * ------------------------------------------------------------------ */
typedef struct {
    Conn    *c;
    W2kWin  *win;
    W2kTabs *tabs;
    W2kRect  props, disable, close_r;
    /* Wireless Networks page */
    W2kList *nets;
    W2kRect  refresh_r, connect_r, disconnect_r;
    Net      net[MAX_NETS];
    int      nnet;
    int      down;
    int      dirty_conn;
    int      scanned;             /* the Wireless Networks page has been filled */
} StatusDlg;

static StatusDlg *sd_active;

/* ------------------------------------------------------------------ *
 * What NetworkManager takes its time over -- a scan, a connection (which
 * can wait a long while for the network) -- runs beside the dialog: the
 * output is read through the event loop and `done` gets it, the dialog
 * saying what is going on meanwhile. These used to freeze the window,
 * with no repaint, until nmcli came back. One at a time.
 * ------------------------------------------------------------------ */
typedef struct {
    StatusDlg *sd;                /* NULL once the dialog has closed */
    Conn *c;                      /* the adapter it works on */
    pid_t pid;
    int   fd, len;
    char  out[4096];
    char  what[160];
    void (*done)(StatusDlg *, int rc, const char *out);
} Job;
static Job job = { .pid = -1, .fd = -1 };

/* A job whose dialog has closed still holds the buttons of the next one:
 * they used to look ready and do nothing, as only one runs at a time. */
static int job_busy(const StatusDlg *sd) { return job.pid > 0 && (job.sd == sd || !job.sd); }

static void job_orphaned(const Job *j, int rc);
static void report(W2kWin *over, const char *title, const char *out);

static void job_io(void *u)
{
    (void)u;
    for (;;) {
        char b[512];
        ssize_t r = read(job.fd, b, sizeof b);
        if (r > 0) {
            int take = (int)r;
            if (take > (int)sizeof job.out - 1 - job.len) take = (int)sizeof job.out - 1 - job.len;
            if (take > 0) { memcpy(job.out + job.len, b, (size_t)take); job.len += take; job.out[job.len] = 0; }
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;    /* more to come */
        break;                                                      /* the end */
    }
    w2k_del_fd(job.fd);
    close(job.fd);
    int st = 0;
    int rc = waitpid(job.pid, &st, 0) == job.pid && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    Job j = job;
    job.pid = -1;
    job.fd = -1;
    if (j.sd && j.done) j.done(j.sd, rc, j.out);
    if (j.sd) w2k_win_dirty(j.sd->win);
    else job_orphaned(&j, rc);
}

/* Start argv beside the dialog; `input`, when given, is written to its
 * standard input as one line (a network key, for nmcli --ask: never on
 * the command line, where every local user could read it in /proc). */
static int job_start(StatusDlg *sd, char *const argv[], const char *input, const char *what,
                     void (*done)(StatusDlg *, int, const char *))
{
    if (job.pid > 0) {
        report(sd->win, sd->c->label, "Another operation is still in progress. Try again when it has finished.");
        return 0;
    }
    int out[2], in[2] = { -1, -1 };
    if (pipe(out) < 0) { report(sd->win, sd->c->label, NULL); return 0; }
    if (input && pipe(in) < 0) { close(out[0]); close(out[1]); report(sd->win, sd->c->label, NULL); return 0; }
    pid_t p = fork();
    if (p < 0) {
        close(out[0]); close(out[1]);
        if (input) { close(in[0]); close(in[1]); }
        report(sd->win, sd->c->label, NULL);
        return 0;
    }
    if (p == 0) {
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(out[0]);
        close(out[1]);
        if (input) { dup2(in[0], 0); close(in[0]); close(in[1]); }
        else { int nul = open("/dev/null", O_RDONLY); if (nul >= 0) { dup2(nul, 0); close(nul); } }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(out[1]);
    if (input) {
        close(in[0]);
        size_t l = strlen(input);
        ssize_t w = write(in[1], input, l);
        w = write(in[1], "\n", 1);
        (void)w;
        close(in[1]);
    }
    memset(&job, 0, sizeof job);
    job.sd = sd;
    job.c = sd->c;
    job.pid = p;
    job.fd = out[0];
    job.done = done;
    snprintf(job.what, sizeof job.what, "%s", what);
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    w2k_add_fd(out[0], job_io, NULL);
    w2k_win_dirty(sd->win);
    return 1;
}

static void nets_parse(StatusDlg *sd, char *out)
{
    sd->nnet = 0;
    w2k_list_clear(sd->nets);
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line && sd->nnet < MAX_NETS;
         line = strtok_r(NULL, "\n", &save)) {
        char *f[6];
        int n = split_terse(line, f, 6);
        if (n < 4 || !f[1][0]) continue;
        /* One row per network name; keep the strongest. */
        int dup = -1;
        for (int i = 0; i < sd->nnet; i++)
            if (!strcmp(sd->net[i].ssid, f[1])) { dup = i; break; }
        Net *nt = dup >= 0 ? &sd->net[dup] : &sd->net[sd->nnet];
        if (dup >= 0 && nt->signal >= atoi(f[2]) && !(f[0][0] == '*')) continue;
        /* A new row starts clean: it took over the slot, and the flag, of
         * the last scan's -- the old network kept saying "(connected)". */
        if (dup < 0) memset(nt, 0, sizeof *nt);
        snprintf(nt->ssid, sizeof nt->ssid, "%s", f[1]);
        nt->signal = atoi(f[2]);
        snprintf(nt->security, sizeof nt->security, "%s",
                 f[3][0] && strcmp(f[3], "--") ? f[3] : "Open");
        nt->in_use = nt->in_use || f[0][0] == '*';
        if (dup < 0) sd->nnet++;
    }
    for (int i = 0; i < sd->nnet; i++) {
        int r = w2k_list_add(sd->nets, ICO_NET_WIRELESS, NULL);
        char buf[100];
        snprintf(buf, sizeof buf, "%s%s", sd->net[i].ssid,
                 sd->net[i].in_use ? " (connected)" : "");
        w2k_list_set(sd->nets, r, 0, buf);
        snprintf(buf, sizeof buf, "%d%%", sd->net[i].signal);
        w2k_list_set(sd->nets, r, 1, buf);
        w2k_list_set(sd->nets, r, 2, sd->net[i].security);
    }
}

static void nets_done(StatusDlg *sd, int rc, const char *out)
{
    char copy[sizeof job.out];
    snprintf(copy, sizeof copy, "%s", out);
    if (rc == 0) nets_parse(sd, copy);
}

/* The networks NetworkManager knows of; `rescan` asks it to look again
 * first, which takes seconds and runs beside the dialog. Without, it is
 * the list NetworkManager has ("--rescan no": "auto" had nmcli scan and
 * wait whenever its list was half a minute old -- nearly every time the
 * dialog opened, before it could appear). */
static void nets_scan(StatusDlg *sd, int rescan)
{
    if (!nw.have_nmcli) { sd->nnet = 0; w2k_list_clear(sd->nets); return; }
    char *argv[] = { "nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "dev",
                     "wifi", "list", "ifname", sd->c->ifname, "--rescan",
                     rescan ? "yes" : "no", NULL };
    if (rescan) { job_start(sd, argv, NULL, "Scanning for wireless networks...", nets_done); return; }
    char out[16384];
    if (run_capture(argv, out, sizeof out) != 0) return;
    nets_parse(sd, out);
}

static void duration_text(const Conn *c, char *out, int n)
{
    long s = (long)(time(NULL) - c->since);
    if (s < 0) s = 0;
    long d = s / 86400;
    s %= 86400;
    if (d > 0)
        snprintf(out, (size_t)n, "%ld day%s %02ld:%02ld:%02ld", d,
                 d == 1 ? "" : "s", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(out, (size_t)n, "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60,
                 s % 60);
}

static void right_text(Drawable d, int font, int right, int y, const char *s)
{
    w2k_text(d, font, right - w2k_text_width(font, s, -1), y, s, C_TEXT);
}

/* The five signal bars Windows XP drew, in the Windows 2000 palette. */
static void signal_bars(Drawable d, int x, int y, int pct)
{
    int lit = (pct + 10) / 20;
    for (int i = 0; i < 5; i++) {
        int h = 4 + i * 2, bx = x + i * 5, by = y + 12 - h;
        if (i < lit) w2k_fill(d, bx, by, 3, h, C_HIGHLIGHT);
        else         w2k_frame(d, bx, by, 3, h, C_SHADOW);
    }
}

static void status_paint(W2kWin *w, Drawable d)
{
    StatusDlg *sd = w->user;
    Conn *c = sd->c;
    int fh = w2k_font_height(F_UI);
    w2k_tabs_draw(d, sd->tabs);
    W2kRect cl = w2k_tabs_client(sd->tabs);

    if (sd->tabs->sel == 0) {
        /* Connection: Status, Duration, Speed (and the signal). */
        int rows = c->wireless ? 4 : 3;
        W2kRect g = { cl.x + 9, cl.y + 12, cl.w - 18, 20 + rows * 19 + 8 };
        w2k_draw_groupbox(d, &g, "Connection");
        int lx = g.x + 10, rx = g.x + g.w - 12, y = g.y + 20;
        char buf[80];
        w2k_text(d, F_UI, lx, y, "Status:", C_TEXT);
        right_text(d, F_UI, rx, y, c->wireless && c->up && c->ssid[0] ?
                   "Connected" : !c->up ? "Disabled" :
                   c->wireless ? "Not connected" :
                   c->carrier ? "Connected" : "Network cable unplugged");
        y += 19;
        w2k_text(d, F_UI, lx, y, "Duration:", C_TEXT);
        duration_text(c, buf, sizeof buf);
        right_text(d, F_UI, rx, y, c->up ? buf : "--");
        y += 19;
        w2k_text(d, F_UI, lx, y, "Speed:", C_TEXT);
        speed_text(c, buf, sizeof buf);
        right_text(d, F_UI, rx, y, buf);
        if (c->wireless) {
            y += 19;
            w2k_text(d, F_UI, lx, y, "Signal Strength:", C_TEXT);
            if (c->up && c->ssid[0]) {
                snprintf(buf, sizeof buf, "%s  ", c->ssid);
                right_text(d, F_UI, rx - 28, y, buf);
                signal_bars(d, rx - 25, y, c->signal);
            } else {
                right_text(d, F_UI, rx, y, "--");
            }
        }

        /* Activity: Sent -- icon -- Received, and the packet counts. */
        W2kRect a = { cl.x + 9, g.y + g.h + 10, cl.w - 18, 84 };
        w2k_draw_groupbox(d, &a, "Activity");
        int cx = a.x + a.w / 2, iy = a.y + 14;
        w2k_bigicon_draw(d, cx - 16, iy, c->wireless ? ICO_NET_WIRELESS : ICO_NET_LAN);
        right_text(d, F_UI, cx - 44, iy + 10, "Sent");
        w2k_text(d, F_UI, cx + 44, iy + 10, "Received", C_TEXT);
        w2k_hline(d, cx - 40, iy + 16, 20, C_SHADOW);
        w2k_hline(d, cx + 20, iy + 16, 20, C_SHADOW);
        int py = a.y + 58;
        w2k_text(d, F_UI, a.x + 12, py, "Packets:", C_TEXT);
        snprintf(buf, sizeof buf, "%lu", c->tx);
        right_text(d, F_UI, cx - 20, py, buf);
        w2k_vline(d, cx, py, fh, C_SHADOW);
        snprintf(buf, sizeof buf, "%lu", c->rx);
        right_text(d, F_UI, a.x + a.w - 12, py, buf);

        w2k_draw_pushbutton(d, &sd->props, "&Properties",
                            sd->down == 1 ? BS_PRESSED : 0);
        w2k_draw_pushbutton(d, &sd->disable, c->up ? "&Disable" : "&Enable",
                            (job_busy(sd) ? BS_DISABLED : 0) | (sd->down == 2 ? BS_PRESSED : 0));
        if (job_busy(sd))
            w2k_text(d, F_UI, sd->disable.x + sd->disable.w + 10, sd->disable.y + 5, job.what, C_TEXT);
    } else {
        w2k_text(d, F_UI, cl.x + 9, cl.y + 10, "Available networks:", C_TEXT);
        w2k_list_draw(d, sd->nets);
        if (!nw.have_nmcli) {
            w2k_text(d, F_UI, cl.x + 9, sd->nets->r.y + sd->nets->r.h + 6,
                     "Scanning needs NetworkManager (nmcli).", C_TEXT);
        } else if (job_busy(sd)) {
            w2k_text(d, F_UI, cl.x + 9, sd->nets->r.y + sd->nets->r.h + 6, job.what, C_TEXT);
        } else {
            w2k_text(d, F_UI, cl.x + 9, sd->nets->r.y + sd->nets->r.h + 6,
                     "To connect to a network, select it and click Connect.",
                     C_TEXT);
        }
        int busy = job_busy(sd) ? BS_DISABLED : 0;
        w2k_draw_pushbutton(d, &sd->refresh_r, "&Refresh",
                            busy | (sd->down == 4 ? BS_PRESSED : 0));
        w2k_draw_pushbutton(d, &sd->connect_r, "&Connect",
                            busy | (sd->nets->sel < 0 ? BS_DISABLED : 0) |
                            (sd->down == 5 ? BS_PRESSED : 0));
        w2k_draw_pushbutton(d, &sd->disconnect_r, "D&isconnect",
                            busy | (c->ssid[0] ? 0 : BS_DISABLED) |
                            (sd->down == 6 ? BS_PRESSED : 0));
    }
    w2k_draw_pushbutton(d, &sd->close_r, "&Close",
                        BS_DEFAULT | (sd->down == 3 ? BS_PRESSED : 0));
}

static void status_tick(void *u)
{
    StatusDlg *sd = u;
    refresh_stats(sd->c);
    w2k_win_dirty(sd->win);
}

static void report(W2kWin *over, const char *title, const char *out)
{
    char msg[1200];
    snprintf(msg, sizeof msg, "%.*s", (int)sizeof msg - 1, out && out[0] ? out : "The command failed.");
    w2k_msgbox(over, title, msg, MB_OK | MB_ICONERROR);
}

/* After a connect, a disconnect or a toggle: what it came to. */
static void conn_done(StatusDlg *sd, int rc, const char *out)
{
    if (rc != 0) report(sd->win, sd->c->label, out);
    sd->c->nm = nm_state(sd->c);
    refresh_stats(sd->c);
    if (sd->c->wireless) wireless_state(sd->c);
    sd->c->since = connected_since(sd->c);
    if (sd->scanned) nets_scan(sd, 0);
    sd->dirty_conn = 1;
}

/* A job that outlived its dialog: what it came to still reaches the user
 * and the folder. A failed connect (a wrong key) used to go unsaid, and
 * the folder kept showing the adapter as it was before. */
static void job_orphaned(const Job *j, int rc)
{
    if (sd_active) w2k_win_dirty(sd_active->win);       /* its buttons are free again */
    if (!j->c || j->done != conn_done) return;          /* a scan: nobody is waiting for it */
    if (rc != 0) report(sd_active ? sd_active->win : nw.win, j->c->label, j->out);
    j->c->nm = nm_state(j->c);
    refresh_stats(j->c);
    if (j->c->wireless) wireless_state(j->c);
    j->c->since = connected_since(j->c);
    pane_fill(nw.fw->list->sel);
    w2k_win_dirty(nw.win);
}

static void do_toggle(StatusDlg *sd)
{
    Conn *c = sd->c;
    if (nw.have_nmcli) {
        char *argv[] = { "nmcli", "dev", c->up ? "disconnect" : "connect",
                         c->ifname, NULL };
        job_start(sd, argv, NULL, c->up ? "Disabling..." : "Enabling...", conn_done);
    } else {
        char *argv[] = { "pkexec", "ip", "link", "set", "dev", c->ifname,
                         c->up ? "down" : "up", NULL };
        job_start(sd, argv, NULL, c->up ? "Disabling..." : "Enabling...", conn_done);
    }
}

static void do_connect(StatusDlg *sd)
{
    int i = sd->nets->sel;
    if (i < 0 || i >= sd->nnet) return;
    Net *nt = &sd->net[i];
    char pw[128] = "";
    /* The key is typed as stars, scrubbed once used, and handed to nmcli
     * on its standard input (--ask): it used to show in clear in the box
     * and ride on nmcli's command line, readable to anyone in /proc for as
     * long as the connection took. */
    if (strcmp(nt->security, "Open") != 0) {
        char label[200];
        snprintf(label, sizeof label, "Network key for %s:", nt->ssid);
        if (!w2k_prompt_secret(sd->win, "Wireless Network Connection", label,
                               pw, sizeof pw, ICO_NET_WIRELESS))
            return;
    }
    char what[200];
    snprintf(what, sizeof what, "Connecting to %.120s...", nt->ssid);
    if (pw[0]) {
        char *argv[] = { "nmcli", "--ask", "dev", "wifi", "connect", nt->ssid,
                         "ifname", sd->c->ifname, NULL };
        job_start(sd, argv, pw, what, conn_done);
    } else {
        char *argv[] = { "nmcli", "dev", "wifi", "connect", nt->ssid, "ifname",
                         sd->c->ifname, NULL };
        job_start(sd, argv, NULL, what, conn_done);
    }
    explicit_bzero(pw, sizeof pw);
}

static void do_disconnect(StatusDlg *sd)
{
    char *argv[] = { "nmcli", "dev", "disconnect", sd->c->ifname, NULL };
    job_start(sd, argv, NULL, "Disconnecting...", conn_done);
}

static void do_properties(StatusDlg *sd)
{
    if (in_path("nm-connection-editor")) {
        spawn("nm-connection-editor");
        return;
    }
    char msg[400];
    snprintf(msg, sizeof msg,
             "%s\n\n%s\n\nThe connection's settings are edited with "
             "NetworkManager's connection editor (nm-connection-editor), "
             "which is not installed.", sd->c->label, sd->c->desc);
    w2k_msgbox(sd->win, sd->c->label, msg, MB_OK | MB_ICONINFO);
}

static void status_on_tab(void *u, int i)
{
    StatusDlg *sd = u;
    /* The networks are listed when their page is first looked at, not
     * before the dialog opens for a page nobody may look at. */
    if (i == 1 && !sd->scanned) { sd->scanned = 1; nets_scan(sd, 0); }
    w2k_win_dirty(sd->win);
}

static int status_event(W2kWin *w, XEvent *e)
{
    StatusDlg *sd = w->user;
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_tabs_press(sd->tabs, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        /* Only the left button presses a button: a wheel notch over
         * Disable (a press and a release on the spot) used to take the
         * network down, and one over Connect joined the network. */
        if (e->xbutton.button != Button1) {
            if (sd->tabs->sel == 1 && w2k_list_press(sd->nets, &e->xbutton)) w2k_win_dirty(w);
            return 1;
        }
        if (sd->tabs->sel == 0) {
            if (w2k_rect_hit(&sd->props, x, y)) sd->down = 1;
            else if (w2k_rect_hit(&sd->disable, x, y)) sd->down = 2;
        } else {
            if (w2k_list_press(sd->nets, &e->xbutton)) { w2k_win_dirty(w); return 1; }
            if (w2k_rect_hit(&sd->refresh_r, x, y)) sd->down = 4;
            else if (w2k_rect_hit(&sd->connect_r, x, y) && sd->nets->sel >= 0)
                sd->down = 5;
            else if (w2k_rect_hit(&sd->disconnect_r, x, y) && sd->c->ssid[0])
                sd->down = 6;
        }
        if (w2k_rect_hit(&sd->close_r, x, y)) sd->down = 3;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        if (e->xbutton.button != Button1) {
            if (sd->tabs->sel == 1) w2k_list_release(sd->nets, &e->xbutton);
            return 1;
        }
        int b = sd->down, x = e->xbutton.x, y = e->xbutton.y;
        sd->down = 0;
        if (sd->tabs->sel == 1) w2k_list_release(sd->nets, &e->xbutton);
        if (b == 1 && w2k_rect_hit(&sd->props, x, y)) do_properties(sd);
        else if (job_busy(sd) && (b == 2 || b >= 4)) { /* one thing at a time */ }
        else if (b == 2 && w2k_rect_hit(&sd->disable, x, y)) do_toggle(sd);
        else if (b == 3 && w2k_rect_hit(&sd->close_r, x, y)) w2k_win_close(w, ID_OK);
        else if (b == 4 && w2k_rect_hit(&sd->refresh_r, x, y)) nets_scan(sd, 1);
        else if (b == 5 && w2k_rect_hit(&sd->connect_r, x, y)) do_connect(sd);
        else if (b == 6 && w2k_rect_hit(&sd->disconnect_r, x, y)) do_disconnect(sd);
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (sd->tabs->sel == 1 && w2k_list_motion(sd->nets, &e->xmotion)) {
            w2k_win_dirty(w);
            return 1;
        }
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter) {
            w2k_win_close(w, ID_OK);
            return 1;
        }
        if (w2k_tabs_key(sd->tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (sd->tabs->sel == 1 && w2k_list_key(sd->nets, &e->xkey)) {
            w2k_win_dirty(w);
            return 1;
        }
        /* The underlined letters: the buttons could only be clicked. With
         * the network list to type in, the Alt key is wanted on its page;
         * of two buttons sharing a letter, the first that can be used. */
        if (e->xkey.state & ControlMask) return 1;
        if (sd->tabs->sel == 1 && !(e->xkey.state & Mod1Mask)) return 1;
        int busy = job_busy(sd);
        if (sd->tabs->sel == 0) {
            if (ks == XK_p) do_properties(sd);
            else if (ks == (sd->c->up ? XK_d : XK_e)) { if (!busy) do_toggle(sd); }
            else if (ks == XK_c) w2k_win_close(w, ID_OK);
        } else {
            if (ks == XK_r) { if (!busy) nets_scan(sd, 1); }
            else if (ks == XK_c && !busy && sd->nets->sel >= 0) do_connect(sd);
            else if (ks == XK_i) { if (!busy && sd->c->ssid[0]) do_disconnect(sd); }
            else if (ks == XK_c) w2k_win_close(w, ID_OK);
        }
        w2k_win_dirty(w);
        return 1;
    }
    }
    return 0;
}

static void open_status(Conn *c)
{
    StatusDlg sd = { 0 };
    sd.c = c;
    c->since = connected_since(c);
    c->nm = nm_state(c);
    refresh_stats(c);
    if (c->wireless) wireless_state(c);

    /* The reference dialog: 334 by 316 inside its frame. */
    int cw = 334, chh = c->wireless ? 356 : 316;
    char title[120];
    snprintf(title, sizeof title, "%s Status", c->label);
    W2kWin *w = w2k_win_new(title, "l2knetwork", cw, chh, 0);

    sd.tabs = w2k_tabs_new(&sd, status_on_tab);
    w2k_tabs_add(sd.tabs, "General");
    if (c->wireless) w2k_tabs_add(sd.tabs, "Wireless Networks");
    sd.tabs->r = (W2kRect){ 7, 7, cw - 14, chh - 7 - 41 };
    W2kRect cl = w2k_tabs_client(sd.tabs);

    int rows = c->wireless ? 4 : 3;
    int gh = 20 + rows * 19 + 8;
    int by = cl.y + 12 + gh + 10 + 84 + 10;
    sd.props   = (W2kRect){ cl.x + 9, by, 75, 23 };
    sd.disable = (W2kRect){ cl.x + 9 + 75 + 6, by, 75, 23 };
    sd.close_r = (W2kRect){ cw - 7 - 75, chh - 7 - 23, 75, 23 };

    sd.nets = w2k_list_new(LV_REPORT);
    sd.nets->focused = 1;
    sd.nets->fullrow = 1;
    sd.nets->user = &sd;
    w2k_list_add_col(sd.nets, "Network Name", cl.w - 18 - 60 - 90 - SCROLL_W, 0);
    w2k_list_add_col(sd.nets, "Signal", 60, 1);
    w2k_list_add_col(sd.nets, "Security", 90, 0);
    sd.nets->r = (W2kRect){ cl.x + 9, cl.y + 26, cl.w - 18, cl.h - 26 - 60 };
    w2k_scroll_bind(&sd.nets->vsb, w);
    int wy = cl.y + cl.h - 9 - 23;
    sd.refresh_r    = (W2kRect){ cl.x + 9, wy, 75, 23 };
    sd.connect_r    = (W2kRect){ cl.x + cl.w - 9 - 75 * 2 - 6, wy, 75, 23 };
    sd.disconnect_r = (W2kRect){ cl.x + cl.w - 9 - 75, wy, 75, 23 };
    w->user = &sd;
    sd.win = w;
    w->paint = status_paint;
    w->event = status_event;
    w2k_win_center(w, nw.win);

    Atom t = w2k.a_net_wm_wt_dialog;
    XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&t, 1);

    w2k_add_timer(1000, status_tick, &sd);
    /* Reopened while its last job still runs: this dialog shows it, and
     * gets what it comes to. */
    if (job.pid > 0 && !job.sd && job.c == c) job.sd = &sd;
    sd_active = &sd;
    w2k_win_modal(w);
    sd_active = NULL;
    if (job.pid > 0 && job.sd == &sd) job.sd = NULL;   /* it finishes on its own */
    w2k_del_timer(status_tick, &sd);
    w2k_list_free(sd.nets);
    w2k_tabs_free(sd.tabs);
    if (sd.dirty_conn) {
        pane_fill(nw.fw->list->sel);
        w2k_win_dirty(nw.win);
    }
}

/* ------------------------------------------------------------------ *
 * The folder window's callbacks
 * ------------------------------------------------------------------ */
static void open_item(int idx)
{
    if (idx == 0) {
        if (in_path("nm-connection-editor")) spawn("nm-connection-editor");
        else
            w2k_msgbox(nw.win, "Network Connection Wizard",
                       "New connections are made with NetworkManager's "
                       "connection editor (nm-connection-editor), which is "
                       "not installed.", MB_OK | MB_ICONINFO);
        return;
    }
    if (idx > 0 && idx <= nw.nconn) open_status(&nw.conn[idx - 1]);
}

static void on_activate(void *u, int idx) { (void)u; open_item(idx); }

static void on_select(void *u, int idx)
{
    (void)u;
    pane_fill(idx);
    w2k_win_dirty(nw.win);
}

static void rescan(void)
{
    int sel = nw.fw->list->sel;
    scan_adapters();
    fill_list();
    if (sel >= nw.fw->list->n) sel = -1;
    nw.fw->list->sel = sel;
    pane_fill(sel);
    w2k_win_dirty(nw.win);
}

static void network_identification(void)
{
    char host[256] = "";
    gethostname(host, sizeof host - 1);
    char msg[400];
    snprintf(msg, sizeof msg,
             "Full computer name:\t%s\n\nThe computer's name is set with "
             "hostnamectl, which needs an administrator.", host);
    w2k_msgbox(nw.win, "Network Identification", msg, MB_OK | MB_ICONINFO);
}

static void command(void *u, int id)
{
    (void)u;
    /* A pane link arrives as FW_LAST + its line: the link is the ninth
     * line, and clicking it used to do nothing (FW_LAST + 0 was asked). */
    if (nw.ident_link >= 0 && id == FW_LAST + nw.ident_link) { network_identification(); return; }
    switch (id) {
    case FW_OPEN:       open_item(nw.fw->list->sel); break;
    case FW_REFRESH:    rescan(); break;
    case FW_PROPERTIES: {
        int i = nw.fw->list->sel;
        if (i > 0 && i <= nw.nconn) open_status(&nw.conn[i - 1]);
        break;
    }
    case FW_LAST + 10:  network_identification(); break;
    }
}

static W2kMenu *build_file(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, FW_OPEN, "&Open", NULL, ICO_NONE);
    w2k_menu_default(m);
    w2k_menu_item(m, FW_PROPERTIES, "P&roperties", NULL, ICO_PROPERTIES);
    return m;
}

static W2kMenu *build_advanced(void *u)
{
    (void)u;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, FW_LAST + 10, "&Network Identification...", NULL, ICO_NONE);
    w2k_menu_item(m, 0, "&Dial-up Preferences...", NULL, ICO_NONE);
    w2k_menu_item(m, 0, "&Advanced Settings...", NULL, ICO_NONE);
    w2k_menu_item(m, 0, "&Optional Networking Components...", NULL, ICO_NONE);
    return m;
}

static void paint(W2kWin *w, Drawable d)
{
    (void)w;
    w2k_folderwin_paint(nw.fw, d);
}

static int event(W2kWin *w, XEvent *e)
{
    if (e->type == KeyPress) {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, 0); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) {
            open_item(nw.fw->list->sel);
            return 1;
        }
    }
    if (e->type == ButtonPress && e->xbutton.button == 3) {
        int i = w2k_list_hit(nw.fw->list, e->xbutton.x, e->xbutton.y);
        if (i > 0) {
            /* The one row clicked is the selection: the row left-clicked
             * before used to stay highlighted beside it. */
            W2kList *l = nw.fw->list;
            for (int k = 0; k < l->n; k++) l->items[k].selected = k == i;
            l->sel = i;
            pane_fill(i);
            W2kMenu *m = w2k_menu_new();
            w2k_menu_item(m, FW_OPEN, "&Status", NULL, ICO_NONE);
            w2k_menu_default(m);
            w2k_menu_sep(m);
            w2k_menu_item(m, FW_PROPERTIES, "P&roperties", NULL, ICO_PROPERTIES);
            int id = w2k_menu_popup(m, e->xbutton.x_root, e->xbutton.y_root,
                                    MPOP_LEFT);
            w2k_menu_free(m);
            if (id) command(NULL, id);
            w2k_win_dirty(w);
            return 1;
        }
    }
    if (w2k_folderwin_event(nw.fw, e)) return 1;
    return e->type == ButtonPress || e->type == ButtonRelease || e->type == KeyPress;
}

static void resized(W2kWin *w)
{
    (void)w;
    w2k_folderwin_layout(nw.fw);
}

int main(int argc, char **argv)
{
    if (w2k_init("l2knetwork") < 0) return 1;
    nw.have_nmcli = in_path("nmcli");
    scan_adapters();

    /* "l2knetwork status eth0" opens that adapter's Status dialog. */
    if (argc > 2 && !strcmp(argv[1], "status")) {
        for (int i = 0; i < nw.nconn; i++)
            if (!strcmp(nw.conn[i].ifname, argv[2])) {
                nw.fw = w2k_folderwin_new("Network and Dial-up Connections",
                                          "l2knetwork", ICO_CP_NETWORK, 870, 682,
                                          NULL, command);
                nw.win = nw.fw->win;
                open_status(&nw.conn[i]);
                w2k_fini();
                return 0;
            }
    }

    nw.fw = w2k_folderwin_new("Network and Dial-up Connections", "l2knetwork",
                              ICO_CP_NETWORK, 870, 682, NULL, command);
    nw.win = nw.fw->win;
    nw.win->paint = paint;
    nw.win->event = event;
    nw.win->resized = resized;
    nw.fw->build_file = build_file;
    w2k_folderwin_extra_menu(nw.fw, "Adva&nced", build_advanced);

    nw.fw->list->on_activate = on_activate;
    nw.fw->list->on_select = on_select;
    fill_list();
    pane_fill(-1);

    w2k_folderwin_layout(nw.fw);
    w2k_win_center(nw.win, NULL);
    w2k_win_show(nw.win);
    w2k_run();
    w2k_folderwin_free(nw.fw);
    w2k_fini();
    return 0;
}
