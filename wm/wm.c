/* wm.c -- the window manager proper: root setup, the event loop, EWMH.
 *
 * w2kwm reparents every managed window into a decoration frame, owns the
 * desktop and the taskbar, and is the only process that talks to the X
 * server about window layout. */
#include "wm.h"
#include "w2kui.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

Window wm_check;
int    wa_x, wa_y, wa_w, wa_h;
volatile sig_atomic_t running = 1;
Time wm_last_time;
volatile sig_atomic_t restarting;
static char **saved_argv;

static int other_wm;

/* ------------------------------------------------------------------ *
 * Error handling
 * ------------------------------------------------------------------ */
static int xerror_startup(Display *d, XErrorEvent *e)
{
    (void)d; (void)e;
    other_wm = 1;
    return -1;
}

/* Used while deliberately poking at windows that may already be gone. */
int wm_xerror_ignore(Display *d, XErrorEvent *e)
{
    (void)d; (void)e;
    return 0;
}

/* Windows vanish between our asking about them and our acting on them.
 * Those races are normal; anything else gets logged and shrugged off. */
int wm_xerror(Display *d, XErrorEvent *e)
{
    if (e->error_code == BadWindow || e->error_code == BadDrawable ||
        e->error_code == BadMatch  || e->error_code == BadAccess)
        return 0;
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "w2kwm: X error: %s (request %d.%d)\n", buf,
            e->request_code, e->minor_code);
    return 0;
}

static void on_sigchld(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) ;
}

static void on_sigterm(int sig)
{
    (void)sig;
    running = 0;
    /* Also unwind any dialog that happens to be up, or the shell would
     * sit in its modal loop with nobody watching `running`. */
    w2k_win_abort = 1;
}

/* ------------------------------------------------------------------ *
 * Spawning
 * ------------------------------------------------------------------ */
void wm_spawn(const char *cmd)
{
    if (!cmd || !*cmd) return;
    pid_t pid = fork();
    if (pid != 0) return;

    /* Child: detach from the WM's X connection and session. */
    if (w2k.dpy) close(ConnectionNumber(w2k.dpy));
    setsid();
    signal(SIGCHLD, SIG_DFL);
    execlp("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    fprintf(stderr, "w2kwm: cannot run \"%s\": %s\n", cmd, strerror(errno));
    _exit(127);
}

/* ------------------------------------------------------------------ *
 * ICCCM / EWMH properties
 * ------------------------------------------------------------------ */
void wm_set_state(Window w, long state)
{
    long data[2] = { state, None };
    XChangeProperty(w2k.dpy, w, w2k.a_wm_state, w2k.a_wm_state, 32,
                    PropModeReplace, (unsigned char *)data, 2);
}

void wm_update_client_list(void)
{
    Window list[256];
    int n = 0;
    for (Client *c = clients; c && n < 256; c = c->next)
        list[n++] = c->win;
    XChangeProperty(w2k.dpy, w2k.root, w2k.a_net_client_list, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)list, n);
}

void wm_set_active(Client *c)
{
    Window w = c ? c->win : None;
    XChangeProperty(w2k.dpy, w2k.root, w2k.a_net_active_window, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&w, 1);
}

void wm_workarea_of(const W2kMonitor *m, int *x, int *y, int *w, int *h)
{
    *x = m->x;
    *y = m->y;
    *w = m->w;
    *h = m->h;
    /* Only the primary monitor carries the taskbar, so only it gives up
     * room -- and an auto-hiding bar takes none, which is the point of it.
     * Which side it takes the room from depends on the edge it is on. */
    if (m->primary && !w2k_taskbar_autohide) {
        int t = taskbar_thickness();
        switch (w2k_taskbar_edge) {
        case TB_TOP:   *y += t; *h -= t; break;
        case TB_LEFT:  *x += t; *w -= t; break;
        case TB_RIGHT: *w -= t; break;
        default:       *h -= t; break;
        }
    }
}

