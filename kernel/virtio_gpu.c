#include "virtio_gpu.h"
#include "platform.h"
#include "user_abi.h"
#include "pixel_frame.h"
#include "kernel_io.h"

/* Virtio 1.2 sections 4.2 and 5.7: modern MMIO, split queues, one command in
 * flight, without fences (see submit_from).
 * https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html
 * Optional virgl composes with kernel-built texture copies in a private context.
 * Pages may also get their own virgl contexts (virtio_gpu_client_*): their
 * command streams are copied into kernel memory and only reach resources the
 * kernel attached to that context. No blobs, indirect descriptors or event-index.
 * Static BSS is identity mapped and uncached on SOS2, before and after vm_init.
 * All DMA storage stays reserved even if the device refuses reset.
 */
enum {
    WIDTH = 480,
    HEIGHT = 800,
    QUEUE_SIZE = 8,
    TIMEOUT_US = 1000000,
    POLL_LIMIT = 100000,
    GET_DISPLAY_INFO = 0x100,
    CREATE_2D = 0x101,
    SET_SCANOUT = 0x103,
    FLUSH = 0x104,
    TRANSFER = 0x105,
    ATTACH = 0x106,
    GET_CAPSET_INFO = 0x108,
    GET_CAPSET = 0x109,
    CTX_CREATE = 0x200,
    CTX_ATTACH = 0x202,
    CREATE_3D = 0x204,
    TRANSFER_3D = 0x205,
    READBACK_3D = 0x206,
    SUBMIT_3D = 0x207,
    UNREF = 0x102,
    DETACH = 0x107,
    CTX_DESTROY = 0x201,
    CTX_DETACH = 0x203,
    OK_NODATA = 0x1100,
    OK_DISPLAY_INFO = 0x1101,
    OK_CAPSET_INFO = 0x1102,
    OK_CAPSET = 0x1103,
    ERR_UNSPEC = 0x1200,
    ERR_OUT_OF_MEMORY = 0x1201,
    ERR_LAST = 0x1205,
    CLIENT_WORDS = USER_GPU_STREAM_MAX / 4,
    PAGE_TOP = 48,
    PAGE_HEIGHT = HEIGHT - PAGE_TOP,
    CACHE_COUNT = 4,
    SCRATCH = 3 + CACHE_COUNT,
    /* A composition: a scroll (2 blits), the page's patches, header, page
     * and the rows below a shortened view (prepare_composition). */
    COPY_WORDS = 22,
    MAX_WORDS = (USER_FRAME_RECTS + 5) * COPY_WORDS
};

_Static_assert(WEB_WIDTH == WIDTH && WEB_TOP == PAGE_TOP && WEB_HEIGHT == PAGE_HEIGHT,
               "GPU page cache must match the shell viewport");

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
};

struct header {
    uint32_t type, flags;
    uint64_t fence;
    uint32_t context;
    uint8_t ring, pad[3];
};

struct rect {
    uint32_t x, y, width, height;
};

struct rect_list {
    unsigned count;
    struct rect rects[USER_FRAME_RECTS + 1];
};

union request {
    struct header header;

    struct {
        struct header h;
        uint32_t id, format, width, height;
    } create;

    struct {
        struct header h;
        uint32_t id, entries;
        uint64_t address;
        uint32_t bytes, pad;
    } attach;

    struct {
        struct header h;
        struct rect rect;
        uint32_t scanout, id;
    } scanout;

    struct {
        struct header h;
        struct rect rect;
        uint64_t offset;
        uint32_t id, pad;
    } transfer;

    struct {
        struct header h;
        struct rect rect;
        uint32_t id, pad;
    } flush;

    struct {
        struct header h;
        uint32_t index, pad;
    } cap_info;

    struct {
        struct header h;
        uint32_t id, version;
    } capset;

    struct {
        struct header h;
        uint32_t length, init;
        char name[64];
    } context;

    struct {
        struct header h;
        uint32_t id, pad;
    } resource;

    struct {
        struct header h;
        uint32_t id, target, format, bind, width, height, depth, layers, level, samples, flags, pad;
    } create3d;

    struct {
        struct header h;
        uint32_t x, y, z, width, height, depth;
        uint64_t offset;
        uint32_t id, level, stride, layer_stride;
    } transfer3d;

    struct {
        struct header h;
        uint32_t bytes, pad, words[MAX_WORDS];
    } commands;
};

struct display_info {
    struct header h;

    struct {
        struct rect rect;
        uint32_t enabled, flags;
    } modes[16];
};

struct virgl_caps {
    uint32_t version, sampler[16], render[16], other[44];
};

union reply {
    struct header h;
    struct display_info display;

    struct {
        struct header h;
        uint32_t id, version, bytes, pad;
    } cap_info;

    struct {
        struct header h;
        struct virgl_caps data;
    } caps;

    struct {
        struct header h;
        uint32_t words[1024];
    } caps2;
};

