#include "config.h"
#include "web_gpu.h"
#include "web_clock.h"
#include "web_damage.h"
#include <WebCore/FontRenderOptions.h>
#include <WebCore/GLContext.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/PlatformDisplaySOS2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm_status.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkSurface.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <sos2/services.h>
#include <cstring>
#include <unistd.h>

namespace SOS2 {

static constexpr uint64_t probeIntervalUs = 250000;

std::unique_ptr<GPURaster> GPURaster::create(unsigned width, unsigned height) {
    if (sos2_gpu(USER_GPU_INFO, 0, 0) <= 0) return nullptr;
    std::unique_ptr<GPURaster> gpu(new GPURaster);
    auto fail = [&]() -> std::unique_ptr<GPURaster> {
        // A display that cannot paint must not turn on WebCore's GPU paths.
        gpu.reset();
        if (WebCore::PlatformDisplay::sharedDisplayIfExists())
            WebCore::PlatformDisplay::destroySharedDisplay();
        return nullptr;
    };
    // WebKit's shared display: its Skia context (surfaceless, with Skia's
    // cache limit and no MSAA, see PlatformDisplaySkia) is the one WebKit's
    // compositor draws tiles with, so its textures are usable here.
    if (!WebCore::PlatformDisplaySOS2::initialize()) return fail();
    auto &display = WebCore::PlatformDisplay::sharedDisplay();
    auto *context = display.skiaGLContext();
    if (!context || !context->makeContextCurrent()) return fail();
    gpu->m_display = display.eglDisplay();
    gpu->m_context = context;
    gpu->m_skia = sk_ref_sp(display.skiaGrContext());
    if (!gpu->m_skia) return fail();
    // The pixel buffer's layout, so readbacks are plain copies, and WebKit's
    // text contrast and gamma, as on every page surface (web_main.cpp).
    auto info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    auto props = WebCore::FontRenderOptions::singleton().createSurfaceProps();
    gpu->m_surface = SkSurfaces::RenderTarget(gpu->m_skia.get(), skgpu::Budgeted::kNo, info, 0,
                                              kTopLeft_GrSurfaceOrigin, &props);
    gpu->m_probe = SkSurfaces::RenderTarget(
        gpu->m_skia.get(), skgpu::Budgeted::kNo,
        SkImageInfo::Make(1, 1, kBGRA_8888_SkColorType, kPremul_SkAlphaType), 0,
        kTopLeft_GrSurfaceOrigin, nullptr);
    if (!gpu->m_surface || !gpu->m_probe) return fail();
    gpu->m_surface->getCanvas()->clear(SK_ColorWHITE);
    if (!gpu->probe()) return fail();
    gpu->m_resource = gpu->exportSurface();
    return gpu;
}

// The surface's texture, exported through EGL as a dma-buf whose SYS_GPU
// resource the shim knows. 0 when any step is missing: frames are then read back.
int GPURaster::exportSurface() {
    auto *display = static_cast<EGLDisplay>(m_display);
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_KHR_gl_texture_2D_image") ||
        !strstr(extensions, "EGL_MESA_image_dma_buf_export"))
        return 0;
    auto createImage =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroyImage =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto exportImage = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(
        eglGetProcAddress("eglExportDMABUFImageMESA"));
    GrGLTextureInfo texture;
    auto backend =
        SkSurfaces::GetBackendTexture(m_surface.get(), SkSurfaces::BackendHandleAccess::kFlushRead);
    if (!createImage || !destroyImage || !exportImage || !backend.isValid() ||
        !GrBackendTextures::GetGLTextureInfo(backend, &texture))
        return 0;
    EGLImageKHR image = createImage(
        display, static_cast<EGLContext>(m_context->platformContext()), EGL_GL_TEXTURE_2D_KHR,
        reinterpret_cast<EGLClientBuffer>(uintptr_t(texture.fID)), nullptr);
    if (image == EGL_NO_IMAGE_KHR) return 0;
    int descriptor = -1;
    EGLint stride = 0, offset = 0;
    int resource = exportImage(display, image, &descriptor, &stride, &offset)
                       ? sos2_drm_buffer_resource(descriptor)
                       : -1;
    destroyImage(display, image);
    // Mesa's texture keeps the buffer; only its resource number is needed.
    if (descriptor >= 0) close(descriptor);
    return resource > 0 ? resource : 0;
}