void wm_workarea_at(int px, int py, int *x, int *y, int *w, int *h)
{
    wm_workarea_of(w2k_monitor_at(px, py), x, y, w, h);
}

/* The monitor a window is on is the one under its centre -- the same rule
 * Windows uses to decide where a maximised window lands. */
void wm_workarea_of_client(Client *c, int *x, int *y, int *w, int *h)
{
    if (!c) { wm_workarea_of(w2k_monitor_primary(), x, y, w, h); return; }
    wm_workarea_at(c->x + c->w / 2, c->y + c->h / 2, x, y, w, h);
}

void wm_update_workarea(void)
{
    /* _NET_WORKAREA is a single rectangle for the whole desktop, so it can
     * only describe one monitor; the primary is the useful answer. Our own
     * placement code asks wm_workarea_of_client() instead. */
    wm_workarea_of(w2k_monitor_primary(), &wa_x, &wa_y, &wa_w, &wa_h);
    long wa[4] = { wa_x, wa_y, wa_w, wa_h };
    XChangeProperty(w2k.dpy, w2k.root, w2k.a_net_workarea, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)wa, 4);
}

/* A monitor was plugged in, unplugged, moved or resized. */
void wm_layout_changed(void)
{
    wm_update_workarea();
    taskbar_init();                 /* re-homes the bar on the primary */
    desktop_init();
    /* A maximised window is sized to a work area that may have just moved
     * out from under it; re-apply so it fills its monitor again. */
    for (Client *c = clients; c; c = c->next) {
        if (!c->maximized) continue;
        int x, y, w, h;
        wm_workarea_of_client(c, &x, &y, &w, &h);
        int b = client_border(c), cap = client_caption_h(c);
        client_move_resize(c, x + b, y + b + cap, w - 2 * b, h - 2 * b - cap);
        frame_paint(c);
    }
}

static void ewmh_init(void)
{
    wm_check = XCreateSimpleWindow(w2k.dpy, w2k.root, -100, -100, 1, 1, 0, 0, 0);
    XChangeProperty(w2k.dpy, w2k.root, w2k.a_net_supporting_wm_check, XA_WINDOW,
                    32, PropModeReplace, (unsigned char *)&wm_check, 1);
    XChangeProperty(w2k.dpy, wm_check, w2k.a_net_supporting_wm_check, XA_WINDOW,
                    32, PropModeReplace, (unsigned char *)&wm_check, 1);
    w2k_set_wm_name(wm_check, "w2kwm");

    Atom supported[] = {
        w2k.a_net_supported, w2k.a_net_wm_name, w2k.a_net_wm_state,
        w2k.a_net_wm_state_fullscreen, w2k.a_net_wm_state_maxv,
        w2k.a_net_wm_state_maxh, w2k.a_net_wm_state_hidden,
        w2k.a_net_wm_state_skip_taskbar, w2k.a_net_wm_state_above,
        w2k.a_net_wm_window_type, w2k.a_net_wm_wt_dock,
        w2k.a_net_wm_wt_dialog, w2k.a_net_wm_wt_normal, w2k.a_net_wm_wt_menu,
        w2k.a_net_wm_wt_utility, w2k.a_net_wm_wt_splash,
        w2k.a_net_client_list, w2k.a_net_active_window, w2k.a_net_workarea,
        w2k.a_net_close_window, w2k.a_net_moveresize_window,
        w2k.a_net_frame_extents,
        w2k.a_net_current_desktop,
        w2k.a_net_number_of_desktops, w2k.a_net_supporting_wm_check,
    };
    XChangeProperty(w2k.dpy, w2k.root, w2k.a_net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported,
                    sizeof supported / sizeof *supported);

    long one = 1, zero = 0;
    XChangeProperty(w2k.dpy, w2k.root, w2k.a_net_number_of_desktops, XA_CARDINAL,
                    32, PropModeReplace, (unsigned char *)&one, 1);
    XChangeProperty(w2k.dpy, w2k.root, w2k.a_net_current_desktop, XA_CARDINAL,
                    32, PropModeReplace, (unsigned char *)&zero, 1);
}

