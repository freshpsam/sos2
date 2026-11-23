#include "vm.h"
#include "platform.h"
#include "kernel_io.h"
#include "boot_memory.h"
#include "runtime_files.h"
#include "arm_machine.h"
#include "boot_progress.h"
#include "arm_cache.h"
#include "arm_smp.h"

/* VMSAv8-64 stage 1 with 4 KiB pages and 39-bit addresses (TCR_EL1.T0SZ 25):
 * level 1 entries cover 1 GiB, level 2 2 MiB, level 3 one page. Each address
 * space has one level 1 root in TTBR0_EL1 holding the identity-mapped kernel
 * (global, EL1 only) and, from USER_BASE, its own user tables; TTBR1_EL1
 * walks are disabled. The memory types are arch/arm/mmu.c's, as MAIR_EL1
 * attributes: 0 Device-nGnRE, 1 Normal non-cacheable, 2 Normal write-back.
 * Code, immutable kernel data and explicitly CPU-private memory are cached.
 * User pages and their supervisor physical aliases use identical shareable
 * write-back attributes. Scanout and DMA remain non-cacheable. The
 * allocator's free pages are non-cacheable; transitions happen only while no
 * CPU can execute the owning address space. No device uses user pages as DMA.
 * The shared engine is mapped with 2 MiB level 2 blocks, as arch/arm/mmu.c
 * maps it with sections.
 * Translation tables are cached and walked through the caches (TCR_EL1's
 * IRGN0/ORGN0 write-back, inner shareable), which keeps walks coherent with
 * the stores that change tables; uncached walks cost every TLB miss up to
 * three trips to DRAM. Table pages come from the pool, aliased cached while
 * they hold a table, as vm_cpu_alloc's buffers are.
 * https://developer.arm.com/documentation/101811/latest
 */
#define DESCRIPTOR 3ull /* table (levels 1-2) or page (level 3) */
#define BLOCK      1ull /* 2 MiB at level 2 */
#define BLOCK_PAGES 512
#define DEVICE     (0ull << 2)
#define NORMAL     (1ull << 2)
#define CACHED     (2ull << 2)
#define ATTRIBUTE  (7ull << 2)
#define USER       (1ull << 6) /* AP[1]: EL0 access */
#define READ_ONLY  (1ull << 7) /* AP[2] */
#define SHARED     (3ull << 8) /* inner shareable */
#define ACCESSED   (1ull << 10)
#define NOT_GLOBAL (1ull << 11)
#define PXN        (1ull << 53)
#define UXN        (1ull << 54)
#define XN         (PXN | UXN)
#define PXN_TABLE  (1ull << 59)
#define ADDRESS    0x0000fffffffff000ull
#define KERNEL_RW  0ull
#define KERNEL_RO  READ_ONLY
#define USER_RW    USER
#define USER_RO    (USER | READ_ONLY)
#define ENTRIES    512
#define L3_SPAN    0x200000ull /* bytes one level 3 table maps */
/* Invalid level 3 entries (bit 0 clear, the rest ignored by walks) of ranges
 * in progress (include/vm.h): RESERVED awaits its page, FREEING holds the
 * page its retired translation mapped, RETIRED held one freed since. */
#define RESERVED   (1ull << 2)
#define FREEING    (1ull << 3)
#define RETIRED    (RESERVED | FREEING)
_Static_assert(USER_BASE % 0x40000000 == 0 && USER_END % 0x40000000 == 0 &&
                   USER_END <= (1ull << 39),
               "user window: whole level 1 entries inside the 39-bit space");

static pte_t kernel_root[ENTRIES] __attribute__((aligned(4096), section(".cpu_private")));
/* Tables are allocated cached once vm_init has made the kernel's own cached. */
static bool tables_cached;
static void *table_alloc(void);
static struct vm_space kernel_space = {.root = kernel_root};
extern char __image_start[], __image_end[], __text_end[], __rodata_end[];
extern char __stack_guard[];
extern char __cpu_private_start[], __cpu_private_end[], __exception_stack_top[];

static unsigned index_at(uint64_t address, unsigned level) {
    return (unsigned)(address >> (39 - 9 * level)) & (ENTRIES - 1);
}

/* The next-level table an entry points to, allocated if absent and wanted. */
static pte_t *next_table(pte_t *entry, bool allocate, pte_t flags) {
    if ((*entry & 3) != DESCRIPTOR) {
        if (!allocate || *entry) return 0; /* A block has no table. */
        pte_t *table = tables_cached ? table_alloc() : pages_alloc(1, 1);
        if (!table) return 0;
        /* Zeroed before it is linked: a mapping may add tables while the
         * process's other threads run (kernel/protection/process.c). */
        __asm__ volatile("dsb ishst" ::: "memory");
        *entry = (uintptr_t)table | DESCRIPTOR | flags;
    }
    return (pte_t *)(uintptr_t)(*entry & ADDRESS);
}

/* The level 3 table that maps address in root. */
static pte_t *leaf_table(pte_t *root, uint64_t address, bool allocate, pte_t flags) {
    pte_t *level2 = next_table(&root[index_at(address, 1)], allocate, flags);
    return level2 ? next_table(&level2[index_at(address, 2)], allocate, flags) : 0;
}

