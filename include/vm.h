#ifndef SOS2_VM_H
#define SOS2_VM_H
#include "fdt.h"
#include "page_alloc.h"
#include "user_abi.h"
#include "arch/cpu.h"

enum vm_permission { VM_READ = 1, VM_WRITE = 2, VM_EXEC = 4 };

struct vm_space {
    pte_t *root;
    unsigned mapped_pages; /* Private pages only; shared pages are not owned. */
    unsigned promised;     /* Pages promised to reserved ranges, not mapped yet. */
    pte_t *folded;         /* ARM32: the page tables that private sections stand for. */
    bool shared;           /* Maps the shared read-only block (one reference). */
};

bool vm_init(const struct platform_devices *devices, uintptr_t dtb);
bool vm_secondary_init(void);
/* Supervisor-only, cached CPU buffers. Never hand these pages to DMA. Allocate
 * and release on the boot CPU; the caller must quiesce all users before free. */
void *vm_cpu_alloc(unsigned count);
void vm_cpu_free(void *buffer, unsigned count);
/* One immutable block of read-only pages shared by every address space that
 * maps it (the engine's code and read-only data), freed with its last user.
 * Create, fill through the returned cached pointer, publish, then map pages
 * read-only. Shared pages cannot be unmapped, protected or cleared. */
void *vm_shared_create(unsigned pages);
void vm_shared_publish(void);
/* The shared block without a copy: pages of the immutable resources that
 * already hold it, where they line up for sections or blocks, else false. */
bool vm_shared_adopt(const void *data, unsigned pages);
bool vm_shared_active(void);
void vm_shared_trim(void); /* Free a block that no address space maps. */
/* Maps some of count shared pages from page at address, in 1 MiB sections
 * (2 MiB blocks on AArch64) where address and block line up, as a table's
 * 4 KiB entries otherwise; returns how many, 0 for none. */
unsigned vm_map_shared(struct vm_space *space, user_ptr address, unsigned page, unsigned count,
                       unsigned permissions);
bool vm_create(struct vm_space *space);
void vm_destroy(struct vm_space *space);
bool vm_map(struct vm_space *space, user_ptr address, unsigned permissions);
bool vm_mapped(const struct vm_space *space, user_ptr address);
#ifndef __aarch64__
/* SYS_ICACHE_SYNC (arch/arm/mmu.c): on the CPU running space's process. */
int32_t vm_icache_sync(const struct vm_space *space, user_ptr address, uint32_t size);
#endif
bool vm_protect(struct vm_space *space, user_ptr address, unsigned permissions);
bool vm_clear(struct vm_space *space, user_ptr address);
void *vm_resolve(const struct vm_space *space, user_ptr address, unsigned permissions);

/* Validate the complete range before an operation consumes or modifies it.
 * The caller must prevent its address space from changing until it finishes. */
static inline bool vm_accessible(const struct vm_space *space, user_ptr address, size_t size,
                                 unsigned permissions) {
    if (size && size - 1 > (user_ptr)-1 - address) return false;
    for (size_t offset = 0; offset < size;) {
        if (!vm_resolve(space, address + (user_ptr)offset, permissions)) return false;
        size_t n = PAGE_SIZE - ((address + offset) & (PAGE_SIZE - 1));
        offset += n < size - offset ? n : size - offset;
    }
    return true;
}

bool vm_copy_from_user(const struct vm_space *space, void *out, user_ptr address, size_t size);
bool vm_copy_to_user(const struct vm_space *space, user_ptr address, const void *data, size_t size);
/* Anonymous, zero-filled pages in the heap or the JIT window, with permissions
 * (0: none, kernel-only), starting at a multiple of alignment pages (a power
 * of two): the first such run in [lower, upper). vm_release unmaps count pages
 * there, all mapped and the process's own, and advances the epoch once. */
user_ptr vm_allocate_range(struct vm_space *space, unsigned count, unsigned alignment,
                           user_ptr lower, user_ptr upper, unsigned permissions);
bool vm_release(struct vm_space *space, user_ptr address, unsigned count);
/* A big mapping or unmapping in steps, a slice of pages per kernel loop turn
 * (kernel/protection/process.c). Reserving claims the range at once, with
 * entries that fault but keep other mappings out, counts its pages mapped and
 * promises them (page_alloc.h), so filling it cannot fail. ARM32 fills whole
 * aligned megabytes of it as sections when memory has such runs, until a
 * change within one splits it.
 * vm_unmap_begin, with the space quiescent, replaces the entries with ones
 * that fault and keep their pages' addresses, and advances the epoch;
 * vm_unmap_page then frees each page, and vm_unmap_end, quiescent again
 * since CPUs may hold walks through them, the emptied tables. */
bool vm_occupied(const struct vm_space *space, user_ptr address);
bool vm_reserve(struct vm_space *space, user_ptr address, unsigned count, unsigned permissions);
user_ptr vm_reserve_range(struct vm_space *space, unsigned count, unsigned alignment,
                          user_ptr lower, user_ptr upper, unsigned permissions);
void vm_fill_reserved(struct vm_space *space, user_ptr address, unsigned permissions);
bool vm_unmap_begin(struct vm_space *space, user_ptr address, unsigned count);
void vm_unmap_page(struct vm_space *space, user_ptr address);
void vm_unmap_end(struct vm_space *space, user_ptr address, unsigned count);
/* User slices: enter flushes only when asked; leave never flushes. */
uint64_t vm_epoch(void);
void vm_enter(const struct vm_space *space, bool flush);
void vm_leave(void);
#endif
