#ifndef SOS2_FDT_H
#define SOS2_FDT_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"
#define MAX_VIRTIO           32
#define MAX_RAM_REGIONS      16
#define MAX_RESERVED_REGIONS 64
#define MAX_BOOT_CPUS        4

enum cpu_boot_method { CPU_BOOT_NONE, CPU_BOOT_SMC, CPU_BOOT_HVC };

struct memory_region {
    uint64_t start, size;
};

struct platform_devices {
    uintptr_t fwcfg, virtio[MAX_VIRTIO];
    size_t virtio_count;
    uint64_t ram_start, ram_size;
    uint32_t dtb_size;
    uint64_t initrd_start, initrd_end;
    bool has_initrd_start, has_initrd_end;
    char model[128], root_compatible[128];
    char bootargs[256];
    bool redmi2, redmi2_wt88047;
    struct memory_region ram_regions[MAX_RAM_REGIONS];
    size_t ram_region_count;
    struct memory_region reserved_regions[MAX_RESERVED_REGIONS];
    size_t reserved_region_count;
    struct framebuffer framebuffer;
    uint64_t framebuffer_size;
    bool has_framebuffer;
    uint32_t cpu_mpidr[MAX_BOOT_CPUS];
    unsigned cpu_count;
    enum cpu_boot_method psci_method;
    uint32_t psci_cpu_on;
    /* A PL061 GPIO controller and a gpio-keys power key (Linux KEY_POWER) on
     * one of its pins, such as QEMU virt's power button. */
    uintptr_t gpio;
    uint32_t gpio_phandle, power_key_phandle, power_key_pin;
    bool power_key;
};

bool fdt_discover(const void *blob, struct platform_devices *devices);

/* True when [start, start+size) lies inside one reported RAM region. Firmware may
 * list the regions in any order, so every one is checked. */
static inline bool fdt_range_in_ram(const struct platform_devices *d, uint64_t start,
                                    uint64_t size) {
    if (!size || start > UINT64_MAX - size) return false;
    for (size_t i = 0; i < d->ram_region_count; ++i) {
        const struct memory_region *r = &d->ram_regions[i];
        if (r->start <= start && r->size <= UINT64_MAX - r->start &&
            start + size <= r->start + r->size)
            return true;
    }
    return false;
}
#endif
