/* gtkcolors.c -- the desktop's colours, handed to everyone else's toolkit.
 *
 * Programs built on this toolkit read ~/.w2k/scheme; GTK and Qt programs
 * wear Chicago95 and the Windows style, whose colours are their own. So
 * whenever the scheme is saved (and at every logon) the scheme's colours
 * are written out in the forms those toolkits read:
 *
 *   ~/.config/gtk-3.0/w2k-colors.css   @define-color overrides for the
 *   ~/.config/gtk-4.0/w2k-colors.css   names Chicago95's gtk.css uses,
 *                                      imported from the user's gtk.css
 *   ~/.gtkrc-2.0                        a gtk-color-scheme line, which
 *                                      outranks the theme's own
 *   ~/.config/qt5ct/colors/Windows2000.conf   the palette qt5ct/qt6ct
 *   ~/.config/qt6ct/colors/Windows2000.conf   apply to the Windows style
 *
 * GTK and Qt read these when a program starts; the ones already running
 * keep their colours until they are restarted. */
#include "w2k.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#define MARK_BEGIN "# --- Linux 2000 colours (managed; edit ~/.w2k/scheme) ---\n"
#define MARK_END   "# --- end of Linux 2000 colours ---\n"

static void hex(int c, char *out)
{
    const unsigned char *p = w2k_scheme_rgb(c);
    snprintf(out, 8, "#%02x%02x%02x", p[0], p[1], p[2]);
}

/* A colour between two of the scheme's, for the roles Windows derives. */
static void mix(int a, int b, char *out)
{
    const unsigned char *p = w2k_scheme_rgb(a), *q = w2k_scheme_rgb(b);
    snprintf(out, 8, "#%02x%02x%02x", (p[0] + q[0]) / 2, (p[1] + q[1]) / 2,
             (p[2] + q[2]) / 2);
}

static int mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    return mkdir(tmp, 0755) == 0 || access(tmp, W_OK) == 0;
}

/* Read a whole file; NULL when it is not there. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1 << 20) { fclose(f); return NULL; }
    char *s = malloc((size_t)n + 1);
    if (!s) { fclose(f); return NULL; }
    size_t got = fread(s, 1, (size_t)n, f);
    fclose(f);
    s[got] = 0;
    return s;
}

/* Replace (or append) the block between the markers in a file the user
 * may also have written to. */
static void write_block(const char *path, const char *block)
{
    char *old = slurp(path);
    FILE *f = fopen(path, "w");
    if (!f) { free(old); return; }
    if (old) {
        char *b = strstr(old, MARK_BEGIN);
        char *e = b ? strstr(b, MARK_END) : NULL;
        if (b && e) {
            fwrite(old, 1, (size_t)(b - old), f);
            fputs(block, f);
            fputs(e + strlen(MARK_END), f);
        } else {
            fputs(old, f);
            if (old[0] && old[strlen(old) - 1] != '\n') fputc('\n', f);
            fputs(block, f);
        }
        free(old);
    } else {
        fputs(block, f);
    }
    fclose(f);
}

/* Set key=value in [section] of an ini file, keeping everything else. */
static void ini_set(const char *path, const char *section, const char *key,
                    const char *value)
{
    char *old = slurp(path);
    FILE *f = fopen(path, "w");
    if (!f) { free(old); return; }
    char head[80];
    snprintf(head, sizeof head, "[%s]", section);
    int in_section = 0, done = 0, seen_section = 0;
    size_t klen = strlen(key);
    if (old) {
        char *save = NULL;
        for (char *line = strtok_r(old, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == '[') {
                if (in_section && !done) { fprintf(f, "%s=%s\n", key, value); done = 1; }
                in_section = !strncmp(line, head, strlen(head));
                if (in_section) seen_section = 1;
                fprintf(f, "%s\n", line);
                continue;
            }
            if (in_section && !strncmp(line, key, klen) && line[klen] == '=') {
                if (!done) { fprintf(f, "%s=%s\n", key, value); done = 1; }
                continue;
            }
            fprintf(f, "%s\n", line);
        }
        if (in_section && !done) { fprintf(f, "%s=%s\n", key, value); done = 1; }
        free(old);
    }
    if (!done) {
        if (!seen_section) fprintf(f, "%s\n", head);
        fprintf(f, "%s=%s\n", key, value);
    }
    fclose(f);
}

/* ---- What is installed to choose from ------------------------------ */
static int name_cmp(const void *a, const void *b) { return strcasecmp(a, b); }

