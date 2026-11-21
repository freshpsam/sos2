#include "nexus5.h"
#include "platform.h"

/* The Krait cores' and their L2 cache's clocks, as the stock kernel's
 * acpuclock-krait.c switches them (acpuclock-8974.c's tables): each core
 * and the L2 have their own HFPLL (L x 19.2 MHz) and a CPMR, an L2 indirect
 * register whose primary mux takes the HFPLL or the secondary mux, which
 * takes GPLL0's 300 MHz (the auxiliary source). A running clock moves by
 * parking on the auxiliary source, restarting its HFPLL at the new L, and
 * taking the HFPLL again.
 * The bootloader leaves CPU 0 at 960 MHz and the L2 at 729.6 MHz, the CPU
 * rail (PM8841 S5) at 900 mV, the memory rail (S1) at 1.05 V and the logic
 * rail (S2) at 1.03 V, its NORMAL level (read 2026-11-20). This phone's
 * part is speed bin 1, voltage class 2 (its fuses at 0xFC4B80B0). Its
 * table pairs each core rate with an L2 rate, and L2 rates above 1036.8 MHz
 * need the logic rail above NORMAL, which only the RPM sets; a core's own
 * HFPLL needs NORMAL up to L 104, and the memory rail is already at the
 * table's highest, 1.05 V. So the L2 runs at 1036.8 MHz and the cores at
 * 1497.6 MHz, which the table gives 910 mV: the CPU rail rises 10 mV first,
 * written over SPMI and read back, given the time its slew needs (2.4 mV a
 * microsecond), then confirmed ready. Without that the cores run at
 * 1190.4 MHz, which wants 865 mV. In standby CPU 0 and the L2 wait on the
 * auxiliary source with their HFPLLs off; the rail stays, as the other
 * cores keep their rate. */
enum {
    HFPLL0 = 0xf908a000, /* CPU n's at + 0x10000 n */
    HFPLL_L2 = 0xf9016000,
    PLL_MODE = 0x00,
    PLL_L = 0x04,
    PLL_M = 0x08,
    PLL_N = 0x0c,
    PLL_USER = 0x10,
    PLL_CONFIG = 0x14,
    PLL_STATUS = 0x1c,
    PLL_LOCKED = 1u << 16,
    PLL_CONFIG_VALUE = 0x04d0405d,
    PLL_USER_VALUE = 0x8,
    PLL_HIGH_VCO = 1u << 20, /* for L above 65 */
    PLL_BYPASS_OFF = 2,
    PLL_RESET_OFF = 6,
    PLL_OUTPUT_ON = 7,
    LOCK_US = 1000,
    /* CPMR: the primary mux (1:0; 0 the secondary mux, 1 the HFPLL), the
     * secondary mux (3:2; 2 the auxiliary source), the HFPLL divider (7:6). */
    CPMR0 = 0x4501, /* CPU n's at + 0x1000 n */
    CPMR_L2 = 0x0500,
    PRIMARY = 3,
    PRIMARY_SECONDARY = 0,
    PRIMARY_HFPLL = 1,
    SECONDARY = 3 << 2,
    SECONDARY_AUX = 2 << 2,
    DIVIDER = 3 << 6,
    GPLL0_VOTE = 0xfc401480,
    GPLL0_STATUS = 0xfc40001c,
    GPLL0_ACTIVE = 1u << 17,
    /* 1497.6 MHz, or 1190.4 MHz on the bootloader's rail; 1036.8 MHz; the
     * bootloader's 960 MHz and 729.6 MHz. */
    FAST_L = 78,
    RAIL_L = 62,
    L2_L = 54,
    BOOT_CPU_L = 50,
    BOOT_L2_L = 38,
    /* PM8841 S5, the CPU rail (slave 5, 0x2000; an FTSMPS: range 0 counts
     * 5 mV steps): status (bit 7 ready), range, voltage. */
    S5_STATUS = 0x52008,
    S5_RANGE = 0x52040,
    S5_VOLTAGE = 0x52041,
    VREG_READY = 0x80,
    BOOT_VOLTAGE = 180, /* 900 mV */
    FAST_VOLTAGE = 182, /* 910 mV */
    SLEW_UV_PER_US = 2395,
};

static bool ready;
static unsigned cpu_l;

/* The CPU rail from the bootloader's 900 mV to code (5 mV steps). */
static bool rail(uint8_t code) {
    uint8_t range, now, status = 0;
    if (!nexus5_pmic_read(S5_RANGE, &range) || range || !nexus5_pmic_read(S5_VOLTAGE, &now) ||
        now != BOOT_VOLTAGE || code < now || code > FAST_VOLTAGE)
        return false;
    if (!nexus5_pmic_write(S5_VOLTAGE, code)) return false;
    delay_us((code - now) * 5000u / SLEW_UV_PER_US + 10);
    for (uint64_t start = time_us(); !(status & VREG_READY);)
        if (!nexus5_pmic_read(S5_STATUS, &status) || time_us() - start >= LOCK_US) return false;
    return true;
}

