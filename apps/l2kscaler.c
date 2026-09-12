/* l2kscaler.c -- the nested compositor: the whole desktop, scaled by us.
 *
 * X11 lets nothing but xrandr's two filters between the framebuffer and a
 * scaled monitor, so the desktop that wants a better picture runs inside
 * a headless X server (Xvfb) at its logical size, and this program shows
 * that server's screen on the real one: one window per monitor, each
 * drawn by the GPU with mpv's EWA Lanczos-sharp -- a polar jinc filter,
 * blurred a hair, in sigmoidised linear light, with a light anti-ringing
 * clamp -- and input on those windows sent back into the nested server
 * with XTest at the matching logical position. The nested server's
 * pointer is drawn here, scaled, since the real one is hidden.
 *
 * Experimental, off unless Display Properties turns it on; l2k-session
 * starts it. With --composite there is no nested server: this display's
 * own windows are composited, as picom does it, and the programs keep the
 * GPU (see "--composite" below). Programs in a nested server render in
 * software -- Xvfb
 * has no GPU, and Xephyr, which draws with one, passes it on to nobody
 * (no DRI2 or DRI3) -- which a desktop bears and a game does not.
 *
 *   l2kscaler --nested :1 --layout "name,hW,hH,hX,hY,nW,nH,nX,nY;..."
 *             [--nested-window TITLE] [--filter NAME] [--antiring 0|1]
 *             [--linear-light] [--window] [--no-input]
 *   l2kscaler --composite --layout "..." [--filter NAME] ...
 *   l2kscaler --restore-input
 *   l2kscaler --set "filter=NAME;light=gamma|linear;antiring=0|1"
 *
 * The filters: nearest, bilinear, bicubic (Catmull-Rom), lanczos (3),
 * ewa_lanczos and ewa_lanczossharp (the default). They change while it
 * runs: the settings live in the _L2K_SCALER string property on the
 * nested server's root, which --set writes from inside the session and
 * Display Properties writes as its box is changed.
 *
 * Each layout entry is one monitor: its window on this display (size and
 * position) and the rectangle of the nested screen it shows; hW/nW is the
 * scale. With --nested-window the nested server is Xephyr and its window
 * on this display carries that title: the screen is then taken straight
 * from that window's pixmap on the GPU (Composite and texture-from-pixmap)
 * and never copied; without it the nested server is Xvfb and the screen
 * is read through shared memory. --linear-light filters in sigmoidised
 * linear light as mpv does, which keeps a photograph honest but makes
 * thin dark text look lighter; the default filters in gamma space, which
 * text weight is drawn for. --window makes ordinary windows instead of
 * full-screen ones and --no-input sends nothing back, for trying it out
 * on a desktop that is already running. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XInput2.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/select.h>
#include <sys/time.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * GL 2.0 entry points, fetched by name: libGL exports 1.x by contract.
 * ------------------------------------------------------------------ */
