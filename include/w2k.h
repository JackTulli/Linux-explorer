/* w2k.h -- Windows 2000 look-and-feel micro toolkit for X11.
 *
 * Deliberately tiny: Xlib only, core bitmap fonts, no Xft/cairo/pango.
 * Everything here exists to reproduce the exact pixel conventions of the
 * classic Windows "3D" look as shipped in Windows 2000.
 */
#ifndef W2K_H
#define W2K_H

#include <stddef.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>

/* ------------------------------------------------------------------ *
 * System colours (the "Windows Standard" scheme, 2000 defaults)
 * ------------------------------------------------------------------ */
enum {
    C_FACE,             /* 212,208,200 -- button face / dialog background   */
    C_HILIGHT,          /* 255,255,255 -- 3D highlight                      */
    C_LIGHT,            /* 212,208,200 -- ButtonLight, == face in this scheme */
    C_SHADOW,           /* 128,128,128 -- 3D shadow                         */
    C_DKSHADOW,         /*  64, 64, 64 -- 3D dark shadow                    */
    C_TEXT,             /* button text                                      */
    C_GRAYTEXT,         /* disabled text                                    */
    C_WINDOW,           /* 255,255,255 -- client area background            */
    C_WINDOWTEXT,
    C_WINDOWFRAME,      /* 0,0,0 -- hard 1px frames                         */
    C_ACTIVETITLE,      /*  10, 36,106                                      */
    C_ACTIVETITLE2,     /* 166,202,240 -- gradient right end                */
    C_INACTIVETITLE,    /* 128,128,128                                      */
    C_INACTIVETITLE2,   /* 192,192,192                                      */
    C_TITLETEXT,
    C_INACTIVETITLETEXT,
    C_MENU,
    C_MENUTEXT,
    C_HIGHLIGHT,        /*  10, 36,106 -- selection                         */
    C_HIGHLIGHTTEXT,
    C_DESKTOP,          /*  58,110,165 -- the classic blue                  */
    C_SCROLLBAR,        /* 212,208,200 -- trough is a 50% dither            */
    C_TOOLTIP,          /* 255,255,225                                      */
    C_TOOLTIPTEXT,
    C_APPWORKSPACE,     /* 128,128,128 -- MDI backdrop                      */
    C_BLACK,
    C_WHITE,
    N_COLORS
};

/* Non-client metrics, the Windows 2000 defaults at 96 dpi. The window
 * manager has the same numbers (see wm/wm.h, which takes them from here):
 * a program needs them to centre itself by its frame rather than by its
 * client area, which is what "centred" looks like on screen. */
#define W2K_CAPTION_H    18    /* SM_CYCAPTION                    */
#define W2K_FRAME_SIZE    4    /* SM_CXSIZEFRAME, resizable       */
#define W2K_FRAME_FIXED   3    /* SM_CXFIXEDFRAME, a dialog       */

/* Fonts */
enum { F_UI, F_UI_BOLD, F_FIXED, F_ICON, N_FONTS };

/* Edge styles for w2k_edge(), matching DrawEdge() semantics. */
enum {
    EDGE_RAISED,        /* window frame, panel               (2px) */
    EDGE_BUTTON,        /* button, caption button, tab body  (2px) */
    EDGE_SUNKEN,        /* text field well, client edge      (2px) */
    EDGE_ETCHED,        /* group box, separator              (2px) */
    EDGE_BUMP,          /* inverse of etched                 (2px) */
    EDGE_RAISED_THIN,   /* hot toolbar button                (1px) */
    EDGE_SUNKEN_THIN,   /* pressed toolbar button, statusbar (1px) */
    EDGE_FLAT           /* nothing drawn                           */
};

/* Which sides to draw an edge on (default BF_RECT = all four). */
#define BF_LEFT   0x01
#define BF_TOP    0x02
#define BF_RIGHT  0x04
#define BF_BOTTOM 0x08
#define BF_RECT   (BF_LEFT|BF_TOP|BF_RIGHT|BF_BOTTOM)

/* ------------------------------------------------------------------ *
 * Global toolkit state
 * ------------------------------------------------------------------ */
