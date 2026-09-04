/* w2kui.h -- windows, controls and dialogs built on the w2k drawing layer.
 *
 * This is deliberately not a widget tree. Each control is a plain struct
 * with a rectangle and some state; the owning window draws them in its
 * paint callback and routes events to them. That keeps the applications
 * short without dragging in a retained-mode framework. */
#ifndef W2KUI_H
#define W2KUI_H

#include "w2k.h"
#include <signal.h>

/* ------------------------------------------------------------------ *
 * Top-level windows
 * ------------------------------------------------------------------ */
typedef struct W2kWin W2kWin;

struct W2kWin {
    Window   win;
    int      w, h;
    int      min_w, min_h;
    int      resizable;
    Pixmap   buf;                 /* backbuffer; recreated on resize */
    void    *user;

    void   (*paint)(W2kWin *, Drawable);
    int    (*event)(W2kWin *, XEvent *);   /* 1 = handled */
    void   (*resized)(W2kWin *);
    /* Return 0 to veto the close (e.g. "Save changes?"). */
    int    (*closing)(W2kWin *);

    unsigned dirty : 1;
    unsigned alive : 1;
    int      result;
    W2kWin  *next;
};

W2kWin *w2k_win_new(const char *title, const char *cls,
                    int w, int h, int resizable);
void    w2k_win_show(W2kWin *w);
void    w2k_win_dirty(W2kWin *w);
void    w2k_win_close(W2kWin *w, int result);
void    w2k_win_title(W2kWin *w, const char *title);
void    w2k_win_destroy(W2kWin *w);
/* Centre on screen, or over `over` when it is non-NULL. */
void    w2k_win_center(W2kWin *w, W2kWin *over);

int     w2k_run(void);                 /* until every window has closed */
int     w2k_win_owns(Window win);      /* is this window one of ours? */
void    w2k_win_repaint_now(W2kWin *w);  /* paint this instant */
/* The Start menu's banner gradient, also used by its settings preview. */
void    w2k_menu_banner_fill(Drawable d, int x, int y, int w, int h);
/* Point at a 2-byte buffer to collect the first unclaimed printable key a
 * menu sees; the menu then closes so the caller can act on it. */
extern char *w2k_menu_typeahead;
/* Called when an item in an open menu is right-clicked. Put a context
 * menu up and return 1 if something was done (the menu then closes), or
 * 0 to leave the menu as it was. NULL for the usual "no context menu". */
extern int (*w2k_menu_on_context)(int id, int root_x, int root_y);
/* Events for windows this framework does not own go here. The window
 * manager points it at its own handler so its dialogs work while the
 * modal loop is running. */
extern void (*w2k_win_foreign_event)(XEvent *e);
/* Called after one of this process's windows is mapped. The window
 * manager sets it so that its own dialogs get frames -- X does not
 * redirect a map request back to the client that asked for redirection. */
extern void (*w2k_win_mapped)(Window w);
/* Setting this unwinds w2k_run() and every nested modal loop at the next
 * turn: a program that catches SIGTERM sets it from the handler, so that
 * a dialog being open is not the difference between exiting and hanging.
 * Signal-safe on purpose -- nothing else in this header is. */
extern volatile sig_atomic_t w2k_win_abort;
int     w2k_win_modal(W2kWin *dlg);    /* nested loop; returns ->result */

/* Timers. Up to eight; `fn` fires every `ms` milliseconds. */
void    w2k_add_timer(int ms, void (*fn)(void *), void *user);
void    w2k_del_timer(void (*fn)(void *), void *user);

/* ------------------------------------------------------------------ *
 * Simple drawn controls (no state of their own)
 * ------------------------------------------------------------------ */
typedef struct { int x, y, w, h; } W2kRect;

int  w2k_rect_hit(const W2kRect *r, int x, int y);

/* Push button. `state`: bit0 pressed, bit1 focused, bit2 default, bit3 disabled */
#define BS_PRESSED  1
#define BS_FOCUS    2
#define BS_DEFAULT  4
#define BS_DISABLED 8
void w2k_draw_pushbutton(Drawable d, const W2kRect *r, const char *text,
                         int state);