typedef char GLchar_;
static GLuint (*p_glCreateShader)(GLenum);
static void   (*p_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void   (*p_glCompileShader)(GLuint);
static void   (*p_glGetShaderiv)(GLuint, GLenum, GLint *);
static void   (*p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (*p_glCreateProgram)(void);
static void   (*p_glAttachShader)(GLuint, GLuint);
static void   (*p_glLinkProgram)(GLuint);
static void   (*p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void   (*p_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static void   (*p_glUseProgram)(GLuint);
static GLint  (*p_glGetUniformLocation)(GLuint, const char *);
static void   (*p_glUniform1i)(GLint, GLint);
static void   (*p_glUniform1f)(GLint, GLfloat);
static void   (*p_glUniform2f)(GLint, GLfloat, GLfloat);
static void   (*p_glActiveTexture)(GLenum);
static void   (*p_glGenFramebuffers)(GLsizei, GLuint *);
static void   (*p_glBindFramebuffer)(GLenum, GLuint);
static void   (*p_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLenum (*p_glCheckFramebufferStatus)(GLenum);
static void   (*p_glDeleteFramebuffers)(GLsizei, const GLuint *);
static void   (*p_glXSwapIntervalEXT)(Display *, GLXDrawable, int);
static void   (*p_glXBindTexImageEXT)(Display *, GLXDrawable, int, const int *);
static void   (*p_glXReleaseTexImageEXT)(Display *, GLXDrawable, int);

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
#endif
#ifndef GL_TEXTURE4
#define GL_TEXTURE4 0x84C4
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#define GETPROC(name) do { p_##name = (void *)glXGetProcAddressARB((const GLubyte *)#name); \
    if (!p_##name) { fprintf(stderr, "l2kscaler: no %s\n", #name); return 0; } } while (0)

static int load_gl(void)
{
    GETPROC(glCreateShader); GETPROC(glShaderSource); GETPROC(glCompileShader);
    GETPROC(glGetShaderiv); GETPROC(glGetShaderInfoLog); GETPROC(glCreateProgram);
    GETPROC(glAttachShader); GETPROC(glLinkProgram); GETPROC(glGetProgramiv);
    GETPROC(glGetProgramInfoLog); GETPROC(glUseProgram); GETPROC(glGetUniformLocation);
    GETPROC(glUniform1i); GETPROC(glUniform1f); GETPROC(glUniform2f); GETPROC(glActiveTexture);
    GETPROC(glGenFramebuffers); GETPROC(glBindFramebuffer); GETPROC(glFramebufferTexture2D);
    GETPROC(glCheckFramebufferStatus); GETPROC(glDeleteFramebuffers);
    p_glXSwapIntervalEXT = (void *)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    p_glXBindTexImageEXT = (void *)glXGetProcAddressARB((const GLubyte *)"glXBindTexImageEXT");
    p_glXReleaseTexImageEXT = (void *)glXGetProcAddressARB((const GLubyte *)"glXReleaseTexImageEXT");
    return 1;
}

/* ------------------------------------------------------------------ *
 * The filter: EWA Lanczos-sharp as mpv defines it. jinc(x) is the polar
 * sinc, 2 J1(pi x) / (pi x); the window is a jinc stretched so that its
 * first zero sits at the radius, the third zero of the kernel; the
 * whole thing is blurred by 0.9812505644269356, which is what makes it
 * "sharp" (a little less ringing than raw). Baked into a lookup table
 * on the CPU, sampled in the shader by distance.
 * ------------------------------------------------------------------ */
#define EWA_RADIUS 3.2383154841662362
#define EWA_BLUR   0.9812505644269356
#define JINC_ZERO1 1.2196698912665045
#define LUT_N      1024

static double jinc(double x)
{
    if (x < 1e-9) return 1.0;
    double px = M_PI * x;
    return 2.0 * j1(px) / px;
}

static void make_lut(float *lut, double blur)
{
    for (int i = 0; i < LUT_N; i++) {
        double d = EWA_RADIUS * i / (LUT_N - 1);   /* distance in source pixels */
        double x = d / blur;
        double k = jinc(x) * jinc(x * JINC_ZERO1 / EWA_RADIUS);
        lut[i] = (float)k;
    }
    lut[LUT_N - 1] = 0.0f;
}

/* sRGB in, sigmoidised linear out (256 entries), and the way back (1024).
 * mpv's sigmoid: centre 0.75, slope 6.5. */
#define SIG_CENTER 0.75
#define SIG_SLOPE  6.5
static double srgb_to_linear(double c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double linear_to_srgb(double c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
static void make_sigmoid_luts(float *fwd, float *inv)
{
    double off = 1.0 / (1.0 + exp(SIG_SLOPE * SIG_CENTER));
    double scl = 1.0 / (1.0 + exp(SIG_SLOPE * (SIG_CENTER - 1.0))) - off;
    for (int i = 0; i < 256; i++) {
        double l = srgb_to_linear(i / 255.0);
        fwd[i] = (float)((1.0 / (1.0 + exp(SIG_SLOPE * (SIG_CENTER - l))) - off) / scl);
    }
    for (int i = 0; i < 1024; i++) {
        double y = i / 1023.0;
        double v = y * scl + off;
        if (v <= 1e-6) v = 1e-6;
        if (v >= 1.0 - 1e-6) v = 1.0 - 1e-6;
        double l = SIG_CENTER - log(1.0 / v - 1.0) / SIG_SLOPE;
        if (l < 0) l = 0;
        if (l > 1) l = 1;
        inv[i] = (float)linear_to_srgb(l);
    }
}

static const char *vert_src =
    "#version 120\n"
    "varying vec2 p;\n"                    /* window pixel, y down */
    "void main() { p = gl_MultiTexCoord0.xy; gl_Position = gl_Vertex; }\n";

/* Scaled pass: writes one output pixel from the nested texture. */
/* Scaled pass: writes one output pixel from the nested texture, by the
 * method chosen -- switchable while running (see the _L2K_SCALER
 * property). Nearest and bilinear are what xrandr offers; Catmull-Rom
 * and Lanczos-3 are the separable classics; EWA Lanczos is the polar
 * jinc, and EWA Lanczos-sharp the same blurred a hair, mpv's choice. */
static const char *ewa_src =
    "#version 120\n"
    "uniform sampler2D tex;\n"           /* the nested screen, sRGB bytes */
    "uniform sampler1D lut;\n"           /* EWA Lanczos kernel by distance / radius */
    "uniform sampler1D lut2;\n"          /* the same, sharp */
    "uniform sampler1D sig;\n"           /* sRGB byte -> sigmoid linear */
    "uniform sampler1D unsig;\n"         /* sigmoid linear -> sRGB */
    "uniform vec2 texsize;\n"
    "uniform vec2 origin;\n"             /* nested pixel at the window's corner */
    "uniform float scale;\n"             /* host pixels per nested pixel */
    "uniform float radius;\n"
    "uniform float linlight;\n"          /* 1: filter in sigmoidised linear light */
    "uniform float texflip;\n"           /* 1: the texture's row 0 is the bottom */
    "uniform float antiring;\n"          /* 1: clamp toward the four neighbours */
    "uniform int method;\n"              /* 0 nearest 1 bilinear 2 cubic 3 lanczos 4 ewa 5 ewa sharp */
    "varying vec2 p;\n"
    "vec3 fetch(vec2 j) {\n"
    "    vec2 t = (j + 0.5) / texsize;\n"
    "    if (texflip > 0.5) t.y = 1.0 - t.y;\n"
    "    vec3 c = texture2D(tex, t).rgb;\n"
    "    if (linlight < 0.5) return c;\n"
    "    return vec3(texture1D(sig, c.r).r, texture1D(sig, c.g).r, texture1D(sig, c.b).r);\n"
    "}\n"
    "float cubic_w(float x) {\n"         /* Catmull-Rom, a = -1/2 */
    "    x = abs(x);\n"
    "    if (x < 1.0) return 1.5 * x * x * x - 2.5 * x * x + 1.0;\n"
    "    if (x < 2.0) return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;\n"
    "    return 0.0;\n"
    "}\n"
    "float sinc(float x) { if (abs(x) < 1e-5) return 1.0; float px = 3.14159265 * x; return sin(px) / px; }\n"
    "float lanczos_w(float x) { x = abs(x); if (x >= 3.0) return 0.0; return sinc(x) * sinc(x / 3.0); }\n"
    "void main() {\n"
    "    vec2 src = origin + p / scale;\n"          /* continuous nested coords */
    "    vec2 c = src - 0.5;\n"                      /* pixel-index space */
    "    vec2 base = floor(c);\n"
    "    vec2 f = c - base;\n"
    "    vec3 res;\n"
    "    if (method == 0) {\n"
    "        res = fetch(floor(src));\n"
    "    } else if (method == 1) {\n"
    "        vec3 a = fetch(base), b = fetch(base + vec2(1.0, 0.0));\n"
    "        vec3 cc = fetch(base + vec2(0.0, 1.0)), d4 = fetch(base + vec2(1.0, 1.0));\n"
    "        res = mix(mix(a, b, f.x), mix(cc, d4, f.x), f.y);\n"
    "    } else if (method == 2 || method == 3) {\n"
    "        int R = method == 2 ? 2 : 3;\n"
    "        vec3 acc = vec3(0.0); float wsum = 0.0;\n"
    "        for (int dy = -2; dy <= 3; dy++)\n"
    "            for (int dx = -2; dx <= 3; dx++) {\n"
    "                if (dx < 1 - R || dx > R || dy < 1 - R || dy > R) continue;\n"
    "                float wx = method == 2 ? cubic_w(float(dx) - f.x) : lanczos_w(float(dx) - f.x);\n"
    "                float wy = method == 2 ? cubic_w(float(dy) - f.y) : lanczos_w(float(dy) - f.y);\n"
    "                float w = wx * wy;\n"
    "                acc += w * fetch(base + vec2(float(dx), float(dy))); wsum += w;\n"
    "            }\n"
    "        res = wsum != 0.0 ? acc / wsum : fetch(base);\n"
    "    } else {\n"
    "        vec3 acc = vec3(0.0); float wsum = 0.0;\n"
    "        for (int dy = -3; dy <= 3; dy++)\n"
    "            for (int dx = -3; dx <= 3; dx++) {\n"
    "                vec2 j = base + vec2(float(dx), float(dy));\n"
    "                float d = distance(j, c);\n"
    "                if (d < radius) {\n"
    "                    float w = method == 5 ? texture1D(lut2, d / radius).r : texture1D(lut, d / radius).r;\n"
    "                    acc += w * fetch(j); wsum += w;\n"
    "                }\n"
    "            }\n"
    "        res = wsum != 0.0 ? acc / wsum : fetch(base);\n"
    "    }\n"
    /* Anti-ringing: pull the result toward the range of the four pixels
     * around the sample point, most of the way. Nearest and bilinear
     * cannot ring. */
    "    if (antiring > 0.5 && method >= 2) {\n"
    "        vec3 a = fetch(base), b = fetch(base + vec2(1.0, 0.0)),\n"
    "             cc = fetch(base + vec2(0.0, 1.0)), d4 = fetch(base + vec2(1.0, 1.0));\n"
    "        vec3 lo = min(min(a, b), min(cc, d4)), hi = max(max(a, b), max(cc, d4));\n"
    "        res = mix(res, clamp(res, lo, hi), 0.8);\n"
    "    }\n"
    "    res = clamp(res, 0.0, 1.0);\n"
    "    if (linlight < 0.5) { gl_FragColor = vec4(res, 1.0); return; }\n"
    "    gl_FragColor = vec4(texture1D(unsig, res.r).r, texture1D(unsig, res.g).r,\n"
    "                        texture1D(unsig, res.b).r, 1.0);\n"
    "}\n";

/* Plain pass: a texture rectangle, as is. Used for 1:1 monitors, for
 * presenting the scaled framebuffer, and for the cursor (blended). */
static const char *blit_src =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 texsize;\n"
    "uniform vec2 origin;\n"
    "uniform float scale;\n"
    "uniform float flipy;\n"            /* 1: the texture is stored bottom-up */
    "varying vec2 p;\n"
    "void main() {\n"
    "    vec2 q = origin + p / scale;\n"
    "    if (flipy > 0.5) q.y = texsize.y - q.y;\n"
    "    gl_FragColor = texture2D(tex, q / texsize);\n"
    "}\n";

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */
typedef struct {
    char   name[64];
    int    hw, hh, hx, hy;          /* the window on this display */
    int    nw, nh, nx, ny;          /* the nested rectangle it shows */
    double scale;
    Window win;
    GLuint fbo, fbo_tex;            /* the scaled picture, kept (scale != 1) */
    int    dirty;                   /* needs presenting */
    int    fbo_x0, fbo_y0, fbo_x1, fbo_y1;   /* pending EWA rect, host px */
    int    hq_x0, hq_y0, hq_x1, hq_y1;       /* drawn cheaply in motion: redo when it settles */
} Mon;

/* While a large part of the screen keeps changing -- a window dragged, a
 * page scrolled, a video -- the expensive filters cost more than a frame
 * (EWA Lanczos: some 26 ms for all of 1920x1080 on an integrated GPU), so
 * those frames are drawn bilinear, and the area is drawn again with the
 * chosen filter once nothing has changed for SETTLE_MS. Small changes --
 * typing, a clock, a cursor -- cost little and get the real filter at
 * once. */
#define MOTION_MS    100      /* damage this soon after the last is motion */
#define SETTLE_MS    150      /* quiet this long and the picture is redone */
#define MOTION_SHARE 16       /* a pass over 1/16 of the window or more is large */
static long last_damage, prev_damage;

static Display *hd, *nd;            /* host, nested */
static int hscreen, nscreen;
static Window nroot;
static int NW, NH;                  /* nested screen size */
static Mon mons[8];
static int nmons;
static int windowed, no_input;   /* --window and --no-input: for trying it out */
static int linear_light;         /* --linear-light, or light=linear */
static int antiring = 1;         /* --antiring 0|1, or antiring=0|1 */
static int method = 5;           /* --filter NAME, or filter=NAME; see methods[] */
static const char *const methods[] = { "nearest", "bilinear", "bicubic", "lanczos",
                                       "ewa_lanczos", "ewa_lanczossharp" };
#define NMETHODS 6
static Atom a_scaler;            /* _L2K_SCALER on the nested root: settings, changed live */

static int method_by_name(const char *n)
{
    for (int i = 0; i < NMETHODS; i++) if (!strcasecmp(n, methods[i])) return i;
    return -1;
}

/* "filter=NAME;light=gamma|linear;antiring=0|1", any of them. Returns
 * 1 when something changed. */
static int apply_settings(const char *spec)
{
    int changed = 0;
    char *dup = strdup(spec), *save = NULL;
    for (char *tok = strtok_r(dup, ";\n", &save); tok; tok = strtok_r(NULL, ";\n", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *val = eq + 1;
        if (!strcasecmp(tok, "filter")) {
            int m = method_by_name(val);
            if (m >= 0 && m != method) { method = m; changed = 1; }
        } else if (!strcasecmp(tok, "light")) {
            int l = !strcasecmp(val, "linear");
            if (l != linear_light) { linear_light = l; changed = 1; }
        } else if (!strcasecmp(tok, "antiring")) {
            int ar = atoi(val) != 0;
            if (ar != antiring) { antiring = ar; changed = 1; }
        }
    }
    free(dup);
    return changed;
}

/* Read the settings property off the nested root, if anyone set one. */
static void settings_from_property(void)
{
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    if (XGetWindowProperty(nd, nroot, a_scaler, 0, 256, False, XA_STRING, &type, &fmt, &n, &after, &data) == Success && data) {
        if (n && apply_settings((const char *)data))
            for (int i = 0; i < nmons; i++) {
                mons[i].dirty = 1;
                if (mons[i].fbo) { mons[i].fbo_x0 = 0; mons[i].fbo_y0 = 0; mons[i].fbo_x1 = mons[i].hw; mons[i].fbo_y1 = mons[i].hh; }
            }
        XFree(data);
    }
}
static const char *nested_title; /* --nested-window: Xephyr's window on this display */
static Window xwin;              /* that window */
static Pixmap xpix;              /* its pixmap, named by Composite */
static GLXPixmap xglx;           /* bound to desk_tex */
static int tex_flip;             /* the bound texture's row 0 is the bottom */
static Damage hdamage;           /* damage on that window, on this display */
static int hdamage_base;
static volatile sig_atomic_t quit;

static GLXContext ctx;
static XVisualInfo *vi;
static GLuint desk_tex, lut_tex, lut2_tex, sig_tex, unsig_tex, cur_tex;
static GLuint prog_ewa, prog_blit;
static XShmSegmentInfo shm;
static XImage *shmimg;
static Damage damage;
static int damage_base, xfixes_base;
static int dmg_x0 = 0, dmg_y0 = 0, dmg_x1 = 0, dmg_y1 = 0, dmg_any = 1;
static int cur_w, cur_h, cur_xhot, cur_yhot, cur_valid;
static int ptr_x = -1, ptr_y = -1;

static void on_signal(int s) { (void)s; quit = 1; }

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static int composite;             /* --composite: this display's own windows */
static Window cow;                /* compositing: the overlay window, ours to draw on */
static int access_denied;         /* a BadAccess came back: someone else composites */

static int xerror(Display *d, XErrorEvent *e)
{
    if (e->error_code == BadAccess) access_denied = 1;
    /* Compositing, windows come and go between a request and its answer:
     * a window gone or unviewable is the ordinary case, not news. */
    if (composite && (e->error_code == BadWindow || e->error_code == BadDrawable ||
                      e->error_code == BadMatch || e->error_code == BadPixmap ||
                      e->error_code == BadValue)) return 0;
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof buf);
    fprintf(stderr, "l2kscaler: X error: %s (request %d.%d)\n", buf,
            e->request_code, e->minor_code);
    return 0;
}

/* ------------------------------------------------------------------ *
 * Layout
 * ------------------------------------------------------------------ */
static int parse_layout(const char *s)
{
    nmons = 0;
    char *dup = strdup(s), *save = NULL;
    for (char *tok = strtok_r(dup, ";", &save); tok && nmons < 8; tok = strtok_r(NULL, ";", &save)) {
        Mon *m = &mons[nmons];
        memset(m, 0, sizeof *m);
        char name[64];
        if (sscanf(tok, "%63[^,],%d,%d,%d,%d,%d,%d,%d,%d", name, &m->hw, &m->hh, &m->hx, &m->hy,
                   &m->nw, &m->nh, &m->nx, &m->ny) != 9 || m->hw < 1 || m->hh < 1 || m->nw < 1 || m->nh < 1) {
            fprintf(stderr, "l2kscaler: bad layout entry \"%s\"\n", tok);
            free(dup);
            return 0;
        }
        snprintf(m->name, sizeof m->name, "%s", name);
        m->scale = (double)m->hw / m->nw;
        nmons++;
    }
    free(dup);
    return nmons > 0;
}

/* ------------------------------------------------------------------ *
 * GL setup
 * ------------------------------------------------------------------ */
static GLuint compile(GLenum kind, const char *src)
{
    GLuint sh = p_glCreateShader(kind);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "l2kscaler: shader: %s\n", log);
        return 0;
    }
    return sh;
}

static GLuint link_program(const char *frag)
{
    GLuint v = compile(GL_VERTEX_SHADER, vert_src), f = compile(GL_FRAGMENT_SHADER, frag);
    if (!v || !f) return 0;
    GLuint pr = p_glCreateProgram();
    p_glAttachShader(pr, v);
    p_glAttachShader(pr, f);
    p_glLinkProgram(pr);
    GLint ok = 0;
    p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetProgramInfoLog(pr, sizeof log, NULL, log);
        fprintf(stderr, "l2kscaler: link: %s\n", log);
        return 0;
    }
    return pr;
}

static GLuint tex1d(const float *data, int n)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_1D, t);
    glTexImage1D(GL_TEXTURE_1D, 0, GL_LUMINANCE32F_ARB, n, 0, GL_LUMINANCE, GL_FLOAT, data);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    return t;
}

static GLuint tex2d(int w, int h, GLenum filter)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static int gl_init(void)
{
    int attrs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
    vi = glXChooseVisual(hd, hscreen, attrs);
    if (!vi) { fprintf(stderr, "l2kscaler: no double-buffered RGBA visual\n"); return 0; }
    ctx = glXCreateContext(hd, vi, NULL, True);
    if (!ctx) { fprintf(stderr, "l2kscaler: cannot create a GL context\n"); return 0; }
    return 1;
}

static void mon_window(Mon *m)
{
    XSetWindowAttributes a;
    a.colormap = XCreateColormap(hd, RootWindow(hd, hscreen), vi->visual, AllocNone);
    a.override_redirect = !windowed;
    a.background_pixel = BlackPixel(hd, hscreen);
    a.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | EnterWindowMask | LeaveWindowMask | ExposureMask |
                   StructureNotifyMask | FocusChangeMask;
    /* Compositing, the monitors' windows are the overlay window's, and let
     * every click and key through to the windows under them. */
    if (composite) a.event_mask = ExposureMask | StructureNotifyMask;
    m->win = XCreateWindow(hd, composite ? cow : RootWindow(hd, hscreen), m->hx, m->hy,
                           (unsigned)m->hw, (unsigned)m->hh, 0, vi->depth, InputOutput, vi->visual,
                           CWColormap | CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    if (composite) {
        XserverRegion none = XFixesCreateRegion(hd, NULL, 0);
        XFixesSetWindowShapeRegion(hd, m->win, ShapeInput, 0, 0, none);
        XFixesDestroyRegion(hd, none);
    }
    char title[128];
    snprintf(title, sizeof title, "Linux 2000 (%s)", m->name);
    XStoreName(hd, m->win, title);
    XClassHint ch = { "l2kscaler", "l2kscaler" };
    XSetClassHint(hd, m->win, &ch);
    XMapRaised(hd, m->win);
    XFixesHideCursor(hd, m->win);
}

static void gl_after_context(void)
{
    /* One vertical-blank wait per frame, not one per window: the first
     * scaled window (or the first) syncs, the rest are told not to. */
    if (p_glXSwapIntervalEXT) {
        int synced = -1;
        for (int i = 0; i < nmons && synced < 0; i++)
            if (fabs(mons[i].scale - 1.0) > 1e-6) synced = i;
        if (synced < 0) synced = 0;
        for (int i = 0; i < nmons; i++) {
            glXMakeCurrent(hd, mons[i].win, ctx);
            p_glXSwapIntervalEXT(hd, mons[i].win, i == synced ? 1 : 0);
        }
        glXMakeCurrent(hd, mons[0].win, ctx);
    }
    float lut[LUT_N], sfwd[256], sinv[1024];
    make_lut(lut, 1.0);
    make_sigmoid_luts(sfwd, sinv);
    lut_tex = tex1d(lut, LUT_N);
    make_lut(lut, EWA_BLUR);
    lut2_tex = tex1d(lut, LUT_N);
    sig_tex = tex1d(sfwd, 256);
    unsig_tex = tex1d(sinv, 1024);
    desk_tex = tex2d(NW, NH, GL_NEAREST);
    cur_tex = tex2d(1, 1, GL_LINEAR);
    prog_ewa = link_program(ewa_src);
    prog_blit = link_program(blit_src);
    for (int i = 0; i < nmons; i++) {
        Mon *m = &mons[i];
        if (fabs(m->scale - 1.0) < 1e-6) continue;
        m->fbo_tex = tex2d(m->hw, m->hh, GL_NEAREST);
        p_glGenFramebuffers(1, &m->fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, m->fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m->fbo_tex, 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "l2kscaler: framebuffer for %s incomplete\n", m->name);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m->fbo_x0 = 0; m->fbo_y0 = 0; m->fbo_x1 = m->hw; m->fbo_y1 = m->hh;
    }
}

/* A quad over the window rectangle (x0,y0)-(x1,y1) in pixels, y down,
 * carrying the same coordinates as texcoord for the shaders. */
static void quad(int x0, int y0, int x1, int y1, int vw, int vh)
{
    float fx0 = 2.0f * x0 / vw - 1.0f, fx1 = 2.0f * x1 / vw - 1.0f;
    float fy0 = 1.0f - 2.0f * y0 / vh, fy1 = 1.0f - 2.0f * y1 / vh;
    glBegin(GL_QUADS);
    glTexCoord2f((float)x0, (float)y0); glVertex2f(fx0, fy0);
    glTexCoord2f((float)x1, (float)y0); glVertex2f(fx1, fy0);
    glTexCoord2f((float)x1, (float)y1); glVertex2f(fx1, fy1);
    glTexCoord2f((float)x0, (float)y1); glVertex2f(fx0, fy1);
    glEnd();
}

/* A uniform's location is fixed once the program is linked, but it was
 * looked up by name on every use -- around twenty-five string lookups per
 * monitor per frame at up to 125 frames a second. The names are literals,
 * so the pointer identifies them. */
static GLint uloc(GLuint prog, const char *name)
{
    static struct { GLuint prog; const char *name; GLint loc; } cache[64];
    static int n;
    for (int i = 0; i < n; i++)
        if (cache[i].prog == prog && cache[i].name == name) return cache[i].loc;
    GLint l = p_glGetUniformLocation(prog, name);
    if (n < (int)(sizeof cache / sizeof *cache)) {
        cache[n].prog = prog; cache[n].name = name; cache[n].loc = l; n++;
    }
    return l;
}

static void bind_common(GLuint prog, GLuint tex, int tw, int th, double ox, double oy, double scale, int flipy)
{
    p_glUseProgram(prog);
    if (prog == prog_blit) p_glUniform1f(uloc(prog, "flipy"), flipy ? 1.0f : 0.0f);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    p_glUniform1i(uloc(prog, "tex"), 0);
    p_glUniform2f(uloc(prog, "texsize"), (float)tw, (float)th);
    p_glUniform2f(uloc(prog, "origin"), (float)ox, (float)oy);
    p_glUniform1f(uloc(prog, "scale"), (float)scale);
}

/* ------------------------------------------------------------------ *
 * The nested screen: damage in, texture out
 * ------------------------------------------------------------------ */
/* Xephyr's window on this display, by title. */
static Window find_window(Window root, const char *title, int depth)
{
    Window rr, parent, *kids = NULL;
    unsigned n = 0;
    if (!XQueryTree(hd, root, &rr, &parent, &kids, &n)) return 0;
    Window found = 0;
    for (unsigned i = 0; i < n && !found; i++) {
        char *name = NULL;
        if (XFetchName(hd, kids[i], &name) && name) {
            if (!strcmp(name, title)) found = kids[i];
            XFree(name);
        }
        if (!found && depth < 3) found = find_window(kids[i], title, depth + 1);
    }
    if (kids) XFree(kids);
    return found;
}

/* Take the window's pixmap as our screen texture: no copy, the GPU reads
 * what Xephyr drew. Done again whenever the window changes size. */
static void tfp_bind(void)
{
    if (xglx) { p_glXReleaseTexImageEXT(hd, xglx, GLX_FRONT_LEFT_EXT); glXDestroyPixmap(hd, xglx); xglx = 0; }
    if (xpix) { XFreePixmap(hd, xpix); xpix = 0; }
    xpix = XCompositeNameWindowPixmap(hd, xwin);
    int attrs[] = { GLX_BIND_TO_TEXTURE_RGB_EXT, True, GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
                    GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT, GLX_DOUBLEBUFFER, False,
                    GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
    int n = 0;
    GLXFBConfig *fbc = glXChooseFBConfig(hd, hscreen, attrs, &n);
    if (!fbc || !n) { fprintf(stderr, "l2kscaler: no framebuffer config binds a pixmap to a texture\n"); exit(1); }
    /* The one whose depth is the window's. */
    XWindowAttributes wa;
    XGetWindowAttributes(hd, xwin, &wa);
    NW = wa.width; NH = wa.height;         /* the window is the nested screen */
    GLXFBConfig pick = fbc[0];
    for (int i = 0; i < n; i++) {
        XVisualInfo *v = glXGetVisualFromFBConfig(hd, fbc[i]);
        if (v && v->depth == wa.depth) { pick = fbc[i]; XFree(v); break; }
        if (v) XFree(v);
    }
    XFree(fbc);
    int pattrs[] = { GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
                     GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT, None };
    xglx = glXCreatePixmap(hd, pick, xpix, pattrs);
    unsigned inv = 0;
    glXQueryDrawable(hd, xglx, GLX_Y_INVERTED_EXT, &inv);
    tex_flip = inv != 0;                   /* as Mesa binds them: rows from the bottom */
    glBindTexture(GL_TEXTURE_2D, desk_tex);
    p_glXBindTexImageEXT(hd, xglx, GLX_FRONT_LEFT_EXT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static int tfp_init(void)
{
    if (!p_glXBindTexImageEXT || !p_glXReleaseTexImageEXT) {
        fprintf(stderr, "l2kscaler: no GLX_EXT_texture_from_pixmap\n");
        return 0;
    }
    int ev, err;
    if (!XCompositeQueryExtension(hd, &ev, &err)) { fprintf(stderr, "l2kscaler: this server has no Composite\n"); return 0; }
    if (!XDamageQueryExtension(hd, &hdamage_base, &err)) { fprintf(stderr, "l2kscaler: this server has no DAMAGE\n"); return 0; }
    for (int tries = 0; tries < 100 && !xwin; tries++) {
        xwin = find_window(RootWindow(hd, hscreen), nested_title, 0);
        if (!xwin) usleep(100000);
    }
    if (!xwin) { fprintf(stderr, "l2kscaler: no window called \"%s\" on this display\n", nested_title); return 0; }
    XCompositeRedirectWindow(hd, xwin, CompositeRedirectAutomatic);
    XSelectInput(hd, xwin, StructureNotifyMask);
    hdamage = XDamageCreate(hd, xwin, XDamageReportBoundingBox);
    XSync(hd, False);
    return 1;
}

static int shm_init(void)
{
    if (!XShmQueryExtension(nd)) { fprintf(stderr, "l2kscaler: the nested server has no MIT-SHM\n"); return 0; }
    shmimg = XShmCreateImage(nd, DefaultVisual(nd, nscreen), (unsigned)DefaultDepth(nd, nscreen),
                             ZPixmap, NULL, &shm, (unsigned)NW, (unsigned)NH);
    if (!shmimg || shmimg->bits_per_pixel != 32) {
        fprintf(stderr, "l2kscaler: the nested server is not 32 bits per pixel\n");
        return 0;
    }
    shm.shmid = shmget(IPC_PRIVATE, (size_t)shmimg->bytes_per_line * NH, IPC_CREAT | 0600);
    if (shm.shmid < 0) { perror("shmget"); return 0; }
    shm.shmaddr = shmimg->data = shmat(shm.shmid, NULL, 0);
    shm.readOnly = False;
    if (!XShmAttach(nd, &shm)) { fprintf(stderr, "l2kscaler: XShmAttach failed\n"); return 0; }
    XSync(nd, False);
    shmctl(shm.shmid, IPC_RMID, NULL);      /* freed when both detach */
    return 1;
}

/* ------------------------------------------------------------------ *
 * --composite: this display's own windows, composited
 *
 * The picom way, all on the one X server: every top-level window is
 * redirected into a pixmap of its own (Composite, manually), each pixmap
 * is a texture (texture-from-pixmap: no copy), and the windows are put
 * together, bottom to top, into the logical desktop -- the monitors'
 * logical rectangles, from the screen's top-left -- which is then scaled
 * onto the monitors through the overlay window, as the nested screen is.
 * The programs never leave the server, so they keep the GPU.
 *
 * The desktop lives in logical pixels, so the pointer must too: every
 * pointer's Coordinate Transformation Matrix is scaled by 1/scale, so a
 * mouse moves the logical pointer as far as it would have moved the real
 * one over the scaled picture (a touch screen or a tablet is mapped onto
 * the logical desktop the same way), and pointer barriers keep it there.
 * The matrices as they were are kept on the root (_L2K_SAVED_CTM) until
 * they are put back, so a scaler that dies and is started again does not
 * scale them twice, and --restore-input puts them back from outside. The
 * pointer has one scale: the first monitor's that is scaled at all.
 * ------------------------------------------------------------------ */
typedef struct {
    Window     id;
    int        x, y, w, h, bw, depth;
    int        mapped, input_only;
    Pixmap     pix;
    GLXPixmap  glx;
    GLuint     tex;
    int        flip, damaged;
    Damage     dmg;
    XRectangle *shape;              /* the bounding shape, window-relative */
    int        nshape;
} CWin;

static CWin *cws;
static int ncws, capcws;             /* the bottom of the stack first */
static GLuint desk_fbo;              /* the composed desktop, drawn into desk_tex */
static int shape_base = -1, xi_opcode = -1;
static PointerBarrier barriers[2];
static int nbarriers;
static GLXFBConfig fbc_depth[2];     /* binds a 24-bit, a 32-bit pixmap */
static int have_fbc[2];
static double ptr_scale = 1.0;

static int cw_index(Window id)
{
    for (int i = 0; i < ncws; i++) if (cws[i].id == id) return i;
    return -1;
}

/* Part of the logical desktop to put together again. */
static void damage_rect(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) return;
    if (!dmg_any) { dmg_x0 = x0; dmg_y0 = y0; dmg_x1 = x1; dmg_y1 = y1; dmg_any = 1; return; }
    if (x0 < dmg_x0) dmg_x0 = x0;
    if (y0 < dmg_y0) dmg_y0 = y0;
    if (x1 > dmg_x1) dmg_x1 = x1;
    if (y1 > dmg_y1) dmg_y1 = y1;
}

static void damage_win(const CWin *c)
{
    damage_rect(c->x, c->y, c->x + c->w + 2 * c->bw, c->y + c->h + 2 * c->bw);
}

static void cw_unbind(CWin *c)
{
    if (c->glx) {
        glBindTexture(GL_TEXTURE_2D, c->tex);
        p_glXReleaseTexImageEXT(hd, c->glx, GLX_FRONT_LEFT_EXT);
        glXDestroyPixmap(hd, c->glx);
        c->glx = 0;
    }
    if (c->pix) { XFreePixmap(hd, c->pix); c->pix = 0; }
}

static void cw_shape(CWin *c)
{
    if (c->shape) { XFree(c->shape); c->shape = NULL; }
    c->nshape = 0;
    if (shape_base < 0) return;
    int n = 0, order;
    XRectangle *r = XShapeGetRectangles(hd, c->id, ShapeBounding, &n, &order);
    if (r && n > 0) { c->shape = r; c->nshape = n; }
    else if (r) XFree(r);
}

/* A mapped window's pixmap, as a texture. Named again after every map and
 * every resize, which is when Composite gives the window a new one. */
static void cw_bind(CWin *c)
{
    cw_unbind(c);
    if (!c->mapped || c->input_only) return;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(hd, c->id, &wa) || wa.map_state != IsViewable) return;
    c->depth = wa.depth;
    int k = wa.depth == 32 ? 1 : 0;
    if (!have_fbc[k]) return;
    c->pix = XCompositeNameWindowPixmap(hd, c->id);
    int pattrs[] = { GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
                     GLX_TEXTURE_FORMAT_EXT, k ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT,
                     None };
    c->glx = glXCreatePixmap(hd, fbc_depth[k], c->pix, pattrs);
    if (!c->glx) { XFreePixmap(hd, c->pix); c->pix = 0; return; }
    unsigned inv = 0;
    glXQueryDrawable(hd, c->glx, GLX_Y_INVERTED_EXT, &inv);
    c->flip = inv != 0;
    if (!c->tex) glGenTextures(1, &c->tex);
    glBindTexture(GL_TEXTURE_2D, c->tex);
    p_glXBindTexImageEXT(hd, c->glx, GLX_FRONT_LEFT_EXT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    c->damaged = 0;
}

/* A new top-level window, on top of the stack as X puts it. */
static CWin *cw_add(Window id)
{
    if (id == cow || cw_index(id) >= 0) return NULL;
    for (int i = 0; i < nmons; i++) if (mons[i].win == id) return NULL;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(hd, id, &wa)) return NULL;
    if (ncws == capcws) {
        int cap = capcws ? capcws * 2 : 64;
        CWin *n = realloc(cws, sizeof *cws * (size_t)cap);
        if (!n) return NULL;
        cws = n; capcws = cap;
    }
    CWin *c = &cws[ncws++];
    memset(c, 0, sizeof *c);
    c->id = id;
    c->x = wa.x; c->y = wa.y; c->w = wa.width; c->h = wa.height; c->bw = wa.border_width;
    c->depth = wa.depth;
    c->input_only = wa.class == InputOnly;
    c->mapped = wa.map_state != IsUnmapped;
    if (!c->input_only) {
        c->dmg = XDamageCreate(hd, id, XDamageReportBoundingBox);
        if (shape_base >= 0) XShapeSelectInput(hd, id, ShapeNotifyMask);
        cw_shape(c);
        if (c->mapped) { cw_bind(c); damage_win(c); }
    }
    return c;
}

static void cw_remove(int i, int destroyed)
{
    CWin *c = &cws[i];
    if (c->mapped) damage_win(c);
    cw_unbind(c);
    if (c->tex) glDeleteTextures(1, &c->tex);
    if (c->dmg && !destroyed) XDamageDestroy(hd, c->dmg);   /* a destroyed window takes its own */
    if (c->shape) XFree(c->shape);
    memmove(&cws[i], &cws[i + 1], sizeof *cws * (size_t)(ncws - i - 1));
    ncws--;
}

/* Put window i just above `above` (None: at the bottom). */
static void cw_restack(int i, Window above)
{
    CWin c = cws[i];
    memmove(&cws[i], &cws[i + 1], sizeof *cws * (size_t)(ncws - i - 1));
    ncws--;
    int pos = 0;
    if (above) { int a = cw_index(above); pos = a >= 0 ? a + 1 : ncws; }
    memmove(&cws[pos + 1], &cws[pos], sizeof *cws * (size_t)(ncws - pos));
    cws[pos] = c;
    ncws++;
}

/* Put the logical desktop together again inside (x0,y0)-(x1,y1): black,
 * then every window that reaches into it, bottom to top, each within its
 * shape, a 32-bit one blended as premultiplied alpha. */
static void compose(int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < ncws; i++) {
        CWin *c = &cws[i];
        if (!c->damaged || !c->glx) continue;
        /* The pixmap was drawn into: let go and take it again, as the
         * extension asks of a reader. */
        XDamageSubtract(hd, c->dmg, None, None);
        glBindTexture(GL_TEXTURE_2D, c->tex);
        p_glXReleaseTexImageEXT(hd, c->glx, GLX_FRONT_LEFT_EXT);
        p_glXBindTexImageEXT(hd, c->glx, GLX_FRONT_LEFT_EXT, NULL);
        c->damaged = 0;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, desk_fbo);
    glViewport(0, 0, NW, NH);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, NH - y1, x1 - x0, y1 - y0);               /* rows from the bottom */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int i = 0; i < ncws; i++) {
        CWin *c = &cws[i];
        if (!c->glx) continue;
        int pw = c->w + 2 * c->bw, ph = c->h + 2 * c->bw;
        if (c->x >= x1 || c->y >= y1 || c->x + pw <= x0 || c->y + ph <= y0) continue;
        if (c->depth == 32) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); }
        else glDisable(GL_BLEND);
        bind_common(prog_blit, c->tex, pw, ph, -c->x, -c->y, 1.0, c->flip);
        if (c->nshape)
            for (int k = 0; k < c->nshape; k++) {
                XRectangle *r = &c->shape[k];
                int rx = c->x + c->bw + r->x, ry = c->y + c->bw + r->y;   /* the shape is inside the border */
                quad(rx, ry, rx + r->width, ry + r->height, NW, NH);
            }
        else quad(c->x, c->y, c->x + pw, c->y + ph, NW, NH);
    }
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void composite_event(XEvent *e)
{
    int i;
    if (e->type == damage_base + XDamageNotify) {
        XDamageNotifyEvent *d = (XDamageNotifyEvent *)e;
        if ((i = cw_index(d->drawable)) < 0) return;
        CWin *c = &cws[i];
        c->damaged = 1;
        damage_rect(c->x + c->bw + d->area.x, c->y + c->bw + d->area.y,
                    c->x + c->bw + d->area.x + d->area.width, c->y + c->bw + d->area.y + d->area.height);
        return;
    }
    if (shape_base >= 0 && e->type == shape_base + ShapeNotify) {
        XShapeEvent *s = (XShapeEvent *)e;
        if ((i = cw_index(s->window)) < 0) return;
        damage_win(&cws[i]);
        cw_shape(&cws[i]);
        damage_win(&cws[i]);
        return;
    }
    switch (e->type) {
    case CreateNotify:
        if (e->xcreatewindow.parent == RootWindow(hd, hscreen)) cw_add(e->xcreatewindow.window);
        break;
    case DestroyNotify:
        if ((i = cw_index(e->xdestroywindow.window)) >= 0) cw_remove(i, 1);
        break;
    case ReparentNotify:
        if (e->xreparent.parent == RootWindow(hd, hscreen)) cw_add(e->xreparent.window);
        else if ((i = cw_index(e->xreparent.window)) >= 0) cw_remove(i, 0);
        break;
    case MapNotify:
        if ((i = cw_index(e->xmap.window)) < 0) break;
        cws[i].mapped = 1;
        cw_shape(&cws[i]);
        cw_bind(&cws[i]);
        damage_win(&cws[i]);
        break;
    case UnmapNotify:
        if ((i = cw_index(e->xunmap.window)) < 0) break;
        damage_win(&cws[i]);
        cws[i].mapped = 0;
        cw_unbind(&cws[i]);
        break;
    case ConfigureNotify: {
        XConfigureEvent *c = &e->xconfigure;
        if (c->window == RootWindow(hd, hscreen) || (i = cw_index(c->window)) < 0) break;
        CWin *w = &cws[i];
        damage_win(w);
        int resized = c->width != w->w || c->height != w->h || c->border_width != w->bw;
        w->x = c->x; w->y = c->y; w->w = c->width; w->h = c->height; w->bw = c->border_width;
        cw_restack(i, c->above);
        w = &cws[cw_index(c->window)];
        if (resized && w->mapped) { cw_shape(w); cw_bind(w); }
        damage_win(w);
        break;
    }
    case CirculateNotify:
        if ((i = cw_index(e->xcirculate.window)) < 0) break;
        if (e->xcirculate.place == PlaceOnTop) cw_restack(i, ncws > 1 ? cws[ncws - 1].id : None);
        else cw_restack(i, None);
        damage_win(&cws[cw_index(e->xcirculate.window)]);
        break;
    }
}

/* ---- the pointer, in logical pixels ---- */
static Atom a_ctm, a_float, a_saved;

typedef struct { int id; float m[9]; } SavedCtm;
static SavedCtm saved[32];
static int nsaved;

static void saved_load(Display *d, Window root)
{
    nsaved = 0;
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    if (XGetWindowProperty(d, root, a_saved, 0, 4096, False, XA_STRING, &type, &fmt, &n, &after, &data) != Success || !data)
        return;
    char *save = NULL;
    for (char *t = strtok_r((char *)data, ";", &save); t && nsaved < 32; t = strtok_r(NULL, ";", &save)) {
        SavedCtm *s = &saved[nsaved];
        if (sscanf(t, "%d:%f,%f,%f,%f,%f,%f,%f,%f,%f", &s->id, &s->m[0], &s->m[1], &s->m[2], &s->m[3],
                   &s->m[4], &s->m[5], &s->m[6], &s->m[7], &s->m[8]) == 10) nsaved++;
    }
    XFree(data);
}

static void saved_store(Display *d, Window root)
{
    char buf[4096];
    int o = 0;
    for (int i = 0; i < nsaved && o < (int)sizeof buf - 200; i++)
        o += snprintf(buf + o, sizeof buf - (size_t)o, "%d:%g,%g,%g,%g,%g,%g,%g,%g,%g;", saved[i].id,
                      saved[i].m[0], saved[i].m[1], saved[i].m[2], saved[i].m[3], saved[i].m[4],
                      saved[i].m[5], saved[i].m[6], saved[i].m[7], saved[i].m[8]);
    XChangeProperty(d, root, a_saved, XA_STRING, 8, PropModeReplace, (unsigned char *)buf, o);
}

static int ctm_get(Display *d, int dev, float *m)
{
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    if (XIGetProperty(d, dev, a_ctm, 0, 9, False, a_float, &type, &fmt, &n, &after, &data) != Success || !data)
        return 0;
    int ok = fmt == 32 && n == 9;
    if (ok) memcpy(m, data, sizeof(float) * 9);
    XFree(data);
    return ok;
}

/* Scale one pointer: its matrix as it was, times 1/scale -- or, putting
 * back, as it was. */
static void ctm_device(Display *d, int dev, int restore)
{
    float orig[9];
    int k;
    for (k = 0; k < nsaved; k++) if (saved[k].id == dev) break;
    if (k < nsaved) memcpy(orig, saved[k].m, sizeof orig);
    else {
        if (restore || !ctm_get(d, dev, orig) || nsaved >= 32) return;
        saved[nsaved].id = dev;
        memcpy(saved[nsaved].m, orig, sizeof orig);
        nsaved++;
    }
    float m[9];
    memcpy(m, orig, sizeof m);
    if (!restore) {
        float s = (float)(1.0 / ptr_scale);
        for (int j = 0; j < 6; j++) m[j] *= s;
    }
    XIChangeProperty(d, dev, a_ctm, a_float, 32, PropModeReplace, (unsigned char *)m, 9);
}

static void ctm_all(Display *d, int restore)
{
    int n = 0;
    XIDeviceInfo *di = XIQueryDevice(d, XIAllDevices, &n);
    for (int i = 0; i < n; i++) if (di[i].use == XISlavePointer) ctm_device(d, di[i].deviceid, restore);
    if (di) XIFreeDeviceInfo(di);
}

static void input_init(void)
{
    Window root = RootWindow(hd, hscreen);
    a_ctm = XInternAtom(hd, "Coordinate Transformation Matrix", False);
    a_float = XInternAtom(hd, "FLOAT", False);
    a_saved = XInternAtom(hd, "_L2K_SAVED_CTM", False);
    saved_load(hd, root);                  /* a scaler before us may have died */
    ctm_all(hd, 0);
    saved_store(hd, root);
    /* New mice and touch screens are scaled as they come. */
    XIEventMask em;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = { 0 };
    XISetMask(mask, XI_HierarchyChanged);
    em.deviceid = XIAllDevices; em.mask_len = sizeof mask; em.mask = mask;
    XISelectEvents(hd, root, &em, 1);
    /* Fences at the logical desktop's right and bottom edges. */
    int rw = DisplayWidth(hd, hscreen), rh = DisplayHeight(hd, hscreen);
    if (NW < rw) barriers[nbarriers++] = XFixesCreatePointerBarrier(hd, root, NW, 0, NW, rh, BarrierNegativeX, 0, NULL);
    if (NH < rh) barriers[nbarriers++] = XFixesCreatePointerBarrier(hd, root, 0, NH, rw, NH, BarrierNegativeY, 0, NULL);
    Window rr, cc; int px, py, wx, wy; unsigned mk;
    if (XQueryPointer(hd, root, &rr, &cc, &px, &py, &wx, &wy, &mk) && (px >= NW || py >= NH))
        XWarpPointer(hd, None, root, 0, 0, 0, 0, NW / 2, NH / 2);
}

static void input_event(XEvent *e)
{
    if (e->type != GenericEvent || e->xcookie.extension != xi_opcode) return;
    if (!XGetEventData(hd, &e->xcookie)) return;
    if (e->xcookie.evtype == XI_HierarchyChanged) {
        XIHierarchyEvent *h = e->xcookie.data;
        for (int i = 0; i < h->num_info; i++)
            if ((h->info[i].flags & XISlaveAdded) && h->info[i].use == XISlavePointer)
                ctm_device(hd, h->info[i].deviceid, 0);
        saved_store(hd, RootWindow(hd, hscreen));
    }
    XFreeEventData(hd, &e->xcookie);
}

static void input_restore(Display *d)
{
    Window root = DefaultRootWindow(d);
    if (!a_ctm) {
        a_ctm = XInternAtom(d, "Coordinate Transformation Matrix", False);
        a_float = XInternAtom(d, "FLOAT", False);
        a_saved = XInternAtom(d, "_L2K_SAVED_CTM", False);
        saved_load(d, root);
    }
    ctm_all(d, 1);
    XDeleteProperty(d, root, a_saved);
    XFlush(d);
}

/* The pixmap configs, one per depth a window can have. */
static int composite_fbconfigs(void)
{
    for (int k = 0; k < 2; k++) {
        int attrs[] = { k ? GLX_BIND_TO_TEXTURE_RGBA_EXT : GLX_BIND_TO_TEXTURE_RGB_EXT, True,
                        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
                        GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT, GLX_DOUBLEBUFFER, False,
                        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, k ? GLX_ALPHA_SIZE : None, 8,
                        None };
        int n = 0;
        GLXFBConfig *fbc = glXChooseFBConfig(hd, hscreen, attrs, &n);
        for (int i = 0; fbc && i < n; i++) {
            XVisualInfo *v = glXGetVisualFromFBConfig(hd, fbc[i]);
            int ok = v && v->depth == (k ? 32 : 24);
            if (v) XFree(v);
            if (ok) { fbc_depth[k] = fbc[i]; have_fbc[k] = 1; break; }
        }
        if (fbc) XFree(fbc);
    }
    if (!have_fbc[0]) fprintf(stderr, "l2kscaler: no framebuffer config binds a 24-bit pixmap to a texture\n");
    return have_fbc[0];
}

/* Take over the screen's drawing: every top-level window redirected, the
 * overlay window ours, input passing straight through it. */
static int composite_init(void)
{
    Window root = RootWindow(hd, hscreen);
    int ev, err, maj = 0, min = 2;
    if (!XCompositeQueryExtension(hd, &ev, &err)) { fprintf(stderr, "l2kscaler: this server has no Composite\n"); return 0; }
    XCompositeQueryVersion(hd, &maj, &min);
    if (maj == 0 && min < 3) { fprintf(stderr, "l2kscaler: Composite 0.3 is needed for the overlay window\n"); return 0; }
    if (!XDamageQueryExtension(hd, &damage_base, &err)) { fprintf(stderr, "l2kscaler: this server has no DAMAGE\n"); return 0; }
    int xmaj = 5, xmin = 0;
    XFixesQueryVersion(hd, &xmaj, &xmin);
    if (xmaj < 5) { fprintf(stderr, "l2kscaler: XFixes 5 is needed for pointer barriers\n"); return 0; }
    int xi_ev, xi_err;
    if (!XQueryExtension(hd, "XInputExtension", &xi_opcode, &xi_ev, &xi_err)) { fprintf(stderr, "l2kscaler: this server has no XInput\n"); return 0; }
    int shape_err;
    if (!XShapeQueryExtension(hd, &shape_base, &shape_err)) shape_base = -1;
    if (!p_glXBindTexImageEXT || !p_glXReleaseTexImageEXT) { fprintf(stderr, "l2kscaler: no GLX_EXT_texture_from_pixmap\n"); return 0; }

    XGrabServer(hd);
    access_denied = 0;
    XCompositeRedirectSubwindows(hd, root, CompositeRedirectManual);
    XSync(hd, False);
    if (access_denied) {
        XUngrabServer(hd);
        fprintf(stderr, "l2kscaler: another compositor is running on this display\n");
        return 0;
    }
    XSelectInput(hd, root, SubstructureNotifyMask);
    cow = XCompositeGetOverlayWindow(hd, root);
    XserverRegion none = XFixesCreateRegion(hd, NULL, 0);
    XFixesSetWindowShapeRegion(hd, cow, ShapeInput, 0, 0, none);
    XFixesDestroyRegion(hd, none);
    XUngrabServer(hd);
    XSync(hd, False);
    return 1;
}

/* Every top-level window there already, bottom to top; the server held
 * still meanwhile so nothing is missed between the list and the events. */
static void composite_windows(void)
{
    Window root = RootWindow(hd, hscreen), r, p, *kids = NULL;
    unsigned n = 0;
    XGrabServer(hd);
    if (XQueryTree(hd, root, &r, &p, &kids, &n))
        for (unsigned i = 0; i < n; i++) cw_add(kids[i]);
    if (kids) XFree(kids);
    XUngrabServer(hd);
    XSync(hd, False);
}

/* Fetch the damaged rectangle of the nested root into the texture. The
 * image is told the rectangle's size for the call: the server writes
 * rows of exactly that width. */
static void fetch_damage(void)
{
    int x0 = dmg_x0, y0 = dmg_y0, x1 = dmg_x1, y1 = dmg_y1;
    if (!dmg_any) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > NW) x1 = NW;
    if (y1 > NH) y1 = NH;
    if (x1 <= x0 || y1 <= y0) { dmg_any = 0; return; }
    prev_damage = last_damage;
    last_damage = now_ms();
    if (composite) {
        dmg_any = 0;
        compose(x0, y0, x1, y1);
    } else if (xwin) {
        /* The texture is the pixmap: let go and take it again, which is
         * what the extension asks of a reader after the drawer has drawn. */
        dmg_any = 0;
        glBindTexture(GL_TEXTURE_2D, desk_tex);
        p_glXReleaseTexImageEXT(hd, xglx, GLX_FRONT_LEFT_EXT);
        p_glXBindTexImageEXT(hd, xglx, GLX_FRONT_LEFT_EXT, NULL);
    } else {
        int rw = x1 - x0, rh = y1 - y0;
        shmimg->width = rw; shmimg->height = rh; shmimg->bytes_per_line = rw * 4;
        /* Cleared only once the fetch has worked: dropping the damage on
         * a failure left that region stale on screen until something else
         * dirtied it. */
        if (!XShmGetImage(nd, nroot, shmimg, x0, y0, AllPlanes)) return;
        glBindTexture(GL_TEXTURE_2D, desk_tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rw);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, rw, rh, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, shmimg->data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        dmg_any = 0;
    }
    /* Which windows it touches, and where in their scaled picture. */
    for (int i = 0; i < nmons; i++) {
        Mon *m = &mons[i];
        int ax0 = x0 > m->nx ? x0 : m->nx, ay0 = y0 > m->ny ? y0 : m->ny;
        int ax1 = x1 < m->nx + m->nw ? x1 : m->nx + m->nw, ay1 = y1 < m->ny + m->nh ? y1 : m->ny + m->nh;
        if (ax1 <= ax0 || ay1 <= ay0) continue;
        m->dirty = 1;
        if (!m->fbo) continue;
        /* The filter reaches four source pixels around a sample: widen. */
        int hx0 = (int)floor((ax0 - m->nx - 4) * m->scale), hy0 = (int)floor((ay0 - m->ny - 4) * m->scale);
        int hx1 = (int)ceil((ax1 - m->nx + 4) * m->scale), hy1 = (int)ceil((ay1 - m->ny + 4) * m->scale);
        if (hx0 < 0) hx0 = 0;
        if (hy0 < 0) hy0 = 0;
        if (hx1 > m->hw) hx1 = m->hw;
        if (hy1 > m->hh) hy1 = m->hh;
        if (m->fbo_x1 <= m->fbo_x0) { m->fbo_x0 = hx0; m->fbo_y0 = hy0; m->fbo_x1 = hx1; m->fbo_y1 = hy1; }
        else {
            if (hx0 < m->fbo_x0) m->fbo_x0 = hx0;
            if (hy0 < m->fbo_y0) m->fbo_y0 = hy0;
            if (hx1 > m->fbo_x1) m->fbo_x1 = hx1;
            if (hy1 > m->fbo_y1) m->fbo_y1 = hy1;
        }
    }
}

