#include "platform.h"
#include "fdt.h"
#include "display.h"
#include "input.h"
#include "board.h"
#include "process.h"
#include "page_host.h"
#include "battery.h"
#include "haptics.h"
#include "arm_machine.h"
#ifdef SOS2_BOARD_REDMI2
#include "phone_input.h"
#include "redmi2_power.h"
#include "redmi2_startup.h"
#include "redmi2_display.h"
#include "redmi2_cpu_clock.h"
#include "redmi2_adreno.h"
#include "redmi2_wifi.h"
#include "arm_smp.h"
#include "redmi2_cpu_power.h"
#include "redmi2_chip_sleep.h"
#include "redmi2_idle_devices.h"
#include "kernel_io.h"
#include "wifi_setup.h"
#include "handoff.h"
#endif
extern char __image_start[], __image_end[];
static struct framebuffer screen;

/* Standby: a power-button tap turns the screen off (on the phone the backlight,
 * then the panel goes to sleep, held in reset; black on QEMU), cancels input,
 * hibernates the touch controller (on the phone then holds it in reset),
 * hides and freezes pages, and lets CPU 0 sleep between
 * polls of the power key, network and service worker (on the phone powered
 * down). On the phone, Wi-Fi saves power in the firmware, an idle GPU stops
 * its clocks, the idle secondary CPUs power down and the CPU cluster runs at
 * 200 MHz; once Wi-Fi's radio is off and the power key's interrupt is armed,
 * the whole chip sleeps 10 s at a time (chip_sleep.c; 1 s polls where it
 * cannot), and a press or release wakes it at once. While it sleeps, the
 * panel's, touch controller's and motion sensor's supply (L17) is off
 * (rails.c), so waking puts the motion sensor back to sleep. The battery's
 * current is averaged meanwhile. The next tap wakes the system with a full
 * redraw. */
enum {
    STANDBY_POLL_US = 20000,
    STANDBY_IDLE_US = 1000000,
    STANDBY_DEEP_US = 10000000,
    KEY_SETTLE_US = 1000
};

static bool standby;
#ifdef SOS2_BOARD_REDMI2
/* A whole-chip sleep since the motion sensor last went back to sleep: each
 * chip wake powers it up running (L17, rails.c). */
static bool chip_slept;

/* 1 s once nothing needs the 20 ms polls: CPU 0 powers down, Wi-Fi's radio
 * is off (or there is no Wi-Fi), the GPU rests, no service job is part way
 * (an eMMC write yields while the card programs), no vibrator stop waits to
 * be tried again, and the key's interrupt is armed with no press or release
 * being debounced and its shared line low. */
