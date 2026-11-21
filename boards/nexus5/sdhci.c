#include "sdhci.h"
#include "nexus5.h"
#include "platform.h"

/* MSM8974's SD controllers: an SDHCI 3.0 (HOST_VERSION 0x3802) behind
 * Qualcomm's wrapper, as Linux's sdhci-msm.c ("qcom,sdhci-msm-v4") runs it,
 * polled in PIO. Started cold: the bootloader leaves SDCC2 off, and SDCC1
 * with HC mode off and its SDHCI registers as after a reset. HC mode goes on
 * with software resets kept off the feedback clock's domain, which samples
 * the card's responses; the vendor register goes to its power-on value with
 * the I/O pads switched to 1.8 V (both buses' I/O supply is PM8941 S3);
 * each bus power or voltage change is confirmed on the wrapper's power
 * control registers, which Linux does from an interrupt; and the card's
 * clock is set at GCC (sdcc*_apps_clk_src in Linux's gcc-msm8974.c), the
 * SDHCI divider unused. A status clear lands late while the bus runs at
 * 400 kHz: a command sent right after one finished with nothing latched
 * (2026-11-20), so commands wait 40 us after a clear then, as the stock
 * kernel's SLOW_INT_CLR has its older controllers do. */

enum {
    /* The wrapper (sdhci-msm.c CORE_*). */
    MCI_GENERICS = 0x70,
    SWITCHABLE_SIGNALLING = 1u << 29,
    HC_MODE = 0x78,
    HC_MODE_ENABLE = 1,
    FF_CLOCK_RESET_OFF = 1 << 13, /* FF_CLK_SW_RST_DIS */
    POWER_STATUS = 0xdc,
    POWER_MASK = 0xe0,
    POWER_CLEAR = 0xe4,
    POWER_CONTROL = 0xe8,
    POWER_REQUESTS = 15, /* bus off, bus on, I/O low, I/O high */
    POWER_US = 100000,
    /* SDHCI. */
    BLOCK_SIZE = 0x04,
    BLOCK_COUNT = 0x06,
    ARGUMENT = 0x08,
    TRANSFER_MODE = 0x0c,
    COMMAND = 0x0e,
    RESPONSE = 0x10,
    BUFFER = 0x20,
    PRESENT = 0x24,
    HOST1 = 0x28,
    POWER = 0x29,
    CLOCK = 0x2c,
    TIMEOUT = 0x2e,
    RESET = 0x2f,
    STATUS = 0x30,
    STATUS_ENABLE = 0x34,
    SIGNAL_ENABLE = 0x38,
    HOST2 = 0x3e,
    CAPABILITIES = 0x40,
    DLL_CONFIG = 0x100,
    /* Qualcomm's vendor register (msm_v4: 0x10c): its power-on value, then
     * the I/O pads at 1.8 V, as sdhci_msm_set_regulator_caps sets them when
     * the I/O supply is 1.8 V only. */
    VENDOR_SPEC = 0x10c,
    VENDOR_SPEC_RESET = 0xa9c,
    PAD_SWITCH_ENABLE = 1u << 15,
    PAD_1V8 = 1u << 16,
    COMMAND_DONE = 1,
    TRANSFER_DONE = 2,
    WRITE_READY = 16,
    READ_READY = 32,
    ERRORS = 0xffff8000u,
    GPLL0_VOTE = 0xfc401480,
    COMMAND_US = 100000,
    DATA_US = 500000,
    SPIN_US = 200,
};

static uint32_t rd(uintptr_t address, unsigned width) {
    uint32_t value = width == 1   ? mmio_read8(address)
                     : width == 2 ? mmio_read16(address)
                                  : mmio_read32(address);
    barrier();
    return value;
}

static void wr(uintptr_t address, uint32_t value, unsigned width) {
    barrier();
    if (width == 1) mmio_write8(address, (uint8_t)value);
    else if (width == 2) mmio_write16(address, (uint16_t)value);
    else mmio_write32(address, value);
    barrier();
}

