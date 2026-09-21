/* proton.c -- Windows programs through Proton: which builds there are,
 * and how each program is to be run.
 *
 * A Windows program runs with Wine unless something says otherwise.
 * Proton Manager (apps/l2kproton.c) downloads GE-Proton builds and picks a
 * default; the Compatibility tab of a program's Properties picks a build
 * for that one program -- and, as Windows' own tab does, the version of
 * Windows the program is told it runs on.
 *
 * The builds are the ones Proton Manager installed, under
 * $XDG_DATA_HOME/l2k/proton, and the custom ones Steam uses, in its
 * compatibilitytools.d (where ProtonUp-Qt puts them too). Valve's own
 * Proton, under steamapps, only starts with the Steam client running, and
 * is not offered.
 *
 * The settings are ~/.w2k/compat: the default and the options, and one
 * Program= line for each program with settings of its own. */
#include "w2k.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * The Windows versions a program can be told it runs on
 * ------------------------------------------------------------------ */
const W2kWinVersion w2k_win_versions[] = {
    /* Wine's names for them (HKCU\Software\Wine\AppDefaults\...\Version);
     * a 64-bit program has its own for XP, and none for the versions
     * that never ran one. */
    { "Windows 95",                  "win95",  NULL },
    { "Windows 98 / Windows Me",     "win98",  NULL },
    { "Windows 2000",                "win2k",  NULL },
    { "Windows XP (Service Pack 3)", "winxp",  "winxp64" },
    { "Windows Vista",               "vista",  "vista" },
    { "Windows 7",                   "win7",   "win7" },
    { "Windows 8",                   "win8",   "win8" },
    { "Windows 8.1",                 "win81",  "win81" },
    { "Windows 10",                  "win10",  "win10" },
    { "Windows 11",                  "win11",  "win11" },
};
const int w2k_n_win_versions = (int)(sizeof w2k_win_versions / sizeof *w2k_win_versions);

/* Is this a 64-bit Windows program? From the PE header: the machine
 * field after the "PE\0\0" signature the DOS header points at. */
int w2k_exe_is_64bit(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char dos[64], pe[6];
    int is64 = 0;
    if (fread(dos, 1, sizeof dos, f) == sizeof dos && dos[0] == 'M' && dos[1] == 'Z') {
        long off = (long)(dos[60] | dos[61] << 8 | dos[62] << 16 | (unsigned long)dos[63] << 24);
        if (off > 0 && off < (1L << 24) && fseek(f, off, SEEK_SET) == 0 &&
            fread(pe, 1, sizeof pe, f) == sizeof pe && !memcmp(pe, "PE\0\0", 4)) {
            unsigned machine = (unsigned)(pe[4] | pe[5] << 8);
            is64 = machine == 0x8664 || machine == 0xaa64;
        }
    }
    fclose(f);
    return is64;
}

/* ------------------------------------------------------------------ *
 * Where things live
 * ------------------------------------------------------------------ */
void w2k_proton_data_dir(char *buf, int n)
{
    const char *x = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (x && x[0] == '/') snprintf(buf, (size_t)n, "%s/l2k", x);
    else snprintf(buf, (size_t)n, "%s/.local/share/l2k", home ? home : ".");
}

/* Digit runs compared as numbers: GE-Proton11-10 is newer than 11-9. */
static int natural_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (*ea >= '0' && *ea <= '9') ea++;
            while (*eb >= '0' && *eb <= '9') eb++;
            if (ea - a != eb - b) return (int)(ea - a) - (int)(eb - b);
            int c = strncmp(a, b, (size_t)(ea - a));
            if (c) return c;
            a = ea;
            b = eb;
            continue;
        }
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int w2k_natural_cmp(const char *a, const char *b) { return natural_cmp(a, b); }

static int by_name_newest(const void *x, const void *y)
{
    const W2kProton *a = x, *b = y;
    return natural_cmp(b->name, a->name);
}

