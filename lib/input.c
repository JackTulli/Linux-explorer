/* input.c -- push the Mouse and Keyboard settings into the X server.
 *
 * Windows keeps these in the registry and the driver reads them; here the
 * equivalent is XChangePointerControl and friends, applied when the
 * session starts and again whenever an applet commits. */
#include "w2k.h"
#include <X11/XKBlib.h>
#include <X11/extensions/dpms.h>

void w2k_input_apply(void)
{
    if (!w2k.dpy) return;

    /* "Turn off monitor": DPMS, the power scheme's minutes; the X screen
     * saver's blanking is set to the same so the two never disagree. */
    {
        int ev, err;
        int secs = w2k_monitor_off_min > 0 ? w2k_monitor_off_min * 60 : 0;
        if (DPMSQueryExtension(w2k.dpy, &ev, &err) && DPMSCapable(w2k.dpy)) {
            if (secs > 0) {
                DPMSEnable(w2k.dpy);
                DPMSSetTimeouts(w2k.dpy, (CARD16)secs, (CARD16)secs, (CARD16)secs);
            } else {
                DPMSDisable(w2k.dpy);
            }
        }
        XSetScreenSaver(w2k.dpy, secs, 0, DefaultBlanking, DefaultExposures);
    }

    /* Button order. Anything past the first three (wheel, side buttons)
     * keeps its identity mapping. */
    unsigned char map[32];
    int n = XGetPointerMapping(w2k.dpy, map, sizeof map);
    if (n >= 3) {
        map[0] = (unsigned char)(w2k_mouse_swap ? 3 : 1);
        map[1] = 2;
        map[2] = (unsigned char)(w2k_mouse_swap ? 1 : 3);
        /* A button held down makes the server refuse the change; that is
         * the caller's problem to retry, not a reason to complain. */
        XSetPointerMapping(w2k.dpy, map, n);
    }

    /* Pointer speed. The slider is 1..10 with 4 as "no acceleration";
     * above that the numerator rises, below it the threshold does. */
    int accel = w2k_mouse_speed < 1 ? 1 : w2k_mouse_speed > 10 ? 10
                                                               : w2k_mouse_speed;
    XChangePointerControl(w2k.dpy, True, True,
                          accel <= 4 ? 1 : accel - 3, 1,
                          accel <= 4 ? 12 - accel * 2 : 4);

    /* Auto-repeat. XKB takes milliseconds for both; the applet thinks in
     * characters per second for the rate, as the Windows dialog does. */
    int rate = w2k_key_rate < 1 ? 1 : w2k_key_rate;
    XkbSetAutoRepeatRate(w2k.dpy, XkbUseCoreKbd, (unsigned)w2k_key_delay,
                         (unsigned)(1000 / rate));

    XKeyboardControl kc;
    kc.bell_percent = w2k_bell_on ? w2k_bell_volume : 0;
    kc.bell_pitch = w2k_bell_pitch;
    kc.bell_duration = w2k_bell_duration;
    XChangeKeyboardControl(w2k.dpy, KBBellPercent | KBBellPitch | KBBellDuration,
                           &kc);
    XFlush(w2k.dpy);
}
