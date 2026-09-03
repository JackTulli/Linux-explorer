/* w2klogon.c -- "Log On to Windows": the Windows 2000 logon screen, as a
 * LightDM greeter.
 *
 * LightDM starts it on its own X server; it paints the desktop blue, puts
 * up the logon dialog with the Windows 2000 Professional banner, and
 * talks to the daemon through liblightdm-gobject: the user name goes in,
 * the daemon asks for the password, and a good answer starts the
 * w2k-session. Shutdown... offers the Shut Down Windows dialog. Built
 * without the library (HAVE_LIGHTDM undefined) it is the picture alone,
 * for looking at with W2K_RENDER. */
#include "w2kui.h"
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_LIGHTDM
#include <lightdm.h>
#endif

/* Measured off a 800x600 screenshot: the dialog's frame is 415 by 254 at
 * (191,116); the banner fills its top 92 rows under the caption. */
#define DLG_W   415
#define DLG_H   254
#define CAP_H    18
#define BANNER_H 92

typedef struct {
    W2kWin  *win;                /* the whole screen */
    W2kEdit *user, *pass;
    W2kRect  dlg, ok, cancel, shutdown, options, dialup;
    int      down, options_open, busy;
    char     message[200];       /* the daemon's last word, if any */
    W2kSkin *banner;
#ifdef HAVE_LIGHTDM
    LightDMGreeter *greeter;
#endif
} Logon;

static Logon lg;

/* ------------------------------------------------------------------ *
 * Painting
 * ------------------------------------------------------------------ */
static void paint(W2kWin *w, Drawable d)
{
    int fh = w2k_font_height(F_UI);
    /* The desktop behind: Windows 2000's blue. */
    XSetForeground(w2k.dpy, w2k.gc, w2k_rgb(58, 110, 165));
    XFillRectangle(w2k.dpy, d, w2k.gc, 0, 0, (unsigned)w->w, (unsigned)w->h);

    W2kRect r = lg.dlg;
    /* A dialog frame, drawn here since no window manager runs on the
     * greeter's server: raised edge, then the caption. */
    w2k_fill(d, r.x, r.y, r.w, r.h, C_FACE);
    w2k_edge(d, r.x, r.y, r.w, r.h, EDGE_RAISED, BF_RECT);
    w2k_gradient(d, r.x + 3, r.y + 3, r.w - 6, CAP_H, C_ACTIVETITLE, C_ACTIVETITLE2);
    w2k_text(d, F_UI_BOLD, r.x + 3 + 5, r.y + 3 + (CAP_H - w2k_font_height(F_UI_BOLD)) / 2,
             "Log On to Windows", C_TITLETEXT);

    /* The banner: white with the logo, a dark line under it. */
    int by = r.y + 3 + CAP_H;
    w2k_fill(d, r.x + 3, by, r.w - 6, BANNER_H, C_WINDOW);
    if (lg.banner)
        w2k_skin_draw(d, lg.banner, r.x + 3, by, 0, 0, w2k_skin_w(lg.banner), w2k_skin_h(lg.banner));
    else {
        w2k_bigicon_draw(d, r.x + 50, by + 30, ICO_STARTFLAG);
        w2k_text(d, F_UI_BOLD, r.x + 110, by + 34, "Windows 2000 Professional", C_TEXT);
    }
    w2k_hline(d, r.x + 3, by + BANNER_H, r.w - 6, C_SHADOW);
    w2k_hline(d, r.x + 3, by + BANNER_H + 1, r.w - 6, C_HILIGHT);

    w2k_text_mnemonic(d, F_UI, lg.user->r.x - 76, lg.user->r.y + (21 - fh) / 2, "&User name:", C_TEXT, 1);
    w2k_edit_draw(d, lg.user);
    w2k_text_mnemonic(d, F_UI, lg.pass->r.x - 76, lg.pass->r.y + (21 - fh) / 2, "&Password:", C_TEXT, 1);
    w2k_edit_draw(d, lg.pass);
    if (lg.options_open)
        w2k_draw_checkbox(d, lg.dialup.x, lg.dialup.y, "&Log on using dial-up connection", 0, 0, 1);
    if (lg.message[0])
        w2k_text(d, F_UI, lg.dialup.x, lg.dialup.y + 22, lg.message, C_TEXT);

    w2k_draw_pushbutton(d, &lg.ok, "OK", BS_DEFAULT | (lg.down == 1 ? BS_PRESSED : 0) | (lg.busy ? BS_DISABLED : 0));
    w2k_draw_pushbutton(d, &lg.cancel, "Cancel", BS_DISABLED);
    w2k_draw_pushbutton(d, &lg.shutdown, "&Shutdown...", lg.down == 3 ? BS_PRESSED : 0);
    w2k_draw_pushbutton(d, &lg.options, lg.options_open ? "&Options <<" : "&Options >>",
                        lg.down == 4 ? BS_PRESSED : 0);
}

