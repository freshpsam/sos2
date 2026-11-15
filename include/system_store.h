#ifndef SOS2_SYSTEM_STORE_H
#define SOS2_SYSTEM_STORE_H
#include "fdt.h"

/* The installed system on the phone's eMMC (docs/redmi2.md#installing), as
 * tools/package_redmi2.py lays it out:
 *   boot   lk2nd in the first 512 KiB, then SOS2's boot image: Android boot
 *          v0 with the kernel and DTB, no ramdisk, and sos2.system=<SHA-256>
 *          naming the compressed resources it needs.
 *   cache  1 MiB left unused, then two 64 MiB slots, each a 4 KiB header
 *          (struct system_slot) and the compressed resources (SORZ v2), at
 *          most the 58 MiB the kernel reads them into; then the app store
 *          (include/app_store.h); then the data volume (fs.c) to the end of
 *          the partition, at most 1 GiB: on the Redmi 2's 320 MiB, 131 MiB.
 * The boot image's slot is loaded at boot; an update writes the other slot,
 * then the boot image. SOS2 never writes lk2nd or any other partition. */
enum system_region { SYSTEM_BOOT, SYSTEM_CACHE, SYSTEM_REGIONS };

enum {
    SYSTEM_LK2ND_SECTORS = 1024,
    SYSTEM_SLOT_START = 2048,
    SYSTEM_SLOT_SECTORS = 131072,
    SYSTEM_APPS_SECTORS = 122880, /* the app store's */
    SYSTEM_DATA_START = SYSTEM_SLOT_START + 2 * SYSTEM_SLOT_SECTORS + SYSTEM_APPS_SECTORS,
    SYSTEM_DATA_MAX = 2097152, /* the largest volume fs.c mounts */
    SYSTEM_SLOT_HEADER = 4096,
    SYSTEM_BOOT_LIMIT = 4 * 1024 * 1024,
    SYSTEM_UPDATE_HEADER = 4096,
    SYSTEM_RELEASE_MAX = 47
};

/* A slot's first sector; the rest of its 4 KiB header is zero. */
struct system_slot {
    char magic[8];                                      /* "SOS2SLOT" */
    uint32_t version, data_offset, data_size, reserved; /* 1, 4096, size, 0 */
    uint8_t sha256[32];                                 /* of the data */
    uint32_t crc32;                                     /* of the 56 bytes above */
};

/* The first 4 KiB of sos2-redmi2.update; the rest is zero. The boot image
 * follows at 4 KiB, the compressed resources at the next 4 KiB boundary. */
struct system_update_header {
    char magic[8];                   /* "SOS2UPDT" */
    uint32_t version, header_size;   /* 1, 4096 */
    char board[16];                  /* "redmi2-wt88047" */
    char release[48];                /* 1-47 printable ASCII characters */
    uint32_t boot_offset, boot_size; /* 4096 and a whole number of 2 KiB pages */
    uint8_t boot_sha256[32];
    uint32_t system_offset, system_size;
    uint8_t system_sha256[32];
    uint32_t total_size; /* the file's */
    uint32_t crc32;      /* of the 164 bytes above */
};

_Static_assert(sizeof(struct system_slot) == 60, "slot header layout");
_Static_assert(sizeof(struct system_update_header) == 168, "update header layout");

/* The eMMC driver's view of the two regions (boards/redmi2/emmc.c): the boot
 * partition after lk2nd, and the cache partition, found by their GPT names.
 * Reads and writes go straight between the card and the caller's kernel
 * buffer; any failure stops the card for the boot. */
bool system_store_init(const struct platform_devices *);
uint64_t system_store_sectors(enum system_region);
bool system_store_read(enum system_region, uint64_t sector, void *data, unsigned count);
bool system_store_write(enum system_region, uint64_t sector, const void *data, unsigned count);
/* Flush the card's cache, so what was written survives a power loss. */
bool system_store_sync(void);
#endif
