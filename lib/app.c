/* app.c -- toolkit initialisation: colours, fonts, GCs, cursors, atoms. */
#define _POSIX_C_SOURCE 200809L
#include "w2k.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

W2k w2k;

/* The Windows 2000 "Windows Standard" appearance scheme, verbatim. */
static const unsigned char standard[N_COLORS][3] = {
    [C_FACE]              = { 212, 208, 200 },
    [C_HILIGHT]           = { 255, 255, 255 },
    [C_LIGHT]             = { 212, 208, 200 },
    [C_SHADOW]            = { 128, 128, 128 },
    [C_DKSHADOW]          = {  64,  64,  64 },
    [C_TEXT]              = {   0,   0,   0 },
    [C_GRAYTEXT]          = { 128, 128, 128 },
    [C_WINDOW]            = { 255, 255, 255 },
    [C_WINDOWTEXT]        = {   0,   0,   0 },
    [C_WINDOWFRAME]       = {   0,   0,   0 },
    [C_ACTIVETITLE]       = {  10,  36, 106 },
    [C_ACTIVETITLE2]      = { 166, 202, 240 },
    [C_INACTIVETITLE]     = { 128, 128, 128 },
    [C_INACTIVETITLE2]    = { 192, 192, 192 },
    [C_TITLETEXT]         = { 255, 255, 255 },
    [C_INACTIVETITLETEXT] = { 212, 208, 200 },
    [C_MENU]              = { 212, 208, 200 },
    [C_MENUTEXT]          = {   0,   0,   0 },
    [C_HIGHLIGHT]         = {  10,  36, 106 },
    [C_HIGHLIGHTTEXT]     = { 255, 255, 255 },
    [C_DESKTOP]           = {  58, 110, 165 },
    [C_SCROLLBAR]         = { 212, 208, 200 },
    [C_TOOLTIP]           = { 255, 255, 225 },
    [C_TOOLTIPTEXT]       = {   0,   0,   0 },
    [C_APPWORKSPACE]      = { 128, 128, 128 },
    [C_BLACK]             = {   0,   0,   0 },
    [C_WHITE]             = { 255, 255, 255 },
};

/* The Windows XP "Luna" scheme, as an option beside Windows Standard.
 *
 * The values are the ones in luna.theme's [Control Panel\\Colors], checked
 * against a 1:1 screenshot of Windows XP: the face there measures
 * 236,233,216 and the active caption 0,83,225, which is what the theme
 * file says. XP paints its captions and taskbar with gradients and rounded
 * corners that no colour table can describe -- the shell draws those
 * separately (see w2k_theme) -- but the flat colours are all here. */
static const unsigned char luna[N_COLORS][3] = {
    [C_FACE]              = { 236, 233, 216 },
    [C_HILIGHT]           = { 255, 255, 255 },
    [C_LIGHT]             = { 241, 239, 226 },
    [C_SHADOW]            = { 172, 168, 153 },
    [C_DKSHADOW]          = { 113, 111, 100 },
    [C_TEXT]              = {   0,   0,   0 },
    [C_GRAYTEXT]          = { 172, 168, 153 },
    [C_WINDOW]            = { 255, 255, 255 },
    [C_WINDOWTEXT]        = {   0,   0,   0 },
    [C_WINDOWFRAME]       = {   0,   0,   0 },
    [C_ACTIVETITLE]       = {   0,  84, 227 },
    [C_ACTIVETITLE2]      = {  61, 149, 255 },
    [C_INACTIVETITLE]     = { 122, 150, 223 },
    [C_INACTIVETITLE2]    = { 157, 185, 235 },
    [C_TITLETEXT]         = { 255, 255, 255 },
    [C_INACTIVETITLETEXT] = { 216, 228, 248 },
    [C_MENU]              = { 255, 255, 255 },
    [C_MENUTEXT]          = {   0,   0,   0 },
    [C_HIGHLIGHT]         = {  49, 106, 197 },
    [C_HIGHLIGHTTEXT]     = { 255, 255, 255 },
    [C_DESKTOP]           = {   0,  78, 152 },
    [C_SCROLLBAR]         = { 212, 208, 200 },
    [C_TOOLTIP]           = { 255, 255, 225 },
    [C_TOOLTIPTEXT]       = {   0,   0,   0 },
    [C_APPWORKSPACE]      = { 128, 128, 128 },
    [C_BLACK]             = {   0,   0,   0 },
    [C_WHITE]             = { 255, 255, 255 },
};

/* Windows 7 Basic -- the scheme Windows 7 falls back to without Aero
 * (Personalization > Basic and High Contrast Themes). Flat greys, a pale
 * blue caption with dark text, and the bright blue selection that
 * replaced XP's navy. Values from the theme's colour table. */
static const unsigned char basic7[N_COLORS][3] = {
    [C_FACE]              = { 240, 240, 240 },
    [C_HILIGHT]           = { 255, 255, 255 },
    [C_LIGHT]             = { 227, 227, 227 },
    [C_SHADOW]            = { 160, 160, 160 },
    [C_DKSHADOW]          = { 105, 105, 105 },
    [C_TEXT]              = {   0,   0,   0 },
    [C_GRAYTEXT]          = { 109, 109, 109 },
    [C_WINDOW]            = { 255, 255, 255 },
    [C_WINDOWTEXT]        = {   0,   0,   0 },
    [C_WINDOWFRAME]       = { 100, 100, 100 },
    [C_ACTIVETITLE]       = { 185, 209, 234 },
    [C_ACTIVETITLE2]      = { 215, 228, 242 },
    [C_INACTIVETITLE]     = { 215, 228, 242 },
    [C_INACTIVETITLE2]    = { 239, 245, 250 },
    /* Windows 7 Basic writes its caption text in black, not white. */
    [C_TITLETEXT]         = {   0,   0,   0 },
    [C_INACTIVETITLETEXT] = { 109, 109, 109 },
    [C_MENU]              = { 240, 240, 240 },
    [C_MENUTEXT]          = {   0,   0,   0 },
    [C_HIGHLIGHT]         = {  51, 153, 255 },
    [C_HIGHLIGHTTEXT]     = { 255, 255, 255 },
    [C_DESKTOP]           = {  33,  60, 100 },
    [C_SCROLLBAR]         = { 240, 240, 240 },
    [C_TOOLTIP]           = { 255, 255, 255 },
    [C_TOOLTIPTEXT]       = {   0,   0,   0 },
    [C_APPWORKSPACE]      = { 171, 171, 171 },
    [C_BLACK]             = {   0,   0,   0 },
    [C_WHITE]             = { 255, 255, 255 },
};

