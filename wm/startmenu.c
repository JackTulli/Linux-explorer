/* startmenu.c -- the Start menu.
 *
 * Built fresh on every open so that state-dependent items (the user name,
 * which programs are actually installed) are always current. */
#include "wm.h"
#include "w2kui.h"
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int open_flag;
static int reopening;    /* set while reopening after a chevron */

int startmenu_is_open(void) { return open_flag; }

/* The Start button must un-press itself when the menu chain closes. */
static void on_menu_closed(void)
{
    open_flag = 0;
    taskbar_paint();
}

/* Is `cmd` on PATH? Used to grey out programs this machine does not have. */
static int have_cmd(const char *cmd)
{
    if (!cmd) return 0;
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin:/usr/local/bin";
    char buf[512];
    const char *p = path;
    while (*p) {
        const char *q = strchr(p, ':');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len && len + strlen(cmd) + 2 <= sizeof buf) {
            memcpy(buf, p, len);
            buf[len] = '/';
            strcpy(buf + len + 1, cmd);
            if (access(buf, X_OK) == 0) return 1;
        }
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

static const char *user_name(void)
{
    const char *u = getenv("USER");
    if (u && *u) return u;
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_name) ? pw->pw_name : "Owner";
}

/* Add an item, greying it out when its program is missing. */
static void prog_item(W2kMenu *m, int id, const char *label, const char *cmd,
                      int icon)
{
    w2k_menu_item(m, id, label, NULL, icon);
    if (!have_cmd(cmd)) w2k_menu_disable(m);
}

/* The terminal "Command Prompt" opens: the user's own choice ($TERMINAL),
 * then the system's default (x-terminal-emulator is Debian's alternatives
 * link to it), then whatever is installed, with xterm as the last resort
 * rather than the first. */
const char *wm_terminal_cmd(void)
{
    static char chosen[256];
    if (chosen[0]) return chosen;
    const char *env = getenv("TERMINAL");
    if (env && env[0] && have_cmd(env)) {
        snprintf(chosen, sizeof chosen, "%s", env);
        return chosen;
    }
    static const char *cands[] = {
        "x-terminal-emulator", "gnome-terminal", "konsole", "xfce4-terminal",
        "mate-terminal", "terminator", "kitty", "alacritty", "foot", "lxterminal",
        "urxvt", "st", "uxterm", "xterm", NULL
    };
    for (int i = 0; cands[i]; i++)
        if (have_cmd(cands[i])) {
            snprintf(chosen, sizeof chosen, "%s", cands[i]);
            return chosen;
        }
    return NULL;
}

static const char *terminal_cmd(void) { return wm_terminal_cmd(); }

/* ------------------------------------------------------------------ *
 * Right-clicking an item in the Start menu
 *
 * A program gets Pin to Start menu / Add to Quick Launch; something
 * already pinned gets Rename, Change Icon and Remove. The menu that
 * appears is a nested popup, and returning 0 leaves the Start menu open
 * behind it -- which is what Windows does.
 * ------------------------------------------------------------------ */
enum { CM_PIN_START = 1, CM_PIN_QL, CM_UNPIN, CM_RENAME, CM_ICON };

static int pinned_ctx(int which, const Pin *p, int x, int y)
{
    /* The nested menu is not itself right-clickable. */
    int (*saved)(int, int, int) = w2k_menu_on_context;
    w2k_menu_on_context = NULL;

    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, CM_RENAME, "Rena&me...", NULL, ICO_NONE);
    w2k_menu_item(m, CM_ICON, "Change &Icon...", NULL, ICO_NONE);
    w2k_menu_sep(m);
    w2k_menu_item(m, CM_UNPIN, "&Remove from this list", NULL, ICO_DELETE);
    int id = w2k_menu_popup(m, x, y, MPOP_LEFT);
    w2k_menu_free(m);
    w2k_menu_on_context = saved;

    switch (id) {
    case CM_RENAME: {
        char name[128];
        if (w2k_prompt(NULL, "Rename", "&Name:", p->label, name, sizeof name,
                       ICO_NONE) && name[0])
            pins_rename(which, p->cmd, name);
        break;
    }
    case CM_ICON: {
        char icon[256];
        if (wm_change_icon_dialog(icon, sizeof icon))
            pins_set_icon(which, p->cmd, icon);
        break;
    }
    case CM_UNPIN:
        pins_remove(which, p->cmd);
        break;
    default:
        return 0;                      /* nothing chosen: menu stays up */
    }
    taskbar_paint();
    return 1;
}