void w2k_draw_checkbox(Drawable d, int x, int y, const char *text,
                       int checked, int focused, int disabled);
void w2k_draw_radio(Drawable d, int x, int y, const char *text,
                    int checked, int focused, int disabled);
void w2k_draw_groupbox(Drawable d, const W2kRect *r, const char *text);
/* A sunken white field (edit boxes, list views, client areas). */
void w2k_draw_well(Drawable d, const W2kRect *r);
/* A progress bar; percent 0..100, or -1 for a marquee stepped by `phase`. */
void w2k_draw_progress(Drawable d, const W2kRect *r, int percent, int phase);

/* ------------------------------------------------------------------ *
 * Scrollbar
 * ------------------------------------------------------------------ */
#define SCROLL_W 16                 /* SM_CXVSCROLL */

enum { SB_NONE = 0, SB_LINEUP, SB_LINEDOWN, SB_PAGEUP, SB_PAGEDOWN, SB_THUMB };

typedef struct {
    W2kRect r;
    int vertical;
    int pos, page, total;           /* pos in [0, total-page] */
    int line;                       /* one "line" click step */
    int pressed;                    /* SB_* currently held    */
    int     drag_off;
    long    repeat_at;
    W2kWin *owner;                  /* set by w2k_scroll_bind() */
} W2kScroll;

/* Binding a scrollbar to its window enables click-and-hold auto-repeat. */
void w2k_scroll_bind(W2kScroll *s, W2kWin *w);
void w2k_scroll_draw(Drawable d, W2kScroll *s);
int  w2k_scroll_part(W2kScroll *s, int x, int y);
/* Returns 1 when `pos` changed. */
int  w2k_scroll_press(W2kScroll *s, int x, int y);
int  w2k_scroll_motion(W2kScroll *s, int x, int y);
void w2k_scroll_release(W2kScroll *s);
int  w2k_scroll_wheel(W2kScroll *s, int dir);
void w2k_scroll_clamp(W2kScroll *s);
int  w2k_scroll_needed(W2kScroll *s);

/* ------------------------------------------------------------------ *
 * Edit control -- single or multi line
 * ------------------------------------------------------------------ */
typedef struct {
    W2kRect  r;
    char    *text;                  /* NUL-terminated, grows as needed */
    int      len, cap;
    int      caret, sel;            /* caret and selection anchor      */
    int      multiline, readonly, password;
    int      font;
    int      scroll_x, scroll_y;    /* pixels / lines                  */
    int      focused, caret_on;
    int      wrap;
    int     *vls;                   /* visual line start offsets */
    int      nvl, vlcap;
    int      layout_w;              /* width the wrap was computed for */
    W2kScroll vsb, hsb;
    W2kWin  *owner;                 /* for caret blink + repaint */
    void   (*on_change)(void *user);
    void    *user;
    int      noframe;       /* container drew the sunken well already */
} W2kEdit;

W2kEdit *w2k_edit_new(int multiline);
/* Binding gives the control its caret blink and automatic repaints. */
void     w2k_edit_bind(W2kEdit *e, W2kWin *w);
void     w2k_edit_free(W2kEdit *e);
void     w2k_edit_set(W2kEdit *e, const char *text);
const char *w2k_edit_text(W2kEdit *e);
void     w2k_edit_draw(Drawable d, W2kEdit *e);
int      w2k_edit_key(W2kEdit *e, XKeyEvent *k);   /* 1 if consumed */
int      w2k_edit_press(W2kEdit *e, XButtonEvent *b);
int      w2k_edit_motion(W2kEdit *e, XMotionEvent *m);
void     w2k_edit_release(W2kEdit *e);
void     w2k_edit_blink(W2kEdit *e);
void     w2k_edit_scroll_to_caret(W2kEdit *e);
void     w2k_edit_select_all(W2kEdit *e);
void     w2k_edit_insert(W2kEdit *e, const char *s);
int      w2k_edit_line_count(W2kEdit *e);
void     w2k_edit_caret_rowcol(W2kEdit *e, int *row, int *col);
void     w2k_edit_layout(W2kEdit *e);   /* recompute scrollbars for e->r */
/* Clipboard-ish: cut/copy/paste through an in-process buffer plus the
 * X CLIPBOARD selection when the owner cooperates. */
