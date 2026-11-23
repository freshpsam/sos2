#include "config.h"
#include "web_compositor.h"

#include "web_clock.h"
#include "web_scrolling.h"
#include "MediaPlayerPrivateSOS2.h"
#include <WebCore/FloatQuad.h>
#include <WebCore/FloatRoundedRect.h>
#include <WebCore/FontRenderOptions.h>
#include <WebCore/GraphicsContextSkia.h>
#include <WebCore/GraphicsLayerClient.h>
#include <WebCore/HTMLVideoElement.h>
#include <WebCore/LayoutRect.h>
#include <WebCore/MediaPlayer.h>
#include <WebCore/RenderLayer.h>
#include <WebCore/RenderLayerBacking.h>
#include <WebCore/RenderVideo.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkM44.h>
#include <skia/core/SkRRect.h>
#include <skia/core/SkSamplingOptions.h>
#include <cmath>

namespace SOS2 {
using namespace WebCore;

namespace {
// Every backing is RAM the page keeps; beyond this the layers are painted
// flattened. A backing the page cannot get lowers the budget for a while.
constexpr uint64_t backingBudgetBytes = 24 * 1024 * 1024;
constexpr uint64_t budgetRetryUs = 5000000;
constexpr uint64_t refineDelayUs = 150000;
}

std::atomic<unsigned> presentationsMade;

std::recursive_mutex &surfaceLock() {
    // Never destroyed: the video presenter may wait on it as the process ends.
    static auto *lock = new std::recursive_mutex;
    return *lock;
}

Ref<GraphicsLayer> LayerCompositor::Factory::createGraphicsLayer(GraphicsLayerType type,
                                                                 GraphicsLayerClient &client) {
    return adoptRef(*new CompositingLayer(type, client, m_compositor));
}

CompositingLayer::CompositingLayer(Type type, GraphicsLayerClient &client,
                                   LayerCompositor &compositor)
    : GraphicsLayer(type, client), m_compositor(compositor) { }

CompositingLayer::~CompositingLayer() {
    willBeDestroyed();
    if (m_compositor.m_presentedVideo == this) m_compositor.invalidatePresentation();
    m_compositor.m_videoLayers.remove(this);
    m_compositor.releaseBacking(*this);
    m_compositor.addDamage(m_drawnBounds);
    m_compositor.changed();
}

void CompositingLayer::noteChange() {
    m_changed = true;
    m_compositor.changed();
    // During a flush (e.g. an invalidation raised while painting), ask for the
    // next one instead; requesting the current flush again asserts.
    if (client().isFlushingLayers()) client().notifySubsequentFlushRequired(this);
    else client().notifyFlushRequired(this);
}

void CompositingLayer::setNeedsDisplay() {
    if (!drawsContent()) return;
    m_dirty = Region(IntRect(IntPoint(), expandedIntSize(size())));
    noteChange();
}

void CompositingLayer::setNeedsDisplayInRect(const FloatRect &rect, ShouldClipToLayer clip) {
    if (!drawsContent()) return;
    auto dirty = enclosingIntRect(rect);
    if (clip == ShouldClipToLayer::Clip)
        dirty.intersect(IntRect(IntPoint(), expandedIntSize(size())));
    if (dirty.isEmpty()) return;
    m_dirty.unite(Region(dirty));
    if (m_dirty.gridSize() > 64) m_dirty = Region(m_dirty.bounds());
    noteChange();
}

bool CompositingLayer::setChildren(Vector<Ref<GraphicsLayer>> &&children) {
    // WebCore re-sets unchanged child lists on every compositing update.
    bool changed = GraphicsLayer::setChildren(WTF::move(children));
    if (changed) noteChange();
    return changed;
}

void CompositingLayer::addChild(Ref<GraphicsLayer> &&child) {
    GraphicsLayer::addChild(WTF::move(child));
    noteChange();
}

void CompositingLayer::addChildAtIndex(Ref<GraphicsLayer> &&child, int index) {
    GraphicsLayer::addChildAtIndex(WTF::move(child), index);
    noteChange();
}

void CompositingLayer::addChildAbove(Ref<GraphicsLayer> &&child, GraphicsLayer *sibling) {
    GraphicsLayer::addChildAbove(WTF::move(child), sibling);
    noteChange();
}

void CompositingLayer::addChildBelow(Ref<GraphicsLayer> &&child, GraphicsLayer *sibling) {
    GraphicsLayer::addChildBelow(WTF::move(child), sibling);
    noteChange();
}

bool CompositingLayer::replaceChild(GraphicsLayer *oldChild, Ref<GraphicsLayer> &&newChild) {
    bool replaced = GraphicsLayer::replaceChild(oldChild, WTF::move(newChild));
    if (replaced) noteChange();
    return replaced;
}

void CompositingLayer::willModifyChildren() { noteChange(); }

void CompositingLayer::setMaskLayer(RefPtr<GraphicsLayer> &&layer) {
    GraphicsLayer::setMaskLayer(WTF::move(layer));
    noteChange();
}

void CompositingLayer::setReplicatedByLayer(RefPtr<GraphicsLayer> &&layer) {
    GraphicsLayer::setReplicatedByLayer(WTF::move(layer));
    noteChange();
}

#define SOS2_LAYER_SETTER(name, type, getter) \
    void CompositingLayer::name(type value) { \
        if (value == getter()) return; \
        GraphicsLayer::name(value); \
        noteChange(); \
    }
SOS2_LAYER_SETTER(setPosition, const FloatPoint &, position)
SOS2_LAYER_SETTER(syncPosition, const FloatPoint &, position)
SOS2_LAYER_SETTER(setAnchorPoint, const FloatPoint3D &, anchorPoint)
SOS2_LAYER_SETTER(setBoundsOrigin, const FloatPoint &, boundsOrigin)
SOS2_LAYER_SETTER(syncBoundsOrigin, const FloatPoint &, boundsOrigin)
SOS2_LAYER_SETTER(setTransform, const TransformationMatrix &, transform)
SOS2_LAYER_SETTER(setChildrenTransform, const TransformationMatrix &, childrenTransform)
SOS2_LAYER_SETTER(setPreserves3D, bool, preserves3D)
SOS2_LAYER_SETTER(setMasksToBounds, bool, masksToBounds)
SOS2_LAYER_SETTER(setContentsVisible, bool, contentsAreVisible)
SOS2_LAYER_SETTER(setBackfaceVisibility, bool, backfaceVisibility)
SOS2_LAYER_SETTER(setOpacity, float, opacity)
SOS2_LAYER_SETTER(setBlendMode, BlendMode, blendMode)
SOS2_LAYER_SETTER(setBackgroundColor, const Color &, backgroundColor)
SOS2_LAYER_SETTER(setContentsRect, const FloatRect &, contentsRect)
SOS2_LAYER_SETTER(setContentsClippingRect, const FloatRoundedRect &, contentsClippingRect)
SOS2_LAYER_SETTER(setContentsRectClipsDescendants, bool, contentsRectClipsDescendants)
#undef SOS2_LAYER_SETTER

void CompositingLayer::setSize(const FloatSize &value) {
    if (value == size()) return;
    GraphicsLayer::setSize(value);
    if (drawsContent()) setNeedsDisplay();
    noteChange();
}

void CompositingLayer::setDrawsContent(bool value) {
    if (value == drawsContent()) return;
    GraphicsLayer::setDrawsContent(value);
    if (value) setNeedsDisplay();
    else {
        m_compositor.releaseBacking(*this);
        m_dirty = Region();
    }
    noteChange();
}

void CompositingLayer::setContentsOpaque(bool value) {
    if (value == contentsOpaque()) return;
    GraphicsLayer::setContentsOpaque(value);
    // The backing's alpha type follows this flag; allocate it again.
    m_compositor.releaseBacking(*this);
    setNeedsDisplay();
    noteChange();
}

void CompositingLayer::setContentsToSolidColor(const Color &color) {
    if (color == m_solidColor) return;
    m_solidColor = color;
    noteChange();
}

void CompositingLayer::setContentsToPlatformLayer(PlatformLayer *, ContentsLayerPurpose purpose) {
    bool video = purpose == ContentsLayerPurpose::Media;
    if (video == m_showsVideo) return;
    m_showsVideo = video;
    m_videoFrame = nullptr;
    if (video) m_compositor.m_videoLayers.add(this);
    else m_compositor.m_videoLayers.remove(this);
    noteChange();
}

// Only RenderLayerBacking gives a layer a video, and a video showing its
// poster paints it instead.
RefPtr<NativeImage> CompositingLayer::currentVideoFrame() const {
    if (!m_showsVideo) return nullptr;
    auto &backing = static_cast<RenderLayerBacking &>(client());
    auto *video = dynamicDowncast<RenderVideo>(backing.owningLayer().renderer());
    if (!video || !video->shouldDisplayVideo()) return nullptr;
    RefPtr player = video->videoElement().player();
    return player ? player->nativeImageForCurrentTime() : nullptr;
}

// The renderer behind a video's frames: SOS2's media engine, the only one,
// made the frame the layer shows.
RefPtr<MediaRendererSOS2> CompositingLayer::videoRenderer() const {
    if (!m_showsVideo || !m_videoFrame) return nullptr;
    auto &backing = static_cast<RenderLayerBacking &>(client());
    auto *video = dynamicDowncast<RenderVideo>(backing.owningLayer().renderer());
    RefPtr player = video ? video->videoElement().player() : nullptr;
    auto *engine =
        player ? dynamicDowncast<MediaPlayerPrivateSOS2>(player->playerPrivate()) : nullptr;
    return engine ? &engine->renderer() : nullptr;
}

void CompositingLayer::flushCompositingState(const FloatRect &visibleRect) {
    // WebCore flushes the root of each frame's tree. Subframe roots are children
    // of the main tree, so updating from the attached root covers every layer.
    if (m_compositor.m_root.get() == this) m_compositor.update(*this, visibleRect);
}

double CompositingLayer::backingStoreMemoryEstimate() const { return m_backing.computeByteSize(); }

LayerCompositor::LayerCompositor(unsigned width, unsigned height)
    : m_viewport(width, height), m_fullCoverPixels(2ull * width * height),
      m_budget(backingBudgetBytes) { }

// Only the main thread gives the presentation a renderer: this read needs no
// lock. (The presenter may end it on its own, when a present fails.)
void LayerCompositor::invalidatePresentation() {
    if (!m_presentation.renderer) return;
    std::lock_guard lock(surfaceLock());
    m_presentation.valid = false;
    m_presentation.renderer = nullptr;
    m_presentation.shown = nullptr;
    m_presentedVideo = nullptr;
}

void LayerCompositor::setRoot(GraphicsLayer *root) {
    m_root = root;
    m_changed = true;
    m_damage = Region(IntRect(IntPoint(), m_viewport));
    m_budget = backingBudgetBytes;
    m_budgetRetryAt = 0;
}

// New video frames come from the player, not from WebCore's layer changes;
// videos outside the tree (a hidden frame's) show none, and one the video
// presenter already put in the surface needs no composite.
bool LayerCompositor::needsComposite() const {
    if (m_changed) return true;
    std::lock_guard lock(surfaceLock());
    for (auto *layer : m_videoLayers) {
        if (!layer->m_reached) continue;
        auto frame = layer->currentVideoFrame();
        if (frame != layer->m_videoFrame &&
            !(m_presentation.valid && frame == m_presentation.shown))
            return true;
    }
    return false;
}

// A new frame: its contents rectangle is drawn again, unless the video
// presenter already drew it there. The layer keeps the frame it drew, so a
// new one can never reuse its address. Under the lock, the presenter shows no
// newer frame meanwhile, which drawing this one would then cover.
bool LayerCompositor::takeVideoFrame(CompositingLayer &layer) {
    std::lock_guard lock(surfaceLock());
    auto frame = layer.currentVideoFrame();
    if (frame == layer.m_videoFrame) return false;
    bool presented = m_presentation.valid && frame == m_presentation.shown;
    layer.m_videoFrame = WTF::move(frame);
    if (presented) return false;
    addDamage(layer.m_videoBounds, &layer == m_presentedVideo);
    return true;
}

void LayerCompositor::addDamage(const FloatRect &rect, bool presentedFrame) {
    auto damage = intersection(enclosingIntRect(rect), IntRect(IntPoint(), m_viewport));
    if (damage.isEmpty()) return;
    // Only the main thread gives the presentation a renderer and a rectangle.
    if (!presentedFrame && m_presentation.renderer && damage.intersects(m_presentation.rect))
        m_presentationStale = true;
    m_damage.unite(Region(damage));
    if (m_damage.gridSize() > 32) m_damage = Region(m_damage.bounds());
}

void LayerCompositor::releaseBacking(CompositingLayer &layer) {
    if (layer.m_backing.isNull()) return;
    m_backingBytes -= layer.m_backing.computeByteSize();
    layer.m_backing.reset();
    layer.m_cover = {};
}

// First the layers' places and covers, then their backings: only once all
// covers fit the budget is any painted.
void LayerCompositor::update(CompositingLayer &root, const FloatRect &viewport) {
    uint64_t now = microseconds();
    bool refine = refinementDue(now);
    if (refine) m_refineAfter = 0;
    if (m_budgetRetryAt && now >= m_budgetRetryAt) {
        m_budget = backingBudgetBytes;
        m_budgetRetryAt = 0;
    }
    m_unsupported = false;
    m_wantedBytes = 0;
    for (auto *layer : m_videoLayers) layer->m_reached = false;
    updateLayer(root, TransformationMatrix(), viewport);
    // Without a feature or the memory to draw the layers, the caller paints
    // them flattened, where they changed: no layer keeps a backing, and what
    // WebCore repaints becomes damage. Switching either way redraws all once.
    bool flatten = m_unsupported || m_wantedBytes > m_budget;
    if (flatten != m_flatten) {
        m_flatten = flatten;
        m_damage = Region(IntRect(IntPoint(), m_viewport));
        m_changed = true;
    }
    updateBackings(root, refine);
}

// Transforms follow WebCore's Skia compositor
// (SkiaCompositingLayer::computeTransformsAndAnimations).
void LayerCompositor::updateLayer(CompositingLayer &layer,
                                  const TransformationMatrix &parentForChildren,
                                  const FloatRect &clip) {
    if (layer.maskLayer() || layer.replicaLayer() || layer.blendMode() != BlendMode::Normal)
        m_unsupported = true;

    auto &size = layer.size();
    auto &anchor = layer.anchorPoint();
    FloatPoint origin(anchor.x() * size.width(), anchor.y() * size.height());
    TransformationMatrix combined = parentForChildren;
    combined.translate3d(origin.x() + layer.position().x() - layer.boundsOrigin().x(),
                         origin.y() + layer.position().y() - layer.boundsOrigin().y(), anchor.z());
    combined.multiply(layer.transform());
    TransformationMatrix forChildren = combined;
    combined.translate3d(-origin.x(), -origin.y(), -anchor.z());
    if (!layer.preserves3D()) forChildren.flatten();
    forChildren.multiply(layer.childrenTransform());
    forChildren.translate3d(-origin.x(), -origin.y(), -anchor.z());
    layer.m_combined = combined;

    FloatRect bounds(FloatPoint(), size);
    // Small layers keep all their pixels while any shows, so movement never
    // repaints them. Larger ones keep only what the viewport shows.
    IntRect cover;
    if (layer.drawsContent() && !intersection(combined.mapRect(bounds), clip).isEmpty()) {
        IntRect whole(IntPoint(), expandedIntSize(size));
        if (uint64_t(whole.width()) * whole.height() <= m_fullCoverPixels) cover = whole;
        else if (auto inverse = combined.inverse())
            cover = intersection(
                enclosingIntRect(inverse->clampedBoundsOfProjectedQuad(FloatQuad(clip))), whole);
    }
    layer.m_wantedCover = cover;
    m_wantedBytes += uint64_t(cover.width()) * cover.height() * 4;
    if (layer.m_showsVideo) {
        layer.m_reached = true;
        layer.m_videoBounds = intersection(combined.mapRect(layer.contentsRect()), clip);
        if (takeVideoFrame(layer)) m_changed = true;
    }

    FloatRect childClip = clip;
    if (layer.masksToBounds() && !layer.preserves3D()) {
        TransformationMatrix clipTransform = combined;
        clipTransform.translate(layer.boundsOrigin().x(), layer.boundsOrigin().y());
        childClip.intersect(clipTransform.mapRect(bounds));
    }
    layer.m_subtreeBounds = intersection(combined.mapRect(bounds), clip);
    for (auto &child : layer.children()) {
        auto &childLayer = static_cast<CompositingLayer &>(child.get());
        updateLayer(childLayer, forChildren, childClip);
        layer.m_subtreeBounds.unite(childLayer.m_subtreeBounds);
    }
    // A change can move, reveal or hide anything in the subtree: redraw where
    // it was and where it is now.
    if (layer.m_changed) {
        addDamage(layer.m_drawnBounds);
        addDamage(layer.m_subtreeBounds);
        layer.m_changed = false;
    }
    layer.m_drawnBounds = layer.m_subtreeBounds;
}

void LayerCompositor::updateBackings(CompositingLayer &layer, bool refine) {
    IntRect cover = m_flatten ? IntRect() : layer.m_wantedCover;
    if (cover.isEmpty()) {
        releaseBacking(layer);
        // Flattened, what WebCore repainted is the caller's to paint. A layer
        // out of view paints whole once it shows again.
        if (m_flatten)
            for (auto &rect : layer.m_dirty.rects())
                addDamage(layer.m_combined.mapRect(FloatRect(rect)));
        layer.m_dirty = Region();
    } else if ((cover == layer.m_cover && !layer.m_backing.isNull()) || moveCover(layer, cover)) {
        if (refine && layer.m_coverMoved) {
            layer.m_dirty = Region(layer.m_cover);
            layer.m_coverMoved = false;
        }
        paintDirty(layer);
    }
    for (auto &child : layer.children())
        updateBackings(static_cast<CompositingLayer &>(child.get()), refine);
}

bool LayerCompositor::moveCover(CompositingLayer &layer, const IntRect &cover) {
    IntRect old = layer.m_backing.isNull() ? IntRect() : layer.m_cover;
    IntRect kept = intersection(old, cover);
    if (!layer.m_backing.isNull() && old.size() == cover.size()) {
        // Scrolling: keep the bitmap and shift the pixels that stay in view.
        if (!kept.isEmpty()) {
            IntSize delta = old.location() - cover.location();
            scrollPixels(static_cast<uint32_t *>(layer.m_backing.getPixels()), cover.width(),
                         cover.height(), {0, 0, unsigned(cover.width()), unsigned(cover.height())},
                         delta.width(), delta.height());
        }
    } else {
        uint64_t bytes = uint64_t(cover.width()) * cover.height() * 4;
        SkBitmap backing;
        auto info =
            SkImageInfo::Make(cover.width(), cover.height(), kBGRA_8888_SkColorType,
                              layer.contentsOpaque() ? kOpaque_SkAlphaType : kPremul_SkAlphaType);
        if (!backing.tryAllocPixels(info)) {
            // The page has no more memory: what it holds becomes its budget
            // until it may have freed some, and the caller paints the layers
            // flattened meanwhile.
            releaseBacking(layer);
            m_budget = std::min(m_budget, m_backingBytes);
            m_budgetRetryAt = microseconds() + budgetRetryUs;
            m_flatten = true;
            m_damage = Region(IntRect(IntPoint(), m_viewport));
            m_changed = true;
            return false;
        }
        if (!kept.isEmpty())
            layer.m_backing.readPixels(backing.info().makeWH(kept.width(), kept.height()),
                                       backing.getAddr(kept.x() - cover.x(), kept.y() - cover.y()),
                                       backing.rowBytes(), kept.x() - old.x(), kept.y() - old.y());
        releaseBacking(layer);
        layer.m_backing = WTF::move(backing);
        m_backingBytes += bytes;
    }
    layer.m_cover = cover;
    Region exposed(cover);
    exposed.subtract(Region(kept));
    if (!old.isEmpty() && !kept.isEmpty() && !exposed.isEmpty()) {
        layer.m_coverMoved = true;
        m_refineAfter = microseconds() + refineDelayUs;
    }
    layer.m_dirty.unite(exposed);
    return true;
}

void LayerCompositor::paintDirty(CompositingLayer &layer) {
    Region dirty = layer.m_dirty;
    dirty.intersect(Region(layer.m_cover));
    layer.m_dirty = Region();
    if (dirty.isEmpty()) return;
    // As in the direct path: paint sparse damage rect by rect, dense damage at once.
    auto rects = dirty.rects();
    auto extent = dirty.bounds();
    if (coalesceDamage(rects.size(), dirty.totalArea(), extent.size().unclampedArea()))
        rects = {extent};
    // WebKit's text contrast and gamma, as on the page surface.
    auto props = FontRenderOptions::singleton().createSurfaceProps();
    auto canvas = SkCanvas::MakeRasterDirect(layer.m_backing.info(), layer.m_backing.getPixels(),
                                             layer.m_backing.rowBytes(), &props);
    if (!canvas) return;
    for (auto &rect : rects) {
        canvas->save();
        canvas->translate(-layer.m_cover.x(), -layer.m_cover.y());
        canvas->clipRect(SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()));
        if (!layer.contentsOpaque()) canvas->clear(SK_ColorTRANSPARENT);
        {
            GraphicsContextSkia context(*canvas, RenderingMode::Unaccelerated,
                                        RenderingPurpose::LayerBacking);
            layer.paintGraphicsLayerContents(context, rect);
        }
        canvas->restore();
        addDamage(layer.m_combined.mapRect(FloatRect(rect)));
    }
    m_changed = true;
}

