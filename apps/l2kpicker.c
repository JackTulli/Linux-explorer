/* l2kpicker -- tiny X11 desktop colour picker. */
#include "w2kui.h"
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    W2kWin *win;
    W2kRect swatch, pick, copy;
    W2kEdit *ed_r, *ed_g, *ed_b, *ed_hex;
    int r, g, b, a;
    float h, s, v;
    int down;
    int picking;
} Picker;

static void rgb_to_hsv(int r,int g,int b,float *h,float *s,float *v)
{
    float rf=r/255.f,gf=g/255.f,bf=b/255.f;
    float mx=fmaxf(rf,fmaxf(gf,bf)), mn=fminf(rf,fminf(gf,bf)), d=mx-mn;
    *v=mx; *s=mx>0.f?d/mx:0.f;
    if(d<1e-6f){*h=0;return;}
    if(mx==rf)*h=60.f*fmodf((gf-bf)/d,6.f);
    else if(mx==gf)*h=60.f*((bf-rf)/d+2.f);
    else *h=60.f*((rf-gf)/d+4.f);
    if(*h<0)*h+=360.f;
}

static unsigned chan(unsigned long p, unsigned long mask)
{
    if(!mask) return 0;
    unsigned shift=0; while(!(mask&(1ul<<shift))) shift++;
    unsigned bits=0; while(mask&(1ul<<(shift+bits))) bits++;
    unsigned v=(p&mask)>>shift, max=(1u<<bits)-1u;
    return (v*255u + max/2u)/max;
}

static int sample_root(Picker *p, int x, int y)
{
    if(x<0||y<0||x>=w2k.sw||y>=w2k.sh) return 0;
    XImage *im=XGetImage(w2k.dpy,w2k.root,x,y,1,1,AllPlanes,ZPixmap);
    if(!im) return 0;
    unsigned long px=XGetPixel(im,0,0);
    p->r=chan(px,w2k.visual->red_mask);
    p->g=chan(px,w2k.visual->green_mask);
    p->b=chan(px,w2k.visual->blue_mask);
    p->a=255;
    XDestroyImage(im);
    rgb_to_hsv(p->r,p->g,p->b,&p->h,&p->s,&p->v);
    return 1;
}

static void sync_edits(Picker *p)
{
    char buf[32];
    if (p->ed_r) { snprintf(buf, sizeof buf, "%d", p->r); w2k_edit_set(p->ed_r, buf); }
    if (p->ed_g) { snprintf(buf, sizeof buf, "%d", p->g); w2k_edit_set(p->ed_g, buf); }
    if (p->ed_b) { snprintf(buf, sizeof buf, "%d", p->b); w2k_edit_set(p->ed_b, buf); }
    if (p->ed_hex) {
        snprintf(buf, sizeof buf, "#%02X%02X%02X", p->r, p->g, p->b);
        w2k_edit_set(p->ed_hex, buf);
    }
}

static void picker_paint(W2kWin *w, Drawable d)
{
    Picker *p=w->user;
    char buf[128];
    w2k_fill(d,0,0,w->w,w->h,C_FACE);
    w2k_text(d,F_UI_BOLD,12,14,"Desktop Color Picker",C_TEXT);
    w2k_fill_rgb(d,p->swatch.x,p->swatch.y,p->swatch.w,p->swatch.h,p->r,p->g,p->b);
    w2k_edge(d,p->swatch.x,p->swatch.y,p->swatch.w,p->swatch.h,EDGE_SUNKEN_THIN,BF_RECT);

    w2k_text(d,F_UI,86,40,"R",C_TEXT);
    w2k_text(d,F_UI,86,64,"G",C_TEXT);
    w2k_text(d,F_UI,86,88,"B",C_TEXT);
    w2k_text(d,F_UI,86,112,"HEX",C_TEXT);
    if (p->ed_r) w2k_edit_draw(d, p->ed_r);
    if (p->ed_g) w2k_edit_draw(d, p->ed_g);
    if (p->ed_b) w2k_edit_draw(d, p->ed_b);
    if (p->ed_hex) w2k_edit_draw(d, p->ed_hex);

    snprintf(buf,sizeof buf,"HSV  %.0f  %.0f%%  %.0f%%",p->h,p->s*100.f,p->v*100.f);
    w2k_text(d,F_UI,86,140,buf,C_TEXT);

    w2k_draw_pushbutton(d,&p->pick, p->picking ? "Click on screen..." : "Pick from screen",
                        p->down==1?BS_PRESSED:0);
    w2k_draw_pushbutton(d,&p->copy,"Copy HEX",p->down==2?BS_PRESSED:0);
    w2k_text(d,F_UI,12,200,"Press Pick (or Space), then click anywhere on the desktop.",C_TEXT);
}

