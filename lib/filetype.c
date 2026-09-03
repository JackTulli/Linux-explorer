/* filetype.c -- what a file is called, and which icon it wears.
 *
 * Windows keeps this in the registry, one key per extension, pointing at
 * a friendly name and an icon inside a DLL. There is no registry here, so
 * the common types are a table and everything else falls back the way
 * Explorer does: "PNG File", with the generic document icon.
 *
 * Explorer, the desktop and the property sheet all ask here, so a file
 * cannot be a "Text Document" in one window and a "TXT File" in another. */
#include "w2k.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const struct { const char *ext, *name; int icon; } types[] = {
    /* Text and configuration */
    { "txt",  "Text Document",              ICO_FILE_TEXT },
    { "log",  "Text Document",              ICO_FILE_TEXT },
    { "md",   "Text Document",              ICO_FILE_TEXT },
    { "ini",  "Configuration Settings",     ICO_FILE_INI  },
    { "inf",  "Setup Information",          ICO_FILE_INI  },
    { "conf", "Configuration Settings",     ICO_FILE_INI  },
    { "cfg",  "Configuration Settings",     ICO_FILE_INI  },
    { "rtf",  "Rich Text Format",           ICO_FILE_RTF  },
    { "doc",  "WordPad Document",           ICO_FILE_RTF  },
    { "pdf",  "Adobe Acrobat Document",     ICO_FILE      },
    /* Web */
    { "html", "HTML Document",              ICO_FILE_HTML },
    { "htm",  "HTML Document",              ICO_FILE_HTML },
    { "xml",  "XML Document",               ICO_FILE_HTML },
    { "css",  "Cascading Style Sheet",      ICO_FILE_HTML },
    { "url",  "Internet Shortcut",          ICO_FILE_HTML },
    /* Pictures */
    { "bmp",  "Bitmap Image",               ICO_FILE_BITMAP },
    { "png",  "PNG Image",                  ICO_FILE_BITMAP },
    { "jpg",  "JPEG Image",                 ICO_FILE_JPEG },
    { "jpeg", "JPEG Image",                 ICO_FILE_JPEG },
    { "gif",  "GIF Image",                  ICO_FILE_GIF  },
    { "tif",  "TIFF Image",                 ICO_FILE_BITMAP },
    { "tiff", "TIFF Image",                 ICO_FILE_BITMAP },
    { "ico",  "Icon",                       ICO_FILE_BITMAP },
    { "xpm",  "Bitmap Image",               ICO_FILE_BITMAP },
    { "svg",  "SVG Image",                  ICO_FILE_BITMAP },
    { "webp", "WebP Image",                 ICO_FILE_BITMAP },
    /* Sound and video */
    { "wav",  "Wave Sound",                 ICO_FILE_WAVE },
    { "mp3",  "MP3 Format Sound",           ICO_FILE_MEDIA },
    { "ogg",  "Ogg Format Sound",           ICO_FILE_MEDIA },
    { "flac", "FLAC Format Sound",          ICO_FILE_MEDIA },
    { "m4a",  "MPEG Format Sound",          ICO_FILE_MEDIA },
    { "mid",  "MIDI Sequence",              ICO_FILE_MIDI },
    { "midi", "MIDI Sequence",              ICO_FILE_MIDI },
    { "avi",  "Video Clip",                 ICO_FILE_MOVIE },
    { "mpg",  "Movie Clip",                 ICO_FILE_MOVIE },
    { "mpeg", "Movie Clip",                 ICO_FILE_MOVIE },
    { "mp4",  "Movie Clip",                 ICO_FILE_MOVIE },
    { "mkv",  "Movie Clip",                 ICO_FILE_MOVIE },
    { "webm", "Movie Clip",                 ICO_FILE_MOVIE },
    { "mov",  "Movie Clip",                 ICO_FILE_MOVIE },
    /* Archives */
    { "zip",  "Compressed (zipped) Folder", ICO_FILE_ZIP },
    { "gz",   "Compressed Archive",         ICO_FILE_ZIP },
    { "bz2",  "Compressed Archive",         ICO_FILE_ZIP },
    { "xz",   "Compressed Archive",         ICO_FILE_ZIP },
    { "zst",  "Compressed Archive",         ICO_FILE_ZIP },
    { "tar",  "Tape Archive",               ICO_FILE_ZIP },
    { "tgz",  "Compressed Archive",         ICO_FILE_ZIP },
    { "rar",  "RAR Archive",                ICO_FILE_ZIP },
    { "7z",   "7-Zip Archive",              ICO_FILE_ZIP },
    { "deb",  "Debian Package",             ICO_FILE_ZIP },
    { "rpm",  "RPM Package",                ICO_FILE_ZIP },
    { "iso",  "Disc Image File",            ICO_DRIVE_CD },
    /* Programs and libraries */
    { "exe",  "Application",                ICO_APP },
    { "com",  "MS-DOS Application",         ICO_TERMINAL },
    { "bat",  "MS-DOS Batch File",          ICO_FILE_BAT },
    { "sh",   "Shell Script",               ICO_FILE_BAT },
    { "so",   "Application Extension",      ICO_FILE_SYS },
    { "dll",  "Application Extension",      ICO_FILE_SYS },
    { "sys",  "System File",                ICO_FILE_SYS },
    { "o",    "Object File",                ICO_FILE_SYS },
    { "a",    "Static Library",             ICO_FILE_SYS },
    /* Fonts */
    { "ttf",  "TrueType Font File",         ICO_FILE_FONT },
    { "otf",  "OpenType Font File",         ICO_FILE_FONT },
    { "pfb",  "Type 1 Font File",           ICO_FILE_FONT },
    /* Shortcuts */
    /* A shortcut wears its target's icon plus the arrow overlay, which
     * the caller composites; the type table only supplies a fallback. */
    { "lnk",  "Shortcut",                   ICO_APP },
    { "desktop", "Shortcut",                ICO_APP },
    { NULL, NULL, 0 }
};