_Static_assert(sizeof(struct header) == 24, "GPU control header ABI");
_Static_assert(sizeof(struct display_info) == 408, "GPU display-info ABI");
_Static_assert(sizeof(((union request *)0)->transfer) == 56, "GPU transfer ABI");
_Static_assert(sizeof(((union request *)0)->transfer3d) == 72, "GPU transfer3D ABI");
_Static_assert(sizeof(struct virgl_caps) == 308, "virgl v1 capability ABI");
static struct queue queues[2];
static union request request __attribute__((aligned(16)));
static volatile union reply response __attribute__((aligned(16)));
static uint32_t shadow[WIDTH * HEIGHT] __attribute__((aligned(4096)));
static uint32_t backing[WIDTH * HEIGHT] __attribute__((aligned(4096)));
static uintptr_t device;
static bool online, busy, quarantined, attempted, virgl;
/* A page's GPU requests hold the queue (virtio_gpu_client_begin/end). */
static bool client_reserved;
static uint16_t completed;
static uint64_t submitted_at;
static unsigned polls, expected_type, expected_bytes, scanout, scanout_count;

static enum { IDLE, UPLOADING, COMPOSING, FLUSHING, DISPLAY_INFO } phase;
static struct rect pending, upload, frame;
static struct rect_list uploads;
static unsigned upload_index;
static unsigned view_owner = UINT32_MAX, view_height = PAGE_HEIGHT, command_words;
static uint32_t composition[MAX_WORDS];

static struct texture {
    unsigned owner;
    uint64_t used;
    struct rect_list dirty;
    struct user_pixel_scroll scroll;
    bool valid;
} textures[CACHE_COUNT];

/* Pages presenting GPU frames (USER_GPU_PRESENT): one snapshot texture per GPU
 * client in the compositor's context, host-only like the page caches. While a
 * page resource is shown it is also attached to the compositor's context; each
 * present blits its damage into the snapshot at once, and the page's cache
 * texture then copies from the snapshot instead of from staging. */
enum { FRAMES = VIRTIO_GPU_FRAMES, FRAME_FIRST = 16 };

static struct page_frame {
    unsigned owner;
    uint32_t shown;
    bool created, active;
} page_frames[FRAMES];

/* A page command stream travels in its own buffer, after the submit header. */
static struct {
    struct header h;
    uint32_t bytes, pad, words[CLIENT_WORDS];
} client_request __attribute__((aligned(16)));

static bool client_command;
static uint32_t client_reply, caps_v1[77], caps_v2[1024];
static unsigned caps_v2_bytes;
/* The first failure; submissions also age the texture cache. */
static const char *error;
static uint64_t submitted;

static void reg(unsigned offset, uint32_t value) {
    barrier();
    mmio_write32(device + offset, value);
    barrier();
}

static void address(unsigned offset, const void *p) {
    uint64_t value = (uintptr_t)p;
    reg(offset, (uint32_t)value);
    reg(offset + 4, (uint32_t)(value >> 32));
}

bool virtio_gpu_active(void) { return online; }

static bool virtio_gpu_accelerated(void) { return online && virgl; }

static bool reset(void) {
    reg(0x70, 0);
    uint64_t start = time_us();
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        if (!mmio_read32(device + 0x70)) return true;
        if (time_us() - start >= TIMEOUT_US) break;
    }
    quarantined = true;
    return false;
}

/* A failed reset quarantines all static DMA storage until reboot. */
static bool virtio_gpu_stop(void) {
    online = false;
    busy = false;
    phase = IDLE;
    pending = (struct rect){0};
    memset(page_frames, 0, sizeof(page_frames));
    return !device || reset();
}

static bool fail(const char *reason) {
    if (!error) error = reason;
    (void)virtio_gpu_stop();
    return false;
}

static bool queue_init(unsigned index) {
    struct queue *q = &queues[index];
    reg(0x30, index);
    if (mmio_read32(device + 0x44) || mmio_read32(device + 0x34) < QUEUE_SIZE)
        return fail("invalid queue configuration");
    memset(q, 0, sizeof(*q));
    q->avail.flags = 1;
    reg(0x38, QUEUE_SIZE);
    address(0x80, q->desc);
    address(0x90, (const void *)&q->avail);
    address(0xa0, (const void *)&q->used);
    reg(0x44, 1);
    return true;
}

static bool submit_from(void *buffer, unsigned type, unsigned bytes, unsigned reply_type,
                        unsigned reply_bytes, unsigned context, bool client) {
    if (busy || !device || quarantined) return fail("invalid submission state");
    if (mmio_read32(device + 0x70) != 15) return fail("device needs reset");
    client_command = client;
    /* No command is fenced. QEMU processes each command before answering it,
     * fence or not: uploads have left guest memory, readbacks hold finished
     * rendering and a virgl context runs its commands in order. A fence would
     * also wait for the host GPU, which QEMU polls only every 10 ms. */
    *(struct header *)buffer = (struct header){.type = type, .context = context};
    memset((void *)&response, 0, sizeof(response));
    struct queue *q = &queues[0];
    q->desc[0] = (struct descriptor){(uintptr_t)buffer, bytes, 1, 1};
    q->desc[1] = (struct descriptor){(uintptr_t)&response, reply_bytes, 2, 0};
    expected_type = reply_type;
    expected_bytes = reply_bytes;
    submitted_at = time_us();
    polls = 0;
    busy = true;
    q->avail.ring[q->avail.index % QUEUE_SIZE] = 0;
    barrier();
    ++q->avail.index;
    barrier();
    reg(0x50, 0);
    ++submitted;
    return true;
}

