#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "device.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>

static void cp(char *dst, size_t n, const char *s) {
    if (!n) return;
    if (!s) s="";
    size_t z=strlen(s); if(z>=n) z=n-1;
    memcpy(dst,s,z); dst[z]=0;
}
static int readfile(const char *path, char *buf, size_t n) {
    if (!n) return -1;
    FILE *f=fopen(path,"r");
    if(!f){buf[0]=0;return -1;}
    size_t z=fread(buf,1,n-1,f);
    fclose(f); buf[z]=0;
    while(z && (buf[z-1]=='\n'||buf[z-1]=='\r')) buf[--z]=0;
    return 0;
}
static int exists(const char *p){return access(p,F_OK)==0;}
static void base_name(const char *p,char *b,size_t n){const char *s=strrchr(p,'/');cp(b,n,s?s+1:p);}
static void trim(char *s) {
    if (!s) return;
    char *a=s; while (*a && isspace((unsigned char)*a)) a++;
    if (a!=s) memmove(s,a,strlen(a)+1);
    size_t n=strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n]=0;
}

static int addcat(W2kDeviceSet *s,const char *name,const char *icon){
    for(size_t i=0;i<s->count;i++) if(!strcmp(s->cats[i].name,name)) return (int)i;
    if(s->count>=W2K_DEV_MAX_CATS)return -1;
    W2kDeviceCategory *c=&s->cats[s->count++]; memset(c,0,sizeof *c);
    cp(c->name,sizeof c->name,name); cp(c->icon,sizeof c->icon,icon);
    c->devices=calloc(64,sizeof *c->devices); return (int)(s->count-1);
}
static W2kDevice *adddev(W2kDeviceSet *s,int ci){W2kDeviceCategory *c=&s->cats[ci];if(c->count%64==0){size_t nc=c->count+64;W2kDevice *p=realloc(c->devices,nc*sizeof *p);if(!p)return NULL;c->devices=p;}W2kDevice *d=&c->devices[c->count++];memset(d,0,sizeof *d);cp(d->status,sizeof d->status,"This device is working properly.");return d;}
static int validmod(const char *s);
static int runv(const char *const av[], char *err, size_t n);

static int module_blacklisted(const char *module)
{
    if (!validmod(module)) return 0;
    FILE *f=fopen("/etc/modprobe.d/devicemanager-disabled.conf","r");
    if(!f)return 0;
    char line[512],target[512]; snprintf(target,sizeof target,"blacklist %s",module);
    int yes=0;
    while(fgets(line,sizeof line,f)){line[strcspn(line,"\r\n")]=0;if(!strcmp(line,target)){yes=1;break;}}
    fclose(f); return yes;
}