void     w2k_edit_cut(W2kEdit *e);
void     w2k_edit_copy(W2kEdit *e);
void     w2k_edit_paste(W2kEdit *e);
void     w2k_edit_delete_sel(W2kEdit *e);
int      w2k_edit_has_sel(W2kEdit *e);

/* ------------------------------------------------------------------ *
 * Clipboard (the X CLIPBOARD selection, with an in-process fast path)
 * ------------------------------------------------------------------ */
void  w2k_clipboard_set(const char *text);
/* Own the clipboard with a picture, offered as image/png. */
void  w2k_clipboard_set_image(const unsigned char *rgba, int w, int h);
/* Caller frees. Returns NULL when the clipboard is empty. */
char *w2k_clipboard_get(void);
/* win.c routes selection events here; applications never call it. */
int   w2k_clipboard_event(XEvent *e);

/* ------------------------------------------------------------------ *
 * List view (report and list modes)
 * ------------------------------------------------------------------ */
#define LIST_MAXCOL 8

typedef struct {
    char *text[LIST_MAXCOL];
    int   icon;
    int   link;                     /* draw the shortcut arrow over it */
    int   selected;
    int   checked;                  /* when the list has check boxes */
    void *data;
} W2kListItem;

enum { LV_REPORT, LV_LIST, LV_ICON };

typedef struct {
    W2kRect      r;
    int          mode;
    struct { char *title; int w; int right; } col[LIST_MAXCOL];
    int          ncols;
    W2kListItem *items;
    int          n, cap;
    int          sel;               /* focused item, -1 for none */
    int          anchor;            /* where a Shift-click range starts */
    int          defer_single;      /* item to select alone on release, -1 */
    int          top;               /* first visible row         */
    int          row_h;
    int          hdr_h;
    int          multisel;
    int          checkboxes;        /* a check box in front of every row */
    int          singleclick;       /* one click opens (Folder Options) */
    /* Single-click mode opens on release, not press, so that dragging an
     * item does not also open it. */
    int          click_pending, click_item, click_x, click_y;
    /* Rubber band: dragged from empty space to select a run of items. */
    int          band_on, band_add;
    int          band_x0, band_y0, band_x1, band_y1;
    int          fullrow;           /* highlight the whole row, not just col 0 */
    int          focused;
    int          sort_col, sort_dir;
    int          drag_col;          /* column divider being dragged */
    int          drag_x;
    W2kScroll    vsb, hsb;
    int          scroll_x;
    void       (*on_activate)(void *user, int idx);
    void       (*on_check)(void *user, int idx);   /* a check box changed */
    void       (*on_select)(void *user, int idx);
    void       (*on_sort)(void *user, int col);
    void        *user;
} W2kList;

W2kList *w2k_list_new(int mode);
void     w2k_list_free(W2kList *l);
void     w2k_list_clear(W2kList *l);
void     w2k_list_add_col(W2kList *l, const char *title, int w, int right);
int      w2k_list_add(W2kList *l, int icon, void *data);
void     w2k_list_set(W2kList *l, int row, int col, const char *text);
void     w2k_list_draw(Drawable d, W2kList *l);
int      w2k_list_press(W2kList *l, XButtonEvent *b);
int      w2k_list_motion(W2kList *l, XMotionEvent *m);
/* Pass the release event when there is one: in single-click mode a
 * release over the pressed item is what opens it. NULL just ends the
 * press (a drag took over, say). */
void     w2k_list_release(W2kList *l, XButtonEvent *b);
int      w2k_list_key(W2kList *l, XKeyEvent *k);
void     w2k_list_layout(W2kList *l);
void     w2k_list_ensure_visible(W2kList *l, int idx);
int      w2k_list_hit(W2kList *l, int x, int y);

/* ------------------------------------------------------------------ *
 * Tree view
 * ------------------------------------------------------------------ */
typedef struct W2kTreeNode W2kTreeNode;
struct W2kTreeNode {
    char        *text;
    int          icon, icon_open;
    int          expanded, has_kids;
    void        *data;
    W2kTreeNode *parent, *child, *sibling;
    int          depth;
};

