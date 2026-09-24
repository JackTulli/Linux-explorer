/* clipboard.c -- CLIPBOARD selection ownership, so cut and paste work both
 * inside this desktop and with the rest of X. */
#include "w2kui.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char  *clip;                 /* what we own, when we own it */
static unsigned char *clip_png;     /* or a picture, as a PNG file */
static size_t clip_png_n;
static Window owner_win;
static Atom   a_clipboard, a_targets, a_text, a_prop;

static void ensure_atoms(void)
{
    if (a_clipboard) return;
    a_clipboard = XInternAtom(w2k.dpy, "CLIPBOARD", False);
    a_targets   = XInternAtom(w2k.dpy, "TARGETS", False);
    a_text      = XInternAtom(w2k.dpy, "TEXT", False);
    a_prop      = XInternAtom(w2k.dpy, "_W2K_CLIP", False);
    owner_win   = XCreateSimpleWindow(w2k.dpy, w2k.root, -10, -10, 1, 1, 0, 0, 0);
    XSelectInput(w2k.dpy, owner_win, PropertyChangeMask);
}

static Atom a_image_png;

/* A picture is made into a PNG by a child process, beside the program:
 * done here it held the Snipping Tool frozen for a tenth of a second to
 * half a second on every snip (compressing a 4K screen), before its window
 * could even paint. The clipboard is taken when the PNG is ready, as it
 * was when the encoding ended here, and not at all if it fails. The
 * program's loop reads the pipe (w2k_run, and a dialog's). */
static pid_t enc_pid = -1;
static int   enc_fd = -1;
static unsigned char *enc_buf;          /* the PNG, as it arrives */
static size_t enc_n, enc_cap;

/* Forget a picture still being encoded: a newer copy replaces it. */
static void enc_stop(void)
{
    if (enc_pid <= 0) return;
    w2k_del_fd(enc_fd);
    close(enc_fd);
    kill(enc_pid, SIGKILL);
    waitpid(enc_pid, NULL, 0);
    enc_pid = -1;
    enc_fd = -1;
    free(enc_buf);
    enc_buf = NULL;
    enc_n = enc_cap = 0;
}

static void enc_io(void *u)
{
    (void)u;
    for (;;) {
        if (enc_cap - enc_n < 65536) {
            size_t cap = enc_cap ? enc_cap * 2 : 1 << 20;
            unsigned char *grown = realloc(enc_buf, cap);
            if (!grown) { enc_stop(); return; }
            enc_buf = grown;
            enc_cap = cap;
        }
        ssize_t r = read(enc_fd, enc_buf + enc_n, enc_cap - enc_n);
        if (r > 0) { enc_n += (size_t)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;    /* more to come */
        break;                                                      /* the end */
    }
    w2k_del_fd(enc_fd);
    close(enc_fd);
    int st = 0;
    int ok = waitpid(enc_pid, &st, 0) != enc_pid || (WIFEXITED(st) && WEXITSTATUS(st) == 0);
    /* All of it, or it is not a picture: the file ends with IEND. */
    static const unsigned char iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82 };
    ok = ok && enc_n > 8 + 12 && !memcmp(enc_buf + enc_n - 12, iend, 12);
    enc_pid = -1;
    enc_fd = -1;
    if (ok) {
        clip_png = enc_buf;
        clip_png_n = enc_n;
        XSetSelectionOwner(w2k.dpy, a_clipboard, owner_win, CurrentTime);
    } else {
        free(enc_buf);
    }
    enc_buf = NULL;
    enc_n = enc_cap = 0;
}

void w2k_clipboard_set_image(const unsigned char *rgba, int w, int h)
{
    ensure_atoms();
    if (!a_image_png) a_image_png = XInternAtom(w2k.dpy, "image/png", False);
    enc_stop();
    free(clip); clip = NULL;
    free(clip_png); clip_png = NULL;
    int fd[2];
    pid_t pid = -1;
    if (pipe(fd) == 0) {
        pid = fork();
        if (pid == 0) {
            /* The child: the picture as it was at the fork, into the pipe.
             * Nothing of X here, and _exit, not exit. */
            close(fd[0]);
            size_t n = 0, off = 0;
            unsigned char *png = w2k_png_encode(rgba, w, h, &n);
            if (!png) _exit(1);
            while (off < n) {
                ssize_t r = write(fd[1], png + off, n - off);
                if (r > 0) off += (size_t)r;
                else if (r < 0 && errno == EINTR) continue;
                else _exit(1);
            }
            _exit(0);
        }
        close(fd[1]);
        if (pid < 0) close(fd[0]);
    }
    if (pid > 0) {
        enc_pid = pid;
        enc_fd = fd[0];
        fcntl(enc_fd, F_SETFL, O_NONBLOCK);
        fcntl(enc_fd, F_SETFD, FD_CLOEXEC);
        w2k_add_fd(enc_fd, enc_io, NULL);
        return;
    }
    /* No child to be had: encoded here, as it always was. */
    clip_png = w2k_png_encode(rgba, w, h, &clip_png_n);
    if (!clip_png) return;
    XSetSelectionOwner(w2k.dpy, a_clipboard, owner_win, CurrentTime);
}