static int scan_builds(const char *dir, int steam, W2kProton *out, int n, int max)
{
    DIR *dp = opendir(dir);
    if (!dp) return n;
    struct dirent *de;
    while ((de = readdir(dp)) && n < max) {
        if (de->d_name[0] == '.') continue;       /* "..", and half-unpacked ones */
        char path[PATH_MAX], script[PATH_MAX + 16];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        snprintf(script, sizeof script, "%s/proton", path);
        if (access(script, X_OK) != 0) continue;
        int dup = 0;
        for (int i = 0; i < n && !dup; i++) dup = !strcmp(out[i].name, de->d_name);
        if (dup) continue;                          /* ours comes first and wins */
        W2kProton *p = &out[n++];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%.127s", de->d_name);
        snprintf(p->dir, sizeof p->dir, "%.1023s", path);
        p->steam = steam;
    }
    closedir(dp);
    return n;
}

int w2k_proton_list(W2kProton *out, int max)
{
    if (max <= 0) return 0;
    char data[PATH_MAX], dir[PATH_MAX + 32];
    const char *home = getenv("HOME");
    int n = 0;
    w2k_proton_data_dir(data, sizeof data);
    snprintf(dir, sizeof dir, "%s/proton", data);
    n = scan_builds(dir, 0, out, n, max);
    static const char *const steam[] = {
        ".steam/root/compatibilitytools.d",
        ".local/share/Steam/compatibilitytools.d",
        ".steam/steam/compatibilitytools.d",
        ".var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d",
    };
    for (size_t i = 0; home && i < sizeof steam / sizeof *steam; i++) {
        snprintf(dir, sizeof dir, "%s/%s", home, steam[i]);
        n = scan_builds(dir, 1, out, n, max);
    }
    qsort(out, (size_t)n, sizeof *out, by_name_newest);
    return n;
}

int w2k_proton_find(const char *name, W2kProton *out)
{
    W2kProton *all = malloc(64 * sizeof *all);
    if (!all || !name || !name[0]) { free(all); return 0; }
    int n = w2k_proton_list(all, 64), found = 0;
    for (int i = 0; i < n && !found; i++)
        if (!strcmp(all[i].name, name)) {
            if (out) *out = all[i];
            found = 1;
        }
    free(all);
    return found;
}

/* Is `path` inside the folder `dir` (both resolved)? */
static int inside(const char *path, const char *dir)
{
    char rp[PATH_MAX], rd[PATH_MAX];
    if (!realpath(path, rp) || !realpath(dir, rd)) return 0;
    size_t l = strlen(rd);
    return !strncmp(rp, rd, l) && (rp[l] == '/' || (l == 1 && rd[0] == '/'));
}

/* Each build runs programs in a Windows of its own, a prefix under
 * $XDG_DATA_HOME/l2k/prefixes named after it. A program installed there
 * belongs to that prefix whatever runs it: `name` gets the prefix's. */
int w2k_proton_prefix_of(const char *exe, char *name, int n)
{
    char data[PATH_MAX], base[PATH_MAX + 16], rb[PATH_MAX], real[PATH_MAX];
    w2k_proton_data_dir(data, sizeof data);
    snprintf(base, sizeof base, "%s/prefixes", data);
    if (!exe || !realpath(exe, real) || !realpath(base, rb)) return 0;
    size_t l = strlen(rb);
    if (strncmp(real, rb, l) || real[l] != '/') return 0;
    const char *s = real + l + 1, *e = strchr(s, '/');
    if (!e || e == s || e - s >= n) return 0;
    snprintf(name, (size_t)n, "%.*s", (int)(e - s), s);
    return 1;
}

/* What runs a program that has not been given a choice of its own: the
 * build whose prefix it was installed into, Wine for one installed into
 * Wine's; otherwise the default. A default that has been removed falls to
 * the newest build there is, and to Wine when there is none. */
