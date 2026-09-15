/* l2kportal.c -- the file chooser portal: Flatpak programs, and anything
 * else that asks xdg-desktop-portal for a file, get the shell's own Open,
 * Save As and Select Folder dialogs.
 *
 * xdg-desktop-portal is the front end sandboxed programs talk to; for each
 * interface it hands the work to a back end, which w2k-portals.conf names
 * for this desktop (XDG_CURRENT_DESKTOP=W2K). This is the back end for
 * org.freedesktop.impl.portal.FileChooser, started by D-Bus the first time
 * it is wanted; the GTK back end keeps the rest. The front end turns the
 * paths chosen here into ones the sandbox can open.
 *
 * One dialog at a time: a second request waits for the first to be
 * answered, as a second Open on Windows waits behind a modal one.
 *
 * W2K_PORTAL_TEST=open|save|folder shows the dialog without D-Bus and
 * prints the uri chosen, for trying it by hand. */
#include "w2kui.h"
#include <dbus/dbus.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.impl.portal.desktop.w2k"
#define OBJ_PATH "/org/freedesktop/portal/desktop"
#define IFACE    "org.freedesktop.impl.portal.FileChooser"

enum { RESP_OK = 0, RESP_CANCEL = 1, RESP_ERROR = 2 };

#define MAXFILTER 8          /* the dialog's own limit */
#define PATLEN    63         /* ...and its pattern's */
#define MAXFILES  64

typedef struct {
    int  multiple, directory;
    char accept[64];
    char name[256];                    /* SaveFile's current_name */
    char folder[1024];                 /* current_folder */
    char file[1024];                   /* SaveFile's current_file */
    char filters[MAXFILTER * 140];     /* "Label|pattern|..." for the dialog */
    char label[MAXFILTER][64];         /* each filter's name, for current_filter */
    int  nfilters;
    char cur_filter[64];
    char files[MAXFILES][256];         /* SaveFiles: the names to be saved */
    int  nfiles;
    int  refused;                      /* ...and how many were not plain names */
} Req;

/* ------------------------------------------------------------------ *
 * Filters
 * ------------------------------------------------------------------ */
/* The dialog matches "*.ext" alone, case-insensitively: a GTK program's
 * "*.[pP][nN][gG]" becomes "*.png", and anything else is left out. */
static int simple_glob(const char *g, char *out, int n)
{
    if (g[0] != '*' || g[1] != '.') return 0;
    int o = 0;
    out[o++] = '*'; out[o++] = '.';
    for (const char *p = g + 2; *p && o < n - 1; p++) {
        if (p[0] == '[' && p[1] && p[2] && p[3] == ']') { out[o++] = (char)tolower((unsigned char)p[1]); p += 3; }
        else if (strchr("*?[]", *p)) return 0;
        else out[o++] = (char)tolower((unsigned char)*p);
    }
    out[o] = 0;
    return o > 2;
}

static void add_pattern(char *pats, const char *glob)
{
    char g[64];
    /* "All Files": everything, whatever else the filter names. */
    if (!strcmp(pats, "*")) return;
    if (!strcmp(glob, "*") || !strcmp(glob, "*.*")) { strcpy(pats, "*"); return; }
    if (!simple_glob(glob, g, sizeof g)) return;
    /* Already there? */
    for (const char *p = pats; (p = strstr(p, g)); p += strlen(g))
        if ((p == pats || p[-1] == ';') && (p[strlen(g)] == 0 || p[strlen(g)] == ';')) return;
    size_t l = strlen(pats);
    if (l + strlen(g) + (l ? 1 : 0) > PATLEN) return;
    if (l) strcat(pats, ";");
    strcat(pats, g);
}

/* A MIME type's globs, out of the shared MIME database: "image/png" is
 * *.png, and a whole family ("image/" and a star) every picture. */