typedef struct {
    Display     *dpy;
    int          screen;
    Window       root;
    Visual      *visual;
    Colormap     cmap;
    int          depth;
    int          sw, sh;            /* screen dimensions */

    unsigned long col[N_COLORS];
    XFontStruct *font[N_FONTS];
    GC           gc;                /* general purpose, changes freely */
    GC           gc_dither;         /* 50% checkerboard stipple, for troughs */
    GC           gc_focus;          /* 1px dotted XOR-ish focus rectangle */
    GC           gc_icon;           /* masked icon blits                  */
    Pixmap       pm_dither;

    Cursor       cur_arrow, cur_wait, cur_text, cur_hand, cur_move;
    Cursor       cur_no;            /* the "unavailable" circle-and-slash */
    Cursor       cur_size_ns, cur_size_we, cur_size_nwse, cur_size_nesw;

    /* Interned atoms (ICCCM + a useful subset of EWMH). */
    Atom a_wm_protocols, a_wm_delete, a_wm_state, a_wm_take_focus,
         a_wm_change_state, a_motif_hints,
         a_net_supported, a_net_wm_name, a_net_wm_state,
         a_net_wm_state_fullscreen, a_net_wm_state_maxv, a_net_wm_state_maxh,
         a_net_wm_state_hidden, a_net_wm_state_skip_taskbar,
         a_net_wm_state_above, a_net_wm_state_modal,
         a_net_wm_window_type, a_net_wm_wt_dock, a_net_wm_wt_dialog,
         a_net_wm_wt_normal, a_net_wm_wt_menu, a_net_wm_wt_utility,
         a_net_wm_wt_splash, a_net_wm_wt_toolbar,
         a_net_client_list, a_net_active_window, a_net_current_desktop,
         a_net_number_of_desktops, a_net_wm_desktop, a_net_supporting_wm_check,
         a_net_close_window, a_net_workarea, a_net_wm_pid,
         a_net_wm_moveresize, a_net_moveresize_window,
         a_net_frame_extents, a_net_wm_icon,
         a_utf8, a_w2k_command, a_w2k_scheme, a_w2k_notify;
} W2k;

extern W2k w2k;

/* ------------------------------------------------------------------ *
 * Monitors
 * ------------------------------------------------------------------ *
 * w2k.sw/sh describe the bounding box of every monitor together, which is
 * the wrong rectangle for anything that has to look "on screen". Ask RandR
 * for the real ones instead. There is always at least one monitor, and
 * always exactly one primary. */
typedef struct {
    char name[64];                  /* RandR output name, e.g. "HDMI-A-0" */
    int  x, y, w, h;
    int  primary;
} W2kMonitor;

/* The monitor arrangement Display Properties last applied, kept in the
 * scheme file so it comes back with the session: one entry per output. */
typedef struct {
    char name[64];
    char mode[16];      /* "1920x1080", or "" for the output's preferred mode */
    int  x, y;
    int  primary, enabled;
} W2kMonitorCfg;
extern W2kMonitorCfg w2k_monitor_cfg[8];
extern int           w2k_monitor_cfg_n;
/* Re-apply the saved arrangement with xrandr, for the outputs that are
 * actually connected now. 1 if anything was applied. */
int  w2k_monitors_apply_saved(void);

void w2k_monitors_init(void);
void w2k_monitors_refresh(void);
int  w2k_monitors_event(XEvent *e);    /* 1 if the layout changed */
int  w2k_monitor_count(void);
const W2kMonitor *w2k_monitor(int i);
const W2kMonitor *w2k_monitor_primary(void);
const W2kMonitor *w2k_monitor_at(int x, int y);
const W2kMonitor *w2k_monitor_of_window(Window w);
const W2kMonitor *w2k_monitor_of_pointer(void);

/* Load the cursor set from ~/.w2k/cursors (see lib/cursor.c). */
void w2k_cursors_init(void);

/* Bring up the display connection, colours, fonts, GCs and atoms.
 * Returns 0 on success, -1 if the display could not be opened. */
int  w2k_init(const char *appname);
void w2k_fini(void);

/* ------------------------------------------------------------------ *
 * Drawing primitives
 * ------------------------------------------------------------------ */
/* ---- Colour schemes ------------------------------------------------ *
 * The "Windows Standard" colours are built in; ~/.w2k/scheme overrides
 * them with lines like "ActiveTitle=10 36 106" (the registry names).
 * Any process can change a colour and broadcast it; every w2k program
 * reloads when the _W2K_SCHEME root property changes. */
const char *w2k_color_name(int color);          /* registry name of C_* */
int   w2k_color_by_name(const char *name);      /* -1 if unknown         */
void  w2k_color_set(int color, int r, int g, int b);
int   w2k_scheme_load(const char *path);        /* NULL = default file   */
int   w2k_scheme_save(const char *path);
void  w2k_scheme_broadcast(void);               /* tell other processes  */
void  w2k_scheme_reset(void);                   /* back to Windows Standard */
/* Wallpaper settings live in the same file: Wallpaper=<path>, WallpaperStyle=center|tile|stretch */
extern char w2k_wallpaper[1024];
extern int  w2k_wallpaper_style;
/* ForceDecorations=1 (the default) makes the window manager frame ordinary
 * windows even when the application asks for no decorations. Set it to 0 in
 * ~/.w2k/scheme for apps that draw their own title bar. */
