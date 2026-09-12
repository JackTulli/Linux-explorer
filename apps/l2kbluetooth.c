/* l2kbluetooth.c -- Bluetooth Devices, the Control Panel applet.
 *
 * Windows brought Bluetooth in with XP's second service pack, as a sheet
 * of this name (bthprops.cpl); this is that sheet in Windows 2000's
 * clothes, talking to BlueZ over the system bus.
 *
 *   Devices: what this computer has been paired with, grouped by kind,
 *   with Add, Remove, Connect and Properties. Add runs the Add Bluetooth
 *   Device Wizard: search, passkey, pair, connect.
 *   Options: the radio on or off, discovery, whether devices may connect,
 *   whether to ask first, and the icon in the notification area.
 *   Hardware: the Bluetooth radios, and each one's properties.
 *
 * Pairing asks questions -- a passkey to type, a number to compare -- and
 * this program answers them as BlueZ's agent: inside the wizard for the
 * device it is adding, in a dialog for anything else.
 *
 * "l2kbluetooth --agent" is the part that stays. The session starts it; it
 * answers the devices that come to this computer to pair or connect while
 * the sheet is closed, and it is the Bluetooth icon in the notification
 * area. "l2kbluetooth add" opens the wizard by itself.
 *
 * Development aids: W2K_RENDER=<file.ppm> paints the first window and
 * exits, W2K_RENDER_TAB=n picks the sheet's page and W2K_RENDER_PAGE=n the
 * wizard's, and W2K_BT_DEMO=1 fills the lists with made-up devices instead
 * of asking BlueZ; W2K_BT_DEMO=confirm (pin, passkey, display, service,
 * authorize) also puts up the question a pairing device would ask. */
#include "w2k.h"
#include "w2kui.h"
#include <dbus/dbus.h>
#include <X11/Xatom.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BLUEZ       "org.bluez"
#define IF_ADAPTER  "org.bluez.Adapter1"
#define IF_DEVICE   "org.bluez.Device1"
#define IF_BATTERY  "org.bluez.Battery1"
#define IF_AGENT    "org.bluez.Agent1"
#define IF_PROPS    "org.freedesktop.DBus.Properties"
#define IF_OBJMGR   "org.freedesktop.DBus.ObjectManager"
#define AGENT_PATH  "/org/linux2000/bluetooth/agent"

static int agent_mode;          /* --agent: the background part */
static int demo;                /* W2K_BT_DEMO: no BlueZ, made-up devices */

/* ------------------------------------------------------------------ *
 * Settings, ~/.w2k/bluetooth. The radio's own settings live in BlueZ;
 * these are the ones that are this desktop's.
 * ------------------------------------------------------------------ */
static struct {
    int allow;      /* devices may connect to this computer at all */
    int alert;      /* ask before a paired device uses a service here */
    int tray;       /* the icon in the notification area */
} opt = { 1, 1, 1 };
static long long opt_mtime;         /* the file's, in ns: the agent watches it */

static long long mtime_ns(const struct stat *st)
{
    return (long long)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
}

static void opt_path(char *out, int n)
{
    const char *h = getenv("HOME");
    snprintf(out, (size_t)n, "%s/.w2k/bluetooth", h && *h ? h : ".");
}

static void opt_load(void)
{
    char p[1024], line[128];
    opt_path(p, sizeof p);
    struct stat st;
    opt_mtime = stat(p, &st) == 0 ? mtime_ns(&st) : 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "AllowConnections=%d", &v) == 1)  opt.allow = v != 0;
        else if (sscanf(line, "Alert=%d", &v) == 1)        opt.alert = v != 0;
        else if (sscanf(line, "TrayIcon=%d", &v) == 1)     opt.tray = v != 0;
    }
    fclose(f);
}

static void opt_save(void)
{
    char p[1024], dir[1024];
    const char *h = getenv("HOME");
    snprintf(dir, sizeof dir, "%s/.w2k", h && *h ? h : ".");
    mkdir(dir, 0755);
    opt_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "AllowConnections=%d\nAlert=%d\nTrayIcon=%d\n", opt.allow, opt.alert, opt.tray);
    fclose(f);
    struct stat st;
    opt_mtime = stat(p, &st) == 0 ? mtime_ns(&st) : 0;
}

/* ------------------------------------------------------------------ *
 * What BlueZ knows: the radios (adapters) and the devices, kept up to
 * date from its signals. Everything that shows a device names it by its
 * object path and looks it up again: the table moves under it.
 * ------------------------------------------------------------------ */
#define MAX_DEV   256
#define MAX_UUID  32
#define MAX_AD    4

enum { BUSY_NONE, BUSY_CONNECT, BUSY_DISCONNECT, BUSY_PAIR, BUSY_REMOVE };

typedef struct {
    char path[128], adapter[64];
    char address[18], name[128], alias[128], icon[40];
    unsigned cls;
    int  paired, bonded, trusted, blocked, connected, legacy;
    int  rssi, has_rssi;
    int  battery;                   /* per cent; -1 when the device does not say */
    char uuid[MAX_UUID][37];
    int  nuuid;
    int  seen;                      /* heard during the wizard's search */
    int  busy;                      /* what this program is waiting on it for */
} Dev;

typedef struct {
    char path[64];
    char address[18], name[128], alias[128], power_state[24];
    int  powered, discoverable, pairable, connectable, discovering;
    int  manufacturer, version;
} Adapter;

static Dev     devs[MAX_DEV];
static int     ndevs;
static Adapter ads[MAX_AD];
static int     nads;
static int     bluez_up;            /* bluetoothd is on the bus */
static int     model_dirty;
static int     searching;           /* the wizard has discovery running */

static Dev *dev_find(const char *path)
{
    if (!path || !*path) return NULL;
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].path, path)) return &devs[i];
    return NULL;
}

static Dev *dev_get(const char *path)
{
    Dev *d = dev_find(path);
    if (d || ndevs >= MAX_DEV) return d;
    d = &devs[ndevs++];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    /* /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF: the radio is the parent. */
    snprintf(d->adapter, sizeof d->adapter, "%s", path);
    char *slash = strrchr(d->adapter, '/');
    if (slash) *slash = 0;
    d->battery = -1;
    return d;
}

static void dev_remove(const char *path)
{
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].path, path)) {
            memmove(&devs[i], &devs[i + 1], sizeof *devs * (size_t)(ndevs - i - 1));
            ndevs--;
            return;
        }
}

static Adapter *ad_find(const char *path)
{
    for (int i = 0; i < nads; i++)
        if (!strcmp(ads[i].path, path)) return &ads[i];
    return NULL;
}

static Adapter *ad_get(const char *path)
{
    Adapter *a = ad_find(path);
    if (a || nads >= MAX_AD) return a;
    a = &ads[nads++];
    memset(a, 0, sizeof *a);
    snprintf(a->path, sizeof a->path, "%s", path);
    return a;
}

static void ad_remove(const char *path)
{
    for (int i = 0; i < ndevs; )
        if (!strcmp(devs[i].adapter, path)) dev_remove(devs[i].path);
        else i++;
    for (int i = 0; i < nads; i++)
        if (!strcmp(ads[i].path, path)) {
            memmove(&ads[i], &ads[i + 1], sizeof *ads * (size_t)(nads - i - 1));
            nads--;
            return;
        }
}

/* The radio this program works with: the first that is on, else the
 * first. NULL when the computer has none. */
static Adapter *adapter(void)
{
    for (int i = 0; i < nads; i++)
        if (ads[i].powered) return &ads[i];
    return nads ? &ads[0] : NULL;
}

static const char *dev_name(const Dev *d)
{
    return d->alias[0] ? d->alias : d->name[0] ? d->name : d->address;
}

/* A device this computer has been introduced to: it belongs on the
 * Devices page. */
static int dev_known(const Dev *d)
{
    return d->paired || d->bonded || d->trusted;
}

/* ---- kinds of device ---------------------------------------------- */
enum { CAT_AUDIO, CAT_INPUT, CAT_PHONE, CAT_COMPUTER, CAT_IMAGING, CAT_OTHER, NCAT };
static const char *const cat_name[NCAT] = {
    "Audio and Video Devices", "Keyboards, Mice and Game Controllers",
    "Phones and Modems", "Computers", "Imaging Devices and Printers",
    "Other Devices" };

static int prefix(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

/* BlueZ names an icon for the device from its class or appearance
 * ("audio-headset", "input-keyboard"); the class itself is the fallback. */
static int dev_cat(const Dev *d)
{
    const char *i = d->icon;
    if (prefix(i, "audio") || prefix(i, "video") || prefix(i, "multimedia") || prefix(i, "camera-video"))
        return CAT_AUDIO;
    if (prefix(i, "input")) return CAT_INPUT;
    if (prefix(i, "phone") || prefix(i, "modem")) return CAT_PHONE;
    if (prefix(i, "computer")) return CAT_COMPUTER;
    if (prefix(i, "printer") || prefix(i, "camera") || prefix(i, "scanner")) return CAT_IMAGING;
    switch ((d->cls >> 8) & 0x1f) {
    case 1: return CAT_COMPUTER;
    case 2: return CAT_PHONE;
    case 4: return CAT_AUDIO;
    case 5: return CAT_INPUT;
    case 6: return CAT_IMAGING;
    }
    return CAT_OTHER;
}

static int dev_icon(const Dev *d)
{
    const char *i = d->icon;
    if (!strcmp(i, "audio-headset") || !strcmp(i, "audio-headphones")) return ICO_BT_HEADSET;
    if (prefix(i, "audio") || prefix(i, "multimedia")) return ICO_SPEAKER;
    if (!strcmp(i, "input-keyboard")) return ICO_CP_KEYBOARD;
    if (!strcmp(i, "input-mouse") || !strcmp(i, "input-tablet")) return ICO_CP_MOUSE;
    if (!strcmp(i, "input-gaming")) return ICO_BT_GAMEPAD;
    if (prefix(i, "phone") || prefix(i, "modem")) return ICO_BT_PHONE;
    if (prefix(i, "computer")) return ICO_MYCOMPUTER;
    if (prefix(i, "printer")) return ICO_CP_PRINTERS;
    if (prefix(i, "network")) return ICO_NET_WIRELESS;
    if (prefix(i, "video") || prefix(i, "camera")) return ICO_FILE_MOVIE;
    switch (dev_cat(d)) {
    case CAT_AUDIO:    return ICO_BT_HEADSET;
    case CAT_PHONE:    return ICO_BT_PHONE;
    case CAT_COMPUTER: return ICO_MYCOMPUTER;
    case CAT_INPUT:    return (d->cls & 0xc0) == 0x80 ? ICO_CP_MOUSE : ICO_CP_KEYBOARD;
    }
    return ICO_CP_BLUETOOTH;
}

static const char *dev_kind(const Dev *d)
{
    static const struct { const char *icon, *kind; } k[] = {
        { "audio-headset", "Headset" }, { "audio-headphones", "Headphones" },
        { "audio-card", "Speakers" }, { "multimedia-player", "Media player" },
        { "input-keyboard", "Keyboard" }, { "input-mouse", "Mouse" },
        { "input-tablet", "Pen tablet" }, { "input-gaming", "Game controller" },
        { "phone", "Phone" }, { "modem", "Modem" }, { "computer", "Computer" },
        { "printer", "Printer" }, { "camera-photo", "Camera" },
        { "camera-video", "Video camera" }, { "video-display", "Display" },
        { "network-wireless", "Network access point" }, { "scanner", "Scanner" } };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (!strcmp(d->icon, k[i].icon)) return k[i].kind;
    switch (dev_cat(d)) {
    case CAT_AUDIO:    return "Audio device";
    case CAT_INPUT:    return "Input device";
    case CAT_PHONE:    return "Phone";
    case CAT_COMPUTER: return "Computer";
    case CAT_IMAGING:  return "Imaging device";
    }
    return "Bluetooth device";
}

/* ---- services: the 16-bit numbers the Bluetooth SIG gives them ---- */
static const struct { unsigned id; const char *name; int hidden; } services[] = {
    { 0x1101, "Serial port", 0 },               { 0x1103, "Dial-up networking", 0 },
    { 0x1105, "Object push (sending files)", 0 }, { 0x1106, "File transfer", 0 },
    { 0x1108, "Headset", 0 },                   { 0x110A, "Audio source", 0 },
    { 0x110B, "High-quality audio (audio sink)", 0 }, { 0x110C, "Remote control target", 0 },
    { 0x110E, "Remote control", 0 },            { 0x110F, "Remote control", 1 },
    { 0x1112, "Headset audio gateway", 0 },     { 0x1115, "Personal area network user", 0 },
    { 0x1116, "Network access point", 0 },      { 0x1117, "Group ad-hoc network", 0 },
    { 0x111E, "Handsfree", 0 },                 { 0x111F, "Handsfree audio gateway", 0 },
    { 0x1124, "Input device (keyboard, mouse)", 0 }, { 0x112D, "SIM access", 0 },
    { 0x112F, "Phonebook access", 0 },          { 0x1132, "Message access", 0 },
    { 0x1133, "Message notification", 0 },      { 0x1200, "Plug and Play information", 1 },
    { 0x1800, "Generic access", 1 },            { 0x1801, "Generic attribute", 1 },
    { 0x1805, "Current time", 0 },              { 0x180A, "Device information", 1 },
    { 0x180D, "Heart rate", 0 },                { 0x180F, "Battery level", 0 },
    { 0x1812, "Input device (keyboard, mouse)", 0 }, { 0x1813, "Scan parameters", 1 },
    { 0x1844, "Volume control", 0 },            { 0x1846, "Coordinated set", 1 },
    { 0x1848, "Media control", 0 },             { 0x184E, "Audio stream control", 0 },
    { 0x184F, "Broadcast audio", 0 },           { 0x1850, "Audio capabilities", 1 },
    { 0x1853, "Common audio", 1 } };

/* The service's name, or NULL for one not worth listing. */
static const char *service_name(const char *uuid, char *buf, int n)
{
    unsigned id;
    if (strlen(uuid) == 36 && !strcasecmp(uuid + 8, "-0000-1000-8000-00805f9b34fb") &&
        sscanf(uuid, "%8x", &id) == 1) {
        for (size_t i = 0; i < sizeof services / sizeof *services; i++)
            if (services[i].id == id) return services[i].hidden ? NULL : services[i].name;
        snprintf(buf, (size_t)n, "Service %04X", id);
        return buf;
    }
    return NULL;                    /* a vendor's own: nothing to say about it */
}

/* ---- the radio's words --------------------------------------------- */
static const char *company(int id)
{
    static const struct { int id; const char *name; } c[] = {
        { 0x0000, "Ericsson" }, { 0x0001, "Nokia" }, { 0x0002, "Intel Corporation" },
        { 0x0003, "IBM" }, { 0x0004, "Toshiba" }, { 0x0005, "3Com" },
        { 0x0006, "Microsoft" }, { 0x0008, "Motorola" }, { 0x0009, "Infineon" },
        { 0x000A, "Cambridge Silicon Radio" }, { 0x000D, "Texas Instruments" },
        { 0x000F, "Broadcom" }, { 0x001D, "Qualcomm" }, { 0x0025, "NXP Semiconductors" },
        { 0x0030, "STMicroelectronics" }, { 0x0046, "MediaTek" }, { 0x0048, "Marvell" },
        { 0x004C, "Apple" }, { 0x0059, "Nordic Semiconductor" }, { 0x005D, "Realtek Semiconductor" },
        { 0x0075, "Samsung" }, { 0x00E0, "Google" }, { 0x0131, "Cypress Semiconductor" },
        { 0x02E5, "Espressif" } };
    for (size_t i = 0; i < sizeof c / sizeof *c; i++)
        if (c[i].id == id) return c[i].name;
    return NULL;
}

static const char *bt_version(int v)
{
    static const char *const t[] = { "1.0b", "1.1", "1.2", "2.0 + EDR", "2.1 + EDR",
        "3.0 + HS", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2", "5.3", "5.4", "6.0" };
    return v >= 0 && v < (int)(sizeof t / sizeof *t) ? t[v] : NULL;
}

/* ------------------------------------------------------------------ *
 * The system bus
 * ------------------------------------------------------------------ */
static DBusConnection *bus;

/* Work that must not be done inside libdbus's dispatch -- anything that
 * opens a dialog, whose loop would dispatch again -- is queued here and
 * run from a timer, at the top of the loop. */
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static Job *jobs, **jobs_tail = &jobs;

static void jobs_tick(void *unused)
{
    (void)unused;
    w2k_del_timer(jobs_tick, NULL);
    while (jobs) {
        Job *j = jobs;
        jobs = j->next;
        if (!jobs) jobs_tail = &jobs;
        j->fn(j->arg);
        free(j);
    }
}

static void later(void (*fn)(void *), void *arg)
{
    Job *j = w2k_alloc(sizeof *j);
    j->fn = fn;
    j->arg = arg;
    *jobs_tail = j;
    jobs_tail = &j->next;
    w2k_add_timer(1, jobs_tick, NULL);
}

static void refresh(void *unused);
static int refresh_queued;

/* A search in a crowded room is a stream of signal-strength changes: the
 * windows catch up with them a dozen times a second, not at each one. */
static void refresh_tick(void *unused)
{
    w2k_del_timer(refresh_tick, NULL);
    refresh(unused);
}

static void bt_dispatch(void)
{
    if (bus)
        while (dbus_connection_dispatch(bus) == DBUS_DISPATCH_DATA_REMAINS) {}
    if (model_dirty && !refresh_queued) {
        refresh_queued = 1;
        w2k_add_timer(80, refresh_tick, NULL);
    }
}

static void bt_io(void *unused)
{
    (void)unused;
    if (bus) dbus_connection_read_write(bus, 0);
    bt_dispatch();
}

/* A blocking call leaves whatever arrived meanwhile queued inside libdbus,
 * where the descriptor no longer shows it: dispatch it at the next turn. */
static void bt_kick_job(void *unused) { (void)unused; bt_dispatch(); }
static void bt_kick(void) { later(bt_kick_job, NULL); }

/* libdbus's timeouts -- a call that never gets a reply -- on the
 * toolkit's timers. */
static void dt_fire(void *t)
{
    dbus_timeout_handle(t);
    bt_kick();
}
static dbus_bool_t dt_add(DBusTimeout *t, void *u)
{
    (void)u;
    if (dbus_timeout_get_enabled(t)) w2k_add_timer(dbus_timeout_get_interval(t), dt_fire, t);
    return TRUE;
}
static void dt_remove(DBusTimeout *t, void *u) { (void)u; w2k_del_timer(dt_fire, t); }
static void dt_toggle(DBusTimeout *t, void *u)
{
    if (dbus_timeout_get_enabled(t)) dt_add(t, u);
    else dt_remove(t, u);
}

/* ---- reading replies ----------------------------------------------- */
static int it_bool(DBusMessageIter *v)
{
    dbus_bool_t b = 0;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_BOOLEAN) dbus_message_iter_get_basic(v, &b);
    return b != 0;
}

static void it_str(DBusMessageIter *v, char *out, int n)
{
    const char *s = "";
    int t = dbus_message_iter_get_arg_type(v);
    if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) dbus_message_iter_get_basic(v, &s);
    snprintf(out, (size_t)n, "%s", s ? s : "");
}

static long it_num(DBusMessageIter *v)
{
    switch (dbus_message_iter_get_arg_type(v)) {
    case DBUS_TYPE_BYTE:   { unsigned char x;  dbus_message_iter_get_basic(v, &x); return x; }
    case DBUS_TYPE_INT16:  { dbus_int16_t x;   dbus_message_iter_get_basic(v, &x); return x; }
    case DBUS_TYPE_UINT16: { dbus_uint16_t x;  dbus_message_iter_get_basic(v, &x); return x; }
    case DBUS_TYPE_INT32:  { dbus_int32_t x;   dbus_message_iter_get_basic(v, &x); return x; }
    case DBUS_TYPE_UINT32: { dbus_uint32_t x;  dbus_message_iter_get_basic(v, &x); return (long)x; }
    }
    return 0;
}

