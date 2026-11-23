#include "kernel_board.h"
#include "platform.h"
#include "display.h"
#include "page_host.h"
#include "boot_progress.h"
#include "arm_machine.h"
#include "nexus5.h"
#include "wifi_setup.h"
#include "wifi_device.h"
#include "process.h"

/* The Nexus 5's part of the kernel's start and main loop (kernel/main.c):
 * the stock bootloader's handoff, the panel kept fed with frames and the USB
 * link answering from the first frame (through the long boot work too), the
 * CPUs' faster clocks before it and their rise while pages work, and touch.
 * Its standby: touch asleep, the backlight off and the panel asleep, Wi-Fi's
 * radio down once quiet, CPU 0 and the L2 at 300 MHz. */
extern char __image_start[], __image_end[];

static const char *usb; /* why the USB link is unavailable, or NULL */

/* Long boot work keeps sending the panel what the screen holds, and keeps
 * the USB link answering. */
static void boot_frames(enum boot_phase phase, uint32_t done, uint32_t total) {
    (void)phase;
    (void)done;
    (void)total;
    nexus5_display_poll();
    nexus5_usb_poll();
}

void board_handoff(struct platform_devices *d, uintptr_t dtb_address, struct framebuffer *screen) {
    const char *error =
        nexus5_handoff(d, (uintptr_t)__image_start, (uintptr_t)__image_end, dtb_address);
    if (error) {
        if (d->has_framebuffer) {
            *screen = d->framebuffer;
            platform_set_framebuffer(screen);
        }
        panic(error);
    }
    boot_progress_set_handler(boot_frames);
    /* The faster clocks before the long work of expanding the resources. */
    (void)nexus5_cpu_clock_init();
    if (!display_init(d, screen)) panic("display initialization failed");
    platform_set_framebuffer(screen);
}

void board_before_pages(const struct platform_devices *d) {
    (void)d;
    /* The USB link, connected once polls run often (before process_init):
     * fastboot from the computer. */
    usb = nexus5_usb_init();
}

void board_pages_ready(struct platform_devices *d, struct framebuffer *screen) {
    (void)d;
    (void)screen;
    page_host_usb_status(usb);
    page_host_render();
    display_poll();
    barrier();
    nexus5_usb_connect();
}

bool board_processes_ready(const struct platform_devices *d, const struct framebuffer *screen) {
    (void)d;
    page_host_touch_status(nexus5_touch_init(screen->width, screen->height));
    /* The main loop polls the panel and the link from here on; allocations
     * then must not (boot_progress.h). */
    boot_progress_set_handler(0);
    return false;
}

void board_poll(void) { nexus5_usb_poll(); }

enum board_power board_power_poll(void) {
    return nexus5_power_key_pressed() ? BOARD_POWER_PRESSED : BOARD_POWER_NONE;
}

void board_input(struct pointer_event *event) {
    if (nexus5_touch_poll(event)) page_host_input(event);
    nexus5_cpu_clock_poll(process_user_us());
}

void board_after_frame(void) { }

void board_screen_standby(bool on, struct pointer_event *cancel) {
    if (on) {
        *cancel = (struct pointer_event){.cancelled = true};
        display_blank();
        nexus5_touch_standby(true);
        (void)nexus5_backlight(false);
        nexus5_display_standby(true);
        return;
    }
    nexus5_display_standby(false);
    (void)nexus5_backlight(true);
    nexus5_touch_standby(false);
}

void board_system_standby(bool on) {
    if (on) {
        wifi_setup_standby(true);
        wifi_device_standby(true);
        nexus5_cpu_clock_standby(true);
        return;
    }
    nexus5_cpu_clock_standby(false);
    wifi_device_standby(false);
    wifi_setup_standby(false);
}

void board_standby_step(struct pointer_event *scratch) { (void)scratch; }

void board_standby_sleep(void) { arm_cpu_sleep_us(BOARD_STANDBY_POLL_US); }
