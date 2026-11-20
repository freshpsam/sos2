#include "gta7lite.h"
#include "platform.h"

/* The volume keys, as Samsung's kernel reads them (drivers/input/keyboard/
 * mediatek/kpd.c and mtk-pmic-keys.c; the stock DT's kp node, whose key 1
 * is KEY_VOLUMEUP, and mt6357keys, whose home key is KEY_VOLUMEDOWN).
 * Volume Up is the keypad's KPCOL0 (GPIO 93), whose bit in KP_MEM1 is clear
 * while it is held, with the keypad scanning as LK leaves it; Volume Down is
 * the MT6357's home key, clear in TOPSTATUS while held. Both are sampled
 * every 20 ms and taken once steady for 40 ms. A press steps once; a key held
 * half a second steps again every 150 ms; keys held through the boot, or
 * both together, step nothing. */
enum {
    KEYPAD = 0x10010000,
    KP_MEM1 = 0x04,
    KP_EN = 0x24,
    VOLUME_UP = 1 << 1,
    TOPSTATUS = 0x024,
    HOMEKEY_DEB = 1 << 3,
    SAMPLE_US = 20000,
    STEADY_US = 40000,
    REPEAT_DELAY_US = 500000,
    REPEAT_US = 150000
};

static struct {
    uint64_t sampled, since, stepped;
    int candidate, steady; /* +1 Volume Up held, -1 Volume Down, 0 neither */
    bool started, repeating;
} keys;

static int held(void) {
    uint16_t status;
    bool up = (mmio_read32(KEYPAD + KP_EN) & 1) && !(mmio_read32(KEYPAD + KP_MEM1) & VOLUME_UP);
    bool down = gta7lite_pmic_read(TOPSTATUS, &status) && !(status & HOMEKEY_DEB);
    return up == down ? 0 : up ? 1 : -1;
}

int gta7lite_volume_key(void) {
    uint64_t now = time_us();
    if (now - keys.sampled < SAMPLE_US) return 0;
    keys.sampled = now;
    int key = held();
    if (!keys.started) {
        keys.started = true;
        keys.candidate = keys.steady = key;
        keys.since = now;
        return 0;
    }
    if (key != keys.candidate) {
        keys.candidate = key;
        keys.since = now;
        return 0;
    }
    if (now - keys.since < STEADY_US) return 0;
    if (key != keys.steady) {
        keys.steady = key;
        keys.stepped = now;
        keys.repeating = false;
        return key;
    }
    /* Nothing stepped yet: the key was held through the boot. */
    if (!key || !keys.stepped ||
        now - keys.stepped < (keys.repeating ? REPEAT_US : REPEAT_DELAY_US))
        return 0;
    keys.stepped = now;
    keys.repeating = true;
    return key;
}
