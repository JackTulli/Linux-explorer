#define _POSIX_C_SOURCE 200809L
#include "w2kui.h"
#include "device.h"
#include <X11/keysym.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ID_EXIT=1, ID_PROPERTIES, ID_SCAN, ID_ENABLE, ID_DISABLE,
       ID_UNINSTALL, ID_UPDATE, ID_DETAILS, ID_RESOURCES, ID_ABOUT,
       ID_HIDDEN, ID_BYTYPE, ID_BYCONNECTION };

typedef struct {
    W2kWin *win;
    W2kMenubar *mb;
    W2kTree *tree;
    W2kStatus *status;
    W2kDeviceSet devices;
    W2kDevice machine;
    int busy;
    int show_hidden;
} DM;
static DM dm;

static int icon_for(const char *name) {
    if (!name) return ICO_MYCOMPUTER;
    if (!strcmp(name,"drive-harddisk")) return ICO_DRIVE_HDD;
    if (!strcmp(name,"drive-removable-media-usb")) return ICO_DRIVE_FLOPPY;
    if (!strcmp(name,"network-card") || !strcmp(name,"network-bluetooth")) return ICO_NETWORK;
    if (!strcmp(name,"video-display")) return ICO_FILE_BITMAP;
    if (!strcmp(name,"input-keyboard")) return ICO_FILE_TEXT;
    if (!strcmp(name,"input-mouse")) return ICO_CURSORFILE;
    if (!strcmp(name,"cpu")) return ICO_MYCOMPUTER;
    if (!strcmp(name,"kmix")) return ICO_SPEAKER;
    if (!strcmp(name,"dialog-question")) return ICO_QUESTION;
    if (!strcmp(name,"dialog-cancel")) return ICO_ERROR;
    if (!strcmp(name,"kded5")) return ICO_MYCOMPUTER;
    return ICO_FOLDER;
}
static int cat_icon(const char *name) { return icon_for(name); }

static void set_status(const char *s) { if (dm.status) { w2k_status_set(dm.status,0,s); w2k_win_dirty(dm.win); } }
static W2kDevice *node_dev(W2kTreeNode *n) { return n && n->data ? (W2kDevice*)n->data : NULL; }

static void tree_build(void) {
    w2k_tree_clear_children(&*dm.tree, NULL);
    W2kTreeNode *host = w2k_tree_add(dm.tree,NULL,dm.devices.host,ICO_MYCOMPUTER,ICO_MYCOMPUTER,NULL);
    host->expanded=1; host->has_kids=1;
    W2kTreeNode *cc=w2k_tree_add(dm.tree,host,"Computer",ICO_MYCOMPUTER,ICO_MYCOMPUTER,NULL);
    cc->expanded=1; cc->has_kids=1;
    w2k_tree_add(dm.tree,cc,dm.machine.name,ICO_MYCOMPUTER,ICO_MYCOMPUTER,&dm.machine);
    for(size_t i=0;i<dm.devices.count;i++) {
        W2kDeviceCategory *c=&dm.devices.cats[i];
        if(!strcmp(c->name,"__disabled__") && !dm.show_hidden) continue;
        W2kTreeNode *cn=w2k_tree_add(dm.tree,host,c->name,cat_icon(c->icon),cat_icon(c->icon),c);
        cn->expanded=1; cn->has_kids=c->count>0;
        for(size_t j=0;j<c->count;j++) {
            W2kDevice *d=&c->devices[j];
            if(d->disabled && !dm.show_hidden) continue;
            int ico=icon_for(d->icon[0]?d->icon:c->icon);
            W2kTreeNode *dn=w2k_tree_add(dm.tree,cn,d->name,ico,ico,d);
            dn->has_kids=0;
        }
    }
    w2k_tree_select(dm.tree,host);
    w2k_tree_layout(dm.tree);
}
static void scan(void) {
    dm.busy=1; set_status("Scanning for hardware...");
    w2k_devices_scan(&dm.devices);
    memset(&dm.machine,0,sizeof dm.machine);
    char product[W2K_DEV_STR]={0}, vendor[W2K_DEV_STR]={0}, board[W2K_DEV_STR]={0};
    FILE *mf=fopen("/sys/class/dmi/id/product_name","r"); if(mf){fgets(product,sizeof product,mf);fclose(mf);product[strcspn(product,"\r\n")]=0;}
    mf=fopen("/sys/class/dmi/id/sys_vendor","r"); if(mf){fgets(vendor,sizeof vendor,mf);fclose(mf);vendor[strcspn(vendor,"\r\n")]=0;}
    mf=fopen("/sys/class/dmi/id/board_name","r"); if(mf){fgets(board,sizeof board,mf);fclose(mf);board[strcspn(board,"\r\n")]=0;}
    if(product[0] && vendor[0]) snprintf(dm.machine.name,sizeof dm.machine.name,"%s %s",vendor,product);
    else if(product[0]) snprintf(dm.machine.name,sizeof dm.machine.name,"%s",product);
    else if(board[0]) snprintf(dm.machine.name,sizeof dm.machine.name,"%s",board);
    else snprintf(dm.machine.name,sizeof dm.machine.name,"%s",dm.devices.host);
    snprintf(dm.machine.manufacturer,sizeof dm.machine.manufacturer,"%s",vendor[0]?vendor:"Unknown");
    snprintf(dm.machine.description,sizeof dm.machine.description,"System: %s%s%s",product[0]?product:"Unknown system",board[0]?" — Motherboard: ":"",board[0]?board:"");
    snprintf(dm.machine.status,sizeof dm.machine.status,"This device is working properly.");
    snprintf(dm.machine.driver,sizeof dm.machine.driver,"acpi");
    snprintf(dm.machine.location,sizeof dm.machine.location,"ACPI x64-based PC");
    snprintf(dm.machine.subsystem,sizeof dm.machine.subsystem,"Computer / DMI");
    tree_build();
    char s[160]; size_t total=0; for(size_t i=0;i<dm.devices.count;i++) total+=dm.devices.cats[i].count; snprintf(s,sizeof s,"%zu categories, %zu devices — sysfs / procfs",dm.devices.count,total); set_status(s);
    dm.busy=0; w2k_win_dirty(dm.win);
}