/* An L2 indirect register, through this core's CP15 window; the caller has
 * interrupts masked, so nothing comes between the select and the data. */
static uint32_t indirect_read(uint32_t address) {
    uint32_t value;
    __asm__ volatile("mcr p15, 3, %1, c15, c0, 6\n isb\n mrc p15, 3, %0, c15, c0, 7"
                     : "=r"(value)
                     : "r"(address)
                     : "memory");
    return value;
}

static void indirect_write(uint32_t address, uint32_t value) {
    __asm__ volatile("mcr p15, 3, %0, c15, c0, 6\n isb\n mcr p15, 3, %1, c15, c0, 7\n isb"
                     :
                     : "r"(address), "r"(value)
                     : "memory");
}

static void mux(uint32_t cpmr, uint32_t mask, uint32_t value) {
    indirect_write(cpmr, (indirect_read(cpmr) & ~mask) | value);
    barrier();
    delay_us(1);
}

static void store(uintptr_t address, uint32_t value) {
    mmio_write32(address, value);
    barrier();
}

static bool wait_set(uintptr_t address, uint32_t bits) {
    for (uint64_t start = time_us(); !(mmio_read32(address) & bits);)
        if (time_us() - start >= LOCK_US) return false;
    return true;
}

static bool pll_start(uintptr_t pll, unsigned l) {
    store(pll + PLL_MODE, 0);
    store(pll + PLL_CONFIG, PLL_CONFIG_VALUE);
    store(pll + PLL_M, 0);
    store(pll + PLL_N, 1);
    store(pll + PLL_USER, PLL_USER_VALUE | (l > 65 ? PLL_HIGH_VCO : 0));
    store(pll + PLL_L, l);
    store(pll + PLL_MODE, PLL_BYPASS_OFF);
    delay_us(10);
    store(pll + PLL_MODE, PLL_RESET_OFF);
    if (!wait_set(pll + PLL_STATUS, PLL_LOCKED)) return false;
    store(pll + PLL_MODE, PLL_OUTPUT_ON);
    return true;
}

/* A clock (a core's, from that core or while it is off, or the L2's) onto
 * the auxiliary source, its HFPLL stopped. */
static void park(uint32_t cpmr, uintptr_t pll) {
    mux(cpmr, SECONDARY, SECONDARY_AUX);
    mux(cpmr, PRIMARY, PRIMARY_SECONDARY);
    store(pll + PLL_MODE, 0);
}

/* From the auxiliary source onto its HFPLL at l x 19.2 MHz. */
static bool run(uint32_t cpmr, uintptr_t pll, unsigned l) {
    if (!pll_start(pll, l)) return false;
    mux(cpmr, DIVIDER, 0);
    mux(cpmr, PRIMARY, PRIMARY_HFPLL);
    return true;
}

bool nexus5_cpu_clock_init(void) {
    if (ready) return true;
    /* Only from the bootloader's rates, which this table starts from. */
    if ((mmio_read32(HFPLL0 + PLL_L) & 0xff) != BOOT_CPU_L ||
        (mmio_read32(HFPLL_L2 + PLL_L) & 0xff) != BOOT_L2_L)
        return false;
    store(GPLL0_VOTE, mmio_read32(GPLL0_VOTE) | 1);
    if (!wait_set(GPLL0_STATUS, GPLL0_ACTIVE)) return false;
    cpu_l = rail(FAST_VOLTAGE) ? FAST_L : RAIL_L;
    /* A new rate that does not lock gives the bootloader's back. */
    park(CPMR0, HFPLL0);
    if (!run(CPMR0, HFPLL0, cpu_l)) {
        (void)run(CPMR0, HFPLL0, BOOT_CPU_L);
        return false;
    }
    park(CPMR_L2, HFPLL_L2);
    if (!run(CPMR_L2, HFPLL_L2, L2_L)) {
        (void)run(CPMR_L2, HFPLL_L2, BOOT_L2_L);
        return false;
    }
    ready = true;
    return true;
}

bool nexus5_cpu_clock_start(unsigned cpu) {
    if (!ready || !cpu || cpu > 3) return false;
    uint32_t cpmr = CPMR0 + 0x1000u * cpu;
    uintptr_t pll = HFPLL0 + 0x10000u * cpu;
    park(cpmr, pll);
    return run(cpmr, pll, cpu_l);
}

void nexus5_cpu_clock_standby(bool on) {
    if (!ready) return;
    if (on) {
        park(CPMR0, HFPLL0);
        park(CPMR_L2, HFPLL_L2);
        return;
    }
    /* A PLL that does not lock leaves its clock on the auxiliary source. */
    (void)run(CPMR_L2, HFPLL_L2, L2_L);
    (void)run(CPMR0, HFPLL0, cpu_l);
}
