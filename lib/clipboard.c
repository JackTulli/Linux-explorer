/* clipboard.c -- CLIPBOARD selection ownership, so cut and paste work both
 * inside this desktop and with the rest of X. */
#include "w2kui.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>

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

void w2k_clipboard_set_image(const unsigned char *rgba, int w, int h)
{
    ensure_atoms();
    if (!a_image_png) a_image_png = XInternAtom(w2k.dpy, "image/png", False);
    free(clip); clip = NULL;
    free(clip_png);
    clip_png = w2k_png_encode(rgba, w, h, &clip_png_n);
    if (!clip_png) return;
    XSetSelectionOwner(w2k.dpy, a_clipboard, owner_win, CurrentTime);
}

void w2k_clipboard_set(const char *text)
{
    ensure_atoms();
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
