#ifndef SOS2_BOOT_MEMORY_H
#define SOS2_BOOT_MEMORY_H
#include "fdt.h"
/* Validate before reading a resource header or zeroing an allocation arena. */
bool boot_memory_available(const struct platform_devices *, uint64_t start, uint64_t size,
                           uintptr_t dtb, uintptr_t kernel_start, uintptr_t kernel_end,
                           bool avoid_initrd);
bool redmi2_browser_arena(const struct platform_devices *, uintptr_t dtb, uintptr_t kernel_start,
                          uintptr_t kernel_end, uintptr_t *start, size_t *size);
bool redmi2_resource_arena(const struct platform_devices *, uintptr_t dtb, uintptr_t kernel_start,
                           uintptr_t kernel_end, size_t size, uintptr_t *start);
#ifdef __aarch64__
/* arm64's page pool: the largest free stretch of RAM below 4 GiB, at most
 * `maximum` bytes, clear of the resources as well (QEMU's loader may place
 * them outside any initrd). */
bool boot_memory_pool(const struct platform_devices *, uintptr_t dtb, uintptr_t kernel_start,
                      uintptr_t kernel_end, uintptr_t resources, size_t resource_size,
                      uint64_t maximum, uintptr_t *start, size_t *size);
/* Where the resources a SORZ ramdisk expands to go: the start of the smallest
 * free stretch that holds them, clear of the ramdisk, so the largest stays
 * whole for the page pool whenever another fits. */
bool boot_memory_resources(const struct platform_devices *, uintptr_t dtb, uintptr_t kernel_start,
                           uintptr_t kernel_end, size_t size, uintptr_t *start);
#endif
#endif
