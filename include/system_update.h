#ifndef SOS2_SYSTEM_UPDATE_H
#define SOS2_SYSTEM_UPDATE_H
#include "fdt.h"
#include "vm.h"

/* SYS_UPDATE (include/user_abi.h), the system updater's service. A phone
 * started from its installed system (include/system_store.h) installs a
 * downloaded sos2-redmi2.update into the cache slot its boot image does not
 * use, then that boot image, and apps into its app store
 * (include/app_store.h); QEMU and a RAM session only check files. Restart
 * works everywhere. */
void system_update_init(const struct platform_devices *);
/* USER_UPDATE_INFO and USER_UPDATE_APPS, answered on the syscall path without
 * I/O. */
int32_t system_update_info(struct vm_space *, uint32_t address, uint32_t size);
int32_t system_update_apps(struct vm_space *, uint32_t address, uint32_t size);
/* The other operations, on the service worker with the caller frozen. */
int32_t system_update_call(uint32_t pid, struct vm_space *, uint32_t op, uint32_t address,
                           uint32_t size);
/* A process ended: an update it was still receiving is abandoned. */
void system_update_release(uint32_t pid);

#ifdef SOS2_BOARD_REDMI2
/* Start from the installed system: when the boot arguments name compressed
 * resources with sos2.system=<SHA-256>, read the cache slot holding them to
 * BOOT_RAMDISK_BASE and expand them as a ramdisk. False when the boot
 * arguments name none; a phone that cannot start its installed system stops
 * with a message saying what to flash. */
bool system_boot_load(struct platform_devices *, uintptr_t dtb, uintptr_t kernel_start,
                      uintptr_t kernel_end);
#endif
#endif
