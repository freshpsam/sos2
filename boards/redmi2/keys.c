#include "redmi2_audio.h"
#include "redmi2_power_hw.h"
#include "platform.h"
#include "volume_keys.h"

/* The volume keys (msm8916-wingtech-wt88047.dts): Volume Up is GPIO 107,
 * low while held, on its pull-up (gpio-keys, set up here as that DT sets
 * it); Volume Down is the PM8916's RESIN, whose bit in the power-on block's
 * status is set while held (pm8941-pwrkey.c's PON_RESIN_N_SET), read
 * through the power driver's PMIC access. Presses and repeats as
 * include/volume_keys.h takes them. */
enum {
    VOLUME_UP = 0x0106b000, /* TLMM GPIO 107 */
    GPIO_IO = 4,
    GPIO_IN = 1,
    PIN_FIELDS = 0x3ff,
    PULLED_UP_INPUT = 3, /* GPIO, 2 mA, pull-up, no output */
    PON = 0x008,
    RT_STATUS = 0x10,
    RESIN = 1 << 1
};

static struct volume_keys keys;

static int held(void) {
    static bool ready;
    if (!ready) {
        mmio_write32(VOLUME_UP, (mmio_read32(VOLUME_UP) & ~(uint32_t)PIN_FIELDS) | PULLED_UP_INPUT);
        ready = true;
    }
    uint8_t status;
    bool up = !(mmio_read32(VOLUME_UP + GPIO_IO) & GPIO_IN);
    bool down = redmi2_power_hw_read(PON, RT_STATUS, &status) && (status & RESIN);
    return up == down ? 0 : up ? 1 : -1;
}

int redmi2_volume_key(void) { return volume_keys_step(&keys, held); }