static void fill_common(W2kDevice *d,const char *sysfs,const char *name)
{
    cp(d->name,sizeof d->name,name); cp(d->sysfs_path,sizeof d->sysfs_path,sysfs);
    cp(d->raw_location,sizeof d->raw_location,sysfs);
    char p[PATH_MAX],v[512],target[PATH_MAX];
    snprintf(p,sizeof p,"%s/vendor",sysfs); if(readfile(p,v,sizeof v)==0) cp(d->vendor_id,sizeof d->vendor_id,v);
    snprintf(p,sizeof p,"%s/device",sysfs); if(readfile(p,v,sizeof v)==0) cp(d->device_id,sizeof d->device_id,v);
    snprintf(p,sizeof p,"%s/modalias",sysfs); if(readfile(p,v,sizeof v)==0) cp(d->modalias,sizeof d->modalias,v);
    snprintf(p,sizeof p,"%s/manufacturer",sysfs); if(readfile(p,v,sizeof v)==0) cp(d->manufacturer,sizeof d->manufacturer,v);
    snprintf(p,sizeof p,"%s/driver",sysfs);
    ssize_t z=readlink(p,target,sizeof target-1);
    if(z>0){target[z]=0;base_name(target,d->driver,sizeof d->driver);}
    if(!d->driver[0]){snprintf(p,sizeof p,"%s/driver_override",sysfs);readfile(p,d->driver,sizeof d->driver);}
    if(d->driver[0]){
        char mi[4096]={0}; const char *av[]={"modinfo","--",d->driver,NULL};
        if(runv(av,mi,sizeof mi)==0){char *save=NULL;for(char *line=strtok_r(mi,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){char*c=strchr(line,':');if(!c)continue;*c=0;char*val=c+1;while(*val==' '||*val=='\t')val++;if(!strcmp(line,"version"))cp(d->driver_version,sizeof d->driver_version,val);else if(!strcmp(line,"author"))cp(d->driver_author,sizeof d->driver_author,val);}}
        char dk[PATH_MAX];snprintf(dk,sizeof dk,"/var/lib/dkms/%s",d->driver);if(exists(dk)){d->is_dkms=1;struct stat st;if(stat(dk,&st)==0){struct tm*t=localtime(&st.st_mtime);if(t)strftime(d->driver_date,sizeof d->driver_date,"%Y-%m-%d",t);}}
        d->disabled=module_blacklisted(d->driver); if(d->disabled)cp(d->status,sizeof d->status,"This device is disabled.");
    }
}

static void attr_path(char *dst, size_t n, const char *root, const char *attr)
{
    snprintf(dst, n, "%s/%s", root, attr);
}

static void read_attr(const char *root, const char *attr, char *out, size_t n)
{
    char p[PATH_MAX]; attr_path(p, sizeof p, root, attr); readfile(p, out, n);
}

static void human_pci_name(W2kDevice *d, const char *p, const char *addr, const char *cls)
{
    char vendor[64]={0}, device[64]={0}, subvendor[64]={0}, subdevice[64]={0}, rev[64]={0};
    char label[256]={0}, lspci[1024]={0};
    read_attr(p, "vendor", vendor, sizeof vendor); read_attr(p, "device", device, sizeof device);
    read_attr(p, "subsystem_vendor", subvendor, sizeof subvendor); read_attr(p, "subsystem_device", subdevice, sizeof subdevice);
    read_attr(p, "revision", rev, sizeof rev);
    cp(d->vendor_id, sizeof d->vendor_id, vendor); cp(d->device_id, sizeof d->device_id, device);

    const char *kind = "PCI device";
    if (!strncmp(cls, "0x03", 4)) kind = "Display controller";
    else if (!strncmp(cls, "0x02", 4)) kind = "Network controller";
    else if (!strncmp(cls, "0x01", 4)) kind = "Storage controller";
    else if (!strncmp(cls, "0x04", 4)) kind = "Multimedia controller";
    else if (!strncmp(cls, "0x0c03", 6)) kind = "USB controller";
    else if (!strncmp(cls, "0x0c", 4)) kind = "Serial bus controller";
    else if (!strncmp(cls, "0x06", 4)) kind = "Bridge";
    else if (!strncmp(cls, "0x0d", 4)) kind = "Wireless controller";

    /* lspci is only an optional name database. sysfs remains the authoritative
       inventory, so the manager still works when pciutils is not installed. */
    const char *av[] = {"lspci", "-D", "-nn", "-s", addr, NULL};
    if (runv(av, lspci, sizeof lspci) == 0 && lspci[0]) {
        char *line=lspci, *colon=strchr(line, ':');
        if (colon) {
            char *name=colon+1; while (*name==' '||*name=='\t') name++;
            char *br=strrchr(name, '[');
            if (br) {
                char *close=strchr(br, ']');
                if (close) *close=0;
            }
            trim(name);
            if (name[0]) cp(label, sizeof label, name);
        }
    }
    if (!label[0]) {
        char driver_name[128]={0}; cp(driver_name,sizeof driver_name,d->driver);
        if (!strcmp(driver_name,"(null)")) driver_name[0]=0;
        snprintf(label,sizeof label,"%s%s%s [%s:%s]",driver_name[0]?driver_name:"",driver_name[0]?" — ":"",kind,
                 vendor[0]?vendor:"????",device[0]?device:"????");
    } else if (d->driver[0] && strcmp(d->driver,"(null)")) {
        char tmp[256]; snprintf(tmp,sizeof tmp,"%s — %s",label,d->driver); cp(label,sizeof label,tmp);
    }
    cp(d->name,sizeof d->name,label);
    snprintf(d->description,sizeof d->description,"%s, PCI address %s%s%s",kind,addr,rev[0]?", revision ":"",rev[0]?rev:"");
    snprintf(d->location,sizeof d->location,"PCI bus %s",addr);
    snprintf(d->address,sizeof d->address,"%s",addr);
    (void)subvendor; (void)subdevice;
}

static void scan_dmi(W2kDeviceSet *s)
{
    char vendor[256]={0}, board[256]={0}, boardver[256]={0}, product[256]={0}, sysver[256]={0}, bios[256]={0};
    readfile("/sys/devices/virtual/dmi/id/board_vendor",vendor,sizeof vendor);
    readfile("/sys/devices/virtual/dmi/id/board_name",board,sizeof board);
    readfile("/sys/devices/virtual/dmi/id/board_version",boardver,sizeof boardver);
    readfile("/sys/devices/virtual/dmi/id/product_name",product,sizeof product);
    readfile("/sys/devices/virtual/dmi/id/product_version",sysver,sizeof sysver);
    readfile("/sys/devices/virtual/dmi/id/bios_version",bios,sizeof bios);
    if (!vendor[0] && !board[0] && !product[0]) return;

    int ci=addcat(s,"System information","computer"); if(ci<0)return;
    if (board[0] || vendor[0]) {
        W2kDevice*d=adddev(s,ci); if(!d)return;
        snprintf(d->name,sizeof d->name,"%s%s%s — Motherboard",vendor[0]?vendor:"",vendor[0]&&board[0]?" ":"",board[0]?board:"System board");
        snprintf(d->description,sizeof d->description,"Motherboard%s%s%s%s",boardver[0]?", version ":"",boardver[0]?boardver:"",vendor[0]?", manufacturer ":"",vendor[0]?vendor:"");
        cp(d->manufacturer,sizeof d->manufacturer,vendor); cp(d->location,sizeof d->location,"DMI / board");
        cp(d->address,sizeof d->address,"/sys/devices/virtual/dmi/id"); cp(d->subsystem,sizeof d->subsystem,"DMI"); cp(d->icon,sizeof d->icon,"computer");
    }
    if (product[0]) {
        W2kDevice*d=adddev(s,ci); if(!d)return;
        snprintf(d->name,sizeof d->name,"%s%s%s — System",vendor[0]?vendor:"",vendor[0]&&product[0]?" ":"",product);
        snprintf(d->description,sizeof d->description,"System model%s%s%s%s",sysver[0]?", version ":"",sysver[0]?sysver:"",bios[0]?", BIOS ":"",bios[0]?bios:"");
        cp(d->manufacturer,sizeof d->manufacturer,vendor); cp(d->location,sizeof d->location,"DMI / system");
        cp(d->address,sizeof d->address,"/sys/devices/virtual/dmi/id"); cp(d->subsystem,sizeof d->subsystem,"DMI"); cp(d->icon,sizeof d->icon,"computer");
    }
}

static int pci_category(const char *cls, char *cat, size_t cn, char *icon, size_t in)
{
    if (!strncmp(cls,"0x03",4)) { cp(cat,cn,"Display adapters"); cp(icon,in,"video-display"); return 1; }
    if (!strncmp(cls,"0x02",4)) { cp(cat,cn,"Network adapters"); cp(icon,in,"network-card"); return 1; }
    if (!strncmp(cls,"0x0c03",6)) { cp(cat,cn,"USB controllers"); cp(icon,in,"drive-removable-media-usb"); return 1; }
    if (!strncmp(cls,"0x04",4)) { cp(cat,cn,"Audio and multimedia"); cp(icon,in,"kmix"); return 1; }
    if (!strncmp(cls,"0x01",4)) { cp(cat,cn,"Storage controllers"); cp(icon,in,"drive-harddisk"); return 1; }
    if (!strncmp(cls,"0x0d",4)) { cp(cat,cn,"Input and human interface"); cp(icon,in,"input_devices_settings"); return 1; }
    if (!strncmp(cls,"0x06",4)) { cp(cat,cn,"Bridges and buses"); cp(icon,in,"computer"); return 1; }
    cp(cat,cn,"System devices"); cp(icon,in,"computer"); return 1;
}

static void scan_pci(W2kDeviceSet *s)
{
    DIR *dp = opendir("/sys/bus/pci/devices"); if (!dp) return; struct dirent *e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX], cls[64], cat[96], icon[64];
        snprintf(p,sizeof p,"/sys/bus/pci/devices/%s",e->d_name); read_attr(p,"class",cls,sizeof cls);
        pci_category(cls,cat,sizeof cat,icon,sizeof icon); int ci=addcat(s,cat,icon); if(ci<0)continue;
        W2kDevice *d=adddev(s,ci); if(!d)break; fill_common(d,p,e->d_name); human_pci_name(d,p,e->d_name,cls);
        cp(d->subsystem,sizeof d->subsystem,"PCI"); cp(d->icon,sizeof d->icon,icon);
    }
    closedir(dp);
}

