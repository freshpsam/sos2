#include "qcom_smem.h"
#include "qcom_smd.h"
#include "platform.h"

enum { TOC = 208, ITEMS = 512, HEADER = TOC + 16 * ITEMS };

enum {
    PTABLE_BYTES = 4096,
    PTABLE_HEADER = 32,
    PTABLE_ENTRY = 48,
    PART_HEADER = 32,
    ITEM_HEADER = 16
};

struct partition {
    volatile unsigned char *base;
    unsigned uncached, cached;
};

static uint32_t get(const volatile unsigned char *p) {
    uint32_t n = *(const volatile uint32_t *)p;
    barrier();
    return n;
}

static void put(volatile unsigned char *p, uint32_t n) {
    barrier();
    *(volatile uint32_t *)p = n;
}

static int invalid(struct smem_region *s, const char *why, uint32_t a, uint32_t b) {
    if (!s) return -1;
    unsigned n = 0;
    const char prefix[] = "SMEM ";
    memcpy(s->error, prefix, sizeof(prefix) - 1);
    n = sizeof(prefix) - 1;
    while (*why && n < sizeof(s->error) - 19) s->error[n++] = *why++;
    s->error[n++] = ' ';
    const uint32_t values[] = {a, b};
    for (unsigned j = 0; j < 2; ++j) {
        if (j) s->error[n++] = '/';
        for (unsigned i = 0; i < 8; ++i)
            s->error[n++] = "0123456789abcdef"[(values[j] >> (28 - 4 * i)) & 15];
    }
    s->error[n] = 0;
    return -1;
}

static bool valid(struct smem_region *s) {
    if (!s || !s->base || ((uintptr_t)s->base & 7) || (s->size & 7) || s->size < HEADER ||
        s->size > 1024 * 1024 || !s->lock || !s->unlock) {
        invalid(s, "mapping", s ? (uint32_t)(uintptr_t)s->base : 0, s ? (uint32_t)s->size : 0);
        return false;
    }
    volatile unsigned char *p = s->base;
    uint32_t free = get(p + 196), available = get(p + 200), version = get(p + 64 + 7 * 4),
             initialized = get(p + 192);
    if (initialized != 1 || get(p + 204) || (version >> 16) != 11) {
        invalid(s, "header/version", initialized, version);
        return false;
    }
    if ((free & 7) || free < HEADER || free > s->size || available > s->size - free) {
        invalid(s, "global bounds", free, available);
        return false;
    }
    return true;
}

/* The vendor ignores a slot if any of these three fields is zero, before
 * reading its partition header. In particular, a zero-cacheline slot can
 * retain an old offset/size/host pair without creating a private namespace. */
static bool active_slot(const volatile unsigned char *e) {
    return get(e) && get(e + 4) && get(e + 16);
}

/* SMEM v11 can contain private heaps as well as the legacy global TOC. The
 * bootloader's $TOC lives in the final 4 KiB; only read a partition's body if
 * its host pair grants APPS access. Metadata is checked under mutex 3 before
 * using offsets. Selection follows the vendor's first matching APPS/peer
 * slot; other processors' partition bodies are never inspected. */
