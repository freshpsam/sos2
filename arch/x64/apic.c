#include "x86.h"
#include "machine.h"
#include "platform.h"

/* The TSC as the kernel's clock and the local APIC's timer as each CPU's
 * scheduler timer: TSC-deadline where CPUID offers it, else one-shot counts
 * calibrated against the TSC (QEMU without KVM). The only interrupts are
 * that timer, the wake an idle CPU halts for and the APIC's spurious one;
 * the 8259 and every IOAPIC input stay masked, since the drivers poll, as on
 * ARM (arch/arm/timer.c). The TSC's frequency comes from the hypervisor's
 * leaf (QEMU's with invtsc), CPUID 0x15, or the PIT.
 * https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html */
enum {
    MSR_APIC_BASE = 0x1b,
    MSR_TSC_DEADLINE = 0x6e0,
    MSR_X2APIC = 0x800,
    APIC_ID = 0x20,
    APIC_TPR = 0x80,
    APIC_EOI = 0xb0,
    APIC_SVR = 0xf0,
    APIC_ICR = 0x300,
    APIC_ICR_HIGH = 0x310,
    APIC_LVT_TIMER = 0x320,
    APIC_LVT_LINT0 = 0x350,
    APIC_LVT_LINT1 = 0x360,
    APIC_LVT_ERROR = 0x370,
    APIC_TIMER_INITIAL = 0x380,
    APIC_TIMER_CURRENT = 0x390,
    APIC_TIMER_DIVIDE = 0x3e0,
    LVT_MASKED = 1u << 16,
    LVT_TSC_DEADLINE = 2u << 17
};

uint32_t x86_counter_frequency;
uint64_t x86_tsc_frequency;
static struct timer_route route;
static bool ready, deadline;
static uint64_t apic_ticks_per_ms; /* one-shot counts, divided by 16 */
static const char *error;

static uint32_t apic_read(unsigned reg) {
    return route.x2apic ? (uint32_t)x86_rdmsr(MSR_X2APIC + reg / 16)
                        : mmio_read32(route.lapic + reg);
}

static void apic_write(unsigned reg, uint32_t value) {
    if (route.x2apic) x86_wrmsr(MSR_X2APIC + reg / 16, value);
    else mmio_write32(route.lapic + reg, value);
}

/* The PIT's channel 0 count, latched. */
static uint16_t pit_count(void) {
    x86_outb(0x43, 0x00);
    uint8_t low = x86_inb(0x40);
    return (uint16_t)(x86_inb(0x40) << 8 | low);
}

/* The TSC's ticks per second against at least 10 ms of the PIT's channel 0
 * (1.193182 MHz), counting down from 0xffff: channel 2 would need the PC
 * speaker's gate (port 0x61), which QEMU's microvm lacks. The least of three
 * tries, since a late read only ever adds ticks. Channel 0's interrupt goes
 * nowhere: the 8259 and the IOAPIC's inputs stay masked. A PIT that does
 * not count (PCs that gate it off) is given up on within a second or so. */
static uint64_t pit_measure(void) {
    enum { SPAN = 11932 };

    uint64_t least = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        x86_outb(0x43, 0x30); /* channel 0, both bytes, mode 0 */
        x86_outb(0x40, 0xff);
        x86_outb(0x40, 0xff);
        unsigned polls = 0;
        uint16_t start, now;
        do { /* until it counts down, within some microseconds */
            start = pit_count();
            if (++polls > 100000) return 0; /* no PIT, or a gated one */
        } while (start > 0xfff0);
        uint64_t begin = x86_rdtsc();
        do {
            now = pit_count();
            if (++polls > 1000000) return 0;
        } while ((uint16_t)(start - now) < SPAN);
        uint64_t hz = (x86_rdtsc() - begin) * 1193182 / (uint16_t)(start - now);
        if (!least || hz < least) least = hz;
    }
    return least;
}

bool x86_tsc_calibrate(void) {
    uint32_t r[4];
    uint64_t hz = 0;
    x86_cpuid(1, 0, r);
    if (r[2] & (1u << 31)) { /* a hypervisor: its timing leaf, in kHz */
        x86_cpuid(0x40000000, 0, r);
        if (r[0] >= 0x40000010 && r[0] < 0x40010000) {
            x86_cpuid(0x40000010, 0, r);
            hz = (uint64_t)r[0] * 1000;
        }
    }
    if (!hz) {
        x86_cpuid(0, 0, r);
        uint32_t highest = r[0];
        if (highest >= 0x15) {
            x86_cpuid(0x15, 0, r);
            if (r[0] && r[1] && r[2]) hz = (uint64_t)r[2] * r[1] / r[0];
        }
        /* Skylake to Comet Lake report no crystal, and many of their PCs
         * gate the PIT off: the base frequency, which their TSC runs at (as
         * Linux takes it). */
        if (!hz && highest >= 0x16) {
            x86_cpuid(0x16, 0, r);
            hz = (uint64_t)(r[0] & 0xffff) * 1000000;
        }
    }
    if (!hz) hz = pit_measure();
    if (hz < 100000000 || hz > 10000000000ull) return false;
    x86_tsc_frequency = hz;
    x86_counter_frequency = (uint32_t)(hz >> ARCH_COUNTER_SHIFT);
    return true;
}

const char *timer_error(void) { return error; }

static bool fail(const char *reason) {
    error = reason;
    return false;
}

static void apic_enable(void) {
    uint64_t base = x86_rdmsr(MSR_APIC_BASE) | (1u << 11);
    if (route.x2apic) base |= 1u << 10;
    x86_wrmsr(MSR_APIC_BASE, base);
}

