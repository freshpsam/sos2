/* libdrm for SOS2: the libdrm functions Mesa uses, and a virtio-gpu render
 * node, /dev/dri/renderD128, served in user space over SYS_GPU.
 *
 * Mesa's virgl winsys speaks the Linux virtio-gpu DRM interface. Here a buffer
 * object is a SYS_GPU resource plus a shadow copy in page memory, allocated on
 * first mapping; mmap returns the shadow, and the transfer ioctls copy boxes
 * between it and the resource. The device never sees the shadow, so the
 * capabilities under which the host reads guest memory while executing
 * commands (inline and copy transfers) are hidden and Mesa always uses the
 * transfer ioctls. Formats, sample counts and binds are narrowed to what
 * SYS_GPU accepts. SYS_GPU operations complete before they return, so waits
 * never block, and there are no fence descriptors (the DRM version is 0.0).
 *
 * A description's buffer objects are released when it closes, and buffer
 * descriptors (PRIME) keep theirs alive; shadows still mapped stay valid
 * until munmap, like Linux mappings outliving the object. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sos2/device.h>
#include <sos2/services.h>
#include <xf86drm.h>
#include <virtgpu_drm.h>
#include "virgl_hw.h"
#include "drm_status.h"

#define RENDER_NODE "/dev/dri/renderD128"
#define OBJECTS     USER_GPU_RESOURCES
#define PAGE        4096u

struct file {
    int unused;
};

/* A SYS_GPU resource and its shadow. Handles and exported descriptors hold
 * references; the last to go destroys the resource. */
struct buffer {
    uint32_t resource, size, references;
    void *shadow;
    unsigned maps;
};

struct object {
    struct file *owner;
    struct buffer *buffer;
};
/* Indexed by buffer object handle; 0 is never a handle. */
static struct object objects[OBJECTS + 1];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int failed;

static int fail(int error) {
    errno = error;
    return -1;
}

static int fail_sos2(int result) {
    errno = sos2_errno(result);
    return -1;
}

/* Every SYS_GPU request after setup goes through here, so a failure is seen. */
static int request(unsigned op, uint32_t argument, uint32_t size) {
    int result = sos2_gpu(op, argument, size);
    if (result < 0) __atomic_store_n(&failed, 1, __ATOMIC_RELEASE);
    return result;
}

int sos2_drm_failed(void) { return __atomic_load_n(&failed, __ATOMIC_ACQUIRE); }

static uint32_t pages(uint32_t size) { return (size + PAGE - 1) & ~(PAGE - 1); }

static uint32_t address(const void *p) { return (uint32_t)(uintptr_t)p; }

/* Caller holds lock. */
static struct object *find(struct file *file, uint32_t handle) {
    return handle && handle <= OBJECTS && objects[handle].owner == file ? &objects[handle] : NULL;
}

static unsigned free_handle(void) {
    unsigned handle = 1;
    while (handle <= OBJECTS && objects[handle].owner) ++handle;
    return handle <= OBJECTS ? handle : 0;
}