static bool stop(struct sdhci *h) {
    h->failed = true;
    return false;
}

/* Confirms what the wrapper asks after a power or voltage change (bus on or
 * off, I/O low or high), as sdhci_msm_handle_pwr_irq does: the request
 * cleared (again while it shows: a first clear can be lost), then success
 * for it, a bus change counting as the I/O's too. Waits up to POWER_US for
 * a request when one is due. */
static bool confirm_power(struct sdhci *h, bool due) {
    uint32_t request = 0;
    for (uint64_t start = time_us(); !(request = rd(h->core + POWER_STATUS, 4) & POWER_REQUESTS);)
        if (!due || time_us() - start >= POWER_US) return !due;
    for (unsigned tries = 0; tries < 10 && (rd(h->core + POWER_STATUS, 4) & POWER_REQUESTS);
         ++tries) {
        wr(h->core + POWER_CLEAR, rd(h->core + POWER_STATUS, 4) & POWER_REQUESTS, 4);
        delay_us(10);
    }
    wr(h->core + POWER_CONTROL, ((request & 3) ? 5u : 0) | ((request & 12) ? 4u : 0), 4);
    return true;
}

/* Status bits cleared (write 1 to clear), with the wait a clear needs at
 * 400 kHz. */
static void clear(struct sdhci *h, uint32_t bits) {
    wr(h->hc + STATUS, bits, 4);
    if (h->slow) delay_us(40);
}

static bool wait_branch(uintptr_t cbcr) {
    for (uint64_t start = time_us(); rd(cbcr, 4) & 0x80000000u;)
        if (time_us() - start >= 1000) return false;
    return true;
}

/* The card's clock at GCC, as clk-rcg2 sets it from the frequency table:
 * 400 kHz is the 19.2 MHz crystal / 12 with M/N 1/4 in dual-edge mode;
 * 50 MHz is GPLL0 (600 MHz) / 12. */
static bool clock_rate(struct sdhci *h, bool fast) {
    uintptr_t source = h->clock_source;
    if (!fast) {
        wr(source + 8, (rd(source + 8, 4) & ~0xffu) | 1, 4);
        wr(source + 12, (rd(source + 12, 4) & ~0xffu) | (~3u & 0xff), 4);
        wr(source + 16, (rd(source + 16, 4) & ~0xffu) | (~4u & 0xff), 4);
    }
    uint32_t config = fast ? 1u << 8 | (2 * 12 - 1) : 2u << 12 | (2 * 12 - 1);
    wr(source + 4, (rd(source + 4, 4) & ~0x371fu) | config, 4);
    wr(source, rd(source, 4) | 1, 4);
    for (uint64_t start = time_us(); rd(source, 4) & 1;)
        if (time_us() - start >= 1000) return stop(h);
    return true;
}

/* The bus clock changed with the card's clock stopped, then running again
 * for at least the card's 74 cycles before a command. */
bool sdhci_clock(struct sdhci *h, bool fast) {
    wr(h->hc + CLOCK, rd(h->hc + CLOCK, 2) & ~4u, 2);
    if (!clock_rate(h, fast)) return false;
    h->slow = !fast;
    for (uint64_t start = time_us(); !(rd(h->hc + CLOCK, 2) & 2);)
        if (time_us() - start >= 10000) return stop(h);
    wr(h->hc + CLOCK, rd(h->hc + CLOCK, 2) | 4, 2);
    delay_us(1000);
    return true;
}

void sdhci_bus(struct sdhci *h, unsigned bits, bool high_speed) {
    uint32_t host = rd(h->hc + HOST1, 1) & ~(2u | 4u | 32u);
    host |= (bits == 8 ? 32u : bits == 4 ? 2u : 0) | (high_speed ? 4u : 0);
    wr(h->hc + HOST1, host, 1);
}

static uint64_t busy_limit(const struct sdhci *h) { return h->busy_us ? h->busy_us : DATA_US; }