static int select_heap(struct smem_region *s, unsigned peer, struct partition *part,
                       unsigned *global_limit) {
    unsigned table_at = (unsigned)s->size - PTABLE_BYTES;
    volatile unsigned char *table = s->base + table_at, *selected = 0;
    *part = (struct partition){0};
    *global_limit = (unsigned)s->size;
    if (get(table) != 0x434f5424) return 0;
    unsigned count = get(table + 8);
    if (get(table + 4) != 1 || count > (PTABLE_BYTES - PTABLE_HEADER) / PTABLE_ENTRY)
        return invalid(s, "table version/count", get(table + 4), count);
    *global_limit = table_at;
    for (unsigned i = 0; i < count; ++i) {
        volatile unsigned char *e = table + PTABLE_HEADER + i * PTABLE_ENTRY;
        unsigned offset = get(e), hosts = get(e + 12);
        if (!active_slot(e)) continue;
        /* This conservative fence is only used for new global allocations.
         * Global reads and a peer's heap must not depend on another heap's
         * free pointer or on parsing unrelated partition descriptors. */
        if (offset < *global_limit) *global_limit = offset;
        if (!selected && peer != SMEM_HOST_GLOBAL && (hosts == peer || hosts == (peer << 16)))
            selected = e;
    }
    if (!selected) return 0;
    unsigned offset = get(selected), extent = get(selected + 4);
    if ((offset & 7) || (extent & 7) || offset < HEADER || extent < PART_HEADER ||
        offset > table_at || extent > table_at - offset)
        return invalid(s, "partition extent", offset, extent);
    /* Cacheline padding applies to cached items only. The vendor's flags=0
     * path uses eight-byte alignment regardless of the nonzero cacheline. */
    for (unsigned i = 0; i < count; ++i) {
        volatile unsigned char *e = table + PTABLE_HEADER + i * PTABLE_ENTRY;
        unsigned at = get(e), size = get(e + 4), hosts = get(e + 12);
        if (hosts == peer || hosts == (peer << 16)) continue; /* Stock ignores later duplicates. */
        if (active_slot(e) && at <= table_at && size <= table_at - at && offset < at + size &&
            at < offset + extent)
            return invalid(s, "partition overlap", offset, at);
    }
    volatile unsigned char *p = s->base + offset;
    unsigned hosts = get(p + 4), size = get(p + 8), uncached = get(p + 12), cached = get(p + 16);
    if (get(p) != 0x54525024 || (hosts != peer && hosts != (peer << 16)))
        return invalid(s, "partition magic/hosts", get(p), hosts);
    if (size < PART_HEADER || size > extent || (size & 7))
        return invalid(s, "partition size", size, extent);
    if ((uncached & 7) || (cached & 7) || uncached < PART_HEADER || uncached > cached ||
        cached > size)
        return invalid(s, "partition cursors", uncached, cached);
    *part = (struct partition){p, uncached, cached};
    return 1;
}

static int lookup_private(struct smem_region *s, const struct partition *part, unsigned id,
                          struct smem_item *out) {
    struct smem_item found = {0};
    unsigned at = PART_HEADER;
    uint32_t seen[ITEMS / 32] = {0};
    while (at < part->uncached) {
        unsigned left = part->uncached - at;
        if (left < ITEM_HEADER) return invalid(s, "private header", at, left);
        volatile unsigned char *p = part->base + at;
        unsigned tag = get(p), size = get(p + 4), padding = get(p + 8), data_pad = padding & 65535,
                 header_pad = padding >> 16;
        if ((tag & 65535) != 0xa5a5 || (tag >> 16) < 8 || (tag >> 16) >= ITEMS)
            return invalid(s, "private item tag", at, tag);
        if (!size || (size & 7) || data_pad >= size || (header_pad & 7) ||
            header_pad > left - ITEM_HEADER || size > left - ITEM_HEADER - header_pad)
            return invalid(s, "private item bounds", tag >> 16, size);
        unsigned item = tag >> 16;
        uint32_t bit = 1u << (item % 32);
        if (seen[item / 32] & bit) return invalid(s, "duplicate private item", item, at);
        seen[item / 32] |= bit;
        if ((tag >> 16) == id)
            found = (struct smem_item){p + ITEM_HEADER + header_pad, (size - data_pad + 7) & ~7u};
        at += ITEM_HEADER + header_pad + size;
    }
    if (!found.data) return 0;
    *out = found;
    return 1;
}

