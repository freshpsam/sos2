#include "gta7lite.h"
#include "platform.h"

/* The MT6357 PMIC behind MT6765's PMIC wrapper, which LK leaves running:
 * single 16-bit reads and writes through the AP's WACS2 channel (Linux's
 * drivers/soc/mediatek/mtk-pmic-wrap.c, pwrap_mt6765 with pmic_mt6357). The
 * MT6357's registers are Linux's (include/linux/mfd/mt6357/registers.h,
 * mt6397-core.c, rtc-mt6397.c, mtk-pmic-keys.c, mt6359-auxadc.c) and
 * MediaTek's own mt6357 drivers'. The only writes are the battery voltage's
 * measurement requests and the supplies' enables. The battery's charger is
 * the MT6370 (mt6370.c), left as LK configured it: its software supervision
 * (temperature, full charge, its watchdog) is not done here, so the kernel
 * only reports whether a charger is connected and the MT6370 charging. */
enum {
    PWRAP = 0x1000d000,
    WACS2_EN = 0x09c,
    INIT_DONE2 = 0x0a0,
    WACS2_CMD = 0xc20,
    WACS2_RDATA = 0xc24, /* data [15:0], state machine [18:16] */
    WACS2_VLDCLR = 0xc28,
    WACS_IDLE = 0,
    WACS_WAIT_FOR_CLEAR = 6,
    WACS_INIT_DONE = 1 << 21,
    WACS_US = 10000,
    /* MT6357 */
    SWCID = 0x00a, /* MT6357_CHIP_ID in [15:8] */
    MT6357_CHIP_ID = 0x57,
    TOPSTATUS = 0x024,
    PWRKEY_DEB = 1 << 1, /* clear while the power key is held */
    RTC_TC_SEC = 0x592,  /* then minute, hour, day, weekday, month and year */
    AUXADC_ADC0 = 0x1088,
    ADC_READY = 1 << 15,
    AUXADC_RQST0 = 0x110e,
    BATADC_REQUEST = 1 << 0,
    CHR_TOP_CON0 = 0xa88,
    RGS_CHRDET = 1 << 4, /* a charger is connected */
    /* VUSB33: enable in CON0_0, the output's state in CON1 (as for the
     * VCN18 and VCN33 LDOs Wi-Fi uses). Its ramp is 264 us (mt6765.dts). */
    LDO_VUSB33_CON0_0 = 0x199c,
    LDO_VUSB33_CON1 = 0x19ac,
    LDO_EN = 1 << 0,
    LDO_DA_EN = 1 << 15,
    LDO_RAMP_US = 5000,
    /* The connectivity subsystem's supplies (consys_hw_vcn18_ctrl and
     * consys_hw_vcn28_ctrl in MediaTek's mt6765 WMT driver): VCN18, VCN33's
     * Bluetooth output at 3.3 V (VOSEL 1), and, as the subsystem has no
     * co-clock (WMT_SOC.cfg), VCN28 for its crystal under its own control. */
    LDO_VCN18_CON0 = 0x1a58,
    LDO_VCN18_CON1 = 0x1a66,
    LDO_VCN33_CON0_0 = 0x1b1c,
    LDO_VCN33_CON1 = 0x1b2c,
    VCN33_ANA_CON0 = 0x1c1c,
    VCN33_VOSEL = 3 << 8,
    VCN33_3V3 = 1 << 8,
    LDO_VCN28_CON0 = 0x1b88,
    LDO_VCN28_OP_EN_SET = 0x1b8c,
    LDO_VCN28_OP_CFG_CLR = 0x1b94,
    LDO_VCN28_CON1 = 0x1b96,
    VCN28_HW_MODE = 1 << 1,
    /* The power key sampled every 20 ms and taken once steady for 40 ms. */
    KEY_SAMPLE_US = 20000,
    KEY_STEADY_US = 40000
};

static bool attempted, ready;

static unsigned wacs_state(uint32_t rdata) { return rdata >> 16 & 7; }

/* Until the WACS2 state machine reaches `state`: microseconds at most. */
static bool wacs_wait(unsigned state, uint32_t *rdata) {
    uint64_t start = time_us();
    for (;;) {
        uint32_t value = mmio_read32(PWRAP + WACS2_RDATA);
        if (wacs_state(value) == state) {
            if (rdata) *rdata = value;
            return true;
        }
        if (time_us() - start >= WACS_US) return false;
    }
}