static void scan_usb(W2kDeviceSet *s)
{
    DIR *dp=opendir("/sys/bus/usb/devices"); if(!dp)return; struct dirent *e;
    while((e=readdir(dp))){
        if(e->d_name[0]=='.' || !strncmp(e->d_name,"usb",3)) continue;
        char p[PATH_MAX], man[256], prod[256], serial[256], vid[64], pid[64], speed[64], cls[64];
        snprintf(p,sizeof p,"/sys/bus/usb/devices/%s",e->d_name);
        read_attr(p,"manufacturer",man,sizeof man); read_attr(p,"product",prod,sizeof prod); read_attr(p,"serial",serial,sizeof serial);
        read_attr(p,"idVendor",vid,sizeof vid); read_attr(p,"idProduct",pid,sizeof pid); read_attr(p,"speed",speed,sizeof speed); read_attr(p,"bDeviceClass",cls,sizeof cls);
        if(!man[0]&&!prod[0]&&!vid[0]&&!pid[0]) continue;
        int ci=addcat(s,"USB devices","drive-removable-media-usb"); if(ci<0)break; W2kDevice*d=adddev(s,ci);if(!d)break;
        snprintf(d->name,sizeof d->name,"%s%s%s",man,man[0]&&prod[0]?" ":"",prod[0]?prod:e->d_name);
        snprintf(d->description,sizeof d->description,"USB device %s:%s%s%s",vid[0]?vid:"????",pid[0]?pid:"????",speed[0]?" — ":"",speed[0]?speed:"unknown speed");
        cp(d->manufacturer,sizeof d->manufacturer,man); cp(d->vendor_id,sizeof d->vendor_id,vid); cp(d->device_id,sizeof d->device_id,pid);
        cp(d->location,sizeof d->location,e->d_name); cp(d->address,sizeof d->address,e->d_name); cp(d->subsystem,sizeof d->subsystem,"USB"); cp(d->icon,sizeof d->icon,"drive-removable-media-usb");
        if(serial[0]) snprintf(d->raw_location,sizeof d->raw_location,"%s — serial %s",p,serial); else cp(d->raw_location,sizeof d->raw_location,p);
    }
    closedir(dp);
}

