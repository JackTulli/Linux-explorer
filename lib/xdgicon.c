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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CACHE 512

static struct { char name[128]; int id; } cache[MAX_CACHE];
static int ncache;

/* Sizes to try, best first, for a target of 32. */
static const int size_pref[] = { 32, 48, 24, 64, 22, 16, 96, 128, 256 };
static const char *categories[] = { "apps", "categories", "devices", "places",
                                    "mimetypes", "actions", "status", NULL };

static int readable(const char *p) { return access(p, R_OK) == 0; }

/* The icon theme the desktop is set to, from the GTK settings we write. */
static const char *user_theme(void)
{
    static char theme[64];
    static int done;
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

/* Look for `name` in one theme directory, in both layouts. */
static int find_in_theme(const char *base, const char *theme, const char *name,
                         char *out, int n)
{
    char path[1024];
    for (unsigned s = 0; s < sizeof size_pref / sizeof *size_pref; s++) {
        int sz = size_pref[s];
        for (int c = 0; categories[c]; c++) {
            snprintf(path, sizeof path, "%s/%s/%dx%d/%s/%s.png",
                     base, theme, sz, sz, categories[c], name);
            if (try_path(path, out, n)) return 1;
            snprintf(path, sizeof path, "%s/%s/%s/%d/%s.png",
                     base, theme, categories[c], sz, name);
            if (try_path(path, out, n)) return 1;
            snprintf(path, sizeof path, "%s/%s/%dx%d/%s/%s.png",
                     base, theme, sz, sz, "legacy", name);
            if (try_path(path, out, n)) return 1;
        }
    }
    return 0;
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

    for (int t = 0; themes[t]; t++) {
        if (!themes[t][0]) continue;
        for (int b = 0; bases[b]; b++) {
            if (!bases[b][0]) continue;
            if (find_in_theme(bases[b], themes[t], bare, out, n)) return 1;
        }
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

int w2k_icon_by_name(const char *name)
{
    if (!name || !*name) return ICO_APP;
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].name, name)) return cache[i].id;

    int id = ICO_APP;
    /* A path is a file to read, in whatever format it is; only a bare
     * name goes through the icon themes. */
    if (strchr(name, '/')) {
        id = w2k_icon_from_file(name);
        if (ncache < MAX_CACHE) {
            snprintf(cache[ncache].name, sizeof cache[ncache].name, "%s", name);
            cache[ncache].id = id;
            ncache++;
        }
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

    if (ncache < MAX_CACHE) {
        snprintf(cache[ncache].name, sizeof cache[ncache].name, "%s", name);
        cache[ncache].id = id;
        ncache++;
    }
    return id;
}