/* Font fallback chains. The first entry is the authentic choice; the rest
 * keep us alive on a machine without the adobe-75dpi bitmap package. */
static const char *font_chain[N_FONTS][6] = {
    [F_UI] = {
        "-*-helvetica-medium-r-normal--11-*-*-*-*-*-iso8859-1",
        "-*-helvetica-medium-r-normal--12-*-*-*-*-*-iso8859-1",
        "-*-lucida-medium-r-normal-sans-11-*-*-*-*-*-iso8859-1",
        "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso8859-1",
        "fixed", NULL },
    [F_UI_BOLD] = {
        "-*-helvetica-bold-r-normal--11-*-*-*-*-*-iso8859-1",
        "-*-helvetica-bold-r-normal--12-*-*-*-*-*-iso8859-1",
        "-*-lucida-bold-r-normal-sans-11-*-*-*-*-*-iso8859-1",
        "-misc-fixed-bold-r-normal--13-*-*-*-*-*-iso8859-1",
        "fixed", NULL },
    /* Notepad/console text: Fixedsys' closest free relative. */
    [F_FIXED] = {
        "-misc-fixed-medium-r-normal--13-*-*-*-c-70-iso8859-1",
        "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso8859-1",
        "-*-courier-medium-r-normal--12-*-*-*-*-*-iso8859-1",
        "9x15", "fixed", NULL },
    /* Desktop / large icon captions. */
    [F_ICON] = {
        "-*-helvetica-medium-r-normal--11-*-*-*-*-*-iso8859-1",
        "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso8859-1",
        "fixed", NULL },
};

/* The live scheme: starts as Windows Standard, then takes overrides. */
static unsigned char scheme[N_COLORS][3];
char w2k_wallpaper[1024];
int  w2k_wallpaper_style;
int  w2k_force_decorations = 1;

int  w2k_start_banner_mode = SB_WINDOWS;
char w2k_start_banner_custom[128];
int  w2k_start_banner_top[3]    = { 0, 0,   0 };
int  w2k_start_banner_bottom[3] = { 0, 0, 255 };
int  w2k_start_banner_dither;
int  w2k_start_icon = SI_FLAG;
int  w2k_start_search = 1;
/* The two-column Start menu, as Windows XP introduced it. Off is the
 * classic single column of Windows 2000. */
int  w2k_start_panel;
int  w2k_start_small_icons;
int  w2k_start_personalized;
int  w2k_taskbar_ontop = 1;
int  w2k_taskbar_autohide;
int  w2k_taskbar_showclock = 1;
int  w2k_taskbar_edge = TB_BOTTOM;
int  w2k_taskbar_rows = 1;
int  w2k_taskbar_quicklaunch = 1;
int  w2k_taskbar_labels;        /* Windows 7's "Never combine" */
int  w2k_taskbar_small;         /* Windows 7's "Use small icons" */

/* The effects list, in the order the Performance Options dialog shows it. */
unsigned char w2k_effects[N_EFFECTS];

/* The monitor arrangement, as Display Properties last applied it. */
W2kMonitorCfg w2k_monitor_cfg[8];
int           w2k_monitor_cfg_n;

/* Underlines under menu mnemonics. With the effect on they stay hidden
 * until Alt is pressed, which is what Windows 2000 does. */
int w2k_accel_shown = 1;
char w2k_gtk_theme[64] = "Chicago95", w2k_icon_theme[64] = "Chicago95", w2k_qt_style[64] = "Windows";

void w2k_accel_show(void)
{
    w2k_accel_shown = 1;
}

void w2k_accel_reset(void)
{
    w2k_accel_shown = !w2k_effects[FX_HIDE_ACCEL];
}

/* Mouse, keyboard and bell -- the Control Panel's input applets. Values
 * are what the X server is told (see lib/input.c), not just decoration. */
int w2k_dblclk_ms = 500;        /* double-click speed */
int w2k_mouse_swap;             /* left-handed button order */
int w2k_mouse_speed = 4;        /* pointer acceleration, 1..10 */
int w2k_key_delay = 500;        /* auto-repeat delay, ms */
int w2k_key_rate = 30;          /* auto-repeat rate, characters/second */
int w2k_caret_blink = 530;      /* caret blink half-period, ms */
int w2k_bell_on = 1;
int w2k_bell_volume = 50;       /* percent */
int w2k_bell_pitch = 400;       /* Hz */
int w2k_bell_duration = 100;    /* ms */

/* Folder Options (Explorer's View tab). */
int w2k_folder_hidden;          /* show hidden files and folders */
int w2k_folder_hide_ext = 1;    /* hide extensions for known file types */
int w2k_folder_fullpath;        /* full path in the title bar */
int w2k_folder_singleclick;     /* click items as follows */
int w2k_folder_newwindow;       /* open each folder in its own window */
int w2k_folder_tooltips = 1;    /* pop-up descriptions for shell items */
/* Explorer's View > Toolbars and Status Bar. */
int w2k_view_toolbar = 1;
int w2k_view_address = 1;
int w2k_view_status = 1;