static void props_paint(W2kWin *w, Drawable d);
static int props_event(W2kWin *w, XEvent *e);
static void props_layout(W2kWin *w);
static void show_properties(W2kDevice *dev);
static void update_driver_wizard(W2kDevice *dev);
static void command(void *u,int id);

static void on_tree_select(void *u,W2kTreeNode*n){(void)u;if(node_dev(n)){char s[W2K_DEV_STR];W2kDevice*d=node_dev(n);snprintf(s,sizeof s,"%.*s — %.*s — Driver: %.*s",110,n->text,90,d->description[0]?d->description:"Linux device",60,d->driver[0]?d->driver:"Unknown");set_status(s);}else set_status("Device Manager — Linux hardware inventory");}

static W2kMenu *file_menu(void *u){(void)u;W2kMenu*m=w2k_menu_new();w2k_menu_item(m,ID_SCAN,"Scan for hardware changes","F5",ICO_NONE);w2k_menu_sep(m);w2k_menu_item(m,ID_EXIT,"E&xit","Alt+F4",ICO_NONE);return m;}
static W2kMenu *action_menu(void *u){(void)u;W2kMenu*m=w2k_menu_new();w2k_menu_item(m,ID_PROPERTIES,"&Properties","Enter",ICO_PROPERTIES);w2k_menu_item(m,ID_UPDATE,"&Update Driver...",NULL,ICO_WINUPDATE);w2k_menu_item(m,ID_ENABLE,"&Enable Device",NULL,ICO_NONE);w2k_menu_item(m,ID_DISABLE,"&Disable Device",NULL,ICO_NONE);w2k_menu_item(m,ID_UNINSTALL,"&Uninstall Device",NULL,ICO_DELETE);w2k_menu_item(m,ID_DETAILS,"Driver &Details...",NULL,ICO_FILE_SYS);return m;}
static W2kMenu *view_menu(void *u){(void)u;W2kMenu*m=w2k_menu_new();w2k_menu_item(m,ID_SCAN,"&Refresh","F5",ICO_NONE);w2k_menu_item(m,ID_HIDDEN,"&Show hidden devices",NULL,ICO_NONE);w2k_menu_check(m,dm.show_hidden);W2kMenu*sub=w2k_menu_new();w2k_menu_item(sub,ID_BYTYPE,"Devices by &type",NULL,ICO_NONE);w2k_menu_radio(sub,1);w2k_menu_item(sub,ID_BYCONNECTION,"Devices by &connection",NULL,ICO_NONE);w2k_menu_radio(sub,0);w2k_menu_sub(m,"Arrange devices",ICO_NONE,sub);return m;}
static W2kMenu *help_menu(void *u){(void)u;W2kMenu*m=w2k_menu_new();w2k_menu_item(m,ID_ABOUT,"&About Device Manager",NULL,ICO_INFO);return m;}
static void build_menus(void){w2k_menubar_clear(dm.mb);w2k_menubar_add(dm.mb,"&File",file_menu);w2k_menubar_add(dm.mb,"&Action",action_menu);w2k_menubar_add(dm.mb,"&View",view_menu);w2k_menubar_add(dm.mb,"&Help",help_menu);}