/* ------------------------------------------------------------------ *
 * Session end
 * ------------------------------------------------------------------ */
static int  logging_out;          /* 1 + what, while waiting for windows to close */
static long logout_deadline;

void wm_logout(int what)
{
    for (Client *c = clients; c; c = c->next) client_close(c);
    XFlush(w2k.dpy);
    logging_out = 1 + what;
    /* Five seconds of politeness before asking about the stragglers:
     * twenty was long enough for the user to conclude nothing happened. */
    logout_deadline = w2k_now_ms() + 5000;
    if (!clients) running = 0;
}

void wm_logout_check(void)
{
    if (logging_out && !clients) running = 0;
}

/* Called from the main loop: finish the log-off once the last window has
 * gone. A program that will not close -- a browser asking about its tabs,
 * an editor with unsaved work -- used to abandon the whole log-off after
 * twenty seconds without saying anything, which looked exactly like the
 * Log Off button being broken. Windows names the program and offers to
 * end it; so does this. */
static void logout_poll(void)
{
    if (!logging_out) return;
    if (!clients) { running = 0; return; }
    if (w2k_now_ms() <= logout_deadline) return;

    char list[512] = "";
    int n = 0;
    for (Client *c = clients; c && n < 6; c = c->next) {
        if (w2k_win_owns(c->win)) continue;      /* one of our own dialogs */
        size_t len = strlen(list);
        snprintf(list + len, sizeof list - len, "    %.60s\n",
                 c->name ? c->name : "(untitled)");
        n++;
    }
    if (!n) { running = 0; return; }              /* only our own left */

    char msg[900];
    snprintf(msg, sizeof msg,
             "These programs are still running:\n\n%s\n"
             "End them now and log off?", list);
    /* The dialog is modal, so windows that close while it is up are gone
     * by the time the answer comes back -- check again before killing. */
    int yes = w2k_msgbox(NULL, "Log Off Windows", msg,
                         MB_YESNO | MB_ICONWARNING) == ID_YES;
    if (!clients) { running = 0; return; }
    if (!yes) { logging_out = 0; return; }        /* stay logged in */

    for (Client *c = clients; c; c = c->next)
        if (!w2k_win_owns(c->win)) XKillClient(w2k.dpy, c->win);
    XFlush(w2k.dpy);
    running = 0;
}

/* ------------------------------------------------------------------ *
 * Event handling
 * ------------------------------------------------------------------ */
static void handle_maprequest(XMapRequestEvent *e)
{
    Client *c = client_find(e->window);
    if (c) { client_restore(c); return; }
    client_manage(e->window, 1);
}

static void handle_configurerequest(XConfigureRequestEvent *e)
{
    Client *c = client_find(e->window);
    if (!c) {
        /* Unmanaged (or not yet managed): grant it verbatim. */
        XWindowChanges wc = {
            .x = e->x, .y = e->y, .width = e->width, .height = e->height,
            .border_width = e->border_width, .sibling = e->above,
            .stack_mode = e->detail
        };
        XConfigureWindow(w2k.dpy, e->window, e->value_mask, &wc);
        return;
    }
    if (c->maximized || c->fullscreen) {
        /* Refuse the geometry but re-affirm the current one, or the client
         * will sit there believing it got what it asked for. */
        client_move_resize(c, c->x, c->y, c->w, c->h);
        return;
    }
    int x = c->x, y = c->y, w = c->w, h = c->h;
    int b = c->static_gravity ? 0 : client_border(c);
    int cap = c->static_gravity ? 0 : client_caption_h(c);
    if (e->value_mask & CWX)      x = e->x + b;
    if (e->value_mask & CWY)      y = e->y + b + cap;
    if (e->value_mask & CWWidth)  w = e->width;
    if (e->value_mask & CWHeight) h = e->height;
    client_constrain(c, &w, &h);
    client_move_resize(c, x, y, w, h);
}

