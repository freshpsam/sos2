#include "nexus5.h"
#include "platform.h"

/* The panel: 1080x1920 on DSI0 in command mode ("orise" in the stock
 * bootloader's command line), which keeps showing the last frame it was
 * sent. The bootloader leaves its splash there: MDP 1.2's pipe RGB0 reads a
 * 1080x1920 RGB888 buffer (blue, green, red in memory; rows 3240 bytes
 * apart; at 0x0D200000 from HHZ20h, as lk2nd logs it on stock phones)
 * through layer mixer 0, CTL0, ping-pong 0 and DSI0, with the clocks
 * running. SOS2 draws into that buffer and has the display processor send it
 * to the panel by starting the pipe's CTL, as the bootloader does for each
 * splash, while the screen is on: a frame once the last has gone out, at
 * most every 16 ms. DSI0 shows a frame gone by the done flag it latches for
 * the MDP's command-mode frames (CMD_MDP_DONE, which the stock
 * mdss_dsi_host.c waits on), cleared as each starts, or by its status (its
 * command-mode engine busy, then idle again), which a slow pass of the main
 * loop can miss; after 250 ms a frame is taken as gone, however DSI0 looks
 * (a busy that never ends included). A start that meets a frame still going
 * out leaves the panel showing what it
 * had (lk2nd waits 20 ms after each start). The MDP's own interrupt status
 * is not touched: every start-up that read or cleared it reset the phone or
 * stalled it (2026-11-20).
 * Addresses: Linux mmcc-msm8974.c (the MDSS power domain and clocks),
 * mdp5.xml.h with mdp5_cfg.c's msm8x74v2 offsets, which count from the MDP
 * block at MDSS + 0x100, the stock kernel's mdss_mdp_hwio.h (the interrupt
 * status), and dsi.xml.h, whose 6G controller has its registers 4 bytes in.
 * A probe read them on the phone (2026-11-20).
 * Standby puts the panel itself to sleep once its last frame has gone, as
 * the stock driver's off commands do (display off, 50 ms, sleep in,
 * 100 ms), and wakes it (sleep out, 120 ms, display on) before frames go
 * again: DCS short writes through DSI0's command DMA, which the bootloader
 * leaves set for software triggers in high speed, as the stock
 * mdss_dsi_cmd_dma_tx sends them: a 4-byte packet in RAM, its address and
 * length, the trigger, then the DMA's done bit, with its mask on as the
 * bootloader's own sender sets it. */
enum {
    MMCC = 0xfd8c0000,
    MDSS_GDSCR = MMCC + 0x2304,
    MDSS_AHB_CBCR = MMCC + 0x2308,
    MDSS_AXI_CBCR = MMCC + 0x2310,
    MDSS_MDP_CBCR = MMCC + 0x231c,
    MDP = 0xfd900100,
    RGB0 = MDP + 0x1d00,
    SRC_SIZE = 0x00,
    SRC0_ADDR = 0x14,
    SRC_STRIDE = 0x24,
    SRC_FORMAT = 0x30,
    SRC_UNPACK = 0x34,
    CTL0 = MDP + 0x500,
    CTL_STRIDE = 0x100,
    CTLS = 5,
    MIXERS = 5,     /* CTL_LAYER(0..4) at 0x00..0x10, one per layer mixer */
    RGB0_STAGE = 9, /* RGB0's 3-bit stage in a CTL_LAYER register */
    CTL_START = 0x1c,
    DSI0_CTRL = 0xfd922800 + 4,
    DSI0_STATUS = DSI0_CTRL + 4,
    DSI0_DMA_CTRL = 0xfd922800 + 0x3c,
    DSI0_DMA_BASE = 0xfd922800 + 0x48,
    DSI0_DMA_LENGTH = 0xfd922800 + 0x4c,
    DSI0_DMA_TRIGGER = 0xfd922800 + 0x90,
    DSI0_INTERRUPTS = 0xfd922800 + 0x110,
    DMA_LOW_POWER = 1u << 26,
    DMA_DONE = 1u,
    DMA_DONE_MASK = 2u,
    MDP_DONE = 1u << 8,
    INTERRUPT_MASKS = 0xaaaaaaaau, /* the odd bits; the even ones clear when written 1 */
    DMA_US = 200000,
    PACKET_LAST = 1u << 31,
    DCS_SHORT_WRITE = 0x05,
    DISPLAY_OFF = 0x28,
    SLEEP_IN = 0x10,
    SLEEP_OUT = 0x11,
    DISPLAY_ON = 0x29,
    DSI_COMMAND_BUSY = 1u | 4u, /* the command-mode engine; the MDP's stream */
    DSI_ENABLE = 1,
    DSI_VIDEO = 2,
    DSI_COMMAND = 4,
    WIDTH = 1080,
    HEIGHT = 1920,
    STRIDE = WIDTH * 3,
    FORMAT_RGB888 = 0x0002243f, /* 8 bits a component, 3 bytes, unpacked tight */
    UNPACK_BGR = 0x00020001,    /* bytes: blue, green, red */
    FRAME_US = 16000,
    FRAME_TIMEOUT_US = 250000
};

#define GDSC_ON   (1u << 31)
#define CLOCK_OFF (1u << 31)

