#include "qcom_mdt.h"
#include "platform.h"
#include "wire.h"

static struct firmware_file segment(firmware_lookup lookup, void *context, struct firmware_file mdt,
                                    const unsigned char *ph, unsigned index, bool split) {
    unsigned offset = le32(ph + 4), size = le32(ph + 16);
    if (!split && offset <= mdt.size && size <= mdt.size - offset)
        return (struct firmware_file){mdt.data + offset, size};
    char name[] = "/firmware/wcnss/wcnss.b00";
    name[sizeof(name) - 3] = (char)('0' + index / 10);
    name[sizeof(name) - 2] = (char)('0' + index % 10);
    struct firmware_file f = lookup(context, name);
    if (f.size != size) f.data = 0;
    return f;
}

bool qcom_mdt_prepare(firmware_lookup lookup, void *context, struct qcom_mdt *out, void *metadata,
                      size_t capacity) {
    if (!out) return false;
    *out = (struct qcom_mdt){0};
    if (!lookup || !metadata) return false;
    struct firmware_file file = lookup(context, "/firmware/wcnss/wcnss.mdt");
    const unsigned char *p = file.data;
    if (!p || file.size < 52 || file.size > WCNSS_MEMORY_SIZE || memcmp(p, "\177ELF\1\1\1", 7) ||
        le16(p + 16) != 2 || le16(p + 18) != 40 || le32(p + 20) != 1 || le32(p + 28) != 52 ||
        le16(p + 40) != 52 || le16(p + 42) != 32)
        return false;
    unsigned count = le16(p + 44), header_size = 52 + count * 32, hash_index = 0, hashes = 0;
    if (count < 2 || count > QCOM_MDT_SEGMENTS || header_size > file.size) return false;
    const unsigned char *first = p + 52;
    unsigned auth_header = le32(first + 16);
    if (le32(first) == 1 || le32(first + 4) || auth_header < header_size || auth_header > file.size)
        return false;
    uint32_t low = UINT32_MAX, high = 0, entry = le32(p + 24);
    bool entry_loaded = false, split = false;
    for (unsigned i = 1; i < count; ++i) {
        const unsigned char *ph = p + 52 + i * 32;
        uint32_t flags = le32(ph + 24), address = le32(ph + 12), bytes = le32(ph + 16),
                 memory = le32(ph + 20), offset = le32(ph + 4);
        if ((flags & 0x07000000) == 0x02000000) {
            hash_index = i;
            ++hashes;
            continue;
        }
        if (le32(ph) != 1 || !memory) continue;
        /* Stock relocates only flagged segments. SOS2 owns one relocation
         * arena, so fixed or mixed images cannot be loaded safely here. */
        if (!(flags & 0x08000000) || bytes > memory || address > UINT32_MAX - memory ||
            address + memory > UINT32_MAX - 4095)
            return false;
        if (address < low) low = address;
        uint32_t end = (address + memory + 4095) & ~4095u;
        if (end > high) high = end;
        entry_loaded |= entry >= address && entry < address + memory;
        if (bytes && (offset > file.size || bytes > file.size - offset)) split = true;
    }
    if (hashes != 1 || !entry_loaded || low == UINT32_MAX || (low & 4095) || high <= low ||
        high - low > WCNSS_MEMORY_SIZE)
        return false;
    const unsigned char *hp = p + 52 + hash_index * 32;
    unsigned hash_size = le32(hp + 16);
    if (!hash_size || auth_header > capacity || hash_size > capacity - auth_header ||
        auth_header + hash_size > QCOM_MDT_METADATA)
        return false;
    struct firmware_file hash;
    if ((uint64_t)auth_header + hash_size == file.size)
        hash = (struct firmware_file){p + auth_header, hash_size};
    else hash = segment(lookup, context, file, hp, hash_index, false);
    if (!hash.data) return false;
    struct qcom_mdt plan = {.span = high - low, .metadata_size = auth_header + hash_size};
    for (unsigned i = 1; i < count; ++i) {
        const unsigned char *ph = p + 52 + i * 32;
        uint32_t memory = le32(ph + 20), bytes = le32(ph + 16), address = le32(ph + 12);
        if (i == hash_index || le32(ph) != 1 || !memory) continue;
        struct firmware_file data = {0};
        if (bytes) data = segment(lookup, context, file, ph, i, split);
        if (bytes && !data.data) return false;
        unsigned offset = address - low;
        for (unsigned j = 0; j < plan.count; ++j) {
            const struct qcom_mdt_segment *s = &plan.segments[j];
            if (offset < s->offset + s->memory && s->offset < offset + memory) return false;
        }
        plan.segments[plan.count++] = (struct qcom_mdt_segment){data.data, offset, bytes, memory};
    }
    if (!plan.count) return false;
    memcpy(metadata, p, auth_header);
    memcpy((unsigned char *)metadata + auth_header, hash.data, hash_size);
    *out = plan;
    return true;
}
