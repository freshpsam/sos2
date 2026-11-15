#include "ramfb.h"
#include "platform.h"

static uint8_t read8(uintptr_t address) { return *(volatile uint8_t *)address; }

static void write16(uintptr_t address, uint16_t value) { *(volatile uint16_t *)address = value; }

static void write64(uintptr_t address, uint64_t value) { *(volatile uint64_t *)address = value; }

#define WIDTH  480
#define HEIGHT 800
static uint32_t pixels[WIDTH * HEIGHT] __attribute__((aligned(4096)));

struct dma_request {
    volatile uint32_t control;
    uint32_t length;
    uint64_t address;
};
static struct dma_request request __attribute__((aligned(16)));

struct ramfb_config {
    uint64_t address;
    uint32_t fourcc, flags, width, height, stride;
} __attribute__((packed));
static struct ramfb_config config;

static uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

static uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }

static void select_item(uintptr_t base, uint16_t item) {
    write16(base + 8, be16(item));
    barrier();
}

static void read_bytes(uintptr_t base, void *out, size_t size) {
    unsigned char *p = out;
    while (size--) *p++ = read8(base);
}

bool ramfb_init(uintptr_t base, struct framebuffer *fb) {
    if (!base) return false;
    char signature[4];
    select_item(base, 0);
    read_bytes(base, signature, 4);
    if (memcmp(signature, "QEMU", 4)) return false;
    uint32_t features;
    select_item(base, 1);
    read_bytes(base, &features, 4);
    if (!(features & 2)) return false;
    select_item(base, 0x19);
    uint32_t count;
    read_bytes(base, &count, 4);
    count = be32(count);
    if (count > 4096) return false;
    uint16_t item = 0;
    for (uint32_t i = 0; i < count; ++i) {
        struct {
            uint32_t size;
            uint16_t key, reserved;
            char name[56];
        } entry;

        read_bytes(base, &entry, sizeof(entry));
        if (!memcmp(entry.name, "etc/ramfb", 10) && be32(entry.size) == sizeof(config)) {
            item = be16(entry.key);
            break;
        }
    }
    if (!item) return false;
    config = (struct ramfb_config){.address = be64((uintptr_t)pixels),
                                   .fourcc = be32(0x34325258),
                                   .width = be32(WIDTH),
                                   .height = be32(HEIGHT),
                                   .stride = be32(WIDTH * 4)};
    request.control = be32(((uint32_t)item << 16) | 8 | 16);
    request.length = be32(sizeof(config));
    request.address = be64((uintptr_t)&config);
    barrier();
    write64(base + 16, be64((uintptr_t)&request));
    barrier();
    uint64_t start = time_us();
    unsigned polls = 0;
    while (request.control) {
        /* A wedged early-boot timer must not turn a missing DMA completion into
         * an infinite boot wait. Elapsed subtraction also handles timer wrap. */
        if ((be32(request.control) & 1) || time_us() - start >= 1000000 || ++polls == 1000000)
            return false;
        barrier();
    }
    *fb = (struct framebuffer){.pixels = pixels,
                               .width = WIDTH,
                               .height = HEIGHT,
                               .stride = WIDTH * 4,
                               .format = PIXEL_XRGB8888};
    return true;
}