static W2kDevice *selected(void){return node_dev(dm.tree->sel);}
static void command(void *u,int id){(void)u;W2kDevice*d=selected();char err[1024];err[0]=0;
    switch(id){
    case ID_EXIT:w2k_win_close(dm.win,0);break;
    case ID_SCAN:scan();break;
    case ID_PROPERTIES:if(d)show_properties(d);break;
    case ID_DETAILS:if(d){char out[8192];w2k_device_modinfo(d->driver,out,sizeof out);/* dialog implemented by properties */show_properties(d); }break;
    case ID_ENABLE: if(d && d->disabled){ if(w2k_device_set_enabled(d,1,err,sizeof err)==0) scan(); else w2k_notify("Device Manager",err); } break;
    case ID_DISABLE: if(d && !d->disabled){ if(w2k_device_set_enabled(d,0,err,sizeof err)==0) scan(); else w2k_notify("Device Manager",err); } break;
    case ID_UNINSTALL:if(d&&d->is_dkms){if(w2k_device_uninstall_dkms(d,err,sizeof err)==0)scan();else w2k_notify("Device Manager",err);}break;
    case ID_UPDATE:if(d)update_driver_wizard(d);break;
    case ID_HIDDEN:dm.show_hidden=!dm.show_hidden;tree_build();build_menus();w2k_win_dirty(dm.win);break;
    case ID_BYTYPE:case ID_BYCONNECTION:break;
    case ID_ABOUT:w2k_notify("Device Manager","Windows 2000 Device Manager\nNative X11/Xft Linux hardware manager\nPowered by sysfs/procfs");break;
    }
}

static void layout(W2kWin*w){dm.mb->r=(W2kRect){0,0,w->w,MENUBAR_H};int bot=w->h-STATUS_H;dm.status->r=(W2kRect){0,bot,w->w,STATUS_H};dm.tree->r=(W2kRect){4,MENUBAR_H+2,w->w-8,bot-MENUBAR_H-4};w2k_tree_layout(dm.tree);}
static void paint(W2kWin*w,Drawable d){w2k_menubar_draw(d,dm.mb);w2k_tree_draw(d,dm.tree);w2k_status_draw(d,dm.status);}
static void context_menu(int rx,int ry){W2kDevice*d=selected();if(!d)return;W2kMenu*m=w2k_menu_new();w2k_menu_item(m,ID_PROPERTIES,"&Properties","Enter",ICO_PROPERTIES);w2k_menu_item(m,ID_UPDATE,"&Update Driver...",NULL,ICO_WINUPDATE);w2k_menu_item(m,d->disabled?ID_ENABLE:ID_DISABLE,d->disabled?"&Enable Device":"&Disable Device",NULL,ICO_NONE);if(d->is_dkms)w2k_menu_item(m,ID_UNINSTALL,"&Uninstall Device",NULL,ICO_DELETE);int id=w2k_menu_popup(m,rx,ry,MPOP_LEFT);w2k_menu_free(m);if(id)command(NULL,id);}
static int event(W2kWin*w,XEvent*e){switch(e->type){case ButtonPress:if(e->xbutton.button==Button3 && w2k_rect_hit(&dm.tree->r,e->xbutton.x,e->xbutton.y)){context_menu(e->xbutton.x_root,e->xbutton.y_root);return 1;}if(w2k_menubar_press(dm.mb,&e->xbutton)){w2k_win_dirty(w);return 1;}if(w2k_tree_press(dm.tree,&e->xbutton)){w2k_win_dirty(w);return 1;}break;case ButtonRelease:break;case KeyPress:if(w2k_menubar_key(dm.mb,&e->xkey)){w2k_win_dirty(w);return 1;}if(w2k_tree_key(dm.tree,&e->xkey)){w2k_win_dirty(w);return 1;}if(XLookupKeysym(&e->xkey,0)==XK_F5){scan();return 1;}if(XLookupKeysym(&e->xkey,0)==XK_Return||XLookupKeysym(&e->xkey,0)==XK_KP_Enter){W2kDevice*d=selected();if(d)show_properties(d);return 1;}break;}return 0;}