extern int  w2k_force_decorations;

/* ---- Start menu appearance (also kept in ~/.w2k/scheme) ------------- *
 * The banner is the vertical strip down the left of the Start menu. Its
 * text is either the Windows product name, this machine's distribution, or
 * whatever the user typed; its gradient is two colours, drawn smoothly or
 * with the classic 8-bit dither between bands. */
enum { SB_WINDOWS, SB_DISTRO, SB_CUSTOM };
enum { SI_FLAG, SI_TUX, SI_DISTRO };            /* Start button icon */

extern int  w2k_start_banner_mode;
extern char w2k_start_banner_custom[128];
extern int  w2k_start_banner_top[3];            /* r, g, b */
extern int  w2k_start_banner_bottom[3];
extern int  w2k_start_banner_dither;
extern int  w2k_start_icon;
extern int  w2k_start_search;   /* type in the Start menu to search */
extern int  w2k_start_panel;    /* two-column Start menu (XP style) */
extern int  w2k_start_small_icons;
extern int  w2k_start_personalized;

/* ---- Visual effects ------------------------------------------------- *
 * The list from the Performance Options dialog, in its order. Some of them
 * describe things this desktop does not have (font smoothing needs scalable
 * fonts; folder backgrounds and visual styles are XP shell furniture), and
 * those are listed but not settable -- see w2k_effect_supported(). */
enum {
    FX_ANIM_MINMAX,        FX_FADE_MENUS,      FX_FADE_TOOLTIPS,
    FX_FADE_MENUITEMS,     FX_MENU_SHADOW,     FX_CURSOR_SHADOW,
    FX_TRANSLUCENT_SEL,    FX_DRAG_CONTENTS,   FX_SLIDE_COMBO,
    FX_SLIDE_TASKBUTTONS,  FX_SMOOTH_FONTS,    FX_SMOOTH_SCROLL,
    FX_FOLDER_BACKGROUND,  FX_COMMON_TASKS,    FX_ICON_SHADOW,
    FX_VISUAL_STYLES,      FX_HIDE_ACCEL,      N_EFFECTS
};

extern unsigned char w2k_effects[N_EFFECTS];
const char *w2k_effect_label(int i);
int         w2k_effect_supported(int i);
void        w2k_effects_preset(int which);   /* 0 best appearance, 1 best
                                                performance, 2 let us choose */

/* ---- Taskbar (also in ~/.w2k/scheme) -------------------------------- */
extern int  w2k_taskbar_ontop;      /* keep the bar above other windows */
extern int  w2k_taskbar_autohide;   /* slide it away until pointed at   */
extern int  w2k_taskbar_showclock;
enum { TB_BOTTOM, TB_TOP, TB_LEFT, TB_RIGHT };
extern int  w2k_taskbar_edge;       /* which side of the screen  */
extern int  w2k_taskbar_rows;       /* 1..4 rows of task buttons */
extern int  w2k_taskbar_quicklaunch;
extern int  w2k_taskbar_labels;     /* Windows 7: never combine, show labels */
extern int  w2k_taskbar_small;      /* Windows 7: small icons, a 30-row bar   */

/* PRETTY_NAME from /etc/os-release, e.g. "Debian GNU/Linux 13 (trixie)". */
const char *w2k_distro_name(void);
/* What the banner should actually read, for the mode in force. */
const char *w2k_start_banner_text(void);
/* Load the Start button icon the settings ask for into ICO_STARTFLAG. */
void        w2k_start_icon_apply(void);
void  w2k_scheme_default_path(char *buf, int n);
/* Windows bitmap -> RGBA (caller frees), or NULL. */
unsigned char *w2k_bmp_load(const char *path, int *w, int *h);
/* PNG -> RGBA (caller frees), or NULL. 8-bit, non-interlaced. */
unsigned char *w2k_png_load(const char *path, int *w, int *h);
unsigned char *w2k_jpeg_load(const char *path, int *w, int *h);
/* Any of the above, chosen by what the file actually contains. */
unsigned char *w2k_image_load(const char *path, int *w, int *h);
/* File operations shared by everything that takes a drop (lib/fileops.c). */
int  w2k_fs_copy_tree(const char *from, const char *to);
int  w2k_fs_remove_tree(const char *path);
int  w2k_fs_move(const char *from, const char *to);      /* rename, or copy and delete */
/* Move or copy `n` paths into `dir`; `confirm` answers 1 replace, 0 skip,
 * -1 stop for a name already there (NULL: always replace). Returns how
 * many landed. */
int  w2k_fs_transfer(char paths[][1024], int n, const char *dir, int move,
                     int (*confirm)(const char *dst, void *user), void *user);
