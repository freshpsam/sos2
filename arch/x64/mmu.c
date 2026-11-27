#include "vm.h"
#include "platform.h"
#include "kernel_io.h"
#include "boot_memory.h"
#include "runtime_files.h"
#include "machine.h"
#include "boot_progress.h"
#include "x86.h"

/* x64 4-level paging with 4 KiB pages, shaped like arch/arm64/mmu.c:
 * each address space has a PML4 whose entry 0 (the low 512 GiB) points to
 * its own PDPT, which plays arm64's level 1 root: entries 0-3 the shared
 * identity map of the kernel and RAM below 4 GiB (global, supervisor-only),
 * entries from USER_BASE its own user tables. PDPT, PD and PT entries cover
 * 1 GiB, 2 MiB and a page, as arm64's levels 1 to 3 do, so index_at is the
 * same. Devices may lie anywhere in the physical address space: those past
 * the first 512 GiB are mapped through PML4 entries of their own, which
 * every space shares. Every page is write-back, devices' MMIO uncached
 * (PAT's PCD|PWT) and a framebuffer write-combining (PWT alone): x86's DMA
 * is cache-coherent, so there are no uncached aliases, attribute changes or
 * cache maintenance. Writable memory is never executable outside
 * the JIT window; SMEP keeps the kernel from running user pages, and the
 * kernel reaches user memory only through its own alias (vm_resolve), which
 * SMAP allows. The shared engine is mapped with 2 MiB pages, as on arm64.
 * Kernel mappings never change after vm_init (but for the SMP
 * trampoline's page, on the boot CPU before the others start), so no TLB
 * shootdown is needed: user changes follow the mapping epoch, which makes
 * every CPU write CR3 before running a space again. */
#define PRESENT     (1ull << 0)
#define WRITABLE    (1ull << 1)
#define USER        (1ull << 2)
#define UNCACHED    (3ull << 3) /* PCD | PWT */
#define COMBINING   (1ull << 3) /* PWT: PAT entry 1, write-combining (cpu.c) */
#define ACCESSED    (1ull << 5)
#define DIRTY       (1ull << 6)
#define LARGE       (1ull << 7) /* a 2 MiB page, in a PD entry */
#define GLOBAL      (1ull << 8)
#define NX          (1ull << 63)
#define ADDRESS     0x000ffffffffff000ull
#define TABLE       (PRESENT | WRITABLE)
#define USER_TABLE  (PRESENT | WRITABLE | USER)
#define KERNEL_RO   NX
#define KERNEL_RX   0ull
#define KERNEL_RW   (WRITABLE | NX)
#define BLOCK_PAGES 512
#define ENTRIES     512
#define L3_SPAN     0x200000ull /* bytes one page table maps */
/* Entries without PRESENT, which the CPU ignores, mark ranges in progress
 * (include/vm.h), as arm64's invalid ones: RESERVED awaits its page,
 * FREEING holds the page its retired translation mapped, RETIRED held one
 * freed since. */
#define RESERVED (1ull << 2)
#define FREEING  (1ull << 3)
#define RETIRED  (RESERVED | FREEING)
_Static_assert(USER_BASE % 0x40000000 == 0 && USER_END % 0x40000000 == 0 &&
                   USER_END <= (1ull << 39),
               "user window: whole PDPT entries inside the PML4's first");

static pte_t kernel_pml4[ENTRIES] __attribute__((aligned(4096)));
static pte_t kernel_root[ENTRIES] __attribute__((aligned(4096)));
static struct vm_space kernel_space = {.root = kernel_pml4};
extern char __image_start[], __image_end[], __text_end[], __rodata_end[], __stack_guard[];

static unsigned index_at(uint64_t address, unsigned level) {
    return (unsigned)(address >> (39 - 9 * level)) & (ENTRIES - 1);
}

/* A space's PDPT, PML4 entry 0's table. */
static pte_t *pdpt(const struct vm_space *s) { return (pte_t *)(uintptr_t)(s->root[0] & ADDRESS); }

/* The next-level table an entry points to, allocated (zeroed) if absent and
 * wanted. */
