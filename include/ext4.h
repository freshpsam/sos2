#ifndef SOS2_EXT4_H
#define SOS2_EXT4_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Files read from an ext4 file system another system wrote: the Nexus 5's
 * Android /system, which holds its audio DSP's firmware
 * (kernel/protection/ext4.c). Read-only and by path, as make_ext4fs and
 * Linux lay it out: blocks of 1 to 4 KiB, 32- or 64-bit group descriptors,
 * files in extents, directories read linearly (a hashed one's index blocks
 * read as empty entries). Anything else it refuses, as it does every offset,
 * length and count the disk gives that does not fit. */
typedef bool (*ext4_read_sectors)(void *context, uint64_t sector, void *data, unsigned count);

struct ext4_volume {
    ext4_read_sectors read; /* 512-byte sectors into a word-aligned buffer */
    void *context;
    uint64_t sectors; /* the partition's */
    uint64_t blocks;
    uint64_t descriptors; /* the group descriptors' first block */
    uint32_t block_size, sectors_per_block, first_data_block;
    uint32_t inodes, inodes_per_group, inode_size, desc_size, groups;
};

struct ext4_file {
    uint64_t size;
    uint16_t mode;
    uint8_t root[60]; /* the extent tree's root, i_block */
};

bool ext4_mount(struct ext4_volume *, ext4_read_sectors, void *context, uint64_t sectors);
/* A regular file by its absolute path ("/vendor/firmware/adsp.mdt"). */
bool ext4_open(struct ext4_volume *, const char *path, struct ext4_file *);
/* count bytes of the file from offset into data, which whole blocks reach
 * in runs straight from the disk: data must be word-aligned, and offset a
 * multiple of 4. */
bool ext4_read(struct ext4_volume *, const struct ext4_file *, uint64_t offset, void *data,
               size_t count);
#endif
