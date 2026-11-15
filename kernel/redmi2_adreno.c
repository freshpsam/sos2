#include "redmi2_adreno.h"
#include "adreno.h"
#include "runtime_files.h"
#include "page_alloc.h"
#include "qcom_scm.h"
#include "platform.h"
#include "pixel_frame.h"

static struct adreno_device gpu;
static struct framebuffer screen, latest;
static bool enabled, dirty, submitted, have_frame, napping;
/* Napping with the power domain off (adreno_power_off), not just the clocks. */
static bool collapsed;
static unsigned left, top, right, bottom;
static unsigned view_owner = UINT32_MAX, view_generation, submitted_generation;
static bool retained_valid, full_upload = true, page_updates, scroll_pending;
static struct adreno_copy scroll_copy;
static struct adreno_rect patches[ADRENO_PATCHES];
static unsigned patch_count;
_Static_assert(WEB_WIDTH == 480 && WEB_HEIGHT == 752 && WEB_TOP == 48,
               "retained composition geometry");

static uint32_t io_read(void *p, uintptr_t a) {
    (void)p;
    return mmio_read32(a);
}

static void io_write(void *p, uintptr_t a, uint32_t v) {
    (void)p;
    mmio_write32(a, v);
}

static uint64_t io_now(void *p) {
    (void)p;
    return time_us();
}

static void io_barrier(void *p) {
    (void)p;
    barrier();
}

static bool io_restore(void *p, unsigned cb) {
    (void)p;
    return qcom_scm_gpu_restore(cb);
}

static void display_sync(void) { barrier(); }

void redmi2_adreno_init(struct framebuffer *fb) {
    if (gpu.attempted || enabled) return;
    if (!gfx_framebuffer_valid(fb) || fb->width != 720 || fb->height != 1280) {
        gpu.error = ADRENO_ARGUMENT;
        return;
    }
    screen = *fb;
    const struct runtime_file *pm4 = runtime_file_find("/firmware/adreno/a300_pm4.fw");
    const struct runtime_file *pfp = runtime_file_find("/firmware/adreno/a300_pfp.fw");
    if (!pm4 || !pfp ||
        !adreno_firmware_valid(runtime_file_data(pm4), pm4->size, runtime_file_data(pfp),
                               pfp->size)) {
        gpu.error = ADRENO_FIRMWARE;
        return;
    }
    void *memory = pages_alloc(ADRENO_ARENA_SIZE / PAGE_SIZE, 1);
    if (!memory) {
        gpu.error = ADRENO_ARGUMENT;
        return;
    }
    struct adreno_io io = {0, io_read, io_write, io_now, io_barrier, io_restore};
    bool started = adreno_init(&gpu, &io, memory, runtime_file_data(pm4), pm4->size,
                               runtime_file_data(pfp), pfp->size, fb->format, fb->stride);
    if (!started || !adreno_selftest(&gpu)) {
        if (gpu.running) adreno_disable(&gpu, ADRENO_READBACK);
        /* Once powered, even a failed reset may leave DMA in flight. These
         * pages stay reserved and are never returned to the process pool. */
        if (!gpu.powered && !gpu.quarantined)
            (void)pages_free(memory, ADRENO_ARENA_SIZE / PAGE_SIZE);
        return;
    }
    enabled = true;
}

static void patch(unsigned x, unsigned y, unsigned width, unsigned height) {
    if (full_upload || !width || !height || x >= 480 || y >= 800) return;
    if (width > 480 - x) width = 480 - x;
    if (height > 800 - y) height = 800 - y;
    unsigned right = (x + width + 15) & ~15u, bottom = (y + height + 7) & ~7u;
    x &= ~15u;
    y &= ~7u;
    struct adreno_rect r = {x, y, right - x, bottom - y};
    /* Merge only when the bounding box costs no more upload than the two
     * rectangles. In particular, preserve L-shaped exposed scroll strips. */
    for (unsigned i = 0; i < patch_count;) {
        struct adreno_rect p = patches[i];
        unsigned x0 = r.x < p.x ? r.x : p.x, y0 = r.y < p.y ? r.y : p.y;
        unsigned x1 = r.x + r.width > p.x + p.width ? r.x + r.width : p.x + p.width;
        unsigned y1 = r.y + r.height > p.y + p.height ? r.y + r.height : p.y + p.height;
        if ((x1 - x0) * (y1 - y0) <= r.width * r.height + p.width * p.height) {
            r = (struct adreno_rect){x0, y0, x1 - x0, y1 - y0};
            patches[i] = patches[--patch_count];
            i = 0;
        } else ++i;
    }
    if (patch_count == ADRENO_PATCHES) {
        full_upload = true;
        scroll_pending = false;
        patch_count = 0;
        return;
    }
    patches[patch_count++] = r;
}

