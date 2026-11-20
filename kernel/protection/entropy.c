#include "entropy.h"
#include "platform.h"

/* Virtio RNG split queue. The device owns the buffer only while offered.
 * Like the input and display drivers, this uses uncached DMA memory. */
struct rng_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};

struct rng_used {
    uint16_t flags, index;
    uint32_t id, length;
};
static struct rng_descriptor descriptor __attribute__((aligned(16)));

static volatile struct {
    uint16_t flags, index, ring;
} available __attribute__((aligned(16)));
static volatile struct rng_used used __attribute__((aligned(16)));
static unsigned char buffer[256] __attribute__((aligned(16)));
static uintptr_t device;
static bool initialized;
static unsigned offset, count;
static uint16_t used_index;

static void reg(unsigned offset, uint32_t value) {
    mmio_write32(device + offset, value);
    barrier();
}

static void offer(void) {
    available.ring = 0;
    barrier();
    ++available.index;
    barrier();
    reg(0x50, 0);
}

bool entropy_init(const struct platform_devices *devices) {
    device = 0;
    initialized = false;
    offset = count = used_index = 0;
    for (size_t i = 0; i < devices->virtio_count; ++i) {
        uintptr_t at = devices->virtio[i];
        if (mmio_read32(at) == 0x74726976 && mmio_read32(at + 4) == 2 && mmio_read32(at + 8) == 4) {
            device = at;
            break;
        }
    }
    if (!device) return false;
    reg(0x70, 0);
    reg(0x70, 3);
    reg(0x14, 1);
    if (!(mmio_read32(device + 0x10) & 1)) return false;
    reg(0x24, 0);
    reg(0x20, 0);
    reg(0x24, 1);
    reg(0x20, 1);
    reg(0x70, 11);
    if (!(mmio_read32(device + 0x70) & 8)) return false;
    reg(0x30, 0);
    if (!mmio_read32(device + 0x34)) return false;
    reg(0x38, 1);
    descriptor =
        (struct rng_descriptor){.address = (uintptr_t)buffer, .length = sizeof(buffer), .flags = 2};
    available.flags = 1;
    available.index = 0;
    used.index = 0;
    reg(0x80, (uint32_t)(uintptr_t)&descriptor);
    reg(0x84, (uint32_t)((uint64_t)(uintptr_t)&descriptor >> 32));
    reg(0x90, (uint32_t)(uintptr_t)&available);
    reg(0x94, (uint32_t)((uint64_t)(uintptr_t)&available >> 32));
    reg(0xa0, (uint32_t)(uintptr_t)&used);
    reg(0xa4, (uint32_t)((uint64_t)(uintptr_t)&used >> 32));
    reg(0x44, 1);
    reg(0x70, 15);
    offer();
    initialized = true;
    return true;
}

bool entropy_available(void) {
    return initialized && device && !(mmio_read32(device + 0x70) & 0xc0);
}

const char *entropy_error(void) { return entropy_available() ? 0 : "Random device unavailable"; }

size_t entropy_read(void *output, size_t size) {
    if (!entropy_available()) return 0;
    if (!count) {
        uint16_t index = used.index;
        barrier();
        if (index == used_index) return 0;
        if ((uint16_t)(index - used_index) != 1 || used.id || !used.length ||
            used.length > sizeof(buffer)) {
            reg(0x70, 128);
            device = 0;
            return 0;
        }
        count = used.length;
        offset = 0;
        used_index = index;
        reg(0x64, mmio_read32(device + 0x60) & 3);
    }
    if (size > count) size = count;
    memcpy(output, buffer + offset, size);
    offset += size;
    count -= size;
    if (!count) offer();
    return size;
}

/* Virtio's reads are offered and completed through the service worker. */
size_t entropy_read_now(void *output, size_t size) {
    (void)output;
    (void)size;
    return 0;
}
