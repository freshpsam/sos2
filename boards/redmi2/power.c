#include "redmi2_power.h"
#include "redmi2_power_hw.h"
#include "qcom_scm.h"
#include <stdint.h>

#include "platform.h"
#define now_us time_us

enum {
    SAMPLE_US = 10000,
    DEBOUNCE_US = 20000,
    RETRY_US = 1000000,
    MAX_FAILURES = 3,
    MAX_RESTORE_ATTEMPTS = 3,
    KEY_IRQ = 32 + 190 /* the SPMI arbiter's interrupt for this processor */
};

static bool ready, awake, candidate, stable, wait_release, restore_pending, key_irq;
static unsigned failures, restore_attempts;
static uint64_t last_sample, candidate_since, last_restore;

/* A failed key transport must not strand the normal input loop asleep. All
 * transactions and the three restoration attempts are bounded. */
static enum redmi2_power_change fail_awake(bool restore) {
    bool was_awake = awake;
    ready = false;
    awake = true;
    if (restore) {
        restore_attempts = 1;
        restore_pending = !redmi2_power_hw_set_backlight(true);
        last_restore = now_us();
    }
    return was_awake ? REDMI2_POWER_ERROR : REDMI2_POWER_SCREEN_ON;
}

bool redmi2_power_init(void) {
    /* Preserve an active toggle/held-key state and any bounded recovery work.
     * The hardware initializer refuses to recapture a blanked backlight;
     * resetting policy state first would falsely mark it awake and lose retries. */
    if (ready) return true;
    if (restore_pending) return false;
    ready = false;
    awake = true;
    restore_pending = false;
    failures = restore_attempts = 0;
    stable = false;
    wait_release = true;
    if (!redmi2_power_hw_init() || !redmi2_power_hw_read_key(&candidate)) return false;
    last_sample = candidate_since = now_us();
    wait_release = candidate;
    ready = true;
    key_irq = redmi2_power_hw_key_irq();
    return true;
}

unsigned redmi2_power_key_irq(void) { return ready && key_irq ? KEY_IRQ : 0; }

bool redmi2_power_key_latched(void) { return ready && key_irq && redmi2_power_hw_key_latched(); }

void redmi2_power_key_missed(void) {
    key_irq = false; /* standby polls the key every 20 ms from now on */
}

bool redmi2_power_quiet(void) {
    if (!ready || !key_irq || last_sample - candidate_since < DEBOUNCE_US) return false;
    /* An edge between the last sample and the rearm is in the key's state. */
    bool pressed;
    return redmi2_power_hw_key_rearm() && redmi2_power_hw_read_key(&pressed) &&
           pressed == candidate;
}

bool redmi2_power_screen_on(void) { return awake; }

/* The backlight switched and the state flipped, as a press does. */
static enum redmi2_power_change toggle(void) {
    if (!redmi2_power_hw_set_backlight(!awake)) return fail_awake(true);
    awake = !awake;
    return awake ? REDMI2_POWER_SCREEN_ON : REDMI2_POWER_SCREEN_OFF;
}

enum redmi2_power_change redmi2_power_poll(void) {
    uint64_t now = now_us();
    if (!ready) {
        if (restore_pending && now - last_restore >= RETRY_US) {
            ++restore_attempts;
            bool restored = redmi2_power_hw_set_backlight(true);
            last_restore = now_us();
            restore_pending = !restored && restore_attempts < MAX_RESTORE_ATTEMPTS;
            return REDMI2_POWER_ERROR;
        }
        return REDMI2_POWER_UNCHANGED;
    }
    if (now - last_sample < SAMPLE_US) return REDMI2_POWER_UNCHANGED;
    bool pressed;
    bool read_ok = redmi2_power_hw_read_key(&pressed);
    last_sample = now_us();
    if (!read_ok) {
        /* An uncertain key sample cannot complete a press or release. */
        wait_release = true;
        candidate_since = last_sample;
        if (++failures >= MAX_FAILURES) return fail_awake(!awake);
        return REDMI2_POWER_UNCHANGED;
    }
    failures = 0;
    if (pressed != candidate) {
        candidate = pressed;
        candidate_since = last_sample;
        return REDMI2_POWER_UNCHANGED;
    }
    if (last_sample - candidate_since < DEBOUNCE_US) return REDMI2_POWER_UNCHANGED;
    if (wait_release) {
        if (!pressed) {
            wait_release = false;
            stable = false;
        }
        return REDMI2_POWER_UNCHANGED;
    }
    if (pressed == stable) return REDMI2_POWER_UNCHANGED;
    stable = pressed;
    if (!pressed) return REDMI2_POWER_UNCHANGED;
    return toggle();
}

/* lk2nd's reboot_device and pm8x41_reset_configure (platform/msm_shared/
 * reboot.c, dev/pmic/pm8x41/pm8x41.c at 3e86db9): PS_HOLD's reset off in the
 * PM8916's PON (PS_HOLD_RESET_CTL2, 0x85b), 300 us, its type a hard reset
 * (PS_HOLD_RESET_CTL, 0x85a = 7), on again (bit 7), the PMIC arbiter halted
 * through the secure monitor, then PS_HOLD low (MPM2_MPM_PS_HOLD,
 * 0x004ab000). A hard reset powers the phone down and up again, like a cold
 * boot. */
bool redmi2_power_restart(void) {
    enum {
        PON = 0x008,
        PS_HOLD_RESET_CTL = 0x5a,
        PS_HOLD_RESET_CTL2 = 0x5b,
        HARD_RESET = 7,
        RESET_ENABLE = 0x80,
        PS_HOLD = 0x004ab000
    };

    if (!redmi2_power_hw_write(PON, PS_HOLD_RESET_CTL2, 0)) return false;
    for (uint64_t start = now_us(); now_us() - start < 300;) { }
    if (!redmi2_power_hw_write(PON, PS_HOLD_RESET_CTL, HARD_RESET) ||
        !redmi2_power_hw_write(PON, PS_HOLD_RESET_CTL2, RESET_ENABLE))
        return false;
    qcom_scm_halt_pmic_arbiter();
    barrier();
    mmio_write32(PS_HOLD, 0);
    barrier();
    for (uint64_t start = now_us(); now_us() - start < 2000000;) { }
    return false;
}