/* Mounted media and /mnt, lettered from D:. */
typedef struct { char path[512], label[128]; char letter; int optical, removable; } W2kDrive;
int  w2k_fs_drives(W2kDrive *out, int max);
int  w2k_fs_write_url_shortcut(const char *dir, const char *url);
int  w2k_uri_list_urls(const char *uris, char urls[][1024], int max);

/* Writing pictures: RGBA in, a file out. 1 on success. */
int  w2k_png_save(const char *path, const unsigned char *rgba, int w, int h);
int  w2k_jpeg_save(const char *path, const unsigned char *rgba, int w, int h);
int  w2k_bmp_save(const char *path, const unsigned char *rgba, int w, int h);
/* The PNG file's bytes in memory (malloc'd), for the clipboard. */
unsigned char *w2k_png_encode(const unsigned char *rgba, int w, int h, size_t *out_n);
int            w2k_image_is_image(const char *path);

/* ---- Themed painting ----------------------------------------------- *
 * The gradients, rounded corners and coloured buttons that arrived with
 * Luna. Only meaningful for THEME_XP and THEME_BASIC7; the classic look
 * is drawn with w2k_edge() as before. */
enum { W2K_CAP_MIN, W2K_CAP_MAX, W2K_CAP_RESTORE, W2K_CAP_CLOSE };
enum { W2K_TB_NORMAL, W2K_TB_HOT, W2K_TB_DOWN };

void w2k_theme_caption(Drawable d, int x, int y, int w, int h, int active,
                       int theme);
int  w2k_theme_caption_h(int theme);      /* rows above the client, less the border */
void w2k_theme_frame_edges(Drawable d, int fw, int fh, int b, int active,
                           int theme);
int  w2k_theme_capbtn_size(int theme);   /* a caption button's height */
int  w2k_theme_capbtn_w(int theme, int kind); /* and its width: Windows 7's differ by kind */
void w2k_theme_capbtn_place(int theme, int fw, int *y, int *close_x,
                            int *max_x, int *min_x);
void w2k_theme_capbtn(Drawable d, int x, int y, int w, int h, int kind,
                      int active, int pressed, int theme);
int  w2k_theme_task_h(int theme);
void w2k_theme_taskbutton(Drawable d, int x, int y, int w, int h, int state,
                          int theme);
void w2k_theme_bar(Drawable d, int x, int y, int w, int h, int theme);

/* ---- Bitmap skins -------------------------------------------------- *
 * An RGBA image kept on the server and drawn in pieces -- for the parts
 * of a themed shell that are artwork rather than edges and gradients
 * (Windows XP's Start button, a strip of three states). */
typedef struct W2kSkin W2kSkin;
W2kSkin *w2k_skin_load(const char *path);
/* Find a skin by file name: the user's ~/.w2k/skins, then skins/ beside
 * the binaries' directory, then the installed copy. 1 if found. */
int  w2k_skin_path(const char *name, char *out, int n);
W2kSkin *w2k_skin_load_scaled(const char *path, int scale);
/* A skin from pixels already in memory (RGBA, row-major); the caller keeps
 * the buffer. */
W2kSkin *w2k_skin_from_rgba(const unsigned char *rgba, int w, int h);
void     w2k_skin_free(W2kSkin *s);
void     w2k_skin_cache_flush(void);   /* re-read the artwork on the next paint */
GC       w2k_copy_gc(void);   /* a GC that copies and nothing else: no clip, no tile */
int      w2k_skin_w(const W2kSkin *s);
int      w2k_skin_h(const W2kSkin *s);
void     w2k_skin_draw(Drawable d, const W2kSkin *s, int x, int y,
                       int sx, int sy, int sw, int sh);
/* Fill w by h at (x,y) with the sw by sh piece at (sx,sy) repeated: one
 * request, whatever the size. Solid pieces only. */
void     w2k_skin_tile(Drawable d, W2kSkin *s, int x, int y, int w, int h,
                       int sx, int sy, int sw, int sh);

/* ---- Theme --------------------------------------------------------- *
 * Two built-in looks: the Windows 2000 "Windows Standard" scheme, and
 * Windows XP's Luna. The theme decides the colour table and the handful
 * of things XP draws differently -- gradient taskbar, skinned Start
 * button, the two-column Start menu. */
enum { THEME_CLASSIC = 0, THEME_XP, THEME_BASIC7, N_THEMES };
extern const char *w2k_theme_name(int theme);
extern int w2k_theme;
void w2k_theme_colours(int theme);   /* load that theme's colour table */

/* ---- Input settings ------------------------------------------------ *
 * The Mouse, Keyboard and Sounds applets. w2k_input_apply() pushes them
 * into the X server; the shell reads the rest directly. */
