/* sound.c -- the desktop's sound events.
 *
 * The events Windows 2000's Sounds and Multimedia applet lists, each with
 * the .wav a sound pack gives it: Windows 98, 2000, XP and 7 and the 7
 * themes, shipped under sounds/<pack>/ with their original file names.
 * ~/.w2k/scheme keeps the pack (SoundPack), the volume (SoundVolume) and
 * any event the user pointed elsewhere (Sound.<event>=<file>|none).
 *
 * Playback goes to whatever player the machine has -- paplay, pw-play,
 * aplay, ffplay or mpv -- in a child that is never waited for, so a
 * sound costs the caller a fork and nothing else. */
#include "w2k.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

char w2k_sound_pack[32] = "win2000";
int  w2k_sound_volume = 100;
char w2k_sound_override[N_SOUNDS][256];

/* The events, in the order the applet lists them. `group` 1 is the
 * Windows Explorer group. */
static const struct { const char *slug, *label; int group; } events[N_SOUNDS] = {
    [SND_SYSTEMSTART]   = { "SystemStart",        "Start Windows",        0 },
    [SND_SYSTEMEXIT]    = { "SystemExit",         "Exit Windows",         0 },
    [SND_WINDOWSLOGON]  = { "WindowsLogon",       "Windows Logon",        0 },
    [SND_WINDOWSLOGOFF] = { "WindowsLogoff",      "Windows Logoff",       0 },
    [SND_DEFAULT]       = { "Default",            "Default Beep",         0 },
    [SND_ASTERISK]      = { "SystemAsterisk",     "Asterisk",             0 },
    [SND_HAND]          = { "SystemHand",         "Critical Stop",        0 },
    [SND_EXCLAMATION]   = { "SystemExclamation",  "Exclamation",          0 },
    [SND_QUESTION]      = { "SystemQuestion",     "Question",             0 },
    [SND_APPGPFAULT]    = { "AppGPFault",         "Program Error",        0 },
    [SND_OPEN]          = { "Open",               "Open Program",         0 },
    [SND_CLOSE]         = { "Close",              "Close Program",        0 },
    [SND_MINIMIZE]      = { "Minimize",           "Minimize",             0 },
    [SND_MAXIMIZE]      = { "Maximize",           "Maximize",             0 },
    [SND_RESTOREUP]     = { "RestoreUp",          "Restore Up",           0 },
    [SND_RESTOREDOWN]   = { "RestoreDown",        "Restore Down",         0 },
    [SND_MENUCOMMAND]   = { "MenuCommand",        "Menu Command",         0 },
    [SND_MENUPOPUP]     = { "MenuPopup",          "Menu Popup",           0 },
    [SND_MAILBEEP]      = { "MailBeep",           "New Mail Notification", 0 },
    [SND_NOTIFICATION]  = { "SystemNotification", "System Notification",  0 },
    [SND_NAVIGATING]    = { "Navigating",         "Start Navigation",     1 },
    [SND_EMPTYRECYCLE]  = { "EmptyRecycleBin",    "Empty Recycle Bin",    1 },
};

const char *w2k_sound_slug(int ev)  { return ev >= 0 && ev < N_SOUNDS ? events[ev].slug : ""; }
const char *w2k_sound_label(int ev) { return ev >= 0 && ev < N_SOUNDS ? events[ev].label : ""; }
int         w2k_sound_group(int ev) { return ev >= 0 && ev < N_SOUNDS ? events[ev].group : 0; }

int w2k_sound_by_slug(const char *slug)
{
    for (int i = 0; i < N_SOUNDS; i++)
        if (!strcasecmp(events[i].slug, slug)) return i;
    return -1;
}

/* ---- What each pack plays --------------------------------------------- */
typedef struct { const char *files[N_SOUNDS]; } PackMap;

