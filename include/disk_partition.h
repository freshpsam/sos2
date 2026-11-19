#ifndef SOS2_DISK_PARTITION_H
#define SOS2_DISK_PARTITION_H
#include <stdbool.h>
#include <stdint.h>

/* On-disk GPT byte order (UUID 0f8d5c18-7f04-4af2-98d5-937c32e075a7).
 * This private type and the exact UTF-16 label "sos2-data" are both required. */
#define SOS2_DATA_GUID_BYTES \
    {0x18, 0x5c, 0x8d, 0x0f, 0x04, 0x7f, 0xf2, 0x4a, 0x98, 0xd5, 0x93, 0x7c, 0x32, 0xe0, 0x75, 0xa7}

struct disk_partition {
    uint64_t start, sectors;
};

typedef bool (*disk_read_sector)(void *context, uint64_t sector, void *data);
/* Read-only, serialized discovery from the primary GPT, as Linux reads it.
 * Rejects a damaged or ambiguous one; never repairs it or falls back to the
 * backup. Supports headers of 92 to 512 bytes and 1 to 128 entries of 128
 * bytes (Qualcomm's tools declare 32, the Galaxy Tab A7 Lite's 56) on
 * 512-byte sectors. */
bool disk_partition_find(disk_read_sector read, void *context, uint64_t sectors,
                         struct disk_partition *out);
/* The one partition whose UTF-16 GPT name is exactly name, whatever its type,
 * such as the phone's boot and cache partitions; false when there are none
 * or several, or the GPT is invalid. */
bool disk_partition_named(disk_read_sector read, void *context, uint64_t sectors, const char *name,
                          struct disk_partition *out);
#endif
