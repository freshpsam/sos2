#include "nexus5.h"
#include "platform.h"

/* The Krait cores' and their L2 cache's clocks, as the stock kernel's
 * acpuclock-krait.c switches them (acpuclock-8974.c's v2 tables): each core
 * and the L2 have their own HFPLL (L x 19.2 MHz) and a CPMR, an L2 indirect
 * register whose primary mux takes the HFPLL or the secondary mux, which
 * takes GPLL0's 300 MHz (the auxiliary source). A running clock moves by
 * parking on the auxiliary source, restarting its HFPLL at the new L, and
 * taking the HFPLL again; any core's, from CPU 0.
 * The bootloader leaves CPU 0 at 960 MHz and the L2 at 729.6 MHz, the CPU
 * rail (PM8841 S5) at 900 mV, the memory rail (S1) at 1.05 V and the logic
 * rail (S2) at 1.03 V (read 2026-11-20). The table's voltages follow the
 * part's speed bin and voltage class, fused at 0xFC4B80B0 (this phone's:
 * bin 1, class 2). The memory rail is already at the table's highest; L2
 * rates above 1036.8 MHz and HFPLLs above L 104 need the logic rail at its
 * SUPER_TURBO corner, which only the RPM sets (rpm.c). Its corners' nominal
 * voltages run from 0.9 V (NORMAL) to 1.05 V (SUPER_TURBO, the kernel's
 * rpm-regulator-smd.txt), so the bootloader's 1.03 V is about that already;
 * the vote keeps it there, and the rail must read at least TURBO's 0.9875 V
 * after it. The vote is never taken back, which keeps what the bootloader
 * left for the display and the rest. So the floor is 1497.6 MHz on the
 * cores with the L2 at 1036.8 MHz: the boot
 * moves there, the CPU rail rising first (for this part 10 mV, to 910 mV),
 * written to S5 over SPMI, read back, given the time its slew needs
 * (2.4 mV a microsecond) and confirmed ready; without that the cores keep
 * the bootloader's 960 MHz and the L2 its 729.6 MHz, and go no higher.
 * Above the floor, the table's points up to 2265.6 MHz pair each core rate
 * with its L2 rate; the cores rise while pages work and go back a second
 * after, as the tablet's do; work that goes on past 15 s with no touch for
 * 3 s (a playing video) steps them back to the floor, a point every 2 s, until
 * a quiet second or a touch; all within a cap that the cores' temperature sets
 * (thermal.c): it falls a point every 250 ms from 80 C, to the floor, and
 * from 90 C (the stock kernel's own limit), or without a reading, below it
 * to 960 MHz with the L2 at 960 MHz, as all four cores working can reach
 * when the floor is not enough; it rises again below 70 C. Going up, the
 * logic rail first, then the CPU rail, then the cores and the L2; going
 * down, the clocks first. In standby the cores are at the floor or below,
 * and CPU 0 and the L2 wait on the auxiliary source with their HFPLLs off. */
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
    PTE_FUSE = 0xfc4b80b0, /* the speed bin and voltage class, then their status */
    PVS_BLOWN = 1u << 21,
    /* The bootloader's 960 MHz and 729.6 MHz. */
    BOOT_CPU_L = 50,
    BOOT_L2_L = 38,
    /* PM8841 S5, the CPU rail, and S2, the logic rail (slave 5, at 0x2000
     * and 0x1700; FTSMPSs whose range 0 counts 5 mV steps): status (bit 7
     * ready), range, voltage. */
    S5_STATUS = 0x52008,
    S5_RANGE = 0x52040,
    S5_VOLTAGE = 0x52041,
    S2_RANGE = 0x51740,
    S2_VOLTAGE = 0x51741,
    TURBO_LOGIC = 198, /* 0.99 V, TURBO's nominal 0.9875 V rounded up */
    VREG_READY = 0x80,
    BOOT_VOLTAGE = 180, /* 900 mV */
    RAIL_MIN = 155,     /* 775 mV */
    RAIL_MAX = 220,     /* 1100 mV, the stock regulator's highest */
    SLEW_UV_PER_US = 2395,
    CORNER_SUPER_TURBO = 7,
    /* The load and temperature watch. */
    WINDOW_US = 20000,
    BUSY_US = 2000,
    IDLE_US = 1000000,
    WATCH_US = 250000,
    THERMAL_RETRY_US = 1000000,
    /* Work that goes on for 15 s with no touch for 3 s (a playing video,
     * whose page keeps the main thread busy at any rate) steps the
     * ceiling down a point every 2 s, to the floor. */
    SUSTAINED_US = 15000000,
    TOUCH_US = 3000000,
    STEP_US = 2000000,
};
/* The cores' temperatures for the watch, in degrees. */
enum { COOL_C = 70, HOT_C = 80, EMERGENCY_C = 90 };