static void invalidate_view(void) {
    ++view_generation;
    retained_valid = false;
    full_upload = true;
    patch_count = 0;
    scroll_pending = false;
    page_updates = false;
    /* Wait for composition after the view notification; the previous view's
     * queued damage cannot establish the new view's retained contents. */
    dirty = false;
}

void redmi2_adreno_page_view(unsigned owner) {
    if (owner == view_owner) return;
    view_owner = owner;
    invalidate_view();
}

void redmi2_adreno_page_closed(unsigned owner) {
    if (owner != UINT32_MAX && owner == view_owner) {
        view_owner = UINT32_MAX;
        invalidate_view();
    }
}

void redmi2_adreno_page_changed(unsigned owner, unsigned x, unsigned y, unsigned width,
                                unsigned height) {
    if (owner == UINT32_MAX || owner != view_owner) return;
    if (!pixel_rect_valid((struct user_pixel_rect){x, y, width, height})) return;
    page_updates = true;
    patch(x, y + WEB_TOP, width, height);
}

void redmi2_adreno_page_scroll(unsigned owner, const struct user_pixel_scroll *s) {
    if (owner == UINT32_MAX || owner != view_owner || !s || !pixel_scroll_valid(s)) return;
    if (!retained_valid || full_upload || page_updates) {
        full_upload = true;
        scroll_pending = false;
        page_updates = true;
        return;
    }
    struct user_pixel_rect from, to;
    pixel_scroll_boxes(s, &from, &to);
    unsigned w = to.width & ~15u, h = to.height & ~7u;
    page_updates = true;
    if (!w || !h) {
        patch(s->rect.x, s->rect.y + WEB_TOP, s->rect.width, s->rect.height);
        return;
    }
    scroll_copy = (struct adreno_copy){from.x, from.y + WEB_TOP, to.x, to.y + WEB_TOP, w, h};
    scroll_pending = true;
    /* The closed compute program has whole 16x8 workgroups. Upload rounded
     * tails from the authoritative CPU shadow instead of writing past clips. */
    patch(to.x + w, to.y + WEB_TOP, to.width - w, to.height);
    patch(to.x, to.y + WEB_TOP + h, w, to.height - h);
}

static void fallback(void) {
    enabled = false;
    submitted = false;
    dirty = false;
    if (latest.pixels) (void)gfx_present_scaled(&screen, &latest);
}

void redmi2_adreno_poll(void) {
    if (!enabled || napping) return;
    int status = adreno_poll(&gpu);
    if (status < 0) {
        fallback();
        return;
    }
    if (!status) return;
    if (submitted) {
        /* Publish only a complete fenced job. The GPU has no mapping of the
         * live framebuffer, so fallback never races stale GPU writes. */
        if (submitted_generation == view_generation) {
            unsigned offset = gpu.x * gpu.bytes, size = gpu.width * gpu.bytes;
            for (unsigned y = gpu.y; y < gpu.y + gpu.height; ++y)
                memcpy((unsigned char *)screen.pixels + y * screen.stride + offset,
                       gpu.memory + ADRENO_OUTPUT + y * gpu.stride + offset, size);
            display_sync();
            have_frame = true;
        }
        submitted = false;
    }
    if (!dirty) return;
    /* Snapshot only patches; the moved content stays in GPU-owned SOURCE.
     * UPLOAD is separate and immutable until the complete batch retires. */
    unsigned x0 = left, y0 = top, x1 = right, y1 = bottom;
    if (!have_frame || !retained_valid || full_upload) {
        x0 = y0 = 0;
        x1 = 480;
        y1 = 800;
        scroll_pending = false;
        patches[0] = (struct adreno_rect){0, 0, 480, 800};
        patch_count = 1;
    }
    unsigned dx0 = ((x0 * 3 + 1) / 2) & ~15u, dy0 = ((y0 * 8 + 4) / 5) & ~7u;
    unsigned dx1 = (((x1 * 3 + 1) / 2) + 15) & ~15u, dy1 = (((y1 * 8 + 4) / 5) + 7) & ~7u;
    if (dx1 > 720) dx1 = 720;
    if (dy1 > 1280) dy1 = 1280;
    for (unsigned i = 0; i < patch_count; ++i) {
        struct adreno_rect r = patches[i];
        for (unsigned y = r.y; y < r.y + r.height; ++y)
            memcpy(gpu.memory + ADRENO_UPLOAD + (y * 480 + r.x) * 4,
                   (const unsigned char *)latest.pixels + (y * 480 + r.x) * 4, r.width * 4);
    }
    if (!adreno_frame(&gpu, scroll_pending ? &scroll_copy : 0, patches, patch_count,
                      (struct adreno_rect){dx0, dy0, dx1 - dx0, dy1 - dy0})) {
        adreno_disable(&gpu, ADRENO_ENGINE);
        fallback();
        return;
    }
    dirty = false;
    patch_count = 0;
    scroll_pending = false;
    page_updates = false;
    full_upload = false;
    retained_valid = true;
    submitted = true;
    submitted_generation = view_generation;
}

