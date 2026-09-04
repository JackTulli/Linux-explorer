/* notifyd.c -- the shell as the desktop's notification service.
 *
 * Programs that want to tell the user something send a Notify call to
 * org.freedesktop.Notifications on the session bus: browsers, mail, music
 * players, notify-send, anything using libnotify. The shell owns that
 * name and answers with the balloon (balloon.c), so every program's
 * notice looks like the shell's own. Without libdbus at build time the
 * service is simply absent and only the shell's own balloons show. */
#include "wm.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_DBUS
#include <dbus/dbus.h>

#define NOTIFY_PATH  "/org/freedesktop/Notifications"
#define NOTIFY_IFACE "org.freedesktop.Notifications"

static DBusConnection *conn;
static int             fd = -1;
static unsigned        next_id = 1;

/* A little of the markup libnotify allows in the body: the tags come
 * out, the entities go back to characters. */
static void strip_markup(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; ) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            /* A line break tag becomes a newline. */
            if (!strncmp(p, "<br", 3)) out[o++] = '\n';
            p = e + 1;
            continue;
        }
        if (*p == '&') {
            static const struct { const char *ent; char ch; } ents[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
                { "&quot;", '"' }, { "&apos;", '\'' },
            };
            int done = 0;
            for (size_t i = 0; i < sizeof ents / sizeof *ents; i++) {
                size_t l = strlen(ents[i].ent);
                if (!strncmp(p, ents[i].ent, l)) { out[o++] = ents[i].ch; p += l; done = 1; break; }
            }
            if (done) continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* The icon a notification names, or the one its urgency implies. */
static int icon_for(const char *app_icon, int urgency)
{
    if (app_icon && *app_icon) {
        if (strstr(app_icon, "warning") || strstr(app_icon, "caution")) return ICO_WARNING;
        if (strstr(app_icon, "error") || strstr(app_icon, "critical")) return ICO_ERROR;
        if (strstr(app_icon, "question")) return ICO_QUESTION;
        int id = w2k_icon_by_name(app_icon);
        if (id != ICO_APP) return id;
    }
    if (urgency == 2) return ICO_WARNING;
    return ICO_INFO;
}

/* The process behind a bus name, through the bus driver. */
static pid_t owner_process(const char *name)
{
    DBusMessage *m = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "GetNameOwner");
    if (!m) return -1;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(conn, m, 1000, NULL);
    dbus_message_unref(m);
    if (!r) return -1;
    const char *owner = NULL;
    dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID);
    char unique[128];
    snprintf(unique, sizeof unique, "%s", owner ? owner : "");
    dbus_message_unref(r);
    if (!unique[0]) return -1;
    m = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus", "GetConnectionUnixProcessID");
    if (!m) return -1;
    const char *u = unique;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &u, DBUS_TYPE_INVALID);
    r = dbus_connection_send_with_reply_and_block(conn, m, 1000, NULL);
    dbus_message_unref(m);
    if (!r) return -1;
    dbus_uint32_t pid = 0;
    dbus_message_get_args(r, NULL, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);
    dbus_message_unref(r);
    return (pid_t)pid;
}

static void reply_u32(DBusMessage *msg, dbus_uint32_t v)
{
    DBusMessage *r = dbus_message_new_method_return(msg);
    if (!r) return;
    dbus_message_append_args(r, DBUS_TYPE_UINT32, &v, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, r, NULL);
    dbus_message_unref(r);
}

