#ifndef SOS2_BOOT_HANDOFF_H
#define SOS2_BOOT_HANDOFF_H
#include "fdt.h"

/* The checks of a bootloader's handoff that the devices share
 * (kernel/boot_handoff.c), around each board's own (boards/redmi2/handoff.c,
 * boards/gta7lite/board.c, boards/nexus5/board.c): the kernel, at the
 * address the loader must use (kernel_at, 0 for any), and its device tree
 * in reported RAM and apart; the screen's framebuffer in RAM below fb_limit,
 * clear of both, and inside a memory reservation when fb_reserved; the
 * kernel clear of reserved memory; the ramdisk, if any, whole, in RAM and
 * clear of the kernel, the device tree and the framebuffer. In two steps, so
 * that a board can show later errors on the framebuffer once the first has
 * placed it. Each returns NULL, or why not; none reads a register or
 * writes. */
struct boot_handoff {
    uint64_t kernel_start, kernel_end, kernel_at, dtb;
    uint64_t fb, fb_size, fb_limit;
    bool fb_reserved;
};

const char *boot_handoff_screen(const struct platform_devices *, const struct boot_handoff *);
const char *boot_handoff_memory(const struct platform_devices *, const struct boot_handoff *);
#endif