/* The points: a core rate's L and its L2 rate's L. The floor keeps the L2 at
 * 1036.8 MHz; every other point pairs them as the stock table does. */
static const struct point {
    uint8_t cpu_l, l2_l;
} points[] = {
    {50, 50},  /* 960 MHz, L2 960 MHz: too hot */
    {78, 54},  /* the floor: 1497.6 MHz, L2 1036.8 MHz */
    {82, 82},  /* 1574.4, L2 1574.4 */
    {90, 86},  /* 1728.0, L2 1651.2 */
    {102, 90}, /* 1958.4, L2 1728.0 */
    {118, 90}, /* 2265.6, L2 1728.0 */
};
enum { POINTS = sizeof(points) / sizeof(points[0]), FLOOR = 1 };

/* The CPU rail each point needs, in mV, by speed bin and voltage class
 * (class 7 uses class 6's, as in the stock tables); 0 where the bin's
 * table stops. A bin without a table keeps the floor, at the most any
 * table asks there. */
static const uint16_t millivolts[3][7][POINTS] = {
    {
        /* speed bin 0: up to 1958.4 MHz */
        {915, 1010, 1025, 1055, 1100, 0},
        {895, 990, 1005, 1030, 1075, 0},
        {875, 970, 980, 1005, 1050, 0},
        {860, 950, 960, 985, 1025, 0},
        {850, 930, 940, 960, 1000, 0},
        {840, 910, 920, 940, 975, 0},
        {830, 895, 905, 920, 950, 0},
    },
    {
        /* speed bin 1: up to 2265.6 MHz */
        {865, 950, 965, 995, 1040, 1100},
        {850, 930, 945, 975, 1020, 1075},
        {835, 910, 925, 955, 995, 1050},
        {820, 895, 910, 935, 970, 1025},
        {810, 880, 895, 920, 950, 1000},
        {800, 870, 880, 900, 930, 975},
        {790, 860, 870, 885, 915, 950},
    },
    {
        /* speed bin 2: up to 2150.4 MHz, so 1958.4 here */
        {875, 965, 980, 1010, 1055, 0},
        {860, 945, 960, 990, 1030, 0},
        {845, 925, 940, 970, 1005, 0},
        {830, 910, 925, 950, 985, 0},
        {820, 895, 910, 930, 960, 0},
        {810, 880, 890, 910, 940, 0},
        {800, 870, 875, 895, 920, 0},
    },
};

static struct {
    bool ready, governed, turbo;
    struct point floor; /* points[FLOOR], or the bootloader's rates */
    uint16_t mv[POINTS];
    unsigned top, cap, point; /* indices into points */
    unsigned started;         /* cores whose clocks run, by bit */
    uint64_t window_at, window_user, busy_at, watch_at;
    /* Sustained work: since when the cores have worked without a quiet
     * second, the last touch, the ceiling it has stepped down to and when. */
    uint64_t working_since, touched_at, stepped_at;
    unsigned ceiling;
    bool thermal; /* the temperature watch has its calibration */
} clock;

/* The part's voltages, as select_freq_plan and get_krait_bin_format_b read
 * its fuses: an unblown voltage class counts as class 0. */
static void select_table(void) {
    uint32_t fuse = mmio_read32(PTE_FUSE), status = mmio_read32(PTE_FUSE + 4);
    unsigned speed = fuse & 7, pvs = (fuse >> 6) & 7, redundant = (fuse >> 24) & 7;
    if (redundant == 1) speed = (fuse >> 27) & 7;
    if (redundant == 2) pvs = (fuse >> 27) & 7;
    if (!(status & PVS_BLOWN)) pvs = 0;
    if (pvs > 6) pvs = 6;
    clock.top = FLOOR;
    if (speed > 2) {
        clock.mv[0] = 915;
        clock.mv[FLOOR] = 1010;
        return;
    }
    for (unsigned i = 0; i < POINTS; ++i) {
        clock.mv[i] = millivolts[speed][pvs][i];
        if (clock.mv[i]) clock.top = i;
    }
}

static const struct point *at(unsigned i) { return i == FLOOR ? &clock.floor : &points[i]; }

