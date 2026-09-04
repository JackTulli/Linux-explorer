/* wm.h -- shared state for the Windows 2000 window manager. */
#ifndef W2KWM_H
#define W2KWM_H

#include "w2k.h"
#include <signal.h>

/* ------------------------------------------------------------------ *
 * Non-client metrics (the Windows 2000 defaults at 96 dpi)
 * ------------------------------------------------------------------ */
#define FRAME_SIZE   W2K_FRAME_SIZE    /* resizable window border  */
#define FRAME_FIXED  W2K_FRAME_FIXED   /* dialog border            */
#define CAPTION_H    W2K_CAPTION_H     /* SM_CYCAPTION             */
#define CAPBTN_W      16    /* SM_CXSIZE                                  */
#define CAPBTN_H      14    /* SM_CYSIZE                                  */
/* One row of task buttons plus the bar's edge. Windows XP and 7 are two
 * pixels taller than Windows 2000, which is what their Start button
 * artwork is drawn for. */
#define TASKBAR_ROW   (w2k_theme == THEME_CLASSIC ? 28 : \
                       w2k_theme == THEME_BASIC7 ? (w2k_taskbar_small ? 30 : 40) : 30)
#define TASKBAR_H     (TASKBAR_ROW * w2k_taskbar_rows)
int     taskbar_thickness(void);      /* height, or width when on a side */
#define CORNER_GRAB   16    /* diagonal resize hot-zone at each corner    */
#define MIN_CLIENT_W  64
#define MIN_CLIENT_H  24

/* Hit-test results, mirroring WM_NCHITTEST. */
enum {
    HT_NOWHERE = 0, HT_CLIENT, HT_CAPTION, HT_SYSMENU,
    HT_MINBUTTON, HT_MAXBUTTON, HT_CLOSE,
    HT_LEFT, HT_RIGHT, HT_TOP, HT_BOTTOM,
    HT_TOPLEFT, HT_TOPRIGHT, HT_BOTTOMLEFT, HT_BOTTOMRIGHT
};

typedef struct Client Client;
struct Client {
    Window   win;                  /* the application's window    */
    Window   frame;                /* our decoration parent       */
    char    *name;                 /* caption text                */
    char    *cls;                  /* WM_CLASS instance name      */
    char    *cls_name;             /* WM_CLASS class: the app's name */
    int      static_gravity;       /* the position given is the client's */
    int      x, y, w, h;           /* client rect, root-relative  */
    int      rx, ry, rw, rh;       /* restore rect when maximized */
    int      minw, minh, maxw, maxh;
    int      incw, inch, basew, baseh;

    unsigned decorate    : 1;      /* draw a frame at all         */
    unsigned resizable   : 1;
    unsigned maximized   : 1;
    unsigned minimized   : 1;
    unsigned shaded      : 1;
    unsigned fullscreen  : 1;
    unsigned above       : 1;
    unsigned skip_taskbar: 1;
    unsigned is_dialog   : 1;
    unsigned mapped      : 1;
    unsigned takes_focus : 1;
    unsigned deleteable  : 1;      /* supports WM_DELETE_WINDOW   */
    unsigned take_focus_proto : 1; /* supports WM_TAKE_FOCUS      */
    unsigned state_read  : 1;      /* _NET_WM_STATE taken from the app once */

    int      icon;                 /* w2k icon id for caption/taskbar */
    int      btn_down;             /* caption button being clicked    */
    int      btn_hot;              /* caption button under pointer    */
    int      ignore_unmap;         /* UnmapNotifys we caused ourselves */
    Pixmap   capbuf;               /* caption back buffer, kept between paints */
    int      capbuf_w, capbuf_h;

    long     tb_order;             /* taskbar position, draggable     */
    Client  *next;                 /* creation order (taskbar order)  */
    Client  *snext;                /* stacking order, topmost first   */
};

/* ------------------------------------------------------------------ *
 * Globals (wm.c)
 * ------------------------------------------------------------------ */
