#include "nexus5.h"
#include "battery.h"
#include "platform.h"
#include "qup_i2c.h"

/* The battery, as Google's msm8974-hammerhead device tree wires it: a
 * MAX17048 fuel gauge at 0x36 and a BQ24192 charger at 0x6b on BLSP1 QUP1
 * (GPIO2 SDA, GPIO3 SCL) at 100 kHz. Both are only read, as the Redmi 2's
 * gauge is: the gauge's cell voltage, which must be a live one before its
 * level counts, and its level; the charger's system status. The level is
 * the stock driver's (max17048_battery.c): the gauge's state of charge
 * scaled from its empty point (1.0 %) to its full one (97.0 %), the device
 * tree's empty-soc and full-soc. The phone's gauge counts its state of
 * charge in half the datasheet's steps (Linux's hammerhead device tree:
 * maxim,double-soc): it read 193.2 % at 4.29 V. */
enum {
    GAUGE = 0x36,
    VCELL = 0x02, /* 78.125 uV (5/64 mV) a unit, big-endian */
    SOC = 0x04,   /* 1/512 % a unit here, big-endian */
    EMPTY_TENTHS = 10,
    FULL_TENTHS = 970,
    CHARGER = 0x6b,
    SYSTEM_STATUS = 0x08,
    POWER_GOOD = 1 << 2,
    CHARGE_STATE = 3 << 4, /* not charging, pre-charging, fast charging, done */
    PRE_CHARGING = 1 << 4,
    FAST_CHARGING = 2 << 4,
    SDA = 2,
    SCL = 3,
};

static struct qup_i2c bus = {.base = 0xf9923000, .khz = 100};

bool nexus5_battery_read(struct battery_view *out) {
    if (!out || (!bus.ready && !nexus5_i2c_start(&bus, SDA, SCL))) return false;
    uint8_t voltage[2], charge[2], status;
    if (!qup_i2c_read(&bus, GAUGE, VCELL, voltage, 2) ||
        !qup_i2c_read(&bus, GAUGE, SOC, charge, 2) ||
        !qup_i2c_read(&bus, CHARGER, SYSTEM_STATUS, &status, 1))
        return false;
    unsigned millivolts = ((unsigned)voltage[0] << 8 | voltage[1]) * 5u / 64u;
    if (millivolts < 2500 || millivolts > 4600) return false;
    unsigned tenths = ((unsigned)charge[0] << 8 | charge[1]) * 10u / 512u;
    unsigned percent = tenths <= EMPTY_TENTHS ? 0
                       : tenths >= FULL_TENTHS
                           ? 100
                           : (tenths - EMPTY_TENTHS) * 100u / (FULL_TENTHS - EMPTY_TENTHS);
    unsigned state = status & CHARGE_STATE;
    bool charger = status & POWER_GOOD;
    *out = (struct battery_view){.available = true,
                                 .charger = charger,
                                 .charging =
                                     charger && (state == PRE_CHARGING || state == FAST_CHARGING),
                                 .percent = percent};
    return true;
}
