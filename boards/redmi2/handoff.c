#include "handoff.h"

static bool overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return a < b + m && b < a + n;
}

const char *redmi2_validate_handoff(const struct platform_devices *d, uint64_t kernel_start,
                                    uint64_t kernel_end, uint64_t dtb_address) {
    if (!d->redmi2_wt88047) return "device tree is not Redmi 2 wt88047";
    const uint64_t expected_start = 0x80008000;
    if (kernel_start != expected_start || kernel_end <= kernel_start ||
        !fdt_range_in_ram(d, kernel_start, kernel_end - kernel_start))
        return "kernel outside reported RAM";
    if (!fdt_range_in_ram(d, dtb_address, d->dtb_size)) return "device tree outside reported RAM";
    if (overlaps(kernel_start, kernel_end - kernel_start, dtb_address, d->dtb_size))
        return "device tree overlaps kernel";
    for (size_t i = 0; i < d->reserved_region_count; ++i) {
        const struct memory_region *r = &d->reserved_regions[i];
        if (r->size > UINT64_MAX - r->start) return "invalid reserved memory";
        if (overlaps(kernel_start, kernel_end - kernel_start, r->start, r->size))
            return "kernel overlaps reserved memory";
    }
    if (!d->has_framebuffer) return "lk2nd framebuffer handoff missing";
    uint64_t fb = (uintptr_t)d->framebuffer.pixels;
    if (!fdt_range_in_ram(d, fb, d->framebuffer_size)) return "framebuffer outside reported RAM";
    if (overlaps(fb, d->framebuffer_size, kernel_start, kernel_end - kernel_start) ||
        overlaps(fb, d->framebuffer_size, dtb_address, d->dtb_size))
        return "framebuffer overlaps boot data";
    /* Reject a bad ramdisk before the first paint into inherited display
     * memory. Full resource validation/expansion happens later in process_init. */
    if (d->has_initrd_start || d->has_initrd_end) {
        if (!d->has_initrd_start || !d->has_initrd_end || d->initrd_end <= d->initrd_start ||
            !fdt_range_in_ram(d, d->initrd_start, d->initrd_end - d->initrd_start))
            return "invalid ramdisk handoff";
        uint64_t size = d->initrd_end - d->initrd_start;
        if (overlaps(d->initrd_start, size, fb, d->framebuffer_size) ||
            overlaps(d->initrd_start, size, kernel_start, kernel_end - kernel_start) ||
            overlaps(d->initrd_start, size, dtb_address, d->dtb_size))
            return "ramdisk overlaps boot data or framebuffer";
    }
    bool reserved = false;
    for (size_t i = 0; i < d->reserved_region_count; ++i) {
        const struct memory_region *r = &d->reserved_regions[i];
        if (r->start <= fb && fb + d->framebuffer_size <= r->start + r->size) reserved = true;
    }
    if (!reserved) return "framebuffer has no memory reservation";
    return 0;
}
