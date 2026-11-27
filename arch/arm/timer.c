#include "machine.h"

/* ARM GICv2 Non-secure interface and architectural virtual timer. The phone's
 * qcom,msm-qgic2 route is selected by machine.c, not inferred from QEMU addresses.
 * Match the Linux qgic2 binding rather than requiring ARM's optional peripheral
 * identity layout. In both cases a bounded live timer-route probe must pass
 * before any user IRQ is unmasked. Only standby sleeps (cpu_sleep_us),
 * powering CPU 0 down on the phone.
 * https://documentation-service.arm.com/static/5f8ff21df86e16515cdbfafe
 * https://github.com/torvalds/linux/blob/master/drivers/irqchip/irq-gic.c
 */
#include "platform.h"
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_cpu_power.h"
#endif
#ifdef SOS2_BOARD_GTA7LITE
#include "smp.h"
#endif
#define read32  mmio_read32
#define write32 mmio_write32
#define sync    barrier

enum {
    D_CTLR = 0,
    D_TYPER = 4,
    D_ENABLE = 0x100,
    D_DISABLE = 0x180,
    D_PENDING = 0x200,
    D_CLEAR = 0x280,
    D_ACTIVE = 0x300,
    D_PRIORITY = 0x400,
    D_CONFIG = 0xc04,
    D_PIDR2 = 0xfe8,
    C_CTLR = 0,
    C_PMR = 4,
    C_IAR = 0xc,
    C_EOIR = 0x10,
    C_RPR = 0x14,
    C_HPPIR = 0x18
};
static struct timer_route route;
static bool ready, changed;
static unsigned banks;
static uint32_t hz, bit, priority_offset, priority_shift, edge;
static const char *error;
#ifdef SOS2_BOARD_GTA7LITE
static uintptr_t gicv3_frames[CPU_MAX];
static uintptr_t gicv3_local(void);
#endif

static struct {
    uint32_t enabled[32], distributor, cpu, pmr, priority, config, timer;
    uint64_t compare;
} saved;

const char *timer_error(void) { return error; }

static void stop(void) {
    arch_timer_set_control(0);
    sync();
#ifdef SOS2_BOARD_GTA7LITE
    if (route.gicv3) {
        write32(gicv3_local() + 0x280, bit); /* GICR_ICPENDR0 */
        sync();
        return;
    }
#endif
    write32(route.distributor + D_CLEAR, bit);
    sync();
}

static void restore(void) {
    if (!changed) return;
    stop();
    write32(route.distributor + priority_offset, saved.priority);
    write32(route.distributor + D_CONFIG, saved.config);
    for (unsigned i = 0; i < banks; ++i) {
        write32(route.distributor + D_DISABLE + 4 * i, i ? UINT32_MAX : 0xffff0000u);
        write32(route.distributor + D_ENABLE + 4 * i, saved.enabled[i]);
    }
    write32(route.cpu + C_PMR, saved.pmr);
    write32(route.cpu + C_CTLR, saved.cpu);
    write32(route.distributor + D_CTLR, saved.distributor);
    arch_timer_set_compare(saved.compare);
    arch_timer_set_control(saved.timer);
    sync();
    changed = false;
}

static bool fail(const char *reason) {
    error = reason;
    ready = false;
    restore();
    return false;
}