static DBusHandlerResult on_message(DBusConnection *c, DBusMessage *msg, void *u)
{
    (void)c; (void)u;
    if (dbus_message_is_method_call(msg, NOTIFY_IFACE, "GetCapabilities")) {
        DBusMessage *r = dbus_message_new_method_return(msg);
        DBusMessageIter it, arr;
        dbus_message_iter_init_append(r, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
        const char *caps[] = { "body", "body-markup", "icon-static", "actions" };
        for (size_t i = 0; i < sizeof caps / sizeof *caps; i++)
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &caps[i]);
        dbus_message_iter_close_container(&it, &arr);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, NOTIFY_IFACE, "GetServerInformation")) {
        const char *name = "w2kwm", *vendor = "Linux-explorer", *ver = W2K_VERSION, *spec = "1.2";
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_message_append_args(r, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &vendor,
                                 DBUS_TYPE_STRING, &ver, DBUS_TYPE_STRING, &spec, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, NOTIFY_IFACE, "Notify")) {
        /* Notify(app_name s, replaces_id u, app_icon s, summary s, body s,
         *        actions as, hints a{sv}, expire_timeout i) -> id u */
        DBusMessageIter it;
        const char *app = "", *icon = "", *summary = "", *body = "";
        dbus_uint32_t replaces = 0;
        dbus_int32_t timeout = -1;
        int urgency = 1;
        if (!dbus_message_iter_init(msg, &it)) return DBUS_HANDLER_RESULT_HANDLED;
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) { dbus_message_iter_get_basic(&it, &app); dbus_message_iter_next(&it); }
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) { dbus_message_iter_get_basic(&it, &replaces); dbus_message_iter_next(&it); }
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) { dbus_message_iter_get_basic(&it, &icon); dbus_message_iter_next(&it); }
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) { dbus_message_iter_get_basic(&it, &summary); dbus_message_iter_next(&it); }
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) { dbus_message_iter_get_basic(&it, &body); dbus_message_iter_next(&it); }
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) dbus_message_iter_next(&it);   /* actions */
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {                              /* hints */
            DBusMessageIter dict;
            dbus_message_iter_recurse(&it, &dict);
            while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entry, var;
                const char *key = "";
                dbus_message_iter_recurse(&dict, &entry);
                if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&entry, &key);
                    dbus_message_iter_next(&entry);
                    if (!strcmp(key, "urgency") && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                        dbus_message_iter_recurse(&entry, &var);
                        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BYTE) {
                            unsigned char b = 1;
                            dbus_message_iter_get_basic(&var, &b);
                            urgency = b;
                        }
                    }
                }
                dbus_message_iter_next(&dict);
            }
            dbus_message_iter_next(&it);
        }
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32) dbus_message_iter_get_basic(&it, &timeout);

        dbus_uint32_t id = replaces ? replaces : next_id++;
        if (!next_id) next_id = 1;
        char title[128], text[512];
        strip_markup(summary && *summary ? summary : app, title, sizeof title);
        strip_markup(body, text, sizeof text);
        /* -1 is "the server decides"; 0 is "never", which we cap. */
        int ms = timeout < 0 ? 0 : timeout == 0 ? 30000 : timeout;
        if (urgency == 2 && ms == 0) ms = 20000;
        balloon_queue(title, text, icon_for(icon, urgency), ms, id);
        reply_u32(msg, id);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, NOTIFY_IFACE, "CloseNotification")) {
        dbus_uint32_t id = 0;
        dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
        balloon_close_id(id);
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        const char *xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node><interface name=\"org.freedesktop.Notifications\">"
            "<method name=\"GetCapabilities\"><arg direction=\"out\" type=\"as\"/></method>"
            "<method name=\"Notify\"><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"u\"/>"
            "<arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/>"
            "<arg direction=\"in\" type=\"as\"/><arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"in\" type=\"i\"/>"
            "<arg direction=\"out\" type=\"u\"/></method>"
            "<method name=\"CloseNotification\"><arg direction=\"in\" type=\"u\"/></method>"
            "<method name=\"GetServerInformation\"><arg direction=\"out\" type=\"s\"/><arg direction=\"out\" type=\"s\"/>"
            "<arg direction=\"out\" type=\"s\"/><arg direction=\"out\" type=\"s\"/></method>"
            "<signal name=\"NotificationClosed\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
            "<signal name=\"ActionInvoked\"><arg type=\"u\"/><arg type=\"s\"/></signal>"
            "</interface></node>";
        DBusMessage *r = dbus_message_new_method_return(msg);
        dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, r, NULL);
        dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable vtable = { NULL, on_message, NULL, NULL, NULL, NULL };

