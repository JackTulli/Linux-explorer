/* power.c -- the battery, the mains and the backlight, read from sysfs.
 *
 * The notification area's battery and the Power Options applet both come
 * here. A battery is a /sys/class/power_supply entry of type Battery whose
 * scope is not "Device" (a wireless mouse reports one of those too); the
 * mains is any Mains, USB or ADP supply that says it is online. The
 * backlight is the first /sys/class/backlight entry; setting it writes
 * the file when the user may, and otherwise goes through brightnessctl
 * or pkexec, whichever the machine has. */
#include "w2k.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_line(const char *path, char *out, int n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, n, f)) { fclose(f); out[0] = 0; return 0; }
    fclose(f);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' ')) out[--l] = 0;
    return 1;
}

static long read_num(const char *dir, const char *leaf, long dflt)
{
    char path[512], buf[64];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    if (!read_line(path, buf, sizeof buf)) return dflt;
    char *end;
    long v = strtol(buf, &end, 10);
    return end == buf ? dflt : v;
}

int w2k_power_read(W2kPower *p)
{
    memset(p, 0, sizeof *p);
    p->percent = -1;
    p->minutes_left = -1;
    p->ac_online = -1;
    /* Development aid: W2K_FAKE_BATTERY=<pct>,<charging|discharging|full>[,<minutes>]
     * pretends there is one, for looking at the meter on a desktop. */
    const char *fake = getenv("W2K_FAKE_BATTERY");
    if (fake && *fake) {
        char st[32] = "";
        int mins = -1;
        if (sscanf(fake, "%d,%31[a-z],%d", &p->percent, st, &mins) >= 1) {
            p->present = 1;
            p->charging = !strcmp(st, "charging") ? 1 : !strcmp(st, "full") ? 2 : 0;
            p->ac_online = p->charging != 0;
            p->minutes_left = mins;
            snprintf(p->name, sizeof p->name, "BAT0");
            return 1;
        }
    }
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char dir[400], path[512], type[32], buf[64];
        snprintf(dir, sizeof dir, "/sys/class/power_supply/%.200s", de->d_name);
        snprintf(path, sizeof path, "%s/type", dir);
        if (!read_line(path, type, sizeof type)) continue;

        if (!strcmp(type, "Battery")) {
            if (p->present) continue;              /* the first one wins */
            snprintf(path, sizeof path, "%s/scope", dir);
            if (read_line(path, buf, sizeof buf) && !strcmp(buf, "Device")) continue;
            if (read_num(dir, "present", 1) == 0) continue;

            long pct = read_num(dir, "capacity", -1);
            long now = read_num(dir, "energy_now", -1), full = read_num(dir, "energy_full", -1);
            long rate = read_num(dir, "power_now", -1);
            if (now < 0 || full <= 0) {
                now = read_num(dir, "charge_now", -1);
                full = read_num(dir, "charge_full", -1);
                rate = read_num(dir, "current_now", -1);
            }
            if (pct < 0 && now >= 0 && full > 0) pct = now * 100 / full;
            if (pct < 0) continue;                 /* nothing to show */
            if (pct > 100) pct = 100;
            p->present = 1;
            p->percent = (int)pct;
            snprintf(p->name, sizeof p->name, "%s", de->d_name);

            snprintf(path, sizeof path, "%s/status", dir);
            p->charging = 0;
            if (read_line(path, buf, sizeof buf)) {
                if (!strcmp(buf, "Charging")) p->charging = 1;
                else if (!strcmp(buf, "Full") || !strcmp(buf, "Not charging")) p->charging = 2;
            }
            if (rate > 0 && now >= 0 && full > 0) {
                long left = p->charging == 1 ? full - now : now;
                if (left < 0) left = 0;
                p->minutes_left = (int)(left * 60 / rate);
            }
        } else if (!strcmp(type, "Mains") || !strcmp(type, "USB") ||
                   !strncmp(type, "ADP", 3)) {
            long on = read_num(dir, "online", -1);
            if (on >= 0 && p->ac_online != 1) p->ac_online = on ? 1 : 0;
        }
    }
    closedir(d);
    if (p->present && p->ac_online < 0) p->ac_online = p->charging != 0;
    return p->present;
}

