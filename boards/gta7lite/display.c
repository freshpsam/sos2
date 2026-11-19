#include "gta7lite.h"
#include "platform.h"

/* The panel in standby. LK leaves the NT36523 running in video mode on DSI0,
 * fed by OVL0 and OVL0_2L through RDMA0. Standby turns the backlight off,
 * sends display off (28h) and sleep in (10h) inside video mode, as
 * MediaTek's driver sends DCS commands to video panels (DSI_VM_CMD_CON: a
 * short packet in the next vertical front porch, ddp_dsi.c), and turns the
 * overlay layers off, so the engines stop reading the framebuffer. The
 * panel's reset line, supplies and registers stay as they are, so sleep out
 * (11h) and display on (29h) wake it without its init table; the timings
 * are Samsung's suspend's (nt36523bh_qunchuang_inx_incell_vdo): 20 ms after
 * display off, and 120 ms between sleep in and sleep out and after sleep
 * out. The DSI link keeps running: stopping it would save another 6 mA but
 * needs the pipeline restarted at the wake. */
enum {
    OVL0 = 0x1400b000,
    OVL0_2L = 0x1400c000,
    OVL_SRC_CON = 0x02c, /* layer enables */
    DSI0 = 0x14014000,
    DSI_START = 0x000,
    DSI_INTSTA = 0x00c, /* a bit is cleared by writing 0 to it */
    DSI_MODE_CTRL = 0x014,
    DSI_VM_CMD_CON = 0x130,
    VIDEO_RUNNING = 1 << 0,
    VM_CMD_START = 1 << 16,
    VM_CMD_DONE = 1 << 5,
    MODE_MASK = 3, /* 0 is command mode */
    VM_CMD_EN = 1 << 0,
    TS_VFP_EN = 1 << 5,
    DCS_SHORT_WRITE = 0x05,
    DISPLAY_OFF = 0x28,
    SLEEP_IN = 0x10,
    SLEEP_OUT = 0x11,
    DISPLAY_ON = 0x29,
    FRAME_US = 17000,
    DISPLAY_OFF_US = 20000,
    SLEEP_US = 120000
};

static bool asleep;
static uint32_t layers[2];
static uint64_t slept_at;


/* True once DSI0 has sent the command, within a few frames. */
static bool dcs(uint8_t command) {
    mmio_write32(DSI0 + DSI_INTSTA, ~(uint32_t)VM_CMD_DONE);
    mmio_write32(DSI0 + DSI_VM_CMD_CON,
                 VM_CMD_EN | TS_VFP_EN | DCS_SHORT_WRITE << 8 | (uint32_t)command << 16);
    uint32_t start = mmio_read32(DSI0 + DSI_START);
    mmio_write32(DSI0 + DSI_START, start & ~(uint32_t)VM_CMD_START);
    mmio_write32(DSI0 + DSI_START, start | VM_CMD_START);
    bool sent = false;
    for (uint64_t t = time_us(); !sent && time_us() - t < 4 * FRAME_US;)
        sent = mmio_read32(DSI0 + DSI_INTSTA) & VM_CMD_DONE;
    mmio_write32(DSI0 + DSI_INTSTA, ~(uint32_t)VM_CMD_DONE);
    return sent;
}

void gta7lite_display_standby(bool standby) {
    if (standby) {
        gta7lite_backlight(false);
        /* Only the video-mode pipeline LK leaves. */
        if (asleep || !(mmio_read32(DSI0 + DSI_MODE_CTRL) & MODE_MASK) ||
            !(mmio_read32(DSI0 + DSI_START) & VIDEO_RUNNING) || !dcs(DISPLAY_OFF))
            return;
        asleep = true;
        delay_us(DISPLAY_OFF_US);
        (void)dcs(SLEEP_IN);
        slept_at = time_us();
        layers[0] = mmio_read32(OVL0 + OVL_SRC_CON);
        layers[1] = mmio_read32(OVL0_2L + OVL_SRC_CON);
        mmio_write32(OVL0 + OVL_SRC_CON, 0);
        mmio_write32(OVL0_2L + OVL_SRC_CON, 0);
        return;
    }
    if (asleep) {
        mmio_write32(OVL0 + OVL_SRC_CON, layers[0]);
        mmio_write32(OVL0_2L + OVL_SRC_CON, layers[1]);
        uint64_t since = time_us() - slept_at;
        if (since < SLEEP_US) delay_us(SLEEP_US - since);
        (void)dcs(SLEEP_OUT);
        delay_us(SLEEP_US);
        (void)dcs(DISPLAY_ON);
        asleep = false;
        delay_us(2 * FRAME_US); /* lit once the panel shows frames again */
    }
    gta7lite_backlight(true);
}
