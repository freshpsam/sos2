#include "gta7lite.h"
#include "platform.h"

/* The CPU clocks, as MediaTek's cpufreq drives them from the AP (Samsung's
 * gta7lite kernel, drivers/misc/mediatek/base/power/cpufreq_v1: mt6765's
 * mtk_cpufreq_platform.c and the C65 table of mtk_cpufreq_opp_table.h, the
 * 2.3 GHz bin that the stock device tree's operating points also list).
 * ARMPLL_L clocks cluster 0 (CPUs 0-3), ARMPLL cluster 1 (CPUs 4-7) and
 * CCIPLL the clusters' interconnect; each PLL's CON1 holds the change bit
 * (31), the post divider (26:24, 0 for /1, 1 for /2, 2 for /4) and the
 * frequency word (21:0, the VCO in units of 26 MHz / 2^14). All three run
 * on the MT6357's VPROC, with VSRAM_PROC for their SRAM kept 100 mV above it
 * (at most 1.125 V, and never more than 350 mV above).
 *
 * LK leaves 1500, 920 and 600 MHz on 0.8 V and 0.9 V. The top operating
 * point is 2301, 1800 and 1051 MHz on 1.11875 V and 1.125 V. Going up, the
 * supplies rise first, then each PLL takes its new word while still behind
 * its old post divider and drops that divider 20 us later; going down, the
 * divider rises first, then the word, then the supplies fall. No clock ever
 * runs above both its old and its new rate, nor above what the supply
 * carries. Nothing is changed unless the PLLs and supplies are as LK left
 * them, or as the last change left them. */
enum {
    APMIXED = 0x1000c000,
    CHANGE = 1u << 31,
    POSTDIV_SHIFT = 24,
    POSTDIV = 7u << POSTDIV_SHIFT,
    WORD = 0x3fffff,
    VPROC_DBG0 = 0x14a2, /* DA_VPROC_VOSEL, 6:0 */
    VPROC_ELR0 = 0x14aa, /* RG_BUCK_VPROC_VOSEL, 6:0 */
    VSRAM_DBG0 = 0x19cc, /* DA_VSRAM_PROC_VOSEL, 14:8 */
    VSRAM_CON0 = 0x19fa, /* RG_LDO_VSRAM_PROC_VOSEL, 6:0 */
    VOSEL = 0x7f,
    VPROC_FAST = 96, /* 518.75 mV + 6.25 mV steps: 1.11875 V */
    VSRAM_FAST = 97, /* 1.125 V */
    SUPPLY_US = 500, /* ramps of 8.33 mV/us, then the readback */
    LOCK_US = 20,
    POSTDIV_US = 1
};

#define WORD_FOR(mhz) ((((uint32_t)(mhz)) << 14) / 26)

static const struct pll {
    uintptr_t con1;
    uint32_t fast; /* the top operating point's CON1 */
} plls[] = {
    {APMIXED + 0x220, CHANGE | 0u << POSTDIV_SHIFT | WORD_FOR(2301)}, /* cluster 0 */
    {APMIXED + 0x210, CHANGE | 0u << POSTDIV_SHIFT | WORD_FOR(1800)}, /* cluster 1 */
    {APMIXED + 0x230, CHANGE | 1u << POSTDIV_SHIFT | WORD_FOR(2102)}, /* CCI: 1051 MHz */
};

enum { PLLS = sizeof(plls) / sizeof(plls[0]) };

static struct {
    bool checked, usable, fast;
    uint32_t slow[PLLS];
    uint16_t vproc, vsram; /* LK's selections */
} clock;

static bool supply(uint16_t vproc, uint16_t vsram) {
    uint16_t at, sram;
    return gta7lite_pmic_read(VPROC_DBG0, &at) && gta7lite_pmic_read(VSRAM_DBG0, &sram) &&
           (at & VOSEL) == vproc && ((sram >> 8) & VOSEL) == vsram;
}

/* Both supplies set, VSRAM_PROC first going up and VPROC first going down,
 * then confirmed at their new levels. */
static bool set_supply(uint16_t vproc, uint16_t vsram, bool up) {
    bool written = up ? gta7lite_pmic_update(VSRAM_CON0, vsram, VOSEL) &&
                            gta7lite_pmic_update(VPROC_ELR0, vproc, VOSEL)
                      : gta7lite_pmic_update(VPROC_ELR0, vproc, VOSEL) &&
                            gta7lite_pmic_update(VSRAM_CON0, vsram, VOSEL);
    if (!written) return false;
    for (uint64_t start = time_us(); time_us() - start < SUPPLY_US;)
        if (supply(vproc, vsram)) return true;
    return false;
}