static bool submit(unsigned type, unsigned bytes, unsigned reply_type, unsigned reply_bytes) {
    unsigned context = (type == CTX_CREATE || type == CTX_ATTACH || type == TRANSFER_3D ||
                        type == READBACK_3D || type == SUBMIT_3D)
                           ? 1
                           : 0;
    return submit_from(&request, type, bytes, reply_type, reply_bytes, context, false);
}

/* 0: pending, 1: validated completion, -1: failure. Never spin at runtime. */
static int complete(void) {
    struct queue *q = &queues[0];
    uint16_t end = q->used.index;
    barrier();
    if (mmio_read32(device + 0x70) != 15) {
        fail("device needs reset");
        return -1;
    }
    if (end == completed) {
        if (++polls >= POLL_LIMIT || time_us() - submitted_at >= TIMEOUT_US) {
            fail("command timeout");
            return -1;
        }
        return 0;
    }
    /* A page's invalid request is answered with an error reply; that fails the
     * request, not the device. The compositor's own commands must succeed. */
    bool replied = response.h.type == expected_type ||
                   (client_command && response.h.type >= ERR_UNSPEC && response.h.type <= ERR_LAST);
    if ((uint16_t)(end - completed) != 1 || q->used.ring[completed % QUEUE_SIZE].id ||
        q->used.ring[completed % QUEUE_SIZE].length != expected_bytes || !replied ||
        response.h.flags || response.h.fence || response.h.context || response.h.ring) {
        fail("invalid command completion");
        return -1;
    }
    client_reply = response.h.type;
    ++completed;
    busy = false;
    reg(0x64, mmio_read32(device + 0x60) & 3);
    return 1;
}

static bool synchronous(unsigned type, unsigned bytes, unsigned reply_type, unsigned reply_bytes) {
    if (!submit(type, bytes, reply_type, reply_bytes)) return false;
    int result;
    do {
        result = complete();
    } while (!result);
    return result > 0;
}

#define COMMAND(type, member) \
    synchronous(type, sizeof(request.member), OK_NODATA, sizeof(struct header))

static void merge(struct rect *r, struct rect add) {
    if (!add.width || !add.height) return;
    if (!r->width) {
        *r = add;
        return;
    }
    unsigned right = r->x + r->width, bottom = r->y + r->height;
    if (add.x + add.width > right) right = add.x + add.width;
    if (add.y + add.height > bottom) bottom = add.y + add.height;
    if (add.x < r->x) r->x = add.x;
    if (add.y < r->y) r->y = add.y;
    r->width = right - r->x;
    r->height = bottom - r->y;
}

static void add_rect(struct rect_list *list, struct rect r, unsigned limit, unsigned slack) {
    if (!r.width || !r.height) return;
    for (unsigned i = 0; i < list->count;) {
        struct rect combined = list->rects[i];
        merge(&combined, r);
        if ((uint64_t)combined.width * combined.height <=
            (uint64_t)r.width * r.height + (uint64_t)list->rects[i].width * list->rects[i].height +
                slack) {
            r = combined;
            list->rects[i] = list->rects[--list->count];
            i = 0;
        } else ++i;
    }
    if (list->count == limit) {
        for (unsigned i = 0; i < list->count; ++i) merge(&r, list->rects[i]);
        list->count = 0;
    }
    list->rects[list->count++] = r;
}

/* Virgl's fixed BLIT encoding (opcode 16, 21 payload words).
 * https://gitlab.freedesktop.org/virgl/virglrenderer/-/blob/main/src/virgl_protocol.h
 * Only bounded, same-format, level-zero nearest blits are submitted, clipped
 * to their destination. Every texture uses ordinary GL coordinates (flags=0):
 * CPU row zero maps to texture row zero, and scanout supplies the display flip.
 * Mixing top-origin transfers with raw texture coordinates loses partial rows.
 * No shader, object, command or resource identifier comes from a web process. */
/* A scissored nearest-filter blit (VIRGL_CCMD_BLIT) of r to (dx,dy). */
static void blit_words(uint32_t *words, unsigned dst, unsigned dx, unsigned dy, unsigned src,
                       unsigned src_format, struct rect r) {
    uint32_t blit[COPY_WORDS] = {16u | (21u << 16),
                                 0xfu | (1u << 10),
                                 dx | (dy << 16),
                                 (dx + r.width) | ((dy + r.height) << 16),
                                 dst,
                                 0,
                                 2,
                                 dx,
                                 dy,
                                 0,
                                 r.width,
                                 r.height,
                                 1,
                                 src,
                                 0,
                                 src_format,
                                 r.x,
                                 r.y,
                                 0,
                                 r.width,
                                 r.height,
                                 1};
    memcpy(words, blit, sizeof(blit));
}

static void copy_texture(unsigned dst, unsigned dx, unsigned dy, unsigned src, struct rect r) {
    blit_words(composition + command_words, dst, dx, dy, src, 2, r);
    command_words += COPY_WORDS;
}

static bool compose(bool wait) {
    request.commands.bytes = command_words * 4;
    request.commands.pad = 0;
    memcpy(request.commands.words, composition, request.commands.bytes);
    unsigned bytes = sizeof(struct header) + 8 + request.commands.bytes;
    return wait ? synchronous(SUBMIT_3D, bytes, OK_NODATA, sizeof(struct header))
                : submit(SUBMIT_3D, bytes, OK_NODATA, sizeof(struct header));
}

