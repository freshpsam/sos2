#include "kernel_board.h"
#include "platform.h"
#include "audio.h"
#include "display.h"
#include "page_host.h"
#include "process.h"
#include "boot_progress.h"
#include "machine.h"
#include "smp.h"
#include "gta7lite.h"
#include "wifi_setup.h"
#include "wifi_device.h"

/* The Galaxy Tab A7 Lite's part of the kernel's start and main loop
 * (kernel/main.c): LK's handoff with its watchdog fed throughout, the USB
 * link from the first frame, touch, the volume keys and the CPUs' clocks.
 * Its standby: the panel and touch asleep, the CPUs at their lowest clock,
 * the idle secondary CPUs powered down, Wi-Fi's power saving. */
extern char __image_start[], __image_end[];

/* The volume keys step the level of the output in use, which the header
 * shows, as it shows the output's level when a plug goes in or out. */
static void volume_poll(void) {
    static bool headphones;
    int step = gta7lite_volume_key();
    bool plugged = gta7lite_audio_headphones();
    if (!step && plugged == headphones) return;
    headphones = plugged;
    page_host_volume(gta7lite_audio_volume(step), AUDIO_VOLUME_STEPS, plugged);
}

/* Long boot work (expanding the ramdisk, page tables, zeroing pages) reports
 * progress every 64 KiB to 1 MiB; each report feeds the watchdog. */
static void boot_feed(enum boot_phase phase, uint32_t done, uint32_t total) {
    (void)phase;
    (void)done;
    (void)total;
    gta7lite_watchdog_feed();
}

void board_handoff(struct platform_devices *d, uintptr_t dtb_address, struct framebuffer *screen) {
    gta7lite_watchdog_feed();
    boot_progress_set_handler(boot_feed);
    const char *error =
        gta7lite_handoff(d, (uintptr_t)__image_start, (uintptr_t)__image_end, dtb_address);
    if (error) {
        if (d->has_framebuffer) {
            *screen = d->framebuffer;
            platform_set_framebuffer(screen);
        }
        panic(error);
    }
    if (!display_init(d, screen)) panic("display initialization failed");
    platform_set_framebuffer(screen);
}

void board_before_pages(const struct platform_devices *d) { (void)d; }

void board_pages_ready(struct platform_devices *d, struct framebuffer *screen) {
    (void)d;
    (void)screen;
    /* The USB link from here on, so that after a panic the tablet can be
     * restarted from the computer; the host sees it once the main loop
     * polls it (gta7lite_usb_connect). */
    page_host_usb_status(gta7lite_usb_init());
    page_host_render();
    display_poll();
    barrier();
}

bool board_processes_ready(const struct platform_devices *d, const struct framebuffer *screen) {
    /* Its firmware is a runtime file and its SPI a device mapping, both ready now. */
    page_host_touch_status(gta7lite_touch_init(d->lcm_name, screen->width, screen->height));
    gta7lite_usb_connect();
    return false;
}

void board_poll(void) {
    gta7lite_watchdog_feed();
    gta7lite_usb_poll();
}

enum board_power board_power_poll(void) {
    return gta7lite_power_key_pressed() ? BOARD_POWER_PRESSED : BOARD_POWER_NONE;
}

void board_input(struct pointer_event *event) {
    if (gta7lite_touch_poll(event)) page_host_input(event);
    volume_poll();
    gta7lite_cpu_clock_poll(process_user_us());
}

void board_after_frame(void) { }

void board_screen_standby(bool on, struct pointer_event *cancel) {
    if (on) {
        *cancel = (struct pointer_event){.cancelled = true};
        display_blank();
        gta7lite_touch_standby(true);
        gta7lite_display_standby(true);
        return;
    }
    gta7lite_display_standby(false);
    gta7lite_touch_standby(false);
}

void board_system_standby(bool on) {
    if (on) {
        (void)gta7lite_cpu_clock(false);
        arm_smp_sleep();
        wifi_setup_standby(true);
        wifi_device_standby(true);
        return;
    }
    arm_smp_wake();
    wifi_device_standby(false);
    wifi_setup_standby(false);
}

void board_standby_step(struct pointer_event *scratch) { (void)scratch; }

void board_standby_sleep(void) {
    arm_smp_standby_poll();
    cpu_sleep_us(BOARD_STANDBY_POLL_US);
}
