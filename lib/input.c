/* input.c -- push the Mouse and Keyboard settings into the X server.
 *
 * Windows keeps these in the registry and the driver reads them; here the
 * equivalent is XChangePointerControl and friends, applied when the
 * session starts and again whenever an applet commits. */
#include "w2k.h"
#include <X11/XKBlib.h>
#include <X11/extensions/dpms.h>
#include <X11/Xatom.h>
#include <stdint.h>
#include <string.h>
#ifdef HAVE_XI2
#include <X11/extensions/XInput2.h>
#endif

#ifdef HAVE_XI2
/* The libinput driver -- what a current Xorg uses for every mouse and
 * touchpad -- turns the server's own acceleration off and ignores what
 * XChangePointerControl sets: the pointer speed is its "Accel Speed"
 * property, -1 to 1, and acceleration is its adaptive profile, or none
 * with the flat one. Set on every pointer that has them; 1 if any did. */
static int libinput_apply(int speed, int accel)
{
    int op, ev, err, major = 2, minor = 0;
    if (!XQueryExtension(w2k.dpy, "XInputExtension", &op, &ev, &err) ||
        XIQueryVersion(w2k.dpy, &major, &minor) != Success) return 0;
    Atom a_speed = XInternAtom(w2k.dpy, "libinput Accel Speed", True);
    Atom a_prof = XInternAtom(w2k.dpy, "libinput Accel Profile Enabled", True);
    Atom a_float = XInternAtom(w2k.dpy, "FLOAT", True);
    if (a_speed == None || a_float == None) return 0;

    /* 4, the middle of the slider and the default, is the driver's 0. */
    float v = speed <= 4 ? (speed - 4) / 3.0f : (speed - 4) / 6.0f;
    if (v < -1) v = -1;
    if (v > 1) v = 1;

    int n = 0, done = 0;
    XIDeviceInfo *devs = XIQueryDevice(w2k.dpy, XIAllDevices, &n);
    for (int i = 0; devs && i < n; i++) {
        if (devs[i].use != XISlavePointer || !devs[i].enabled) continue;
        Atom type;
        int fmt;
        unsigned long items, after;
        unsigned char *data = NULL;
        if (XIGetProperty(w2k.dpy, devs[i].deviceid, a_speed, 0, 1, False,
                          a_float, &type, &fmt, &items, &after, &data) != Success ||
            type != a_float || fmt != 32 || items < 1) {
            if (data) XFree(data);
            continue;
        }
        XFree(data);
        uint32_t word;                  /* XI2 format 32 is 32 bits, not a long */
        memcpy(&word, &v, sizeof word);
        XIChangeProperty(w2k.dpy, devs[i].deviceid, a_speed, a_float, 32,
                         PropModeReplace, (unsigned char *)&word, 1);
        done = 1;

        /* Adaptive, flat[, custom]: one of them on. */
        data = NULL;
        if (a_prof != None &&
            XIGetProperty(w2k.dpy, devs[i].deviceid, a_prof, 0, 8, False,
                          XA_INTEGER, &type, &fmt, &items, &after, &data) == Success &&
            type == XA_INTEGER && fmt == 8 && items >= 2 && items <= 8) {
            unsigned char want[8] = { 0 };
            want[accel ? 0 : 1] = 1;
            XIChangeProperty(w2k.dpy, devs[i].deviceid, a_prof, XA_INTEGER, 8,
                             PropModeReplace, want, (int)items);
        }
        if (data) XFree(data);
    }
    if (devs) XIFreeDeviceInfo(devs);
    return done;
}
#endif

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

    /* Pointer speed and acceleration, which Windows keeps apart: the
     * speed scales every movement, the acceleration decides how much
     * further a fast one goes. X has one mechanism for both -- a
     * numerator over a denominator, past a threshold -- so the speed
     * sets the ratio and the acceleration sets the threshold, with
     * "None" turning the server's acceleration off altogether. */
    int sp = w2k_mouse_speed < 1 ? 1 : w2k_mouse_speed > 10 ? 10 : w2k_mouse_speed;
    int ac = w2k_mouse_accel < 0 ? 0 : w2k_mouse_accel > 3 ? 3 : w2k_mouse_accel;
    /* A threshold of 1 for "None": 0 picks the server's polynomial
     * profile, which accelerates -- the opposite of what was asked. */
    static const int thresh[4] = { 1, 8, 4, 2 };       /* sooner is more */
    XChangePointerControl(w2k.dpy, True, True, sp, 4, thresh[ac]);
#ifdef HAVE_XI2
    libinput_apply(sp, ac);
#endif

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