/* One interface's properties, a{sv}, into the model. */
static void apply_props(const char *path, const char *iface, DBusMessageIter *arr)
{
    int is_ad = !strcmp(iface, IF_ADAPTER), is_dev = !strcmp(iface, IF_DEVICE),
        is_bat = !strcmp(iface, IF_BATTERY);
    if (!is_ad && !is_dev && !is_bat) return;
    if (dbus_message_iter_get_arg_type(arr) != DBUS_TYPE_ARRAY) return;
    Adapter *a = is_ad ? ad_get(path) : NULL;
    Dev *d = is_ad ? NULL : dev_get(path);
    if (!a && !d) return;

    DBusMessageIter e;
    dbus_message_iter_recurse(arr, &e);
    while (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter kv, v;
        const char *k = "";
        dbus_message_iter_recurse(&e, &kv);
        dbus_message_iter_get_basic(&kv, &k);
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &v);
        if (a) {
            if (!strcmp(k, "Address"))              it_str(&v, a->address, sizeof a->address);
            else if (!strcmp(k, "Name"))            it_str(&v, a->name, sizeof a->name);
            else if (!strcmp(k, "Alias"))           it_str(&v, a->alias, sizeof a->alias);
            else if (!strcmp(k, "PowerState"))      it_str(&v, a->power_state, sizeof a->power_state);
            else if (!strcmp(k, "Powered"))         a->powered = it_bool(&v);
            else if (!strcmp(k, "Discoverable"))    a->discoverable = it_bool(&v);
            else if (!strcmp(k, "Pairable"))        a->pairable = it_bool(&v);
            else if (!strcmp(k, "Connectable"))     a->connectable = it_bool(&v);
            else if (!strcmp(k, "Discovering"))     a->discovering = it_bool(&v);
            else if (!strcmp(k, "Manufacturer"))    a->manufacturer = (int)it_num(&v);
            else if (!strcmp(k, "Version"))         a->version = (int)it_num(&v);
        } else if (is_bat) {
            if (!strcmp(k, "Percentage"))           d->battery = (int)it_num(&v);
        } else {
            if (!strcmp(k, "Address"))              it_str(&v, d->address, sizeof d->address);
            else if (!strcmp(k, "Name"))            it_str(&v, d->name, sizeof d->name);
            else if (!strcmp(k, "Alias"))           it_str(&v, d->alias, sizeof d->alias);
            else if (!strcmp(k, "Icon"))            it_str(&v, d->icon, sizeof d->icon);
            else if (!strcmp(k, "Adapter"))         it_str(&v, d->adapter, sizeof d->adapter);
            else if (!strcmp(k, "Class"))           d->cls = (unsigned)it_num(&v);
            else if (!strcmp(k, "Paired"))          d->paired = it_bool(&v);
            else if (!strcmp(k, "Bonded"))          d->bonded = it_bool(&v);
            else if (!strcmp(k, "Trusted"))         d->trusted = it_bool(&v);
            else if (!strcmp(k, "Blocked"))         d->blocked = it_bool(&v);
            else if (!strcmp(k, "Connected"))       d->connected = it_bool(&v);
            else if (!strcmp(k, "LegacyPairing"))   d->legacy = it_bool(&v);
            else if (!strcmp(k, "RSSI")) {
                d->rssi = (int)it_num(&v);
                d->has_rssi = 1;
                if (searching) d->seen = 1;
            } else if (!strcmp(k, "UUIDs") && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_ARRAY) {
                DBusMessageIter u;
                dbus_message_iter_recurse(&v, &u);
                d->nuuid = 0;
                while (dbus_message_iter_get_arg_type(&u) == DBUS_TYPE_STRING && d->nuuid < MAX_UUID) {
                    it_str(&u, d->uuid[d->nuuid], sizeof d->uuid[0]);
                    d->nuuid++;
                    dbus_message_iter_next(&u);
                }
            }
        }
        dbus_message_iter_next(&e);
    }
    model_dirty = 1;
}

/* An object's interfaces, a{sa{sv}}. */
static void apply_ifaces(const char *path, DBusMessageIter *arr)
{
    if (dbus_message_iter_get_arg_type(arr) != DBUS_TYPE_ARRAY) return;
    DBusMessageIter e;
    dbus_message_iter_recurse(arr, &e);
    while (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter kv;
        const char *iface = "";
        dbus_message_iter_recurse(&e, &kv);
        dbus_message_iter_get_basic(&kv, &iface);
        dbus_message_iter_next(&kv);
        apply_props(path, iface, &kv);
        dbus_message_iter_next(&e);
    }
}

static void load_objects(void)
{
    ndevs = nads = 0;
    model_dirty = 1;
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, "/", IF_OBJMGR, "GetManagedObjects");
    if (!m) return;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, 5000, &err);
    dbus_message_unref(m);
    bt_kick();
    if (!r) { dbus_error_free(&err); return; }
    DBusMessageIter it, o;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &o);
        while (dbus_message_iter_get_arg_type(&o) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter kv;
            const char *path = "";
            dbus_message_iter_recurse(&o, &kv);
            dbus_message_iter_get_basic(&kv, &path);
            dbus_message_iter_next(&kv);
            apply_ifaces(path, &kv);
            dbus_message_iter_next(&o);
        }
    }
    dbus_message_unref(r);
}

/* ---- calls that take a while ---------------------------------------- *
 * Connecting, pairing, even setting a property can take seconds; each is
 * sent and its reply handed to `done` later, from the top of the loop. */
typedef struct Pending Pending;
typedef void (*Done)(Pending *p, const char *err, const char *msg);
struct Pending {
    Done         done;
    char         path[128];
    char         what[96];          /* for a message: "turn on Bluetooth" */
    DBusMessage *reply;
};

static void pending_run(void *arg)
{
    Pending *p = arg;
    const char *err = NULL, *msg = "";
    if (!p->reply) {
        err = "org.freedesktop.DBus.Error.NoReply";
    } else if (dbus_message_get_type(p->reply) == DBUS_MESSAGE_TYPE_ERROR) {
        err = dbus_message_get_error_name(p->reply);
        if (!dbus_message_get_args(p->reply, NULL, DBUS_TYPE_STRING, &msg, DBUS_TYPE_INVALID) || !msg)
            msg = "";
    }
    if (p->done) p->done(p, err, msg);
    if (p->reply) dbus_message_unref(p->reply);
    free(p);
}

static void pending_notify(DBusPendingCall *pc, void *arg)
{
    Pending *p = arg;
    p->reply = dbus_pending_call_steal_reply(pc);
    dbus_pending_call_unref(pc);
    later(pending_run, p);
}

static void send_async(DBusMessage *m, int timeout, Done done, const char *path, const char *what)
{
    Pending *p = w2k_alloc(sizeof *p);
    p->done = done;
    snprintf(p->path, sizeof p->path, "%s", path ? path : "");
    snprintf(p->what, sizeof p->what, "%s", what ? what : "");
    DBusPendingCall *pc = NULL;
    if (!bus || !dbus_connection_send_with_reply(bus, m, &pc, timeout) || !pc) {
        dbus_message_unref(m);
        later(pending_run, p);          /* no reply: reported as such */
        return;
    }
    dbus_message_unref(m);
    dbus_pending_call_set_notify(pc, pending_notify, p, NULL);
    dbus_connection_flush(bus);
}

static void call(const char *path, const char *iface, const char *method, int timeout,
                 Done done, const char *what)
{
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, iface, method);
    if (m) send_async(m, timeout, done, path, what);
}

static void call_str(const char *path, const char *iface, const char *method, const char *arg,
                     int timeout, Done done, const char *what)
{
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, iface, method);
    if (!m) return;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
    send_async(m, timeout, done, path, what);
}

static void set_prop(const char *path, const char *iface, const char *name, int type,
                     const void *val, Done done, const char *what)
{
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, IF_PROPS, "Set");
    if (!m) return;
    DBusMessageIter it, v;
    char sig[2] = { (char)type, 0 };
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &v);
    dbus_message_iter_append_basic(&v, type, val);
    dbus_message_iter_close_container(&it, &v);
    send_async(m, 15000, done, path, what);
}

static void set_bool(const char *path, const char *iface, const char *name, int on,
                     Done done, const char *what)
{
    dbus_bool_t b = on != 0;
    set_prop(path, iface, name, DBUS_TYPE_BOOLEAN, &b, done, what);
}

static void set_str(const char *path, const char *iface, const char *name, const char *s,
                    Done done, const char *what)
{
    set_prop(path, iface, name, DBUS_TYPE_STRING, &s, done, what);
}

static void set_u32(const char *path, const char *iface, const char *name, unsigned u,
                    Done done, const char *what)
{
    dbus_uint32_t x = u;
    set_prop(path, iface, name, DBUS_TYPE_UINT32, &x, done, what);
}

/* What went wrong, in words. BlueZ's errors are names and a terse
 * message; the ones a user meets are said plainly. */
static void explain(const char *err, const char *msg, char *out, int n)
{
    const char *e = err ? strrchr(err, '.') : NULL;
    e = e ? e + 1 : "";
    const char *t = NULL;
    if (strstr(msg, "page-timeout") || strstr(msg, "Page Timeout") || strstr(msg, "Host is down") ||
        !strcmp(e, "ConnectionAttemptFailed"))
        t = "The device did not answer. Make sure it is turned on and near this computer.";
    else if (strstr(msg, "profile-unavailable") || strstr(msg, "Protocol not available"))
        t = "The device offers nothing this computer knows how to use.";
    else if (strstr(msg, "refused") || strstr(msg, "Connection refused"))
        t = "The device refused the connection.";
    else if (!strcmp(e, "NotReady") || strstr(msg, "Not Powered"))
        t = "Bluetooth is turned off.";
    else if (!strcmp(e, "Blocked") || strstr(msg, "rfkill") || strstr(msg, "Blocked"))
        t = "Bluetooth is switched off by the computer's wireless switch or airplane mode.";
    else if (!strcmp(e, "AuthenticationFailed"))
        t = "The passkey was not accepted.";
    else if (!strcmp(e, "AuthenticationCanceled") || !strcmp(e, "AuthenticationRejected"))
        t = "The pairing was cancelled.";
    else if (!strcmp(e, "AuthenticationTimeout") || !strcmp(e, "NoReply") || !strcmp(e, "Timeout"))
        t = "The device took too long to answer.";
    else if (!strcmp(e, "InProgress"))
        t = "The device is busy with another request. Try again in a moment.";
    else if (!strcmp(e, "DoesNotExist") || !strcmp(e, "UnknownObject"))
        t = "The device is no longer there.";
    else if (!strcmp(e, "AccessDenied") || !strcmp(e, "NotAuthorized") || !strcmp(e, "NotPermitted"))
        t = "This account is not allowed to change the Bluetooth settings.";
    else if (!strcmp(e, "ServiceUnknown") || !strcmp(e, "NameHasNoOwner"))
        t = "The Bluetooth service (bluetoothd) is not running.";
    if (t) snprintf(out, (size_t)n, "%s", t);
    else snprintf(out, (size_t)n, "%s", *msg ? msg : err ? err : "An unknown error occurred.");
}

static W2kWin *err_parent(void);

/* The usual end of a call: say what failed, if it did. */
static void done_report(Pending *p, const char *err, const char *msg)
{
    Dev *d = dev_find(p->path);
    if (d) { d->busy = BUSY_NONE; model_dirty = 1; bt_kick(); }
    if (!err || !strcmp(err, "org.bluez.Error.AlreadyConnected") ||
        !strcmp(err, "org.bluez.Error.AlreadyExists")) return;
    if (!strcmp(err, "org.bluez.Error.NotConnected") && strstr(p->what, "disconnect")) return;
    char why[256], text[512];
    explain(err, msg, why, sizeof why);
    snprintf(text, sizeof text, "Could not %s.\n\n%s", p->what[0] ? p->what : "do that", why);
    w2k_msgbox(err_parent(), "Bluetooth Devices", text, MB_OK | MB_ICONWARNING);
}

static void done_quiet(Pending *p, const char *err, const char *msg)
{
    (void)err; (void)msg;
    Dev *d = dev_find(p->path);
    if (d) { d->busy = BUSY_NONE; model_dirty = 1; bt_kick(); }
}

/* Turning the radio on can meet the kill switch in software (rfkill);
 * that one this account may usually lift, so it is lifted and the radio
 * asked again. */
static void done_power(Pending *p, const char *err, const char *msg)
{
    if (err && (strstr(err, "Blocked") || strstr(msg, "rfkill") || strstr(msg, "Blocked")) &&
        strcmp(p->what, "turn on Bluetooth (again)")) {
        pid_t pid = fork();
        if (pid == 0) {
            int fd = open("/dev/null", 1);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
            execlp("rfkill", "rfkill", "unblock", "bluetooth", (char *)NULL);
            _exit(127);
        }
        int st;
        if (pid > 0) waitpid(pid, &st, 0);
        set_bool(p->path, IF_ADAPTER, "Powered", 1, done_power, "turn on Bluetooth (again)");
        return;
    }
    done_report(p, err, msg);
}

static void set_power(Adapter *a, int on)
{
    if (!a) return;
    set_bool(a->path, IF_ADAPTER, "Powered", on, on ? done_power : done_report,
             on ? "turn on Bluetooth" : "turn off Bluetooth");
}

/* ------------------------------------------------------------------ *
 * The agent: BlueZ's questions while a device pairs or connects
 * ------------------------------------------------------------------ */
enum { AR_NONE, AR_PIN, AR_PASSKEY, AR_CONFIRM, AR_AUTHORIZE, AR_SERVICE };

static struct {
    DBusMessage *msg;               /* the call to answer; NULL when none */
    int          kind;
    char         device[128], uuid[40];
    unsigned     passkey;
    int          serial;            /* which request a dialog was opened for */
    W2kWin      *dlg;
} areq;

/* What a device asks this computer to show: DisplayPasskey, DisplayPinCode. */
static struct {
    int     active;
    char    device[128];
    char    text[24];               /* the passkey or PIN */
    int     entered;                /* digits typed on the device so far; -1 not told */
    W2kWin *dlg;
} adisp;

static void agent_answer(int serial, int ok, const char *pin, unsigned passkey)
{
    if (!areq.msg || serial != areq.serial) return;
    DBusMessage *r = NULL;
    if (!bus) {
        /* W2K_BT_DEMO's question: nobody to answer. */
    } else if (!ok) {
        r = dbus_message_new_error(areq.msg, "org.bluez.Error.Rejected", "Refused");
    } else {
        r = dbus_message_new_method_return(areq.msg);
        if (r && areq.kind == AR_PIN) {
            if (!pin) pin = "";
            dbus_message_append_args(r, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
        } else if (r && areq.kind == AR_PASSKEY) {
            dbus_uint32_t u = passkey;
            dbus_message_append_args(r, DBUS_TYPE_UINT32, &u, DBUS_TYPE_INVALID);
        }
    }
    if (r && bus) { dbus_connection_send(bus, r, NULL); dbus_connection_flush(bus); }
    if (r) dbus_message_unref(r);
    dbus_message_unref(areq.msg);
    areq.msg = NULL;
    areq.kind = AR_NONE;
    model_dirty = 1;
    bt_kick();
}

static void reply_empty(DBusMessage *m)
{
    DBusMessage *r = dbus_message_new_method_return(m);
    if (!r) return;
    dbus_connection_send(bus, r, NULL);
    dbus_connection_flush(bus);
    dbus_message_unref(r);
}

static void agent_ask(void *unused);
static void agent_display(void *unused);

/* BlueZ gave up on the question (the device went away, or cancelled). */
static void agent_cancel(void)
{
    if (areq.msg) { dbus_message_unref(areq.msg); areq.msg = NULL; areq.kind = AR_NONE; }
    areq.serial++;
    if (areq.dlg) w2k_win_close(areq.dlg, ID_CANCEL);
    adisp.active = 0;
    if (adisp.dlg) w2k_win_close(adisp.dlg, ID_CANCEL);
    model_dirty = 1;
}

static DBusHandlerResult agent_message(DBusConnection *c, DBusMessage *m, void *u)
{
    (void)c; (void)u;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL || !dbus_message_has_interface(m, IF_AGENT))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *mem = dbus_message_get_member(m), *dev = "", *str = "";
    dbus_uint32_t u32 = 0;
    dbus_uint16_t u16 = 0;
    if (!mem) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (!strcmp(mem, "Release")) { reply_empty(m); return DBUS_HANDLER_RESULT_HANDLED; }
    if (!strcmp(mem, "Cancel"))  { reply_empty(m); agent_cancel(); return DBUS_HANDLER_RESULT_HANDLED; }
    if (!strcmp(mem, "DisplayPasskey") || !strcmp(mem, "DisplayPinCode")) {
        int pk = !strcmp(mem, "DisplayPasskey");
        int ok = pk ? dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_UINT32, &u32,
                                            DBUS_TYPE_UINT16, &u16, DBUS_TYPE_INVALID)
                    : dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_STRING, &str,
                                            DBUS_TYPE_INVALID);
        reply_empty(m);
        if (!ok) return DBUS_HANDLER_RESULT_HANDLED;
        int fresh = !adisp.active || strcmp(adisp.device, dev);
        adisp.active = 1;
        snprintf(adisp.device, sizeof adisp.device, "%s", dev);
        if (pk) snprintf(adisp.text, sizeof adisp.text, "%06u", (unsigned)u32);
        else    snprintf(adisp.text, sizeof adisp.text, "%s", str);
        adisp.entered = pk ? u16 : -1;
        model_dirty = 1;
        if (fresh) later(agent_display, NULL);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    int kind = !strcmp(mem, "RequestPinCode")       ? AR_PIN
             : !strcmp(mem, "RequestPasskey")       ? AR_PASSKEY
             : !strcmp(mem, "RequestConfirmation")  ? AR_CONFIRM
             : !strcmp(mem, "RequestAuthorization") ? AR_AUTHORIZE
             : !strcmp(mem, "AuthorizeService")     ? AR_SERVICE : AR_NONE;
    if (!kind) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    int ok = kind == AR_CONFIRM
        ? dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_UINT32, &u32, DBUS_TYPE_INVALID)
        : kind == AR_SERVICE
        ? dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_STRING, &str, DBUS_TYPE_INVALID)
        : dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_INVALID);
    if (!ok || areq.msg) {
        /* One question at a time: BlueZ asks nothing else while one is
         * open, and a malformed one is not answered with a guess. */
        DBusMessage *r = dbus_message_new_error(m, "org.bluez.Error.Rejected", "Busy");
        if (r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    areq.msg = dbus_message_ref(m);
    areq.kind = kind;
    areq.serial++;
    areq.passkey = u32;
    snprintf(areq.device, sizeof areq.device, "%s", dev);
    snprintf(areq.uuid, sizeof areq.uuid, "%s", str);
    model_dirty = 1;
    later(agent_ask, NULL);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* The sheet asks to be the default agent only where no --agent is
 * running, so as not to take the questions from it. */
static int agent_running(void);

static void agent_register(void)
{
    if (!bus) return;
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent");
    if (!m) return;
    const char *path = AGENT_PATH, *cap = "KeyboardDisplay";
    dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_STRING, &cap, DBUS_TYPE_INVALID);
    send_async(m, 5000, NULL, NULL, NULL);
    if (agent_mode || !agent_running()) {
        m = dbus_message_new_method_call(BLUEZ, "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent");
        if (!m) return;
        dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
        send_async(m, 5000, NULL, NULL, NULL);
    }
}

static void enforce_connectable(void);

static void bluez_appeared(void *unused)
{
    (void)unused;
    load_objects();
    agent_register();
    enforce_connectable();
}

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *m, void *u)
{
    (void)c; (void)u;
    DBusMessageIter it, sub;
    const char *path = "";
    if (dbus_message_is_signal(m, IF_OBJMGR, "InterfacesAdded")) {
        if (!dbus_message_iter_init(m, &it)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        dbus_message_iter_get_basic(&it, &path);
        dbus_message_iter_next(&it);
        apply_ifaces(path, &it);
        Dev *d = dev_find(path);
        if (d && searching) d->seen = 1;
    } else if (dbus_message_is_signal(m, IF_OBJMGR, "InterfacesRemoved")) {
        if (!dbus_message_iter_init(m, &it)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        dbus_message_iter_get_basic(&it, &path);
        dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        dbus_message_iter_recurse(&it, &sub);
        while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
            const char *iface = "";
            dbus_message_iter_get_basic(&sub, &iface);
            if (!strcmp(iface, IF_DEVICE)) dev_remove(path);
            else if (!strcmp(iface, IF_ADAPTER)) ad_remove(path);
            else if (!strcmp(iface, IF_BATTERY)) { Dev *d = dev_find(path); if (d) d->battery = -1; }
            dbus_message_iter_next(&sub);
        }
        model_dirty = 1;
    } else if (dbus_message_is_signal(m, IF_PROPS, "PropertiesChanged")) {
        const char *iface = "";
        path = dbus_message_get_path(m);
        if (!path || !dbus_message_iter_init(m, &it)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        dbus_message_iter_get_basic(&it, &iface);
        dbus_message_iter_next(&it);
        /* Only for objects already known: a stray signal must not make one. */
        if (!strcmp(iface, IF_ADAPTER) ? !ad_find(path) : !dev_find(path))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        apply_props(path, iface, &it);
        /* Invalidated: RSSI goes when the device stops being heard. */
        if (dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&it, &sub);
            while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
                const char *k = "";
                dbus_message_iter_get_basic(&sub, &k);
                Dev *d = dev_find(path);
                if (d && !strcmp(k, "RSSI")) d->has_rssi = 0;
                dbus_message_iter_next(&sub);
            }
        }
    } else if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name = "", *old = "", *new_ = "";
        if (!dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old,
                                   DBUS_TYPE_STRING, &new_, DBUS_TYPE_INVALID) || strcmp(name, BLUEZ))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        bluez_up = new_ && *new_;
        ndevs = nads = 0;
        model_dirty = 1;
        if (bluez_up) later(bluez_appeared, NULL);
        else agent_cancel();
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int bt_connect(void)
{
    DBusError err;
    dbus_error_init(&err);
    bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!bus) { dbus_error_free(&err); return 0; }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    dbus_connection_set_timeout_functions(bus, dt_add, dt_remove, dt_toggle, NULL, NULL);
    dbus_bus_add_match(bus, "type='signal',sender='" BLUEZ "',interface='" IF_OBJMGR "'", NULL);
    dbus_bus_add_match(bus, "type='signal',sender='" BLUEZ "',interface='" IF_PROPS "',member='PropertiesChanged'", NULL);
    dbus_bus_add_match(bus, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                            "member='NameOwnerChanged',arg0='" BLUEZ "'", NULL);
    dbus_connection_add_filter(bus, filter, NULL, NULL);
    static const DBusObjectPathVTable vt = { .message_function = agent_message };
    dbus_connection_register_object_path(bus, AGENT_PATH, &vt, NULL);
    int fd = -1;
    if (dbus_connection_get_unix_fd(bus, &fd) && fd >= 0) w2k_add_fd(fd, bt_io, NULL);
    bluez_up = dbus_bus_name_has_owner(bus, BLUEZ, NULL);
    if (bluez_up) bluez_appeared(NULL);
    model_dirty = 1;
    bt_kick();
    return 1;
}

