#include "gta7lite.h"
#include "platform.h"

/* The MT6370 sub-PMIC at address 0x34 on I2C5 (the stock dtbo's
 * subpmic_pmu@34): its charger charges the battery, and its backlight driver
 * lights the panel at the brightness the panel's own PWM output gives it
 * (led@6's mode 4). Registers as Linux's drivers/mfd/mt6370.c,
 * mt6370-backlight.c and mt6370-charger.c name them; transfers through
 * i2c.c, with the timing LK left. */
enum {
    MT6370 = 0x34,
    REG_CHG_STAT = 0x4a, /* [7:6]: ready, charging, done, fault */
    CHG_STAT_SHIFT = 6,
    REG_BL_EN = 0xa0,
    BL_EN = 1 << 6
};

static bool transfer(uint8_t reg, bool write, uint8_t *value) {
    return gta7lite_i2c_transfer(GTA7LITE_I2C5, MT6370, reg, write, value);
}

/* BL_EN as LK left it, while standby holds it off; 0 otherwise. */
static uint8_t backlight_saved;

void gta7lite_backlight(bool on) {
    if (on) {
        if (backlight_saved && transfer(REG_BL_EN, true, &backlight_saved)) backlight_saved = 0;
        return;
    }
    /* Only a backlight LK turned on is turned off. */
    uint8_t value;
    if (backlight_saved || !transfer(REG_BL_EN, false, &value) || !(value & BL_EN)) return;
    uint8_t off = value & (uint8_t)~BL_EN;
    if (transfer(REG_BL_EN, true, &off)) backlight_saved = value;
}

int gta7lite_charge_state(void) {
    uint8_t value;
    return transfer(REG_CHG_STAT, false, &value) ? value >> CHG_STAT_SHIFT : -1;
}
