#ifndef SOS2_PAGE_ALLOC_H
#define SOS2_PAGE_ALLOC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PAGE_SIZE 4096u
/* The page pool's largest span: 3 GiB on AArch64 (QEMU's 2 GiB, the Galaxy
 * Tab A7 Lite's 3 GB, from 0x40000000), where it spans the free stretches of
 * RAM with the gaps between them withheld; 768 MiB on ARM32, one stretch. */
#ifdef __LP64__
#define PAGE_POOL_PAGES 786432u
#else
#define PAGE_POOL_PAGES 196608u
#endif
/* The pool's span: QEMU ARM32's linker arena, the phone's validated browser
 * arena, or on 64-bit systems the first stretch's start to the last's end. */
bool pages_init(void *start, size_t size);
/* Takes a gap between stretches of RAM out of the pool, before any allocation
 * there. */
bool pages_withhold(void *start, size_t size);
void *pages_alloc(unsigned count, unsigned alignment_pages);
/* Like pages_alloc but not zeroed: the caller overwrites every byte. */
void *pages_reserve(unsigned count, unsigned alignment_pages);
bool pages_free(void *start, unsigned count);
unsigned pages_available(void);
/* Single pages promised to a mapping that installs them later (vm.h): taken
 * out of pages_available now, so that no other allocation can use them up,
 * and allocated (zeroed) one at a time. */
bool pages_promise(unsigned count);
void pages_unpromise(unsigned count);
void *pages_alloc_promised(void);
/* count single pages that lie in one run aligned to count, a power of two,
 * from the pool's top, away from where single pages are taken; not zeroed,
 * each freed alone. pages_zero zeroes one through its uncached alias. */
void *pages_reserve_run(unsigned count);
void pages_zero(void *page);
#endif