/* Devices may connect to this computer only when the Options page says
 * so: with it off the radio stops listening for them at all. */
static void enforce_connectable(void)
{
    Adapter *a = adapter();
    if (!a || demo || !a->powered) return;
    if (!opt.allow && a->connectable) set_bool(a->path, IF_ADAPTER, "Connectable", 0, done_quiet, NULL);
}

/* ------------------------------------------------------------------ *
 * Drawing helpers
 * ------------------------------------------------------------------ */
/* Text wrapped to `w` from (x, y), a new line at each '\n'; returns the y
 * under it. Measures only when d is 0. */
static int para(Drawable d, int font, int x, int y, int w, const char *text, int color)
{
    int fh = w2k_font_height(font);
    const char *p = text;
    while (*p) {
        if (*p == '\n') { y += fh; p++; continue; }
        const char *end = p, *fit = NULL;
        for (;;) {
            const char *q = end;
            while (*q && *q != ' ' && *q != '\n') q++;
            if (fit && w2k_text_width(font, p, (int)(q - p)) > w) break;
            fit = q;
            if (!*q || *q == '\n') break;
            end = q + 1;
        }
        if (d) w2k_textn(d, font, x, y, p, (int)(fit - p), color);
        y += fh;
        p = fit;
        if (*p == ' ' || *p == '\n') p++;
    }
    return y;
}

/* A list with the round bullets Windows 2000's wizards set them with. */
static int bullets(Drawable d, int x, int y, int w, const char *const *items)
{
    int fh = w2k_font_height(F_UI);
    for (int i = 0; items[i]; i++) {
        if (d) {
            int cy = y + fh / 2 - 2;
            w2k_fill(d, x + 1, cy, 4, 4, C_WINDOWTEXT);
            w2k_fill(d, x, cy + 1, 6, 2, C_WINDOWTEXT);
            w2k_fill(d, x + 2, cy - 1, 2, 6, C_WINDOWTEXT);
        }
        y = para(d, F_UI, x + 14, y, w - 14, items[i], C_WINDOWTEXT);
    }
    return y;
}

static W2kFace *face_title, *face_big;

static void faces_open(void)
{
    if (!face_title) face_title = w2k_face_open_bold("Verdana", 16);
    if (!face_big) face_big = w2k_face_open_bold("Tahoma", 24);
}

/* A passkey, large: the face when there is one, bold otherwise. */
static void big_text(Drawable d, int x, int y, const char *s, int color)
{
    if (face_big) w2k_face_text(d, face_big, x, y, s, color);
    else w2k_text(d, F_UI_BOLD, x, y, s, color);
}

static int big_height(void)
{
    return face_big ? w2k_face_height(face_big) : w2k_font_height(F_UI_BOLD);
}

/* Spaced out, the way a device shows it: "123 456". */
static void spaced(const char *in, char *out, int n)
{
    int len = (int)strlen(in), k = 0;
    for (int i = 0; i < len && k < n - 2; i++) {
        if (i && len > 4 && (len - i) % 3 == 0) out[k++] = ' ';
        out[k++] = in[i];
    }
    out[k] = 0;
}

static void dialog_hints(W2kWin *w, W2kWin *over)
{
    /* A dialog over the sheet; alone (the wizard from the tray, a question
     * from the agent) it is a window of its own, with a taskbar button. */
    if (over) {
        Atom t = w2k.a_net_wm_wt_dialog;
        XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_window_type, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&t, 1);
        XSetTransientForHint(w2k.dpy, w->win, over->win);
    } else if (agent_mode) {
        /* Nothing to stand over: a question from the background agent
         * stays above what the user is doing. */
        Atom s = w2k.a_net_wm_state_above;
        XChangeProperty(w2k.dpy, w->win, w2k.a_net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&s, 1);
    }
}

/* ------------------------------------------------------------------ *
 * A question: words, perhaps a passkey shown large, perhaps a box to type
 * one in, and one or two buttons. The agent's dialogs are all this.
 * ------------------------------------------------------------------ */
typedef struct {
    int         icon;
    const char *text;
    char        big[32];
    const int  *entered;            /* digits typed on the device, live; NULL */
    W2kEdit    *edit;
    int         digits;             /* the box takes digits only */
    const char *b1, *b2;
    W2kRect     r1, r2;
    int         down;
    int         text_y, text_w;
} Ask;

#define ASK_W 400

static int ask_layout(Ask *a, int paint_h)
{
    int y = para(0, F_UI, 58, 16, a->text_w, a->text, 0);
    if (a->big[0]) y += 10 + big_height();
    if (a->entered) y += 4 + w2k_font_height(F_UI);
    if (a->edit) { a->edit->r = (W2kRect){ 58, y + 10, ASK_W - 58 - 16, 21 }; y += 10 + 21; }
    if (y < 16 + 32) y = 16 + 32;
    int by = paint_h ? paint_h - 12 - 23 : y + 18;
    if (a->b2) {
        a->r1 = (W2kRect){ ASK_W - 14 - 75 * 2 - 6, by, 75, 23 };
        a->r2 = (W2kRect){ ASK_W - 14 - 75, by, 75, 23 };
    } else {
        a->r1 = (W2kRect){ ASK_W - 14 - 75, by, 75, 23 };
    }
    return y + 18 + 23 + 12;
}

static void ask_paint(W2kWin *w, Drawable d)
{
    Ask *a = w->user;
    w2k_bigicon_draw(d, 14, 14, a->icon);
    int y = para(d, F_UI, 58, 16, a->text_w, a->text, C_TEXT);
    if (a->big[0]) { big_text(d, 58, y + 10, a->big, C_TEXT); y += 10 + big_height(); }
    if (a->entered) {
        char t[64];
        int total = (int)strlen(a->big) - (strchr(a->big, ' ') ? 1 : 0);
        if (*a->entered >= 0) snprintf(t, sizeof t, "Digits typed on the device: %d of %d", *a->entered, total);
        else snprintf(t, sizeof t, " ");
        w2k_text(d, F_UI, 58, y + 4, t, C_GRAYTEXT);
    }
    if (a->edit) w2k_edit_draw(d, a->edit);
    int ok = !a->edit || w2k_edit_text(a->edit)[0];
    w2k_draw_pushbutton(d, &a->r1, a->b1, BS_DEFAULT | (a->down == 1 ? BS_PRESSED : 0) | (ok ? 0 : BS_DISABLED));
    if (a->b2) w2k_draw_pushbutton(d, &a->r2, a->b2, a->down == 2 ? BS_PRESSED : 0);
}

