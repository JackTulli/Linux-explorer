/* xdgicon.c -- find an application's icon and turn it into a w2k icon.
 *
 * A .desktop file names its icon ("firefox", "text-editor") and the file
 * itself lives somewhere under an icon theme. Themes disagree about where:
 * hicolor and Adwaita use <theme>/48x48/apps/name.png, while others --
 * Chicago95 among them -- use <theme>/apps/48/name.png. Both layouts are
 * tried rather than parsing every index.theme.
 *
 * Whatever size turns up is scaled to the 16 and 32 pixel cells the shell
 * draws, by averaging (a box filter): nearest-neighbour turns a 48-pixel
 * icon into a mess at 16. */
#include "w2k.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Sizes to try, best first, for a target of 32. */
static const int size_pref[] = { 32, 48, 24, 64, 22, 16, 96, 128, 256 };
static const char *categories[] = { "apps", "categories", "devices", "places",
                                    "mimetypes", "actions", "status", NULL };

static int readable(const char *p) { return access(p, R_OK) == 0; }

/* ------------------------------------------------------------------ *
 * The directories a name is looked for in, and what is in them.
 *
 * A name was looked for by asking for every candidate file in turn: six
 * themes under six bases, nine sizes, seven categories, three layouts --
 * 6,804 access() calls for a name that is not there, and the first
 * classic Start menu of a session asked 327,517 times (the better part
 * of a second). Now the candidate directories that exist are found once,
 * in the same order of preference, and each one's PNG names are read on
 * first use into a sorted list: a lookup is a binary search per
 * directory and no system calls.
 * ------------------------------------------------------------------ */
typedef struct {
    char  *path;
    char **names;                 /* sorted, ".png" dropped */
    int    n, loaded;
} IconDir;
static IconDir *dirs;
static int ndirs, capdirs;
static char dirs_key[256];        /* the theme order they were found for */

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void dirs_free(void)
{
    for (int i = 0; i < ndirs; i++) {
        for (int k = 0; k < dirs[i].n; k++) free(dirs[i].names[k]);
        free(dirs[i].names);
        free(dirs[i].path);
    }
    ndirs = 0;
}

static void dirs_add(const char *p)
{
    for (int i = 0; i < ndirs; i++) if (!strcmp(dirs[i].path, p)) return;
    if (ndirs == capdirs) {
        int cap = capdirs ? capdirs * 2 : 64;
        IconDir *d = realloc(dirs, (size_t)cap * sizeof *d);
        if (!d) return;
        dirs = d;
        capdirs = cap;
    }
    char *dup = strdup(p);
    if (!dup) return;
    dirs[ndirs++] = (IconDir){ dup, NULL, 0, 0 };
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void dir_load(IconDir *d)
{
    d->loaded = 1;
    DIR *dp = opendir(d->path);
    if (!dp) return;
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        size_t l = strlen(de->d_name);
        if (l <= 4 || strcmp(de->d_name + l - 4, ".png")) continue;
        if (d->n == cap) {
            int nc = cap ? cap * 2 : 64;
            char **g = realloc(d->names, (size_t)nc * sizeof *g);
            if (!g) break;
            d->names = g;
            cap = nc;
        }
        char *s = malloc(l - 3);
        if (!s) break;
        memcpy(s, de->d_name, l - 4);
        s[l - 4] = 0;
        d->names[d->n++] = s;
    }
    closedir(dp);
    if (d->n > 1) qsort(d->names, (size_t)d->n, sizeof *d->names, cmp_name);
}

static int dir_has(IconDir *d, const char *name)
{
    if (!d->loaded) dir_load(d);
    return d->n && bsearch(&name, d->names, (size_t)d->n, sizeof *d->names, cmp_name) != NULL;
}

/* The icon theme the desktop is set to, from the GTK settings we write. */
static const char *override_theme;      /* w2k_icon_by_name_in() */

static const char *user_theme(void)
{
    static char theme[64];
    static int done;
    if (override_theme && override_theme[0]) return override_theme;
    /* The scheme's choice, which every process reloads on a change. */
    if (w2k_icon_theme[0]) return w2k_icon_theme;
    if (done) return theme;
    done = 1;

    const char *home = getenv("HOME");
    char path[512];
    if (home) {
        snprintf(path, sizeof path, "%s/.config/gtk-3.0/settings.ini", home);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "gtk-icon-theme-name=", 20)) {
                    char *v = line + 20;
                    v[strcspn(v, "\r\n")] = 0;
                    snprintf(theme, sizeof theme, "%.63s", v);
                    break;
                }
            fclose(f);
        }
    }
    return theme;
}