void w2k_power_describe(const W2kPower *p, char *out, int n)
{
    if (!p->present) { snprintf(out, (size_t)n, "No battery is detected"); return; }
    char when[48] = "";
    if (p->minutes_left >= 0 && p->charging != 2) {
        int h = p->minutes_left / 60, m = p->minutes_left % 60;
        if (h) snprintf(when, sizeof when, "%d hr %02d min ", h, m);
        else   snprintf(when, sizeof when, "%d min ", m);
    }
    if (p->charging == 2 && p->percent >= 100)
        snprintf(out, (size_t)n, "Fully charged (100%%)");
    else if (p->charging == 1)
        snprintf(out, (size_t)n, "%s(%d%%) until fully charged", when, p->percent);
    else if (p->charging == 2)
        snprintf(out, (size_t)n, "%d%% available (plugged in, not charging)", p->percent);
    else
        snprintf(out, (size_t)n, "%s(%d%%) remaining", when, p->percent);
}

/* ------------------------------------------------------------------ *
 * The backlight
 * ------------------------------------------------------------------ */
static int backlight_dir(char *out, int n)
{
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while (!found && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(out, (size_t)n, "/sys/class/backlight/%.200s", de->d_name);
        if (read_num(out, "max_brightness", 0) > 0) found = 1;
    }
    closedir(d);
    return found;
}

static int fake_backlight = -1;

int w2k_backlight_available(void)
{
    if (getenv("W2K_FAKE_BACKLIGHT")) return 1;
    char dir[400];
    return backlight_dir(dir, sizeof dir);
}

int w2k_backlight_get(int *pct)
{
    if (getenv("W2K_FAKE_BACKLIGHT")) {
        if (fake_backlight < 0) fake_backlight = atoi(getenv("W2K_FAKE_BACKLIGHT"));
        *pct = fake_backlight;
        return 1;
    }
    char dir[400];
    if (!backlight_dir(dir, sizeof dir)) return 0;
    long max = read_num(dir, "max_brightness", 0);
    long cur = read_num(dir, "actual_brightness", -1);
    if (cur < 0) cur = read_num(dir, "brightness", -1);
    if (max <= 0 || cur < 0) return 0;
    *pct = (int)((cur * 100 + max / 2) / max);
    return 1;
}

static int have_program(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    char buf[1024];
    while (*path) {
        const char *e = strchr(path, ':');
        size_t l = e ? (size_t)(e - path) : strlen(path);
        snprintf(buf, sizeof buf, "%.*s/%s", (int)l, path, name);
        if (access(buf, X_OK) == 0) return 1;
        if (!e) break;
        path = e + 1;
    }
    return 0;
}

int w2k_backlight_set(int pct)
{
    if (pct < 1) pct = 1;                 /* never all the way off */
    if (pct > 100) pct = 100;
    if (getenv("W2K_FAKE_BACKLIGHT")) { fake_backlight = pct; return 0; }
    char dir[400];
    if (!backlight_dir(dir, sizeof dir)) return -1;
    long max = read_num(dir, "max_brightness", 0);
    if (max <= 0) return -1;
    long value = (max * pct + 50) / 100;
    if (value < 1) value = 1;

    /* The plain way first: a udev rule or a group often makes the file
     * writable for the console user. */
    char path[512];
    snprintf(path, sizeof path, "%s/brightness", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        int ok = fprintf(f, "%ld\n", value) > 0;
        ok = fclose(f) == 0 && ok;
        if (ok) return 0;
    }
    char cmd[1024];
    if (have_program("brightnessctl")) {
        snprintf(cmd, sizeof cmd, "brightnessctl -q s %d%% >/dev/null 2>&1", pct);
        if (system(cmd) == 0) return 0;
    }
    if (have_program("pkexec")) {
        /* The path is sysfs's own, built above from a directory name that
         * is checked to exist; the number is a number. */
        snprintf(cmd, sizeof cmd, "pkexec sh -c 'echo %ld > \"%s\"' >/dev/null 2>&1",
                 value, path);
        if (system(cmd) == 0) return 0;
    }
    return -1;
}

/* logind answers CanSuspend / CanHibernate with "yes", "no", "na" or
 * "challenge"; without busctl the kernel's own list of sleep states
 * (/sys/power/state: "mem" to stand by, "disk" to hibernate) decides. */
