#include "kernel_board.h"
#include "platform.h"
#include "display.h"
#include "page_host.h"
#include "haptics.h"
#include "kernel_io.h"
#include "machine.h"
#include "smp.h"
#include "phone_input.h"
#include "redmi2_power.h"
#include "redmi2_startup.h"
#include "redmi2_display.h"
#include "redmi2_cpu_clock.h"
#include "redmi2_adreno.h"
#include "redmi2_wifi.h"
#include "redmi2_cpu_power.h"
#include "redmi2_chip_sleep.h"
#include "redmi2_idle_devices.h"
#include "redmi2_audio.h"
#include "audio.h"
#include "wifi_setup.h"
#include "wifi_device.h"
#include "handoff.h"

/* The Redmi 2's part of the kernel's start and main loop (kernel/main.c):
 * lk2nd's verified handoff and its startup screen, the power driver's
 * screen changes, the touch screen, the keys and the volume keys. Its
 * standby: the backlight off, then the panel asleep, held in reset, the
 * touch controller asleep, Wi-Fi's power saving in its firmware, an idle
 * GPU's clocks stopped, the idle secondary CPUs and then CPU 0 powered down
 * and the CPU cluster at 200 MHz; once Wi-Fi's radio is off, the sound
 * hardware is off (3 s after the last frame) and the power key's interrupt
 * is armed, the whole chip sleeps 10 s at a time (chip_sleep.c; 1 s polls
 * where it cannot), and a press or release wakes it at once. While it sleeps, the
 * panel's, touch controller's and motion sensor's supply (L17) is off
 * (rails.c), so waking puts the motion sensor back to sleep. */
extern char __image_start[], __image_end[];

enum { STANDBY_IDLE_US = 1000000, STANDBY_DEEP_US = 10000000, KEY_SETTLE_US = 1000 };

/* A whole-chip sleep since the motion sensor last went back to sleep: each
 * chip wake powers it up running (L17, rails.c). */
static bool chip_slept;

/* 1 s once nothing needs the 20 ms polls: CPU 0 powers down, Wi-Fi's radio
 * is off (or there is no Wi-Fi), the GPU rests, no service job is part way
 * (an eMMC write yields while the card programs), no vibrator stop waits to
 * be tried again, the sound hardware is off, and the key's interrupt is
 * armed with no press or release being debounced and its shared line low. */
static uint32_t standby_sleep_us(void) {
    bool quiet = redmi2_cpu0_wake_irq() && redmi2_power_key_irq() &&
                 redmi2_wifi_standby_settled() && redmi2_adreno_resting() && !kernel_io_busy() &&
                 haptics_quiet() && redmi2_power_quiet() && redmi2_audio_quiet() &&
                 !redmi2_cpu0_source_raised();
    return quiet ? STANDBY_IDLE_US : BOARD_STANDBY_POLL_US;
}

/* After a long sleep the key's line did not end: an edge latched since, with
 * the line still low once the arbiter has had time to raise it, never
 * reached CPU 0, so the power driver takes it now. An edge just after CPU 0
 * woke is latched too, but raises the line, so the line is read after the
 * latch. */
static void standby_key_check(void) {
    if (redmi2_cpu0_source_seen() || !redmi2_power_key_latched()) return;
    for (uint64_t start = time_us(); time_us() - start < KEY_SETTLE_US;)
        if (redmi2_cpu0_source_raised()) return;
    redmi2_power_key_missed();
}

void board_handoff(struct platform_devices *d, uintptr_t dtb_address, struct framebuffer *screen) {
    const char *error =
        redmi2_validate_handoff(d, (uintptr_t)__image_start, (uintptr_t)__image_end, dtb_address);
    if (error) {
        /* The message on screen too, when only something else failed: a
         * framebuffer in RAM, clear of this kernel, can take it. */
        uintptr_t fb = (uintptr_t)d->framebuffer.pixels;
        if (d->has_framebuffer && gfx_framebuffer_valid(&d->framebuffer) &&
            fdt_range_in_ram(d, fb, d->framebuffer_size) &&
            (fb >= (uintptr_t)__image_end ||
             fb + d->framebuffer_size <= (uintptr_t)__image_start)) {
            *screen = d->framebuffer;
            platform_set_framebuffer(screen);
        }
        panic(error);
    }
    *screen = d->framebuffer;
    platform_set_framebuffer(screen);
}

