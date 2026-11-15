#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "apps.h"
#include "lfs.h"

/* Host-side access to an existing native disk. An exclusive whole-file lock
 * also conflicts with QEMU's byte-range locks: never edit a mounted guest disk. */
struct disk {
    FILE *file;
    off_t base;
};

static int position(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, lfs_size_t size) {
    struct disk *d = c->context;
    return block < c->block_count && off <= 4096 && size <= 4096 - off &&
                   !fseeko(d->file, d->base + (off_t)block * 4096 + off, SEEK_SET)
               ? 0
               : LFS_ERR_IO;
}

static int read_blocks(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, void *p,
                       lfs_size_t n) {
    struct disk *d = c->context;
    return position(c, b, o, n) || fread(p, 1, n, d->file) != n ? LFS_ERR_IO : 0;
}

static int write_blocks(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, const void *p,
                        lfs_size_t n) {
    struct disk *d = c->context;
    return position(c, b, o, n) || fwrite(p, 1, n, d->file) != n ? LFS_ERR_IO : 0;
}

static int erase_blocks(const struct lfs_config *c, lfs_block_t b) {
    return b < c->block_count ? 0 : LFS_ERR_IO;
}

static int sync_blocks(const struct lfs_config *c) {
    struct disk *d = c->context;
    return fflush(d->file) || fsync(fileno(d->file)) ? LFS_ERR_IO : 0;
}

static bool valid_path(const char *p) {
    size_t n = strlen(p);
    if (n < 2 || n > 122 || p[0] != '/' || p[n - 1] == '/') return false;
    for (const char *at = p + 1; *at;) {
        const char *end = strchr(at, '/');
        size_t len = end ? (size_t)(end - at) : strlen(at);
        if (!len || (len == 1 && at[0] == '.') || (len == 2 && at[0] == '.' && at[1] == '.'))
            return false;
        at += len;
        if (*at) ++at;
    }
    return true;
}

/* Profiles as the kernel names them (include/fs.h): every web page uses
 * "browser"; an app uses "apps/<id>", with an id as kernel/protection/apps.c
 * accepts it. */
static bool valid_profile(const char *p) {
    if (!strcmp(p, "browser")) return true;
    return !strncmp(p, "apps/", 5) && app_id_valid(p + 5);
}

enum { DISK_PATH = 192 };

static int parents(lfs_t *fs, const char *name) {
    char path[DISK_PATH];
    strcpy(path, name);
    for (char *p = path + 1; *p; ++p)
        if (*p == '/') {
            *p = 0;
            int r = lfs_mkdir(fs, path);
            *p = '/';
            if (r && r != LFS_ERR_EXIST) return r;
        }
    return 0;
}

static int copy_file(lfs_t *fs, const char *operation, const char *path, const char *source) {
    bool reading = !strcmp(operation, "get"), create = !strcmp(operation, "create");
    struct lfs_info info;
    int r = lfs_stat(fs, path, &info);
    if (create && !r) return info.type == LFS_TYPE_REG ? 0 : LFS_ERR_ISDIR;
    if (r && r != LFS_ERR_NOENT) return r;
    FILE *input = reading ? NULL : fopen(source, "rb");
    if (!reading && !input) return LFS_ERR_IO;
    if (!reading && (r = parents(fs, path))) {
        fclose(input);
        return r;
    }
    char temporary[64];
    snprintf(temporary, sizeof(temporary), "/.sos2-import-%ld", (long)getpid());
    unsigned char cache[512], buffer[65536];
    struct lfs_file_config config = {.buffer = cache};
    lfs_file_t file;
    r = lfs_file_opencfg(fs, &file, reading ? path : temporary,
                         reading ? LFS_O_RDONLY : LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL, &config);
    if (r) {
        if (input) fclose(input);
        return r;
    }
    if (reading) {
        lfs_ssize_t n;
        while ((n = lfs_file_read(fs, &file, buffer, sizeof(buffer))) > 0)
            if (fwrite(buffer, 1, (size_t)n, stdout) != (size_t)n) {
                r = LFS_ERR_IO;
                break;
            }
        if (n < 0) r = (int)n;
    } else {
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), input))) {
            lfs_ssize_t written = lfs_file_write(fs, &file, buffer, n);
            if (written != (lfs_ssize_t)n) {
                r = written < 0 ? (int)written : LFS_ERR_IO;
                break;
            }
        }
        if (ferror(input)) r = LFS_ERR_IO;
        if (fclose(input) && !r) r = LFS_ERR_IO;
    }
    int closed = lfs_file_close(fs, &file);
    if (!r) r = closed;
    if (!reading) {
        if (!r) r = lfs_rename(fs, temporary, path);
        if (r) lfs_remove(fs, temporary);
    }
    return r;
}

int main(int argc, char **argv) {
    if (argc < 5 || argc > 6 || (!strcmp(argv[3], "get") ? argc != 5 : argc != 6) ||
        (strcmp(argv[3], "get") && strcmp(argv[3], "put") && strcmp(argv[3], "create")) ||
        !valid_path(argv[4])) {
        fprintf(stderr, "usage: fs-image IMAGE PROFILE get PATH | put|create PATH HOST_FILE\n"
                        "PROFILE is browser (every web page) or apps/<id>\n");
        return 2;
    }
    if (!valid_profile(argv[2])) {
        fprintf(stderr, "invalid profile: browser or apps/<id>\n");
        return 2;
    }
    /* PATH is the profile's /data path; the kernel keeps it below /profiles. */
    char path[DISK_PATH];
    snprintf(path, sizeof(path), "/profiles/%s%s", argv[2], argv[4]);
    FILE *f = fopen(argv[1], "r+b");
    if (!f) {
        perror("open image");
        return 1;
    }
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
    if (fcntl(fileno(f), F_SETLK, &lock)) {
        perror("disk is in use; stop QEMU before editing");
        fclose(f);
        return 1;
    }
    struct stat st;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size < 2 * 1024 * 1024 ||
        st.st_size > 1024ll * 1024 * 1024 || st.st_size % 4096) {
        fprintf(stderr, "invalid native disk image size/type\n");
        fclose(f);
        return 1;
    }
    struct disk disk = {f, 0};
    unsigned char rd[512], wr[512], lookahead[128];
    lfs_t fs;
    struct lfs_config config = {.context = &disk,
                                .read = read_blocks,
                                .prog = write_blocks,
                                .erase = erase_blocks,
                                .sync = sync_blocks,
                                .read_size = 512,
                                .prog_size = 512,
                                .block_size = 4096,
                                .block_count = st.st_size / 4096,
                                .block_cycles = -1,
                                .cache_size = 512,
                                .lookahead_size = 128,
                                .read_buffer = rd,
                                .prog_buffer = wr,
                                .lookahead_buffer = lookahead};
    int r = lfs_mount(&fs, &config);
    if (!r) {
        r = copy_file(&fs, argv[3], path, argc == 6 ? argv[5] : NULL);
        int u = lfs_unmount(&fs);
        if (!r) r = u;
    }
    if (!r) r = sync_blocks(&config);
    if (fclose(f) && !r) r = LFS_ERR_IO;
    if (r)
        fprintf(stderr, "native filesystem operation failed: %d (image is never formatted)\n", r);
    return r ? 1 : 0;
}
