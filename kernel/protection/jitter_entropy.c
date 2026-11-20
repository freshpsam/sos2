#include "entropy.h"
#include "platform.h"
#include "kernel_io.h"
#include "sha256.h"
#include "jitterentropy.h"
#include <setjmp.h>
#ifdef SOS2_BOARD_GTA7LITE
#include "gta7lite.h"
#endif

/* Secure randomness on the Redmi 2 and the Galaxy Tab A7 Lite, which have no
 * virtio RNG: jitterentropy's CPU timing jitter, measured with the generic
 * timer's count, seeds an HMAC_DRBG with SHA-256 (NIST SP 800-90A), which
 * serves every request. A jitter collection takes tens of milliseconds per
 * 32 bytes, so the generator is seeded from 48 bytes of it on the first
 * request and reseeded the same way every 10 minutes or 1 MiB; requests in
 * between take microseconds and never wait for the service worker.
 * Private, supervisor-only memory, including all temporary self-test state.
 * No user mapping, device PRNG credit, deterministic seed or timer fallback. */
enum {
    UNIT = 4096,
    UNITS = 1024,
    CHUNK = 32,
    SEED = 48,
    RESEED_BYTES = 1 << 20,
    RESEED_US = 600000000
};

static unsigned char memory[UNIT * UNITS] __attribute__((aligned(4096), section(".cpu_private")));
static unsigned short allocated[UNITS];
static struct rand_data *collector;
static jmp_buf escape;
static uint64_t started, budget;
static uint32_t hz, samples;
static bool attempted, ready, active;
static const char *failure = "Not initialized";

enum entropy_phase { STARTUP, COLLECTOR, READ };
static enum entropy_phase phase;

/* The generator's key and value; seeded once a collection has filled them. */
static struct {
    uint8_t key[32], value[32];
    uint64_t seeded_at, output;
    bool seeded;
} drbg __attribute__((section(".cpu_private")));

bool entropy_available(void) { return ready; }

const char *entropy_error(void) { return failure; }

static uint32_t frequency(void) { return arch_counter_frequency(); }

static uint64_t counter(void) { return arch_counter_synchronized(); }

void jent_memset_secure(void *p, size_t n) {
    volatile unsigned char *s = p;
    while (n--) *s++ = 0;
}

void *jent_zalloc(size_t size) {
    if (!size || size > sizeof(memory)) return 0;
    unsigned count = (size + UNIT - 1) / UNIT, run = 0;
    for (unsigned i = 0; i < UNITS; ++i) {
        run = allocated[i] ? 0 : run + 1;
        if (run == count) {
            unsigned first = i + 1 - count;
            for (unsigned j = first; j <= i; ++j) allocated[j] = (unsigned short)(i + 1 - j);
            memset(memory + first * UNIT, 0, count * UNIT);
            return memory + first * UNIT;
        }
    }
    return 0;
}

void jent_zfree(void *p, size_t size) {
    (void)size;
    if (!p) return;
    uintptr_t address = (uintptr_t)p, base = (uintptr_t)memory;
    if (address < base || address >= base + sizeof(memory) || (address - base) % UNIT)
        panic("entropy allocation invalid");
    unsigned first = (address - base) / UNIT, count = allocated[first];
    if (!count || count > UNITS - first) panic("entropy allocation corrupt");
    jent_memset_secure(p, count * UNIT);
    for (unsigned i = 0; i < count; ++i) allocated[first + i] = 0;
}

void jent_get_nstime(uint64_t *out) {
    uint64_t now = counter();
    /* A sample cap also terminates a stuck architectural counter. No aborted
     * collector is reused: all its allocations are wiped by disable(). */
    if (!active) panic("entropy sampling outside guarded call");
#ifdef SOS2_BOARD_GTA7LITE
    /* LK's watchdog keeps running through the startup tests, which may take
     * seconds. */
    if (!(samples % 1024)) gta7lite_watchdog_feed();
#endif
    if (++samples > 1000000) failure = "Timing sample limit reached";
    else if (now < started) failure = "Timing counter moved backwards";
    else if (now - started >= budget)
        failure = phase == STARTUP     ? "Startup test timed out"
                  : phase == COLLECTOR ? "Collector startup timed out"
                                       : "Random collection timed out";
    else {
        *out = now;
        return;
    } /* Raw ticks preserve every bit of counter resolution. */
    longjmp(escape, 1);
}