Composition LayerCompositor::composite(SkCanvas &canvas) {
    // Frames that came without a rendering update.
    for (auto *layer : m_videoLayers)
        if (layer->m_reached) takeVideoFrame(*layer);
    m_changed = false;
    Composition composition{std::exchange(m_damage, Region()), m_flatten || !m_root};
    auto &damage = composition.damage;
    if (damage.isEmpty()) {
        updatePresentation();
        return composition;
    }
    // Sparse damage goes rectangle by rectangle, dense damage as its bounds.
    auto rects = damage.rects();
    auto extent = damage.bounds();
    if (coalesceDamage(rects.size(), damage.totalArea(), extent.size().unclampedArea())) {
        damage = Region(extent);
        rects = {extent};
    }
    if (composition.flatten) {
        invalidatePresentation();
        return composition;
    }
    // Draw only the damage: the rest of the surface already shows these layers.
    auto &root = static_cast<CompositingLayer &>(*m_root);
    for (auto &rect : rects) {
        canvas.save();
        canvas.resetMatrix();
        canvas.clipRect(SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()));
        // Under a layer that hides the whole rectangle (a playing video),
        // nothing is drawn, not even the white.
        Vector<CompositingLayer *> path;
        m_occluder.clear();
        findOccluder(root, rect, path);
        m_occluded = !m_occluder.isEmpty();
        if (!m_occluded) canvas.clear(SK_ColorWHITE);
        drawLayer(canvas, root, rect);
        canvas.restore();
        m_occluded = false;
    }
    updatePresentation();
    return composition;
}