static void handle_clientmessage(XClientMessageEvent *e)
{
    Client *c = client_find(e->window);

    if (e->message_type == w2k.a_net_close_window && c) {
        client_close(c);
        return;
    }
    if (e->message_type == w2k.a_net_active_window && c) {
        client_restore(c);
        return;
    }
    /* _NET_WM_MOVERESIZE: an application asking us to take over a drag it
     * started -- the size grip in the corner of a status bar, which is
     * inside the client area where we never see the button press. */
    if (e->message_type == w2k.a_net_wm_moveresize && c) {
        static const int dir_ht[] = {
            HT_TOPLEFT, HT_TOP, HT_TOPRIGHT, HT_RIGHT,
            HT_BOTTOMRIGHT, HT_BOTTOM, HT_BOTTOMLEFT, HT_LEFT
        };
        long dir = e->data.l[2];
        XButtonEvent be = {
            .type = ButtonPress, .display = w2k.dpy, .window = c->frame,
            .root = w2k.root, .x_root = (int)e->data.l[0],
            .y_root = (int)e->data.l[1], .button = Button1,
            .time = CurrentTime
        };
        client_raise(c);
        client_focus(c);
        if (dir >= 0 && dir <= 7) do_resize(c, &be, dir_ht[dir]);
        else if (dir == 8)        do_move(c, &be);      /* _MOVE */
        return;
    }
    /* _NET_MOVERESIZE_WINDOW: a program placing a window that is not its
     * own -- Task Manager's Tile and Cascade. The low byte of data.l[0]
     * is the gravity (ignored: ours is always north-west) and bits 8..11
     * say which of x, y, width and height were supplied. */
    if (e->message_type == w2k.a_net_moveresize_window && c) {
        long flags = e->data.l[0];
        int x = (flags & (1 << 8))  ? (int)e->data.l[1] : c->x;
        int y = (flags & (1 << 9))  ? (int)e->data.l[2] : c->y;
        int cw = (flags & (1 << 10)) ? (int)e->data.l[3] : c->w;
        int ch = (flags & (1 << 11)) ? (int)e->data.l[4] : c->h;
        if (c->maximized) client_maximize(c, 0);
        client_restore(c);
        client_move_resize(c, x, y, cw, ch);
        return;
    }
    if (e->message_type == w2k.a_wm_change_state && c) {
        if (e->data.l[0] == IconicState) client_minimize(c);
        return;
    }
    if (e->message_type == w2k.a_net_wm_state && c) {
        /* data.l[0]: 0 remove, 1 add, 2 toggle */
        long act = e->data.l[0];
        int max_done = 0;
        for (int i = 1; i <= 2; i++) {
            Atom a = e->data.l[i];
            if (!a) continue;
            if (a == w2k.a_net_wm_state_maxv || a == w2k.a_net_wm_state_maxh) {
                /* Both halves usually come together; a toggle must not
                 * flip twice. */
                if (max_done) continue;
                max_done = 1;
                int on = (act == 2) ? !c->maximized : (act == 1);
                client_maximize(c, on);
            } else if (a == w2k.a_net_wm_state_fullscreen) {
                int on = (act == 2) ? !c->fullscreen : (act == 1);
                client_fullscreen(c, on);
            } else if (a == w2k.a_net_wm_state_above) {
                c->above = (act == 2) ? !c->above : (act == 1);
                clients_restack();
            } else if (a == w2k.a_net_wm_state_hidden) {
                if (act == 1) client_minimize(c);
                else          client_restore(c);
            }
        }
        client_publish_state(c);
        return;
    }
    /* Our own shell apps ask the WM for things through _W2K_COMMAND. */
    if (e->message_type == w2k.a_w2k_command) {
        switch (e->data.l[0]) {
        case 1: startmenu_open(); break;
        case 2: wm_logout(0); break;
        case 3: wm_startmenu_dialog(); break;    /* Control Panel applet */
        case 5: wm_search_dialog(""); break;     /* Explorer's Search button */
        case 6: restarting = 1; break;           /* w2kwm --restart */
        case 4: {
            /* A balloon: title and text sit in _W2K_NOTIFY on the root,
             * NUL-separated, so the message itself stays 32 bytes. */
            Atom type;
            int fmt;
            unsigned long n, after;
            unsigned char *data = NULL;
            if (XGetWindowProperty(w2k.dpy, w2k.root, w2k.a_w2k_notify, 0,
                                   1024, False, AnyPropertyType, &type, &fmt,
                                   &n, &after, &data) == Success && data) {
                const char *title = (const char *)data;
                size_t tl = strnlen(title, n);
                const char *text = (tl + 1 < n) ? title + tl + 1 : "";
                balloon_show(title, text);
                XFree(data);
            }
            break;
        }
        }
    }
}

