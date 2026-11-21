#include "redmi2_display.h"
#include "platform.h"

/* Panel standby. The panel gets "display off" and "enter sleep mode" (MIPI
 * DCS 0x28, 0x10: the off commands of both Redmi 2 panels in lk2nd, R61308
 * and HX8394D, with R61308's 50 and 120 ms waits); waking sends "exit sleep
 * mode" and "display on" (0x11, 0x29: 120 ms after exit, as R61308's init).
 * In sleep mode the panel keeps its configuration and ignores the stream.
 * Commands go as Linux v6.6 drivers/gpu/drm/msm/dsi/dsi_host.c sends them to a
 * video-mode panel: msm_dsi_host_xfer_prepare adds command mode to DSI_CTRL
 * for the transfer (lk2nd leaves it off after the panel's init commands, and
 * the command DMA does not run without it; lk2nd's mdss_dsi_panel_initialize
 * sets it the same way), dsi_wait4video_eng_busy waits for the frame's video
 * to end and skips the blanking packets, then the command DMA from memory
 * runs and msm_dsi_host_xfer_restore puts DSI_CTRL back. Registers from
 * dsi.xml.h; MSM8916's 6G DSI controller at 0x1a98000 has them 4 bytes in, as
 * lk2nd's msm8916 iomap.h lists them. Kernel data is identity-mapped and
 * uncached, so the command word needs no cache maintenance.
 *
 * Below the sleeping panel, standby turns the display pipeline off, in the
 * order Linux v6.6 takes a DSI display down
 * (mdp5_vid_encoder_disable, msm_dsi_host_disable and _power_off,
 * dsi_phy_28nm.c, the MDSS runtime suspend):
 * - the MDP's timing generator stops at a frame's end (a CTL flush latches
 *   it; INTF1's frame counter, Linux's vblank counter, shows it stopped);
 * - the DSI controller leaves video mode and is soft-reset with its clocks
 *   forced on, because its video engine does not stop otherwise;
 * - the link clocks (escape, pixel, byte), the DSI PLL and the PHY power
 *   down, then the MDSS core clocks (MDP, vsync, AXI, AHB last).
 * The MDSS power domain stays on, so every register keeps its value. R69431
 * needs 220 ms after "enter sleep mode" before its link stops.
 * The R61308 panels (vendor dsi-panel-r61308-720p-video.dtsi and its
 * s88047a1 variant, AUO; known by INTF1's line: 720 pixels and porches, 892
 * or 986 clocks, pulse 2) are also held in reset under it, as the vendor
 * kernel leaves a panel with the screen off (mdss_dsi_panel_reset(0), the
 * reset line GPIO 25 low), once the pipeline is off: asleep but out of reset
 * the panel drew about 4 mA, and its supply (L17) then goes off while the
 * chip sleeps (rails.c). Waking brings the panel back as the vendor powers
 * it on (mdss_dsi_on with lp11-init, then mdss_dsi_panel_on): with the lanes
 * in LP-11, the line high 5 ms, low 10 ms, high 30 ms (reset-sequence),
 * 50 ms (init-delay-us), then its on-commands, identical in both files and
 * in lk2nd's lk_panel_r61308_720p_video.h, in low-power command mode with
 * their waits, "exit sleep mode" first and "display on" last. Another panel
 * keeps its supplies and reset line.
 * Waking reverses this as Linux and lk2nd power a display on: core clocks,
 * the PHY regulator and PHY, the PLL relocked with lk2nd's 8916 sequences
 * (the boot's calibration first, then TSMC, GF1, GF2), the link clocks, a
 * DSI soft reset (any setting it lost is written back), the lanes in LP-11,
 * video mode, then the timing generator with the whole path flushed. The
 * panel wakes only once frames run. If that fails, the PHY and PLL are reset
 * and reprogrammed from the values read at boot in lk2nd's order
 * (mdss_dsi_auto_pll_config, mdss_dsi_phy_28nm_init), as both do on every
 * power-on. If that fails too, the pipeline stays off and each wake tries
 * again. A pipeline that is not as lk2nd leaves it at boot keeps running, and
 * if frames do not stop, the clocks stay on.
 * Addresses: Linux gcc-msm8916.c, mdp5.xml.h with mdp5_cfg.c's msm8x16
 * offsets, dsi.xml.h, dsi_phy_28nm.xml.h and msm8916.dtsi; lk2nd's
 * mipi_dsi.h for the DSI timing double buffer. */
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

#define CLOCK_OFF 0x80000000u /* GCC branch status: the clock is not running. */
#define DOMAIN_ON 0x80000000u /* GDSCR status: the power domain is on. */