static int ask_event(W2kWin *w, XEvent *e)
{
    Ask *a = w->user;
    int ok = !a->edit || w2k_edit_text(a->edit)[0];
    switch (e->type) {
    case ButtonPress:
        if (a->edit && w2k_edit_press(a->edit, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        if (ok && w2k_rect_hit(&a->r1, e->xbutton.x, e->xbutton.y)) a->down = 1;
        else if (a->b2 && w2k_rect_hit(&a->r2, e->xbutton.x, e->xbutton.y)) a->down = 2;
        w2k_win_dirty(w);
        return 1;
    case MotionNotify:
        if (a->edit && w2k_edit_motion(a->edit, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        if (a->edit) w2k_edit_release(a->edit);
        int dn = a->down;
        a->down = 0;
        if (dn == 1 && w2k_rect_hit(&a->r1, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_OK);
        else if (dn == 2 && w2k_rect_hit(&a->r2, e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, a->b2 || !a->edit ? ID_CANCEL : ID_OK); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { if (ok) w2k_win_close(w, ID_OK); return 1; }
        if (a->edit) {
            char ch[8];
            int n = XLookupString(&e->xkey, ch, sizeof ch, NULL, NULL);
            if (a->digits && n == 1 && isprint((unsigned char)ch[0]) && !isdigit((unsigned char)ch[0]))
                return 1;
            if (a->digits && n == 1 && isdigit((unsigned char)ch[0]) && strlen(w2k_edit_text(a->edit)) >= 6 &&
                !w2k_edit_has_sel(a->edit))
                return 1;
            if (w2k_edit_key(a->edit, &e->xkey)) { w2k_win_dirty(w); return 1; }
        }
        return 1;
    }
    }
    return 0;
}

static void ask_blink(void *v) { w2k_edit_blink(v); }

/* 1 when the first button was pressed. `out` gets what was typed. */
static int ask(W2kWin *over, W2kWin **handle, const char *title, int icon, const char *text,
               const char *big, const int *entered, int with_edit, int digits,
               const char *b1, const char *b2, char *out, int outn)
{
    Ask a = { .icon = icon, .text = text, .entered = entered, .digits = digits, .b1 = b1, .b2 = b2 };
    a.text_w = ASK_W - 58 - 16;
    if (big) spaced(big, a.big, sizeof a.big);
    if (with_edit) a.edit = w2k_edit_new(0);
    faces_open();
    int h = ask_layout(&a, 0);
    W2kWin *w = w2k_win_new(title, "l2kbluetooth", ASK_W, h, 0);
    ask_layout(&a, h);
    w->user = &a;
    w->paint = ask_paint;
    w->event = ask_event;
    if (a.edit) {
        w2k_edit_bind(a.edit, w);
        a.edit->focused = 1;
        w2k_add_timer(w2k_caret_blink, ask_blink, a.edit);
    }
    w2k_win_center(w, over);
    dialog_hints(w, over);
    if (handle) *handle = w;
    if (agent_mode) w2k_sound_play(SND_NOTIFICATION);
    int r = w2k_win_modal(w);
    if (handle) *handle = NULL;
    if (a.edit) {
        w2k_del_timer(ask_blink, a.edit);
        if (out) snprintf(out, (size_t)outn, "%s", w2k_edit_text(a.edit));
        w2k_edit_wipe(a.edit);
        w2k_edit_free(a.edit);
    }
    return r == ID_OK;
}

/* ------------------------------------------------------------------ *
 * The device list: a white well of rows, each a 32-pixel icon, the name
 * and a line of status under it -- under headings by kind on the Devices
 * page, as XP grouped them, and plain in the wizard's search.
 * ------------------------------------------------------------------ */
#define DV_HEAD 24
#define DV_ROW  42

typedef struct {
    W2kRect   r;
    struct { int head, cat; char path[128]; } row[MAX_DEV + NCAT];
    int       nrows;
    int       sel;                  /* the selected row, -1 */
    char      selpath[128];         /* the same, kept across rebuilds */
    W2kScroll sb;
    int       focused;
    int       grouped;
    int       wizard;               /* rows say "New device", not "Connected" */
    long      last_click;
    int       last_row;
    char      empty[320];           /* shown when there are no rows */
    void    (*on_activate)(void *user);
    void    (*on_menu)(void *user, int root_x, int root_y);
    void    (*on_select)(void *user);
    void     *user;
    W2kWin   *owner;
} DevView;

static void dev_status(const Dev *d, int wizard, char *out, int n)
{
    static const char *const busy[] = { "", "Connecting...", "Disconnecting...", "Pairing...", "Removing..." };
    if (d->busy) { snprintf(out, (size_t)n, "%s", busy[d->busy]); return; }
    if (wizard) {
        snprintf(out, (size_t)n, "%s  (%s)", dev_known(d) ? "Already added" : "New device", d->address);
        return;
    }
    if (d->blocked) { snprintf(out, (size_t)n, "Blocked"); return; }
    if (d->connected && d->battery >= 0) snprintf(out, (size_t)n, "Connected, battery %d%%", d->battery);
    else snprintf(out, (size_t)n, "%s", d->connected ? "Connected" : "Not connected");
}

static int dv_height(DevView *v)
{
    int h = 0;
    for (int i = 0; i < v->nrows; i++) h += v->row[i].head ? DV_HEAD : DV_ROW;
    return h;
}

static W2kRect dv_inner(DevView *v)
{
    W2kRect in = { v->r.x + 2, v->r.y + 2, v->r.w - 4, v->r.h - 4 };
    if (w2k_scroll_needed(&v->sb)) in.w -= SCROLL_W;
    return in;
}

static void dv_layout(DevView *v)
{
    v->sb.vertical = 1;
    v->sb.r = (W2kRect){ v->r.x + v->r.w - 2 - SCROLL_W, v->r.y + 2, SCROLL_W, v->r.h - 4 };
    v->sb.page = v->r.h - 4;
    v->sb.total = dv_height(v);
    v->sb.line = DV_ROW / 2;
    w2k_scroll_clamp(&v->sb);
}

static int dv_row_y(DevView *v, int row)
{
    int y = 0;
    for (int i = 0; i < row; i++) y += v->row[i].head ? DV_HEAD : DV_ROW;
    return y;
}

static void dv_ensure_visible(DevView *v, int row)
{
    if (row < 0) return;
    int y = dv_row_y(v, row), h = v->row[row].head ? DV_HEAD : DV_ROW;
    /* The heading above the first device of a kind comes into view with it. */
    if (row > 0 && v->row[row - 1].head && y - DV_HEAD < v->sb.pos) y -= DV_HEAD, h += DV_HEAD;
    if (y < v->sb.pos) v->sb.pos = y;
    else if (y + h > v->sb.pos + v->sb.page) v->sb.pos = y + h - v->sb.page;
    w2k_scroll_clamp(&v->sb);
}

static int dv_cmp(const void *a, const void *b)
{
    const Dev *x = dev_find(*(const char *const *)a), *y = dev_find(*(const char *const *)b);
    return strcasecmp(x ? dev_name(x) : "", y ? dev_name(y) : "");
}

/* Fill the rows with the devices `want` accepts, keeping the selection. */
static void dv_fill(DevView *v, int (*want)(const Dev *))
{
    v->nrows = 0;
    static const char *pick[MAX_DEV];
    int n = 0;
    for (int i = 0; i < ndevs; i++)
        if (want(&devs[i])) pick[n++] = devs[i].path;
    if (v->grouped) {
        qsort(pick, (size_t)n, sizeof *pick, dv_cmp);
        for (int c = 0; c < NCAT; c++) {
            int any = 0;
            for (int i = 0; i < n; i++) {
                const Dev *d = dev_find(pick[i]);
                if (!d || dev_cat(d) != c) continue;
                if (!any) { v->row[v->nrows].head = 1; v->row[v->nrows].cat = c; v->row[v->nrows].path[0] = 0; v->nrows++; any = 1; }
                v->row[v->nrows].head = 0;
                v->row[v->nrows].cat = c;
                snprintf(v->row[v->nrows].path, sizeof v->row[0].path, "%s", pick[i]);
                v->nrows++;
            }
        }
    } else {
        for (int i = 0; i < n; i++) {
            v->row[v->nrows].head = 0;
            snprintf(v->row[v->nrows].path, sizeof v->row[0].path, "%s", pick[i]);
            v->nrows++;
        }
    }
    v->sel = -1;
    for (int i = 0; i < v->nrows; i++)
        if (!v->row[i].head && v->selpath[0] && !strcmp(v->row[i].path, v->selpath)) v->sel = i;
    if (v->sel < 0) v->selpath[0] = 0;
    dv_layout(v);
}

static const Dev *dv_selected(DevView *v)
{
    return v->sel >= 0 ? dev_find(v->row[v->sel].path) : NULL;
}

static void dv_select(DevView *v, int row)
{
    if (row < 0 || row >= v->nrows || v->row[row].head) return;
    v->sel = row;
    snprintf(v->selpath, sizeof v->selpath, "%s", v->row[row].path);
    dv_ensure_visible(v, row);
    if (v->on_select) v->on_select(v->user);
}

static void dv_draw(Drawable d, DevView *v)
{
    w2k_draw_well(d, &v->r);
    W2kRect in = dv_inner(v);
    int fh = w2k_font_height(F_UI);
    w2k_clip_set(in.x, in.y, in.w, in.h);
    int y = in.y - v->sb.pos;
    for (int i = 0; i < v->nrows; i++) {
        int h = v->row[i].head ? DV_HEAD : DV_ROW;
        if (y + h > in.y && y < in.y + in.h) {
            if (v->row[i].head) {
                const char *t = cat_name[v->row[i].cat];
                int tw = w2k_text_width(F_UI_BOLD, t, -1);
                w2k_text(d, F_UI_BOLD, in.x + 6, y + 7, t, C_WINDOWTEXT);
                int lx = in.x + 6 + tw + 8, lw = in.x + in.w - 8 - lx;
                if (lw > 0) w2k_hline(d, lx, y + 7 + fh / 2 + 1, lw, C_SHADOW);
            } else {
                const Dev *dv = dev_find(v->row[i].path);
                int sel = i == v->sel, hi = sel && v->focused;
                if (sel) w2k_fill(d, in.x + 2, y + 1, in.w - 4, h - 2, hi ? C_HIGHLIGHT : C_FACE);
                if (dv) {
                    char name[160], st[160];
                    int ix = in.x + (v->grouped ? 18 : 8);
                    w2k_bigicon_draw(d, ix, y + 5, dev_icon(dv));
                    w2k_ellipsis(F_UI, dev_name(dv), in.x + in.w - ix - 48, name, sizeof name);
                    w2k_text(d, F_UI, ix + 42, y + 7, name, hi ? C_HIGHLIGHTTEXT : C_WINDOWTEXT);
                    dev_status(dv, v->wizard, st, sizeof st);
                    w2k_text(d, F_UI, ix + 42, y + 9 + fh, st, hi ? C_HIGHLIGHTTEXT : C_GRAYTEXT);
                }
                if (hi) w2k_focus_rect(d, in.x + 2, y + 1, in.w - 4, h - 2);
            }
        }
        y += h;
    }
    if (!v->nrows && v->empty[0])
        para(d, F_UI, in.x + 12, in.y + 12, in.w - 24, v->empty, C_GRAYTEXT);
    w2k_clip_clear();
    if (w2k_scroll_needed(&v->sb)) w2k_scroll_draw(d, &v->sb);
}

static int dv_row_at(DevView *v, int x, int y)
{
    W2kRect in = dv_inner(v);
    if (!w2k_rect_hit(&in, x, y)) return -1;
    int ry = in.y - v->sb.pos;
    for (int i = 0; i < v->nrows; i++) {
        int h = v->row[i].head ? DV_HEAD : DV_ROW;
        if (y >= ry && y < ry + h) return i;
        ry += h;
    }
    return -1;
}

static int dv_press(DevView *v, XButtonEvent *b)
{
    if (!w2k_rect_hit(&v->r, b->x, b->y)) { v->focused = 0; return 0; }
    v->focused = 1;
    if (b->button == Button4 || b->button == Button5) {
        w2k_scroll_wheel(&v->sb, b->button == Button4 ? -1 : 1);
        return 1;
    }
    if (w2k_scroll_needed(&v->sb) && w2k_scroll_part(&v->sb, b->x, b->y) != SB_NONE) {
        w2k_scroll_press(&v->sb, b->x, b->y);
        return 1;
    }
    int row = dv_row_at(v, b->x, b->y);
    if (row >= 0 && v->row[row].head) row = -1;
    if (row < 0) {
        v->sel = -1;
        v->selpath[0] = 0;
        if (v->on_select) v->on_select(v->user);
        if (b->button == Button3 && v->on_menu) v->on_menu(v->user, b->x_root, b->y_root);
        return 1;
    }
    dv_select(v, row);
    long now = (long)b->time;
    if (b->button == Button1 && row == v->last_row && now - v->last_click < w2k_dblclk_ms) {
        v->last_click = 0;
        if (v->on_activate) v->on_activate(v->user);
        return 1;
    }
    v->last_click = now;
    v->last_row = row;
    if (b->button == Button3 && v->on_menu) v->on_menu(v->user, b->x_root, b->y_root);
    return 1;
}

static int dv_motion(DevView *v, XMotionEvent *m)
{
    if (!v->sb.pressed) return 0;
    w2k_scroll_motion(&v->sb, m->x, m->y);
    return 1;
}

static void dv_release(DevView *v) { w2k_scroll_release(&v->sb); }

static int dv_key(DevView *v, XKeyEvent *k)
{
    if (!v->focused) return 0;
    KeySym ks = XLookupKeysym(k, 0);
    int row = v->sel, dir = 0;
    switch (ks) {
    case XK_Up:   case XK_KP_Up:   dir = -1; break;
    case XK_Down: case XK_KP_Down: dir = 1; break;
    case XK_Home: row = -1; dir = 1; break;
    case XK_End:  row = v->nrows; dir = -1; break;
    case XK_Return: case XK_KP_Enter:
        if (v->sel >= 0 && v->on_activate) { v->on_activate(v->user); return 1; }
        return 0;
    case XK_Menu:
        if (v->on_menu && v->sel >= 0) {
            Window child;
            int rx, ry;
            W2kRect in = dv_inner(v);
            XTranslateCoordinates(w2k.dpy, v->owner->win, w2k.root, w2k_px(in.x + 40),
                                  w2k_px(in.y + dv_row_y(v, v->sel) - v->sb.pos + DV_ROW / 2), &rx, &ry, &child);
            v->on_menu(v->user, rx, ry);
        }
        return 1;
    default: return 0;
    }
    if (row < 0 && ks != XK_Home) row = -1, dir = 1;
    for (int r = row + dir; r >= 0 && r < v->nrows; r += dir)
        if (!v->row[r].head) { dv_select(v, r); break; }
    return 1;
}

/* ------------------------------------------------------------------ *
 * Doing things to devices
 * ------------------------------------------------------------------ */
/* Each takes a copy of the path first: a dialog in between runs the loop,
 * and the table the caller's pointer points into can move. */
static void dev_toggle(W2kWin *over, const char *path_in)
{
    char path[128];
    snprintf(path, sizeof path, "%s", path_in);
    Dev *d = dev_find(path);
    if (!d || d->busy) return;
    char what[200];
    Adapter *a = ad_find(d->adapter);
    if (!d->connected && a && !a->powered) {
        w2k_msgbox(over, "Bluetooth Devices",
                   "Bluetooth is turned off.\n\nTo connect to the device, turn Bluetooth on, on the Options page.",
                   MB_OK | MB_ICONINFO);
        return;
    }
    if (!d->connected && d->blocked) {
        w2k_msgbox(over, "Bluetooth Devices",
                   "This device is blocked, so it cannot connect.\n\nTo unblock it, open its properties and clear \"Block this device\".",
                   MB_OK | MB_ICONINFO);
        return;
    }
    snprintf(what, sizeof what, "%s %s", d->connected ? "disconnect" : "connect to", dev_name(d));
    d->busy = d->connected ? BUSY_DISCONNECT : BUSY_CONNECT;
    call(path, IF_DEVICE, d->connected ? "Disconnect" : "Connect", d->connected ? 20000 : 45000,
         done_report, what);
    model_dirty = 1;
    bt_kick();
}

static void dev_rename(W2kWin *over, const char *path_in)
{
    char path[128];
    snprintf(path, sizeof path, "%s", path_in);
    Dev *d = dev_find(path);
    if (!d) return;
    char name[128];
    if (!w2k_prompt(over, "Rename Device", "Type a new name for this device:", dev_name(d),
                    name, sizeof name, dev_icon(d)))
        return;
    if (!dev_find(path)) return;
    /* An empty name gives the device back its own. */
    set_str(path, IF_DEVICE, "Alias", name, done_report, "rename the device");
}

static void dev_forget(W2kWin *over, const char *path_in)
{
    char path[128];
    snprintf(path, sizeof path, "%s", path_in);
    Dev *d = dev_find(path);
    if (!d) return;
    char text[400];
    snprintf(text, sizeof text, "Are you sure you want to remove %s?\n\n"
             "To use it with this computer again, you will have to add it again.", dev_name(d));
    if (w2k_msgbox(over, "Remove Bluetooth Device", text, MB_YESNO | MB_ICONQUESTION) != ID_YES) return;
    d = dev_find(path);
    if (!d || !bus) return;
    DBusMessage *m = dbus_message_new_method_call(BLUEZ, d->adapter, IF_ADAPTER, "RemoveDevice");
    if (!m) return;
    const char *p = d->path;
    dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
    d->busy = BUSY_REMOVE;
    send_async(m, 15000, done_report, path, "remove the device");
    model_dirty = 1;
    bt_kick();
}

/* ------------------------------------------------------------------ *
 * A device's properties: General and Services
 * ------------------------------------------------------------------ */
enum { DP_OK = 1, DP_CANCEL, DP_APPLY, DP_CONNECT, DP_SCONNECT, DP_SDISCONNECT, NDP };

static struct {
    W2kWin  *w;
    W2kTabs *tabs;
    char     path[128];
    W2kEdit *name;
    W2kList *svc;
    int      trusted, blocked, dirty;
    W2kRect  b[NDP], cb_trust, cb_block;
    int      down;
} dp;

#define DP_W 380
#define DP_H 420

static void dp_apply(void)
{
    Dev *d = dev_find(dp.path);
    if (!d || !dp.dirty) return;
    const char *nm = w2k_edit_text(dp.name);
    if (strcmp(nm, dev_name(d))) set_str(d->path, IF_DEVICE, "Alias", nm, done_report, "rename the device");
    if (dp.trusted != d->trusted) set_bool(d->path, IF_DEVICE, "Trusted", dp.trusted, done_report, "change the device's settings");
    if (dp.blocked != d->blocked) set_bool(d->path, IF_DEVICE, "Blocked", dp.blocked, done_report, "change the device's settings");
    dp.dirty = 0;
}

static void dp_fill_services(void)
{
    Dev *d = dev_find(dp.path);
    int keep = dp.svc->sel;
    w2k_list_clear(dp.svc);
    if (d)
        for (int i = 0; i < d->nuuid; i++) {
            char buf[64];
            const char *n = service_name(d->uuid[i], buf, sizeof buf);
            if (!n) continue;
            int r = w2k_list_add(dp.svc, ICO_NONE, (void *)(intptr_t)i);
            w2k_list_set(dp.svc, r, 0, n);
        }
    if (keep >= 0 && keep < dp.svc->n) { dp.svc->sel = keep; dp.svc->items[keep].selected = 1; }
    w2k_list_layout(dp.svc);
}

static const char *dp_selected_uuid(void)
{
    Dev *d = dev_find(dp.path);
    if (!d || dp.svc->sel < 0 || dp.svc->sel >= dp.svc->n) return NULL;
    int i = (int)(intptr_t)dp.svc->items[dp.svc->sel].data;
    return i >= 0 && i < d->nuuid ? d->uuid[i] : NULL;
}

static int dp_on_page(int b)
{
    int p = dp.tabs->sel;
    return b <= DP_APPLY || (b == DP_CONNECT && p == 0) || ((b == DP_SCONNECT || b == DP_SDISCONNECT) && p == 1);
}

static int dp_enabled(int b)
{
    Dev *d = dev_find(dp.path);
    switch (b) {
    case DP_APPLY:       return dp.dirty && d;
    case DP_CONNECT:     return d && !d->busy;
    case DP_SCONNECT: case DP_SDISCONNECT: return d && dp_selected_uuid() && !d->busy;
    }
    return 1;
}

static void dp_paint(W2kWin *w, Drawable dr)
{
    (void)w;
    Dev *d = dev_find(dp.path);
    int fh = w2k_font_height(F_UI);
    w2k_tabs_draw(dr, dp.tabs);
    W2kRect c = w2k_tabs_client(dp.tabs);
    if (dp.tabs->sel == 0) {
        w2k_bigicon_draw(dr, c.x + 14, c.y + 14, d ? dev_icon(d) : ICO_CP_BLUETOOTH);
        w2k_edit_draw(dr, dp.name);
        w2k_hline(dr, c.x + 12, c.y + 58, c.w - 24, C_SHADOW);
        w2k_hline(dr, c.x + 12, c.y + 59, c.w - 24, C_HILIGHT);
        int y = c.y + 72, lx = c.x + 14, vx = c.x + 110;
        if (!d) {
            w2k_text(dr, F_UI, lx, y, "This device has been removed.", C_TEXT);
        } else {
            char t[200];
            w2k_text(dr, F_UI, lx, y, "Device type:", C_TEXT);
            w2k_text(dr, F_UI, vx, y, dev_kind(d), C_TEXT);                          y += fh + 8;
            w2k_text(dr, F_UI, lx, y, "Address:", C_TEXT);
            w2k_text(dr, F_UI, vx, y, d->address, C_TEXT);                           y += fh + 8;
            w2k_text(dr, F_UI, lx, y, "Status:", C_TEXT);
            dev_status(d, 0, t, sizeof t);
            w2k_text(dr, F_UI, vx, y, t, C_TEXT);                                    y += fh + 8;
            w2k_text(dr, F_UI, lx, y, "Paired:", C_TEXT);
            w2k_text(dr, F_UI, vx, y, d->paired ? (d->legacy ? "Yes, with a passkey" : "Yes") : "No", C_TEXT);
            y += fh + 8;
            if (d->battery >= 0) {
                w2k_text(dr, F_UI, lx, y, "Battery:", C_TEXT);
                W2kRect pb = { vx, y - 1, 120, fh + 2 };
                w2k_draw_progress(dr, &pb, d->battery, 0);
                snprintf(t, sizeof t, "%d%%", d->battery);
                w2k_text(dr, F_UI, vx + 128, y, t, C_TEXT);
                y += fh + 8;
            }
            if (d->has_rssi) {
                w2k_text(dr, F_UI, lx, y, "Signal:", C_TEXT);
                snprintf(t, sizeof t, "%s (%d dBm)", d->rssi > -60 ? "Strong" : d->rssi > -75 ? "Good" : "Weak", d->rssi);
                w2k_text(dr, F_UI, vx, y, t, C_TEXT);
            }
        }
        int ly = dp.cb_trust.y - 12;
        w2k_hline(dr, c.x + 12, ly, c.w - 24, C_SHADOW);
        w2k_hline(dr, c.x + 12, ly + 1, c.w - 24, C_HILIGHT);
        w2k_draw_checkbox(dr, dp.cb_trust.x, dp.cb_trust.y, "&Always allow this device to connect without asking",
                          dp.trusted, 0, !d);
        w2k_draw_checkbox(dr, dp.cb_block.x, dp.cb_block.y, "&Block this device", dp.blocked, 0, !d);
    } else {
        para(dr, F_UI, c.x + 12, c.y + 12, c.w - 24,
             "This Bluetooth device offers the following services. To use one, select it and click Connect.",
             C_TEXT);
        w2k_list_draw(dr, dp.svc);
        if (d && !dp.svc->n)
            para(dr, F_UI, dp.svc->r.x + 8, dp.svc->r.y + 24, dp.svc->r.w - 16,
                 "The device has not said which services it offers. Connect to it to find out.", C_GRAYTEXT);
    }
    for (int b = 1; b < NDP; b++) {
        if (!dp_on_page(b)) continue;
        static const char *const lab[NDP] = { [DP_OK] = "OK", [DP_CANCEL] = "Cancel", [DP_APPLY] = "&Apply",
                                              [DP_SCONNECT] = "C&onnect", [DP_SDISCONNECT] = "&Disconnect" };
        const char *l = b == DP_CONNECT ? (d && d->connected ? "Dis&connect" : "&Connect") : lab[b];
        w2k_draw_pushbutton(dr, &dp.b[b], l, (b == DP_OK ? BS_DEFAULT : 0) | (dp.down == b ? BS_PRESSED : 0) |
                            (dp_enabled(b) ? 0 : BS_DISABLED));
    }
}

static void dp_press(int b)
{
    Dev *d = dev_find(dp.path);
    const char *u = dp_selected_uuid();
    switch (b) {
    case DP_OK:     dp_apply(); w2k_win_close(dp.w, ID_OK); break;
    case DP_CANCEL: w2k_win_close(dp.w, ID_CANCEL); break;
    case DP_APPLY:  dp_apply(); break;
    case DP_CONNECT: dev_toggle(dp.w, dp.path); break;
    case DP_SCONNECT: case DP_SDISCONNECT:
        if (d && u) {
            char what[64], buf[64];
            const char *n = service_name(u, buf, sizeof buf);
            snprintf(what, sizeof what, "%s %s", b == DP_SCONNECT ? "connect to" : "disconnect from", n ? n : "the service");
            d->busy = b == DP_SCONNECT ? BUSY_CONNECT : BUSY_DISCONNECT;
            call_str(d->path, IF_DEVICE, b == DP_SCONNECT ? "ConnectProfile" : "DisconnectProfile", u,
                     30000, done_report, what);
        }
        break;
    }
    w2k_win_dirty(dp.w);
}

static void dp_blink(void *v) { w2k_edit_blink(v); }

static int dp_event(W2kWin *w, XEvent *e)
{
    Dev *d = dev_find(dp.path);
    switch (e->type) {
    case ButtonPress: {
        XButtonEvent *b = &e->xbutton;
        if (w2k_tabs_press(dp.tabs, b)) { dp.name->focused = 0; w2k_win_dirty(w); return 1; }
        if (dp.tabs->sel == 0) {
            if (w2k_edit_press(dp.name, b)) { w2k_win_dirty(w); return 1; }
            dp.name->focused = 0;
            if (d && w2k_rect_hit(&dp.cb_trust, b->x, b->y)) { dp.trusted = !dp.trusted; dp.dirty = 1; }
            if (d && w2k_rect_hit(&dp.cb_block, b->x, b->y)) { dp.blocked = !dp.blocked; dp.dirty = 1; }
        } else if (w2k_list_press(dp.svc, b)) {
            w2k_win_dirty(w);
            return 1;
        }
        for (int i = 1; i < NDP; i++)
            if (dp_on_page(i) && dp_enabled(i) && w2k_rect_hit(&dp.b[i], b->x, b->y)) dp.down = i;
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (dp.tabs->sel == 0 && w2k_edit_motion(dp.name, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        if (dp.tabs->sel == 1 && w2k_list_motion(dp.svc, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        w2k_edit_release(dp.name);
        w2k_list_release(dp.svc, &e->xbutton);
        int b = dp.down;
        dp.down = 0;
        if (b && w2k_rect_hit(&dp.b[b], e->xbutton.x, e->xbutton.y)) dp_press(b);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { dp_press(DP_OK); return 1; }
        if (w2k_tabs_key(dp.tabs, &e->xkey)) { w2k_win_dirty(w); return 1; }
        if (dp.tabs->sel == 0 && dp.name->focused && w2k_edit_key(dp.name, &e->xkey)) {
            if (d && strcmp(w2k_edit_text(dp.name), dev_name(d))) dp.dirty = 1;
            w2k_win_dirty(w);
            return 1;
        }
        if (dp.tabs->sel == 1 && w2k_list_key(dp.svc, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void device_properties(W2kWin *over, const char *path_in)
{
    char path[128];
    snprintf(path, sizeof path, "%s", path_in);
    Dev *d = dev_find(path);
    if (!d || dp.w) return;
    memset(&dp, 0, sizeof dp);
    snprintf(dp.path, sizeof dp.path, "%s", path);
    char title[200];
    snprintf(title, sizeof title, "%s Properties", dev_name(d));
    dp.w = w2k_win_new(title, "l2kbluetooth", DP_W, DP_H, 0);
    dp.w->paint = dp_paint;
    dp.w->event = dp_event;
    dp.tabs = w2k_tabs_new(NULL, NULL);
    w2k_tabs_add(dp.tabs, "General");
    w2k_tabs_add(dp.tabs, "Services");
    dp.tabs->r = (W2kRect){ 6, 6, DP_W - 12, DP_H - 6 - 40 };
    W2kRect c = w2k_tabs_client(dp.tabs);
    dp.name = w2k_edit_new(0);
    w2k_edit_bind(dp.name, dp.w);
    w2k_edit_set(dp.name, dev_name(d));
    dp.name->r = (W2kRect){ c.x + 60, c.y + 20, c.w - 74, 21 };
    dp.trusted = d->trusted;
    dp.blocked = d->blocked;
    int fh = w2k_font_height(F_UI);
    dp.cb_block = (W2kRect){ c.x + 14, c.y + c.h - 14 - 23 - 12 - 14, 180, fh > 13 ? fh : 13 };
    dp.cb_trust = (W2kRect){ c.x + 14, dp.cb_block.y - 22, c.w - 28, fh > 13 ? fh : 13 };
    dp.b[DP_OK]      = (W2kRect){ DP_W - 8 - 75 * 3 - 12, DP_H - 8 - 23, 75, 23 };
    dp.b[DP_CANCEL]  = (W2kRect){ DP_W - 8 - 75 * 2 - 6, DP_H - 8 - 23, 75, 23 };
    dp.b[DP_APPLY]   = (W2kRect){ DP_W - 8 - 75, DP_H - 8 - 23, 75, 23 };
    dp.b[DP_CONNECT] = (W2kRect){ c.x + c.w - 14 - 90, c.y + c.h - 14 - 23, 90, 23 };
    dp.b[DP_SDISCONNECT] = (W2kRect){ c.x + c.w - 12 - 90, c.y + c.h - 12 - 23, 90, 23 };
    dp.b[DP_SCONNECT]    = (W2kRect){ c.x + c.w - 12 - 90 * 2 - 6, c.y + c.h - 12 - 23, 90, 23 };
    dp.svc = w2k_list_new(LV_REPORT);
    dp.svc->fullrow = 1;
    w2k_scroll_bind(&dp.svc->vsb, dp.w);
    w2k_list_add_col(dp.svc, "Service", c.w - 24 - 4, 0);
    dp.svc->r = (W2kRect){ c.x + 12, c.y + 12 + 2 * fh + 10, c.w - 24, c.h - (12 + 2 * fh + 10) - 12 - 23 - 10 };
    dp_fill_services();
    if (getenv("W2K_RENDER_TAB")) dp.tabs->sel = atoi(getenv("W2K_RENDER_TAB")) == 1;
    w2k_win_center(dp.w, over);
    dialog_hints(dp.w, over);
    w2k_add_timer(w2k_caret_blink, dp_blink, dp.name);
    w2k_win_modal(dp.w);
    w2k_del_timer(dp_blink, dp.name);
    dp.w = NULL;
    w2k_edit_free(dp.name);
    w2k_list_free(dp.svc);
    w2k_tabs_free(dp.tabs);
}

/* ------------------------------------------------------------------ *
 * A radio's properties: General and Advanced
 * ------------------------------------------------------------------ */
/* Where the radio is and what drives it, from sysfs. */
static void ad_sysfs(const Adapter *a, char *loc, int nl, char *drv, int nd)
{
    const char *hci = strrchr(a->path, '/');
    hci = hci ? hci + 1 : a->path;
    char p[256], link[512], sub[512];
    snprintf(loc, (size_t)nl, "Unknown");
    snprintf(drv, (size_t)nd, "Unknown");
    snprintf(p, sizeof p, "/sys/class/bluetooth/%s/device/driver", hci);
    ssize_t n = readlink(p, link, sizeof link - 1);
    if (n > 0) { link[n] = 0; const char *b = strrchr(link, '/'); snprintf(drv, (size_t)nd, "%s", b ? b + 1 : link); }
    snprintf(p, sizeof p, "/sys/class/bluetooth/%s/device/subsystem", hci);
    n = readlink(p, sub, sizeof sub - 1);
    sub[n > 0 ? n : 0] = 0;
    snprintf(p, sizeof p, "/sys/class/bluetooth/%s/device", hci);
    n = readlink(p, link, sizeof link - 1);
    if (n <= 0) return;
    link[n] = 0;
    const char *base = strrchr(link, '/');
    base = base ? base + 1 : link;
    const char *s = strrchr(sub, '/');
    s = s ? s + 1 : sub;
    int bus_no, dev_no, fn;
    char port[64];
    if (!strcmp(s, "usb") && sscanf(base, "%d-%63[0-9.]", &bus_no, port) == 2)
        snprintf(loc, (size_t)nl, "USB bus %d, port %s", bus_no, port);
    else if (!strcmp(s, "pci") && sscanf(base, "%*x:%x:%x.%x", &bus_no, &dev_no, &fn) == 3)
        snprintf(loc, (size_t)nl, "PCI bus %d, device %d, function %d", bus_no, dev_no, fn);
    else if (!strcmp(s, "sdio") || !strcmp(s, "mmc"))
        snprintf(loc, (size_t)nl, "On the SDIO bus");
    else if (*s)
        snprintf(loc, (size_t)nl, "Built in (%s)", s);
}

static void ad_status(const Adapter *a, char *out, int n)
{
    if (a->powered) snprintf(out, (size_t)n, "This device is working properly.");
    else if (strstr(a->power_state, "blocked"))
        snprintf(out, (size_t)n, "This device is switched off by the computer's wireless switch or airplane mode.");
    else snprintf(out, (size_t)n, "This device is turned off. To turn it on, use the Options page.");
}

enum { AP_OK = 1, AP_CANCEL, NAP };

static struct {
    W2kWin  *w;
    W2kTabs *tabs;
    char     path[64];
    W2kEdit *name;
    W2kRect  b[NAP];
    int      down;
} ap;

#define AP_W 380
#define AP_H 400

static void ap_paint(W2kWin *w, Drawable d)
{
    (void)w;
    Adapter *a = ad_find(ap.path);
    int fh = w2k_font_height(F_UI);
    w2k_tabs_draw(d, ap.tabs);
    W2kRect c = w2k_tabs_client(ap.tabs);
    char loc[128], drv[64], t[256];
    if (a) ad_sysfs(a, loc, sizeof loc, drv, sizeof drv);
    if (ap.tabs->sel == 0) {
        w2k_bigicon_draw(d, c.x + 14, c.y + 14, ICO_CP_BLUETOOTH);
        w2k_text(d, F_UI, c.x + 60, c.y + 24, a ? (a->alias[0] ? a->alias : a->name) : "", C_TEXT);
        int y = c.y + 66, vx = c.x + 110;
        const char *mf = a ? company(a->manufacturer) : NULL;
        w2k_text(d, F_UI, c.x + 14, y, "Device type:", C_TEXT);
        w2k_text(d, F_UI, vx, y, "Bluetooth Radios", C_TEXT);                   y += fh + 8;
        w2k_text(d, F_UI, c.x + 14, y, "Manufacturer:", C_TEXT);
        if (!mf && a) snprintf(t, sizeof t, "Company %d", a->manufacturer);
        w2k_text(d, F_UI, vx, y, mf ? mf : a ? t : "", C_TEXT);                  y += fh + 8;
        w2k_text(d, F_UI, c.x + 14, y, "Location:", C_TEXT);
        w2k_text(d, F_UI, vx, y, a ? loc : "", C_TEXT);                          y += fh + 16;
        W2kRect g = { c.x + 10, y, c.w - 20, 100 };
        w2k_draw_groupbox(d, &g, "Device status");
        W2kRect well = { g.x + 12, g.y + 20, g.w - 24, g.h - 32 };
        w2k_draw_well(d, &well);
        if (a) ad_status(a, t, sizeof t);
        else snprintf(t, sizeof t, "This device is not present.");
        para(d, F_UI, well.x + 6, well.y + 5, well.w - 12, t, C_WINDOWTEXT);
    } else {
        w2k_text_mnemonic(d, F_UI, c.x + 14, c.y + 18, "Computer &name:", C_TEXT, 1);
        w2k_edit_draw(d, ap.name);
        para(d, F_UI, c.x + 14, c.y + 72, c.w - 28,
             "Other Bluetooth devices see this computer by this name when they search for it.", C_TEXT);
        int y = c.y + 118, vx = c.x + 130;
        W2kRect g = { c.x + 10, y, c.w - 20, 5 * (fh + 8) + 24 };
        w2k_draw_groupbox(d, &g, "Radio information");
        y += 22;
        if (a) {
            const char *v = bt_version(a->version);
            w2k_text(d, F_UI, c.x + 22, y, "Address:", C_TEXT);
            w2k_text(d, F_UI, vx, y, a->address, C_TEXT);                        y += fh + 8;
            w2k_text(d, F_UI, c.x + 22, y, "Bluetooth version:", C_TEXT);
            w2k_text(d, F_UI, vx, y, v ? v : "Unknown", C_TEXT);                 y += fh + 8;
            w2k_text(d, F_UI, c.x + 22, y, "Driver:", C_TEXT);
            w2k_text(d, F_UI, vx, y, drv, C_TEXT);                               y += fh + 8;
            w2k_text(d, F_UI, c.x + 22, y, "Visible to others:", C_TEXT);
            w2k_text(d, F_UI, vx, y, a->discoverable ? "Yes" : "No", C_TEXT);    y += fh + 8;
            w2k_text(d, F_UI, c.x + 22, y, "Devices connected:", C_TEXT);
            int n = 0;
            for (int i = 0; i < ndevs; i++) n += devs[i].connected && !strcmp(devs[i].adapter, a->path);
            snprintf(t, sizeof t, "%d", n);
            w2k_text(d, F_UI, vx, y, t, C_TEXT);
        }
    }
    w2k_draw_pushbutton(d, &ap.b[AP_OK], "OK", BS_DEFAULT | (ap.down == AP_OK ? BS_PRESSED : 0));
    w2k_draw_pushbutton(d, &ap.b[AP_CANCEL], "Cancel", ap.down == AP_CANCEL ? BS_PRESSED : 0);
}

static void ap_ok(void)
{
    Adapter *a = ad_find(ap.path);
    const char *nm = w2k_edit_text(ap.name);
    if (a && *nm && strcmp(nm, a->alias)) set_str(a->path, IF_ADAPTER, "Alias", nm, done_report, "rename this computer");
    w2k_win_close(ap.w, ID_OK);
}

static int ap_event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress:
        if (w2k_tabs_press(ap.tabs, &e->xbutton)) { ap.name->focused = ap.tabs->sel == 1; w2k_win_dirty(w); return 1; }
        if (ap.tabs->sel == 1 && w2k_edit_press(ap.name, &e->xbutton)) { w2k_win_dirty(w); return 1; }
        for (int b = 1; b < NAP; b++)
            if (w2k_rect_hit(&ap.b[b], e->xbutton.x, e->xbutton.y)) ap.down = b;
        w2k_win_dirty(w);
        return 1;
    case MotionNotify:
        if (ap.tabs->sel == 1 && w2k_edit_motion(ap.name, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        w2k_edit_release(ap.name);
        int b = ap.down;
        ap.down = 0;
        if (b == AP_OK && w2k_rect_hit(&ap.b[b], e->xbutton.x, e->xbutton.y)) ap_ok();
        if (b == AP_CANCEL && w2k_rect_hit(&ap.b[b], e->xbutton.x, e->xbutton.y)) w2k_win_close(w, ID_CANCEL);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { ap_ok(); return 1; }
        if (w2k_tabs_key(ap.tabs, &e->xkey)) { ap.name->focused = ap.tabs->sel == 1; w2k_win_dirty(w); return 1; }
        if (ap.tabs->sel == 1 && w2k_edit_key(ap.name, &e->xkey)) { w2k_win_dirty(w); return 1; }
        return 1;
    }
    }
    return 0;
}

static void adapter_properties(W2kWin *over, const char *path)
{
    Adapter *a = ad_find(path);
    if (!a || ap.w) return;
    memset(&ap, 0, sizeof ap);
    snprintf(ap.path, sizeof ap.path, "%s", path);
    char title[200];
    snprintf(title, sizeof title, "%s Properties", a->alias[0] ? a->alias : "Bluetooth Radio");
    ap.w = w2k_win_new(title, "l2kbluetooth", AP_W, AP_H, 0);
    ap.w->paint = ap_paint;
    ap.w->event = ap_event;
    ap.tabs = w2k_tabs_new(NULL, NULL);
    w2k_tabs_add(ap.tabs, "General");
    w2k_tabs_add(ap.tabs, "Advanced");
    ap.tabs->r = (W2kRect){ 6, 6, AP_W - 12, AP_H - 6 - 40 };
    W2kRect c = w2k_tabs_client(ap.tabs);
    ap.name = w2k_edit_new(0);
    w2k_edit_bind(ap.name, ap.w);
    w2k_edit_set(ap.name, a->alias);
    ap.name->r = (W2kRect){ c.x + 14, c.y + 38, c.w - 28, 21 };
    ap.b[AP_OK]     = (W2kRect){ AP_W - 8 - 75 * 2 - 6, AP_H - 8 - 23, 75, 23 };
    ap.b[AP_CANCEL] = (W2kRect){ AP_W - 8 - 75, AP_H - 8 - 23, 75, 23 };
    if (getenv("W2K_RENDER_TAB")) ap.tabs->sel = atoi(getenv("W2K_RENDER_TAB")) == 1;
    ap.name->focused = ap.tabs->sel == 1;
    w2k_win_center(ap.w, over);
    dialog_hints(ap.w, over);
    w2k_add_timer(w2k_caret_blink, dp_blink, ap.name);
    w2k_win_modal(ap.w);
    w2k_del_timer(dp_blink, ap.name);
    ap.w = NULL;
    w2k_edit_free(ap.name);
    w2k_tabs_free(ap.tabs);
}

/* ------------------------------------------------------------------ *
 * The sheet: Devices, Options, Hardware
 * ------------------------------------------------------------------ */
enum { SH_OK = 1, SH_CANCEL, SH_APPLY, SH_ADD, SH_REMOVE, SH_CONNECT, SH_PROPS,
       SH_DEFAULTS, SH_TROUBLE, SH_HWPROPS, NSH };
enum { OC_POWER, OC_DISC, OC_ALLOW, OC_ALERT, OC_TRAY, NOC };

static struct {
    W2kWin  *w;
    W2kTabs *tabs;
    DevView  dv;
    W2kList *hw;
    W2kRect  b[NSH];
    int      down;
    W2kRect  oc[NOC], og[3];        /* the Options page's check boxes and groups */
    int      ov[NOC];
    int      odirty;
} sh;

#define SH_W 410
#define SH_H 452

static void wizard(W2kWin *over);

static void options_load(void)
{
    Adapter *a = adapter();
    sh.ov[OC_POWER] = a && a->powered;
    sh.ov[OC_DISC]  = a && a->discoverable;
    sh.ov[OC_ALLOW] = opt.allow;
    sh.ov[OC_ALERT] = opt.alert;
    sh.ov[OC_TRAY]  = opt.tray;
}

static int oc_enabled(int i)
{
    Adapter *a = adapter();
    switch (i) {
    case OC_POWER: return a != NULL;
    case OC_DISC:  return a && sh.ov[OC_POWER];
    case OC_ALLOW: return a != NULL;
    case OC_ALERT: return sh.ov[OC_ALLOW];
    }
    return 1;
}

static int pending_disc = -1;       /* discovery to set once the radio is on */

static void set_discovery(Adapter *a, int on)
{
    if (on) {
        /* On until turned off, as the check box says, and devices that
         * find this computer may pair with it. */
        set_u32(a->path, IF_ADAPTER, "DiscoverableTimeout", 0, done_quiet, NULL);
        set_bool(a->path, IF_ADAPTER, "Pairable", 1, done_quiet, NULL);
    }
    set_bool(a->path, IF_ADAPTER, "Discoverable", on, done_report,
             on ? "turn discovery on" : "turn discovery off");
    if (!on) set_bool(a->path, IF_ADAPTER, "Pairable", 0, done_quiet, NULL);
}

static void options_apply(void)
{
    Adapter *a = adapter();
    if (a && !demo) {
        int on = sh.ov[OC_POWER];
        pending_disc = -1;
        if (on != a->powered) set_power(a, on);
        /* A radio still coming on would refuse: wait for it. */
        if (on && !a->powered && sh.ov[OC_DISC]) pending_disc = 1;
        else if (on && sh.ov[OC_DISC] != a->discoverable) set_discovery(a, sh.ov[OC_DISC]);
        if (on && sh.ov[OC_ALLOW] != opt.allow)
            set_bool(a->path, IF_ADAPTER, "Connectable", sh.ov[OC_ALLOW], done_report,
                     sh.ov[OC_ALLOW] ? "let devices connect" : "stop devices connecting");
    }
    opt.allow = sh.ov[OC_ALLOW];
    opt.alert = sh.ov[OC_ALERT];
    opt.tray  = sh.ov[OC_TRAY];
    opt_save();
    sh.odirty = 0;
}

static const char *const oc_label[NOC] = {
    "&Turn on Bluetooth", "Turn &discovery on",
    "A&llow Bluetooth devices to connect to this computer",
    "Alert &me when a Bluetooth device wants to connect",
    "&Show the Bluetooth icon in the notification area" };

static const char *const og_text[2] = {
    "When Bluetooth is off, this computer cannot find or use Bluetooth devices.",
    "Bluetooth devices can then find this computer and ask to pair with it. "
    "To protect your privacy, turn discovery on only when you want a device "
    "to find this computer." };

static void options_layout(void)
{
    W2kRect c = w2k_tabs_client(sh.tabs);
    int fh = w2k_font_height(F_UI), ch = fh > 13 ? fh : 13;
    int gw = c.w - 20, tw = gw - 24 - 18;
    int y = c.y + 10;
    for (int g = 0; g < 3; g++) {
        int h;
        if (g < 2) h = 20 + ch + 6 + (para(0, F_UI, 0, 0, tw, og_text[g], 0)) + 12;
        else h = 20 + ch * 2 + 10 + 12;
        sh.og[g] = (W2kRect){ c.x + 10, y, gw, h };
        y += h + 10;
    }
    for (int i = 0; i < NOC; i++) {
        int x, yy;
        if (i < 2)       { x = sh.og[i].x + 12; yy = sh.og[i].y + 20; }
        else if (i < 4)  { x = sh.og[2].x + 12; yy = sh.og[2].y + 20 + (i - 2) * (ch + 10); }
        else             { x = c.x + 22; yy = sh.og[2].y + sh.og[2].h + 14; }
        sh.oc[i] = (W2kRect){ x, yy, 13 + 5 + w2k_mnemonic_width(F_UI, oc_label[i]) + 2, ch };
    }
}

static void hw_fill(void)
{
    int keep = sh.hw->sel;
    w2k_list_clear(sh.hw);
    for (int i = 0; i < nads; i++) {
        int r = w2k_list_add(sh.hw, ICO_CP_BLUETOOTH, (void *)(intptr_t)i);
        w2k_list_set(sh.hw, r, 0, ads[i].alias[0] ? ads[i].alias : ads[i].name[0] ? ads[i].name : ads[i].address);
        w2k_list_set(sh.hw, r, 1, "Bluetooth Radios");
    }
    if (keep < 0 || keep >= sh.hw->n) keep = sh.hw->n ? 0 : -1;
    if (keep >= 0) { sh.hw->sel = keep; sh.hw->items[keep].selected = 1; }
    w2k_list_layout(sh.hw);
}

static Adapter *hw_selected(void)
{
    if (sh.hw->sel < 0 || sh.hw->sel >= sh.hw->n) return NULL;
    int i = (int)(intptr_t)sh.hw->items[sh.hw->sel].data;
    return i < nads ? &ads[i] : NULL;
}

static void sh_empty_text(void)
{
    Adapter *a = adapter();
    const char *t =
        !demo && !bus  ? "Bluetooth is not available: this program could not reach the system bus." :
        !demo && !bluez_up ? "The Bluetooth service (bluetoothd) is not running, so this computer cannot use "
                             "Bluetooth devices. Start it -- \"systemctl start bluetooth\", as the "
                             "administrator -- and this page fills in." :
        !a             ? "No Bluetooth radio was found in this computer.\n\nIf it has one, it may be "
                         "switched off in the computer's setup program, or by a switch or a key on the computer." :
        !a->powered    ? "Bluetooth is turned off.\n\nTo use Bluetooth devices, turn it on, on the Options page." :
                         "No Bluetooth devices have been added to this computer.\n\nTo add one, click Add.";
    snprintf(sh.dv.empty, sizeof sh.dv.empty, "%s", t);
}

static int sh_on_page(int b)
{
    int p = sh.tabs->sel;
    switch (b) {
    case SH_OK: case SH_CANCEL: case SH_APPLY: return 1;
    case SH_ADD: case SH_REMOVE: case SH_CONNECT: case SH_PROPS: return p == 0;
    case SH_DEFAULTS: return p == 1;
    case SH_TROUBLE: case SH_HWPROPS: return p == 2;
    }
    return 0;
}

static int sh_enabled(int b)
{
    const Dev *d = dv_selected(&sh.dv);
    switch (b) {
    case SH_APPLY:   return sh.odirty;
    case SH_ADD:     return demo || (bluez_up && nads);
    case SH_REMOVE:  return d && d->busy != BUSY_REMOVE;
    case SH_PROPS:   return d != NULL;
    case SH_CONNECT: return d && !d->busy;
    case SH_TROUBLE: return 0;
    case SH_HWPROPS: return hw_selected() != NULL;
    }
    return 1;
}

static const char *sh_label(int b)
{
    static const char *const t[NSH] = {
        [SH_OK] = "OK", [SH_CANCEL] = "Cancel", [SH_APPLY] = "&Apply", [SH_ADD] = "A&dd...",
        [SH_REMOVE] = "&Remove", [SH_PROPS] = "P&roperties", [SH_DEFAULTS] = "R&estore Defaults",
        [SH_TROUBLE] = "&Troubleshoot...", [SH_HWPROPS] = "P&roperties" };
    if (b == SH_CONNECT) {
        const Dev *d = dv_selected(&sh.dv);
        return d && d->connected ? "Dis&connect" : "&Connect";
    }
    return t[b];
}

static void sh_paint(W2kWin *w, Drawable d)
{
    (void)w;
    int fh = w2k_font_height(F_UI);
    w2k_tabs_draw(d, sh.tabs);
    W2kRect c = w2k_tabs_client(sh.tabs);
    int page = sh.tabs->sel;
    if (page == 0) {
        dv_draw(d, &sh.dv);
    } else if (page == 1) {
        static const char *const gt[3] = { "Bluetooth radio", "Discovery", "Connections" };
        for (int g = 0; g < 3; g++) {
            w2k_draw_groupbox(d, &sh.og[g], gt[g]);
            if (g < 2)
                para(d, F_UI, sh.og[g].x + 12 + 18, sh.oc[g].y + sh.oc[g].h + 6, sh.og[g].w - 24 - 18,
                     og_text[g], oc_enabled(g) ? C_TEXT : C_GRAYTEXT);
        }
        for (int i = 0; i < NOC; i++)
            w2k_draw_checkbox(d, sh.oc[i].x, sh.oc[i].y, oc_label[i], sh.ov[i], 0, !oc_enabled(i));
    } else {
        w2k_text(d, F_UI, c.x + 10, c.y + 12, "Devices:", C_TEXT);
        w2k_list_draw(d, sh.hw);
        W2kRect g = { c.x + 10, sh.hw->r.y + sh.hw->r.h + 14, c.w - 20, 0 };
        g.h = c.y + c.h - 10 - g.y;
        w2k_draw_groupbox(d, &g, "Device Properties");
        Adapter *a = hw_selected();
        int y = g.y + 22, vx = g.x + 100;
        if (a) {
            char loc[128], drv[64], t[256];
            ad_sysfs(a, loc, sizeof loc, drv, sizeof drv);
            const char *mf = company(a->manufacturer);
            if (!mf) snprintf(t, sizeof t, "Company %d", a->manufacturer);
            w2k_text(d, F_UI, g.x + 12, y, "Manufacturer:", C_TEXT);
            w2k_text(d, F_UI, vx, y, mf ? mf : t, C_TEXT);                        y += fh + 6;
            w2k_text(d, F_UI, g.x + 12, y, "Location:", C_TEXT);
            w2k_text(d, F_UI, vx, y, loc, C_TEXT);                                 y += fh + 6;
            w2k_text(d, F_UI, g.x + 12, y, "Device status:", C_TEXT);
            ad_status(a, t, sizeof t);
            para(d, F_UI, vx, y, g.x + g.w - 12 - vx, t, C_TEXT);
        } else {
            para(d, F_UI, g.x + 12, y, g.w - 24, "No Bluetooth radio was found in this computer.", C_TEXT);
        }
    }
    for (int b = 1; b < NSH; b++) {
        if (!sh_on_page(b)) continue;
        w2k_draw_pushbutton(d, &sh.b[b], sh_label(b), (b == SH_OK ? BS_DEFAULT : 0) |
                            (sh.down == b ? BS_PRESSED : 0) | (sh_enabled(b) ? 0 : BS_DISABLED));
    }
}

enum { DM_CONNECT = 1, DM_RENAME, DM_REMOVE, DM_PROPS, DM_ADD };

static void sh_activate(void *u)
{
    (void)u;
    const Dev *d = dv_selected(&sh.dv);
    if (d) dev_toggle(sh.w, d->path);
}

static void sh_menu(void *u, int rx, int ry)
{
    (void)u;
    const Dev *d = dv_selected(&sh.dv);
    W2kMenu *m = w2k_menu_new();
    char path[128] = "";
    if (d) {
        snprintf(path, sizeof path, "%s", d->path);
        w2k_menu_item(m, DM_CONNECT, d->connected ? "&Disconnect" : "&Connect", NULL, ICO_NONE);
        w2k_menu_default(m);
        if (d->busy) w2k_menu_disable(m);
        w2k_menu_sep(m);
        w2k_menu_item(m, DM_RENAME, "Re&name", NULL, ICO_NONE);
        w2k_menu_item(m, DM_REMOVE, "&Remove", NULL, ICO_NONE);
        w2k_menu_sep(m);
        w2k_menu_item(m, DM_PROPS, "P&roperties", NULL, ICO_NONE);
    } else {
        w2k_menu_item(m, DM_ADD, "A&dd a Bluetooth Device...", NULL, ICO_NONE);
        if (!sh_enabled(SH_ADD)) w2k_menu_disable(m);
    }
    int id = w2k_menu_popup(m, rx, ry, 0);
    w2k_menu_free(m);
    switch (id) {
    case DM_CONNECT: dev_toggle(sh.w, path); break;
    case DM_RENAME:  dev_rename(sh.w, path); break;
    case DM_REMOVE:  dev_forget(sh.w, path); break;
    case DM_PROPS:   device_properties(sh.w, path); break;
    case DM_ADD:     wizard(sh.w); break;
    }
    w2k_win_dirty(sh.w);
}

static void sh_press(int b)
{
    const Dev *d = dv_selected(&sh.dv);
    char path[128];
    snprintf(path, sizeof path, "%s", d ? d->path : "");
    switch (b) {
    case SH_OK:       if (sh.odirty) options_apply(); w2k_win_close(sh.w, ID_OK); break;
    case SH_CANCEL:   w2k_win_close(sh.w, ID_CANCEL); break;
    case SH_APPLY:    options_apply(); break;
    case SH_ADD:      wizard(sh.w); break;
    case SH_REMOVE:   dev_forget(sh.w, path); break;
    case SH_CONNECT:  dev_toggle(sh.w, path); break;
    case SH_PROPS:    device_properties(sh.w, path); break;
    case SH_DEFAULTS:
        sh.ov[OC_POWER] = 1; sh.ov[OC_DISC] = 0; sh.ov[OC_ALLOW] = 1;
        sh.ov[OC_ALERT] = 1; sh.ov[OC_TRAY] = 1;
        sh.odirty = 1;
        break;
    case SH_HWPROPS: { Adapter *a = hw_selected(); if (a) { char p[64]; snprintf(p, sizeof p, "%s", a->path); adapter_properties(sh.w, p); } break; }
    }
    w2k_win_dirty(sh.w);
}

/* Alt and a letter presses the button, or ticks the box, it underlines. */
static int sh_mnemonic(XKeyEvent *k)
{
    char ch[4];
    if (XLookupString(k, ch, sizeof ch, NULL, NULL) != 1) return 0;
    int c = tolower((unsigned char)ch[0]);
    for (int b = 1; b < NSH; b++) {
        const char *l = sh_label(b), *amp = strchr(l, '&');
        if (sh_on_page(b) && sh_enabled(b) && amp && tolower((unsigned char)amp[1]) == c) { sh_press(b); return 1; }
    }
    if (sh.tabs->sel == 1)
        for (int i = 0; i < NOC; i++) {
            const char *amp = strchr(oc_label[i], '&');
            if (amp && tolower((unsigned char)amp[1]) == c && oc_enabled(i)) {
                sh.ov[i] = !sh.ov[i];
                sh.odirty = 1;
                return 1;
            }
        }
    return 0;
}

static int sh_event(W2kWin *w, XEvent *e)
{
    int page = sh.tabs->sel;
    switch (e->type) {
    case ButtonPress: {
        XButtonEvent *b = &e->xbutton;
        if (w2k_tabs_press(sh.tabs, b)) { w2k_win_dirty(w); return 1; }
        if (page == 0 && dv_press(&sh.dv, b)) { w2k_win_dirty(w); return 1; }
        if (page == 2 && w2k_list_press(sh.hw, b)) {
            if (b->button == Button1 && sh.hw->sel >= 0) {
                static long last;
                if ((long)b->time - last < w2k_dblclk_ms) { last = 0; sh_press(SH_HWPROPS); }
                else last = (long)b->time;
            }
            w2k_win_dirty(w);
            return 1;
        }
        if (page == 1 && b->button == Button1)
            for (int i = 0; i < NOC; i++)
                if (oc_enabled(i) && w2k_rect_hit(&sh.oc[i], b->x, b->y)) { sh.ov[i] = !sh.ov[i]; sh.odirty = 1; }
        if (b->button == Button1)
            for (int i = 1; i < NSH; i++)
                if (sh_on_page(i) && sh_enabled(i) && w2k_rect_hit(&sh.b[i], b->x, b->y)) sh.down = i;
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (page == 0 && dv_motion(&sh.dv, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        if (page == 2 && w2k_list_motion(sh.hw, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        dv_release(&sh.dv);
        w2k_list_release(sh.hw, &e->xbutton);
        int b = sh.down;
        sh.down = 0;
        if (b && w2k_rect_hit(&sh.b[b], e->xbutton.x, e->xbutton.y)) sh_press(b);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        XKeyEvent *k = &e->xkey;
        KeySym ks = XLookupKeysym(k, 0);
        if (ks == XK_Escape) { w2k_win_close(w, ID_CANCEL); return 1; }
        if (w2k_tabs_key(sh.tabs, k)) { w2k_win_dirty(w); return 1; }
        if (k->state & Mod1Mask) { if (sh_mnemonic(k)) w2k_win_dirty(w); return 1; }
        if (page == 0) {
            if (ks == XK_Delete && dv_selected(&sh.dv)) { sh_press(SH_REMOVE); return 1; }
            if (ks == XK_F2 && dv_selected(&sh.dv)) { dev_rename(sh.w, dv_selected(&sh.dv)->path); return 1; }
            if (ks == XK_F10 && (k->state & ShiftMask)) ks = XK_Menu, k->keycode = XKeysymToKeycode(w2k.dpy, XK_Menu);
            int was = sh.dv.focused;
            sh.dv.focused = 1;
            if (dv_key(&sh.dv, k)) { w2k_win_dirty(w); return 1; }
            sh.dv.focused = was;
        }
        if (page == 2 && w2k_list_key(sh.hw, k)) { w2k_win_dirty(w); return 1; }
        if (ks == XK_Return || ks == XK_KP_Enter) { sh_press(SH_OK); return 1; }
        if (sh_mnemonic(k)) w2k_win_dirty(w);
        return 1;
    }
    }
    return 0;
}

static void sheet(void)
{
    memset(&sh, 0, sizeof sh);
    sh.w = w2k_win_new("Bluetooth Devices", "l2kbluetooth", SH_W, SH_H, 0);
    sh.w->paint = sh_paint;
    sh.w->event = sh_event;
    sh.tabs = w2k_tabs_new(NULL, NULL);
    w2k_tabs_add(sh.tabs, "Devices");
    w2k_tabs_add(sh.tabs, "Options");
    w2k_tabs_add(sh.tabs, "Hardware");
    sh.tabs->r = (W2kRect){ 6, 6, SH_W - 12, SH_H - 6 - 40 };
    W2kRect c = w2k_tabs_client(sh.tabs);

    sh.dv.r = (W2kRect){ c.x + 10, c.y + 10, c.w - 20, c.h - 10 - 12 - 23 - 12 };
    sh.dv.grouped = 1;
    sh.dv.sel = -1;
    sh.dv.focused = 1;
    sh.dv.owner = sh.w;
    sh.dv.on_activate = sh_activate;
    sh.dv.on_menu = sh_menu;
    w2k_scroll_bind(&sh.dv.sb, sh.w);
    int by = c.y + c.h - 12 - 23;
    sh.b[SH_ADD]     = (W2kRect){ c.x + 10, by, 75, 23 };
    sh.b[SH_REMOVE]  = (W2kRect){ c.x + 10 + 81, by, 75, 23 };
    sh.b[SH_CONNECT] = (W2kRect){ c.x + 10 + 162, by, 75, 23 };
    sh.b[SH_PROPS]   = (W2kRect){ c.x + c.w - 10 - 75, by, 75, 23 };
    sh.b[SH_DEFAULTS] = (W2kRect){ c.x + c.w - 10 - 110, by, 110, 23 };

    sh.hw = w2k_list_new(LV_REPORT);
    sh.hw->fullrow = 1;
    w2k_scroll_bind(&sh.hw->vsb, sh.w);
    w2k_list_add_col(sh.hw, "Name", c.w - 20 - 130 - 4, 0);
    w2k_list_add_col(sh.hw, "Type", 130, 0);
    sh.hw->r = (W2kRect){ c.x + 10, c.y + 28, c.w - 20, 96 };
    sh.b[SH_HWPROPS] = (W2kRect){ c.x + c.w - 22 - 75, c.y + c.h - 22 - 23, 75, 23 };
    sh.b[SH_TROUBLE] = (W2kRect){ c.x + c.w - 22 - 75 - 6 - 100, c.y + c.h - 22 - 23, 100, 23 };

    sh.b[SH_OK]     = (W2kRect){ SH_W - 8 - 75 * 3 - 12, SH_H - 8 - 23, 75, 23 };
    sh.b[SH_CANCEL] = (W2kRect){ SH_W - 8 - 75 * 2 - 6, SH_H - 8 - 23, 75, 23 };
    sh.b[SH_APPLY]  = (W2kRect){ SH_W - 8 - 75, SH_H - 8 - 23, 75, 23 };

    options_layout();
    options_load();
    dv_fill(&sh.dv, dev_known);
    hw_fill();
    sh_empty_text();
    if (getenv("W2K_RENDER_TAB")) {
        int t = atoi(getenv("W2K_RENDER_TAB"));
        if (t >= 0 && t < 3) sh.tabs->sel = t;
        if (sh.dv.nrows > 1) dv_select(&sh.dv, 1);
    }
    w2k_win_center(sh.w, NULL);
    w2k_win_show(sh.w);
}

/* ------------------------------------------------------------------ *
 * The Add Bluetooth Device Wizard, in Windows 2000's wizard dress: the
 * welcome and the last page with the picture down the left, the pages
 * between with the white band across the top.
 * ------------------------------------------------------------------ */
enum { WP_WELCOME, WP_SEARCH, WP_PASSKEY, WP_PAIR, WP_DONE };
enum { PK_AUTO, PK_DOC, PK_OWN, PK_NONE };
enum { WB_BACK = 1, WB_NEXT, WB_CANCEL, WB_AGAIN, WB_YES, WB_NO, WB_ENTER, NWB };
enum { WS_PAIRING, WS_TRUSTING, WS_CONNECTING };

#define WZ_W   497
#define WZ_H   362
#define WZ_BAR (WZ_H - 47)          /* the etched line over the buttons */
#define WZ_X   40                   /* an inside page's margins */
#define WM_W   164                  /* the picture down the left */

static struct {
    W2kWin  *w;
    int      page;
    int      ready, showall;
    DevView  dv;
    char     sel[128];              /* the device being added */
    char     name[160];             /* its name, kept for the last page */
    int      icon;
    int      paged;                 /* the passkey page was shown */
    int      pk_mode;
    W2kEdit *pk_doc, *pk_own, *entry;
    char     autopin[20];           /* a passkey chosen for the user, shown */
    int      state;
    int      ok;
    char     result[480];
    int      want_search;           /* discovery waits for the radio to come on */
    long     search_end;
    int      phase;
    W2kRect  b[NWB], rb[4], cb_ready, cb_all;
    int      down;
    W2kSkin *water;
} wiz;

static int wiz_takes(const char *device)
{
    return wiz.w && wiz.page == WP_PAIR && !strcmp(device, wiz.sel);
}

/* The picture: a blue ground and the badge, large. */
static W2kSkin *watermark(int w, int h)
{
    unsigned char *img = w2k_alloc((size_t)w * (size_t)h * 4);
    float cx = w / 2.0f, cy = h * 0.36f, bw = 36, bh = 54, br = 34;
    float s = bh / 14.5f;
    static const float rune[6][2] = { { 10.5f, 10.5f }, { 21, 21 }, { 16, 26.2f },
                                      { 16, 5.8f }, { 21, 11 }, { 10.5f, 21.5f } };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float t = (float)y / h, u = (float)x / w;
            float r = 8 + 40 * t + 20 * u, g = 36 + 90 * t + 30 * u, b = 120 + 90 * t + 20 * u;
            float px = x + 0.5f, py = y + 0.5f;
            /* The badge's shadow, then the badge: a rounded box's distance. */
            for (int pass = 0; pass < 2; pass++) {
                float ox = pass ? 0 : 5, oy = pass ? 0 : 6;
                float qx = fabsf(px - cx - ox) - (bw - br), qy = fabsf(py - cy - oy) - (bh - br);
                float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
                float in = (qx > qy ? qx : qy);
                float dist = sqrtf(mx * mx + my * my) + (in < 0 ? in : 0) - br;
                if (!pass) {
                    float a = dist < 0 ? 0.35f : dist < 8 ? 0.35f * (1 - dist / 8) : 0;
                    r *= 1 - a; g *= 1 - a; b *= 1 - a;
                    continue;
                }
                float cov = 0.5f - dist;
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
                float k = (py - (cy - bh)) / (2 * bh) + (px - cx) / (4 * bw);
                if (k < 0) k = 0;
                if (k > 1) k = 1;
                float fr = 80 - 80 * k, fg = 150 - 110 * k, fb = 255 - 85 * k;
                if (dist > -2.5f) fr = 0, fg = 0, fb = 96;         /* the outline */
                r = r * (1 - cov) + fr * cov;
                g = g * (1 - cov) + fg * cov;
                b = b * (1 - cov) + fb * cov;
            }
            /* The rune, white. */
            float best = 1e9f;
            for (int i = 0; i < 5; i++) {
                float ax = cx + (rune[i][0] - 16) * s, ay = cy + (rune[i][1] - 16) * s;
                float bx = cx + (rune[i + 1][0] - 16) * s, by = cy + (rune[i + 1][1] - 16) * s;
                float dx = bx - ax, dy = by - ay;
                float tt = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
                if (tt < 0) tt = 0;
                if (tt > 1) tt = 1;
                float ex = px - ax - tt * dx, ey = py - ay - tt * dy;
                float dd = sqrtf(ex * ex + ey * ey);
                if (dd < best) best = dd;
            }
            float cov = 0.5f - (best - 1.2f * s);
            if (cov > 0) {
                if (cov > 1) cov = 1;
                r = r * (1 - cov) + 255 * cov;
                g = g * (1 - cov) + 255 * cov;
                b = b * (1 - cov) + 255 * cov;
            }
            unsigned char *o = img + ((size_t)y * w + x) * 4;
            o[0] = (unsigned char)(r > 255 ? 255 : r < 0 ? 0 : r);
            o[1] = (unsigned char)(g > 255 ? 255 : g < 0 ? 0 : g);
            o[2] = (unsigned char)(b > 255 ? 255 : b < 0 ? 0 : b);
            o[3] = 255;
        }
    W2kSkin *sk = w2k_skin_from_rgba(img, w, h);
    free(img);
    return sk;
}

/* A wizard's title, in its own face, wrapped. */
static int title_para(Drawable d, int x, int y, int w, const char *t)
{
    if (!face_title) return para(d, F_UI_BOLD, x, y, w, t, C_WINDOWTEXT);
    int lh = w2k_face_height(face_title);
    const char *p = t;
    while (*p) {
        const char *end = p, *fit = NULL;
        for (;;) {
            const char *q = end;
            while (*q && *q != ' ') q++;
            if (fit && w2k_face_width(face_title, p, (int)(q - p)) > w) break;
            fit = q;
            if (!*q) break;
            end = q + 1;
        }
        char line[160];
        snprintf(line, sizeof line, "%.*s", (int)(fit - p), p);
        if (d) w2k_face_text(d, face_title, x, y, line, C_WINDOWTEXT);
        y += lh;
        p = *fit ? fit + 1 : fit;
    }
    return y;
}

static Adapter *wz_adapter(void)
{
    Dev *d = dev_find(wiz.sel);
    Adapter *a = d ? ad_find(d->adapter) : NULL;
    return a ? a : adapter();
}

/* ---- searching ----------------------------------------------------- */
static void done_discovery(Pending *p, const char *err, const char *msg)
{
    (void)p; (void)msg;
    if (err && !strstr(err, "InProgress")) {
        searching = 0;
        model_dirty = 1;
        bt_kick();
    }
}

static void search_start(void)
{
    Adapter *a = adapter();
    for (int i = 0; i < ndevs; i++) devs[i].seen = devs[i].has_rssi;
    searching = 1;
    wiz.search_end = w2k_now_ms() + 30000;
    model_dirty = 1;
    bt_kick();
    if (!a || demo) return;
    if (!a->powered) {
        wiz.want_search = 1;
        set_power(a, 1);
        return;
    }
    wiz.want_search = 0;
    call(a->path, IF_ADAPTER, "StartDiscovery", 10000, done_discovery, NULL);
}

static void search_stop(void)
{
    Adapter *a = adapter();
    wiz.want_search = 0;
    if (!searching) return;
    searching = 0;
    model_dirty = 1;
    bt_kick();
    if (a && !demo) call(a->path, IF_ADAPTER, "StopDiscovery", 10000, done_quiet, NULL);
}

static int wz_want(const Dev *d)
{
    if (!d->seen && !(demo && !dev_known(d))) return 0;
    if (wiz.showall) return 1;
    return !dev_known(d) && d->name[0];
}

/* ---- pairing ------------------------------------------------------- */
static void wz_go(int page);

static void wz_finish(int ok, const char *text)
{
    wiz.ok = ok;
    snprintf(wiz.result, sizeof wiz.result, "%s", text);
    wz_go(WP_DONE);
}

static void done_wconnect(Pending *p, const char *err, const char *msg)
{
    Dev *d = dev_find(p->path);
    if (d) { d->busy = BUSY_NONE; model_dirty = 1; bt_kick(); }
    if (!wiz.w || wiz.page != WP_PAIR || strcmp(p->path, wiz.sel)) return;
    char t[480], why[256];
    if (!err || strstr(err, "AlreadyConnected")) {
        snprintf(t, sizeof t, "%s was added to this computer and is connected.\n\n"
                 "It connects again by itself whenever it is on and near this computer. "
                 "To disconnect or remove it, use Bluetooth Devices in Control Panel.", wiz.name);
    } else if (strstr(msg, "profile-unavailable") || strstr(msg, "Protocol not available")) {
        snprintf(t, sizeof t, "%s was added to this computer.\n\n"
                 "It has nothing to connect to right away; the programs that use it connect "
                 "to it when they need it.", wiz.name);
    } else {
        explain(err, msg, why, sizeof why);
        snprintf(t, sizeof t, "%s was added to this computer, but it could not be connected just now.\n\n%s\n\n"
                 "It connects when it is turned on and near this computer, or from the Devices page "
                 "of Bluetooth Devices.", wiz.name, why);
    }
    wz_finish(1, t);
}

static void done_trust(Pending *p, const char *err, const char *msg)
{
    (void)err; (void)msg;
    if (!wiz.w || wiz.page != WP_PAIR || strcmp(p->path, wiz.sel)) return;
    Dev *d = dev_find(p->path);
    wiz.state = WS_CONNECTING;
    if (d) d->busy = BUSY_CONNECT;
    model_dirty = 1;
    call(p->path, IF_DEVICE, "Connect", 45000, done_wconnect, NULL);
}

static void done_pair(Pending *p, const char *err, const char *msg)
{
    Dev *d = dev_find(p->path);
    if (d) { d->busy = BUSY_NONE; model_dirty = 1; bt_kick(); }
    if (adisp.active && !strcmp(adisp.device, p->path)) {
        adisp.active = 0;
        if (adisp.dlg) w2k_win_close(adisp.dlg, ID_CANCEL);
    }
    if (!wiz.w || wiz.page != WP_PAIR || strcmp(p->path, wiz.sel)) return;
    if (err && !strstr(err, "AlreadyExists")) {
        char why[256], t[480];
        explain(err, msg, why, sizeof why);
        snprintf(t, sizeof t, "%s could not be added.\n\n%s\n\nTo try again, click Back. "
                 "Make sure the device is on and discoverable, and, if it asks for a passkey, "
                 "that the right one is used.", wiz.name, why);
        wz_finish(0, t);
        return;
    }
    /* Trusted, so it may reconnect without asking; then connected. */
    wiz.state = WS_TRUSTING;
    set_bool(p->path, IF_DEVICE, "Trusted", 1, done_trust, NULL);
}

static void pair_start(void)
{
    Dev *d = dev_find(wiz.sel);
    wiz.autopin[0] = 0;
    wiz.state = WS_PAIRING;
    if (!d) {
        wz_finish(0, "The device could not be added: it is no longer there. "
                     "Make sure it is on and discoverable, then click Back to search again.");
        return;
    }
    snprintf(wiz.name, sizeof wiz.name, "%s", dev_name(d));
    wiz.icon = dev_icon(d);
    if (demo) return;
    d->busy = BUSY_PAIR;
    model_dirty = 1;
    call(d->path, IF_DEVICE, "Pair", 120000, done_pair, NULL);
}

/* The passkey the wizard gives a device that asks for one. */
static const char *wz_pin(void)
{
    switch (wiz.pk_mode) {
    case PK_DOC:  return w2k_edit_text(wiz.pk_doc);
    case PK_OWN:  return w2k_edit_text(wiz.pk_own);
    case PK_NONE: return "0000";
    }
    if (!wiz.autopin[0]) {
        unsigned v = 0;
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, &v, sizeof v) != (ssize_t)sizeof v) v = (unsigned)time(NULL) * 2654435761u;
        if (fd >= 0) close(fd);
        snprintf(wiz.autopin, sizeof wiz.autopin, "%08u", v % 100000000u);
    }
    return wiz.autopin;
}

/* ---- pages --------------------------------------------------------- */
static void wz_go(int page)
{
    if (wiz.page == WP_SEARCH && page != WP_SEARCH) search_stop();
    wiz.page = page;
    wiz.down = 0;
    if (page == WP_SEARCH) search_start();
    if (page == WP_PAIR) pair_start();
    model_dirty = 1;
    bt_kick();
    if (wiz.w) w2k_win_dirty(wiz.w);
}

static int wz_enabled(int b)
{
    const Dev *d = dv_selected(&wiz.dv);
    switch (b) {
    case WB_BACK:
        return wiz.page == WP_SEARCH || wiz.page == WP_PASSKEY || (wiz.page == WP_DONE && !wiz.ok);
    case WB_NEXT:
        switch (wiz.page) {
        case WP_WELCOME: return wiz.ready;
        case WP_SEARCH:  return d && !dev_known(d);
        case WP_PASSKEY: return (wiz.pk_mode != PK_DOC || w2k_edit_text(wiz.pk_doc)[0]) &&
                                (wiz.pk_mode != PK_OWN || w2k_edit_text(wiz.pk_own)[0]);
        case WP_PAIR:    return 0;
        }
        return 1;
    case WB_CANCEL: return wiz.page != WP_DONE;
    case WB_AGAIN:  return !searching;
    case WB_ENTER:  return w2k_edit_text(wiz.entry)[0] != 0;
    }
    return 1;
}

static int wz_on_page(int b)
{
    if (b <= WB_CANCEL) return 1;
    if (b == WB_AGAIN) return wiz.page == WP_SEARCH;
    if (wiz.page != WP_PAIR || !areq.msg || !wiz_takes(areq.device)) return 0;
    if (b == WB_YES || b == WB_NO) return areq.kind == AR_CONFIRM;
    if (b == WB_ENTER) return areq.kind == AR_PASSKEY || areq.kind == AR_PIN;
    return 0;
}

static void wz_layout(void)
{
    int fh = w2k_font_height(F_UI), ch = fh > 13 ? fh : 13;
    int by = WZ_BAR + 12;
    wiz.b[WB_CANCEL] = (W2kRect){ WZ_W - 11 - 75, by, 75, 23 };
    wiz.b[WB_NEXT]   = (W2kRect){ WZ_W - 11 - 75 - 8 - 75, by, 75, 23 };
    wiz.b[WB_BACK]   = (W2kRect){ WZ_W - 11 - 75 - 8 - 150, by, 75, 23 };
    wiz.dv.r = (W2kRect){ WZ_X, 72, WZ_W - 2 * WZ_X, 166 };
    wiz.cb_all = (W2kRect){ WZ_X, 250, 13 + 5 + w2k_mnemonic_width(F_UI, "Show &all devices") + 2, ch };
    wiz.b[WB_AGAIN] = (W2kRect){ WZ_W - WZ_X - 100, 246, 100, 23 };
    static const char *const rl[4] = { "Choose a &passkey for me",
        "&Use the passkey found in the documentation:", "&Let me choose my own passkey:",
        "&Don't use a passkey" };
    int y = 72 + 3 * fh + 12;
    for (int i = 0; i < 4; i++, y += 26)
        wiz.rb[i] = (W2kRect){ WZ_X, y, 13 + 5 + w2k_mnemonic_width(F_UI, rl[i]) + 2, ch };
    wiz.pk_doc->r = (W2kRect){ WZ_W - WZ_X - 130, wiz.rb[1].y - 4, 130, 21 };
    wiz.pk_own->r = (W2kRect){ WZ_W - WZ_X - 130, wiz.rb[2].y - 4, 130, 21 };
}

static const char *const welcome_steps[] = {
    "Turn it on.",
    "Make it discoverable (visible). Its instructions say how: often you hold down its "
    "power or pairing button until a light flashes.",
    "Bring it near this computer.", NULL };

/* The welcome page's words, measured or drawn; returns where the check
 * box goes. */
static int welcome_body(Drawable d, int x, int y, int w)
{
    y = para(d, F_UI, x, y, w, "This wizard helps you add a Bluetooth device to this computer: "
             "a phone, a headset, a keyboard, a mouse, or another computer.", C_WINDOWTEXT) + 10;
    y = para(d, F_UI, x, y, w, "First, set up the device so that this computer can find it:", C_WINDOWTEXT) + 6;
    y = bullets(d, x + 4, y, w - 4, welcome_steps) + 14;
    return y;
}

static void wz_paint(W2kWin *w, Drawable d)
{
    (void)w;
    int fh = w2k_font_height(F_UI);
    int ext = wiz.page == WP_WELCOME || wiz.page == WP_DONE;
    Adapter *a = wz_adapter();
    if (ext) {
        w2k_fill(d, 0, 0, WZ_W, WZ_BAR, C_WINDOW);
        w2k_skin_draw(d, wiz.water, 0, 0, 0, 0, WM_W, WZ_BAR);
        int x = WM_W + 14, tw = WZ_W - x - 16;
        int y = title_para(d, x, 16, tw, wiz.page == WP_WELCOME
                           ? "Welcome to the Add Bluetooth Device Wizard"
                           : "Completing the Add Bluetooth Device Wizard") + 16;
        if (wiz.page == WP_WELCOME) {
            y = welcome_body(d, x, y, tw);
            wiz.cb_ready = (W2kRect){ x, y, 13 + 5 + w2k_mnemonic_width(F_UI, "&My device is set up and ready to be found.") + 2, fh > 13 ? fh : 13 };
            w2k_draw_checkbox(d, x, y, "&My device is set up and ready to be found.", wiz.ready, 0, 0);
            y += 26;
            y = para(d, F_UI, x, y, tw, "Add only Bluetooth devices that you trust.", C_WINDOWTEXT) + 6;
            if (a && !a->powered) para(d, F_UI, x, y, tw, "Bluetooth is turned off; the wizard will turn it on.", C_WINDOWTEXT);
            w2k_text(d, F_UI, x, WZ_BAR - 12 - fh, "To continue, click Next.", C_WINDOWTEXT);
        } else {
            if (wiz.ok) w2k_bigicon_draw(d, x, y, wiz.icon);
            y = para(d, F_UI, x + (wiz.ok ? 44 : 0), y + (wiz.ok ? 8 : 0), tw - (wiz.ok ? 44 : 0), wiz.result, C_WINDOWTEXT);
            w2k_text(d, F_UI, x, WZ_BAR - 12 - fh, "To close this wizard, click Finish.", C_WINDOWTEXT);
        }
    } else {
        static const char *const sub[] = {
            [WP_SEARCH]  = "Select the Bluetooth device that you want to add.",
            [WP_PASSKEY] = "Do you need a passkey to add your device?",
            [WP_PAIR]    = "Please wait while the wizard adds the device." };
        w2k_fill(d, 0, 0, WZ_W, 58, C_WINDOW);
        w2k_text(d, F_UI_BOLD, 21, 10, "Add Bluetooth Device Wizard", C_WINDOWTEXT);
        para(d, F_UI, 41, 12 + fh, WZ_W - 41 - 70, sub[wiz.page], C_WINDOWTEXT);
        w2k_icon_draw_scaled(d, WZ_W - 10 - 48, 5, ICO_CP_BLUETOOTH, 48);
        w2k_hline(d, 0, 58, WZ_W, C_SHADOW);
        w2k_hline(d, 0, 59, WZ_W, C_HILIGHT);
        if (wiz.page == WP_SEARCH) {
            dv_draw(d, &wiz.dv);
            w2k_draw_checkbox(d, wiz.cb_all.x, wiz.cb_all.y, "Show &all devices", wiz.showall, 0, 0);
            int y = 282;
            if (searching) {
                w2k_text(d, F_UI, WZ_X, y, "Searching for Bluetooth devices...", C_TEXT);
                W2kRect pr = { WZ_W - WZ_X - 180, y - 1, 180, fh + 3 };
                w2k_draw_progress(d, &pr, -1, wiz.phase);
            } else {
                para(d, F_UI, WZ_X, y, WZ_W - 2 * WZ_X,
                     "If your device is not listed, make sure it is turned on and discoverable, "
                     "then click Search Again.", C_TEXT);
            }
        } else if (wiz.page == WP_PASSKEY) {
            para(d, F_UI, WZ_X, 72, WZ_W - 2 * WZ_X,
                 "To answer this question, look in the Bluetooth section of your device's instructions. "
                 "If they give a passkey, use that one.", C_TEXT);
            static const char *const rl[4] = { "Choose a &passkey for me",
                "&Use the passkey found in the documentation:", "&Let me choose my own passkey:",
                "&Don't use a passkey" };
            for (int i = 0; i < 4; i++)
                w2k_draw_radio(d, wiz.rb[i].x, wiz.rb[i].y, rl[i], wiz.pk_mode == i, 0, 0);
            w2k_edit_draw(d, wiz.pk_doc);
            w2k_edit_draw(d, wiz.pk_own);
            para(d, F_UI, WZ_X, wiz.rb[3].y + 34, WZ_W - 2 * WZ_X,
                 "Use a passkey whenever the device takes one: the longer it is, the safer the "
                 "connection, and 8 to 16 digits is best. Newer devices show a number or ask you "
                 "to type one instead; the wizard tells you what to do when they do.", C_GRAYTEXT);
        } else {
            char t[400];
            Dev *dv = dev_find(wiz.sel);
            w2k_bigicon_draw(d, WZ_X, 74, wiz.icon);
            w2k_text(d, F_UI_BOLD, WZ_X + 44, 76, wiz.name, C_TEXT);
            const char *st = wiz.state == WS_PAIRING ? "Pairing with the device..."
                           : wiz.state == WS_TRUSTING ? "Paired. Setting the device up..."
                           : "Connecting to the device...";
            w2k_text(d, F_UI, WZ_X + 44, 78 + fh + 2, st, C_TEXT);
            int y = 128, tw = WZ_W - 2 * WZ_X;
            int ours = areq.msg && wiz_takes(areq.device);
            if (ours && areq.kind == AR_CONFIRM) {
                snprintf(t, sizeof t, "Make sure that %s shows the passkey below. If it does, click Yes, "
                         "and confirm on the device if it asks; if not, click No.", wiz.name);
                y = para(d, F_UI, WZ_X, y, tw, t, C_TEXT) + 8;
                char pk[16], sp[24];
                snprintf(pk, sizeof pk, "%06u", areq.passkey);
                spaced(pk, sp, sizeof sp);
                big_text(d, WZ_X + 14, y, sp, C_TEXT);
                wiz.b[WB_YES] = (W2kRect){ WZ_X + 200, y + 2, 75, 23 };
                wiz.b[WB_NO]  = (W2kRect){ WZ_X + 281, y + 2, 75, 23 };
            } else if (ours && (areq.kind == AR_PASSKEY || areq.kind == AR_PIN)) {
                snprintf(t, sizeof t, areq.kind == AR_PASSKEY
                         ? "Type the passkey that %s shows, then click Enter."
                         : "%s asks for a passkey. Type the one from its instructions (often 0000 or 1234), "
                           "then click Enter.", wiz.name);
                y = para(d, F_UI, WZ_X, y, tw, t, C_TEXT) + 8;
                wiz.entry->r = (W2kRect){ WZ_X + 14, y, 150, 21 };
                w2k_edit_draw(d, wiz.entry);
                wiz.b[WB_ENTER] = (W2kRect){ WZ_X + 14 + 150 + 8, y - 1, 75, 23 };
            } else if (adisp.active && wiz_takes(adisp.device)) {
                snprintf(t, sizeof t, "Type this passkey on %s, then press ENTER on it:", wiz.name);
                y = para(d, F_UI, WZ_X, y, tw, t, C_TEXT) + 8;
                char sp[32];
                spaced(adisp.text, sp, sizeof sp);
                big_text(d, WZ_X + 14, y, sp, C_TEXT);
                y += big_height() + 6;
                if (adisp.entered >= 0) {
                    snprintf(t, sizeof t, "Digits typed on the device: %d of %d", adisp.entered, (int)strlen(adisp.text));
                    w2k_text(d, F_UI, WZ_X + 14, y, t, C_GRAYTEXT);
                }
            } else if (wiz.autopin[0] && wiz.state == WS_PAIRING) {
                snprintf(t, sizeof t, "When %s asks for a passkey, type this one on it:", wiz.name);
                y = para(d, F_UI, WZ_X, y, tw, t, C_TEXT) + 8;
                char sp[32];
                spaced(wiz.autopin, sp, sizeof sp);
                big_text(d, WZ_X + 14, y, sp, C_TEXT);
            } else if (dv && dv->legacy && wiz.state == WS_PAIRING) {
                para(d, F_UI, WZ_X, y, tw, "If the device asks for a passkey, type the one you chose.", C_TEXT);
            }
            W2kRect pr = { WZ_X, WZ_BAR - 34, WZ_W - 2 * WZ_X, fh + 3 };
            w2k_draw_progress(d, &pr, -1, wiz.phase);
        }
    }
    w2k_hline(d, 0, WZ_BAR, WZ_W, C_SHADOW);
    w2k_hline(d, 0, WZ_BAR + 1, WZ_W, C_HILIGHT);
    for (int b = 1; b < NWB; b++) {
        if (!wz_on_page(b)) continue;
        static const char *const lab[NWB] = { [WB_BACK] = "< &Back", [WB_NEXT] = "&Next >",
            [WB_CANCEL] = "Cancel", [WB_AGAIN] = "&Search Again", [WB_YES] = "&Yes",
            [WB_NO] = "&No", [WB_ENTER] = "&Enter" };
        const char *l = b == WB_NEXT && wiz.page == WP_DONE ? "Finish" : lab[b];
        int def = (b == WB_NEXT && wz_enabled(WB_NEXT)) || b == WB_YES || b == WB_ENTER;
        w2k_draw_pushbutton(d, &wiz.b[b], l, (def ? BS_DEFAULT : 0) | (wiz.down == b ? BS_PRESSED : 0) |
                            (wz_enabled(b) ? 0 : BS_DISABLED));
    }
}

static void wz_cancel(void)
{
    if (wiz.page == WP_PAIR && wiz.state == WS_PAIRING && !demo) {
        if (areq.msg && wiz_takes(areq.device)) agent_answer(areq.serial, 0, NULL, 0);
        call(wiz.sel, IF_DEVICE, "CancelPairing", 5000, done_quiet, NULL);
    }
    search_stop();
    w2k_win_close(wiz.w, ID_CANCEL);
}

static void wz_press(int b)
{
    switch (b) {
    case WB_CANCEL: wz_cancel(); return;
    case WB_BACK:
        if (wiz.page == WP_SEARCH) wz_go(WP_WELCOME);
        else if (wiz.page == WP_PASSKEY || wiz.page == WP_DONE) wz_go(WP_SEARCH);
        return;
    case WB_NEXT:
        if (wiz.page == WP_WELCOME) wz_go(WP_SEARCH);
        else if (wiz.page == WP_SEARCH) {
            const Dev *d = dv_selected(&wiz.dv);
            if (!d) return;
            snprintf(wiz.sel, sizeof wiz.sel, "%s", d->path);
            /* Only a device that pairs the old way, with a PIN, is asked
             * about passkeys; the rest show or confirm a number. */
            wiz.paged = d->legacy;
            wz_go(d->legacy ? WP_PASSKEY : WP_PAIR);
        } else if (wiz.page == WP_PASSKEY) wz_go(WP_PAIR);
        else if (wiz.page == WP_DONE) w2k_win_close(wiz.w, ID_OK);
        return;
    case WB_AGAIN: search_start(); return;
    case WB_YES: case WB_NO:
        if (areq.msg && wiz_takes(areq.device)) agent_answer(areq.serial, b == WB_YES, NULL, 0);
        return;
    case WB_ENTER:
        if (areq.msg && wiz_takes(areq.device)) {
            const char *t = w2k_edit_text(wiz.entry);
            agent_answer(areq.serial, 1, t, (unsigned)strtoul(t, NULL, 10));
            w2k_edit_set(wiz.entry, "");
        }
        return;
    }
}

static void wz_tick(void *u)
{
    (void)u;
    if (!wiz.w) return;
    if (searching || wiz.page == WP_PAIR) { wiz.phase++; w2k_win_dirty(wiz.w); }
    if (searching && w2k_now_ms() > wiz.search_end) search_stop();
}

static void wz_blink(void *u)
{
    (void)u;
    if (wiz.page == WP_PASSKEY) { w2k_edit_blink(wiz.pk_doc); w2k_edit_blink(wiz.pk_own); }
    if (wiz.page == WP_PAIR) w2k_edit_blink(wiz.entry);
}

static W2kEdit *wz_focused_edit(void)
{
    if (wiz.page == WP_PASSKEY) return wiz.pk_doc->focused ? wiz.pk_doc : wiz.pk_own->focused ? wiz.pk_own : NULL;
    if (wiz.page == WP_PAIR && wz_on_page(WB_ENTER)) return wiz.entry;
    return NULL;
}

static int wz_event(W2kWin *w, XEvent *e)
{
    switch (e->type) {
    case ButtonPress: {
        XButtonEvent *b = &e->xbutton;
        if (wiz.page == WP_SEARCH && dv_press(&wiz.dv, b)) { w2k_win_dirty(w); return 1; }
        if (wiz.page == WP_PASSKEY) {
            if (w2k_edit_press(wiz.pk_doc, b)) { wiz.pk_own->focused = 0; wiz.pk_mode = PK_DOC; w2k_win_dirty(w); return 1; }
            if (w2k_edit_press(wiz.pk_own, b)) { wiz.pk_doc->focused = 0; wiz.pk_mode = PK_OWN; w2k_win_dirty(w); return 1; }
            for (int i = 0; i < 4; i++)
                if (w2k_rect_hit(&wiz.rb[i], b->x, b->y)) {
                    wiz.pk_mode = i;
                    wiz.pk_doc->focused = i == PK_DOC;
                    wiz.pk_own->focused = i == PK_OWN;
                }
        }
        if (wiz.page == WP_PAIR && wz_on_page(WB_ENTER) && w2k_edit_press(wiz.entry, b)) { w2k_win_dirty(w); return 1; }
        if (wiz.page == WP_WELCOME && w2k_rect_hit(&wiz.cb_ready, b->x, b->y)) wiz.ready = !wiz.ready;
        if (wiz.page == WP_SEARCH && w2k_rect_hit(&wiz.cb_all, b->x, b->y)) { wiz.showall = !wiz.showall; model_dirty = 1; bt_kick(); }
        if (b->button == Button1)
            for (int i = 1; i < NWB; i++)
                if (wz_on_page(i) && wz_enabled(i) && w2k_rect_hit(&wiz.b[i], b->x, b->y)) wiz.down = i;
        w2k_win_dirty(w);
        return 1;
    }
    case MotionNotify:
        if (wiz.page == WP_SEARCH && dv_motion(&wiz.dv, &e->xmotion)) { w2k_win_dirty(w); return 1; }
        return 0;
    case ButtonRelease: {
        dv_release(&wiz.dv);
        w2k_edit_release(wiz.pk_doc);
        w2k_edit_release(wiz.pk_own);
        w2k_edit_release(wiz.entry);
        int b = wiz.down;
        wiz.down = 0;
        if (b && w2k_rect_hit(&wiz.b[b], e->xbutton.x, e->xbutton.y) && wz_enabled(b)) wz_press(b);
        w2k_win_dirty(w);
        return 1;
    }
    case KeyPress: {
        XKeyEvent *k = &e->xkey;
        KeySym ks = XLookupKeysym(k, 0);
        if (ks == XK_Escape) { if (wz_enabled(WB_CANCEL)) wz_cancel(); else w2k_win_close(w, ID_OK); return 1; }
        W2kEdit *ed = wz_focused_edit();
        if (ks == XK_Return || ks == XK_KP_Enter) {
            if (wz_on_page(WB_ENTER) && wz_enabled(WB_ENTER)) wz_press(WB_ENTER);
            else if (wz_on_page(WB_YES)) wz_press(WB_YES);
            else if (wz_enabled(WB_NEXT)) wz_press(WB_NEXT);
            w2k_win_dirty(w);
            return 1;
        }
        if (!(k->state & Mod1Mask) && ed && w2k_edit_key(ed, k)) { w2k_win_dirty(w); return 1; }
        if (wiz.page == WP_SEARCH && !(k->state & Mod1Mask) && !ed) {
            int was = wiz.dv.focused;
            wiz.dv.focused = 1;
            if (dv_key(&wiz.dv, k)) { w2k_win_dirty(w); return 1; }
            wiz.dv.focused = was;
        }
        char ch[4];
        if (XLookupString(k, ch, sizeof ch, NULL, NULL) == 1) {
            int c = tolower((unsigned char)ch[0]);
            if (c == 'b' && wz_enabled(WB_BACK)) wz_press(WB_BACK);
            else if (c == 'n' && wz_enabled(WB_NEXT) && wiz.page != WP_DONE) wz_press(WB_NEXT);
            else if (c == 'm' && wiz.page == WP_WELCOME) wiz.ready = !wiz.ready;
            else if (c == 'a' && wiz.page == WP_SEARCH) { wiz.showall = !wiz.showall; model_dirty = 1; bt_kick(); }
            else if (c == 's' && wiz.page == WP_SEARCH && wz_enabled(WB_AGAIN)) wz_press(WB_AGAIN);
            else if (c == 'y' && wz_on_page(WB_YES)) wz_press(WB_YES);
            else if (c == 'n' && wz_on_page(WB_NO)) wz_press(WB_NO);
        }
        w2k_win_dirty(w);
        return 1;
    }
    }
    return 0;
}

static int wz_closing(W2kWin *w)
{
    (void)w;
    if (wz_enabled(WB_CANCEL)) wz_cancel();
    else w2k_win_close(wiz.w, ID_OK);
    return 0;
}

static void wz_refresh(void)
{
    Adapter *a = adapter();
    if (wiz.want_search && a && a->powered) {
        wiz.want_search = 0;
        call(a->path, IF_ADAPTER, "StartDiscovery", 10000, done_discovery, NULL);
    }
    dv_fill(&wiz.dv, wz_want);
    w2k_win_dirty(wiz.w);
}

static void wizard(W2kWin *over)
{
    if (wiz.w) return;
    memset(&wiz, 0, sizeof wiz);
    faces_open();
    wiz.w = w2k_win_new("Add Bluetooth Device Wizard", "l2kbluetooth", WZ_W, WZ_H, 0);
    wiz.w->paint = wz_paint;
    wiz.w->event = wz_event;
    wiz.w->closing = wz_closing;
    wiz.water = watermark(WM_W, WZ_BAR);
    wiz.dv.sel = -1;
    wiz.dv.wizard = 1;
    wiz.dv.focused = 1;
    wiz.dv.owner = wiz.w;
    snprintf(wiz.dv.empty, sizeof wiz.dv.empty, "No devices found yet.");
    w2k_scroll_bind(&wiz.dv.sb, wiz.w);
    wiz.pk_doc = w2k_edit_new(0);
    wiz.pk_own = w2k_edit_new(0);
    wiz.entry = w2k_edit_new(0);
    w2k_edit_bind(wiz.pk_doc, wiz.w);
    w2k_edit_bind(wiz.pk_own, wiz.w);
    w2k_edit_bind(wiz.entry, wiz.w);
    wiz.entry->focused = 1;
    wz_layout();
    if (getenv("W2K_RENDER_PAGE")) {
        wiz.page = atoi(getenv("W2K_RENDER_PAGE"));
        wiz.ready = 1;
        searching = wiz.page == WP_SEARCH;
        for (int i = 0; i < ndevs; i++) devs[i].seen = !dev_known(&devs[i]);
        dv_fill(&wiz.dv, wz_want);
        if (wiz.dv.nrows) dv_select(&wiz.dv, 0);
        if (wiz.page >= WP_PAIR && ndevs) {
            snprintf(wiz.sel, sizeof wiz.sel, "%s", devs[ndevs - 1].path);
            snprintf(wiz.name, sizeof wiz.name, "%s", dev_name(&devs[ndevs - 1]));
            wiz.icon = dev_icon(&devs[ndevs - 1]);
            wiz.ok = 1;
            snprintf(wiz.result, sizeof wiz.result, "%s was added to this computer and is connected.\n\n"
                     "It connects again by itself whenever it is on and near this computer. "
                     "To disconnect or remove it, use Bluetooth Devices in Control Panel.", wiz.name);
            adisp.active = wiz.page == WP_PAIR;
            snprintf(adisp.device, sizeof adisp.device, "%s", wiz.sel);
            snprintf(adisp.text, sizeof adisp.text, "482913");
            adisp.entered = 2;
        }
    }
    w2k_win_center(wiz.w, over);
    dialog_hints(wiz.w, over);
    w2k_add_timer(100, wz_tick, NULL);
    w2k_add_timer(w2k_caret_blink, wz_blink, NULL);
    int r = w2k_win_modal(wiz.w);
    w2k_del_timer(wz_tick, NULL);
    w2k_del_timer(wz_blink, NULL);
    search_stop();
    wiz.w = NULL;
    w2k_edit_free(wiz.pk_doc);
    w2k_edit_free(wiz.pk_own);
    w2k_edit_free(wiz.entry);
    w2k_skin_free(wiz.water);
    if (r == ID_OK && wiz.ok && sh.w) {
        snprintf(sh.dv.selpath, sizeof sh.dv.selpath, "%s", wiz.sel);
        model_dirty = 1;
        bt_kick();
    }
}

static W2kWin *err_parent(void)
{
    if (wiz.w) return wiz.w;
    if (dp.w) return dp.w;
    if (ap.w) return ap.w;
    return sh.w;
}

/* ------------------------------------------------------------------ *
 * The agent's questions, when the wizard is not the one asking
 * ------------------------------------------------------------------ */
static void agent_ask(void *unused)
{
    (void)unused;
    if (!areq.msg) return;
    int serial = areq.serial;
    char dev[128], text[600], got[64] = "";
    snprintf(dev, sizeof dev, "%s", areq.device);
    Dev *d = dev_find(dev);
    char name[160];
    snprintf(name, sizeof name, "%s", d ? dev_name(d) : "A Bluetooth device");

    if (wiz_takes(dev)) {
        /* The wizard is adding this device: it answers in its page. */
        switch (areq.kind) {
        case AR_PIN:
            if (wiz.paged || wiz.pk_mode != PK_AUTO) agent_answer(serial, 1, wz_pin(), 0);
            break;
        case AR_AUTHORIZE: case AR_SERVICE: agent_answer(serial, 1, NULL, 0); break;
        }
        w2k_win_dirty(wiz.w);
        return;
    }
    W2kWin *over = err_parent();
    int ok = 0;
    switch (areq.kind) {
    case AR_SERVICE: {
        char buf[64];
        const char *svc = service_name(areq.uuid, buf, sizeof buf);
        if (d && d->trusted) { agent_answer(serial, 1, NULL, 0); return; }
        if (!opt.allow) { agent_answer(serial, 0, NULL, 0); return; }
        if (!opt.alert && d && d->paired) { agent_answer(serial, 1, NULL, 0); return; }
        snprintf(text, sizeof text, "%s wants to connect to this computer%s%s.\n\nDo you want to allow it? "
                 "If you do, it can connect again without asking.", name, svc ? " to use " : "",
                 svc ? svc : "");
        ok = ask(over, &areq.dlg, "Bluetooth Devices", ICO_CP_BLUETOOTH, text, NULL, NULL, 0, 0, "&Allow", "&Deny", NULL, 0);
        if (ok && d && dev_find(dev)) set_bool(dev, IF_DEVICE, "Trusted", 1, done_quiet, NULL);
        agent_answer(serial, ok, NULL, 0);
        return;
    }
    case AR_AUTHORIZE:
        if (!opt.allow) { agent_answer(serial, 0, NULL, 0); return; }
        snprintf(text, sizeof text, "%s wants to pair with this computer.\n\nDo you want to allow it? "
                 "Pair only with devices you trust.", name);
        ok = ask(over, &areq.dlg, "Bluetooth Pairing", ICO_CP_BLUETOOTH, text, NULL, NULL, 0, 0, "&Allow", "&Deny", NULL, 0);
        agent_answer(serial, ok, NULL, 0);
        return;
    case AR_CONFIRM: {
        char pk[16];
        snprintf(pk, sizeof pk, "%06u", areq.passkey);
        snprintf(text, sizeof text, "%s wants to pair with this computer.\n\n"
                 "If the device shows the passkey below, click Yes (and confirm on the device if it asks). "
                 "If it shows another, or none, click No.", name);
        ok = ask(over, &areq.dlg, "Bluetooth Pairing", ICO_CP_BLUETOOTH, text, pk, NULL, 0, 0, "&Yes", "&No", NULL, 0);
        agent_answer(serial, ok, NULL, 0);
        return;
    }
    case AR_PIN:
        snprintf(text, sizeof text, "%s needs a passkey to pair with this computer.\n\n"
                 "Type the passkey from the device's instructions (often 0000 or 1234), "
                 "or make one up and type the same on the device.", name);
        ok = ask(over, &areq.dlg, "Bluetooth Pairing", ICO_CP_BLUETOOTH, text, NULL, NULL, 1, 0, "OK", "Cancel", got, sizeof got);
        agent_answer(serial, ok && got[0], got, 0);
        return;
    case AR_PASSKEY:
        snprintf(text, sizeof text, "Type the passkey that %s shows.", name);
        ok = ask(over, &areq.dlg, "Bluetooth Pairing", ICO_CP_BLUETOOTH, text, NULL, NULL, 1, 1, "OK", "Cancel", got, sizeof got);
        agent_answer(serial, ok && got[0], NULL, (unsigned)strtoul(got, NULL, 10));
        return;
    }
    agent_answer(serial, 0, NULL, 0);
}

static void agent_display(void *unused)
{
    (void)unused;
    if (!adisp.active || adisp.dlg) return;
    if (wiz_takes(adisp.device)) { w2k_win_dirty(wiz.w); return; }
    char dev[128], text[400];
    snprintf(dev, sizeof dev, "%s", adisp.device);
    Dev *d = dev_find(dev);
    snprintf(text, sizeof text, "To pair %s with this computer, type this passkey on it, then press ENTER on it:",
             d ? dev_name(d) : "the device");
    ask(err_parent(), &adisp.dlg, "Bluetooth Pairing", ICO_CP_BLUETOOTH, text, adisp.text, &adisp.entered,
        0, 0, "Cancel", NULL, NULL, 0);
    /* Still showing when the dialog went: the user cancelled it. */
    if (adisp.active && !strcmp(adisp.device, dev)) {
        adisp.active = 0;
        if (!demo) call(dev, IF_DEVICE, "CancelPairing", 5000, done_quiet, NULL);
    }
}

/* ------------------------------------------------------------------ *
 * The icon in the notification area (--agent)
 * ------------------------------------------------------------------ */
static struct {
    Window  win;
    Atom    sel, opcode, manager, xembed_info, agent_sel;
    W2kWin *keeper;                 /* keeps w2k_run going with nothing shown */
    long    last_click;
    int     tip_x, tip_y, tip_on;
} tray;

static char self_path[1024] = "l2kbluetooth";

static int agent_running(void)
{
    if (!tray.agent_sel) {
        char name[64];
        snprintf(name, sizeof name, "_L2K_BLUETOOTH_AGENT_S%d", w2k.screen);
        tray.agent_sel = XInternAtom(w2k.dpy, name, False);
    }
    return XGetSelectionOwner(w2k.dpy, tray.agent_sel) != None;
}

/* Start another copy of this program, not waited for. */
static void spawn_self(const char *arg)
{
    pid_t p = fork();
    if (p == 0) {
        setsid();
        if (fork() == 0) {
            execl(self_path, self_path, arg, (char *)NULL);
            execlp("l2kbluetooth", "l2kbluetooth", arg, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static int tray_wanted(void) { return agent_mode && opt.tray && nads > 0; }

static void tray_dock(void)
{
    Window owner = XGetSelectionOwner(w2k.dpy, tray.sel);
    if (!owner || !tray.win) return;
    XEvent e = { 0 };
    e.xclient.type = ClientMessage;
    e.xclient.window = owner;
    e.xclient.message_type = tray.opcode;
    e.xclient.format = 32;
    e.xclient.data.l[0] = CurrentTime;
    e.xclient.data.l[1] = 0;                        /* SYSTEM_TRAY_REQUEST_DOCK */
    e.xclient.data.l[2] = (long)tray.win;
    XSendEvent(w2k.dpy, owner, False, NoEventMask, &e);
    XFlush(w2k.dpy);
}

static void tray_refresh(void)
{
    if (!agent_mode) return;
    if (tray_wanted() && !tray.win) {
        /* The bar's own background shows through around the icon. */
        XSetWindowAttributes a = {
            .background_pixmap = ParentRelative,
            .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                          EnterWindowMask | LeaveWindowMask | StructureNotifyMask };
        tray.win = XCreateWindow(w2k.dpy, w2k.root, 0, 0, (unsigned)w2k_px(16), (unsigned)w2k_px(16), 0,
                                 CopyFromParent, InputOutput, CopyFromParent, CWBackPixmap | CWEventMask, &a);
        XClassHint ch = { (char *)"l2kbluetooth", (char *)"W2k" };
        XSetClassHint(w2k.dpy, tray.win, &ch);
        w2k_set_wm_name(tray.win, "Bluetooth");
        unsigned long info[2] = { 0, 1 };           /* XEMBED version 0, mapped */
        XChangeProperty(w2k.dpy, tray.win, tray.xembed_info, tray.xembed_info, 32, PropModeReplace,
                        (unsigned char *)info, 2);
        tray_dock();
    } else if (!tray_wanted() && tray.win) {
        XDestroyWindow(w2k.dpy, tray.win);
        tray.win = 0;
        w2k_tooltip_hide();
    } else if (tray.win) {
        XClearArea(w2k.dpy, tray.win, 0, 0, 0, 0, True);
    }
}

static void tray_paint(void)
{
    Adapter *a = adapter();
    XClearWindow(w2k.dpy, tray.win);
    if (a && a->powered) w2k_icon_draw(tray.win, 0, 0, ICO_CP_BLUETOOTH);
    else w2k_icon_draw_disabled(tray.win, 0, 0, ICO_CP_BLUETOOTH);
}

static void tray_tip_text(char *out, int n)
{
    Adapter *a = adapter();
    int conn = 0;
    const Dev *one = NULL;
    for (int i = 0; i < ndevs; i++) if (devs[i].connected) { conn++; one = &devs[i]; }
    if (!a || !a->powered) snprintf(out, (size_t)n, "Bluetooth is turned off");
    else if (conn == 1) snprintf(out, (size_t)n, "Bluetooth: %s connected", dev_name(one));
    else if (conn) snprintf(out, (size_t)n, "Bluetooth: %d devices connected", conn);
    else snprintf(out, (size_t)n, "Bluetooth Devices");
}

static void tray_tip(void *u)
{
    (void)u;
    w2k_del_timer(tray_tip, NULL);
    if (!tray.tip_on) return;
    char t[200];
    tray_tip_text(t, sizeof t);
    w2k_tooltip_show(t, tray.tip_x, tray.tip_y);
}

enum { TM_SHOW = 1, TM_ADD, TM_POWER, TM_REMOVE };

static void tray_menu(int rx, int ry)
{
    Adapter *a = adapter();
    w2k_tooltip_hide();
    tray.tip_on = 0;
    W2kMenu *m = w2k_menu_new();
    w2k_menu_item(m, TM_SHOW, "&Show Bluetooth Devices", NULL, ICO_NONE);
    w2k_menu_default(m);
    w2k_menu_item(m, TM_ADD, "&Add a Bluetooth Device", NULL, ICO_NONE);
    if (!a || !a->powered) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, TM_POWER, a && a->powered ? "Turn Bluetooth &Off" : "Turn Bluetooth &On", NULL, ICO_NONE);
    if (!a) w2k_menu_disable(m);
    w2k_menu_sep(m);
    w2k_menu_item(m, TM_REMOVE, "&Remove Bluetooth Icon", NULL, ICO_NONE);
    int id = w2k_menu_popup(m, rx, ry, MPOP_BOTTOMUP);
    w2k_menu_free(m);
    switch (id) {
    case TM_SHOW:  spawn_self(NULL); break;
    case TM_ADD:   spawn_self("add"); break;
    case TM_POWER: if ((a = adapter())) set_power(a, !a->powered); break;
    case TM_REMOVE:
        opt.tray = 0;
        opt_save();
        tray_refresh();
        break;
    }
}

static void tray_event(XEvent *e)
{
    if (w2k_tooltip_event(e)) return;
    if (e->type == ClientMessage && e->xclient.window == w2k.root &&
        e->xclient.message_type == tray.manager && (Atom)e->xclient.data.l[1] == tray.sel) {
        tray_dock();                    /* a (new) notification area appeared */
        return;
    }
    if (!tray.win || e->xany.window != tray.win) return;
    switch (e->type) {
    case Expose:
        if (e->xexpose.count == 0) tray_paint();
        break;
    case ReparentNotify:
        /* Back on the root: the notification area went away. Wait for
         * the next one to announce itself. */
        if (e->xreparent.parent == w2k.root) XUnmapWindow(w2k.dpy, tray.win);
        break;
    case EnterNotify:
        tray.tip_on = 1;
        tray.tip_x = e->xcrossing.x_root;
        tray.tip_y = e->xcrossing.y_root;
        w2k_add_timer(600, tray_tip, NULL);
        break;
    case LeaveNotify:
        tray.tip_on = 0;
        w2k_del_timer(tray_tip, NULL);
        w2k_tooltip_hide();
        break;
    case ButtonPress:
        tray.tip_on = 0;
        w2k_tooltip_hide();
        if (e->xbutton.button == Button3) {
            tray_menu(e->xbutton.x_root, e->xbutton.y_root);
        } else if (e->xbutton.button == Button1) {
            long now = (long)e->xbutton.time;
            if (now - tray.last_click < w2k_dblclk_ms) { tray.last_click = 0; spawn_self(NULL); }
            else tray.last_click = now;
        }
        break;
    }
}

/* The sheet saves the settings; the agent picks them up. */
static void opt_watch(void *u)
{
    (void)u;
    char p[1024];
    struct stat st;
    opt_path(p, sizeof p);
    long long m = stat(p, &st) == 0 ? mtime_ns(&st) : 0;
    if (m == opt_mtime) return;
    opt_load();
    tray_refresh();
    enforce_connectable();
}

static void on_term(int s) { (void)s; w2k_win_abort = 1; }

/* ------------------------------------------------------------------ *
 * The model changed: everything that shows it is brought up to date
 * ------------------------------------------------------------------ */
static int dp_nuuid = -1;

static void refresh(void *unused)
{
    (void)unused;
    refresh_queued = 0;
    model_dirty = 0;
    /* A passkey shown for a device that has since paired has done its job. */
    if (adisp.active) {
        Dev *d = dev_find(adisp.device);
        if (!d || (d->paired && !d->busy)) {
            adisp.active = 0;
            if (adisp.dlg) w2k_win_close(adisp.dlg, ID_CANCEL);
        }
    }
    if (adisp.dlg) w2k_win_dirty(adisp.dlg);
    Adapter *a = adapter();
    if (pending_disc >= 0 && a && a->powered) {
        set_discovery(a, pending_disc);
        pending_disc = -1;
    }
    if (sh.w) {
        dv_fill(&sh.dv, dev_known);
        hw_fill();
        if (!sh.odirty) options_load();
        sh_empty_text();
        w2k_win_dirty(sh.w);
    }
    if (dp.w) {
        Dev *d = dev_find(dp.path);
        if ((d ? d->nuuid : 0) != dp_nuuid) { dp_nuuid = d ? d->nuuid : 0; dp_fill_services(); }
        w2k_win_dirty(dp.w);
    }
    if (ap.w) w2k_win_dirty(ap.w);
    if (wiz.w) wz_refresh();
    tray_refresh();
}

/* ------------------------------------------------------------------ *
 * Made-up devices, for looking at the windows without a radio
 * ------------------------------------------------------------------ */
static void demo_dev(const char *mac, const char *name, const char *icon, int paired, int conn,
                     int battery, int legacy, const char *uuids)
{
    char path[128];
    snprintf(path, sizeof path, "/org/bluez/hci0/dev_%s", mac);
    for (char *c = path + 20; *c; c++) if (*c == ':') *c = '_';
    Dev *d = dev_get(path);
    if (!d) return;
    snprintf(d->address, sizeof d->address, "%s", mac);
    snprintf(d->name, sizeof d->name, "%s", name);
    snprintf(d->alias, sizeof d->alias, "%s", name);
    snprintf(d->icon, sizeof d->icon, "%s", icon);
    d->paired = d->bonded = d->trusted = paired;
    d->connected = conn;
    d->battery = battery;
    d->legacy = legacy;
    d->has_rssi = !paired;
    d->rssi = -58 - (int)(strlen(name) % 20);
    for (const char *u = uuids; *u && d->nuuid < MAX_UUID; ) {
        unsigned id;
        if (sscanf(u, "%4x", &id) != 1) break;
        snprintf(d->uuid[d->nuuid++], sizeof d->uuid[0], "%08x-0000-1000-8000-00805f9b34fb", id);
        u += 4;
        while (*u == ',') u++;
    }
}

static void demo_fill(void)
{
    Adapter *a = ad_get("/org/bluez/hci0");
    snprintf(a->address, sizeof a->address, "CC:2F:71:FE:1D:0B");
    snprintf(a->name, sizeof a->name, "linux2000");
    snprintf(a->alias, sizeof a->alias, "linux2000");
    snprintf(a->power_state, sizeof a->power_state, "on");
    a->powered = a->connectable = 1;
    a->manufacturer = 2;
    a->version = 8;
    bluez_up = 1;
    demo_dev("00:1B:66:A1:22:10", "WH-1000XM4", "audio-headset", 1, 1, 80, 0, "110B,110E,111E,1108,110C");
    demo_dev("F4:73:35:0C:9A:01", "MX Keys", "input-keyboard", 1, 1, 60, 0, "1812,180F,180A");
    demo_dev("E1:22:7B:4D:5E:02", "MX Master 3", "input-mouse", 1, 0, -1, 0, "1812,180F");
    demo_dev("3C:28:6D:11:02:AB", "Pixel 7", "phone", 1, 0, -1, 0, "1105,1112,111F,112F,1132,110A,110C");
    demo_dev("98:7A:14:55:60:0C", "Xbox Wireless Controller", "input-gaming", 1, 0, -1, 0, "1812");
    demo_dev("B8:D5:0B:77:12:3E", "JBL Flip 5", "audio-card", 0, 0, -1, 0, "");
    demo_dev("7C:F3:1B:90:44:5A", "LIVINGROOM-PC", "computer", 0, 0, -1, 0, "");
    demo_dev("00:0A:95:9D:68:16", "Nokia 6310i", "phone", 0, 0, -1, 1, "");
}

/* W2K_BT_DEMO=<kind>: a question as if a device were pairing. */
static void demo_request(const char *kind)
{
    static const struct { const char *word; int kind; } k[] = {
        { "pin", AR_PIN }, { "passkey", AR_PASSKEY }, { "confirm", AR_CONFIRM },
        { "authorize", AR_AUTHORIZE }, { "service", AR_SERVICE } };
    const char *dev = "/org/bluez/hci0/dev_3C_28_6D_11_02_AB";
    if (!strcmp(kind, "display")) {
        adisp.active = 1;
        snprintf(adisp.device, sizeof adisp.device, "%s", dev);
        snprintf(adisp.text, sizeof adisp.text, "482913");
        adisp.entered = 2;
        agent_display(NULL);
        return;
    }
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (!strcmp(kind, k[i].word)) {
            areq.msg = dbus_message_new_method_call(BLUEZ, AGENT_PATH, IF_AGENT, "Demo");
            areq.kind = k[i].kind;
            areq.serial++;
            areq.passkey = 482913;
            snprintf(areq.device, sizeof areq.device, "%s", dev);
            snprintf(areq.uuid, sizeof areq.uuid, "0000110b-0000-1000-8000-00805f9b34fb");
            agent_ask(NULL);
        }
}

int main(int argc, char **argv)
{
    int add = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--agent")) agent_mode = 1;
        else if (!strcasecmp(argv[i], "add")) add = 1;
        else {
            fprintf(stderr, "usage: l2kbluetooth            Bluetooth Devices\n"
                            "       l2kbluetooth add        the Add Bluetooth Device Wizard\n"
                            "       l2kbluetooth --agent    answer pairing requests; the tray icon\n");
            return 2;
        }
    }
    if (w2k_init("l2kbluetooth") < 0) return 1;
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
    if (n > 0) {
        self_path[n] = 0;
        char *del = strstr(self_path, " (deleted)");
        if (del) *del = 0;
    }
    signal(SIGPIPE, SIG_IGN);
    opt_load();
    demo = getenv("W2K_BT_DEMO") != NULL;

    if (agent_mode) {
        if (agent_running()) return 0;          /* one per display */
        tray.keeper = w2k_win_new("Bluetooth", "l2kbluetooth-agent", 1, 1, 0);
        XSetSelectionOwner(w2k.dpy, tray.agent_sel, tray.keeper->win, CurrentTime);
        char name[64];
        snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", w2k.screen);
        tray.sel = XInternAtom(w2k.dpy, name, False);
        tray.opcode = XInternAtom(w2k.dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
        tray.manager = XInternAtom(w2k.dpy, "MANAGER", False);
        tray.xembed_info = XInternAtom(w2k.dpy, "_XEMBED_INFO", False);
        XWindowAttributes wa;
        if (XGetWindowAttributes(w2k.dpy, w2k.root, &wa))
            XSelectInput(w2k.dpy, w2k.root, wa.your_event_mask | StructureNotifyMask);
        w2k_win_foreign_event = tray_event;
        signal(SIGTERM, on_term);
        signal(SIGINT, on_term);
        if (!demo && !bt_connect()) {
            fprintf(stderr, "l2kbluetooth: cannot reach the system bus\n");
            return 1;
        }
        if (demo) { demo_fill(); model_dirty = 1; bt_kick(); }
        w2k_add_timer(2000, opt_watch, NULL);
        w2k_run();
        if (tray.win) XDestroyWindow(w2k.dpy, tray.win);
        w2k_fini();
        return 0;
    }

    if (demo) {
        demo_fill();
        if (strcmp(getenv("W2K_BT_DEMO"), "1")) demo_request(getenv("W2K_BT_DEMO"));
    } else {
        bt_connect();
    }
    if (add) {
        wizard(NULL);
    } else {
        sheet();
        w2k_run();
    }
    w2k_fini();
    return 0;
}