static void scan_disks(W2kDeviceSet *s)
{
    DIR *dp=opendir("/sys/block"); if(!dp)return; struct dirent*e;
    while((e=readdir(dp))){
        if(e->d_name[0]=='.')continue;
        char p[PATH_MAX],model[256],vendor[256],rev[128],tran[128],rem[64],sizeb[128],name[W2K_DEV_STR];
        snprintf(p,sizeof p,"/sys/block/%s",e->d_name); read_attr(p,"device/model",model,sizeof model); read_attr(p,"device/vendor",vendor,sizeof vendor);
        read_attr(p,"device/rev",rev,sizeof rev); read_attr(p,"queue/logical_block_size",tran,sizeof tran); read_attr(p,"removable",rem,sizeof rem); read_attr(p,"size",sizeb,sizeof sizeb);
        unsigned long long sectors=strtoull(sizeb,NULL,10), bytes=sectors*512ULL;
        const char *kind=!strncmp(e->d_name,"sr",2)?"Optical drive":(!strncmp(e->d_name,"mmc",3)?"MMC storage":(!strncmp(e->d_name,"loop",4)?"Loop device":(!strncmp(e->d_name,"ram",3)?"RAM disk":"Disk drive")));
        const char *friendly=model[0]?model:e->d_name;
        snprintf(name,sizeof name,"%s (%s)",friendly,kind);
        const char *dcat=!strcmp(kind,"Optical drive")?"Optical drives":(!strcmp(kind,"Loop device")?"Loop devices":(!strcmp(kind,"RAM disk")?"RAM disks":"Disk drives"));
        const char *dicon=!strcmp(kind,"Optical drive")?"drive-optical":"drive-harddisk";
        int ci=addcat(s,dcat,dicon);if(ci<0)continue;
        W2kDevice*d=adddev(s,ci);if(!d)break;fill_common(d,p,name);cp(d->name,sizeof d->name,name);cp(d->manufacturer,sizeof d->manufacturer,vendor);cp(d->description,sizeof d->description,model[0]?model:"Block storage device");
        snprintf(d->location,sizeof d->location,"/dev/%s%s",e->d_name,rem[0]&&rem[0]=='1'?" — removable":""); cp(d->address,sizeof d->address,e->d_name);cp(d->subsystem,sizeof d->subsystem,"Block storage");
        if(bytes) snprintf(d->raw_location,sizeof d->raw_location,"%s — %llu MiB",p,(unsigned long long)(bytes/(1024ULL*1024ULL))); else cp(d->raw_location,sizeof d->raw_location,p);
        (void)tran;(void)rev;
    }
    closedir(dp);
}

static void scan_cpus(W2kDeviceSet *s)
{
    char model[256]={0}, vendor[256]={0}, mhz_cpu[128]={0};
    FILE *f=fopen("/proc/cpuinfo","r");
    if(f){char line[512];while(fgets(line,sizeof line,f)){if(!strncmp(line,"model name",10)){char *c=strchr(line,':');if(c){cp(model,sizeof model,c+1);trim(model);}}else if(!strncmp(line,"vendor_id",9)){char*c=strchr(line,':');if(c){cp(vendor,sizeof vendor,c+1);trim(vendor);}}else if(!strncmp(line,"cpu MHz",7)&&!mhz_cpu[0]){char*c=strchr(line,':');if(c){cp(mhz_cpu,sizeof mhz_cpu,c+1);trim(mhz_cpu);}}if(model[0]&&vendor[0]&&mhz_cpu[0])break;}fclose(f);}
    DIR*dp=opendir("/sys/devices/system/cpu");if(!dp)return;struct dirent*e;int ci=-1;
    while((e=readdir(dp))){if(strncmp(e->d_name,"cpu",3)||!isdigit((unsigned char)e->d_name[3]))continue;if(ci<0)ci=addcat(s,"Processors","cpu");if(ci<0)break;
        char p[PATH_MAX],mhz[128];snprintf(p,sizeof p,"/sys/devices/system/cpu/%s",e->d_name);read_attr(p,"cpufreq/cpuinfo_cur_freq",mhz,sizeof mhz);
        W2kDevice*d=adddev(s,ci);if(!d)break;
        snprintf(d->name,sizeof d->name,"%s — Processor %s",model[0]?model:"Processor",e->d_name+3);
        snprintf(d->description,sizeof d->description,"Logical processor %s%s%s%s%s",e->d_name+3,vendor[0]?" — ":"",vendor[0]?vendor:"",(mhz[0]||mhz_cpu[0])?" — ":"",mhz[0]?mhz:mhz_cpu);
        cp(d->manufacturer,sizeof d->manufacturer,vendor);cp(d->location,sizeof d->location,e->d_name);cp(d->address,sizeof d->address,e->d_name);cp(d->subsystem,sizeof d->subsystem,"CPU");cp(d->icon,sizeof d->icon,"cpu");
    }
    closedir(dp);
}