extern Client *clients;            /* creation order list  */
extern Client *stack;              /* stacking order list  */
extern Client *focused;
extern Window  wm_check;           /* _NET_SUPPORTING_WM_CHECK window */
extern int     wa_x, wa_y, wa_w, wa_h;  /* primary monitor, minus the taskbar */
/* Cleared by the SIGTERM/SIGINT handler, so it is signal-safe rather
 * than a plain int the compiler may keep in a register. */
extern volatile sig_atomic_t running;
extern Time    wm_last_time;       /* of the last input event seen */

/* client.c */
Client *client_find(Window w);
Client *client_find_frame(Window w);
void    client_manage(Window w, int initial_map);
void    client_unmanage(Client *c, int destroyed);
void    client_update_name(Client *c);
void    client_update_hints(Client *c);
void    client_update_type(Client *c);
void    client_move_resize(Client *c, int x, int y, int w, int h);
void    client_focus(Client *c);
void    client_raise(Client *c);
void    client_close(Client *c);
void    client_minimize(Client *c);
void    client_restore(Client *c);
void    client_maximize(Client *c, int on);
void    client_fullscreen(Client *c, int on);
void    client_publish_state(Client *c);  /* write _NET_WM_STATE back */
void    client_send_protocol(Client *c, Atom proto);
void    clients_restack(void);
int     client_frame_w(Client *c);
int     client_frame_h(Client *c);
int     client_border(Client *c);
int     client_caption_h(Client *c);
void    client_constrain(Client *c, int *w, int *h);

/* frame.c */
void    frame_paint(Client *c);
void    frame_shape(Client *c);       /* rounded corners, themed only */
int     frame_hittest(Client *c, int fx, int fy);
void    frame_button_press(Client *c, XButtonEvent *e);
void    frame_button_release(Client *c, XButtonEvent *e);
void    frame_motion(Client *c, XMotionEvent *e);
void    frame_leave(Client *c);
Cursor  frame_cursor(int ht);

/* input.c */
void    do_move(Client *c, XButtonEvent *e);
void    do_resize(Client *c, XButtonEvent *e, int ht);
void    grab_keys(void);
void    handle_key(XKeyEvent *e);
void    handle_key_release(XKeyEvent *e);
void    taskbar_skins_reload(void);
void    startpanel_skins_reload(void);
extern volatile sig_atomic_t restarting;   /* w2kwm --restart / SIGHUP */
void    sysmenu_popup(Client *c, int x, int y);
void    alt_tab(int backwards);
/* Zoom a wire frame between two rectangles (the minimise animation). */
void    wm_animate_rect(int fx, int fy, int fw, int fh,
                        int tx, int ty, int tw, int th);
/* Where a window's taskbar button is, for that animation. */
int     taskbar_button_rect(Client *c, int *x, int *y, int *w, int *h);

/* taskbar.c */
void    taskbar_init(void);
void    taskbar_paint(void);
void    taskbar_sync(void);           /* rebuild the task button list */
int     taskbar_event(XEvent *e);     /* returns 1 if consumed        */
void    taskbar_tick(void);           /* clock update                 */
void    taskbar_hover_tick(void);     /* tooltip after a pause        */
void    taskbar_relayout(void);       /* edge or size changed         */
int     taskbar_dnd_accept(int x, int y);
void    taskbar_dnd_drop(int x, int y, const char *uris);
Window  taskbar_window(void);
void    taskbar_reveal(int show);     /* auto-hide slide */
Window  taskbar_trigger_window(void);
void    taskbar_start_origin(int *x, int *y);

/* tray.c -- the notification area (freedesktop system tray protocol) */
void tray_init(Window taskbar);
void tray_layout(int x, int y, int h);
void tray_layout_column(int x, int y);
int  tray_width(void);
int  tray_count(void);
int  tray_owns(Window w);
int  tray_event(XEvent *e);
void tray_fini(void);