static void pick_screen(Picker *p)
{
    Cursor cross = XCreateFontCursor(w2k.dpy, XC_crosshair);
    /* Keep the window mapped so the user sees the result; grab the root. */
    if (XGrabPointer(w2k.dpy, w2k.root, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, cross, CurrentTime) != GrabSuccess) {
        XFreeCursor(w2k.dpy, cross);
        return;
    }
    p->picking = 1;
    w2k_win_dirty(p->win);
    XFlush(w2k.dpy);

    /* Discard the release of the button that started the pick, then wait
     * for a full press+release on the screen. */
    for (;;) {
        XEvent e;
        XNextEvent(w2k.dpy, &e);
        if (e.type == ButtonPress && e.xbutton.button == Button1) {
            int x = e.xbutton.x_root, y = e.xbutton.y_root;
            sample_root(p, x, y);
            sync_edits(p);
            /* Wait for the matching release so the click does not fall through. */
            while (XNextEvent(w2k.dpy, &e) == 0) {
                if (e.type == ButtonRelease && e.xbutton.button == Button1) break;
                if (e.type == KeyPress && XLookupKeysym(&e.xkey, 0) == XK_Escape) break;
            }
            break;
        }
        if (e.type == KeyPress && XLookupKeysym(&e.xkey, 0) == XK_Escape)
            break;
        if (e.type == MotionNotify) {
            sample_root(p, e.xmotion.x_root, e.xmotion.y_root);
            sync_edits(p);
            w2k_win_dirty(p->win);
            XFlush(w2k.dpy);
        }
    }
    XUngrabPointer(w2k.dpy, CurrentTime);
    XFreeCursor(w2k.dpy, cross);
    p->picking = 0;
    w2k_win_dirty(p->win);
}

static void unfocus_edits(Picker *p)
{
    if (p->ed_r) p->ed_r->focused = 0;
    if (p->ed_g) p->ed_g->focused = 0;
    if (p->ed_b) p->ed_b->focused = 0;
    if (p->ed_hex) p->ed_hex->focused = 0;
}

static int picker_event(W2kWin *w,XEvent *e)
{
    Picker *p=w->user;
    if(e->type==ButtonPress){
        int x=e->xbutton.x,y=e->xbutton.y;
        if (p->ed_r && w2k_edit_press(p->ed_r, &e->xbutton)) {
            unfocus_edits(p); p->ed_r->focused = 1; w2k_win_dirty(w); return 1;
        }
        if (p->ed_g && w2k_edit_press(p->ed_g, &e->xbutton)) {
            unfocus_edits(p); p->ed_g->focused = 1; w2k_win_dirty(w); return 1;
        }
        if (p->ed_b && w2k_edit_press(p->ed_b, &e->xbutton)) {
            unfocus_edits(p); p->ed_b->focused = 1; w2k_win_dirty(w); return 1;
        }
        if (p->ed_hex && w2k_edit_press(p->ed_hex, &e->xbutton)) {
            unfocus_edits(p); p->ed_hex->focused = 1; w2k_win_dirty(w); return 1;
        }
        unfocus_edits(p);
        if (w2k_rect_hit(&p->swatch, x, y)) {
            if (w2k_color_picker_rgba(w, &p->r, &p->g, &p->b, &p->a)) {
                rgb_to_hsv(p->r, p->g, p->b, &p->h, &p->s, &p->v);
                sync_edits(p);
            }
            w2k_win_dirty(w);
            return 1;
        }
        if(w2k_rect_hit(&p->pick,x,y)){p->down=1;w2k_win_dirty(w);pick_screen(p);p->down=0;return 1;}
        if(w2k_rect_hit(&p->copy,x,y)){
            p->down=2;
            char s[32];snprintf(s,sizeof s,"#%02X%02X%02X",p->r,p->g,p->b);
            w2k_clipboard_set(s);w2k_win_dirty(w);return 1;
        }
        return 1;
    }
    if(e->type==ButtonRelease){
        if (p->ed_r) w2k_edit_release(p->ed_r);
        if (p->ed_g) w2k_edit_release(p->ed_g);
        if (p->ed_b) w2k_edit_release(p->ed_b);
        if (p->ed_hex) w2k_edit_release(p->ed_hex);
        p->down=0;w2k_win_dirty(w);return 1;
    }
    if (e->type == MotionNotify) {
        if (p->ed_r && w2k_edit_motion(p->ed_r, &e->xmotion)) return 1;
        if (p->ed_g && w2k_edit_motion(p->ed_g, &e->xmotion)) return 1;
        if (p->ed_b && w2k_edit_motion(p->ed_b, &e->xmotion)) return 1;
        if (p->ed_hex && w2k_edit_motion(p->ed_hex, &e->xmotion)) return 1;
        return 0;
    }
    if(e->type==KeyPress){
        W2kEdit *focused = NULL;
        if (p->ed_r && p->ed_r->focused) focused = p->ed_r;
        else if (p->ed_g && p->ed_g->focused) focused = p->ed_g;
        else if (p->ed_b && p->ed_b->focused) focused = p->ed_b;
        else if (p->ed_hex && p->ed_hex->focused) focused = p->ed_hex;
        if (focused) {
            KeySym ks = XLookupKeysym(&e->xkey, 0);
            if (ks == XK_Return || ks == XK_KP_Enter || ks == XK_Tab) {
                if (focused == p->ed_hex) {
                    const char *s = w2k_edit_text(p->ed_hex);
                    while (*s == ' ' || *s == '#') s++;
                    unsigned v = 0;
                    if (sscanf(s, "%x", &v) == 1) {
                        p->r = (v >> 16) & 255; p->g = (v >> 8) & 255; p->b = v & 255;
                        rgb_to_hsv(p->r, p->g, p->b, &p->h, &p->s, &p->v);
                        sync_edits(p);
                    }
                } else {
                    p->r = atoi(w2k_edit_text(p->ed_r));
                    p->g = atoi(w2k_edit_text(p->ed_g));
                    p->b = atoi(w2k_edit_text(p->ed_b));
                    if (p->r < 0) p->r = 0; if (p->r > 255) p->r = 255;
                    if (p->g < 0) p->g = 0; if (p->g > 255) p->g = 255;
                    if (p->b < 0) p->b = 0; if (p->b > 255) p->b = 255;
                    rgb_to_hsv(p->r, p->g, p->b, &p->h, &p->s, &p->v);
                    sync_edits(p);
                }
                if (ks == XK_Tab) {
                    unfocus_edits(p);
                    if (focused == p->ed_r) p->ed_g->focused = 1;
                    else if (focused == p->ed_g) p->ed_b->focused = 1;
                    else if (focused == p->ed_b) p->ed_hex->focused = 1;
                    else p->ed_r->focused = 1;
                }
                w2k_win_dirty(w);
                return 1;
            }
            if (w2k_edit_key(focused, &e->xkey)) {
                if (focused != p->ed_hex) {
                    p->r = atoi(w2k_edit_text(p->ed_r));
                    p->g = atoi(w2k_edit_text(p->ed_g));
                    p->b = atoi(w2k_edit_text(p->ed_b));
                    if (p->r < 0) p->r = 0; if (p->r > 255) p->r = 255;
                    if (p->g < 0) p->g = 0; if (p->g > 255) p->g = 255;
                    if (p->b < 0) p->b = 0; if (p->b > 255) p->b = 255;
                    rgb_to_hsv(p->r, p->g, p->b, &p->h, &p->s, &p->v);
                    char buf[32];
                    snprintf(buf, sizeof buf, "#%02X%02X%02X", p->r, p->g, p->b);
                    if (p->ed_hex) w2k_edit_set(p->ed_hex, buf);
                }
                w2k_win_dirty(w);
                return 1;
            }
        }
        KeySym ks=XLookupKeysym(&e->xkey,0);
        if(ks==XK_Escape||ks==XK_q||ks==XK_Q){w2k_win_close(w,0);return 1;}
        if(ks==XK_space){pick_screen(p);return 1;}
        if(ks==XK_c||ks==XK_C){char s[32];snprintf(s,sizeof s,"#%02X%02X%02X",p->r,p->g,p->b);w2k_clipboard_set(s);return 1;}
    }
    return 0;
}