static bool init_virgl(void) {
    unsigned count = mmio_read32(device + 0x10c), caps2_bytes = 0;
    bool found = false;
    if (!count || count > 16) return fail("invalid capability count");
    for (unsigned index = 0; index < count; ++index) {
        request.cap_info.index = index;
        request.cap_info.pad = 0;
        if (!synchronous(GET_CAPSET_INFO, sizeof(request.cap_info), OK_CAPSET_INFO,
                         sizeof(response.cap_info)))
            return false;
        if (response.cap_info.id == 1 && response.cap_info.version >= 1 &&
            response.cap_info.bytes == 308)
            found = true;
        if (response.cap_info.id == 2 && response.cap_info.version >= 1 &&
            response.cap_info.bytes >= 396 &&
            response.cap_info.bytes <= sizeof(response.caps2.words))
            caps2_bytes = response.cap_info.bytes;
    }
    if (!found) return fail("virgl v1 unavailable");
    request.capset.id = 1;
    request.capset.version = 1;
    if (!synchronous(GET_CAPSET, sizeof(request.capset), OK_CAPSET,
                     sizeof(struct header) + sizeof(struct virgl_caps)))
        return false;
    if (response.caps.data.version < 1 || !(response.caps.data.sampler[0] & 4) ||
        !(response.caps.data.render[0] & 4))
        return fail("virgl pixel format unavailable");
    memcpy(caps_v1, (const void *)&response.caps.data, sizeof(caps_v1));
    bool bgra = (response.caps.data.render[0] & 2) != 0;
    if (caps2_bytes && bgra) {
        request.capset.id = 2;
        request.capset.version = 1;
        if (!synchronous(GET_CAPSET, sizeof(request.capset), OK_CAPSET,
                         sizeof(struct header) + caps2_bytes))
            return false;
        memcpy(caps_v2, (const void *)response.caps2.words, caps2_bytes);
        caps_v2_bytes = caps2_bytes;
    }
    memset(&request.context, 0, sizeof(request.context));
    request.context.length = 15;
    memcpy(request.context.name, "sos2 compositor", 15);
    if (!COMMAND(CTX_CREATE, context)) return false;
    /* Fixed budget: staging + scanout + four pages + one scroll scratch texture.
     * Only staging has guest backing; textures stay on the host between frames.
     * Preallocation keeps process teardown independent of device DMA lifetimes. */
    for (unsigned id = 1; id <= SCRATCH; ++id) {
        memset(&request.create3d, 0, sizeof(request.create3d));
        request.create3d.id = id;
        request.create3d.target = 2;
        request.create3d.format = 2;
        request.create3d.bind = (1u << 1) | (1u << 3) | (id == 2 ? 1u << 18 : 0);
        request.create3d.width = WIDTH;
        request.create3d.height = HEIGHT;
        request.create3d.depth = request.create3d.layers = 1;
        if (!COMMAND(CREATE_3D, create3d)) return false;
        request.resource.id = id;
        request.resource.pad = 0;
        if (!COMMAND(CTX_ATTACH, resource)) return false;
    }
    for (unsigned i = 0; i < CACHE_COUNT; ++i) textures[i].owner = UINT32_MAX;
    return true;
}

static bool transfer3d(bool wait) {
    memset(&request.transfer3d, 0, sizeof(request.transfer3d));
    request.transfer3d.x = upload.x;
    request.transfer3d.y = upload.y;
    request.transfer3d.width = upload.width;
    request.transfer3d.height = upload.height;
    request.transfer3d.depth = 1;
    request.transfer3d.offset = ((uint64_t)upload.y * WIDTH + upload.x) * 4;
    request.transfer3d.id = 1;
    request.transfer3d.stride = WIDTH * 4;
    request.transfer3d.layer_stride = WIDTH * HEIGHT * 4;
    return wait ? COMMAND(TRANSFER_3D, transfer3d)
                : submit(TRANSFER_3D, sizeof(request.transfer3d), OK_NODATA, sizeof(struct header));
}