enum {
    DSI_BASE = 0x01a98000,
    DSI = DSI_BASE + 4,
    DSI_CTRL = 0x00,
    DSI_STATUS0 = 0x04,
    DSI_FIFO_STATUS = 0x08,
    DSI_VID_CFG0 = 0x0c,
    DSI_VID_CFG1 = 0x1c,
    DSI_ACTIVE_H = 0x20,
    DSI_ACTIVE_V = 0x24,
    DSI_TOTAL = 0x28,
    DSI_ACTIVE_HSYNC = 0x2c,
    DSI_VSYNC_HPOS = 0x30,
    DSI_VSYNC_VPOS = 0x34,
    DSI_CMD_DMA_CTRL = 0x38,
    DSI_DMA_BASE = 0x44,
    DSI_DMA_LEN = 0x48,
    DSI_TRIG_CTRL = 0x80,
    DSI_TRIG_DMA = 0x8c,
    DSI_LANE_CTRL = 0xa8,
    DSI_LANE_SWAP = 0xac,
    DSI_DLN0_PHY_ERR = 0xb0,
    DSI_HS_TIMER = 0xb8,
    DSI_CLKOUT_TIMING = 0xc0,
    DSI_EOT = 0xc8,
    DSI_ERR_MASK = 0x108,
    DSI_INTR_CTRL = 0x10c,
    DSI_RESET = 0x114,
    DSI_CLK_CTRL = 0x118,
    DSI_CLK_STATUS = 0x11c,
    DSI_PHY_RESET = 0x128,
    DSI_TIMING_FLUSH = 0x1e0,
    DSI_TIMING_DB = 0x1e4,
    CTRL_ENABLE = 1,
    CTRL_VIDEO = 2,
    CTRL_COMMAND = 4,
    STATUS_DMA_BUSY = 2,
    STATUS_VIDEO_BUSY = 8,
    DMA_FROM_FRAME_BUFFER = 1u << 28,
    TRIGGER_MASK = 7,
    TRIGGER_SW = 4,
    PLL_UNLOCKED = 1u << 16,
    DSI_CLOCKS_ON = 0x23f,
    /* Interrupt status bits are write-one-to-clear; each has its enable above it. */
    DMA_DONE = 1,
    DMA_DONE_ENABLE = 2,
    VIDEO_DONE = 1u << 16,
    VIDEO_DONE_ENABLE = 1u << 17,
    STATUS_BITS = 1u | (1u << 8) | (1u << 16) | (1u << 20) | (1u << 24),
    /* GCC's MDSS clocks (branch CBCRs) and power domain. */
    GCC_MDSS = 0x0184d000,
    GDSCR = 0x078,
    GDSC_HW_CONTROL = 2,
    AHB = 0x07c,
    AXI = 0x080,
    PCLK0 = 0x084,
    MDP_CORE = 0x088,
    VSYNC = 0x090,
    BYTE0 = 0x094,
    ESC0 = 0x098,
    /* MDP 1.6: top, CTL0 and INTF1 (the path RGB0, LM0, INTF1, DSI0). */
    MDP = 0x01a01000,
    MDP_INTR_CLEAR = 0x18,
    INTF1_UNDERRUN = 1u << 26,
    INTF1_VSYNC = 1u << 27,
    CTL0 = 0x01a02000,
    CTL_LAYER0 = 0x00,
    CTL_OP = 0x14,
    CTL_FLUSH = 0x18,
    FLUSH_TIMING_1 = 1u << 30,
    FLUSH_PATH = 0x40022048, /* TIMING_1, CTL, DSPP0, LM0, RGB0: lk2nd's mask */
    INTF1 = 0x01a6b800,
    TG_ENABLE = 0x00,
    INTF_HSYNC = 0x08,
    INTF_COUNTERS = 0xa8,
    INTF_FRAMES = 0xac,
    /* The 28 nm LP DSI PLL, PHY and PHY regulator. */
    PLL = 0x01a98300,
    PLL_GLB_CFG = 0x20,
    PLL_SDM_CFG4 = 0x48,
    PLL_LKDET_CFG2 = 0x64,
    PLL_TEST_CFG = 0x68,
    PLL_CAL_CFG1 = 0x70,
    PLL_STATUS = 0xc0,
    PHY = 0x01a98500,
    PHY_CTRL_0 = 0x170,
    PHY_CTRL_1 = 0x174,
    PHY_CTRL_4 = 0x180,
    PHY_STRENGTH_0 = 0x184,
    PHY_STRENGTH_1 = 0x188,
    PHY_GLBL_TEST = 0x1d4,
    PHY_LDO = 0x1dc,
    REGULATOR = 0x01a98780,
    REGULATOR_CAL = 0x18,
    DISPLAY_OFF = 0x28,
    SLEEP_IN = 0x10,
    SLEEP_OUT = 0x11,
    DISPLAY_ON = 0x29,
    TIMEOUT_US = 50000,
    BLLP_US = 4000,
    OFF_US = 50000,
    ON_US = 20000,
    SLEEP_US = 120000,
    LINK_SLEEP_US = 220000,
    QUIET_US = 50000,
    STOP_US = 200000,
    START_US = 100000,
    RESET_US = 20000,
    LP11_US = 1000,
    CLOCK_US = 200,
    /* The panel's reset line, GPIO 25: TLMM GPIO_CFG (function 5:2, output
     * enable 9) and IN_OUT (output 1); the vendor's reset sequence. */
    PANEL_RESET_CFG = 0x01019000,
    PANEL_RESET_IO = 0x01019004,
    GPIO_FUNCTION = 0xfu << 2,
    GPIO_OE = 1u << 9,
    GPIO_OUT = 2,
    RESET_HIGH_US = 5000,
    RESET_LOW_US = 10000,
    RESET_DONE_US = 30000,
    INIT_DELAY_US = 50000,
};