// After a composite. Unless it drew more than the video's frames into the
// presented rectangle, the overlay holds, as long as frames keep their size.
void LayerCompositor::updatePresentation() {
    std::lock_guard lock(surfaceLock());
    auto &p = m_presentation;
    auto *video = m_presentedVideo;
    auto image = video && video->m_videoFrame ? video->m_videoFrame->platformImage() : nullptr;
    if (p.valid && !m_presentationStale && video->m_reached && image &&
        IntSize(image->width(), image->height()) == p.frameSize &&
        video->videoRenderer() == p.renderer)
        p.shown = video->m_videoFrame;
    else preparePresentation();
    m_presentationStale = false;
}

// The topmost layer that draws all of `damage` opaquely, with no group
// opacity, 3D or clip short of the damage above it. Its path from the root
// goes to m_occluder.
void LayerCompositor::findOccluder(CompositingLayer &layer, const IntRect &damage,
                                   Vector<CompositingLayer *> &path) {
    // drawLayer skips at least what this skips, so it reaches the occluder.
    bool contentsVisible = layer.contentsAreVisible();
    if ((!contentsVisible && layer.children().isEmpty()) ||
        layer.renderingIsSuppressedIncludingDescendants() || layer.opacity() < 1 ||
        layer.preserves3D() || !layer.m_subtreeBounds.intersects(damage))
        return;
    path.append(&layer);
    if (contentsVisible && coversOpaquely(layer, damage)) m_occluder = path;
    // As drawLayer clips the children.
    auto clipping = layer.contentsClippingRect();
    bool clipsToContents =
        layer.contentsRectClipsDescendants() &&
        (clipping.isRounded() || !clipping.rect().contains(layer.contentsRect()));
    bool clipped = false;
    if (clipsToContents || layer.masksToBounds()) {
        FloatRect clip = clipsToContents
                             ? clipping.rect()
                             : FloatRect(FloatPoint(layer.boundsOrigin()), layer.size());
        clipped = (clipsToContents && clipping.isRounded()) ||
                  !layer.m_combined.isIdentityOrTranslation() ||
                  !layer.m_combined.mapRect(clip).contains(damage);
    }
    if (!clipped)
        for (auto &child : layer.children())
            findOccluder(static_cast<CompositingLayer &>(child.get()), damage, path);
    path.removeLast();
}