static void fetch_cursor(void)
{
    XFixesCursorImage *ci = XFixesGetCursorImage(nd);
    if (!ci) { cur_valid = 0; return; }
    cur_w = ci->width; cur_h = ci->height; cur_xhot = ci->xhot; cur_yhot = ci->yhot;
    unsigned *px = malloc((size_t)cur_w * cur_h * 4);
    if (px) {
        for (int i = 0; i < cur_w * cur_h; i++) px[i] = (unsigned)ci->pixels[i];   /* ARGB, premultiplied */
        glBindTexture(GL_TEXTURE_2D, cur_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cur_w, cur_h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        free(px);
        cur_valid = cur_w > 0 && cur_h > 0;
    }
    XFree(ci);
    for (int i = 0; i < nmons; i++) mons[i].dirty = 1;
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */
static void draw_mon(Mon *m)
{
    glXMakeCurrent(hd, m->win, ctx);
    if (m->fbo && m->fbo_x1 > m->fbo_x0 && m->fbo_y1 > m->fbo_y0) {
        /* The scaled pass, only where the nested screen changed: bilinear
         * for a large area in motion, remembered to be redone. */
        int pass = method, ar = antiring;
        long area = (long)(m->fbo_x1 - m->fbo_x0) * (m->fbo_y1 - m->fbo_y0);
        if (method >= 2 && last_damage - prev_damage < MOTION_MS &&
            area * MOTION_SHARE >= (long)m->hw * m->hh) {
            pass = 1; ar = 0;
            if (m->hq_x1 <= m->hq_x0) { m->hq_x0 = m->fbo_x0; m->hq_y0 = m->fbo_y0; m->hq_x1 = m->fbo_x1; m->hq_y1 = m->fbo_y1; }
            else {
                if (m->fbo_x0 < m->hq_x0) m->hq_x0 = m->fbo_x0;
                if (m->fbo_y0 < m->hq_y0) m->hq_y0 = m->fbo_y0;
                if (m->fbo_x1 > m->hq_x1) m->hq_x1 = m->fbo_x1;
                if (m->fbo_y1 > m->hq_y1) m->hq_y1 = m->fbo_y1;
            }
        }
        p_glBindFramebuffer(GL_FRAMEBUFFER, m->fbo);
        glViewport(0, 0, m->hw, m->hh);
        bind_common(prog_ewa, desk_tex, NW, NH, m->nx, m->ny, m->scale, 0);
        p_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_1D, lut_tex);
        p_glUniform1i(uloc(prog_ewa, "lut"), 1);
        p_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_1D, sig_tex);
        p_glUniform1i(uloc(prog_ewa, "sig"), 2);
        p_glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_1D, unsig_tex);
        p_glUniform1i(uloc(prog_ewa, "unsig"), 3);
        p_glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_1D, lut2_tex);
        p_glUniform1i(uloc(prog_ewa, "lut2"), 4);
        p_glUniform1f(uloc(prog_ewa, "radius"), (float)EWA_RADIUS);
        p_glUniform1f(uloc(prog_ewa, "linlight"), linear_light ? 1.0f : 0.0f);
        p_glUniform1f(uloc(prog_ewa, "texflip"), tex_flip ? 1.0f : 0.0f);
        p_glUniform1f(uloc(prog_ewa, "antiring"), ar ? 1.0f : 0.0f);
        p_glUniform1i(uloc(prog_ewa, "method"), pass);
        p_glActiveTexture(GL_TEXTURE0);
        glEnable(GL_SCISSOR_TEST);
        /* The framebuffer texture is y-up; our quad is y-down, so the
         * picture lands flipped in the texture and is flipped back when
         * presented (origin at the bottom). Scissor in texture rows. */
        glScissor(m->fbo_x0, m->hh - m->fbo_y1, m->fbo_x1 - m->fbo_x0, m->fbo_y1 - m->fbo_y0);
        quad(m->fbo_x0, m->fbo_y0, m->fbo_x1, m->fbo_y1, m->hw, m->hh);
        glDisable(GL_SCISSOR_TEST);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m->fbo_x0 = m->fbo_y0 = m->fbo_x1 = m->fbo_y1 = 0;
    }
    glViewport(0, 0, m->hw, m->hh);
    glDisable(GL_BLEND);
    if (m->fbo) {
        /* Present the kept picture. It sits upside down in the texture
         * (see above): sample with the rows reversed. */
        bind_common(prog_blit, m->fbo_tex, m->hw, m->hh, 0, 0, 1.0, 1);
        quad(0, 0, m->hw, m->hh, m->hw, m->hh);
    } else {
        bind_common(prog_blit, desk_tex, NW, NH, m->nx, m->ny, 1.0, tex_flip);
        quad(0, 0, m->hw, m->hh, m->hw, m->hh);
    }
    /* The nested pointer, where the nested server says it is. */
    if (cur_valid && ptr_x >= 0) {
        int cx = ptr_x - cur_xhot, cy = ptr_y - cur_yhot;
        if (cx + cur_w > m->nx && cy + cur_h > m->ny && cx < m->nx + m->nw && cy < m->ny + m->nh) {
            double s = m->scale;
            int x0 = (int)lround((cx - m->nx) * s), y0 = (int)lround((cy - m->ny) * s);
            int x1 = x0 + (int)lround(cur_w * s), y1 = y0 + (int)lround(cur_h * s);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);       /* premultiplied */
            bind_common(prog_blit, cur_tex, cur_w, cur_h, -x0 / s, -y0 / s, s, 0);
            quad(x0, y0, x1, y1, m->hw, m->hh);
            glDisable(GL_BLEND);
        }
    }
    glXSwapBuffers(hd, m->win);
    m->dirty = 0;
}