static bool pmic_read(uint16_t address, uint16_t *value) {
    if (!ready) return false;
    if (!wacs_wait(WACS_IDLE, 0)) {
        /* A read whose result nobody took: release the channel, as
         * pwrap_read16 does, and give up this one. */
        if (wacs_state(mmio_read32(PWRAP + WACS2_RDATA)) == WACS_WAIT_FOR_CLEAR)
            mmio_write32(PWRAP + WACS2_VLDCLR, 1);
        return false;
    }
    mmio_write32(PWRAP + WACS2_CMD, (uint32_t)(address >> 1) << 16);
    uint32_t rdata;
    if (!wacs_wait(WACS_WAIT_FOR_CLEAR, &rdata)) return false;
    *value = (uint16_t)rdata;
    mmio_write32(PWRAP + WACS2_VLDCLR, 1);
    return true;
}

static bool pmic_write(uint16_t address, uint16_t value) {
    if (!ready || !wacs_wait(WACS_IDLE, 0)) return false;
    mmio_write32(PWRAP + WACS2_CMD, 1u << 31 | (uint32_t)(address >> 1) << 16 | value);
    return true;
}

static struct {
    uint64_t sampled, since;
    bool candidate, steady;
} key;

/* The wrapper as LK leaves it (its init done, WACS2 on) and an MT6357
 * behind it; then the power key's state, so that a key held through the
 * boot counts only once released and pressed again. */
bool gta7lite_pmic_init(void) {
    if (attempted) return ready;
    attempted = true;
    if (!mmio_read32(PWRAP + INIT_DONE2) || !mmio_read32(PWRAP + WACS2_EN) ||
        !(mmio_read32(PWRAP + WACS2_RDATA) & WACS_INIT_DONE))
        return false;
    ready = true;
    uint16_t id, status;
    if (!pmic_read(SWCID, &id) || (id >> 8) != MT6357_CHIP_ID || !pmic_read(TOPSTATUS, &status)) {
        ready = false;
        return false;
    }
    key.candidate = key.steady = !(status & PWRKEY_DEB);
    key.sampled = key.since = time_us();
    return true;
}

/* An LDO on, its output confirmed within the ramp. */
static bool ldo_on(uint16_t con0, uint16_t con1) {
    uint16_t value;
    if (!pmic_read(con0, &value)) return false;
    if (!(value & LDO_EN) && !pmic_write(con0, value | LDO_EN)) return false;
    for (uint64_t start = time_us(); time_us() - start < LDO_RAMP_US;)
        if (pmic_read(con1, &value) && (value & LDO_DA_EN)) return true;
    return false;
}

/* MediaTek's mt_usb_init enables the regulator before the PHY starts. */
bool gta7lite_usb_supply(void) {
    return gta7lite_pmic_init() && ldo_on(LDO_VUSB33_CON0_0, LDO_VUSB33_CON1);
}

bool gta7lite_connsys_supplies(void) {
    uint16_t value;
    return gta7lite_pmic_init() && ldo_on(LDO_VCN18_CON0, LDO_VCN18_CON1) &&
           pmic_read(VCN33_ANA_CON0, &value) &&
           pmic_write(VCN33_ANA_CON0, (uint16_t)((value & ~VCN33_VOSEL) | VCN33_3V3)) &&
           ldo_on(LDO_VCN33_CON0_0, LDO_VCN33_CON1);
}

bool gta7lite_connsys_crystal_supply(void) {
    return gta7lite_pmic_init() && pmic_write(LDO_VCN28_OP_EN_SET, VCN28_HW_MODE) &&
           pmic_write(LDO_VCN28_OP_CFG_CLR, VCN28_HW_MODE) && ldo_on(LDO_VCN28_CON0, LDO_VCN28_CON1);
}

bool gta7lite_power_key_pressed(void) {
    if (!gta7lite_pmic_init()) return false;
    uint64_t now = time_us();
    if (now - key.sampled < KEY_SAMPLE_US) return false;
    key.sampled = now;
    uint16_t status;
    if (!pmic_read(TOPSTATUS, &status)) return false;
    bool pressed = !(status & PWRKEY_DEB);
    if (pressed != key.candidate) {
        key.candidate = pressed;
        key.since = now;
        return false;
    }
    if (now - key.since < KEY_STEADY_US || pressed == key.steady) return false;
    key.steady = pressed;
    return pressed;
}