bool LayerCompositor::coversOpaquely(const CompositingLayer &layer, const IntRect &damage) const {
    auto &matrix = layer.m_combined;
    if (!matrix.isIdentityOrTranslation() || matrix.m43()) return false;
    FloatSize offset(std::round(matrix.m41()), std::round(matrix.m42()));
    auto image = layer.m_videoFrame ? layer.m_videoFrame->platformImage() : nullptr;
    auto clipping = layer.contentsClippingRect();
    if (image && image->isOpaque() && !clipping.isRounded()) {
        auto shown = intersection(layer.contentsRect(), clipping.rect());
        shown.move(offset);
        if (shown.contains(damage)) return true;
    }
    if (layer.contentsOpaque() && !layer.m_backing.isNull()) {
        FloatRect shown(layer.m_cover);
        shown.move(offset);
        if (shown.contains(damage)) return true;
    }
    return false;
}

// Layers that are only translated draw at whole pixels: exact copies, and text
// stays sharp while it slides. Other transforms are filtered.
static SkM44 drawingMatrix(const TransformationMatrix &matrix, bool &exact) {
    exact = matrix.isIdentityOrTranslation() && !matrix.m43();
    if (!exact) return SkM44(matrix);
    return SkM44::Translate(std::round(matrix.m41()), std::round(matrix.m42()));
}

// Paint order and clipping follow SkiaCompositingLayer::recursivePaint. The
// canvas matrix is identity between layers; 3D transforms are drawn flattened.
void LayerCompositor::drawLayer(SkCanvas &canvas, CompositingLayer &layer, const IntRect &damage) {
    // Hidden contents (visibility: hidden) still draw their children, which
    // may be visible (SkiaCompositingLayer::isVisible). Subtrees away from
    // the damage draw nothing there.
    bool contentsVisible = layer.contentsAreVisible();
    if ((!contentsVisible && layer.children().isEmpty()) ||
        layer.renderingIsSuppressedIncludingDescendants() || layer.opacity() <= 0 ||
        !layer.m_subtreeBounds.intersects(damage))
        return;
    // Before the occluder, only its path is walked, for the clips.
    if (m_occluded) {
        if (!m_occluder.contains(&layer)) return;
        if (&layer == m_occluder.last()) m_occluded = false;
        else contentsVisible = false;
    }
    float alpha = layer.opacity();
    bool group = alpha < 1 && !layer.children().isEmpty();
    int saved = canvas.getSaveCount();
    if (group) {
        SkRect bounds = layer.m_subtreeBounds;
        canvas.saveLayerAlphaf(&bounds, alpha);
    }
    if (contentsVisible && (layer.backfaceVisibility() || !layer.m_combined.isBackFaceVisible()))
        drawContents(canvas, layer, group ? 1 : alpha);
    if (!layer.children().isEmpty()) {
        auto clipping = layer.contentsClippingRect();
        bool clipsToContents =
            !layer.preserves3D() && layer.contentsRectClipsDescendants() &&
            (clipping.isRounded() || !clipping.rect().contains(layer.contentsRect()));
        bool masks = !layer.preserves3D() && layer.masksToBounds();
        if (clipsToContents || masks) {
            canvas.save();
            TransformationMatrix clipTransform = layer.m_combined;
            if (!clipsToContents)
                clipTransform.translate(layer.boundsOrigin().x(), layer.boundsOrigin().y());
            bool exact;
            canvas.setMatrix(drawingMatrix(clipTransform, exact));
            bool antialias = !exact;
            if (clipsToContents) canvas.clipRRect(SkRRect(clipping), antialias);
            else canvas.clipRect(SkRect(FloatRect(FloatPoint(), layer.size())), antialias);
            canvas.resetMatrix();
        }
        for (auto &child : layer.children())
            drawLayer(canvas, static_cast<CompositingLayer &>(child.get()), damage);
    }
    canvas.restoreToCount(saved);
}