static const struct { const char *label; unsigned char supported, best; }
effect_info[N_EFFECTS] = {
    [FX_ANIM_MINMAX]       = { "Animate windows when minimizing and maximizing", 1, 1 },
    [FX_FADE_MENUS]        = { "Fade or slide menus into view",                  1, 0 },
    [FX_FADE_TOOLTIPS]     = { "Fade or slide ToolTips into view",               0, 0 },
    [FX_FADE_MENUITEMS]    = { "Fade out menu items after clicking",             0, 0 },
    [FX_MENU_SHADOW]       = { "Show shadows under menus",                       1, 0 },
    [FX_CURSOR_SHADOW]     = { "Show shadows under mouse pointer",               1, 0 },
    [FX_TRANSLUCENT_SEL]   = { "Show translucent selection rectangle",           1, 1 },
    [FX_DRAG_CONTENTS]     = { "Show window contents while dragging",            1, 1 },
    [FX_SLIDE_COMBO]       = { "Slide open combo boxes",                         1, 1 },
    [FX_SLIDE_TASKBUTTONS] = { "Slide taskbar buttons",                          0, 0 },
    [FX_SMOOTH_FONTS]      = { "Smooth edges of screen fonts",                   1, 0 },
    [FX_SMOOTH_SCROLL]     = { "Smooth-scroll list boxes",                       1, 1 },
    [FX_FOLDER_BACKGROUND] = { "Use a background image for each folder type",    0, 0 },
    [FX_COMMON_TASKS]      = { "Use common tasks in folders",                    0, 0 },
    [FX_ICON_SHADOW]       = { "Use drop shadows for icon labels on the desktop", 1, 1 },
    [FX_VISUAL_STYLES]     = { "Use visual styles on windows and buttons",       0, 0 },
    [FX_HIDE_ACCEL]        = { "Hide keyboard navigation indicators until I "
                               "use the Alt key",                              1, 1 },
};

const char *w2k_effect_label(int i)
{
    return (i >= 0 && i < N_EFFECTS) ? effect_info[i].label : "";
}

int w2k_effect_supported(int i)
{
    return (i >= 0 && i < N_EFFECTS) ? effect_info[i].supported : 0;
}

/* 0 = adjust for best appearance, 1 = for best performance, 2 = let the
 * desktop choose (which, on hardware this old-looking, means everything
 * that costs nothing). */
void w2k_effects_preset(int which)
{
    for (int i = 0; i < N_EFFECTS; i++) {
        if (!effect_info[i].supported) { w2k_effects[i] = 0; continue; }
        if (which == 1) w2k_effects[i] = 0;
        else            w2k_effects[i] = effect_info[i].best;
    }
    /* Dragging a window's contents is worth keeping even when trimming. */
    if (which == 1) w2k_effects[FX_DRAG_CONTENTS] = 1;
}

static const char *const color_names[N_COLORS] = {
    [C_FACE] = "ButtonFace", [C_HILIGHT] = "ButtonHilight", [C_LIGHT] = "ButtonLight",
    [C_SHADOW] = "ButtonShadow", [C_DKSHADOW] = "ButtonDkShadow",
    [C_TEXT] = "ButtonText", [C_GRAYTEXT] = "GrayText", [C_WINDOW] = "Window",
    [C_WINDOWTEXT] = "WindowText", [C_WINDOWFRAME] = "WindowFrame",
    [C_ACTIVETITLE] = "ActiveTitle", [C_ACTIVETITLE2] = "GradientActiveTitle",
    [C_INACTIVETITLE] = "InactiveTitle", [C_INACTIVETITLE2] = "GradientInactiveTitle",
    [C_TITLETEXT] = "TitleText", [C_INACTIVETITLETEXT] = "InactiveTitleText",
    [C_MENU] = "Menu", [C_MENUTEXT] = "MenuText", [C_HIGHLIGHT] = "Hilight",
    [C_HIGHLIGHTTEXT] = "HilightText", [C_DESKTOP] = "Background",
    [C_SCROLLBAR] = "Scrollbar", [C_TOOLTIP] = "InfoWindow",
    [C_TOOLTIPTEXT] = "InfoText", [C_APPWORKSPACE] = "AppWorkspace",
    [C_BLACK] = "Black", [C_WHITE] = "White",
};

const char *w2k_color_name(int color)
{
    return (color >= 0 && color < N_COLORS) ? color_names[color] : NULL;
}

int w2k_color_by_name(const char *name)
{
    for (int i = 0; i < N_COLORS; i++)
        if (color_names[i] && !strcasecmp(color_names[i], name)) return i;
    return -1;
}

static unsigned long alloc_pixel(int r, int g, int b)
{
    XColor c = { .red = r * 257, .green = g * 257, .blue = b * 257,
                 .flags = DoRed | DoGreen | DoBlue };
    if (!XAllocColor(w2k.dpy, w2k.cmap, &c))
        return (r + g + b > 382) ? WhitePixel(w2k.dpy, w2k.screen)
                                 : BlackPixel(w2k.dpy, w2k.screen);
    return c.pixel;
}

void w2k_color_set(int color, int r, int g, int b)
{
    w2k_font_colours_dirty();
    if (color < 0 || color >= N_COLORS) return;
    scheme[color][0] = r; scheme[color][1] = g; scheme[color][2] = b;
    if (w2k.dpy) w2k.col[color] = alloc_pixel(r, g, b);
}

/* Which of the built-in schemes the shell is wearing. */
int w2k_theme = THEME_CLASSIC;

const char *w2k_theme_name(int theme)
{
    switch (theme) {
    case THEME_XP:     return "Windows XP";
    case THEME_BASIC7: return "Windows 7 Basic";
    }
    return "Windows Standard";
}

void w2k_theme_colour(int theme, int color, unsigned char rgb[3])
{
    const unsigned char (*t)[3] = theme == THEME_XP    ? luna :
                                  theme == THEME_BASIC7 ? basic7 : standard;
    if (color < 0 || color >= N_COLORS) { rgb[0] = rgb[1] = rgb[2] = 0; return; }
    rgb[0] = t[color][0];
    rgb[1] = t[color][1];
    rgb[2] = t[color][2];
}

void w2k_theme_colours(int theme)
{
    const unsigned char (*t)[3] = theme == THEME_XP    ? luna :
                                  theme == THEME_BASIC7 ? basic7 : standard;
    for (int i = 0; i < N_COLORS; i++)
        w2k_color_set(i, t[i][0], t[i][1], t[i][2]);
}

