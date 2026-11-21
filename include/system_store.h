#ifndef SOS2_SYSTEM_STORE_H
#define SOS2_SYSTEM_STORE_H
#include "fdt.h"

/* The installed system on the eMMC (docs/redmi2.md#installing,
 * docs/gta7lite.md, docs/nexus5.md), as tools/package_redmi2.py,
 * tools/package_gta7lite.py and tools/package_nexus5.py lay it out:
 *   boot   the whole system: a boot image with the kernel, the device tree
 *          and the compressed resources (SORZ v3, include/boot_bundle.h) as
 *          its ramdisk, which the bootloader loads, and sos2.storage=installed
 *          in its command line. On the phone lk2nd keeps the first 512 KiB
 *          and starts the Android boot v0 image after them; the tablet's LK
 *          starts a header v2 image at the partition's start, the Nexus 5's
 *          stock bootloader a v0 image.
 *   cache  1 MiB left unused, then the app store (include/app_store.h), then
 *          the data volume (fs.c) to the end of the partition, at most 1 GiB:
 *          259 MiB of the Redmi 2's 320 MiB, 139 MiB of the tablet's 200 MiB,
 *          639 MiB of the Nexus 5's 700 MiB.
 * An update writes the boot image (kernel/protection/system_update.c). SOS2
 * never writes lk2nd or any other partition. */
enum system_region { SYSTEM_BOOT, SYSTEM_CACHE, SYSTEM_REGIONS };

enum {
    SYSTEM_LK2ND_SECTORS = 1024,
    SYSTEM_APPS_START = 2048,
    SYSTEM_APPS_SECTORS = 122880, /* the app store's */
    SYSTEM_DATA_START = SYSTEM_APPS_START + SYSTEM_APPS_SECTORS,
    SYSTEM_DATA_MAX = 2097152, /* the largest volume fs.c mounts */
#ifdef SOS2_BOARD_NEXUS5
    /* The largest boot image: the Nexus 5's whole 22 MiB boot partition. */
    SYSTEM_BOOT_LIMIT = 22 * 1024 * 1024,
#else
    /* The largest boot image: what lk2nd leaves of the phone's 32 MiB boot
     * partition, which the tablet's 32 MiB hold too. */
    SYSTEM_BOOT_LIMIT = 32 * 1024 * 1024 - 512 * 1024,
#endif
    SYSTEM_UPDATE_HEADER = 4096,
    SYSTEM_RELEASE_MAX = 47
};

/* The first 4 KiB of a device's update (sos2-redmi2.update and the
 * others); the rest is zero. The boot image follows at 4 KiB and ends the
 * file. */
struct system_update_header {
    char magic[8];                   /* "SOS2UPDT" */
    uint32_t version, header_size;   /* 2, 4096 */
    char board[16];                  /* "redmi2-wt88047", "gta7litewifi", "lge-hammerhead" */
    char release[48];                /* 1-47 printable ASCII characters */
    uint32_t boot_offset, boot_size; /* 4096 and a whole number of 2 KiB pages */
    uint8_t boot_sha256[32];
    uint32_t total_size; /* the file's */
    uint32_t crc32;      /* of the 124 bytes above */
};

_Static_assert(sizeof(struct system_update_header) == 128, "update header layout");

/* The two regions on the devices' eMMC (kernel/protection/emmc_store.c,
 * over boards/<board>/emmc.c): the boot partition (on the phone after
 * lk2nd) and the cache partition, found by their GPT names. Reads go
 * straight to the caller's kernel buffer; any failure stops the card for the
 * boot. */
bool system_store_init(const struct platform_devices *);
uint64_t system_store_sectors(enum system_region);
bool system_store_read(enum system_region, uint64_t sector, void *data, unsigned count);
bool system_store_write(enum system_region, uint64_t sector, const void *data, unsigned count);
/* Flush the card's cache, so what was written survives a power loss. */
bool system_store_sync(void);
#endif
