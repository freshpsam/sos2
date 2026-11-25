#include "ext4.h"
#include "platform.h"
#include "wire.h"

/* A read-only reader of ext4 (include/ext4.h), after Linux's on-disk
 * structures (fs/ext4/ext4.h, ext4_extents.h): the superblock 1024 bytes
 * in; each group's descriptor, after the block that holds the superblock,
 * with the group's inode table; an inode's mode, size, flags and its
 * extent tree's root in i_block, where each node is a header and either
 * index entries (the first logical block below each child) or extents (a
 * run of logical blocks at a physical one); directories as chains of
 * entries. Metadata passes through one block buffer, one block at a time. */
enum {
    SUPERBLOCK = 1024,
    MAGIC = 0xef53,
    INCOMPAT_FILETYPE = 0x2,
    INCOMPAT_RECOVER = 0x4,
    INCOMPAT_EXTENTS = 0x40,
    INCOMPAT_64BIT = 0x80,
    INCOMPAT_FLEX_BG = 0x200,
    /* The rest change where data lies (meta_bg, inline data) or how it reads
     * (compression, encryption, case folding): refused. */
    INCOMPAT_KNOWN =
        INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG,
    ROOT_INODE = 2,
    EXTENTS_FL = 0x80000,
    INLINE_DATA_FL = 0x10000000,
    EXTENT_MAGIC = 0xf30a,
    EXTENT_ENTRY = 12,
    UNWRITTEN = 32768,
    MAX_BLOCK = 4096,
    MAX_DEPTH = 5,
    TYPE = 0xf000,
    DIRECTORY = 0x4000,
    REGULAR = 0x8000,
    NAME_MAX = 255,
    PATH_DEPTH = 40 /* components in a path */
};

static uint8_t block[MAX_BLOCK] __attribute__((aligned(8)));

/* count blocks from number into data, inside the volume and partition. */
static bool read_blocks(struct ext4_volume *v, uint64_t number, uint32_t count, void *data) {
    if (!count || number >= v->blocks || count > v->blocks - number) return false;
    uint64_t sector = number * v->sectors_per_block,
             sectors = (uint64_t)count * v->sectors_per_block;
    if (sector >= v->sectors || sectors > v->sectors - sector) return false;
    return v->read(v->context, sector, data, (unsigned)sectors);
}

bool ext4_mount(struct ext4_volume *v, ext4_read_sectors read, void *context, uint64_t sectors) {
    *v = (struct ext4_volume){.read = read, .context = context, .sectors = sectors};
    if (!read || sectors < 4 || !read(context, SUPERBLOCK / 512, block, 2)) return false;
    const uint8_t *s = block;
    uint32_t log = le32(s + 24), incompat = le32(s + 96), revision = le32(s + 76);
    if (le16(s + 56) != MAGIC || log > 2 || (incompat & ~(uint32_t)INCOMPAT_KNOWN) ||
        !(incompat & INCOMPAT_EXTENTS))
        return false;
    v->block_size = 1024u << log;
    v->sectors_per_block = v->block_size / 512;
    v->blocks = le32(s + 4);
    if (incompat & INCOMPAT_64BIT) v->blocks |= (uint64_t)le32(s + 0x150) << 32;
    v->first_data_block = le32(s + 20);
    v->inodes = le32(s);
    v->inodes_per_group = le32(s + 40);
    v->inode_size = revision ? le16(s + 88) : 128;
    v->desc_size = incompat & INCOMPAT_64BIT ? le16(s + 254) : 32;
    uint32_t per_group = le32(s + 32);
    if (v->blocks > sectors / v->sectors_per_block) v->blocks = 0; /* larger than its partition */
    if (!v->blocks || !per_group || !v->inodes_per_group || v->first_data_block >= v->blocks ||
        v->inode_size < 128 || v->inode_size > 512 || (v->inode_size & (v->inode_size - 1)) ||
        v->desc_size < 32 || v->desc_size > v->block_size || (v->desc_size & (v->desc_size - 1)) ||
        ((incompat & INCOMPAT_64BIT) && v->desc_size < 64))
        return false;
    uint64_t groups = (v->blocks - v->first_data_block + per_group - 1) / per_group;
    if (groups > UINT32_MAX / v->inodes_per_group ||
        (uint64_t)groups * v->inodes_per_group < v->inodes)
        return false;
    v->groups = (uint32_t)groups;
    v->descriptors = v->first_data_block + 1;
    return true;
}

/* An inode's mode, size, flags and extent root. */
static bool inode(struct ext4_volume *v, uint32_t number, struct ext4_file *out) {
    if (!number || number > v->inodes) return false;
    uint32_t group = (number - 1) / v->inodes_per_group, index = (number - 1) % v->inodes_per_group;
    if (group >= v->groups) return false;
    uint64_t at = (uint64_t)group * v->desc_size;
    if (!read_blocks(v, v->descriptors + at / v->block_size, 1, block)) return false;
    const uint8_t *d = block + at % v->block_size;
    uint64_t table = le32(d + 8);
    if (v->desc_size >= 64) table |= (uint64_t)le32(d + 0x28) << 32;
    uint64_t byte = (uint64_t)index * v->inode_size;
    uint64_t number_block = table + byte / v->block_size;
    if (number_block < table || !read_blocks(v, number_block, 1, block)) return false;
    const uint8_t *i = block + byte % v->block_size;
    uint32_t flags = le32(i + 32);
    if (!(flags & EXTENTS_FL) || (flags & INLINE_DATA_FL)) return false;
    *out = (struct ext4_file){.size = le32(i + 4) | (uint64_t)le32(i + 108) << 32, .mode = le16(i)};
    memcpy(out->root, i + 40, sizeof(out->root));
    return true;
}

