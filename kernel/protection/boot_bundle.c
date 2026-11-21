#include "boot_bundle.h"
#include "boot_memory.h"
#include "boot_progress.h"
#include "platform.h"
#include "LzmaDec.h"
/* Only where the loader hands the resources over as a SORZ ramdisk: the
 * phones and arm64 (runtime_files.c). */
#if defined(SOS2_PHONE) || defined(__aarch64__)
#include "arm_cache.h"

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

/* The LZMA properties byte (lc, lp, pb) sits in the header's eighth word.
 * Only lc+lp <= 3 is accepted, so the probability table has a fixed size. */
static bool properties_supported(uint32_t value) {
    if (value >= 9 * 5 * 5) return false;
    unsigned lc = value % 9, lp = value / 9 % 5;
    return lc + lp <= 3;
}

static uint32_t half(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static void store(unsigned char *p, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) p[byte] = (unsigned char)(value >> 8 * byte);
}

/* The branch filters: known ones over whole instructions inside the output,
 * in order and apart, then unused ones, zero. */
static bool filters_supported(const unsigned char *p, uint32_t expanded) {
    uint32_t end = 0;
    bool unused = false;
    for (unsigned i = 0; i < BOOT_BUNDLE_FILTERS; ++i) {
        const unsigned char *f = p + 32 + 12 * i;
        uint32_t filter = word(f), start = word(f + 4), length = word(f + 8);
        uint32_t align = filter == BOOT_BUNDLE_FILTER_THUMB ? 2 : 4;
        if (filter == BOOT_BUNDLE_FILTER_NONE) {
            if (start || length) return false;
            unused = true;
        } else if (unused || filter > BOOT_BUNDLE_FILTER_THUMB || !length || start % align ||
                   length % align || start < end || start > expanded || length > expanded - start) {
            return false;
        } else {
            end = start + length;
        }
    }
    return true;
}

static bool header(const unsigned char *p, size_t size) {
    return size >= BOOT_BUNDLE_HEADER && word(p) == BOOT_BUNDLE_MAGIC &&
           word(p + 4) == BOOT_BUNDLE_VERSION && word(p + 8) == BOOT_BUNDLE_HEADER &&
           word(p + 12) == size - BOOT_BUNDLE_HEADER && word(p + 12) > 0 && word(p + 16) >= 16 &&
           word(p + 16) <= BOOT_BUNDLE_LIMIT && properties_supported(word(p + 28)) &&
           filters_supported(p, word(p + 16));
}

/* Filtering continues the loading phase: after the LZMA stream's output,
 * every filtered byte counts once more. */
static uint32_t load_done, load_total;

static uint32_t filtering(uint32_t i) {
    boot_progress_report(BOOT_LOAD_ENGINE, load_done + i, load_total);
    return i + 65536;
}

/* xz's ARM64 branch filter (liblzma/simple/arm64.c), decoding: each BL's and
 * ADRP's absolute target, from the word's position in the output, made
 * relative again. */
static void arm64_branches(unsigned char *p, uint32_t length, uint32_t position) {
    for (uint32_t i = 0, report = 0; i < length; i += 4) {
        if (i >= report) report = filtering(i);
        uint32_t pc = position + i, instruction = word(p + i);
        if (instruction >> 26 == 0x25) {
            instruction = 0x94000000u | ((instruction - (pc >> 2)) & 0x03ffffffu);
        } else if ((instruction & 0x9f000000u) == 0x90000000u) {
            uint32_t source = (instruction >> 29 & 3) | (instruction >> 3 & 0x001ffffcu);
            if ((source + 0x00020000u) & 0x001c0000u) continue;
            uint32_t target = source - (pc >> 12);
            instruction = (instruction & 0x9000001fu) | (target & 3) << 29 |
                          (target & 0x0003fffcu) << 3 |
                          ((0u - (target & 0x00020000u)) & 0x00e00000u);
        } else continue;
        store(p + i, instruction);
    }
}

/* ARM's B and BL with the always condition, and BLX, decoding as for ARM64:
 * B's and BL's targets count words, BLX's, a Thumb function's, halfwords,
 * with its H bit the lowest. */
static void arm_branches(unsigned char *p, uint32_t length, uint32_t position) {
    for (uint32_t i = 0, report = 0; i < length; i += 4) {
        if (i >= report) report = filtering(i);
        uint32_t pc = position + i + 8, instruction = word(p + i), top = instruction >> 24;
        if (top == 0xea || top == 0xeb) {
            instruction = top << 24 | ((instruction - (pc >> 2)) & 0x00ffffffu);
        } else if (top == 0xfa || top == 0xfb) {
            uint32_t target = ((instruction & 0x00ffffffu) << 1 | (top & 1)) - (pc >> 1);
            instruction = (0xfau | (target & 1)) << 24 | (target >> 1 & 0x00ffffffu);
        } else continue;
        store(p + i, instruction);
    }
}

/* Thumb-2's BL, BLX and B.W, decoding as for ARM, found by walking the
 * instructions: a halfword from 0xe800 up starts a 32-bit one. Their 24-bit
 * fields hold the target in halfwords, with S, J1 and J2 as its top bits;
 * BLX's counts from the word-aligned PC, as the instruction does. Decoding
 * sees the instructions the packager saw: filtering keeps the bits read. */