static void add_mime(char *pats, const char *mime)
{
    static const char *const dbs[] = { "/usr/share/mime/globs2", "/usr/local/share/mime/globs2", NULL };
    size_t ml = strlen(mime);
    int any = ml > 2 && !strcmp(mime + ml - 2, "/*");
    for (int d = 0; dbs[d]; d++) {
        FILE *f = fopen(dbs[d], "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            char *t = strchr(line, ':');            /* weight:type:glob[:flags] */
            if (!t) continue;
            char *g = strchr(++t, ':');
            if (!g) continue;
            *g++ = 0;
            g[strcspn(g, ":\r\n")] = 0;
            if (any ? !strncmp(t, mime, ml - 1) : !strcmp(t, mime)) add_pattern(pats, g);
        }
        fclose(f);
        return;
    }
}

/* One filter, (sa(us)): its name and its globs (0) or MIME types (1). */
static void read_filter(DBusMessageIter *st, char *label, int ln, char *pats)
{
    DBusMessageIter s, list, one;
    const char *name = "";
    pats[0] = 0;
    dbus_message_iter_recurse(st, &s);
    if (dbus_message_iter_get_arg_type(&s) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&s, &name);
    snprintf(label, (size_t)ln, "%s", name);
    for (char *p = label; *p; p++) if (*p == '|') *p = '/';
    if (!dbus_message_iter_next(&s) || dbus_message_iter_get_arg_type(&s) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&s, &list);
    while (dbus_message_iter_get_arg_type(&list) == DBUS_TYPE_STRUCT) {
        dbus_uint32_t kind = 0;
        const char *pat = "";
        dbus_message_iter_recurse(&list, &one);
        if (dbus_message_iter_get_arg_type(&one) == DBUS_TYPE_UINT32) dbus_message_iter_get_basic(&one, &kind);
        if (dbus_message_iter_next(&one) && dbus_message_iter_get_arg_type(&one) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&one, &pat);
        if (kind == 0) add_pattern(pats, pat);
        else add_mime(pats, pat);
        dbus_message_iter_next(&list);
    }
}

/* ------------------------------------------------------------------ *
 * The request
 * ------------------------------------------------------------------ */
/* An ay holding a path, NUL-terminated by the sender. */
static void read_bytes(DBusMessageIter *v, char *out, int n)
{
    DBusMessageIter a;
    const char *bytes = NULL;
    int len = 0;
    out[0] = 0;
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(v, &a);
    if (dbus_message_iter_get_arg_type(&a) != DBUS_TYPE_BYTE) return;
    dbus_message_iter_get_fixed_array(&a, &bytes, &len);
    if (len >= n) len = n - 1;
    if (len > 0) memcpy(out, bytes, (size_t)len);
    out[len > 0 ? len : 0] = 0;
}

/* GTK's "_Open" is Windows' "&Open". */
static void accel_label(const char *in, char *out, int n)
{
    int o = 0;
    for (const char *p = in; *p && o < n - 1; p++) {
        if (p[0] == '_' && p[1] == '_') { out[o++] = '_'; p++; }
        else if (*p == '_') out[o++] = '&';
        else out[o++] = *p;
    }
    out[o] = 0;
}

/* A name a sandboxed program may ask to have saved in a folder the user
 * picks: one plain name. Not a path, not "." or "..", nothing hidden (a
 * ".bashrc" of its choosing, written into the home folder on one click,
 * runs outside the sandbox at the next shell), no control characters. */