void w2k_scheme_reset(void)
{
    w2k_theme_colours(w2k_theme);
    snprintf(w2k_gtk_theme, sizeof w2k_gtk_theme, "Chicago95");
    snprintf(w2k_icon_theme, sizeof w2k_icon_theme, "Chicago95");
    snprintf(w2k_qt_style, sizeof w2k_qt_style, "Windows");
    w2k_wallpaper[0] = 0;
    w2k_wallpaper_style = 0;
    w2k_force_decorations = 1;
    w2k_start_banner_mode = SB_WINDOWS;
    w2k_start_banner_custom[0] = 0;
    w2k_start_banner_top[0] = 0;
    w2k_start_banner_top[1] = 0;
    w2k_start_banner_top[2] = 0;
    w2k_start_banner_bottom[0] = 0;
    w2k_start_banner_bottom[1] = 0;
    w2k_start_banner_bottom[2] = 255;
    w2k_start_banner_dither = 0;
    w2k_start_icon = SI_FLAG;
    w2k_start_search = 1;
    w2k_start_panel = 0;
    w2k_start_small_icons = 0;
    w2k_start_personalized = 0;
    w2k_taskbar_ontop = 1;
    w2k_taskbar_autohide = 0;
    w2k_taskbar_showclock = 1;
    w2k_taskbar_edge = TB_BOTTOM;
    w2k_taskbar_rows = 1;
    w2k_taskbar_quicklaunch = 1;
    w2k_taskbar_labels = 0;
    w2k_taskbar_small = 0;
    w2k_folder_hidden = 0;
    w2k_folder_hide_ext = 1;
    w2k_folder_fullpath = 0;
    w2k_dblclk_ms = 500;
    w2k_mouse_swap = 0;
    w2k_mouse_speed = 4;
    w2k_key_delay = 500;
    w2k_key_rate = 30;
    w2k_caret_blink = 530;
    w2k_bell_on = 1;
    w2k_bell_volume = 50;
    w2k_bell_pitch = 400;
    w2k_bell_duration = 100;
    w2k_folder_singleclick = 0;
    w2k_folder_newwindow = 0;
    w2k_folder_tooltips = 1;
    w2k_view_toolbar = w2k_view_address = w2k_view_status = 1;
    w2k_effects_preset(0);
    w2k_monitor_cfg_n = 0;
}

/* PRETTY_NAME out of /etc/os-release: "Debian GNU/Linux 13 (trixie)". */
const char *w2k_distro_name(void)
{
    static char name[128];
    if (name[0]) return name;
    snprintf(name, sizeof name, "Linux");

    FILE *f = fopen("/etc/os-release", "r");
    if (!f) return name;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "PRETTY_NAME=", 12)) continue;
        char *v = line + 12;
        v[strcspn(v, "\r\n")] = 0;
        if (*v == '"' || *v == '\'') {
            char q = *v++;
            char *end = strrchr(v, q);
            if (end) *end = 0;
        }
        if (*v) snprintf(name, sizeof name, "%.127s", v);
        break;
    }
    fclose(f);
    return name;
}

/* The Start button's icon follows the setting: the Windows flag is the
 * built-in artwork (or ~/.w2k/icons/startflag.ico if one was dropped
 * there), the others are files beside it. */
void w2k_start_icon_apply(void)
{
    const char *home = getenv("HOME");
    char path[1024];
    const char *file = w2k_start_icon == SI_TUX    ? "startflag-tux.ico" :
                       w2k_start_icon == SI_DISTRO ? "startflag-distro.ico" :
                                                     "startflag.ico";
    if (home) {
        snprintf(path, sizeof path, "%s/.w2k/icons/%s", home, file);
        if (w2k_icon_load_file(ICO_STARTFLAG, path)) return;
    }
    snprintf(path, sizeof path, "/usr/local/share/w2k/icons/%s", file);
    if (w2k_icon_load_file(ICO_STARTFLAG, path)) return;
    w2k_icon_load_file(ICO_STARTFLAG, NULL);        /* built-in flag */
}

const char *w2k_start_banner_text(void)
{
    switch (w2k_start_banner_mode) {
    case SB_DISTRO: return w2k_distro_name();
    case SB_CUSTOM: return w2k_start_banner_custom[0] ? w2k_start_banner_custom
                                                      : "Windows 2000";
    }
    return "Windows 2000 Professional";
}

void w2k_scheme_default_path(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.w2k/scheme", home ? home : ".");
}