/* ---------- Native Update Driver wizard ---------- */
typedef struct { W2kWin *win; W2kDevice *dev; W2kRect automatic, browse, cancel; int down; } UpdateDlg;
static void update_paint(W2kWin*w,Drawable d){UpdateDlg*p=w->user;int y=18;w2k_text(d,F_UI_BOLD,20,y,"How do you want to search for driver software?",C_WINDOWTEXT);y+=34;w2k_draw_pushbutton(d,&p->automatic,"Search automatically for updated driver software",p->down==1?BS_PRESSED:0);y+=34;w2k_text(d,F_UI,34,y,"Search using the system's installed software updater.",C_WINDOWTEXT);y+=38;w2k_draw_pushbutton(d,&p->browse,"Browse my computer for driver software",p->down==2?BS_PRESSED:0);y+=34;w2k_text(d,F_UI,34,y,"Install a DKMS driver from a local source directory.",C_WINDOWTEXT);w2k_draw_pushbutton(d,&p->cancel,"Cancel",p->down==3?BS_PRESSED:0);}
static int update_event(W2kWin*w,XEvent*e){UpdateDlg*p=w->user;if(e->type==ButtonPress){if(w2k_rect_hit(&p->automatic,e->xbutton.x,e->xbutton.y))p->down=1;else if(w2k_rect_hit(&p->browse,e->xbutton.x,e->xbutton.y))p->down=2;else if(w2k_rect_hit(&p->cancel,e->xbutton.x,e->xbutton.y))p->down=3;else return 0;w2k_win_dirty(w);return 1;}if(e->type==ButtonRelease&&p->down){int d=p->down;p->down=0;if(d==3){w2k_win_close(w,0);return 1;}if(d==1){char err[1024];if(w2k_device_update_driver(p->dev,err,sizeof err)!=0)w2k_notify("Update Driver",err);w2k_win_close(w,0);return 1;}if(d==2){char path[1024];if(w2k_file_dialog(w,0,path,sizeof path)){char *slash=strrchr(path,'/');if(slash)*slash=0;char err[1024];if(w2k_device_install_dkms(path,err,sizeof err)!=0)w2k_notify("Driver installation failed",err);else w2k_notify("Update Driver","The DKMS driver was installed successfully. Rescan hardware with F5.");}return 1;}}if(e->type==KeyPress&&XLookupKeysym(&e->xkey,0)==XK_Escape){w2k_win_close(w,0);return 1;}return 0;}
static void update_driver_wizard(W2kDevice*dev){if(!dev)return;UpdateDlg*p=calloc(1,sizeof*p);if(!p)return;p->dev=dev;p->win=w2k_win_new("Update Driver Software","l2kdevmgmt-update",520,260,0);p->win->user=p;p->win->paint=update_paint;p->win->event=update_event;p->automatic=(W2kRect){20,48,480,25};p->browse=(W2kRect){20,123,480,25};p->cancel=(W2kRect){420,220,76,23};w2k_win_center(p->win,dm.win);w2k_win_modal(p->win);free(p);}

