#pragma once
// GPU layer compositing with WebKit's own compositor. When the page paints on
// the GPU (web_gpu.h), WebCore's composited layers are WebKit's coordinated
// layers: WebCore records each layer's painting, Skia replays the recordings
// into GPU tiles, and WebKit's Skia compositor draws the layer tree into the
// page's GPU surface. The compositor runs transform, opacity and filter
// animations itself, so animation frames need no WebCore rendering update,
// and it draws masks, filters, blend modes and 3D transforms, which the CPU
// compositor flattens.
//
// WPE commits layer state to a compositing thread and paints tiles on worker
// threads. Here the page thread does it all: tiles are painted where WebCore
// records them (SkiaPaintingEngine without a worker pool, see the
// gpu-compositing WebKit patch), and the page loop commits and composites
// right after WebCore's layer flush, before the frame is presented.
#include "web_compositor.h"
#include <WebCore/CoordinatedPlatformLayer.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/RunLoop.h>
#include <memory>

namespace WebCore {
class CoordinatedImageBackingStore;
class NativeImage;
class SkiaPaintingEngine;
}

namespace SOS2 {

class GPUCompositor final : public Compositor, public WebCore::CoordinatedPlatformLayer::Client {
public:
    // Composites with WebKit's shared Skia context, which GPURaster sets up.
    // Null without it.
    static std::unique_ptr<GPUCompositor> create(unsigned width, unsigned height);
    ~GPUCompositor();

    WebCore::GraphicsLayerFactory &factory() final { return m_factory; }

    void setRoot(WebCore::GraphicsLayer *) final;

    bool needsComposite() const final { return !m_lost && (m_changed || m_animating); }

    bool compositesWithoutUpdate() const final { return !m_lost && m_animating; }

    void redrawAll() final { m_changed = true; }

    void commit() final;
    // Draws the whole tree: there is no damage tracking yet.
    std::optional<WebCore::Region> composite(SkCanvas &) final;
    // The GPU is lost: draw nothing more. web_main.cpp then turns layer
    // compositing off, so WebCore drops the layers and paints the view directly.
    void lose();

private:
    GPUCompositor(unsigned width, unsigned height, int maxTextureSize);
    void releaseDetached();

    // CoordinatedPlatformLayer::Client, all called on the page thread.
    void attachLayer(WebCore::CoordinatedPlatformLayer &) final;
    void detachLayer(WebCore::CoordinatedPlatformLayer &) final;

    WebCore::SkiaPaintingEngine &paintingEngine() const final { return *m_paintingEngine; }

    Ref<WebCore::CoordinatedImageBackingStore>
    imageBackingStore(Ref<WebCore::NativeImage> &&) final;

    void notifyCompositionRequired() final { m_changed = true; }

    bool isCompositionRequiredOrOngoing() const final { return m_changed; }

    void requestComposition(WebCore::CompositionReason) final { m_changed = true; }

    // Only video buffer proxies use it, to wait for the compositing thread.
    RunLoop *compositingRunLoop() const final { return nullptr; }

    int maxTextureSize() const final { return m_maxTextureSize; }

    // Tiles are painted before the call that requests them returns, so a
    // composite never waits for one.
    void willPaintTile() final { }

    void didPaintTile() final { }

    class Factory final : public WebCore::GraphicsLayerFactory {
    public:
        explicit Factory(GPUCompositor &compositor) : m_compositor(compositor) { }

        Ref<WebCore::GraphicsLayer> createGraphicsLayer(WebCore::GraphicsLayerType,
                                                        WebCore::GraphicsLayerClient &) final;

    private:
        GPUCompositor &m_compositor;
    };

    Factory m_factory{*this};
    WebCore::IntSize m_viewport;
    int m_maxTextureSize;
    // The scene's root: WebCore's root layer is its only child.
    Ref<WebCore::CoordinatedPlatformLayer> m_sceneRoot;
    HashSet<Ref<WebCore::CoordinatedPlatformLayer>> m_layers;
    // Detached since the last commit or composite, which drops their
    // compositor state; lost or out of compositing, it goes at once.
    HashSet<Ref<WebCore::CoordinatedPlatformLayer>> m_detached;
    std::unique_ptr<WebCore::SkiaPaintingEngine> m_paintingEngine;
    // Directly composited images, by NativeImage id; dropped once only the
    // cache holds them.
    HashMap<uint64_t, Ref<WebCore::CoordinatedImageBackingStore>> m_imageBackingStores;
    bool m_changed{true};    // layer state changed since the last composite
    bool m_animating{false}; // the last composite ran animations
    bool m_lost{false};
};

}
