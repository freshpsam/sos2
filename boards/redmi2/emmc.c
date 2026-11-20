#include "emmc_store.h"
#include "kernel_io.h"
#include "platform.h"
#include "system_store.h"

/* Native MSM8916 SDCC1 SDHCI PIO. Adopt lk2nd's tuned controller: re-enable
 * HC mode, power the bus and run the card clock again as lk2nd set them up,
 * wake RCA 2 (where lk2nd's HS200 tuning no longer reaches the card, at
 * 400 kHz, then in high-speed timing at 50 MHz), and reselect the eMMC user
 * area. No regulator, bus-width, boot-partition, RPMB, erase or tuning
 * commands.
 * See docs/redmi2.md for the pinned handoff and register references. The
 * eMMC store (kernel/protection/emmc_store.c) moves the data volume's and
 * the installed system's sectors through it. */
enum {
    CORE = 0x07824000,
    HC = 0x07824900,
    RCA = 2,
    PRESENT = 0x24,
    STATUS = 0x30,
    RESPONSE = 0x10,
    FIFO = 0x20,
    COMMAND_DONE = 1,
    TRANSFER_DONE = 2,
    WRITE_READY = 16,
    READ_READY = 32,
    POLLS = 1000000,
    COMMAND_US = 1000000,
    DATA_US = 5000000,
    /* The SDCC's request to confirm a bus power change (its power-control
     * registers, iomap.h's SDCC_HC_PWRCTL_*). */
    POWER_STATUS = 0xdc,
    POWER_CLEAR = 0xe4,
    POWER_CONTROL = 0xe8,
    POWER_US = 100000,
    /* GCC's eMMC clock source (Linux's gcc-msm8916.c sdcc1_apps_clk_src):
     * command, configuration, then M, N and D registers. The MSM controller
     * leaves its own divider unused: GCC sets the card's clock rate. */
    CLOCK_SOURCE = 0x01842004,
    /* A sector's command and transfer take microseconds: polled through on
     * the service worker, which lets the desktop run only through longer
     * waits (programming) and between pieces of work. */
    SPIN_US = 200
};

#define R1_ERRORS 0xffffa080u /* Includes CARD_IS_LOCKED as well as command errors. */

static uint32_t rd(uintptr_t address, unsigned width) {
    uint32_t value = width == 1   ? *(volatile uint8_t *)address
                     : width == 2 ? *(volatile uint16_t *)address
                                  : *(volatile uint32_t *)address;
    barrier();
    return value;
}

static void wr(uintptr_t address, uint32_t value, unsigned width) {
    barrier();
    if (width == 1) *(volatile uint8_t *)address = (uint8_t)value;
    else if (width == 2) *(volatile uint16_t *)address = (uint16_t)value;
    else *(volatile uint32_t *)address = value;
    barrier();
}

static bool controller, failed, cache_enabled;
static uint32_t raw_sectors;
static unsigned char bounce[512];

/* Any failure stops the device for the boot, for both of its users. */
static bool fail(void) {
    failed = true;
    if (controller) {
        /* No DMA is ever submitted. Stop command/data engines without changing
         * the inherited bus voltage or power. Never retry uncertain writes. */
        wr(HC + 0x38, 0, 4);
        wr(HC + 0x34, 0, 4);
        wr(HC + 0x2f, 6, 1);
    }
    controller = false;
    return false;
}

static bool wait_mask(unsigned reg, uint32_t mask, uint32_t value, uint64_t limit) {
    uint64_t start = time_us();
    for (unsigned polls = 0; polls < POLLS; ++polls) {
        if (rd(HC + STATUS, 4) & 0xffff8000u) return fail(); /* controller reported an error */
        if ((rd(HC + reg, 4) & mask) == value) return true;
        uint64_t waited = time_us() - start;
        if (waited >= limit) {
            /* Completion can occur while the service worker was suspended. */
            if (!(rd(HC + STATUS, 4) & 0xffff8000u) && (rd(HC + reg, 4) & mask) == value)
                return true;
            break;
        }
        if (waited >= SPIN_US) kernel_io_wait();
    }
    return fail(); /* controller timed out */
}