static void scan_network(W2kDeviceSet *s)
{
    DIR*dp=opendir("/sys/class/net");if(!dp)return;struct dirent*e;int ci=-1;
    while((e=readdir(dp))){if(e->d_name[0]=='.')continue;char p[PATH_MAX],type[64],oper[64],mac[128],mtu[64];snprintf(p,sizeof p,"/sys/class/net/%s",e->d_name);read_attr(p,"type",type,sizeof type);read_attr(p,"operstate",oper,sizeof oper);read_attr(p,"address",mac,sizeof mac);read_attr(p,"mtu",mtu,sizeof mtu);if(ci<0)ci=addcat(s,"Network adapters","network-card");if(ci<0)break;W2kDevice*d=adddev(s,ci);if(!d)break;const char*kind=!strcmp(e->d_name,"lo")?"Loopback adapter":(e->d_name[0]=='w' ? "Wireless network adapter" : "Network adapter");snprintf(d->name,sizeof d->name,"%s — %s",kind,e->d_name);snprintf(d->description,sizeof d->description,"%s, state %s, MAC %s",kind,oper[0]?oper:"unknown",mac[0]?mac:"unknown");cp(d->location,sizeof d->location,e->d_name);cp(d->address,sizeof d->address,e->d_name);cp(d->subsystem,sizeof d->subsystem,"Network");cp(d->icon,sizeof d->icon,"network-card");(void)type;(void)mtu;}
    closedir(dp);
}

static void scan_input(W2kDeviceSet *s)
{
    DIR*dp=opendir("/sys/class/input");if(!dp)return;struct dirent*e;int ci=-1;
    while((e=readdir(dp))){if(strncmp(e->d_name,"event",5)&&strncmp(e->d_name,"js",2)&&strncmp(e->d_name,"mouse",5)&&strncmp(e->d_name,"kbd",3))continue;char p[PATH_MAX],name[256],phys[256],uniq[256];snprintf(p,sizeof p,"/sys/class/input/%s",e->d_name);read_attr(p,"device/name",name,sizeof name);read_attr(p,"device/phys",phys,sizeof phys);read_attr(p,"device/uniq",uniq,sizeof uniq);if(!name[0])continue;if(ci<0)ci=addcat(s,"Input devices","input_devices_settings");if(ci<0)break;W2kDevice*d=adddev(s,ci);if(!d)break;snprintf(d->name,sizeof d->name,"%s",name);snprintf(d->description,sizeof d->description,"%s (%s)",name,e->d_name);cp(d->location,sizeof d->location,phys[0]?phys:e->d_name);cp(d->address,sizeof d->address,e->d_name);cp(d->subsystem,sizeof d->subsystem,"Input");cp(d->icon,sizeof d->icon,"input_devices_settings");if(uniq[0])cp(d->raw_location,sizeof d->raw_location,uniq);}
    closedir(dp);
}

static void scan_sound(W2kDeviceSet *s)
{
    DIR*dp=opendir("/sys/class/sound");if(!dp)return;struct dirent*e;int ci=-1;
    while((e=readdir(dp))){if(strncmp(e->d_name,"card",4))continue;char p[PATH_MAX],id[256],name[256];snprintf(p,sizeof p,"/sys/class/sound/%s",e->d_name);read_attr(p,"id",id,sizeof id);read_attr(p,"device/name",name,sizeof name);if(!id[0]&&!name[0])continue;if(ci<0)ci=addcat(s,"Audio devices","kmix");if(ci<0)break;W2kDevice*d=adddev(s,ci);if(!d)break;snprintf(d->name,sizeof d->name,"%s",name[0]?name:id);snprintf(d->description,sizeof d->description,"Sound card %s",e->d_name);cp(d->location,sizeof d->location,e->d_name);cp(d->address,sizeof d->address,e->d_name);cp(d->subsystem,sizeof d->subsystem,"ALSA");cp(d->icon,sizeof d->icon,"kmix");}
    closedir(dp);
}