typedef struct {
    W2kRect      r;
    W2kTreeNode *root;              /* invisible; its children are level 0 */
    W2kTreeNode *sel;
    int          top, row_h, focused;
    W2kScroll    vsb;
    void       (*on_select)(void *user, W2kTreeNode *n);
    void       (*on_expand)(void *user, W2kTreeNode *n);
    void        *user;
} W2kTree;

W2kTree     *w2k_tree_new(void);
void         w2k_tree_free(W2kTree *t);
W2kTreeNode *w2k_tree_add(W2kTree *t, W2kTreeNode *parent, const char *text,
                          int icon, int icon_open, void *data);
void         w2k_tree_clear_children(W2kTree *t, W2kTreeNode *n);
void         w2k_tree_draw(Drawable d, W2kTree *t);
int          w2k_tree_press(W2kTree *t, XButtonEvent *b);
int          w2k_tree_key(W2kTree *t, XKeyEvent *k);
void         w2k_tree_layout(W2kTree *t);
void         w2k_tree_select(W2kTree *t, W2kTreeNode *n);

/* ------------------------------------------------------------------ *
 * Menu bar, toolbar, status bar, tabs
 * ------------------------------------------------------------------ */
#define MENUBAR_H 19

typedef struct {
    W2kRect   r;
    struct { char *text; W2kMenu *(*build)(void *user); int x, w; } item[10];
    int       n;
    int       open;                 /* index of the dropped menu, -1 */
    Window    win_ref;              /* for translating to root coordinates */
    void     *user;
    void    (*on_command)(void *user, int id);
} W2kMenubar;

W2kMenubar *w2k_menubar_new(void *user, void (*on_command)(void *, int));
void        w2k_menubar_add(W2kMenubar *mb, const char *text,
                            W2kMenu *(*build)(void *user));
void        w2k_menubar_clear(W2kMenubar *mb);
void        w2k_menubar_draw(Drawable d, W2kMenubar *mb);
int         w2k_menubar_press(W2kMenubar *mb, XButtonEvent *b);
/* Handles Alt+mnemonic and F10. */
int         w2k_menubar_key(W2kMenubar *mb, XKeyEvent *k);
void        w2k_menubar_free(W2kMenubar *mb);

#define TOOLBAR_H  26
#define TBB_SEP   -1

typedef struct {
    W2kRect r;
    struct { int id, icon; char *text; int x, w, disabled, checked, drop; } b[24];
    int     n, hot, pressed;
    int     show_text;
    void   *user;
    void  (*on_command)(void *user, int id);
} W2kToolbar;

W2kToolbar *w2k_toolbar_new(void *user, void (*on_command)(void *, int));
void        w2k_toolbar_add(W2kToolbar *tb, int id, int icon, const char *text);
void        w2k_toolbar_sep(W2kToolbar *tb);
/* The last button added gets a drop-down arrow beside it (Back, Views), in
 * a bay `bay` pixels wide, or the shell's usual width when `bay` is 1. */
void        w2k_toolbar_drop(W2kToolbar *tb, int bay);
void        w2k_toolbar_enable(W2kToolbar *tb, int id, int on);
void        w2k_toolbar_draw(Drawable d, W2kToolbar *tb);
int         w2k_toolbar_press(W2kToolbar *tb, XButtonEvent *b);
int         w2k_toolbar_motion(W2kToolbar *tb, XMotionEvent *m);
void        w2k_toolbar_release(W2kToolbar *tb);
void        w2k_toolbar_free(W2kToolbar *tb);

#define STATUS_H 20

typedef struct {
    W2kRect r;
    struct { char *text; int w; int icon; } pane[6];   /* w<=0 = stretch */
    int     n;
    int     sizegrip;
} W2kStatus;

W2kStatus *w2k_status_new(void);
void       w2k_status_add(W2kStatus *s, int w);
void       w2k_status_set(W2kStatus *s, int i, const char *text);
/* A 16-pixel icon in front of a pane's text ("My Computer"). */
void       w2k_status_icon(W2kStatus *s, int i, int icon);
void       w2k_status_draw(Drawable d, W2kStatus *s);
void       w2k_status_free(W2kStatus *s);