static const PackMap map98 = { {
    [SND_SYSTEMSTART] = "The Microsoft Sound.wav", [SND_SYSTEMEXIT] = "LOGOFF.WAV",
    [SND_WINDOWSLOGON] = "TADA.WAV", [SND_WINDOWSLOGOFF] = "LOGOFF.WAV",
    [SND_DEFAULT] = "DING.WAV", [SND_ASTERISK] = "CHORD.WAV", [SND_HAND] = "CHORD.WAV",
    [SND_EXCLAMATION] = "CHORD.WAV", [SND_QUESTION] = "CHORD.WAV",
    [SND_MAILBEEP] = "NOTIFY.WAV", [SND_NOTIFICATION] = "CHIMES.WAV",
    [SND_EMPTYRECYCLE] = "RECYCLE.WAV",
} };
static const PackMap map2000 = { {
    [SND_SYSTEMSTART] = "Windows Logon Sound.wav", [SND_SYSTEMEXIT] = "Windows Logoff Sound.wav",
    [SND_WINDOWSLOGON] = "tada.wav", [SND_WINDOWSLOGOFF] = "Windows Logoff Sound.wav",
    [SND_DEFAULT] = "ding.wav", [SND_ASTERISK] = "chord.wav", [SND_HAND] = "chord.wav",
    [SND_EXCLAMATION] = "chord.wav", [SND_QUESTION] = "chord.wav",
    [SND_MAILBEEP] = "notify.wav", [SND_NOTIFICATION] = "chimes.wav",
    [SND_EMPTYRECYCLE] = "recycle.wav",
} };
static const PackMap mapxp = { {
    [SND_SYSTEMSTART] = "Windows XP Startup.wav", [SND_SYSTEMEXIT] = "Windows XP Shutdown.wav",
    [SND_WINDOWSLOGON] = "Windows XP Logon Sound.wav", [SND_WINDOWSLOGOFF] = "Windows XP Logoff Sound.wav",
    [SND_DEFAULT] = "Windows XP Ding.wav", [SND_ASTERISK] = "Windows XP Error.wav",
    [SND_HAND] = "Windows XP Critical Stop.wav", [SND_EXCLAMATION] = "Windows XP Exclamation.wav",
    [SND_APPGPFAULT] = "Windows XP Error.wav",
    [SND_MINIMIZE] = "Windows XP Minimize.wav", [SND_MAXIMIZE] = "Windows XP Restore.wav",
    [SND_RESTOREUP] = "Windows XP Restore.wav", [SND_RESTOREDOWN] = "Windows XP Restore.wav",
    [SND_MENUCOMMAND] = "Windows XP Menu Command.wav",
    [SND_MAILBEEP] = "Windows XP Notify.wav", [SND_NOTIFICATION] = "Windows XP Balloon.wav",
    [SND_NAVIGATING] = "Windows Navigation Start.wav", [SND_EMPTYRECYCLE] = "Windows XP Recycle.wav",
} };
static const PackMap map7 = { {
    [SND_SYSTEMSTART] = "Startup.wav", [SND_SYSTEMEXIT] = "Windows Shutdown.wav",
    [SND_WINDOWSLOGON] = "Windows Logon Sound.wav", [SND_WINDOWSLOGOFF] = "Windows Logoff Sound.wav",
    [SND_DEFAULT] = "Windows Default.wav", [SND_ASTERISK] = "Windows Ding.wav",
    [SND_HAND] = "Windows Critical Stop.wav", [SND_EXCLAMATION] = "Windows Exclamation.wav",
    [SND_APPGPFAULT] = "Windows Error.wav",
    [SND_MINIMIZE] = "Windows Minimize.wav", [SND_MAXIMIZE] = "Windows Restore.wav",
    [SND_RESTOREUP] = "Windows Restore.wav", [SND_RESTOREDOWN] = "Windows Restore.wav",
    [SND_MENUCOMMAND] = "Windows Menu Command.wav",
    [SND_MAILBEEP] = "Windows Notify.wav", [SND_NOTIFICATION] = "Windows Balloon.wav",
    [SND_NAVIGATING] = "Windows Navigation Start.wav", [SND_EMPTYRECYCLE] = "Windows Recycle.wav",
} };