static void scan_drm(W2kDeviceSet *s)
{
    DIR*dp=opendir("/sys/class/drm");if(!dp)return;struct dirent*e;int ci=-1;
    while((e=readdir(dp))){if(strncmp(e->d_name,"card",4)||strchr(e->d_name,'-'))continue;char p[PATH_MAX],status[64],name[256];snprintf(p,sizeof p,"/sys/class/drm/%s",e->d_name);read_attr(p,"status",status,sizeof status);read_attr(p,"device/driver/module",name,sizeof name);if(ci<0)ci=addcat(s,"Graphics adapters","video-display");if(ci<0)break;W2kDevice*d=adddev(s,ci);if(!d)break;snprintf(d->name,sizeof d->name,"Graphics adapter %s",e->d_name);snprintf(d->description,sizeof d->description,"DRM device %s%s%s",e->d_name,status[0]?" — ":"",status);cp(d->location,sizeof d->location,e->d_name);cp(d->address,sizeof d->address,e->d_name);cp(d->subsystem,sizeof d->subsystem,"DRM");cp(d->icon,sizeof d->icon,"video-display");}
    closedir(dp);
}

static void scan_tty(W2kDeviceSet *s)
{
    DIR*dp=opendir("/sys/class/tty");if(!dp)return;struct dirent*e;int ci=-1;
    while((e=readdir(dp))){if(e->d_name[0]=='.')continue;if(strncmp(e->d_name,"ttyS",4)&&strncmp(e->d_name,"ttyUSB",6)&&strncmp(e->d_name,"ttyACM",6)&&strncmp(e->d_name,"ttyAMA",6)&&strcmp(e->d_name,"console"))continue;char p[PATH_MAX],devname[256],driver[256];snprintf(p,sizeof p,"/sys/class/tty/%s",e->d_name);read_attr(p,"device/name",devname,sizeof devname);read_attr(p,"device/driver",driver,sizeof driver);if(ci<0)ci=addcat(s,"Serial ports and terminals","computer");if(ci<0)break;W2kDevice*d=adddev(s,ci);if(!d)break;snprintf(d->name,sizeof d->name,"%s",devname[0]?devname:e->d_name);snprintf(d->description,sizeof d->description,"Serial/terminal device /dev/%s",e->d_name);cp(d->location,sizeof d->location,e->d_name);cp(d->address,sizeof d->address,e->d_name);cp(d->subsystem,sizeof d->subsystem,"TTY");cp(d->icon,sizeof d->icon,"computer");}
    closedir(dp);
}

static void scan_power_sensors(W2kDeviceSet *s)
{
    DIR*dp=opendir("/sys/class/power_supply");if(dp){struct dirent*e;int ci=-1;while((e=readdir(dp))){if(e->d_name[0]=='.')continue;char p[PATH_MAX],type[64],cap[64],status[64],model[256];snprintf(p,sizeof p,"/sys/class/power_supply/%s",e->d_name);read_attr(p,"type",type,sizeof type);read_attr(p,"capacity",cap,sizeof cap);read_attr(p,"status",status,sizeof status);read_attr(p,"model_name",model,sizeof model);if(ci<0)ci=addcat(s,"Batteries and power","kded5");if(ci<0)break;W2kDevice*d=adddev(s,ci);if(!d)break;snprintf(d->name,sizeof d->name,"%s",model[0]?model:e->d_name);snprintf(d->description,sizeof d->description,"%s%s%s%s",type[0]?type:"Power supply",cap[0]?" — ":"",cap[0]?cap:"",status[0]?status:"");cp(d->location,sizeof d->location,e->d_name);cp(d->subsystem,sizeof d->subsystem,"Power");cp(d->icon,sizeof d->icon,"kded5");}closedir(dp);}
    dp=opendir("/sys/class/hwmon");if(dp){struct dirent*e;int ci=-1;while((e=readdir(dp))){if(e->d_name[0]=='.')continue;char p[PATH_MAX],name[256];snprintf(p,sizeof p,"/sys/class/hwmon/%s",e->d_name);read_attr(p,"name",name,sizeof name);if(ci<0)ci=addcat(s,"Sensors and hardware monitoring","computer");if(ci<0)break;W2kDevice*d=adddev(s,ci);if(!d)break;snprintf(d->name,sizeof d->name,"%s",name[0]?name:e->d_name);snprintf(d->description,sizeof d->description,"Hardware monitoring sensor %s",e->d_name);cp(d->location,sizeof d->location,e->d_name);cp(d->subsystem,sizeof d->subsystem,"hwmon");cp(d->icon,sizeof d->icon,"computer");}closedir(dp);}
}

static void scan_virtual(W2kDeviceSet *s)
{
    DIR*dp=opendir("/sys/devices/virtual");if(!dp)return;struct dirent*e;int ci=addcat(s,"Virtual and platform devices","computer");if(ci<0){closedir(dp);return;}while((e=readdir(dp))){if(e->d_name[0]=='.')continue;char p[PATH_MAX];snprintf(p,sizeof p,"/sys/devices/virtual/%s",e->d_name);W2kDevice*d=adddev(s,ci);if(!d)break;snprintf(d->name,sizeof d->name,"%s devices",e->d_name);snprintf(d->description,sizeof d->description,"Linux virtual device subsystem: %s",e->d_name);cp(d->location,sizeof d->location,p);cp(d->subsystem,sizeof d->subsystem,"Virtual");cp(d->icon,sizeof d->icon,"computer");}closedir(dp);
}

