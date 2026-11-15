/* SOS2 freestanding platform hooks; upstream library sources are unmodified. */
#ifndef SOS2_JITTERENTROPY_PLATFORM_H
#define SOS2_JITTERENTROPY_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#ifndef EAGAIN
#define EAGAIN 11 /* Internal GCD retry sentinel; no host errno/ABI dependency. */
#endif
typedef ptrdiff_t ssize_t;
void *memcpy(void *,const void *,size_t);
void *memset(void *,int,size_t);
int memcmp(const void *,const void *,size_t);
void jent_get_nstime(uint64_t *);
void *jent_zalloc(size_t);
void jent_zfree(void *,size_t);
void jent_memset_secure(void *,size_t);
static inline int jent_fips_enabled(void) {return 1;}
static inline long jent_ncpu(void) {return 1;}
static inline uint32_t jent_cache_size_roundup(int all) {(void)all;return 1024*1024;}
static inline void jent_yield(void) { } /* The threaded timer is disabled. */
static inline uint64_t rol64(uint64_t x,int n) {return (x<<(n&63))|(x>>((64-n)&63));}
/* Keep upstream's extra backtracking-resistance step even with private memory. */
#endif