static bool command(unsigned index, uint32_t arg, bool busy, bool data, bool write) {
    if (!controller || !wait_mask(PRESENT, 3, 0, COMMAND_US)) return false;
    wr(HC + STATUS, 0xffffffffu, 4);
    wr(HC + 0x04, data ? 512 : 0, 2);
    wr(HC + 0x06, data ? 1 : 0, 2);
    wr(HC + 0x0c, data && !write ? 16 : 0, 2); /* PIO, single block, no auto commands. */
    wr(HC + 0x08, arg, 4);
    wr(HC + 0x0e, (index << 8) | 0x18 | (data ? 0x20 : 0) | (busy ? 3 : 2), 2);
    if (!wait_mask(STATUS, COMMAND_DONE, COMMAND_DONE, COMMAND_US)) return false;
    uint32_t status = rd(HC + RESPONSE, 4);
    wr(HC + STATUS, COMMAND_DONE, 4);
    if (status & R1_ERRORS) return fail(); /* card rejected command */
    if (busy) {
        if (!wait_mask(STATUS, TRANSFER_DONE, TRANSFER_DONE, DATA_US)) return false;
        wr(HC + STATUS, TRANSFER_DONE, 4);
    }
    return true;
}

static bool transfer(unsigned cmd, uint32_t sector, unsigned char *buffer, bool write) {
    if (!command(cmd, sector, false, true, write)) return false;
    unsigned event = write ? WRITE_READY : READ_READY;
    if (!wait_mask(STATUS, event, event, DATA_US)) return false;
    if (!(rd(HC + PRESENT, 4) & (write ? 1u << 10 : 1u << 11)))
        return fail(); /* PIO buffer not ready */
    wr(HC + STATUS, event, 4);
    /* Whole words for an aligned buffer: at boot, with the MMU and caches
     * off, this reads the installed resources straight into RAM. */
    uint32_t *words = ((uintptr_t)buffer & 3) ? 0 : (uint32_t *)(void *)buffer;
    for (unsigned i = 0; i < 512; i += 4) {
        if (write)
            wr(HC + FIFO,
               words ? words[i / 4]
                     : (uint32_t)buffer[i] | (uint32_t)buffer[i + 1] << 8 |
                           (uint32_t)buffer[i + 2] << 16 | (uint32_t)buffer[i + 3] << 24,
               4);
        else {
            uint32_t word = rd(HC + FIFO, 4);
            if (words) words[i / 4] = word;
            else
                for (unsigned byte = 0; byte < 4; ++byte)
                    buffer[i + byte] = (unsigned char)(word >> (byte * 8));
        }
    }
    if (!wait_mask(STATUS, TRANSFER_DONE, TRANSFER_DONE, DATA_US)) return false;
    wr(HC + STATUS, TRANSFER_DONE, 4);
    return true;
}

static bool card_ready(void) {
    uint64_t start = time_us();
    for (unsigned polls = 0; polls < 1000; ++polls) {
        if (!command(13, RCA << 16, false, false, false)) return false;
        uint32_t status = rd(HC + RESPONSE, 4), state = (status >> 9) & 15;
        if (state == 4 && (status & 256)) return true;
        if (state != 7) return fail(); /* card not in transfer state */
        uint64_t waited = time_us() - start;
        if (waited >= DATA_US) break;
        if (waited >= SPIN_US) kernel_io_wait();
    }
    return fail(); /* card programming timed out */
}

static bool raw_read(void *context, uint64_t sector, void *data) {
    (void)context;
    return sector < raw_sectors && transfer(17, (uint32_t)sector, data, false) && card_ready();
}

/* Bus power as lk2nd's sdhci_set_bus_power_on gives it: the voltage the
 * capabilities name (lk2nd's sdhci_msm_init adds 1.8 V there for this slot),
 * then power on. The SDCC holds each power change until software confirms
 * it on its power-control registers, which lk2nd does from an interrupt
 * (sdhci_int_handler, sdhci_clear_power_ctrl_irq): here by polling. */
static bool bus_power(void) {
    uint32_t caps = rd(HC + 0x40, 4);
    unsigned volts = (caps & 0x01000000u)   ? 7
                     : (caps & 0x02000000u) ? 6
                     : (caps & 0x04000000u) ? 5
                                            : 0;
    if (!volts) return fail(); /* no bus voltage in the capabilities */
    wr(HC + 0x29, volts << 1, 1);
    wr(HC + 0x29, volts << 1 | 1, 1);
    uint32_t request = 0;
    for (uint64_t start = time_us(); time_us() - start < POWER_US;)
        if ((request = rd(CORE + POWER_STATUS, 4) & 15)) break;
    if (request) {
        wr(CORE + POWER_CLEAR, request, 4);
        wr(CORE + POWER_CONTROL, ((request & 3) ? 1u : 0) | ((request & 12) ? 4u : 0), 4);
    }
    return (rd(HC + 0x29, 1) & 1) || fail(); /* bus power stays off */
}

/* A command the card may leave unanswered: 1 answered, its R1 in RESPONSE
 * for the caller to judge; 0 unanswered or garbled (a timeout, CRC, end bit
 * or index error on the command line), with the controller's command and
 * data lines reset for the next command; -1 any other failure (stopped). */
static int ask(unsigned index, uint32_t arg, bool busy) {
    if (!controller || !wait_mask(PRESENT, 3, 0, COMMAND_US)) return -1;
    wr(HC + STATUS, 0xffffffffu, 4);
    wr(HC + 0x04, 0, 2);
    wr(HC + 0x06, 0, 2);
    wr(HC + 0x0c, 0, 2);
    wr(HC + 0x08, arg, 4);
    wr(HC + 0x0e, (index << 8) | 0x18 | (busy ? 3 : 2), 2);
    uint32_t status;
    for (uint64_t start = time_us(); !((status = rd(HC + STATUS, 4)) & 0x8001u);)
        if (time_us() - start >= COMMAND_US) {
            fail(); /* controller timed out */
            return -1;
        }
    if ((status & 0x8000u) && !(status & 0xfff00000u)) { /* Command-line errors alone. */
        wr(HC + STATUS, 0xffffffffu, 4);
        wr(HC + 0x2f, 6, 1);
        for (uint64_t start = time_us(); rd(HC + 0x2f, 1) & 6;)
            if (time_us() - start >= COMMAND_US) {
                fail(); /* line reset stuck */
                return -1;
            }
        return 0;
    }
    if (status & 0xffff8000u) {
        fail(); /* controller reported an error */
        return -1;
    }
    wr(HC + STATUS, COMMAND_DONE, 4);
    if (busy) {
        if (!wait_mask(STATUS, TRANSFER_DONE, TRANSFER_DONE, DATA_US)) return -1;
        wr(HC + STATUS, TRANSFER_DONE, 4);
    }
    return 1;
}

/* The card's clock rate, set at GCC as Linux's clk-rcg2 sets it: 400 kHz is
 * the 19.2 MHz crystal / 12 with M/N 1/4, in dual-edge mode; 50 MHz is GPLL0
 * / 16. The card's clock is stopped meanwhile (bus_speed). */
static bool clock_rate(bool fast) {
    if (!fast) {
        wr(CLOCK_SOURCE + 8, (rd(CLOCK_SOURCE + 8, 4) & ~0xffu) | 1, 4);
        wr(CLOCK_SOURCE + 12, (rd(CLOCK_SOURCE + 12, 4) & ~0xffu) | (~3u & 0xff), 4);
        wr(CLOCK_SOURCE + 16, (rd(CLOCK_SOURCE + 16, 4) & ~0xffu) | (~4u & 0xff), 4);
    }
    uint32_t config = fast ? 1u << 8 | (2 * 16 - 1) : 2u << 12 | (2 * 12 - 1);
    wr(CLOCK_SOURCE + 4, (rd(CLOCK_SOURCE + 4, 4) & ~0x371fu) | config, 4);
    wr(CLOCK_SOURCE, rd(CLOCK_SOURCE, 4) | 1, 4); /* Take the new configuration. */
    for (uint64_t start = time_us(); rd(CLOCK_SOURCE, 4) & 1;)
        if (time_us() - start >= 1000) return fail(); /* clock source keeps its old rate */
    return true;
}