static int safe_name(const char *s)
{
    if (!s[0] || s[0] == '.' || strchr(s, '/')) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

static void read_options(DBusMessageIter *arr, Req *r)
{
    DBusMessageIter e, kv, v;
    dbus_message_iter_recurse(arr, &e);
    while (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY) {
        const char *key = "";
        dbus_message_iter_recurse(&e, &kv);
        /* a{sv} is what the front end sends; anything else is skipped,
         * not read as though it were (an a{uv} crashed the backend). */
        if (dbus_message_iter_get_arg_type(&kv) != DBUS_TYPE_STRING) { dbus_message_iter_next(&e); continue; }
        dbus_message_iter_get_basic(&kv, &key);
        dbus_message_iter_next(&kv);
        if (dbus_message_iter_get_arg_type(&kv) != DBUS_TYPE_VARIANT) { dbus_message_iter_next(&e); continue; }
        dbus_message_iter_recurse(&kv, &v);
        int t = dbus_message_iter_get_arg_type(&v);
        if ((!strcmp(key, "multiple") || !strcmp(key, "directory")) && t == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t b = 0;
            dbus_message_iter_get_basic(&v, &b);
            if (key[0] == 'm') r->multiple = b; else r->directory = b;
        } else if ((!strcmp(key, "accept_label") || !strcmp(key, "current_name")) && t == DBUS_TYPE_STRING) {
            const char *s = "";
            dbus_message_iter_get_basic(&v, &s);
            if (key[0] == 'a') accel_label(s, r->accept, sizeof r->accept);
            else snprintf(r->name, sizeof r->name, "%s", s);
        } else if (!strcmp(key, "current_folder")) {
            read_bytes(&v, r->folder, sizeof r->folder);
        } else if (!strcmp(key, "current_file")) {
            read_bytes(&v, r->file, sizeof r->file);
        } else if (!strcmp(key, "filters") && t == DBUS_TYPE_ARRAY) {
            DBusMessageIter fl;
            dbus_message_iter_recurse(&v, &fl);
            while (dbus_message_iter_get_arg_type(&fl) == DBUS_TYPE_STRUCT && r->nfilters < MAXFILTER) {
                char pats[PATLEN + 1];
                read_filter(&fl, r->label[r->nfilters], sizeof r->label[0], pats);
                if (pats[0]) {
                    size_t l = strlen(r->filters);
                    snprintf(r->filters + l, sizeof r->filters - l, "%s%s|%s", l ? "|" : "",
                             r->label[r->nfilters], pats);
                    r->nfilters++;
                }
                dbus_message_iter_next(&fl);
            }
        } else if (!strcmp(key, "current_filter") && t == DBUS_TYPE_STRUCT) {
            char pats[PATLEN + 1];
            read_filter(&v, r->cur_filter, sizeof r->cur_filter, pats);
        } else if (!strcmp(key, "files") && t == DBUS_TYPE_ARRAY) {
            DBusMessageIter fl;
            dbus_message_iter_recurse(&v, &fl);
            while (dbus_message_iter_get_arg_type(&fl) == DBUS_TYPE_ARRAY && r->nfiles < MAXFILES) {
                read_bytes(&fl, r->files[r->nfiles], sizeof r->files[0]);
                if (safe_name(r->files[r->nfiles])) r->nfiles++;
                else r->refused++;
                dbus_message_iter_next(&fl);
            }
        }
        dbus_message_iter_next(&e);
    }
}

/* ------------------------------------------------------------------ *
 * The answer
 * ------------------------------------------------------------------ */
static void file_uri(const char *path, char *out, int n)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = snprintf(out, (size_t)n, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p && o < n - 4; p++) {
        if (isalnum(*p) || strchr("/-._~!$&'()*+,;=:@", *p)) out[o++] = (char)*p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

static void send_reply(DBusConnection *c, DBusMessage *m, unsigned response,
                       char uris[][4200], int n, int writable)
{
    DBusMessage *rep = dbus_message_new_method_return(m);
    if (!rep) return;
    DBusMessageIter it, dict, ent, var, arr;
    dbus_uint32_t u = response;
    dbus_message_iter_init_append(rep, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &u);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (response == RESP_OK && n > 0) {
        const char *key = "uris";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
        dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "as", &var);
        dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &arr);
        for (int i = 0; i < n; i++) {
            const char *s = uris[i];
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &s);
        }
        dbus_message_iter_close_container(&var, &arr);
        dbus_message_iter_close_container(&ent, &var);
        dbus_message_iter_close_container(&dict, &ent);
        if (writable) {
            const char *wk = "writable";
            dbus_bool_t b = 1;
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
            dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &wk);
            dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "b", &var);
            dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
            dbus_message_iter_close_container(&ent, &var);
            dbus_message_iter_close_container(&dict, &ent);
        }
    }
    dbus_message_iter_close_container(&it, &dict);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
}

/* ------------------------------------------------------------------ *
 * The dialogs
 * ------------------------------------------------------------------ */
enum { K_OPEN, K_SAVE, K_SAVEFILES };

/* Run the dialog a request asks for; `out` gets the path chosen: the file,
 * or for a folder pick the folder. */
