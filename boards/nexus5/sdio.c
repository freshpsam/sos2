#include "nexus5.h"
#include "kernel_io.h"
#include "platform.h"
#include "sdhci.h"

/* The Wi-Fi chip's SDIO bus: the BCM4339 on SDCC2, as Linux's
 * qcom-msm8974-lge-nexus5-hammerhead.dts describes it (sdhc_2: 4 bits, not
 * removable), the controller run by sdhci.c. The card comes up as Linux's
 * MMC core brings up an SDIO card (drivers/mmc/core/sdio.c, sdio_ops.c):
 * CMD5's operating conditions, CMD3's address, CMD7's selection, the CIS's
 * identity, then high speed at 50 MHz and a 4-bit bus through the CCCR.
 * Transfers are CMD52 (a byte) and CMD53 (bytes, or blocks of the
 * function's block size). */

enum {
    GCC = 0xfc400000,
    BLOCK = 512,
    R5_ERRORS = 0x0b00, /* error, function number, out of range */
};

/* SDC2's pads (Linux's pinctrl-msm8x74.c, TLMM_SDC2_HDRV_PULL_CTL): the
 * clock without pulls, command and data pulled up, all at 6 mA. */
static struct sdhci host = {
    .core = 0xf98a4000,
    .hc = 0xf98a4900,
    .apps_cbcr = GCC + 0x0504,
    .ahb_cbcr = GCC + 0x0508,
    .clock_source = GCC + 0x0510,
    .pads = 0xfd512048,
    .pads_mask = 0xdfff,
    .pads_value = (0u << 14 | 2u << 6) | (3u << 11 | 2u << 3) | (3u << 9 | 2u << 0),
    .yield = kernel_io_wait,
};

static bool ready, failed;
static unsigned block_size[3];

static bool stop(void) {
    failed = true;
    ready = false;
    return false;
}

/* CMD52 (IO_RW_DIRECT): one byte, read or written (and read back). R5's
 * flags fail it as Linux's mmc_io_rw_direct_host judges them: a general
 * error, a function number or an address out of range. */
static bool direct(unsigned function, uint32_t address, uint8_t *value, bool write) {
    uint32_t response;
    uint32_t argument = (write ? 1u << 31 : 0) | (uint32_t)function << 28 | (write ? 1u << 27 : 0) |
                        (address & 0x1ffff) << 9 | (write ? *value : 0);
    if (!sdhci_command(&host, 52, argument, SDHCI_R1, R5_ERRORS, &response)) return false;
    *value = (uint8_t)response;
    return true;
}

bool nexus5_sdio_byte(unsigned function, uint32_t address, uint8_t *value, bool write) {
    return ready && function <= 2 && value && direct(function, address, value, write);
}

/* CMD53 (IO_RW_EXTENDED): count bytes (1-512) or blocks of the function's
 * block size. */
static bool extended(unsigned function, uint32_t address, uint8_t *data, unsigned count,
                     bool blocks, bool write, bool increment) {
    unsigned size = blocks ? block_size[function] : count;
    if (!size || size > BLOCK || !count || count > (blocks ? 511u : 512u)) return false;
    uint32_t argument = (write ? 1u << 31 : 0) | (uint32_t)function << 28 |
                        (blocks ? 1u << 27 : 0) | (increment ? 1u << 26 : 0) |
                        (address & 0x1ffff) << 9 | (count & 511);
    return sdhci_data(&host, 53, argument, R5_ERRORS, data, size, blocks ? count : 1, write);
}

/* size bytes at address of a function: whole blocks while more than a block
 * remains, then the rest in byte mode, 512 at a time. increment walks the
 * address; a FIFO (false) keeps it. */
bool nexus5_sdio_transfer(unsigned function, uint32_t address, void *data, unsigned size,
                          bool write, bool increment) {
    if (!ready || function > 2 || !data || !size) return false;
    uint8_t *p = data;
    unsigned block = block_size[function];
    while (size) {
        unsigned n;
        if (block && size > block) {
            unsigned blocks = size / block > 511 ? 511 : size / block;
            n = blocks * block;
            if (!extended(function, address, p, blocks, true, write, increment)) return false;
        } else {
            n = size > BLOCK ? BLOCK : size;
            if (!extended(function, address, p, n, false, write, increment)) return false;
        }
        p += n;
        size -= n;
        if (increment) address += n;
    }
    return true;
}

static bool cccr(uint32_t address, uint8_t *value) { return direct(0, address, value, false); }

static bool cccr_set(uint32_t address, uint8_t value) {
    uint8_t v = value;
    return direct(0, address, &v, true) && v == value;
}