/* Drops land on one of the shell's two surfaces; route by window. */
static int shell_dnd_accept(Window w, int x, int y)
{
    if (w == taskbar_window()) return taskbar_dnd_accept(x, y);
    return w == desktop_window();
}

static void shell_dnd_drop(Window w, int x, int y, const char *uris, int move)
{
    if (w == taskbar_window())      taskbar_dnd_drop(x, y, uris);
    else if (w == desktop_window()) desktop_dnd_drop(x, y, uris, move);
}

void wm_handle_event(XEvent *e)
{
    /* Alt reveals the mnemonic underlines on the shell's own menus, the
     * way it does inside an application. */
    if (e->type == KeyPress && !w2k_accel_shown) {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Alt_L || ks == XK_Alt_R || (e->xkey.state & Mod1Mask)) {
            w2k_accel_show();
            taskbar_paint();
        }
    }
    if (balloon_event(e)) return;
    if (w2k_dnd_event(e)) return;       /* drag and drop protocol */
    if (tray_event(e)) return;          /* docked notification icons */
    if (taskbar_event(e)) return;
    if (desktop_event(e)) return;

    Client *c;
    switch (e->type) {
    case MapRequest:
        handle_maprequest(&e->xmaprequest);
        break;

    case ConfigureRequest:
        handle_configurerequest(&e->xconfigurerequest);
        break;

    case UnmapNotify:
        /* Only a real (non-synthetic) unmap of a managed window withdraws it;
         * we unmap frames ourselves when minimising. */
        c = client_find(e->xunmap.window);
        if (!c || e->xunmap.event == w2k.root) break;
        if (c->ignore_unmap > 0) { c->ignore_unmap--; break; }
        /* The client window itself is never unmapped here (minimising
         * hides the frame), so any real unmap is the app withdrawing --
         * an iconified window included. */
        client_unmanage(c, 0);
        break;

    case DestroyNotify:
        c = client_find(e->xdestroywindow.window);
        if (c) client_unmanage(c, 1);
        break;

    case Expose:
        if (e->xexpose.count) break;
        c = client_find_frame(e->xexpose.window);
        if (c) frame_paint(c);
        break;

    case ButtonPress:
        wm_last_time = e->xbutton.time;
        c = client_find_frame(e->xbutton.window);
        if (c) { frame_button_press(c, &e->xbutton); break; }
        /* A click inside an unfocused client: focus it, then replay so the
         * application still receives the click. */
        c = client_find(e->xbutton.window);
        if (c) {
            client_raise(c);
            client_focus(c);
            XAllowEvents(w2k.dpy, ReplayPointer, e->xbutton.time);
            XSync(w2k.dpy, False);
        }
        break;

    case ButtonRelease:
        c = client_find_frame(e->xbutton.window);
        if (c) frame_button_release(c, &e->xbutton);
        break;

    case MotionNotify:
        wm_last_time = e->xmotion.time;
        c = client_find_frame(e->xmotion.window);
        if (c) frame_motion(c, &e->xmotion);
        break;

    case LeaveNotify:
        c = client_find_frame(e->xcrossing.window);
        if (c) frame_leave(c);
        break;

    case PropertyNotify:
        if (e->xproperty.window == w2k.root) {
            if (e->xproperty.atom == w2k.a_w2k_scheme) {
                w2k_scheme_load(NULL);
                taskbar_skins_reload();
                startpanel_skins_reload();
                /* Effects can change with the scheme: the pointer shadow
                 * is baked into the cursor images, so they are rebuilt. */
                w2k_font_reload();
                w2k_input_apply();      /* mouse and keyboard settings */
                /* The theme may have changed with the scheme: the bar,
                 * the Start button and the menu style all follow it. */
                taskbar_init();
                w2k_cursors_init();
                XDefineCursor(w2k.dpy, w2k.root, w2k.cur_arrow);
                desktop_reload();
                for (Client *k = clients; k; k = k->next) {
                    XSetWindowBackground(w2k.dpy, k->frame, w2k.col[C_FACE]);
                    /* ForceDecorations lives in the scheme file too, so a
                     * window may have just gained or lost its frame. */
                    int was = k->decorate;
                    client_update_type(k);
                    if (k->decorate != was)
                        client_move_resize(k, k->x, k->y, k->w, k->h);
                    frame_paint(k);
                }
                taskbar_paint();
            }
            break;
        }
        c = client_find(e->xproperty.window);
        if (!c) break;
        if (e->xproperty.atom == XA_WM_NAME ||
            e->xproperty.atom == w2k.a_net_wm_name) {
            client_update_name(c);
            frame_paint(c);
            taskbar_paint();
        } else if (e->xproperty.atom == XA_WM_NORMAL_HINTS) {
            client_update_hints(c);
        }
        break;

    case ClientMessage:
        handle_clientmessage(&e->xclient);
        break;

    case KeyPress:
        wm_last_time = e->xkey.time;
        handle_key(&e->xkey);
        break;

    case KeyRelease:
        wm_last_time = e->xkey.time;
        handle_key_release(&e->xkey);
        break;

    case MappingNotify:
        XRefreshKeyboardMapping(&e->xmapping);
        if (e->xmapping.request == MappingKeyboard) grab_keys();
        break;

    case ConfigureNotify:
        if (e->xconfigure.window == w2k.root) {
            w2k.sw = e->xconfigure.width;
            w2k.sh = e->xconfigure.height;
            w2k_monitors_refresh();
            wm_layout_changed();
        }
        break;

    default:
        /* RandR reports monitor changes that never show up as a root
         * ConfigureNotify -- a second screen switched on at the same total
         * size, or the primary moving to another output. */
        if (w2k_monitors_event(e)) wm_layout_changed();
        break;
    }
}

