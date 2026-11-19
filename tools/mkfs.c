#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "lfs.h"

/* Creates a NEW regular image holding one littlefs volume with the SOS2
 * layout (/settings, /profiles). Never formats an existing path or device;
 * nor does the kernel format such a disk when it does not mount. */
static int read_blocks(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *data,
                       lfs_size_t size) {
    FILE *file = c->context;
    if (block >= c->block_count || off > 4096 || size > 4096 - off ||
        fseeko(file, (off_t)block * 4096 + off, SEEK_SET))
        return LFS_ERR_IO;
    return fread(data, 1, size, file) == size ? 0 : LFS_ERR_IO;
}

static int write_blocks(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                        const void *data, lfs_size_t size) {
    FILE *file = c->context;
    if (block >= c->block_count || off > 4096 || size > 4096 - off ||
        fseeko(file, (off_t)block * 4096 + off, SEEK_SET))
        return LFS_ERR_IO;
    return fwrite(data, 1, size, file) == size ? 0 : LFS_ERR_IO;
}

static int erase_blocks(const struct lfs_config *c, lfs_block_t block) {
    return block < c->block_count ? 0 : LFS_ERR_IO;
}

static int sync_blocks(const struct lfs_config *c) {
    FILE *file = c->context;
    return fflush(file) || fsync(fileno(file)) ? LFS_ERR_IO : 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: mkfs NEW_IMAGE SIZE_MIB (2..1024)\n");
        return 2;
    }
    char *end;
    unsigned long mib = strtoul(argv[2], &end, 10);
    if (*end || mib < 2 || mib > 1024) {
        fprintf(stderr, "invalid size\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "w+bx");
    if (!f) {
        perror("create new image (existing files are never replaced)");
        return 1;
    }
    unsigned bytes = (unsigned)mib * 1024 * 1024;
    bool ok = ftruncate(fileno(f), bytes) == 0;
    unsigned char read_cache[512], write_cache[512], lookahead[128];
    lfs_t fs;
    struct lfs_config config = {.context = f,
                                .read = read_blocks,
                                .prog = write_blocks,
                                .erase = erase_blocks,
                                .sync = sync_blocks,
                                .read_size = 512,
                                .prog_size = 512,
                                .block_size = 4096,
                                .block_count = bytes / 4096,
                                .block_cycles = -1,
                                .cache_size = 512,
                                .lookahead_size = 128,
                                .read_buffer = read_cache,
                                .prog_buffer = write_cache,
                                .lookahead_buffer = lookahead};
    if (ok) ok = lfs_format(&fs, &config) == 0 && lfs_mount(&fs, &config) == 0;
    if (ok) {
        ok = lfs_mkdir(&fs, "/settings") == 0 && lfs_mkdir(&fs, "/profiles") == 0;
        if (lfs_unmount(&fs)) ok = false;
    }
    if (fclose(f)) ok = false;
    if (!ok) {
        unlink(argv[1]);
        fprintf(stderr, "format failed\n");
        return 1;
    }
    printf("Created %s: %lu MiB, one native littlefs volume\n", argv[1], mib);
    return 0;
}
