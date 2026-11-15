#include "redmi2_smp.h"
#include "qcom_scm.h"
#include "platform.h"

/* wt88047 vendor kernel: arch/arm/mach-msm/platsmp.c arm_release_secondary,
 * msm8916_boot_secondary and arm_smp_init_cpus. See docs/architecture.md. */
static uint32_t midr(void) {
    uint32_t n;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(n));
    return n;
}

#define read32  mmio_read32
#define write32 mmio_write32
#define sync    barrier
static unsigned cores, released, warm;

unsigned redmi2_smp_prepare(uint32_t boot_mpidr, uintptr_t entry, uintptr_t warm_entry) {
    /* The phone emulator has an A15 and substituted peripherals. Do not
     * access ACC or invoke Qualcomm firmware on that CPU. */
    if ((midr() & 0xff0ffff0) != 0x410fd030 || boot_mpidr) return 1;
    unsigned count = read32(0x0b011030) & 15;
    if (count < 2 || count > 4 || !qcom_scm_cpu_boot(entry, (1u << count) - 1)) return 1;
    /* After a standby power-down the firmware enters the shared warm entry
     * (CPU 0 resumes, the others restart). Register it now, before the PAS
     * setup reinitializes the transport; without CPU 0, for the others only. */
    unsigned all = (1u << count) - 1, secondary = all & ~1u;
    warm = qcom_scm_cpu_warm_boot(warm_entry, all)         ? all
           : qcom_scm_cpu_warm_boot(warm_entry, secondary) ? secondary
                                                           : 0;
    cores = count;
    return count;
}

unsigned redmi2_smp_warm_mask(void) { return warm; }

static void store(uintptr_t at, uint32_t value) {
    write32(at, value);
    sync();
}

static bool delay(void) {
    uint64_t start = time_us();
    for (unsigned n = 0; n < 100000; ++n) {
        /* time_us truncates counter fractions. Three reported microseconds
         * guarantee at least the stock driver's two actual microseconds. */
        if (time_us() - start >= 3) return true;
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

bool redmi2_smp_release(unsigned cpu) {
    if (!cpu || cpu >= cores || (released & (1u << cpu))) return false;
    released |= 1u << cpu; /* Never replay a partial power sequence. */
    if (!delay()) return false;
    uintptr_t base = 0x0b088000 + cpu * 0x10000;
    store(base + 4, 0x33);
    store(base + 0x14, 0x10000001);
    if (!delay()) return false;
    store(base + 4, 0x31);
    store(base + 4, 0x39);
    if (!delay()) return false;
    store(base + 4, 0x20038);
    if (!delay()) return false;
    store(base + 4, 0x20008);
    store(base + 4, 0x20088);
    /* GICv2 SGI 0, target list selects this MSM8916 CPU. */
    store(0x0b000f00, (1u << cpu) << 16);
    return true;
}