GPURaster::~GPURaster() {
    // Skia releases its GL objects while the context is current, unless lost.
    // The display and context stay: WebKit owns them.
    m_surface.reset();
    m_probe.reset();
    m_skia.reset();
}

void GPURaster::lose() {
    if (m_lost) return;
    m_lost = true;
    // Issue no further GL work; the destructor frees what is left.
    m_skia->abandonContext();
}

// The host ignores a disabled context's commands without an error, so a new
// probe value drawn after the frame must come back from the device.
bool GPURaster::probe() {
    ++m_frame;
    SkColor expected = SkColorSetARGB(255, m_frame & 0xff, (m_frame >> 8) & 0xff, 0x5a);
    m_probe->getCanvas()->clear(expected);
    m_skia->flushAndSubmit();
    uint32_t value = 0;
    auto info = SkImageInfo::Make(1, 1, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    return m_probe->readPixels(info, &value, sizeof(value), 0, 0) && value == expected &&
           !sos2_drm_failed();
}

void GPURaster::scroll(const WebCore::IntRect &destination, int dx, int dy) {
    if (m_lost || destination.isEmpty()) return;
    auto source = SkIRect::MakeXYWH(destination.x() - dx, destination.y() - dy, destination.width(),
                                    destination.height());
    auto image = m_surface->makeImageSnapshot(source);
    if (!image) {
        lose();
        return;
    }
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    m_surface->getCanvas()->drawImage(image, destination.x(), destination.y(), SkSamplingOptions(),
                                      &paint);
}

bool GPURaster::readBack(std::span<const WebCore::IntRect> rectangles, uint32_t *pixels,
                         unsigned stride) {
    if (m_lost) return false;
    auto started = microseconds();
    // Every readback is a round trip to the device (a submission and a
    // download): read dense rectangles as their bounds at once.
    WebCore::IntRect bounds;
    uint64_t area = 0;
    for (const auto &rect : rectangles) {
        bounds.unite(rect);
        area += uint64_t(rect.width()) * rect.height();
    }
    bool merge = rectangles.size() > 1 &&
                 coalesceDamage(rectangles.size(), area, bounds.size().unclampedArea());
    auto read = [&](const WebCore::IntRect &rect) {
        if (rect.isEmpty()) return true;
        auto info = SkImageInfo::Make(rect.width(), rect.height(), kBGRA_8888_SkColorType,
                                      kPremul_SkAlphaType);
        return m_surface->readPixels(info, pixels + size_t(rect.y()) * stride + rect.x(),
                                     stride * 4, rect.x(), rect.y());
    };
    bool copied = true;
    if (merge) copied = read(bounds);
    else {
        for (const auto &rect : rectangles) copied = copied && read(rect);
    }
    // Reading pixels submits the drawing; without a readback, submit it here.
    if (!area) m_skia->flushAndSubmit();
    if (!copied) {
        lose();
        return false;
    }
    return check(started);
}

// Device failures show at once; a context the host disabled shows only in the
// probe, every probeIntervalUs.
bool GPURaster::check(uint64_t now) {
    if (now - m_probedAt >= probeIntervalUs) {
        m_probedAt = now;
        if (!probe()) {
            lose();
            return false;
        }
    } else if (sos2_drm_failed()) {
        lose();
        return false;
    }
    return true;
}

bool GPURaster::present(std::span<const WebCore::IntRect> rectangles) {
    if (m_lost || !m_resource) return false;
    auto started = microseconds();
    // Skia calls glFlush only when it must; Mesa then keeps the frame in its
    // command buffer, and the compositor would copy the previous frame.
    m_skia->flushAndSubmit();
    m_context->makeContextCurrent();
    glFlush();
    user_gpu_present frame{uint32_t(m_resource), 0, {}};
    WebCore::IntRect bounds;
    for (const auto &rect : rectangles) bounds.unite(rect);
    auto add = [&](const WebCore::IntRect &rect) {
        if (!rect.isEmpty())
            frame.damage[frame.count++] = {unsigned(rect.x()), unsigned(rect.y()),
                                           unsigned(rect.width()), unsigned(rect.height())};
    };
    if (rectangles.size() > USER_FRAME_RECTS) add(bounds);
    else {
        for (const auto &rect : rectangles) add(rect);
    }
    int result =
        frame.count ? sos2_gpu(USER_GPU_PRESENT, uint32_t(uintptr_t(&frame)), sizeof(frame)) : 0;
    if (result < 0) {
        lose();
        return false;
    }
    return check(started);
}

}
