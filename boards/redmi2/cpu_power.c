#include "redmi2_cpu_power.h"
#include "platform.h"

/* Standalone power collapse of an MSM8916 core, as the vendor kernel
 * hotplugs one (msm-pm.c msm_pm_pc_hotplug) or idles one (msm_pm_collapse),
 * and mainline Linux v6.6 idles one on 32-bit MSM8916 (cpuidle-qcom-spm.c
 * qcom_cpu_spc with spm.c's 8916 data):
 * - each core's SAW2 v3.0 power controller (0x0b089000 + 0x10000 n; version
 *   at 0xfd0, major in bits 31:28, as the vendor's spm.c reads it; CFG 0x08,
 *   SPM_CTL 0x30, DLY 0x34, sequence at 0x400) gets spm.c's 8916 sequence,
 *   padded to 64 bytes, then CFG 1 and DLY 0x3c102800, then standby (start
 *   index 0) as its mode: sequence first, so a reset cannot start a partial one;
 * - to power down, the core selects the power-collapse sequence (index 5),
 *   takes the secure monitor's hand-off lock (TCSR mutex 7 at 0x0190c000,
 *   with 0x80 + its number), which the monitor releases, so that it sees the
 *   cores go down one at a time (lpm-levels.c lpm_cpu_pre_pc_cb takes it
 *   before every power collapse; one left held is released on the way back),
 *   cleans its L1 with caching on, as msm-pm.c flushes it before
 *   TERMINATE_PC (lines dirtied after that hold only dead stack), and calls
 *   SCM_CMD_TERMINATE_PC with L2 on (0): a secondary as
 *   SCM_CMD_CORE_HOTPLUGGED (0x10), restarting from scratch, CPU 0 as idle
 *   (mainline's QCOM_SCM_CPU_PWR_DOWN_L2_ON), resuming its saved context. If
 *   the firmware returns, an interrupt was pending: standby comes back; back
 *   from a power-down, standby comes back too, as qcom_cpu_spc restores it;
 * - SGI 0, the interrupt that releases a core at cold boot, powers a
 *   secondary up; the firmware then enters the warm-boot address. The core
 *   enables SGI 0 for itself before powering down, at Linux's default priority
 *   0xa0, as Linux's gic_cpu_config enables SGIs on every CPU (SOS2 otherwise
 *   leaves them alone), and disables it again once restarted, so a late
 *   wake-up SGI can neither disturb the restart nor reach a running core;
 * - CPU 0 wakes from frame 0 of the memory-mapped timer (0x0b020000 control,
 *   frame at 0x0b021000; physical SPI 8, virtual SPI 7, level, as the stock
 *   and mainline device trees list them), set up as Linux's arm_arch_timer.c
 *   chooses a frame: all of CNTACR's access bits tried, the virtual timer if
 *   CNTTIDR offers it and the bits stick, else the physical one. Its interrupt
 *   is enabled only while CPU 0 is down, so it never reaches the scheduler.
 *   Boot checks live that it reaches CPU 0's interface, as the scheduler
 *   timer's route is checked: otherwise a powered-down CPU 0 would never wake.
 *   A clock that jumps across a power-down stops further ones. CPU 0's
 *   banked GIC state (CPU interface control, priority mask and binary point;
 *   SGI/PPI enables, priorities and PPI configuration) is saved before and
 *   written back after, as Linux's gic_cpu_save/gic_cpu_restore and
 *   gic_cpu_if_up do around a CPU power-down. One more level-triggered SPI
 *   (the PMIC arbiter's, for the power key) can wake it too: routed to CPU 0
 *   the same way, enabled only while CPU 0 is down and only if its line is
 *   low as CPU 0 goes down, so a stuck line cannot keep CPU 0 up;
 * - ACC + 8 bit 18 reports a core collapsed (the vendor's
 *   qcom,cpu-sleep-status, read by msm_pm_wait_cpu_shutdown).
 * The L2 cache can go off with CPU 0, as the vendor's lpm-levels.c takes
 * the cluster down when its last CPU suspends (level l2-pc of
 * msm8916-pm.dtsi):
 * - boot programs the L2's SAW2 v3.0 (0x0b012000) as spm.c programs it from
 *   that device tree: CFG 0x1f, the AVS registers cleared, SPM_CTL 0x0e
 *   (disabled), SPM_DLY 0x3c102800, PMIC data 0, 1, 4 and 5 (the APC rail's
 *   commands), and its retention, GDHS and power-collapse sequences packed
 *   from 0x400 (starting at 0, 4 and 28), the rest of the entries zero; it
 *   routes the RPM's wake interrupt to CPU 0 (SPI 171, rising edge,
 *   mpm-of.c), enabled only during such a power-down;
 * - with the three secondaries down, the L2's SAW is enabled with power
 *   collapse (the L2 and the APC rail off) and the PC mode bit, CPU 0's SAW
 *   gets the vendor's collapse value (0x0001005f), and the collapse cleans
 *   every cache level (msm_pm_collapse's flush_cache_all) and calls
 *   TERMINATE_PC with L2 off (1). The sequence hands the chip to the RPM,
 *   which applies the sleep set and wakes it through the MPM (programmed by
 *   chip_sleep.c);
 * - back, CPU 0's SAW gets its value from before and the L2's SAW is
 *   disabled again. */