bool virtio_gpu_init(const struct platform_devices *devices, struct framebuffer *out) {
    /* One attempt per boot. Never reassign DMA storage after an uncertain reset. */
    if (!devices || !out || attempted) return false;
    attempted = true;
    for (size_t i = 0; i < devices->virtio_count; ++i) {
        uintptr_t p = devices->virtio[i];
        if (mmio_read32(p) == 0x74726976 && mmio_read32(p + 8) == 16) {
            if (device) return fail("multiple GPU devices");
            device = p;
        }
    }
    if (!device) return false;
    if (mmio_read32(device + 4) != 2) return fail("modern MMIO required");
    if (!reset()) return fail("reset was not acknowledged");
    reg(0x70, 3);
    reg(0x14, 1);
    if (!(mmio_read32(device + 0x10) & 1)) return fail("modern feature missing");
    reg(0x14, 0);
    virgl = (mmio_read32(device + 0x10) & 1) != 0;
    reg(0x24, 0);
    reg(0x20, virgl ? 1 : 0);
    reg(0x24, 1);
    reg(0x20, 1);
    reg(0x70, 11);
    if (mmio_read32(device + 0x70) != 11) return fail("features rejected");
    if (!queue_init(0) || !queue_init(1)) return false;
    bool stable = false;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned generation = mmio_read32(device + 0xfc);
        scanout_count = mmio_read32(device + 0x108);
        if (generation == mmio_read32(device + 0xfc)) {
            stable = true;
            break;
        }
    }
    if (!stable || !scanout_count || scanout_count > 16) return fail("invalid scanout count");
    reg(0x70, 15);
    if (!synchronous(GET_DISPLAY_INFO, sizeof(struct header), OK_DISPLAY_INFO,
                     sizeof(response.display)))
        return false;
    for (scanout = 0; scanout < scanout_count; ++scanout)
        if (response.display.modes[scanout].enabled) break;
    if (scanout == scanout_count) return fail("no connected display");
    if (virgl) {
        if (!init_virgl()) return false;
    } else {
        request.create.id = 1;
        request.create.format = 2; /* B8G8R8X8_UNORM */
        request.create.width = WIDTH;
        request.create.height = HEIGHT;
        if (!COMMAND(CREATE_2D, create)) return false;
    }
    request.attach.id = 1;
    request.attach.entries = 1;
    request.attach.address = (uintptr_t)backing;
    request.attach.bytes = sizeof(backing);
    request.attach.pad = 0;
    if (!COMMAND(ATTACH, attach)) return false;
    if (virgl) {
        upload = (struct rect){0, 0, WIDTH, HEIGHT};
        if (!transfer3d(true)) return false;
        copy_texture(2, 0, 0, 1, upload);
        if (!compose(true)) return false;
    } else {
        request.transfer.rect = (struct rect){0, 0, WIDTH, HEIGHT};
        request.transfer.offset = 0;
        request.transfer.id = 1;
        request.transfer.pad = 0;
        if (!COMMAND(TRANSFER, transfer)) return false;
    }
    request.scanout.rect = (struct rect){0, 0, WIDTH, HEIGHT};
    request.scanout.scanout = scanout;
    request.scanout.id = virgl ? 2 : 1;
    if (!COMMAND(SET_SCANOUT, scanout)) return false;
    request.flush.rect = (struct rect){0, 0, WIDTH, HEIGHT};
    request.flush.id = virgl ? 2 : 1;
    request.flush.pad = 0;
    if (!COMMAND(FLUSH, flush)) return false;
    *out = (struct framebuffer){shadow, WIDTH, HEIGHT, WIDTH * 4, PIXEL_XRGB8888};
    online = true;
    phase = IDLE;
    pending = (struct rect){0, 0, WIDTH, HEIGHT};
    memset(page_frames, 0, sizeof(page_frames));
    return true;
}

void virtio_gpu_damage(unsigned x, unsigned y, unsigned width, unsigned height) {
    if (!online || !width || !height || x >= WIDTH || y >= HEIGHT) return;
    if (width > WIDTH - x) width = WIDTH - x;
    if (height > HEIGHT - y) height = HEIGHT - y;
    merge(&pending, (struct rect){x, y, width, height});
}

void virtio_gpu_page_view(unsigned owner, unsigned height) {
    if (!virtio_gpu_accelerated()) return;
    if (height > PAGE_HEIGHT) height = PAGE_HEIGHT;
    /* The caller has composed the complete new view before the next poll. */
    if (owner != view_owner || height != view_height) pending = (struct rect){0, 0, WIDTH, HEIGHT};
    view_owner = owner;
    view_height = height;
}

void virtio_gpu_page_changed(unsigned owner, unsigned x, unsigned y, unsigned width,
                             unsigned height) {
    if (!virtio_gpu_accelerated() || owner == UINT32_MAX || !width || !height || x >= WIDTH ||
        y >= PAGE_HEIGHT || width > WIDTH - x || height > PAGE_HEIGHT - y)
        return;
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (textures[i].owner == owner)
            /* A few unchanged pixels cost less than another transfer and blit.
             * Bound each merge to at most 4 KiB extra; separated strips stay
             * separate. */
            add_rect(&textures[i].dirty, (struct rect){x, y, width, height}, USER_FRAME_RECTS,
                     1024);
}

void virtio_gpu_page_scroll(unsigned owner, const struct user_pixel_scroll *scroll) {
    if (!virtio_gpu_accelerated() || owner == UINT32_MAX || !scroll || !pixel_scroll_valid(scroll))
        return;
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (textures[i].owner == owner) {
            struct texture *t = &textures[i];
            if (t->valid && !t->dirty.count && !t->scroll.rect.width) t->scroll = *scroll;
            else {
                /* More than one queued scroll, or patches preceding a scroll:
                 * snapshot the final CPU image instead of reordering operations. */
                t->scroll = (struct user_pixel_scroll){0};
                t->dirty = (struct rect_list){.count = 1, .rects = {{0, 0, WIDTH, PAGE_HEIGHT}}};
            }
        }
}

void virtio_gpu_page_closed(unsigned owner) {
    if (!virtio_gpu_accelerated() || owner == UINT32_MAX) return;
    for (unsigned i = 0; i < CACHE_COUNT; ++i)
        if (textures[i].owner == owner) textures[i] = (struct texture){.owner = UINT32_MAX};
    virtio_gpu_frame_inactive(owner);
    if (view_owner == owner) virtio_gpu_page_view(UINT32_MAX, PAGE_HEIGHT);
}

