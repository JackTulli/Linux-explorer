/* recent.c -- the Documents menu.
 *
 * Windows keeps the last fifteen documents you opened and shows them under
 * Start, Documents, with a Clear that empties the list. The list here is
 * the freedesktop recently-used file, which is what every other program on
 * the system already writes to -- so opening a file in another editor puts
 * it in this menu too.
 *
 * The XML is read with a deliberately small parser: two attributes off each
 * bookmark element, no entity decoding beyond the handful that appear in
 * file URIs. Anything it cannot make sense of is skipped. */
#include "wm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_RECENT 15

static struct { char path[1024]; char label[128]; } recent[MAX_RECENT];
static int nrecent;

static void recent_path(char *buf, int n)
{
    const char *home = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/.local/share/recently-used.xbel",
             home ? home : ".");
}

/* %20 and friends, in place. */
static void unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; ) {
        if (p[0] == '%' && p[1] && p[2]) {
            int hi = p[1], lo = p[2];
            hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
            lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
            if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                *o++ = (char)(hi * 16 + lo);
                p += 3;
                continue;
            }
        }
        *o++ = *p++;
    }
    *o = 0;
}

int recent_load(void)
{
    nrecent = 0;
    char path[1024];
    recent_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    /* Read the whole file: it is a few tens of kilobytes at worst. */
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0 || len > (4 << 20)) { fclose(f); return 0; }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return 0;
    }
    buf[len] = 0;
    fclose(f);

    /* Newest last in the file, and we want newest first. */
    char *hrefs[256];
    int n = 0;
    for (char *p = strstr(buf, "href=\"file://"); p && n < 256;
         p = strstr(p + 1, "href=\"file://")) {
        char *start = p + 13;
        char *end = strchr(start, '"');
        if (!end) break;
        *end = 0;
        hrefs[n++] = start;
        p = end;
    }
    for (int i = n - 1; i >= 0 && nrecent < MAX_RECENT; i--) {
        char decoded[1024];
        snprintf(decoded, sizeof decoded, "%.1023s", hrefs[i]);
        unescape(decoded);
        if (access(decoded, R_OK) != 0) continue;      /* gone since */

        int dup = 0;
        for (int k = 0; k < nrecent; k++)
            if (!strcmp(recent[k].path, decoded)) { dup = 1; break; }
        if (dup) continue;

        snprintf(recent[nrecent].path, sizeof recent[nrecent].path, "%s", decoded);
        const char *base = strrchr(decoded, '/');
        snprintf(recent[nrecent].label, sizeof recent[nrecent].label, "%.127s",
                 base ? base + 1 : decoded);
        nrecent++;
    }
    free(buf);
    return nrecent;
}

int recent_count(void) { return nrecent; }
const char *recent_label(int i)
{
    return (i >= 0 && i < nrecent) ? recent[i].label : "";
}
const char *recent_file(int i)
{
    return (i >= 0 && i < nrecent) ? recent[i].path : NULL;
}

/* Clear: truncate the list to an empty document, which is what the
 * freedesktop file looks like with nothing in it. */
void recent_clear(void)
{
    char path[1024];
    recent_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<xbel version=\"1.0\"\n"
                   "      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
                   "      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
                   "</xbel>\n");
        fclose(f);
    }
    nrecent = 0;
}
