#ifndef SOS2_WIRE_H
#define SOS2_WIRE_H
#include <stdint.h>

/* Little-endian loads and stores at any byte, for wire and file formats. */
static inline uint16_t le16(const void *v) {
    const unsigned char *p = v;
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static inline uint32_t le32(const void *v) {
    const unsigned char *p = v;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t le64(const void *v) {
    const unsigned char *p = v;
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static inline void put16(void *v, uint16_t n) {
    unsigned char *p = v;
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}

static inline void put32(void *v, uint32_t n) {
    unsigned char *p = v;
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}
#endif