#define TABS_H  20      /* strip top to the body's top edge         */
#define TAB_PAD  6      /* space between a tab label and its edges  */

typedef struct {
    W2kRect r;
    struct { char *text; int x, w; } tab[8];
    int     n, sel;
    void   *user;
    void  (*on_change)(void *user, int idx);
} W2kTabs;

W2kTabs *w2k_tabs_new(void *user, void (*on_change)(void *, int));
void     w2k_tabs_add(W2kTabs *t, const char *text);
void     w2k_tabs_draw(Drawable d, W2kTabs *t);
int      w2k_tabs_press(W2kTabs *t, XButtonEvent *b);
int      w2k_tabs_key(W2kTabs *t, XKeyEvent *k);
void     w2k_tabs_free(W2kTabs *t);
/* The client rectangle inside the tab body. */
W2kRect  w2k_tabs_client(W2kTabs *t);

/* ------------------------------------------------------------------ *
 * Trackbar (slider)
 * ------------------------------------------------------------------ */
#define SLIDER_THICK 27        /* thumb height plus the tick row */

typedef struct {
    W2kRect r;                 /* the whole control, thumb and ticks */
    int     vertical;
    int     lo, hi, pos;
    int     ticks;             /* number of intervals; 0 for none */
    int     focused, dragging, grab;
    W2kWin *owner;             /* repainted when the value changes */
    void   *user;
    void  (*on_change)(void *user, int pos);
} W2kSlider;

void w2k_slider_draw(Drawable d, W2kSlider *s);
int  w2k_slider_press(W2kSlider *s, XButtonEvent *b);
int  w2k_slider_motion(W2kSlider *s, XMotionEvent *m);
void w2k_slider_release(W2kSlider *s);
int  w2k_slider_key(W2kSlider *s, XKeyEvent *k);

/* ------------------------------------------------------------------ *
 * Combo box (drop-down list)
 * ------------------------------------------------------------------ */
typedef struct {
    W2kRect r;
    char  **items;
    int     n, cap, sel;
    int     pressed, focused;
    int     editable;
    W2kEdit *edit;
    int     icon;                   /* drawn before the text; ICO_NONE for none */
    void  *user;
    void (*on_change)(void *user, int idx);
} W2kCombo;

W2kCombo *w2k_combo_new(int editable);
void      w2k_combo_free(W2kCombo *c);
void      w2k_combo_add(W2kCombo *c, const char *text);
void      w2k_combo_clear(W2kCombo *c);
void      w2k_combo_draw(Drawable d, W2kCombo *c);
int       w2k_combo_press(W2kCombo *c, XButtonEvent *b);
/* Editable combos (the Run box, an address bar): the text is the edit's,
 * and the list is history to pick from. */
int         w2k_combo_key(W2kCombo *c, XKeyEvent *k);
const char *w2k_combo_text(W2kCombo *c);
void        w2k_combo_set_text(W2kCombo *c, const char *text);

/* ------------------------------------------------------------------ *
 * Common dialogs
 * ------------------------------------------------------------------ */
#define MB_OK              0x00
#define MB_OKCANCEL        0x01
#define MB_YESNO           0x02
#define MB_YESNOCANCEL     0x03
#define MB_ICONINFO        0x10
#define MB_ICONWARNING     0x20
#define MB_ICONQUESTION    0x30
#define MB_ICONERROR       0x40

enum { ID_OK = 1, ID_CANCEL, ID_YES, ID_NO };

int  w2k_msgbox(W2kWin *over, const char *title, const char *text, int flags);
/* The colour dialog's 48 basic colours as a drop-down at root (rx, ry).
 * Returns 1 and sets r, g, b when one was picked. */
int  w2k_color_popup(int rx, int ry, int *r, int *g, int *b);
/* Single-line prompt. Returns 1 and fills `out` on OK. */
int  w2k_prompt(W2kWin *over, const char *title, const char *label,
                const char *initial, char *out, int outsz, int icon);
