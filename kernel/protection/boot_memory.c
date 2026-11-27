#include "boot_memory.h"
#include "board.h"
#include "page_alloc.h"

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
#ifdef __LP64__
    /* Up to 4 GiB inclusive: the Galaxy Tab A7 Lite's RAM ends there. */
    const uint64_t limit = 1ull << 32;
    if (!size || start & 4095 || start >= limit || size > limit - start || ke <= ks ||
        !fdt_range_in_ram(d, start, size))
        return false;
    uint64_t end = (start + size + 4095) & ~4095ull;
    if (end > limit || !fdt_range_in_ram(d, start, end - start)) return false;
#else
    if (!size || start & 4095 || start > UINT32_MAX || size > UINT32_MAX - start || ke <= ks ||
        !fdt_range_in_ram(d, start, size))
        return false;
    uint64_t end = (start + size + 4095) & ~4095ull;
    if (end > UINT32_MAX || !fdt_range_in_ram(d, start, end - start)) return false;
#endif
    for (unsigned i = 0; i < d->reserved_region_count + 3 + (unsigned)avoid_initrd; ++i) {
        struct memory_region r = obstacle(d, i, dtb, ks, ke);
        if (r.size > UINT64_MAX - r.start || (r.size && start < r.start + r.size && r.start < end))
            return false;
    }
    return true;
}

#ifdef SOS2_PHONE
/* The largest free stretch of the arena window (include/board.h), at least
 * minimum and at most maximum bytes. */
static bool arena(const struct platform_devices *d, uintptr_t dtb, uintptr_t ks, uintptr_t ke,
                  uint64_t minimum, uint64_t maximum, uintptr_t *out, size_t *size) {
    const uint64_t floor = SOS2_ARENA_START, ceiling = SOS2_ARENA_END;
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

bool phone_browser_arena(const struct platform_devices *d, uintptr_t dtb, uintptr_t ks,
                         uintptr_t ke, uintptr_t *out, size_t *size) {
    return arena(d, dtb, ks, ke, 256ull << 20, (uint64_t)PAGE_POOL_PAGES * PAGE_SIZE, out, size);
}

bool phone_resource_arena(const struct platform_devices *d, uintptr_t dtb, uintptr_t ks,
                          uintptr_t ke, size_t size, uintptr_t *out) {
    size_t ignored;
    /* On a megabyte, for the engine's sections (kernel/protection/elf_load.c). */
    uint64_t room = (((uint64_t)size + 4095) & ~4095ull) + 0x100000 - 4096;
    uintptr_t start;
    if (!size || !arena(d, dtb, ks, ke, room, room, &start, &ignored)) return false;
    *out = (start + 0xfffffu) & ~(uintptr_t)0xfffffu;
    return true;
}
#endif

#ifdef __LP64__
/* Calls visit for each free stretch of RAM below 4 GiB, clear of the
 * firmware's reservations, the kernel, the DTB, the framebuffer, the initrd
 * and `extra`; false on an impossible region. */
static bool free_stretches(const struct platform_devices *d, uintptr_t dtb, uintptr_t ks,
                           uintptr_t ke, struct memory_region extra,
                           void (*visit)(void *, uint64_t start, uint64_t length), void *context) {
    unsigned obstacles = (unsigned)d->reserved_region_count + 5;
    for (unsigned n = 0; n < d->ram_region_count; ++n) {
        struct memory_region ram = d->ram_regions[n];
        if (ram.size > UINT64_MAX - ram.start) return false;
        uint64_t end = ram.start + ram.size;
        if (end > 1ull << 32) end = 1ull << 32;
        uint64_t at = (ram.start + 4095) & ~4095ull;
        while (at < end) {
            uint64_t gap_end = end, next = 0;
            for (unsigned i = 0; i < obstacles; ++i) {
                struct memory_region r = i + 1 == obstacles ? extra : obstacle(d, i, dtb, ks, ke);
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
            if (length) visit(context, at, length);
            if (gap_end == end) break;
            at = gap_end & ~4095ull;
            if (at < gap_end) at += 4096;
        }
    }
    return true;
}

struct pool_search {
    struct memory_region *regions;
    unsigned count;
};

/* Keeps the BOOT_POOL_REGIONS largest stretches of at least 1 MiB. */
static void pool_visit(void *context, uint64_t start, uint64_t length) {
    struct pool_search *s = context;
    if (length < (1u << 20)) return;
    unsigned at = s->count;
    if (at == BOOT_POOL_REGIONS) {
        at = 0;
        for (unsigned i = 1; i < s->count; ++i)
            if (s->regions[i].size < s->regions[at].size) at = i;
        if (s->regions[at].size >= length) return;
    } else {
        ++s->count;
    }
    s->regions[at] = (struct memory_region){start, length};
}

unsigned boot_memory_pool(const struct platform_devices *d, uintptr_t dtb, uintptr_t ks,
                          uintptr_t ke, uintptr_t resources, size_t resource_size, uint64_t span,
                          struct memory_region regions[BOOT_POOL_REGIONS]) {
    struct pool_search search = {regions, 0};
    if (!free_stretches(d, dtb, ks, ke, (struct memory_region){resources, resource_size},
                        pool_visit, &search) ||
        !search.count)
        return 0;
    /* In address order, then within `span` bytes from the first's start to
     * the last's end, dropping the smaller stretch at either end first. */
    for (unsigned i = 1; i < search.count; ++i)
        for (unsigned j = i; j && regions[j - 1].start > regions[j].start; --j) {
            struct memory_region swap = regions[j];
            regions[j] = regions[j - 1];
            regions[j - 1] = swap;
        }
    unsigned first = 0, count = search.count;
    while (regions[first + count - 1].start + regions[first + count - 1].size -
               regions[first].start >
           span) {
        if (count == 1) {
            regions[first].size = span;
            break;
        }
        if (regions[first].size < regions[first + count - 1].size) ++first;
        --count;
    }
    for (unsigned i = 0; i < count; ++i) regions[i] = regions[first + i];
    return count;
}

struct fit_search {
    uint64_t fit, best, best_size;
};

static void fit_visit(void *context, uint64_t start, uint64_t length) {
    struct fit_search *s = context;
    if (length >= s->fit && (!s->best_size || length < s->best_size)) {
        s->best = start;
        s->best_size = length;
    }
}

bool boot_memory_resources(const struct platform_devices *d, uintptr_t dtb, uintptr_t ks,
                           uintptr_t ke, size_t size, uintptr_t *out) {
    uint64_t pages = ((uint64_t)size + 4095) & ~4095ull;
    struct fit_search search = {pages, 0, 0};
    if (!size ||
        !free_stretches(d, dtb, ks, ke, (struct memory_region){0, 0}, fit_visit, &search) ||
        !search.best_size || !boot_memory_available(d, search.best, pages, dtb, ks, ke, true))
        return false;
    *out = (uintptr_t)search.best;
    return true;
}
#endif
