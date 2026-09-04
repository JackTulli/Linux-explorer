/* tooltip.c -- the little yellow box.
 *
 * One shared window, mapped where it is needed and hidden again: a tooltip
 * is never nested and never more than one at a time, so a single
 * override-redirect window with save_under is the whole mechanism.
 *
 * Callers decide when: they know when the pointer settled and on what. The
 * toolkit only knows how to draw it and where it fits. */
#include "w2k.h"
#include <stdio.h>
#include <string.h>

#define TIP_PAD_X 4
#define TIP_PAD_Y 2

static Window tip;
static char   tip_text[256];

static void tip_paint(int w, int h)
{
    w2k_fill(tip, 0, 0, w, h, C_TOOLTIP);
    w2k_frame(tip, 0, 0, w, h, C_WINDOWFRAME);
    w2k_text(tip, F_UI, TIP_PAD_X, TIP_PAD_Y, tip_text, C_TOOLTIPTEXT);
}

/* Show `text` near (x, y) in root coordinates -- below and right of the
 * pointer, nudged back onto the monitor if it would hang off. */
void w2k_tooltip_show(const char *text, int x, int y)
{
    if (!text || !*text) { w2k_tooltip_hide(); return; }
    if (!strcmp(text, tip_text) && tip) return;      /* already up */

    snprintf(tip_text, sizeof tip_text, "%s", text);
    int w = w2k_text_width(F_UI, tip_text, -1) + 2 * TIP_PAD_X + 1;
    int h = w2k_font_height(F_UI) + 2 * TIP_PAD_Y + 1;

    const W2kMonitor *m = w2k_monitor_at(x, y);
    if (x + w > m->x + m->w) x = m->x + m->w - w;
    if (y + h > m->y + m->h) y = y - h - 22;
    if (x < m->x) x = m->x;
    if (y < m->y) y = m->y;

    if (!tip) {
        XSetWindowAttributes a = {
            .override_redirect = True, .save_under = True,
            .background_pixel = w2k.col[C_TOOLTIP],
            .event_mask = ExposureMask
        };
        tip = XCreateWindow(w2k.dpy, w2k.root, x, y, w, h, 0, CopyFromParent,
                            InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWSaveUnder | CWBackPixel |
                            CWEventMask, &a);
    } else {
        XMoveResizeWindow(w2k.dpy, tip, x, y, w, h);
    }
    XMapRaised(w2k.dpy, tip);
    tip_paint(w, h);
    XFlush(w2k.dpy);
}

void w2k_tooltip_hide(void)
{
    if (!tip) return;
    w2k_font_forget(tip);
    XDestroyWindow(w2k.dpy, tip);
    tip = 0;
    tip_text[0] = 0;
}

/* Repaint if the tooltip window was exposed. Returns 1 if it was ours. */
int w2k_tooltip_event(XEvent *e)
{
    if (!tip || e->type != Expose || e->xexpose.window != tip) return 0;
    XWindowAttributes wa;
    if (XGetWindowAttributes(w2k.dpy, tip, &wa)) tip_paint(wa.width, wa.height);
    return 1;
}

/* Ask the shell for a balloon: the strings go into a root property, and a
 * client message tells the shell to read them. */
void w2k_notify(const char *title, const char *text)
{
    if (!title) title = "";
    if (!text) text = "";
    char buf[1024];
    int tl = snprintf(buf, sizeof buf, "%.200s", title);
    if (tl < 0) return;
    int total = tl + 1;
    int xl = snprintf(buf + total, sizeof buf - (size_t)total, "%.700s", text);
    if (xl < 0) return;
    total += xl + 1;

    XChangeProperty(w2k.dpy, w2k.root, w2k.a_w2k_notify, XA_STRING, 8,
                    PropModeReplace, (unsigned char *)buf, total);

    XEvent e = { 0 };
    e.xclient.type = ClientMessage;
    e.xclient.window = w2k.root;
    e.xclient.message_type = w2k.a_w2k_command;
    e.xclient.format = 32;
    e.xclient.data.l[0] = 4;                 /* show a balloon */
    XSendEvent(w2k.dpy, w2k.root, False, SubstructureNotifyMask, &e);
    XFlush(w2k.dpy);
}
