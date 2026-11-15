#include "net_device.h"
#include "platform.h"

static uint8_t read8(uintptr_t at) { return *(volatile uint8_t *)at; }

/* Modern virtio 1.2: the header is 12 bytes even without merged RX buffers.
 * No offload, indirect descriptors, event index or multiqueue is negotiated.
 * DMA uses kernel identity mappings with uncached memory, like the other
 * existing virtio drivers. Enabling CPU caches requires a coherency contract. */
enum { QUEUE_SIZE = 64, HEADER_SIZE = 12, BUFFER_SIZE = HEADER_SIZE + NET_FRAME_MAX };

struct descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};

struct queue {
    struct descriptor desc[QUEUE_SIZE] __attribute__((aligned(16)));

    volatile struct {
        uint16_t flags, index, ring[QUEUE_SIZE];
    } avail __attribute__((aligned(16)));

    volatile struct {
        uint16_t flags, index;

        struct {
            uint32_t id, length;
        } ring[QUEUE_SIZE];
    } used __attribute__((aligned(16)));

    uint8_t buffers[QUEUE_SIZE][BUFFER_SIZE] __attribute__((aligned(16)));
    bool owned[QUEUE_SIZE];
    uint16_t completed;
};
static struct queue queues[2];
static struct net_device *interface;
static uintptr_t device;
static bool has_status;

static void reg(unsigned off, uint32_t value) {
    barrier();
    mmio_write32(device + off, value);
    barrier();
}

static void address(unsigned off, const void *value) {
    uint64_t physical = (uintptr_t)value;
    reg(off, (uint32_t)physical);
    reg(off + 4, (uint32_t)(physical >> 32));
}

static void stop(void) {
    if (device) reg(0x70, 0);
    device = 0;
}

static bool fail(void) {
    stop();
    return false;
}

static bool online(void) { return device != 0; }

static bool link(void) { return device && (!has_status || (read8(device + 0x106) & 1)); }

static void post(struct queue *q, unsigned id) {
    q->owned[id] = true;
    q->avail.ring[q->avail.index % QUEUE_SIZE] = (uint16_t)id;
    barrier();
    ++q->avail.index;
}

static bool complete(unsigned which, void (*receive)(const void *, unsigned)) {
    struct queue *q = &queues[which];
    uint16_t end = q->used.index;
    barrier();
    unsigned pending = (uint16_t)(end - q->completed);
    if (pending > QUEUE_SIZE) return fail();
    /* Validate the complete snapshot before delivering anything or reposting
     * buffers: duplicate IDs must not masquerade as newly completed buffers. */
    uint64_t seen = 0;
    for (unsigned i = 0; i < pending; ++i) {
        unsigned at = (uint16_t)(q->completed + i) % QUEUE_SIZE;
        uint32_t id = q->used.ring[at].id, length = q->used.ring[at].length;
        if (id >= QUEUE_SIZE || !q->owned[id] || (seen & (UINT64_C(1) << id)) ||
            (which ? length != 0 : length < HEADER_SIZE || length > BUFFER_SIZE))
            return fail();
        seen |= UINT64_C(1) << id;
    }
    unsigned budget = which ? QUEUE_SIZE : 32;
    if (pending > budget) pending = budget;
    uint16_t recycled[QUEUE_SIZE];
    for (unsigned i = 0; i < pending; ++i) {
        unsigned at = q->completed % QUEUE_SIZE, id = q->used.ring[at].id;
        unsigned length = q->used.ring[at].length;
        ++q->completed;
        q->owned[id] = false;
        if (!which) {
            const uint8_t *data = q->buffers[id];
            /* A short frame or unnegotiated checksum/GSO request is dropped.
             * num_buffers has no meaning without VIRTIO_NET_F_MRG_RXBUF. */
            if (length >= HEADER_SIZE + 14 && !data[0] && !data[1]) {
                ++interface->rx_packets;
                receive(data + HEADER_SIZE, length - HEADER_SIZE);
            }
            recycled[i] = (uint16_t)id;
        }
    }
    if (!which && pending) {
        for (unsigned i = 0; i < pending; ++i) post(q, recycled[i]);
        barrier();
        reg(0x50, 0);
    }
    return true;
}

static void poll(void (*receive)(const void *, unsigned)) {
    if (!device) return;
    if ((mmio_read32(device + 0x70) & 0xcf) != 15) {
        fail();
        return;
    }
    if (!complete(1, receive) || !complete(0, receive)) return;
    reg(0x64, mmio_read32(device + 0x60) & 3);
}

static bool send(const void *data, unsigned length) {
    if (!device || !link() || length < 14 || length > NET_FRAME_MAX) return false;
    struct queue *q = &queues[1];
    for (unsigned id = 0; id < QUEUE_SIZE; ++id)
        if (!q->owned[id]) {
            memset(q->buffers[id], 0, HEADER_SIZE);
            memcpy(q->buffers[id] + HEADER_SIZE, data, length);
            q->desc[id].length = HEADER_SIZE + length;
            post(q, id);
            barrier();
            reg(0x50, 1);
            ++interface->tx_packets;
            return true;
        }
    return false;
}

bool virtio_net_init(const struct platform_devices *devices, struct net_device *out) {
    device = 0;
    interface = out;
    has_status = false;
    *out = (struct net_device){
        .online = online, .link = link, .poll = poll, .send = send, .stop = stop};
    for (size_t i = 0; i < devices->virtio_count; ++i) {
        uintptr_t at = devices->virtio[i];
        if (mmio_read32(at) == 0x74726976 && mmio_read32(at + 4) == 2 && mmio_read32(at + 8) == 1) {
            if (device) return fail();
            device = at;
        }
    }
    if (!device) return false;
    reg(0x70, 0);
    if (mmio_read32(device + 0x70)) return fail();
    reg(0x70, 3);
    reg(0x14, 0);
    uint32_t low = mmio_read32(device + 0x10);
    reg(0x14, 1);
    if (!(mmio_read32(device + 0x10) & 1) || !(low & (1u << 5))) return fail();
    has_status = (low & (1u << 16)) != 0;
    reg(0x24, 0);
    reg(0x20, (1u << 5) | (has_status ? 1u << 16 : 0));
    reg(0x24, 1);
    reg(0x20, 1);
    reg(0x70, 11);
    if (!(mmio_read32(device + 0x70) & 8)) return fail();
    bool stable = false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        unsigned generation = mmio_read32(device + 0xfc);
        for (unsigned i = 0; i < 6; ++i) out->mac[i] = read8(device + 0x100 + i);
        if (generation == mmio_read32(device + 0xfc)) {
            stable = true;
            break;
        }
    }
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 6; ++i) nonzero |= out->mac[i];
    if (!stable || !nonzero || (out->mac[0] & 1)) return fail();
    memset(queues, 0, sizeof(queues));
    for (unsigned n = 0; n < 2; ++n) {
        struct queue *q = &queues[n];
        reg(0x30, n);
        if (mmio_read32(device + 0x34) < QUEUE_SIZE || mmio_read32(device + 0x44)) return fail();
        reg(0x38, QUEUE_SIZE);
        q->avail.flags = 1;
        for (unsigned id = 0; id < QUEUE_SIZE; ++id) {
            q->desc[id] = (struct descriptor){(uintptr_t)q->buffers[id], BUFFER_SIZE, n ? 0 : 2, 0};
            if (!n) post(q, id);
        }
        address(0x80, q->desc);
        address(0x90, (const void *)&q->avail);
        address(0xa0, (const void *)&q->used);
        reg(0x44, 1);
    }
    reg(0x70, 15);
    reg(0x50, 0);
    return true;
}