/* Days from 1970-01-01 to a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil). */
static int64_t days_from_civil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    unsigned of_era = (unsigned)(year - era * 400);
    unsigned of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    unsigned days = of_era * 365 + of_era / 4 - of_era / 100 + of_year;
    return era * 146097 + days - 719468;
}

/* The calendar registers, read again when the seconds rolled over meanwhile
 * (mtk_rtc_read_time); the year counts from 1968. */
bool gta7lite_rtc_read(uint32_t *seconds) {
    if (!seconds || !gta7lite_pmic_init()) return false;
    uint16_t field[7], again;
    for (unsigned attempt = 0;; ++attempt) {
        for (unsigned i = 0; i < 7; ++i)
            if (!pmic_read((uint16_t)(RTC_TC_SEC + 2 * i), &field[i])) return false;
        if (!pmic_read(RTC_TC_SEC, &again)) return false;
        if ((again & 63) >= (field[0] & 63)) break;
        if (attempt == 2) return false;
    }
    unsigned second = field[0] & 63, minute = field[1] & 63, hour = field[2] & 31,
             day = field[3] & 31, month = field[5] & 15, year = 1968 + (field[6] & 127);
    if (second > 59 || minute > 59 || hour > 23 || !day || !month || month > 12) return false;
    int64_t unix =
        days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + (int64_t)second;
    if (unix < 0 || unix > UINT32_MAX) return false;
    *seconds = (uint32_t)unix;
    return true;
}

/* The battery's open-circuit voltage at each 5% of charge, from 100% down,
 * in mV: the stock DT's battery0_profile_t1 (25 C) for its 5064 mAh. */
static const uint16_t ocv[21] = {4383, 4322, 4264, 4205, 4149, 4096, 4047, 3997, 3958, 3902, 3862,
                                 3837, 3816, 3800, 3786, 3767, 3748, 3724, 3695, 3685, 3394};

/* BATADC: a measurement averaged over 128 samples, 10 us apart, then the
 * result's ready bit; 15 bits of 1.8 V behind a 3:1 divider. */
static bool battery_millivolts(unsigned *millivolts) {
    if (!pmic_write(AUXADC_RQST0, BATADC_REQUEST)) return false;
    uint64_t start = time_us();
    while (time_us() - start < 1280) { }
    uint16_t value = 0;
    bool done = false;
    while (!done && time_us() - start < 32000) {
        if (!pmic_read(AUXADC_ADC0, &value)) return false;
        done = value & ADC_READY;
    }
    (void)pmic_write(AUXADC_RQST0, 0);
    if (!done) return false;
    *millivolts = (unsigned)(value & 0x7fff) * 3 * 1800 / 32768;
    return true;
}

/* The level from the voltage, through the OCV table. Off a charger the
 * battery feeds the screen and CPUs, which lower it below the open-circuit
 * voltage: about 60 mV is added back, for half an ampere through the cell's
 * resistance (the profile's 0.1 ohm and the path's). On a charger the
 * voltage reads high instead, and battery.c lets the level only rise. */
bool gta7lite_battery_read(struct battery_view *out) {
    if (!out || !gta7lite_pmic_init()) return false;
    uint16_t detect;
    unsigned millivolts;
    if (!pmic_read(CHR_TOP_CON0, &detect) || !battery_millivolts(&millivolts) ||
        millivolts < 2500 || millivolts > 4700)
        return false;
    bool charger = detect & RGS_CHRDET;
    unsigned open = charger ? millivolts : millivolts + 60, percent = 0;
    if (open >= ocv[0]) percent = 100;
    else
        for (unsigned i = 1; i < 21; ++i)
            if (open >= ocv[i]) {
                percent = 100 - 5 * i + 5 * (open - ocv[i]) / (ocv[i - 1] - ocv[i]);
                break;
            }
    *out = (struct battery_view){.available = true,
                                 .charger = charger,
                                 .charging = charger && gta7lite_charge_state() == 1,
                                 .percent = percent};
    return true;
}