/* ---------- Native property sheet ---------- */
typedef struct {W2kWin*win;W2kTabs*tabs;W2kDevice*d;W2kRect ok,details,update,enable,uninstall;int down;int tab;} Props;
static void props_tab(void*u,int i){Props*p=u;p->tab=i;p->win->dirty=1;}
static void draw_pair(Drawable d,int y,const char*k,const char*v){w2k_text(d,F_UI,16,y,k,C_WINDOWTEXT);w2k_text(d,F_UI,170,y,v&&*v?v:"Unknown",C_WINDOWTEXT);}
static void props_paint(W2kWin*w,Drawable d){Props*p=w->user;w2k_tabs_draw(d,p->tabs);W2kRect c=w2k_tabs_client(p->tabs);w2k_fill(d,c.x,c.y,c.w,c.h,C_WINDOW);W2kDevice*q=p->d;int y=c.y+12;char b[512];
    if(p->tab==0){draw_pair(d,y,"Device status:",q->status);y+=22;draw_pair(d,y,"Manufacturer:",q->manufacturer);y+=22;draw_pair(d,y,"Location:",q->location);y+=22;draw_pair(d,y,"Driver:",q->driver);y+=22;draw_pair(d,y,"Driver version:",q->driver_version);y+=22;draw_pair(d,y,"Driver author:",q->driver_author);y+=22;draw_pair(d,y,"Subsystem:",q->subsystem);y+=22;draw_pair(d,y,"Vendor ID:",q->vendor_id);y+=22;draw_pair(d,y,"Device ID:",q->device_id);y+=22;draw_pair(d,y,"Modalias:",q->modalias);}
    else if(p->tab==1){draw_pair(d,y,"Driver:",q->driver);y+=22;draw_pair(d,y,"Version:",q->driver_version);y+=22;draw_pair(d,y,"Date:",q->driver_date);y+=22;draw_pair(d,y,"Provider / author:",q->driver_author);y+=30;w2k_draw_pushbutton(d,&p->details,"Driver Details...",0);w2k_draw_pushbutton(d,&p->update,"Update Driver...",0);
        p->enable=(W2kRect){c.x+272,c.y+95,105,23};
        w2k_draw_pushbutton(d,&p->enable,q->disabled?"Enable Device":"Disable Device",q->disabled?0:0);
        if(q->is_dkms){p->uninstall=(W2kRect){c.x+384,c.y+95,110,23};w2k_draw_pushbutton(d,&p->uninstall,"Uninstall...",0);}}
    else if(p->tab==2){snprintf(b,sizeof b,"sysfs path: %.480s",q->sysfs_path);w2k_text(d,F_UI,16,y,b,C_WINDOWTEXT);y+=24;draw_pair(d,y,"Name:",q->name);y+=22;draw_pair(d,y,"Raw location:",q->raw_location);y+=22;draw_pair(d,y,"Subsystem:",q->subsystem);y+=22;draw_pair(d,y,"Vendor ID:",q->vendor_id);y+=22;draw_pair(d,y,"Device ID:",q->device_id);y+=22;draw_pair(d,y,"Modalias:",q->modalias);}
    else {w2k_text(d,F_UI,16,y,"Resource information",C_WINDOWTEXT);y+=24;char out[4096];w2k_device_resources(q,out,sizeof out);for(char*line=strtok(out,"\n");line&&y<c.y+c.h-30;line=strtok(NULL,"\n")){w2k_text(d,F_FIXED,16,y,line,C_WINDOWTEXT);y+=15;}}
    w2k_draw_pushbutton(d,&p->ok,"OK",BS_DEFAULT|(p->down==1?BS_PRESSED:0));}