#include "arm_cache.h"
#include "smp.h"
#include "qcom_scm.h"

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

#define clean_l1     arm_dcache_clean_invalidate_local
#define power_down   qcom_scm_cpu_power_down
#define cpu0_suspend arm_cpu0_suspend
#define runtime_init user_runtime_init

enum {
    SAW = 0x0b089000,
    SAW_STRIDE = 0x10000,
    SAW_CFG = 0x08,
    SAW_CTL = 0x30,
    SAW_DLY = 0x34,
    SAW_SEQ = 0x400,
    SAW_VERSION = 0xfd0,
    CTL_EN = 1,
    CTL_INDEX_SHIFT = 4,
    CTL_INDEX = 0x7fu << 4,
    STANDBY = 0,
    COLLAPSE = 5,
    SEQ_WORDS = 16,
    ACC = 0x0b088000,
    ACC_STRIDE = 0x10000,
    ACC_SLEEP_STATUS = 0x08,
    COLLAPSED = 1u << 18,
    GICD = 0x0b000000,
    GICD_ISENABLER = 0x100,
    GICD_ICENABLER = 0x180,
    GICD_ISPENDR = 0x200,
    GICD_ICPENDR = 0x280,
    GICD_IPRIORITYR = 0x400,
    GICD_ITARGETSR = 0x800,
    GICD_ICFGR = 0xc00,
    GICD_SGIR = 0xf00,
    GICD_CPENDSGIR = 0xf10,
    GICC_HPPIR = 0x0b002018,
    SPURIOUS = 1023,
    PROBE_US = 1000,
    PROBE_LIMIT_US = 20000,
    JUMP_US = 1000000,
    GICC_CTLR = 0x0b002000,
    GICC_PMR = 0x0b002004,
    GICC_BPR = 0x0b002008,
    PRIVATE_PRIORITIES = 8,
    SGI0 = 0,
    PRIORITY = 0xa0,
    TIMER_CONTROL = 0x0b020000,
    CNTTIDR = 0x08,
    CNTACR0 = 0x40,
    FRAME = 0x0b021000,
    CNTPCT = 0x00,
    CNTVCT = 0x08,
    CNTFRQ = 0x10,
    CNTP_CVAL = 0x20,
    CNTP_CTL = 0x2c,
    CNTV_CVAL = 0x30,
    CNTV_CTL = 0x3c,
    RPCT = 1,
    RVCT = 2,
    RFRQ = 4,
    RVOFF = 8,
    RWVT = 16,
    RWPT = 32,
    FRAME0_VIRTUAL = 2,
    TIMER_ENABLE = 1,
    PHYSICAL_SPI = 32 + 8,
    VIRTUAL_SPI = 32 + 7,
    HOTPLUGGED = 0x10,
    L2_ON = 0,
    L2_SAW = 0x0b012000,
    SAW_ID = 0x04,
    SAW_AVS = 0x20,
    SAW_PMIC_DATA = 0x40,
    L2_CFG = 0x1f,
    CTL_VENDOR = 0x0e,
    PC_MODE = 1u << 16,
    L2_PC = 28,
    L2_OFF_FLAG = 1,
    CPU_COLLAPSE = PC_MODE | CTL_VENDOR | (COLLAPSE << 4) | CTL_EN,
    L2_COLLAPSE = PC_MODE | CTL_VENDOR | (L2_PC << 4),
    MPM_WAKE = 32 + 171,
    HANDOFF = 0x0190c000,
    HANDOFF_TOKEN = 0x80,
    HANDOFF_US = 10000,
};

