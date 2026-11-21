#include "nexus5.h"
#include "boot_handoff.h"
#include "platform.h"
#include "user_abi.h"

/* The stock bootloader's handoff: the kernel at 0x8000, the device tree
 * (boards/nexus5/boot.dts, completed with RAM, the command line and the
 * ramdisk) and the splash, which becomes the screen. Every range must lie in
 * reported RAM, and whatever the kernel maps for itself below ARM32 user
 * space (include/user_abi.h). */
enum { KERNEL = 0x8000 };

void nexus5_store(uintptr_t address, uint32_t value) {
    mmio_write32(address, value);
    barrier();
}

bool nexus5_wait_bits(uintptr_t address, uint32_t bits, bool set, uint64_t us) {
    for (uint64_t start = time_us(); !(mmio_read32(address) & bits) == set;)
        if (time_us() - start >= us) return false;
    return true;
}

static const struct platform_devices *handed;

const struct platform_devices *nexus5_devices(void) { return handed; }

const char *nexus5_handoff(struct platform_devices *d, uint64_t kernel_start, uint64_t kernel_end,
                           uint64_t dtb_address) {
    /* Only the validated splash can be the screen, including for errors. */
    d->has_framebuffer = false;
    d->framebuffer = (struct framebuffer){0};
    d->framebuffer_size = 0;
    if (!d->nexus5_hammerhead) return "device tree is not the Nexus 5's";
    handed = d;
    uint32_t fb;
    const char *error = nexus5_display_handoff(&fb);
    if (error) return error;
    if (!fb || (fb & 4095)) return "splash not page-aligned";
    const struct boot_handoff h = {
        .kernel_start = kernel_start,
        .kernel_end = kernel_end,
        .kernel_at = KERNEL,
        .dtb = dtb_address,
        .fb = fb,
        .fb_size = ((uint64_t)NEXUS5_STRIDE * NEXUS5_HEIGHT + 4095) & ~4095ull,
        .fb_limit = USER_BASE, /* mapped where the kernel maps itself, below user space */
        .fb_reserved = true};
    error = boot_handoff_screen(d, &h);
    if (error) return error;
    d->framebuffer = (struct framebuffer){(void *)(uintptr_t)fb, NEXUS5_WIDTH, NEXUS5_HEIGHT,
                                          NEXUS5_STRIDE, PIXEL_RGB888};
    d->framebuffer_size = h.fb_size;
    d->has_framebuffer = true;
    return boot_handoff_memory(d, &h);
}
