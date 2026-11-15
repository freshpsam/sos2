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
#endif
