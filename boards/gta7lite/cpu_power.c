#include "gta7lite.h"
#include "smp.h"
#include "platform.h"

/* Standby's CPU power-down on MT6765. PSCI's CPU_OFF only parks a CPU in
 * MediaTek's ATF, whose power-off hook is empty: the CPU stays powered in
 * the firmware's WFI, and the next CPU_ON, finding its power already on,
 * gives it no reset and leaves it ON_PENDING for good. MediaTek's kernels cut
 * the power themselves once PSCI reports the CPU off, through the ATF's SiP
 * calls (hps_v3/mtk_hotplug_cb.c: POWER_DOWN_CORE, then POWER_DOWN_CLUSTER
 * once the cluster is empty); CPU_ON then powers the CPU, and its cluster,
 * back up. Neither SiP call checks its target: a running CPU, or the boot
 * CPU's cluster, would lose its power, and a number past the eight CPUs
 * (four per cluster) stops the firmware. The calls pass through GenieZone,
 * the hypervisor the kernel runs under, to the ATF. */
enum {
    PSCI_CPU_OFF = 0x84000002,
    PSCI_CPU_ON = 0xc4000003,
    PSCI_AFFINITY_INFO = 0xc4000004,
    AFFINITY_OFF = 1,
    SIP_POWER_DOWN_CLUSTER = 0xc2000210,
    SIP_POWER_DOWN_CORE = 0xc2000212,
    CLUSTER1 = 0xf0, /* CPUs 4-7, by linear number */
    /* From PSCI's OFF to the CPU's last instructions before its WFI. */
    SETTLE_US = 1000
};

static unsigned cut; /* linear CPU numbers whose power is cut */

static int32_t smc(uint32_t function, uintptr_t a, uintptr_t b) {
    return arm_psci_call(CPU_BOOT_SMC, function, a, b);
}

/* cluster * 4 + core, or -1 outside MT6765's two clusters of four. */
static int linear(uint32_t mpidr) {
    unsigned core = mpidr & 0xff, cluster = mpidr >> 8 & 0xff;
    return mpidr >> 16 || core > 3 || cluster > 1 ? -1 : (int)(cluster * 4 + core);
}

void gta7lite_cpu_off(void) { (void)smc(PSCI_CPU_OFF, 0, 0); }

bool gta7lite_cpu_cut(uint32_t mpidr) {
    int n = linear(mpidr);
    /* CPU 0 boots the system and stays on, so cluster 0 does. */
    if (n <= 0 || linear(cpu_records[0].mpidr) != 0) return false;
    if (cut & 1u << n) return true;
    if (smc(PSCI_AFFINITY_INFO, mpidr, 0) != AFFINITY_OFF) return false;
    delay_us(SETTLE_US);
    if (smc(SIP_POWER_DOWN_CORE, (uint32_t)n, 0)) return false;
    cut |= 1u << n;
    if ((cut & CLUSTER1) == CLUSTER1) (void)smc(SIP_POWER_DOWN_CLUSTER, 1, 0);
    return true;
}

bool gta7lite_cpu_on(uint32_t mpidr, uintptr_t entry) {
    if (!gta7lite_cpu_cut(mpidr) || smc(PSCI_CPU_ON, mpidr, entry)) return false;
    cut &= ~(1u << linear(mpidr));
    return true;
}
