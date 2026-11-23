#include "emmc_store.h"
#include "kernel_io.h"
#include "nexus5.h"
#include "platform.h"
#include "sdhci.h"

/* The Nexus 5's eMMC on SDCC1, as Linux's qcom-msm8974.dtsi and the
 * hammerhead device tree describe it (sdhc_1: 8 bits, its I/O at PM8941
 * S3's 1.8 V), the controller run by boards/qcom/sdhci.c. The stock
 * bootloader hands it over with HC mode off and the SDHCI registers as
 * after a reset (bus power and card clock off, one bit wide), so the card
 * is started afresh as Linux's MMC core starts an eMMC
 * (drivers/mmc/core/mmc.c, mmc_ops.c): CMD0 back to idle, CMD1 until it is
 * ready (sector addressing, its 1.8 V I/O), its CID, address 2, selected;
 * its EXT_CSD, then high-speed timing at 50 MHz and an 8-bit bus, checked
 * by reading the EXT_CSD again. A read of several sectors is one CMD18,
 * which the host stops with its own CMD12, and of one sector CMD17; so are
 * writes (CMD25 or CMD24, then CMD13 until programming ends). The card's
 * cache stays off, so a written sector is on the flash. */

enum {
    GCC = 0xfc400000,
    RCA = 2,
    /* R1's error bits (address, length, erase, protection, lock, CRC, illegal
     * command, ECC, controller, CID/CSD, switch). */
    R1_ERRORS = 0xffffa080u,
    STATE_TRANSFER = 4,
    READY_FOR_DATA = 1 << 8,
    PROGRAM_US = 2000000,
};

/* SDC1's pads (Linux's pinctrl-msm8x74.c, TLMM_SDC1_HDRV_PULL_CTL), as the
 * hammerhead device tree sets them: the clock at 16 mA without pulls,
 * command and data at 10 mA pulled up. */
static struct sdhci host = {
    .core = 0xf9824000,
    .hc = 0xf9824900,
    .apps_cbcr = GCC + 0x04c4,
    .ahb_cbcr = GCC + 0x04c8,
    .clock_source = GCC + 0x04d0,
    .pads = 0xfd512044,
    .pads_mask = 0x7fff,
    .pads_value = (0u << 13 | 7u << 6) | (3u << 11 | 4u << 3) | (3u << 9 | 4u << 0),
    .busy_us = PROGRAM_US,
    .auto_stop = true,
    .yield = kernel_io_wait,
};

/* busy: inside a transfer, which the service worker may have left waiting
 * (its yield) while the main loop runs. */
static bool ready, failed, busy;
static uint32_t sector_count;
static uint8_t ext_csd[512] __attribute__((aligned(4)));

static bool stop(void) {
    failed = true;
    ready = false;
    return false;
}

static bool status(uint32_t *out) {
    return sdhci_command(&host, 13, RCA << 16, SDHCI_R1, R1_ERRORS, out);
}

/* The card back in its transfer state with nothing to program. */
static bool card_ready(void) {
    for (uint64_t start = time_us();;) {
        uint32_t s;
        if (!status(&s)) return stop();
        unsigned state = s >> 9 & 15;
        if (state == STATE_TRANSFER && (s & READY_FOR_DATA)) return true;
        if (state != 7 && state != STATE_TRANSFER) return stop();
        if (time_us() - start >= PROGRAM_US) return stop();
        if (host.yield) host.yield();
    }
}

/* CMD6 SWITCH: one EXT_CSD byte written, then the card's status. */
static bool switch_byte(unsigned index, unsigned value) {
    uint32_t s;
    return sdhci_command(&host, 6, 3u << 24 | index << 16 | value << 8, SDHCI_R1B, R1_ERRORS, 0) &&
           card_ready() && status(&s) && !(s & 0x80);
}

static bool read_ext_csd(uint8_t *out) {
    return sdhci_data(&host, 8, 0, R1_ERRORS, out, 512, 1, false);
}