bool redmi2_adreno_present(struct framebuffer *fb, const struct framebuffer *source, unsigned x,
                           unsigned y, unsigned width, unsigned height) {
    if (!enabled || !fb || !source || fb->pixels != screen.pixels || fb->width != screen.width ||
        fb->height != screen.height || fb->stride != screen.stride || fb->format != screen.format ||
        !gfx_framebuffer_valid(source) || source->width != 480 || source->height != 800 ||
        source->stride != 1920 || source->format != PIXEL_XRGB8888)
        return false;
    uintptr_t start = (uintptr_t)source->pixels, end = start + source->stride * source->height;
    if ((start < (uintptr_t)gpu.memory + ADRENO_ARENA_SIZE && (uintptr_t)gpu.memory < end) ||
        (start < (uintptr_t)screen.pixels + screen.stride * screen.height &&
         (uintptr_t)screen.pixels < end))
        return false;
    if (!width || !height || x >= 480 || y >= 800) return true;
    if (width > 480 - x) width = 480 - x;
    if (height > 800 - y) height = 800 - y;
    if (latest.pixels && latest.pixels != source->pixels) {
        invalidate_view();
        x = y = 0;
        width = 480;
        height = 800;
    }
    latest = *source;
    if (view_owner == UINT32_MAX || !page_updates) patch(x, y, width, height);
    else if (y < WEB_TOP) patch(x, y, width, height < WEB_TOP - y ? height : WEB_TOP - y);
    if (!dirty) {
        left = x;
        top = y;
        right = x + width;
        bottom = y + height;
        dirty = true;
    } else {
        if (x < left) left = x;
        if (y < top) top = y;
        if (x + width > right) right = x + width;
        if (y + height > bottom) bottom = y + height;
    }
    return true;
}

void redmi2_adreno_stop(void) {
    if (!enabled) return;
    if (napping) {
        napping = false;
        collapsed = false;
        (void)adreno_nap(&gpu, false);
    }
    adreno_disable(&gpu, ADRENO_ENGINE);
    enabled = false;
    submitted = false;
    dirty = false;
}

bool redmi2_adreno_standby(bool nap) {
    if (nap == napping) return napping;
    if (!nap) {
        napping = false;
        if (collapsed) {
            /* The engine restarts empty. The arena, the retained view with it,
             * is in DDR, and the last job flushed the GPU's caches into it. */
            collapsed = false;
            if (!adreno_power_on(&gpu)) {
                if (gpu.error == ADRENO_OK) gpu.error = ADRENO_POWER;
                fallback();
            }
            return false;
        }
        if (!adreno_nap(&gpu, false)) {
            adreno_disable(&gpu, ADRENO_CLOCK);
            fallback();
        }
        return false;
    }
    /* Retire a finished job first; rest only with nothing in flight: the
     * power domain off as Linux's slumber, else just the clocks. */
    redmi2_adreno_poll();
    if (!enabled || submitted || adreno_poll(&gpu) != 1) return false;
    collapsed = adreno_power_off(&gpu);
    napping = collapsed || adreno_nap(&gpu, true);
    return napping;
}

bool redmi2_adreno_resting(void) { return !enabled || napping; }
