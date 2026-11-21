#include "vm.h"
#include "platform.h"
#include "kernel_io.h"
#include "boot_memory.h"
#include "runtime_files.h"
#include "arm_machine.h"
#include "boot_progress.h"
#include "arm_cache.h"
#include "arm_smp.h"

/* ARMv7-A short descriptors, domain 0 Client, SCTLR.AFE=0/TRE=0.
 * ARM DDI0406C B3: small-page AP[2:0] = 001 kernel RW, 101 kernel RO,
 * 011 user RW, 110 user RO; XN bit 0. Normal memory uses TEX=001,C=B=0.
 * Code, immutable kernel data and explicitly CPU-private memory are cached.
 * User pages and their supervisor physical aliases use identical shareable
 * write-back attributes. Page tables, scanout and DMA remain non-cacheable.
 * The allocator's free pages are non-cacheable; transitions happen only while
 * no CPU can execute the owning address space. No device uses user pages as DMA.
 * https://documentation-service.arm.com/static/5f8daeb7f86e16515cdb8c4e
 */
enum {
    SMALL = 2,
    XN = 1,
    NORMAL = 1 << 6,
    CACHED = (1 << 6) | (3 << 2),
    SHARED = 1 << 10,
    KERNEL_RW = 1 << 4,
    KERNEL_RO = (1 << 9) | (1 << 4),
    USER_RW = 3 << 4,
    USER_RO = (1 << 9) | (2 << 4),
    DEVICE = 1 << 2
};

static uint32_t kernel_root[4096] __attribute__((aligned(16384)));
static struct vm_space kernel_space = {.root = kernel_root};
extern char __image_start[], __image_end[], __text_end[], __rodata_end[];
extern char __page_pool_start[], __page_pool_end[];
extern char __stack_guard[], __irq_guard[], __abort_guard[], __undef_guard[];
extern char __cpu_private_start[], __cpu_private_end[], __exception_stack_top[];

static uint32_t *table_for(uint32_t *root, uint32_t address) {
    unsigned index = address >> 20;
    if (!root[index]) {
        uint32_t *table = pages_alloc(1, 1);
        if (!table) return 0;
        root[index] = (uintptr_t)table | 1;
    }
    return (uint32_t *)(uintptr_t)(root[index] & 0xfffffc00u);
}

static bool kernel_map(uint32_t start, uint32_t size, uint32_t flags) {
    if (!size || start > UINT32_MAX - (size - 1)) return false;
    uint32_t last = (start + size - 1) & ~(PAGE_SIZE - 1);
    for (uint32_t p = start & ~(PAGE_SIZE - 1);; p += PAGE_SIZE) {
        uint32_t *table = table_for(kernel_root, p);
        if (!table) return false;
        table[(p >> 12) & 255] = p | SMALL | SHARED | flags;
        if (!(p & 0xfffff)) boot_progress_poll();
        if (p == last) return true;
    }
}

static void kernel_unmap(uintptr_t address) {
    uint32_t *table = table_for(kernel_root, address);
    if (!table) panic("kernel page table allocation failed");
    table[(address >> 12) & 255] = 0;
}

static void clean_user_page(uintptr_t address) {
    uint32_t ctr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
    unsigned line = 4u << ((ctr >> 16) & 15);
    __asm__ volatile("dsb ish" ::: "memory");
    for (uintptr_t at = address; at < address + PAGE_SIZE; at += line)
        /* Clean/invalidate to PoC also covers A53 revisions whose clean-only
         * operations require the stronger operation. Writers are quiescent. */
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(at) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}

/* Writable and executable at once only in the engine's JIT window, which
 * kernel/protection/process.c offers to web page processes alone. */
static bool user_permissions(user_ptr address, unsigned permissions) {
    return (permissions & (VM_WRITE | VM_EXEC)) != (VM_WRITE | VM_EXEC) ||
           (address >= USER_JIT_BASE && address < USER_JIT_END);
}

/* A private page's entry. No permissions make it kernel-only: mapped and
 * counted, but inaccessible to the process. */
