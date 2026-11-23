#include "redmi2_battery.h"
#include "qup_i2c.h"
#include "redmi2_power_hw.h"

/* WT88047: SMB1360 at 0x14 on BLSP1 QUP4 (GPIO14/15, mux 2), and the
 * PM8916's voltage ADC (VADC).
 * The level is estimated from the battery's voltage, as on the tablet: the
 * SMB1360 gauge's own level (MSYS_SOC) stayed at 100% for hours on the
 * handset, its readings refreshing rarely at this phone's currents, and it
 * serves only when the VADC cannot be read. The voltage is VPH_PWR, the
 * supply the battery feeds (the PM8916's own charger and BMS are off in the
 * stock DT: use-external-charger, disable-bms), converted as Linux's
 * qcom-spmi-vadc.c converts it.
 * The SMB1360 is only read: its shadow voltage, validating a live gauge, its
 * charging status and its USB input; do not change charger limits,
 * calibration, OTP, shutdown or IRQ settings, nor reset the gauge: a reset
 * re-estimated its charge from the voltage of the moment, which a load
 * lowers, and its level then jumped between half and near empty.
 * Primary register/wiring references:
 * https://github.com/msm8916-mainline/linux/blob/wip/msm8916/7.0/drivers/power/supply/smb1360.c
 * https://github.com/msm8916-mainline/linux/blob/wip/msm8916/7.0/arch/arm64/boot/dts/qcom/msm8916-wingtech-wt88047.dts
 * https://github.com/torvalds/linux/blob/master/drivers/iio/adc/qcom-spmi-vadc.c
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

/* The VADC (SID 0 at 0x3100): one polled conversion at a time, in normal
 * mode with both trims, decimation 512, no settling delay or averaging. */
enum {
    VADC = 0x031,
    VADC_STATUS1 = 0x08,
    VADC_MODE_CTL = 0x40,
    VADC_EN_CTL1 = 0x46,
    VADC_CH_SEL_CTL = 0x48,
    VADC_DIG_PARAM = 0x50,
    VADC_HW_SETTLE_DELAY = 0x51,
    VADC_CONV_REQ = 0x52,
    VADC_FAST_AVG_CTL = 0x5a,
    VADC_FAST_AVG_EN = 0x5b,
    VADC_DATA = 0x60,  /* 16 bits, low byte first */
    VADC_TRIMS = 3,    /* MODE_CTL: normal mode, ADC and AMUX trims */
    VADC_SET = 1 << 7, /* EN_CTL1 and CONV_REQ */
    VADC_EOC = 1,      /* STATUS1 & 3: converted, no request pending */
    VADC_VSYS = 0x07,  /* VPH_PWR, through a 1:3 divider */
    VADC_REF_625MV = 0x09,
    VADC_REF_1250MV = 0x0a,
    VADC_MIN_CODE = 0x6000, /* Linux's valid result range */
    VADC_MAX_CODE = 0xa800,
    VADC_TIMEOUT_US = 20000,
};

/* The battery's open-circuit voltage at each 5% of charge, from 100% down,
 * in mV: Qualcomm's QRD SKUI profile of a 4.35 V, 2200 mAh cell like the
 * BM44 (batterydata-qrd-skui-4v35-2200mah.dtsi, 25 C), with 0% at its 3.4 V
 * cutoff, where the profile's own row is a 3.0 V floor. */
static const uint16_t ocv[21] = {4322, 4250, 4192, 4138, 4086, 4039, 3991, 3953, 3912, 3865, 3832,
                                 3810, 3793, 3778, 3768, 3756, 3733, 3700, 3682, 3602, 3400};

/* Off a charger the battery feeds the screen and CPUs, which lower it below
 * the open-circuit voltage: about 45 mV is added back to the least loaded
 * reading below, for an awake phone's lightest 0.2 A through the profile's
 * 0.15 ohm cell, its 0.05 ohm capacitive part and the path. On a charger the
 * voltage reads high instead, and battery.c lets the level only rise. */
enum { LOAD_MV = 45 };

/* A reading takes the load of its moment, and battery.c takes any small
 * fall off a charger at once: from single readings the level would follow
 * the heaviest load down and stay there. So the level comes from the
 * highest of the readings of the last 2.5 minutes off a charger, where load
 * only lowers the voltage, and the lowest on one, where charging only
 * raises it. A change of charger starts afresh, and so does a wake, as the
 * readings before standby are too old. */
enum { RECENT = 6, RECENT_US = 150000000 };

static struct {
    uint64_t at;
    unsigned millivolts;
} recent[RECENT];

static unsigned recent_count;
static bool recent_charger;