/* volume.c -- the speaker in the notification area */
void volume_poll(void);
int  volume_available(void);   /* is there a mixer to talk to at all? */
int  volume_level(void);          /* 0..100, -1 when there is no mixer */
int  volume_is_muted(void);
void volume_set(int pct);
void volume_toggle_mute(void);
void volume_draw(Drawable d, int x, int y);
void volume_popup(int bx, int by);   /* the slider, above the speaker */

/* balloon.c -- notification balloons */
void balloon_show(const char *title, const char *text);
/* Queue a balloon: icon id, timeout in ms (0 = the default), and the
 * notification id a program gave it (0 for the shell's own). */
void balloon_queue(const char *title, const char *text, int icon, int ms, unsigned id);
void balloon_close_id(unsigned id);
int  balloon_render(const char *path);   /* development aid */
void balloon_hide(void);
/* notifyd.c: the org.freedesktop.Notifications service. */
int  notifyd_init(void);
int  notifyd_fd(void);                /* -1 when not serving */
void notifyd_dispatch(void);
void notifyd_closed(unsigned id, int reason);
void notifyd_action(unsigned id, const char *action);
void notifyd_fini(void);
/* taskbar.c: where a balloon's tail should point -- the notification
 * area's root position; *top says the bar is along the top edge. */
void taskbar_tray_anchor(int *x, int *y, int *top);
void balloon_tick(void);
int  balloon_event(XEvent *e);

/* startmenu.c */
void    startmenu_open(void);
void    startmenu_close(void);
int     startmenu_is_open(void);

/* pins.c -- pinned applications */
#define PIN_MAX 16
enum { PIN_TASKBAR, PIN_START };
/* A pin is a command, the name to show and the icon to show it with.
 * The icon is a freedesktop icon name, a path to an icon file, or
 * "w2k:<slug>" for one of the shell's own -- see pin_icon(). */
typedef struct { char cmd[512]; char label[128]; char icon[256]; } Pin;
int  pins_load(int which, Pin *out, int max);
int  pin_icon(const Pin *p);            /* icon id for a pin */
/* Change one pin in place, found by its command. */
void pins_rename(int which, const char *cmd, const char *label);
void pins_set_icon(int which, const char *cmd, const char *icon);
int  pins_contains(int which, const char *cmd);
void pins_add(int which, const char *cmd, const char *label,
              const char *icon);
void pins_remove(int which, const char *cmd);
void pin_command_for_client(Client *c, char *cmd, int cn, char *label, int ln,
                            char *icon, int in);

/* startdir.c -- the Start menu's own folder tree */
#define STARTDIR_BASE 2000        /* command ids: STARTDIR_BASE + index */
void        startdir_path(char *buf, int n);
void        startdir_ensure(void);
int         startdir_add_programs(W2kMenu *m);
const char *startdir_command(int id);
void        startdir_run_startup(void);

/* recent.c -- the Documents menu */
/* Start menu command ids, shared by the classic menu and the two-column
 * panel so that both run the same dispatch. */
#define SM_PIN_BASE 300          /* pinned entries: SM_PIN_BASE + index */

enum {
    SM_UPDATE = 100,
    SM_EXPLORER, SM_NOTEPAD, SM_TASKMGR, SM_CALC, SM_PAINT, SM_TERMINAL, SM_SNIP, SM_DEVMGMT,
    SM_CHARMAP, SM_CONTROLPANEL, SM_NETWORK, SM_FOLDEROPTS, SM_TASKBARPROPS, SM_MYDOCS,
    SM_MYCOMPUTER, SM_IMAGING, SM_DEFAULTS,
    SM_SEARCH, SM_HELP, SM_RUN, SM_LOGOFF, SM_SHUTDOWN, SM_DISPLAY,
    SM_CLEARDOCS,
    /* Only the two-column panel has these. */
    SM_ALLPROGRAMS, SM_MYPICS, SM_MYMUSIC, SM_RECENTSUB, SM_PANELSEARCH
};

/* The two-column Start menu (startpanel.c). Returns the command id the
 * user picked, or 0. */