static void add_name(char (*out)[64], int *n, int max, const char *name)
{
    for (int i = 0; i < *n; i++) if (!strcmp(out[i], name)) return;
    if (*n < max) snprintf(out[(*n)++], 64, "%s", name);
}

static int dir_has(const char *dir, const char *sub)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, sub);
    return access(p, F_OK) == 0;
}

int w2k_gtk_themes(char (*out)[64], int max)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/";
    char dirs[3][1024];
    snprintf(dirs[0], sizeof dirs[0], "%s/.themes", home);
    snprintf(dirs[1], sizeof dirs[1], "%s/.local/share/themes", home);
    snprintf(dirs[2], sizeof dirs[2], "/usr/share/themes");
    int n = 0;
    for (int d = 0; d < 3; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            if (e->d_name[0] == '.') continue;
            char t[1100];
            snprintf(t, sizeof t, "%s/%s", dirs[d], e->d_name);
            if (dir_has(t, "gtk-3.0") || dir_has(t, "gtk-2.0") || dir_has(t, "gtk-4.0"))
                add_name(out, &n, max, e->d_name);
        }
        closedir(dp);
    }
    qsort(out, (size_t)n, 64, name_cmp);
    return n;
}

int w2k_icon_themes(char (*out)[64], int max)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/";
    char dirs[3][1024];
    snprintf(dirs[0], sizeof dirs[0], "%s/.icons", home);
    snprintf(dirs[1], sizeof dirs[1], "%s/.local/share/icons", home);
    snprintf(dirs[2], sizeof dirs[2], "/usr/share/icons");
    int n = 0;
    for (int d = 0; d < 3; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            if (e->d_name[0] == '.') continue;
            if (!strcmp(e->d_name, "default") || !strcmp(e->d_name, "hicolor") ||
                !strcmp(e->d_name, "locolor")) continue;
            char t[1100];
            snprintf(t, sizeof t, "%s/%s", dirs[d], e->d_name);
            if (!dir_has(t, "index.theme")) continue;
            /* A cursor theme has nothing but its cursors directory. */
            DIR *q = opendir(t);
            int icons = 0;
            struct dirent *f;
            while (q && (f = readdir(q)))
                if (f->d_name[0] != '.' && strcmp(f->d_name, "cursors") &&
                    strcmp(f->d_name, "index.theme") && strcmp(f->d_name, "cursor.theme"))
                    icons = 1;
            if (q) closedir(q);
            if (icons) add_name(out, &n, max, e->d_name);
        }
        closedir(dp);
    }
    qsort(out, (size_t)n, 64, name_cmp);
    return n;
}

int w2k_qt_styles(char (*out)[64], int max)
{
    int n = 0;
    add_name(out, &n, max, "Windows");
    add_name(out, &n, max, "Fusion");
    const char *dirs[] = { "/usr/lib/x86_64-linux-gnu/qt5/plugins/styles",
                           "/usr/lib/x86_64-linux-gnu/qt6/plugins/styles",
                           "/usr/lib/qt5/plugins/styles", "/usr/lib/qt6/plugins/styles",
                           "/usr/lib64/qt5/plugins/styles", "/usr/lib64/qt6/plugins/styles",
                           "/usr/lib/qt/plugins/styles", NULL };
    for (int d = 0; dirs[d]; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            /* libqcleanlooksstyle.so -> Cleanlooks; the ct plugins are not styles. */
            const char *nm = e->d_name;
            const char *end = strstr(nm, "style.so");
            if (strncmp(nm, "libq", 4) || !end || strstr(nm, "ct-style")) continue;
            int len = (int)(end - nm - 4);
            if (len <= 0 || len > 60) continue;
            char name[64];
            snprintf(name, sizeof name, "%.*s", len, nm + 4);
            if (name[0] >= 'a' && name[0] <= 'z') name[0] = (char)(name[0] - 'a' + 'A');
            add_name(out, &n, max, name);
        }
        closedir(dp);
    }
    qsort(out, (size_t)n, 64, name_cmp);
    return n;
}