static int choose(int kind, Req *r, const char *title, unsigned long parent, char *out, int n)
{
    const char *home = getenv("HOME");
    W2kFileDlgOpts o = { 0 };
    o.title = title;
    o.accept = r->accept[0] ? r->accept : NULL;
    o.folder = kind == K_SAVEFILES || (kind == K_OPEN && r->directory);
    o.parent = parent;
    for (int i = 0; i < r->nfilters; i++)
        if (r->cur_filter[0] && !strcmp(r->label[i], r->cur_filter)) o.filter = i;

    if (kind == K_SAVE && r->file[0]) snprintf(out, (size_t)n, "%s", r->file);
    else if (kind == K_SAVE)
        snprintf(out, (size_t)n, "%s/%s", r->folder[0] ? r->folder : home ? home : "/", r->name);
    else snprintf(out, (size_t)n, "%s", r->folder[0] ? r->folder : home ? home : "/");

    /* The scheme may have changed since the last one. */
    w2k_scheme_load(NULL);
    for (;;) {
        if (!w2k_file_dialog_opts(NULL, kind == K_SAVE, out, n, o.folder ? NULL : r->filters, &o))
            return 0;
        struct stat st;
        if (kind != K_SAVE || stat(out, &st) != 0) return 1;
        /* Save As over a file that is there: ask, as Windows does. */
        char msg[400];
        const char *base = strrchr(out, '/');
        snprintf(msg, sizeof msg, "%.300s already exists.\nDo you want to replace it?", base ? base + 1 : out);
        if (w2k_msgbox(NULL, title && *title ? title : "Save As", msg, MB_YESNO | MB_ICONWARNING) == ID_YES)
            return 1;
    }
}

