#include "platform.h"
#include "fdt.h"
#include "display.h"
#include "board.h"
#include "kernel_board.h"
#include "process.h"
#include "page_host.h"
#include "battery.h"
#include "haptics.h"
static struct framebuffer screen;

/* Standby: a power-button tap turns the screen off, cancels input, hides and
 * freezes pages, and lets CPU 0 sleep between polls of the power key,
 * network and service worker; the next tap wakes the system with a full
 * redraw. What each board turns off and how deep it sleeps is its own
 * (kernel_board.h): on the Redmi 2, for one, the backlight, then the panel,
 * held in reset, and the touch controller go to sleep, Wi-Fi saves power in
 * its firmware, an idle GPU stops its clocks, the idle secondary CPUs and
 * then CPU 0 power down, the CPU cluster runs at 200 MHz and, once Wi-Fi's
 * radio is off and the power key's interrupt is armed, the whole chip sleeps
 * 10 s at a time (boards/redmi2/kernel_board.c); QEMU's screen goes black.
 * The battery gauge is not read meanwhile. */
static bool standby;

static void enter_standby(struct pointer_event *cancel) {
    standby = true;
    haptics_stop();
    page_host_screen_active(false);
    board_screen_standby(true, cancel);
    page_host_input(cancel);
    process_freeze(true);
    battery_standby(true);
    board_system_standby(true);
}

static void leave_standby(void) {
    standby = false;
    board_system_standby(false);
    battery_standby(false);
    process_freeze(false);
    page_host_screen_active(true);
    board_screen_standby(false, 0);
    page_host_redraw();
}

void kernel_main(const void *dtb) {
    /* Board RAM window: verify the maximum DTB span before parsing its header. */
    uintptr_t dtb_address = (uintptr_t)dtb;
    if ((dtb_address & 7) || (uint64_t)dtb_address - SOS2_RAM_START >
                                 (uint64_t)SOS2_RAM_END - SOS2_RAM_START - 2 * 1024 * 1024)
        panic("DTB outside board RAM window");
    static struct platform_devices devices;
    if (!fdt_discover(dtb, &devices)) panic("invalid device tree");
    board_handoff(&devices, dtb_address, &screen);
    board_before_pages(&devices);
    if (!gfx_framebuffer_valid(&screen)) panic("browser framebuffer invalid");
    page_host_init(&screen);
    board_pages_ready(&devices, &screen);
    haptics_init(); /* On the phone, through the power driver's PMIC access. */
    if (!process_init(&devices, dtb_address)) panic("protected process initialization failed");
    struct pointer_event event;
    if (board_processes_ready(&devices, &screen)) enter_standby(&event);
    for (;;) {
        loop_time_update();
        board_poll();
        (void)haptics_poll();
        enum board_power power = board_power_poll();
        if (power == BOARD_POWER_SCREEN_OFF || (power == BOARD_POWER_PRESSED && !standby))
            enter_standby(&event);
        else if (power == BOARD_POWER_SCREEN_ON || (power == BOARD_POWER_PRESSED && standby))
            leave_standby();
        if (standby) {
            board_standby_step(&event);
            process_step();
            display_poll();
            board_standby_sleep();
            continue;
        }
        process_turnaround();
        board_input(&event);
        page_host_render();
        process_turnaround();
        display_poll();
        board_after_frame();
        /* A tap's buzz ends here, not after process_step, whose service
         * worker can hold CPU 0 for tens of ms; meanwhile the display keeps
         * publishing the press. */
        while (haptics_poll()) display_poll();
        arch_yield();
        process_step();
    }
}