static const unsigned char sequence[] = {0x60, 0x03, 0x60, 0x0b, 0x0f, 0x20, 0x10, 0x80, 0x30,
                                         0x90, 0x5b, 0x60, 0x03, 0x60, 0x3b, 0x76, 0x76, 0x0b,
                                         0x94, 0x5b, 0x80, 0x10, 0x26, 0x30, 0x0f};
/* The L2's sequences, packed as spm.c packs them: retention (0), GDHS (4),
 * power collapse (28), each ending with 0x0f. */
static const unsigned char l2_sequence[] = {
    0x00,0x03,0x00,0x0f,
    0x00,0x20,0x32,0x6b,0xc0,0xe0,0xd0,0x42,0xf0,0x03,0x50,0x4e,0x02,0x02,0xd0,0xe0,0xc0,0x22,0x6b,0x02,0x32,0x52,0xf0,0x0f,
    0x00,0x32,0xb0,0x10,0xe0,0xd0,0x6b,0xc0,0x42,0x51,0x11,0x07,0x01,0x41,0xb0,0x50,0x4e,0x02,0x02,0xc0,0xd0,0x12,0xe0,0x6b,0x02,0x32,0x50,0x0f
};
static const uint32_t l2_pmic_data[8] = {0x05030080, 0x00030000, 0, 0,
                                         0x00010080, 0x00010000, 0, 0};
static bool l2_ready;
static unsigned ready;
/* CPU 0's wake-up: timer frame registers, interrupt and counter frequency. */
static uintptr_t count_reg, cval_reg, ctl_reg;
static unsigned wake_irq;
static uint32_t frequency, timer_access;
static bool cpu0_ready;
/* The extra wake-up interrupt, and whether its line was up around the last
 * power-down. */
static unsigned source_irq;
static bool source_seen;

/* spm.c spm_set_low_power_mode and spm_register_write_sync, bounded. */
static bool mode(unsigned cpu, unsigned index) {
    uintptr_t ctl = SAW + cpu * SAW_STRIDE + SAW_CTL;
    uint32_t value = (read32(ctl) & ~(uint32_t)CTL_INDEX) | (index << CTL_INDEX_SHIFT) | CTL_EN;
    for (unsigned i = 0; i < 1000; ++i) {
        write32(ctl, value);
        if (read32(ctl) == value) return true;
    }
    return false;
}

/* GICv2 byte-per-interrupt fields, through their 32-bit words. */
static void irq_byte(uintptr_t reg, unsigned irq, uint32_t value) {
    uintptr_t word = GICD + reg + (irq & ~3u);
    unsigned shift = 8 * (irq & 3);
    write32(word, (read32(word) & ~(0xffu << shift)) | (value << shift));
}

static void irq_bit(uintptr_t reg, unsigned irq) {
    write32(GICD + reg + 4 * (irq / 32), 1u << (irq % 32));
}

/* Level-triggered, to CPU 0, at Linux's default priority. */
static void route_to_cpu0(unsigned irq) {
    uintptr_t config = GICD + GICD_ICFGR + 4 * (irq / 16);
    write32(config, read32(config) & ~(2u << (2 * (irq % 16))));
    irq_byte(GICD_IPRIORITYR, irq, PRIORITY);
    irq_byte(GICD_ITARGETSR, irq, 1);
}

/* A level interrupt is pending while its line is up, enabled or not. */
static bool raised(unsigned irq) {
    return read32(GICD + GICD_ISPENDR + 4 * (irq / 32)) & (1u << (irq % 32));
}

unsigned redmi2_cpu_power_init(unsigned mask) {
    ready = 0;
    cpu0_ready = false;
    for (unsigned cpu = 0; cpu < 4; ++cpu) {
        if (!(mask & (1u << cpu))) continue;
        uintptr_t saw = SAW + cpu * SAW_STRIDE;
        if ((read32(saw + SAW_VERSION) >> 28) != 3) continue;
        for (unsigned i = 0; i < SEQ_WORDS; ++i) {
            uint32_t word = 0;
            for (unsigned b = 0; b < 4; ++b) {
                unsigned at = i * 4 + b;
                if (at < sizeof(sequence)) word |= (uint32_t)sequence[at] << (8 * b);
            }
            write32(saw + SAW_SEQ + 4 * i, word);
        }
        write32(saw + SAW_CFG, 1);
        write32(saw + SAW_DLY, 0x3c102800);
        if (mode(cpu, STANDBY)) ready |= 1u << cpu;
    }
    return ready;
}

