#ifndef SOS2_RUNTIME_INTERNAL_H
#define SOS2_RUNTIME_INTERNAL_H
#include <sos2/features.h>
#include <sos2/services.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/reent.h>
#include <pthread.h>
#include "syscall.h"
#define SOS2_KEYS 64
/* Keep malloc's growing break separate from mmap, stacks and TLS. These are
 * virtual address limits; all allocations still share the process quota. The
 * engine maps everything (programs/browser/web_malloc.c), so mmap's range
 * spans the quota: 2 GiB of the heap window on 64-bit systems, 544 MiB on
 * ARM32, which leaves programs' malloc a 48 MiB break. */
#ifdef __LP64__
#define SOS2_BRK_BASE 0x10c0000000ul
#else
#define SOS2_BRK_BASE 0x3c000000u
#endif

static inline sos2_status sos2_map(unsigned pages, unsigned permissions) {
    return call4(SYS_VM_ALLOC_RANGE, USER_HEAP_BASE, SOS2_BRK_BASE, pages, permissions);
}

struct sos2_thread {
    uint32_t used, tid, completion, detached, joining;
    void *(*entry)(void *);
    void *argument, *result;
    void *mapping, *tls;
    unsigned pages, tls_pages;
    struct _reent reent;
    const void *keys[SOS2_KEYS];
    uint32_t key_generations[SOS2_KEYS];
#ifdef __aarch64__
    /* Emulated TLS (threads.c): the table of this thread's variables, the
     * pages mapped for them, and the free space left to carve them from. */
    void **emutls;
    uintptr_t emutls_count, emutls_free, emutls_end;
    struct emutls_chunk *emutls_chunks;
#endif
};

struct sos2_lock {
    uint32_t owner, depth, waiters;
};
struct sos2_thread *sos2_self(void);
#ifdef __x86_64__
/* FS's first base, a TCB without a thread, before anything reads it: x64
 * code reads the thread pointer through FS, which starts at 0. */
void sos2_threads_bootstrap(void);
#endif
void sos2_threads_init(void);
void sos2_lock(struct sos2_lock *);
int sos2_trylock(struct sos2_lock *);
void sos2_unlock(struct sos2_lock *);
int sos2_result(int);
uint64_t sos2_clock(int);
int sos2_clock_read(int, uint64_t *);

static inline sos2_word sos2_ptr(const void *p) { return (sos2_word)(uintptr_t)p; }

static inline uint32_t sos2_load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static inline void sos2_store(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static inline int sos2_cas(uint32_t *p, uint32_t expected, uint32_t value) {
    return __atomic_compare_exchange_n(p, &expected, value, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif
