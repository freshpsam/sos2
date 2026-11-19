#include "arm_cache.h"
#include "platform.h"

/* SORZ decoding (kernel/protection/boot_bundle.c) runs before vm_init, while
 * translation is off and every data access is uncached Device memory. These
 * temporary identity tables cache what the decoder touches, as
 * arch/arm/boot_cache.c does on ARM32; vm_init then builds the real ones. The
 * rest of the low 4 GiB stays Device memory for early MMIO and the inherited
 * scanout is Normal non-cacheable. Only the boot CPU runs, and no device
 * writes RAM meanwhile. Descriptors and memory types as arch/arm64/mmu.c:
 * MAIR_EL1 index 0 Device-nGnRE, 1 Normal non-cacheable, 2 Normal write-back.
 */
#define TABLE     3ull /* table (levels 1-2) or page (level 3) */
#define BLOCK     1ull /* 1 GiB (level 1) or 2 MiB (level 2) */
#define DEVICE    (0ull << 2)
#define NORMAL    (1ull << 2)
#define CACHED    (2ull << 2)
#define READ_ONLY (1ull << 7)
#define SHARED    (3ull << 8)
#define ACCESSED  (1ull << 10)
#define XN        ((1ull << 53) | (1ull << 54))
#define ADDRESS   0x0000fffffffff000ull
#define SPAN_2M   0x200000ull

/* A level 2 table per GiB below 4 GiB, and level 3 tables for each mapped
 * range's partial 2 MiB ends: two per range at most. */
enum { TABLES = 24 };

static uint64_t root[512] __attribute__((aligned(4096)));
static uint64_t tables[TABLES][512] __attribute__((aligned(4096)));
static unsigned used;
static uint32_t saved_control;
static bool enabled;
extern char __image_start[], __image_end[], __text_end[], __rodata_end[];

/* The table under entry. A block becomes one: 512 entries of `span` bytes
 * with the block's attributes, `kind` being a block or a page. */
static uint64_t *split(uint64_t *entry, uint64_t base, uint64_t span, uint64_t kind) {
    if ((*entry & 3) == TABLE) return (uint64_t *)(uintptr_t)(*entry & ADDRESS);
    if (used == TABLES) return 0;
    uint64_t *table = tables[used++], attributes = *entry & ~(ADDRESS | 3);
    for (unsigned i = 0; i < 512; ++i) table[i] = (base + i * span) | attributes | kind;
    *entry = (uintptr_t)table | TABLE;
    return table;
}

/* 2 MiB blocks where the range covers them, pages at its edges. A page shared
 * with an earlier range takes this range's attributes. */
static bool map(uint64_t start, uint64_t size, uint64_t flags) {
    const uint64_t limit = 1ull << 32;
    if (!size || start >= limit || size > limit - start) return false;
    uint64_t end = (start + size + 4095) & ~4095ull;
    for (uint64_t at = start & ~4095ull; at < end;) {
        uint64_t *level2 = split(&root[at >> 30], at & ~0x3fffffffull, SPAN_2M, BLOCK);
        if (!level2) return false;
        uint64_t *entry = &level2[(at >> 21) & 511];
        if (!(at & (SPAN_2M - 1)) && end - at >= SPAN_2M && (*entry & 3) != TABLE) {
            *entry = at | BLOCK | SHARED | ACCESSED | flags;
            at += SPAN_2M;
            continue;
        }
        uint64_t *level3 = split(entry, at & ~(SPAN_2M - 1), 4096, TABLE);
        if (!level3) return false;
        level3[(at >> 12) & 511] = at | TABLE | SHARED | ACCESSED | flags;
        at += 4096;
    }
    return true;
}

bool arm_boot_cache_begin(const struct platform_devices *d, uintptr_t dtb, uintptr_t source,
                          size_t source_size, uintptr_t output, size_t output_size) {
    uint64_t control = arm_cache_control();
    if (enabled || (control & 5)) return false;
    saved_control = (uint32_t)control;
    used = 0;
    for (unsigned i = 0; i < 512; ++i)
        root[i] = i < 4 ? (uint64_t)i << 30 | BLOCK | DEVICE | ACCESSED | XN : 0;
    if (!map((uintptr_t)__image_start, (uintptr_t)(__text_end - __image_start),
             CACHED | READ_ONLY) ||
        !map((uintptr_t)__text_end, (uintptr_t)(__rodata_end - __text_end),
             CACHED | READ_ONLY | XN) ||
        !map((uintptr_t)__rodata_end, (uintptr_t)(__image_end - __rodata_end), CACHED | XN) ||
        !map(dtb, d->dtb_size, CACHED | READ_ONLY | XN) ||
        !map(source, source_size, CACHED | READ_ONLY | XN) ||
        !map(output, output_size, CACHED | XN) ||
        (d->framebuffer_size &&
         !map((uintptr_t)d->framebuffer.pixels, d->framebuffer_size, NORMAL | XN)))
        return false;
    arm_dcache_invalidate_all();
    arm_instruction_sync();
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    /* As enable_translation: T0SZ 25, non-cacheable inner shareable walks,
     * 4 KiB granule, no TTBR1 walks. */
    uint64_t tcr = 25 | (3u << 12) | (1u << 23) | ((mmfr0 & 7) << 32);
    uint64_t mair = 0x04 | (0x44 << 8) | (0xff << 16);
    __asm__ volatile("msr mair_el1, %0\n msr tcr_el1, %1\n msr ttbr0_el1, %2\n isb\n"
                     "tlbi vmalle1\n dsb sy\n isb"
                     :
                     : "r"(mair), "r"(tcr), "r"((uintptr_t)root)
                     : "memory");
    control |= (1u << 0) | (1u << 2) | (1u << 12); /* M, C, I */
    __asm__ volatile("dsb sy\n msr sctlr_el1, %0\n isb" : : "r"(control) : "memory");
    enabled = true;
    return true;
}

void arm_boot_cache_end(void) {
    if (!enabled) panic("boot cache not active");
    enabled = false;
    arm_cache_restore(saved_control);
}