static bool ensure_shadow(struct buffer *b) {
    if (b->shadow) return true;
    void *p =
        mmap(NULL, pages(b->size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;
    b->shadow = p;
    return true;
}

/* Caller holds lock; a buffer it returns has no references left and must be
 * destroyed once the lock is released. */
static struct buffer *unreference(struct buffer *b) { return --b->references ? NULL : b; }

static int destroy(struct buffer *b, bool counted) {
    int result = counted ? request(USER_GPU_DESTROY, b->resource, 0)
                         : sos2_gpu(USER_GPU_DESTROY, b->resource, 0);
    /* A shadow still mapped stays until munmap, like a Linux mapping. */
    if (b->shadow && !b->maps) munmap(b->shadow, pages(b->size));
    free(b);
    return result;
}

static int getparam(struct drm_virtgpu_getparam *p) {
    int value;
    switch (p->param) {
        case VIRTGPU_PARAM_3D_FEATURES:
        case VIRTGPU_PARAM_CAPSET_QUERY_FIX: value = 1; break;
        case VIRTGPU_PARAM_RESOURCE_BLOB:
        case VIRTGPU_PARAM_HOST_VISIBLE:
        case VIRTGPU_PARAM_CROSS_DEVICE:
        case VIRTGPU_PARAM_CONTEXT_INIT: value = 0; break;
        default: return fail(EINVAL);
    }
    /* Like Linux, the result is an int. */
    memcpy((void *)(uintptr_t)p->value, &value, sizeof(value));
    return 0;
}

static void narrow_formats(struct virgl_supported_format_mask *mask) {
    uint32_t allowed[16] = {0};
#define FORMAT(id, bytes) allowed[(id) / 32] |= 1u << ((id) % 32);
    USER_GPU_FORMATS(FORMAT)
#undef FORMAT
    for (unsigned i = 0; i < 16; ++i) mask->bitmask[i] &= allowed[i];
}

#define HAS(bytes, type, field) ((bytes) >= offsetof(type, field) + sizeof(((type *)0)->field))

static int get_caps(struct drm_virtgpu_get_caps *c) {
    union virgl_caps *caps = (union virgl_caps *)(uintptr_t)c->addr;
    struct user_gpu_caps query = {c->cap_set_id, address(caps), c->size};
    int bytes = sos2_gpu(USER_GPU_CAPS, address(&query), sizeof(query));
    if (bytes < 0) return fail(bytes == USER_ENODEV ? EINVAL : sos2_errno(bytes));
    unsigned n = (unsigned)bytes;
    if (HAS(n, struct virgl_caps_v1, depthstencil)) {
        narrow_formats(&caps->v1.sampler);
        narrow_formats(&caps->v1.render);
        narrow_formats(&caps->v1.depthstencil);
    }
    if (HAS(n, struct virgl_caps_v1, max_samples)) caps->v1.max_samples = 0;
    if (c->cap_set_id != 2) return 0;
    if (HAS(n, struct virgl_caps_v2, capability_bits))
        caps->v2.capability_bits &= ~(VIRGL_CAP_TRANSFER | VIRGL_CAP_COPY_TRANSFER);
    if (HAS(n, struct virgl_caps_v2, capability_bits_v2))
        caps->v2.capability_bits_v2 &= ~VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS;
    if (HAS(n, struct virgl_caps_v2, supported_multisample_formats))
        memset(&caps->v2.supported_multisample_formats, 0,
               sizeof(caps->v2.supported_multisample_formats));
    return 0;
}

static int resource_create(struct file *file, struct drm_virtgpu_resource_create *c) {
    uint32_t bind = c->bind;
    /* Guest-memory-only buffers (fences, query results) become staging
     * buffers, which the host also keeps in guest memory. Sharing and
     * display hints do not apply to a page's private resources. */
    if (bind & VIRGL_BIND_CUSTOM) bind = (bind & ~VIRGL_BIND_CUSTOM) | VIRGL_BIND_STAGING;
    bind &= ~(VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT | VIRGL_BIND_SHARED |
              VIRGL_BIND_CURSOR | VIRGL_BIND_PREFER_EMULATED_BGRA | VIRGL_BIND_SHARED_SUBFLAGS);
    struct buffer *b = calloc(1, sizeof(*b));
    if (!b) return fail(ENOMEM);
    pthread_mutex_lock(&lock);
    unsigned handle = free_handle();
    if (handle) objects[handle] = (struct object){file, NULL};
    pthread_mutex_unlock(&lock);
    if (!handle) {
        free(b);
        return fail(ENOMEM);
    }
    /* Mesa leaves depth and array size 0 for some buffers; like Linux, a zero
     * size means one page. */
    uint32_t size = c->size ? c->size : PAGE;
    struct user_gpu_resource r = {0,
                                  c->target,
                                  c->format,
                                  bind,
                                  c->width,
                                  c->height,
                                  c->depth ? c->depth : 1,
                                  c->array_size ? c->array_size : 1,
                                  c->last_level,
                                  c->nr_samples,
                                  c->flags,
                                  size};
    int id = request(USER_GPU_CREATE, address(&r), sizeof(r));
    if (id >= 0) *b = (struct buffer){(uint32_t)id, size, 1, NULL, 0};
    pthread_mutex_lock(&lock);
    if (id < 0) objects[handle].owner = NULL;
    else objects[handle].buffer = b;
    pthread_mutex_unlock(&lock);
    if (id < 0) {
        free(b);
        return fail_sos2(id);
    }
    c->bo_handle = handle;
    c->res_handle = (uint32_t)id;
    return 0;
}

static int object_call(struct file *file, uint32_t handle,
                       int (*call)(struct buffer *, uint32_t, void *), void *argument) {
    pthread_mutex_lock(&lock);
    struct object *o = find(file, handle);
    int result = o ? call(o->buffer, handle, argument) : fail(ENOENT);
    pthread_mutex_unlock(&lock);
    return result;
}

static int resource_info(struct buffer *b, uint32_t handle, void *argument) {
    (void)handle;
    struct drm_virtgpu_resource_info *i = argument;
    i->res_handle = b->resource;
    i->size = b->size;
    i->blob_mem = 0;
    return 0;
}

static int map(struct buffer *b, uint32_t handle, void *argument) {
    (void)b;
    struct drm_virtgpu_map *m = argument;
    m->offset = (uint64_t)handle * PAGE;
    return 0;
}

static int transfer(struct buffer *b, void *argument, bool upload) {
    struct drm_virtgpu_3d_transfer_to_host *t = argument;
    if (!ensure_shadow(b)) return fail(ENOMEM);
    if (t->offset >= b->size) return fail(EINVAL);
    struct user_gpu_transfer u = {
        b->resource, t->level,  t->box.x,        t->box.y,
        t->box.z,    t->box.w,  t->box.h,        t->box.d,
        t->offset,   t->stride, t->layer_stride, address((char *)b->shadow + t->offset)};
    int result = request(upload ? USER_GPU_UPLOAD : USER_GPU_DOWNLOAD, address(&u), sizeof(u));
    return result < 0 ? fail_sos2(result) : 0;
}

static int upload(struct buffer *b, uint32_t handle, void *argument) {
    (void)handle;
    return transfer(b, argument, true);
}

static int download(struct buffer *b, uint32_t handle, void *argument) {
    (void)handle;
    return transfer(b, argument, false);
}

static int wait_idle(struct buffer *b, uint32_t handle, void *argument) {
    (void)b;
    (void)handle;
    (void)argument;
    return 0;
}

static int execbuffer(struct drm_virtgpu_execbuffer *e) {
    if (e->flags || e->num_in_syncobjs || e->num_out_syncobjs) return fail(EINVAL);
    if (!e->size) return 0;
    struct user_gpu_submit s = {(uint32_t)e->command, e->size};
    int result = request(USER_GPU_SUBMIT, address(&s), sizeof(s));
    return result < 0 ? fail_sos2(result) : 0;
}

static int gem_close(struct file *file, struct drm_gem_close *c) {
    pthread_mutex_lock(&lock);
    struct object *o = find(file, c->handle);
    struct buffer *gone = o ? unreference(o->buffer) : NULL;
    if (o) *o = (struct object){0};
    pthread_mutex_unlock(&lock);
    if (!o) return fail(EINVAL);
    int result = gone ? destroy(gone, true) : 0;
    return result < 0 ? fail_sos2(result) : 0;
}

static int version(struct drm_version *v) {
    static const char name[] = "virtio_gpu", date[] = "0", desc[] = "virtio-gpu over SOS2 SYS_GPU";
    v->version_major = v->version_minor = v->version_patchlevel = 0;
    const char *text[3] = {name, date, desc};
    char *out[3] = {v->name, v->date, v->desc};
    __kernel_size_t *length[3] = {&v->name_len, &v->date_len, &v->desc_len};
    for (unsigned i = 0; i < 3; ++i) {
        size_t n = strlen(text[i]);
        if (out[i] && *length[i]) memcpy(out[i], text[i], n < *length[i] ? n : *length[i]);
        *length[i] = n;
    }
    return 0;
}

static int get_cap(struct drm_get_cap *c) {
    switch (c->capability) {
        case DRM_CAP_PRIME: c->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; return 0;
        case DRM_CAP_SYNCOBJ:
        case DRM_CAP_SYNCOBJ_TIMELINE: c->value = 0; return 0;
        default: return fail(EINVAL);
    }
}

static int device_ioctl(void *state, unsigned long request, void *argument) {
    struct file *file = state;
    if (!argument) return fail(EFAULT);
    switch (request) {
        case DRM_IOCTL_VERSION: return version(argument);
        case DRM_IOCTL_GET_CAP: return get_cap(argument);
        case DRM_IOCTL_GEM_CLOSE: return gem_close(file, argument);
        case DRM_IOCTL_VIRTGPU_GETPARAM: return getparam(argument);
        case DRM_IOCTL_VIRTGPU_GET_CAPS: return get_caps(argument);
        case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE: return resource_create(file, argument);
        case DRM_IOCTL_VIRTGPU_RESOURCE_INFO:
            return object_call(file, ((struct drm_virtgpu_resource_info *)argument)->bo_handle,
                               resource_info, argument);
        case DRM_IOCTL_VIRTGPU_MAP:
            return object_call(file, ((struct drm_virtgpu_map *)argument)->handle, map, argument);
        case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
            return object_call(file,
                               ((struct drm_virtgpu_3d_transfer_to_host *)argument)->bo_handle,
                               upload, argument);
        case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
            return object_call(file,
                               ((struct drm_virtgpu_3d_transfer_from_host *)argument)->bo_handle,
                               download, argument);
        case DRM_IOCTL_VIRTGPU_WAIT:
            return object_call(file, ((struct drm_virtgpu_3d_wait *)argument)->handle, wait_idle,
                               argument);
        case DRM_IOCTL_VIRTGPU_EXECBUFFER: return execbuffer(argument);
        default: return fail(EINVAL);
    }
}

static void *device_mmap(void *state, size_t length, int prot, int flags, off_t offset) {
    (void)prot;
    (void)flags;
    if (offset < 0 || (offset & (PAGE - 1))) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    pthread_mutex_lock(&lock);
    struct object *o = find(state, (uint32_t)offset / PAGE);
    void *result = MAP_FAILED;
    struct buffer *b = o ? o->buffer : NULL;
    if (!b || !length || length > pages(b->size)) errno = EINVAL;
    else if (!ensure_shadow(b)) errno = ENOMEM;
    else {
        ++b->maps;
        result = b->shadow;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

static int device_munmap(void *state, void *where, size_t length) {
    (void)state;
    (void)length;
    int mine = 0;
    pthread_mutex_lock(&lock);
    for (unsigned i = 1; i <= OBJECTS && !mine; ++i) {
        struct buffer *b = objects[i].buffer;
        if (b && b->shadow == where && b->maps) {
            --b->maps;
            mine = 1;
        }
    }
    pthread_mutex_unlock(&lock);
    return mine;
}

static void *device_open(int flags) {
    (void)flags;
    if (sos2_gpu(USER_GPU_INFO, 0, 0) <= 0) {
        errno = ENODEV;
        return NULL;
    }
    struct file *file = calloc(1, sizeof(*file));
    if (!file) errno = ENOMEM;
    return file;
}

static void device_release(void *state) {
    for (unsigned i = 1; i <= OBJECTS; ++i) {
        pthread_mutex_lock(&lock);
        struct buffer *gone = NULL;
        if (objects[i].owner == state) {
            if (objects[i].buffer) gone = unreference(objects[i].buffer);
            objects[i] = (struct object){0};
        }
        pthread_mutex_unlock(&lock);
        if (gone) (void)destroy(gone, false);
    }
    free(state);
}

static const struct sos2_device render_node = {RENDER_NODE,  device_open, device_release,
                                               device_ioctl, device_mmap, device_munmap};

__attribute__((constructor)) static void register_render_node(void) {
    (void)sos2_device_register(&render_node);
}

/* PRIME: a buffer descriptor holds a reference to its buffer, so the buffer
 * outlives the handle it came from. Every file of the process shares one
 * SYS_GPU context, so any of them may import it. */
struct export {
    struct buffer *buffer;
};

static void export_release(void *state) {
    struct export *e = state;
    pthread_mutex_lock(&lock);
    struct buffer *gone = unreference(e->buffer);
    pthread_mutex_unlock(&lock);
    if (gone) (void)destroy(gone, false);
    free(e);
}
static const struct sos2_device buffer_descriptor = {"dma-buf", NULL, export_release,
                                                     NULL,      NULL, NULL};

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime) {
    struct file *file = sos2_device_state(fd, &render_node);
    if (!file) return -1;
    struct export *e = malloc(sizeof(*e));
    if (!e) return fail(ENOMEM);
    pthread_mutex_lock(&lock);
    struct object *o = find(file, handle);
    if (o && o->buffer) {
        e->buffer = o->buffer;
        ++e->buffer->references;
    }
    pthread_mutex_unlock(&lock);
    if (!o || !o->buffer) {
        free(e);
        return fail(ENOENT);
    }
    int out = sos2_device_open_state(&buffer_descriptor, e, (flags & DRM_CLOEXEC) ? O_CLOEXEC : 0);
    if (out < 0) {
        int error = errno;
        export_release(e);
        errno = error;
        return -1;
    }
    *prime = out;
    return 0;
}

int drmPrimeFDToHandle(int fd, int prime, uint32_t *handle) {
    struct file *file = sos2_device_state(fd, &render_node);
    struct export *e = file ? sos2_device_state(prime, &buffer_descriptor) : NULL;
    if (!e) return -1;
    pthread_mutex_lock(&lock);
    /* Like Linux, a file has one handle per buffer. */
    unsigned found = 0;
    for (unsigned i = 1; i <= OBJECTS && !found; ++i)
        if (objects[i].owner == file && objects[i].buffer == e->buffer) found = i;
    if (!found && (found = free_handle())) {
        objects[found] = (struct object){file, e->buffer};
        ++e->buffer->references;
    }
    pthread_mutex_unlock(&lock);
    if (!found) return fail(ENOMEM);
    *handle = found;
    return 0;
}

int sos2_drm_buffer_resource(int prime) {
    struct export *e = sos2_device_state(prime, &buffer_descriptor);
    return e ? (int)e->buffer->resource : -1;
}

/* libdrm interface. */
int drmIoctl(int fd, unsigned long request, void *argument) {
    int result;
    do result = ioctl(fd, request, argument);
    while (result == -1 && (errno == EINTR || errno == EAGAIN));
    return result;
}

static bool is_render_node(int fd) {
    struct drm_version v = {0};
    return !drmIoctl(fd, DRM_IOCTL_VERSION, &v);
}

drmVersionPtr drmGetVersion(int fd) {
    char name[32] = {0}, date[8] = {0}, desc[64] = {0};
    struct drm_version v = {.name_len = sizeof(name) - 1,
                            .name = name,
                            .date_len = sizeof(date) - 1,
                            .date = date,
                            .desc_len = sizeof(desc) - 1,
                            .desc = desc};
    if (drmIoctl(fd, DRM_IOCTL_VERSION, &v)) return NULL;
    drmVersionPtr out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    out->version_major = v.version_major;
    out->version_minor = v.version_minor;
    out->version_patchlevel = v.version_patchlevel;
    out->name = strdup(name);
    out->date = strdup(date);
    out->desc = strdup(desc);
    if (!out->name || !out->date || !out->desc) {
        drmFreeVersion(out);
        return NULL;
    }
    out->name_len = strlen(name);
    out->date_len = strlen(date);
    out->desc_len = strlen(desc);
    return out;
}

void drmFreeVersion(drmVersionPtr v) {
    if (!v) return;
    free(v->name);
    free(v->date);
    free(v->desc);
    free(v);
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value) {
    struct drm_get_cap c = {capability, 0};
    int result = drmIoctl(fd, DRM_IOCTL_GET_CAP, &c);
    if (!result) *value = c.value;
    return result;
}

int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size) {
    unsigned long request =
        DRM_IOC(DRM_IOC_READWRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, size);
    return drmIoctl(fd, request, data) ? -errno : 0;
}

int drmCloseBufferHandle(int fd, uint32_t handle) {
    struct drm_gem_close c = {handle, 0};
    return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

/* One platform device with only a render node. Like libdrm, each device is
 * one allocation that drmFreeDevice releases. */
static drmDevicePtr new_device(void) {
    static const char name[] = "virtio-gpu", compatible[] = "virtio,gpu";

    struct block {
        drmDevice device;
        char *nodes[DRM_NODE_MAX];
        char node[sizeof(RENDER_NODE)];
        drmPlatformBusInfo bus;
        drmPlatformDeviceInfo info;
        char *compatible[2];
        char compatible_name[sizeof(compatible)];
    } *b = calloc(1, sizeof(struct block));

    if (!b) return NULL;
    memcpy(b->node, RENDER_NODE, sizeof(RENDER_NODE));
    b->nodes[DRM_NODE_RENDER] = b->node;
    memcpy(b->bus.fullname, name, sizeof(name));
    memcpy(b->compatible_name, compatible, sizeof(compatible));
    b->compatible[0] = b->compatible_name;
    b->info.compatible = b->compatible;
    b->device.nodes = b->nodes;
    b->device.available_nodes = 1 << DRM_NODE_RENDER;
    b->device.bustype = DRM_BUS_PLATFORM;
    b->device.businfo.platform = &b->bus;
    b->device.deviceinfo.platform = &b->info;
    return &b->device;
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max) {
    (void)flags;
    if (sos2_gpu(USER_GPU_INFO, 0, 0) <= 0) return 0;
    if (!devices || max < 1) return 1;
    devices[0] = new_device();
    return devices[0] ? 1 : -ENOMEM;
}

int drmGetDevices(drmDevicePtr devices[], int max) { return drmGetDevices2(0, devices, max); }

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) {
    (void)flags;
    if (!device) return -EINVAL;
    if (!is_render_node(fd)) return -ENODEV;
    *device = new_device();
    return *device ? 0 : -ENOMEM;
}

int drmGetDevice(int fd, drmDevicePtr *device) { return drmGetDevice2(fd, 0, device); }

int drmGetDeviceFromDevId(dev_t id, uint32_t flags, drmDevicePtr *device) {
    (void)id;
    (void)flags;
    (void)device;
    return -ENODEV;
}

void drmFreeDevice(drmDevicePtr *device) {
    if (!device) return;
    free(*device);
    *device = NULL;
}

void drmFreeDevices(drmDevicePtr devices[], int count) {
    for (int i = 0; devices && i < count; ++i) drmFreeDevice(&devices[i]);
}

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b) {
    return a && b && a->bustype == b->bustype && a->bustype == DRM_BUS_PLATFORM &&
           !strcmp(a->businfo.platform->fullname, b->businfo.platform->fullname);
}

char *drmGetDeviceNameFromFd2(int fd) { return is_render_node(fd) ? strdup(RENDER_NODE) : NULL; }

char *drmGetRenderDeviceNameFromFd(int fd) { return drmGetDeviceNameFromFd2(fd); }

char *drmGetPrimaryDeviceNameFromFd(int fd) {
    (void)fd;
    return NULL;
}

int drmGetNodeTypeFromFd(int fd) { return is_render_node(fd) ? DRM_NODE_RENDER : -1; }

/* No sync objects: every operation completes before it returns. */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle) {
    (void)fd;
    (void)flags;
    (void)handle;
    return fail(ENOSYS);
}

