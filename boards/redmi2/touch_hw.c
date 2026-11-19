#include "redmi2_touch_hw.h"
#include <stdint.h>

/* Independently implemented from these primary hardware descriptions:
 * https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/msm8916-wingtech-wt88047.dts
 * https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/msm8916-pm8916.dtsi
 * https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/msm8916.dtsi
 * https://github.com/torvalds/linux/blob/master/drivers/pinctrl/qcom/pinctrl-msm8916.c
 * https://github.com/torvalds/linux/blob/master/drivers/spmi/spmi-pmic-arb.c
 * https://github.com/torvalds/linux/blob/master/drivers/regulator/qcom_spmi-regulator.c
 * https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/gcc-msm8916.c
 * https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/clk-rcg2.c
 * https://github.com/torvalds/linux/blob/master/drivers/input/touchscreen/edt-ft5x06.c
 *
 * Exact WT88047 board validation is the caller's responsibility. The boot
 * firmware must leave the upstream PMIC supplies, XO and PCNOC clock running,
 * and permit application-processor access to these SPMI channels and GCC/TLMM.
 * This does not implement RPM resource voting or alter channel ownership.
 */
#include "platform.h"

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

#define now_us time_us

enum {
    TLMM = 0x01000000,
    GCC = 0x01800000,
    ARB_CORE = 0x0200f000,
    ARB_WRITE = 0x02400000,
    ARB_READ = 0x02c00000,
    /* 4 MiB channel windows / 0x8000 bytes per peripheral. */
    ARB_APIDS = 128,
    ARB_CMD = 0,
    ARB_CONFIG = 4,
    ARB_STATUS = 8,
    ARB_WDATA = 0x10,
    ARB_RDATA = 0x18,
    GPIO_CFG_MASK = 0x3ff, /* pull, function, drive strength, output enable */
    GPIO_OUTPUT = 1 << 9,
    RCG_MASK = 0x1f | (7 << 8) | (3 << 12) | (1 << 20),
    TRANSFER_TIMEOUT_US = 1000,
    POLL_LIMIT = 100000,
};

struct rail {
    unsigned apid;
    uint8_t selector, enable, mode;
};

static bool active;

/* Iteration guards also terminate if the architectural counter stops. */
static bool wait_bits(uintptr_t address, uint32_t mask, uint32_t expected) {
    uint64_t start = now_us();
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        if ((read32(address) & mask) == expected) return true;
        if (now_us() - start >= TRANSFER_TIMEOUT_US) break;
    }
    return false;
}

static bool bounded_delay_us(uint32_t duration) {
    uint64_t start = now_us();
    for (unsigned i = 0; i < 20000000; ++i)
        if (now_us() - start >= duration) return true;
    return false; /* timer did not advance during reset/power delay */
}

static bool find_apid(uint16_t ppid, unsigned *apid) {
    for (unsigned i = 0; i < ARB_APIDS; ++i) {
        uint32_t map = read32(ARB_CORE + 0x800 + 4 * i);
        if (map && ((map >> 8) & 0xfff) == ppid) {
            *apid = i;
            return true;
        }
    }
    return false; /* PM8916 L6/L17 SPMI mapping unavailable */
}

static bool spmi_byte(unsigned apid, uint8_t reg, uint8_t *value, bool write) {
    /* v2, execution environment 0. Byte count is length minus one: zero. */
    uintptr_t channel = (write ? ARB_WRITE : ARB_READ) + 0x8000u * apid;
    write32(channel + ARB_CONFIG, 0); /* Polled channel, no completion IRQ. */
    if (write) write32(channel + ARB_WDATA, *value);
    write32(channel + ARB_CMD, (write ? 0u : 1u << 27) | ((uint32_t)reg << 4));
    if (!wait_bits(channel + ARB_STATUS, 1, 1)) return false; /* SPMI transaction timed out */
    uint32_t status = read32(channel + ARB_STATUS);
    if (status & 4) return false;    /* SPMI access denied by firmware ownership/permissions */
    if (status & 0xau) return false; /* SPMI transaction failed or was dropped */
    if (!write) *value = (uint8_t)read32(channel + ARB_RDATA);
    return true;
}

static bool rail_read(struct rail *rail, uint8_t reg, uint8_t *value) {
    return spmi_byte(rail->apid, reg, value, false);
}

static bool rail_write(struct rail *rail, uint8_t reg, uint8_t value) {
    uint8_t actual;
    return spmi_byte(rail->apid, reg, &value, true) && rail_read(rail, reg, &actual) &&
           actual == value;
}