void w2k_devices_init(W2kDeviceSet*s){memset(s,0,sizeof *s);struct utsname u;if(uname(&u)==0)cp(s->host,sizeof s->host,u.nodename);if(!s->host[0])cp(s->host,sizeof s->host,"LOCALHOST");}
void w2k_devices_free(W2kDeviceSet*s){for(size_t i=0;i<s->count;i++)free(s->cats[i].devices);memset(s,0,sizeof *s);}
int w2k_devices_scan(W2kDeviceSet*s){char host[W2K_DEV_STR];cp(host,sizeof host,s->host);w2k_devices_free(s);cp(s->host,sizeof s->host,host);
    scan_dmi(s); scan_pci(s); scan_usb(s); scan_disks(s); scan_cpus(s); scan_network(s); scan_input(s); scan_sound(s); scan_drm(s); scan_tty(s); scan_power_sensors(s); scan_virtual(s);
    int di=addcat(s,"Disabled devices","dialog-cancel"); if(di>=0){FILE*f=fopen("/etc/modprobe.d/devicemanager-disabled.conf","r");if(f){char line[512],mod[256];while(fgets(line,sizeof line,f)){if(sscanf(line,"blacklist %255s",mod)!=1)continue;W2kDevice*d=adddev(s,di);if(!d)break;snprintf(d->name,sizeof d->name,"%s (disabled)",mod);snprintf(d->description,sizeof d->description,"Blacklisted kernel module");cp(d->driver,sizeof d->driver,mod);cp(d->status,sizeof d->status,"This device is disabled.");cp(d->location,sizeof d->location,"Driver blacklist");cp(d->subsystem,sizeof d->subsystem,"Kernel modules");d->disabled=1;}fclose(f);}if(s->cats[di].count==0){free(s->cats[di].devices);s->count--;}}
    return 0;
}

static int runv(const char *const av[], char *err, size_t n)
{
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        close(p[0]); close(p[1]);
        execvp(av[0], (char *const *)av);
        _exit(127);
    }
    close(p[1]);
    size_t z=0; char b[512]; ssize_t r;
    while((r=read(p[0],b,sizeof b))>0 && err && n>1) {
        size_t k=(size_t)r;
        if(k>n-1-z) k=n-1-z;
        memcpy(err+z,b,k); z+=k; err[z]=0;
        if(z==n-1) { char sink[512]; while(read(p[0],sink,sizeof sink)>0) {} break; }
    }
    close(p[0]);
    int st=0; waitpid(pid,&st,0);
    return WIFEXITED(st)?WEXITSTATUS(st):-1;
}

int w2k_device_modinfo(const char *m,char*out,size_t n)
{
    if(!m||!m[0]||!strcmp(m,"(kernel)")){cp(out,n,"module: (built into kernel)\n");return 0;}
    const char *av[]={"modinfo","--",m,NULL};
    return runv(av,out,n);
}

int w2k_device_resources(const W2kDevice*d,char*out,size_t n)
{
    if(!d||!n)return -1;
    out[0]=0; char p[PATH_MAX],b[4096];
    snprintf(p,sizeof p,"%s/resource",d->sysfs_path);
    if(readfile(p,b,sizeof b)==0){snprintf(out,n,"PCI resources:\n%s\n",b);return 0;}
    snprintf(out,n,"System resources:\n\n/proc/interrupts\n/proc/ioports\n/proc/iomem\n\nDevice location:\n%s\n",d->location); return 0;
}

static int validmod(const char*s)
{
    if(!s||!*s)return 0;
    for(;*s;s++) if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.')) return 0;
    return 1;
}

static int write_blacklist(const char *module,int enable,char *err,size_t n)
{
    if(!validmod(module)){cp(err,n,"No safe loadable driver name is available.");return -1;}
    char in[16384]={0}; readfile("/etc/modprobe.d/devicemanager-disabled.conf",in,sizeof in);
    char target[512]; snprintf(target,sizeof target,"blacklist %s",module);
    char tmp[]="/tmp/w2k-modprobe-XXXXXX"; int fd=mkstemp(tmp); if(fd<0){cp(err,n,strerror(errno));return -1;}
    FILE*f=fdopen(fd,"w"); if(!f){close(fd);unlink(tmp);cp(err,n,"Could not create temporary configuration.");return -1;}
    int found=0; char *save=NULL;
    for(char*q=strtok_r(in,"\n",&save);q;q=strtok_r(NULL,"\n",&save)){
        if(!strcmp(q,target)){found=1;if(enable)continue;}
        fprintf(f,"%s\n",q);
    }
    if(!enable&&!found)fprintf(f,"%s\n",target);
    if(fclose(f)!=0){unlink(tmp);cp(err,n,"Could not write temporary configuration.");return -1;}
    const char *av[]={"pkexec","install","-m","0644",tmp,"/etc/modprobe.d/devicemanager-disabled.conf",NULL};
    int rc=runv(av,err,n); unlink(tmp); if(rc)return rc;
    const char *mv_enable[]={"pkexec","modprobe",module,NULL};
    const char *mv_disable[]={"pkexec","modprobe","-r",module,NULL};
    if(enable) return runv(mv_enable,err,n);
    /* Removing a module can legitimately fail when it is in use; the blacklist
     * itself is still a successful persistent disable. */
    runv(mv_disable,err,n); return 0;
}