void w2k_compat_default_runner(const char *exe, char *out, int n)
{
    char pfx[128];
    int in_pfx = w2k_proton_prefix_of(exe, pfx, sizeof pfx);
    if (in_pfx && w2k_proton_find(pfx, NULL)) {
        snprintf(out, (size_t)n, "%s", pfx);
        return;
    }
    if (exe && !in_pfx) {
        char wp[PATH_MAX];
        w2k_wine_prefix(wp, sizeof wp);
        if (inside(exe, wp)) { snprintf(out, (size_t)n, "wine"); return; }
    }
    W2kCompatOptions o;
    w2k_compat_options(&o);
    /* Wine is no default for a program in a Proton prefix: Wine's own
     * prefix would not have what was installed with it. */
    if (strcmp(o.def, "wine") ? w2k_proton_find(o.def, NULL) : !in_pfx) {
        snprintf(out, (size_t)n, "%s", o.def);
        return;
    }
    W2kProton *all = malloc(64 * sizeof *all);
    int nb = all ? w2k_proton_list(all, 64) : 0;
    snprintf(out, (size_t)n, "%s", nb > 0 ? all[0].name : "wine");
    free(all);
}

/* ------------------------------------------------------------------ *
 * ~/.w2k/compat
 * ------------------------------------------------------------------ */
typedef struct {
    W2kCompatOptions o;
    int n, cap;
    struct Prog { char path[PATH_MAX]; W2kCompat c; } *prog;
} CompatFile;

static void compat_path(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.w2k/compat", home ? home : ".");
}

static void options_default(W2kCompatOptions *o)
{
    memset(o, 0, sizeof *o);
    snprintf(o->def, sizeof o->def, "wine");
    o->dxvk = o->esync = o->fsync = 1;
}

static void cf_load(CompatFile *f)
{
    memset(f, 0, sizeof *f);
    options_default(&f->o);
    char path[PATH_MAX];
    compat_path(path, sizeof path);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) > 0) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        W2kCompatOptions *o = &f->o;
        if (!strcasecmp(line, "Default")) { if (*v) snprintf(o->def, sizeof o->def, "%.127s", v); }
        else if (!strcasecmp(line, "Runtime")) o->umu = !strcasecmp(v, "umu");
        else if (!strcasecmp(line, "DXVK"))    o->dxvk = atoi(v) != 0;
        else if (!strcasecmp(line, "Esync"))   o->esync = atoi(v) != 0;
        else if (!strcasecmp(line, "Fsync"))   o->fsync = atoi(v) != 0;
        else if (!strcasecmp(line, "NVAPI"))   o->nvapi = atoi(v) != 0;
        else if (!strcasecmp(line, "HUD"))     o->hud = atoi(v) != 0;
        else if (!strcasecmp(line, "Program")) {
            /* runner TAB winver TAB flags TAB path -- the path last, whole */
            char *f1 = (char *)v, *f2 = strchr(f1, '\t');
            char *f3 = f2 ? strchr(f2 + 1, '\t') : NULL;
            char *f4 = f3 ? strchr(f3 + 1, '\t') : NULL;
            if (!f4 || f4[1] != '/') continue;
            *f2++ = 0; *f3++ = 0; *f4++ = 0;
            if (f->n == f->cap) {
                int nc = f->cap ? f->cap * 2 : 16;
                struct Prog *np = realloc(f->prog, (size_t)nc * sizeof *np);
                if (!np) break;
                f->prog = np;
                f->cap = nc;
            }
            struct Prog *p = &f->prog[f->n++];
            memset(p, 0, sizeof *p);
            snprintf(p->path, sizeof p->path, "%s", f4);
            snprintf(p->c.runner, sizeof p->c.runner, "%.127s", f1);
            snprintf(p->c.winver, sizeof p->c.winver, "%.15s", f2);
            p->c.wined3d = strchr(f3, 'w') != NULL;
            p->c.nosync = strchr(f3, 's') != NULL;
            p->c.hud = strchr(f3, 'h') != NULL;
            p->c.wmp = strchr(f3, 'm') != NULL;
            p->c.wow64 = strchr(f3, 'x') != NULL;
        }
    }
    free(line);
    fclose(fp);
}

