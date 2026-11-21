#include "handoff.h"
#include "boot_handoff.h"

/* lk2nd's handoff: the Redmi 2's own device tree, the kernel at 0x80008000
 * and lk2nd's simple-framebuffer, inside a memory reservation
 * (include/boot_handoff.h). */
const char *redmi2_validate_handoff(const struct platform_devices *d, uint64_t kernel_start,
                                    uint64_t kernel_end, uint64_t dtb_address) {
    if (!d->redmi2_wt88047) return "device tree is not Redmi 2 wt88047";
    if (!d->has_framebuffer) return "lk2nd framebuffer handoff missing";
    const struct boot_handoff h = {.kernel_start = kernel_start,
                                   .kernel_end = kernel_end,
                                   .kernel_at = 0x80008000,
                                   .dtb = dtb_address,
                                   .fb = (uintptr_t)d->framebuffer.pixels,
                                   .fb_size = d->framebuffer_size,
                                   .fb_limit = UINT64_MAX,
                                   .fb_reserved = true};
    const char *error = boot_handoff_screen(d, &h);
    return error ? error : boot_handoff_memory(d, &h);
}
