/* One allocator for the whole engine. WebKit allocates through mimalloc; these
 * definitions send the C library's malloc family there as well, so Skia,
 * HarfBuzz, FreeType, ICU, libxml2, SQLite, mbedTLS, the SDK and C++ operator
 * new share it. Newlib allocates internally through its reentrant _*_r entry
 * points (stdio buffers, strdup, number conversion), so those are replaced too:
 * Newlib's own malloc is then never linked, and no block can reach the other
 * allocator's free. */
#include <bmalloc/mimalloc.h>
#include <malloc.h>
#include <reent.h>
#include <stdlib.h>

void *malloc(size_t size) { return mi_malloc(size); }

void *calloc(size_t count, size_t size) { return mi_calloc(count, size); }

void *realloc(void *pointer, size_t size) { return mi_realloc(pointer, size); }

void *reallocf(void *pointer, size_t size) { return mi_reallocf(pointer, size); }

void free(void *pointer) { mi_free(pointer); }

void *memalign(size_t alignment, size_t size) { return mi_memalign(alignment, size); }

void *aligned_alloc(size_t alignment, size_t size) { return mi_aligned_alloc(alignment, size); }

int posix_memalign(void **out, size_t alignment, size_t size) {
    return mi_posix_memalign(out, alignment, size);
}

void *valloc(size_t size) { return mi_valloc(size); }

size_t malloc_usable_size(void *pointer) { return mi_usable_size(pointer); }

void *_malloc_r(struct _reent *reent, size_t size) {
    (void)reent;
    return mi_malloc(size);
}

void *_calloc_r(struct _reent *reent, size_t count, size_t size) {
    (void)reent;
    return mi_calloc(count, size);
}

void *_realloc_r(struct _reent *reent, void *pointer, size_t size) {
    (void)reent;
    return mi_realloc(pointer, size);
}

void *_reallocf_r(struct _reent *reent, void *pointer, size_t size) {
    (void)reent;
    return mi_reallocf(pointer, size);
}

void _free_r(struct _reent *reent, void *pointer) {
    (void)reent;
    mi_free(pointer);
}

void *_memalign_r(struct _reent *reent, size_t alignment, size_t size) {
    (void)reent;
    return mi_memalign(alignment, size);
}

void *_valloc_r(struct _reent *reent, size_t size) {
    (void)reent;
    return mi_valloc(size);
}

size_t _malloc_usable_size_r(struct _reent *reent, void *pointer) {
    (void)reent;
    return mi_usable_size(pointer);
}