/* The card's maker from its common CIS: CISTPL_MANFID's manufacturer ID. */
static bool identity(uint16_t *manufacturer) {
    uint8_t p[3];
    for (unsigned i = 0; i < 3; ++i)
        if (!cccr(0x09 + i, p + i)) return false;
    uint32_t at = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    for (unsigned tuples = 0; tuples < 64 && at < 0x20000; ++tuples) {
        uint8_t code, link;
        if (!cccr(at, &code) || code == 0xff) return false;
        if (!cccr(at + 1, &link)) return false;
        if (code == 0x20 && link >= 4) {
            uint8_t b[2];
            for (unsigned i = 0; i < 2; ++i)
                if (!cccr(at + 2 + i, b + i)) return false;
            *manufacturer = (uint16_t)(b[0] | b[1] << 8);
            return true;
        }
        at += 2u + link;
    }
    return false;
}

/* A function's block size, in its FBR (function 0's in the CCCR). */
bool nexus5_sdio_block_size(unsigned function, unsigned size) {
    if (!ready || function > 2 || !size || size > BLOCK) return false;
    uint32_t base = function ? 0x100u * function : 0;
    if (!cccr_set(base + 0x10, (uint8_t)size) || !cccr_set(base + 0x11, (uint8_t)(size >> 8)))
        return false;
    block_size[function] = size;
    return true;
}

/* Function enable (CCCR IOEx) and, when enabling, its ready bit (IORx). */
bool nexus5_sdio_function(unsigned function, bool on, unsigned wait_ms) {
    uint8_t enable;
    if (!ready || !function || function > 7 || !cccr(0x02, &enable)) return false;
    enable = on ? enable | (uint8_t)(1u << function) : enable & (uint8_t)~(1u << function);
    if (!cccr_set(0x02, enable)) return false;
    if (!on) return true;
    for (uint64_t start = time_us();;) {
        uint8_t io_ready;
        if (!cccr(0x03, &io_ready)) return false;
        if (io_ready & (1u << function)) return true;
        if (time_us() - start >= (uint64_t)wait_ms * 1000) return false;
        kernel_io_wait();
    }
}

/* The card: CMD0, CMD5 until it is ready at 3.2-3.4 V (its own supply; its
 * I/O stays at the board's 1.8 V), its address, selected, its identity;
 * then, in Linux's order (mmc_sdio_init_card), high speed (the BCM4339 has
 * SHS) and the bus at 50 MHz, then the 4-bit bus (card detection off: the
 * chip is soldered). */
static bool card(uint16_t *manufacturer) {
    uint32_t ocr = 0;
    (void)sdhci_command(&host, 0, 0, SDHCI_NONE, 0, 0);
    delay_us(1000);
    if (!sdhci_command(&host, 5, 0, SDHCI_R3, 0, &ocr)) return stop();
    if (!(ocr & 0x00300000u)) return stop();
    bool up = false;
    for (uint64_t start = time_us(); !up && time_us() - start < 1000000;) {
        if (!sdhci_command(&host, 5, ocr & 0x00300000u, SDHCI_R3, 0, &ocr)) return stop();
        up = ocr & 0x80000000u;
        if (!up) delay_us(10000);
    }
    if (!up) return stop();
    uint32_t response;
    if (!sdhci_command(&host, 3, 0, SDHCI_R1, 0, &response)) return stop();
    if (!sdhci_command(&host, 7, response & 0xffff0000u, SDHCI_R1, 0, 0)) return stop();
    ready = true;
    if (!identity(manufacturer)) return stop();
    uint8_t bus, speed;
    if (!cccr(0x13, &speed) || !(speed & 1)) return stop();
    if (!cccr_set(0x13, (uint8_t)(speed | 2))) return stop();
    sdhci_bus(&host, 1, true);
    if (!sdhci_clock(&host, true)) return false;
    if (!cccr(0x07, &bus) || !cccr_set(0x07, (uint8_t)((bus & ~3u) | 0x80 | 2))) return stop();
    sdhci_bus(&host, 4, true);
    return true;
}

void nexus5_sdio_stop(void) { (void)stop(); }

const char *nexus5_sdio_start(void) {
    if (ready) return 0;
    if (failed) return "Wi-Fi bus failed";
    block_size[0] = block_size[1] = block_size[2] = 0;
    const char *why = nexus5_wifi_power();
    if (why) {
        failed = true;
        return why;
    }
    uint16_t manufacturer = 0;
    if (!sdhci_start(&host) || !card(&manufacturer)) {
        failed = true;
        ready = false;
        return "Wi-Fi chip does not answer";
    }
    if (manufacturer != 0x02d0) return "Wi-Fi chip is not Broadcom's";
    return 0;
}