int  startpanel_run(int bx, int by);
int  startpanel_render(const char *path);   /* W2K_RENDER aid */
int  taskbar_render(const char *path, int w);  /* likewise: the bar, `w` wide */
int  frame_render(const char *path, int cw, int ch, int active, int maximized);
void startmenu_dispatch(int id);     /* run one of the ids above */
const char *wm_terminal_cmd(void);    /* the terminal Command Prompt opens, or NULL */
int  startmenu_context(int id, int x, int y);   /* right-click menu for an id; 1 if it did something */

#define RECENT_BASE 2500          /* command ids: RECENT_BASE + index */
int         recent_load(void);
int         recent_count(void);
const char *recent_label(int i);
const char *recent_file(int i);
void        recent_clear(void);

/* programs.c */
void     programs_add_groups(W2kMenu *m);
int      programs_run(int id, const char *terminal);
int      programs_search(const char *query, int *ids, const char **names,
                         int max);
int      programs_icon(int id);
/* Name and icon of an installed application, found by the command that
 * starts it or by its WM_CLASS. Returns 1 when something matched. */
int      programs_lookup(const char *key, char *name, int nn,
                         char *icon, int in);
/* The command and .desktop icon behind one of the Programs menu ids. */
int      programs_entry(int id, char *cmd, int cn, char *name, int nn,
                        char *icon, int in);
void     programs_note_use(const char *name);
int      programs_is_chevron(int id, int *group);
void     programs_expand(int group);
void     programs_collapse_all(void);

/* desktop.c */
void    desktop_init(void);
void    desktop_paint(void);
void    desktop_reload(void);         /* colours / wallpaper changed */
void    desktop_bin_tick(void);       /* has the Recycle Bin filled up? */
/* How long the shell may sleep before each part needs attention again.
 * The main loop sleeps for the smallest of them instead of waking on a
 * fixed tick, so an idle desktop costs almost nothing. */
int     desktop_next_tick_ms(void);
int     taskbar_next_tick_ms(void);
int     balloon_next_tick_ms(void);
void    desktop_hover_tick(void);     /* icon tooltip after a pause */
void    desktop_scan(void);           /* re-read ~/Desktop */
void    desktop_dnd_init(void);
void    desktop_dnd_drop(int x, int y, const char *uris, int move);
int     desktop_event(XEvent *e);
Window  desktop_window(void);

/* wm.c helpers */
void    wm_spawn(const char *cmd);
void    wm_update_client_list(void);
void    wm_update_workarea(void);
/* Usable rectangle of one monitor: its own bounds, less the taskbar on
 * whichever monitor is primary. Every placement decision goes through one
 * of these rather than through the screen-wide wa_* below. */
void    wm_workarea_of(const W2kMonitor *m, int *x, int *y, int *w, int *h);
void    wm_workarea_at(int px, int py, int *x, int *y, int *w, int *h);
void    wm_workarea_of_client(Client *c, int *x, int *y, int *w, int *h);
void    wm_layout_changed(void);      /* monitors added, removed or moved */
void    wm_set_active(Client *c);
void    wm_set_state(Window w, long state);
/* Ends the session: 0 = log off, 1 = shut down, 2 = restart. The WM waits
 * for every window to close first, so apps can ask about unsaved work. */
void    wm_logout(int what);
void    wm_logout_check(void);        /* last window closed: finish a pending log-off */
void    wm_handle_event(XEvent *e);   /* the main dispatcher, for nested loops */
int     wm_xerror_ignore(Display *d, XErrorEvent *e);
int     wm_xerror(Display *d, XErrorEvent *e);
void    wm_shutdown_dialog(void);
void    wm_run_dialog(void);
void    wm_startmenu_dialog(void);   /* right-click on Start */
void    wm_search_dialog(const char *first);
void    wm_logoff_dialog(void);
void    wm_help_dialog(void);
/* The Change Icon picker: writes "w2k:<slug>" or a file path. 1 on OK. */
int     wm_change_icon_dialog(char *out, int outsz);

#endif /* W2KWM_H */