/* On the CPU itself, with the collapse sequence selected: the hand-off lock,
 * the caches (every level with the L2 going off), then the call, which never
 * returns if the firmware powers the CPU down. */
static void collapse(unsigned cpu, uint32_t flags) {
    uint32_t token = HANDOFF_TOKEN + cpu;
    uint64_t start = time_us();
    do write32(HANDOFF, token);
    while (read32(HANDOFF) != token && time_us() - start < HANDOFF_US);
    if (read32(HANDOFF) != token) return;
    if (flags & L2_OFF_FLAG) arm_dcache_clean_invalidate_all();
    else clean_l1();
    power_down(cpu, flags);
}

void redmi2_cpu_power_off(unsigned cpu) {
    if (!cpu || cpu >= 4 || !(ready & (1u << cpu))) return;
    /* A stale wake-up SGI would abort the collapse at once; then the next
     * one must reach this CPU. These GIC registers are banked per CPU. */
    write32(GICD + GICD_CPENDSGIR, 0xff);
    irq_byte(GICD_IPRIORITYR, SGI0, PRIORITY);
    irq_bit(GICD_ISENABLER, SGI0);
    if (mode(cpu, COLLAPSE)) collapse(cpu, HOTPLUGGED | L2_ON);
    redmi2_cpu_power_resumed(cpu);
}

void redmi2_cpu_power_resumed(unsigned cpu) {
    if (cpu < 4 && (ready & (1u << cpu))) (void)mode(cpu, STANDBY);
    if (cpu < 4 && read32(HANDOFF) == HANDOFF_TOKEN + cpu)
        write32(HANDOFF, 0); /* Left held by the monitor. */
    irq_bit(GICD_ICENABLER, SGI0);
    write32(GICD + GICD_CPENDSGIR, 0xff);
}

bool redmi2_cpu_power_collapsed(unsigned cpu) {
    return cpu && cpu < 4 && (ready & (1u << cpu)) &&
           (read32(ACC + cpu * ACC_STRIDE + ACC_SLEEP_STATUS) & COLLAPSED);
}

void redmi2_cpu_power_wake(unsigned mask) {
    mask &= ready & 0xeu;
    if (mask) write32(GICD + GICD_SGIR, mask << 16); /* SGI 0, target list. */
}

static void wake_irq_off(void) {
    write32(ctl_reg, 0); /* The level interrupt drops with the timer. */
    irq_bit(GICD_ICENABLER, wake_irq);
    irq_bit(GICD_ICPENDR, wake_irq);
}

static uint64_t frame_count(void) {
    uint32_t high, low, again;
    do {
        high = read32(count_reg + 4);
        low = read32(count_reg);
        again = read32(count_reg + 4);
    } while (high != again);
    return (uint64_t)high << 32 | low;
}

static uint64_t frame_ticks(uint32_t microseconds) {
    return (uint64_t)microseconds * frequency / 1000000u;
}

static void wake_at(uint64_t when) {
    write32(cval_reg, (uint32_t)when);
    write32(cval_reg + 4, (uint32_t)(when >> 32));
    irq_bit(GICD_ICPENDR, wake_irq);
    write32(ctl_reg, TIMER_ENABLE);
    irq_bit(GICD_ISENABLER, wake_irq);
}

/* With CPU 0's interrupts masked: the timer fires in 1 ms; its interrupt must
 * become CPU 0's highest pending one within 20 ms. */
static bool wake_probe(void) {
    uint64_t start = frame_count();
    bool seen = false;
    wake_at(start + frame_ticks(PROBE_US));
    while (!seen && frame_count() - start < frame_ticks(PROBE_LIMIT_US))
        seen = (read32(GICC_HPPIR) & SPURIOUS) == wake_irq;
    wake_irq_off();
    return seen && (read32(GICC_HPPIR) & SPURIOUS) == SPURIOUS;
}