/* A program entry: offer to pin it either place. */
static int program_ctx(const char *cmd, const char *label, const char *icon,
                       int x, int y)
{
    int on_sm = pins_contains(PIN_START, cmd);
    int on_tb = pins_contains(PIN_TASKBAR, cmd);
    int (*saved)(int, int, int) = w2k_menu_on_context;
    w2k_menu_on_context = NULL;

    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, CM_PIN_START,
                  on_sm ? "Already on the Start men&u" : "Pin to Start Men&u",
                  NULL, ICO_STARTFLAG);
    if (on_sm) w2k_menu_disable(m);
    w2k_menu_item(m, CM_PIN_QL,
                  on_tb ? "Already in Quick &Launch" : "Add to Quick &Launch",
                  NULL, ICO_NONE);
    if (on_tb) w2k_menu_disable(m);
    int id = w2k_menu_popup(m, x, y, MPOP_LEFT);
    w2k_menu_free(m);
    w2k_menu_on_context = saved;

    if (id == CM_PIN_START)   pins_add(PIN_START, cmd, label, icon);
    else if (id == CM_PIN_QL) pins_add(PIN_TASKBAR, cmd, label, icon);
    else return 0;
    taskbar_paint();
    return 1;
}

/* The hook the menu control calls; ids are the same ones the menu was
 * built with. */
static int on_context(int id, int x, int y);

/* The same context menus for the two-column panel, which has no menu
 * control to call the hook for it. */
int startmenu_context(int id, int x, int y) { return on_context(id, x, y); }

static int on_context(int id, int x, int y)
{
    if (id >= SM_PIN_BASE && id < SM_PIN_BASE + PIN_MAX) {
        Pin list[PIN_MAX];
        int n = pins_load(PIN_START, list, PIN_MAX);
        int i = id - SM_PIN_BASE;
        if (i >= n) return 0;
        return pinned_ctx(PIN_START, &list[i], x, y);
    }
    char cmd[512], label[128], icon[256];
    if (programs_entry(id, cmd, sizeof cmd, label, sizeof label,
                       icon, sizeof icon))
        return program_ctx(cmd, label, icon, x, y);

    /* The shell's own accessories can be pinned too. */
    const char *own = NULL, *own_label = NULL;
    switch (id) {
    case SM_EXPLORER: own = "w2kexplorer"; own_label = "Windows Explorer"; break;
    case SM_NOTEPAD:  own = "w2knotepad";  own_label = "Notepad"; break;
    case SM_TASKMGR:  own = "w2ktaskmgr";  own_label = "Task Manager"; break;
    case SM_CALC:     own = "w2kcalc";     own_label = "Calculator"; break;
    case SM_CHARMAP:  own = "w2kcharmap";  own_label = "Character Map"; break;
    case SM_IMAGING:  own = "w2kimage";    own_label = "Imaging"; break;
    case SM_CONTROLPANEL: own = "w2kcontrol"; own_label = "Control Panel"; break;
    }
    if (own) return program_ctx(own, own_label, NULL, x, y);

    /* An entry from the user's own Start Menu folder. */
    const char *sd = startdir_command(id);
    if (sd) return program_ctx(sd, NULL, NULL, x, y);
    return 0;
}

