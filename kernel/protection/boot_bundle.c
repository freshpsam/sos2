#include "boot_bundle.h"
#include "boot_memory.h"
#include "boot_progress.h"
#include "platform.h"
#include "LzmaDec.h"
#if defined(SOS2_BOARD_REDMI2) && defined(__arm__)
#include "arm_cache.h"
#endif

static uint32_t word(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t crc32(const void *data, size_t length, enum boot_phase phase) {
    static uint32_t table[256];
    static bool ready;
    if (!ready) {
        for (unsigned i = 0; i < 256; ++i) {
            uint32_t n = i;
            for (unsigned j = 0; j < 8; ++j) n = (n >> 1) ^ ((n & 1) ? 0xedb88320 : 0);
            table[i] = n;
        }
        ready = true;
    }
    uint32_t crc = UINT32_MAX;
    const unsigned char *p = data;
    for (size_t i = 0; i < length;) {
        size_t end = i + 4096;
        if (end > length) end = length;
        for (; i < end; ++i) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
        boot_progress_report(phase, (uint32_t)i, (uint32_t)length);
    }
    return crc ^ UINT32_MAX;
}

/* The LZMA properties byte (lc, lp, pb) sits in the header's last word. Only
 * lc+lp <= 3 is accepted, so the probability table has a fixed size. */
static bool properties_supported(uint32_t value) {
    if (value >= 9 * 5 * 5) return false;
    unsigned lc = value % 9, lp = value / 9 % 5;
    return lc + lp <= 3;
}

static bool header(const unsigned char *p, size_t size) {
    return size >= BOOT_BUNDLE_HEADER && word(p) == BOOT_BUNDLE_MAGIC &&
           word(p + 4) == BOOT_BUNDLE_VERSION && word(p + 8) == BOOT_BUNDLE_HEADER &&
           word(p + 12) == size - BOOT_BUNDLE_HEADER && word(p + 12) > 0 && word(p + 16) >= 16 &&
           word(p + 16) <= BOOT_BUNDLE_LIMIT && properties_supported(word(p + 28));
}

/* LzmaDec allocates nothing but its probability table: 1984 base entries
 * plus 0x300 << (lc+lp) literal entries (LzmaProps_GetNumProbs). */
static uint32_t probabilities[(1984u + (0x300u << 3)) * sizeof(CLzmaProb) / sizeof(uint32_t)];

static void *probability_table(ISzAllocPtr unused, size_t size) {
    (void)unused;
    return size <= sizeof(probabilities) ? probabilities : 0;
}

static void keep_probabilities(ISzAllocPtr unused, void *table) {
    (void)unused;
    (void)table;
}

static const ISzAlloc probability_allocator = {probability_table, keep_probabilities};

/* One raw LZMA stream with an end marker, decoded straight into the output,
 * which is also the dictionary: it never wraps. Steps of 64 KiB report
 * progress, so polled power and touch controls stay alive meanwhile. */
bool boot_bundle_decode(const void *source, size_t size, void *out, size_t capacity) {
    const unsigned char *p = source;
    if (!p || !out || !header(p, size) || word(p + 16) > capacity) return false;
    size_t in = word(p + 12), expanded = word(p + 16);
    boot_progress_report(BOOT_CHECK_ENGINE, 0, (uint32_t)in);
    if (crc32(p + BOOT_BUNDLE_HEADER, in, BOOT_CHECK_ENGINE) != word(p + 20)) return false;
    /* A dictionary as large as the output admits every distance the encoder used. */
    const Byte properties[LZMA_PROPS_SIZE] = {(Byte)word(p + 28), (Byte)expanded,
                                              (Byte)(expanded >> 8), (Byte)(expanded >> 16),
                                              (Byte)(expanded >> 24)};
    CLzmaDec state;
    LzmaDec_CONSTRUCT(&state) if (LzmaDec_AllocateProbs(&state, properties, LZMA_PROPS_SIZE,
                                                        &probability_allocator) !=
                                  SZ_OK) return false;
    state.dic = out;
    state.dicBufSize = expanded;
    LzmaDec_Init(&state);
    const Byte *stream = p + BOOT_BUNDLE_HEADER;
    size_t consumed = 0;
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    boot_progress_report(BOOT_LOAD_ENGINE, 0, (uint32_t)expanded);
    while (state.dicPos < expanded) {
        size_t before = state.dicPos, limit = expanded - before > 65536 ? before + 65536 : expanded;
        SizeT chunk = in - consumed;
        if (LzmaDec_DecodeToDic(&state, limit, stream + consumed, &chunk, LZMA_FINISH_ANY,
                                &status) != SZ_OK)
            return false;
        consumed += chunk;
        boot_progress_report(BOOT_LOAD_ENGINE, (uint32_t)state.dicPos, (uint32_t)expanded);
        /* An early end marker, or input that ran out, leaves the output short. */
        if (status == LZMA_STATUS_FINISHED_WITH_MARK || (!chunk && state.dicPos == before))
            return false;
    }
    /* The output is complete: only the end marker may follow. */
    SizeT rest = in - consumed;
    if (LzmaDec_DecodeToDic(&state, expanded, stream + consumed, &rest, LZMA_FINISH_END, &status) !=
            SZ_OK ||
        status != LZMA_STATUS_FINISHED_WITH_MARK || consumed + rest != in)
        return false;
    boot_progress_report(BOOT_VERIFY_ENGINE, 0, (uint32_t)expanded);
    return crc32(out, expanded, BOOT_VERIFY_ENGINE) == word(p + 24);
}

bool boot_bundle_prepare(struct platform_devices *d, uintptr_t dtb, uintptr_t ks, uintptr_t ke) {
    /* After the installed system's read from the eMMC, if any. */
    boot_progress_report(BOOT_CHECK_ENGINE, 0, 0);
    if (!d->has_initrd_start || !d->has_initrd_end || d->initrd_start != BOOT_RAMDISK_BASE ||
        d->initrd_end <= d->initrd_start || d->initrd_end > BOOT_RAMDISK_END)
        return false;
    size_t size = (size_t)(d->initrd_end - d->initrd_start);
    if (!boot_memory_available(d, d->initrd_start, size, dtb, ks, ke, false)) return false;
    const unsigned char *source = (const void *)(uintptr_t)d->initrd_start;
    if (!header(source, size)) return false;
    uintptr_t destination, arena;
    size_t arena_size;
    if (!redmi2_resource_arena(d, dtb, ks, ke, word(source + 16), &destination)) return false;
    struct platform_devices expanded = *d;
    expanded.initrd_start = destination;
    expanded.initrd_end = destination + word(source + 16);
    if (!redmi2_browser_arena(&expanded, dtb, ks, ke, &arena, &arena_size)) return false;
#if defined(SOS2_BOARD_REDMI2) && defined(__arm__)
    if (!arm_boot_cache_begin(d, dtb, (uintptr_t)source, size, destination, word(source + 16)))
        return false;
#endif
    bool decoded = boot_bundle_decode(source, size, (void *)destination, word(source + 16));
#if defined(SOS2_BOARD_REDMI2) && defined(__arm__)
    /* This includes failure paths: no dirty output or cached alias survives. */
    arm_boot_cache_end();
#endif
    if (!decoded) return false;
    d->initrd_start = expanded.initrd_start;
    d->initrd_end = expanded.initrd_end;
    return true;
}
