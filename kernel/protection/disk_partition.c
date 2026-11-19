#include "disk_partition.h"
#include "platform.h"
#include "wire.h"
#include "sha256.h"

enum { SECTOR = 512, ENTRIES = 128, ENTRY = 128 };

/* The last validated table: its own count of 128-byte entries, from 1 to
 * 128 (UEFI's minimum reserved space holds 128; Qualcomm's tools declare 32). */
static unsigned char table[ENTRIES * ENTRY];
static unsigned entries;

static bool zero(const unsigned char *p, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
        if (p[i]) return false;
    return true;
}

/* The primary header, at LBA 1: its fields for this device, the entries at
 * LBA 2, and a CRC over its own size, from 92 bytes to a whole sector (the
 * Galaxy Tab A7 Lite's is 512). */
static bool header(const unsigned char *p, uint64_t sectors) {
    uint32_t size = le32(p + 12);
    if (memcmp(p, "EFI PART", 8) || le32(p + 8) != 0x10000 || size < 92 || size > SECTOR ||
        le32(p + 20))
        return false;
    if (le64(p + 24) != 1 || le64(p + 32) != sectors - 1) return false;
    if (le64(p + 40) < 34 || le64(p + 40) > le64(p + 48) || le64(p + 48) >= sectors - 33) return false;
    if (zero(p + 56, 16)) return false;
    if (le64(p + 72) != 2 || !le32(p + 80) || le32(p + 80) > ENTRIES || le32(p + 84) != ENTRY)
        return false;
    unsigned char copy[SECTOR];
    memcpy(copy, p, size);
    memset(copy + 16, 0, 4);
    return crc32_ieee(copy, size) == le32(p + 16);
}

/* The entry's UTF-16 name is exactly name, zero-padded. */
static bool label(const unsigned char *p, const char *name) {
    size_t length = strlen(name);
    for (unsigned i = 0; i < 36; ++i)
        if (p[i * 2] != (i < length ? (unsigned char)name[i] : 0) || p[i * 2 + 1]) return false;
    return true;
}

/* Read the primary GPT into table and check it against the device, as Linux
 * reads one (block/partitions/efi.c): a protective MBR, a CRC-valid header
 * and entries, and entries with unique GUIDs inside the usable range, none
 * overlapping. The backup is not read: vendors' tools leave it different
 * (the tablet's lists 128 entries to the primary's 56, without a disk GUID),
 * and Linux falls back to it only for a damaged primary, which SOS2
 * rejects instead. */
static bool validate(disk_read_sector read, void *context, uint64_t sectors) {
    if (!read || sectors < 4096 + 68 || sectors > UINT32_MAX) return false;
    unsigned char sector[SECTOR];
    if (!read(context, 0, sector)) return false;
    if (sector[510] != 0x55 || sector[511] != 0xaa) return false;
    /* No hybrid MBR: exactly one protective entry covering the device. Its
     * size is the device's, or 0xffffffff, which UEFI allows and Qualcomm's
     * partition tools write. */
    unsigned protective = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned char *p = sector + 446 + i * 16;
        if (zero(p, 16)) continue;
        if (p[0] || p[4] != 0xee || le32(p + 8) != 1 ||
            (le32(p + 12) != sectors - 1 && le32(p + 12) != UINT32_MAX))
            return false;
        ++protective;
    }
    if (protective != 1) return false;
    if (!read(context, 1, sector) || !header(sector, sectors)) return false;
    uint64_t first_usable = le64(sector + 40), last_usable = le64(sector + 48);
    uint32_t crc = le32(sector + 88);
    /* Only the declared entries: their sectors, their CRC. */
    entries = le32(sector + 80);
    unsigned bytes = entries * ENTRY;
    memset(table, 0, sizeof(table));
    for (unsigned i = 0; i < (bytes + SECTOR - 1) / SECTOR; ++i)
        if (!read(context, 2 + i, table + i * SECTOR)) return false;
    if (crc32_ieee(table, bytes) != crc) return false;
    for (unsigned i = 0; i < entries; ++i) {
        const unsigned char *p = table + i * ENTRY;
        if (zero(p, 16)) {
            if (!zero(p, ENTRY)) return false;
            continue;
        }
        uint64_t first = le64(p + 32), last = le64(p + 40);
        if (zero(p + 16, 16) || first < first_usable || last > last_usable || first > last)
            return false;
        for (unsigned j = 0; j < i; ++j) {
            const unsigned char *q = table + j * ENTRY;
            if (zero(q, 16)) continue;
            if (!memcmp(p + 16, q + 16, 16) || (first <= le64(q + 40) && le64(q + 32) <= last))
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
        uint64_t first = le64(p + 32), last = le64(p + 40);
        bool ours = !memcmp(p, type, 16), named = label(p + 56, "sos2-data");
        if (ours || named) {
            uint64_t size = last - first + 1;
            if (!ours || !named || candidate.sectors || le64(p + 48) || first % 8 || size < 4096 ||
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
        found = (struct disk_partition){le64(p + 32), le64(p + 40) - le64(p + 32) + 1};
    }
    if (!found.sectors) return false;
    *out = found;
    return true;
}
