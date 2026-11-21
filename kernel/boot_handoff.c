#include "boot_handoff.h"

#ifdef SOS2_DEVICE /* QEMU's machines check their own boot (kernel/main.c) */
static bool overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return a < b + m && b < a + n;
}

const char *boot_handoff_screen(const struct platform_devices *d, const struct boot_handoff *h) {
    if (!fdt_range_in_ram(d, h->fb, h->fb_size) || h->fb + h->fb_size > h->fb_limit)
        return "framebuffer outside reported RAM";
    bool reserved = !h->fb_reserved;
    for (size_t i = 0; i < d->reserved_region_count && !reserved; ++i) {
        const struct memory_region *r = &d->reserved_regions[i];
        reserved = r->start <= h->fb && h->fb + h->fb_size <= r->start + r->size;
    }
    if (!reserved) return "framebuffer has no memory reservation";
    if ((h->kernel_at && h->kernel_start != h->kernel_at) || h->kernel_end <= h->kernel_start ||
        !fdt_range_in_ram(d, h->kernel_start, h->kernel_end - h->kernel_start))
        return "kernel outside reported RAM";
    if (!fdt_range_in_ram(d, h->dtb, d->dtb_size)) return "device tree outside reported RAM";
    if (overlaps(h->fb, h->fb_size, h->kernel_start, h->kernel_end - h->kernel_start) ||
        overlaps(h->fb, h->fb_size, h->dtb, d->dtb_size))
        return "framebuffer overlaps boot data";
    return 0;
}

const char *boot_handoff_memory(const struct platform_devices *d, const struct boot_handoff *h) {
    uint64_t kernel_size = h->kernel_end - h->kernel_start;
    if (overlaps(h->kernel_start, kernel_size, h->dtb, d->dtb_size))
        return "device tree overlaps kernel";
    for (size_t i = 0; i < d->reserved_region_count; ++i) {
        const struct memory_region *r = &d->reserved_regions[i];
        if (r->size > UINT64_MAX - r->start) return "invalid reserved memory";
        if (overlaps(h->kernel_start, kernel_size, r->start, r->size))
            return "kernel overlaps reserved memory";
    }
    /* A bad ramdisk is turned away before the first paint into the
     * framebuffer; the resources themselves are checked as they expand
     * (kernel/protection/boot_bundle.c). */
    if (d->has_initrd_start || d->has_initrd_end) {
        if (!d->has_initrd_start || !d->has_initrd_end || d->initrd_end <= d->initrd_start ||
            !fdt_range_in_ram(d, d->initrd_start, d->initrd_end - d->initrd_start))
            return "invalid ramdisk handoff";
        uint64_t size = d->initrd_end - d->initrd_start;
        if (overlaps(d->initrd_start, size, h->fb, h->fb_size) ||
            overlaps(d->initrd_start, size, h->kernel_start, kernel_size) ||
            overlaps(d->initrd_start, size, h->dtb, d->dtb_size))
            return "ramdisk overlaps boot data or the framebuffer";
    }
    return 0;
}
#endif