static uintptr_t start_register;
static uint64_t last_frame;
static bool asleep, sending, busy_seen;

const char *nexus5_display_handoff(uint32_t *scanout) {
    start_register = 0;
    if (!(mmio_read32(MDSS_GDSCR) & GDSC_ON) || (mmio_read32(MDSS_AHB_CBCR) & CLOCK_OFF) ||
        (mmio_read32(MDSS_AXI_CBCR) & CLOCK_OFF) || (mmio_read32(MDSS_MDP_CBCR) & CLOCK_OFF))
        return "the bootloader left the display off";
    if ((mmio_read32(DSI0_CTRL) & (DSI_ENABLE | DSI_VIDEO | DSI_COMMAND)) !=
        (DSI_ENABLE | DSI_COMMAND))
        return "the panel is not in command mode";
    if (mmio_read32(RGB0 + SRC_SIZE) != ((uint32_t)HEIGHT << 16 | WIDTH) ||
        mmio_read32(RGB0 + SRC_FORMAT) != FORMAT_RGB888 ||
        mmio_read32(RGB0 + SRC_UNPACK) != UNPACK_BGR ||
        (mmio_read32(RGB0 + SRC_STRIDE) & 0xffff) != STRIDE)
        return "the splash is not 1080x1920 RGB888";
    for (unsigned ctl = 0; ctl < CTLS && !start_register; ++ctl)
        for (unsigned mixer = 0; mixer < MIXERS; ++mixer)
            if (mmio_read32(CTL0 + CTL_STRIDE * ctl + 4 * mixer) >> RGB0_STAGE & 7) {
                start_register = CTL0 + CTL_STRIDE * ctl + CTL_START;
                break;
            }
    if (!start_register) return "the splash pipe is not on a mixer";
    *scanout = mmio_read32(RGB0 + SRC0_ADDR);
    return 0;
}

static void send(void) {
    barrier(); /* The frame's pixels reach memory before the fetch starts. */
    mmio_write32(DSI0_INTERRUPTS, (mmio_read32(DSI0_INTERRUPTS) & INTERRUPT_MASKS) | MDP_DONE);
    mmio_write32(start_register, 1);
    barrier();
    last_frame = time_us();
    sending = true;
    busy_seen = false;
}

/* Whether the last frame has gone out: DSI0 idle and done with it, or
 * 250 ms gone, however DSI0 looks. */
static bool sent(void) {
    if (!sending) return true;
    if (time_us() - last_frame >= FRAME_TIMEOUT_US) sending = false;
    else if (mmio_read32(DSI0_STATUS) & DSI_COMMAND_BUSY) busy_seen = true;
    else if (busy_seen || (mmio_read32(DSI0_INTERRUPTS) & MDP_DONE)) sending = false;
    return !sending;
}

void nexus5_display_poll(void) {
    if (start_register && !asleep && sent() && time_us() - last_frame >= FRAME_US) send();
}

void nexus5_display_flush(void) {
    if (!start_register) return;
    while (!sent() || time_us() - last_frame < FRAME_US) { }
    send();
}

static uint32_t packet[16] __attribute__((aligned(64)));
static bool panel_asleep;

/* A DCS command without parameters: the packet cleaned from the data cache
 * to RAM, where the DMA reads it (the kernel's memory is mapped at its
 * physical address). */
static bool dcs(uint8_t command) {
    packet[0] = PACKET_LAST | (uint32_t)DCS_SHORT_WRITE << 16 | command;
    __asm__ volatile("dsb sy\n mcr p15, 0, %0, c7, c10, 1\n dsb sy" : : "r"(packet) : "memory");
    mmio_write32(DSI0_DMA_CTRL, mmio_read32(DSI0_DMA_CTRL) & ~DMA_LOW_POWER);
    mmio_write32(DSI0_INTERRUPTS,
                 (mmio_read32(DSI0_INTERRUPTS) & INTERRUPT_MASKS) | DMA_DONE_MASK | DMA_DONE);
    mmio_write32(DSI0_DMA_BASE, (uint32_t)(uintptr_t)packet);
    mmio_write32(DSI0_DMA_LENGTH, 4);
    barrier();
    mmio_write32(DSI0_DMA_TRIGGER, 1);
    barrier();
    for (uint64_t start = time_us(); !(mmio_read32(DSI0_INTERRUPTS) & DMA_DONE);)
        if (time_us() - start >= DMA_US) return false;
    mmio_write32(DSI0_INTERRUPTS, (mmio_read32(DSI0_INTERRUPTS) & INTERRUPT_MASKS) | DMA_DONE);
    return true;
}

void nexus5_display_standby(bool on) {
    if (on == asleep || !start_register) return;
    if (on) {
        nexus5_display_flush();
        for (uint64_t start = time_us(); !sent() && time_us() - start < FRAME_TIMEOUT_US;) { }
        panel_asleep = dcs(DISPLAY_OFF);
        delay_us(50000);
        if (panel_asleep && dcs(SLEEP_IN)) delay_us(100000);
    } else if (panel_asleep) {
        if (dcs(SLEEP_OUT)) delay_us(120000);
        (void)dcs(DISPLAY_ON);
        panel_asleep = false;
    }
    asleep = on;
}