/* The bus at 400 kHz, where the card answers without lk2nd's HS200 tuning,
 * or at 50 MHz in high-speed mode, the way Linux's sdhci-msm changes speed:
 * the card's clock stopped while GCC's rate and the host's timing change.
 * A UHS mode under SDR104 makes the controller sample on its feedback
 * clock, not the HS200 delay line. */
static bool bus_speed(bool fast) {
    uint32_t clock = rd(HC + 0x2c, 2), host = rd(HC + 0x28, 1);
    wr(HC + 0x2c, clock & ~4u, 2);
    wr(HC + 0x3e, rd(HC + 0x3e, 2) & ~7u, 2);
    wr(HC + 0x28, fast ? host | 4 : host & ~4u, 1);
    if (!clock_rate(fast)) return false;
    for (uint64_t start = time_us(); !(rd(HC + 0x2c, 2) & 2);)
        if (time_us() - start >= 10000) return fail(); /* internal clock unstable */
    wr(HC + 0x2c, rd(HC + 0x2c, 2) | 4, 2);
    /* The card's 74 clock cycles before a command, generously. */
    delay_us(1000);
    return true;
}

/* Wake the card lk2nd put to sleep and select it. At lk2nd's HS200 speed
 * first; a card that does not answer there gets the bus at 400 kHz, the
 * wake once more, and, unanswered again (it was awake already), a status
 * request that says which state it is in. A card woken at 400 kHz then
 * leaves HS200 for high-speed timing, and the bus runs at 50 MHz. */
static bool wake(void) {
    /* An answer may still report an earlier command the card ignored: its
     * CRC failed (COM_CRC_ERROR) or, awake already, a wake was illegal. */
    const uint32_t earlier = 1u << 23 | 1u << 22;
    bool slow = false, selected = false;
    int answer = ask(5, RCA << 16, true);
    if (!answer) {
        if (!bus_speed(false)) return false;
        slow = true;
        answer = ask(5, RCA << 16, true);
    }
    if (answer < 0) return false;
    uint32_t status = rd(HC + RESPONSE, 4);
    if (answer && (status & R1_ERRORS & ~earlier)) return fail(); /* card rejected the wake */
    if (!answer) {
        /* Unanswered at 400 kHz too: the card is awake. Its state says
         * whether it still needs selecting. */
        answer = ask(13, RCA << 16, false);
        if (answer < 0) return false;
        if (!answer) return fail(); /* card answers neither wake nor status */
        status = rd(HC + RESPONSE, 4);
        uint32_t state = (status >> 9) & 15;
        if ((status & R1_ERRORS & ~earlier) || (state != 3 && state != 4))
            return fail(); /* card in another state */
        selected = state == 4;
    }
    if ((!selected && !command(7, RCA << 16, true, false, false)) || !card_ready()) return false;
    if (!slow) return true;
    /* HS_TIMING 1, high speed: an EXT_CSD write byte, then the faster bus. */
    return command(6, (3u << 24) | (185u << 16) | (1u << 8), true, false, false) && card_ready() &&
           bus_speed(true);
}