static bool fail_value(const char *reason, uint32_t value) {
    /* The detail shows on the panic screen. */
    static char detail[80];
    unsigned n = 0;
    while (*reason && n < sizeof(detail) - 12) detail[n++] = *reason++;
    detail[n++] = ' ';
    detail[n++] = '0';
    detail[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
        detail[n++] = "0123456789abcdef"[(value >> shift) & 15];
    detail[n] = 0;
    return fail(detail);
}

static uint64_t ticks(uint32_t us) { return ((uint64_t)hz * us + 999999) / 1000000; }

#ifdef SOS2_BOARD_GTA7LITE
/* GICv3 (the tablet's MT8768): the CPU interface is in system registers and
 * each CPU's SGIs and PPIs are in its own redistributor, found by affinity.
 * Firmware keeps Secure configuration; SOS2 needs affinity routing and the
 * timer PPI in Non-secure Group 1. With two security states the Non-secure
 * GICD_CTLR names bits 1 and 4 EnableGrp1A and ARE_NS; with one, EnableGrp1
 * and ARE.
 * https://developer.arm.com/documentation/ihi0069/latest
 * https://github.com/torvalds/linux/blob/master/drivers/irqchip/irq-gic-v3.c */
enum {
    D3_PIDR2 = 0xffe8,
    R_CTLR = 0,
    R_TYPER = 8,
    R_WAKER = 0x14,
    R_SGI = 0x10000,
    S_IGROUPR0 = 0x80,
    S_ISENABLER0 = 0x100,
    S_ICENABLER0 = 0x180,
    S_ISPENDR0 = 0x200,
    S_ICPENDR0 = 0x280,
    S_ISACTIVER0 = 0x300,
    S_IPRIORITYR = 0x400,
    S_ICFGR1 = 0xc04
};

#define icc_read(name) \
    ({ \
        uint64_t value; \
        __asm__ volatile("mrs %0, " #name : "=r"(value)); \
        value; \
    })
#define icc_write(name, value) \
    __asm__ volatile("msr " #name ", %0" : : "r"((uint64_t)(value)) : "memory")

static uintptr_t gicv3_local(void) { return gicv3_frames[arch_cpu_local()->index]; }

static bool gicv3_settled(uintptr_t address, uint32_t busy) {
    for (unsigned n = 0; n < 1000000; ++n)
        if (!(read32(address) & busy)) return true;
    return false;
}

/* This CPU's SGI frame: wake its redistributor and route the timer PPI to the
 * system-register CPU interface. */
static uintptr_t gicv3_cpu(void) {
    unsigned index = arch_cpu_local()->index;
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t affinity = (uint32_t)((mpidr >> 32 & 255) << 24 | (mpidr & 0xffffff));
    uintptr_t frame = 0;
    for (uintptr_t at = route.redistributor;
         at - route.redistributor <= route.redistributor_size - 0x20000;) {
        uint32_t typer = read32(at + R_TYPER);
        if (read32(at + R_TYPER + 4) == affinity) {
            frame = at;
            break;
        }
        if (typer & (1u << 4)) break;                /* Last */
        at += typer & (1u << 1) ? 0x40000 : 0x20000; /* GICv4 adds two frames */
    }
    if (!frame || index >= CPU_MAX) return 0;
    write32(frame + R_WAKER, read32(frame + R_WAKER) & ~2u); /* ProcessorSleep */
    if (!gicv3_settled(frame + R_WAKER, 4)) return 0;        /* ChildrenAsleep */
    icc_write(icc_sre_el1, icc_read(icc_sre_el1) | 1);
    __asm__ volatile("isb");
    if (!(icc_read(icc_sre_el1) & 1)) return 0;
    uintptr_t sgi = frame + R_SGI;
    if (read32(sgi + S_ISACTIVER0) & bit) return 0;
    /* Inherited PPIs stay off until an SOS2 driver claims them; SGIs stay. */
    write32(sgi + S_ICENABLER0, 0xffff0000u);
    if (!gicv3_settled(frame + R_CTLR, 1u << 3)) return 0; /* RWP */
    /* Group 1. With two security states (the tablet's firmware) this register
     * is Secure, reads as zero and ignores the write, and the firmware has
     * already made the PPIs Non-secure Group 1. Only such an interrupt can be
     * enabled from here and reach ICC_HPPIR1_EL1 (gicv3_routed). */
    write32(sgi + S_IGROUPR0, read32(sgi + S_IGROUPR0) | bit);
    uint32_t priority = S_IPRIORITYR + (route.interrupt & ~3u);
    write32(sgi + priority,
            (read32(sgi + priority) & ~(255u << priority_shift)) | (0xa0u << priority_shift));
    write32(sgi + S_ICFGR1, read32(sgi + S_ICFGR1) & ~edge);
    write32(sgi + S_ICPENDR0, bit);
    icc_write(icc_pmr_el1, 255);
    icc_write(icc_bpr1_el1, 0);
    icc_write(icc_ctlr_el1, icc_read(icc_ctlr_el1) & ~(uint64_t)2); /* EOImode 0 */
    icc_write(icc_igrpen1_el1, 1);
    __asm__ volatile("isb");
    write32(sgi + S_ISENABLER0, bit);
    sync();
    if (!(read32(sgi + S_ISENABLER0) & bit) || (read32(sgi + S_ICFGR1) & edge)) return 0;
    gicv3_frames[index] = sgi;
    return sgi;
}

/* A bounded live check: this CPU's virtual timer reaches its interface. */
static bool gicv3_routed(uintptr_t sgi) {
    uint64_t previous = arch_timer_compare(), start = arch_counter_synchronized();
    arch_timer_set_compare(start + ticks(1000));
    arch_timer_set_control(1);
    sync();
    bool routed = false;
    for (unsigned n = 0; n < 200000; ++n) {
        if ((arch_timer_control() & 4) &&
            (icc_read(icc_hppir1_el1) & 0xffffff) == route.interrupt) {
            routed = true;
            break;
        }
        if (arch_counter_synchronized() - start > ticks(20000)) break;
    }
    stop();
    arch_timer_set_compare(previous);
    if (!routed) return false;
    /* The redistributor tells the CPU interface asynchronously that the
     * stopped timer is no longer pending. */
    start = arch_counter_synchronized();
    for (unsigned n = 0; n < 200000; ++n) {
        if (!(read32(sgi + S_ISPENDR0) & bit) && (icc_read(icc_hppir1_el1) & 0xffffff) == 1023)
            return true;
        if (arch_counter_synchronized() - start > ticks(20000)) break;
    }
    return false;
}

static bool gicv3_init(const struct timer_route *configuration) {
    if (!configuration->distributor || !configuration->redistributor ||
        ((configuration->distributor | configuration->redistributor) & 0xffff) ||
        configuration->redistributor_size < 0x20000 || configuration->interrupt < 16 ||
        configuration->interrupt > 31)
        return fail("invalid interrupt route");
    route = *configuration;
    if (!arch_kernel_masked()) return fail("requires EL1 with IRQ/FIQ masked");
    hz = arch_counter_frequency();
    if (hz < 1000 || hz > 1000000000) return fail("invalid timer frequency");
    if ((read32(route.distributor + D3_PIDR2) >> 4 & 15) < 3) return fail("GICv3 not identified");
    bit = 1u << route.interrupt;
    priority_shift = (route.interrupt & 3u) * 8;
    edge = 2u << ((route.interrupt - 16) * 2);
    if (arch_timer_control() & 1) return fail("virtual timer already owned");
    /* Affinity routing first: its group enables must be off before changing
     * ARE. Bits 0:1 cover the Non-secure Group 1 alias in legacy mode, or
     * both groups with one security state; Non-secure accesses leave Secure
     * configuration alone. RWP must clear before setting ARE. */
    uint32_t control = read32(route.distributor + D_CTLR);
    if (!(control & 0x10)) {
        write32(route.distributor + D_CTLR, control & ~3u);
        if (!gicv3_settled(route.distributor + D_CTLR, 1u << 31))
            return fail("GICv3 distributor busy");
        control = read32(route.distributor + D_CTLR);
        if (control & 3) return fail("GICv3 refused group disable");
        write32(route.distributor + D_CTLR, control | 0x10);
        if (!gicv3_settled(route.distributor + D_CTLR, 1u << 31))
            return fail("GICv3 distributor busy");
    }
    /* Inherited SPIs stay off until an SOS2 driver claims them, as on GICv2;
     * Non-secure writes leave Secure interrupts alone. Then Group 1. */
    unsigned lines = (read32(route.distributor + D_TYPER) & 31) + 1;
    for (unsigned i = 1; i < lines; ++i) write32(route.distributor + D_DISABLE + 4 * i, UINT32_MAX);
    if (!gicv3_settled(route.distributor + D_CTLR, 1u << 31)) return fail("GICv3 distributor busy");
    write32(route.distributor + D_CTLR, read32(route.distributor + D_CTLR) | 2);
    if (!gicv3_settled(route.distributor + D_CTLR, 1u << 31) ||
        (read32(route.distributor + D_CTLR) & 0x12) != 0x12)
        return fail("GICv3 refused affinity routing or Group 1");
    uintptr_t sgi = gicv3_cpu();
    if (!sgi) return fail("GICv3 redistributor unavailable");
    if (!gicv3_routed(sgi)) return fail("virtual timer did not reach CPU interrupt interface");
    ready = true;
    return true;
}
#endif

bool timer_init(const struct timer_route *configuration) {
    if (ready)
        return configuration && configuration->distributor == route.distributor &&
               configuration->cpu == route.cpu && configuration->interrupt == route.interrupt &&
               configuration->qcom == route.qcom;
    error = 0;
    changed = false;
#ifdef SOS2_BOARD_GTA7LITE
    if (configuration && configuration->gicv3) return gicv3_init(configuration);
#endif
    if (!configuration || !configuration->distributor || !configuration->cpu ||
        ((configuration->distributor | configuration->cpu) & 4095) ||
        configuration->interrupt < 16 || configuration->interrupt > 31)
        return fail("invalid interrupt route");
    route = *configuration;
    if (!arch_kernel_masked()) return fail("requires SVC with IRQ/FIQ masked");
    hz = arch_counter_frequency();
    if (hz < 1000 || hz > 1000000000) return fail("invalid timer frequency");
    if (!route.qcom && (read32(route.distributor + D_PIDR2) & 0xf0) != 0x20)
        return fail("GICv2 not identified");
    uint32_t typer = read32(route.distributor + D_TYPER);
    if (typer == UINT32_MAX) return fail("interrupt controller unavailable");
    banks = (typer & 31) + 1;
    bit = 1u << route.interrupt;
    priority_offset = D_PRIORITY + (route.interrupt & ~3u);
    priority_shift = (route.interrupt & 3u) * 8;
    edge = 2u << ((route.interrupt - 16) * 2);
    saved.timer = arch_timer_control() & 3;
    saved.compare = arch_timer_compare();
    if (saved.timer & 1) return fail("virtual timer already owned");
    if (read32(route.distributor + D_PENDING) & bit)
        return fail_value("inherited pending timer IRQ", route.interrupt);
    uint32_t running = read32(route.cpu + C_RPR) & 255;
    /* A priority-dropped interrupt can still be active in split EOI mode.
     * Refuse that inherited state before changing the CPU's EOI policy. */
    for (unsigned i = 0; i < banks; ++i) {
        uint32_t active = read32(route.distributor + D_ACTIVE + 4 * i);
        if (active) {
            unsigned first = 0;
            while (!(active & (1u << first))) ++first;
            return fail_value("inherited active IRQ", 32 * i + first);
        }
    }
    saved.distributor = read32(route.distributor + D_CTLR);
    saved.cpu = read32(route.cpu + C_CTLR);
    saved.pmr = read32(route.cpu + C_PMR);
    saved.priority = read32(route.distributor + priority_offset);
    saved.config = read32(route.distributor + D_CONFIG);
    for (unsigned i = 0; i < banks; ++i)
        saved.enabled[i] = read32(route.distributor + D_ENABLE + 4 * i);
    changed = true;
    /* The boot CPU owns Non-secure interrupt delivery. Disable inherited
     * device IRQs until an SOS2 driver claims them; Secure Group0 is untouched
     * by these Non-secure bank accesses. SGIs cannot be disabled here. */
    for (unsigned i = 0; i < banks; ++i)
        write32(route.distributor + D_DISABLE + 4 * i, i ? UINT32_MAX : 0xffff0000u);
    /* GICv2's idle RPR can be 0xff OR the largest implemented priority. Its
     * Non-secure view also shifts away the Secure priority bit (IHI 0048B,
     * 4.4.6 and Glossary "Idle priority"). A literal RPR==0xff rejects valid
     * qgic2 handoffs. Measure writable priority bits using only our disabled,
     * inactive PPI; RPR itself cannot tell us the implemented priority width.
     * Neighboring priorities are preserved and failure restores this word. */
    write32(route.distributor + priority_offset, saved.priority | (255u << priority_shift));
    sync();
    uint32_t priority_mask = (read32(route.distributor + priority_offset) >> priority_shift) & 255;
    uint32_t absent = priority_mask ^ 255;
    /* GICv2 has at least four priority bits, or three in the Non-secure view.
     * Implemented bits must be contiguous from the most significant bit. */
    if ((priority_mask & 0xe0) != 0xe0 || (absent & (absent + 1)))
        return fail_value("invalid GIC priority mask", priority_mask);
    if (running != 255 && running != priority_mask)
        return fail_value("non-idle GIC running priority", running);
    write32(route.distributor + D_CONFIG, saved.config & ~edge);
    write32(route.distributor + priority_offset,
            (saved.priority & ~(255u << priority_shift)) | (0x80u << priority_shift));
    write32(route.cpu + C_PMR, 255);
    /* Combined priority-drop/deactivation, preserving bypass and other bits. */
    write32(route.cpu + C_CTLR, (saved.cpu & ~(1u << 9)) | 1);
    write32(route.distributor + D_CTLR, saved.distributor | 1);
    write32(route.distributor + D_ENABLE, bit);
    sync();
    if (!(read32(route.distributor + D_ENABLE) & bit) ||
        !(read32(route.distributor + D_CTLR) & 1) || !(read32(route.cpu + C_CTLR) & 1) ||
        (read32(route.cpu + C_CTLR) & (1u << 9)) || (read32(route.distributor + D_CONFIG) & edge))
        return fail("interrupt configuration denied");
    uint64_t start = arch_counter_synchronized();
    arch_timer_set_compare(start + ticks(1000));
    arch_timer_set_control(1);
    sync();
    bool routed = false;
    for (unsigned n = 0; n < 200000; ++n) {
        if ((arch_timer_control() & 4) && (read32(route.cpu + C_HPPIR) & 1023) == route.interrupt) {
            routed = true;
            break;
        }
        if (arch_counter_synchronized() - start > ticks(20000)) break;
    }
    stop();
    arch_timer_set_compare(saved.compare);
    if (!routed) return fail("virtual timer did not reach CPU interrupt interface");
    if (read32(route.distributor + D_PENDING) & bit)
        return fail("timer pending state did not clear");
    if ((read32(route.cpu + C_HPPIR) & 1023) != 1023) return fail("unexpected inherited interrupt");
    ready = true;
    changed = false;
    return true;
}

bool timer_secondary_init(void) {
#ifdef SOS2_BOARD_GTA7LITE
    if (route.gicv3) {
        if (!ready || arch_counter_frequency() != hz || !arch_kernel_masked() ||
            (arch_timer_control() & 1))
            return false;
        uintptr_t sgi = gicv3_cpu();
        return sgi && gicv3_routed(sgi);
    }
#endif
    /* GICv2 SGIs/PPIs and the CPU interface are banked. Never reinitialize the
     * distributor or touch an SPI here: CPU 0 already owns those registers. */
    if (!ready || arch_counter_frequency() != hz || !arch_kernel_masked() ||
        (arch_timer_control() & 1) || read32(route.distributor + D_ACTIVE) ||
        (read32(route.distributor + D_PENDING) & bit))
        return false;
    uint32_t enabled = read32(route.distributor + D_ENABLE),
             config = read32(route.distributor + D_CONFIG);
    uint32_t priority = read32(route.distributor + priority_offset),
             cpu = read32(route.cpu + C_CTLR), pmr = read32(route.cpu + C_PMR);
    uint32_t running = read32(route.cpu + C_RPR) & 255;
    uint64_t previous_compare = arch_timer_compare();
    write32(route.distributor + D_DISABLE, 0xffff0000u);
    write32(route.distributor + priority_offset, priority | (255u << priority_shift));
    sync();
    uint32_t mask = (read32(route.distributor + priority_offset) >> priority_shift) & 255,
             absent = mask ^ 255;
    bool valid =
        (mask & 0xe0) == 0xe0 && !(absent & (absent + 1)) && (running == 255 || running == mask);
    if (valid) {
        /* SGI 0 is the legacy Qualcomm cold-boot wakeup. It is not an OS IPI. */
        write32(route.distributor + 0xf10, 255);
        write32(route.distributor + D_CONFIG, config & ~edge);
        write32(route.distributor + priority_offset,
                (priority & ~(255u << priority_shift)) | (0x80u << priority_shift));
        write32(route.cpu + C_PMR, 255);
        write32(route.cpu + C_CTLR, (cpu & ~(1u << 9)) | 1);
        write32(route.distributor + D_ENABLE, bit);
        sync();
        valid = (read32(route.distributor + D_ENABLE) & bit) &&
                (read32(route.distributor + D_CTLR) & 1) && (read32(route.cpu + C_CTLR) & 1) &&
                !(read32(route.cpu + C_CTLR) & (1u << 9)) &&
                !(read32(route.distributor + D_CONFIG) & edge);
    }
    bool routed = false;
    if (valid) {
        uint64_t start = arch_counter_synchronized();
        arch_timer_set_compare(start + ticks(1000));
        arch_timer_set_control(1);
        sync();
        for (unsigned n = 0; n < 200000; ++n) {
            if ((arch_timer_control() & 4) &&
                (read32(route.cpu + C_HPPIR) & 1023) == route.interrupt) {
                routed = true;
                break;
            }
            if (arch_counter_synchronized() - start > ticks(20000)) break;
        }
    }
    stop();
    arch_timer_set_compare(previous_compare);
    if (valid && routed && !(read32(route.distributor + D_PENDING) & bit) &&
        (read32(route.cpu + C_HPPIR) & 1023) == 1023)
        return true;
    write32(route.distributor + D_DISABLE, 0xffff0000u);
    write32(route.distributor + D_ENABLE, enabled);
    write32(route.distributor + D_CONFIG, config);
    write32(route.distributor + priority_offset, priority);
    write32(route.cpu + C_PMR, pmr);
    write32(route.cpu + C_CTLR, cpu);
    sync();
    return false;
}

void timer_arm_us(uint32_t microseconds) {
    if (!ready) panic("scheduler timer not initialized");
    if (!microseconds || microseconds > 5000) microseconds = 5000;
    arch_timer_set_control(0);
    arch_timer_set_compare(arch_counter_synchronized() + ticks(microseconds));
    arch_timer_set_control(1);
}

/* IDs 1020-1023 acknowledge nothing (1023: spurious, the interrupt gone by
 * the time it was read, which the Nexus 5's cores see a few times in 10 s
 * under load; 1022: one this world may not take). They are not ended, and
 * the trap is a preemption point like the timer's. Any other interrupt
 * than the scheduler's is a fault: its ID on the panic screen. */
static bool special(uint32_t id) { return id >= 1020 && id <= 1023; }

static _Noreturn void unexpected(uint32_t id) {
    static char text[48];
    *decimal(stpcpy(text, "unexpected scheduler interrupt "), id & 0x1fff) = 0;
    panic(text);
}

void timer_stop(bool acknowledge) {
#ifdef SOS2_BOARD_GTA7LITE
    if (route.gicv3) {
        if (acknowledge) {
            uint64_t id = icc_read(icc_iar1_el1);
            arch_timer_set_control(0);
            sync();
            if (!special((uint32_t)(id & 0xffffff))) {
                if ((id & 0xffffff) != route.interrupt) unexpected((uint32_t)id);
                icc_write(icc_eoir1_el1, id);
            }
        }
        stop();
        return;
    }
#endif
    if (acknowledge) {
        uint32_t id = read32(route.cpu + C_IAR);
        arch_timer_set_control(0);
        sync();
        if (!special(id & 1023)) {
            if ((id & 1023) != route.interrupt) unexpected(id);
            write32(route.cpu + C_EOIR, id);
        }
    }
    stop();
}

/* Standby on CPU 0, with IRQs masked: sleep until about `microseconds` (at most
 * 1 s) have passed; an early wake only means an earlier poll. QEMU halts the
 * CPU in WFI until the virtual timer's interrupt is pending; it is never taken.
 * On the phone CPU 0 powers down until a memory-mapped timer wakes it
 * (boards/redmi2/cpu_power.c). Where that is not ready it waits in WFE, like
 * idle secondaries, woken by the virtual counter's event stream (bit 15
 * rising: 3.4 ms at 19.2 MHz): WFI could start a power-collapse sequence that
 * an inherited SPM configuration may arm.
 * https://developer.arm.com/documentation/ddi0406/c (B8.1.5 event streams) */
void cpu_sleep_us(uint32_t microseconds) {
    if (!ready || !microseconds) return;
    if (microseconds > 1000000) microseconds = 1000000;
    uint64_t deadline = arch_counter_synchronized() + ticks(microseconds);
#ifdef SOS2_BOARD_REDMI2
    if (redmi2_cpu0_sleep(microseconds)) return;
    uint32_t saved_kctl = arch_timer_kernel_control();
    uint32_t stream = (saved_kctl & ~0xfcu) | (15u << 4) | (1u << 2);
    arch_timer_set_kernel_control(stream);
    /* The power key's line rising ends the wait, as it ends a power-down;
     * one already up is left to the caller's polls. */
    bool watch = !redmi2_cpu0_source_raised();
    while ((int64_t)(arch_counter_synchronized() - deadline) < 0 &&
           !(watch && redmi2_cpu0_source_raised()))
        __asm__ volatile("wfe" ::: "memory");
    arch_timer_set_kernel_control(saved_kctl);
#else
    arch_timer_set_control(0);
    arch_timer_set_compare(deadline);
    arch_timer_set_control(1);
    sync();
    __asm__ volatile("dsb sy\n wfi" ::: "memory");
    stop();
#endif
}