/* Source files are all "%s Source File"; listing every language in the
 * table above would be noise. */
static const char *const sources[] = {
    "c", "h", "cc", "cpp", "hpp", "cs", "java", "py", "pl", "rb", "go",
    "rs", "js", "ts", "lua", "php", "sql", "asm", "s", "m", "vb", NULL
};

const char *w2k_file_ext(const char *name)
{
    const char *d = strrchr(name, '.');
    return (d && d != name) ? d + 1 : "";
}

static int find(const char *ext)
{
    if (!*ext) return -1;
    for (int i = 0; types[i].ext; i++)
        if (!strcasecmp(types[i].ext, ext)) return i;
    return -1;
}

void w2k_file_type(const char *name, int isdir, char *out, int n)
{
    if (isdir) { snprintf(out, n, "File Folder"); return; }

    const char *e = w2k_file_ext(name);
    int i = find(e);
    if (i >= 0) { snprintf(out, n, "%s", types[i].name); return; }

    for (int k = 0; sources[k]; k++)
        if (!strcasecmp(sources[k], e)) {
            snprintf(out, n, "%s Source File", e);
            /* Windows uppercases the extension in the type name. */
            for (char *p = out; *p && *p != ' '; p++)
                if (*p >= 'a' && *p <= 'z') *p -= 32;
            return;
        }

    if (!*e) { snprintf(out, n, "File"); return; }
    snprintf(out, n, "%s File", e);
    for (char *p = out; *p && *p != ' '; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
}

/* Is the extension one this shell claims to know? "Hide extensions for
 * known file types" hides exactly these. */
int w2k_file_known_ext(const char *name)
{
    const char *e = w2k_file_ext(name);
    if (!*e) return 0;
    if (find(e) >= 0) return 1;
    for (int k = 0; sources[k]; k++)
        if (!strcasecmp(sources[k], e)) return 1;
    return 0;
}

/* The name as Explorer shows it: without the extension when that is
 * hidden. Returns `out`. */
const char *w2k_file_display_name(const char *name, int isdir, char *out, int n)
{
    snprintf(out, n, "%s", name);
    if (isdir || !w2k_folder_hide_ext || !w2k_file_known_ext(name)) return out;
    char *d = strrchr(out, '.');
    if (d && d != out) *d = 0;
    return out;
}

int w2k_file_icon(const char *name, int isdir)
{
    if (isdir) return ICO_FOLDER;
    const char *e = w2k_file_ext(name);
    int i = find(e);
    if (i >= 0) return types[i].icon;
    for (int k = 0; sources[k]; k++)
        if (!strcasecmp(sources[k], e)) return ICO_FILE_TEXT;
    return *e ? ICO_FILE : ICO_FILE_UNKNOWN;
}

/* A file with the execute bit and no extension is a program, whatever its
 * name says -- that is most of /usr/bin. */
int w2k_file_icon_stat(const char *path, const char *name, int isdir)
{
    int id = w2k_file_icon(name, isdir);
    if (!isdir && id == ICO_FILE_UNKNOWN) {
        struct stat st;
        if (stat(path, &st) == 0 && (st.st_mode & 0111)) return ICO_APP;
    }
    return id;
}
