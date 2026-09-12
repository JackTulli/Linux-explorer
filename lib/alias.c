/* alias.c -- the names Windows users type.
 *
 * "taskmgr", "calc", "winver", "devmgmt.msc", "desk.cpl": what runs on
 * Windows when typed into Run or the Start menu's search runs the same
 * thing here. Each alias names the program it stands for, a label and an
 * icon for the search results, and the command; ".exe", ".msc" and
 * ".cpl" on the end of what was typed are ignored. */
#include "w2k.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

/* "@terminal" runs the desktop's terminal (the window manager knows which). */
static const W2kAlias aliases[] = {
    { "taskmgr",        "Task Manager",         "l2ktaskmgr",         ICO_TASKMGR },
    { "winver",         "About Linux 2000",     "linver",             ICO_STARTFLAG },
    { "devmgmt",        "Device Manager",       "l2kdevmgmt",         ICO_MYCOMPUTER },
    { "diskmgmt",       "Disk Management",      "l2kdiskmgmt",        ICO_DRIVE_HDD },
    { "notepad",        "Notepad",              "l2knotepad",         ICO_NOTEPAD },
    { "calc",           "Calculator",           "l2kcalc",            ICO_CALC },
    { "mspaint",        "Paint",                "l2kpaint",           ICO_PAINT },
    { "pbrush",         "Paint",                "l2kpaint",           ICO_PAINT },
    { "paint",          "Paint",                "l2kpaint",           ICO_PAINT },
    { "explorer",       "Windows Explorer",     "l2kexplorer",        ICO_EXPLORER },
    { "control",        "Control Panel",        "l2kcontrol",         ICO_CONTROLPANEL },
    { "charmap",        "Character Map",        "l2kcharmap",         ICO_CHARMAP },
    { "snippingtool",   "Snipping Tool",        "l2ksnip",            ICO_SNIP },
    { "wuapp",          "Windows Update",       "l2kupdate",          ICO_WINUPDATE },
    { "wuauclt",        "Windows Update",       "l2kupdate",          ICO_WINUPDATE },
    { "desk",           "Display Properties",   "l2kdisplay",         ICO_SETTINGS },
    { "display",        "Display Properties",   "l2kdisplay",         ICO_SETTINGS },
    { "ncpa",           "Network Connections",  "l2knetwork",         ICO_NETWORK },
    { "bthprops",       "Bluetooth Devices",    "l2kbluetooth",       ICO_CP_BLUETOOTH },
    { "bluetooth",      "Bluetooth Devices",    "l2kbluetooth",       ICO_CP_BLUETOOTH },
    { "fsquirt",        "Add Bluetooth Device", "l2kbluetooth add",   ICO_CP_BLUETOOTH },
    { "cmd",            "Command Prompt",       "@terminal",          ICO_TERMINAL },
    { "command",        "Command Prompt",       "@terminal",          ICO_TERMINAL },
    { "timedate",       "Date and Time",        "l2kcontrol datetime", ICO_SETTINGS },
    { "mmsys",          "Sounds and Multimedia", "l2kcontrol sounds",  ICO_SETTINGS },
    { "main",           "Mouse",                "l2kcontrol mouse",   ICO_SETTINGS },
    { "mouse",          "Mouse",                "l2kcontrol mouse",   ICO_SETTINGS },
    { "keyboard",       "Keyboard",             "l2kcontrol keyboard", ICO_SETTINGS },
    { "powercfg",       "Power Options",        "l2kcontrol power",   ICO_SETTINGS },
    { "fonts",          "Fonts",                "l2kcontrol fonts",   ICO_FONTS_FOLDER },
    { "netplwiz",       "User Accounts",        "l2kcontrol users",   ICO_SETTINGS },
    { "lusrmgr",        "User Accounts",        "l2kcontrol users",   ICO_SETTINGS },
    { "sysdm",          "System Properties",    "l2kcontrol system",  ICO_CP_SYSTEM },
    { "cleanmgr",       "Disk Cleanup",         "l2kcontrol cleanup", ICO_DRIVE_HDD },
    { "msinfo32",       "About Linux 2000",     "linver",             ICO_STARTFLAG },
    { "logonui",        "Logon Screen",         "l2kcontrol logon",   ICO_LOGOFF },
};
#define NALIAS ((int)(sizeof aliases / sizeof *aliases))

/* The word typed, lower case, without ".exe", ".msc" or ".cpl". */
static void bare(const char *word, char *out, int n)
{
    int k = 0;
    for (const char *p = word; *p && k < n - 1; p++) out[k++] = (char)tolower((unsigned char)*p);
    out[k] = 0;
    static const char *const ext[] = { ".exe", ".msc", ".cpl", ".com" };
    for (int i = 0; i < 4; i++) {
        size_t el = strlen(ext[i]);
        if ((size_t)k > el && !strcmp(out + k - el, ext[i])) { out[k - el] = 0; break; }
    }
}

const W2kAlias *w2k_alias_find(const char *word)
{
    if (!word || !*word) return NULL;
    char w[64];
    bare(word, w, sizeof w);
    for (int i = 0; i < NALIAS; i++)
        if (!strcmp(aliases[i].name, w)) return &aliases[i];
    return NULL;
}

/* Aliases a search query matches: the name or the label beginning with
 * it, or a word of the label; one per program. */
int w2k_alias_search(const char *query, const W2kAlias **out, int max)
{
    if (!query || !*query || max <= 0) return 0;
    char q[64];
    bare(query, q, sizeof q);
    size_t qn = strlen(q);
    if (!qn) return 0;
    int n = 0;
    for (int i = 0; i < NALIAS && n < max; i++) {
        const W2kAlias *a = &aliases[i];
        int hit = !strncmp(a->name, q, qn);
        if (!hit) {
            char l[64];
            int k = 0;
            for (const char *p = a->label; *p && k < 63; p++) l[k++] = (char)tolower((unsigned char)*p);
            l[k] = 0;
            if (!strncmp(l, q, qn)) hit = 1;
            else {
                const char *sp = l;
                while ((sp = strchr(sp, ' ')) && !hit) { sp++; if (!strncmp(sp, q, qn)) hit = 1; }
            }
        }
        if (!hit) continue;
        int dup = 0;
        for (int j = 0; j < n; j++) if (!strcmp(out[j]->cmd, a->cmd)) dup = 1;
        if (!dup) out[n++] = a;
    }
    return n;
}
