#include "emmc_store.h"
#include "gta7lite.h"
#include "kernel_io.h"
#include "platform.h"

/* MT6765's MSDC0 and its eMMC, in PIO, as LK leaves them after reading the
 * boot image: clock, bus width and timing stay LK's. The registers and the
 * PIO sequence are Linux's drivers/mmc/host/mtk-sd.c and U-Boot's, and
 * MediaTek's own driver for the MT6765 (ComboA/msdc_reg.h). The card must
 * answer at RCA 1, 2 or 0 (LK's is undocumented) in the user area; a
 * deselected card is selected again. Longer data and busy timeouts than LK's
 * are set; no tuning, bus, partition-switch, erase or RPMB commands, and no
 * DMA. The eMMC store (kernel/protection/emmc_store.c) moves the installed
 * system's sectors and its data volume's through it. */
enum {
    MSDC = 0x11230000,
    MSDC_CFG = 0x00,
    MSDC_PS = 0x08,
    MSDC_INT = 0x0c,
    MSDC_INTEN = 0x10,
    MSDC_FIFOCS = 0x14,
    MSDC_TXDATA = 0x18,
    MSDC_RXDATA = 0x1c,
    SDC_CFG = 0x30,
    SDC_CMD = 0x34,
    SDC_ARG = 0x38,
    SDC_STS = 0x3c,
    SDC_RESP0 = 0x40,
    SDC_BLK_NUM = 0x50,
    MSDC_DMA_CTRL = 0x98,
    MSDC_DMA_CFG = 0x9c,
    CFG_MODE = 1 << 0, /* MMC/SD, not memory stick */
    CFG_RST = 1 << 2,  /* clears itself */
    CFG_PIO = 1 << 3,
    CFG_CKSTB = 1 << 7, /* the card clock is stable */
    PS_DAT0 = 1 << 16,  /* high: the card is not busy */
    /* SDC_CFG's timeouts, in 2^20 card clocks: data (DTOC [31:24]) and
     * busy (WRDTOC [14:2]), at most about 1.3 s and 43 s at 200 MHz. */
    CFG_DTOC = 0xffu << 24,
    CFG_WRDTOC = 0x1fff << 2,
    DMA_STOP = 1 << 1,
    DMA_ACTIVE = 1 << 0,
    INT_CMDRDY = 1 << 8,
    INT_CMDTMO = 1 << 9,
    INT_RSPCRCERR = 1 << 10,
    INT_XFER_COMPL = 1 << 12,
    INT_DATTMO = 1 << 14,
    INT_DATCRCERR = 1 << 15,
    INT_COMMAND = INT_CMDRDY | INT_CMDTMO | INT_RSPCRCERR,
    INT_DATA_ERROR = INT_DATTMO | INT_DATCRCERR,
    FIFOCS_CLR = 1u << 31,
    FIFO_BYTES = 128,
    STS_SDCBUSY = 1 << 0,
    STS_CMDBUSY = 1 << 1,
    /* SDC_CMD: opcode [5:0], response type [9:7], data type [12:11] (1 is
     * one block), write [13], block length [27:16]. */
    RESPONSE_R1 = 1 << 7,
    RESPONSE_R1B = 7 << 7,
    SINGLE_BLOCK = 1 << 11,
    WRITE_DATA = 1 << 13,
    BLOCK_LENGTH = 512 << 16,
    COMMAND_US = 100000,
    DATA_US = 1000000,
    PROGRAM_US = 5000000,
    /* A sector takes microseconds: polled through on the service worker,
     * which lets the desktop run only through longer waits (programming)
     * and between pieces of work, as on the phone. */
    SPIN_US = 200
};

#define R1_ERRORS 0xffffa080u /* Includes CARD_IS_LOCKED as well as command errors. */

static uint32_t rd(unsigned offset) {
    uint32_t value = mmio_read32(MSDC + offset);
    barrier();
    return value;
}