static int try_path(const char *path, char *out, int n)
{
    if (!readable(path)) return 0;
    snprintf(out, (size_t)n, "%s", path);
    return 1;
}

/* The candidate directories for the current theme order, in the order a
 * name is looked for in them: theme by theme, base by base, then size,
 * category and layout (<theme>/48x48/apps, <theme>/apps/48, legacy). */
static void dirs_build(void)
{
    const char *home = getenv("HOME");
    char h1[512] = "", h2[512] = "", h3[512] = "";
    if (home) {
        snprintf(h1, sizeof h1, "%s/.local/share/icons", home);
        snprintf(h2, sizeof h2, "%s/.icons", home);
        /* Flatpak applications export their icons here, and they are not on
         * any other search path. */
        snprintf(h3, sizeof h3, "%s/.local/share/flatpak/exports/share/icons",
                 home);
    }
    const char *bases[] = { h1, h2, h3,
                            "/var/lib/flatpak/exports/share/icons",
                            "/usr/local/share/icons",
                            "/usr/share/icons", NULL };
    const char *themes[] = { user_theme(), "Chicago95", "hicolor", "Adwaita",
                             "gnome", "locolor", NULL };
    char key[256];
    snprintf(key, sizeof key, "%.60s|%.180s", user_theme(), home ? home : "");
    if (ndirs && !strcmp(key, dirs_key)) return;
    dirs_free();
    snprintf(dirs_key, sizeof dirs_key, "%s", key);

    for (int t = 0; themes[t]; t++) {
        if (!themes[t][0]) continue;
        int seen = 0;                          /* the user's theme may be one of these */
        for (int u = 0; u < t; u++) if (!strcmp(themes[u], themes[t])) seen = 1;
        if (seen) continue;
        for (int b = 0; bases[b]; b++) {
            if (!bases[b][0]) continue;
            char root[1024], p[1200];
            snprintf(root, sizeof root, "%s/%s", bases[b], themes[t]);
            if (!is_dir(root)) continue;
            for (unsigned s = 0; s < sizeof size_pref / sizeof *size_pref; s++) {
                int sz = size_pref[s];
                for (int c = 0; categories[c]; c++) {
                    snprintf(p, sizeof p, "%s/%dx%d/%s", root, sz, sz, categories[c]);
                    if (is_dir(p)) dirs_add(p);
                    snprintf(p, sizeof p, "%s/%s/%d", root, categories[c], sz);
                    if (is_dir(p)) dirs_add(p);
                    snprintf(p, sizeof p, "%s/%dx%d/legacy", root, sz, sz);
                    if (is_dir(p)) dirs_add(p);
                }
            }
        }
    }
}

/* Resolve an icon name to a PNG on disk. Absolute paths are used as given. */
static int find_icon_file(const char *name, char *out, int n)
{
    if (!name || !*name) return 0;
    if (strchr(name, '/')) return try_path(name, out, n);

    /* A name that already ends in .png is still just a name to look up, but
     * strip the suffix so the search does not double it. */
    char bare[256];
    snprintf(bare, sizeof bare, "%s", name);
    size_t bl = strlen(bare);
    if (bl > 4 && !strcmp(bare + bl - 4, ".png")) bare[bl - 4] = 0;

    dirs_build();
    for (int i = 0; i < ndirs; i++)
        if (dir_has(&dirs[i], bare)) {
            char path[1400];
            snprintf(path, sizeof path, "%s/%s.png", dirs[i].path, bare);
            if (try_path(path, out, n)) return 1;
        }

    /* The flat directories, where a lot of Debian packages put theirs. */
    char path[1024];
    const char *flat[] = { "/usr/share/pixmaps", "/usr/local/share/pixmaps",
                           NULL };
    for (int i = 0; flat[i]; i++) {
        snprintf(path, sizeof path, "%s/%s.png", flat[i], bare);
        if (try_path(path, out, n)) return 1;
    }
    return 0;
}

/* Box-filter scale of an RGBA image to n x n. Shared with the window
 * manager, which scales the icons applications publish. */
