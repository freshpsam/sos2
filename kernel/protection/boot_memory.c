#include "boot_memory.h"

static struct memory_region obstacle(const struct platform_devices *d, unsigned i, uintptr_t dtb,
                                     uintptr_t ks, uintptr_t ke) {
    if (i < d->reserved_region_count) return d->reserved_regions[i];
    switch (i - d->reserved_region_count) {
        case 0: return (struct memory_region){ks, ke - ks};
        case 1: return (struct memory_region){dtb, d->dtb_size};
        case 2:
            return (struct memory_region){(uintptr_t)d->framebuffer.pixels, d->framebuffer_size};
        default: return (struct memory_region){d->initrd_start, d->initrd_end - d->initrd_start};
    }
}

bool boot_memory_available(const struct platform_devices *d, uint64_t start, uint64_t size,
                           uintptr_t dtb, uintptr_t ks, uintptr_t ke, bool avoid_initrd) {
    if (!size || start & 4095 || start > UINT32_MAX || size > UINT32_MAX - start || ke <= ks ||
        !fdt_range_in_ram(d, start, size))
        return false;
    uint64_t end = (start + size + 4095) & ~4095ull;
    if (end > UINT32_MAX || !fdt_range_in_ram(d, start, end - start)) return false;
    for (unsigned i = 0; i < d->reserved_region_count + 3 + (unsigned)avoid_initrd; ++i) {
        struct memory_region r = obstacle(d, i, dtb, ks, ke);
        if (r.size > UINT64_MAX - r.start || (r.size && start < r.start + r.size && r.start < end))
            return false;
    }
    return true;
}

static bool arena(const struct platform_devices *d, uintptr_t dtb, uintptr_t ks, uintptr_t ke,
                  uint64_t minimum, uint64_t maximum, uintptr_t *out, size_t *size) {
    /* Stay above the MSM8916 firmware allocation window (TZ/SMEM/modem/
     * WCNSS/venus/MBA), even if the minimal handoff omits its reservations.
     * Also respect every reservation supplied by firmware and the live FB.
     * Source: upstream qcom/msm8916.dtsi reserved-memory + alloc-ranges. */
    const uint64_t floor = 0x90000000, ceiling = 0xc0000000;
    uint64_t best = 0, best_size = 0;
    if (!d->has_initrd_start || !d->has_initrd_end || d->initrd_end <= d->initrd_start)
        return false;
    for (unsigned n = 0; n < d->ram_region_count; ++n) {
        struct memory_region ram = d->ram_regions[n];
        if (ram.size > UINT64_MAX - ram.start) return false;
        uint64_t end = ram.start + ram.size;
        if (end > ceiling) end = ceiling;
        uint64_t at = ram.start > floor ? ram.start : floor;
        at = (at + 4095) & ~4095ull;
        while (at < end) {
            uint64_t gap_end = end, next = 0;
            for (unsigned i = 0; i < d->reserved_region_count + 4; ++i) {
                struct memory_region r = obstacle(d, i, dtb, ks, ke);
                if (r.size > UINT64_MAX - r.start) return false;
                if (!r.size || r.start + r.size <= at || r.start >= end) continue;
                if (r.start <= at) {
                    next = r.start + r.size >= end ? end : (r.start + r.size + 4095) & ~4095ull;
                    break;
                }
                if (r.start < gap_end) gap_end = r.start;
            }
            if (next) {
                at = next;
                continue;
            }
            uint64_t length = (gap_end & ~4095ull) - at;
            if (length > maximum) length = maximum;
            if (length >= minimum && length > best_size) {
                best = at;
                best_size = length;
            }
            if (gap_end == end) break;
            at = gap_end & ~4095ull;
            /* Include the page containing an unaligned reserved boundary. */
            if (at < gap_end) at += 4096;
        }
    }
    if (!best_size || !boot_memory_available(d, best, best_size, dtb, ks, ke, true)) return false;
    *out = (uintptr_t)best;
    *size = (size_t)best_size;
    return true;
}

bool redmi2_browser_arena(const struct platform_devices *d, uintptr_t dtb, uintptr_t ks,
                          uintptr_t ke, uintptr_t *out, size_t *size) {
    return arena(d, dtb, ks, ke, 256ull << 20, 384ull << 20, out, size);
}

bool redmi2_resource_arena(const struct platform_devices *d, uintptr_t dtb, uintptr_t ks,
                           uintptr_t ke, size_t size, uintptr_t *out) {
    size_t ignored;
    uint64_t aligned = ((uint64_t)size + 4095) & ~4095ull;
    return size && arena(d, dtb, ks, ke, aligned, aligned, out, &ignored);
}