extern int w2k_dblclk_ms;        /* double-click speed */
extern int w2k_mouse_swap;       /* left-handed button order */
extern int w2k_mouse_speed;      /* pointer acceleration, 1..10 */
extern int w2k_key_delay;        /* auto-repeat delay, ms */
extern int w2k_key_rate;         /* auto-repeat rate, characters/second */
extern int w2k_caret_blink;      /* caret blink half-period, ms */
extern int w2k_bell_on;
extern int w2k_bell_volume;      /* percent */
extern int w2k_bell_pitch;       /* Hz */
extern int w2k_bell_duration;    /* ms */
void w2k_input_apply(void);

/* ---- Folder Options ------------------------------------------------ *
 * Explorer's View tab, shared so that every window (and the desktop)
 * agrees about hidden files and extensions. */
extern int w2k_folder_hidden;       /* show hidden files and folders */
extern int w2k_folder_hide_ext;     /* hide extensions for known types */
extern int w2k_folder_fullpath;     /* full path in the title bar */
extern int w2k_folder_singleclick;  /* single-click to open an item */
extern int w2k_folder_newwindow;    /* open each folder in its own window */
extern int w2k_folder_tooltips;     /* pop-up descriptions for shell items */
extern int w2k_view_toolbar;        /* Explorer: View > Toolbars */
extern int w2k_view_address;
extern int w2k_view_status;

/* Keyboard navigation indicators -- the underlines under menu mnemonics.
 * Windows 2000 hides them until the Alt key is pressed (Display
 * Properties > Effects); w2k_accel_show() is what notices the key. */
extern int w2k_accel_shown;
void w2k_accel_show(void);      /* Alt was pressed: reveal them */
void w2k_accel_reset(void);     /* a menu closed: hide them again */

/* ---- .desktop files ------------------------------------------------ *
 * The name, command and icon name out of a freedesktop desktop entry --
 * this system's .lnk. Returns 1, 0 when the file is not a usable entry,
 * or -1 for an entry marked NoDisplay/Hidden (usable, but not to be
 * listed). Any of the three outputs may be NULL. */
int w2k_desktop_entry(const char *path, char *name, int nn,
                      char *exec, int en, char *icon, int in);

/* ---- File types ---------------------------------------------------- *
 * The friendly name and icon for a file, by extension: "Text Document",
 * not "TXT File". Shared so that every window agrees. */
const char *w2k_file_ext(const char *name);
void        w2k_file_type(const char *name, int isdir, char *out, int n);
int         w2k_file_icon(const char *name, int isdir);
int         w2k_file_known_ext(const char *name);
/* The name as Explorer shows it -- the extension is dropped when the
 * "hide extensions for known file types" option is on. */
const char *w2k_file_display_name(const char *name, int isdir,
                                  char *out, int n);
/* As above, but a file with no extension that is executable is a program. */
int         w2k_file_icon_stat(const char *path, const char *name, int isdir);

/* ---- File associations (~/.w2k/associations) ------------------------ *
 * Files are grouped into a few classes -- pictures, video, audio, text --
 * and each class has a program. The Control Panel edits them. */
int         w2k_assoc_count(void);
const char *w2k_assoc_class_at(int i);
const char *w2k_assoc_label_at(int i);
void        w2k_assoc_get(const char *cls, char *out, int n);
void        w2k_assoc_set(const char *cls, const char *cmd);
const char *w2k_assoc_class_for(const char *path);
void        w2k_assoc_command(const char *path, char *out, int n);
int         w2k_assoc_apply_folder_default(void);   /* make the Folders program the XDG default */

/* The Recycle Bin: the freedesktop trash under ~/.local/share/Trash. */
const char *w2k_trash_dir(void);
const char *w2k_trash_files_dir(void);
int         w2k_trash_count(void);
int         w2k_trash_empty(void);      /* entries removed, or -1 */
int         w2k_trash_move(const char *path);      /* delete to the bin */
/* The same, reporting the name the item took inside the bin -- two files
 * of the same name can be deleted, and the second is renamed. */
int         w2k_trash_move_named(const char *path, char *name_out, int nout);
int         w2k_trash_restore(const char *name);   /* put one back */

/* Raw RGB -> pixel value on the current visual (TrueColor fast path). */
unsigned long w2k_rgb(int r, int g, int b);
void w2k_color_rgb(int color, int *r, int *g, int *b);

void w2k_fill(Drawable d, int x, int y, int w, int h, int color);
void w2k_frame(Drawable d, int x, int y, int w, int h, int color);
void w2k_hline(Drawable d, int x, int y, int w, int color);
void w2k_vline(Drawable d, int x, int y, int h, int color);