static const struct { const char *id, *label; const PackMap *map; const char *base; } packs[] = {
    { "win98",   "Windows 98",   &map98,   NULL },
    { "win2000", "Windows 2000", &map2000, NULL },
    { "winxp",   "Windows XP",   &mapxp,   NULL },
    { "win7",    "Windows 7",    &map7,    NULL },
    { "win7-afternoon",   "Windows 7 - Afternoon",   &map7, "win7" },
    { "win7-calligraphy", "Windows 7 - Calligraphy", &map7, "win7" },
    { "win7-characters",  "Windows 7 - Characters",  &map7, "win7" },
    { "win7-cityscape",   "Windows 7 - Cityscape",   &map7, "win7" },
    { "win7-delta",       "Windows 7 - Delta",       &map7, "win7" },
    { "win7-festival",    "Windows 7 - Festival",    &map7, "win7" },
    { "win7-garden",      "Windows 7 - Garden",      &map7, "win7" },
    { "win7-heritage",    "Windows 7 - Heritage",    &map7, "win7" },
    { "win7-landscape",   "Windows 7 - Landscape",   &map7, "win7" },
    { "win7-quirky",      "Windows 7 - Quirky",      &map7, "win7" },
    { "win7-raga",        "Windows 7 - Raga",        &map7, "win7" },
    { "win7-savanna",     "Windows 7 - Savanna",     &map7, "win7" },
    { "win7-sonata",      "Windows 7 - Sonata",      &map7, "win7" },
};
#define NPACKS ((int)(sizeof packs / sizeof *packs))

static int pack_index(const char *id)
{
    for (int i = 0; i < NPACKS; i++) if (!strcasecmp(packs[i].id, id)) return i;
    return -1;
}

/* Where a pack directory lives: the user's own, beside the binaries when
 * run from the source tree, and the installed copy. */
int w2k_sound_pack_dir(const char *pack, char *out, int n)
{
    const char *home = getenv("HOME");
    if (home) {
        snprintf(out, (size_t)n, "%s/.w2k/sounds/%s", home, pack);
        if (access(out, R_OK) == 0) return 1;
    }
    char exe[768];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            snprintf(out, (size_t)n, "%.700s/../sounds/%.40s", exe, pack);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    snprintf(out, (size_t)n, W2K_PREFIX "/share/w2k/sounds/%s", pack);
    return access(out, R_OK) == 0;
}

int w2k_sound_packs(char (*ids)[32], char (*labels)[48], int max)
{
    int n = 0;
    for (int i = 0; i < NPACKS && n < max; i++) {
        char dir[1024];
        if (!w2k_sound_pack_dir(packs[i].id, dir, sizeof dir)) continue;
        snprintf(ids[n], 32, "%s", packs[i].id);
        snprintf(labels[n], 48, "%s", packs[i].label);
        n++;
    }
    return n;
}

const char *w2k_sound_pack_label(const char *id)
{
    int i = pack_index(id);
    return i >= 0 ? packs[i].label : id;
}

/* The file a pack gives an event, or "" -- looked for in the pack's own
 * directory and, for a Windows 7 theme, in Windows 7's. */
const char *w2k_sound_default(int ev, const char *pack)
{
    int i = pack_index(pack);
    if (i < 0 || ev < 0 || ev >= N_SOUNDS) return "";
    const char *f = packs[i].map->files[ev];
    return f ? f : "";
}

static int find_in_pack(const char *pack, const char *file, char *out, int n)
{
    if (!file || !*file) return 0;
    int i = pack_index(pack);
    const char *dirs[2] = { pack, i >= 0 ? packs[i].base : NULL };
    for (int k = 0; k < 2; k++) {
        if (!dirs[k]) continue;
        char dir[1024];
        if (!w2k_sound_pack_dir(dirs[k], dir, sizeof dir)) continue;
        snprintf(out, (size_t)n, "%.900s/%.100s", dir, file);
        if (access(out, R_OK) == 0) return 1;
    }
    return 0;
}

int w2k_sound_file(int ev, char *out, int n)
{
    if (ev < 0 || ev >= N_SOUNDS) return 0;
    const char *ov = w2k_sound_override[ev];
    if (ov[0]) {
        if (!strcasecmp(ov, "none")) return 0;
        if (ov[0] == '/') {
            snprintf(out, (size_t)n, "%s", ov);
            return access(out, R_OK) == 0;
        }
        return find_in_pack(w2k_sound_pack, ov, out, n);
    }
    return find_in_pack(w2k_sound_pack, w2k_sound_default(ev, w2k_sound_pack), out, n);
}