/* This CPU's APIC: no task priority, the external and error inputs masked,
 * the timer masked on its vector, then software-enabled. */
static void local_setup(void) {
    apic_write(APIC_TPR, 0);
    apic_write(APIC_LVT_LINT0, LVT_MASKED);
    apic_write(APIC_LVT_LINT1, LVT_MASKED);
    apic_write(APIC_LVT_ERROR, LVT_MASKED);
    apic_write(APIC_TIMER_DIVIDE, 3); /* by 16 */
    apic_write(APIC_LVT_TIMER, LVT_MASKED | X86_TIMER_VECTOR | (deadline ? LVT_TSC_DEADLINE : 0));
    apic_write(APIC_SVR, 0x100 | X86_SPURIOUS_VECTOR);
}

/* The 8259s remapped to vectors 0xe0-0xef, away from the exceptions, and
 * masked: nothing reaches the CPU through them either way, as LINT0 is
 * masked. */
static void pic_mask(void) {
    static const uint8_t init[] = {0x11, 0xe0, 4, 1}, follower[] = {0x11, 0xe8, 2, 1};
    for (unsigned i = 0; i < 4; ++i) {
        x86_outb(i ? 0x21 : 0x20, init[i]);
        x86_outb(i ? 0xa1 : 0xa0, follower[i]);
    }
    x86_outb(0x21, 0xff);
    x86_outb(0xa1, 0xff);
}

/* Every IOAPIC input masked: its version register gives the last one. */
static void ioapic_mask(void) {
    for (unsigned n = 0; n < x86_ioapic_count; ++n) {
        uintptr_t base = x86_ioapics[n];
        mmio_write32(base, 1);
        unsigned last = (mmio_read32(base + 0x10) >> 16) & 0xff;
        for (unsigned i = 0; i <= last; ++i) {
            mmio_write32(base, 0x10 + 2 * i);
            mmio_write32(base + 0x10, mmio_read32(base + 0x10) | LVT_MASKED);
        }
    }
}

bool timer_init(const struct timer_route *configuration) {
    if (ready) return fail("scheduler timer started twice");
    route = *configuration;
    if (!x86_tsc_frequency) return fail("TSC frequency unknown");
    if (!route.x2apic && !route.lapic) return fail("no local APIC");
    deadline = x86_features.tsc_deadline;
    pic_mask();
    ioapic_mask();
    apic_enable();
    local_setup();
    if (!deadline) {
        /* One-shot counts per millisecond, against the TSC. */
        apic_write(APIC_TIMER_INITIAL, 0xffffffffu);
        uint64_t start = x86_rdtsc(), span = x86_tsc_frequency / 100;
        while (x86_rdtsc() - start < span) { }
        apic_ticks_per_ms = (0xffffffffu - apic_read(APIC_TIMER_CURRENT)) / 10;
        apic_write(APIC_TIMER_INITIAL, 0);
        if (!apic_ticks_per_ms) return fail("local APIC timer does not count");
    }
    apic_write(APIC_LVT_TIMER, X86_TIMER_VECTOR | (deadline ? LVT_TSC_DEADLINE : 0));
    ready = true;
    return true;
}

bool timer_secondary_init(void) {
    if (!ready) return false;
    apic_enable();
    local_setup();
    apic_write(APIC_LVT_TIMER, X86_TIMER_VECTOR | (deadline ? LVT_TSC_DEADLINE : 0));
    return true;
}

static void arm(uint32_t microseconds) {
    if (deadline) {
        x86_wrmsr(MSR_TSC_DEADLINE, x86_rdtsc() + microseconds * x86_tsc_frequency / 1000000);
    } else {
        uint64_t count = microseconds * apic_ticks_per_ms / 1000;
        apic_write(APIC_TIMER_INITIAL,
                   count ? (uint32_t)(count < 0xffffffffu ? count : 0xffffffffu) : 1);
    }
}

void timer_arm_us(uint32_t microseconds) {
    if (!ready) panic("scheduler timer not initialized");
    if (!microseconds || microseconds > 5000) microseconds = 5000;
    arm(microseconds);
}

/* Called from entry.S with GS possibly the user's: touches only this file's
 * state. */
void x86_interrupt_eoi(void) { apic_write(APIC_EOI, 0); }

void timer_stop(bool acknowledge) {
    if (deadline) x86_wrmsr(MSR_TSC_DEADLINE, 0);
    else apic_write(APIC_TIMER_INITIAL, 0);
    if (acknowledge) x86_interrupt_eoi();
}

/* Standby on CPU 0, with interrupts masked: sleep until about
 * `microseconds` (at most 1 s) have passed. HLT ends at the timer's
 * interrupt, which entry.S acknowledges. */
void cpu_sleep_us(uint32_t microseconds) {
    if (!ready || !microseconds) return;
    if (microseconds > 1000000) microseconds = 1000000;
    arm(microseconds);
    __asm__ volatile("sti\n hlt\n cli" ::: "memory");
    timer_stop(false);
}

uint32_t x86_apic_id(void) { return route.x2apic ? apic_read(APIC_ID) : apic_read(APIC_ID) >> 24; }

void x86_send_ipi(uint32_t apic_id, uint32_t command) {
    if (route.x2apic) {
        x86_wrmsr(MSR_X2APIC + APIC_ICR / 16, (uint64_t)apic_id << 32 | command);
        return;
    }
    while (apic_read(APIC_ICR) & (1u << 12)) __asm__ volatile("pause");
    apic_write(APIC_ICR_HIGH, apic_id << 24);
    apic_write(APIC_ICR, command);
}