int w2k_power_can(const char *what)
{
    int hib = !strcmp(what, "hibernate");
    if (!hib && strcmp(what, "suspend")) return 0;
    if (have_program("busctl")) {
        char cmd[200], buf[64] = "";
        snprintf(cmd, sizeof cmd,
                 "busctl --system call org.freedesktop.login1 /org/freedesktop/login1 "
                 "org.freedesktop.login1.Manager %s 2>/dev/null",
                 hib ? "CanHibernate" : "CanSuspend");
        FILE *p = popen(cmd, "r");
        if (p) {
            if (!fgets(buf, sizeof buf, p)) buf[0] = 0;
            pclose(p);
            if (buf[0]) return strstr(buf, "\"yes\"") != NULL || strstr(buf, "\"challenge\"") != NULL;
        }
    }
    char st[128] = "";
    if (!read_line("/sys/power/state", st, sizeof st)) return 0;
    return strstr(st, hib ? "disk" : "mem") != NULL;
}

void w2k_power_action(const char *what)
{
    if (strcmp(what, "suspend") && strcmp(what, "hibernate") && strcmp(what, "hybrid-sleep")) return;
    char cmd[200];
    /* A moment for the screen to settle, then logind; loginctl for the
     * odd machine where systemctl is not on the path. */
    snprintf(cmd, sizeof cmd, "(sleep 0.3; systemctl %s || loginctl %s) >/dev/null 2>&1 &", what, what);
    if (system(cmd) != 0) { /* logind will say why in the journal */ }
}

/* logind's effective HandleLidSwitch / HandlePowerKey: the main file,
 * then the drop-ins in order, last one wins. */
static void logind_key(const char *key, char *out, int n, const char *dflt)
{
    snprintf(out, (size_t)n, "%s", dflt);
    const char *files[] = { "/etc/systemd/logind.conf", NULL };
    char paths[64][256];
    int np = 0;
    for (int i = 0; files[i] && np < 64; i++) snprintf(paths[np++], 256, "%s", files[i]);
    DIR *d = opendir("/etc/systemd/logind.conf.d");
    if (d) {
        char names[64][200];
        int nn = 0;
        struct dirent *de;
        while ((de = readdir(d)) && nn < 64) {
            size_t l = strlen(de->d_name);
            if (l > 5 && !strcmp(de->d_name + l - 5, ".conf")) snprintf(names[nn++], 200, "%s", de->d_name);
        }
        closedir(d);
        for (int i = 1; i < nn; i++) {          /* lexical order, as logind reads them */
            char t[200];
            snprintf(t, sizeof t, "%s", names[i]);
            int j = i - 1;
            while (j >= 0 && strcmp(names[j], t) > 0) { snprintf(names[j + 1], 200, "%s", names[j]); j--; }
            snprintf(names[j + 1], 200, "%s", t);
        }
        for (int i = 0; i < nn && np < 64; i++)
            snprintf(paths[np++], 256, "/etc/systemd/logind.conf.d/%s", names[i]);
    }
    size_t kl = strlen(key);
    for (int i = 0; i < np; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, key, kl) || p[kl] != '=') continue;
            p += kl + 1;
            p[strcspn(p, "\r\n #")] = 0;
            if (*p) snprintf(out, (size_t)n, "%s", p);
        }
        fclose(f);
    }
}

void w2k_power_lid_get(char *lid, int nl, char *button, int nb)
{
    logind_key("HandleLidSwitch", lid, nl, "suspend");
    logind_key("HandlePowerKey", button, nb, "poweroff");
}

static int valid_action(const char *a)
{
    return !strcmp(a, "ignore") || !strcmp(a, "suspend") || !strcmp(a, "hibernate") ||
           !strcmp(a, "poweroff") || !strcmp(a, "lock") || !strcmp(a, "hybrid-sleep");
}

int w2k_power_lid_set(const char *lid, const char *button)
{
    if (!valid_action(lid) || !valid_action(button) || !have_program("pkexec")) return -1;
    /* The words are from the list above, so the command is fixed text. */
    char cmd[700];
    snprintf(cmd, sizeof cmd,
             "pkexec sh -c 'mkdir -p /etc/systemd/logind.conf.d && "
             "printf \"[Login]\\nHandleLidSwitch=%s\\nHandleLidSwitchExternalPower=%s\\n"
             "HandlePowerKey=%s\\n\" > /etc/systemd/logind.conf.d/50-linux2000.conf && "
             "(systemctl kill -s HUP systemd-logind 2>/dev/null; true)' >/dev/null 2>&1",
             lid, lid, button);
    return system(cmd) == 0 ? 0 : -1;
}

int w2k_is_laptop(void)
{
    W2kPower p;
    return w2k_power_read(&p) || w2k_backlight_available();
}
