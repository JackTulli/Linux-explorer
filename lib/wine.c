/* wine.c -- Windows programs, through Wine.
 *
 * The desktop treats a Windows program as a program: an .exe, .msi, .lnk
 * or .bat opens with `wine start /unix`, which hands it to whatever the
 * prefix associates it with (an installer runs, a shortcut is followed);
 * an .exe shows its own icon in Explorer when icoutils' wrestool is
 * there to pull it out; and what Wine installs turns up in the Start
 * menu's Windows Programs group (see wm/programs.c). The prefix is
 * Wine's own (WINEPREFIX, or ~/.wine), made by l2k-session the first
 * time it is missing. */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

static int tool_present(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) return 0;
    char copy[4096], full[4200], *save = NULL;
    snprintf(copy, sizeof copy, "%s", path);
    for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        snprintf(full, sizeof full, "%s/%s", d, name);
        if (access(full, X_OK) == 0) return 1;
    }
    return 0;
}

int w2k_wine_available(void)
{
    static int have = -1;
    if (have < 0) have = tool_present("wine");
    return have;
}

void w2k_wine_prefix(char *buf, int n)
{
    const char *p = getenv("WINEPREFIX");
    if (p && *p) { snprintf(buf, (size_t)n, "%s", p); return; }
    const char *h = getenv("HOME");
    snprintf(buf, (size_t)n, "%s/.wine", h ? h : ".");
}

/* Is the name a Windows program, installer, shortcut or batch file? */
int w2k_wine_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *const exts[] = { ".exe", ".msi", ".lnk", ".bat", ".com", ".cmd" };
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
        if (!strcasecmp(dot, exts[i])) return 1;
    return 0;
}

/* ------------------------------------------------------------------ *
 * The icon inside a Windows program, read from the file itself
 * ------------------------------------------------------------------ *
 * A .exe keeps its icons as resources: a group (type 14) that names the
 * images (type 3) making it up, one per size. This walks the resource
 * tree, takes the image nearest the size the shell draws, and writes it
 * out as a one-image .ico -- so a program wears its own icon whether or
 * not icoutils is installed.
 *
 * Everything read is checked against the length of the file first: this
 * is a program someone downloaded, not a file we wrote, and a malformed
 * one must come to nothing rather than anything else. */
typedef struct { FILE *f; long size; } Pe;