/* ---- Playing ---------------------------------------------------------- */
static int in_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    char *save = NULL;
    for (char *q = strtok_r(copy, ":", &save); q && !found; q = strtok_r(NULL, ":", &save)) {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s/%s", q, prog);
        if (access(tmp, X_OK) == 0) found = 1;
    }
    free(copy);
    return found;
}

/* The player, chosen once: PulseAudio's (which PipeWire answers too),
 * then PipeWire's own, ALSA's, and the media players. */
static const char *player(void)
{
    static const char *chosen;
    static int done;
    if (done) return chosen;
    done = 1;
    const char *env = getenv("W2K_SOUND_PLAYER");
    if (env && *env) { chosen = env; return chosen; }
    const char *cands[] = { "paplay", "pw-play", "aplay", "ffplay", "mpv", NULL };
    for (int i = 0; cands[i]; i++)
        if (in_path(cands[i])) { chosen = cands[i]; break; }
    return chosen;
}

void w2k_sound_play_file(const char *path)
{
    if (!path || !*path || w2k_sound_volume <= 0) return;
    const char *p = player();
    if (!p) return;
    char vol[40];
    char *argv[8];
    int n = 0;
    argv[n++] = (char *)p;
    if (!strcmp(p, "paplay")) {
        snprintf(vol, sizeof vol, "--volume=%d", 65536 * w2k_sound_volume / 100);
        argv[n++] = vol;
    } else if (!strcmp(p, "pw-play")) {
        snprintf(vol, sizeof vol, "--volume=%.2f", w2k_sound_volume / 100.0);
        argv[n++] = vol;
    } else if (!strcmp(p, "ffplay")) {
        argv[n++] = "-nodisp"; argv[n++] = "-autoexit"; argv[n++] = "-loglevel";
        argv[n++] = "quiet"; argv[n++] = "-volume";
        snprintf(vol, sizeof vol, "%d", w2k_sound_volume);
        argv[n++] = vol;
    } else if (!strcmp(p, "mpv")) {
        argv[n++] = "--no-video"; argv[n++] = "--really-quiet";
        snprintf(vol, sizeof vol, "--volume=%d", w2k_sound_volume);
        argv[n++] = vol;
    } else if (!strcmp(p, "aplay")) {
        argv[n++] = "-q";
    }
    argv[n++] = (char *)path;
    argv[n] = NULL;

    /* Twice forked: the player is nobody's child to wait for. */
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        if (fork() == 0) {
            int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
            signal(SIGPIPE, SIG_DFL);
            execvp(argv[0], argv);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);
}

void w2k_sound_play(int ev)
{
    if (getenv("W2K_RENDER")) return;          /* the harness is silent */
    char path[1200];
    if (w2k_sound_file(ev, path, sizeof path)) w2k_sound_play_file(path);
}

/* The .wav files a pack offers, sorted; for the applet's Name box. */
static int name_cmp(const void *a, const void *b) { return strcasecmp(a, b); }

int w2k_sound_pack_files(const char *pack, char (*out)[128], int max)
{
    int n = 0;
    int i = pack_index(pack);
    const char *dirs[2] = { pack, i >= 0 ? packs[i].base : NULL };
    for (int k = 0; k < 2; k++) {
        if (!dirs[k]) continue;
        char dir[1024];
        if (!w2k_sound_pack_dir(dirs[k], dir, sizeof dir)) continue;
        DIR *dp = opendir(dir);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) && n < max) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcasecmp(dot, ".wav")) continue;
            int dup = 0;
            for (int j = 0; j < n; j++) if (!strcmp(out[j], e->d_name)) dup = 1;
            if (!dup) snprintf(out[n++], 128, "%s", e->d_name);
        }
        closedir(dp);
    }
    qsort(out, (size_t)n, 128, name_cmp);
    return n;
}
