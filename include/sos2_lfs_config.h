#ifndef SOS2_LFS_CONFIG_H
#define SOS2_LFS_CONFIG_H
/* Freestanding littlefs configuration. Utilities follow lfs_util.h's contract;
 * upstream BSD-3-Clause license is retained in third_party/littlefs. */
#ifdef SOS2_LFS_HOST
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define LFS_ASSERT(x) assert(x)
#else
#include "platform.h"
#define LFS_ASSERT(x) \
    do { \
        if (!(x)) panic("littlefs invariant"); \
    } while (0)
#endif
#define LFS_NO_MALLOC
#define LFS_NAME_MAX 127
#define LFS_FILE_MAX (32u * 1024 * 1024)
#define LFS_TRACE(...)
#define LFS_DEBUG(...)
#define LFS_WARN(...)
#define LFS_ERROR(...)

static inline uint32_t lfs_min(uint32_t a, uint32_t b) { return a < b ? a : b; }

static inline uint32_t lfs_max(uint32_t a, uint32_t b) { return a > b ? a : b; }

static inline uint32_t lfs_aligndown(uint32_t a, uint32_t n) { return a - a % n; }

static inline uint32_t lfs_alignup(uint32_t a, uint32_t n) { return lfs_aligndown(a + n - 1, n); }

static inline uint32_t lfs_npw2(uint32_t a) {
    return a <= 1 ? 0 : 32 - (uint32_t)__builtin_clz(a - 1);
}

static inline uint32_t lfs_ctz(uint32_t a) { return __builtin_ctz(a); }

/* Integer only: AArch64's libgcc counts bits with SIMD registers, which the
 * kernel leaves to user code (arch/arm64/entry.S). */
static inline uint32_t lfs_popc(uint32_t a) {
    a -= (a >> 1) & 0x55555555u;
    a = (a & 0x33333333u) + ((a >> 2) & 0x33333333u);
    return (((a + (a >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24;
}

static inline int lfs_scmp(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static inline uint32_t lfs_fromle32(uint32_t a) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return a;
#else
    return __builtin_bswap32(a);
#endif
}

static inline uint32_t lfs_tole32(uint32_t a) { return lfs_fromle32(a); }

static inline uint32_t lfs_frombe32(uint32_t a) { return __builtin_bswap32(lfs_fromle32(a)); }

static inline uint32_t lfs_tobe32(uint32_t a) { return lfs_frombe32(a); }

static inline uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size) {
    const uint8_t *p = buffer;
    while (size--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return crc;
}

static inline void *lfs_malloc(size_t n) {
    (void)n;
    return NULL;
}

static inline void lfs_free(void *p) { (void)p; }
#endif
