#ifndef SOS2_MACHINE_H
#define SOS2_MACHINE_H
#include "fdt.h"

/* The machine's description: on ARM its interrupt controller's route for the
 * scheduler's timer (arch/arm/machine.c), on x64 the local APIC's
 * (arch/x64/machine.c); and the MMIO the kernel maps. */
#ifdef __x86_64__
struct timer_route {
    uintptr_t lapic; /* xAPIC MMIO; 0 with x2APIC's MSRs */
    bool x2apic;
};
#else
struct timer_route {
    uintptr_t distributor, cpu;
    unsigned interrupt;
    bool qcom;
#ifdef SOS2_BOARD_GTA7LITE
    /* GICv3 in place of GICv2's CPU interface (cpu is 0): the CPU interface is
     * in system registers, and each CPU's PPIs in its frame among the
     * redistributors, which take `redistributor_size` bytes. */
    bool gicv3;
    uintptr_t redistributor;
    uint32_t redistributor_size;
#endif
};
#endif

struct machine_mapping {
    uintptr_t start;
    uint32_t size;
#ifdef __x86_64__
    bool write_combining; /* a framebuffer's; else uncached */
#endif
};

struct machine {
    struct timer_route timer;
    uintptr_t rtc;
    struct machine_mapping devices[MAX_VIRTIO + 32];
    unsigned device_count;
};

/* Board description only: no MMIO reads or writes. */
bool machine_init(const struct platform_devices *devices);
#ifdef __x86_64__
/* The PVH start info QEMU's loader hands over, or a PC's boot information
 * from SOS2's UEFI loader, in place of a device tree: memory, the initrd,
 * the command line, ACPI's CPUs and a PC's framebuffer. */
bool machine_discover(const void *start_info, struct platform_devices *devices);
/* Restarts through ACPI's reset register, else the keyboard controller,
 * else a triple fault. */
_Noreturn void machine_restart(void);
/* A device's registers, reached before vm_init where the boot map does not
 * reach (a PC's 64-bit PCI windows): mapped uncached, identity, in boot.S's
 * tables (arch/x64/mmu.c). */
bool machine_early_map(uint64_t start, uint64_t size);
#endif
const struct machine *machine_get(void);

/* Stable identifiers used in desktop calibration records. PMIC seconds have
 * no assumed epoch. Reads never write the hardware counter. */
enum rtc_kind { RTC_UNIX = 1, RTC_PMIC = 2 };
enum rtc_kind rtc_kind(void);
bool rtc_read(uint32_t *seconds);

bool timer_init(const struct timer_route *route);
bool timer_secondary_init(void);
const char *timer_error(void);
void timer_arm_us(uint32_t microseconds);
void timer_stop(bool acknowledge);
/* Standby only: CPU 0 sleeps for up to `microseconds` (1 s at most). */
void cpu_sleep_us(uint32_t microseconds);
#endif