static void wr(unsigned offset, uint32_t value) {
    barrier();
    mmio_write32(MSDC + offset, value);
}

static bool controller, failed, cache_enabled;
static uint32_t rca, raw_sectors;
static unsigned char bounce[512] __attribute__((aligned(4)));

/* msdc_reset_hw: the controller's state machines, then its FIFO, then its
 * status; the configuration stays. Bounded spins: both bits clear
 * themselves within cycles. */
static void reset(void) {
    wr(MSDC_CFG, rd(MSDC_CFG) | CFG_RST);
    for (unsigned spins = 0; spins < 100000 && (rd(MSDC_CFG) & CFG_RST); ++spins) { }
    wr(MSDC_FIFOCS, rd(MSDC_FIFOCS) | FIFOCS_CLR);
    for (unsigned spins = 0; spins < 100000 && (rd(MSDC_FIFOCS) & FIFOCS_CLR); ++spins) { }
    wr(MSDC_INT, rd(MSDC_INT));
}

/* Any failure stops the card for the boot, for both of its users, so no
 * uncertain read is ever followed by a write such as a format. Nothing in
 * flight is retried. */
static bool fail(void) {
    failed = true;
    if (controller) reset();
    controller = false;
    return false;
}

/* Until (register & mask) == value, or fail. */
static bool wait_for(unsigned offset, uint32_t mask, uint32_t value, uint64_t limit) {
    uint64_t start = time_us();
    for (;;) {
        if ((rd(offset) & mask) == value) return true;
        uint64_t waited = time_us() - start;
        if (waited >= limit) {
            /* Completion can occur while the service worker was suspended. */
            if ((rd(offset) & mask) == value) return true;
            return fail();
        }
        if (waited >= SPIN_US) kernel_io_wait();
    }
}

/* Until any of `events` is raised in MSDC_INT, which returns them. */
static uint32_t wait_events(uint32_t events, uint64_t limit) {
    uint64_t start = time_us();
    for (;;) {
        uint32_t raised = rd(MSDC_INT) & events;
        if (raised) return raised;
        uint64_t waited = time_us() - start;
        if (waited >= limit) {
            raised = rd(MSDC_INT) & events;
            if (!raised) fail();
            return raised;
        }
        if (waited >= SPIN_US) kernel_io_wait();
    }
}

/* One command with an R1 or R1b response, and with one 512-byte block when
 * `flags` asks for it. Returns the card status, or fails. Every command but
 * CMD13 waits for the card to finish programming first (DAT0 high), as
 * U-Boot's msdc_cmd_is_ready does after a write or an R1b response. */
static bool command(unsigned opcode, uint32_t arg, uint32_t flags, uint32_t *status) {
    bool data = flags & SINGLE_BLOCK, busy = (flags & RESPONSE_R1B) == RESPONSE_R1B;
    if (!controller || !wait_for(SDC_STS, STS_CMDBUSY, 0, COMMAND_US) ||
        (opcode != 13 && (!wait_for(SDC_STS, STS_SDCBUSY, 0, PROGRAM_US) ||
                          !wait_for(MSDC_PS, PS_DAT0, PS_DAT0, PROGRAM_US))))
        return false;
    /* Cleared before the command, which could otherwise lose an early data
     * event (U-Boot's fix for a hang). Write-one-to-clear. */
    wr(MSDC_INT, rd(MSDC_INT));
    if (data) {
        /* An empty FIFO before the transfer, as msdc_start_command wants. */
        wr(MSDC_FIFOCS, rd(MSDC_FIFOCS) | FIFOCS_CLR);
        if (!wait_for(MSDC_FIFOCS, FIFOCS_CLR, 0, COMMAND_US)) return false;
        wr(SDC_BLK_NUM, 1);
    }
    wr(SDC_ARG, arg);
    wr(SDC_CMD, (opcode & 63) | flags | (data ? BLOCK_LENGTH : 0));
    uint32_t raised = wait_events(INT_COMMAND, COMMAND_US);
    if (raised != INT_CMDRDY) return fail(); /* timed out or the response failed its CRC */
    wr(MSDC_INT, INT_CMDRDY);
    *status = rd(SDC_RESP0);
    if (*status & R1_ERRORS) return fail(); /* the card rejected the command */
    if (busy && (!wait_for(SDC_STS, STS_SDCBUSY, 0, PROGRAM_US) ||
                 !wait_for(MSDC_PS, PS_DAT0, PS_DAT0, PROGRAM_US)))
        return false;
    return true;
}

