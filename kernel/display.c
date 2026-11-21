#include "display.h"
#include "platform.h"
/* QEMU's displays: virtio-gpu, else ramfb. The devices keep the framebuffer
 * their loader set up, the Redmi 2 with its Adreno; the Nexus 5's panel
 * shows it only as frames sent to it (boards/nexus5/display.c). */
#ifndef SOS2_DEVICE
#define QEMU_DISPLAY 1
#endif
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_adreno.h"
#endif
#ifdef SOS2_BOARD_NEXUS5
#include "nexus5.h"
#endif
#ifndef SOS2_BOARD_REDMI2
static struct framebuffer *screen;
#endif
#ifdef QEMU_DISPLAY
#include "ramfb.h"
#include "virtio_gpu.h"
static uintptr_t fallback_base;
static bool gpu, failed;
#endif
bool display_init(const struct platform_devices *devices, struct framebuffer *out) {
#ifdef SOS2_BOARD_REDMI2
    (void)devices;
    (void)out;
    return false; /* The phone uses its verified handoff. */
#elif defined(SOS2_LOADER_FRAMEBUFFER)
    /* The bootloader's framebuffer, from the verified handoff; no GPU. */
    screen = out;
    *out = devices->framebuffer;
    return devices->has_framebuffer;
#else
    screen = out;
    fallback_base = devices->fwcfg;
    gpu = virtio_gpu_init(devices, out);
    return gpu || ramfb_init(fallback_base, out);
#endif
}

void display_poll(void) {
#ifdef SOS2_BOARD_REDMI2
    redmi2_adreno_poll();
#elif defined(SOS2_BOARD_NEXUS5)
    nexus5_display_poll();
#elif defined(QEMU_DISPLAY)
    if (!gpu || failed) return;
    virtio_gpu_poll();
    if (virtio_gpu_active()) return;
    /* Keep the desktop and pages alive after a GPU fault. The launcher carries
     * a separate RAMFB console; both the shadow and DMA areas remain reserved. */
    struct framebuffer fallback;
    if (ramfb_init(fallback_base, &fallback)) {
        (void)gfx_present(&fallback, screen);
        *screen = fallback;
        gpu = false;
    } else {
        failed = true;
    }
#endif
}

bool display_gpu_frames(void) {
#ifdef QEMU_DISPLAY
    return gpu && !failed && virtio_gpu_client_available();
#else
    return false;
#endif
}

void display_flush_error(void) {
#ifdef SOS2_BOARD_REDMI2
    redmi2_adreno_stop();
#elif defined(SOS2_BOARD_NEXUS5)
    /* Only the boot CPU sends frames; its waits read the timer. */
    if (!arch_cpu_affinity() && arch_counter_frequency()) nexus5_display_flush();
#elif defined(QEMU_DISPLAY)
    if (!gpu || failed) return;
    /* Only the boot CPU owns the queue. A broken timer must not recurse into
     * panic through the driver's timeout checks. */
    uint32_t affinity = arch_cpu_affinity(), frequency = arch_counter_frequency();
    if (affinity || !frequency) return;
    virtio_gpu_page_view(UINT32_MAX, WEB_HEIGHT);
    virtio_gpu_damage(0, 0, screen->width, screen->height);
    (void)virtio_gpu_sync();
    display_poll();
#endif
}

void display_blank(void) {
#ifndef SOS2_BOARD_REDMI2
    /* Black in standby: QEMU has no backlight, and the tablet's panel wakes
     * showing it until the next full redraw restores the desktop and pages.
     * Never drain here (virtio_gpu_sync is for panics): a page's GPU request
     * may be waiting for its own command. */
    if (!screen) return;
    gfx_rect(screen, 0, 0, (int)screen->width, (int)screen->height, 0);
#ifdef SOS2_BOARD_NEXUS5
    nexus5_display_damage();
#endif
#ifdef QEMU_DISPLAY
    if (gpu && !failed) {
        virtio_gpu_page_view(UINT32_MAX, WEB_HEIGHT);
        virtio_gpu_damage(0, 0, screen->width, screen->height);
    }
#endif
#endif
}

bool display_present_region(struct framebuffer *visible, const struct framebuffer *source,
                            unsigned x, unsigned y, unsigned width, unsigned height) {
#ifdef SOS2_BOARD_REDMI2
    if (redmi2_adreno_present(visible, source, x, y, width, height)) return true;
#endif
    if (!gfx_present_scaled_region(visible, source, x, y, width, height)) return false;
#ifdef SOS2_BOARD_NEXUS5
    if (visible == screen) nexus5_display_damage();
#endif
#ifdef QEMU_DISPLAY
    if (gpu && !failed && visible == screen && width && height && x < source->width &&
        y < source->height) {
        if (width > source->width - x) width = source->width - x;
        if (height > source->height - y) height = source->height - y;
        unsigned x0 = ((uint64_t)x * visible->width + source->width - 1) / source->width;
        unsigned y0 = ((uint64_t)y * visible->height + source->height - 1) / source->height;
        unsigned x1 = ((uint64_t)(x + width) * visible->width + source->width - 1) / source->width;
        unsigned y1 =
            ((uint64_t)(y + height) * visible->height + source->height - 1) / source->height;
        virtio_gpu_damage(x0, y0, x1 - x0, y1 - y0);
    }
#endif
    return true;
}

bool display_present(struct framebuffer *visible, const struct framebuffer *source) {
    return source && display_present_region(visible, source, 0, 0, source->width, source->height);
}

void display_damage(unsigned x, unsigned y, unsigned width, unsigned height) {
#ifdef QEMU_DISPLAY
    /* The virtio scanout has the composition's size. */
    if (gpu && !failed) virtio_gpu_damage(x, y, width, height);
#else
    (void)x;
    (void)y;
    (void)width;
    (void)height; /* The devices' pages present CPU frames only. */
#endif
}

void display_page_view(unsigned owner, unsigned height) {
#ifdef QEMU_DISPLAY
    virtio_gpu_page_view(owner, height);
#elif defined(SOS2_BOARD_REDMI2)
    /* The phone retains only whole page views; it has no GPU frames. */
    redmi2_adreno_page_view(height == WEB_HEIGHT ? owner : UINT32_MAX);
#else
    (void)owner;
    (void)height;
#endif
}

void display_page_changed(unsigned owner, unsigned x, unsigned y, unsigned width, unsigned height) {
#ifdef QEMU_DISPLAY
    virtio_gpu_page_changed(owner, x, y, width, height);
#elif defined(SOS2_BOARD_REDMI2)
    redmi2_adreno_page_changed(owner, x, y, width, height);
#else
    (void)owner;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
#endif
}

void display_page_cpu_frames(unsigned owner) {
#ifdef QEMU_DISPLAY
    virtio_gpu_frame_inactive(owner);
#else
    (void)owner;
#endif
}

void display_page_closed(unsigned owner) {
#ifdef QEMU_DISPLAY
    virtio_gpu_page_closed(owner);
#elif defined(SOS2_BOARD_REDMI2)
    redmi2_adreno_page_closed(owner);
#else
    (void)owner;
#endif
}

void display_page_scroll(unsigned owner, const struct user_pixel_scroll *scroll) {
#ifdef QEMU_DISPLAY
    virtio_gpu_page_scroll(owner, scroll);
#elif defined(SOS2_BOARD_REDMI2)
    redmi2_adreno_page_scroll(owner, scroll);
#else
    (void)owner;
    (void)scroll;
#endif
}
