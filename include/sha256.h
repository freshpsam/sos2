#ifndef SOS2_SHA256_H
#define SOS2_SHA256_H
#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4) for the update service's hashes, and the CRC-32
 * (IEEE 802.3) its formats' headers carry. */
struct sha256 {
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    unsigned used;
};

void sha256_begin(struct sha256 *);
void sha256_add(struct sha256 *, const void *, size_t);
void sha256_end(struct sha256 *, uint8_t digest[32]);
uint32_t crc32_ieee(const void *, size_t);
#endif
