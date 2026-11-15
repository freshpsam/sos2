#ifndef SOS2_DRM_STATUS_H
#define SOS2_DRM_STATUS_H
/* SOS2 libdrm shim (drm.c) status for programs using Mesa. */
#ifdef __cplusplus
extern "C" {
#endif
/* 1 once a request to the render node has failed: the device went away, or
 * refused a resource, transfer or command stream (memory exhausted, a bad
 * box). Drawing since then may be missing, so programs should stop using the
 * GPU. Errors on the host's side do not reach here (see web_gpu.h). */
int sos2_drm_failed(void);
/* The SYS_GPU resource behind a buffer descriptor from drmPrimeHandleToFD
 * (such as EGL's dma-buf export), or -1 with errno. */
int sos2_drm_buffer_resource(int prime_fd);
#ifdef __cplusplus
}
#endif
#endif
