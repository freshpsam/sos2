#include "gpu_client.h"
#include "page_host.h"
#include "page_alloc.h"
#include "platform.h"
#ifndef SOS2_BOARD_REDMI2
#include "virtio_gpu.h"
#endif

/* SYS_GPU: a page's own virgl context (QEMU only; the phone has no client GPU
 * interface yet). Runs on the serialized service worker with the caller's
 * threads stopped, so its buffers cannot change while they are copied.
 *
 * Isolation: the kernel picks context and resource ids, creates every resource,
 * zeroes its backing and attaches it only to the creator's context.
 * virglrenderer resolves resource ids in a command stream against that
 * context alone, so a stream cannot name another page's or the compositor's
 * resources. Streams are copied into kernel memory before submission and are
 * otherwise opaque; a broken stream fails only its own context.
 *
 * Budgets: 16 clients, USER_GPU_RESOURCES resources and USER_GPU_PAGE_MAX
 * backing bytes per page, 128 MiB in all, and 16 MiB of RAM always left free.
 * Backing is uncached kernel memory, freed only after the device detached it
 * or a confirmed reset; after an unconfirmed reset it stays quarantined. */
#ifdef SOS2_BOARD_REDMI2
int gpu_client_call(struct gpu_client **state, const struct vm_space *space, unsigned slot,
                    uint32_t op, uint32_t address, uint32_t size) {
    (void)state;
    (void)space;
    (void)slot;
    (void)address;
    (void)size;
    return op == USER_GPU_INFO ? 0 : USER_ENODEV;
}

void gpu_client_close(struct gpu_client **state) { *state = 0; }

unsigned gpu_client_quarantined_pages(void) { return 0; }
#else
enum {
    CLIENTS = 16,
    FIRST_CONTEXT = 2,
    FIRST_RESOURCE = 1024,
    MAX_DIMENSION = 4096,
    MAX_LEVELS = 13,
    TOTAL_BYTES = 128u * 1024u * 1024u,
    RESERVE_PAGES = 4096
};

/* Gallium targets and the virgl bind flags pages may use (virgl_hw.h). */
enum {
    TARGET_BUFFER = 0,
    TARGET_2D = 2,
    TARGET_RECT = 5,
    FORMAT_R8 = 64,
    FLAG_Y_0_TOP = 1,
    BINDS = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8) |
            (1u << 11) | (1u << 14) | (1u << 15) | (1u << 19) | (1u << 22)
};

struct gpu_resource {
    uint32_t id, target, format, bind, width, height, last_level, bytes;
    void *backing;
};

struct gpu_client {
    bool used, context;
    unsigned count;
    uint32_t bytes;
    struct gpu_resource resources[USER_GPU_RESOURCES];
};
static struct gpu_client clients[CLIENTS];
static uint32_t next_id = FIRST_RESOURCE;
static uint64_t total_bytes;
/* Backing kept after an unconfirmed device reset: the host may still own it. */
static unsigned quarantined_pages;

/* Bytes per pixel of the formats pages may create (virgl_hw.h). */
static unsigned format_bytes(uint32_t format) {
    switch (format) {
#define FORMAT(id, bytes) \
    case id: return bytes;
        USER_GPU_FORMATS(FORMAT)
#undef FORMAT
        default: return 0;
    }
}

static uint32_t level_size(uint32_t size, uint32_t level) {
    return size >> level ? size >> level : 1;
}

static struct gpu_resource *find(struct gpu_client *client, uint32_t id) {
    for (unsigned i = 0; i < client->count; ++i)
        if (client->resources[i].id == id) return &client->resources[i];
    return 0;
}

static unsigned index_of(const struct gpu_client *client) { return (unsigned)(client - clients); }

static unsigned context_of(const struct gpu_client *client) {
    return FIRST_CONTEXT + index_of(client);
}

_Static_assert(CLIENTS <= VIRTIO_GPU_FRAMES, "a GPU frame snapshot per client");

static void free_backing(void *backing, unsigned pages) {
    if (virtio_gpu_client_quarantined()) quarantined_pages += pages;
    else (void)pages_free(backing, pages);
}

unsigned gpu_client_quarantined_pages(void) { return quarantined_pages; }

