#pragma once
// Layer compositing. When WebCore composites (CSS transform or opacity
// animations, will-change), the page's compositor draws the layer tree into
// the page surface instead of web_main.cpp painting the view directly. Moving
// or fading a layer then costs a composite instead of a repaint. Pages without
// composited layers keep the direct paint path.
//
// On the CPU (LayerCompositor), each layer the viewport shows paints into its
// own Skia bitmap during WebCore's layer flush, and each frame draws the layer
// tree with its transforms, opacity and clips where it changed. A video has a
// layer of its own, into which the compositor draws the player's current
// frame, so new frames repaint nothing. Pages painting on the GPU use WebKit's
// own compositor instead (GPUCompositor, web_gpu_compositor.h).
#include <WebCore/Color.h>
#include <WebCore/FloatRect.h>
#include <WebCore/GraphicsLayer.h>
#include <WebCore/GraphicsLayerFactory.h>
#include <WebCore/IntRect.h>
#include <WebCore/NativeImage.h>
#include <WebCore/Region.h>
#include <WebCore/TransformationMatrix.h>
#include <skia/core/SkBitmap.h>
#include <wtf/HashSet.h>
#include <cstdint>

class SkCanvas;

namespace SOS2 {

class LayerCompositor;

// What a composite changed: the damage, in surface coordinates, as rectangles
// to draw (SOS2::coalesceDamage). When the compositor cannot draw the tree it
// draws nothing, and the caller paints the damage with the layers flattened.
struct Composition {
    WebCore::Region damage;
    bool flatten{false};
};

// What web_main.cpp drives each frame, on either compositor.
class Compositor {
public:
    virtual ~Compositor() = default;
    virtual WebCore::GraphicsLayerFactory &factory() = 0;
    virtual void setRoot(WebCore::GraphicsLayer *) = 0;

    bool active() const { return !!m_root; }

    virtual bool needsComposite() const = 0;

    // A composite is due without a WebCore rendering update (animations the
    // compositor runs itself).
    virtual bool compositesWithoutUpdate() const { return false; }

    // Strip paints after scrolling are redone in one paint once movement stops.
    virtual bool refinementDue(uint64_t) const { return false; }

    // The surface lost its contents (the GPU was lost): draw everything again.
    virtual void redrawAll() = 0;

    // After WebCore's layer flush: take the layers' new state.
    virtual void commit() { }

    // Draws the layers into the canvas where they changed since the last
    // composite, or leaves that damage to the caller to paint flattened.
    virtual Composition composite(SkCanvas &) = 0;

protected:
    RefPtr<WebCore::GraphicsLayer> m_root;
};

// A layer's contents live in one bitmap ("cover") while the viewport shows any
// of it: the whole layer when it is at most twice the viewport, otherwise only
// the part the viewport shows, as for the document's root contents. Moving the
// cover keeps overlapping pixels; layers out of view keep none.
class CompositingLayer final : public WebCore::GraphicsLayer {
public:
    CompositingLayer(Type, WebCore::GraphicsLayerClient &, LayerCompositor &);
    ~CompositingLayer() final;

    void setNeedsDisplay() final;
    void setNeedsDisplayInRect(const WebCore::FloatRect &,
                               ShouldClipToLayer = ShouldClipToLayer::Clip) final;

    bool setChildren(Vector<Ref<WebCore::GraphicsLayer>> &&) final;
    void addChild(Ref<WebCore::GraphicsLayer> &&) final;
    void addChildAtIndex(Ref<WebCore::GraphicsLayer> &&, int index) final;
    void addChildAbove(Ref<WebCore::GraphicsLayer> &&, WebCore::GraphicsLayer *sibling) final;
    void addChildBelow(Ref<WebCore::GraphicsLayer> &&, WebCore::GraphicsLayer *sibling) final;
    bool replaceChild(WebCore::GraphicsLayer *oldChild,
                      Ref<WebCore::GraphicsLayer> &&newChild) final;
    void setMaskLayer(RefPtr<WebCore::GraphicsLayer> &&) final;
    void setReplicatedByLayer(RefPtr<WebCore::GraphicsLayer> &&) final;

    void setPosition(const WebCore::FloatPoint &) final;
    void syncPosition(const WebCore::FloatPoint &) final;
    void setAnchorPoint(const WebCore::FloatPoint3D &) final;
    void setSize(const WebCore::FloatSize &) final;
    void setBoundsOrigin(const WebCore::FloatPoint &) final;
    void syncBoundsOrigin(const WebCore::FloatPoint &) final;
    void setTransform(const WebCore::TransformationMatrix &) final;
    void setChildrenTransform(const WebCore::TransformationMatrix &) final;
    void setPreserves3D(bool) final;
    void setMasksToBounds(bool) final;
    void setDrawsContent(bool) final;
    void setContentsVisible(bool) final;
    void setContentsOpaque(bool) final;
    void setBackfaceVisibility(bool) final;
    void setOpacity(float) final;
    void setBlendMode(WebCore::BlendMode) final;
    void setBackgroundColor(const WebCore::Color &) final;
    void setContentsRect(const WebCore::FloatRect &) final;
    void setContentsClippingRect(const WebCore::FloatRoundedRect &) final;
    void setContentsRectClipsDescendants(bool) final;
    void setContentsToSolidColor(const WebCore::Color &) final;
    // A video: the player's frames, drawn in the contents rectangle
    // (MediaPlayerPrivateSOS2::supportsAcceleratedRendering).
    void setContentsToPlatformLayer(PlatformLayer *, ContentsLayerPurpose) final;

