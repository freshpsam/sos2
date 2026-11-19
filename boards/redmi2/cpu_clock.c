#include "redmi2_cpu_clock.h"
#include "platform.h"

/* The A53 cluster clock, from Linux v6.6 drivers/clk/qcom: apcs-msm8916.c puts
 * its mux at APCS + 0x50 (APCS at 0x0b011000, msm8916.dtsi): CMD_RCGR, then
 * CFG_RCGR with the source in bits 10:8 (4 = GPLL0, 5 = the A53 PLL) and a
 * half-integer divider in bits 4:0 (value 2d - 1 divides by d).
 * clk-regmap-mux-div.c changes it by writing CFG_RCGR, setting CMD_RCGR's
 * UPDATE bit and waiting up to 500 us for the bit to clear; a set DIRTY_CFG
 * bit means another change is pending. Linux parks the cluster on GPLL0 / 2
 * while it reprograms the A53 PLL, and lists 200 MHz (GPLL0's 800 MHz / 4) as
 * the CPUs' lowest operating point. GPLL0 is voted on, as gcc-msm8916.c's
 * gpll0_vote does (bit 0 at 0x01845000, active bit 17 at 0x0182101c), and the
 * vote stays: it is shared with storage, touch, the GPU and Wi-Fi. The
 * generic timer runs from the 19.2 MHz crystal, so time keeping is unaffected.
 * lk2nd leaves the cluster on GPLL0 at 800 MHz (divider 1; the A53 PLL stays
 * off), so standby only changes the divider; any other source keeps its
 * rate. A restore the mux does not confirm is undone: the cluster stays at
 * 200 MHz until the next wake-up tries again. */
static uint32_t read32(uintptr_t address) {
    uint32_t value = mmio_read32(address);
    barrier();
    return value;
}

static void write32(uintptr_t address, uint32_t value) {
    barrier();
    mmio_write32(address, value);
    barrier();
}

enum {
    CMD_RCGR = 0x0b011050,
    CFG_RCGR = 0x0b011054,
    UPDATE = 1,
    DIRTY_CFG = 1u << 4,
    SOURCE = 7u << 8,
    DIVIDER = 0x1f,
    FROM_GPLL0 = 4u << 8,
    QUARTER = 7,
    GPLL0_VOTE = 0x01845000,
    GPLL0_STATUS = 0x0182101c,
    GPLL0_ACTIVE = 1u << 17,
    UPDATE_US = 500,
    LOCK_US = 1000,
};

static uint32_t saved;
static bool slowed;

static bool wait_bits(uintptr_t address, uint32_t mask, uint32_t value, uint32_t limit) {
    uint64_t start = time_us();
    while ((read32(address) & mask) != value)
        if (time_us() - start >= limit) return false;
    return true;
}

static bool configure(uint32_t cfg) {
    write32(CFG_RCGR, cfg);
    write32(CMD_RCGR, read32(CMD_RCGR) | UPDATE);
    return wait_bits(CMD_RCGR, UPDATE, 0, UPDATE_US);
}

uint32_t redmi2_cpu_clock_config(void) { return read32(CFG_RCGR); }

void redmi2_cpu_clock_reapply(uint32_t cfg) {
    if (read32(CFG_RCGR) != cfg) (void)configure(cfg);
}

void redmi2_cpu_clock_standby(bool slow) {
    if (slow == slowed) return;
    if (!slow) {
        if (!configure(saved)) {
            /* The change may still land: queue 200 MHz behind it, as standby
             * entry does, and let the next wake-up try again. */
            (void)configure((saved & ~(SOURCE | DIVIDER)) | FROM_GPLL0 | QUARTER);
            return;
        }
        slowed = false;
        return;
    }
    uint32_t cmd = read32(CMD_RCGR), cfg = read32(CFG_RCGR);
    if (cmd & (UPDATE | DIRTY_CFG)) return;
    uint32_t source = cfg & SOURCE;
    if (source != FROM_GPLL0 || (cfg & DIVIDER) >= QUARTER)
        return; /* Not GPLL0, or already at 200 MHz or below. */
    write32(GPLL0_VOTE, read32(GPLL0_VOTE) | 1);
    if (!wait_bits(GPLL0_STATUS, GPLL0_ACTIVE, GPLL0_ACTIVE, LOCK_US)) return;
    saved = cfg;
    if (!configure((cfg & ~(SOURCE | DIVIDER)) | FROM_GPLL0 | QUARTER)) {
        /* The change may still land: queue the old configuration behind it. */
        (void)configure(saved);
        return;
    }
    slowed = true;
}
