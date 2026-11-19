#ifndef SOS2_PAGE_ALLOC_H
#define SOS2_PAGE_ALLOC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PAGE_SIZE 4096u
/* The largest page pool: 384 MiB, or 1408 MiB on AArch64 (QEMU's 2 GiB, the
 * Galaxy Tab A7 Lite's 3 GB), where it is the largest free stretch of RAM. */
#ifdef __aarch64__
#define PAGE_POOL_PAGES 360448u
#else
#define PAGE_POOL_PAGES 98304u
#endif
/* Bounded linker arena, or a validated reserved phone browser arena. */
bool pages_init(void *start, size_t size);
void *pages_alloc(unsigned count, unsigned alignment_pages);
/* Like pages_alloc but not zeroed: the caller overwrites every byte. */
void *pages_reserve(unsigned count, unsigned alignment_pages);
bool pages_free(void *start, unsigned count);
unsigned pages_available(void);
#endif