static uint32_t video_ctrl; /* DSI_CTRL as the loader left it: video mode on. */
static bool ready, asleep;
/* After "exit sleep mode": 120 ms, as R61308's init and the vendor panels;
 * HX8394D (lk2nd's header; INTF1 hsync pulse width 50) needs 200 ms. */
static uint32_t sleep_out_us = SLEEP_US;
/* Reset in standby: an R61308 with its reset line as lk2nd leaves it; held
 * low now; restarted by the last rebuild. */
static bool reset_known, reset_held, restarted;
/* The command DMA reads packets from here, in the controller's layout
 * (dsi_host.c dsi_cmd_dma_add): the header's two data or word-count bytes,
 * its data type, then flags (0x80 last, 0x40 long, 0x20 bus turnaround); a
 * long packet's payload follows, padded with 0xff to whole words. Kernel data
 * is identity-mapped and uncached, so it needs no cache maintenance. */
static volatile uint32_t packet[8] __attribute__((aligned(8)));

static bool until(uint32_t bit) {
    uint64_t start = time_us();
    while (!(read32(DSI + DSI_INTR_CTRL) & bit))
        if (time_us() - start >= TIMEOUT_US) return false;
    return true;
}

/* Clear statuses and set enables; enables otherwise stay as the loader left
 * them, and this kernel takes no display interrupt. */
static void interrupts(uint32_t enables, uint32_t clear) {
    write32(DSI + DSI_INTR_CTRL, enables | clear);
}

/* One packet through the command DMA, from the buffer above. */
static bool dma_bytes(const uint8_t *bytes, unsigned length, uint32_t enables) {
    unsigned words = (length + 3) / 4;
    if (!length || words > sizeof(packet) / sizeof(packet[0])) return false;
    for (unsigned i = 0; i < words; ++i) {
        uint32_t word = 0;
        for (unsigned b = 0; b < 4; ++b)
            word |= (uint32_t)(4 * i + b < length ? bytes[4 * i + b] : 0xff) << (8 * b);
        packet[i] = word;
    }
    interrupts(enables | DMA_DONE_ENABLE, DMA_DONE);
    write32(DSI + DSI_DMA_BASE, (uint32_t)(uintptr_t)packet);
    write32(DSI + DSI_DMA_LEN, 4 * words);
    write32(DSI + DSI_TRIG_DMA, 1);
    return until(DMA_DONE);
}

/* One packet: command mode on, after the frame's video and its blanking
 * packets, through the command DMA, then DSI_CTRL as it was. */
static bool send_packet(const uint8_t *bytes, unsigned length) {
    write32(DSI + DSI_CTRL, video_ctrl | CTRL_COMMAND | CTRL_ENABLE);
    uint32_t enables = read32(DSI + DSI_INTR_CTRL) & ~STATUS_BITS;
    bool sent = true;
    if (read32(DSI + DSI_STATUS0) & STATUS_VIDEO_BUSY) {
        interrupts(enables | VIDEO_DONE_ENABLE, VIDEO_DONE);
        sent = until(VIDEO_DONE);
        if (sent) delay_us(BLLP_US);
    }
    if (sent) sent = dma_bytes(bytes, length, enables);
    interrupts(enables, DMA_DONE | VIDEO_DONE);
    write32(DSI + DSI_CTRL, video_ctrl);
    return sent;
}

/* A DCS short write without a parameter (data type 0x05). */
static bool send(uint8_t dcs) {
    const uint8_t bytes[4] = {dcs, 0, 0x05, 0x80};
    return send_packet(bytes, 4);
}

/* Exit sleep mode, 120 ms, display on; each command gets a second try. */
static bool wake(void) {
    bool awake = send(SLEEP_OUT) || send(SLEEP_OUT);
    delay_us(sleep_out_us);
    awake = (send(DISPLAY_ON) || send(DISPLAY_ON)) && awake;
    delay_us(ON_US);
    return awake;
}

/* The R61308's on-commands as the controller reads them, with the wait after
 * each in ms: exit sleep mode (120), address mode 0, pixel format 0x07,
 * manufacturer access on (B0 04), C1, the gamma tables (C8), CB-CF, access
 * off (B0 03), display on (10). */