/* DrawEdge(). Returns nothing; use w2k_edge_size() for the inset. */
void w2k_edge(Drawable d, int x, int y, int w, int h, int style, int flags);
int  w2k_edge_size(int style);

/* A full pushbutton face: edge + filled interior. `pressed` sinks it. */
void w2k_button(Drawable d, int x, int y, int w, int h, int pressed);

/* Horizontal two-stop gradient, used by title bars. */
void w2k_gradient(Drawable d, int x, int y, int w, int h, int c1, int c2);
/* The themed taskbar background (THEME_XP, THEME_BASIC7). */
void w2k_bar_gradient(Drawable d, int x, int y, int w, int h, int theme);

/* ---- Drag and drop (lib/dnd.c), the XDND protocol ------------------- *
 * The caller owns the pointer grab and feeds us the drag; we speak the
 * protocol. Data is always text/uri-list, which is how files travel
 * between applications on X. */
void  w2k_dnd_accept(Window w);          /* this window takes drops */
void  w2k_dnd_begin(Window from, const char *uri_list, int move);
int   w2k_dnd_active(void);
int   w2k_dnd_motion(int root_x, int root_y);   /* 1 if a target accepts */
int   w2k_dnd_drop(void);                /* 1 if it was taken */
void  w2k_dnd_set_time(Time t);          /* the last event's time, for the protocol */
void  w2k_dnd_cancel(void);
int   w2k_dnd_event(XEvent *e);          /* 1 if the event was ours */

/* Set these to hear about drops on your windows. */
extern void (*w2k_dnd_on_drop)(Window w, int x, int y, const char *uris,
                               int move);
extern int  (*w2k_dnd_will_accept)(Window w, int x, int y);

int   w2k_uri_list_paths(const char *uris, char paths[][1024], int max);
char *w2k_uri_list_build(char paths[][1024], int n);

/* Ask the shell to show a notification balloon. Does nothing when no
 * shell is running. */
void w2k_notify(const char *title, const char *text);

/* Tooltips (lib/tooltip.c). The caller decides when to show one -- it knows
 * what the pointer is over and how long it has been there. */
void w2k_tooltip_show(const char *text, int root_x, int root_y);
void w2k_tooltip_hide(void);
int  w2k_tooltip_event(XEvent *e);

/* Caption button glyphs, drawn at the top-left of a 16x14 button. */
void w2k_capglyph_min(Drawable d, int x, int y, int color);
void w2k_capglyph_max(Drawable d, int x, int y, int color);
void w2k_capglyph_restore(Drawable d, int x, int y, int color, int face);
void w2k_capglyph_close(Drawable d, int x, int y, int color);

/* 1px dotted rectangle -- keyboard focus indication. */
void w2k_focus_rect(Drawable d, int x, int y, int w, int h);

/* Rectangular clipping for everything drawn through the toolkit, icons
 * included. Nest-free: set, draw, clear. */
void w2k_clip_set(int x, int y, int w, int h);
void w2k_clip_clear(void);

/* 50% checkerboard fill in `fg` over `bg` -- scrollbar troughs, rubber bands. */
void w2k_dither(Drawable d, int x, int y, int w, int h, int fg, int bg);

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */
/* Font backend (lib/font.c): Xft when it is usable, core fonts otherwise. */
int  w2k_font_init(void);
void w2k_font_reload(void);          /* the antialias setting changed */
void w2k_font_fini(void);
void w2k_font_forget(Drawable d);    /* a drawable is going away */
void w2k_free_pixmap(Pixmap p);      /* XFreePixmap, cache-aware */
void w2k_font_colours_dirty(void);   /* the palette changed */
int  w2k_font_using_xft(void);
int  w2k_font_px_height(int font);
int  w2k_font_px_ascent(int font);
int  w2k_font_px_width(int font, const char *s, int len);
void w2k_font_draw(Drawable d, int font, int x, int baseline,
                   const char *s, int len, int color);
void w2k_font_draw_mono(Pixmap p, GC g, int font, int x, int baseline,
                        const char *s, int len);

int  w2k_text_width(int font, const char *s, int len);
int  w2k_font_height(int font);
int  w2k_font_ascent(int font);
/* Draw at baseline-independent top-left (y is the top of the line box). */
void w2k_text(Drawable d, int font, int x, int y, const char *s, int color);
/* Text in a colour that is not one of the scheme's. */
void w2k_text_rgb(Drawable d, int font, int x, int y, const char *s,
                  int r, int g, int b);
void w2k_textn(Drawable d, int font, int x, int y, const char *s, int len, int color);
/* Greyed-out text: white offset copy underneath, as Windows does. */
void w2k_text_disabled(Drawable d, int font, int x, int y, const char *s);
/* Handles '&' mnemonics: draws the underline, returns width. '&&' is a literal &. */
int  w2k_text_mnemonic(Drawable d, int font, int x, int y, const char *s,
                       int color, int show_underline);