bool redmi2_cpu0_power_init(void) {
    cpu0_ready = false;
    if (!(ready & 1)) return false;
    uint32_t features = read32(TIMER_CONTROL + CNTTIDR);
    write32(TIMER_CONTROL + CNTACR0, RPCT | RVCT | RFRQ | RVOFF | RWVT | RWPT);
    uint32_t access = timer_access = read32(TIMER_CONTROL + CNTACR0);
    if ((features & FRAME0_VIRTUAL) && (access & (RWVT | RVCT)) == (RWVT | RVCT)) {
        count_reg = FRAME + CNTVCT;
        cval_reg = FRAME + CNTV_CVAL;
        ctl_reg = FRAME + CNTV_CTL;
        wake_irq = VIRTUAL_SPI;
    } else if ((access & (RWPT | RPCT)) == (RWPT | RPCT)) {
        count_reg = FRAME + CNTPCT;
        cval_reg = FRAME + CNTP_CVAL;
        ctl_reg = FRAME + CNTP_CTL;
        wake_irq = PHYSICAL_SPI;
    } else {
        return false;
    }
    frequency = (access & RFRQ) ? read32(FRAME + CNTFRQ) : 0;
    if (!frequency || frequency > 100000000) frequency = 19200000;
    wake_irq_off();
    route_to_cpu0(wake_irq);
    if (!wake_probe()) return false;
    cpu0_ready = true;
    return true;
}

static void cpu0_down(void) {
    if (mode(0, COLLAPSE)) collapse(0, L2_ON);
}

static struct {
    uint32_t ctlr, pmr, bpr, enabled, config, priority[PRIVATE_PRIORITIES];
} gic;

static void gic_save(void) {
    gic.ctlr = read32(GICC_CTLR);
    gic.pmr = read32(GICC_PMR);
    gic.bpr = read32(GICC_BPR);
    gic.enabled = read32(GICD + GICD_ISENABLER);
    gic.config = read32(GICD + GICD_ICFGR + 4);
    for (unsigned i = 0; i < PRIVATE_PRIORITIES; ++i)
        gic.priority[i] = read32(GICD + GICD_IPRIORITYR + 4 * i);
}

static void gic_restore(void) {
    for (unsigned i = 0; i < PRIVATE_PRIORITIES; ++i)
        write32(GICD + GICD_IPRIORITYR + 4 * i, gic.priority[i]);
    write32(GICD + GICD_ICFGR + 4, gic.config);
    write32(GICD + GICD_ICENABLER, ~gic.enabled);
    write32(GICD + GICD_ISENABLER, gic.enabled);
    write32(GICC_PMR, gic.pmr);
    write32(GICC_BPR, gic.bpr);
    write32(GICC_CTLR, gic.ctlr);
}

void redmi2_cpu0_wake_source(unsigned irq) {
    if (source_irq) irq_bit(GICD_ICENABLER, source_irq);
    source_irq = 0;
    if (!cpu0_ready || irq < 32 || irq >= 1020) return;
    irq_bit(GICD_ICENABLER, irq);
    route_to_cpu0(irq);
    source_irq = irq;
}

bool redmi2_cpu0_source_raised(void) { return source_irq && raised(source_irq); }

bool redmi2_cpu0_source_seen(void) { return source_seen; }

bool redmi2_cpu0_sleep(uint32_t microseconds) {
    if (!cpu0_ready || !microseconds) return false;
    uint64_t start = time_us();
    source_seen = source_irq && raised(source_irq);
    bool source = source_irq && !source_seen;
    if (source) irq_bit(GICD_ISENABLER, source_irq);
    gic_save();
    wake_at(frame_count() + frame_ticks(microseconds));
    bool resumed = !cpu0_suspend(cpu0_down);
    if (resumed) gic_restore();
    wake_irq_off();
    if (source) {
        source_seen = raised(source_irq);
        irq_bit(GICD_ICENABLER, source_irq);
    }
    redmi2_cpu_power_resumed(0);
    /* Kept up by the extra line: return at once, so the caller polls now. */
    if (!resumed) return source && source_seen;
    runtime_init(); /* The power-up cleared the VFP enable. */
    uint64_t now = time_us();
    if (now < start || now - start > (uint64_t)microseconds + JUMP_US) {
        cpu0_ready = false;
        return true;
    }
    return true;
}

/* spm.c spm_register_write_sync, bounded. */
static bool sync_write(uintptr_t reg, uint32_t value) {
    for (unsigned i = 0; i < 1000; ++i) {
        write32(reg, value);
        if (read32(reg) == value) return true;
    }
    return false;
}

