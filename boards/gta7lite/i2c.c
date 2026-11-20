#include "gta7lite.h"
#include "platform.h"

/* MT6765's I2C controllers, MediaTek's v2 (the stock DT's i2c_common: ver 2),
 * driven as Linux's i2c-mt65xx.c drives them in FIFO mode: a register write,
 * or the register's address written then one byte read after a repeated
 * start. LK leaves I2C5 running, timed for the MT6370 (mt6370.c). I2C3,
 * where the speaker amplifiers answer (audio.c), shares I2C5's clock, the
 * infracfg gate of I2C0-6 (clk-mt6765.c's ifr_i2c_ap), and its divider
 * (both nodes' clock-div 5), so it gets I2C5's timing, and its pins. I2C3
 * has several channels: the AP's registers are a channel 0x100 in, whose
 * FIFO is cleared with its own bit too, and which keeps the bus after a
 * failed transfer until it completes and the controller's own START register
 * resumes the arbitration (Samsung's i2c-mtk.c: ch_offset, I2C_RESUME_ARBIT). */
enum {
    DATA_PORT = 0x00,
    SLAVE_ADDR = 0x04,
    INTR_STAT = 0x0c,
    CONTROL = 0x10,
    TRANSFER_LEN = 0x14,
    TRANSAC_LEN = 0x18,
    DELAY_LEN = 0x1c,
    TIMING = 0x20,
    START = 0x24,
    RESUME_ARBIT = 1 << 1,
    CHANNEL = 0x100,
    EXT_CONF = 0x28,
    LTIMING = 0x2c,
    HS = 0x30,
    IO_CONFIG = 0x34,
    FIFO_ADDR_CLR = 0x38,
    FIFO_CLEAR = 1 << 0,
    FIFO_CLEAR_CHANNEL = 1 << 2,
    TRANSFER_LEN_AUX = 0x44,
    CLOCK_DIV = 0x48,
    HW_TIMEOUT = 0x4c,
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
    INFRACFG = 0x10001000,
    IFR2_CLR = 0x84,
    IFR2_STA = 0x90,
    IFR_I2C_AP = 1 << 11,
    /* GPIO 48-55's modes, 4 bits each, with set and clear registers after
     * it (pinctrl-mt6765.c): GPIO 50 and 51 are SCL3 and SDA3 in mode 1. */
    GPIO_MODE_48 = 0x10005360,
    GPIO_SET = 4,
    GPIO_CLR = 8,
    I2C3_PINS = 0xff00,
    I2C3_MODE = 0x1100
};

bool gta7lite_i2c_transfer(uintptr_t bus, uint8_t device, uint8_t reg, bool write, uint8_t *value) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        mmio_write32(bus + INTR_STAT, INTR_ALL);
        mmio_write32(bus + FIFO_ADDR_CLR,
                     bus == GTA7LITE_I2C3 ? FIFO_CLEAR_CHANNEL | FIFO_CLEAR : FIFO_CLEAR);
        mmio_write32(bus + SLAVE_ADDR, (uint32_t)device << 1);
        if (write) {
            mmio_write32(bus + TRANSFER_LEN, 2);
            mmio_write32(bus + TRANSAC_LEN, 1);
            mmio_write32(bus + CONTROL, CONTROL_CLK_EXT | CONTROL_ACKERR_DET);
            mmio_write32(bus + DATA_PORT, reg);
            mmio_write32(bus + DATA_PORT, *value);
        } else {
            mmio_write32(bus + TRANSFER_LEN, 1);
            mmio_write32(bus + TRANSFER_LEN_AUX, 1);
            mmio_write32(bus + TRANSAC_LEN, 2);
            mmio_write32(bus + CONTROL,
                         CONTROL_CLK_EXT | CONTROL_ACKERR_DET | CONTROL_DIR_CHANGE | CONTROL_RS);
            mmio_write32(bus + DATA_PORT, reg);
        }
        mmio_write32(bus + START, 1);
        uint32_t status = 0;
        for (uint64_t start = time_us(); time_us() - start < TRANSFER_US;) {
            status = mmio_read32(bus + INTR_STAT);
            if (status & (TRANSAC_COMP | ACKERR | HS_NACKERR | ARB_LOST)) break;
        }
        bool done = (status & TRANSAC_COMP) && !(status & (ACKERR | HS_NACKERR | ARB_LOST));
        if (!done && bus == GTA7LITE_I2C3)
            for (uint64_t start = time_us(); !(mmio_read32(bus + INTR_STAT) & TRANSAC_COMP) &&
                                             time_us() - start < TRANSFER_US;) { }
        mmio_write32(bus + INTR_STAT, INTR_ALL);
        if (done) {
            if (!write) *value = (uint8_t)mmio_read32(bus + DATA_PORT);
            return true;
        }
        if (bus == GTA7LITE_I2C3) mmio_write32(bus - CHANNEL + START, RESUME_ARBIT);
    }
    return false;
}

void gta7lite_i2c3_init(void) {
    static const uint16_t timing[] = {TIMING,   LTIMING,   HS,        CLOCK_DIV,
                                      EXT_CONF, IO_CONFIG, DELAY_LEN, HW_TIMEOUT};
    if (mmio_read32(INFRACFG + IFR2_STA) & IFR_I2C_AP)
        mmio_write32(INFRACFG + IFR2_CLR, IFR_I2C_AP);
    if ((mmio_read32(GPIO_MODE_48) & I2C3_PINS) != I2C3_MODE) {
        mmio_write32(GPIO_MODE_48 + GPIO_CLR, I2C3_PINS);
        mmio_write32(GPIO_MODE_48 + GPIO_SET, I2C3_MODE);
    }
    for (unsigned i = 0; i < sizeof(timing) / sizeof(timing[0]); ++i)
        mmio_write32(GTA7LITE_I2C3 + timing[i], mmio_read32(GTA7LITE_I2C5 + timing[i]));
}
