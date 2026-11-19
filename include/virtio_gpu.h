#ifndef SOS2_VIRTIO_GPU_H
#define SOS2_VIRTIO_GPU_H
#include "fdt.h"
#include "user_abi.h"

/* Kernel-owned scanout/compositor. Pages reach virgl only through their own
 * contexts (virtio_gpu_client_*), never this one. Call only from the boot CPU.
 * Rendering writes the returned shadow surface; the device reads a separate
 * snapshot, refreshed only once the previous frame's flush completed. */
bool virtio_gpu_init(const struct platform_devices *, struct framebuffer *);
bool virtio_gpu_active(void);
void virtio_gpu_damage(unsigned x, unsigned y, unsigned width, unsigned height);
void virtio_gpu_poll(void);
/* Bounded drain for a terminal error screen; ordinary frames stay asynchronous. */
bool virtio_gpu_sync(void);
/* Optional virgl compositor. Four texture cache entries, never a page limit.
 * Owners are kernel page slots, not user-supplied device/resource handles.
 * A view shows the top height rows of owner's cache (display_page_view). */
void virtio_gpu_page_view(unsigned owner, unsigned height);
void virtio_gpu_page_changed(unsigned owner, unsigned x, unsigned y, unsigned width,
                             unsigned height);
void virtio_gpu_page_scroll(unsigned owner, const struct user_pixel_scroll *);
void virtio_gpu_page_closed(unsigned owner);
/* Page GPU clients (see kernel/protection/gpu_client.c), from the service
 * worker only. begin/end bracket every sequence of client commands. */
bool virtio_gpu_client_available(void);
bool virtio_gpu_client_quarantined(void);
const void *virtio_gpu_client_caps(unsigned id, unsigned *bytes);
int virtio_gpu_client_begin(void);
void virtio_gpu_client_end(void);
int virtio_gpu_client_context(unsigned context, bool create);
int virtio_gpu_client_create(unsigned context, unsigned id, const struct user_gpu_resource *,
                             const void *backing, uint32_t bytes);
int virtio_gpu_client_destroy(unsigned context, unsigned id);
int virtio_gpu_client_transfer(unsigned context, unsigned id, bool to_host,
                               const struct user_gpu_transfer *);
/* The kernel copies a page's stream here, then submits that many bytes. */
void *virtio_gpu_client_stream(void);
int virtio_gpu_client_submit(unsigned context, uint32_t bytes);
/* GPU frames (USER_GPU_PRESENT), at most one snapshot per GPU client. present
 * blits the damage of a page resource (validated by the caller) into the
 * client's snapshot, which then feeds page owner's cache texture; detach must
 * precede destroying a shown resource, and release ends the client. inactive
 * makes owner's cache take CPU frames again (from any context). */
#define VIRTIO_GPU_FRAMES 16
int virtio_gpu_frame_present(unsigned client, unsigned owner, unsigned context, uint32_t resource,
                             unsigned format, const struct user_pixel_rect *rects, unsigned count);
int virtio_gpu_frame_detach(unsigned client, uint32_t resource);
void virtio_gpu_frame_release(unsigned client);
void virtio_gpu_frame_inactive(unsigned owner);
#endif