static int lookup_global(struct smem_region *s, unsigned id, struct smem_item *out) {
    volatile unsigned char *entry = s->base + TOC + id * 16;
    uint32_t allocated = get(entry);
    if (!allocated) return 0;
    uint32_t offset = get(entry + 4), size = get(entry + 8), aux = get(entry + 12) & ~3u;
    bool global = !aux || aux == s->physical;
    volatile unsigned char *base = global ? s->base : s->aux;
    size_t limit = global ? get(s->base + 196) : s->aux_size;
    /* The v11 allocated field is boolean; the vendor accepts any nonzero value. */
    if (!global && (aux != s->aux_physical || !base || ((uintptr_t)base & 7)))
        return invalid(s, "global item region", id, aux);
    if ((offset & 7) || !size || (size & 7) || (global && offset < HEADER) || offset > limit ||
        size > limit - offset)
        return invalid(s, "global item bounds", id, offset);
    for (unsigned i = 8; i < ITEMS; ++i)
        if (i != id) {
            volatile unsigned char *other = s->base + TOC + i * 16;
            uint32_t other_aux = get(other + 12) & ~3u;
            if (!get(other) ||
                (global ? (other_aux && other_aux != s->physical) : other_aux != aux))
                continue;
            uint32_t at = get(other + 4), length = get(other + 8);
            if (at > UINT32_MAX - length || (offset < at + length && at < offset + size))
                return invalid(s, "global item overlap", id, i);
        }
    *out = (struct smem_item){base + offset, size};
    return 1;
}

static bool arguments(struct smem_region *s, unsigned peer, unsigned id, struct smem_item *out) {
    if (out) *out = (struct smem_item){0};
    if (s) s->error[0] = 0;
    if (out && s && s->lock && s->unlock && id >= 8 && id < ITEMS &&
        (peer == SMEM_HOST_GLOBAL || (peer > 0 && peer <= SMEM_HOST_RPM)))
        return true;
    invalid(s, "arguments", peer, id);
    return false;
}

enum smem_status smem_get_peer_status(struct smem_region *s, unsigned peer, unsigned id,
                                      struct smem_item *out) {
    if (!arguments(s, peer, id, out)) return SMEM_INVALID;
    if (!s->lock(s->context)) return SMEM_BUSY;
    int result = -1;
    if (valid(s)) {
        if (peer == SMEM_HOST_GLOBAL) result = lookup_global(s, id, out);
        else {
            struct partition part;
            unsigned limit;
            result = select_heap(s, peer, &part, &limit);
            if (result >= 0)
                result = part.base ? lookup_private(s, &part, id, out) : lookup_global(s, id, out);
        }
    }
    s->unlock(s->context);
    return result;
}

enum smem_status smem_get_status(struct smem_region *s, unsigned id, struct smem_item *out) {
    return smem_get_peer_status(s, SMEM_HOST_GLOBAL, id, out);
}

static int smem_get_peer(struct smem_region *s, unsigned peer, unsigned id, struct smem_item *out) {
    enum smem_status result = smem_get_peer_status(s, peer, id, out);
    return result == SMEM_BUSY ? 0 : result;
}

int smem_get(struct smem_region *s, unsigned id, struct smem_item *out) {
    return smem_get_peer(s, SMEM_HOST_GLOBAL, id, out);
}

int smem_allocate_peer(struct smem_region *s, unsigned peer, unsigned id, unsigned size,
                       struct smem_item *out) {
    if (!arguments(s, peer, id, out)) return -1;
    if (!size || size > 4096) return invalid(s, "allocation size", id, size);
    if (!s->lock(s->context)) return 0;
    int result = -1;
    if (!valid(s)) goto done;
    struct partition part;
    unsigned limit, padded = (size + 7) & ~7u;
    result = select_heap(s, peer, &part, &limit);
    if (result < 0) goto done;
    result = part.base ? lookup_private(s, &part, id, out) : lookup_global(s, id, out);
    if (result) {
        if (result == 1 && out->size != padded)
            result = invalid(s, "existing item size", out->size, padded);
        goto done;
    }
    if (part.base) {
        if (ITEM_HEADER + padded > part.cached - part.uncached) {
            result = invalid(s, "private heap full", id, part.cached - part.uncached);
            goto done;
        }
        volatile unsigned char *p = part.base + part.uncached;
        for (unsigned i = 0; i < ITEM_HEADER + padded; ++i) p[i] = 0;
        put(p, 0xa5a5 | (id << 16));
        put(p + 4, padded);
        put(p + 8, padded - size);
        /* Publish the complete item before moving the peer-visible frontier.
         * The cached frontier and all existing items remain untouched. */
        put(part.base + 12, part.uncached + ITEM_HEADER + padded);
        barrier();
        *out = (struct smem_item){p + ITEM_HEADER, padded};
        result = 1;
        goto done;
    }
    size = padded;
    uint32_t free = get(s->base + 196), available = get(s->base + 200);
    if (free > limit) {
        result = invalid(s, "global/partition overlap", free, limit);
        goto done;
    }
    if (size > available || size > limit - free) {
        result = invalid(s, "global heap full", size, limit - free);
        goto done;
    }
    for (unsigned i = 8; i < ITEMS; ++i) {
        volatile unsigned char *p = s->base + TOC + i * 16;
        if (!get(p) || ((get(p + 12) & ~3u) && (get(p + 12) & ~3u) != s->physical)) continue;
        uint32_t at = get(p + 4), n = get(p + 8);
        if (at > free || n > free - at) {
            result = invalid(s, "global allocated extent", i, at);
            goto done;
        }
    }
    for (unsigned i = 0; i < size; ++i) s->base[free + i] = 0;
    volatile unsigned char *entry = s->base + TOC + id * 16;
    put(entry + 4, free);
    put(entry + 8, size);
    put(entry + 12, 0);
    put(s->base + 196, free + size);
    put(s->base + 200, available - size);
    put(entry, 1);
    barrier();
    *out = (struct smem_item){s->base + free, size};
    result = 1;
done:
    if (result < 0) *out = (struct smem_item){0};
    s->unlock(s->context);
    return result;
}

