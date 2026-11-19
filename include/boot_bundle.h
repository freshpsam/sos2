#ifndef SOS2_BOOT_BUNDLE_H
#define SOS2_BOOT_BUNDLE_H
#include "fdt.h"

/* SORZ v3: a 128-byte header (magic, version, header size, stream size,
 * expanded size, stream CRC-32, expanded CRC-32, LZMA properties byte, then
 * 8 branch filters, each a filter, its start and its size in the expanded
 * data: in order and apart, unused ones zero and last) and one raw LZMA
 * stream with an end marker (third_party/lzma). The branch filters made the
 * engine's branch targets absolute before compression: xz's ARM64 one an
 * AArch64 engine's BL and ADRP, the ARM and Thumb-2 ones an ARM32 engine's
 * B, BL and BLX (B.W in Thumb-2). Decoding makes them relative again, before
 * the expanded CRC-32 is checked. */
enum {
    BOOT_BUNDLE_MAGIC = 0x5a524f53 /* "SORZ" */,
    BOOT_BUNDLE_VERSION = 3,
    BOOT_BUNDLE_HEADER = 128,
    BOOT_BUNDLE_FILTERS = 8,
    BOOT_BUNDLE_FILTER_NONE = 0,
    BOOT_BUNDLE_FILTER_ARM64 = 1,
    BOOT_BUNDLE_FILTER_ARM = 2,
    BOOT_BUNDLE_FILTER_THUMB = 3,
    BOOT_BUNDLE_LIMIT = 256u * 1024 * 1024,
    /* The Redmi 2's ramdisk: lk2nd maps only 90 MiB from 0x80000000 to load
     * kernel and ramdisk. arm64 takes the DTB's initrd range instead. */
    BOOT_RAMDISK_BASE = 0x82000000,
    BOOT_RAMDISK_END = 0x85a00000
};

/* Expand the packaged SORZ ramdisk and replace its handoff with the validated
 * output range. All ranges and the future process arena are checked first. */
bool boot_bundle_prepare(struct platform_devices *, uintptr_t dtb, uintptr_t ks, uintptr_t ke);
#endif
