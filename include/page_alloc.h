#ifndef SOS2_PAGE_ALLOC_H
#define SOS2_PAGE_ALLOC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PAGE_SIZE       4096u
#define PAGE_POOL_PAGES 98304u
/* Bounded linker arena, or a validated reserved phone browser arena. */
bool pages_init(void *start, size_t size);
void *pages_alloc(unsigned count, unsigned alignment_pages);
/* Like pages_alloc but not zeroed: the caller overwrites every byte. */
void *pages_reserve(unsigned count, unsigned alignment_pages);
bool pages_free(void *start, unsigned count);
unsigned pages_available(void);
#endif