void w2k_clipboard_set(const char *text)
{
    ensure_atoms();
    enc_stop();
    free(clip);
    free(clip_png); clip_png = NULL;
    clip = w2k_strdup(text ? text : "");
    XSetSelectionOwner(w2k.dpy, a_clipboard, owner_win, CurrentTime);
    /* Mirror into PRIMARY so middle-click paste works too. */
    XSetSelectionOwner(w2k.dpy, XA_PRIMARY, owner_win, CurrentTime);
}

int w2k_clipboard_event(XEvent *e)
{
    ensure_atoms();
    if (e->type == SelectionClear) {
        if (e->xselectionclear.selection == a_clipboard) {
            free(clip);
            clip = NULL;
            free(clip_png);
            clip_png = NULL;
        }
        return 1;
    }
    if (e->type != SelectionRequest) return 0;

    XSelectionRequestEvent *r = &e->xselectionrequest;
    XSelectionEvent n = {
        .type = SelectionNotify, .display = r->display, .requestor = r->requestor,
        .selection = r->selection, .target = r->target, .property = None,
        .time = r->time
    };
    Atom prop = r->property ? r->property : r->target;

    if (!clip && !clip_png) {
        XSendEvent(w2k.dpy, r->requestor, False, 0, (XEvent *)&n);
        return 1;
    }
    if (clip_png) {
        if (!a_image_png) a_image_png = XInternAtom(w2k.dpy, "image/png", False);
        if (r->target == a_targets) {
            Atom list[] = { a_targets, a_image_png };
            XChangeProperty(w2k.dpy, r->requestor, prop, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)list, 2);
            n.property = prop;
        } else if (r->target == a_image_png) {
            XChangeProperty(w2k.dpy, r->requestor, prop, a_image_png, 8,
                            PropModeReplace, clip_png, (int)clip_png_n);
            n.property = prop;
        }
        XSendEvent(w2k.dpy, r->requestor, False, 0, (XEvent *)&n);
        return 1;
    }
    if (r->target == a_targets) {
        Atom list[] = { a_targets, a_text, XA_STRING, w2k.a_utf8 };
        XChangeProperty(w2k.dpy, r->requestor, prop, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)list, 4);
        n.property = prop;
    } else if (r->target == XA_STRING || r->target == a_text ||
               r->target == w2k.a_utf8) {
        XChangeProperty(w2k.dpy, r->requestor, prop, r->target, 8,
                        PropModeReplace, (unsigned char *)clip, strlen(clip));
        n.property = prop;
    }
    XSendEvent(w2k.dpy, r->requestor, False, 0, (XEvent *)&n);
    return 1;
}

char *w2k_clipboard_get(void)
{
    ensure_atoms();
    if (clip) return w2k_strdup(clip);          /* we own it: no round trip */
    /* We own it with a picture, which is not text. Asking ourselves stalled
     * every paste for the whole deadline: the request waits in our own
     * queue, which the loop below does not answer. A picture still being
     * encoded is one too. */
    if (clip_png || enc_pid > 0) return NULL;

    XConvertSelection(w2k.dpy, a_clipboard, w2k.a_utf8, a_prop, owner_win,
                      CurrentTime);
    XFlush(w2k.dpy);

    /* Wait briefly for the owner to answer; a dead owner must not hang us. */
    long deadline = w2k_now_ms() + 400;
    for (;;) {
        XEvent e;
        if (XCheckTypedWindowEvent(w2k.dpy, owner_win, SelectionNotify, &e)) {
            if (e.xselection.property == None) return NULL;
            Atom type;
            int fmt;
            unsigned long n, after;
            unsigned char *data = NULL;
            if (XGetWindowProperty(w2k.dpy, owner_win, a_prop, 0, 1 << 22,
                                   True, AnyPropertyType, &type, &fmt, &n,
                                   &after, &data) != Success || !data)
                return NULL;
            /* A very large selection arrives in INCR pieces, which this
             * does not collect: better nothing than the length word. */
            if (fmt != 8 || (type != w2k.a_utf8 && type != XA_STRING &&
                             type != a_text)) {
                XFree(data);
                return NULL;
            }
            char *out = w2k_alloc(n + 1);
            memcpy(out, data, n);
            out[n] = 0;
            XFree(data);
            return out;
        }
        if (w2k_now_ms() > deadline) return NULL;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}