static pte_t *next_table(pte_t *entry, bool allocate, pte_t flags) {
    if (!(*entry & PRESENT)) {
        if (!allocate || *entry) return 0;
        pte_t *table = pages_alloc(1, 1);
        if (!table) return 0;
        /* Zeroed before it is linked: a mapping may add tables while the
         * process's other threads run (kernel/protection/process.c). */
        __atomic_store_n(entry, (uintptr_t)table | flags, __ATOMIC_RELEASE);
    }
    if (*entry & LARGE) return 0; /* A 2 MiB page has no table. */
    return (pte_t *)(uintptr_t)(*entry & ADDRESS);
}

/* The page table that maps address under a PDPT. */
static pte_t *leaf_table(pte_t *root, uint64_t address, bool allocate, pte_t flags) {
    pte_t *level2 = next_table(&root[index_at(address, 1)], allocate, flags);
    return level2 ? next_table(&level2[index_at(address, 2)], allocate, flags) : 0;
}

/* The kernel's PDPT for an address: kernel_root for the first 512 GiB,
 * which each space copies, else a shared one under its own PML4 entry. */
static pte_t *kernel_pdpt(uint64_t address) {
    unsigned top = (unsigned)(address >> 39) & (ENTRIES - 1);
    return top ? next_table(&kernel_pml4[top], true, TABLE) : kernel_root;
}

static bool kernel_map(uintptr_t start, uintptr_t size, pte_t flags) {
    if (!size || start > UINTPTR_MAX - (size - 1) || start + (size - 1) >= (1ull << 48))
        return false;
    uintptr_t last = (start + size - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    for (uintptr_t p = start & ~(uintptr_t)(PAGE_SIZE - 1);; p += PAGE_SIZE) {
        pte_t *root = kernel_pdpt(p);
        pte_t *table = root ? leaf_table(root, p, true, TABLE) : 0;
        if (!table) return false;
        table[index_at(p, 3)] = p | PRESENT | ACCESSED | DIRTY | GLOBAL | flags;
        if (!(p & 0xfffff)) boot_progress_poll();
        if (p == last) return true;
    }
}

static void kernel_unmap(uintptr_t address) {
    pte_t *table = leaf_table(kernel_root, address, true, TABLE);
    if (!table) panic("kernel page table allocation failed");
    table[index_at(address, 3)] = 0;
}

void *vm_cpu_alloc(unsigned count) { return pages_alloc(count, 1); }

void vm_cpu_free(void *buffer, unsigned count) {
    if (!pages_free(buffer, count)) panic("CPU buffer ownership corrupt");
}

static void free_user_page(uintptr_t address) {
    if (!pages_free((void *)address, 1)) panic("user page ownership corrupt");
}

/* Mapping epoch, as in arch/arm/mmu.c: advanced (by the boot CPU, while that
 * space is quiescent) whenever a user mapping is removed, downgraded or made
 * executable, user code changes, or an address space is destroyed. */
static uint64_t epoch;

static void epoch_advance(void) {
    __atomic_store_n(&epoch, __atomic_load_n(&epoch, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
}

uint64_t vm_epoch(void) { return __atomic_load_n(&epoch, __ATOMIC_ACQUIRE); }

static void load_root(const pte_t *root) {
    __asm__ volatile("movq %0, %%cr3" : : "r"((uintptr_t)root) : "memory");
}

/* Writing CR3 drops every non-global translation, so entering a space always
 * starts from its tables; the kernel's own are global and stay. */
void vm_enter(const struct vm_space *space, bool flush) {
    (void)flush;
    load_root(space->root);
}

void vm_leave(void) { load_root(kernel_space.root); }

bool vm_secondary_init(void) { return true; } /* The trampoline loaded the kernel's root. */

uintptr_t x86_kernel_root(void) { return (uintptr_t)kernel_pml4; }

void x86_trampoline_page(bool writable) {
    pte_t *table = leaf_table(kernel_root, X86_TRAMPOLINE, true, TABLE);
    if (!table) panic("trampoline page table allocation failed");
    table[index_at(X86_TRAMPOLINE, 3)] =
        X86_TRAMPOLINE | PRESENT | ACCESSED | DIRTY | GLOBAL | (writable ? KERNEL_RW : KERNEL_RX);
    __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)X86_TRAMPOLINE) : "memory");
}

/* boot.S's tables, which carry the kernel until vm_init, and the few more
 * that early device windows need. */
