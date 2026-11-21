#include "nexus5.h"
#include "qcom_scm.h"
#include "platform.h"

/* The other three Krait cores, started as the stock kernel starts them
 * (arch/arm/mach-msm/platsmp.c with krait-regulator.c's power-up, and
 * acpuclock-krait.c's clocks; Linux's kpssv2_release_secondary powers them
 * up the same way). The firmware takes their entry once (the legacy SCM
 * boot address, cold-boot flags for CPUs 1-3). Each core's clock is set
 * from CPU 0 while the core is off, as the stock kernel sets a core it
 * plugs in: its own HFPLL at CPU 0's rate (cpu_clock.c). Then its power:
 * the head switch (BHS) on segment by segment with the LDO bypassed, so the
 * core takes the shared rail as CPU 0 does; the rail's four phases (the
 * L2's SAW2 sends that command to the PMIC); its clamp and reset released.
 * The firmware starts a core at its entry at once, so no interrupt wakes
 * it: an SGI sent as Linux sends one would wait until the core first takes
 * interrupts, where only its timer's are expected. All cores share CPU 0's
 * voltage. */
enum {
    CORES = 4,
    ACC = 0xf9088000, /* CPU n's at + 0x10000 n */
    STRIDE = 0x10000,
    CPU_PWR_CTL = 0x04,
    APC_PWR_GATE_CTL = 0x14,
    /* APC_PWR_GATE_CTL: BHS_CNT 64 (31:24), LDO_PWR_DWN (21:16), LDO_BYP
     * (13:8), BHS_SEG (6:1), BHS_EN (0). */
    GATE_BHS = 64u << 24 | 0x3fu << 16 | 1,
    GATE_SEGMENTS = 0x3fu << 1,
    GATE_BYPASS = 0x3fu << 8,
    /* CPU_PWR_CTL: CORE_PWRD_UP (7), COREPOR_RST (5), CLAMP (0). */
    POWERED_UP = 0x80,
    RESET = 0x20,
    CLAMP = 0x01,
    L2_SAW_VCTL = 0xf901201c,
    FOUR_PHASES = 1u << 16 | 3, /* port 1 (the phases), data: count - 1 */
};

static unsigned cores, released;
static bool phases_set;

unsigned nexus5_smp_prepare(uint32_t boot_mpidr, uintptr_t entry) {
    /* The cores' clocks first: each starts at CPU 0's rate (cpu_clock.c). */
    if (boot_mpidr || !nexus5_cpu_clock_init()) return 1;
    if (!qcom_scm_cpu_boot(entry, 0xe)) return 1;
    cores = CORES;
    return cores;
}

bool nexus5_smp_release(unsigned cpu) {
    if (!cpu || cpu >= cores || (released & (1u << cpu))) return false;
    released |= 1u << cpu; /* never a partial sequence twice */
    if (!nexus5_cpu_clock_start(cpu)) return false;
    uintptr_t acc = ACC + STRIDE * cpu;
    nexus5_store(acc + APC_PWR_GATE_CTL, GATE_BHS);
    delay_us(1);
    nexus5_store(acc + APC_PWR_GATE_CTL, GATE_BHS | GATE_SEGMENTS);
    delay_us(1);
    nexus5_store(acc + APC_PWR_GATE_CTL, GATE_BHS | GATE_SEGMENTS | GATE_BYPASS);
    if (!phases_set) {
        phases_set = true;
        nexus5_store(L2_SAW_VCTL, FOUR_PHASES);
        delay_us(50);
    }
    nexus5_store(acc + CPU_PWR_CTL, RESET | CLAMP);
    delay_us(2);
    nexus5_store(acc + CPU_PWR_CTL, RESET);
    delay_us(2);
    nexus5_store(acc + CPU_PWR_CTL, 0);
    nexus5_store(acc + CPU_PWR_CTL, POWERED_UP);
    return true;
}