int w2k_scheme_load(const char *path)
{
    char def[1024];
    if (!path) { w2k_scheme_default_path(def, sizeof def); path = def; }
    w2k_scheme_reset();
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1200];
    int n = 0;
    /* The theme first, whatever line it is on: it brings a whole colour
     * table, and the colour lines (which the file lists before it) must
     * override that table, not be wiped by it. */
    while (fgets(line, sizeof line, f)) {
        if (strncasecmp(line, "Theme=", 6)) continue;
        const char *val = line + 6;
        w2k_theme = !strncasecmp(val, "xp", 2)     ? THEME_XP :
                    !strncasecmp(val, "basic7", 6) ? THEME_BASIC7
                                                   : THEME_CLASSIC;
        w2k_theme_colours(w2k_theme);
        break;
    }
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;
        if (!strcasecmp(line, "Wallpaper")) { snprintf(w2k_wallpaper, sizeof w2k_wallpaper, "%s", val); continue; }
        if (!strcasecmp(line, "ForceDecorations")) {
            w2k_force_decorations = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "StartBannerMode")) {
            w2k_start_banner_mode = !strcasecmp(val, "distro") ? SB_DISTRO :
                                    !strcasecmp(val, "custom") ? SB_CUSTOM :
                                    SB_WINDOWS;
            continue;
        }
        if (!strcasecmp(line, "StartBannerText")) {
            snprintf(w2k_start_banner_custom, sizeof w2k_start_banner_custom,
                     "%s", val);
            continue;
        }
        if (!strcasecmp(line, "StartBannerTop")) {
            sscanf(val, "%d %d %d", &w2k_start_banner_top[0],
                   &w2k_start_banner_top[1], &w2k_start_banner_top[2]);
            continue;
        }
        if (!strcasecmp(line, "StartBannerBottom")) {
            sscanf(val, "%d %d %d", &w2k_start_banner_bottom[0],
                   &w2k_start_banner_bottom[1], &w2k_start_banner_bottom[2]);
            continue;
        }
        if (!strcasecmp(line, "StartBannerDither")) {
            w2k_start_banner_dither = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "Monitor")) {
            /* Monitor=<output> <mode|auto> <x> <y> <primary> <enabled>
             *         [<rate|auto> <scale%>] -- the last two since 1.7 */
            if (w2k_monitor_cfg_n < 8) {
                W2kMonitorCfg *m = &w2k_monitor_cfg[w2k_monitor_cfg_n];
                memset(m, 0, sizeof *m);
                int got = sscanf(val, "%63s %15s %d %d %d %d %15s %d", m->name,
                                 m->mode, &m->x, &m->y, &m->primary, &m->enabled,
                                 m->rate, &m->scale);
                if (got >= 6) {
                    if (!strcmp(m->mode, "auto")) m->mode[0] = 0;
                    if (got < 7 || !strcmp(m->rate, "auto")) m->rate[0] = 0;
                    if (got < 8 || m->scale < 50 || m->scale > 400) m->scale = 0;
                    w2k_monitor_cfg_n++;
                }
            }
            continue;
        }
        if (!strcasecmp(line, "Effects")) {
            for (int i = 0; i < N_EFFECTS && val[i]; i++)
                w2k_effects[i] = (val[i] == '1') &&
                                 w2k_effect_supported(i);
            continue;
        }
        {
            static const struct { const char *key; int *val; int lo, hi; }
            ints[] = {
                { "DoubleClickTime", &w2k_dblclk_ms,     100, 2000 },
                { "MouseSwap",       &w2k_mouse_swap,      0,    1 },
                { "MouseSpeed",      &w2k_mouse_speed,     1,   10 },
                { "KeyRepeatDelay",  &w2k_key_delay,     100, 2000 },
                { "KeyRepeatRate",   &w2k_key_rate,        2,   50 },
                { "CaretBlink",      &w2k_caret_blink,   100, 2000 },
                { "BellOn",          &w2k_bell_on,         0,    1 },
                { "BellVolume",      &w2k_bell_volume,     0,  100 },
                { "BellPitch",       &w2k_bell_pitch,     20, 4000 },
                { "BellDuration",    &w2k_bell_duration,  10, 1000 },
                { "ViewToolbar",     &w2k_view_toolbar,    0,    1 },
                { "ViewAddress",     &w2k_view_address,    0,    1 },
                { "ViewStatusBar",   &w2k_view_status,     0,    1 },
            };
            int done = 0;
            for (int i = 0; i < (int)(sizeof ints / sizeof *ints); i++)
                if (!strcasecmp(line, ints[i].key)) {
                    int v = atoi(val);
                    if (v < ints[i].lo) v = ints[i].lo;
                    if (v > ints[i].hi) v = ints[i].hi;
                    *ints[i].val = v;
                    done = 1;
                    break;
                }
            if (done) continue;
        }
        if (!strcasecmp(line, "Theme")) continue;     /* taken above */
        if (!strcasecmp(line, "FolderHidden")) {
            w2k_folder_hidden = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "FolderHideExt")) {
            w2k_folder_hide_ext = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "FolderFullPath")) {
            w2k_folder_fullpath = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "FolderSingleClick")) {
            w2k_folder_singleclick = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "FolderNewWindow")) {
            w2k_folder_newwindow = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "FolderTooltips")) {
            w2k_folder_tooltips = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "TaskbarOnTop")) {
            w2k_taskbar_ontop = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "TaskbarAutoHide")) {
            w2k_taskbar_autohide = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "TaskbarEdge")) {
            w2k_taskbar_edge = !strcasecmp(val, "top") ? TB_TOP :
                               !strcasecmp(val, "left") ? TB_LEFT :
                               !strcasecmp(val, "right") ? TB_RIGHT : TB_BOTTOM;
            continue;
        }
        if (!strcasecmp(line, "TaskbarLabels")) { w2k_taskbar_labels = atoi(val) != 0; continue; }
        if (!strcasecmp(line, "TaskbarSmallIcons")) { w2k_taskbar_small = atoi(val) != 0; continue; }
        if (!strcasecmp(line, "TaskbarQuickLaunch")) {
            w2k_taskbar_quicklaunch = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "TaskbarRows")) {
            w2k_taskbar_rows = atoi(val);
            if (w2k_taskbar_rows < 1) w2k_taskbar_rows = 1;
            if (w2k_taskbar_rows > 4) w2k_taskbar_rows = 4;
            continue;
        }
        if (!strcasecmp(line, "TaskbarShowClock")) {
            w2k_taskbar_showclock = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "StartSearch")) {
            w2k_start_search = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "StartPanel")) {
            w2k_start_panel = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "StartSmallIcons")) {
            w2k_start_small_icons = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "StartPersonalized")) {
            w2k_start_personalized = atoi(val) != 0;
            continue;
        }
        if (!strcasecmp(line, "Cursors")) {
            w2k_cursors_windows = strcasecmp(val, "x11") != 0;
            continue;
        }
        if (!strcasecmp(line, "IconSet")) {
            snprintf(w2k_icon_set, sizeof w2k_icon_set, "%.31s", val);
            continue;
        }
        if (!strcasecmp(line, "GtkTheme"))  { snprintf(w2k_gtk_theme, sizeof w2k_gtk_theme, "%.63s", val); continue; }
        if (!strcasecmp(line, "IconTheme")) { snprintf(w2k_icon_theme, sizeof w2k_icon_theme, "%.63s", val); continue; }
        if (!strcasecmp(line, "QtStyle"))   { snprintf(w2k_qt_style, sizeof w2k_qt_style, "%.63s", val); continue; }
        if (!strcasecmp(line, "StartIcon")) {
            w2k_start_icon = !strcasecmp(val, "tux") ? SI_TUX :
                             !strcasecmp(val, "distro") ? SI_DISTRO : SI_FLAG;
            continue;
        }
        if (!strcasecmp(line, "WallpaperStyle")) {
            w2k_wallpaper_style = !strcasecmp(val, "tile") ? 1 :
                                  !strcasecmp(val, "stretch") ? 2 :
                                  !strcasecmp(val, "fit") ? 3 :
                                  !strcasecmp(val, "fill") ? 4 :
                                  !strcasecmp(val, "span") ? 5 : 0;
            continue;
        }
        int idx = w2k_color_by_name(line);
        int r, g, b;
        if (idx >= 0 && sscanf(val, "%d %d %d", &r, &g, &b) == 3) {
            w2k_color_set(idx, r & 255, g & 255, b & 255);
            n++;
        }
    }
    fclose(f);
    /* The icon set is part of the scheme: when a reload brings a different
     * one, the artwork follows (every process re-reads the files; the
     * caches drop as each slot is replaced). */
    /* Artwork is re-read on every reload, not only when the set's name
     * changed: the files behind the same name may be new (a rebuilt set,
     * a skin dropped into ~/.w2k/skins), and a broadcast is how a running
     * desktop is told. A few dozen small files: cheap. */
    w2k_skin_cache_flush();
    w2k_icon_load_default();
    return n;
}