int main(void)
{
    if(w2k_init("l2kpicker")<0) return 1;
    Picker p; memset(&p,0,sizeof p); p.a=255;
    int rx=0,ry=0,wx=0,wy=0; Window rr,cc; unsigned mask;
    if(XQueryPointer(w2k.dpy,w2k.root,&rr,&cc,&rx,&ry,&wx,&wy,&mask)) sample_root(&p,rx,ry);
    p.win=w2k_win_new("Color Picker","l2kpicker",340,230,0);
    p.win->user=&p;p.win->paint=picker_paint;p.win->event=picker_event;
    p.swatch=(W2kRect){12,28,58,100};
    p.pick=(W2kRect){12,170,145,24};
    p.copy=(W2kRect){165,170,100,24};

    p.ed_r = w2k_edit_new(0); p.ed_g = w2k_edit_new(0);
    p.ed_b = w2k_edit_new(0); p.ed_hex = w2k_edit_new(0);
    if (p.ed_r) { p.ed_r->r = (W2kRect){110, 36, 70, 21}; p.ed_r->owner = p.win; w2k_edit_bind(p.ed_r, p.win); }
    if (p.ed_g) { p.ed_g->r = (W2kRect){110, 60, 70, 21}; p.ed_g->owner = p.win; w2k_edit_bind(p.ed_g, p.win); }
    if (p.ed_b) { p.ed_b->r = (W2kRect){110, 84, 70, 21}; p.ed_b->owner = p.win; w2k_edit_bind(p.ed_b, p.win); }
    if (p.ed_hex) { p.ed_hex->r = (W2kRect){110, 108, 90, 21}; p.ed_hex->owner = p.win; w2k_edit_bind(p.ed_hex, p.win); }
    sync_edits(&p);

    w2k_win_show(p.win);w2k_run();
    if (p.ed_r) w2k_edit_free(p.ed_r);
    if (p.ed_g) w2k_edit_free(p.ed_g);
    if (p.ed_b) w2k_edit_free(p.ed_b);
    if (p.ed_hex) w2k_edit_free(p.ed_hex);
    w2k_fini();return 0;
}
