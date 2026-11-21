#include "nexus5.h"
#include "platform.h"
#include "qup_i2c.h"

/* The backlight: TI's LM3630 at 0x38 on BLSP2 QUP5 (GPIO83 SDA, GPIO84 SCL)
 * at 100 kHz, enabled by GPIO90, as Google's hammerhead device tree and the
 * stock driver (drivers/video/backlight/lm3630_bl.c) have it: bank A drives
 * both LED strings, linearly, without PWM. The bootloader leaves it lit at
 * its brightness, which standby keeps: off is the control register cleared
 * and the enable dropped; on is the enable raised, the stock settings (boost
 * 0x30: 24 V and 1 A limits; feedback from bank A; bank A's full-scale
 * current 0x12; bank A on, linear, with the second string), and the
 * brightness again. */
enum {
    ADDRESS = 0x38,
    CONTROL = 0x00,
    CONFIG = 0x01,
    BOOST = 0x02,
    BRIGHTNESS_A = 0x03,
    CURRENT_A = 0x05,
    CONTROL_ON = 0x55,
    CONFIG_FEEDBACK_A = 0x08,
    BOOST_LIMITS = 0x30,
    CURRENT_FULL_SCALE = 0x12,
    LOWEST = 5, /* the stock driver's least brightness */
    SDA = 83,
    SCL = 84,
    TLMM = 0xfd511000, /* GPIO_CFG n at 0x10 n, GPIO_IN_OUT n at 0x10 n + 4 */
    ENABLE = 90,
    PIN_OUTPUT = 1 << 9,
    DRIVE_8MA = 3 << 6,
    PIN_HIGH = 2,
    SETTLE_US = 1000,
};

static struct qup_i2c bus = {.base = 0xf9967000, .khz = 100};
static uint8_t brightness; /* the bootloader's, read at the first standby */
static bool dark;

static uintptr_t pin(void) { return TLMM + 0x10u * ENABLE; }

bool nexus5_backlight(bool on) {
    if (on == !dark) return true;
    if (!bus.ready && !nexus5_i2c_start(&bus, SDA, SCL)) return false;
    if (!on) {
        if (!brightness && !qup_i2c_read(&bus, ADDRESS, BRIGHTNESS_A, &brightness, 1)) return false;
        if (brightness < LOWEST) brightness = LOWEST;
        if (!qup_i2c_write(&bus, ADDRESS, CONTROL, 0)) return false;
        mmio_write32(pin() + 4, 0);
        barrier();
        dark = true;
        return true;
    }
    mmio_write32(pin(), PIN_OUTPUT | DRIVE_8MA);
    mmio_write32(pin() + 4, PIN_HIGH);
    barrier();
    delay_us(SETTLE_US);
    if (!qup_i2c_write(&bus, ADDRESS, BOOST, BOOST_LIMITS) ||
        !qup_i2c_write(&bus, ADDRESS, CONFIG, CONFIG_FEEDBACK_A) ||
        !qup_i2c_write(&bus, ADDRESS, CURRENT_A, CURRENT_FULL_SCALE) ||
        !qup_i2c_write(&bus, ADDRESS, CONTROL, CONTROL_ON))
        return false;
    delay_us(SETTLE_US);
    if (!qup_i2c_write(&bus, ADDRESS, BRIGHTNESS_A, brightness)) return false;
    dark = false;
    return true;
}