extern pte_t boot_pml4[ENTRIES];
static pte_t boot_tables[8][ENTRIES] __attribute__((aligned(4096)));
static unsigned boot_tables_used;

static pte_t *boot_table(pte_t *entry) {
    if (!(*entry & PRESENT)) {
        if (boot_tables_used == sizeof(boot_tables) / sizeof(boot_tables[0])) return 0;
        *entry = (uintptr_t)boot_tables[boot_tables_used++] | TABLE;
    }
    return *entry & LARGE ? 0 : (pte_t *)(uintptr_t)(*entry & ADDRESS);
}

bool machine_early_map(uint64_t start, uint64_t size) {
    if (!size || start > (1ull << 48) - size) return false;
    for (uint64_t at = start & ~(L3_SPAN - 1); at < start + size; at += L3_SPAN) {
        pte_t *pdpt = boot_table(&boot_pml4[(at >> 39) & (ENTRIES - 1)]);
        pte_t *pd = pdpt ? boot_table(&pdpt[index_at(at, 1)]) : 0;
        if (!pd) return false;
        pd[index_at(at, 2)] = at | PRESENT | WRITABLE | LARGE | UNCACHED;
    }
    return true;
}

static bool overlap(uint64_t start, uint64_t end, uint64_t other_start, uint64_t other_end) {
    return start < other_end && other_start < end;
}