static void disable(void) {
    active = ready = false;
    collector = 0;
    jent_memset_secure(&drbg, sizeof(drbg));
    jent_memset_secure(memory, sizeof(memory));
    memset(allocated, 0, sizeof(allocated));
}

static const char *startup_error(int code) {
    switch (code) {
        case ENOTIME: return "Timing counter unavailable";
        case ECOARSETIME: return "Timing counter too coarse";
        case ENOMONOTONIC: return "Timing counter not monotonic";
        case EMINVARIATION: return "Insufficient timing variation";
        case EVARVAR:
        case EMINVARVAR: return "Insufficient timing jitter";
        case EPROGERR: return "Startup configuration rejected";
        case ESTUCK: return "Startup timing samples stuck";
        case EHEALTH: return "Startup health test failed";
        case ERCT: return "Startup repetition test failed";
        case EHASH: return "Hash self-test failed";
        case EMEM: return "Startup memory unavailable";
        case EGCD: return "Timing divisor test failed";
        default: return "Unknown startup test failure";
    }
}

bool entropy_init(const struct platform_devices *devices) {
    (void)devices;
    if (attempted) return ready;
    attempted = true;
    hz = frequency();
    if (!hz) {
        failure = "Timing frequency unavailable";
        disable();
        return false;
    }
    started = counter();
    budget = (uint64_t)hz * 10;
    samples = 0;
    active = true;
    phase = STARTUP;
    if (setjmp(escape)) {
        disable();
        return false;
    }
    /* 1 MiB initial noise memory, allowed to grow inside the 4 MiB arena.
     * Upstream enforces its minimum oversampling rate even when zero is used. */
    unsigned flags = JENT_DISABLE_INTERNAL_TIMER | JENT_FORCE_FIPS | JENT_MAX_MEMSIZE_1MB;
    int error = jent_entropy_init_ex(0, flags);
    if (error) {
        failure = startup_error(error);
        disable();
        return false;
    }
    /* Each guarded library operation gets its own deadline. A successful
     * startup test must not consume the collector's entire time allowance. */
    phase = COLLECTOR;
    started = counter();
    budget = (uint64_t)hz * 10;
    samples = 0;
    if (!(collector = jent_entropy_collector_alloc(0, flags))) {
        failure = "Collector startup failed";
        disable();
        return false;
    }
    active = false;
    ready = true;
    failure = 0;
    return true;
}

static bool collect(void *out, size_t size) {
    started = counter();
    budget = hz;
    samples = 0;
    active = true;
    phase = READ;
    if (setjmp(escape)) {
        disable();
        return false;
    }
    if (jent_read_entropy(collector, out, size) != (ssize_t)size) {
        failure = "Continuous health test failed";
        disable();
        return false;
    }
    active = false;
    return true;
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
    jent_memset_secure(pad, sizeof(pad));
    jent_memset_secure(inner, sizeof(inner));
    jent_memset_secure(&hash, sizeof(hash));
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

static bool seed_due(void) {
    return !drbg.seeded || drbg.output >= RESEED_BYTES || time_us() - drbg.seeded_at >= RESEED_US;
}

size_t entropy_read_now(void *out, size_t size) {
    if (!ready || !out || !size || size > 256 || seed_due()) return 0;
    generate(out, size);
    return size;
}

size_t entropy_read(void *out, size_t size) {
    if (!ready || !out || !size || size > 256 || active) return 0;
    if (seed_due()) {
        unsigned char seed[SEED];
        for (size_t i = 0; i < SEED; i += CHUNK) {
            size_t count = SEED - i < CHUNK ? SEED - i : CHUNK;
            if (!collect(seed + i, count)) {
                jent_memset_secure(seed, sizeof(seed));
                return 0;
            }
            /* Yield only between complete collections, never inside a
             * timing measurement. SYS_RANDOM runs on the serialized
             * service worker. */
            kernel_io_wait();
        }
        if (!drbg.seeded) {
            /* Instantiate (10.1.2.3): key all zero, value all one. */
            memset(drbg.key, 0, sizeof(drbg.key));
            memset(drbg.value, 1, sizeof(drbg.value));
        }
        update(seed, sizeof(seed));
        jent_memset_secure(seed, sizeof(seed));
        drbg.seeded = true;
        drbg.seeded_at = time_us();
        drbg.output = 0;
    }
    generate(out, size);
    return size;
}