static bool rail_inspect(struct rail *rail, uint16_t ppid) {
    uint8_t type, subtype, voltage;
    if (!find_apid(ppid, &rail->apid) || !rail_read(rail, 4, &type) ||
        !rail_read(rail, 5, &subtype) || !rail_read(rail, 0x41, &voltage) ||
        !rail_read(rail, 0x45, &rail->mode) || !rail_read(rail, 0x46, &rail->enable))
        return false;
    /* Only the ULT PLDO family uses the 1.75V + selector * 12.5mV table.
     * Reject NLDO and other PMIC types instead of assuming their encoding. */
    bool pldo = (subtype >= 0x08 && subtype <= 0x0b) || (subtype >= 0x28 && subtype <= 0x2a) ||
                subtype == 0x2d;
    if (type != 0x21 || !pldo) return false; /* unsupported PM8916 regulator type/subtype */
    /* Pin-controlled rails may already be live even when software-enable is
     * clear. Never retune a shared active display/IO supply or bypass mode. */
    return !(rail->enable & 0x8f) || (voltage == rail->selector && !(rail->mode & 0x20));
}

static bool rail_enable(struct rail *rail) {
    if (rail->enable & 0x80)
        return true; /* Keep live supply voltage and mode exactly as inherited. */
    if (!(rail->enable & 0x0f)) {
        /* This rail is disabled, so set voltage and normal mode before enable.
         * HPM/AUTO/BYPASS bits are owned; other mode bits remain unchanged. */
        if (!rail_write(rail, 0x41, rail->selector) ||
            !rail_write(rail, 0x45, (rail->mode & ~0xe0u) | 0x80))
            return false;
    }
    return rail_write(rail, 0x46, rail->enable | 0x80);
}

static bool gpio_config(unsigned pin, uint32_t value) {
    uintptr_t cfg = TLMM + 0x1000u * pin;
    write32(cfg, (read32(cfg) & ~GPIO_CFG_MASK) | value);
    return (read32(cfg) & GPIO_CFG_MASK) == value;
}

static bool reset_level(bool high) {
    uintptr_t io = TLMM + 12 * 0x1000 + 4;
    uint32_t desired = high ? 2 : 0;
    write32(io, (read32(io) & ~2u) | desired);
    return (read32(io) & 2) == desired;
}

static bool qup_clocks(void) {
    /* The AHB vote register is shared with other peripherals. Only add BLSP1's
     * bit; never clear another clock vote or retune its PCNOC parent. */
    uintptr_t vote = GCC + 0x45004;
    write32(vote, read32(vote) | (1u << 10));
    if (!(read32(vote) & (1u << 10)) || !wait_bits(GCC + 0x1008, 1u << 31, 0))
        return false; /* BLSP1 AHB clock did not start */

    uintptr_t cfg = GCC + 0x6004, cmd = GCC + 0x6000;
    /* XO parent selector 0; HID divider encodes 2*divisor-1, hence 1. */
    write32(cfg, (read32(cfg) & ~RCG_MASK) | 1);
    write32(cmd, read32(cmd) | 1);
    if (!wait_bits(cmd, 1, 0) || (read32(cfg) & RCG_MASK) != 1)
        return false; /* QUP5 19.2MHz clock source update failed */
    uintptr_t branch = GCC + 0x6020;
    write32(branch, read32(branch) | 1);
    if (!(read32(branch) & 1) || !wait_bits(branch, 1u << 31, 0))
        return false; /* QUP5 I2C application clock did not start */
    return true;
}

bool redmi2_touch_hw_init(void) {
    active = false;
    uint32_t version = read32(ARB_CORE);
    if (version < 0x20010000u || version >= 0x30000000u)
        return false;                   /* unsupported SPMI arbiter version (requires MSM8916 v2) */
    struct rail io = {.selector = 4};   /* L6, 1.8 V */
    struct rail vcc = {.selector = 88}; /* L17, 2.85 V */
    /* Check both shared supplies before changing either one. SID 1 contains
     * PM8916 regulators; peripheral IDs 0x45 and 0x50 are L6 and L17. */
    if (!rail_inspect(&io, 0x145) || !rail_inspect(&vcc, 0x150)) return false;

    /* Assert reset before enabling the output driver or altering the supplies. */
    if (!reset_level(false) || !gpio_config(12, GPIO_OUTPUT) ||
        !gpio_config(13, 3)) /* GPIO input, 2mA, pull-up */
        return false;
    /* Both I2C pins use alternate function 2, 2mA and no internal pulls. */
    if (!gpio_config(18, 2u << 2) || !gpio_config(19, 2u << 2)) return false;

    /* EDT/Focaltech sequence: IOVCC first, >10us, then VCC, reset held at least
     * 5ms, release, 300ms startup. No supply is disabled on a partial failure:
     * L6/L17 are also used by the inherited display. Reassert reset on failure. */
    if (!rail_enable(&io) || !bounded_delay_us(20) || !rail_enable(&vcc) || !qup_clocks() ||
        !bounded_delay_us(5000) || !reset_level(true) || !bounded_delay_us(300000)) {
        (void)reset_level(false);
        return false;
    }
    active = true;
    return true;
}

bool redmi2_touch_hw_reset(bool asserted) { return active && reset_level(!asserted); }

bool redmi2_touch_hw_interrupt_pull(bool up) {
    return active && gpio_config(13, up ? 3 : 1); /* GPIO input, 2mA, pull-up or pull-down */
}

bool redmi2_touch_irq_pending(void) { return active && !(read32(TLMM + 13 * 0x1000 + 4) & 1); }