int drmSyncobjDestroy(int fd, uint32_t handle) {
    (void)fd;
    (void)handle;
    return fail(ENOSYS);
}

int drmSyncobjHandleToFD(int fd, uint32_t handle, int *out) {
    (void)fd;
    (void)handle;
    (void)out;
    return fail(ENOSYS);
}

int drmSyncobjFDToHandle(int fd, int in, uint32_t *handle) {
    (void)fd;
    (void)in;
    (void)handle;
    return fail(ENOSYS);
}

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int in) {
    (void)fd;
    (void)handle;
    (void)in;
    return fail(ENOSYS);
}

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *out) {
    (void)fd;
    (void)handle;
    (void)out;
    return fail(ENOSYS);
}

int drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout, unsigned flags,
                   uint32_t *first) {
    (void)fd;
    (void)handles;
    (void)count;
    (void)timeout;
    (void)flags;
    (void)first;
    return fail(ENOSYS);
}

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count) {
    (void)fd;
    (void)handles;
    (void)count;
    return fail(ENOSYS);
}

int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count) {
    (void)fd;
    (void)handles;
    (void)count;
    return fail(ENOSYS);
}

int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count) {
    (void)fd;
    (void)handles;
    (void)points;
    (void)count;
    return fail(ENOSYS);
}

int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count,
                           int64_t timeout, unsigned flags, uint32_t *first) {
    (void)fd;
    (void)handles;
    (void)points;
    (void)count;
    (void)timeout;
    (void)flags;
    (void)first;
    return fail(ENOSYS);
}

int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags) {
    (void)fd;
    (void)handles;
    (void)points;
    (void)count;
    (void)flags;
    return fail(ENOSYS);
}

int drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point,
                       uint32_t flags) {
    (void)fd;
    (void)dst;
    (void)dst_point;
    (void)src;
    (void)src_point;
    (void)flags;
    return fail(ENOSYS);
}