static bool kernel_map(uintptr_t start, uintptr_t size, pte_t flags) {
    if (!size || start > UINTPTR_MAX - (size - 1)) return false;
    uintptr_t last = (start + size - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    for (uintptr_t p = start & ~(uintptr_t)(PAGE_SIZE - 1);; p += PAGE_SIZE) {
        pte_t *table = leaf_table(kernel_root, p, true, 0);
        if (!table) return false;
        table[index_at(p, 3)] = p | DESCRIPTOR | SHARED | ACCESSED | flags;
        if (!(p & 0xfffff)) boot_progress_poll();
        if (p == last) return true;
    }
}

static void kernel_unmap(uintptr_t address) {
    pte_t *table = leaf_table(kernel_root, address, true, 0);
    if (!table) panic("kernel page table allocation failed");
    table[index_at(address, 3)] = 0;
}

static unsigned data_line(void) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4u << ((ctr >> 16) & 15);
}

static void clean_user_page(uintptr_t address) {
    unsigned line = data_line();
    __asm__ volatile("dsb ish" ::: "memory");
    for (uintptr_t at = address; at < address + PAGE_SIZE; at += line)
        /* Clean and invalidate to the point of coherency; writers are quiescent. */
        __asm__ volatile("dc civac, %0" : : "r"(at) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}

static void user_alias(uintptr_t address, bool cached) {
    pte_t *table = leaf_table(kernel_root, address, false, 0);
    if (!table) panic("missing user physical alias");
    pte_t *pte = &table[index_at(address, 3)], old = *pte;
    if ((old & ADDRESS) != address || (old & 3) != DESCRIPTOR) panic("invalid user physical alias");
    if (!cached) clean_user_page(address);
    /* Break before changing attributes. These shared kernel tables are
     * referenced by every address space; invalidate the global alias on all
     * CPUs before installing its new memory type. */
    *pte = 0;
    __asm__ volatile("dsb ishst\n tlbi vaae1is, %0\n dsb ish\n isb"
                     :
                     : "r"((uint64_t)address >> 12)
                     : "memory");
    *pte = (old & ~ATTRIBUTE) | (cached ? CACHED : NORMAL);
    __asm__ volatile("dsb ishst\n isb" ::: "memory");
    /* Lines may have been allocated speculatively before the break took
     * effect, and pages are zeroed through the uncached alias. Remove them
     * after the change in both directions (Arm mismatched-attribute rules),
     * or a reused page could read its previous owner's data. */
    clean_user_page(address);
}

static void free_user_page(uintptr_t address) {
    user_alias(address, false);
    if (!pages_free((void *)address, 1)) panic("user page ownership corrupt");
}

void *vm_cpu_alloc(unsigned count) {
    void *buffer = pages_alloc(count, 1);
    if (buffer)
        for (unsigned i = 0; i < count; ++i)
            user_alias((uintptr_t)buffer + (uintptr_t)i * PAGE_SIZE, true);
    return buffer;
}

/* A translation table's page, aliased cached for the cached walks;
 * vm_cpu_free returns it. */
static void *table_alloc(void) { return vm_cpu_alloc(1); }

void vm_cpu_free(void *buffer, unsigned count) {
    /* Flush while the buffer is still owned. Free pages must have their
     * original uncached mapping before they can become DMA. */
    for (unsigned i = 0; i < count; ++i)
        user_alias((uintptr_t)buffer + (uintptr_t)i * PAGE_SIZE, false);
    if (!pages_free(buffer, count)) panic("CPU buffer ownership corrupt");
}

/* Mapping epoch, as in arch/arm/mmu.c: advanced (by the boot CPU, while that
 * space is quiescent) whenever a user mapping is removed, downgraded or made
 * executable, user code changes, or an address space is destroyed. A CPU that
 * re-enters the same space at the same epoch may keep its TLB entries and
 * instruction cache. CPU 0 is its only writer; kernel data is non-cacheable,
 * where exclusive instructions are not guaranteed to work. */
static uint64_t epoch;

static void epoch_advance(void) {
    __asm__ volatile("dmb ish" ::: "memory");
    __atomic_store_n(&epoch, __atomic_load_n(&epoch, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
}

uint64_t vm_epoch(void) {
    uint64_t value = __atomic_load_n(&epoch, __ATOMIC_RELAXED);
    __asm__ volatile("dmb ish" ::: "memory");
    return value;
}

static void vm_switch(const struct vm_space *space); /* NULL selects the kernel. */

void vm_enter(const struct vm_space *space, bool flush) {
    if (flush) {
        vm_switch(space);
        return;
    }
    __asm__ volatile("dsb sy\n msr ttbr0_el1, %0\n isb" : : "r"((uintptr_t)space->root) : "memory");
}

void vm_leave(void) {
    /* Keep the user entries: user and kernel addresses are disjoint, the kernel
     * reaches user memory only through physical aliases, and entering another
     * space or a later epoch flushes them first. */
    __asm__ volatile("dsb sy\n msr ttbr0_el1, %0\n isb"
                     :
                     : "r"((uintptr_t)kernel_space.root)
                     : "memory");
}

static void vm_switch(const struct vm_space *space) {
    uintptr_t root = (uintptr_t)(space ? space : &kernel_space)->root;
    /* The ISB makes the new TTBR0 visible to table walks before the TLB is
     * invalidated, so walks of the old tables cannot refill it afterwards. */
    __asm__ volatile("dsb sy\n msr ttbr0_el1, %0\n isb\n tlbi vmalle1\n dsb sy\n isb"
                     :
                     : "r"(root)
                     : "memory");
}

/* SCTLR_EL1: MMU, data cache, stack alignment checks at EL1 and EL0,
 * instruction cache, EL0 DC ZVA, CTR_EL0 reads and cache maintenance (a JIT
 * cleans and invalidates its own code), untrapped EL0 WFI/WFE. WXN stays off
 * for the JIT window; vm_map and vm_protect keep W^X everywhere else. */
#define SCTLR_TRANSLATION \
    ((1u << 0) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 12) | (1u << 14) | (1u << 15) | \
     (1u << 16) | (1u << 18) | (1u << 26))

static bool enable_translation(bool secondary) {
    uint64_t control = arm_cache_control();
    if (control & 5) return false; /* Boot contract: MMU and data cache off. */
    if (secondary) arm_dcache_invalidate_local();
    else arm_dcache_invalidate_all();
    arm_instruction_sync();
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    /* T0SZ 25, write-back write-allocate inner shareable walks (IRGN0,
     * ORGN0, SH0), 4 KiB granule, TTBR1 walks disabled (EPD1), output
     * addresses as wide as the CPU's. */
    uint64_t tcr = 25 | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23) | ((mmfr0 & 7) << 32);
    /* CNTKCTL_EL1.EL0VCTEN only: user code may read the virtual count and its
     * frequency (the kernel's monotonic clock) without a syscall. Timer
     * registers and the physical count stay privileged. */
    uint64_t mair = 0x04 | (0x44 << 8) | (0xff << 16), user_counter = 2;
    __asm__ volatile("msr mair_el1, %0\n msr tcr_el1, %1\n msr cntkctl_el1, %2\n isb"
                     :
                     : "r"(mair), "r"(tcr), "r"(user_counter)
                     : "memory");
    vm_switch(0);
    control |= SCTLR_TRANSLATION;
    __asm__ volatile("dsb sy\n msr sctlr_el1, %0\n isb" : : "r"(control) : "memory");
    return true;
}

bool vm_secondary_init(void) { return enable_translation(true); }

static bool overlap(uint64_t start, uint64_t end, uint64_t other_start, uint64_t other_end) {
    return start < other_end && other_start < end;
}

/* At vm_init: the kernel's entry for a table page, made cached. */
static void cache_table(pte_t *table) {
    pte_t *leaf = leaf_table(kernel_root, (uintptr_t)table, false, 0);
    if (!leaf) panic("kernel table unmapped");
    pte_t *pte = &leaf[index_at((uintptr_t)table, 3)];
    *pte = (*pte & ~ATTRIBUTE) | CACHED;
}

bool vm_init(const struct platform_devices *d, uintptr_t dtb) {
    const struct arm_machine *machine = arm_machine_get();
    uintptr_t start = (uintptr_t)__image_start, end = (uintptr_t)__image_end;
    uintptr_t resources = runtime_files_base();
    uint32_t resource_size = runtime_files_size();
    /* The page pool is every large free stretch of RAM the DTB reports, the
     * reservations between them withheld: most of the tablet's 3 GB, which
     * the firmware's reservations split. */
    struct memory_region pool[BOOT_POOL_REGIONS];
    unsigned regions = boot_memory_pool(d, dtb, start, end, resources, resource_size,
                                        (uint64_t)PAGE_POOL_PAGES * PAGE_SIZE, pool);
    if (!regions || !machine->device_count || end <= start ||
        !fdt_range_in_ram(d, start, end - start) || dtb > UINT32_MAX - d->dtb_size ||
        overlap(start, end, USER_BASE, USER_END) || overlap(dtb, dtb + d->dtb_size, start, end))
        return false;
    /* An inherited scanout (LK's on the tablet) stays mapped non-cacheable. */
    uintptr_t fb = (uintptr_t)d->framebuffer.pixels;
    uint64_t fb_size = d->has_framebuffer ? d->framebuffer_size : 0;
    if (fb_size &&
        (!fdt_range_in_ram(d, fb, fb_size) || overlap(fb, fb + fb_size, USER_BASE, USER_END) ||
         overlap(fb, fb + fb_size, start, end) ||
         overlap(fb, fb + fb_size, dtb, dtb + d->dtb_size)))
        return false;
    if (resource_size && overlap(resources, resources + resource_size, start, end)) return false;
    for (size_t i = 0; i < d->reserved_region_count; ++i) {
        const struct memory_region *r = &d->reserved_regions[i];
        if (r->size > UINT64_MAX - r->start || overlap(start, end, r->start, r->start + r->size))
            return false;
    }
    for (unsigned i = 0; i < machine->device_count; ++i) {
        const struct arm_mapping *r = &machine->devices[i];
        if (overlap(start, end, r->start, (uint64_t)r->start + r->size)) return false;
    }
    uint64_t total = 0;
    for (unsigned n = 0; n < regions; ++n) {
        uint64_t lower = pool[n].start, upper = lower + pool[n].size;
        if ((lower | upper) & (PAGE_SIZE - 1) || upper <= lower ||
            (n && lower < pool[n - 1].start + pool[n - 1].size) ||
            !fdt_range_in_ram(d, lower, upper - lower) ||
            overlap(lower, upper, USER_BASE, USER_END) || overlap(lower, upper, start, end) ||
            overlap(lower, upper, dtb, dtb + d->dtb_size) ||
            (fb_size && overlap(lower, upper, fb, fb + fb_size)) ||
            (resource_size && overlap(lower, upper, resources, resources + resource_size)))
            return false;
        for (size_t i = 0; i < d->reserved_region_count; ++i) {
            const struct memory_region *r = &d->reserved_regions[i];
            if (overlap(lower, upper, r->start, r->start + r->size)) return false;
        }
        for (unsigned i = 0; i < machine->device_count; ++i) {
            const struct arm_mapping *r = &machine->devices[i];
            if (overlap(lower, upper, r->start, (uint64_t)r->start + r->size)) return false;
        }
        total += upper - lower;
    }
    uintptr_t pool_start = pool[0].start;
    uintptr_t pool_end = pool[regions - 1].start + pool[regions - 1].size;
    if (total < (256u << 20) || !pages_init((void *)pool_start, pool_end - pool_start))
        return false;
    for (unsigned n = 1; n < regions; ++n) {
        uintptr_t gap = pool[n - 1].start + pool[n - 1].size;
        if (pool[n].start > gap && !pages_withhold((void *)gap, pool[n].start - gap)) return false;
    }
    memset(kernel_root, 0, sizeof(kernel_root));
    if (!kernel_map(start, (uintptr_t)__text_end - start, CACHED | KERNEL_RO | UXN) ||
        !kernel_map((uintptr_t)__text_end, (uintptr_t)(__rodata_end - __text_end),
                    CACHED | KERNEL_RO | XN) ||
        !kernel_map((uintptr_t)__rodata_end, (uintptr_t)(__image_end - __rodata_end),
                    NORMAL | KERNEL_RW | XN))
        return false;
    if (!kernel_map((uintptr_t)__cpu_private_start,
                    (uintptr_t)(__cpu_private_end - __cpu_private_start),
                    CACHED | KERNEL_RW | XN) ||
        !kernel_map((uintptr_t)__stack_guard, (uintptr_t)(__exception_stack_top - __stack_guard),
                    CACHED | KERNEL_RW | XN))
        return false;
    for (unsigned n = 0; n < regions; ++n)
        if (!kernel_map(pool[n].start, pool[n].size, NORMAL | KERNEL_RW | XN)) return false;
    /* Cached: the resources are immutable from here. They arrived cleaned to
     * the point of coherency (loaded with the MMU off, or decoded through
     * boot_cache.c), nothing writes them again and no device reads them, and
     * this is their only mapping. The engine block is copied from them. */
    if (resource_size && !kernel_map(resources, resource_size, CACHED | KERNEL_RO | XN))
        return false;
    for (unsigned i = 0; i < machine->device_count; ++i) {
        const struct arm_mapping *r = &machine->devices[i];
        if (!kernel_map(r->start, r->size, DEVICE | KERNEL_RW | XN)) return false;
    }
    if (fb_size && !kernel_map(fb, fb_size, NORMAL | KERNEL_RW | XN)) return false;
    kernel_unmap((uintptr_t)__stack_guard);
    kernel_unmap((uintptr_t)kernel_io_stack);
    for (unsigned cpu = 1; cpu < ARM_CPU_MAX; ++cpu)
        kernel_unmap((uintptr_t)arm_cpu_stacks[cpu - 1]);
    for (unsigned cpu = 0; cpu < ARM_CPU_MAX; ++cpu)
        kernel_unmap((uintptr_t)arm_exception_stacks[cpu]);
    /* The kernel's own tables came from the pool with the data cache off;
     * like every table from here on, they are cached (the root is in the
     * cached CPU-private section). */
    for (unsigned i = 0; i < ENTRIES; ++i) {
        pte_t *level2 = next_table(&kernel_root[i], false, 0);
        if (!level2) continue;
        cache_table(level2);
        for (unsigned j = 0; j < ENTRIES; ++j) {
            pte_t *table = next_table(&level2[j], false, 0);
            if (table) cache_table(table);
        }
    }
    tables_cached = true;
    return enable_translation(false);
}

static uintptr_t shared_base;
static unsigned shared_pages, shared_users;
static bool shared_adopted; /* The resources' own pages */

static bool shared_page(uintptr_t physical) {
    return shared_pages && physical - shared_base < (uintptr_t)shared_pages * PAGE_SIZE;
}

/* The block holds the engine's code. The service worker builds and frees it,
 * yielding every 64 KiB so the desktop keeps running meanwhile. */
void *vm_shared_create(unsigned pages) {
    if (shared_pages || !pages) return 0;
    /* Aligned for blocks; unaligned takes 4 KiB pages. */
    unsigned char *block = pages_reserve(pages, BLOCK_PAGES);
    if (!block) block = pages_reserve(pages, 1);
    if (!block) return 0;
    for (unsigned i = 0; i < pages; ++i) {
        user_alias((uintptr_t)block + (uintptr_t)i * PAGE_SIZE, true);
        memset(block + (size_t)i * PAGE_SIZE, 0, PAGE_SIZE);
        if ((i & 15) == 15) kernel_io_wait();
    }
    shared_base = (uintptr_t)block;
    shared_pages = pages;
    return block;
}

void vm_shared_publish(void) {
    /* Publish to the point of coherency; the epoch makes every CPU invalidate
     * its instruction cache before running a space again. */
    for (unsigned i = 0; i < shared_pages; ++i) {
        clean_user_page(shared_base + (uintptr_t)i * PAGE_SIZE);
        if ((i & 15) == 15) kernel_io_wait();
    }
    epoch_advance();
}

bool vm_shared_adopt(const void *data, unsigned pages) {
    uintptr_t base = (uintptr_t)data, start = runtime_files_base();
    uint32_t size = runtime_files_size();
    if (shared_pages || !pages || !size || (base & (L3_SPAN - 1)) || base < start ||
        base - start > size || pages > (size - (base - start)) / PAGE_SIZE)
        return false;
    /* The resources are cached and were cleaned to the point of coherency as
     * they arrived (vm_init); the epoch has every CPU invalidate its
     * instruction cache before running them. */
    shared_base = base;
    shared_pages = pages;
    shared_adopted = true;
    epoch_advance();
    return true;
}

bool vm_shared_active(void) { return shared_pages != 0; }

void vm_shared_trim(void) {
    if (!shared_pages || shared_users) return;
    if (shared_adopted) {
        shared_adopted = false;
        shared_base = 0;
        shared_pages = 0;
        return;
    }
    for (unsigned i = 0; i < shared_pages; ++i) {
        user_alias(shared_base + (uintptr_t)i * PAGE_SIZE, false);
        if ((i & 15) == 15) kernel_io_wait();
    }
    if (!pages_free((void *)shared_base, shared_pages)) panic("shared block ownership corrupt");
    shared_base = 0;
    shared_pages = 0;
}

bool vm_create(struct vm_space *s) {
    *s = (struct vm_space){0};
    s->root = table_alloc();
    if (!s->root) return false;
    memcpy(s->root, kernel_root, sizeof(kernel_root));
    return true;
}

/* The level 3 table holding a user address's entry, if present. */
static pte_t *user_table(const struct vm_space *s, user_ptr address) {
    if (!s->root || address < USER_BASE || address >= USER_END) return 0;
    return leaf_table(s->root, address, false, 0);
}

/* The block over a user address, or 0. */
static pte_t user_block(const struct vm_space *s, user_ptr address) {
    if (!s->root || address < USER_BASE || address >= USER_END) return 0;
    pte_t *level2 = next_table(&s->root[index_at(address, 1)], false, 0);
    pte_t entry = level2 ? level2[index_at(address, 2)] : 0;
    return (entry & 3) == BLOCK ? entry : 0;
}

/* A new user table: privileged instruction fetches never use it (PXNTable). */
static pte_t *new_user_table(struct vm_space *s, user_ptr address) {
    return leaf_table(s->root, address, true, PXN_TABLE);
}

static void release_empty_table(struct vm_space *s, user_ptr address) {
    pte_t *level2 = next_table(&s->root[index_at(address, 1)], false, 0);
    pte_t *table = user_table(s, address);
    if (!level2 || !table) return;
    for (unsigned i = 0; i < ENTRIES; ++i)
        if (table[i]) return;
    level2[index_at(address, 2)] = 0;
    vm_cpu_free(table, 1);
}

/* Frees the tables left empty over count pages from address. */
static void release_empty_tables(struct vm_space *s, user_ptr address, unsigned count) {
    user_ptr last = (address + (user_ptr)(count - 1) * PAGE_SIZE) & ~(L3_SPAN - 1);
    for (user_ptr at = address & ~(L3_SPAN - 1);; at += L3_SPAN) {
        release_empty_table(s, at);
        if (at == last) break;
    }
}

/* On the service worker: a page of a gigabyte frees for a second or more, so
 * the desktop runs every 16 pages, as in vm_shared_trim. */
void vm_destroy(struct vm_space *s) {
    if (s->root) {
        unsigned freed = 0;
        for (unsigned i = index_at(USER_BASE, 1); i < index_at(USER_END - 1, 1) + 1; ++i) {
            pte_t *level2 = next_table(&s->root[i], false, 0);
            if (!level2) continue;
            for (unsigned j = 0; j < ENTRIES; ++j) {
                pte_t *table = next_table(&level2[j], false, 0);
                if (!table) continue;
                for (unsigned k = 0; k < ENTRIES; ++k)
                    if ((table[k] & 3) == DESCRIPTOR || (table[k] & ~ADDRESS) == FREEING) {
                        uintptr_t physical = table[k] & ADDRESS;
                        table[k] = 0;
                        if (!shared_page(physical)) free_user_page(physical);
                        if ((++freed & 15) == 0) kernel_io_wait();
                    }
                vm_cpu_free(table, 1);
            }
            vm_cpu_free(level2, 1);
        }
        vm_cpu_free(s->root, 1);
        if (s->promised) pages_unpromise(s->promised);
        epoch_advance();
        if (s->shared) {
            if (!shared_users) panic("shared page block reference count");
            --shared_users;
            vm_shared_trim();
        }
    }
    *s = (struct vm_space){0};
}

/* Writable and executable at once only in the JIT window; the kernel lets
 * only web page processes map it (kernel/protection/process.c). */
static bool user_permissions(user_ptr address, unsigned permissions) {
    return (permissions & VM_READ) && !(permissions & ~7u) &&
           ((permissions & (VM_WRITE | VM_EXEC)) != (VM_WRITE | VM_EXEC) ||
            (address >= USER_JIT_BASE && address < USER_JIT_END));
}

/* A page's entry. No permissions make it kernel-only: mapped and counted, but
 * inaccessible to the process. */
static pte_t user_entry(uintptr_t physical, unsigned permissions) {
    return physical | DESCRIPTOR | CACHED | SHARED | ACCESSED | NOT_GLOBAL |
           (permissions ? ((permissions & VM_WRITE) ? USER_RW : USER_RO) : KERNEL_RW) |
           ((permissions & VM_EXEC) ? PXN : XN);
}

bool vm_map(struct vm_space *s, user_ptr address, unsigned permissions) {
    if (!s->root || (address & (PAGE_SIZE - 1)) || address < USER_BASE || address >= USER_END ||
        s->mapped_pages >= USER_PAGE_QUOTA ||
        (permissions && !user_permissions(address, permissions)))
        return false;
    pte_t *table = new_user_table(s, address);
    if (!table) return false;
    pte_t *pte = &table[index_at(address, 3)];
    if (*pte) return false;
    void *page = pages_alloc(1, 1);
    if (!page) {
        release_empty_table(s, address);
        return false;
    }
    /* user_alias cleans the page to the point of coherency, as code needs. */
    user_alias((uintptr_t)page, true);
    *pte = user_entry((uintptr_t)page, permissions);
    ++s->mapped_pages;
    if (permissions & VM_EXEC) epoch_advance();
    return true;
}

unsigned vm_map_shared(struct vm_space *s, user_ptr address, unsigned page, unsigned count,
                       unsigned permissions) {
    if (!s->root || (address & (PAGE_SIZE - 1)) || address < USER_BASE || address >= USER_END ||
        !count || page >= shared_pages || count > shared_pages - page ||
        !(permissions & VM_READ) || (permissions & ~(VM_READ | VM_EXEC)))
        return 0;
    uintptr_t physical = shared_base + (uintptr_t)page * PAGE_SIZE;
    pte_t entry = user_entry(physical, permissions);
    unsigned mapped = 1;
    pte_t *level2 = next_table(&s->root[index_at(address, 1)], true, PXN_TABLE);
    if (!level2) return 0;
    pte_t *slot = &level2[index_at(address, 2)];
    if (!*slot && count >= BLOCK_PAGES && !((address | physical) & (L3_SPAN - 1))) {
        __asm__ volatile("dsb ishst" ::: "memory");
        *slot = (entry & ~3ull) | BLOCK;
        mapped = BLOCK_PAGES;
    } else {
        pte_t *table = next_table(slot, true, PXN_TABLE);
        if (!table || table[index_at(address, 3)]) return 0;
        table[index_at(address, 3)] = entry;
    }
    if (!s->shared) {
        s->shared = true;
        ++shared_users;
    }
    return mapped;
}

void *vm_resolve(const struct vm_space *s, user_ptr address, unsigned permissions) {
    pte_t *table = user_table(s, address);
    pte_t pte = table ? table[index_at(address, 3)] : user_block(s, address);
    uint64_t span = table ? PAGE_SIZE : L3_SPAN;
    if ((pte & 3) != (table ? DESCRIPTOR : BLOCK) || !(pte & USER) ||
        ((permissions & VM_WRITE) && (pte & READ_ONLY)) || ((permissions & VM_EXEC) && (pte & UXN)))
        return 0;
    return (void *)(uintptr_t)((pte & ADDRESS) | (address & (span - 1)));
}

bool vm_mapped(const struct vm_space *s, user_ptr address) {
    pte_t *table = user_table(s, address);
    return table ? (table[index_at(address, 3)] & 3) == DESCRIPTOR : user_block(s, address) != 0;
}

bool vm_protect(struct vm_space *s, user_ptr address, unsigned permissions) {
    if ((address & (PAGE_SIZE - 1)) || (permissions && !user_permissions(address, permissions)))
        return false;
    pte_t *table = user_table(s, address);
    if (!table || (table[index_at(address, 3)] & 3) != DESCRIPTOR) return false;
    pte_t *pte = &table[index_at(address, 3)];
    uintptr_t physical = *pte & ADDRESS;
    if (shared_page(physical)) return false;
    if (permissions & VM_EXEC) clean_user_page(physical);
    *pte = user_entry(physical, permissions);
    epoch_advance();
    return true;
}

bool vm_clear(struct vm_space *s, user_ptr address) {
    if (address & (PAGE_SIZE - 1)) return false;
    pte_t *table = user_table(s, address);
    if (!table) return false;
    pte_t pte = table[index_at(address, 3)];
    uintptr_t physical = pte & ADDRESS;
    if ((pte & 3) != DESCRIPTOR || shared_page(physical)) return false;
    memset((void *)physical, 0, PAGE_SIZE);
    if (!(pte & UXN)) {
        clean_user_page(physical);
        epoch_advance();
    }
    return true;
}

bool vm_copy_from_user(const struct vm_space *s, void *out, user_ptr address, size_t size) {
    if (size && (size - 1 > (user_ptr)-1 - address)) return false;
    unsigned char *destination = out;
    while (size) {
        void *source = vm_resolve(s, address, VM_READ);
        if (!source) return false;
        size_t n = PAGE_SIZE - (address & (PAGE_SIZE - 1));
        if (n > size) n = size;
        memcpy(destination, source, n);
        destination += n;
        address += n;
        size -= n;
    }
    return true;
}

bool vm_copy_to_user(const struct vm_space *s, user_ptr address, const void *data, size_t size) {
    /* Never partially deliver an event. This address space cannot change here. */
    if (!vm_accessible(s, address, size, VM_WRITE)) return false;
    const unsigned char *source = data;
    while (size) {
        void *destination = vm_resolve(s, address, VM_WRITE);
        size_t n = PAGE_SIZE - (address & (PAGE_SIZE - 1));
        if (n > size) n = size;
        memcpy(destination, source, n);
        source += n;
        address += n;
        size -= n;
    }
    return true;
}

/* The ranges the range allocator serves: the heap and the JIT window. */
static bool allocation_range(user_ptr lower, user_ptr upper) {
    return lower < upper && ((lower >= USER_HEAP_BASE && upper <= USER_HEAP_END) ||
                             (lower >= USER_JIT_BASE && upper <= USER_JIT_END));
}

bool vm_release(struct vm_space *s, user_ptr address, unsigned count) {
    if (!count || (address & (PAGE_SIZE - 1)) || count > USER_PAGE_QUOTA ||
        !allocation_range(address, address + (user_ptr)count * PAGE_SIZE))
        return false;
    for (unsigned i = 0; i < count; ++i) {
        user_ptr at = address + (user_ptr)i * PAGE_SIZE;
        pte_t *table = user_table(s, at), pte = table ? table[index_at(at, 3)] : 0;
        if ((pte & 3) != DESCRIPTOR || shared_page(pte & ADDRESS)) return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        user_ptr at = address + (user_ptr)i * PAGE_SIZE;
        pte_t *pte = &user_table(s, at)[index_at(at, 3)];
        uintptr_t physical = *pte & ADDRESS;
        *pte = 0;
        free_user_page(physical);
        --s->mapped_pages;
    }
    release_empty_tables(s, address, count);
    epoch_advance(); /* The next entry to any space flushes the TLB. */
    return true;
}

/* Bytes from at to the end of its level 3 table's span, or to end if that
 * is nearer. */
static user_ptr table_stretch(user_ptr at, user_ptr end) {
    user_ptr stretch = L3_SPAN - (at & (L3_SPAN - 1));
    return stretch < end - at ? stretch : end - at;
}

/* The first run of count free pages at a multiple of alignment pages in
 * [lower, upper), or 0. Reserved and retiring entries are not free. It goes
 * a table at a time, as in arch/arm/mmu.c. */
static user_ptr find_range(const struct vm_space *s, unsigned count, unsigned alignment,
                           user_ptr lower, user_ptr upper) {
    if (!allocation_range(lower, upper) || ((lower | upper) & (PAGE_SIZE - 1)) ||
        count > (upper - lower) / PAGE_SIZE || !alignment || (alignment & (alignment - 1)) ||
        alignment > (upper - lower) / PAGE_SIZE)
        return 0;
    if (!s->root || !count || count > USER_PAGE_QUOTA - s->mapped_pages) return 0;
    user_ptr step = (user_ptr)alignment * PAGE_SIZE;
    user_ptr start = (lower + step - 1) & ~(step - 1);
    unsigned free_run = 0; /* Free pages from start to at */
    for (user_ptr at = start; at < upper;) {
        user_ptr stretch = table_stretch(at, upper);
        const pte_t *table = user_table(s, at);
        if (!table) {
            if (count - free_run <= stretch / PAGE_SIZE) return start;
            free_run += (unsigned)(stretch / PAGE_SIZE);
            at += stretch;
            continue;
        }
        const pte_t *entry = &table[index_at(at, 3)], *end = entry + stretch / PAGE_SIZE;
        while (entry < end) {
            if (!*entry) {
                ++entry;
                at += PAGE_SIZE;
                if (++free_run == count) return start;
                continue;
            }
            /* Past the mapped run; the next may start at a multiple of step. */
            do {
                ++entry;
                at += PAGE_SIZE;
            } while (entry < end && *entry);
            free_run = 0;
            start = (at + step - 1) & ~(step - 1);
            if (start < at) return 0;
            if (start != at) {
                at = start;
                break;
            }
        }
    }
    return 0;
}

user_ptr vm_allocate_range(struct vm_space *s, unsigned count, unsigned alignment, user_ptr lower,
                           user_ptr upper, unsigned permissions) {
    user_ptr start = find_range(s, count, alignment, lower, upper);
    if (!start) return 0;
    unsigned mapped = 0;
    for (; mapped < count; ++mapped)
        if (!vm_map(s, start + (user_ptr)mapped * PAGE_SIZE, permissions)) break;
    if (mapped == count) return start;
    if (mapped && !vm_release(s, start, mapped)) panic("heap rollback failed");
    return 0;
}

bool vm_occupied(const struct vm_space *s, user_ptr address) {
    pte_t *table = user_table(s, address);
    return table ? table[index_at(address, 3)] != 0 : user_block(s, address) != 0;
}

bool vm_reserve(struct vm_space *s, user_ptr address, unsigned count, unsigned permissions) {
    if (!s->root || !count || (address & (PAGE_SIZE - 1)) || count > USER_PAGE_QUOTA ||
        count > USER_PAGE_QUOTA - s->mapped_pages ||
        !allocation_range(address, address + (user_ptr)count * PAGE_SIZE) ||
        (permissions && !user_permissions(address, permissions)))
        return false;
    user_ptr end = address + (user_ptr)count * PAGE_SIZE;
    for (user_ptr at = address, n; at < end; at += n) {
        n = table_stretch(at, end);
        const pte_t *table = user_table(s, at);
        if (table)
            for (unsigned i = 0; i < n / PAGE_SIZE; ++i)
                if (table[index_at(at, 3) + i]) return false;
    }
    for (user_ptr at = address; at < end; at += table_stretch(at, end))
        if (!new_user_table(s, at)) {
            release_empty_tables(s, address, count);
            return false;
        }
    if (!pages_promise(count)) {
        release_empty_tables(s, address, count);
        return false;
    }
    for (user_ptr at = address, n; at < end; at += n) {
        n = table_stretch(at, end);
        pte_t *entry = &user_table(s, at)[index_at(at, 3)];
        for (unsigned i = 0; i < n / PAGE_SIZE; ++i) entry[i] = RESERVED;
    }
    s->mapped_pages += count;
    s->promised += count;
    return true;
}

user_ptr vm_reserve_range(struct vm_space *s, unsigned count, unsigned alignment, user_ptr lower,
                          user_ptr upper, unsigned permissions) {
    user_ptr start = find_range(s, count, alignment, lower, upper);
    return start && vm_reserve(s, start, count, permissions) ? start : 0;
}

void vm_fill_reserved(struct vm_space *s, user_ptr address, unsigned permissions) {
    pte_t *table = user_table(s, address);
    pte_t *pte = table ? &table[index_at(address, 3)] : 0;
    if (!pte || *pte != RESERVED || !s->promised) panic("reserved mapping corrupt");
    void *page = pages_alloc_promised();
    --s->promised;
    user_alias((uintptr_t)page, true);
    *pte = user_entry((uintptr_t)page, permissions);
    if (permissions & VM_EXEC) epoch_advance();
}

bool vm_unmap_begin(struct vm_space *s, user_ptr address, unsigned count) {
    if (!count || (address & (PAGE_SIZE - 1)) || count > USER_PAGE_QUOTA ||
        !allocation_range(address, address + (user_ptr)count * PAGE_SIZE))
        return false;
    user_ptr end = address + (user_ptr)count * PAGE_SIZE;
    for (user_ptr at = address, n; at < end; at += n) {
        n = table_stretch(at, end);
        const pte_t *table = user_table(s, at);
        if (!table) return false;
        for (unsigned i = 0; i < n / PAGE_SIZE; ++i) {
            pte_t pte = table[index_at(at, 3) + i];
            if ((pte & 3) != DESCRIPTOR || shared_page(pte & ADDRESS)) return false;
        }
    }
    for (user_ptr at = address, n; at < end; at += n) {
        n = table_stretch(at, end);
        pte_t *entry = &user_table(s, at)[index_at(at, 3)];
        for (unsigned i = 0; i < n / PAGE_SIZE; ++i) entry[i] = (entry[i] & ADDRESS) | FREEING;
    }
    epoch_advance();
    return true;
}

void vm_unmap_page(struct vm_space *s, user_ptr address) {
    pte_t *table = user_table(s, address);
    pte_t *pte = table ? &table[index_at(address, 3)] : 0;
    if (!pte || (*pte & ~ADDRESS) != FREEING) panic("retiring mapping corrupt");
    uintptr_t physical = *pte & ADDRESS;
    *pte = RETIRED;
    free_user_page(physical);
    --s->mapped_pages;
}

void vm_unmap_end(struct vm_space *s, user_ptr address, unsigned count) {
    user_ptr end = address + (user_ptr)count * PAGE_SIZE;
    for (user_ptr at = address, n; at < end; at += n) {
        n = table_stretch(at, end);
        pte_t *table = user_table(s, at);
        if (!table) panic("retired mapping corrupt");
        pte_t *entry = &table[index_at(at, 3)];
        for (unsigned i = 0; i < n / PAGE_SIZE; ++i) {
            if (entry[i] != RETIRED) panic("retired mapping corrupt");
            entry[i] = 0;
        }
    }
    release_empty_tables(s, address, count);
    epoch_advance(); /* CPUs may hold walks through the freed tables. */
}