static const struct panel_command {
    uint8_t length, wait_ms, bytes[29];
} r61308_on[] = {
    {4, 120, {0x11, 0x00, 0x05, 0x80}},
    {6, 0, {0x02, 0x00, 0x39, 0xc0, 0x36, 0x00}},
    {6, 0, {0x02, 0x00, 0x39, 0xc0, 0x3a, 0x07}},
    {6, 0, {0x02, 0x00, 0x29, 0xc0, 0xb0, 0x04}},
    {12, 0, {0x08, 0x00, 0x29, 0xc0, 0xc1, 0x50, 0x02, 0x22, 0x00, 0x00, 0xed, 0x11}},
    {29, 0, {0x19, 0x00, 0x29, 0xc0, 0xc8, 0x1a, 0x24, 0x29, 0x2d, 0x32,
             0x37, 0x14, 0x13, 0x10, 0x0c, 0x0a, 0x06, 0x1a, 0x24, 0x28,
             0x2d, 0x32, 0x37, 0x14, 0x13, 0x10, 0x0c, 0x0a, 0x06}},
    {13, 0, {0x09, 0x00, 0x29, 0xc0, 0xcb, 0x10, 0x20, 0x40, 0x80, 0xa0, 0xc0, 0xd0, 0xe0}},
    {8, 0, {0x04, 0x00, 0x29, 0xc0, 0xcc, 0xc8, 0xd8, 0xff}},
    {12, 0, {0x08, 0x00, 0x29, 0xc0, 0xcd, 0x1c, 0x1e, 0x1e, 0x1d, 0x1c, 0x1e, 0x1e}},
    {12, 0, {0x08, 0x00, 0x29, 0xc0, 0xce, 0x1e, 0x1e, 0x1e, 0x1d, 0x1d, 0x1e, 0x1e}},
    {12, 0, {0x08, 0x00, 0x29, 0xc0, 0xcf, 0x1e, 0x1f, 0x20, 0x20, 0x20, 0x20, 0x21}},
    {6, 0, {0x02, 0x00, 0x29, 0xc0, 0xb0, 0x03}},
    {4, 10, {0x29, 0x00, 0x05, 0x80}},
};

static void set_reset_line(bool high) {
    uint32_t io = read32(PANEL_RESET_IO);
    write32(PANEL_RESET_IO, high ? io | GPIO_OUT : io & ~(uint32_t)GPIO_OUT);
}

/* The reset line as lk2nd leaves it: a GPIO, driven, high. */
static bool reset_line_live(void) {
    uint32_t cfg = read32(PANEL_RESET_CFG);
    return !(cfg & GPIO_FUNCTION) && (cfg & GPIO_OE) && (read32(PANEL_RESET_IO) & GPIO_OUT);
}

/* Out of reset and initialized, with the lanes in LP-11 and no video yet
 * (this panel leaves sleep only once the video's clock runs). The reset line
 * stays held (asked for again by the next rebuild) until the commands all
 * went out. */
static bool panel_restart(void) {
    set_reset_line(true);
    delay_us(RESET_HIGH_US);
    set_reset_line(false);
    delay_us(RESET_LOW_US);
    set_reset_line(true);
    delay_us(RESET_DONE_US);
    delay_us(INIT_DELAY_US);
    for (unsigned i = 0; i < sizeof(r61308_on) / sizeof(r61308_on[0]); ++i) {
        const struct panel_command *c = &r61308_on[i];
        if (!send_packet(c->bytes, c->length) && !send_packet(c->bytes, c->length)) return false;
        if (c->wait_ms) delay_us(1000u * c->wait_ms);
    }
    reset_held = false;
    restarted = true;
    return true;
}

/* The pipeline: how far standby turned it off, and what boot read of it. */
enum depth { ON, STREAM, LINK, CORE };

enum step {
    STEP_NONE,
    STEP_CORE,
    STEP_STATE,
    STEP_PHY,
    STEP_PLL,
    STEP_CLOCK,
    STEP_STREAM,
    STEP_PANEL
};

static bool pipeline, lost; /* boot checks passed; the last rebuild failed */
static enum depth down;     /* how far standby turned it off */
static enum step failed;
static uint32_t pll_saved[0xa0 / 4], phy_saved[0x1e0 / 4], regulator_saved[7], dsi_saved[0x1e8 / 4];
static uint32_t mdp_saved[4]; /* MDP version, CTL0 layer and op, INTF1 hsync */
static bool settings_kept;

/* A GCC branch clock on or off, confirmed by its status as Linux's
 * clk_branch_wait confirms it (200 us). */
static bool branch(unsigned cbcr, bool on) {
    uintptr_t address = GCC_MDSS + cbcr;
    uint32_t value = read32(address);
    write32(address, on ? value | 1u : value & ~1u);
    for (unsigned waited = 0;; ++waited) {
        if (!(read32(address) & CLOCK_OFF) == on) return true;
        if (waited == CLOCK_US) return false;
        delay_us(1);
    }
}

static bool links_running(void) {
    return !((read32(GCC_MDSS + ESC0) | read32(GCC_MDSS + BYTE0) | read32(GCC_MDSS + PCLK0)) &
             CLOCK_OFF);
}

/* INTF1's frame counter, on since boot. Frames are counted as changes, in
 * case starting the timing generator resets it. */
static uint32_t frames(void) { return read32(INTF1 + INTF_FRAMES); }

static bool frames_run(unsigned count, uint32_t timeout_us) {
    uint32_t last = frames();
    uint64_t start = time_us();
    while (count) {
        uint32_t now = frames();
        if (now != last) {
            last = now;
            --count;
        } else if (time_us() - start >= timeout_us) return false;
    }
    return true;
}