static DBusHandlerResult handle(DBusConnection *c, DBusMessage *m, void *user)
{
    (void)user;
    int kind;
    if (dbus_message_is_method_call(m, IFACE, "OpenFile")) kind = K_OPEN;
    else if (dbus_message_is_method_call(m, IFACE, "SaveFile")) kind = K_SAVE;
    else if (dbus_message_is_method_call(m, IFACE, "SaveFiles")) kind = K_SAVEFILES;
    else return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessageIter it;
    const char *handle_path = "", *app_id = "", *parent = "", *title = "";
    if (!dbus_message_iter_init(m, &it)) goto bad;
    const char **str[] = { &handle_path, &app_id, &parent, &title };
    int types[] = { DBUS_TYPE_OBJECT_PATH, DBUS_TYPE_STRING, DBUS_TYPE_STRING, DBUS_TYPE_STRING };
    for (int i = 0; i < 4; i++) {
        if (dbus_message_iter_get_arg_type(&it) != types[i]) goto bad;
        dbus_message_iter_get_basic(&it, str[i]);
        if (!dbus_message_iter_next(&it)) goto bad;
    }
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) goto bad;

    static Req r;
    memset(&r, 0, sizeof r);
    read_options(&it, &r);
    unsigned long xid = 0;
    if (!strncmp(parent, "x11:", 4)) xid = strtoul(parent + 4, NULL, 16);

    static char uris[MAXFILES][4200];
    char path[4096];
    int n = 0;
    /* Names the program sent that were not plain names: nothing is saved
     * for it, and no dialog asks the user to pick a folder for them. */
    if (kind == K_SAVEFILES && (r.refused || !r.nfiles)) {
        send_reply(c, m, RESP_CANCEL, NULL, 0, 0);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!choose(kind, &r, title, xid, path, sizeof path)) {
        send_reply(c, m, RESP_CANCEL, NULL, 0, 0);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (kind == K_SAVEFILES) {
        /* Asked about before anything is replaced, as Save As asks: the
         * names were the program's, the user never saw them. Something
         * there that is not a plain file is not replaced at all. */
        char there[1200] = "";
        int nthere = 0;
        for (int i = 0; i < r.nfiles; i++) {
            char full[4400];
            snprintf(full, sizeof full, "%s%s%s", path, strcmp(path, "/") ? "/" : "", r.files[i]);
            struct stat st;
            if (lstat(full, &st) != 0) continue;
            if (!S_ISREG(st.st_mode)) {
                char msg[700];
                snprintf(msg, sizeof msg, "'%.300s' in that folder is not a file and cannot "
                         "be replaced.", r.files[i]);
                w2k_msgbox(NULL, title, msg, MB_OK | MB_ICONERROR);
                send_reply(c, m, RESP_CANCEL, NULL, 0, 0);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            if (nthere++ < 8) {
                size_t l = strlen(there);
                snprintf(there + l, sizeof there - l, "\n    %.100s", r.files[i]);
            }
        }
        if (nthere) {
            char msg[1600];
            snprintf(msg, sizeof msg, "%s already exist%s in %.300s:\n%s%s\n\nDo you want to "
                     "replace %s?", nthere == 1 ? "This file" : "These files", nthere == 1 ? "s" : "",
                     path, there, nthere > 8 ? "\n    ..." : "", nthere == 1 ? "it" : "them");
            if (w2k_msgbox(NULL, title, msg, MB_YESNO | MB_ICONWARNING) != ID_YES) {
                send_reply(c, m, RESP_CANCEL, NULL, 0, 0);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
        }
        for (int i = 0; i < r.nfiles; i++) {
            char full[4400];
            snprintf(full, sizeof full, "%s%s%s", path, strcmp(path, "/") ? "/" : "", r.files[i]);
            file_uri(full, uris[n++], sizeof uris[0]);
        }
    } else file_uri(path, uris[n++], sizeof uris[0]);
    send_reply(c, m, RESP_OK, uris, n, kind == K_OPEN);
    return DBUS_HANDLER_RESULT_HANDLED;

bad:;
    DBusMessage *err = dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS, "Unexpected arguments");
    if (err) { dbus_connection_send(c, err, NULL); dbus_message_unref(err); }
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ------------------------------------------------------------------ *
 * Running
 * ------------------------------------------------------------------ */
static int test_run(const char *what)
{
    Req r = { 0 };
    int kind = !strcmp(what, "save") ? K_SAVE : K_OPEN;
    r.directory = !strcmp(what, "folder");
    if (kind == K_SAVE) snprintf(r.name, sizeof r.name, "Untitled.txt");
    snprintf(r.filters, sizeof r.filters, "Text Documents|*.txt|Pictures|*.png;*.jpg");
    snprintf(r.label[0], sizeof r.label[0], "Text Documents");
    snprintf(r.label[1], sizeof r.label[1], "Pictures");
    r.nfilters = 2;
    char path[4096], uri[4200];
    if (!choose(kind, &r, NULL, 0, path, sizeof path)) { puts("cancelled"); return 1; }
    file_uri(path, uri, sizeof uri);
    puts(uri);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (w2k_init("l2kportal") < 0) return 1;
    const char *test = getenv("W2K_PORTAL_TEST");
    if (test && *test) return test_run(test);

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!c) {
        fprintf(stderr, "l2kportal: no session bus (%s)\n", dbus_error_is_set(&err) ? err.message : "unknown");
        return 1;
    }
    int rc = dbus_bus_request_name(c, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "l2kportal: %s is taken; another is running\n", BUS_NAME);
        return 0;
    }
    DBusObjectPathVTable vt = { .message_function = handle };
    dbus_connection_register_object_path(c, OBJ_PATH, &vt, NULL);

    int dfd = -1, xfd = ConnectionNumber(w2k.dpy);
    dbus_connection_get_unix_fd(c, &dfd);
    for (;;) {
        while (dbus_connection_dispatch(c) == DBUS_DISPATCH_DATA_REMAINS) {}
        /* Between dialogs nothing of ours is on the screen; what X sends
         * is read and let go, bar a change of scheme. */
        while (XPending(w2k.dpy)) {
            XEvent e;
            XNextEvent(w2k.dpy, &e);
            if (e.type == PropertyNotify && e.xproperty.window == w2k.root &&
                e.xproperty.atom == w2k.a_w2k_scheme) {
                w2k_scheme_load(NULL);
                w2k_font_reload();
            }
        }
        XFlush(w2k.dpy);
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(xfd, &rd);
        if (dfd >= 0) FD_SET(dfd, &rd);
        if (select((dfd > xfd ? dfd : xfd) + 1, &rd, NULL, NULL, NULL) < 0 && errno != EINTR) break;
        if (dfd >= 0 && FD_ISSET(dfd, &rd) && !dbus_connection_read_write(c, 0)) break;
    }
    return 0;
}