int w2k_scheme_save(const char *path)
{
    char def[1024];
    if (!path) { w2k_scheme_default_path(def, sizeof def); path = def; }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# Linux 2000 -- appearance scheme (registry colour names, R G B)\n");
    for (int i = 0; i < N_COLORS; i++)
        if (color_names[i] && i != C_BLACK && i != C_WHITE)
            fprintf(f, "%s=%d %d %d\n", color_names[i], scheme[i][0], scheme[i][1], scheme[i][2]);
    fprintf(f, "Wallpaper=%s\n", w2k_wallpaper);
    fprintf(f, "WallpaperStyle=%s\n", w2k_wallpaper_style == 1 ? "tile" :
            w2k_wallpaper_style == 2 ? "stretch" : w2k_wallpaper_style == 3 ? "fit" :
            w2k_wallpaper_style == 4 ? "fill" : w2k_wallpaper_style == 5 ? "span" :
            "center");
    fprintf(f, "ForceDecorations=%d\n", w2k_force_decorations);
    fprintf(f, "StartBannerMode=%s\n",
            w2k_start_banner_mode == SB_DISTRO ? "distro" :
            w2k_start_banner_mode == SB_CUSTOM ? "custom" : "windows");
    fprintf(f, "StartBannerText=%s\n", w2k_start_banner_custom);
    fprintf(f, "StartBannerTop=%d %d %d\n", w2k_start_banner_top[0],
            w2k_start_banner_top[1], w2k_start_banner_top[2]);
    fprintf(f, "StartBannerBottom=%d %d %d\n", w2k_start_banner_bottom[0],
            w2k_start_banner_bottom[1], w2k_start_banner_bottom[2]);
    fprintf(f, "StartBannerDither=%d\n", w2k_start_banner_dither);
    fprintf(f, "StartSearch=%d\n", w2k_start_search);
    fprintf(f, "StartPanel=%d\n", w2k_start_panel);
    fprintf(f, "StartSmallIcons=%d\n", w2k_start_small_icons);
    fprintf(f, "StartPersonalized=%d\n", w2k_start_personalized);
    char fx[N_EFFECTS + 1];
    for (int i = 0; i < N_EFFECTS; i++) fx[i] = w2k_effects[i] ? '1' : '0';
    fx[N_EFFECTS] = 0;
    fprintf(f, "Effects=%s\n", fx);
    fprintf(f, "Theme=%s\n", w2k_theme == THEME_XP ? "xp" :
            w2k_theme == THEME_BASIC7 ? "basic7" : "classic");
    fprintf(f, "IconSet=%s\n", w2k_icon_set);
    fprintf(f, "GtkTheme=%s\n", w2k_gtk_theme);
    fprintf(f, "IconTheme=%s\n", w2k_icon_theme);
    fprintf(f, "QtStyle=%s\n", w2k_qt_style);
    fprintf(f, "Cursors=%s\n", w2k_cursors_windows ? "windows" : "x11");
    fprintf(f, "DoubleClickTime=%d\n", w2k_dblclk_ms);
    fprintf(f, "MouseSwap=%d\n", w2k_mouse_swap);
    fprintf(f, "MouseSpeed=%d\n", w2k_mouse_speed);
    fprintf(f, "KeyRepeatDelay=%d\n", w2k_key_delay);
    fprintf(f, "KeyRepeatRate=%d\n", w2k_key_rate);
    fprintf(f, "CaretBlink=%d\n", w2k_caret_blink);
    fprintf(f, "BellOn=%d\n", w2k_bell_on);
    fprintf(f, "BellVolume=%d\n", w2k_bell_volume);
    fprintf(f, "BellPitch=%d\n", w2k_bell_pitch);
    fprintf(f, "BellDuration=%d\n", w2k_bell_duration);
    fprintf(f, "FolderHidden=%d\n", w2k_folder_hidden);
    fprintf(f, "FolderHideExt=%d\n", w2k_folder_hide_ext);
    fprintf(f, "FolderFullPath=%d\n", w2k_folder_fullpath);
    fprintf(f, "FolderSingleClick=%d\n", w2k_folder_singleclick);
    fprintf(f, "FolderNewWindow=%d\n", w2k_folder_newwindow);
    fprintf(f, "FolderTooltips=%d\n", w2k_folder_tooltips);
    fprintf(f, "ViewToolbar=%d\n", w2k_view_toolbar);
    fprintf(f, "ViewAddress=%d\n", w2k_view_address);
    fprintf(f, "ViewStatusBar=%d\n", w2k_view_status);
    fprintf(f, "TaskbarOnTop=%d\n", w2k_taskbar_ontop);
    fprintf(f, "TaskbarAutoHide=%d\n", w2k_taskbar_autohide);
    fprintf(f, "TaskbarShowClock=%d\n", w2k_taskbar_showclock);
    fprintf(f, "TaskbarEdge=%s\n",
            w2k_taskbar_edge == TB_TOP ? "top" :
            w2k_taskbar_edge == TB_LEFT ? "left" :
            w2k_taskbar_edge == TB_RIGHT ? "right" : "bottom");
    fprintf(f, "TaskbarRows=%d\n", w2k_taskbar_rows);
    fprintf(f, "TaskbarQuickLaunch=%d\n", w2k_taskbar_quicklaunch);
    fprintf(f, "TaskbarLabels=%d\n", w2k_taskbar_labels);
    fprintf(f, "TaskbarSmallIcons=%d\n", w2k_taskbar_small);
    fprintf(f, "StartIcon=%s\n",
            w2k_start_icon == SI_TUX ? "tux" :
            w2k_start_icon == SI_DISTRO ? "distro" : "flag");
    for (int i = 0; i < w2k_monitor_cfg_n; i++) {
        const W2kMonitorCfg *m = &w2k_monitor_cfg[i];
        fprintf(f, "Monitor=%s %s %d %d %d %d %s %d\n", m->name,
                m->mode[0] ? m->mode : "auto", m->x, m->y, m->primary,
                m->enabled, m->rate[0] ? m->rate : "auto",
                m->scale ? m->scale : 100);
    }
    fclose(f);
    /* The user's own scheme also reaches GTK and Qt programs. */
    if (path == def) w2k_scheme_export_gtk();
    return 0;
}