/* No frame for QUIET_US: the timing generator has stopped. */
static bool frames_stop(void) {
    uint32_t last = frames();
    uint64_t start = time_us(), since = start;
    for (;;) {
        uint64_t now = time_us();
        uint32_t count = frames();
        if (count != last) {
            last = count;
            since = now;
        } else if (now - since >= QUIET_US) return true;
        if (now - start >= STOP_US) return false;
    }
}

/* Registers saved at boot (SAVE), compared on waking (CHECK) or written back
 * after a PHY reset (WRITE). */
enum mode { SAVE, CHECK, WRITE };

static void each(enum mode mode, uintptr_t base, uint32_t *saved, unsigned offset) {
    if (mode == SAVE) saved[offset / 4] = read32(base + offset);
    else if (mode == CHECK) {
        if (read32(base + offset) != saved[offset / 4]) settings_kept = false;
    } else write32(base + offset, saved[offset / 4]);
}

/* lk2nd's mdss_dsi_pll_vco_config order; the spread-spectrum registers it
 * writes only with spread spectrum keep their values otherwise, and the
 * VCO gets its 1 ms after SDM_CFG4. The lock sets CAL_CFG1. */
static void pll_settings(enum mode mode) {
    static const uint8_t order[] = {0x2c, 0x30, 0x34, 0x08, 0x04, 0x24, 0x28, 0x78,
                                    0x7c, 0x64, 0x3c, 0x40, 0x44, 0x4c, 0x50, 0x54,
                                    0x58, 0x48, 0x00, 0x14, 0x0c, 0x10, 0x38, 0x6c,
                                    0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c};
    for (unsigned i = 0; i < sizeof(order); ++i) {
        each(mode, PLL, pll_saved, order[i]);
        if (mode == WRITE && order[i] == PLL_SDM_CFG4) delay_us(1000);
    }
    if (mode == SAVE) pll_saved[PLL_CAL_CFG1 / 4] = read32(PLL + PLL_CAL_CFG1);
}

/* lk2nd's mdss_dsi_phy_regulator_init with the boot's values. */
static void regulator_on(void) {
    static const uint8_t ldo[] = {5, 3, 2, 1, 4}, dcdc[] = {1, 2, 3, 4};
    bool ldo_mode = phy_saved[PHY_LDO / 4] != 0;
    write32(REGULATOR, 0);
    write32(REGULATOR + REGULATOR_CAL, regulator_saved[REGULATOR_CAL / 4]);
    delay_us(1000);
    const uint8_t *order = ldo_mode ? ldo : dcdc;
    for (unsigned i = 0; i < (ldo_mode ? sizeof(ldo) : sizeof(dcdc)); ++i)
        write32(REGULATOR + 4u * order[i], regulator_saved[order[i]]);
    write32(PHY + PHY_LDO, phy_saved[PHY_LDO / 4]);
    if (!ldo_mode) write32(REGULATOR, regulator_saved[0]);
}

/* lk2nd's mdss_dsi_phy_28nm_init order: CTRL_0 0x5b, strength, regulator,
 * timing, the 4 data lanes and the clock lane (9 registers each), CTRL_4,
 * the global test control, CTRL_0, BIST. CTRL_1 and STRENGTH_1, which Linux
 * also sets, follow the timing. Standby turns CTRL_0 and the regulator's
 * calibration power off, so CHECK skips them. */
static void phy_settings(enum mode mode) {
    bool write = mode == WRITE;
    if (write) write32(PHY + PHY_CTRL_0, 0x5b);
    each(mode, PHY, phy_saved, PHY_STRENGTH_0);
    if (write) regulator_on();
    for (unsigned offset = 0x140; offset <= 0x16c; offset += 4) each(mode, PHY, phy_saved, offset);
    each(mode, PHY, phy_saved, PHY_CTRL_1);
    each(mode, PHY, phy_saved, PHY_STRENGTH_1);
    for (unsigned offset = 0; offset < 0x140; offset += 4)
        if ((offset & 0x3f) < 0x24) each(mode, PHY, phy_saved, offset);
    each(mode, PHY, phy_saved, PHY_CTRL_4);
    each(mode, PHY, phy_saved, PHY_GLBL_TEST);
    if (mode != CHECK) each(mode, PHY, phy_saved, PHY_CTRL_0);
    for (unsigned offset = 0x1b4; offset <= 0x1c8; offset += 4) each(mode, PHY, phy_saved, offset);
    if (write) return;
    each(mode, PHY, phy_saved, PHY_LDO);
    for (unsigned offset = 0; offset < REGULATOR_CAL; offset += 4)
        each(mode, REGULATOR, regulator_saved, offset);
    if (mode == SAVE) each(mode, REGULATOR, regulator_saved, REGULATOR_CAL);
}

/* DSI settings a soft reset could lose; the timing double buffer's mode
 * comes first, as it decides how the timing takes effect. */