void LayerCompositor::drawContents(SkCanvas &canvas, CompositingLayer &layer, float alpha) {
    bool integer;
    canvas.save();
    canvas.setMatrix(drawingMatrix(layer.m_combined, integer));
    SkPaint paint;
    paint.setAntiAlias(!integer);
    if (layer.backgroundColor().isVisible()) {
        paint.setColor(SkColor(layer.backgroundColor().colorWithAlphaMultipliedBy(alpha)));
        canvas.drawRect(SkRect(FloatRect(FloatPoint(), layer.size())), paint);
    }
    if (!layer.m_backing.isNull()) {
        SkPaint imagePaint;
        imagePaint.setAlphaf(alpha);
        imagePaint.setAntiAlias(!integer);
        auto image = SkImages::RasterFromPixmap(layer.m_backing.pixmap(), nullptr, nullptr);
        SkSamplingOptions sampling =
            integer ? SkSamplingOptions() : SkSamplingOptions(SkFilterMode::kLinear);
        canvas.drawImage(image, layer.m_cover.x(), layer.m_cover.y(), sampling, &imagePaint);
    }
    bool solidColor = layer.m_solidColor.isValid() && layer.m_solidColor.isVisible();
    if (!solidColor && !layer.m_videoFrame) {
        canvas.restore();
        return;
    }
    auto clipping = layer.contentsClippingRect();
    if (clipping.isRounded() || !clipping.rect().contains(layer.contentsRect()))
        canvas.clipRRect(SkRRect(clipping), !integer);
    if (solidColor) {
        paint.setColor(SkColor(layer.m_solidColor.colorWithAlphaMultipliedBy(alpha)));
        canvas.drawRect(SkRect(layer.contentsRect()), paint);
    }
    if (auto image = layer.m_videoFrame ? layer.m_videoFrame->platformImage() : nullptr) {
        // Frames come at the size the page shows them (MediaRendererSOS2::
        // frameSize): at whole pixels they are copied, never filtered.
        SkPaint imagePaint;
        imagePaint.setAlphaf(alpha);
        auto contents = layer.contentsRect();
        bool copied = integer && contents == FloatRect(enclosingIntRect(contents)) &&
                      contents.size() == FloatSize(image->width(), image->height());
        canvas.drawImageRect(image, SkRect::Make(image->bounds()), SkRect(contents),
                             copied ? SkSamplingOptions()
                                    : SkSamplingOptions(SkFilterMode::kLinear),
                             &imagePaint, SkCanvas::kFast_SrcRectConstraint);
    }
    canvas.restore();
}