static void set_pll(const struct pll *p, uint32_t target) {
    uint32_t now = mmio_read32(p->con1);
    if ((target & POSTDIV) > (now & POSTDIV)) {
        mmio_write32(p->con1, (now & ~POSTDIV) | (target & POSTDIV) | CHANGE);
        delay_us(POSTDIV_US);
    }
    mmio_write32(p->con1, (mmio_read32(p->con1) & ~WORD) | (target & WORD) | CHANGE);
    delay_us(LOCK_US);
    if ((target & POSTDIV) < (now & POSTDIV)) {
        mmio_write32(p->con1, (mmio_read32(p->con1) & ~POSTDIV) | (target & POSTDIV) | CHANGE);
        delay_us(POSTDIV_US);
    }
}

/* LK's clocks: each PLL at most at its top operating point with the same
 * post divider or a larger one, and the supplies below the top. */
static bool check(void) {
    uint16_t vproc, vsram;
    if (!gta7lite_pmic_read(VPROC_DBG0, &vproc) || !gta7lite_pmic_read(VSRAM_DBG0, &vsram))
        return false;
    clock.vproc = vproc & VOSEL;
    clock.vsram = (vsram >> 8) & VOSEL;
    if (clock.vproc > VPROC_FAST || clock.vsram > VSRAM_FAST || clock.vsram < clock.vproc)
        return false;
    for (unsigned i = 0; i < PLLS; ++i) {
        uint32_t now = mmio_read32(plls[i].con1), divider = (now & POSTDIV) >> POSTDIV_SHIFT;
        uint32_t fast = (plls[i].fast & POSTDIV) >> POSTDIV_SHIFT;
        if (!(now & CHANGE) || divider > 2 || divider < fast ||
            ((uint64_t)(now & WORD) >> divider) > (uint64_t)(plls[i].fast & WORD) >> fast)
            return false;
        clock.slow[i] = now;
    }
    return true;
}

bool gta7lite_cpu_clock(bool fast) {
    if (!clock.checked) {
        clock.checked = true;
        clock.usable = check();
    }
    if (!clock.usable) return false;
    if (fast == clock.fast) return true;
    for (unsigned i = 0; i < PLLS; ++i)
        if (mmio_read32(plls[i].con1) != (clock.fast ? plls[i].fast : clock.slow[i])) {
            clock.usable = false;
            return false;
        }
    if (fast) {
        if (!set_supply(VPROC_FAST, VSRAM_FAST, true)) {
            /* Back to LK's supply; the clocks are unchanged. */
            (void)set_supply(clock.vproc, clock.vsram, false);
            clock.usable = false;
            return false;
        }
        for (unsigned i = 0; i < PLLS; ++i) set_pll(&plls[i], plls[i].fast);
    } else {
        for (unsigned i = 0; i < PLLS; ++i) set_pll(&plls[i], clock.slow[i]);
        clock.fast = false;
        if (!set_supply(clock.vproc, clock.vsram, false)) {
            clock.usable = false;
            return false;
        }
    }
    clock.fast = fast;
    return true;
}

/* MediaTek's governor raises the clocks with the load; here a 20 ms window
 * with 2 ms of user time, a tenth of one CPU, raises them at once, and a
 * second of windows below that lowers them again. An idle page's 8 ms
 * timer stays below; loading, scrolling and video stay above. At the top
 * point the tablet draws about 66 mA more even with the CPUs idle. */
enum { WINDOW_US = 20000, BUSY_US = 2000, IDLE_US = 1000000 };

void gta7lite_cpu_clock_poll(uint64_t user_us) {
    static uint64_t window_at, window_user, busy_at;
    uint64_t now = time_us();
    if (now - window_at < WINDOW_US) return;
    bool busy = user_us - window_user >= BUSY_US;
    window_at = now;
    window_user = user_us;
    if (busy) busy_at = now;
    if (busy && !clock.fast) (void)gta7lite_cpu_clock(true);
    else if (!busy && clock.fast && now - busy_at >= IDLE_US) (void)gta7lite_cpu_clock(false);
}
