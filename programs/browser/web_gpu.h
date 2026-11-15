#pragma once
// GPU rasterization. When the kernel gives the page a virgl context (SYS_GPU,
// QEMU only), the page paints into a Skia surface on Mesa's OpenGL ES over
// that context instead of into its pixel buffer. The surface lives in WebKit's
// shared display and Skia context (PlatformDisplaySOS2), which WebKit's GPU
// compositor (web_gpu_compositor.h) also draws with. WebCore's direct paint
// does not know: it paints through the same GraphicsContextSkia, and images
// and filters stay in CPU memory until Skia uploads them. The kernel
// compositor takes frames from the surface itself (USER_GPU_PRESENT, through
// the libdrm shim's dma-buf export); without that, painted rectangles are
// read back into the pixel buffer, which is then presented as before.
//
// A virgl context the host disables (for a command it refuses) fails silently,
// so readbacks also check a probe pixel drawn after the frame, at most every
// 250 ms. When that check, a readback or the device fails, the GPU is lost for
// good and the page paints on the CPU again.
#include <WebCore/IntRect.h>
#include <skia/core/SkRefCnt.h>
#include <cstdint>
#include <memory>
#include <span>

class GrDirectContext;
class SkSurface;

namespace WebCore {
class GLContext;
}

namespace SOS2 {

class GPURaster {
public:
    // Null when the page has no GPU, or Mesa or Skia cannot start on it.
    static std::unique_ptr<GPURaster> create(unsigned width, unsigned height);
    ~GPURaster();

    SkSurface &surface() { return *m_surface; }

    // Moves the pixels of rect by (dx,dy), copying exactly what
    // SOS2::scrollPixels copies in the pixel buffer.
    void scroll(const WebCore::IntRect &rect, int dx, int dy);
    // Submits the frame's drawing and copies the rectangles into pixels
    // (BGRA, `stride` pixels per row). False when the GPU was lost.
    bool readBack(std::span<const WebCore::IntRect>, uint32_t *pixels, unsigned stride);

    // Whether frames can go to the compositor without a readback.
    bool canPresent() const { return m_resource > 0; }

    // Submits the frame's drawing and shows the surface's changed rectangles
    // (at most USER_FRAME_RECTS; the first frame covers the whole surface).
    // False when the GPU was lost.
    bool present(std::span<const WebCore::IntRect>);

private:
    GPURaster() = default;
    bool probe();
    bool check(uint64_t now);
    void lose();
    int exportSurface();

    void *m_display{nullptr};               // WebKit's shared EGL display
    WebCore::GLContext *m_context{nullptr}; // its Skia context, shared with the compositor
    sk_sp<GrDirectContext> m_skia;
    sk_sp<SkSurface> m_surface;
    sk_sp<SkSurface> m_probe;
    uint32_t m_frame{0};
    uint64_t m_probedAt{0};
    int m_resource{0}; // the surface's SYS_GPU resource, when presentable
    bool m_lost{false};
};

}