/* One block through the FIFO in whole words; `buffer` is word-aligned, so at
 * boot, with the MMU off, the installed resources go straight to RAM. */
static bool transfer(unsigned opcode, uint32_t sector, uint32_t *buffer, bool write) {
    uint32_t status;
    if (!command(opcode, sector, RESPONSE_R1 | SINGLE_BLOCK | (write ? WRITE_DATA : 0), &status))
        return false;
    uint64_t start = time_us();
    for (unsigned done = 0; done < 512;) {
        if (rd(MSDC_INT) & INT_DATA_ERROR) return fail(); /* data CRC or timeout */
        uint32_t fifo = rd(MSDC_FIFOCS);
        if (write && !(fifo >> 16 & 0xff)) {
            /* An empty TX FIFO takes a whole FIFO's worth. */
            for (unsigned i = 0; i < FIFO_BYTES; i += 4) wr(MSDC_TXDATA, buffer[(done + i) / 4]);
            done += FIFO_BYTES;
            continue;
        }
        if (!write && (fifo & 0xff) >= 4) {
            for (unsigned n = (fifo & 0xff) / 4; n && done < 512; --n, done += 4)
                buffer[done / 4] = rd(MSDC_RXDATA);
            continue;
        }
        uint64_t waited = time_us() - start;
        if (waited >= DATA_US) return fail(); /* the FIFO stalled */
        if (waited >= SPIN_US) kernel_io_wait();
    }
    uint32_t raised = wait_events(INT_XFER_COMPL | INT_DATA_ERROR, DATA_US);
    wr(MSDC_INT, raised);
    return raised == INT_XFER_COMPL || fail();
}

/* After a write: until the card has programmed it (DAT0 high) and is back in
 * Transfer, ready for data. */
static bool card_ready(void) {
    if (!wait_for(MSDC_FIFOCS, 0xffu << 16, 0, DATA_US) ||
        !wait_for(MSDC_PS, PS_DAT0, PS_DAT0, PROGRAM_US))
        return false;
    uint64_t start = time_us();
    for (;;) {
        uint32_t status;
        if (!command(13, rca << 16, RESPONSE_R1, &status)) return false;
        unsigned state = status >> 9 & 15;
        if (state == 4 && (status & 256)) return true;
        if (state != 7) return fail(); /* neither Transfer nor Programming */
        uint64_t waited = time_us() - start;
        if (waited >= PROGRAM_US) return fail(); /* programming timed out */
        if (waited >= SPIN_US) kernel_io_wait();
    }
}

static bool raw_read(void *context, uint64_t sector, void *data) {
    (void)context;
    if (sector >= raw_sectors) return false;
    if (!((uintptr_t)data & 3)) return transfer(17, (uint32_t)sector, data, false);
    if (!transfer(17, (uint32_t)sector, (uint32_t *)(void *)bounce, false)) return false;
    memcpy(data, bounce, 512);
    return true;
}

/* LK's controller in MMC mode with a stable clock, its DMA stopped; the card
 * at RCA 1 (Linux's and U-Boot's), 2 or 0, in Transfer or, deselected, in
 * Stand-by; then its EXT_CSD must show the user area selected, sector
 * addressing and 512-byte sectors. */