static uint32_t standby_sleep_us(void) {
    bool quiet = redmi2_cpu0_wake_irq() && redmi2_power_key_irq() &&
                 redmi2_wifi_standby_settled() && redmi2_adreno_resting() && !kernel_io_busy() &&
                 haptics_quiet() && redmi2_power_quiet() && !redmi2_cpu0_source_raised();
    return quiet ? STANDBY_IDLE_US : STANDBY_POLL_US;
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
#endif
static void enter_standby(struct pointer_event *cancel) {
    standby = true;
    haptics_stop();
    page_host_screen_active(false);
#ifdef SOS2_BOARD_REDMI2
    phone_input_suspend(cancel);
    redmi2_display_standby(true);
#else
    *cancel = (struct pointer_event){.cancelled = true};
    display_blank();
#endif
    page_host_input(cancel);
    process_freeze(true);
    battery_standby(true);
#ifdef SOS2_BOARD_REDMI2
    chip_slept = false;
    arm_smp_sleep();
    wifi_setup_standby(true);
    redmi2_wifi_standby(true);
    redmi2_adreno_standby(true);
    redmi2_cpu_clock_standby(true);
    redmi2_chip_sleep_standby(true);
    redmi2_idle_devices_standby();
#endif
}

static void leave_standby(void) {
    standby = false;
#ifdef SOS2_BOARD_REDMI2
    redmi2_cpu_clock_standby(false);
    arm_smp_wake();
    redmi2_adreno_standby(false);
    redmi2_wifi_standby(false);
    wifi_setup_standby(false);
#endif
    battery_standby(false);
    process_freeze(false);
    page_host_screen_active(true);
#ifdef SOS2_BOARD_REDMI2
    redmi2_display_standby(false);
    redmi2_chip_sleep_standby(false);
    redmi2_idle_devices_wake(); /* The motion sensor back to sleep soon. */
    phone_input_resume();
#endif
    page_host_redraw();
}

void kernel_main(const void *dtb) {
    /* Board RAM window: verify the maximum DTB span before parsing its header. */
    uintptr_t dtb_address = (uintptr_t)dtb;
    if ((dtb_address & 7) || dtb_address < SOS2_RAM_START ||
        dtb_address > SOS2_RAM_END - 2 * 1024 * 1024)
        panic("DTB outside board RAM window");
    static struct platform_devices devices;
    if (!fdt_discover(dtb, &devices)) panic("invalid device tree");
#ifdef SOS2_BOARD_REDMI2
    const char *error = redmi2_validate_handoff(&devices, (uintptr_t)__image_start,
                                                (uintptr_t)__image_end, dtb_address);
    if (error) {
        /* The message on screen too, when only something else failed: a
         * framebuffer in RAM, clear of this kernel, can take it. */
        uintptr_t fb = (uintptr_t)devices.framebuffer.pixels;
        if (devices.has_framebuffer && gfx_framebuffer_valid(&devices.framebuffer) &&
            fdt_range_in_ram(&devices, fb, devices.framebuffer_size) &&
            (fb >= (uintptr_t)__image_end ||
             fb + devices.framebuffer_size <= (uintptr_t)__image_start)) {
            screen = devices.framebuffer;
            platform_set_framebuffer(&screen);
        }
        panic(error);
    }
    screen = devices.framebuffer;
    platform_set_framebuffer(&screen);
#else
    uintptr_t start = (uintptr_t)__image_start, end = (uintptr_t)__image_end;
    if (end <= start || !fdt_range_in_ram(&devices, start, end - start))
        panic("kernel static memory outside RAM");
    if (dtb_address < end && dtb_address + devices.dtb_size > start)
        panic("DTB overlaps kernel memory");
    if (!display_init(&devices, &screen)) panic("display initialization failed");
    platform_set_framebuffer(&screen);
#endif
    if (!gfx_framebuffer_valid(&screen)) panic("browser framebuffer invalid");
    page_host_init(&screen);
#ifdef SOS2_BOARD_REDMI2
    redmi2_startup_begin(&screen);
    (void)phone_input_init(screen.width, screen.height);
    (void)redmi2_power_init();
    (void)redmi2_display_init();
#else
    page_host_render();
    display_poll();
    barrier();
    if (!input_init(devices.virtio, devices.virtio_count, screen.width, screen.height))
        panic("input initialization failed");
    (void)keyboard_init(devices.virtio, devices.virtio_count);
    if (devices.power_key) (void)power_key_init(devices.gpio, devices.power_key_pin);
#endif
    haptics_init(); /* On the phone, through the power driver's PMIC access. */
    if (!process_init(&devices, dtb_address)) panic("protected process initialization failed");
#ifdef SOS2_BOARD_REDMI2
    redmi2_cpu0_wake_source(redmi2_power_key_irq()); /* CPU 0's power-down is ready now. */
    (void)redmi2_chip_sleep_init();
    redmi2_idle_devices_init();
    redmi2_startup_end();
    page_host_redraw();
    if (redmi2_power_screen_on()) page_host_render();
#endif
    struct pointer_event event;
#ifdef SOS2_BOARD_REDMI2
    /* The screen may have been turned off during startup. */
    if (!redmi2_power_screen_on()) enter_standby(&event);
#endif
    for (;;) {
        (void)haptics_poll();
#ifdef SOS2_BOARD_REDMI2
        enum redmi2_power_change power_change = redmi2_power_poll();
        if (power_change == REDMI2_POWER_SCREEN_OFF) enter_standby(&event);
        else if (power_change == REDMI2_POWER_SCREEN_ON) leave_standby();
#else
        if (power_key_pressed()) {
            if (standby) leave_standby();
            else enter_standby(&event);
        }
#endif
        if (standby) {
#ifdef SOS2_BOARD_REDMI2
            /* A GPU job still in flight at standby retires first; then it naps. */
            redmi2_adreno_standby(true);
#else
            /* Drop input queued while the screen is off; nothing acts on it. */
            int32_t wheel_x, wheel_y;
            struct user_input_event key;
            for (unsigned budget = 0;
                 budget < 16 && input_poll_extended(&event, &wheel_x, &wheel_y); ++budget) { }
            for (unsigned budget = 0; budget < 32 && keyboard_poll(&key); ++budget) { }
#endif
            process_step();
            display_poll();
#ifdef SOS2_BOARD_REDMI2
            uint32_t sleep_us = standby_sleep_us();
            if (sleep_us == STANDBY_IDLE_US && redmi2_chip_sleep(STANDBY_DEEP_US))
                chip_slept = true;
            else {
                /* Staying up with L17 on: the motion sensor a chip wake
                 * powered up goes back to sleep. */
                if (chip_slept) {
                    chip_slept = false;
                    redmi2_idle_devices_wake();
                }
                arm_cpu_sleep_us(sleep_us);
            }
            redmi2_idle_devices_poll();
            if (sleep_us == STANDBY_IDLE_US) standby_key_check();
#else
            arm_cpu_sleep_us(STANDBY_POLL_US);
#endif
            continue;
        }
#ifdef SOS2_BOARD_REDMI2
        enum phone_input_source source = PHONE_INPUT_TOUCH;
        if (phone_input_poll(&event, &source)) {
            if (source == PHONE_INPUT_BACK_PRESS) haptics_tap();
            else if (source == PHONE_INPUT_BACK) page_host_back();
            else page_host_input(&event);
        }
#else
        /* Drain queued tablet reports. Taking one per pass lets the 64-event
         * ring fill while the loop is slow, and the device then drops whole
         * reports, including button releases. page_host merges pure motion. */
        for (unsigned budget = 0; budget < 16; ++budget) {
            int32_t wheel_x = 0, wheel_y = 0;
            if (!input_poll_extended(&event, &wheel_x, &wheel_y)) break;
            page_host_input(&event);
            page_host_wheel(&event, wheel_x, wheel_y);
            if (event.cancelled) break;
        }
#endif
#ifndef SOS2_BOARD_REDMI2
        /* Dispatch pointer releases before keyboard events in the same poll. */
        struct user_input_event key;
        for (unsigned budget = 0; budget < 32 && keyboard_poll(&key); ++budget)
            page_host_keyboard(&key);
#endif
        page_host_render();
        display_poll();
#ifdef SOS2_BOARD_REDMI2
        redmi2_idle_devices_poll();
#endif
        /* A tap's buzz ends here, not after process_step, whose service
         * worker can hold CPU 0 for tens of ms; meanwhile the display keeps
         * publishing the press. */
        while (haptics_poll()) display_poll();
        __asm__ volatile("yield");
        process_step();
    }
}