static void prepare_composition(void) {
    command_words = 0;
    uploads = (struct rect_list){0};
    if (view_owner == UINT32_MAX) {
        add_rect(&uploads, frame, USER_FRAME_RECTS + 1, 0);
        copy_texture(2, frame.x, frame.y, 1, frame);
        return;
    }
    unsigned index = 0;
    for (; index < CACHE_COUNT; ++index)
        if (textures[index].owner == view_owner) break;
    if (index == CACHE_COUNT) {
        index = 0;
        for (unsigned i = 0; i < CACHE_COUNT; ++i) {
            if (textures[i].owner == UINT32_MAX) {
                index = i;
                break;
            }
            if (textures[i].used < textures[index].used) index = i;
        }
        textures[index] = (struct texture){.owner = view_owner};
    }
    struct texture *t = &textures[index];
    t->used = submitted;
    /* A page showing GPU frames copies from its snapshot, in page coordinates. */
    unsigned source = 1;
    for (unsigned i = 0; i < FRAMES; ++i)
        if (page_frames[i].active && page_frames[i].owner == view_owner) source = FRAME_FIRST + i;
    struct rect_list changed = t->dirty;
    if (!t->valid) changed = (struct rect_list){.count = 1, .rects = {{0, 0, WIDTH, PAGE_HEIGHT}}};
    /* Only header pixels and changed page pixels cross the guest/host boundary.
     * A cached page selection copies the existing texture straight to scanout.
     * Runtime ownership changes occur between frames, after the previous flush.
     * Close can discard metadata immediately: no page memory is in any queue. */
    if (t->valid && t->scroll.rect.width) {
        struct user_pixel_rect src, dst;
        pixel_scroll_boxes(&t->scroll, &src, &dst);
        struct rect source = {src.x, src.y, src.width, src.height};
        /* Never blit overlapping regions of the same GL texture. The scratch
         * is private and reused only after the preceding composition. */
        copy_texture(SCRATCH, src.x, src.y, index + 3, source);
        copy_texture(index + 3, dst.x, dst.y, SCRATCH, source);
    }
    for (unsigned i = 0; i < changed.count; ++i) {
        struct rect r = changed.rects[i], from = r;
        if (source == 1) {
            from.y += PAGE_TOP;
            add_rect(&uploads, from, USER_FRAME_RECTS + 1, 0);
        }
        copy_texture(index + 3, r.x, r.y, source, from);
    }
    t->valid = true;
    t->dirty = (struct rect_list){0};
    t->scroll = (struct user_pixel_scroll){0};
    if (frame.y < PAGE_TOP) {
        struct rect header = frame;
        if (header.height > PAGE_TOP - header.y) header.height = PAGE_TOP - header.y;
        add_rect(&uploads, header, USER_FRAME_RECTS + 1, 0);
        copy_texture(2, header.x, header.y, 1, header);
    }
    /* The view's rows of the page come from its cache. Below a shortened view
     * (the soft keyboard over a GPU-frame page) staging shows, as above. */
    unsigned top = frame.y > PAGE_TOP ? frame.y : PAGE_TOP, bottom = frame.y + frame.height,
             split = PAGE_TOP + view_height;
    if (top < split && top < bottom) {
        unsigned end = bottom < split ? bottom : split;
        copy_texture(2, frame.x, top, index + 3,
                     (struct rect){frame.x, top - PAGE_TOP, frame.width, end - top});
    }
    if (bottom > split) {
        struct rect below = {frame.x, top > split ? top : split, frame.width, 0};
        below.height = bottom - below.y;
        add_rect(&uploads, below, USER_FRAME_RECTS + 1, 0);
        copy_texture(2, below.x, below.y, 1, below);
    }
}

void virtio_gpu_poll(void) {
    if (!online || (client_reserved && phase == IDLE)) return;
    if (busy) {
        if (complete() <= 0) return;
        if (phase == UPLOADING) {
            if (virgl) {
                if (++upload_index < uploads.count) {
                    upload = uploads.rects[upload_index];
                    (void)transfer3d(false);
                    return;
                }
                phase = COMPOSING;
                (void)compose(false);
                return;
            }
        }
        if (phase == UPLOADING || phase == COMPOSING) {
            request.flush.rect = frame;
            request.flush.id = virgl ? 2 : 1;
            request.flush.pad = 0;
            phase = FLUSHING;
            (void)submit(FLUSH, sizeof(request.flush), OK_NODATA, sizeof(struct header));
            return;
        }
        if (phase == DISPLAY_INFO) {
            if (!response.display.modes[scanout].enabled) {
                fail("display disconnected");
                return;
            }
            pending = (struct rect){0, 0, WIDTH, HEIGHT};
        }
        phase = IDLE;
    }
    if (client_reserved) return;
    if (mmio_read32(device + 0x70) != 15) {
        fail("device needs reset");
        return;
    }
    if (mmio_read32(device + 0x100) & 1) {
        reg(0x104, 1);
        phase = DISPLAY_INFO;
        (void)submit(GET_DISPLAY_INFO, sizeof(struct header), OK_DISPLAY_INFO,
                     sizeof(response.display));
        return;
    }
    if (!pending.width) return;
    frame = upload = pending;
    pending = (struct rect){0};
    uploads = (struct rect_list){.count = 1, .rects = {upload}};
    upload_index = 0;
    if (virgl) prepare_composition();
    if (!uploads.count) {
        phase = COMPOSING;
        (void)compose(false);
        return;
    }
    /* Snapshot only after the preceding flush completed. New paints accumulate
     * in shadow/pending while the host owns backing; no renderer memory is pinned. */
    for (unsigned i = 0; i < uploads.count; ++i) {
        struct rect r = uploads.rects[i];
        for (unsigned y = r.y; y < r.y + r.height; ++y)
            memcpy(backing + y * WIDTH + r.x, shadow + y * WIDTH + r.x, r.width * 4);
    }
    upload = uploads.rects[0];
    phase = UPLOADING;
    if (virgl) {
        (void)transfer3d(false);
        return;
    }
    request.transfer.rect = upload;
    request.transfer.offset = ((uint64_t)upload.y * WIDTH + upload.x) * 4;
    request.transfer.id = 1;
    request.transfer.pad = 0;
    (void)submit(TRANSFER, sizeof(request.transfer), OK_NODATA, sizeof(struct header));
}

