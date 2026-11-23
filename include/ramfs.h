#ifndef SOS2_RAMFS_H
#define SOS2_RAMFS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The temporary volume's file system (kernel/protection/ramfs.c): files and
 * directories in a region of RAM, written in place. littlefs, which the data
 * volume keeps, rewrites a file from its first changed byte to its end, so
 * every SQLite commit to the browser's working database, whose first page
 * always changes, rewrote all of it. Paths are absolute and canonical; errors
 * are include/user_abi.h's (and -39 for a directory that is not empty, as
 * littlefs gives). Nodes are counted while open: a removed or replaced file
 * stays readable and writable through them until the last is released. Only
 * the service worker calls it. */
enum { RAMFS_FILE = 1, RAMFS_DIR = 2, RAMFS_NAME_MAX = 127 };

enum { RAMFS_CREATE = 1, RAMFS_EXCLUSIVE = 2, RAMFS_TRUNCATE = 4 };

#define RAMFS_FILE_MAX (32u * 1024 * 1024)

struct ramfs_entry {
    unsigned type;
    uint32_t size;
    char name[RAMFS_NAME_MAX + 1];
};

/* An empty root over bytes of memory, a multiple of 4096 from a page boundary. */
bool ramfs_init(void *memory, size_t bytes);
/* A file opened, made first with RAMFS_CREATE, and counted until released. */
int ramfs_open(const char *path, unsigned flags, unsigned *node);
int ramfs_open_dir(const char *path, unsigned *node);
void ramfs_release(unsigned node);
uint32_t ramfs_size(unsigned node);
/* Bytes read (0 at the end) or written; a write needing more pages than are
 * free writes nothing. Unwritten ranges read as zeros. */
int ramfs_read(unsigned node, uint32_t offset, void *out, uint32_t size);
int ramfs_write(unsigned node, uint32_t offset, const void *data, uint32_t size);
int ramfs_truncate(unsigned node, uint32_t size);
/* The entry at *position, "." and ".." first, which it then advances: 1, or
 * 0 after the last. */
int ramfs_read_dir(unsigned node, unsigned *position, struct ramfs_entry *);
int ramfs_stat(const char *path, struct ramfs_entry *);
int ramfs_mkdir(const char *path);
/* A file or an empty directory. */
int ramfs_remove(const char *path);
/* A directory and everything below it. */
int ramfs_remove_tree(const char *path);
/* As littlefs renames: a file replaces a file, a directory an empty one. */
int ramfs_rename(const char *from, const char *to);
/* Pages files and their indexes may use: all, and those free. */
uint32_t ramfs_pages(void);
uint32_t ramfs_free_pages(void);
#endif
