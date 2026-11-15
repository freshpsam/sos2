#include "elf_load.h"
#include "platform.h"
#include "kernel_io.h"

struct elf_header {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct program_header {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

_Static_assert(sizeof(struct elf_header) == 52, "ELF32 header");
_Static_assert(sizeof(struct program_header) == 32, "ELF32 segment");

/* Every page process runs the same immutable engine image. Its read-only
 * segments (code and constants) are loaded once into the shared block and
 * mapped by each page, instead of copying ~90 MiB per Open. Apps load private
 * copies, so an app opened first cannot take the block. */
static const void *shared_image;
static size_t shared_size;
static uint32_t shared_first[8];

static bool share_segments(const void *image, size_t size, const struct program_header *headers,
                           unsigned count) {
    if (vm_shared_active()) return shared_image == image && shared_size == size;
    unsigned pages = 0;
    for (unsigned i = 0; i < count; ++i) {
        const struct program_header *p = &headers[i];
        if (p->type != 1 || (p->flags & 2)) continue;
        shared_first[i] = pages;
        pages += (p->memsz + PAGE_SIZE - 1) / PAGE_SIZE;
    }
    if (!pages) return false;
    unsigned char *block = vm_shared_create(pages);
    if (!block) return false; /* No contiguous block: load private copies. */
    for (unsigned i = 0; i < count; ++i) {
        const struct program_header *p = &headers[i];
        if (p->type != 1 || (p->flags & 2)) continue;
        /* The block is zeroed; copy file bytes in 64 KiB steps between polls. */
        for (uint32_t offset = 0; offset < p->filesz; offset += 0x10000) {
            uint32_t n = p->filesz - offset;
            if (n > 0x10000) n = 0x10000;
            memcpy(block + (size_t)shared_first[i] * PAGE_SIZE + offset,
                   (const unsigned char *)image + p->offset + offset, n);
            kernel_io_wait();
        }
    }
    vm_shared_publish();
    shared_image = image;
    shared_size = size;
    return true;
}

static bool load_segments(struct vm_space *space, const void *image, size_t size,
                          const struct program_header *headers, unsigned count, bool shared) {
    bool share = shared && share_segments(image, size, headers, count);
    for (unsigned i = 0; i < count; ++i) {
        const struct program_header *p = &headers[i];
        if (p->type != 1) continue;
        unsigned permissions =
            VM_READ | ((p->flags & 2) ? VM_WRITE : 0) | ((p->flags & 1) ? VM_EXEC : 0);
        if (share && !(p->flags & 2)) {
            for (uint32_t offset = 0; offset < p->memsz; offset += PAGE_SIZE) {
                if (!vm_map_shared(space, p->vaddr + offset, shared_first[i] + offset / PAGE_SIZE,
                                   permissions))
                    return false;
                if ((offset & 0xffffu) == 0xf000u) kernel_io_wait();
            }
            continue;
        }
        for (uint32_t offset = 0; offset < p->memsz; offset += PAGE_SIZE) {
            /* Populate through the cached supervisor alias before publishing
             * executable permissions. vm_protect cleans the completed code to
             * the point of coherency; an I-cache invalidation alone cannot
             * make dirty data-cache lines visible to instruction fetch. */
            if (!vm_map(space, p->vaddr + offset, permissions & ~VM_EXEC)) return false;
            if (offset < p->filesz) {
                size_t n = p->filesz - offset;
                if (n > PAGE_SIZE) n = PAGE_SIZE;
                memcpy(vm_resolve(space, p->vaddr + offset, VM_READ),
                       (const unsigned char *)image + p->offset + offset, n);
            }
            if ((permissions & VM_EXEC) && !vm_protect(space, p->vaddr + offset, permissions))
                return false;
            /* Open runs on the service worker. Bound copying/zeroing between
             * desktop polls, including large BSS segments. At boot this is a
             * no-op; the worker's owner remains frozen until loading finishes. */
            if ((offset & 0xffffu) == 0xf000u) kernel_io_wait();
        }
    }
    return true;
}

/* The whole image, validated before any segment is allocated or copied. */
static bool parse(const void *image, size_t size, struct elf_header *out,
                  struct program_header headers[8]) {
    struct elf_header h;
    if (!image || size < sizeof(h)) return false;
    memcpy(&h, image, sizeof(h));
    if (memcmp(h.ident, "\177ELF\1\1\1", 7) || h.type != 2 || h.machine != 40 || h.version != 1 ||
        h.ehsize != sizeof(h) || h.phentsize != sizeof(struct program_header) || !h.phnum ||
        h.phnum > 8 || h.phoff > size ||
        h.phnum > (size - h.phoff) / sizeof(struct program_header) || (h.entry & 3))
        return false;
    memcpy(headers, (const unsigned char *)image + h.phoff, h.phnum * sizeof(headers[0]));
    bool executable_entry = false;
    for (unsigned i = 0; i < h.phnum; ++i) {
        const struct program_header *p = &headers[i];
        if (p->type != 1) {
            if (p->type == 2 || p->type == 3) return false;
            continue;
        }
        bool browser = p->vaddr >= WEB_IMAGE_BASE && p->vaddr < WEB_IMAGE_END &&
                       p->memsz <= WEB_IMAGE_END - p->vaddr;
        if (!p->memsz || p->filesz > p->memsz || p->offset > size || p->filesz > size - p->offset ||
            !browser || (p->vaddr & (PAGE_SIZE - 1)) || p->align != PAGE_SIZE ||
            (p->offset & (PAGE_SIZE - 1)) || !(p->flags & 4) || (p->flags & ~7u) ||
            ((p->flags & 3) == 3))
            return false;
        uint32_t end = (p->vaddr + p->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (unsigned j = 0; j < i; ++j) {
            const struct program_header *q = &headers[j];
            if (q->type == 1 && p->vaddr < q->vaddr + q->memsz && q->vaddr < end) return false;
        }
        if ((p->flags & 1) && h.entry >= p->vaddr && h.entry - p->vaddr < p->filesz)
            executable_entry = true;
    }
    *out = h;
    return executable_entry;
}

bool elf_valid(const void *image, size_t size) {
    struct elf_header h;
    struct program_header headers[8];
    return parse(image, size, &h, headers);
}

bool elf_load(struct vm_space *space, const void *image, size_t size, bool shared,
              uint32_t *entry) {
    struct elf_header h;
    struct program_header headers[8];
    if (!entry || !parse(image, size, &h, headers)) return false;
    if (!load_segments(space, image, size, headers, h.phnum, shared)) {
        vm_shared_trim(); /* A block created here that nothing maps yet. */
        return false;
    }
    *entry = h.entry;
    return true;
}
