/* The eMMC store on the devices (include/emmc_store.h). Every transfer stays
 * inside its region, through the board's driver: reads several sectors at a
 * time where it can, writes one at a time; a failure stops the card, and
 * both users, for the boot. */
#include "emmc_store.h"
#include "block.h"
#include "disk_partition.h"
#include "kernel_io.h"
#include "platform.h"
#include "system_store.h"

static bool ready, store_ready;
static uint64_t card_sectors;
static struct disk_partition partition, regions[SYSTEM_REGIONS];
static unsigned char bounce[4096] __attribute__((aligned(4)));

static bool read_sector(void *context, uint64_t sector, void *data) {
    (void)context;
    return emmc_card.read(sector, data);
}

/* count sectors from sector into out: in transfers of up to 256 where the
 * driver can and out is word-aligned, else one at a time. */
static bool read_sectors(uint64_t sector, unsigned char *out, unsigned count) {
    if (emmc_card.read_many && !((uintptr_t)out & 3)) {
        for (unsigned n; count; count -= n, sector += n, out += (size_t)n * 512) {
            n = count < 256 ? count : 256;
            if (!emmc_card.read_many(sector, out, n)) return false;
        }
        return true;
    }
    for (unsigned i = 0; i < count; ++i)
        if (!emmc_card.read(sector + i, out + (size_t)i * 512)) return false;
    return true;
}

bool block_init(const struct platform_devices *devices) {
    ready = false;
    partition = (struct disk_partition){0};
    /* An installed system keeps its data volume in the cache partition after
     * the app store (include/system_store.h), whatever other partitions say. */
    if (store_ready && emmc_card.serving()) {
        uint64_t sectors = regions[SYSTEM_CACHE].sectors;
        if (sectors <= SYSTEM_DATA_START) return false;
        sectors -= SYSTEM_DATA_START;
        if (sectors > SYSTEM_DATA_MAX) sectors = SYSTEM_DATA_MAX;
        partition = (struct disk_partition){regions[SYSTEM_CACHE].start + SYSTEM_DATA_START,
                                            sectors & ~(uint64_t)7};
        ready = partition.sectors != 0;
        return ready;
    }
    /* Otherwise a valid dedicated partition, where the board has one; a GPT
     * without it leaves the card for the installed system. */
    if (!emmc_card.dedicated_partition || !emmc_card.adopt(devices, &card_sectors) ||
        !disk_partition_find(read_sector, 0, card_sectors, &partition))
        return false;
    ready = true;
    return true;
}

uint64_t block_sectors(void) { return ready && emmc_card.serving() ? partition.sectors : 0; }

static bool valid(uint64_t sector, const void *data, unsigned count) {
    return ready && emmc_card.serving() && data && count && count <= 8 &&
           sector < partition.sectors && count <= partition.sectors - sector;
}

bool block_read(uint64_t sector, void *data, unsigned count) {
    if (!valid(sector, data, count) || !read_sectors(partition.start + sector, bounce, count))
        return false;
    memcpy(data, bounce, count * 512);
    return true;
}

bool block_write(uint64_t sector, const void *data, unsigned count) {
    if (!valid(sector, data, count)) return false;
    memcpy(bounce, data, count * 512);
    for (unsigned i = 0; i < count; ++i) {
        if (!emmc_card.write(partition.start + sector + i, bounce + i * 512)) return false;
        kernel_io_wait();
    }
    return true;
}

bool block_sync(void) { return ready && emmc_card.serving() && emmc_card.flush(); }

/* The boot and cache partitions, found by their GPT names, the boot
 * partition after what its loader keeps. The boot partition must take a whole
 * boot image there, the cache partition the app store. */
bool system_store_init(const struct platform_devices *devices) {
    if (store_ready && emmc_card.serving()) return true;
    struct disk_partition boot, cache;
    unsigned kept = emmc_card.loader_sectors;
    if (!emmc_card.adopt(devices, &card_sectors) ||
        !disk_partition_named(read_sector, 0, card_sectors, "boot", &boot) ||
        !disk_partition_named(read_sector, 0, card_sectors, "cache", &cache) ||
        boot.sectors < kept + SYSTEM_BOOT_LIMIT / 512 || cache.sectors < SYSTEM_DATA_START)
        return false;
    regions[SYSTEM_BOOT] = (struct disk_partition){boot.start + kept, boot.sectors - kept};
    regions[SYSTEM_CACHE] = cache;
    store_ready = true;
    return true;
}

uint64_t system_store_sectors(enum system_region region) {
    return store_ready && emmc_card.serving() && (unsigned)region < SYSTEM_REGIONS
               ? regions[region].sectors
               : 0;
}

static bool in_region(enum system_region region, uint64_t sector, const void *data,
                      unsigned count) {
    return store_ready && emmc_card.serving() && (unsigned)region < SYSTEM_REGIONS && data &&
           count && sector < regions[region].sectors && count <= regions[region].sectors - sector;
}

bool system_store_read(enum system_region region, uint64_t sector, void *data, unsigned count) {
    return in_region(region, sector, data, count) &&
           read_sectors(regions[region].start + sector, data, count);
}

bool system_store_write(enum system_region region, uint64_t sector, const void *data,
                        unsigned count) {
    if (!in_region(region, sector, data, count)) return false;
    const unsigned char *in = data;
    for (unsigned i = 0; i < count; ++i) {
        memcpy(bounce, in + (size_t)i * 512, 512);
        if (!emmc_card.write(regions[region].start + sector + i, bounce)) return false;
    }
    return true;
}

bool system_store_sync(void) { return store_ready && emmc_card.serving() && emmc_card.flush(); }