static unsigned settled_millivolts(unsigned millivolts, bool charger) {
    uint64_t now = time_us();
    unsigned kept = 0;
    for (unsigned i = 0; i < recent_count; ++i)
        if (charger == recent_charger && now - recent[i].at < RECENT_US) recent[kept++] = recent[i];
    if (kept == RECENT) {
        for (unsigned i = 1; i < RECENT; ++i) recent[i - 1] = recent[i];
        --kept;
    }
    recent[kept].at = now;
    recent[kept++].millivolts = millivolts;
    recent_count = kept;
    recent_charger = charger;
    unsigned settled = millivolts;
    for (unsigned i = 0; i < kept; ++i)
        if (charger ? recent[i].millivolts < settled : recent[i].millivolts > settled)
            settled = recent[i].millivolts;
    return settled;
}

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
static struct qup_i2c bus = {.base = 0x078b8000};

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
    return qup_i2c_init(&bus);
}

static bool read8(uint8_t reg, uint8_t *value) {
    return qup_i2c_read(&bus, SMB1360, reg, value, 1);
}

static bool vadc_write(uint8_t reg, uint8_t value) {
    return redmi2_power_hw_write(VADC, reg, value);
}

static bool vadc_read(uint8_t reg, uint8_t *value) {
    return redmi2_power_hw_read(VADC, reg, value);
}

/* One channel's raw result; the ADC is off again afterwards. */
static bool convert(uint8_t channel, unsigned *code) {
    uint8_t status = 0, low, high;
    bool done = vadc_write(VADC_MODE_CTL, VADC_TRIMS) && vadc_write(VADC_CH_SEL_CTL, channel) &&
                vadc_write(VADC_DIG_PARAM, 0) && vadc_write(VADC_HW_SETTLE_DELAY, 0) &&
                vadc_write(VADC_FAST_AVG_CTL, 0) && vadc_write(VADC_FAST_AVG_EN, 0) &&
                vadc_write(VADC_EN_CTL1, VADC_SET) && vadc_write(VADC_CONV_REQ, VADC_SET);
    for (uint64_t start = time_us(); done && (status & 3) != VADC_EOC;)
        done = vadc_read(VADC_STATUS1, &status) && time_us() - start < VADC_TIMEOUT_US;
    done = done && vadc_read(VADC_DATA, &low) && vadc_read(VADC_DATA + 1, &high);
    (void)vadc_write(VADC_EN_CTL1, 0);
    if (!done) return false;
    *code = low | (unsigned)high << 8;
    return *code >= VADC_MIN_CODE && *code <= VADC_MAX_CODE;
}

/* VPH_PWR in mV, calibrated against the ADC's 0.625 V and 1.25 V references
 * as Linux calibrates an absolute channel, then times its 1:3 divider. */
static bool vph_millivolts(unsigned *millivolts) {
    unsigned low, high, code;
    if (!convert(VADC_REF_625MV, &low) || !convert(VADC_REF_1250MV, &high) || high <= low ||
        !convert(VADC_VSYS, &code))
        return false;
    int64_t microvolts = 625000 + ((int64_t)code - low) * 625000 / (int64_t)(high - low);
    unsigned value = microvolts > 0 ? (unsigned)((microvolts * 3 + 500) / 1000) : 0;
    if (value < 2500 || value > 4700) return false;
    *millivolts = value;
    return true;
}

bool redmi2_battery_read(struct battery_view *out) {
    if (!out || (!bus.ready && !prepare())) return false;
    uint8_t revision, status, input, soc, voltage[2];
    if (!read8(REVISION, &revision) || revision == 0xff || !read8(MSYS_SOC, &soc) ||
        !qup_i2c_read(&bus, SMB1360, VTG_NOW, voltage, 2) || !read8(STATUS_3, &status) ||
        !read8(IRQ_E, &input))
        return false;
    /* A missing/uninitialized gauge can return a flat 0/0xff register bank.
     * Validate a live shadow voltage before publishing its state. */
    unsigned raw = voltage[0] | (unsigned)voltage[1] << 8;
    unsigned millivolts = raw * 5000u / 32767u;
    if (millivolts < 2000 || millivolts > 5000) return false;
    /* A charger is its live USB input, as the drivers report online. */
    bool charger = !(input & USBIN_UV);
    bool charging = charger && (status & CHARGE_TYPE) && !(status & HOLD_OFF);
    unsigned percent = (soc * 100u + 127) / 255;
    if (vph_millivolts(&millivolts)) {
        millivolts = settled_millivolts(millivolts, charger);
        percent = battery_ocv_percent(ocv, charger ? millivolts : millivolts + LOAD_MV);
    }
    *out = (struct battery_view){
        .available = true, .charger = charger, .charging = charging, .percent = percent};
    return true;
}
