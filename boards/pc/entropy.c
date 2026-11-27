#include "entropy.h"
#include "platform.h"
#include "sha256.h"

/* Secure randomness on a PC: the CPU's own generator, RDSEED's conditioned
 * entropy (RDRAND's output where it has no RDSEED, or runs dry), seeds an
 * HMAC_DRBG with SHA-256 (NIST SP 800-90A, as kernel/protection/
 * jitter_entropy.c's), with the TSC beside it; reseeded every 10 minutes or
 * 1 MiB. Seeding takes microseconds, so every request is served at once.
 * Without RDRAND or RDSEED there is no randomness: no timer fallback.
 * https://www.intel.com/content/www/us/en/developer/articles/guide/intel-digital-random-number-generator-drng-software-implementation-guide.html
 */
enum { SEED = 48, RESEED_BYTES = 1 << 20, RESEED_US = 600000000, RETRIES = 64 };

static struct {
    uint8_t key[32], value[32];
    uint64_t seeded_at, output;
    bool seeded;
} drbg;

static bool rdseed, rdrand, ready;
static const char *failure = "Not initialized";

static void wipe(void *data, size_t size) {
    volatile uint8_t *p = data;
    while (size--) *p++ = 0;
}

static bool rdseed64(uint64_t *value) {
    unsigned char ok;
    __asm__ volatile("rdseed %0\n\tsetc %1" : "=r"(*value), "=qm"(ok)::"cc");
    return ok;
}

static bool rdrand64(uint64_t *value) {
    unsigned char ok;
    __asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(*value), "=qm"(ok)::"cc");
    return ok;
}

/* 64 bits from the CPU, retried a bounded number of times. */
static bool hardware(uint64_t *value) {
    for (unsigned i = 0; rdseed && i < RETRIES; ++i) {
        if (rdseed64(value)) return true;
        __asm__ volatile("pause");
    }
    for (unsigned i = 0; rdrand && i < RETRIES; ++i)
        if (rdrand64(value)) return true;
    return false;
}

/* HMAC-SHA256 of the value with one optional byte and data appended. */
static void hmac(const uint8_t key[32], const uint8_t *separator, const void *data, size_t size,
                 uint8_t out[32]) {
    uint8_t pad[64], inner[32];
    struct sha256 hash;
    for (unsigned i = 0; i < 64; ++i) pad[i] = (uint8_t)((i < 32 ? key[i] : 0) ^ 0x36);
    sha256_begin(&hash);
    sha256_add(&hash, pad, sizeof(pad));
    sha256_add(&hash, drbg.value, sizeof(drbg.value));
    if (separator) sha256_add(&hash, separator, 1);
    if (size) sha256_add(&hash, data, size);
    sha256_end(&hash, inner);
    for (unsigned i = 0; i < 64; ++i) pad[i] = (uint8_t)((i < 32 ? key[i] : 0) ^ 0x5c);
    sha256_begin(&hash);
    sha256_add(&hash, pad, sizeof(pad));
    sha256_add(&hash, inner, sizeof(inner));
    sha256_end(&hash, out);
    wipe(pad, sizeof(pad));
    wipe(inner, sizeof(inner));
    wipe(&hash, sizeof(hash));
}

/* HMAC_DRBG_Update (SP 800-90A 10.1.2.2). */
static void update(const void *data, size_t size) {
    static const uint8_t zero = 0, one = 1;
    hmac(drbg.key, &zero, data, size, drbg.key);
    hmac(drbg.key, 0, 0, 0, drbg.value);
    if (!size) return;
    hmac(drbg.key, &one, data, size, drbg.key);
    hmac(drbg.key, 0, 0, 0, drbg.value);
}

static void generate(unsigned char *out, size_t size) {
    for (size_t at = 0; at < size; at += 32) {
        hmac(drbg.key, 0, 0, 0, drbg.value);
        size_t n = size - at < 32 ? size - at : 32;
        memcpy(out + at, drbg.value, n);
    }
    update(0, 0);
    drbg.output += size;
}

static bool reseed(void) {
    uint64_t seed[SEED / 8 + 1];
    for (unsigned i = 0; i < SEED / 8; ++i)
        if (!hardware(&seed[i])) {
            wipe(seed, sizeof(seed));
            return false;
        }
    seed[SEED / 8] = x86_rdtsc();
    if (!drbg.seeded) {
        /* Instantiate (10.1.2.3): key all zero, value all one. */
        memset(drbg.key, 0, sizeof(drbg.key));
        memset(drbg.value, 1, sizeof(drbg.value));
    }
    update(seed, sizeof(seed));
    wipe(seed, sizeof(seed));
    drbg.seeded = true;
    drbg.seeded_at = time_us();
    drbg.output = 0;
    return true;
}

bool entropy_init(const struct platform_devices *devices) {
    (void)devices;
    uint32_t basic[4], extended[4];
    x86_cpuid(1, 0, basic);
    x86_cpuid(7, 0, extended);
    rdrand = basic[2] >> 30 & 1;
    rdseed = extended[1] >> 18 & 1;
    if (!rdrand && !rdseed) {
        failure = "CPU has no random number generator";
        return false;
    }
    /* A generator stuck on one value (as some firmware leaves AMD's after
     * resume) gives no randomness. */
    uint64_t first, next;
    bool varied = false;
    if (!hardware(&first)) {
        failure = "CPU random number generator failed";
        return false;
    }
    for (unsigned i = 0; i < 8 && !varied; ++i) varied = hardware(&next) && next != first;
    if (!varied || !reseed()) {
        failure = "CPU random number generator failed";
        return false;
    }
    ready = true;
    failure = 0;
    return true;
}

bool entropy_available(void) { return ready; }

const char *entropy_error(void) { return failure; }

size_t entropy_read_now(void *out, size_t size) {
    if (!ready || !out || !size || size > 256) return 0;
    if ((!drbg.seeded || drbg.output >= RESEED_BYTES || time_us() - drbg.seeded_at >= RESEED_US) &&
        !reseed())
        return 0;
    generate(out, size);
    return size;
}

size_t entropy_read(void *out, size_t size) { return entropy_read_now(out, size); }