static void layout(W2kWin *w)
{
    /* Centred on the primary monitor, a little above the middle, where
     * Windows puts it; the window itself spans every monitor. */
    const W2kMonitor *m = w2k_monitor_primary();
    int mx = m ? m->x : 0, my = m ? m->y : 0, mw = m ? m->w : w->w, mh = m ? m->h : w->h;
    lg.dlg = (W2kRect){ mx + (mw - DLG_W) / 2, my + (mh - DLG_H) / 2 - 50, DLG_W, DLG_H };
    W2kRect r = lg.dlg;
    int body = r.y + 3 + CAP_H + BANNER_H + 2;
    lg.user->r = (W2kRect){ r.x + 87, body + 14, 242, 21 };
    lg.pass->r = (W2kRect){ r.x + 87, body + 42, 242, 21 };
    lg.dialup = (W2kRect){ r.x + 87, body + 72, 242, 18 };
    int bx = r.x + 87, byy = r.y + r.h - 12 - 23;
    lg.ok       = (W2kRect){ bx, byy, 75, 23 };
    lg.cancel   = (W2kRect){ bx + 81, byy, 75, 23 };
    lg.shutdown = (W2kRect){ bx + 162, byy, 75, 23 };
    lg.options  = (W2kRect){ bx + 243, byy, 75, 23 };
}

/* ------------------------------------------------------------------ *
 * LightDM
 * ------------------------------------------------------------------ */
static void set_message(const char *m)
{
    snprintf(lg.message, sizeof lg.message, "%s", m ? m : "");
    w2k_win_dirty(lg.win);
}

static void logon_failed(void)
{
    lg.busy = 0;
    w2k_edit_set(lg.pass, "");
    lg.pass->focused = 1;
    lg.user->focused = 0;
    w2k_win_dirty(lg.win);
    w2k_msgbox(lg.win, "Logon Message",
               "The system could not log you on. Make sure your User name and "
               "domain are correct, then type your password again. Letters in "
               "passwords must be typed using the correct case.",
               MB_OK | MB_ICONWARNING);
}

#ifdef HAVE_LIGHTDM
static void on_prompt(LightDMGreeter *g, const gchar *text, LightDMPromptType type, gpointer u)
{
    (void)text; (void)u;
    /* The password is already typed: answer straight away. */
    GError *err = NULL;
    if (type == LIGHTDM_PROMPT_TYPE_SECRET)
        lightdm_greeter_respond(g, w2k_edit_text(lg.pass), &err);
    else
        lightdm_greeter_respond(g, w2k_edit_text(lg.user), &err);
    if (err) g_error_free(err);
}

static void on_message(LightDMGreeter *g, const gchar *text, LightDMMessageType type, gpointer u)
{
    (void)g; (void)type; (void)u;
    set_message(text);
}

static void on_complete(LightDMGreeter *g, gpointer u)
{
    (void)u;
    if (lightdm_greeter_get_is_authenticated(g)) {
        GError *err = NULL;
        if (!lightdm_greeter_start_session_sync(g, "w2k-session", &err)) {
            set_message(err ? err->message : "The session could not be started.");
            if (err) g_error_free(err);
            lg.busy = 0;
        }
    } else
        logon_failed();
}

/* Our loop is select() over X; GLib's runs in slices from a timer. */
static void glib_tick(void *u)
{
    (void)u;
    while (g_main_context_iteration(NULL, FALSE)) ;
}
#endif