static bool wait_status(struct sdhci *h, uint32_t mask, uint64_t limit) {
    uint64_t start = time_us();
    for (;;) {
        uint32_t status = rd(h->hc + STATUS, 4);
        if (status & ERRORS) return false;
        if (status & mask) return true;
        uint64_t waited = time_us() - start;
        if (waited >= limit) return false;
        if (waited >= SPIN_US && h->yield) h->yield();
    }
}

/* After an error: the error bits cleared and the command and data lines
 * reset for the next command. */
static void recover(struct sdhci *h) {
    clear(h, 0xffffffffu);
    wr(h->hc + RESET, 6, 1);
    for (uint64_t start = time_us(); rd(h->hc + RESET, 1) & 6;)
        if (time_us() - start >= COMMAND_US) {
            stop(h);
            return;
        }
}

static bool idle(struct sdhci *h, uint32_t lines) {
    for (uint64_t start = time_us(); rd(h->hc + PRESENT, 4) & lines;)
        if (time_us() - start >= COMMAND_US) return stop(h);
    return true;
}

bool sdhci_command(struct sdhci *h, unsigned index, uint32_t argument, unsigned type,
                   uint32_t errors, uint32_t *response) {
    if (h->failed || !idle(h, (type & 3) == 3 ? 3 : 1)) return false;
    clear(h, 0xffffffffu);
    wr(h->hc + ARGUMENT, argument, 4);
    wr(h->hc + TRANSFER_MODE, 0, 2);
    wr(h->hc + COMMAND, index << 8 | type, 2);
    if (!wait_status(h, COMMAND_DONE, COMMAND_US)) {
        recover(h);
        return false;
    }
    uint32_t r = rd(h->hc + RESPONSE, 4);
    if (response) *response = r;
    clear(h, COMMAND_DONE);
    if ((type & 3) == 3) {
        if (!wait_status(h, TRANSFER_DONE, busy_limit(h))) {
            recover(h);
            return false;
        }
        clear(h, TRANSFER_DONE);
    }
    return !(r & errors);
}

bool sdhci_data(struct sdhci *h, unsigned index, uint32_t argument, uint32_t errors, uint8_t *data,
                unsigned size, unsigned count, bool write) {
    if (h->failed || !size || size > 512 || !count || count > 511 || !idle(h, 3)) return false;
    clear(h, 0xffffffffu);
    wr(h->hc + BLOCK_SIZE, size, 2);
    wr(h->hc + BLOCK_COUNT, count, 2);
    wr(h->hc + ARGUMENT, argument, 4);
    wr(h->hc + TRANSFER_MODE, 2 | (write ? 0 : 16) | (count > 1 ? 32 : 0), 2);
    wr(h->hc + COMMAND, index << 8 | 0x3a, 2);
    if (!wait_status(h, COMMAND_DONE, COMMAND_US)) goto failed;
    uint32_t response = rd(h->hc + RESPONSE, 4);
    clear(h, COMMAND_DONE);
    if (response & errors) goto failed;
    for (unsigned block = 0; block < count; ++block) {
        unsigned ready_bit = write ? WRITE_READY : READ_READY;
        if (!wait_status(h, ready_bit, DATA_US)) goto failed;
        clear(h, ready_bit);
        uint8_t *p = data + block * size;
        if (!((uintptr_t)p & 3) && !(size & 3)) {
            /* Whole words for an aligned buffer. */
            uint32_t *words = (uint32_t *)(void *)p;
            for (unsigned i = 0; i < size / 4; ++i)
                if (write) mmio_write32(h->hc + BUFFER, words[i]);
                else words[i] = mmio_read32(h->hc + BUFFER);
            continue;
        }
        for (unsigned at = 0; at < size; at += 4) {
            unsigned n = size - at < 4 ? size - at : 4;
            if (write) {
                uint32_t word = 0;
                for (unsigned i = 0; i < n; ++i) word |= (uint32_t)p[at + i] << 8 * i;
                mmio_write32(h->hc + BUFFER, word);
            } else {
                uint32_t word = mmio_read32(h->hc + BUFFER);
                for (unsigned i = 0; i < n; ++i) p[at + i] = (uint8_t)(word >> 8 * i);
            }
        }
    }
    /* A write ends once the card has programmed it, its busy released. */
    if (!wait_status(h, TRANSFER_DONE, write ? busy_limit(h) : DATA_US)) goto failed;
    clear(h, TRANSFER_DONE);
    return true;
failed:
    recover(h);
    return false;
}