/* The CPU rail to mv, from where it is. */
static bool rail(unsigned mv) {
    uint8_t range, now, status = 0, check;
    unsigned code = mv / 5;
    if (code < RAIL_MIN || code > RAIL_MAX || !nexus5_pmic_read(S5_RANGE, &range) || range ||
        !nexus5_pmic_read(S5_VOLTAGE, &now))
        return false;
    if (now == code) return true;
    if (!nexus5_pmic_write(S5_VOLTAGE, (uint8_t)code)) return false;
    unsigned step = code > now ? code - now : now - code;
    delay_us(step * 5000u / SLEW_UV_PER_US + 10);
    for (uint64_t start = time_us(); !(status & VREG_READY);)
        if (!nexus5_pmic_read(S5_STATUS, &status) || time_us() - start >= LOCK_US) return false;
    return nexus5_pmic_read(S5_VOLTAGE, &check) && check == code;
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

static bool pll_start(uintptr_t pll, unsigned l) {
    nexus5_store(pll + PLL_MODE, 0);
    nexus5_store(pll + PLL_CONFIG, PLL_CONFIG_VALUE);
    nexus5_store(pll + PLL_M, 0);
    nexus5_store(pll + PLL_N, 1);
    nexus5_store(pll + PLL_USER, PLL_USER_VALUE | (l > 65 ? PLL_HIGH_VCO : 0));
    nexus5_store(pll + PLL_L, l);
    nexus5_store(pll + PLL_MODE, PLL_BYPASS_OFF);
    delay_us(10);
    nexus5_store(pll + PLL_MODE, PLL_RESET_OFF);
    if (!nexus5_wait_bits(pll + PLL_STATUS, PLL_LOCKED, true, LOCK_US)) return false;
    nexus5_store(pll + PLL_MODE, PLL_OUTPUT_ON);
    return true;
}

/* A clock (a core's, running or off, or the L2's) onto the auxiliary
 * source, its HFPLL stopped. */
static void park(uint32_t cpmr, uintptr_t pll) {
    mux(cpmr, SECONDARY, SECONDARY_AUX);
    mux(cpmr, PRIMARY, PRIMARY_SECONDARY);
    nexus5_store(pll + PLL_MODE, 0);
}

/* From the auxiliary source onto its HFPLL at l x 19.2 MHz. */
static bool run(uint32_t cpmr, uintptr_t pll, unsigned l) {
    if (!pll_start(pll, l)) return false;
    mux(cpmr, DIVIDER, 0);
    mux(cpmr, PRIMARY, PRIMARY_HFPLL);
    return true;
}

/* A clock to l, or back to was if its HFPLL does not lock there (on the
 * auxiliary source if neither does). */
static bool retune(uint32_t cpmr, uintptr_t pll, unsigned l, unsigned was) {
    park(cpmr, pll);
    if (run(cpmr, pll, l)) return true;
    (void)run(cpmr, pll, was);
    return false;
}

static bool cores_and_l2(const struct point *to, const struct point *from) {
    bool ok = true;
    for (unsigned cpu = 0; cpu < 4; ++cpu)
        if (clock.started & (1u << cpu))
            ok &= retune(CPMR0 + 0x1000u * cpu, HFPLL0 + 0x10000u * cpu, to->cpu_l, from->cpu_l);
    if (to->l2_l != from->l2_l) ok &= retune(CPMR_L2, HFPLL_L2, to->l2_l, from->l2_l);
    return ok;
}

/* The logic rail at SUPER_TURBO, acknowledged, and reading at least TURBO. */
static bool logic(void) {
    if (clock.turbo) return true;
    uint8_t range = 0xff, code = 0;
    if (!nexus5_rpm_logic_corner(CORNER_SUPER_TURBO) || !nexus5_pmic_read(S2_RANGE, &range) ||
        range || !nexus5_pmic_read(S2_VOLTAGE, &code) || code < TURBO_LOGIC)
        return false;
    clock.turbo = true;
    return true;
}

/* Every core and the L2 to a point; after a failure the clocks stay at
 * the floor, or where they are below it. */
static void move(unsigned target) {
    const struct point *to = at(target), *from = at(clock.point);
    if (target == clock.point) return;
    if (target > clock.point) {
        if ((target > FLOOR && !logic()) || !rail(clock.mv[target])) {
            clock.governed = false;
            return;
        }
        bool ok = cores_and_l2(to, from);
        clock.point = target;
        if (!ok) {
            clock.governed = false;
            if (target > FLOOR) move(FLOOR);
        }
        return;
    }
    bool ok = cores_and_l2(to, from);
    clock.point = target;
    (void)rail(clock.mv[target]);
    if (!ok) clock.governed = false;
}

bool nexus5_cpu_clock_init(void) {
    if (clock.ready) return true;
    /* Only from the bootloader's rates, which this table starts from. */
    if ((mmio_read32(HFPLL0 + PLL_L) & 0xff) != BOOT_CPU_L ||
        (mmio_read32(HFPLL_L2 + PLL_L) & 0xff) != BOOT_L2_L)
        return false;
    nexus5_store(GPLL0_VOTE, mmio_read32(GPLL0_VOTE) | 1);
    if (!nexus5_wait_bits(GPLL0_STATUS, GPLL0_ACTIVE, true, LOCK_US)) return false;
    uint8_t range = 0xff, now = 0;
    if (!nexus5_pmic_read(S5_RANGE, &range) || range || !nexus5_pmic_read(S5_VOLTAGE, &now) ||
        now != BOOT_VOLTAGE)
        return false;
    select_table();
    static const struct point boot = {BOOT_CPU_L, BOOT_L2_L};
    clock.floor = points[FLOOR];
    clock.started = 1;
    clock.governed = true;
    /* A floor the rail cannot carry keeps the bootloader's rates. */
    if (!rail(clock.mv[FLOOR])) {
        clock.floor = boot;
        clock.governed = false;
    } else if (!cores_and_l2(&clock.floor, &boot))
        return false;
    /* The sensors are ready once their first round is in: a round takes
     * 62.5 ms. */
    clock.thermal = nexus5_thermal_init(70000);
    clock.cap = clock.top;
    clock.ceiling = clock.top;
    clock.point = FLOOR;
    clock.ready = true;
    return true;
}

bool nexus5_cpu_clock_start(unsigned cpu) {
    if (!clock.ready || !cpu || cpu > 3) return false;
    uint32_t cpmr = CPMR0 + 0x1000u * cpu;
    uintptr_t pll = HFPLL0 + 0x10000u * cpu;
    park(cpmr, pll);
    if (!run(cpmr, pll, at(clock.point)->cpu_l)) return false;
    clock.started |= 1u << cpu;
    return true;
}

/* The temperature watch: the cap, and the point within it. */
static void watch(void) {
    int degrees;
    if (!nexus5_cpu_temperature(&degrees) || degrees >= EMERGENCY_C) clock.cap = 0;
    else if (degrees >= HOT_C) {
        if (clock.cap > FLOOR) --clock.cap;
    } else if (degrees < COOL_C && clock.cap < clock.top)
        ++clock.cap;
    if (clock.point > clock.cap) move(clock.cap);
}

void nexus5_cpu_clock_touch(void) { clock.touched_at = loop_time_us(); }

void nexus5_cpu_clock_poll(uint64_t user_us) {
    if (!clock.ready || !clock.governed) return;
    uint64_t now = loop_time_us();
    /* Without the sensors' calibration the cores keep the floor; sensors
     * that were not ready at boot are looked at again each second. */
    if (!clock.thermal && now - clock.watch_at >= THERMAL_RETRY_US) {
        clock.watch_at = now;
        clock.thermal = nexus5_thermal_init(0);
    }
    if (clock.thermal && now - clock.watch_at >= WATCH_US) {
        clock.watch_at = now;
        watch();
    }
    if (!clock.thermal || now - clock.window_at < WINDOW_US) return;
    bool busy = user_us - clock.window_user >= BUSY_US;
    clock.window_at = now;
    clock.window_user = user_us;
    bool was_working = now - clock.busy_at < IDLE_US;
    if (busy) clock.busy_at = now;
    bool working = busy || was_working;
    if (!was_working || !working || now - clock.touched_at < TOUCH_US) {
        clock.working_since = now;
        clock.ceiling = clock.top;
    } else if (now - clock.working_since >= SUSTAINED_US && now - clock.stepped_at >= STEP_US &&
               clock.ceiling > FLOOR) {
        --clock.ceiling;
        clock.stepped_at = now;
    }
    unsigned target = clock.cap < clock.ceiling ? clock.cap : clock.ceiling;
    move(working || clock.cap < FLOOR ? target : FLOOR);
}

void nexus5_cpu_clock_standby(bool on) {
    if (!clock.ready) return;
    if (on) {
        if (clock.point > FLOOR) move(FLOOR);
        park(CPMR0, HFPLL0);
        park(CPMR_L2, HFPLL_L2);
        return;
    }
    /* A PLL that does not lock leaves its clock on the auxiliary source. */
    (void)run(CPMR_L2, HFPLL_L2, at(clock.point)->l2_l);
    (void)run(CPMR0, HFPLL0, at(clock.point)->cpu_l);
}
