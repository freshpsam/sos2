#include "gta7lite.h"
#include "platform.h"
#include "volume_keys.h"

/* The volume keys, as Samsung's kernel reads them (drivers/input/keyboard/
 * mediatek/kpd.c and mtk-pmic-keys.c; the stock DT's kp node, whose key 1
 * is KEY_VOLUMEUP, and mt6357keys, whose home key is KEY_VOLUMEDOWN).
 * Volume Up is the keypad's KPCOL0 (GPIO 93), whose bit in KP_MEM1 is clear
 * while it is held, with the keypad scanning as LK leaves it; Volume Down is
 * the MT6357's home key, clear in TOPSTATUS while held. Presses and repeats
 * as include/volume_keys.h takes them. */
enum {
    KEYPAD = 0x10010000,
    KP_MEM1 = 0x04,
    KP_EN = 0x24,
    VOLUME_UP = 1 << 1,
    TOPSTATUS = 0x024,
    HOMEKEY_DEB = 1 << 3
};

static struct volume_keys keys;

static int held(void) {
    uint16_t status;
    bool up = (mmio_read32(KEYPAD + KP_EN) & 1) && !(mmio_read32(KEYPAD + KP_MEM1) & VOLUME_UP);
    bool down = gta7lite_pmic_read(TOPSTATUS, &status) && !(status & HOMEKEY_DEB);
    return up == down ? 0 : up ? 1 : -1;
}

int gta7lite_volume_key(void) { return volume_keys_step(&keys, held); }