static int pe_at(Pe *p, long off, void *buf, long n)
{
    if (off < 0 || n < 0 || off > p->size || n > p->size - off) return 0;
    if (fseek(p->f, off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, (size_t)n, p->f) == (size_t)n;
}
static unsigned pe_u16(const unsigned char *b) { return (unsigned)(b[0] | b[1] << 8); }
static unsigned long pe_u32(const unsigned char *b)
{
    return (unsigned long)b[0] | (unsigned long)b[1] << 8 |
           (unsigned long)b[2] << 16 | (unsigned long)b[3] << 24;
}

#define PE_MAX_SECTIONS 96
typedef struct { unsigned long va, vsize, raw, rawsize; } PeSec;

/* Where an address the program uses lands in the file. */
static long pe_off_of(const PeSec *sec, int nsec, unsigned long rva)
{
    for (int i = 0; i < nsec; i++) {
        unsigned long span = sec[i].vsize > sec[i].rawsize ? sec[i].vsize : sec[i].rawsize;
        if (rva >= sec[i].va && rva < sec[i].va + span) {
            unsigned long d = rva - sec[i].va;
            if (d >= sec[i].rawsize) return -1;      /* in memory only */
            return (long)(sec[i].raw + d);
        }
    }
    return -1;
}

/* One entry of a resource directory: the one whose id matches, or the
 * first of them when `id` is PE_ANY. `sub` says whether what came back
 * is another directory. */
#define PE_ANY 0xffffffffUL
static long pe_res_entry(Pe *p, long root, long dir, unsigned long id, int *sub)
{
    unsigned char hdr[16];
    if (!pe_at(p, dir, hdr, 16)) return -1;
    int nnamed = (int)pe_u16(hdr + 12), nid = (int)pe_u16(hdr + 14);
    if (nnamed < 0 || nid < 0 || nnamed + nid > 8192) return -1;
    for (int i = 0; i < nnamed + nid; i++) {
        unsigned char e[8];
        if (!pe_at(p, dir + 16 + 8 * i, e, 8)) return -1;
        unsigned long name = pe_u32(e), off = pe_u32(e + 4);
        if (id != PE_ANY) {
            if (name & 0x80000000UL) continue;       /* named, not numbered */
            if (name != id) continue;
        }
        if (sub) *sub = (off & 0x80000000UL) ? 1 : 0;
        return root + (long)(off & 0x7fffffffUL);
    }
    return -1;
}

/* Down to the data: directory -> first entry -> first language. */
static int pe_res_data(Pe *p, long root, long dir, unsigned long id,
                       unsigned long *rva, unsigned long *size)
{
    int sub = 0;
    long e = pe_res_entry(p, root, dir, id, &sub);
    if (e < 0) return 0;
    while (sub) {
        e = pe_res_entry(p, root, e, PE_ANY, &sub);
        if (e < 0) return 0;
    }
    unsigned char d[16];
    if (!pe_at(p, e, d, 16)) return 0;
    *rva = pe_u32(d);
    *size = pe_u32(d + 4);
    return 1;
}

/* The program's icon, written to `out` as a .ico. 1 when there was one. */
static int exe_icon_to_file(const char *exe, const char *out)
{
    Pe p;
    p.f = fopen(exe, "rb");
    if (!p.f) return 0;
    int ok = 0;
    unsigned char *grp = NULL, *img = NULL;
    if (fseek(p.f, 0, SEEK_END) != 0) goto done;
    p.size = ftell(p.f);
    if (p.size < 64) goto done;

    unsigned char b[64];
    if (!pe_at(&p, 0, b, 64) || b[0] != 'M' || b[1] != 'Z') goto done;
    long pe_off = (long)pe_u32(b + 60);
    unsigned char coff[24];
    if (!pe_at(&p, pe_off, coff, 24) || memcmp(coff, "PE\0\0", 4)) goto done;
    int nsec = (int)pe_u16(coff + 6);
    int optsize = (int)pe_u16(coff + 20);
    if (nsec <= 0 || nsec > PE_MAX_SECTIONS || optsize < 96) goto done;

    unsigned char magic[2];
    if (!pe_at(&p, pe_off + 24, magic, 2)) goto done;
    long dd = pe_off + 24 + (pe_u16(magic) == 0x20b ? 112 : 96);
    unsigned char rd[8];
    if (!pe_at(&p, dd + 16, rd, 8)) goto done;       /* directory 2: resources */
    unsigned long res_rva = pe_u32(rd);
    if (!res_rva) goto done;

    PeSec sec[PE_MAX_SECTIONS];
    for (int i = 0; i < nsec; i++) {
        unsigned char sh[40];
        if (!pe_at(&p, pe_off + 24 + optsize + 40 * i, sh, 40)) goto done;
        sec[i].vsize = pe_u32(sh + 8);
        sec[i].va    = pe_u32(sh + 12);
        sec[i].rawsize = pe_u32(sh + 16);
        sec[i].raw   = pe_u32(sh + 20);
    }
    long root = pe_off_of(sec, nsec, res_rva);
    if (root < 0) goto done;

    /* The first icon group is the program's own, as Windows takes it. */
    unsigned long grva = 0, gsize = 0;
    if (!pe_res_data(&p, root, root, 14, &grva, &gsize)) goto done;
    long goff = pe_off_of(sec, nsec, grva);
    if (goff < 0 || gsize < 6 || gsize > (1UL << 20)) goto done;
    grp = malloc(gsize);
    if (!grp || !pe_at(&p, goff, grp, (long)gsize)) goto done;

    int count = (int)pe_u16(grp + 4);
    if (count <= 0 || (unsigned long)(6 + 14 * count) > gsize) goto done;
    /* The one nearest 32 pixels, the size the shell draws; a 0 width
     * means 256. Bigger beats smaller at the same distance. */
    int best = -1, best_score = -1;
    for (int i = 0; i < count; i++) {
        const unsigned char *e = grp + 6 + 14 * i;
        int wpx = e[0] ? e[0] : 256, bits = (int)pe_u16(e + 6);
        int d = wpx > 32 ? wpx - 32 : 32 - wpx;
        int score = 1000 - d * 4 + (bits >= 32 ? 3 : bits >= 8 ? 1 : 0);
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best < 0) goto done;
    const unsigned char *ge = grp + 6 + 14 * best;
    unsigned long want = pe_u16(ge + 12);            /* the image's id */

    unsigned long irva = 0, isize = 0;
    if (!pe_res_data(&p, root, root, 3, &irva, &isize)) goto done;
    {   /* type 3 holds every image: find the one this group names. */
        int sub = 0;
        long tdir = pe_res_entry(&p, root, root, 3, &sub);
        if (tdir < 0 || !sub) goto done;
        long idir = pe_res_entry(&p, root, tdir, want, &sub);
        if (idir < 0) goto done;
        while (sub) {
            idir = pe_res_entry(&p, root, idir, PE_ANY, &sub);
            if (idir < 0) goto done;
        }
        unsigned char d[16];
        if (!pe_at(&p, idir, d, 16)) goto done;
        irva = pe_u32(d);
        isize = pe_u32(d + 4);
    }
    long ioff = pe_off_of(sec, nsec, irva);
    if (ioff < 0 || isize < 8 || isize > (16UL << 20)) goto done;
    img = malloc(isize);
    if (!img || !pe_at(&p, ioff, img, (long)isize)) goto done;

    /* A one-image .ico around it: the header Windows would have written. */
    FILE *o = fopen(out, "wb");
    if (!o) goto done;
    unsigned char hdr[22];
    memset(hdr, 0, sizeof hdr);
    hdr[2] = 1;                                      /* an icon, not a cursor */
    hdr[4] = 1;                                      /* one image */
    hdr[6] = ge[0]; hdr[7] = ge[1]; hdr[8] = ge[2];  /* width, height, colours */
    hdr[10] = ge[4]; hdr[11] = ge[5];                /* planes */
    hdr[12] = ge[6]; hdr[13] = ge[7];                /* bit depth */
    hdr[14] = (unsigned char)(isize & 0xff);
    hdr[15] = (unsigned char)((isize >> 8) & 0xff);
    hdr[16] = (unsigned char)((isize >> 16) & 0xff);
    hdr[17] = (unsigned char)((isize >> 24) & 0xff);
    hdr[18] = 22;                                    /* where the image starts */
    ok = fwrite(hdr, 1, sizeof hdr, o) == sizeof hdr &&
         fwrite(img, 1, isize, o) == isize;
    if (fclose(o) != 0) ok = 0;
    if (!ok) unlink(out);
done:
    free(grp);
    free(img);
    fclose(p.f);
    return ok;
}

/* The icon inside an .exe, pulled out with wrestool into ~/.w2k/cache and
 * registered once; ICO_APP without wrestool, or when there is none. Kept
 * by path and modification time, so a replaced program shows its new one. */
int w2k_wine_exe_icon(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return ICO_APP;

    static struct { char *path; long mtime; int id; } seen[256];
    static int nseen, next;
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i].path, path) && seen[i].mtime == (long)st.st_mtime) return seen[i].id;

    /* A stable name for the cache file: the path and time, hashed. */
    unsigned long h = 5381;
    for (const char *p = path; *p; p++) h = h * 33 + (unsigned char)*p;
    h = h * 33 + (unsigned long)st.st_mtime;
    const char *home = getenv("HOME");
    char dir[1100], ico[1200];
    snprintf(dir, sizeof dir, "%s/.w2k/cache/exe-icons", home ? home : ".");
    snprintf(ico, sizeof ico, "%s/%lx.ico", dir, h);
    int id = -1;
    if (access(ico, R_OK) != 0) {
        char mk[1200];
        snprintf(mk, sizeof mk, "%s/.w2k", home ? home : ".");
        mkdir(mk, 0755);                      /* and its parent, on a new home */
        snprintf(mk, sizeof mk, "%s/.w2k/cache", home ? home : ".");
        mkdir(mk, 0755);
        mkdir(dir, 0755);
        /* The file itself first. wrestool is asked only for what this
         * cannot read, and is not needed for an ordinary program. */
        static int have_wrestool = -1;
        if (have_wrestool < 0) have_wrestool = tool_present("wrestool");
        if (!exe_icon_to_file(path, ico) && have_wrestool) {
        /* wrestool writes one file per icon group when told a directory;
         * the first group is the program's own icon. */
        char tmp[1300];
        snprintf(tmp, sizeof tmp, "%s/%lx.d", dir, h);
        mkdir(tmp, 0755);
        char q[4200], qt[2700], cmd[8000];
        w2k_shell_quote(path, q, sizeof q);
        w2k_shell_quote(tmp, qt, sizeof qt);      /* $HOME may hold a space */
        snprintf(cmd, sizeof cmd, "wrestool -x -t 14 -o %s %s >/dev/null 2>&1", qt, q);
        if (system(cmd) == 0) {
            DIR *dp = opendir(tmp);
            char best[1400] = "";
            if (dp) {
                struct dirent *e;
                while ((e = readdir(dp))) {
                    size_t n = strlen(e->d_name);
                    if (n < 5 || strcasecmp(e->d_name + n - 4, ".ico")) continue;
                    if (!best[0] || strcmp(e->d_name, best + strlen(tmp) + 1) < 0)
                        snprintf(best, sizeof best, "%s/%s", tmp, e->d_name);
                }
                closedir(dp);
            }
            if (best[0]) rename(best, ico);
        }
        /* Whatever else came out is not needed. Unlinked here rather than
         * through a shell: the path holds $HOME, and "rm -rf" on an
         * unquoted one with a space in it deletes the wrong thing. */
        DIR *rd = opendir(tmp);
        if (rd) {
            struct dirent *e;
            while ((e = readdir(rd))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char victim[1500];
                snprintf(victim, sizeof victim, "%s/%s", tmp, e->d_name);
                unlink(victim);
            }
            closedir(rd);
        }
        rmdir(tmp);
        }
    }
    if (access(ico, R_OK) == 0) id = w2k_icon_from_file(ico);
    if (id < 0) id = ICO_APP;
    int slot = nseen < 256 ? nseen++ : next++ % 256;
    free(seen[slot].path);
    seen[slot].path = w2k_strdup(path);
    seen[slot].mtime = (long)st.st_mtime;
    seen[slot].id = id;
    return id;
}