static void do_logon(void)
{
    const char *user = w2k_edit_text(lg.user);
    if (!*user || lg.busy) return;
    lg.busy = 1;
    set_message("");
#ifdef HAVE_LIGHTDM
    if (lg.greeter) {
        GError *err = NULL;
        if (!lightdm_greeter_authenticate(lg.greeter, user, &err)) {
            set_message(err ? err->message : "Could not start authentication.");
            if (err) g_error_free(err);
            lg.busy = 0;
        }
        return;
    }
#endif
    logon_failed();                     /* no daemon: the picture only */
}

/* ------------------------------------------------------------------ *
 * Shut Down Windows
 * ------------------------------------------------------------------ */
typedef struct { W2kCombo *what; W2kRect ok, cancel; int down; } ShutDlg;

static void shut_paint(W2kWin *w, Drawable d)
{
    ShutDlg *s = w->user;
    int fh = w2k_font_height(F_UI);
    w2k_bigicon_draw(d, 14, 14, ICO_SHUTDOWN);
    w2k_text(d, F_UI, 60, 14, "What do you want the computer to do?", C_TEXT);
    w2k_combo_draw(d, s->what);
    w2k_draw_pushbutton(d, &s->ok, "OK", BS_DEFAULT | (s->down == 1 ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &s->cancel, "Cancel", s->down == 2 ? BS_PRESSED : 0);
    (void)fh;
}

static int shut_event(W2kWin *w, XEvent *e)
{
    ShutDlg *s = w->user;
    switch (e->type) {
    case ButtonPress:
        if (w2k_combo_press(s->what, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&s->ok, e->xbutton.x, e->xbutton.y)) s->down = 1;
        else if (w2k_rect_hit(&s->cancel, e->xbutton.x, e->xbutton.y)) s->down = 2;
        w2k_win_dirty(w);
        return 1;
    case ButtonRelease: {
        int b = s->down;
        s->down = 0;
        if (b == 1 && w2k_rect_hit(&s->ok, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_OK);
        else if (b == 2 && w2k_rect_hit(&s->cancel, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) w2k_win_close(w, ID_CANCEL);
        if (ks == XK_Return) w2k_win_close(w, ID_OK);
        return 1;
    }
    }
    return 0;
}

static void do_shutdown_dialog(void)
{
    ShutDlg s = { 0 };
    W2kWin *w = w2k_win_new("Shut Down Windows", "w2klogon", 400, 150, 0);
    s.what = w2k_combo_new(0);
    w2k_combo_add(s.what, "Shut down");
    w2k_combo_add(s.what, "Restart");
    s.what->r = (W2kRect){ 60, 40, 300, 21 };
    s.ok = (W2kRect){ 400 - 12 - 75 * 2 - 6, 150 - 12 - 23, 75, 23 };
    s.cancel = (W2kRect){ 400 - 12 - 75, 150 - 12 - 23, 75, 23 };
    w->user = &s;
    w->paint = shut_paint;
    w->event = shut_event;
    w2k_win_center(w, lg.win);
    int rc = w2k_win_modal(w);
    int what = s.what->sel;
    w2k_combo_free(s.what);
    if (rc != ID_OK) return;
#ifdef HAVE_LIGHTDM
    GError *err = NULL;
    if (what == 1) lightdm_restart(&err); else lightdm_shutdown(&err);
    if (err) { set_message(err->message); g_error_free(err); }
#else
    (void)what;
#endif
}

/* ------------------------------------------------------------------ *
 * Events
 * ------------------------------------------------------------------ */
static int event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y;
        if (w2k_edit_press(lg.user, &e->xbutton)) { lg.user->focused = 1; lg.pass->focused = 0; w2k_win_dirty(w); return 1; }
        if (w2k_edit_press(lg.pass, &e->xbutton)) { lg.pass->focused = 1; lg.user->focused = 0; w2k_win_dirty(w); return 1; }
        if (w2k_rect_hit(&lg.ok, x, y) && !lg.busy) lg.down = 1;
        else if (w2k_rect_hit(&lg.shutdown, x, y)) lg.down = 3;
        else if (w2k_rect_hit(&lg.options, x, y)) lg.down = 4;
        w2k_win_dirty(w);
        return 1;
    }
    case ButtonRelease: {
        int x = e->xbutton.x, y = e->xbutton.y, b = lg.down;
        lg.down = 0;
        w2k_edit_release(lg.user);
        w2k_edit_release(lg.pass);
        if (b == 1 && w2k_rect_hit(&lg.ok, x, y)) do_logon();
        else if (b == 3 && w2k_rect_hit(&lg.shutdown, x, y)) do_shutdown_dialog();
        else if (b == 4 && w2k_rect_hit(&lg.options, x, y)) lg.options_open = !lg.options_open;
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (lg.user->focused) w2k_edit_motion(lg.user, &e->xmotion);
        if (lg.pass->focused) w2k_edit_motion(lg.pass, &e->xmotion);
        return 1;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Return || ks == XK_KP_Enter) { do_logon(); return 1; }
        if (ks == XK_Tab) {
            int u = lg.user->focused;
            lg.user->focused = !u;
            lg.pass->focused = u;
            w2k_win_dirty(w);
            return 1;
        }
        if (lg.user->focused && w2k_edit_key(lg.user, &e->xkey)) w2k_win_dirty(w);
        else if (lg.pass->focused && w2k_edit_key(lg.pass, &e->xkey)) w2k_win_dirty(w);
        return 1;
    }
    }
    return 0;
}