void board_before_pages(const struct platform_devices *d) { (void)d; }

void board_pages_ready(struct platform_devices *d, struct framebuffer *screen) {
    (void)d;
    redmi2_startup_begin(screen);
    (void)phone_input_init(screen->width, screen->height);
    (void)redmi2_power_init();
    (void)redmi2_display_init();
}

bool board_processes_ready(const struct platform_devices *d, const struct framebuffer *screen) {
    (void)d;
    (void)screen;
    redmi2_cpu0_wake_source(redmi2_power_key_irq()); /* CPU 0's power-down is ready now. */
    (void)redmi2_chip_sleep_init();
    redmi2_idle_devices_init();
    redmi2_startup_end();
    page_host_redraw();
    if (redmi2_power_screen_on()) page_host_render();
    /* The screen may have been turned off during startup. */
    return !redmi2_power_screen_on();
}

void board_poll(void) { }

enum board_power board_power_poll(void) {
    enum redmi2_power_change change = redmi2_power_poll();
    return change == REDMI2_POWER_SCREEN_OFF  ? BOARD_POWER_SCREEN_OFF
           : change == REDMI2_POWER_SCREEN_ON ? BOARD_POWER_SCREEN_ON
                                              : BOARD_POWER_NONE;
}

/* The volume keys step the loudspeaker's level, which the header shows. */
static void volume_poll(void) {
    int step = redmi2_volume_key();
    if (step) page_host_volume(redmi2_audio_volume(step), AUDIO_VOLUME_STEPS, false);
}

void board_input(struct pointer_event *event) {
    enum phone_input_source source = PHONE_INPUT_TOUCH;
    if (phone_input_poll(event, &source)) {
        if (source == PHONE_INPUT_KEY_PRESS) haptics_tap();
        else if (source == PHONE_INPUT_HOME) page_host_home();
        else if (source == PHONE_INPUT_BACK) page_host_back();
        else page_host_input(event);
    }
    volume_poll();
}

void board_after_frame(void) { redmi2_idle_devices_poll(); }

void board_screen_standby(bool on, struct pointer_event *cancel) {
    if (on) {
        phone_input_suspend(cancel);
        redmi2_display_standby(true);
        return;
    }
    redmi2_display_standby(false);
    redmi2_chip_sleep_standby(false);
    redmi2_idle_devices_wake(); /* The motion sensor back to sleep soon. */
    phone_input_resume();
}

void board_system_standby(bool on) {
    if (on) {
        chip_slept = false;
        arm_smp_sleep();
        wifi_setup_standby(true);
        wifi_device_standby(true);
        redmi2_adreno_standby(true);
        redmi2_cpu_clock_standby(true);
        redmi2_chip_sleep_standby(true);
        redmi2_idle_devices_standby();
        return;
    }
    redmi2_cpu_clock_standby(false);
    arm_smp_wake();
    redmi2_adreno_standby(false);
    wifi_device_standby(false);
    wifi_setup_standby(false);
}

void board_standby_step(struct pointer_event *scratch) {
    (void)scratch;
    /* A GPU job still in flight at standby retires first; then it naps. */
    redmi2_adreno_standby(true);
}

void board_standby_sleep(void) {
    uint32_t sleep_us = standby_sleep_us();
    if (sleep_us == STANDBY_IDLE_US && redmi2_chip_sleep(STANDBY_DEEP_US)) chip_slept = true;
    else {
        /* Staying up with L17 on: the motion sensor a chip wake powered up
         * goes back to sleep. */
        if (chip_slept) {
            chip_slept = false;
            redmi2_idle_devices_wake();
        }
        cpu_sleep_us(sleep_us);
    }
    redmi2_idle_devices_poll();
    if (sleep_us == STANDBY_IDLE_US) standby_key_check();
}