// The playing video the presenter may take over, when the surface shows it as
// the presenter would draw it: one video, its frames opaque and unscaled at
// whole pixels, under no transform, group opacity or clip short of whole
// pixels; rounded clips only where their corners stay outside. The layers
// drawn above it within its rectangle go to the overlay. Under surfaceLock(),
// after a composite.
void LayerCompositor::preparePresentation() {
    auto &p = m_presentation;
    std::lock_guard lock(surfaceLock());
    p.valid = false;
    p.renderer = nullptr;
    p.shown = nullptr;
    p.overlaid = false;
    m_presentedVideo = nullptr;
    if (m_flatten || !m_root) return;
    CompositingLayer *video = nullptr;
    for (auto *layer : m_videoLayers) {
        if (!layer->m_reached || !layer->m_videoFrame) continue;
        if (video) return;
        video = layer;
    }
    if (!video) return;
    auto &layer = *video;
    auto image = layer.m_videoFrame->platformImage();
    auto contents = layer.contentsRect();
    if (!image || !image->isOpaque() || !layer.contentsAreVisible() || layer.opacity() < 1 ||
        layer.renderingIsSuppressedIncludingDescendants() ||
        contents.size() != FloatSize(image->width(), image->height()))
        return;
    // Where drawLayer and drawContents leave the frame, clip by clip.
    FloatRect shown{FloatPoint(), FloatSize(m_viewport)};
    auto clipTo = [&](const FloatRoundedRect &clip, const TransformationMatrix &matrix) {
        if (!matrix.isIdentityOrTranslation() || matrix.m43() ||
            matrix.m41() != std::round(matrix.m41()) || matrix.m42() != std::round(matrix.m42()))
            return false;
        FloatSize offset(matrix.m41(), matrix.m42());
        auto local = shown;
        local.move(-offset);
        if (clip.isRounded() && !clip.intersectionIsRectangular(local)) return false;
        auto rect = clip.rect();
        rect.move(offset);
        shown.intersect(rect);
        return true;
    };
    if (!clipTo(FloatRoundedRect(contents), layer.m_combined)) return;
    auto clipping = layer.contentsClippingRect();
    if ((clipping.isRounded() || !clipping.rect().contains(contents)) &&
        !clipTo(clipping, layer.m_combined))
        return;
    for (auto *at = layer.parent(); at; at = at->parent()) {
        auto &ancestor = static_cast<const CompositingLayer &>(*at);
        if (ancestor.opacity() < 1 || ancestor.preserves3D() ||
            ancestor.renderingIsSuppressedIncludingDescendants())
            return;
        auto clip = ancestor.contentsClippingRect();
        if (ancestor.contentsRectClipsDescendants() &&
            (clip.isRounded() || !clip.rect().contains(ancestor.contentsRect()))) {
            if (!clipTo(clip, ancestor.m_combined)) return;
        } else if (ancestor.masksToBounds()) {
            auto matrix = ancestor.m_combined;
            matrix.translate(ancestor.boundsOrigin().x(), ancestor.boundsOrigin().y());
            if (!clipTo(FloatRoundedRect(FloatRect(FloatPoint(), ancestor.size())), matrix)) return;
        }
    }
    IntRect rect = enclosingIntRect(shown);
    if (rect.isEmpty() || FloatRect(rect) != shown) return;
    if (m_overlay.isNull() &&
        !m_overlay.tryAllocPixels(SkImageInfo::Make(m_viewport.width(), m_viewport.height(),
                                                    kBGRA_8888_SkColorType, kPremul_SkAlphaType)))
        return;
    auto area = SkIRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
    m_overlay.erase(SK_ColorTRANSPARENT, area);
    auto canvas =
        SkCanvas::MakeRasterDirect(m_overlay.info(), m_overlay.getPixels(), m_overlay.rowBytes());
    if (!canvas) return;
    canvas->clipIRect(area);
    bool above = false, drew = false;
    drawAbove(*canvas, static_cast<CompositingLayer &>(*m_root), rect, layer, above, drew);
    p.renderer = layer.videoRenderer();
    if (!p.renderer) return;
    p.rect = rect;
    p.origin =
        IntPoint(layer.m_combined.m41() + contents.x(), layer.m_combined.m42() + contents.y());
    p.frameSize = IntSize(contents.width(), contents.height());
    p.overlaid = drew;
    p.shown = layer.m_videoFrame;
    p.valid = true;
    m_presentedVideo = &layer;
    presentationsMade.fetch_add(1, std::memory_order_release);
    presentationsMade.notify_all();
}

