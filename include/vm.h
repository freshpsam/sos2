#ifndef SOS2_VM_H
#define SOS2_VM_H
#include "fdt.h"
#include "page_alloc.h"
#include "user_abi.h"

enum vm_permission { VM_READ = 1, VM_WRITE = 2, VM_EXEC = 4 };

struct vm_space {
    uint32_t *root;
    unsigned mapped_pages; /* Private pages only; shared pages are not owned. */
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
bool vm_shared_active(void);
void vm_shared_trim(void); /* Free a block that no address space maps. */
bool vm_map_shared(struct vm_space *space, uint32_t address, unsigned page, unsigned permissions);
bool vm_create(struct vm_space *space);
void vm_destroy(struct vm_space *space);
bool vm_map(struct vm_space *space, uint32_t address, unsigned permissions);
bool vm_mapped(const struct vm_space *space, uint32_t address);
bool vm_protect(struct vm_space *space, uint32_t address, unsigned permissions);
bool vm_unmap(struct vm_space *space, uint32_t address);
bool vm_clear(struct vm_space *space, uint32_t address);
void *vm_resolve(const struct vm_space *space, uint32_t address, unsigned permissions);

/* Validate the complete range before an operation consumes or modifies it.
 * The caller must prevent its address space from changing until it finishes. */
static inline bool vm_accessible(const struct vm_space *space, uint32_t address, size_t size,
                                 unsigned permissions) {
    if (size && size - 1 > UINT32_MAX - address) return false;
    for (size_t offset = 0; offset < size;) {
        if (!vm_resolve(space, address + (uint32_t)offset, permissions)) return false;
        size_t n = PAGE_SIZE - ((address + offset) & (PAGE_SIZE - 1));
        offset += n < size - offset ? n : size - offset;
    }
    return true;
}

bool vm_copy_from_user(const struct vm_space *space, void *out, uint32_t address, size_t size);
bool vm_copy_to_user(const struct vm_space *space, uint32_t address, const void *data, size_t size);
/* Anonymous, zero-filled, non-executable allocations. Size is in pages. */
uint32_t vm_allocate(struct vm_space *space, unsigned count);
uint32_t vm_allocate_range(struct vm_space *space, unsigned count, uint32_t lower, uint32_t upper);
bool vm_release(struct vm_space *space, uint32_t address, unsigned count);
/* User slices: enter flushes only when asked; leave never flushes. */
uint64_t vm_epoch(void);
void vm_enter(const struct vm_space *space, bool flush);
void vm_leave(void);
#endif