/* The controller from cold: GCC's clocks (GPLL0 voted, the AHB and card
 * branches, 400 kHz), the pads, HC mode, a full reset, the pads at 1.8 V,
 * the wrapper's requests visible, the delay line off (unused to 100 MHz),
 * the status bits latched but signalling nothing, bus power (written once,
 * as sdhci-msm's SDHCI_QUIRK_SINGLE_POWER_WRITE has it) and 1.8 V
 * signalling, each confirmed where the wrapper asks, then the clock
 * running. */
bool sdhci_start(struct sdhci *h) {
    h->failed = false;
    h->slow = false;
    wr(GPLL0_VOTE, rd(GPLL0_VOTE, 4) | 1, 4);
    wr(h->ahb_cbcr, rd(h->ahb_cbcr, 4) | 1, 4);
    if (!clock_rate(h, false)) return false;
    wr(h->apps_cbcr, rd(h->apps_cbcr, 4) | 1, 4);
    if (!wait_branch(h->ahb_cbcr) || !wait_branch(h->apps_cbcr)) return stop(h);
    wr(h->pads, (rd(h->pads, 4) & ~h->pads_mask) | h->pads_value, 4);
    wr(h->hc + VENDOR_SPEC, VENDOR_SPEC_RESET, 4);
    wr(h->core + HC_MODE, HC_MODE_ENABLE, 4);
    wr(h->core + HC_MODE, rd(h->core + HC_MODE, 4) | FF_CLOCK_RESET_OFF, 4);
    wr(h->hc + RESET, 1, 1);
    for (uint64_t start = time_us(); rd(h->hc + RESET, 1) & 1;)
        if (time_us() - start >= COMMAND_US) return stop(h);
    wr(h->hc + VENDOR_SPEC, VENDOR_SPEC_RESET | PAD_SWITCH_ENABLE | PAD_1V8, 4);
    (void)confirm_power(h, false); /* any request left from before */
    wr(h->core + POWER_MASK, POWER_REQUESTS, 4);
    wr(h->hc + DLL_CONFIG, rd(h->hc + DLL_CONFIG, 4) | 1u << 30, 4);
    wr(h->hc + DLL_CONFIG, rd(h->hc + DLL_CONFIG, 4) | 1u << 29, 4);
    wr(h->hc + SIGNAL_ENABLE, 0, 4);
    wr(h->hc + STATUS_ENABLE, 0x00ff0033u, 4);
    clear(h, 0xffffffffu);
    wr(h->hc + TIMEOUT, 0x0e, 1);
    uint32_t caps = rd(h->hc + CAPABILITIES, 4);
    unsigned volts = (caps & 0x01000000u)   ? 7
                     : (caps & 0x02000000u) ? 6
                     : (caps & 0x04000000u) ? 5
                                            : 0;
    if (!volts) return stop(h);
    wr(h->hc + POWER, volts << 1 | 1, 1);
    if (!confirm_power(h, true) || !(rd(h->hc + POWER, 1) & 1)) return stop(h);
    /* Only a controller whose signalling can switch asks to confirm it
     * (sdhci_msm_check_power_status): SDCC2 does, SDCC1's eMMC, fixed at
     * 1.8 V, does not. */
    wr(h->hc + HOST2, rd(h->hc + HOST2, 2) | 8, 2);
    if ((rd(h->core + MCI_GENERICS, 4) & SWITCHABLE_SIGNALLING) && !confirm_power(h, true))
        return stop(h);
    wr(h->hc + CLOCK, 1, 2);
    return sdhci_clock(h, false);
}