static bool adopt(void) {
    if (failed) return false;
    if (controller) return true;
    uint32_t cfg = rd(MSDC_CFG);
    if (!(cfg & CFG_MODE) || !(cfg & CFG_CKSTB) || (cfg & CFG_RST))
        return fail(); /* not as LK leaves it */
    controller = true;
    wr(MSDC_INTEN, 0); /* polled: its GIC line stays disabled too */
    if (rd(MSDC_DMA_CFG) & DMA_ACTIVE) {
        wr(MSDC_DMA_CTRL, rd(MSDC_DMA_CTRL) | DMA_STOP);
        if (!wait_for(MSDC_DMA_CFG, DMA_ACTIVE, 0, COMMAND_US)) return false;
    }
    if (!wait_for(SDC_STS, STS_CMDBUSY | STS_SDCBUSY, 0, COMMAND_US)) return false;
    wr(MSDC_CFG, rd(MSDC_CFG) | CFG_PIO);
    reset();
    wr(SDC_CFG, rd(SDC_CFG) | CFG_DTOC | CFG_WRDTOC);
    if (!wait_for(MSDC_PS, PS_DAT0, PS_DAT0, PROGRAM_US)) return false;
    static const uint32_t addresses[] = {1, 2, 0};
    uint32_t status = 0;
    unsigned found = 0;
    for (; found < 3; ++found) {
        /* A card at another RCA stays silent: CMD13's timeout is expected. */
        rca = addresses[found];
        wr(MSDC_INT, rd(MSDC_INT));
        wr(SDC_ARG, rca << 16);
        wr(SDC_CMD, 13 | RESPONSE_R1);
        uint32_t raised = wait_events(INT_COMMAND, COMMAND_US);
        if (!controller) return false;
        wr(MSDC_INT, raised);
        if (raised == INT_CMDRDY) {
            status = rd(SDC_RESP0);
            break;
        }
        reset(); /* after a timeout, as msdc_cmd_done does */
    }
    if (found == 3 || (status & R1_ERRORS)) return fail(); /* no card answers */
    unsigned state = status >> 9 & 15;
    if (state == 3 && !command(7, rca << 16, RESPONSE_R1B, &status)) return false;
    else if (state != 3 && state != 4) return fail(); /* not idle in Transfer or Stand-by */
    uint32_t *ext = (uint32_t *)(void *)bounce;
    if (!transfer(8, 0, ext, false)) return false;
    raw_sectors = (uint32_t)bounce[212] | (uint32_t)bounce[213] << 8 | (uint32_t)bounce[214] << 16 |
                  (uint32_t)bounce[215] << 24;
    /* EXT_CSD_REV [192], PARTITION_CONFIG's access [179] (0, the user area),
     * DATA_SECTOR_SIZE [61] (512 bytes), CACHE_CTRL [33]. */
    if (bounce[192] < 5 || (bounce[179] & 7) || bounce[61] || raw_sectors <= 4194304 ||
        (bounce[33] & ~1u))
        return fail(); /* an eMMC this driver does not handle */
    cache_enabled = (bounce[33] & 1) != 0;
    return true;
}

static bool flush(void) {
    /* EXT_CSD FLUSH_CACHE=1 is the driver's only CMD6. */
    uint32_t status;
    if (cache_enabled && !command(6, (3u << 24) | (32u << 16) | (1u << 8), RESPONSE_R1B, &status))
        return false;
    return card_ready();
}

static bool take(const struct platform_devices *devices, uint64_t *sectors) {
    (void)devices;
    if (!adopt()) return false;
    *sectors = raw_sectors;
    return true;
}

static bool serving(void) { return controller; }

static bool read_sector(uint64_t sector, void *data) { return raw_read(0, sector, data); }

static bool write_sector(uint64_t sector, const void *data) {
    return transfer(24, (uint32_t)sector, (uint32_t *)(uintptr_t)data, true) && card_ready();
}

const struct emmc_card emmc_card = {
    .adopt = take, .serving = serving, .read = read_sector, .write = write_sector, .flush = flush};
