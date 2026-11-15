#include "arm_machine.h"

/* ARM GICv2 Non-secure interface and architectural virtual timer. The phone's
 * qcom,msm-qgic2 route is selected by machine.c, not inferred from QEMU addresses.
 * Match the Linux qgic2 binding rather than requiring ARM's optional peripheral
 * identity layout. In both cases a bounded live timer-route probe must pass
 * before any user IRQ is unmasked. Only standby sleeps (arm_cpu_sleep_us),
 * powering CPU 0 down on the phone.
 * https://documentation-service.arm.com/static/5f8ff21df86e16515cdbfafe
 * https://github.com/torvalds/linux/blob/master/drivers/irqchip/irq-gic.c
 */
#include "platform.h"
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_cpu_power.h"
#endif
#define read32  mmio_read32
#define write32 mmio_write32
#define sync    barrier

static uint32_t frequency(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(v));
    return v;
}

static uint64_t count(void) {
    uint32_t lo, hi;
    __asm__ volatile("isb\n mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t compare(void) {
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 3, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void set_compare(uint64_t v) {
    __asm__ volatile("mcrr p15, 3, %0, %1, c14\n isb"
                     :
                     : "r"((uint32_t)v), "r"((uint32_t)(v >> 32))
                     : "memory");
}

static uint32_t control(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c14, c3, 1" : "=r"(v));
    return v;
}

static void set_control(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c14, c3, 1\n isb" : : "r"(v) : "memory");
}

static uint32_t masks(void) {
    uint32_t v;
    __asm__ volatile("mrs %0, cpsr" : "=r"(v));
    return v;
}

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
static struct arm_timer_route route;
static bool ready, changed;
static unsigned banks;
static uint32_t hz, bit, priority_offset, priority_shift, edge;
static const char *error;

static struct {
    uint32_t enabled[32], distributor, cpu, pmr, priority, config, timer;
    uint64_t compare;
} saved;

const char *arm_timer_error(void) { return error; }

static void stop(void) {
    set_control(0);
    sync();
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
    set_compare(saved.compare);
    set_control(saved.timer);
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

bool arm_timer_init(const struct arm_timer_route *configuration) {
    if (ready)
        return configuration && configuration->distributor == route.distributor &&
               configuration->cpu == route.cpu && configuration->interrupt == route.interrupt &&
               configuration->qcom == route.qcom;
    error = 0;
    changed = false;
    if (!configuration || !configuration->distributor || !configuration->cpu ||
        ((configuration->distributor | configuration->cpu) & 4095) ||
        configuration->interrupt < 16 || configuration->interrupt > 31)
        return fail("invalid interrupt route");
    route = *configuration;
    if ((masks() & 0xdf) != 0xd3) return fail("requires SVC with IRQ/FIQ masked");
    hz = frequency();
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
    saved.timer = control() & 3;
    saved.compare = compare();
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
    uint64_t start = count();
    set_compare(start + ticks(1000));
    set_control(1);
    sync();
    bool routed = false;
    for (unsigned n = 0; n < 200000; ++n) {
        if ((control() & 4) && (read32(route.cpu + C_HPPIR) & 1023) == route.interrupt) {
            routed = true;
            break;
        }
        if (count() - start > ticks(20000)) break;
    }
    stop();
    set_compare(saved.compare);
    if (!routed) return fail("virtual timer did not reach CPU interrupt interface");
    if (read32(route.distributor + D_PENDING) & bit)
        return fail("timer pending state did not clear");
    if ((read32(route.cpu + C_HPPIR) & 1023) != 1023) return fail("unexpected inherited interrupt");
    ready = true;
    changed = false;
    return true;
}

bool arm_timer_secondary_init(void) {
    /* GICv2 SGIs/PPIs and the CPU interface are banked. Never reinitialize the
     * distributor or touch an SPI here: CPU 0 already owns those registers. */
    if (!ready || frequency() != hz || (masks() & 0xdf) != 0xd3 || (control() & 1) ||
        read32(route.distributor + D_ACTIVE) || (read32(route.distributor + D_PENDING) & bit))
        return false;
    uint32_t enabled = read32(route.distributor + D_ENABLE),
             config = read32(route.distributor + D_CONFIG);
    uint32_t priority = read32(route.distributor + priority_offset),
             cpu = read32(route.cpu + C_CTLR), pmr = read32(route.cpu + C_PMR);
    uint32_t running = read32(route.cpu + C_RPR) & 255;
    uint64_t previous_compare = compare();
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
        uint64_t start = count();
        set_compare(start + ticks(1000));
        set_control(1);
        sync();
        for (unsigned n = 0; n < 200000; ++n) {
            if ((control() & 4) && (read32(route.cpu + C_HPPIR) & 1023) == route.interrupt) {
                routed = true;
                break;
            }
            if (count() - start > ticks(20000)) break;
        }
    }
    stop();
    set_compare(previous_compare);
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

void arm_timer_arm_us(uint32_t microseconds) {
    if (!ready) panic("scheduler timer not initialized");
    if (!microseconds || microseconds > 5000) microseconds = 5000;
    set_control(0);
    set_compare(count() + ticks(microseconds));
    set_control(1);
}

void arm_timer_stop(bool acknowledge) {
    if (acknowledge) {
        uint32_t id = read32(route.cpu + C_IAR);
        set_control(0);
        sync();
        if ((id & 1023) != route.interrupt) panic("unexpected scheduler interrupt");
        write32(route.cpu + C_EOIR, id);
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
void arm_cpu_sleep_us(uint32_t microseconds) {
    if (!ready || !microseconds) return;
    if (microseconds > 1000000) microseconds = 1000000;
    uint64_t deadline = count() + ticks(microseconds);
#ifdef SOS2_BOARD_REDMI2
    if (redmi2_cpu0_sleep(microseconds)) return;
    uint32_t saved_kctl;
    __asm__ volatile("mrc p15, 0, %0, c14, c1, 0" : "=r"(saved_kctl));
    uint32_t stream = (saved_kctl & ~0xfcu) | (15u << 4) | (1u << 2);
    __asm__ volatile("mcr p15, 0, %0, c14, c1, 0\n isb" : : "r"(stream) : "memory");
    /* The power key's line rising ends the wait, as it ends a power-down;
     * one already up is left to the caller's polls. */
    bool watch = !redmi2_cpu0_source_raised();
    while ((int64_t)(count() - deadline) < 0 && !(watch && redmi2_cpu0_source_raised()))
        __asm__ volatile("wfe" ::: "memory");
    __asm__ volatile("mcr p15, 0, %0, c14, c1, 0\n isb" : : "r"(saved_kctl) : "memory");
#else
    set_control(0);
    set_compare(deadline);
    set_control(1);
    sync();
    __asm__ volatile("dsb sy\n wfi" ::: "memory");
    stop();
#endif
}