static void props_layout(W2kWin*w){Props*p=w->user;p->tabs->r=(W2kRect){8,MENUBAR_H+4,w->w-16,w->h-MENUBAR_H-12};W2kRect c=w2k_tabs_client(p->tabs);p->ok=(W2kRect){w->w-92,w->h-34,76,23};p->details=(W2kRect){c.x+16,c.y+95,120,23};p->update=(W2kRect){c.x+144,c.y+95,120,23};}
static void driver_paint(W2kWin *x, Drawable d) {
    char *copy = x->user ? strdup((char *)x->user) : NULL;
    w2k_draw_well(d, &(W2kRect){8,8,x->w-24,x->h-48});
    int y=14;
    for(char *l=copy ? strtok(copy,"\n") : NULL; l && y<x->h-36; l=strtok(NULL,"\n")) {
        w2k_text(d,F_FIXED,14,y,l,C_WINDOWTEXT); y+=15;
    }
    free(copy);
    w2k_draw_pushbutton(d,&(W2kRect){x->w-92,x->h-32,76,23},"OK",BS_DEFAULT);
}
static int driver_event(W2kWin *x, XEvent *e) {
    if(e->type==ButtonPress && e->xbutton.button==Button1 && e->xbutton.y>x->h-40) {
        w2k_win_close(x,0); return 1;
    }
    if(e->type==KeyPress) {
        KeySym ks=XLookupKeysym(&e->xkey,0);
        if(ks==XK_Return || ks==XK_Escape) { w2k_win_close(x,0); return 1; }
    }
    return 0;
}
static int driver_close(W2kWin *x) { free(x->user); x->user=NULL; return 1; }
static void show_modinfo_dialog(Props*p) {
    char out[16384]; w2k_device_modinfo(p->d->driver,out,sizeof out);
    W2kWin*w=w2k_win_new("Driver File Details","l2kdevmgmt-driver",560,420,1);
    w->min_w=400; w->min_h=260; w->user=strdup(out); w->paint=driver_paint; w->event=driver_event; w->closing=driver_close;
    w2k_win_center(w,p->win); w2k_win_modal(w);
}
static int props_event(W2kWin*w,XEvent*e){Props*p=w->user;if(e->type==ButtonPress){if(w2k_tabs_press(p->tabs,&e->xbutton)){w2k_win_dirty(w);return 1;}if(w2k_rect_hit(&p->ok,e->xbutton.x,e->xbutton.y)){p->down=1;w2k_win_dirty(w);return 1;}if(p->tab==1&&w2k_rect_hit(&p->details,e->xbutton.x,e->xbutton.y)){show_modinfo_dialog(p);return 1;}if(p->tab==1&&w2k_rect_hit(&p->update,e->xbutton.x,e->xbutton.y)){update_driver_wizard(p->d);return 1;}
        if(p->tab==1&&w2k_rect_hit(&p->enable,e->xbutton.x,e->xbutton.y)){char err[1024];if(w2k_device_set_enabled(p->d,p->d->disabled,err,sizeof err)==0){w2k_win_close(w,0);scan();}else w2k_notify("Device Manager",err);return 1;}
        if(p->tab==1&&p->d->is_dkms&&w2k_rect_hit(&p->uninstall,e->xbutton.x,e->xbutton.y)){char err[1024];if(w2k_device_uninstall_dkms(p->d,err,sizeof err)==0){w2k_win_close(w,0);scan();}else w2k_notify("Uninstall failed",err);return 1;}}else if(e->type==ButtonRelease){if(p->down&&w2k_rect_hit(&p->ok,e->xbutton.x,e->xbutton.y))w2k_win_close(w,0);p->down=0;w2k_win_dirty(w);}else if(e->type==KeyPress){if(w2k_tabs_key(p->tabs,&e->xkey)){w2k_win_dirty(w);return 1;}KeySym ks=XLookupKeysym(&e->xkey,0);if(ks==XK_Escape||ks==XK_Return){w2k_win_close(w,0);return 1;}}return 0;}
static void show_properties(W2kDevice*d){if(!d)return;Props*p=calloc(1,sizeof*p);if(!p)return;p->d=d;p->win=w2k_win_new(d->name,"l2kdevmgmt-properties",520,360,0);p->win->user=p;p->win->paint=props_paint;p->win->event=props_event;p->win->resized=props_layout;p->tabs=w2k_tabs_new(p,props_tab);w2k_tabs_add(p->tabs,"General");w2k_tabs_add(p->tabs,"Driver");w2k_tabs_add(p->tabs,"Details");w2k_tabs_add(p->tabs,"Resources");props_layout(p->win);w2k_win_center(p->win,dm.win);w2k_win_modal(p->win);w2k_tabs_free(p->tabs);free(p);}

static void uevent_tick(void *u){(void)u;if(w2k_device_monitor_poll())scan();}

int main(void){if(w2k_init("l2kdevmgmt")<0)return 1;memset(&dm,0,sizeof dm);w2k_devices_init(&dm.devices);dm.win=w2k_win_new("Device Manager","l2kdevmgmt",660,500,1);dm.win->min_w=480;dm.win->min_h=320;dm.win->paint=paint;dm.win->event=event;dm.win->resized=layout;dm.mb=w2k_menubar_new(NULL,command);dm.mb->win_ref=dm.win->win;dm.tree=w2k_tree_new();dm.tree->focused=1;dm.tree->on_select=on_tree_select;w2k_scroll_bind(&dm.tree->vsb,dm.win);dm.status=w2k_status_new();w2k_status_add(dm.status,0);build_menus();layout(dm.win);scan();w2k_device_monitor_open();w2k_add_timer(1000,uevent_tick,NULL);w2k_win_show(dm.win);w2k_run();w2k_tree_free(dm.tree);w2k_menubar_free(dm.mb);w2k_status_free(dm.status);w2k_device_monitor_close();w2k_devices_free(&dm.devices);w2k_fini();return 0;}