static int cf_save(const CompatFile *f)
{
    char path[PATH_MAX], dir[PATH_MAX], tmp[PATH_MAX + 32];
    compat_path(path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }
    char real[PATH_MAX];
    const char *dest = realpath(path, real) ? real : path;
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", dest, (long)getpid());
    FILE *fp = fopen(tmp, "w");
    if (!fp) return -1;
    const W2kCompatOptions *o = &f->o;
    fprintf(fp, "# Linux 2000 -- how Windows programs are run: Proton Manager, and the\n"
                "# Compatibility tab of a program's Properties.\n");
    fprintf(fp, "Default=%s\nRuntime=%s\nDXVK=%d\nEsync=%d\nFsync=%d\nNVAPI=%d\nHUD=%d\n",
            o->def[0] ? o->def : "wine", o->umu ? "umu" : "direct",
            o->dxvk, o->esync, o->fsync, o->nvapi, o->hud);
    for (int i = 0; i < f->n; i++) {
        const struct Prog *p = &f->prog[i];
        fprintf(fp, "Program=%s\t%s\t%s%s%s%s%s\t%s\n", p->c.runner, p->c.winver,
                p->c.wined3d ? "w" : "", p->c.nosync ? "s" : "", p->c.hud ? "h" : "",
                p->c.wmp ? "m" : "", p->c.wow64 ? "x" : "", p->path);
    }
    if (ferror(fp) | fclose(fp) || rename(tmp, dest) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Settings are keyed by the program's real path, so a program reached
 * through a link, or a relative path, finds its own. */
static void key_of(const char *exe, char *out, size_t n)
{
    char real[PATH_MAX];
    snprintf(out, n, "%s", realpath(exe, real) ? real : exe);
}

int w2k_compat_get(const char *exe, W2kCompat *c)
{
    memset(c, 0, sizeof *c);
    char key[PATH_MAX];
    key_of(exe, key, sizeof key);
    CompatFile f;
    cf_load(&f);
    int found = 0;
    for (int i = 0; i < f.n && !found; i++)
        if (!strcmp(f.prog[i].path, key)) { *c = f.prog[i].c; found = 1; }
    free(f.prog);
    return found;
}

int w2k_compat_set(const char *exe, const W2kCompat *c)
{
    char key[PATH_MAX];
    key_of(exe, key, sizeof key);
    if (key[0] != '/' || strpbrk(key, "\t\n\r")) return -1;
    if (c && (strpbrk(c->runner, "\t\n\r") || strpbrk(c->winver, "\t\n\r"))) return -1;
    int empty = !c || (!c->runner[0] && !c->winver[0] && !c->wined3d && !c->nosync && !c->hud &&
                       !c->wmp && !c->wow64);
    CompatFile f;
    cf_load(&f);
    int at = -1;
    for (int i = 0; i < f.n; i++)
        if (!strcmp(f.prog[i].path, key)) { at = i; break; }
    if (empty) {
        if (at >= 0) {
            memmove(&f.prog[at], &f.prog[at + 1], (size_t)(f.n - at - 1) * sizeof *f.prog);
            f.n--;
        }
    } else {
        if (at < 0) {
            if (f.n == f.cap) {
                int nc = f.cap ? f.cap * 2 : 16;
                struct Prog *np = realloc(f.prog, (size_t)nc * sizeof *np);
                if (!np) { free(f.prog); return -1; }
                f.prog = np;
                f.cap = nc;
            }
            at = f.n++;
            memset(&f.prog[at], 0, sizeof f.prog[at]);
            snprintf(f.prog[at].path, sizeof f.prog[at].path, "%s", key);
        }
        f.prog[at].c = *c;
    }
    int rc = cf_save(&f);
    free(f.prog);
    return rc;
}

int w2k_compat_list(char (*paths)[4096], W2kCompat *cs, int max)
{
    CompatFile f;
    cf_load(&f);
    int n = 0;
    for (int i = 0; i < f.n && n < max; i++, n++) {
        snprintf(paths[n], 4096, "%s", f.prog[i].path);
        cs[n] = f.prog[i].c;
    }
    free(f.prog);
    return n;
}

void w2k_compat_options(W2kCompatOptions *o)
{
    CompatFile f;
    cf_load(&f);
    *o = f.o;
    free(f.prog);
}

int w2k_compat_save_options(const W2kCompatOptions *o)
{
    CompatFile f;
    cf_load(&f);
    f.o = *o;
    if (!f.o.def[0]) snprintf(f.o.def, sizeof f.o.def, "wine");
    int rc = cf_save(&f);
    free(f.prog);
    return rc;
}
