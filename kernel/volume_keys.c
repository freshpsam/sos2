#include "volume_keys.h"
#include "platform.h"

enum { SAMPLE_US = 20000, STEADY_US = 40000, REPEAT_DELAY_US = 500000, REPEAT_US = 150000 };

int volume_keys_step(struct volume_keys *keys, int (*held)(void)) {
    uint64_t now = time_us();
    if (now - keys->sampled < SAMPLE_US) return 0;
    keys->sampled = now;
    int key = held();
    if (!keys->started) {
        keys->started = true;
        keys->candidate = keys->steady = key;
        keys->since = now;
        return 0;
    }
    if (key != keys->candidate) {
        keys->candidate = key;
        keys->since = now;
        return 0;
    }
    if (now - keys->since < STEADY_US) return 0;
    if (key != keys->steady) {
        keys->steady = key;
        keys->stepped = now;
        keys->repeating = false;
        return key;
    }
    /* Nothing stepped yet: the key was held through the boot. */
    if (!key || !keys->stepped ||
        now - keys->stepped < (keys->repeating ? REPEAT_US : REPEAT_DELAY_US))
        return 0;
    keys->stepped = now;
    keys->repeating = true;
    return key;
}