const unsigned char *w2k_scheme_rgb(int color)
{
    static const unsigned char black[3];
    return (color >= 0 && color < N_COLORS) ? scheme[color] : black;
}

void w2k_scheme_broadcast(void)
{
    static long serial;
    serial++;
    XChangeProperty(w2k.dpy, w2k.root, w2k.a_w2k_scheme, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&serial, 1);
    XFlush(w2k.dpy);
}

static XFontStruct *load_font(int which)
{
    for (int i = 0; font_chain[which][i]; i++) {
        XFontStruct *f = XLoadQueryFont(w2k.dpy, font_chain[which][i]);
        if (f) return f;
    }
    return NULL;
}

void *w2k_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) { fputs("w2k: out of memory\n", stderr); exit(1); }
    return p;
}

int w2k_cursors_windows = 1;

void w2k_shell_quote(const char *in, char *out, int n)
{
    int o = 0;
    if (n < 3) { if (n > 0) out[0] = 0; return; }
    out[o++] = '\'';
    for (const char *p = in; *p && o < n - 6; p++) {
        if (*p == '\'') {
            out[o++] = '\''; out[o++] = '\\'; out[o++] = '\''; out[o++] = '\'';
        } else out[o++] = *p;
    }
    out[o++] = '\'';
    out[o] = 0;
}

void w2k_splice(const char *tmpl, const char *arg, char *out, int n)
{
    const char *at = strstr(tmpl, "%s");
    if (!at) { snprintf(out, (size_t)n, "%s", tmpl); return; }
    snprintf(out, (size_t)n, "%.*s%s%s", (int)(at - tmpl), tmpl, arg, at + 2);
}

char *w2k_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = w2k_alloc(n);
    memcpy(p, s, n);
    return p;
}

long w2k_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void w2k_set_wm_name(Window w, const char *name)
{
    XStoreName(w2k.dpy, w, name);
    XChangeProperty(w2k.dpy, w, w2k.a_net_wm_name, w2k.a_utf8, 8,
                    PropModeReplace, (const unsigned char *)name, strlen(name));
}

/* Windows can vanish between a query and the request that uses the answer.
 * Xlib's default handler exits the process for that; ours does not. */
static int default_xerror(Display *d, XErrorEvent *e)
{
    if (e->error_code == BadWindow || e->error_code == BadDrawable ||
        e->error_code == BadMatch  || e->error_code == BadPixmap)
        return 0;
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "w2k: X error: %s (request %d.%d)\n", buf,
            e->request_code, e->minor_code);
    return 0;
}

int w2k_init(const char *appname)
{
    /* Text is UTF-8 throughout; the keyboard side needs a locale that
     * says so before the display is opened. */
    if (!setlocale(LC_CTYPE, "") || !strstr(setlocale(LC_CTYPE, NULL), "UTF-8"))
        if (!setlocale(LC_CTYPE, "C.UTF-8")) setlocale(LC_CTYPE, "en_US.UTF-8");
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "%s: cannot open display \"%s\"\n", appname,
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return -1;
    }
    XSetErrorHandler(default_xerror);
    w2k.dpy    = d;
    w2k.screen = DefaultScreen(d);
    w2k.root   = RootWindow(d, w2k.screen);
    w2k.visual = DefaultVisual(d, w2k.screen);
    w2k.cmap   = DefaultColormap(d, w2k.screen);
    w2k.depth  = DefaultDepth(d, w2k.screen);
    w2k.sw     = DisplayWidth(d, w2k.screen);
    w2k.sh     = DisplayHeight(d, w2k.screen);

    w2k_scheme_load(NULL);

    /* Xft first -- the right typeface, UTF-8 and a smoothing switch. The
     * core bitmap fonts stay as the fallback for a system without any
     * scalable fonts at all, but they are only loaded if that happens:
     * four XLoadQueryFont round trips, each with a per-character metrics
     * array, is a lot to pay for a path that will not be taken. */
    if (!w2k_font_init()) {
        for (int i = 0; i < N_FONTS; i++) {
            w2k.font[i] = load_font(i);
            if (!w2k.font[i]) {
                fprintf(stderr, "%s: no usable font for slot %d "
                        "(install xfonts-75dpi / xfonts-base)\n", appname, i);
                return -1;
            }
        }
    }

    XGCValues gv;
    gv.graphics_exposures = False;
    w2k.gc = XCreateGC(d, w2k.root, GCGraphicsExposures, &gv);
    if (w2k.font[F_UI]) XSetFont(d, w2k.gc, w2k.font[F_UI]->fid);

    /* 2x2 checkerboard, used for scrollbar troughs and focus rectangles. */
    static const char dither_bits[] = { 0x01, 0x02 };
    w2k.pm_dither = XCreateBitmapFromData(d, w2k.root, dither_bits, 2, 2);

    gv.fill_style = FillStippled;
    gv.stipple    = w2k.pm_dither;
    gv.graphics_exposures = False;
    w2k.gc_dither = XCreateGC(d, w2k.root,
                              GCFillStyle | GCStipple | GCGraphicsExposures, &gv);

    /* DrawFocusRect() is an XOR of a 50% pattern -- do exactly that. */
    gv.function = GXinvert;
    w2k.gc_focus = XCreateGC(d, w2k.root,
                             GCFillStyle | GCStipple | GCFunction |
                             GCGraphicsExposures, &gv);

    XGCValues iv = { .graphics_exposures = False };
    w2k.gc_icon = XCreateGC(d, w2k.root, GCGraphicsExposures, &iv);

    w2k_cursors_init();
    w2k_monitors_init();
    w2k_accel_reset();              /* underlines hidden until Alt? */