void startmenu_open(void)
{
    if (open_flag) return;

    /* The two-column panel is a different control, not a different menu:
     * it runs its own loop and hands back one of the same command ids. */
    if (w2k_start_panel) {
        open_flag = 1;
        taskbar_paint();
        XFlush(w2k.dpy);
        int bx, by;
        taskbar_start_origin(&bx, &by);
        int id = startpanel_run(bx, by);
        open_flag = 0;
        taskbar_paint();
        if (id) startmenu_dispatch(id);
        return;
    }
    /* Every fresh open starts folded; reopening after a chevron keeps the
     * group the user just expanded. */
    if (!reopening) programs_collapse_all();
    reopening = 0;
    open_flag = 1;
    taskbar_paint();
    XFlush(w2k.dpy);

    /* Accessories, with the System Tools group Windows 2000 keeps inside
     * it -- Character Map lives there, not at the top level. */
    W2kMenu *systools = w2k_menu_new();
    prog_item(systools, SM_CHARMAP, "&Character Map", "w2kcharmap", ICO_CHARMAP);

    W2kMenu *acc = w2k_menu_new();
    w2k_menu_sub(acc, "S&ystem Tools", ICO_ACCESSORIES, systools);
    w2k_menu_sep(acc);
    prog_item(acc, SM_CALC,     "&Calculator", "w2kcalc", ICO_CALC);
    prog_item(acc, SM_NOTEPAD,  "&Notepad",    "w2knotepad", ICO_NOTEPAD);
    prog_item(acc, SM_PAINT,    "&Paint",      "w2kpaint", ICO_PAINT);
    prog_item(acc, SM_SNIP,     "&Snipping Tool", "w2ksnip", ICO_SNIP);
    prog_item(acc, SM_IMAGING,  "&Imaging",    "w2kimage", ICO_PAINT);
    w2k_menu_item(acc, SM_TERMINAL, "Command &Prompt", NULL, ICO_TERMINAL);
    if (!terminal_cmd()) w2k_menu_disable(acc);

    /* Programs: our own accessories on top, then every installed
     * application grouped by category, then Flatpak. */
    W2kMenu *progs = w2k_menu_new();
    w2k_menu_sub(progs, "&Accessories", ICO_ACCESSORIES, acc);
    prog_item(progs, SM_EXPLORER, "&Windows Explorer", "w2kexplorer", ICO_EXPLORER);
    prog_item(progs, SM_TASKMGR,  "&Task Manager", "w2ktaskmgr", ICO_TASKMGR);
    w2k_menu_sep(progs);
    /* The user's own Start Menu tree comes first -- it is the part they
     * can rearrange -- then everything the system has installed. */
    if (startdir_add_programs(progs)) w2k_menu_sep(progs);
    programs_add_groups(progs);

    W2kMenu *docs = w2k_menu_new();
    w2k_menu_item(docs, SM_MYDOCS, "My &Documents", NULL, ICO_MYDOCS);
    int nrec = recent_load();
    if (nrec) {
        w2k_menu_sep(docs);
        for (int i = 0; i < nrec; i++)
            w2k_menu_item(docs, RECENT_BASE + i, recent_label(i), NULL,
                          w2k_icon_by_name(recent_file(i)));
        w2k_menu_sep(docs);
        w2k_menu_item(docs, SM_CLEARDOCS, "&Clear", NULL, ICO_DELETE);
    }

    W2kMenu *settings = w2k_menu_new();
    w2k_menu_item(settings, SM_CONTROLPANEL, "&Control Panel", NULL, ICO_CONTROLPANEL);
    w2k_menu_item(settings, SM_DISPLAY, "&Display Properties...", NULL, ICO_SETTINGS);
    w2k_menu_item(settings, SM_DEFAULTS, "De&fault Programs...", NULL, ICO_PROGRAMS);
    w2k_menu_item(settings, SM_TASKBARPROPS, "&Taskbar and Start Menu...",
                  NULL, ICO_STARTFLAG);
    w2k_menu_item(settings, SM_FOLDEROPTS, "F&older Options...", NULL,
                  ICO_FOLDER_OPEN);

    W2kMenu *search = w2k_menu_new();
    w2k_menu_item(search, SM_SEARCH, "For &Files or Folders...", NULL, ICO_SEARCH);

    char logoff[64];
    snprintf(logoff, sizeof logoff, "L&og Off %s...", user_name());

    W2kMenu *m = w2k_menu_new();
    w2k_menu_set_banner(m, w2k_start_banner_text());

    /* Anything pinned sits above the standard items, as in later Windows.
     * Pin from a program's right-click menu here, or from a task button's;
     * right-clicking a pinned entry edits it. */
    Pin pinned[PIN_MAX];
    int npinned = pins_load(PIN_START, pinned, PIN_MAX);
    for (int i = 0; i < npinned; i++)
        w2k_menu_item(m, SM_PIN_BASE + i, pinned[i].label, NULL,
                      pin_icon(&pinned[i]));
    if (npinned) w2k_menu_sep(m);
    w2k_menu_sub(m, "&Programs",  ICO_PROGRAMS,  progs);
    w2k_menu_sub(m, "&Documents", ICO_DOCUMENTS, docs);
    w2k_menu_sub(m, "&Settings",  ICO_SETTINGS,  settings);
    w2k_menu_sub(m, "Sear&ch",    ICO_SEARCH,    search);
    w2k_menu_item(m, SM_HELP, "&Help", NULL, ICO_HELP);
    w2k_menu_item(m, SM_RUN,  "&Run...", NULL, ICO_RUN);
    w2k_menu_sep(m);
    w2k_menu_item(m, SM_LOGOFF,   logoff, NULL, ICO_LOGOFF);
    w2k_menu_item(m, SM_SHUTDOWN, "Sh&ut Down...", NULL, ICO_SHUTDOWN);

    /* Typing at the Start menu searches, when that is switched on. */
    char typed[2] = { 0, 0 };
    if (w2k_start_search) w2k_menu_typeahead = typed;

    w2k_menu_closed = on_menu_closed;
    w2k_menu_on_context = on_context;
    /* Anchored to the Start button, which is on the primary monitor -- not
     * to the origin of the virtual screen, which may be a different panel. */
    int bx, by;
    taskbar_start_origin(&bx, &by);
    int id = w2k_menu_popup(m, bx, by,
                            w2k_taskbar_edge == TB_TOP ? MPOP_LEFT
                                                       : MPOP_BOTTOMUP);
    w2k_menu_closed = NULL;
    w2k_menu_on_context = NULL;
    w2k_menu_typeahead = NULL;
    w2k_menu_free(m);
    open_flag = 0;
    taskbar_paint();

    if (typed[0]) { wm_search_dialog(typed); return; }
    startmenu_dispatch(id);
}

