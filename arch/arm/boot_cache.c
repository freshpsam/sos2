#include "arm_cache.h"
#include "platform.h"

#ifdef SOS2_BOARD_REDMI2
/* Only the decoder uses these tables. Afterwards vm_init installs the normal
 * restrictive mappings. RAM is mapped at its physical address, with no alias.
 * Default sections retain Device attributes for early touch/PMIC MMIO.
 * No peripheral performs DMA into cached RAM during this stage.
 */
enum {
    SMALL = 2,
    XN = 1,
    NORMAL = 1 << 6,
    CACHED = (1 << 6) | (3 << 2),
    SHARED = 1 << 10,
    RW = 1 << 4,
    RO = (1 << 9) | (1 << 4),
    DEVICE = 1 << 2,
    TABLES = 384
};

static uint32_t root[4096] __attribute__((aligned(16384)));
static uint32_t tables[TABLES][256] __attribute__((aligned(1024)));
static unsigned used;
static uint32_t saved_control;
static bool enabled;
extern char __image_start[], __image_end[], __text_end[], __rodata_end[];

static bool map(uintptr_t address, size_t size, uint32_t flags) {
    if (!size || address > UINT32_MAX - (size - 1)) return false;
    uintptr_t last = (address + size - 1) & ~4095u;
    for (uintptr_t at = address & ~4095u;; at += 4096) {
        unsigned index = at >> 20;
        if ((root[index] & 3) != 1) {
            if (used == TABLES) return false;
            uint32_t *table = tables[used++];
            for (unsigned i = 0; i < 256; ++i)
                table[i] = (index << 20) | (i << 12) | SMALL | SHARED | DEVICE | RW | XN;
            root[index] = (uintptr_t)table | 1;
        }
        uint32_t *table = (void *)(uintptr_t)(root[index] & ~1023u);
        table[(at >> 12) & 255] = (uint32_t)at | SMALL | SHARED | flags;
        if (at == last) return true;
    }
}

bool arm_boot_cache_begin(const struct platform_devices *d, uintptr_t dtb, uintptr_t source,
                          size_t source_size, uintptr_t output, size_t output_size) {
    saved_control = arm_cache_control();
    if (enabled || (saved_control & 5)) return false;
    used = 0;
    for (unsigned i = 0; i < 4096; ++i)
        root[i] = (i << 20) | 2 | DEVICE | (1 << 4) | (1 << 10) | (1 << 16);
    if (!map((uintptr_t)__image_start, __text_end - __image_start, CACHED | RO) ||
        !map((uintptr_t)__text_end, __rodata_end - __text_end, CACHED | RO | XN) ||
        !map((uintptr_t)__rodata_end, __image_end - __rodata_end, CACHED | RW | XN) ||
        !map(dtb, d->dtb_size, CACHED | RO | XN) || !map(source, source_size, CACHED | RO | XN) ||
        !map(output, output_size, CACHED | RW | XN) ||
        !map((uintptr_t)d->framebuffer.pixels, d->framebuffer_size, NORMAL | RW | XN))
        return false;
    arm_dcache_invalidate_all();
    arm_instruction_sync();
    uint32_t zero = 0, domain = 1, ttbr = (uintptr_t)root;
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2\n mcr p15, 0, %1, c3, c0, 0\n"
                     "mcr p15, 0, %2, c2, c0, 0\n mcr p15, 0, %0, c8, c7, 0\n dsb sy\n isb"
                     :
                     : "r"(zero), "r"(domain), "r"(ttbr)
                     : "memory");
    uint32_t control =
        (saved_control & ~((1u << 29) | (1u << 28))) | (1u << 23) | (1u << 19) | (1u << 12) | 5;
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0\n isb" : : "r"(control) : "memory");
    enabled = true;
    return true;
}

void arm_boot_cache_end(void) {
    if (!enabled) panic("boot cache not active");
    enabled = false;
    arm_cache_restore(saved_control);
}
#endif