static uint32_t user_pte(uintptr_t physical, unsigned permissions) {
    return physical | SMALL | CACHED | SHARED |
           (permissions ? ((permissions & VM_WRITE) ? USER_RW : USER_RO) : KERNEL_RW) |
           ((permissions & VM_EXEC) ? 0 : XN);
}

/* The heap, or the JIT window, for page-granular allocation and release. */
static bool allocation_range(user_ptr lower, user_ptr upper) {
    return lower < upper && ((lower >= USER_HEAP_BASE && upper <= USER_HEAP_END) ||
                             (lower >= USER_JIT_BASE && upper <= USER_JIT_END));
}

static void user_alias(uintptr_t address, bool cached) {
    uint32_t *table = table_for(kernel_root, address);
    if (!table) panic("missing user physical alias");
    uint32_t *pte = &table[(address >> 12) & 255], old = *pte;
    if ((old & ~4095u) != address || !(old & SMALL)) panic("invalid user physical alias");
    if (!cached) clean_user_page(address);
    /* Break before changing attributes. These shared kernel L2 tables are
     * referenced by every address space; invalidate the global alias on all
     * CPUs before installing its new memory type. */
    *pte = 0;
    __asm__ volatile("dsb ishst\n mcr p15, 0, %0, c8, c3, 1\n dsb ish\n isb"
                     :
                     : "r"(address)
                     : "memory");
    *pte = (old & ~CACHED) | (cached ? CACHED : NORMAL);
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

void vm_cpu_free(void *buffer, unsigned count) {
    /* Flush while the buffer is still owned. Free pages must have their
     * original uncached mapping before they can become page tables or DMA. */
    for (unsigned i = 0; i < count; ++i)
        user_alias((uintptr_t)buffer + (uintptr_t)i * PAGE_SIZE, false);
    if (!pages_free(buffer, count)) panic("CPU buffer ownership corrupt");
}

/* Mapping epoch: advanced (by the boot CPU, while that space is quiescent)
 * whenever a user mapping is removed, downgraded or made executable, user code
 * changes, or an address space is destroyed. Adding mappings never needs it:
 * TLBs do not cache faulting entries. A CPU that re-enters the same space at
 * the same epoch may keep its TLB entries, instruction cache and branch state.
 * It has 64 bits so it never repeats, kept as two words with plain accesses
 * and barriers: CPU 0 is its only writer, and the phone's kernel data is
 * non-cacheable, where exclusive instructions are not guaranteed to work. */
static uint32_t epoch_low, epoch_high;

static void epoch_advance(void) {
    __asm__ volatile("dmb ish" ::: "memory");
    uint32_t low = __atomic_load_n(&epoch_low, __ATOMIC_RELAXED) + 1;
    if (!low) {
        __atomic_store_n(&epoch_high, __atomic_load_n(&epoch_high, __ATOMIC_RELAXED) + 1,
                         __ATOMIC_RELAXED);
        __asm__ volatile("dmb ish" ::: "memory");
    }
    __atomic_store_n(&epoch_low, low, __ATOMIC_RELAXED);
}

uint64_t vm_epoch(void) {
    uint32_t high, low;
    do {
        high = __atomic_load_n(&epoch_high, __ATOMIC_RELAXED);
        __asm__ volatile("dmb ish" ::: "memory");
        low = __atomic_load_n(&epoch_low, __ATOMIC_RELAXED);
        __asm__ volatile("dmb ish" ::: "memory");
    } while (high != __atomic_load_n(&epoch_high, __ATOMIC_RELAXED));
    return (uint64_t)high << 32 | low;
}

static void vm_switch(const struct vm_space *space); /* NULL selects the kernel. */

void vm_enter(const struct vm_space *space, bool flush) {
    if (flush) {
        vm_switch(space);
        return;
    }
    __asm__ volatile("dsb sy\n mcr p15, 0, %0, c2, c0, 0\n isb"
                     :
                     : "r"((uintptr_t)space->root)
                     : "memory");
}

void vm_leave(void) {
    /* Keep the user entries: user and kernel addresses are disjoint, the kernel
     * reaches user memory only through physical aliases, and entering another
     * space or a later epoch flushes them first. */
    __asm__ volatile("dsb sy\n mcr p15, 0, %0, c2, c0, 0\n isb"
                     :
                     : "r"((uintptr_t)kernel_space.root)
                     : "memory");
}

static void vm_switch(const struct vm_space *space) {
    uintptr_t root = (uintptr_t)(space ? space : &kernel_space)->root;
    uint32_t zero = 0;
    /* The ISB makes the new TTBR0 visible to table walks before the TLB is
     * invalidated, so walks of the old tables cannot refill it afterwards. */
    __asm__ volatile("dsb sy\n mcr p15, 0, %0, c2, c0, 0\n isb\n"
                     "mcr p15, 0, %1, c8, c7, 0\n dsb sy\n isb"
                     :
                     : "r"(root), "r"(zero)
                     : "memory");
}

static bool enable_translation(bool secondary) {
    uint32_t control = arm_cache_control(), zero = 0, domain = 1, cpacr = 0x00f00000;
    /* Boot contract: MMU and data cache off; on CPU 0 translation may be on
     * already, behind the Nexus 5's early tables (arch/arm/boot_cache.c),
     * which map the kernel where these do. */
    if ((control & 4) || ((control & 1) && (secondary || !arm_boot_cache_early()))) return false;
#ifndef SOS2_BOARD_NEXUS5
    /* ARM32 A15/A53 ACTLR.SMP, matching the vendor's __v7_ca53mp_setup.
     * Shared user mappings participate in CPU coherency; DMA is kept separate.
     * Krait (the Nexus 5) has no such bit: Linux's __krait_proc_info setup
     * leaves its implementation-defined ACTLR alone. */
    uint32_t auxiliary;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(auxiliary));
    if (!(auxiliary & 64)) {
        auxiliary |= 64;
        __asm__ volatile("mcr p15, 0, %0, c1, c0, 1\n isb" : : "r"(auxiliary) : "memory");
    }
#endif
    if (secondary) arm_dcache_invalidate_local();
    else arm_dcache_invalidate_all();
    arm_instruction_sync();
    /* CNTKCTL.PL0VCTEN only: user code may read the virtual count and its
     * frequency (the kernel's monotonic clock) without a syscall. Timer
     * registers and the physical count stay privileged. */
    uint32_t user_counter = 2;
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2\n"  /* TTBCR.N=0, EAE=0 */
                     "mcr p15, 0, %2, c14, c1, 0\n" /* CNTKCTL */
                     "mcr p15, 0, %1, c3, c0, 0"
                     :
                     : "r"(zero), "r"(domain), "r"(user_counter)
                     : "memory");
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 2\n isb" : : "r"(cpacr) : "memory");
    vm_switch(0);
    /* AFE, TRE and WXN off: the JIT window is writable and executable; vm_map
     * and vm_protect keep W^X everywhere else, and every kernel RW mapping is XN. */
    control &= ~((1u << 29) | (1u << 28) | (1u << 19));
    /* XP, I-cache, branch prediction, D-cache, MMU. */
    control |= (1u << 23) | (1u << 12) | (1u << 11) | 5;
    __asm__ volatile("dsb sy\n mcr p15, 0, %0, c1, c0, 0\n isb" : : "r"(control) : "memory");
    return true;
}

