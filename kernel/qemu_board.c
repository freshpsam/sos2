#include "kernel_board.h"
#include "platform.h"
#include "display.h"
#include "input.h"
#include "page_host.h"
#include "arm_machine.h"

/* QEMU's machines' part of the kernel's start and main loop (kernel/main.c):
 * the loader's kernel and device tree checked, virtio's display, tablet,
 * keyboard and power key (a PL061 GPIO). Its standby only turns the screen
 * black. */
extern char __image_start[], __image_end[];

void board_handoff(struct platform_devices *d, uintptr_t dtb_address, struct framebuffer *screen) {
    uintptr_t start = (uintptr_t)__image_start, end = (uintptr_t)__image_end;
    if (end <= start || !fdt_range_in_ram(d, start, end - start))
        panic("kernel static memory outside RAM");
    if (dtb_address < end && dtb_address + d->dtb_size > start) panic("DTB overlaps kernel memory");
    if (!display_init(d, screen)) panic("display initialization failed");
    platform_set_framebuffer(screen);
}

void board_before_pages(const struct platform_devices *d) { (void)d; }

void board_pages_ready(struct platform_devices *d, struct framebuffer *screen) {
    page_host_render();
    display_poll();
    barrier();
    if (!input_init(d->virtio, d->virtio_count, screen->width, screen->height))
        panic("input initialization failed");
    (void)keyboard_init(d->virtio, d->virtio_count);
    if (d->power_key) (void)power_key_init(d->gpio, d->power_key_pin);
}

bool board_processes_ready(const struct platform_devices *d, const struct framebuffer *screen) {
    (void)d;
    (void)screen;
    return false;
}

void board_poll(void) { }

enum board_power board_power_poll(void) {
    return power_key_pressed() ? BOARD_POWER_PRESSED : BOARD_POWER_NONE;
}

void board_input(struct pointer_event *event) {
    /* Drain queued virtio tablet reports. Taking one per pass lets the
     * 64-event ring fill while the loop is slow, and the device then drops
     * whole reports, including button releases. page_host merges pure
     * motion. */
    for (unsigned budget = 0; budget < 16; ++budget) {
        int32_t wheel_x = 0, wheel_y = 0;
        if (!input_poll_extended(event, &wheel_x, &wheel_y)) break;
        page_host_input(event);
        page_host_wheel(event, wheel_x, wheel_y);
        if (event->cancelled) break;
    }
    /* Dispatch pointer releases before keyboard events in the same poll. */
    struct user_input_event key;
    for (unsigned budget = 0; budget < 32 && keyboard_poll(&key); ++budget)
        page_host_keyboard(&key);
}

void board_after_frame(void) { }

void board_screen_standby(bool on, struct pointer_event *cancel) {
    if (!on) return;
    *cancel = (struct pointer_event){.cancelled = true};
    display_blank();
}

void board_system_standby(bool on) { (void)on; }

void board_standby_step(struct pointer_event *scratch) {
    /* Drop input queued while the screen is off; nothing acts on it. */
    int32_t wheel_x, wheel_y;
    struct user_input_event key;
    for (unsigned budget = 0; budget < 16 && input_poll_extended(scratch, &wheel_x, &wheel_y);
         ++budget) { }
    for (unsigned budget = 0; budget < 32 && keyboard_poll(&key); ++budget) { }
}

void board_standby_sleep(void) { arm_cpu_sleep_us(BOARD_STANDBY_POLL_US); }
