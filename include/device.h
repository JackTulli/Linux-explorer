#ifndef W2K_DEVICE_H
#define W2K_DEVICE_H

#include <stddef.h>

#define W2K_DEV_MAX_CATS 64
#define W2K_DEV_MAX_DEV  4096
#define W2K_DEV_STR      256

typedef struct {
    char name[W2K_DEV_STR];
    char description[W2K_DEV_STR];
    char status[W2K_DEV_STR];
    char manufacturer[W2K_DEV_STR];
    char driver[W2K_DEV_STR];
    char driver_version[W2K_DEV_STR];
    char driver_date[W2K_DEV_STR];
    char driver_author[W2K_DEV_STR];
    char location[W2K_DEV_STR];
    char address[W2K_DEV_STR];
    char raw_location[W2K_DEV_STR];
    char sysfs_path[512];
    char subsystem[W2K_DEV_STR];
    char vendor_id[64];
    char device_id[64];
    char modalias[W2K_DEV_STR];
    char icon[64];
    char bt_address[64];
    int disabled;
    int is_dkms;
    int no_driver_needed;
    int airpods_battery;
} W2kDevice;

typedef struct {
    char name[W2K_DEV_STR];
    char icon[64];
    W2kDevice *devices;
    size_t count;
} W2kDeviceCategory;

typedef struct {
    W2kDeviceCategory cats[W2K_DEV_MAX_CATS];
    size_t count;
    char host[W2K_DEV_STR];
} W2kDeviceSet;

void w2k_devices_init(W2kDeviceSet *set);
void w2k_devices_free(W2kDeviceSet *set);
int  w2k_devices_scan(W2kDeviceSet *set);

/* Read all currently available modinfo fields into a text buffer. */
int w2k_device_modinfo(const char *module, char *out, size_t outsz);
/* Human-readable resource information from the kernel's procfs tables. */
int w2k_device_resources(const W2kDevice *d, char *out, size_t outsz);
/* Best-effort privileged driver state changes. Returns 0 on success. */
int w2k_device_set_enabled(const W2kDevice *d, int enable, char *err, size_t errsz);
int w2k_device_uninstall_dkms(const W2kDevice *d, char *err, size_t errsz);
/* Install a DKMS source directory after validating it contains dkms.conf. */
int w2k_device_install_dkms(const char *path, char *err, size_t errsz);
/* Open the system's software/driver updater, if one is available. */
int w2k_device_update_driver(const W2kDevice *d, char *err, size_t errsz);
int  w2k_device_monitor_open(void);
int  w2k_device_monitor_poll(void);
void w2k_device_monitor_close(void);

#endif
