#include "disk_partition.h"
#include "platform.h"

enum { SECTOR = 512, ENTRIES = 128, ENTRY = 128 };

/* The last validated table: its own count of 128-byte entries, from 1 to
 * 128 (UEFI's minimum reserved space holds 128; Qualcomm's tools declare 32). */
static unsigned char table[ENTRIES * ENTRY];
static unsigned entries;

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t u64(const unsigned char *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

static bool zero(const unsigned char *p, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
        if (p[i]) return false;
    return true;
}

static uint32_t crc32(const unsigned char *p, unsigned size) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc ^ UINT32_MAX;
}

static bool header(unsigned char *p, uint64_t own, uint64_t other, uint64_t table_lba,
                   uint64_t sectors) {
    if (memcmp(p, "EFI PART", 8)) return false;
    if (u32(p + 8) != 0x10000 || u32(p + 12) != 92 || u32(p + 20)) return false;
    if (u64(p + 24) != own || u64(p + 32) != other) return false;
    if (u64(p + 40) < 34 || u64(p + 40) > u64(p + 48) || u64(p + 48) >= sectors - 33) return false;
    if (zero(p + 56, 16)) return false;
    if (u64(p + 72) != table_lba || !u32(p + 80) || u32(p + 80) > ENTRIES || u32(p + 84) != ENTRY)
        return false;
    if (!zero(p + 92, 420)) return false;
    uint32_t expected = u32(p + 16);
    memset(p + 16, 0, 4);
    uint32_t actual = crc32(p, 92);
    return actual == expected;
}

/* The entry's UTF-16 name is exactly name, zero-padded. */
static bool label(const unsigned char *p, const char *name) {
    size_t length = strlen(name);
    for (unsigned i = 0; i < 36; ++i)
        if (p[i * 2] != (i < length ? (unsigned char)name[i] : 0) || p[i * 2 + 1]) return false;
    return true;
}

/* Read both GPTs into table and check them against each other and the
 * device: a protective MBR, CRC-valid headers and tables, and entries with
 * unique GUIDs inside the usable range, none overlapping. */
static bool validate(disk_read_sector read, void *context, uint64_t sectors) {
    if (!read || sectors < 4096 + 68 || sectors > UINT32_MAX) return false;
    unsigned char primary[SECTOR], backup[SECTOR];
    if (!read(context, 0, primary)) return false;
    if (primary[510] != 0x55 || primary[511] != 0xaa) return false;
    /* No hybrid MBR: exactly one protective entry covering the device. Its
     * size is the device's, or 0xffffffff, which UEFI allows and Qualcomm's
     * partition tools write. */
    unsigned protective = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned char *p = primary + 446 + i * 16;
        if (zero(p, 16)) continue;
        if (p[0] || p[4] != 0xee || u32(p + 8) != 1 ||
            (u32(p + 12) != sectors - 1 && u32(p + 12) != UINT32_MAX))
            return false;
        ++protective;
    }
    if (protective != 1) return false;
    if (!read(context, 1, primary) || !read(context, sectors - 1, backup)) return false;
    if (!header(primary, 1, sectors - 1, 2, sectors) ||
        !header(backup, sectors - 1, 1, sectors - 33, sectors))
        return false;
    if (memcmp(primary + 40, backup + 40, 32) || memcmp(primary + 80, backup + 80, 12))
        return false;
    /* Only the declared entries: their sectors, their CRC. */
    entries = u32(primary + 80);
    unsigned bytes = entries * ENTRY;
    memset(table, 0, sizeof(table));
    for (unsigned i = 0; i < (bytes + SECTOR - 1) / SECTOR; ++i) {
        if (!read(context, 2 + i, table + i * SECTOR) || !read(context, sectors - 33 + i, backup))
            return false;
        if (memcmp(table + i * SECTOR, backup, SECTOR)) return false;
    }
    uint32_t crc = crc32(table, bytes);
    if (crc != u32(primary + 88)) return false;
    for (unsigned i = 0; i < entries; ++i) {
        const unsigned char *p = table + i * ENTRY;
        if (zero(p, 16)) {
            if (!zero(p, ENTRY)) return false;
            continue;
        }
        uint64_t first = u64(p + 32), last = u64(p + 40);
        if (zero(p + 16, 16) || first < u64(primary + 40) || last > u64(primary + 48) ||
            first > last)
            return false;
        for (unsigned j = 0; j < i; ++j) {
            const unsigned char *q = table + j * ENTRY;
            if (zero(q, 16)) continue;
            if (!memcmp(p + 16, q + 16, 16) || (first <= u64(q + 40) && u64(q + 32) <= last))
                return false;
        }
    }
    return true;
}

bool disk_partition_find(disk_read_sector read, void *context, uint64_t sectors,
                         struct disk_partition *out) {
    if (!out) return false;
    *out = (struct disk_partition){0};
    if (!validate(read, context, sectors)) return false;
    static const unsigned char type[16] = SOS2_DATA_GUID_BYTES;
    struct disk_partition candidate = {0};
    for (unsigned i = 0; i < entries; ++i) {
        const unsigned char *p = table + i * ENTRY;
        if (zero(p, 16)) continue;
        uint64_t first = u64(p + 32), last = u64(p + 40);
        bool ours = !memcmp(p, type, 16), named = label(p + 56, "sos2-data");
        if (ours || named) {
            uint64_t size = last - first + 1;
            if (!ours || !named || candidate.sectors || u64(p + 48) || first % 8 || size < 4096 ||
                size > 2u * 1024 * 1024 || size % 16)
                return false;
            candidate = (struct disk_partition){first, size};
        }
    }
    if (!candidate.sectors) return false;
    *out = candidate;
    return true;
}

bool disk_partition_named(disk_read_sector read, void *context, uint64_t sectors, const char *name,
                          struct disk_partition *out) {
    if (!out || !name || !*name || strlen(name) > 36) return false;
    *out = (struct disk_partition){0};
    if (!validate(read, context, sectors)) return false;
    struct disk_partition found = {0};
    for (unsigned i = 0; i < entries; ++i) {
        const unsigned char *p = table + i * ENTRY;
        if (zero(p, 16) || !label(p + 56, name)) continue;
        if (found.sectors) return false;
        found = (struct disk_partition){u64(p + 32), u64(p + 40) - u64(p + 32) + 1};
    }
    if (!found.sectors) return false;
    *out = found;
    return true;
}