#define A(f, n) w2k.f = XInternAtom(d, n, False)
    A(a_wm_protocols,   "WM_PROTOCOLS");
    A(a_wm_delete,      "WM_DELETE_WINDOW");
    A(a_wm_state,       "WM_STATE");
    A(a_wm_take_focus,  "WM_TAKE_FOCUS");
    A(a_wm_change_state,"WM_CHANGE_STATE");
    A(a_motif_hints,    "_MOTIF_WM_HINTS");
    A(a_net_supported,  "_NET_SUPPORTED");
    A(a_net_wm_name,    "_NET_WM_NAME");
    A(a_net_wm_state,   "_NET_WM_STATE");
    A(a_net_wm_state_fullscreen,   "_NET_WM_STATE_FULLSCREEN");
    A(a_net_wm_state_maxv,         "_NET_WM_STATE_MAXIMIZED_VERT");
    A(a_net_wm_state_maxh,         "_NET_WM_STATE_MAXIMIZED_HORZ");
    A(a_net_wm_state_hidden,       "_NET_WM_STATE_HIDDEN");
    A(a_net_wm_state_skip_taskbar, "_NET_WM_STATE_SKIP_TASKBAR");
    A(a_net_wm_state_above,        "_NET_WM_STATE_ABOVE");
    A(a_net_wm_state_modal,        "_NET_WM_STATE_MODAL");
    A(a_net_wm_window_type,        "_NET_WM_WINDOW_TYPE");
    A(a_net_wm_wt_dock,    "_NET_WM_WINDOW_TYPE_DOCK");
    A(a_net_wm_wt_dialog,  "_NET_WM_WINDOW_TYPE_DIALOG");
    A(a_net_wm_wt_normal,  "_NET_WM_WINDOW_TYPE_NORMAL");
    A(a_net_wm_wt_menu,    "_NET_WM_WINDOW_TYPE_MENU");
    A(a_net_wm_wt_utility, "_NET_WM_WINDOW_TYPE_UTILITY");
    A(a_net_wm_wt_splash,  "_NET_WM_WINDOW_TYPE_SPLASH");
    A(a_net_wm_wt_toolbar, "_NET_WM_WINDOW_TYPE_TOOLBAR");
    A(a_net_client_list,   "_NET_CLIENT_LIST");
    A(a_net_active_window, "_NET_ACTIVE_WINDOW");
    A(a_net_current_desktop,   "_NET_CURRENT_DESKTOP");
    A(a_net_number_of_desktops,"_NET_NUMBER_OF_DESKTOPS");
    A(a_net_wm_desktop,        "_NET_WM_DESKTOP");
    A(a_net_supporting_wm_check,"_NET_SUPPORTING_WM_CHECK");
    A(a_net_close_window,  "_NET_CLOSE_WINDOW");
    A(a_net_moveresize_window, "_NET_MOVERESIZE_WINDOW");
    A(a_net_frame_extents, "_NET_FRAME_EXTENTS");
    A(a_net_workarea,      "_NET_WORKAREA");
    A(a_net_wm_moveresize, "_NET_WM_MOVERESIZE");
    A(a_net_wm_icon,       "_NET_WM_ICON");
    A(a_net_wm_pid,        "_NET_WM_PID");
    A(a_utf8,              "UTF8_STRING");
    A(a_w2k_command,       "_W2K_COMMAND");
    A(a_w2k_notify,        "_W2K_NOTIFY");
    A(a_w2k_scheme,        "_W2K_SCHEME");
#undef A
    /* Hear about scheme changes made by Display Properties. The window
     * manager adds its own masks to the root later; this one is harmless. */
    XSelectInput(d, w2k.root, PropertyChangeMask);
    w2k_icon_load_default();
    return 0;
}

void w2k_fini(void)
{
    if (!w2k.dpy) return;
    w2k_font_fini();
    for (int i = 0; i < N_FONTS; i++)
        if (w2k.font[i]) XFreeFont(w2k.dpy, w2k.font[i]);
    XFreeGC(w2k.dpy, w2k.gc);
    XFreeGC(w2k.dpy, w2k.gc_dither);
    XFreeGC(w2k.dpy, w2k.gc_focus);
    XFreeGC(w2k.dpy, w2k.gc_icon);
    w2k_free_pixmap(w2k.pm_dither);
    XCloseDisplay(w2k.dpy);
    w2k.dpy = NULL;
}

void w2k_color_rgb(int color, int *r, int *g, int *b)
{
    if (color < 0 || color >= N_COLORS) { *r = *g = *b = 0; return; }
    *r = scheme[color][0];
    *g = scheme[color][1];
    *b = scheme[color][2];
}

/* Map an RGB triple to a pixel value. TrueColor gets an exact bit-twiddle so
 * gradients cost no colour-map traffic; anything else falls back to a small
 * allocation cache. */
unsigned long w2k_rgb(int r, int g, int b)
{
    Visual *v = w2k.visual;
    if (v->class == TrueColor || v->class == DirectColor) {
        unsigned long out = 0;
        const unsigned long masks[3] = { v->red_mask, v->green_mask, v->blue_mask };
        const int vals[3] = { r, g, b };
        for (int i = 0; i < 3; i++) {
            unsigned long m = masks[i];
            if (!m) continue;
            int shift = 0, bits = 0;
            while (!((m >> shift) & 1)) shift++;
            for (unsigned long t = m >> shift; t & 1; t >>= 1) bits++;
            unsigned long maxv = (1UL << bits) - 1;
            out |= ((unsigned long)vals[i] * maxv / 255UL) << shift;
        }
        return out;
    }
    /* Paletted visual: cache allocations so repeated gradient draws are cheap. */
    static struct { int r, g, b; unsigned long px; } cache[64];
    static int ncache;
    for (int i = 0; i < ncache; i++)
        if (cache[i].r == r && cache[i].g == g && cache[i].b == b)
            return cache[i].px;
    XColor c = { .red = r * 257, .green = g * 257, .blue = b * 257,
                 .flags = DoRed | DoGreen | DoBlue };
    if (!XAllocColor(w2k.dpy, w2k.cmap, &c))
        return (r + g + b > 382) ? WhitePixel(w2k.dpy, w2k.screen)
                                 : BlackPixel(w2k.dpy, w2k.screen);
    if (ncache < (int)(sizeof cache / sizeof *cache)) {
        cache[ncache].r = r; cache[ncache].g = g;
        cache[ncache].b = b; cache[ncache].px = c.pixel;
        ncache++;
    }
    return c.pixel;
}