/* Width of a mnemonic string with the '&' markers removed. */
int  w2k_mnemonic_width(int font, const char *s);
/* Truncate with an ellipsis so the result fits in `maxw`. Writes to buf. */
void w2k_ellipsis(int font, const char *s, int maxw, char *buf, int bufsz);
/* Text rotated 90 degrees counter-clockwise, reading bottom-to-top.
 * (x, y) is the bottom-left of the resulting column. Used by the Start
 * menu's branding banner. */
/* A face asked for by name, for programs that display fonts rather than
 * merely use them. NULL when Xft is unavailable. */
typedef struct W2kFace W2kFace;
W2kFace *w2k_face_open(const char *family, int pixel);
void     w2k_face_close(W2kFace *f);
int      w2k_face_height(W2kFace *f);
int      w2k_face_ascent(W2kFace *f);
int      w2k_face_width(W2kFace *f, const char *s, int len);
int      w2k_face_has(W2kFace *f, unsigned cp);
void     w2k_face_text(Drawable d, W2kFace *f, int x, int y, const char *s,
                       int color);

void w2k_text_vertical(Drawable d, int font, int x, int y, const char *s,
                       int color);

/* ------------------------------------------------------------------ *
 * Menus
 * ------------------------------------------------------------------ */
typedef struct W2kMenu W2kMenu;

W2kMenu *w2k_menu_new(void);
void     w2k_menu_free(W2kMenu *m);
/* `text` may carry '&' mnemonics; `accel` is right-aligned grey text. */
void     w2k_menu_item(W2kMenu *m, int id, const char *text,
                       const char *accel, int icon);
void     w2k_menu_sub(W2kMenu *m, const char *text, int icon, W2kMenu *sub);
void     w2k_menu_sep(W2kMenu *m);
/* Modify the most recently added item. */
void     w2k_menu_disable(W2kMenu *m);
void     w2k_menu_check(W2kMenu *m, int on);
void     w2k_menu_radio(W2kMenu *m, int on);
void     w2k_menu_default(W2kMenu *m);
/* Start-menu styling: 32x32 icons and the vertical branding banner. */
void     w2k_menu_set_banner(W2kMenu *m, const char *text);
int      w2k_menu_count(W2kMenu *m);

/* Alignment flags for w2k_menu_popup(). */
#define MPOP_LEFT     0x00   /* x,y is the menu's top-left        */
#define MPOP_BOTTOMUP 0x01   /* grow upward from y (taskbar menus) */
#define MPOP_RIGHTALIGN 0x02 /* right-align the menu on x          */

/* Runs a modal loop with a pointer+keyboard grab. Returns the chosen
 * command id, or 0 if the menu was dismissed. */
int      w2k_menu_popup(W2kMenu *m, int x, int y, int flags);

/* While a menu is modal the rest of the desktop still needs to repaint.
 * The window manager points this at its own event handler. */
extern void (*w2k_menu_foreign_event)(XEvent *e);
/* Called when the menu chain closes, so the opener can un-press its button. */
extern void (*w2k_menu_closed)(void);

/* ------------------------------------------------------------------ *
 * Icons -- hand-drawn 16-colour art in the classic VGA palette
 * ------------------------------------------------------------------ */
enum {
    ICO_NONE = -1,
    ICO_APP = 0, ICO_FOLDER, ICO_FOLDER_OPEN, ICO_FILE, ICO_FILE_TEXT,
    ICO_MYCOMPUTER, ICO_DRIVE_HDD, ICO_DRIVE_FLOPPY, ICO_DRIVE_CD,
    ICO_NOTEPAD, ICO_EXPLORER, ICO_TASKMGR, ICO_MYDOCS, ICO_RECYCLE,
    ICO_NETWORK, ICO_PROGRAMS, ICO_DOCUMENTS, ICO_SETTINGS, ICO_SEARCH,
    ICO_HELP, ICO_RUN, ICO_SHUTDOWN, ICO_LOGOFF, ICO_UP, ICO_BACK,
    ICO_FORWARD, ICO_CUT, ICO_COPY, ICO_PASTE, ICO_DELETE, ICO_PROPERTIES,
    ICO_VIEWS, ICO_CONTROLPANEL, ICO_TERMINAL, ICO_CALC, ICO_PAINT,
    ICO_CHARMAP,
    ICO_INFO, ICO_WARNING, ICO_QUESTION, ICO_ERROR, ICO_ACCESSORIES,
    ICO_STARTFLAG, ICO_DESKTOP, ICO_WINUPDATE,
    /* Per-type document icons. Windows picks these by extension; so does
     * w2k_file_icon(). */
    ICO_FILE_BITMAP, ICO_FILE_JPEG, ICO_FILE_GIF, ICO_FILE_HTML,
    ICO_FILE_WAVE, ICO_FILE_MIDI, ICO_FILE_MOVIE, ICO_FILE_MEDIA,
    ICO_FILE_ZIP, ICO_FILE_INI, ICO_FILE_BAT, ICO_FILE_SYS, ICO_FILE_RTF,
    ICO_FILE_FONT, ICO_FILE_UNKNOWN,
    /* Shell overlays and states. */
    ICO_LINK_OVERLAY, ICO_RECYCLE_FULL, ICO_SPEAKER, ICO_CURSORFILE,
    ICO_FAVORITES, ICO_FONTS_FOLDER,
    ICO_SNIP,               /* the Snipping Tool */
    N_ICONS
};
/* ---- Optional icon skinning -------------------------------------- *
 * The built-in art can be overridden at runtime by real icon files, so a
 * user who owns a Windows licence can point this at their own extracted
 * shell32 icons. Nothing copyrighted ships with this program.
 *
 * Files are matched by slug and size, newest match winning:
 *     <dir>/<slug>-16.ico   <dir>/<slug>.ico   <dir>/<slug>.xpm
 * Returns the number of icons that were replaced. */