/* The physical block of a logical one (*hole: none, reads as zeros), and
 * how many follow it in the same run, from the extent tree. */
static bool map(struct ext4_volume *v, const struct ext4_file *f, uint32_t logical,
                uint64_t *physical, uint32_t *run, bool *hole) {
    const uint8_t *node = f->root;
    uint32_t size = sizeof(f->root);
    int expected = -1;
    for (unsigned level = 0; level <= MAX_DEPTH; ++level) {
        unsigned entries = le16(node + 2), depth = le16(node + 6);
        if (le16(node) != EXTENT_MAGIC || entries > (size - 12) / EXTENT_ENTRY ||
            depth > MAX_DEPTH || (expected >= 0 && depth != (unsigned)expected))
            return false;
        const uint8_t *entry = node + 12;
        if (!depth) {
            uint32_t next = UINT32_MAX;
            for (unsigned i = 0; i < entries; ++i, entry += EXTENT_ENTRY) {
                uint32_t first = le32(entry), length = le16(entry + 4);
                bool unwritten = length > UNWRITTEN;
                if (unwritten) length -= UNWRITTEN;
                if (logical < first) {
                    if (first < next) next = first;
                    continue;
                }
                if (logical - first >= length) continue;
                uint64_t start =
                    ((uint64_t)le16(entry + 6) << 32 | le32(entry + 8)) + (logical - first);
                *run = length - (logical - first);
                *hole = unwritten;
                *physical = start;
                return unwritten || (start < v->blocks && *run <= v->blocks - start);
            }
            *hole = true;
            *run = next == UINT32_MAX ? 1 : next - logical;
            *physical = 0;
            return true;
        }
        /* The last child whose first block is at or below logical. */
        const uint8_t *child = 0;
        for (unsigned i = 0; i < entries; ++i, entry += EXTENT_ENTRY)
            if (le32(entry) <= logical) child = entry;
        if (!child) {
            *hole = true;
            *run = 1;
            *physical = 0;
            return true;
        }
        uint64_t leaf = (uint64_t)le16(child + 8) << 32 | le32(child + 4);
        if (!read_blocks(v, leaf, 1, block)) return false;
        node = block;
        size = v->block_size;
        expected = (int)depth - 1;
    }
    return false;
}

/* The inode of name in a directory, by a linear walk of its entries. */
static bool lookup(struct ext4_volume *v, const struct ext4_file *dir, const char *name,
                   size_t length, uint32_t *found) {
    if ((dir->mode & TYPE) != DIRECTORY || dir->size / v->block_size > UINT32_MAX) return false;
    uint32_t blocks = (uint32_t)((dir->size + v->block_size - 1) / v->block_size);
    for (uint32_t b = 0; b < blocks; ++b) {
        uint64_t physical;
        uint32_t run;
        bool hole;
        if (!map(v, dir, b, &physical, &run, &hole)) return false;
        if (hole) continue;
        if (!read_blocks(v, physical, 1, block)) return false;
        for (uint32_t at = 0; at + 8 <= v->block_size;) {
            const uint8_t *e = block + at;
            uint32_t number = le32(e), record = le16(e + 4), name_length = e[6];
            if (record < 8 || (record & 3) || record > v->block_size - at) return false;
            if (number && name_length == length && 8 + name_length <= record &&
                !memcmp(e + 8, name, length)) {
                *found = number;
                return true;
            }
            at += record;
        }
    }
    return false;
}

bool ext4_open(struct ext4_volume *v, const char *path, struct ext4_file *out) {
    if (!v->block_size || !path || *path != '/') return false;
    struct ext4_file at;
    if (!inode(v, ROOT_INODE, &at)) return false;
    for (unsigned depth = 0; *path; ++depth) {
        while (*path == '/') ++path;
        if (!*path) break;
        size_t length = 0;
        while (path[length] && path[length] != '/') ++length;
        uint32_t number;
        if (depth >= PATH_DEPTH || length > NAME_MAX || !lookup(v, &at, path, length, &number) ||
            !inode(v, number, &at))
            return false;
        path += length;
    }
    if ((at.mode & TYPE) != REGULAR) return false;
    *out = at;
    return true;
}

bool ext4_read(struct ext4_volume *v, const struct ext4_file *f, uint64_t offset, void *data,
               size_t count) {
    uint8_t *out = data;
    if (!v->block_size || ((uintptr_t)out & 3) || (offset & 3) || offset > f->size ||
        count > f->size - offset || (f->size && (f->size - 1) / v->block_size >= UINT32_MAX))
        return false;
    while (count) {
        uint32_t logical = (uint32_t)(offset / v->block_size), within = offset % v->block_size;
        uint64_t physical;
        uint32_t run;
        bool hole;
        if (!map(v, f, logical, &physical, &run, &hole)) return false;
        if (!within && count >= v->block_size) {
            /* Whole blocks, straight into data: at most 1 MiB a read. */
            uint32_t n = (uint32_t)(count / v->block_size), most = (1u << 20) / v->block_size;
            if (n > run) n = run;
            if (n > most) n = most;
            size_t bytes = (size_t)n * v->block_size;
            if (hole) memset(out, 0, bytes);
            else if (!read_blocks(v, physical, n, out)) return false;
            out += bytes;
            offset += bytes;
            count -= bytes;
            continue;
        }
        size_t n = v->block_size - within;
        if (n > count) n = count;
        if (hole) memset(out, 0, n);
        else {
            if (!read_blocks(v, physical, 1, block)) return false;
            memcpy(out, block + within, n);
        }
        out += n;
        offset += n;
        count -= n;
    }
    return true;
}
