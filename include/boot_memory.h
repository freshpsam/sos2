#ifndef SOS2_BOOT_MEMORY_H
#define SOS2_BOOT_MEMORY_H
#include "fdt.h"
/* Validate before reading a resource header or zeroing an allocation arena. */
bool boot_memory_available(const struct platform_devices *, uint64_t start, uint64_t size,
                           uintptr_t dtb, uintptr_t kernel_start, uintptr_t kernel_end,
                           bool avoid_initrd);
#ifdef SOS2_PHONE
/* In the phones' arena (include/board.h): the browser's, of 256 MiB to the
 * page pool's size, and the expanded resources', of size bytes. */
bool phone_browser_arena(const struct platform_devices *, uintptr_t dtb, uintptr_t kernel_start,
                         uintptr_t kernel_end, uintptr_t *start, size_t *size);
bool phone_resource_arena(const struct platform_devices *, uintptr_t dtb, uintptr_t kernel_start,
                          uintptr_t kernel_end, size_t size, uintptr_t *start);
#endif
#ifdef __aarch64__
/* arm64's page pool: up to BOOT_POOL_REGIONS free stretches of RAM below
 * 4 GiB, the largest of at least 1 MiB, in address order and within `span`
 * bytes from the first's start to the last's end; clear of the resources as
 * well (QEMU's loader may place them outside any initrd). How many, or 0. */
#define BOOT_POOL_REGIONS 16
unsigned boot_memory_pool(const struct platform_devices *, uintptr_t dtb, uintptr_t kernel_start,
                          uintptr_t kernel_end, uintptr_t resources, size_t resource_size,
                          uint64_t span, struct memory_region regions[BOOT_POOL_REGIONS]);
/* Where the resources a SORZ ramdisk expands to go: the start of the smallest
 * free stretch that holds them, clear of the ramdisk, so the largest stays
 * whole for the page pool whenever another fits. */
bool boot_memory_resources(const struct platform_devices *, uintptr_t dtb, uintptr_t kernel_start,
                           uintptr_t kernel_end, size_t size, uintptr_t *start);
#endif
#endif