int w2k_device_set_enabled(const W2kDevice*d,int enable,char*err,size_t n)
{
    if(!d)return -1;
    return write_blacklist(d->driver,enable,err,n);
}

int w2k_device_uninstall_dkms(const W2kDevice*d,char*err,size_t n){
    if(!d||!validmod(d->driver)){cp(err,n,"Invalid driver.");return -1;}
    char out[8192]={0}; const char *qv[]={"dkms","status",NULL};
    if(runv(qv,out,sizeof out)!=0 && !out[0]){cp(err,n,"Could not query DKMS status.");return -1;}
    char spec[256]={0}; char *save=NULL;
    for(char *line=strtok_r(out,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){
        if(strncmp(line,d->driver,strlen(d->driver))==0 && (line[strlen(d->driver)]=='/' || line[strlen(d->driver)]==',')){
            snprintf(spec,sizeof spec,"%s",line); char *comma=strchr(spec,','); char *slash=strchr(spec,'/');
            char *end=slash&&(!comma||slash<comma)?slash:comma; if(end)*end=0; break;
        }
    }
    if(!spec[0]){cp(err,n,"DKMS module/version was not found.");return -1;}
    const char *av[]={"pkexec","dkms","remove",spec,"--all",NULL}; return runv(av,err,n);
}
int w2k_device_install_dkms(const char*path,char*err,size_t n){if(!path||path[0]!='/'){cp(err,n,"Driver source must be an absolute path.");return -1;}char real[PATH_MAX];if(!realpath(path,real)){cp(err,n,strerror(errno));return -1;}struct stat st;if(stat(real,&st)||!S_ISDIR(st.st_mode)){cp(err,n,"Driver source is not a directory.");return -1;}char conf[PATH_MAX];snprintf(conf,sizeof conf,"%s/dkms.conf",real);if(access(conf,R_OK)){cp(err,n,"Selected directory has no dkms.conf.");return -1;}const char *av[]={"pkexec","dkms","install",real,NULL};return runv(av,err,n);}
static int launchv(char *const av[]){ pid_t pid=fork(); if(pid<0)return -1; if(pid==0){setsid(); execvp(av[0],av); _exit(127);} return 0; }
int w2k_device_update_driver(const W2kDevice*d,char*err,size_t n){(void)d; const char *names[]={"plasma-discover","gnome-software",NULL}; for(int i=0;names[i];i++){char *path=getenv("PATH"); if(!path) path="/usr/bin:/bin"; char *copy=strdup(path); if(!copy) continue; char *save=NULL; for(char*q=strtok_r(copy,":",&save);q;q=strtok_r(NULL,":",&save)){char tmp[PATH_MAX];snprintf(tmp,sizeof tmp,"%s/%s",q,names[i]);if(access(tmp,X_OK)==0){free(copy);if(!strcmp(names[i],"plasma-discover")){char*av[]={(char*)names[i],"--mode","Update",NULL};return launchv(av);}char*av[]={(char*)names[i],NULL};return launchv(av);}}free(copy);}cp(err,n,"No graphical driver/software updater was found.");return -1;}

static int uevent_fd=-1;
int w2k_device_monitor_open(void){
    if(uevent_fd>=0)return uevent_fd;
    uevent_fd=socket(AF_NETLINK,SOCK_DGRAM,NETLINK_KOBJECT_UEVENT);
    if(uevent_fd<0)return -1;
    int rcvbuf=256*1024; setsockopt(uevent_fd,SOL_SOCKET,SO_RCVBUF,&rcvbuf,sizeof rcvbuf);
    struct sockaddr_nl a; memset(&a,0,sizeof a); a.nl_family=AF_NETLINK; a.nl_pid=getpid(); a.nl_groups=1;
    if(bind(uevent_fd,(struct sockaddr*)&a,sizeof a)<0){close(uevent_fd);uevent_fd=-1;return -1;}
    fcntl(uevent_fd,F_SETFL,fcntl(uevent_fd,F_GETFL,0)|O_NONBLOCK); return uevent_fd;
}
int w2k_device_monitor_poll(void){
    if(uevent_fd<0 && w2k_device_monitor_open()<0)return 0;
    int changed=0; char b[4096];
    for(;;){ssize_t n=recv(uevent_fd,b,sizeof b,MSG_DONTWAIT);if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK)break;break;}if(n>0)changed=1;}
    return changed;
}
void w2k_device_monitor_close(void){if(uevent_fd>=0){close(uevent_fd);uevent_fd=-1;}}