// As drawLayer, but only what comes after the video's own contents: its
// children and the layers painted after it.
void LayerCompositor::drawAbove(SkCanvas &canvas, CompositingLayer &layer, const IntRect &rect,
                                const CompositingLayer &video, bool &above, bool &drew) {
    bool contentsVisible = layer.contentsAreVisible();
    if ((!contentsVisible && layer.children().isEmpty()) ||
        layer.renderingIsSuppressedIncludingDescendants() || layer.opacity() <= 0 ||
        !layer.m_subtreeBounds.intersects(rect))
        return;
    float alpha = layer.opacity();
    bool group = above && alpha < 1 && !layer.children().isEmpty();
    int saved = canvas.getSaveCount();
    if (group) {
        SkRect bounds = layer.m_subtreeBounds;
        canvas.saveLayerAlphaf(&bounds, alpha);
    }
    if (&layer == &video) above = true;
    else if (above && contentsVisible &&
             (layer.backfaceVisibility() || !layer.m_combined.isBackFaceVisible())) {
        drawContents(canvas, layer, group ? 1 : alpha);
        drew = true;
    }
    if (!layer.children().isEmpty()) {
        auto clipping = layer.contentsClippingRect();
        bool clipsToContents =
            !layer.preserves3D() && layer.contentsRectClipsDescendants() &&
            (clipping.isRounded() || !clipping.rect().contains(layer.contentsRect()));
        bool masks = !layer.preserves3D() && layer.masksToBounds();
        if (clipsToContents || masks) {
            canvas.save();
            TransformationMatrix clipTransform = layer.m_combined;
            if (!clipsToContents)
                clipTransform.translate(layer.boundsOrigin().x(), layer.boundsOrigin().y());
            bool exact;
            canvas.setMatrix(drawingMatrix(clipTransform, exact));
            if (clipsToContents) canvas.clipRRect(SkRRect(clipping), !exact);
            else canvas.clipRect(SkRect(FloatRect(FloatPoint(), layer.size())), !exact);
            canvas.resetMatrix();
        }
        for (auto &child : layer.children())
            drawAbove(canvas, static_cast<CompositingLayer &>(child.get()), rect, video, above,
                      drew);
    }
    canvas.restoreToCount(saved);
}

}