static bool card(void) {
    (void)sdhci_command(&host, 0, 0, SDHCI_NONE, 0, 0);
    delay_us(1000);
    uint32_t ocr = 0;
    bool up = false;
    for (uint64_t start = time_us(); !up && time_us() - start < 1000000;) {
        if (!sdhci_command(&host, 1, 0x40ff8080u, SDHCI_R3, 0, &ocr)) return stop();
        up = ocr & 0x80000000u;
        if (!up) delay_us(10000);
    }
    if (!up) return stop();
    if (!(ocr & 0x40000000u)) return stop();
    if (!sdhci_command(&host, 2, 0, SDHCI_R2, 0, 0) ||
        !sdhci_command(&host, 3, RCA << 16, SDHCI_R1, R1_ERRORS, 0) ||
        !sdhci_command(&host, 7, RCA << 16, SDHCI_R1B, R1_ERRORS, 0))
        return stop();
    if (!read_ext_csd(ext_csd)) return stop();
    sector_count = (uint32_t)ext_csd[212] | (uint32_t)ext_csd[213] << 8 |
                   (uint32_t)ext_csd[214] << 16 | (uint32_t)ext_csd[215] << 24;
    /* Only eMMC 4.4 and later, its user area of 512-byte sectors chosen. */
    if (ext_csd[192] < 5 || ext_csd[61] || (ext_csd[179] & 7) || sector_count <= 4194304)
        return stop();
    if (!switch_byte(185, 1)) return stop();
    sdhci_bus(&host, 1, true);
    if (!sdhci_clock(&host, true)) return false;
    if (!switch_byte(183, 2)) return stop();
    sdhci_bus(&host, 8, true);
    static uint8_t check[512] __attribute__((aligned(4)));
    if (!read_ext_csd(check) || memcmp(check + 212, ext_csd + 212, 4)) return stop();
    return true;
}

static bool adopt(const struct platform_devices *devices, uint64_t *sectors) {
    if (!ready) {
        if (failed || busy || !devices || !devices->nexus5_hammerhead) return false;
        busy = true;
        ready = sdhci_start(&host) && card();
        failed = !ready;
        busy = false;
        if (!ready) return false;
    }
    *sectors = sector_count;
    return true;
}

static bool serving(void) { return ready; }

bool nexus5_emmc_idle(void) { return !busy; }

static bool read_sector(uint64_t sector, void *data) {
    if (!ready || busy || sector >= sector_count) return false;
    busy = true;
    bool done = sdhci_data(&host, 17, (uint32_t)sector, R1_ERRORS, data, 512, 1, false);
    busy = false;
    return done || stop();
}

/* CMD18: count sectors in one transfer, which the host stops with CMD12
 * (one sector is CMD17's: CMD18 would leave the card sending). */
static bool read_sectors(uint64_t sector, void *data, unsigned count) {
    if (count == 1) return read_sector(sector, data);
    if (!ready || busy || !count || sector >= sector_count || count > sector_count - sector)
        return false;
    busy = true;
    bool done = sdhci_data(&host, 18, (uint32_t)sector, R1_ERRORS, data, 512, count, false);
    busy = false;
    return done || stop();
}

static bool write_sector(uint64_t sector, const void *data) {
    if (!ready || busy || sector >= sector_count) return false;
    busy = true;
    bool done = sdhci_data(&host, 24, (uint32_t)sector, R1_ERRORS, (uint8_t *)(uintptr_t)data, 512,
                           1, true) ||
                stop();
    done = done && card_ready();
    busy = false;
    return done;
}

/* CMD25: count sectors in one transfer, which the host stops with CMD12,
 * then CMD13 until the card has programmed them all (one sector is CMD24's,
 * as one read is CMD17's). */
static bool write_sectors(uint64_t sector, const void *data, unsigned count) {
    if (count == 1) return write_sector(sector, data);
    if (!ready || busy || !count || count > 8 || sector >= sector_count ||
        count > sector_count - sector)
        return false;
    busy = true;
    bool done = sdhci_data(&host, 25, (uint32_t)sector, R1_ERRORS, (uint8_t *)(uintptr_t)data, 512,
                           count, true) ||
                stop();
    done = done && card_ready();
    busy = false;
    return done;
}

static bool flush(void) {
    if (!ready || busy) return false;
    busy = true;
    bool done = card_ready();
    busy = false;
    return done;
}

/* The installed system's boot image starts the boot partition (the stock
 * bootloader keeps nothing there); without one, the phone keeps no data. */
const struct emmc_card emmc_card = {.adopt = adopt,
                                    .serving = serving,
                                    .read = read_sector,
                                    .read_many = read_sectors,
                                    .write = write_sector,
                                    .write_many = write_sectors,
                                    .flush = flush,
                                    .loader_sectors = 0,
                                    .dedicated_partition = false};
