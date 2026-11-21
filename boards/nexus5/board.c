#include "nexus5.h"
#include "platform.h"
#include "user_abi.h"

/* The stock bootloader's handoff: the kernel at 0x8000, the device tree
 * (boards/nexus5/boot.dts, completed with RAM, the command line and the
 * ramdisk) and the splash, which becomes the screen. Every range must lie in
 * reported RAM, and whatever the kernel maps for itself below ARM32 user
 * space (include/user_abi.h). */
enum { WIDTH = 1080, HEIGHT = 1920, STRIDE = WIDTH * 3, KERNEL = 0x8000 };

static const struct platform_devices *handed;

const struct platform_devices *nexus5_devices(void) { return handed; }

static bool overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return a < b + m && b < a + n;
}

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
    uint64_t fb_size = ((uint64_t)STRIDE * HEIGHT + 4095) & ~4095ull;
    if (!fb || (fb & 4095) || !fdt_range_in_ram(d, fb, fb_size) || fb + fb_size > USER_BASE)
        return "splash outside low RAM";
    bool reserved = false;
    for (size_t i = 0; i < d->reserved_region_count; ++i) {
        const struct memory_region *r = &d->reserved_regions[i];
        if (r->start <= fb && fb + fb_size <= r->start + r->size) reserved = true;
    }
    if (!reserved) return "splash has no memory reservation";
    if (kernel_start != KERNEL || kernel_end <= kernel_start ||
        !fdt_range_in_ram(d, kernel_start, kernel_end - kernel_start))
        return "kernel outside reported RAM";
    if (!fdt_range_in_ram(d, dtb_address, d->dtb_size)) return "device tree outside reported RAM";
    if (overlaps(fb, fb_size, kernel_start, kernel_end - kernel_start) ||
        overlaps(fb, fb_size, dtb_address, d->dtb_size))
        return "splash overlaps boot data";
    d->framebuffer =
        (struct framebuffer){(void *)(uintptr_t)fb, WIDTH, HEIGHT, STRIDE, PIXEL_RGB888};
    d->framebuffer_size = fb_size;
    d->has_framebuffer = true;
    if (overlaps(kernel_start, kernel_end - kernel_start, dtb_address, d->dtb_size))
        return "device tree overlaps kernel";
    for (size_t i = 0; i < d->reserved_region_count; ++i) {
        const struct memory_region *r = &d->reserved_regions[i];
        if (r->size > UINT64_MAX - r->start) return "invalid reserved memory";
        if (overlaps(kernel_start, kernel_end - kernel_start, r->start, r->size))
            return "kernel overlaps reserved memory";
    }
    /* Reject a bad ramdisk before the first paint into the splash; the
     * resources are checked and expanded later (boot_bundle.c). */
    if (d->has_initrd_start || d->has_initrd_end) {
        if (!d->has_initrd_start || !d->has_initrd_end || d->initrd_end <= d->initrd_start ||
            !fdt_range_in_ram(d, d->initrd_start, d->initrd_end - d->initrd_start))
            return "invalid ramdisk handoff";
        uint64_t size = d->initrd_end - d->initrd_start;
        if (overlaps(d->initrd_start, size, fb, fb_size) ||
            overlaps(d->initrd_start, size, kernel_start, kernel_end - kernel_start) ||
            overlaps(d->initrd_start, size, dtb_address, d->dtb_size))
            return "ramdisk overlaps boot data or the splash";
    }
    return 0;
}