int  w2k_icon_load_dir(const char *dir);
/* The icon set: "win2k" is the built-in artwork; any other name is a
 * directory of <slug>.ico files under icons/sets (beside the binaries,
 * installed under PREFIX/share/w2k, or ~/.w2k/iconsets). */
extern char w2k_icon_set[32];
int  w2k_icon_set_apply(void);              /* load the set named in w2k_icon_set */
int  w2k_icon_sets(char names[][32], int max);  /* the sets available, win2k first */
const char *w2k_icon_set_label(const char *name);  /* "Windows XP" for "winxp" */
/* Override one icon from a file, or restore the built-in when path is NULL. */
int  w2k_icon_load_file(int id, const char *path);
/* Register an icon file (.ico, .png, .bmp, .jpg) as a new icon id. */
int  w2k_icon_from_file(const char *path);
/* The built-in icon a slug names, or -1: the reverse of w2k_icon_slug(). */
int  w2k_icon_by_slug(const char *slug);
/* Loads from $W2K_ICON_DIR, then ~/.w2k/icons, then the install prefix. */
int  w2k_icon_load_default(void);
/* The slug used in file names for an icon id, or NULL. */
const char *w2k_icon_slug(int id);

/* Register an icon at run time (16x16 and 32x32 RGBA, both taken over by
 * the toolkit) and get an id usable anywhere a built-in ICO_* is. */
int  w2k_icon_register(unsigned char *rgba16, unsigned char *rgba32);
int  w2k_icon_valid(int id);
/* Resolve a freedesktop icon name (or a path) from the icon themes, load it
 * and register it. Results are cached by name; ICO_APP if there is no such
 * icon. */
int  w2k_icon_by_name(const char *name);
/* Box-filter an RGBA image down (or up) to n x n; caller frees. */
unsigned char *w2k_rgba_scale(const unsigned char *src, int sw, int sh, int n);

void w2k_icon_draw(Drawable d, int x, int y, int id);
/* 32x32 rendering of the same icon id. Ids with dedicated large art use it;
 * the rest are scaled 2x from the 16x16 original. */
void w2k_bigicon_draw(Drawable d, int x, int y, int id);
/* The same, with the shortcut arrow composited over the corner. */
void w2k_icon_draw_link(Drawable d, int x, int y, int id);
void w2k_bigicon_draw_link(Drawable d, int x, int y, int id);
/* Draw greyed/dimmed (disabled menu item, cut file). */
void w2k_icon_draw_disabled(Drawable d, int x, int y, int id);

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */
#ifndef W2K_VERSION
#define W2K_VERSION "dev"
#endif
#ifndef W2K_PREFIX
#define W2K_PREFIX "/usr/local"         /* where make install put the data */
#endif
void *w2k_alloc(size_t n);          /* calloc that aborts on failure */
/* Quote a string for /bin/sh: single quotes, with any quote inside spliced
 * as '\''. Every path that reaches a shell must go through this. */
void  w2k_shell_quote(const char *in, char *out, int n);
/* Copy `tmpl` to `out` with the first "%s" replaced by `arg` (a literal
 * splice, never a printf format: the template is user configuration). */
void  w2k_splice(const char *tmpl, const char *arg, char *out, int n);
char *w2k_strdup(const char *s);
void  w2k_set_wm_name(Window w, const char *name);
long  w2k_now_ms(void);

#endif /* W2K_H */