/* The file/folder property sheet. Returns 1 when the user pressed OK. */
int  w2k_file_properties(W2kWin *over, const char *path);
/* Tools > Folder Options. Returns 1 when the user pressed OK. */
int  w2k_folder_options(W2kWin *over);
/* File open/save. `path` is in/out. Returns 1 on OK. */
int  w2k_file_dialog(W2kWin *over, int save, char *path, int pathsz);
/* The same with a "Files of type" list. `filters` is
 * "Text Documents (*.txt)|*.txt|All Files (*.*)|*" -- Windows' filter
 * string with bars in place of its embedded NULs. Several extensions in
 * one pattern are separated by semicolons. */
int  w2k_file_dialog_filter(W2kWin *over, int save, char *path, int pathsz,
                            const char *filters);


/* ------------------------------------------------------------------ *
 * Folder window: the Windows 2000 shell's chrome around a list of items
 * -- menu bar, the standard toolbar, the Address bar, a web-view pane on
 * the left with the banner, the folder's name and some text, and the
 * status bar. Control Panel and Network and Dial-up Connections are
 * built on it. (lib/folderwin.c)
 * ------------------------------------------------------------------ */
enum { FW_BACK = 9001, FW_FORWARD, FW_UP, FW_SEARCH, FW_FOLDERS, FW_HISTORY,
       FW_MOVETO, FW_COPYTO, FW_DELETE, FW_UNDO, FW_VIEWS, FW_GO,
       FW_CLOSE, FW_TB_STANDARD, FW_TB_ADDRESS, FW_STATUSBAR, FW_V_LARGE,
       FW_V_LIST, FW_REFRESH, FW_SELECTALL, FW_FOLDEROPTS, FW_ABOUT,
       FW_HELPTOPICS, FW_PROPERTIES, FW_OPEN, FW_LAST };

#define FW_PANE_LINES 24
enum { FW_PLAIN = 0, FW_BOLD, FW_LINK, FW_BLANK };

typedef struct W2kFolderWin W2kFolderWin;
struct W2kFolderWin {
    W2kWin     *win;
    W2kMenubar *mb;
    W2kToolbar *tb;
    W2kCombo   *addr;
    W2kStatus  *sb;
    W2kList    *list;
    W2kRect     addr_label, go_r, pane_r, list_r;
    int         pane_w;             /* the web-view pane; 0 hides it */
    int         icon;               /* the folder's icon */
    char        title[96];          /* the pane's heading */
    struct { char *text; int style; W2kRect r; } line[FW_PANE_LINES];
    int         nlines;
    int         hot_link, go_down;
    int         show_toolbar, show_address, show_status;
    W2kFace    *title_face;
    W2kSkin    *banner;
    void       *user;
    void      (*on_command)(void *user, int id);   /* ids the app handles */
    W2kMenu  *(*build_file)(void *user);           /* extra File items, or NULL */
    W2kMenu  *(*build_help)(void *user);
    W2kMenu  *(*build_extra)(void *user);          /* a menu between Tools and Help */
    char        extra_title[32];
};

W2kFolderWin *w2k_folderwin_new(const char *title, const char *cls, int icon,
                                int w, int h, void *user,
                                void (*on_command)(void *, int));
void w2k_folderwin_free(W2kFolderWin *f);
/* Add a menu of the app's own between Tools and Help ("Advanced"). */
void w2k_folderwin_extra_menu(W2kFolderWin *f, const char *title,
                              W2kMenu *(*build)(void *user));
/* Call from the window's resized hook, and after changing the bars. */
void w2k_folderwin_layout(W2kFolderWin *f);
void w2k_folderwin_paint(W2kFolderWin *f, Drawable d);
/* Bars, list, links and the Go button. Returns 1 when the event was used;
 * link clicks and toolbar buttons arrive through on_command. */
int  w2k_folderwin_event(W2kFolderWin *f, XEvent *e);
/* The pane's text under the heading, one line at a time. Long plain and
 * bold lines wrap; FW_LINK lines are clickable and reported to
 * on_command as FW_LAST + their index. */
void w2k_folderwin_pane_clear(W2kFolderWin *f);
void w2k_folderwin_pane_add(W2kFolderWin *f, int style, const char *text);
/* The status bar's first pane, as the shell fills it. */
void w2k_folderwin_status(W2kFolderWin *f, const char *text);

#endif /* W2KUI_H */
