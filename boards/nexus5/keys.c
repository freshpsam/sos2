#include "nexus5.h"
#include "volume_keys.h"

/* The volume keys (the stock device tree's gpio-keys): Volume Up on PM8941
 * GPIO 2 and Volume Down on GPIO 3, inputs on their 30 uA pull-ups that read
 * low while held (STATUS1's bit 0, valid while its bit 7 says the pin is
 * enabled; qpnp-pin.c), set up here as that tree sets them. Presses and
 * repeats as include/volume_keys.h takes them. */
enum {
    VOLUME_UP = 0xc100,
    VOLUME_DOWN = 0xc200,
    STATUS1 = 0x08,
    VALUE = 1 << 0,
    ENABLED = 1 << 7,
    MODE = 0x40,
    SUPPLY = 0x41,
    PULL = 0x42,
    ENABLE = 0x46,
    INPUT = 0,
    S3 = 2,
    PULL_UP_30UA = 0,
    ON = 0x80
};

static struct volume_keys keys;

static bool input(uint16_t base) {
    return nexus5_pmic_write(base + SUPPLY, S3) && nexus5_pmic_write(base + PULL, PULL_UP_30UA) &&
           nexus5_pmic_write(base + MODE, INPUT) && nexus5_pmic_write(base + ENABLE, ON);
}

static bool down(uint16_t base) {
    uint8_t status;
    return nexus5_pmic_read(base + STATUS1, &status) && (status & ENABLED) && !(status & VALUE);
}

static int held(void) {
    static bool ready;
    if (!ready) ready = input(VOLUME_UP) && input(VOLUME_DOWN);
    if (!ready) return 0;
    bool up = down(VOLUME_UP), lower = down(VOLUME_DOWN);
    return up == lower ? 0 : up ? 1 : -1;
}

int nexus5_volume_key(void) { return volume_keys_step(&keys, held); }
