#include "block.h"
#include "platform.h"
#include "kernel_io.h"
#include "system_store.h"

/* Virtio 1.2 modern MMIO, split virtqueue, block IN/OUT/FLUSH. RAM and DMA
 * remain uncached. The filesystem worker serializes requests and yields to
 * the shell/scheduler while waiting. Host scheduling and durable flushes can
 * exceed 100 ms; allow bounded seconds, without blocking the UI or a peer.
 * https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html */
struct descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};
static struct descriptor desc[8] __attribute__((aligned(16)));

static volatile struct {
    uint16_t flags, index, ring[8];
} avail __attribute__((aligned(16)));

static volatile struct {
    uint16_t flags, index;

    struct {
        uint32_t id, length;
    } ring[8];
} used __attribute__((aligned(16)));

static struct {
    uint32_t type, reserved;
    uint64_t sector;
} request __attribute__((aligned(16)));

static unsigned char bounce[4096] __attribute__((aligned(4096)));
static volatile unsigned char status;
static uintptr_t device;
static uint64_t capacity;
static uint16_t completed;

static void reg(unsigned at, uint32_t value) {
    barrier();
    mmio_write32(device + at, value);
    barrier();
}

static bool fail(void) {
    if (device) reg(0x70, 0); /* Revoke DMA before accepting another request. */
    device = 0;
    capacity = 0;
    return false;
}

bool block_init(const struct platform_devices *d) {
    device = 0;
    capacity = 0;
    completed = 0;
    for (size_t i = 0; i < d->virtio_count; ++i) {
        uintptr_t at = d->virtio[i];
        if (mmio_read32(at) == 0x74726976 && mmio_read32(at + 4) == 2 && mmio_read32(at + 8) == 2) {
            if (device) return fail();
            device = at;
        }
    }
    if (!device) return false;
    reg(0x70, 0);
    reg(0x70, 3);
    reg(0x14, 0);
    uint32_t features = mmio_read32(device + 0x10);
    if ((features & (1u << 5)) || !(features & (1u << 9))) return fail();
    reg(0x14, 1);
    if (!(mmio_read32(device + 0x10) & 1)) return fail();
    reg(0x24, 0);
    reg(0x20, 1u << 9);
    reg(0x24, 1);
    reg(0x20, 1);
    reg(0x70, 11);
    if (!(mmio_read32(device + 0x70) & 8)) return fail();
    reg(0x30, 0);
    if (mmio_read32(device + 0x34) < 8) return fail();
    reg(0x38, 8);
    memset(desc, 0, sizeof(desc));
    memset((void *)&avail, 0, sizeof(avail));
    memset((void *)&used, 0, sizeof(used));
    avail.flags = 1;
    reg(0x80, (uint32_t)(uintptr_t)desc);
    reg(0x84, (uint32_t)((uint64_t)(uintptr_t)desc >> 32));
    reg(0x90, (uint32_t)(uintptr_t)&avail);
    reg(0x94, (uint32_t)((uint64_t)(uintptr_t)&avail >> 32));
    reg(0xa0, (uint32_t)(uintptr_t)&used);
    reg(0xa4, (uint32_t)((uint64_t)(uintptr_t)&used >> 32));
    reg(0x44, 1);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint32_t generation = mmio_read32(device + 0xfc);
        uint32_t low = mmio_read32(device + 0x100), high = mmio_read32(device + 0x104);
        if (generation == mmio_read32(device + 0xfc)) {
            capacity = ((uint64_t)high << 32) | low;
            break;
        }
    }
    if (!capacity) return fail();
    reg(0x70, 15);
    return true;
}

uint64_t block_sectors(void) { return capacity; }

static bool submit(uint32_t type, uint64_t sector, unsigned bytes) {
    if (!device || (mmio_read32(device + 0x70) & 0xc0)) return false;
    request.type = type;
    request.reserved = 0;
    request.sector = sector;
    status = 0xff;
    desc[0] = (struct descriptor){(uintptr_t)&request, sizeof(request), 1, bytes ? 1 : 2};
    desc[1] = (struct descriptor){(uintptr_t)bounce, bytes, (uint16_t)(1 | (type == 0 ? 2 : 0)), 2};
    desc[2] = (struct descriptor){(uintptr_t)&status, 1, 2, 0};
    avail.ring[avail.index % 8] = 0;
    barrier();
    ++avail.index;
    barrier();
    reg(0x50, 0);
    uint64_t deadline = time_us() + (type == 4 ? 10000000 : 5000000);
    while (used.index == completed) {
        if (mmio_read32(device + 0x70) & 0xc0) return fail();
        if (time_us() >= deadline) {
            /* The device may have completed while the worker was descheduled. */
            barrier();
            if (used.index == completed) return fail();
            break;
        }
        kernel_io_wait();
        barrier();
    }
    barrier();
    if ((uint16_t)(used.index - completed) != 1 || used.ring[completed % 8].id ||
        used.ring[completed % 8].length != (type == 0 ? bytes + 1 : 1))
        return fail();
    ++completed;
    reg(0x64, mmio_read32(device + 0x60) & 3);
    return status == 0;
}

static bool valid(uint64_t sector, unsigned count) {
    return device && count && count <= 8 && sector < capacity && count <= capacity - sector;
}

bool block_read(uint64_t sector, void *data, unsigned count) {
    if (!valid(sector, count) || !submit(0, sector, count * 512)) return false;
    memcpy(data, bounce, count * 512);
    return true;
}

bool block_write(uint64_t sector, const void *data, unsigned count) {
    if (!valid(sector, count)) return false;
    memcpy(bounce, data, count * 512);
    return submit(1, sector, count * 512);
}

bool block_sync(void) { return submit(4, 0, 0); }

/* QEMU has no installed system (include/system_store.h): the updater
 * installs nothing here. */
uint64_t system_store_sectors(enum system_region region) {
    (void)region;
    return 0;
}

bool system_store_read(enum system_region region, uint64_t sector, void *data, unsigned count) {
    (void)region, (void)sector, (void)data, (void)count;
    return false;
}

bool system_store_write(enum system_region region, uint64_t sector, const void *data,
                        unsigned count) {
    (void)region, (void)sector, (void)data, (void)count;
    return false;
}

bool system_store_sync(void) { return false; }