/* Our own dialogs: nothing redirects their map, so they are handed to
 * client_manage() directly. Everything else the shell puts on screen --
 * the taskbar, menus, tooltips -- is override-redirect and never comes
 * through here. */
static void manage_own_window(Window w)
{
    if (!client_find(w)) client_manage(w, 1);
}

/* Adopt whatever is already on screen when we start. */
/* Start over with the same windows: every client goes back to the root,
 * mapped, and the new process finds them there and frames them again. The
 * process keeps its pid, so w2k-session is none the wiser. */
static void wm_restart(void)
{
    for (Client *c = clients; c; c = c->next)
        if (c->minimized) { c->minimized = 0; XMapWindow(w2k.dpy, c->frame); }
    XSetInputFocus(w2k.dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    tray_fini();
    while (clients) client_unmanage(clients, 0);
    XDestroyWindow(w2k.dpy, wm_check);
    XSync(w2k.dpy, False);
    w2k_fini();
    execv("/proc/self/exe", saved_argv);
    fprintf(stderr, "w2kwm: cannot restart: %s\n", strerror(errno));
    exit(1);
}

static void on_sighup(int s) { (void)s; restarting = 1; }

static void scan_existing(void)
{
    Window root_ret, parent_ret, *kids = NULL;
    unsigned n = 0;
    if (!XQueryTree(w2k.dpy, w2k.root, &root_ret, &parent_ret, &kids, &n))
        return;
    for (unsigned i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(w2k.dpy, kids[i], &wa)) continue;
        if (wa.override_redirect || wa.map_state != IsViewable) continue;
        client_manage(kids[i], 0);
    }
    if (kids) XFree(kids);
}