int notifyd_init(void)
{
    DBusError err;
    dbus_error_init(&err);
    conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fprintf(stderr, "w2kwm: notifications: no session bus (%s)\n",
                dbus_error_is_set(&err) ? err.message : "unknown");
        dbus_error_free(&err);
        return -1;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    int rc = dbus_bus_request_name(conn, NOTIFY_IFACE,
                                   DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_ALLOW_REPLACEMENT,
                                   &err);
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        /* Another daemon (dunst, xfce4-notifyd...) holds the name and did
         * not allow replacement. It is the user's own process in the
         * user's own session, and the shell is the desktop here: ask it
         * to leave, then take the name. */
        dbus_error_free(&err);
        pid_t owner_pid = owner_process(NOTIFY_IFACE);
        if (owner_pid > 0 && owner_pid != getpid()) {
            fprintf(stderr, "w2kwm: notifications: stopping the daemon holding %s (pid %ld)\n",
                    NOTIFY_IFACE, (long)owner_pid);
            kill(owner_pid, SIGTERM);
            for (int i = 0; i < 20 && kill(owner_pid, 0) == 0; i++) usleep(50 * 1000);
            dbus_error_init(&err);
            rc = dbus_bus_request_name(conn, NOTIFY_IFACE,
                                       DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_ALLOW_REPLACEMENT,
                                       &err);
        }
    }
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "w2kwm: notifications: another service holds %s (%s)\n",
                NOTIFY_IFACE, dbus_error_is_set(&err) ? err.message : "not replaceable");
        dbus_error_free(&err);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        conn = NULL;
        return -1;
    }
    dbus_connection_register_object_path(conn, NOTIFY_PATH, &vtable, NULL);
    if (!dbus_connection_get_unix_fd(conn, &fd)) fd = -1;
    if (getenv("W2K_DEBUG") || !getenv("W2K_QUIET"))
        fprintf(stderr, "w2kwm: notifications: serving %s\n", NOTIFY_IFACE);
    return 0;
}

int notifyd_fd(void) { return conn ? fd : -1; }

void notifyd_dispatch(void)
{
    if (!conn) return;
    dbus_connection_read_write(conn, 0);
    while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS) ;
    dbus_connection_flush(conn);
}

static void emit(const char *name, unsigned id, const char *s)
{
    if (!conn) return;
    DBusMessage *sig = dbus_message_new_signal(NOTIFY_PATH, NOTIFY_IFACE, name);
    if (!sig) return;
    dbus_uint32_t u = id;
    dbus_message_append_args(sig, DBUS_TYPE_UINT32, &u, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(conn);
}

void notifyd_closed(unsigned id, int reason)
{
    if (!conn) return;
    DBusMessage *sig = dbus_message_new_signal(NOTIFY_PATH, NOTIFY_IFACE, "NotificationClosed");
    if (!sig) return;
    dbus_uint32_t u = id, r = (dbus_uint32_t)reason;
    dbus_message_append_args(sig, DBUS_TYPE_UINT32, &u, DBUS_TYPE_UINT32, &r, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(conn);
}

void notifyd_action(unsigned id, const char *action)
{
    emit("ActionInvoked", id, action);
}

void notifyd_fini(void)
{
    if (!conn) return;
    dbus_bus_release_name(conn, NOTIFY_IFACE, NULL);
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    conn = NULL;
    fd = -1;
}

#else  /* !HAVE_DBUS: no service, the shell's own balloons only */

int  notifyd_init(void) { return -1; }
int  notifyd_fd(void) { return -1; }
void notifyd_dispatch(void) {}
void notifyd_closed(unsigned id, int reason) { (void)id; (void)reason; }
void notifyd_action(unsigned id, const char *action) { (void)id; (void)action; }
void notifyd_fini(void) {}

#endif