bool vm_secondary_init(void) { return enable_translation(true); }

bool vm_init(const struct platform_devices *d, uintptr_t dtb) {
    const struct arm_machine *machine = arm_machine_get();
    uintptr_t start = (uintptr_t)__image_start, end = (uintptr_t)__image_end;
    uintptr_t pool_start = (uintptr_t)__page_pool_start, pool_end = (uintptr_t)__page_pool_end;
#ifdef SOS2_PHONE
    if (runtime_files_size()) {
        size_t size;
        if (!phone_browser_arena(d, dtb, start, end, &pool_start, &size)) return false;
        pool_end = pool_start + size;
    }
#endif
    if (!machine->device_count || end <= start || !fdt_range_in_ram(d, start, end - start) ||
        dtb > UINT32_MAX - d->dtb_size || (start < USER_END && end > USER_BASE))
        return false;
    if (pool_end <= pool_start || !fdt_range_in_ram(d, pool_start, pool_end - pool_start))
        return false;
    if (pool_start < USER_END && pool_end > USER_BASE) return false;
    for (size_t i = 0; i < d->reserved_region_count; ++i) {
        const struct memory_region *r = &d->reserved_regions[i];
        if (r->size > UINT64_MAX - r->start || (start < r->start + r->size && r->start < end) ||
            (pool_start < r->start + r->size && r->start < pool_end))
            return false;
    }
    if (dtb < end && start < dtb + d->dtb_size) return false;
    if (dtb < pool_end && pool_start < dtb + d->dtb_size) return false;
    for (unsigned i = 0; i < machine->device_count; ++i) {
        const struct arm_mapping *r = &machine->devices[i];
        uint64_t finish = (uint64_t)r->start + r->size;
        if ((r->start < end && start < finish) || (r->start < pool_end && pool_start < finish))
            return false;
    }
#ifdef SOS2_PHONE
    uintptr_t fb = (uintptr_t)d->framebuffer.pixels;
    uint64_t fb_end = (uint64_t)fb + d->framebuffer_size;
    if (!d->has_framebuffer || !d->framebuffer_size || d->framebuffer_size > UINT32_MAX ||
        !fdt_range_in_ram(d, fb, d->framebuffer_size) || fb_end > UINT32_MAX ||
        (fb < USER_END && fb_end > USER_BASE) || (fb < end && start < fb_end) ||
        (fb < pool_end && pool_start < fb_end) || (fb < dtb + d->dtb_size && dtb < fb_end))
        return false;
#endif
    uintptr_t resources = runtime_files_base();
    uint32_t resource_size = runtime_files_size();
    if (resource_size && ((resources < end && start < resources + resource_size) ||
                          (resources < pool_end && pool_start < resources + resource_size)))
        return false;
    if (!pages_init((void *)pool_start, pool_end - pool_start)) return false;
    memset(kernel_root, 0, sizeof(kernel_root));
    if (!kernel_map(start, (uintptr_t)__text_end - start, CACHED | KERNEL_RO) ||
        !kernel_map((uintptr_t)__text_end, __rodata_end - __text_end, CACHED | KERNEL_RO | XN) ||
        !kernel_map((uintptr_t)__rodata_end, __image_end - __rodata_end, NORMAL | KERNEL_RW | XN))
        return false;
    if (!kernel_map((uintptr_t)__cpu_private_start, __cpu_private_end - __cpu_private_start,
                    CACHED | KERNEL_RW | XN) ||
        !kernel_map((uintptr_t)__stack_guard, __exception_stack_top - __stack_guard,
                    CACHED | KERNEL_RW | XN))
        return false;
    if (!kernel_map(pool_start, pool_end - pool_start, NORMAL | KERNEL_RW | XN)) return false;
    /* Cached: the resources are immutable from here. They arrived with the
     * MMU off (a ramdisk, or read from the eMMC), or were decoded through
     * boot_cache.c, which cleans them to the point of coherency; nothing
     * writes them again, and this is their only mapping. The engine block
     * is copied from them. */
    if (resource_size && !kernel_map(resources, resource_size, CACHED | KERNEL_RO | XN))
        return false;
    for (unsigned i = 0; i < machine->device_count; ++i) {
        const struct arm_mapping *r = &machine->devices[i];
        if (!kernel_map(r->start, r->size, DEVICE | KERNEL_RW | XN)) return false;
    }
#ifdef SOS2_PHONE
    if (!kernel_map(fb, (uint32_t)d->framebuffer_size, NORMAL | KERNEL_RW | XN)) return false;
#endif
    kernel_unmap((uintptr_t)__stack_guard);
    kernel_unmap((uintptr_t)__irq_guard);
    kernel_unmap((uintptr_t)__abort_guard);
    kernel_unmap((uintptr_t)__undef_guard);
    kernel_unmap((uintptr_t)kernel_io_stack);
    for (unsigned cpu = 1; cpu < ARM_CPU_MAX; ++cpu)
        for (unsigned page = 0; page < 8; page += 2)
            kernel_unmap((uintptr_t)arm_cpu_stacks[cpu - 1] + page * PAGE_SIZE);
    if (!enable_translation(false)) return false;
    return true;
}

