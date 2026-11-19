#include "gta7lite.h"
#include "platform.h"

/* The MT6370 sub-PMIC at address 0x34 on I2C5 (the stock dtbo's
 * subpmic_pmu@34): its charger charges the battery, and its backlight driver
 * lights the panel at the brightness the panel's own PWM output gives it
 * (led@6's mode 4). Registers as Linux's drivers/mfd/mt6370.c,
 * mt6370-backlight.c and mt6370-charger.c name them. I2C5 is MediaTek's v2
 * controller, driven as Linux's i2c-mt65xx.c drives it in FIFO mode, with
 * the timing LK left: a register write, or the register's address written
 * then one byte read after a repeated start. */
enum {
    I2C5 = 0x11016000,
    DATA_PORT = 0x00,
    SLAVE_ADDR = 0x04,
    INTR_STAT = 0x0c,
    CONTROL = 0x10,
    TRANSFER_LEN = 0x14,
    TRANSAC_LEN = 0x18,
    START = 0x24,
    FIFO_ADDR_CLR = 0x38,
    TRANSFER_LEN_AUX = 0x44,
    TRANSAC_COMP = 1 << 0,
    ACKERR = 1 << 1,
    HS_NACKERR = 1 << 2,
    ARB_LOST = 1 << 3,
    INTR_ALL = 0x1ff,
    CONTROL_RS = 1 << 1,
    CONTROL_CLK_EXT = 1 << 3,
    CONTROL_DIR_CHANGE = 1 << 4,
    CONTROL_ACKERR_DET = 1 << 5,
    TRANSFER_US = 20000,
    MT6370 = 0x34,
    REG_CHG_STAT = 0x4a, /* [7:6]: ready, charging, done, fault */
    CHG_STAT_SHIFT = 6,
    REG_BL_EN = 0xa0,
    BL_EN = 1 << 6
};

static bool transfer(uint8_t reg, bool write, uint8_t *value) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        mmio_write32(I2C5 + INTR_STAT, INTR_ALL);
        mmio_write32(I2C5 + FIFO_ADDR_CLR, 1);
        mmio_write32(I2C5 + SLAVE_ADDR, MT6370 << 1);
        if (write) {
            mmio_write32(I2C5 + TRANSFER_LEN, 2);
            mmio_write32(I2C5 + TRANSAC_LEN, 1);
            mmio_write32(I2C5 + CONTROL, CONTROL_CLK_EXT | CONTROL_ACKERR_DET);
            mmio_write32(I2C5 + DATA_PORT, reg);
            mmio_write32(I2C5 + DATA_PORT, *value);
        } else {
            mmio_write32(I2C5 + TRANSFER_LEN, 1);
            mmio_write32(I2C5 + TRANSFER_LEN_AUX, 1);
            mmio_write32(I2C5 + TRANSAC_LEN, 2);
            mmio_write32(I2C5 + CONTROL,
                         CONTROL_CLK_EXT | CONTROL_ACKERR_DET | CONTROL_DIR_CHANGE | CONTROL_RS);
            mmio_write32(I2C5 + DATA_PORT, reg);
        }
        mmio_write32(I2C5 + START, 1);
        uint32_t status = 0;
        for (uint64_t start = time_us(); time_us() - start < TRANSFER_US;) {
            status = mmio_read32(I2C5 + INTR_STAT);
            if (status & (TRANSAC_COMP | ACKERR | HS_NACKERR | ARB_LOST)) break;
        }
        mmio_write32(I2C5 + INTR_STAT, INTR_ALL);
        if ((status & TRANSAC_COMP) && !(status & (ACKERR | HS_NACKERR | ARB_LOST))) {
            if (!write) *value = (uint8_t)mmio_read32(I2C5 + DATA_PORT);
            return true;
        }
    }
    return false;
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
