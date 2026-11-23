#ifndef SOS2_EMMC_STORE_H
#define SOS2_EMMC_STORE_H
#include "fdt.h"

/* The eMMC's two users on the devices (kernel/protection/emmc_store.c): the
 * data volume (block_*, include/block.h) and the installed system's boot and
 * cache regions (system_store_*, include/system_store.h), over the board's
 * card driver, which moves 512-byte sectors of the user area: one at a
 * time, or several in one transfer where it can. */
struct emmc_card {
    /* Takes over the card as the loader left it, once per boot: true while
     * it serves, with the user area's size in sectors. */
    bool (*adopt)(const struct platform_devices *, uint64_t *sectors);
    /* False once a failure has stopped the card for the boot. */
    bool (*serving)(void);
    /* A sector read into any buffer, or written from a word-aligned one
     * with its programming finished. */
    bool (*read)(uint64_t sector, void *data);
    bool (*write)(uint64_t sector, const void *data);
    /* count (at most 256) consecutive sectors read in one transfer into a
     * word-aligned buffer; NULL where the driver has no such read. */
    bool (*read_many)(uint64_t sector, void *data, unsigned count);
    /* count (at most 8) consecutive sectors written in one transfer from a
     * word-aligned buffer, their programming finished; NULL where the driver
     * has no such write. */
    bool (*write_many)(uint64_t sector, const void *data, unsigned count);
    /* The card's cache written back. */
    bool (*flush)(void);
    /* The boot partition's first sectors, which the loader keeps. */
    unsigned loader_sectors;
    /* Without an installed system, the data volume is a dedicated GPT
     * partition (the Redmi 2's; include/disk_partition.h). */
    bool dedicated_partition;
};

/* The board's driver: boards/<board>/emmc.c. */
extern const struct emmc_card emmc_card;
#endif