static uintptr_t shared_base;
static unsigned shared_pages, shared_users;

static bool shared_page(uintptr_t physical) {
    return shared_pages && physical - shared_base < (uintptr_t)shared_pages * PAGE_SIZE;
}

/* The block holds ~90 MiB of engine code. The service worker builds and
 * frees it, yielding every 64 KiB so the desktop keeps running meanwhile. */
void *vm_shared_create(unsigned pages) {
    if (shared_pages || !pages) return 0;
    unsigned char *block = pages_reserve(pages, 1);
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

bool vm_shared_active(void) { return shared_pages != 0; }

void vm_shared_trim(void) {
    if (!shared_pages || shared_users) return;
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
    s->root = pages_alloc(4, 4);
    if (!s->root) return false;
    memcpy(s->root, kernel_root, sizeof(kernel_root));
    return true;
}

static uint32_t *user_table(const struct vm_space *s, uint32_t address) {
    if (!s->root || address < USER_BASE || address >= USER_END) return 0;
    uint32_t descriptor = s->root[address >> 20];
    if (!(descriptor & 1)) return 0;
    return (uint32_t *)(uintptr_t)(descriptor & 0xfffffc00u);
}

static void release_empty_table(struct vm_space *s, uint32_t address) {
    uint32_t *table = user_table(s, address);
    if (!table) return;
    for (unsigned i = 0; i < 256; ++i)
        if (table[i]) return;
    s->root[address >> 20] = 0;
    if (!pages_free(table, 1)) panic("user table ownership corrupt");
}

/* On the service worker: a page of hundreds of megabytes frees for a second
 * or more, so the desktop runs every 16 pages, as in vm_shared_trim. */
void vm_destroy(struct vm_space *s) {
    if (s->root) {
        unsigned freed = 0;
        for (uint32_t address = USER_BASE; address < USER_END; address += 0x100000) {
            uint32_t *table = user_table(s, address);
            if (!table) continue;
            for (unsigned i = 0; i < 256; ++i)
                if (table[i] & SMALL) {
                    uintptr_t address = table[i] & ~4095u;
                    table[i] = 0;
                    if (!shared_page(address)) free_user_page(address);
                    if ((++freed & 15) == 0) kernel_io_wait();
                }
            if (!pages_free(table, 1)) panic("user table ownership corrupt");
        }
        if (!pages_free(s->root, 4)) panic("address space ownership corrupt");
        epoch_advance();
        if (s->shared) {
            if (!shared_users) panic("shared page block reference count");
            --shared_users;
            vm_shared_trim();
        }
    }
    *s = (struct vm_space){0};
}

bool vm_map(struct vm_space *s, user_ptr address, unsigned permissions) {
    if (!s->root || (address & (PAGE_SIZE - 1)) || address < USER_BASE || address >= USER_END ||
        s->mapped_pages >= USER_PAGE_QUOTA || (permissions & ~7u) ||
        (permissions && (!(permissions & VM_READ) || !user_permissions(address, permissions))))
        return false;
    uint32_t *table = user_table(s, address);
    if (!table) {
        table = pages_alloc(1, 1);
        if (!table) return false;
        /* Zeroed before it is linked: a mapping may add tables while the
         * process's other threads run (kernel/protection/process.c). PXN
         * prevents privileged instruction fetches from user mappings. */
        __asm__ volatile("dsb ishst" ::: "memory");
        s->root[address >> 20] = (uintptr_t)table | 1 | 4;
    }
    uint32_t *pte = &table[(address >> 12) & 255];
    if (*pte) return false;
    void *page = pages_alloc(1, 1);
    if (!page) {
        release_empty_table(s, address);
        return false;
    }
    /* user_alias cleans the page to the point of coherency, as code needs. */
    user_alias((uintptr_t)page, true);
    *pte = user_pte((uintptr_t)page, permissions);
    ++s->mapped_pages;
    if (permissions & VM_EXEC) epoch_advance();
    return true;
}

bool vm_map_shared(struct vm_space *s, user_ptr address, unsigned page, unsigned permissions) {
    if (!s->root || (address & (PAGE_SIZE - 1)) || address < USER_BASE || address >= USER_END ||
        page >= shared_pages || !(permissions & VM_READ) || (permissions & ~(VM_READ | VM_EXEC)))
        return false;
    uint32_t *table = user_table(s, address);
    if (!table) {
        table = pages_alloc(1, 1);
        if (!table) return false;
        s->root[address >> 20] = (uintptr_t)table | 1 | 4;
    }
    uint32_t *pte = &table[(address >> 12) & 255];
    if (*pte) return false;
    *pte = (shared_base + (uintptr_t)page * PAGE_SIZE) | SMALL | CACHED | SHARED | USER_RO |
           ((permissions & VM_EXEC) ? 0 : XN);
    if (!s->shared) {
        s->shared = true;
        ++shared_users;
    }
    return true;
}

void *vm_resolve(const struct vm_space *s, user_ptr address, unsigned permissions) {
    uint32_t *table = user_table(s, address);
    if (!table) return 0;
    uint32_t pte = table[(address >> 12) & 255];
    if (!(pte & SMALL) || (pte & (3 << 4)) == (1 << 4) ||
        ((permissions & VM_WRITE) && (pte & (1 << 9))) || ((permissions & VM_EXEC) && (pte & XN)))
        return 0;
    return (void *)(uintptr_t)((pte & ~4095u) | (address & 4095));
}

/* SYS_ICACHE_SYNC, on the CPU whose TTBR0 holds s's tables: the call trapped
 * from that process's user mode on this CPU. Code just written into the JIT
 * window becomes visible to instruction fetch on every core: data lines are
 * cleaned, then instruction lines and branch predictors invalidated, all
 * broadcast to the inner shareable domain. It takes the user address, since
 * the A53's instruction cache is virtually indexed and the kernel's physical
 * alias would miss its lines. Every page is checked first: a fault here would
 * stop the CPU. Interrupts are masked throughout, so the length is capped
 * (the SDK splits longer ranges). */
int32_t vm_icache_sync(const struct vm_space *s, user_ptr address, uint32_t size) {
    enum { SYNC_MAX = 256 * 1024 };

    if (!size) return 0;
    if (!s || size > SYNC_MAX || address < USER_JIT_BASE || address >= USER_JIT_END ||
        size > USER_JIT_END - address)
        return USER_EINVAL;
    user_ptr end = address + size;
    for (user_ptr page = address & ~(user_ptr)(PAGE_SIZE - 1); page < end; page += PAGE_SIZE)
        if (!vm_resolve(s, page, VM_READ)) return USER_EINVAL;
    uint32_t ctr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
    uint32_t data_line = 4u << ((ctr >> 16) & 15), code_line = 4u << (ctr & 15);
    /* Clean/invalidate to PoC, as clean_user_page, for the A53's errata. */
    for (user_ptr at = address & ~(data_line - 1); at < end; at += data_line)
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(at) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (user_ptr at = address & ~(code_line - 1); at < end; at += code_line)
        __asm__ volatile("mcr p15, 0, %0, c7, c5, 1" : : "r"(at) : "memory"); /* ICIMVAU */
    __asm__ volatile("mcr p15, 0, %0, c7, c1, 6\n dsb ish\n isb"              /* BPIALLIS */
                     :
                     : "r"(0)
                     : "memory");
    return 0;
}

bool vm_mapped(const struct vm_space *s, user_ptr address) {
    uint32_t *table = user_table(s, address);
    return table && (table[(address >> 12) & 255] & SMALL);
}

bool vm_protect(struct vm_space *s, user_ptr address, unsigned permissions) {
    if ((address & (PAGE_SIZE - 1)) || (permissions & ~7u) ||
        !user_permissions(address, permissions) || (permissions && !(permissions & VM_READ)))
        return false;
    uint32_t *table = user_table(s, address);
    if (!table || !(table[(address >> 12) & 255] & SMALL)) return false;
    uint32_t *pte = &table[(address >> 12) & 255];
    if (shared_page(*pte & ~4095u)) return false;
    if (permissions & VM_EXEC) clean_user_page(*pte & ~4095u);
    *pte = user_pte(*pte & ~4095u, permissions);
    epoch_advance();
    return true;
}

bool vm_clear(struct vm_space *s, user_ptr address) {
    if (address & (PAGE_SIZE - 1)) return false;
    uint32_t *table = user_table(s, address);
    if (!table || !(table[(address >> 12) & 255] & SMALL) ||
        shared_page(table[(address >> 12) & 255] & ~4095u))
        return false;
    memset((void *)(uintptr_t)(table[(address >> 12) & 255] & ~4095u), 0, PAGE_SIZE);
    if (!(table[(address >> 12) & 255] & XN)) {
        clean_user_page(table[(address >> 12) & 255] & ~4095u);
        epoch_advance();
    }
    return true;
}

bool vm_copy_from_user(const struct vm_space *s, void *out, user_ptr address, size_t size) {
    if (size && (size - 1 > UINT32_MAX - address)) return false;
    unsigned char *destination = out;
    while (size) {
        void *source = vm_resolve(s, address, VM_READ);
        if (!source) return false;
        size_t n = PAGE_SIZE - (address & (PAGE_SIZE - 1));
        if (n > size) n = size;
        memcpy(destination, source, n);
        destination += n;
        address += (uint32_t)n;
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
        address += (uint32_t)n;
        size -= n;
    }
    return true;
}

bool vm_release(struct vm_space *s, user_ptr address, unsigned count) {
    if (!count || (address & (PAGE_SIZE - 1)) || address >= USER_END ||
        count > (USER_END - address) / PAGE_SIZE ||
        !allocation_range(address, address + count * PAGE_SIZE))
        return false;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t at = address + i * PAGE_SIZE, *table = user_table(s, at);
        uint32_t pte = table ? table[(at >> 12) & 255] : 0;
        if (!(pte & SMALL) || shared_page(pte & ~4095u)) return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        uint32_t at = address + i * PAGE_SIZE;
        uint32_t *pte = &user_table(s, at)[(at >> 12) & 255];
        uintptr_t physical = *pte & ~4095u;
        *pte = 0;
        free_user_page(physical);
        --s->mapped_pages;
    }
    uint32_t last = (address + (count - 1) * PAGE_SIZE) & ~0xfffffu;
    for (uint32_t at = address & ~0xfffffu;; at += 0x100000) {
        release_empty_table(s, at);
        if (at == last) break;
    }
    epoch_advance(); /* The next entry to any space flushes the TLB. */
    return true;
}