uint64_t redmi2_cpu0_count(void) {
    if (!cpu0_ready || !(timer_access & RPCT)) return 0;
    uint32_t high, low, again;
    do {
        high = read32(FRAME + CNTPCT + 4);
        low = read32(FRAME + CNTPCT);
        again = read32(FRAME + CNTPCT + 4);
    } while (high != again);
    return (uint64_t)high << 32 | low;
}

bool redmi2_l2_power_init(void) {
    l2_ready = false;
    if (!cpu0_ready || !(timer_access & RPCT)) return false;
    if ((read32(L2_SAW + SAW_VERSION) >> 28) != 3) return false;
    unsigned entries = read32(L2_SAW + SAW_ID) >> 24;
    if (entries < (sizeof(l2_sequence) + 3) / 4 || entries > 64) return false;
    write32(L2_SAW + SAW_CFG, L2_CFG);
    for (unsigned i = 0; i < 4; ++i) write32(L2_SAW + SAW_AVS + 4 * i, 0);
    if (!sync_write(L2_SAW + SAW_CTL, CTL_VENDOR)) return false;
    write32(L2_SAW + SAW_DLY, 0x3c102800);
    for (unsigned i = 0; i < 8; ++i) write32(L2_SAW + SAW_PMIC_DATA + 4 * i, l2_pmic_data[i]);
    for (unsigned i = 0; i < entries; ++i) {
        uint32_t word = 0;
        for (unsigned b = 0; b < 4; ++b) {
            unsigned at = i * 4 + b;
            if (at < sizeof(l2_sequence)) word |= (uint32_t)l2_sequence[at] << (8 * b);
        }
        write32(L2_SAW + SAW_SEQ + 4 * i, word);
    }
    /* The RPM's wake interrupt: rising edge, to CPU 0, at Linux's priority. */
    irq_bit(GICD_ICENABLER, MPM_WAKE);
    uintptr_t config = GICD + GICD_ICFGR + 4 * (MPM_WAKE / 16);
    write32(config, read32(config) | (2u << (2 * (MPM_WAKE % 16))));
    irq_byte(GICD_IPRIORITYR, MPM_WAKE, PRIORITY);
    irq_byte(GICD_ITARGETSR, MPM_WAKE, 1);
    irq_bit(GICD_ICPENDR, MPM_WAKE);
    l2_ready = true;
    return true;
}

/* As msm_pm_collapse with the L2 going off. */
static void cpu0_down_deep(void) {
    if (sync_write(SAW + SAW_CTL, CPU_COLLAPSE)) collapse(0, L2_OFF_FLAG);
}

int redmi2_cpu0_deep_sleep(uint32_t microseconds) {
    if (!cpu0_ready || !l2_ready || !microseconds) return -1;
    uint64_t start = time_us();
    uint32_t cpu_ctl = read32(SAW + SAW_CTL);
    if (!sync_write(L2_SAW + SAW_CTL, L2_COLLAPSE | CTL_EN)) {
        (void)sync_write(L2_SAW + SAW_CTL, CTL_VENDOR);
        l2_ready = false;
        return -1;
    }
    irq_bit(GICD_ICPENDR, MPM_WAKE);
    irq_bit(GICD_ISENABLER, MPM_WAKE);
    source_seen = source_irq && raised(source_irq);
    bool source = source_irq && !source_seen;
    if (source) irq_bit(GICD_ISENABLER, source_irq);
    gic_save();
    wake_at(frame_count() + frame_ticks(microseconds));
    bool resumed = !cpu0_suspend(cpu0_down_deep);
    if (resumed) gic_restore();
    wake_irq_off();
    if (source) {
        source_seen = raised(source_irq);
        irq_bit(GICD_ICENABLER, source_irq);
    }
    (void)sync_write(SAW + SAW_CTL, cpu_ctl);
    irq_bit(GICD_ICENABLER, MPM_WAKE);
    irq_bit(GICD_ICPENDR, MPM_WAKE);
    (void)sync_write(L2_SAW + SAW_CTL,
                     L2_COLLAPSE); /* Disabled, the rest kept (the vendor's shadow). */
    redmi2_cpu_power_resumed(0);
    if (!resumed) return 0;
    runtime_init();
    uint64_t now = time_us();
    if (now < start || now - start > (uint64_t)microseconds + JUMP_US) {
        cpu0_ready = false;
        return 1;
    }
    return 1;
}

unsigned redmi2_cpu0_wake_irq(void) { return cpu0_ready ? wake_irq : 0; }