bool virtio_gpu_sync(void) {
    /* Terminal drain only (panic): a worker parked in a page's GPU request
     * never resumes, so drop its reservation and let poll complete the
     * outstanding command. Standby must not use it (display_blank). */
    client_reserved = false;
    for (unsigned i = 0; i < POLL_LIMIT && online; ++i) {
        virtio_gpu_poll();
        if (online && !busy && !pending.width) return true;
    }
    return online ? fail("display drain timeout") : false;
}

/* Page GPU clients. Called only from the service worker. A reservation waits
 * for the compositor's command in flight, then pauses compositing; every
 * client command then completes before the next is sent, yielding while the
 * host works. Resource ids and contexts are chosen and validated by the kernel
 * (kernel/protection/gpu_client.c); only command streams come from pages. */
bool virtio_gpu_client_available(void) { return online && virgl; }

bool virtio_gpu_client_quarantined(void) { return quarantined; }

const void *virtio_gpu_client_caps(unsigned id, unsigned *bytes) {
    if (id == 1 && virgl) {
        *bytes = sizeof(caps_v1);
        return caps_v1;
    }
    if (id == 2 && caps_v2_bytes) {
        *bytes = caps_v2_bytes;
        return caps_v2;
    }
    *bytes = 0;
    return 0;
}

int virtio_gpu_client_begin(void) {
    if (!virtio_gpu_client_available() || client_reserved) return USER_ENODEV;
    client_reserved = true;
    while (online && phase != IDLE) {
        virtio_gpu_poll();
        if (phase != IDLE) kernel_io_wait();
    }
    if (!online) {
        client_reserved = false;
        return USER_ENODEV;
    }
    return 0;
}

void virtio_gpu_client_end(void) { client_reserved = false; }

static int client_run(void *buffer, unsigned type, unsigned bytes, unsigned context) {
    if (!online) return USER_ENODEV;
    if (!submit_from(buffer, type, bytes, OK_NODATA, sizeof(struct header), context, true))
        return USER_EIO;
    int status;
    while (!(status = complete())) kernel_io_wait();
    client_command = false;
    if (status < 0) return USER_EIO;
    return client_reply == OK_NODATA           ? 0
           : client_reply == ERR_OUT_OF_MEMORY ? USER_ENOMEM
                                               : USER_EINVAL;
}

/* A command naming one resource: (CTX_)ATTACH/DETACH or UNREF, in context
 * (1 is the compositor's, 0 none). */
static int resource_run(unsigned type, uint32_t id, unsigned context) {
    request.resource.id = id;
    request.resource.pad = 0;
    return client_run(&request, type, sizeof(request.resource), context);
}

int virtio_gpu_client_context(unsigned context, bool create) {
    if (!create) return client_run(&request, CTX_DESTROY, sizeof(struct header), context);
    memset(&request.context, 0, sizeof(request.context));
    request.context.length = 9;
    memcpy(request.context.name, "sos2 page", 9);
    return client_run(&request, CTX_CREATE, sizeof(request.context), context);
}

int virtio_gpu_client_create(unsigned context, unsigned id, const struct user_gpu_resource *r,
                             const void *backing, uint32_t bytes) {
    memset(&request.create3d, 0, sizeof(request.create3d));
    request.create3d.id = id;
    request.create3d.target = r->target;
    request.create3d.format = r->format;
    request.create3d.bind = r->bind;
    request.create3d.width = r->width;
    request.create3d.height = r->height;
    request.create3d.depth = r->depth;
    request.create3d.layers = r->array_size;
    request.create3d.level = r->last_level;
    request.create3d.samples = r->nr_samples;
    request.create3d.flags = r->flags;
    int result = client_run(&request, CREATE_3D, sizeof(request.create3d), 0);
    if (result) return result;
    request.attach.id = id;
    request.attach.entries = 1;
    request.attach.address = (uintptr_t)backing;
    request.attach.bytes = bytes;
    request.attach.pad = 0;
    if (!(result = client_run(&request, ATTACH, sizeof(request.attach), 0)) &&
        !(result = resource_run(CTX_ATTACH, id, context)))
        return 0;
    /* Undo a partial creation; an unreferenced id has no backing left in use. */
    (void)resource_run(DETACH, id, 0);
    (void)resource_run(UNREF, id, 0);
    return result;
}