static void release_backing(struct gpu_client *client, struct gpu_resource *r) {
    free_backing(r->backing, (r->bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    client->bytes -= r->bytes;
    total_bytes -= r->bytes;
    *r = client->resources[--client->count];
}

static int create(struct gpu_client *client, const struct vm_space *space, uint32_t address,
                  uint32_t size) {
    struct user_gpu_resource desc;
    if (size != sizeof(desc)) return USER_EINVAL;
    if (!vm_copy_from_user(space, &desc, address, sizeof(desc))) return USER_EFAULT;
    unsigned bytes_per_pixel = format_bytes(desc.format);
    if (!bytes_per_pixel || (desc.bind & ~BINDS) || (desc.flags & ~FLAG_Y_0_TOP) ||
        desc.depth != 1 || desc.array_size != 1 || desc.nr_samples > 1)
        return USER_EINVAL;
    uint64_t needed = 0;
    if (desc.target == TARGET_BUFFER) {
        if (desc.format != FORMAT_R8 || !desc.width || desc.width > USER_GPU_RESOURCE_MAX ||
            desc.height != 1 || desc.last_level)
            return USER_EINVAL;
        needed = desc.width;
    } else if (desc.target == TARGET_2D || desc.target == TARGET_RECT) {
        if (!desc.width || !desc.height || desc.width > MAX_DIMENSION ||
            desc.height > MAX_DIMENSION || desc.last_level >= MAX_LEVELS ||
            (desc.target == TARGET_RECT && desc.last_level) ||
            (desc.width | desc.height) >> desc.last_level == 0)
            return USER_EINVAL;
        for (uint32_t level = 0; level <= desc.last_level; ++level)
            needed += (uint64_t)level_size(desc.width, level) * level_size(desc.height, level) *
                      bytes_per_pixel;
    } else return USER_EINVAL;
    /* Like Linux, the page chooses the backing size; it must hold every level. */
    uint64_t bytes = desc.size ? desc.size : needed;
    if (bytes < needed || bytes > USER_GPU_RESOURCE_MAX) return USER_EINVAL;
    unsigned pages = (unsigned)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    if (client->count == USER_GPU_RESOURCES || client->bytes + bytes > USER_GPU_PAGE_MAX ||
        total_bytes + bytes > TOTAL_BYTES || pages_available() < pages + RESERVE_PAGES)
        return USER_ENOMEM;
    void *backing = pages_alloc(pages, 1); /* zeroed */
    if (!backing) return USER_ENOMEM;
    uint32_t id = next_id++;
    if (next_id >= 0x40000000u) next_id = FIRST_RESOURCE;
    desc.handle = id;
    int result = virtio_gpu_client_create(context_of(client), id, &desc, backing, (uint32_t)bytes);
    if (result) {
        free_backing(backing, pages);
        return result;
    }
    client->resources[client->count++] =
        (struct gpu_resource){id,          desc.target,     desc.format,     desc.bind, desc.width,
                              desc.height, desc.last_level, (uint32_t)bytes, backing};
    client->bytes += (uint32_t)bytes;
    total_bytes += bytes;
    if (!vm_copy_to_user(space, address, &id, sizeof(id))) return USER_EFAULT;
    return (int)id;
}

/* The box must lie inside the level, and every row inside the backing. The
 * device receives the strides used for the copy, never implicit ones. */
static int transfer(struct gpu_client *client, const struct vm_space *space, uint32_t address,
                    uint32_t size, bool upload) {
    struct user_gpu_transfer t;
    if (size != sizeof(t)) return USER_EINVAL;
    if (!vm_copy_from_user(space, &t, address, sizeof(t))) return USER_EFAULT;
    struct gpu_resource *r = find(client, t.handle);
    if (!r) return USER_EBADF;
    if (t.level > r->last_level || t.z || t.depth != 1 || !t.width || !t.height) return USER_EINVAL;
    uint32_t width = r->target == TARGET_BUFFER ? r->width : level_size(r->width, t.level);
    uint32_t height = r->target == TARGET_BUFFER ? 1 : level_size(r->height, t.level);
    if (t.x >= width || t.width > width - t.x || t.y >= height || t.height > height - t.y)
        return USER_EINVAL;
    unsigned bytes_per_pixel = r->target == TARGET_BUFFER ? 1 : format_bytes(r->format);
    uint64_t row = (uint64_t)t.width * bytes_per_pixel;
    /* As in the virgl protocol, a zero stride means the level's packed rows
     * and a zero layer stride the box's rows. virglrenderer fails the whole
     * context for strides below those minimums, so they are refused here. */
    if (!t.stride) t.stride = width * bytes_per_pixel;
    uint64_t layer = (uint64_t)t.stride * t.height;
    if (!t.layer_stride && layer <= UINT32_MAX) t.layer_stride = (uint32_t)layer;
    if (t.stride < row || t.layer_stride < layer) return USER_EINVAL;
    uint64_t span = (uint64_t)(t.height - 1) * t.stride + row;
    if (t.offset > r->bytes || span > r->bytes - t.offset || t.data > UINT32_MAX - span + 1)
        return USER_EINVAL;
    if (!vm_accessible(space, t.data, (size_t)span, upload ? VM_READ : VM_WRITE))
        return USER_EFAULT;
    unsigned char *backing = (unsigned char *)r->backing + t.offset;
    if (upload)
        for (uint32_t y = 0; y < t.height; ++y)
            if (!vm_copy_from_user(space, backing + (uint64_t)y * t.stride, t.data + y * t.stride,
                                   (size_t)row))
                return USER_EFAULT;
    int result = virtio_gpu_client_transfer(context_of(client), r->id, upload, &t);
    if (!result && !upload)
        for (uint32_t y = 0; y < t.height; ++y)
            if (!vm_copy_to_user(space, t.data + y * t.stride, backing + (uint64_t)y * t.stride,
                                 (size_t)row))
                return USER_EFAULT;
    return result;
}

static int submit(struct gpu_client *client, const struct vm_space *space, uint32_t address,
                  uint32_t size) {
    struct user_gpu_submit s;
    if (size != sizeof(s)) return USER_EINVAL;
    if (!vm_copy_from_user(space, &s, address, sizeof(s))) return USER_EFAULT;
    if (!s.bytes || s.bytes > USER_GPU_STREAM_MAX || (s.bytes & 3)) return USER_EINVAL;
    if (!vm_copy_from_user(space, virtio_gpu_client_stream(), s.commands, s.bytes))
        return USER_EFAULT;
    return virtio_gpu_client_submit(context_of(client), s.bytes);
}

/* The page's frame goes to its compositor snapshot before page_host learns
 * of it, so the compositor never draws a frame it has not copied. */
static int present(struct gpu_client *client, const struct vm_space *space, unsigned slot,
                   uint32_t address, uint32_t size) {
    struct user_gpu_present g;
    if (size != sizeof(g)) return USER_EINVAL;
    if (!vm_copy_from_user(space, &g, address, sizeof(g))) return USER_EFAULT;
    struct gpu_resource *r = find(client, g.handle);
    if (!r) return USER_EBADF;

    enum { RENDER_TARGET = 1u << 1, SAMPLER_VIEW = 1u << 3 };

    if (r->target != TARGET_2D || r->width != WEB_WIDTH || r->height != WEB_HEIGHT ||
        (r->format != 1 && r->format != 2) ||
        (r->bind & (RENDER_TARGET | SAMPLER_VIEW)) != (RENDER_TARGET | SAMPLER_VIEW))
        return USER_EINVAL;
    int result = page_host_gpu_frame_check(slot, g.damage, g.count);
    if (!result)
        result = virtio_gpu_frame_present(index_of(client), slot, context_of(client), r->id,
                                          r->format, g.damage, g.count);
    if (!result) page_host_gpu_frame(slot, g.damage, g.count);
    return result;
}

static int caps(const struct vm_space *space, uint32_t address, uint32_t size) {
    struct user_gpu_caps c;
    if (size != sizeof(c)) return USER_EINVAL;
    if (!vm_copy_from_user(space, &c, address, sizeof(c))) return USER_EFAULT;
    unsigned bytes;
    const void *data = virtio_gpu_client_caps(c.capset, &bytes);
    if (!data) return USER_ENODEV;
    if (c.size < bytes) bytes = c.size;
    if (bytes && !vm_copy_to_user(space, c.buffer, data, bytes)) return USER_EFAULT;
    return (int)bytes;
}

int gpu_client_call(struct gpu_client **state, const struct vm_space *space, unsigned slot,
                    uint32_t op, uint32_t address, uint32_t size) {
    if (op == USER_GPU_INFO) return virtio_gpu_client_available() ? 1 : 0;
    if (!virtio_gpu_client_available()) return USER_ENODEV;
    if (op == USER_GPU_CAPS) return caps(space, address, size);
    if (op < USER_GPU_CREATE || op > USER_GPU_PRESENT) return USER_EINVAL;
    struct gpu_client *client = *state;
    if (!client) {
        for (unsigned i = 0; i < CLIENTS && !client; ++i)
            if (!clients[i].used) client = &clients[i];
        if (!client) return USER_EBUSY;
        *client = (struct gpu_client){.used = true};
        *state = client;
    }
    int result = virtio_gpu_client_begin();
    if (result) return result;
    if (!client->context) {
        result = virtio_gpu_client_context(context_of(client), true);
        client->context = !result;
    }
    if (!result) switch (op) {
            case USER_GPU_CREATE: result = create(client, space, address, size); break;
            case USER_GPU_DESTROY: {
                struct gpu_resource *r = find(client, address);
                if (!r || size) {
                    result = USER_EBADF;
                    break;
                }
                /* The compositor lets go of a shown resource first; the destroy runs
                 * regardless, so the device never keeps backing that is freed. */
                int detached = virtio_gpu_frame_detach(index_of(client), r->id);
                result = virtio_gpu_client_destroy(context_of(client), r->id);
                if (!result) result = detached;
                release_backing(client, r);
                break;
            }
            case USER_GPU_UPLOAD:
            case USER_GPU_DOWNLOAD:
                result = transfer(client, space, address, size, op == USER_GPU_UPLOAD);
                break;
            case USER_GPU_PRESENT: result = present(client, space, slot, address, size); break;
            default: result = submit(client, space, address, size); break;
        }
    virtio_gpu_client_end();
    return result;
}

void gpu_client_close(struct gpu_client **state) {
    struct gpu_client *client = *state;
    *state = 0;
    if (!client) return;
    bool device = !virtio_gpu_client_begin();
    if (device) virtio_gpu_frame_release(index_of(client));
    while (client->count) {
        struct gpu_resource *r = &client->resources[client->count - 1];
        if (device) (void)virtio_gpu_client_destroy(context_of(client), r->id);
        release_backing(client, r);
    }
    if (device && client->context) (void)virtio_gpu_client_context(context_of(client), false);
    if (device) virtio_gpu_client_end();
    *client = (struct gpu_client){0};
}
#endif