/* ---- GTK 3 and 4 ---------------------------------------------------- */
static void gtk_css(const char *dir, int adwaita)
{
    char path[1100];
    if (!mkdirs(dir)) return;
    snprintf(path, sizeof path, "%s/w2k-colors.css", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    char face[8], hil[8], light[8], shadow[8], dk[8], btext[8], gray[8], win[8],
         wtext[8], atitle[8], ititle[8], ttext[8], ittext[8], menu[8], mtext[8],
         sel[8], seltext[8], tip[8], tiptext[8], scroll[8], frame[8];
    hex(C_FACE, face);        hex(C_HILIGHT, hil);      hex(C_LIGHT, light);
    hex(C_SHADOW, shadow);    hex(C_DKSHADOW, dk);      hex(C_TEXT, btext);
    hex(C_GRAYTEXT, gray);    hex(C_WINDOW, win);       hex(C_WINDOWTEXT, wtext);
    hex(C_ACTIVETITLE, atitle); hex(C_INACTIVETITLE, ititle);
    hex(C_TITLETEXT, ttext);  hex(C_INACTIVETITLETEXT, ittext);
    hex(C_MENU, menu);        hex(C_MENUTEXT, mtext);   hex(C_HIGHLIGHT, sel);
    hex(C_HIGHLIGHTTEXT, seltext); hex(C_TOOLTIP, tip); hex(C_TOOLTIPTEXT, tiptext);
    hex(C_SCROLLBAR, scroll); hex(C_WINDOWFRAME, frame);
    fprintf(f,
        "/* Written by Linux 2000 from ~/.w2k/scheme -- do not edit;\n"
        " * change the colours in Display Properties > Appearance. These are the\n"
        " * names Chicago95's gtk.css draws with. */\n"
        "@define-color bg_color %s;\n"
        "@define-color fg_color %s;\n"
        "@define-color base_color %s;\n"
        "@define-color text_color %s;\n"
        "@define-color text_color_light %s;\n"
        "@define-color selected_bg_color %s;\n"
        "@define-color selected_fg_color %s;\n"
        "@define-color selected_inactive_bg_color %s;\n"
        "@define-color tooltip_bg_color %s;\n"
        "@define-color tooltip_fg_color %s;\n"
        "@define-color bg_bright %s;\n"
        "@define-color border_color %s;\n"
        "@define-color border_bright %s;\n"
        "@define-color border_light %s;\n"
        "@define-color border_dark %s;\n"
        "@define-color border_shade %s;\n"
        "@define-color outline_color %s;\n"
        "@define-color font_color %s;\n"
        "@define-color disabled_font_shadow %s;\n"
        "@define-color disabled_font %s;\n"
        "@define-color menu_text_color %s;\n"
        "@define-color menu_bg_color %s;\n"
        "@define-color button_text_color %s;\n"
        "@define-color button_bg_color %s;\n"
        "@define-color scrollbar_trough_bg_color %s;\n"
        "@define-color scrollbar_bg_color %s;\n"
        "@define-color scrollbar_solid_color %s;\n"
        "@define-color window_title_bg_color %s;\n"
        "@define-color window_title_text_color %s;\n"
        "@define-color inactive_title_bg_color %s;\n"
        "@define-color inactive_title_text_color %s;\n"
        "@define-color dark_bg_color %s;\n"
        "@define-color dark_fg_color %s;\n"
        "@define-color theme_bg_color %s;\n"
        "@define-color theme_fg_color %s;\n"
        "@define-color theme_base_color %s;\n"
        "@define-color theme_text_color %s;\n"
        "@define-color theme_selected_bg_color %s;\n"
        "@define-color theme_selected_fg_color %s;\n"
        "@define-color theme_tooltip_bg_color %s;\n"
        "@define-color theme_tooltip_fg_color %s;\n"
        "@define-color theme_unfocused_selected_bg_color %s;\n"
        "@define-color info_bg_color %s;\n"
        "@define-color info_fg_color %s;\n"
        "@define-color link_color %s;\n"
        "@define-color insensitive_bg_color %s;\n"
        "@define-color insensitive_fg_color %s;\n"
        "@define-color insensitive_base_color %s;\n"
        "@define-color unfocused_bg_color %s;\n"
        "@define-color unfocused_fg_color %s;\n"
        "@define-color unfocused_text_color %s;\n"
        "@define-color unfocused_base_color %s;\n"
        "@define-color borders %s;\n"
        "@define-color unfocused_borders %s;\n",
        face, btext, win, wtext, gray, sel, seltext, shadow, tip, tiptext, win,
        face, hil, light, dk, shadow, frame, wtext, hil, shadow, mtext, menu,
        btext, face, scroll, face, face, atitle, ttext, ititle, ittext,
        face, btext, face, btext, win, wtext, sel, seltext, tip, tiptext, sel,
        tip, tiptext, sel, face, gray, face, face, btext, wtext, win, shadow,
        shadow);
    /* libadwaita programs ignore the theme but honour these names. */
    if (adwaita)
        fprintf(f,
            "@define-color window_bg_color %s;\n"
            "@define-color window_fg_color %s;\n"
            "@define-color view_bg_color %s;\n"
            "@define-color view_fg_color %s;\n"
            "@define-color headerbar_bg_color %s;\n"
            "@define-color headerbar_fg_color %s;\n"
            "@define-color headerbar_backdrop_color %s;\n"
            "@define-color headerbar_border_color %s;\n"
            "@define-color accent_bg_color %s;\n"
            "@define-color accent_fg_color %s;\n"
            "@define-color accent_color %s;\n"
            "@define-color dialog_bg_color %s;\n"
            "@define-color dialog_fg_color %s;\n"
            "@define-color popover_bg_color %s;\n"
            "@define-color popover_fg_color %s;\n"
            "@define-color card_bg_color %s;\n"
            "@define-color card_fg_color %s;\n"
            "@define-color sidebar_bg_color %s;\n"
            "@define-color sidebar_fg_color %s;\n"
            "@define-color sidebar_backdrop_color %s;\n"
            "@define-color secondary_sidebar_bg_color %s;\n"
            "@define-color secondary_sidebar_fg_color %s;\n"
            "@define-color thumbnail_bg_color %s;\n"
            "@define-color thumbnail_fg_color %s;\n"
            "@define-color shade_color %s;\n",
            face, btext, win, wtext, atitle, ttext, ititle, ititle, sel, seltext,
            sel, face, btext, menu, mtext, face, btext, face, btext, face, face,
            btext, win, wtext, shadow);
    /* libadwaita (1.7 here) takes neither the named colours nor its CSS
     * variables from the user's stylesheet, but it does take rules, so
     * the colours go on as rules: flat, but the desktop's. GTK 4 has no
     * Chicago95 anyway. */
    if (adwaita)
        fprintf(f,
            "\n/* Rules, for libadwaita programs. */\n"
            "window, window.background, dialog, .dialog, .sidebar-pane,\n"
            "  .content-pane, .navigation-sidebar, preferencespage, statuspage {\n"
            "  background-color: %s; color: %s; }\n"
            "headerbar, .titlebar, .top-bar { background: %s; color: %s;\n"
            "  box-shadow: none; }\n"
            "headerbar:backdrop, .titlebar:backdrop { background: %s; color: %s; }\n"
            "button, .toggle { background: %s; color: %s; }\n"
            "button.suggested-action, button.default { background: %s; color: %s; }\n"
            "entry, textview, text, .view, listview, columnview, treeview,\n"
            "  scrolledwindow > viewport { background-color: %s; color: %s; }\n"
            "selection, *:selected, row:selected, listview > row:selected,\n"
            "  columnview > listview > row:selected { background-color: %s; color: %s; }\n"
            "menu, popover > contents, popover > arrow, .menu, menubar,\n"
            "  .menubar { background: %s; color: %s; }\n"
            "tooltip, tooltip > * { background: %s; color: %s; }\n"
            "label:disabled, button:disabled { color: %s; }\n"
            "switch:checked, checkbutton check:checked, radiobutton radio:checked,\n"
            "  progressbar > trough > progress, scale > trough > highlight {\n"
            "  background: %s; color: %s; }\n",
            face, btext, atitle, ttext, ititle, ittext, face, btext, sel, seltext,
            win, wtext, sel, seltext, menu, mtext, tip, tiptext, gray, sel, seltext);
    fclose(f);

    /* The theme and icons the desktop is set to. */
    snprintf(path, sizeof path, "%s/settings.ini", dir);
    ini_set(path, "Settings", "gtk-theme-name", w2k_gtk_theme);
    ini_set(path, "Settings", "gtk-icon-theme-name", w2k_icon_theme);

    /* The user's gtk.css imports it; created when there is none. */
    snprintf(path, sizeof path, "%s/gtk.css", dir);
    char *css = slurp(path);
    if (!css || !strstr(css, "w2k-colors.css")) {
        FILE *g = fopen(path, "a");
        if (g) {
            if (css && css[0] && css[strlen(css) - 1] != '\n') fputc('\n', g);
            fputs("/* Linux 2000: the desktop's colours, kept in "
                  "w2k-colors.css. */\n@import url(\"w2k-colors.css\");\n", g);
            fclose(g);
        }
    }
    free(css);
}

/* ---- GTK 2 ---------------------------------------------------------- */
static void gtk2(const char *home)
{
    char face[8], btext[8], win[8], wtext[8], sel[8], seltext[8], tip[8], tiptext[8];
    hex(C_FACE, face);  hex(C_TEXT, btext);  hex(C_WINDOW, win);
    hex(C_WINDOWTEXT, wtext);  hex(C_HIGHLIGHT, sel);  hex(C_HIGHLIGHTTEXT, seltext);
    hex(C_TOOLTIP, tip);  hex(C_TOOLTIPTEXT, tiptext);
    char block[1200];
    /* An rc file's gtk-color-scheme outranks the theme's, which is how
     * the desktop's own colours reach a GTK 2 program. */
    snprintf(block, sizeof block,
             MARK_BEGIN
             "gtk-theme-name=\"%s\"\n"
             "gtk-icon-theme-name=\"%s\"\n"
             "gtk-color-scheme = \"bg_color:%s\\nfg_color:%s\\nbase_color:%s\\n"
             "text_color:%s\\nselected_bg_color:%s\\nselected_fg_color:%s\\n"
             "tooltip_bg_color:%s\\ntooltip_fg_color:%s\"\n"
             MARK_END,
             w2k_gtk_theme, w2k_icon_theme,
             face, btext, win, wtext, sel, seltext, tip, tiptext);
    char path[1100];
    snprintf(path, sizeof path, "%s/.gtkrc-2.0", home);
    write_block(path, block);
}

/* ---- Qt, through qt5ct and qt6ct ----------------------------------- */
static void qtct(const char *cfg, const char *which)
{
    char dir[1100], path[1200];
    snprintf(dir, sizeof dir, "%s/%s/colors", cfg, which);
    if (!mkdirs(dir)) return;
    snprintf(path, sizeof path, "%s/Windows2000.conf", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    char wtext[8], face[8], light[8], midlight[8], dark[8], mid[8], white[8],
         btext[8], base[8], dk[8], sel[8], seltext[8], tip[8], tiptext[8],
         gray[8], black[8];
    hex(C_WINDOWTEXT, wtext); hex(C_FACE, face); hex(C_HILIGHT, light);
    mix(C_FACE, C_HILIGHT, midlight); hex(C_SHADOW, dark);
    mix(C_FACE, C_SHADOW, mid); hex(C_WHITE, white); hex(C_TEXT, btext);
    hex(C_WINDOW, base); hex(C_DKSHADOW, dk); hex(C_HIGHLIGHT, sel);
    hex(C_HIGHLIGHTTEXT, seltext); hex(C_TOOLTIP, tip); hex(C_TOOLTIPTEXT, tiptext);
    hex(C_GRAYTEXT, gray); hex(C_BLACK, black);
    /* The roles in qt5ct's order: WindowText, Button, Light, Midlight,
     * Dark, Mid, Text, BrightText, ButtonText, Base, Window, Shadow,
     * Highlight, HighlightedText, Link, LinkVisited, AlternateBase,
     * NoRole, ToolTipBase, ToolTipText. */
    fprintf(f, "[ColorScheme]\n");
    for (int state = 0; state < 3; state++) {
        const char *t = state == 1 ? gray : wtext;
        const char *bt = state == 1 ? gray : btext;
        fprintf(f, "%s=%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, "
                   "#0000ff, #55007f, %s, %s, %s, %s\n",
                state == 0 ? "active_colors" : state == 1 ? "disabled_colors"
                                                          : "inactive_colors",
                t, face, light, midlight, dark, mid, t, white, bt, base, face,
                dk, sel, seltext, face, black, tip, tiptext);
    }
    fclose(f);

    /* The style and the icon theme, in qt5ct.conf / qt6ct.conf. */
    snprintf(path, sizeof path, "%s/%s/%s.conf", cfg, which, which);
    ini_set(path, "Appearance", "style", w2k_qt_style);
    ini_set(path, "Appearance", "icon_theme", w2k_icon_theme);
    ini_set(path, "Appearance", "custom_palette", "true");
}

void w2k_scheme_export_gtk(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    /* GTK and qt5ct look under $XDG_CONFIG_HOME, ~/.config by default. */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char cfg[1100];
    if (xdg && *xdg) snprintf(cfg, sizeof cfg, "%s", xdg);
    else             snprintf(cfg, sizeof cfg, "%s/.config", home);
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/gtk-3.0", cfg);
    gtk_css(dir, 0);
    snprintf(dir, sizeof dir, "%s/gtk-4.0", cfg);
    gtk_css(dir, 1);
    gtk2(home);
    qtct(cfg, "qt5ct");
    qtct(cfg, "qt6ct");
}
