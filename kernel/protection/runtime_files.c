#include "runtime_files.h"
#include "platform.h"
#include "boot_memory.h"
#include "board.h"
#include "boot_bundle.h"

struct bundle {
    uint32_t magic, version, size, count;
    struct runtime_file files[];
};
static const struct bundle *bundle;

bool runtime_files_init(struct platform_devices *devices, uintptr_t dtb, uintptr_t ks,
                        uintptr_t ke) {
    bundle = 0;
    uint64_t start = RUNTIME_FILES_BASE, limit = RUNTIME_FILES_LIMIT;
#if defined(SOS2_BOARD_REDMI2) || defined(SOS2_BOARD_NEXUS5)
    /* The bootloader's ramdisk carries the resources, whether fastboot sent
     * the image or the installed system's boot partition holds it. */
    if (devices->has_initrd_start && devices->has_initrd_end &&
        devices->initrd_end > devices->initrd_start && devices->initrd_start == BOOT_RAMDISK_BASE &&
        !boot_bundle_prepare(devices, dtb, ks, ke))
        return false;
#ifdef SOS2_BOARD_NEXUS5
    /* Expanded into the second bank (boot_memory.c). */
    if (!devices->has_initrd_start || devices->initrd_start < 0x40000000 ||
        devices->initrd_end > 0x80000000)
        return false;
#else
    /* The phone has no fixed-address loader and must have a real handoff. */
    if (!devices->has_initrd_start || devices->initrd_start < 0x90000000 ||
        devices->initrd_end > 0xc0000000)
        return false;
#endif
#endif
#ifdef __aarch64__
    /* A SORZ ramdisk expands into free RAM first; a SORB one is used in place. */
    if (devices->has_initrd_start && devices->has_initrd_end &&
        devices->initrd_end > devices->initrd_start &&
        boot_memory_available(devices, devices->initrd_start, 4, dtb, ks, ke, false) &&
        *(const uint32_t *)(uintptr_t)devices->initrd_start == BOOT_BUNDLE_MAGIC &&
        !boot_bundle_prepare(devices, dtb, ks, ke))
        return false;
#endif
    if (devices->has_initrd_start || devices->has_initrd_end) {
        if (!devices->has_initrd_start || !devices->has_initrd_end) return false;
        start = devices->initrd_start;
        limit = devices->initrd_end;
    }
    if (start - SOS2_RAM_START >= (uint64_t)SOS2_RAM_END - SOS2_RAM_START ||
        limit > SOS2_RAM_END || limit <= start || limit - start < 16 ||
        limit - start > 256u * 1024 * 1024 ||
        !boot_memory_available(devices, start, limit - start, dtb, ks, ke, false))
        return false;
    const struct bundle *b = (const struct bundle *)(uintptr_t)start;
    if (b->magic != 0x42524f53 /* "SORB" */ || b->version != 1 || b->count > 128 ||
        b->size < 16 + b->count * sizeof(struct runtime_file) || b->size > limit - start)
        return false;
    for (uint32_t i = 0; i < b->count; ++i) {
        const struct runtime_file *f = &b->files[i];
        if (f->name[0] != '/' || !memchr(f->name, 0, sizeof(f->name)) ||
            f->offset < 16 + b->count * sizeof(*f) || f->offset > b->size ||
            f->size > b->size - f->offset)
            return false;
        for (uint32_t j = 0; j < i; ++j)
            if (!strcmp(f->name, b->files[j].name)) return false;
    }
    bundle = b;
    return true;
}

uintptr_t runtime_files_base(void) { return (uintptr_t)bundle; }

uint32_t runtime_files_size(void) { return bundle ? bundle->size : 0; }

unsigned runtime_files_count(void) { return bundle ? bundle->count : 0; }

const struct runtime_file *runtime_file_at(unsigned index) {
    return bundle && index < bundle->count ? &bundle->files[index] : 0;
}

const struct runtime_file *runtime_file_find(const char *path) {
    if (bundle)
        for (uint32_t i = 0; i < bundle->count; ++i)
            if (!strcmp(path, bundle->files[i].name)) return &bundle->files[i];
    return 0;
}

const void *runtime_file_data(const struct runtime_file *file) {
    return (const unsigned char *)bundle + file->offset;
}