bool vm_init(const struct platform_devices *d, uintptr_t dtb) {
    const struct machine *machine = machine_get();
    uintptr_t start = (uintptr_t)__image_start, end = (uintptr_t)__image_end;
    uintptr_t resources = runtime_files_base();
    uint32_t resource_size = runtime_files_size();
    /* The page pool is every large free stretch of RAM below 4 GiB the
     * memory map reports, the reservations between them withheld. */
    struct memory_region pool[BOOT_POOL_REGIONS];
    unsigned regions = boot_memory_pool(d, dtb, start, end, resources, resource_size,
                                        (uint64_t)PAGE_POOL_PAGES * PAGE_SIZE, pool);
    if (!regions || end <= start || !fdt_range_in_ram(d, start, end - start) ||
        overlap(start, end, USER_BASE, USER_END))
        return false;
    if (resource_size && overlap(resources, resources + resource_size, start, end)) return false;
    for (size_t i = 0; i < d->reserved_region_count; ++i) {
        const struct memory_region *r = &d->reserved_regions[i];
        if (r->size > UINT64_MAX - r->start || overlap(start, end, r->start, r->start + r->size))
            return false;
    }
    uint64_t total = 0;
    for (unsigned n = 0; n < regions; ++n) {
        uint64_t lower = pool[n].start, upper = lower + pool[n].size;
        if ((lower | upper) & (PAGE_SIZE - 1) || upper <= lower ||
            (n && lower < pool[n - 1].start + pool[n - 1].size) ||
            !fdt_range_in_ram(d, lower, upper - lower) ||
            overlap(lower, upper, USER_BASE, USER_END) || overlap(lower, upper, start, end) ||
            (resource_size && overlap(lower, upper, resources, resources + resource_size)))
            return false;
        for (size_t i = 0; i < d->reserved_region_count; ++i) {
            const struct memory_region *r = &d->reserved_regions[i];
            if (overlap(lower, upper, r->start, r->start + r->size)) return false;
        }
        for (unsigned i = 0; i < machine->device_count; ++i) {
            const struct machine_mapping *r = &machine->devices[i];
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
    memset(kernel_pml4, 0, sizeof(kernel_pml4));
    kernel_pml4[0] = (uintptr_t)kernel_root | TABLE;
    if (!kernel_map(start, (uintptr_t)__text_end - start, KERNEL_RX) ||
        !kernel_map((uintptr_t)__text_end, (uintptr_t)(__rodata_end - __text_end), KERNEL_RO) ||
        !kernel_map((uintptr_t)__rodata_end, (uintptr_t)(__image_end - __rodata_end), KERNEL_RW))
        return false;
    for (unsigned n = 0; n < regions; ++n)
        if (!kernel_map(pool[n].start, pool[n].size, KERNEL_RW)) return false;
    if (resource_size && !kernel_map(resources, resource_size, KERNEL_RO)) return false;
    for (unsigned i = 0; i < machine->device_count; ++i) {
        const struct machine_mapping *r = &machine->devices[i];
        if (overlap(r->start, (uint64_t)r->start + r->size, USER_BASE, USER_END) ||
            !kernel_map(r->start, r->size, (r->write_combining ? COMBINING : UNCACHED) | KERNEL_RW))
            return false;
    }
    kernel_unmap((uintptr_t)__stack_guard);
    kernel_unmap((uintptr_t)kernel_io_stack);
    for (unsigned cpu = 1; cpu < CPU_MAX; ++cpu) kernel_unmap((uintptr_t)cpu_stacks[cpu - 1]);
    for (unsigned cpu = 0; cpu < CPU_MAX; ++cpu) {
        kernel_unmap((uintptr_t)x86_exception_stacks[cpu]);
        kernel_unmap((uintptr_t)x86_fatal_stacks[cpu]);
    }
    load_root(kernel_pml4);
    return true;
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
    /* Aligned for 2 MiB pages; unaligned takes 4 KiB ones. */
    unsigned char *block = pages_reserve(pages, BLOCK_PAGES);
    if (!block) block = pages_reserve(pages, 1);
    if (!block) return 0;
    for (unsigned i = 0; i < pages; ++i) {
        memset(block + (size_t)i * PAGE_SIZE, 0, PAGE_SIZE);
        if ((i & 15) == 15) kernel_io_wait();
    }
    shared_base = (uintptr_t)block;
    shared_pages = pages;
    return block;
}

/* Code changes reach instruction fetch by themselves on x86; the epoch has
 * every CPU enter its space afresh. */
void vm_shared_publish(void) { epoch_advance(); }

bool vm_shared_adopt(const void *data, unsigned pages) {
    uintptr_t base = (uintptr_t)data, start = runtime_files_base();
    uint32_t size = runtime_files_size();
    if (shared_pages || !pages || !size || (base & (L3_SPAN - 1)) || base < start ||
        base - start > size || pages > (size - (base - start)) / PAGE_SIZE)
        return false;
    shared_base = base;
    shared_pages = pages;
    shared_adopted = true;
    epoch_advance();
    return true;
}

bool vm_shared_active(void) { return shared_pages != 0; }

void vm_shared_trim(void) {
    if (!shared_pages || shared_users) return;
    if (!shared_adopted && !pages_free((void *)shared_base, shared_pages))
        panic("shared block ownership corrupt");
    shared_adopted = false;
    shared_base = 0;
    shared_pages = 0;
}

bool vm_create(struct vm_space *s) {
    *s = (struct vm_space){0};
    pte_t *root = pages_alloc(1, 1), *level1 = root ? pages_alloc(1, 1) : 0;
    if (!level1) {
        if (root) vm_cpu_free(root, 1);
        return false;
    }
    memcpy(level1, kernel_root, sizeof(kernel_root));
    root[0] = (uintptr_t)level1 | USER_TABLE;
    memcpy(root + 1, kernel_pml4 + 1, (ENTRIES - 1) * sizeof(pte_t));
    s->root = root;
    return true;
}

/* The page table holding a user address's entry, if present. */
static pte_t *user_table(const struct vm_space *s, user_ptr address) {
    if (!s->root || address < USER_BASE || address >= USER_END) return 0;
    return leaf_table(pdpt(s), address, false, 0);
}

/* The 2 MiB page over a user address, or 0. */
static pte_t user_block(const struct vm_space *s, user_ptr address) {
    if (!s->root || address < USER_BASE || address >= USER_END) return 0;
    pte_t *level2 = next_table(&pdpt(s)[index_at(address, 1)], false, 0);
    pte_t entry = level2 ? level2[index_at(address, 2)] : 0;
    return (entry & (PRESENT | LARGE)) == (PRESENT | LARGE) ? entry : 0;
}

static pte_t *new_user_table(struct vm_space *s, user_ptr address) {
    return leaf_table(pdpt(s), address, true, USER_TABLE);
}

static void release_empty_table(struct vm_space *s, user_ptr address) {
    pte_t *level2 = next_table(&pdpt(s)[index_at(address, 1)], false, 0);
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
        pte_t *level1 = pdpt(s);
        unsigned freed = 0;
        for (unsigned i = index_at(USER_BASE, 1); i < index_at(USER_END - 1, 1) + 1; ++i) {
            pte_t *level2 = next_table(&level1[i], false, 0);
            if (!level2) continue;
            for (unsigned j = 0; j < ENTRIES; ++j) {
                if ((level2[j] & (PRESENT | LARGE)) == (PRESENT | LARGE)) {
                    level2[j] = 0; /* the shared engine's: not owned */
                    continue;
                }
                pte_t *table = next_table(&level2[j], false, 0);
                if (!table) continue;
                for (unsigned k = 0; k < ENTRIES; ++k)
                    if ((table[k] & PRESENT) || (table[k] & ~ADDRESS) == FREEING) {
                        uintptr_t physical = table[k] & ADDRESS;
                        table[k] = 0;
                        if (!shared_page(physical)) free_user_page(physical);
                        if ((++freed & 15) == 0) kernel_io_wait();
                    }
                vm_cpu_free(table, 1);
            }
            vm_cpu_free(level2, 1);
        }
        vm_cpu_free(level1, 1);
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
    pte_t entry = physical | PRESENT | ACCESSED | DIRTY;
    if (!permissions) return entry | KERNEL_RW;
    return entry | USER | ((permissions & VM_WRITE) ? WRITABLE : 0) |
           ((permissions & VM_EXEC) ? 0 : NX);
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
    *pte = user_entry((uintptr_t)page, permissions);
    ++s->mapped_pages;
    if (permissions & VM_EXEC) epoch_advance();
    return true;
}

unsigned vm_map_shared(struct vm_space *s, user_ptr address, unsigned page, unsigned count,
                       unsigned permissions) {
    if (!s->root || (address & (PAGE_SIZE - 1)) || address < USER_BASE || address >= USER_END ||
        !count || page >= shared_pages || count > shared_pages - page || !(permissions & VM_READ) ||
        (permissions & ~(VM_READ | VM_EXEC)))
        return 0;
    uintptr_t physical = shared_base + (uintptr_t)page * PAGE_SIZE;
    pte_t entry = user_entry(physical, permissions);
    unsigned mapped = 1;
    pte_t *level2 = next_table(&pdpt(s)[index_at(address, 1)], true, USER_TABLE);
    if (!level2) return 0;
    pte_t *slot = &level2[index_at(address, 2)];
    if (!*slot && count >= BLOCK_PAGES && !((address | physical) & (L3_SPAN - 1))) {
        __atomic_store_n(slot, entry | LARGE, __ATOMIC_RELEASE);
        mapped = BLOCK_PAGES;
    } else {
        pte_t *table = next_table(slot, true, USER_TABLE);
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
    if (!(pte & PRESENT) || !(pte & USER) || ((permissions & VM_WRITE) && !(pte & WRITABLE)) ||
        ((permissions & VM_EXEC) && (pte & NX)))
        return 0;
    return (void *)(uintptr_t)((pte & ADDRESS & ~(span - 1)) | (address & (span - 1)));
}

bool vm_mapped(const struct vm_space *s, user_ptr address) {
    pte_t *table = user_table(s, address);
    return table ? (table[index_at(address, 3)] & PRESENT) != 0 : user_block(s, address) != 0;
}

bool vm_protect(struct vm_space *s, user_ptr address, unsigned permissions) {
    if ((address & (PAGE_SIZE - 1)) || (permissions && !user_permissions(address, permissions)))
        return false;
    pte_t *table = user_table(s, address);
    if (!table || !(table[index_at(address, 3)] & PRESENT)) return false;
    pte_t *pte = &table[index_at(address, 3)];
    uintptr_t physical = *pte & ADDRESS;
    if (shared_page(physical)) return false;
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
    if (!(pte & PRESENT) || shared_page(physical)) return false;
    memset((void *)physical, 0, PAGE_SIZE);
    if (!(pte & NX)) epoch_advance();
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
        if (!(pte & PRESENT) || shared_page(pte & ADDRESS)) return false;
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

/* Bytes from at to the end of its page table's span, or to end if that is
 * nearer. */
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
            if (!(pte & PRESENT) || shared_page(pte & ADDRESS)) return false;
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