    bool usesContentsLayer() const final { return m_solidColor.isValid() || m_showsVideo; }

    // Declined, so WebCore paints filters and images into the layer itself.
    // Backdrop filters are not drawn, as without compositing.
    bool setFilters(const WebCore::FilterOperations &) final { return false; }

    bool setBackdropFilters(const WebCore::FilterOperations &) final { return false; }

    bool shouldDirectlyCompositeImage(WebCore::Image *) const final { return false; }

    void flushCompositingState(const WebCore::FloatRect &visibleRect) final;
    double backingStoreMemoryEstimate() const final;

private:
    friend class LayerCompositor;
    void willModifyChildren() final;
    void noteChange();
    RefPtr<WebCore::NativeImage> currentVideoFrame() const;

    LayerCompositor &m_compositor;
    WebCore::Color m_solidColor;
    bool m_showsVideo{false};
    bool m_reached{false};                     // in the tree the last update walked
    RefPtr<WebCore::NativeImage> m_videoFrame; // the frame drawn
    WebCore::FloatRect m_videoBounds;          // surface coordinates, clipped
    WebCore::TransformationMatrix m_combined; // layer coordinates to the page surface
    WebCore::FloatRect m_subtreeBounds;       // surface coordinates, clipped
    WebCore::IntRect m_cover;                 // layer coordinates of m_backing
    WebCore::IntRect m_wantedCover;           // what this update gives it
    SkBitmap m_backing;
    WebCore::Region m_dirty;          // layer coordinates
    bool m_coverMoved{false};         // strips were painted since the last full repaint
    bool m_changed{true};             // properties changed since the last composite
    WebCore::FloatRect m_drawnBounds; // m_subtreeBounds at the last composite
};

class LayerCompositor final : public Compositor {
public:
    LayerCompositor(unsigned width, unsigned height);

    WebCore::GraphicsLayerFactory &factory() final { return m_factory; }

    void setRoot(WebCore::GraphicsLayer *) final;

    void changed() { m_changed = true; }

    void redrawAll() final {
        m_changed = true;
        m_damage = WebCore::Region(WebCore::IntRect(WebCore::IntPoint(), m_viewport));
    }

    bool needsComposite() const final;

    // New video frames alone are drawn without a WebCore rendering update.
    bool compositesWithoutUpdate() const final { return !m_changed; }

    bool refinementDue(uint64_t nowUs) const final {
        return m_refineAfter && nowUs >= m_refineAfter;
    }

    // Flattened when a layer needs a feature this compositor lacks (mask,
    // replica, blend mode) or the backings would exceed the memory budget.
    Composition composite(SkCanvas &) final;

private:
    friend class CompositingLayer;

    class Factory final : public WebCore::GraphicsLayerFactory {
    public:
        explicit Factory(LayerCompositor &compositor) : m_compositor(compositor) { }

        Ref<WebCore::GraphicsLayer> createGraphicsLayer(WebCore::GraphicsLayerType,
                                                        WebCore::GraphicsLayerClient &) final;

    private:
        LayerCompositor &m_compositor;
    };

    void update(CompositingLayer &root, const WebCore::FloatRect &viewport);
    void updateLayer(CompositingLayer &, const WebCore::TransformationMatrix &parentForChildren,
                     const WebCore::FloatRect &clip);
    void updateBackings(CompositingLayer &, bool refine);
    bool moveCover(CompositingLayer &, const WebCore::IntRect &cover);
    void paintDirty(CompositingLayer &);
    void releaseBacking(CompositingLayer &);
    void addDamage(const WebCore::FloatRect &);
    bool takeVideoFrame(CompositingLayer &);
    void findOccluder(CompositingLayer &, const WebCore::IntRect &damage,
                      Vector<CompositingLayer *> &path);
    bool coversOpaquely(const CompositingLayer &, const WebCore::IntRect &damage) const;
    void drawLayer(SkCanvas &, CompositingLayer &, const WebCore::IntRect &damage);
    void drawContents(SkCanvas &, CompositingLayer &, float alpha);

    Factory m_factory{*this};
    WebCore::IntSize m_viewport;
    uint64_t m_fullCoverPixels;
    uint64_t m_refineAfter{0};
    WebCore::Region m_damage; // surface coordinates
    bool m_changed{true};
    bool m_unsupported{false};
    bool m_flatten{false};      // the caller paints the layers flattened
    uint64_t m_backingBytes{0}; // all layers' backings, held within the memory budget
    uint64_t m_wantedBytes{0};  // what this update's covers need
    uint64_t m_budget;          // lowered for a while when memory runs out
    uint64_t m_budgetRetryAt{0};
    HashSet<CompositingLayer *> m_videoLayers; // polled for new frames
    // While drawing a rectangle: the path from the root to the topmost layer
    // that hides all of it, and whether drawing has yet to reach that layer.
    Vector<CompositingLayer *> m_occluder;
    bool m_occluded{false};
};

}