/* Take over lk2nd's controller once per boot; true while it serves. */
static bool adopt(const struct platform_devices *devices) {
    if (controller) return true;
    if (failed) return false;
    cache_enabled = false;
    raw_sectors = 0;
    if (!devices || !devices->redmi2_wt88047) return fail(); /* unsupported board */
    uint32_t version = rd(CORE + 0x50, 4), mode = rd(CORE + 0x78, 4);
    /* lk2nd's handoff turns HC mode off (sdhci_mode_disable), and the whole
     * register then reads zero on this phone, FF_CLK_SW_RST_DIS included.
     * Both go back on, as lk2nd's sdhci_msm_init and Linux's sdhci-msm set
     * them. */
    if ((version >> 28) != 1 || (mode & 1)) return fail(); /* SDCC not as lk2nd leaves it */
    wr(CORE + 0x78, mode | 1 | 1u << 13, 4);
    /* An idle SDHCI v3 with its internal clock running. Its registers are
     * read only once HC mode is on. */
    uint32_t enabled = rd(CORE + 0x78, 4), spec = 0, clock = 0, power = 0, present = 0;
    if (enabled & 1) {
        spec = rd(HC + 0xfe, 2);
        clock = rd(HC + 0x2c, 2);
        power = rd(HC + 0x29, 1);
        present = rd(HC + PRESENT, 4);
    }
    if (!(enabled & 1) || (spec & 255) != 2 || (clock & 3) != 3 || (present & 3)) {
        wr(CORE + 0x78, mode, 4);
        return fail(); /* idle, powered SDHCI v3 required */
    }
    /* Preserve the trained signaling mode; disable all CPU interrupts and DMA
     * selection. Poll only events needed by one single-block PIO request. */
    controller = true;
    wr(HC + 0x38, 0, 4);
    wr(HC + 0x34, 0xffff0033u, 4);
    wr(HC + STATUS, 0xffffffffu, 4);
    wr(HC + 0x28, rd(HC + 0x28, 1) & ~0x18u, 1);
    wr(HC + 0x2e, 14, 1);
    /* With HC mode off, lk2nd's handoff left the bus unpowered and the card's
     * clock stopped, its divider and the card's own state kept: power the
     * bus, then run the clock again, in sdhci_init's order. */
    if (!(power & 1) && !bus_power()) return false;
    if (!(clock & 4)) {
        wr(HC + 0x2c, clock | 4, 2);
        /* The card's 74 clock cycles before a command, generously. */
        delay_us(1000);
    }
    if (!wake() || !transfer(8, 0, bounce, false)) return false;
    unsigned timing = bounce[185] & 15, signaling = rd(HC + 0x3e, 2) & 7;
    /* Only high-capacity eMMC in the user partition with 512-byte logical
     * sectors, in a timing that matches the host's mode: HS200 with SDR104,
     * high speed with SDR or DDR50, backward-compatible timing with SDR.
     * BUS_WIDTH (EXT_CSD[183]) is write-only and reads zero: this EXT_CSD,
     * read intact over the host's data lines, shows the width works. No
     * automatic partition switch. */
    raw_sectors = (uint32_t)bounce[212] | (uint32_t)bounce[213] << 8 | (uint32_t)bounce[214] << 16 |
                  (uint32_t)bounce[215] << 24;
    if (bounce[192] < 5 || bounce[192] > 8 || bounce[61] || bounce[62] || (bounce[179] & 7) ||
        raw_sectors <= 4194304 || timing > 2 ||
        (timing == 2 ? signaling != 3 : signaling > 1 && (timing != 1 || signaling != 4)) ||
        (bounce[33] & ~1u) || ((bounce[33] & 1) && bounce[192] < 6))
        return fail(); /* unsupported eMMC handoff */
    cache_enabled = (bounce[33] & 1) != 0;
    return true;
}

static bool flush(void) {
    /* EXT_CSD FLUSH_CACHE=1 is the driver's only CMD6 besides wake's
     * HS_TIMING. */
    if (cache_enabled && !command(6, (3u << 24) | (32u << 16) | (1u << 8), true, false, false))
        return false;
    return card_ready();
}

static bool take(const struct platform_devices *devices, uint64_t *sectors) {
    if (!adopt(devices)) return false;
    *sectors = raw_sectors;
    return true;
}

static bool serving(void) { return controller; }

static bool read_sector(uint64_t sector, void *data) { return raw_read(0, sector, data); }

static bool write_sector(uint64_t sector, const void *data) {
    return transfer(24, (uint32_t)sector, (unsigned char *)(uintptr_t)data, true) && card_ready();
}

/* The data volume, without an installed system, is the dedicated sos2-data
 * partition; the installed system's boot image follows lk2nd's 512 KiB. */
const struct emmc_card emmc_card = {.adopt = take,
                                    .serving = serving,
                                    .read = read_sector,
                                    .write = write_sector,
                                    .flush = flush,
                                    .loader_sectors = SYSTEM_LK2ND_SECTORS,
                                    .dedicated_partition = true};
