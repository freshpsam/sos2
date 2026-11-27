#ifndef SOS2_FDT_H
#define SOS2_FDT_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"
#include "arch/smp.h"
#define MAX_VIRTIO           32
#define MAX_RAM_REGIONS      16
#define MAX_RESERVED_REGIONS 64
#define MAX_BOOT_CPUS        ARCH_CPU_MAX

enum cpu_boot_method { CPU_BOOT_NONE, CPU_BOOT_SMC, CPU_BOOT_HVC };

struct memory_region {
    uint64_t start, size;
};

struct platform_devices {
    uintptr_t fwcfg, virtio[MAX_VIRTIO];
    size_t virtio_count;
    uint32_t dtb_size;
    uint64_t initrd_start, initrd_end;
    bool has_initrd_start, has_initrd_end;
    char bootargs[256]; /* the sos2.* arguments alone (kernel/fdt.c) */
    bool redmi2_wt88047;
    /* The Nexus 5's own tree (boards/nexus5/boot.dts), which its stock
     * bootloader completed. */
    bool nexus5_hammerhead;
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
#ifdef SOS2_BOARD_GTA7LITE
    /* The Galaxy Tab A7 Lite: its MT8768's arm,gic-v3 interrupt controller. */
    bool gicv3;
    /* MediaTek LK's framebuffer handoff (/chosen atag,videolfb): its base and
     * size, and the panel driver LK found. */
    uint64_t videolfb_base;
    uint32_t videolfb_vram;
    char lcm_name[64];
    bool has_videolfb;
    /* MediaTek's boot-params buffer in SRAM (/chosen ram_console), which keeps
     * the reboot reason across a restart (boards/gta7lite/board.c): taken only
     * as a small SRAM buffer. */
    uint32_t ram_console_start, ram_console_size;
    bool has_ram_console;
#endif
#ifdef __x86_64__
    /* A PC: the GPT GUID of the disk SOS2's UEFI loader started it from
     * (include/boot/sos2_boot_info.h), where its system and data are. */
    uint8_t boot_disk_guid[16];
    bool has_boot_disk;
    /* PCI Express configuration space (ACPI's MCFG) for segment 0: buses
     * ecam_first_bus to ecam_last_bus, 1 MiB each from ecam. */
    uint64_t ecam;
    uint8_t ecam_first_bus, ecam_last_bus;
    /* The MMIO a PC's drivers use (boards/pc/pci.c), which the kernel maps
     * uncached beside its own devices (arch/x64/machine.c). */
    struct memory_region mmio[24];
    size_t mmio_count;
    /* ACPI's fixed hardware (the FADT): the PM1 event and control blocks
     * (I/O ports), whether the power button is a fixed one, and the SMI
     * command that turns ACPI mode on. */
    uint16_t pm1a_event, pm1b_event, pm1a_control;
    uint8_t pm1_event_length, acpi_enable;
    uint16_t smi_command;
    bool fixed_power_button;
#endif
};

/* Fills devices from a flattened device tree. */
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