/* ------------------------------------------------------------------ *
 * Input: host events become XTest events in the nested server
 * ------------------------------------------------------------------ */
static void pointer_to(Mon *m, int x, int y)
{
    if (no_input) return;
    int nx = m->nx + (int)floor(x / m->scale), ny = m->ny + (int)floor(y / m->scale);
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= NW) nx = NW - 1;
    if (ny >= NH) ny = NH - 1;
    XTestFakeMotionEvent(nd, nscreen, nx, ny, CurrentTime);
    XFlush(nd);
}

static Mon *mon_of(Window w)
{
    for (int i = 0; i < nmons; i++) if (mons[i].win == w) return &mons[i];
    return NULL;
}

static void host_event(XEvent *e)
{
    if (composite) { composite_event(e); input_event(e); }
    if (xwin && e->type == hdamage_base + XDamageNotify) {
        XDamageNotifyEvent *d = (XDamageNotifyEvent *)e;
        int x0 = d->area.x, y0 = d->area.y, x1 = x0 + d->area.width, y1 = y0 + d->area.height;
        if (!dmg_any) { dmg_x0 = x0; dmg_y0 = y0; dmg_x1 = x1; dmg_y1 = y1; dmg_any = 1; }
        else {
            if (x0 < dmg_x0) dmg_x0 = x0;
            if (y0 < dmg_y0) dmg_y0 = y0;
            if (x1 > dmg_x1) dmg_x1 = x1;
            if (y1 > dmg_y1) dmg_y1 = y1;
        }
        return;
    }
    if (xwin && e->xany.window == xwin) {
        if (e->type == ConfigureNotify) {
            glXMakeCurrent(hd, mons[0].win, ctx);
            tfp_bind();
            dmg_x0 = 0; dmg_y0 = 0; dmg_x1 = NW; dmg_y1 = NH; dmg_any = 1;
        }
        return;
    }
    Mon *m = mon_of(e->xany.window);
    if (!m) return;
    switch (e->type) {
    case MotionNotify:
        pointer_to(m, e->xmotion.x, e->xmotion.y);
        break;
    case EnterNotify:
        pointer_to(m, e->xcrossing.x, e->xcrossing.y);
        XSetInputFocus(hd, m->win, RevertToPointerRoot, CurrentTime);
        break;
    case ButtonPress:
    case ButtonRelease:
        if (no_input) break;
        pointer_to(m, e->xbutton.x, e->xbutton.y);
        XTestFakeButtonEvent(nd, e->xbutton.button, e->type == ButtonPress, CurrentTime);
        XFlush(nd);
        break;
    case KeyPress:
    case KeyRelease:
        if (no_input) break;
        XTestFakeKeyEvent(nd, e->xkey.keycode, e->type == KeyPress, CurrentTime);
        XFlush(nd);
        break;
    case Expose:
    case MapNotify:
        m->dirty = 1;
        break;
    case ConfigureNotify:
        if (windowed && (e->xconfigure.width != m->hw || e->xconfigure.height != m->hh)) {
            /* Trying it in a window: the window's size is the monitor's. */
            m->hw = e->xconfigure.width; m->hh = e->xconfigure.height;
            m->scale = (double)m->hw / m->nw;
            if (m->fbo) {
                glXMakeCurrent(hd, m->win, ctx);
                glBindTexture(GL_TEXTURE_2D, m->fbo_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m->hw, m->hh, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
                m->fbo_x0 = 0; m->fbo_y0 = 0; m->fbo_x1 = m->hw; m->fbo_y1 = m->hh;
            }
            m->dirty = 1;
        }
        break;
    }
}

static void nested_event(XEvent *e)
{
    if (e->type == PropertyNotify && e->xproperty.atom == a_scaler) {
        settings_from_property();
        return;
    }
    if (e->type == damage_base + XDamageNotify) {
        XDamageNotifyEvent *d = (XDamageNotifyEvent *)e;
        int x0 = d->area.x, y0 = d->area.y, x1 = x0 + d->area.width, y1 = y0 + d->area.height;
        if (!dmg_any) { dmg_x0 = x0; dmg_y0 = y0; dmg_x1 = x1; dmg_y1 = y1; dmg_any = 1; }
        else {
            if (x0 < dmg_x0) dmg_x0 = x0;
            if (y0 < dmg_y0) dmg_y0 = y0;
            if (x1 > dmg_x1) dmg_x1 = x1;
            if (y1 > dmg_y1) dmg_y1 = y1;
        }
    } else if (e->type == xfixes_base + XFixesCursorNotify) {
        fetch_cursor();
    }
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */
static void usage(void)
{
    fprintf(stderr, "usage: l2kscaler --nested DISPLAY --layout \"name,hW,hH,hX,hY,nW,nH,nX,nY;...\" [--nested-window TITLE]\n"
                    "       l2kscaler --composite --layout \"...\"   (this display's own windows, composited)\n"
                    "       l2kscaler --restore-input   (put the pointers' matrices back after a scaler that died)\n"
                    "                 [--filter NAME] [--antiring 0|1] [--linear-light] [--window] [--no-input]\n"
                    "       l2kscaler --set \"filter=NAME;light=gamma|linear;antiring=0|1\"   (while one runs)\n"
                    "       l2kscaler --list-filters\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *nested = NULL, *layout = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nested") && i + 1 < argc) nested = argv[++i];
        else if (!strcmp(argv[i], "--composite")) composite = 1;
        else if (!strcmp(argv[i], "--restore-input")) {
            Display *d = XOpenDisplay(NULL);
            if (!d) { fprintf(stderr, "l2kscaler: cannot open the display\n"); return 1; }
            input_restore(d);
            XCloseDisplay(d);
            return 0;
        }
        else if (!strcmp(argv[i], "--layout") && i + 1 < argc) layout = argv[++i];
        else if (!strcmp(argv[i], "--window")) windowed = 1;
        else if (!strcmp(argv[i], "--no-input")) no_input = 1;
        else if (!strcmp(argv[i], "--linear-light")) linear_light = 1;
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
            method = method_by_name(argv[++i]);
            if (method < 0) { fprintf(stderr, "l2kscaler: no filter called %s\n", argv[i]); usage(); }
        }
        else if (!strcmp(argv[i], "--antiring") && i + 1 < argc) antiring = atoi(argv[++i]) != 0;
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            /* Change the running scaler's settings: the property on this
             * display's root, which is the nested one from inside. */
            Display *d = XOpenDisplay(NULL);
            if (!d) { fprintf(stderr, "l2kscaler: cannot open the display\n"); return 1; }
            Atom a = XInternAtom(d, "_L2K_SCALER", False);
            XChangeProperty(d, DefaultRootWindow(d), a, XA_STRING, 8, PropModeReplace,
                            (unsigned char *)argv[i + 1], (int)strlen(argv[i + 1]));
            XCloseDisplay(d);
            return 0;
        }
        else if (!strcmp(argv[i], "--list-filters")) {
            for (int k = 0; k < NMETHODS; k++) puts(methods[k]);
            return 0;
        }
        else if (!strcmp(argv[i], "--nested-window") && i + 1 < argc) nested_title = argv[++i];
        else usage();
    }
    if ((!nested && !composite) || !layout || !parse_layout(layout)) usage();

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    hd = XOpenDisplay(NULL);
    if (!hd) { fprintf(stderr, "l2kscaler: cannot open the display\n"); return 1; }
    /* Compositing, the "nested" display is this one: a second
     * connection, for the cursor, the pointer and the settings. */
    nd = XOpenDisplay(composite ? NULL : nested);
    if (!nd) { fprintf(stderr, "l2kscaler: cannot open the nested display %s\n", composite ? "(this one)" : nested); return 1; }
    XSetErrorHandler(xerror);
    hscreen = DefaultScreen(hd);
    nscreen = DefaultScreen(nd);
    nroot = RootWindow(nd, nscreen);
    NW = DisplayWidth(nd, nscreen);
    NH = DisplayHeight(nd, nscreen);
    if (composite) {
        /* The logical desktop: the monitors' logical rectangles, from the
         * top-left. The pointer takes the first scaled monitor's scale. */
        NW = NH = 0;
        for (int i = 0; i < nmons; i++) {
            if (mons[i].nx + mons[i].nw > NW) NW = mons[i].nx + mons[i].nw;
            if (mons[i].ny + mons[i].nh > NH) NH = mons[i].ny + mons[i].nh;
            if (ptr_scale == 1.0 && fabs(mons[i].scale - 1.0) > 1e-6) ptr_scale = mons[i].scale;
        }
        if (NW > DisplayWidth(hd, hscreen) || NH > DisplayHeight(hd, hscreen)) {
            fprintf(stderr, "l2kscaler: the logical desktop (%dx%d) is larger than the screen; scales under 100%% are not composited\n", NW, NH);
            return 1;
        }
        no_input = 1;                   /* input goes to the windows themselves */
    }

    int ev, err, xtest_maj, xtest_min, xtest_ev, xtest_err;
    if (!XDamageQueryExtension(nd, &damage_base, &err)) { fprintf(stderr, "l2kscaler: the nested server has no DAMAGE\n"); return 1; }
    if (!XFixesQueryExtension(nd, &xfixes_base, &err)) { fprintf(stderr, "l2kscaler: the nested server has no XFIXES\n"); return 1; }
    if (!XTestQueryExtension(nd, &xtest_ev, &xtest_err, &xtest_maj, &xtest_min)) { fprintf(stderr, "l2kscaler: the nested server has no XTEST\n"); return 1; }
    if (!XFixesQueryExtension(hd, &ev, &err)) { fprintf(stderr, "l2kscaler: this server has no XFIXES\n"); return 1; }
    if (!composite && !nested_title && !shm_init()) return 1;
    if (!load_gl() || !gl_init()) return 1;
    if (composite && !composite_init()) return 1;

    for (int i = 0; i < nmons; i++) mon_window(&mons[i]);
    XSync(hd, False);
    glXMakeCurrent(hd, mons[0].win, ctx);
    gl_after_context();
    if (!prog_ewa || !prog_blit) return 1;
    if (nested_title) {
        if (!tfp_init()) return 1;
        tfp_bind();
        for (int i = 0; i < nmons; i++) XRaiseWindow(hd, mons[i].win);
    }
    if (composite) {
        /* The desktop is put together in desk_tex, through a framebuffer:
         * its rows run from the bottom. */
        p_glGenFramebuffers(1, &desk_fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, desk_fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, desk_tex, 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "l2kscaler: framebuffer for the desktop incomplete\n");
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        tex_flip = 1;
        if (!composite_fbconfigs()) return 1;
        composite_windows();
        input_init();
        XFixesHideCursor(hd, RootWindow(hd, hscreen));
    } else XSetInputFocus(hd, mons[0].win, RevertToPointerRoot, CurrentTime);

    if (!xwin && !composite) damage = XDamageCreate(nd, nroot, XDamageReportBoundingBox);
    XFixesSelectCursorInput(nd, nroot, XFixesDisplayCursorNotifyMask);
    a_scaler = XInternAtom(nd, "_L2K_SCALER", False);
    XSelectInput(nd, nroot, PropertyChangeMask);
    settings_from_property();
    dmg_x0 = 0; dmg_y0 = 0; dmg_x1 = NW; dmg_y1 = NH; dmg_any = 1;
    fetch_cursor();

    int hfd = ConnectionNumber(hd), nfd = ConnectionNumber(nd);
    long last_frame = 0;
    while (!quit) {
        while (XPending(hd)) { XEvent e; XNextEvent(hd, &e); host_event(&e); }
        while (XPending(nd)) { XEvent e; XNextEvent(nd, &e); nested_event(&e); }
        /* The motion is over: draw again with the real filter what was
         * drawn bilinear while it lasted. */
        if (now_ms() - last_damage >= SETTLE_MS)
            for (int i = 0; i < nmons; i++) {
                Mon *m = &mons[i];
                if (m->hq_x1 <= m->hq_x0) continue;
                if (m->fbo_x1 <= m->fbo_x0) { m->fbo_x0 = m->hq_x0; m->fbo_y0 = m->hq_y0; m->fbo_x1 = m->hq_x1; m->fbo_y1 = m->hq_y1; }
                else {
                    if (m->hq_x0 < m->fbo_x0) m->fbo_x0 = m->hq_x0;
                    if (m->hq_y0 < m->fbo_y0) m->fbo_y0 = m->hq_y0;
                    if (m->hq_x1 > m->fbo_x1) m->fbo_x1 = m->hq_x1;
                    if (m->hq_y1 > m->fbo_y1) m->fbo_y1 = m->hq_y1;
                }
                m->hq_x0 = m->hq_y0 = m->hq_x1 = m->hq_y1 = 0;
                m->dirty = 1;
                prev_damage = 0;                 /* this pass is not motion */
            }
        int any_dirty = dmg_any;
        for (int i = 0; i < nmons && !any_dirty; i++) any_dirty |= mons[i].dirty;
        /* The nested pointer moves without an event of its own when a
         * program warps it: ask every frame that draws. */
        long t = now_ms();
        if (any_dirty && t - last_frame >= 8) {
            last_frame = t;
            if (xwin) XDamageSubtract(hd, hdamage, None, None);
            else if (!composite) XDamageSubtract(nd, damage, None, None);
            glXMakeCurrent(hd, mons[0].win, ctx);
            fetch_damage();
            Window rr, cw; int rx, ry, wx, wy; unsigned mask;
            if (XQueryPointer(nd, nroot, &rr, &cw, &rx, &ry, &wx, &wy, &mask) && (rx != ptr_x || ry != ptr_y)) {
                ptr_x = rx; ptr_y = ry;
                for (int i = 0; i < nmons; i++) mons[i].dirty = 1;
            }
            for (int i = 0; i < nmons; i++)
                if (mons[i].dirty) draw_mon(&mons[i]);
            continue;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(hfd, &fds);
        FD_SET(nfd, &fds);
        struct timeval tv = { 0, any_dirty ? 4000 : 16000 };
        select((hfd > nfd ? hfd : nfd) + 1, &fds, NULL, NULL, &tv);
        if (!any_dirty) {
            /* Idle: still follow a pointer warped by a program, at 60 Hz. */
            Window rr, cw; int rx, ry, wx, wy; unsigned mask;
            if (XQueryPointer(nd, nroot, &rr, &cw, &rx, &ry, &wx, &wy, &mask) && (rx != ptr_x || ry != ptr_y)) {
                ptr_x = rx; ptr_y = ry;
                for (int i = 0; i < nmons; i++) mons[i].dirty = 1;
            }
        }
    }
    if (composite) {
        /* Give the screen back: pointers as they were, no fences, the
         * cursor shown, the windows drawn by the server again. */
        input_restore(hd);
        for (int i = 0; i < nbarriers; i++) XFixesDestroyPointerBarrier(hd, barriers[i]);
        XFixesShowCursor(hd, RootWindow(hd, hscreen));
        XCompositeUnredirectSubwindows(hd, RootWindow(hd, hscreen), CompositeRedirectManual);
        XCompositeReleaseOverlayWindow(hd, RootWindow(hd, hscreen));
        XSync(hd, False);
    } else if (xwin) { XDamageDestroy(hd, hdamage); XCompositeUnredirectWindow(hd, xwin, CompositeRedirectAutomatic); }
    else { XDamageDestroy(nd, damage); XShmDetach(nd, &shm); shmdt(shm.shmaddr); }
    XCloseDisplay(nd);
    XCloseDisplay(hd);
    return 0;
}
