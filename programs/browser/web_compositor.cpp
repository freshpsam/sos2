#include "config.h"
#include "web_compositor.h"

#include "web_clock.h"
#include "web_scrolling.h"
#include <WebCore/FloatQuad.h>
#include <WebCore/FloatRoundedRect.h>
#include <WebCore/FontRenderOptions.h>
#include <WebCore/GraphicsContextSkia.h>
#include <WebCore/GraphicsLayerClient.h>
#include <WebCore/LayoutRect.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkM44.h>
#include <skia/core/SkRRect.h>
#include <skia/core/SkSamplingOptions.h>
#include <cmath>

namespace SOS2 {
using namespace WebCore;

namespace {
// Every backing is RAM the page keeps; beyond this the view is painted flattened.
constexpr uint64_t backingBudgetBytes = 24 * 1024 * 1024;
constexpr uint64_t refineDelayUs = 150000;
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

void CompositingLayer::flushCompositingState(const FloatRect &visibleRect) {
    // WebCore flushes the root of each frame's tree. Subframe roots are children
    // of the main tree, so updating from the attached root covers every layer.
    if (m_compositor.m_root.get() == this) m_compositor.update(*this, visibleRect);
}

double CompositingLayer::backingStoreMemoryEstimate() const { return m_backing.computeByteSize(); }

LayerCompositor::LayerCompositor(unsigned width, unsigned height)
    : m_viewport(width, height), m_fullCoverPixels(2ull * width * height) { }

void LayerCompositor::setRoot(GraphicsLayer *root) {
    m_root = root;
    m_changed = true;
    m_damage = Region(IntRect(IntPoint(), m_viewport));
}

void LayerCompositor::addDamage(const FloatRect &rect) {
    auto damage = intersection(enclosingIntRect(rect), IntRect(IntPoint(), m_viewport));
    if (damage.isEmpty()) return;
    m_damage.unite(Region(damage));
    if (m_damage.gridSize() > 32) m_damage = Region(m_damage.bounds());
}

void LayerCompositor::releaseBacking(CompositingLayer &layer) {
    if (layer.m_backing.isNull()) return;
    m_backingBytes -= layer.m_backing.computeByteSize();
    layer.m_backing.reset();
    layer.m_cover = {};
}

void LayerCompositor::update(CompositingLayer &root, const FloatRect &viewport) {
    uint64_t now = microseconds();
    bool refine = refinementDue(now);
    if (refine) m_refineAfter = 0;
    m_unsupported = false;
    m_overBudget = false;
    updateLayer(root, TransformationMatrix(), viewport, refine);
}

// Transforms follow WebCore's Skia compositor
// (SkiaCompositingLayer::computeTransformsAndAnimations).
void LayerCompositor::updateLayer(CompositingLayer &layer,
                                  const TransformationMatrix &parentForChildren,
                                  const FloatRect &clip, bool refine) {
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
    FloatRect visible;
    if (auto inverse = combined.inverse())
        visible =
            intersection(FloatRect(inverse->clampedBoundsOfProjectedQuad(FloatQuad(clip))), bounds);
    if (layer.drawsContent()) updateBacking(layer, visible, refine);
    else releaseBacking(layer);

    FloatRect childClip = clip;
    if (layer.masksToBounds() && !layer.preserves3D()) {
        TransformationMatrix clipTransform = combined;
        clipTransform.translate(layer.boundsOrigin().x(), layer.boundsOrigin().y());
        childClip.intersect(clipTransform.mapRect(bounds));
    }
    layer.m_subtreeBounds = intersection(combined.mapRect(bounds), clip);
    for (auto &child : layer.children()) {
        auto &childLayer = static_cast<CompositingLayer &>(child.get());
        updateLayer(childLayer, forChildren, childClip, refine);
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

void LayerCompositor::updateBacking(CompositingLayer &layer, const FloatRect &visible,
                                    bool refine) {
    IntRect bounds(IntPoint(), expandedIntSize(layer.size()));
    // Small layers keep all their pixels, so movement never repaints them.
    // Larger ones keep only what the viewport shows.
    IntRect cover = uint64_t(bounds.width()) * bounds.height() <= m_fullCoverPixels
                        ? bounds
                        : intersection(enclosingIntRect(visible), bounds);
    if (cover.isEmpty()) {
        releaseBacking(layer);
        return;
    }
    if ((cover != layer.m_cover || layer.m_backing.isNull()) && !moveCover(layer, cover)) return;
    if (refine && layer.m_coverMoved) {
        layer.m_dirty = Region(layer.m_cover);
        layer.m_coverMoved = false;
    }
    paintDirty(layer);
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
        uint64_t oldBytes = layer.m_backing.isNull() ? 0 : layer.m_backing.computeByteSize();
        if (m_backingBytes - oldBytes + bytes > backingBudgetBytes) {
            m_overBudget = true;
            releaseBacking(layer);
            return false;
        }
        SkBitmap backing;
        auto info =
            SkImageInfo::Make(cover.width(), cover.height(), kBGRA_8888_SkColorType,
                              layer.contentsOpaque() ? kOpaque_SkAlphaType : kPremul_SkAlphaType);
        if (!backing.tryAllocPixels(info)) {
            m_overBudget = true;
            releaseBacking(layer);
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

std::optional<Region> LayerCompositor::composite(SkCanvas &canvas) {
    m_changed = false;
    auto damage = std::exchange(m_damage, Region());
    if (!m_root || m_unsupported || m_overBudget) {
        // The flattened paint covers the viewport; redraw it all once layers return.
        m_damage = Region(IntRect(IntPoint(), m_viewport));
        return std::nullopt;
    }
    if (damage.isEmpty()) return damage;
    // Draw only the damage: the rest of the surface already shows these layers.
    auto rects = damage.rects();
    auto extent = damage.bounds();
    if (coalesceDamage(rects.size(), damage.totalArea(), extent.size().unclampedArea())) {
        damage = Region(extent);
        rects = {extent};
    }
    for (auto &rect : rects) {
        canvas.save();
        canvas.resetMatrix();
        canvas.clipRect(SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()));
        canvas.clear(SK_ColorWHITE);
        drawLayer(canvas, static_cast<CompositingLayer &>(*m_root));
        canvas.restore();
    }
    return damage;
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
void LayerCompositor::drawLayer(SkCanvas &canvas, CompositingLayer &layer) {
    // Hidden contents (visibility: hidden) still draw their children, which
    // may be visible (SkiaCompositingLayer::isVisible).
    bool contentsVisible = layer.contentsAreVisible();
    if ((!contentsVisible && layer.children().isEmpty()) ||
        layer.renderingIsSuppressedIncludingDescendants() || layer.opacity() <= 0)
        return;
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
            (clipping.hasNonZeroRadii() || !clipping.rect().contains(layer.contentsRect()));
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
            drawLayer(canvas, static_cast<CompositingLayer &>(child.get()));
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
    if (layer.m_solidColor.isValid() && layer.m_solidColor.isVisible()) {
        auto clipping = layer.contentsClippingRect();
        if (clipping.hasNonZeroRadii() || !clipping.rect().contains(layer.contentsRect()))
            canvas.clipRRect(SkRRect(clipping), !integer);
        paint.setColor(SkColor(layer.m_solidColor.colorWithAlphaMultipliedBy(alpha)));
        canvas.drawRect(SkRect(layer.contentsRect()), paint);
    }
    canvas.restore();
}

}