unsigned char *w2k_rgba_scale(const unsigned char *src, int sw, int sh, int n)
{
    unsigned char *dst = malloc((size_t)n * n * 4);
    if (!dst) return NULL;
    for (int y = 0; y < n; y++) {
        int y0 = y * sh / n, y1 = (y + 1) * sh / n;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < n; x++) {
            int x0 = x * sw / n, x1 = (x + 1) * sw / n;
            if (x1 <= x0) x1 = x0 + 1;
            long r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int sy = y0; sy < y1 && sy < sh; sy++)
                for (int sx = x0; sx < x1 && sx < sw; sx++) {
                    const unsigned char *p = src + ((size_t)sy * sw + sx) * 4;
                    /* Weight colour by alpha so transparent edges do not
                     * drag black into the visible pixels. */
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3];
                    a += p[3];
                    count++;
                }
            unsigned char *o = dst + ((size_t)y * n + x) * 4;
            if (a > 0) {
                o[0] = (unsigned char)(r / a);
                o[1] = (unsigned char)(g / a);
                o[2] = (unsigned char)(b / a);
                o[3] = (unsigned char)(a / (count ? count : 1));
            } else {
                o[0] = o[1] = o[2] = o[3] = 0;
            }
        }
    }
    return dst;
}

static int lookup(const char *name, const char *key);

int w2k_icon_by_name(const char *name)
{
    return lookup(name, name);
}

int w2k_icon_by_name_in(const char *name, const char *theme)
{
    if (!name || !*name) return ICO_APP;
    char key[300];
    snprintf(key, sizeof key, "%.63s|%.200s", theme ? theme : "", name);
    override_theme = theme;
    int id = lookup(name, key);
    override_theme = NULL;
    return id;
}

/* Names already looked up, and what they came to: a hash table that
 * grows, where a 512-entry list stopped caching when it was full (and
 * from then on looked every name up, and registered its icon, afresh).
 * Emptied when the icon theme changes, so a new theme's icons show. */
typedef struct { char *key; int id; } Named;
static Named *named;
static unsigned nnamed, capnamed;
static char named_theme[64];

static unsigned hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static void named_clear(void)
{
    for (unsigned i = 0; i < capnamed; i++) free(named[i].key);
    free(named);
    named = NULL;
    nnamed = capnamed = 0;
}

static Named *named_slot(const char *key)
{
    if (!capnamed) return NULL;
    for (unsigned i = hash(key) & (capnamed - 1);; i = (i + 1) & (capnamed - 1))
        if (!named[i].key || !strcmp(named[i].key, key)) return &named[i];
}

static void named_put(const char *key, int id)
{
    if ((nnamed + 1) * 4 > capnamed * 3) {
        unsigned cap = capnamed ? capnamed * 2 : 256;
        Named *old = named;
        unsigned oldcap = capnamed;
        named = calloc(cap, sizeof *named);
        if (!named) { named = old; return; }
        capnamed = cap;
        for (unsigned i = 0; i < oldcap; i++)
            if (old[i].key) *named_slot(old[i].key) = old[i];
        free(old);
    }
    Named *s = named_slot(key);
    if (s->key) { s->id = id; return; }
    s->key = strdup(key);
    if (!s->key) return;
    s->id = id;
    nnamed++;
}

static int lookup(const char *name, const char *key)
{
    if (!name || !*name) return ICO_APP;
    if (strcmp(named_theme, user_theme())) {
        named_clear();
        snprintf(named_theme, sizeof named_theme, "%s", user_theme());
    }
    Named *hit = named_slot(key);
    if (hit && hit->key) return hit->id;

    int id = ICO_APP;
    /* A path is a file to read, in whatever format it is; only a bare
     * name goes through the icon themes. Not anything at all, though: an
     * icon is small, and a big file named as one (a photo, a video --
     * music players pass cover art) is not decoded to find that out. */
    if (strchr(name, '/')) {
        struct stat st;
        if (stat(name, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= 2 * 1024 * 1024)
            id = w2k_icon_from_file(name);
        named_put(key, id);
        return id;
    }
    char path[1024];
    if (find_icon_file(name, path, sizeof path)) {
        int w = 0, h = 0;
        unsigned char *rgba = w2k_png_load(path, &w, &h);
        if (rgba && w > 0 && h > 0) {
            unsigned char *i16 = w2k_rgba_scale(rgba, w, h, 16);
            unsigned char *i32 = w2k_rgba_scale(rgba, w, h, 32);
            free(rgba);
            if (i16 && i32) id = w2k_icon_register(i16, i32);
            else { free(i16); free(i32); }
        } else {
            free(rgba);
        }
    }
    named_put(key, id);
    return id;
}
