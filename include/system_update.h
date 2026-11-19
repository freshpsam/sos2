#ifndef SOS2_SYSTEM_UPDATE_H
#define SOS2_SYSTEM_UPDATE_H
#include "fdt.h"
#include "vm.h"

/* SYS_UPDATE (include/user_abi.h), the system updater's service. A device
 * started from its installed system (include/system_store.h) installs a
 * downloaded update (sos2-redmi2.update or sos2-gta7lite.update), a whole
 * boot image, into its boot partition, and apps into its app store
 * (include/app_store.h); QEMU and a RAM session only check files. Restart
 * works everywhere. */
/* At boot, after the resources and before the app store: an installed system
 * (sos2.storage=installed) opens its eMMC regions. */
void system_update_init(const struct platform_devices *);
/* USER_UPDATE_INFO and USER_UPDATE_APPS, answered on the syscall path without
 * I/O. */
int32_t system_update_info(struct vm_space *, user_ptr address, uint32_t size);
int32_t system_update_apps(struct vm_space *, user_ptr address, uint32_t size);
/* The other operations, on the service worker with the caller frozen. */
int32_t system_update_call(uint32_t pid, struct vm_space *, uint32_t op, user_ptr address,
                           uint32_t size);
/* A process ended: an update it was still receiving is abandoned, one
 * already writing the boot image finished. */
void system_update_release(uint32_t pid);
/* For the tablet's USB link (boards/gta7lite/fastboot.c), on CPU 0: whether
 * a boot image is being written, during which nothing restarts; a restart,
 * into the tablet's download mode if asked, which returns only when it did
 * not happen; and the running system's release name ("" without one). */
bool system_update_busy(void);
int32_t system_update_restart(bool download_mode);
const char *system_update_running_release(void);
#ifdef SOS2_BOARD_GTA7LITE
/* fastboot's download and flash of the installed boot image, in place of a
 * downloaded update. Begin reserves RAM for an image of `size` bytes;
 * receive fills it from read, a piece at a time; install checks it is the
 * tablet's installed image (tools/package_gta7lite.py) and writes it as an
 * update is written, the first sector last, reporting progress as it goes.
 * Each returns NULL or why it failed; the eMMC must be free of the service
 * worker's jobs while install runs. */
/* The largest image begin takes and the boot partition's size: 0 when this
 * is not the installed system. */
uint32_t system_update_boot_capacity(void);
uint32_t system_update_boot_partition(void);
const char *system_update_boot_begin(uint32_t size);
const char *system_update_boot_receive(bool (*read)(void *data, uint32_t size));
const char *system_update_boot_install(void (*progress)(uint32_t done, uint32_t total,
                                                        bool checking));
#endif

#endif