static const uint16_t dsi_settings[] = {
    DSI_TIMING_DB, DSI_VID_CFG0,     DSI_VID_CFG1,   DSI_ACTIVE_H,   DSI_ACTIVE_V,
    DSI_TOTAL,     DSI_ACTIVE_HSYNC, DSI_VSYNC_HPOS, DSI_VSYNC_VPOS, DSI_CMD_DMA_CTRL,
    DSI_TRIG_CTRL, DSI_LANE_CTRL,    DSI_LANE_SWAP,  DSI_HS_TIMER,   DSI_CLKOUT_TIMING,
    DSI_EOT,       DSI_ERR_MASK,     DSI_INTR_CTRL,  DSI_CLK_CTRL};

static uint32_t dsi_setting(unsigned offset) {
    uint32_t value = read32(DSI + offset);
    return offset == DSI_INTR_CTRL ? value & ~(uint32_t)STATUS_BITS : value;
}

static bool mdss_kept(void) {
    return read32(MDP) == mdp_saved[0] && read32(CTL0 + CTL_LAYER0) == mdp_saved[1] &&
           read32(CTL0 + CTL_OP) == mdp_saved[2] && read32(INTF1 + INTF_HSYNC) == mdp_saved[3] &&
           dsi_setting(DSI_VID_CFG0) == dsi_saved[DSI_VID_CFG0 / 4];
}

/* Linux's dsi_sw_reset: clocks forced on, then the reset pulse. */
static void dsi_reset(void) {
    write32(DSI + DSI_CLK_CTRL, DSI_CLOCKS_ON);
    write32(DSI + DSI_RESET, 1);
    delay_us(RESET_US);
    write32(DSI + DSI_RESET, 0);
}

/* The timing generator off at a frame's end, then the DSI controller off
 * with its video engine reset. Each step is safe to repeat. Returns whether
 * the frames stopped; if not, the MDP may still be fetching, and its clocks
 * stay on. The enable reads back as written (MDP 1.6 double-buffers INTF
 * registers), so the frame counter decides, every time. */
static bool stream_off(void) {
    write32(INTF1 + TG_ENABLE, 0);
    write32(CTL0 + CTL_FLUSH, FLUSH_TIMING_1);
    bool stopped = frames_stop();
    uint64_t start = time_us();
    while ((read32(DSI + DSI_STATUS0) & STATUS_DMA_BUSY) && time_us() - start < TIMEOUT_US) { }
    write32(DSI + DSI_CTRL,
            read32(DSI + DSI_CTRL) & ~(uint32_t)(CTRL_ENABLE | CTRL_VIDEO | CTRL_COMMAND));
    if (links_running()) dsi_reset();
    write32(DSI + DSI_CTRL, 0);
    return stopped;
}

/* Link clocks, then the PLL, then the PHY (Linux's order). */
static void link_off(void) {
    (void)(branch(ESC0, false) & branch(PCLK0, false) & branch(BYTE0, false));
    write32(PLL + PLL_GLB_CFG, 0);
    write32(PHY + PHY_CTRL_0, 0);
    write32(REGULATOR + REGULATOR_CAL, 0);
}

static void core_off(void) {
    (void)(branch(MDP_CORE, false) & branch(VSYNC, false) & branch(AXI, false) &
           branch(AHB, false));
}

static bool core_on(void) {
    return branch(AHB, true) && branch(AXI, true) && branch(VSYNC, true) && branch(MDP_CORE, true);
}

/* Down to target from wherever the pipeline is, including partway up after
 * a failed rebuild; MDSS registers only while its AHB clock runs. */
static void teardown(enum depth target) {
    if (target == ON) return;
    if (down < CORE) {
        if (!stream_off()) target = STREAM;
        if (target >= LINK) link_off();
    }
    if (target == CORE) core_off();
    if (target > down) down = target;
}

/* lk2nd's dsi_pll_enable_seq_8916: software reset, calibration, power-up,
 * lock detection; the boot's calibration first, then TSMC's, GF1's and
 * GF2's, three tries each. Linux polls the lock 10 times, 50 us apart. */
static bool pll_lock(void) {
    const uint32_t calibration[] = {pll_saved[PLL_CAL_CFG1 / 4], 0x34, 0x14, 0x04};
    for (unsigned c = 0; c < 4; ++c) {
        if (c && calibration[c] == calibration[0]) continue;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            write32(PLL + PLL_TEST_CFG, 1);
            delay_us(1);
            write32(PLL + PLL_TEST_CFG, 0);
            delay_us(1);
            write32(PLL + PLL_CAL_CFG1, calibration[c]);
            delay_us(1);
            write32(PLL + PLL_GLB_CFG, 0x01);
            delay_us(1);
            write32(PLL + PLL_GLB_CFG, 0x05);
            delay_us(3);
            write32(PLL + PLL_GLB_CFG, 0x0f);
            delay_us(500);
            write32(PLL + PLL_LKDET_CFG2, 0x04);
            delay_us(1);
            write32(PLL + PLL_LKDET_CFG2, 0x05);
            delay_us(512);
            for (unsigned poll = 0; poll < 10; ++poll) {
                if (read32(PLL + PLL_STATUS) & 1) return true;
                delay_us(50);
            }
        }
    }
    return false;
}

