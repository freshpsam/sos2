#include "arm_cache.h"
#include "platform.h"

#ifdef SOS2_PHONE
/* Only the decoder uses these tables (and, on the Nexus 5, the start up to
 * it). Afterwards vm_init installs the normal restrictive mappings. RAM is
 * mapped at its physical address, with no alias. Default sections retain
 * Device attributes for early touch/PMIC MMIO. No peripheral performs DMA
 * into cached RAM during this stage.
 * On the Nexus 5 the tables first hold the kernel alone, from its entry
 * (boot.S, arm_early_mmu): with translation off nothing is execute-never,
 * and Krait's speculative instruction fetches reached memory its TrustZone
 * protects, which resets the phone, so its instruction cache and branch
 * prediction stay off until translation is on. Behind these tables only the
 * kernel's text is executable and everything else is Device memory, which
 * is never accessed speculatively; the data cache stays off, as it is with
 * translation off. The decoder then adds its ranges to the same tables. */
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
#ifdef SOS2_BOARD_NEXUS5
static bool early; /* the tables in use since the entry (arm_early_mmu) */
#else
enum { early = 0 };
#endif
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

/* Every section Device and execute-never, then the kernel: its text
 * executable, its read-only data and the rest (data, BSS, stacks) not. */
static bool kernel_tables(void) {
    used = 0;
    for (unsigned i = 0; i < 4096; ++i)
        root[i] = (i << 20) | 2 | DEVICE | (1 << 4) | (1 << 10) | (1 << 16);
    return map((uintptr_t)__image_start, __text_end - __image_start, CACHED | RO) &&
           map((uintptr_t)__text_end, __rodata_end - __text_end, CACHED | RO | XN) &&
           map((uintptr_t)__rodata_end, __image_end - __rodata_end, CACHED | RW | XN);
}

/* The tables in use (client domain 0, TTBR0 alone), the TLB emptied. */
static void install(void) {
    uint32_t zero = 0, domain = 1, ttbr = (uintptr_t)root;
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2\n mcr p15, 0, %1, c3, c0, 0\n"
                     "mcr p15, 0, %2, c2, c0, 0\n mcr p15, 0, %0, c8, c7, 0\n dsb sy\n isb"
                     :
                     : "r"(zero), "r"(domain), "r"(ttbr)
                     : "memory");
}

/* SCTLR with translation, the instruction cache and branch prediction on,
 * TEX remap and the access flag off, XP and WXN on, and caches as asked. */
static uint32_t translated(uint32_t control, bool data_cache) {
    return (control & ~((1u << 29) | (1u << 28))) | (1u << 23) | (1u << 19) | (1u << 12) |
           (1u << 11) | 1 | (data_cache ? 4 : 0);
}

#ifdef SOS2_BOARD_NEXUS5
void arm_early_mmu(void) {
    uint32_t control = arm_cache_control();
    if (early || (control & 5) || !kernel_tables()) return; /* left as boot.S set it */
    arm_instruction_sync();
    install();
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0\n isb"
                     :
                     : "r"(translated(control, false))
                     : "memory");
    early = true;
}

bool arm_boot_cache_early(void) { return early; }
#endif

bool arm_boot_cache_begin(const struct platform_devices *d, uintptr_t dtb, uintptr_t source,
                          size_t source_size, uintptr_t output, size_t output_size) {
    saved_control = arm_cache_control();
    /* Translation already on only behind the early tables, which stay in
     * use: the kernel's own entries are kept, the decoder's ranges added. */
    if (enabled || (saved_control & 4) || ((saved_control & 1) && !early)) return false;
    if (!early && !kernel_tables()) return false;
    if (!map(dtb, d->dtb_size, CACHED | RO | XN) || !map(source, source_size, CACHED | RO | XN) ||
        !map(output, output_size, CACHED | RW | XN) ||
        !map((uintptr_t)d->framebuffer.pixels, d->framebuffer_size, NORMAL | RW | XN))
        return false;
    arm_dcache_invalidate_all();
    arm_instruction_sync();
    install();
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0\n isb"
                     :
                     : "r"(translated(saved_control, true))
                     : "memory");
    enabled = true;
    return true;
}

void arm_boot_cache_end(void) {
    if (!enabled) panic("boot cache not active");
    enabled = false;
    arm_cache_restore(saved_control);
}
#endif
