/* Newlib's memcmp for ARMv7-A compares a byte at a time unless both sides
 * are word aligned, which the engine's string and hash compares seldom are:
 * 1.5% of YouTube's main thread on the Nexus 5. SOS2 loads words from any
 * address (SCTLR.A off), so this compares words and orders the first that
 * differ by their bytes. AArch64's Newlib already does as much. */
#ifndef __aarch64__
#include <stddef.h>
#include <stdint.h>

typedef uint32_t __attribute__((aligned(1), may_alias)) word;

int memcmp(const void *left, const void *right, size_t size) {
    const unsigned char *a = left, *b = right;
    for (; size >= 16; size -= 16, a += 16, b += 16) {
        const word *x = (const word *)a, *y = (const word *)b;
        if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) break;
    }
    for (; size >= 4; size -= 4, a += 4, b += 4) {
        uint32_t x = *(const word *)a, y = *(const word *)b;
        if (x != y) return __builtin_bswap32(x) < __builtin_bswap32(y) ? -1 : 1;
    }
    for (; size; --size, ++a, ++b)
        if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
#endif