/* Linux's order: escape, byte, pixel; never before the PLL locks. */
static bool link_on(void) {
    return branch(ESC0, true) && branch(BYTE0, true) && branch(PCLK0, true);
}

/* Linux's msm_dsi_host_reset_phy; it resets the PLL's registers too. */
static void phy_reset(void) {
    write32(DSI + DSI_PHY_RESET, 1);
    delay_us(1000);
    write32(DSI + DSI_PHY_RESET, 0);
    delay_us(100);
}

/* Soft reset, settings it lost written back (the timing always, when it is
 * double-buffered, then flushed), errors cleared, the lanes held in LP-11,
 * then video mode before the timing generator starts (Linux's
 * msm_dsi_host_power_on and _enable). */
static bool dsi_arm(void) {
    dsi_reset();
    bool double_buffered = dsi_saved[DSI_TIMING_DB / 4] & 1;
    for (unsigned i = 0; i < sizeof(dsi_settings) / sizeof(dsi_settings[0]); ++i) {
        unsigned offset = dsi_settings[i];
        if (dsi_setting(offset) != dsi_saved[offset / 4] ||
            (double_buffered && offset >= DSI_ACTIVE_H && offset <= DSI_VSYNC_VPOS))
            write32(DSI + offset, dsi_saved[offset / 4]);
    }
    if (double_buffered) write32(DSI + DSI_TIMING_FLUSH, 1);
    write32(DSI + DSI_FIFO_STATUS, read32(DSI + DSI_FIFO_STATUS));
    write32(DSI + DSI_CLK_STATUS, read32(DSI + DSI_CLK_STATUS));
    write32(DSI + DSI_DLN0_PHY_ERR, read32(DSI + DSI_DLN0_PHY_ERR));
    write32(DSI + DSI_CTRL, video_ctrl & ~(uint32_t)(CTRL_VIDEO | CTRL_COMMAND));
    delay_us(LP11_US);
    bool panel = !reset_held || panel_restart();
    write32(DSI + DSI_CTRL, video_ctrl);
    return panel;
}

/* The timing generator on with the whole path flushed (Linux's
 * mdp5_vid_encoder_enable), frames counted, then two more checked for the
 * PLL losing lock. */
static bool stream_on(void) {
    write32(MDP + MDP_INTR_CLEAR, INTF1_UNDERRUN | INTF1_VSYNC);
    write32(INTF1 + INTF_COUNTERS, 3);
    write32(INTF1 + TG_ENABLE, 1);
    write32(CTL0 + CTL_FLUSH, FLUSH_PATH);
    if (!frames_run(2, START_US)) return false;
    uint64_t start = time_us();
    while (read32(CTL0 + CTL_FLUSH) && time_us() - start < TIMEOUT_US) { }
    write32(MDP + MDP_INTR_CLEAR, INTF1_UNDERRUN | INTF1_VSYNC);
    write32(DSI + DSI_FIFO_STATUS, read32(DSI + DSI_FIFO_STATUS));
    write32(DSI + DSI_CLK_STATUS, read32(DSI + DSI_CLK_STATUS));
    if (!frames_run(2, START_US)) return false;
    return !(read32(DSI + DSI_CLK_STATUS) & PLL_UNLOCKED);
}

static bool fail(enum step step) {
    failed = step;
    return false;
}

/* Up from down to the running stream. reset: the PHY and PLL reset and
 * reprogrammed in lk2nd's order (PLL, lock, link clocks, PHY); otherwise
 * powered up with the settings they kept, in Linux's order (PHY, PLL lock,
 * link clocks). */
static bool rebuild(bool reset) {
    if (down == CORE) {
        if (!core_on()) return fail(STEP_CORE);
        down = LINK;
        if (!mdss_kept()) return fail(STEP_STATE);
    }
    if (down == LINK) {
        if (reset) {
            phy_reset();
            pll_settings(WRITE);
        } else {
            settings_kept = true;
            pll_settings(CHECK);
            phy_settings(CHECK);
            if (!settings_kept) return fail(STEP_PHY);
            regulator_on();
            write32(PHY + PHY_CTRL_0, phy_saved[PHY_CTRL_0 / 4]);
        }
        if (!pll_lock()) return fail(STEP_PLL);
        if (!link_on()) return fail(STEP_CLOCK);
        if (reset) phy_settings(WRITE);
        down = STREAM;
    }
    if (down == STREAM) {
        if (!dsi_arm()) return fail(STEP_PANEL);
        if (!stream_on()) return fail(STEP_STREAM);
        down = ON;
    }
    return true;
}

/* The ladder: as kept, then reprogrammed; a failure that lost the MDSS
 * itself (core clocks or registers) skips the second. A panel restarted from
 * reset already got its wake commands. */
