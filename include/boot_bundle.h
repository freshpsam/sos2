#ifndef SOS2_BOOT_BUNDLE_H
#define SOS2_BOOT_BUNDLE_H
#include "fdt.h"

/* SORZ v2: a 32-byte header (magic, version, header size, stream size,
 * expanded size, stream CRC-32, expanded CRC-32, LZMA properties byte) and one
 * raw LZMA stream with an end marker (third_party/lzma). */
enum {
    BOOT_BUNDLE_MAGIC = 0x5a524f53 /* "SORZ" */,
    BOOT_BUNDLE_VERSION = 2,
    BOOT_BUNDLE_HEADER = 32,
    BOOT_BUNDLE_LIMIT = 256u * 1024 * 1024,
    BOOT_RAMDISK_BASE = 0x82000000,
    /* lk2nd maps only 90 MiB from 0x80000000 to load kernel and ramdisk. */
    BOOT_RAMDISK_END = 0x85a00000
};

bool boot_bundle_decode(const void *source, size_t size, void *out, size_t capacity);
/* Expand the packaged SORZ ramdisk and replace its handoff with the validated
 * output range. All ranges and the future process arena are checked first. */
bool boot_bundle_prepare(struct platform_devices *, uintptr_t dtb, uintptr_t ks, uintptr_t ke);
#endif