int main(int argc, char **argv)
{
    saved_argv = argv;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            puts("w2kwm 1.0 -- Windows 2000 window manager for X11");
            return 0;
        }
        if (!strcmp(argv[i], "--restart")) {
            /* Ask the running shell to start itself again, keeping every
             * window: the way to pick up a new build without logging off. */
            if (w2k_init("w2kwm") < 0) return 1;
            XEvent ev = { 0 };
            ev.xclient.type = ClientMessage;
            ev.xclient.window = w2k.root;
            ev.xclient.message_type = w2k.a_w2k_command;
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = 6;
            XSendEvent(w2k.dpy, w2k.root, False, SubstructureNotifyMask, &ev);
            XSync(w2k.dpy, False);
            return 0;
        }
        if (!strcmp(argv[i], "--icons")) {
            puts("Icon slugs (drop <slug>.ico into ~/.w2k/icons to override):");
            for (int k = 0; k < N_ICONS; k++)
                if (w2k_icon_slug(k)) printf("  %s\n", w2k_icon_slug(k));
            return 0;
        }
    }
    if (w2k_init("w2kwm") < 0) return 1;

    /* The Start menu's folder tree exists from the first run, so there is
     * somewhere to put shortcuts before anyone opens the menu. */
    startdir_ensure();
    w2k_dnd_on_drop = shell_dnd_drop;
    w2k_dnd_will_accept = shell_dnd_accept;

    /* W2K_RENDER (see w2k_win_show) captures a dialog to a file and exits.
     * Done before the window manager selection is claimed, so the shell's
     * own dialogs can be looked at while another WM is running. */
    if (getenv("W2K_RENDER")) {
        const char *which = getenv("W2K_RENDER_DIALOG");
        if (which && !strcmp(which, "shutdown"))       wm_shutdown_dialog();
        else if (which && !strcmp(which, "startmenu")) wm_startmenu_dialog();
        else if (which && !strcmp(which, "search"))    wm_search_dialog("f");
        else if (which && !strcmp(which, "frame")) {
            int rw = getenv("W2K_RENDER_W") ? atoi(getenv("W2K_RENDER_W")) : 360;
            int rh = getenv("W2K_RENDER_H") ? atoi(getenv("W2K_RENDER_H")) : 120;
            frame_render(getenv("W2K_RENDER"), rw, rh,
                         !getenv("W2K_RENDER_INACTIVE"),
                         getenv("W2K_RENDER_MAXIMIZED") != NULL);
        }
        else if (which && !strcmp(which, "taskbar")) {
            int rw = getenv("W2K_RENDER_W") ? atoi(getenv("W2K_RENDER_W")) : 1024;
            taskbar_render(getenv("W2K_RENDER"), rw);
        }
        else if (which && !strcmp(which, "startpanel")) {
            startpanel_render(getenv("W2K_RENDER"));
        }
        else if (which && !strcmp(which, "changeicon")) {
            char icon[256];
            wm_change_icon_dialog(icon, sizeof icon);
        }
        else                                           wm_run_dialog();
        return 0;
    }

    /* Folders open in the file manager Default Programs names -- Explorer,
     * unless changed -- for other programs too, through the XDG default. */
    w2k_assoc_apply_folder_default();

    /* The monitor arrangement Display Properties saved comes back before
     * the desktop is laid out, so the taskbar lands on the right screen. */
    if (w2k_monitors_apply_saved()) {
        XSync(w2k.dpy, False);
        w2k_monitors_refresh();
    }

    /* Claim the substructure-redirect selection; only one WM may hold it. */
    other_wm = 0;
    XSetErrorHandler(xerror_startup);
    XSelectInput(w2k.dpy, w2k.root,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                 StructureNotifyMask | PropertyChangeMask |
                 ButtonPressMask | KeyPressMask);
    XSync(w2k.dpy, False);
    if (other_wm) {
        fprintf(stderr, "w2kwm: another window manager is already running\n");
        return 1;
    }
    XSetErrorHandler(wm_xerror);

    struct sigaction sa = { .sa_handler = on_sigchld, .sa_flags = SA_RESTART | SA_NOCLDSTOP };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    /* These two only set a flag, which the main loop notices when it comes
     * out of select(). Linux returns EINTR from select whether or not
     * SA_RESTART is set, so this is not a fix for anything -- but restart
     * semantics are not what is wanted here, so do not ask for them. */
    sa.sa_handler = on_sigterm;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_sighup;              /* kill -HUP: restart in place */
    sigaction(SIGHUP, &sa, NULL);

    XDefineCursor(w2k.dpy, w2k.root, w2k.cur_arrow);
    ewmh_init();
    wm_update_workarea();
    w2k_input_apply();              /* mouse, keyboard, bell */
    desktop_init();
    taskbar_init();
    grab_keys();

    /* Menus run their own modal loop; let them keep the shell painted. */
    w2k_menu_foreign_event = wm_handle_event;
    w2k_win_foreign_event = wm_handle_event;
    w2k_win_mapped = manage_own_window;    /* frame our own dialogs */

    scan_existing();
    /* Publish the list even when empty, so a stale one from a previous
     * session cannot mislead panels and task managers. */
    wm_update_client_list();
    wm_set_active(NULL);
    taskbar_sync();
    XSync(w2k.dpy, False);

    startdir_run_startup();

    /* Anything on the command line after "--" is autostarted. */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--") ) {
            for (int j = i + 1; j < argc; j++) wm_spawn(argv[j]);
            break;
        }

    int fd = ConnectionNumber(w2k.dpy);
    while (running) {
        while (XPending(w2k.dpy)) {
            XEvent e;
            XNextEvent(w2k.dpy, &e);
            wm_handle_event(&e);
        }
        if (!running) break;
        if (restarting) wm_restart();

        /* Sleep until either X has something to say or something on the
         * shell is due: the clock at the next minute, a tooltip half a
         * second after the pointer settled, a balloon's timeout. An idle
         * desktop with the clock showing therefore wakes once a minute
         * rather than four times a second. */
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        int wait = taskbar_next_tick_ms();
        int d = desktop_next_tick_ms();
        if (d < wait) wait = d;
        int b = balloon_next_tick_ms();
        if (b >= 0 && b < wait) wait = b;
        /* A log-off is waiting on windows to close; one that refuses
         * sends no events, so keep checking the deadline. */
        if (logging_out && wait > 200) wait = 200;
        if (wait < 10) wait = 10;
        if (wait > 60000) wait = 60000;
        struct timeval tv = { .tv_sec = wait / 1000,
                              .tv_usec = (wait % 1000) * 1000 };
        int rc = select(fd + 1, &r, NULL, NULL, &tv);
        if (rc < 0 && errno != EINTR) break;
        taskbar_tick();
        taskbar_hover_tick();
        balloon_tick();
        desktop_bin_tick();
        desktop_hover_tick();
        logout_poll();
    }

    XSetInputFocus(w2k.dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    tray_fini();                        /* hand the icons back to the root */
    while (clients) client_unmanage(clients, 0);
    XDestroyWindow(w2k.dpy, wm_check);
    w2k_fini();
    /* 0 = log off, 10 = shut down, 11 = restart (see w2k-session). */
    return logging_out == 2 ? 10 : logging_out == 3 ? 11 : 0;
}
