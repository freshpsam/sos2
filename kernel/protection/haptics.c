#include "haptics.h"
#include "platform.h"
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_vibrator.h"
#elif defined(SOS2_BOARD_NEXUS5)
#include "nexus5.h"
#endif

/* Tap feedback as Android gives it for keys: one pulse as the finger lands,
 * none on release or for a held key's repeats (PhoneWindowManager's
 * VIRTUAL_KEY on a virtual key's press; LatinIME's onPressKey, repeat count
 * 0). On a vibrator without prebaked effects, like this handset's, Android
 * 10 plays both as config_virtualKeyVibePattern and
 * config_keyboardTapVibePattern, which PM8916 phones tune to 18 to 35 ms
 * pulses: this handset's LineageOS build 28 ms at its 3.0 V, the Moto G 2015
 * 25 to 35 ms at 2.4 V (the level Motorola's driver uses for pulses under
 * 60 ms), the Wileyfox Swift 18 to 20 ms (AOSP's defaults: 10 ms, a 20 ms
 * pause and 30 ms; 40 ms). A tap here is shorter than all of them, 15 ms at
 * the handset's own 3.0 V: a coin motor gathers speed over tens of ms, so a
 * shorter pulse is a lighter tick. Much shorter ones, like the 5 to 12 ms
 * clicks AOSP specifies for actuators with overdrive and braking, would
 * hardly be felt.
 * https://github.com/LineageOS/android_device_wingtech_wt88047/blob/lineage-17.1/overlay/frameworks/base/core/res/res/values/config.xml
 * https://github.com/LineageOS/android_device_motorola_osprey/blob/lineage-17.1/overlay/frameworks/base/core/res/res/values/config.xml
 * https://github.com/LineageOS/android_kernel_motorola_msm8916/blob/lineage-17.1/arch/arm/boot/dts/qcom/msm8916-osprey.dtsi
 * https://github.com/LineageOS/android_device_wileyfox_crackling/blob/lineage-17.1/overlay/frameworks/base/core/res/res/values/config.xml
 * https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android10-release/core/res/res/values/config.xml
 * https://source.android.com/docs/core/interaction/haptics/haptics-constants-primitives
 * CPU 0 runs its loop with interrupts masked, so only the loop ends a pulse,
 * and one of its steps, process_step, can hold CPU 0 for tens of ms (a
 * service worker job, such as a jitterentropy chunk or a PBKDF2 hash). So a
 * tap starts at the next poll, and the loop waits the pulse out, still
 * publishing the display, before process_step: no pulse spans it, nor the
 * brake that follows it where the motor has one (the Nexus 5's, driving it
 * backwards for 15 ms), which must end on time. A stop that fails is tried
 * again every 20 ms until it works. */
enum { TAP_US = 15000, TAP_MV = 3000, RETRY_US = 20000 };
#ifdef SOS2_BOARD_REDMI2
_Static_assert(TAP_MV >= REDMI2_VIBRATOR_MIN_MV && TAP_MV <= REDMI2_VIBRATOR_MAX_MV &&
                   TAP_MV % 100 == 0,
               "TAP_MV is not a PM8916 vibrator level");
#endif

/* requested: a tap waits for the next poll; running: a pulse is on until
 * due; braking: its brake is, until due; stuck: a stop failed, to be tried
 * again at due. */
static bool ready, requested, running, braking, stuck;
static uint64_t due;

static bool run(bool on) {
#ifdef SOS2_BOARD_REDMI2
    return redmi2_vibrator_run(on);
#elif defined(SOS2_BOARD_NEXUS5)
    return nexus5_vibrator_run(on);
#else
    (void)on;
    return false;
#endif
}

/* After a pulse: how long the motor brakes before it stops, 0 for none. */
static uint32_t brake(void) {
#ifdef SOS2_BOARD_NEXUS5
    return nexus5_vibrator_brake();
#else
    return 0;
#endif
}

static void stop(uint64_t now) {
    running = braking = false;
    stuck = !run(false);
    if (stuck) due = now + RETRY_US;
}

void haptics_init(void) {
    requested = running = braking = stuck = false;
#ifdef SOS2_BOARD_REDMI2
    ready = redmi2_vibrator_init(TAP_MV);
#elif defined(SOS2_BOARD_NEXUS5)
    /* The Nexus 5's PWM-driven motor (boards/nexus5/vibrator.c): full drive
     * for the tap, then its brake. */
    ready = nexus5_vibrator_init();
#else
    /* QEMU virt has no vibrator, nor has the Galaxy Tab A7 Lite: its stock
     * vendor partition has no vibrator HAL, and pulses of the PMIC's VIBR
     * supply, which MediaTek's reference device tree names, are not felt. */
    ready = false;
#endif
}

/* A tap during a pulse gets the next one. */
void haptics_tap(void) {
    if (ready && !stuck) requested = true;
}

bool haptics_poll(void) {
    if (!running && !braking && !stuck && !requested) return false;
    uint64_t now = time_us();
    if (running || braking || stuck) {
        if (now < due) return running || braking;
        uint32_t us = running ? brake() : 0;
        if (us) {
            running = false;
            braking = true;
            due = now + us;
            return true;
        }
        stop(now);
        return false;
    }
    if (!requested) return false;
    requested = false;
    running = true;
    due = now + TAP_US;
    /* A start that failed may still have started the motor: the stop is checked. */
    (void)run(true);
    return true;
}

/* At once, without the brake. */
void haptics_stop(void) {
    requested = false;
    if (running || braking) stop(time_us());
}

bool haptics_quiet(void) { return !running && !braking && !stuck; }

/* Once: a fault in this stop panics again. */
void haptics_panic(void) {
    static bool tried;
    if (tried || (!running && !braking && !stuck)) return;
    tried = true;
    (void)run(false);
}
