#ifndef SOS2_KERNEL_BOARD_H
#define SOS2_KERNEL_BOARD_H
#include <stdbool.h>
#include <stdint.h>
#include "fdt.h"
#include "graphics.h"
#include "input.h"

/* The board's part of the kernel's start and main loop (kernel/main.c):
 * each device's in its kernel_board.c (boards/redmi2, boards/gta7lite,
 * boards/nexus5), QEMU's machines' in kernel/qemu_board.c. */

/* The bootloader's handoff checked and the screen taken: the framebuffer the
 * kernel draws into, given to platform_set_framebuffer for panics. Panics
 * when it cannot be. */
void board_handoff(struct platform_devices *, uintptr_t dtb, struct framebuffer *screen);
/* Before the desktop's pages start (page_host_init), then once they have:
 * the board's devices up to the first frame. */
void board_before_pages(const struct platform_devices *);
void board_pages_ready(struct platform_devices *, struct framebuffer *screen);
/* After the protected processes start: what needs runtime files or device
 * mappings. True when the system starts in standby. */
bool board_processes_ready(const struct platform_devices *, const struct framebuffer *screen);

/* Each pass of the main loop: the board's own polls first, then the power
 * key's news. */
enum board_power {
    BOARD_POWER_NONE,
    BOARD_POWER_PRESSED,    /* standby on or off */
    BOARD_POWER_SCREEN_OFF, /* the board's power driver says which */
    BOARD_POWER_SCREEN_ON
};

void board_poll(void);
enum board_power board_power_poll(void);
/* Awake: input to the desktop's pages; after each frame. */
void board_input(struct pointer_event *event);
void board_after_frame(void);

/* Standby's board steps, entered screen first (the pointer cancelled into
 * cancel), then the system once pages are frozen; left in the reverse
 * order. In standby, each pass: before the service step, and its sleep. */
void board_screen_standby(bool on, struct pointer_event *cancel);
void board_system_standby(bool on);
void board_standby_step(struct pointer_event *scratch);
void board_standby_sleep(void);

/* The main loop's sleep in standby between polls, where the board has
 * nothing longer. */
enum { BOARD_STANDBY_POLL_US = 20000 };
#endif