/* Run one Start menu command. Shared by the classic menu and the
 * two-column panel, which are built from the same ids. */
void startmenu_dispatch(int id)
{
    if (id >= RECENT_BASE && id < RECENT_BASE + recent_count()) {
        char cmd[2048];
        w2k_assoc_command(recent_file(id - RECENT_BASE), cmd, sizeof cmd);
        wm_spawn(cmd);
        return;
    }
    if (id >= STARTDIR_BASE && id < RECENT_BASE) {
        const char *cmd = startdir_command(id);
        if (cmd) wm_spawn(cmd);
        return;
    }
    if (id >= SM_PIN_BASE && id < SM_PIN_BASE + PIN_MAX) {
        Pin list[PIN_MAX];
        int n = pins_load(PIN_START, list, PIN_MAX);
        if (id - SM_PIN_BASE < n) wm_spawn(list[id - SM_PIN_BASE].cmd);
        return;
    }
    /* The chevron at the foot of a folded group: reopen the menu with that
     * group showing everything. */
    int group;
    if (programs_is_chevron(id, &group)) {
        programs_expand(group);
        open_flag = 0;
        reopening = 1;
        startmenu_open();
        return;
    }
    if (programs_run(id, terminal_cmd())) return;
    switch (id) {
    case SM_EXPLORER:     wm_spawn("w2kexplorer"); break;
    case SM_NOTEPAD:      wm_spawn("w2knotepad"); break;
    case SM_TASKMGR:      wm_spawn("w2ktaskmgr"); break;
    case SM_CALC:         wm_spawn("w2kcalc"); break;
    case SM_CHARMAP:      wm_spawn("w2kcharmap"); break;
    case SM_FOLDEROPTS:   wm_spawn("w2kcontrol folders"); break;
    case SM_PAINT:        wm_spawn("w2kpaint"); break;
    case SM_SNIP:         wm_spawn("w2ksnip"); break;
    case SM_TERMINAL:     wm_spawn(terminal_cmd()); break;
    case SM_MYDOCS:       wm_spawn("w2kexplorer ~"); break;
    case SM_MYCOMPUTER:   wm_spawn("w2kexplorer /"); break;
    case SM_CONTROLPANEL: wm_spawn("w2kcontrol"); break;
    case SM_DEFAULTS:     wm_spawn("w2kcontrol defaults"); break;
    case SM_TASKBARPROPS: wm_startmenu_dialog(); break;
    case SM_IMAGING:      wm_spawn("w2kimage"); break;
    case SM_SEARCH:       wm_spawn("w2kexplorer ~"); break;
    case SM_HELP:         wm_help_dialog(); break;
    case SM_DISPLAY:      wm_spawn("w2kdisplay"); break;
    case SM_RUN:          wm_run_dialog(); break;
    case SM_LOGOFF:       wm_logoff_dialog(); break;
    case SM_SHUTDOWN:     wm_shutdown_dialog(); break;
    case SM_CLEARDOCS:    recent_clear(); break;
    case SM_MYPICS:       wm_spawn("w2kexplorer ~/Pictures"); break;
    case SM_MYMUSIC:      wm_spawn("w2kexplorer ~/Music"); break;
    }
}

void startmenu_close(void)
{
    /* The menu owns a pointer grab and its own loop; releasing the grab is
     * what actually tears the chain down. */
    if (open_flag) XUngrabPointer(w2k.dpy, CurrentTime);
    open_flag = 0;
}