int virtio_gpu_client_destroy(unsigned context, unsigned id) {
    /* Detach from the page first, then release the backing, then the resource:
     * after the backing detach completes the host no longer reads its pages. */
    int result = resource_run(CTX_DETACH, id, context);
    int detached = resource_run(DETACH, id, 0);
    int released = resource_run(UNREF, id, 0);
    return detached ? detached : released ? released : result;
}

int virtio_gpu_client_transfer(unsigned context, unsigned id, bool to_host,
                               const struct user_gpu_transfer *t) {
    memset(&request.transfer3d, 0, sizeof(request.transfer3d));
    request.transfer3d.x = t->x;
    request.transfer3d.y = t->y;
    request.transfer3d.z = t->z;
    request.transfer3d.width = t->width;
    request.transfer3d.height = t->height;
    request.transfer3d.depth = t->depth;
    request.transfer3d.offset = t->offset;
    request.transfer3d.id = id;
    request.transfer3d.level = t->level;
    request.transfer3d.stride = t->stride;
    request.transfer3d.layer_stride = t->layer_stride;
    return client_run(&request, to_host ? TRANSFER_3D : READBACK_3D, sizeof(request.transfer3d),
                      context);
}

void *virtio_gpu_client_stream(void) { return client_request.words; }

int virtio_gpu_client_submit(unsigned context, uint32_t bytes) {
    if (!bytes || bytes > sizeof(client_request.words) || (bytes & 3)) return USER_EINVAL;
    client_request.bytes = bytes;
    client_request.pad = 0;
    return client_run(&client_request, SUBMIT_3D, sizeof(struct header) + 8 + bytes, context);
}

static int attach_compositor(uint32_t id, bool attach) {
    return resource_run(attach ? CTX_ATTACH : CTX_DETACH, id, 1);
}

int virtio_gpu_frame_present(unsigned client, unsigned owner, unsigned context, uint32_t resource,
                             unsigned format, const struct user_pixel_rect *rects, unsigned count) {
    if (client >= FRAMES || !virgl || !count || count > USER_FRAME_RECTS) return USER_EINVAL;
    struct page_frame *f = &page_frames[client];
    uint32_t id = FRAME_FIRST + client;
    /* A snapshot starts, or changes pages, with a frame covering all of it. */
    bool whole = count == 1 && !rects[0].x && !rects[0].y && rects[0].width == WIDTH &&
                 rects[0].height == PAGE_HEIGHT;
    if ((!f->active || f->owner != owner) && !whole) return USER_EINVAL;
    int result;
    if (!f->created) {
        memset(&request.create3d, 0, sizeof(request.create3d));
        request.create3d.id = id;
        request.create3d.target = 2;
        request.create3d.format = 2;
        request.create3d.bind = (1u << 1) | (1u << 3);
        request.create3d.width = WIDTH;
        request.create3d.height = PAGE_HEIGHT;
        request.create3d.depth = request.create3d.layers = 1;
        if ((result = client_run(&request, CREATE_3D, sizeof(request.create3d), 0))) return result;
        if ((result = attach_compositor(id, true))) {
            (void)resource_run(UNREF, id, 0);
            return result;
        }
        f->created = true;
    }
    if (f->shown != resource) {
        if (f->shown && (result = attach_compositor(f->shown, false))) return result;
        f->shown = 0;
        if ((result = attach_compositor(resource, true))) return result;
        f->shown = resource;
    }
    /* The host runs each context on its own GPU queue, and pages' commands
     * are unfenced. Reading one pixel in the page's context returns only once
     * its rendering finished, so the blit never copies a partial frame. */
    struct user_gpu_transfer drain = {
        .width = 1, .height = 1, .depth = 1, .stride = 4, .layer_stride = 4};
    if ((result = virtio_gpu_client_transfer(context, resource, false, &drain))) return result;
    for (unsigned i = 0; i < count; ++i) {
        struct rect r = {rects[i].x, rects[i].y, rects[i].width, rects[i].height};
        blit_words(client_request.words + i * COPY_WORDS, id, r.x, r.y, resource, format, r);
    }
    if ((result = virtio_gpu_client_submit(1, count * COPY_WORDS * 4))) return result;
    f->owner = owner;
    f->active = true;
    return 0;
}

int virtio_gpu_frame_detach(unsigned client, uint32_t resource) {
    if (client >= FRAMES || !resource || page_frames[client].shown != resource) return 0;
    page_frames[client].shown = 0;
    return attach_compositor(resource, false);
}

void virtio_gpu_frame_release(unsigned client) {
    if (client >= FRAMES) return;
    struct page_frame *f = &page_frames[client];
    if (f->shown) (void)attach_compositor(f->shown, false);
    if (f->created) {
        (void)attach_compositor(FRAME_FIRST + client, false);
        (void)resource_run(UNREF, FRAME_FIRST + client, 0);
    }
    *f = (struct page_frame){0};
}

void virtio_gpu_frame_inactive(unsigned owner) {
    for (unsigned i = 0; i < FRAMES; ++i)
        if (page_frames[i].owner == owner) page_frames[i].active = false;
}