int smem_smd_find(struct smem_region *s, unsigned edge, const char *name, struct smem_item *info,
                  struct smem_item *fifo) {
    if (s) s->error[0] = 0;
    if (info) *info = (struct smem_item){0};
    if (fifo) *fifo = (struct smem_item){0};
    if (!s || !name || !info || !fifo || strlen(name) > 19 || edge > 255)
        return invalid(s, "SMD arguments", edge, 0);
    unsigned peer;
    switch (edge) {
        case 0: peer = 1; break;
        case 1: peer = 2; break;
        case 3: peer = 3; break;
        case 6: peer = SMEM_HOST_WCNSS; break;
        case 10: peer = 5; break;
        case 15: peer = SMEM_HOST_RPM; break;
        default: return invalid(s, "SMD edge", edge, 0); /* APPS must be an endpoint. */
    }
    static const unsigned tables[][3] = {{13, 14, 338}, {266, 138, 202}};
    bool found = false;
    struct smem_item selected_info = {0}, selected_fifo = {0};
    for (unsigned table = 0; table < 2; ++table) {
        struct smem_item allocation;
        int result = smem_get_peer(s, peer, tables[table][0], &allocation);
        if (result < 0) return -1;
        if (!result) continue;
        if (!allocation.size || allocation.size % 32 || allocation.size > 64 * 32)
            return invalid(s, "SMD table size", tables[table][0], allocation.size);
        for (unsigned i = 0; i < allocation.size / 32; ++i) {
            volatile unsigned char *p = allocation.data + i * 32;
            if (!get(p + 28)) continue;
            uint32_t flags = get(p + 24), cid = get(p + 20);
            if ((flags & 255) != edge || !p[0]) continue;
            char candidate[20];
            for (unsigned j = 0; j < 20; ++j) candidate[j] = (char)p[j];
            candidate[19] = 0; /* Stock truncates the fixed-width name after copying. */
            if (strcmp(candidate, name)) continue;
            if (found) return invalid(s, "SMD duplicate channel", edge, cid);
            if (cid >= 64 || ((flags >> 8) & 15) != 2)
                return invalid(s, "SMD channel flags/id", flags, cid);
            result = smem_get_peer(s, peer, tables[table][1] + cid, &selected_info);
            if (result != 1) return result;
            result = smem_get_peer(s, peer, tables[table][2] + cid, &selected_fifo);
            if (result != 1) return result;
            if (selected_info.size != (peer == SMEM_HOST_RPM ? 88u : 40u) ||
                !smd_fifo_size_valid(selected_fifo.size))
                return invalid(s, "SMD info/FIFO size", selected_info.size, selected_fifo.size);
            found = true;
        }
    }
    if (!found) return 0;
    *info = selected_info;
    *fifo = selected_fifo;
    return 1;
}