static void thumb_branches(unsigned char *p, uint32_t length, uint32_t position) {
    for (uint32_t i = 0, report = 0; i + 4 <= length;) {
        if (i >= report) report = filtering(i);
        uint32_t first = half(p + i), second = half(p + i + 2), kind = second & 0xd000;
        if ((first & 0xf800) != 0xf000 ||
            (kind != 0xd000 && kind != 0x9000 && (second & 0xd001) != 0xc000)) {
            i += first >= 0xe800 ? 4 : 2;
            continue;
        }
        uint32_t pc = position + i + 4;
        if (kind == 0xc000) pc &= ~3u;
        uint32_t target = (first & 0x7ff) << 13 | (second & 0x2000) >> 1 | (second & 0xfff);
        uint32_t offset = (target - (pc >> 1)) & 0x00ffffffu, s = offset >> 23;
        first = (first & 0xf800) | s << 10 | (offset >> 11 & 0x3ff);
        second = (second & 0xd000) | ((offset >> 22 ^ s ^ 1) & 1) << 13 |
                 ((offset >> 21 ^ s ^ 1) & 1) << 11 | (offset & 0x7ff);
        store(p + i, first | second << 16);
        i += 4;
    }
}

/* The bytes the branch filters cover: at most the output, as they are apart. */
static uint32_t filtered(const unsigned char *p) {
    uint32_t total = 0;
    for (unsigned i = 0; i < BOOT_BUNDLE_FILTERS; ++i) total += word(p + 40 + 12 * i);
    return total;
}

static void unfilter(const unsigned char *p, unsigned char *out) {
    for (unsigned i = 0; i < BOOT_BUNDLE_FILTERS; ++i) {
        const unsigned char *f = p + 32 + 12 * i;
        uint32_t filter = word(f), start = word(f + 4), length = word(f + 8);
        if (filter == BOOT_BUNDLE_FILTER_ARM64) arm64_branches(out + start, length, start);
        else if (filter == BOOT_BUNDLE_FILTER_ARM) arm_branches(out + start, length, start);
        else if (filter == BOOT_BUNDLE_FILTER_THUMB) thumb_branches(out + start, length, start);
        load_done += length;
    }
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
static bool boot_bundle_decode(const void *source, size_t size, void *out, size_t capacity) {
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
    load_done = (uint32_t)expanded;
    load_total = load_done + filtered(p);
    boot_progress_report(BOOT_LOAD_ENGINE, 0, load_total);
    while (state.dicPos < expanded) {
        size_t before = state.dicPos, limit = expanded - before > 65536 ? before + 65536 : expanded;
        SizeT chunk = in - consumed;
        if (LzmaDec_DecodeToDic(&state, limit, stream + consumed, &chunk, LZMA_FINISH_ANY,
                                &status) != SZ_OK)
            return false;
        consumed += chunk;
        boot_progress_report(BOOT_LOAD_ENGINE, (uint32_t)state.dicPos, load_total);
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
    unfilter(p, out);
    boot_progress_report(BOOT_VERIFY_ENGINE, 0, (uint32_t)expanded);
    return crc32(out, expanded, BOOT_VERIFY_ENGINE) == word(p + 24);
}

#ifdef __aarch64__
/* The ramdisk lies wherever the DTB says (QEMU's -initrd, an Android boot
 * image's ramdisk). Its output goes to the smallest free stretch of RAM that
 * holds it, decoded through arch/arm64/boot_cache.c's cached mapping. */
bool boot_bundle_prepare(struct platform_devices *d, uintptr_t dtb, uintptr_t ks, uintptr_t ke) {
    boot_progress_report(BOOT_CHECK_ENGINE, 0, 0);
    if (!d->has_initrd_start || !d->has_initrd_end || d->initrd_end <= d->initrd_start)
        return false;
    size_t size = (size_t)(d->initrd_end - d->initrd_start);
    if (!boot_memory_available(d, d->initrd_start, size, dtb, ks, ke, false)) return false;
    const unsigned char *source = (const void *)(uintptr_t)d->initrd_start;
    uintptr_t destination;
    if (!header(source, size) ||
        !boot_memory_resources(d, dtb, ks, ke, word(source + 16), &destination) ||
        !arm_boot_cache_begin(d, dtb, (uintptr_t)source, size, destination, word(source + 16)))
        return false;
    bool decoded = boot_bundle_decode(source, size, (void *)destination, word(source + 16));
    /* This includes failure paths: no dirty output or cached alias survives. */
    arm_boot_cache_end();
    if (!decoded) return false;
    d->initrd_start = destination;
    d->initrd_end = destination + word(source + 16);
    return true;
}
#elif defined(SOS2_PHONE)
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
    if (!phone_resource_arena(d, dtb, ks, ke, word(source + 16), &destination)) return false;
    struct platform_devices expanded = *d;
    expanded.initrd_start = destination;
    expanded.initrd_end = destination + word(source + 16);
    if (!phone_browser_arena(&expanded, dtb, ks, ke, &arena, &arena_size)) return false;
    if (!arm_boot_cache_begin(d, dtb, (uintptr_t)source, size, destination, word(source + 16)))
        return false;
    bool decoded = boot_bundle_decode(source, size, (void *)destination, word(source + 16));
    /* This includes failure paths: no dirty output or cached alias survives. */
    arm_boot_cache_end();
    if (!decoded) return false;
    d->initrd_start = expanded.initrd_start;
    d->initrd_end = expanded.initrd_end;
    return true;
}
#endif
#endif