user_ptr vm_allocate_range(struct vm_space *s, unsigned count, unsigned alignment, user_ptr lower,
                           user_ptr upper, unsigned permissions) {
    if (!allocation_range(lower, upper) || ((lower | upper) & (PAGE_SIZE - 1)) ||
        count > (upper - lower) / PAGE_SIZE || !alignment || (alignment & (alignment - 1)) ||
        alignment > (upper - lower) / PAGE_SIZE)
        return 0;
    if (!s->root || !count || count > USER_PAGE_QUOTA - s->mapped_pages) return 0;
    uint32_t step = (uint32_t)alignment * PAGE_SIZE;
    uint32_t start = (lower + step - 1) & ~(step - 1);
    unsigned free_run = 0;
    for (uint32_t at = start; at < upper; at += PAGE_SIZE) {
        if (vm_mapped(s, at)) {
            /* The next run may start only at the next multiple of step. */
            free_run = 0;
            start = (at + step) & ~(step - 1);
            at = start - PAGE_SIZE;
            continue;
        }
        if (++free_run < count) continue;
        unsigned mapped = 0;
        for (; mapped < count; ++mapped)
            if (!vm_map(s, start + (uint32_t)mapped * PAGE_SIZE, permissions)) break;
        if (mapped == count) return start;
        if (mapped && !vm_release(s, start, mapped)) panic("heap rollback failed");
        return 0;
    }
    return 0;
}
