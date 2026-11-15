#include "config.h"
#include "web_gpu_compositor.h"

#include <WebCore/CoordinatedImageBackingStore.h>
#include <WebCore/Damage.h>
#include <WebCore/GraphicsLayerCoordinated.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/SkiaCompositingLayer.h>
#include <WebCore/SkiaPaintingEngine.h>
#include <skia/core/SkCanvas.h>
#include <skia/gpu/ganesh/GrDirectContext.h>

namespace SOS2 {
using namespace WebCore;

std::unique_ptr<GPUCompositor> GPUCompositor::create(unsigned width, unsigned height) {
    auto *display = PlatformDisplay::sharedDisplayIfExists();
    if (!display || !display->skiaGLContext()) return nullptr;
    auto *skia = display->skiaGrContext();
    if (!skia || skia->abandoned()) return nullptr;
    return std::unique_ptr<GPUCompositor>(new GPUCompositor(width, height, skia->maxTextureSize()));
}

GPUCompositor::GPUCompositor(unsigned width, unsigned height, int maxTextureSize)
    : m_viewport(width, height), m_maxTextureSize(maxTextureSize),
      m_sceneRoot(CoordinatedPlatformLayer::create())
      // Tiles are recorded as deferred display lists for the shared context and
      // drawn into tile textures when composited.
      ,
      m_paintingEngine(SkiaPaintingEngine::create(
          PlatformDisplay::sharedDisplay().skiaGrContext()->threadSafeProxy())) {
    Locker locker{m_sceneRoot->lock()};
    m_sceneRoot->setAnchorPoint(FloatPoint3D(0, 0, 0));
    m_sceneRoot->setSize(FloatSize(width, height));
}

GPUCompositor::~GPUCompositor() {
    // As WPE's scene state does: layers outliving the compositor lose their client.
    for (auto &layer : std::exchange(m_layers, {})) layer->invalidateClient();
}

Ref<GraphicsLayer> GPUCompositor::Factory::createGraphicsLayer(GraphicsLayerType type,
                                                               GraphicsLayerClient &client) {
    return adoptRef(*new GraphicsLayerCoordinated(type, client,
                                                  CoordinatedPlatformLayer::create(m_compositor)));
}

void GPUCompositor::setRoot(GraphicsLayer *root) {
    m_root = root;
    Vector<Ref<CoordinatedPlatformLayer>> children;
    if (root) children.append(downcast<GraphicsLayerCoordinated>(*root).coordinatedPlatformLayer());
    {
        Locker locker{m_sceneRoot->lock()};
        m_sceneRoot->setChildren(WTF::move(children));
    }
    m_changed = true;
    // Leaving compositing: no commit or composite comes to drop the old tree.
    if (!root) releaseDetached();
}

void GPUCompositor::attachLayer(CoordinatedPlatformLayer &layer) {
    m_layers.add(layer);
    m_detached.remove(layer);
    m_changed = true;
}

void GPUCompositor::detachLayer(CoordinatedPlatformLayer &layer) {
    m_layers.remove(layer);
    m_detached.add(layer);
    m_changed = true;
    // Lost or out of compositing, no commit or composite comes: drop it now.
    if (m_lost || !active()) releaseDetached();
}

// As WPE's CoordinatedSceneState::flushCompositingState: detached layers drop
// their tiles and compositor layers, which leave the compositor tree. Images
// go once only this cache holds them.
void GPUCompositor::releaseDetached() {
    for (auto &layer : std::exchange(m_detached, {})) layer->invalidateTarget();
    m_imageBackingStores.removeIf([](auto &entry) { return entry.value->hasOneRef(); });
}

Ref<CoordinatedImageBackingStore> GPUCompositor::imageBackingStore(Ref<NativeImage> &&image) {
    auto id = image->uniqueID();
    return m_imageBackingStores
        .ensure(id,
                [&] {
                    return CoordinatedImageBackingStore::create(
                        WTF::move(image),
                        PlatformDisplay::sharedDisplay().skiaGrContext()->threadSafeProxy());
                })
        .iterator->value;
}

// WPE's CoordinatedSceneState::flush, after WebCore's layer flush.
void GPUCompositor::commit() {
    // Scroll positions and bounds origins reach the layers' committed state.
    for (auto &layer : copyToVector(m_layers)) layer->flushPendingState();
    // Now, even when the page is hidden and no composite follows.
    releaseDetached();
}

std::optional<Region> GPUCompositor::composite(SkCanvas &canvas) {
    if (m_lost) return std::nullopt;
    // A rendering update applies the layers' changes and painted tiles; a frame
    // that only runs animations just draws again (as ThreadedCompositor does).
    if (std::exchange(m_changed, false)) {
        OptionSet<CompositionReason> reasons{CompositionReason::RenderingUpdate};
        // WPE's CoordinatedSceneState::flushCompositingState, on this thread.
        releaseDetached();
        auto layers = copyToVector(m_layers);
        m_sceneRoot->flushPositionChanges(reasons, true);
        for (auto &layer : layers) layer->flushPositionChanges(reasons, true);
        m_sceneRoot->flushCompositingState(reasons, true);
        for (auto &layer : layers) layer->flushCompositingState(reasons, true);
        for (auto &layer : layers) {
            if (layer->hasPendingBackingStoreTileUpdates())
                layer->processPendingBackingStoreTileUpdates();
        }
    }

    auto &root = m_sceneRoot->ensureSkiaTarget();
    root.setTransform(TransformationMatrix());
    // No damage tracking: the whole surface is drawn, on white like the view.
    std::optional<Damage> frameDamage;
    canvas.save();
    m_animating = root.paint(canvas, frameDamage, std::nullopt, SK_ColorWHITE);
    canvas.restore();
    return Region(IntRect(IntPoint(), m_viewport));
}

void GPUCompositor::lose() {
    m_lost = true;
    m_animating = false;
    // The context is abandoned: dropping compositor state makes no GL calls.
    // Leaving compositing drops the detached layers.
    setRoot(nullptr);
    m_imageBackingStores.clear();
}

}
