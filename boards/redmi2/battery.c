#include "redmi2_battery.h"
#include "msm8916_i2c.h"

/* WT88047: SMB1360 at 0x14 on BLSP1 QUP4 (GPIO14/15, mux 2).
 * Only read the already-configured gauge's shadow SOC, its charging status
 * and its USB input; do not change charger limits, calibration, OTP,
 * shutdown or IRQ settings, nor reset the gauge.
 * Primary register/wiring references:
 * https://github.com/msm8916-mainline/linux/blob/wip/msm8916/7.0/drivers/power/supply/smb1360.c
 * https://github.com/msm8916-mainline/linux/blob/wip/msm8916/7.0/arch/arm64/boot/dts/qcom/msm8916-wingtech-wt88047.dts
 * https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/gcc-msm8916.c
 */
enum {
    SMB1360 = 0x14,
    STATUS_3 = 0x4b,
    REVISION = 0x4f,
    IRQ_E = 0x54, /* interrupt status; even bits live */
    MSYS_SOC = 0x61,
    VTG_NOW = 0x69,
    CHARGE_TYPE = 3 << 1, /* STATUS_3: none, pre, fast or taper */
    HOLD_OFF = 1 << 3,    /* STATUS_3 */
    USBIN_UV = 1 << 0,    /* IRQ_E: USB input under voltage, no charger */
};

static uint32_t read32(uintptr_t p) {
    uint32_t value = mmio_read32(p);
    barrier();
    return value;
}

static void write32(uintptr_t p, uint32_t value) {
    barrier();
    mmio_write32(p, value);
    barrier();
}
static struct msm8916_i2c bus = {.base = 0x078b8000};

static bool wait_bits(uintptr_t address, uint32_t mask, uint32_t value) {
    uint64_t start = time_us();
    for (unsigned i = 0; i < 100000; ++i) {
        if ((read32(address) & mask) == value) return true;
        if (time_us() - start >= 1000) break;
    }
    return false;
}

static bool prepare(void) {
    /* Add the shared AHB vote, preserving every other consumer's bits. */
    write32(0x01845004, read32(0x01845004) | (1u << 10));
    if (!(read32(0x01845004) & (1u << 10)) || !wait_bits(0x01801008, 1u << 31, 0)) return false;
    const unsigned source_mask = 0x1f | (7 << 8) | (3 << 12) | (1 << 20);
    write32(0x01805004, (read32(0x01805004) & ~source_mask) | 1); /* XO /1, 19.2 MHz */
    write32(0x01805000, read32(0x01805000) | 1);
    if (!wait_bits(0x01805000, 1, 0) || (read32(0x01805004) & source_mask) != 1) return false;
    write32(0x01805020, read32(0x01805020) | 1);
    if (!(read32(0x01805020) & 1) || !wait_bits(0x01805020, 1u << 31, 0)) return false;
    for (unsigned pin = 14; pin <= 15; ++pin) {
        uintptr_t address = 0x01000000 + pin * 0x1000;
        write32(address, (read32(address) & ~0x3ffu) | (2u << 2));
        if ((read32(address) & 0x3ffu) != (2u << 2)) return false;
    }
    return msm8916_i2c_bus_init(&bus);
}

static bool read8(uint8_t reg, uint8_t *value) {
    return msm8916_i2c_bus_read(&bus, SMB1360, reg, value, 1);
}

bool redmi2_battery_read(struct battery_view *out) {
    if (!out || (!bus.ready && !prepare())) return false;
    uint8_t revision, status, input, soc, voltage[2];
    if (!read8(REVISION, &revision) || revision == 0xff || !read8(MSYS_SOC, &soc) ||
        !msm8916_i2c_bus_read(&bus, SMB1360, VTG_NOW, voltage, 2) || !read8(STATUS_3, &status) ||
        !read8(IRQ_E, &input))
        return false;
    /* A missing/uninitialized gauge can return a flat 0/0xff register bank.
     * Validate a live shadow voltage before publishing its SOC. */
    unsigned raw = voltage[0] | (unsigned)voltage[1] << 8;
    unsigned millivolts = raw * 5000u / 32767u;
    if (millivolts < 2000 || millivolts > 5000) return false;
    /* A charger is its live USB input, as the drivers report online. */
    bool charger = !(input & USBIN_UV);
    bool charging = charger && (status & CHARGE_TYPE) && !(status & HOLD_OFF);
    *out = (struct battery_view){.available = true,
                                 .charger = charger,
                                 .charging = charging,
                                 .percent = (soc * 100u + 127) / 255};
    return true;
}
