#include "nexus5.h"
#include "platform.h"
#include "wire.h"

/* The PM8941 PMIC, read through MSM8974's SPMI PMIC arbiter (v1: the stock
 * kernel's drivers/spmi/spmi-pmic-arb.c and Linux's spmi-pmic-arb.c): this
 * execution environment's channel 0 takes one command at a time, an extended
 * register read of up to 4 bytes from a slave's 16-bit address, then reports
 * done, failed, denied or dropped in its status. An address here carries
 * its slave in bits 19:16 (the PM8941's are 0 and 1, the PM8841's 4 and 5). Reads: the power key's
 * state in the power-on block (PON_RT_STS bit 0, KPDPWR_N_SET; Linux
 * pm8941-pwrkey.c) and the real-time clock's seconds counter (RTC_RDATA,
 * little-endian; Linux rtc-pm8xxx.c, pm8941_regs), both on slave 0, and the
 * GPIOs the Wi-Fi chip needs (wifi.c). Writes: those GPIOs, and for a
 * restart, PS_HOLD's reset type, as CAF LK's pm8x41_reset_configure sets it
 * (MSM8974 v2.2 takes CTL2's enable bit), before the restart reason goes
 * into IMEM and PS_HOLD drops, as the stock kernel's restart.c and NetBSD's
 * Nexus 5 port do; the bootloader reads 0x77665500 as fastboot. */
enum {
    CHANNEL = 0xfc4cf800,
    CMD = 0x00,
    STATUS = 0x08,
    WDATA0 = 0x10,
    RDATA0 = 0x18,
    OP_EXT_WRITEL = 0,
    OP_EXT_READL = 1,
    DONE = 1,
    FAILED = 2 | 4 | 8, /* failed, denied, dropped */
    TIMEOUT_US = 1000,
    PON_RT_STS = 0x0810,
    KPDPWR_N_SET = 1,
    RTC_RDATA = 0x6048,
    RTC_EN_CTL = 0x6046,
    RTC_ENABLE = 0x80,
    KEY_POLL_US = 10000,
    PON_PS_HOLD_RESET_CTL = 0x085a,
    PON_PS_HOLD_RESET_CTL2 = 0x085b,
    PS_HOLD_RESET_ENABLE = 0x80,
    PS_HOLD_WARM_RESET = 1,
    IMEM_RESTART_REASON = 0xfe80565c,
    RESTART_FASTBOOT = 0x77665500,
    RESTART_NORMAL = 0x77665501,
    MPM_PS_HOLD = 0xfc4ab000
};

/* The command's slave (23:20) and register address (19:4). */
static uint32_t target(uint32_t address) {
    return (address >> 16 & 15) << 20 | (address & 0xffff) << 4;
}

static bool read_bytes(uint32_t address, uint8_t *out, unsigned count) {
    if (!count || count > 4) return false;
    mmio_write32(CHANNEL + CMD, (uint32_t)OP_EXT_READL << 27 | target(address) | (count - 1));
    barrier();
    uint32_t status = 0;
    for (uint64_t start = time_us(); !(status = mmio_read32(CHANNEL + STATUS));)
        if (time_us() - start >= TIMEOUT_US) return false;
    if (!(status & DONE) || (status & FAILED)) return false;
    uint32_t data = mmio_read32(CHANNEL + RDATA0);
    for (unsigned i = 0; i < count; ++i) out[i] = (uint8_t)(data >> 8 * i);
    return true;
}

static bool write_byte(uint32_t address, uint8_t value) {
    mmio_write32(CHANNEL + WDATA0, value);
    mmio_write32(CHANNEL + CMD, (uint32_t)OP_EXT_WRITEL << 27 | target(address));
    barrier();
    uint32_t status = 0;
    for (uint64_t start = time_us(); !(status = mmio_read32(CHANNEL + STATUS));)
        if (time_us() - start >= TIMEOUT_US) return false;
    return (status & DONE) && !(status & FAILED);
}

bool nexus5_pmic_read(uint32_t address, uint8_t *value) { return read_bytes(address, value, 1); }

bool nexus5_pmic_write(uint32_t address, uint8_t value) {
    uint8_t back;
    return write_byte(address, value) && read_bytes(address, &back, 1) && back == value;
}

_Noreturn void nexus5_restart(bool bootloader) {
    (void)write_byte(PON_PS_HOLD_RESET_CTL2, 0);
    delay_us(500); /* for the disable to take effect */
    (void)write_byte(PON_PS_HOLD_RESET_CTL, PS_HOLD_WARM_RESET);
    (void)write_byte(PON_PS_HOLD_RESET_CTL2, PS_HOLD_RESET_ENABLE);
    mmio_write32(IMEM_RESTART_REASON, bootloader ? RESTART_FASTBOOT : RESTART_NORMAL);
    barrier();
    mmio_write32(MPM_PS_HOLD, 0);
    for (;;) __asm__ volatile("wfi");
}

static bool key_down;
static uint64_t key_polled;

bool nexus5_power_key_pressed(void) {
    uint64_t now = loop_time_us();
    if (now - key_polled < KEY_POLL_US) return false;
    key_polled = now;
    uint8_t status;
    if (!read_bytes(PON_RT_STS, &status, 1)) return false;
    bool down = status & KPDPWR_N_SET, pressed = down && !key_down;
    key_down = down;
    return pressed;
}

bool nexus5_rtc_read(uint32_t *seconds) {
    uint8_t control, first[4], low;
    if (!seconds || !read_bytes(RTC_EN_CTL, &control, 1) || !(control & RTC_ENABLE) ||
        !read_bytes(RTC_RDATA, first, 4) || !read_bytes(RTC_RDATA, &low, 1))
        return false;
    /* The low byte wrapped between the reads: the whole count again. */
    if (low < first[0] && !read_bytes(RTC_RDATA, first, 4)) return false;
    *seconds = le32(first);
    return true;
}