static void blink(void *u) { w2k_edit_blink(u); }

int main(void)
{
    if (w2k_init("w2klogon") < 0) return 1;

    /* The screen is the window: as wide as the display, no frame. */
    lg.win = w2k_win_new("Log On to Windows", "w2klogon", w2k.sw, w2k.sh, 0);
    lg.win->paint = paint;
    lg.win->event = event;
    lg.win->resized = layout;
    XSetWindowAttributes a = { .override_redirect = True };
    XChangeWindowAttributes(w2k.dpy, lg.win->win, CWOverrideRedirect, &a);
    XMoveWindow(w2k.dpy, lg.win->win, 0, 0);

    lg.user = w2k_edit_new(0);
    w2k_edit_bind(lg.user, lg.win);
    lg.pass = w2k_edit_new(0);
    lg.pass->password = 1;
    w2k_edit_bind(lg.pass, lg.win);
    /* The last user, as Windows remembers it; else the first account. */
    const char *last = getenv("W2K_LOGON_USER");
    if (last && *last) w2k_edit_set(lg.user, last);
    else {
        struct passwd *pw;
        setpwent();
        while ((pw = getpwent()))
            if (pw->pw_uid >= 1000 && pw->pw_uid < 60000 && pw->pw_shell &&
                !strstr(pw->pw_shell, "nologin") && !strstr(pw->pw_shell, "false")) {
                w2k_edit_set(lg.user, pw->pw_name);
                break;
            }
        endpwent();
    }
    lg.pass->focused = 1;
    lg.options_open = 1;
    layout(lg.win);

    char path[1024];
    if (w2k_skin_path("logon-banner.png", path, sizeof path)) lg.banner = w2k_skin_load(path);

#ifdef HAVE_LIGHTDM
    if (!getenv("W2K_RENDER")) {
        lg.greeter = lightdm_greeter_new();
        g_signal_connect(lg.greeter, "show-prompt", G_CALLBACK(on_prompt), NULL);
        g_signal_connect(lg.greeter, "show-message", G_CALLBACK(on_message), NULL);
        g_signal_connect(lg.greeter, "authentication-complete", G_CALLBACK(on_complete), NULL);
        GError *err = NULL;
        if (!lightdm_greeter_connect_to_daemon_sync(lg.greeter, &err)) {
            fprintf(stderr, "w2klogon: %s\n", err ? err->message : "cannot reach lightdm");
            if (err) g_error_free(err);
            g_object_unref(lg.greeter);
            lg.greeter = NULL;
        }
        w2k_add_timer(50, glib_tick, NULL);
    }
#endif
    w2k_add_timer(w2k_caret_blink, blink, lg.user);
    w2k_add_timer(w2k_caret_blink, blink, lg.pass);
    w2k_win_show(lg.win);
    XSetInputFocus(w2k.dpy, lg.win->win, RevertToParent, CurrentTime);
    int rc = w2k_run();
    w2k_fini();
    return rc;
}