static void restore(void) {
    restarted = false;
    bool ok = rebuild(false) && (restarted || wake() || fail(STEP_PANEL));
    if (!ok && failed > STEP_STATE) {
        teardown(LINK);
        ok = rebuild(true) && (restarted || wake() || fail(STEP_PANEL));
    }
    lost = !ok;
    if (lost) teardown(CORE); /* the next wake tries again */
}

/* As lk2nd leaves it: the power domain on under software control, the seven
 * clocks running, MDP 1.6 and DSI 6G 1.3, the PLL locked, the timing
 * generator on; then INTF1's frame counter on (as Linux's mode set) and
 * counting. Whether so, after saving what waking needs. */
static bool pipeline_init(void) {
    static const uint16_t clocks[] = {AHB, AXI, VSYNC, MDP_CORE, ESC0, BYTE0, PCLK0};
    if ((read32(GCC_MDSS + GDSCR) & (DOMAIN_ON | GDSC_HW_CONTROL)) != DOMAIN_ON) return false;
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i)
        if (read32(GCC_MDSS + clocks[i]) & CLOCK_OFF) return false;
    if (read32(MDP) >> 16 != 0x1006 || read32(DSI_BASE) >> 16 != 0x1003) return false;
    if (!(read32(PLL + PLL_STATUS) & 1)) return false;
    if (!(read32(INTF1 + TG_ENABLE) & 1)) return false;
    write32(INTF1 + INTF_COUNTERS, 3);
    if (!frames_run(2, START_US)) return false;
    pll_settings(SAVE);
    phy_settings(SAVE);
    for (unsigned i = 0; i < sizeof(dsi_settings) / sizeof(dsi_settings[0]); ++i)
        dsi_saved[dsi_settings[i] / 4] = dsi_setting(dsi_settings[i]);
    mdp_saved[0] = read32(MDP);
    mdp_saved[1] = read32(CTL0 + CTL_LAYER0);
    mdp_saved[2] = read32(CTL0 + CTL_OP);
    mdp_saved[3] = read32(INTF1 + INTF_HSYNC);
    unsigned line = mdp_saved[3] >> 16, pulse = mdp_saved[3] & 0xffff;
    /* Held in reset in standby, as the vendor kernel's panel off leaves it:
     * asleep but out of reset, the panel drew about 4 mA. */
    reset_known = pulse == 2 && (line == 892 || line == 986) && reset_line_live();
    return true;
}

bool redmi2_display_off(void) { return pipeline && asleep && down == CORE; }

bool redmi2_display_init(void) {
    ready = asleep = pipeline = lost = false;
    reset_known = reset_held = restarted = false;
    down = ON;
    sleep_out_us = SLEEP_US;
    /* No MDSS register before its power domain and bus clocks are known on. */
    if (!(read32(GCC_MDSS + GDSCR) & DOMAIN_ON) ||
        ((read32(GCC_MDSS + AHB) | read32(GCC_MDSS + AXI) | read32(GCC_MDSS + MDP_CORE)) &
         CLOCK_OFF))
        return false;
    if ((read32(INTF1 + INTF_HSYNC) & 0xffff) == 50) sleep_out_us = 200000;
    video_ctrl = read32(DSI + DSI_CTRL);
    if ((video_ctrl & (CTRL_ENABLE | CTRL_VIDEO)) != (CTRL_ENABLE | CTRL_VIDEO)) return false;
    /* Linux and lk2nd both send commands this way: DMA from memory (the
     * FROM_FRAME_BUFFER bit), started by software. */
    if ((read32(DSI + DSI_TRIG_CTRL) & TRIGGER_MASK) != TRIGGER_SW ||
        !(read32(DSI + DSI_CMD_DMA_CTRL) & DMA_FROM_FRAME_BUFFER))
        return false;
    ready = true;
    pipeline = pipeline_init();
    return true;
}

/* Once the pipeline is off: the reset line low, the panel's state lost,
 * so waking restarts it (panel_restart). */
static void hold_reset(void) {
    if (!reset_known || !asleep || down != CORE) return;
    set_reset_line(false);
    reset_held = true;
}

void redmi2_display_standby(bool off) {
    if (!ready || off == asleep) return;
    if (off) {
        /* After a failed rebuild nothing runs to take commands. */
        if (lost) {
            teardown(CORE);
            asleep = true;
            hold_reset();
            return;
        }
        enum depth target = pipeline ? CORE : ON;
        bool off_sent = send(DISPLAY_OFF), done = off_sent;
        if (done) {
            delay_us(OFF_US);
            done = send(SLEEP_IN);
        }
        if (!done) {
            /* An unconfirmed command may still have reached the panel: after
             * a failed "enter sleep mode", wake it the full way. */
            if (off_sent) {
                delay_us(SLEEP_US);
                (void)wake();
            } else (void)send(DISPLAY_ON);
            ready = false;
            return;
        }
        delay_us(target == ON ? SLEEP_US : LINK_SLEEP_US);
        asleep = true;
        teardown(target);
        hold_reset();
        return;
    }
    asleep = false;
    if (down != ON) {
        restore();
        return;
    }
    (void)wake();
}

bool redmi2_display_reset_held(void) { return reset_held; }
