#include "config.h"
#include <WebCore/Document.h>
#include <WebCore/DocumentView.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/FontRenderOptions.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/FrameLoadRequest.h>
#include <WebCore/KeyframeEffect.h>
#include <WebCore/GraphicsContextSkia.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/PaintPhase.h>
#include <WebCore/Page.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/ProcessWarming.h>
#include <WebCore/ProcessCapabilities.h>
#include <WebCore/ScriptController.h>
#include <WebCore/Settings.h>
#include <WebCore/WebAnimation.h>
#include <WebCore/Region.h>
#include <WebCore/RenderLayer.h>
#include <WebCore/RenderLayerCompositor.h>
#include <WebCore/RenderView.h>
#include <WebCore/Scrollbar.h>
#include <pal/SessionID.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/Options.h>
#include <wtf/MainThread.h>
#include <wtf/RunLoop.h>
#include <wtf/text/WTFString.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkCanvas.h>
#include <sos2/services.h>
#include <bmalloc/mimalloc.h>
#include <time.h>
#include <algorithm>
#include <vector>
#include "web_input.h"
#include "web_storage.h"
#include "web_browser.h"
#include "web_clock.h"
#include "web_damage.h"
#include "web_scrolling.h"
#include "web_compositor.h"
#include "web_gpu.h"
#include "web_gpu_compositor.h"
#include "http_client.h"

namespace SOS2 {
void installPlatformStrategies();
void configureNavigation(WebCore::PageConfiguration &);
}

using namespace WebCore;
using namespace WTF;
using SOS2::microseconds;
static constexpr unsigned pageWidth = 480, pageHeight = 752;

// The page's compositors: WebKit's when the page paints on the GPU, else the
// CPU one. The choice is made once, before WebCore creates any layer.
struct Compositors {
    SOS2::LayerCompositor cpu{pageWidth, pageHeight};
    std::unique_ptr<SOS2::GPUCompositor> gpu;
    SOS2::Compositor *current = &cpu;

    bool usesGPU() const { return gpu && current == gpu.get(); }
};

// A blending layer can share a backing with an ancestor while its backdrop
// lives in another backing. WebCore needs the whole page to blend it correctly.
static bool needsBlendFallback(Page &page) {
    for (RefPtr<Frame> frame = &page.mainFrame(); frame; frame = frame->tree().traverseNext()) {
        auto *local = dynamicDowncast<LocalFrame>(*frame);
        auto *view = local ? local->contentRenderer() : nullptr;
        for (auto *layer = view ? view->layer() : nullptr; layer;) {
            if (layer->blendMode() != BlendMode::Normal)
                return true;
            if (auto *child = layer->firstChild()) {
                layer = child;
                continue;
            }
            while (layer && !layer->nextSibling()) layer = layer->parent();
            if (layer) layer = layer->nextSibling();
        }
    }
    return false;
}

static void setLayerCompositing(Page &page, bool enabled) {
    page.settings().setAcceleratedCompositingEnabled(enabled);
    for (RefPtr<Frame> frame = &page.mainFrame(); frame; frame = frame->tree().traverseNext()) {
        auto *local = dynamicDowncast<LocalFrame>(*frame);
        auto *view = local ? local->contentRenderer() : nullptr;
        auto *root = view ? view->layer() : nullptr;
        if (!root) continue;
        // Re-evaluate even unchanged layers, such as a will-change element.
        // With no backing, WebCore only starts the traversal for a dirty child.
        root->setDescendantsNeedCompositingRequirementsTraversal();
        if (auto *child = root->firstChild()) child->setNeedsPostLayoutCompositingUpdate();
        view->compositor().updateCompositingLayers(CompositingUpdateType::AfterStyleChange);
    }
}

// Once WebCore drops its layers, hand compositor animations back to the page
// thread so that direct painting uses the current transform, opacity and filters.
static void handBackAcceleratedAnimations(Page &page) {
    for (RefPtr<Frame> frame = &page.mainFrame(); frame; frame = frame->tree().traverseNext()) {
        auto *local = dynamicDowncast<LocalFrame>(*frame);
        RefPtr document = local ? local->document() : nullptr;
        if (!document) continue;
        for (auto &animation : document->getAnimations()) {
            auto *effect = dynamicDowncast<KeyframeEffect>(animation->effect());
            if (effect && effect->isRunningAccelerated())
                effect->effectStackNoLongerAllowsAcceleration();
        }
    }
}

// A flattened paint leaves out the frame's scrollbars when WebCore keeps them
// in layers of their own, as it does once the page composites. Paint them as
// ScrollView::paint would; the main frame's view sits at the origin.
static void paintLayeredScrollbars(LocalFrameView &view, GraphicsContext &context,
                                   const IntRect &rect) {
    if (view.scrollbarsSuppressed()) return;
    bool layered = false;
    if (auto *bar = view.horizontalScrollbar(); bar && view.layerForHorizontalScrollbar()) {
        bar->paint(context, rect);
        layered = true;
    }
    if (auto *bar = view.verticalScrollbar(); bar && view.layerForVerticalScrollbar()) {
        bar->paint(context, rect);
        layered = true;
    }
    if (layered && !view.scrollCornerRect().isEmpty())
        view.paintScrollCorner(context, view.scrollCornerRect());
}

struct FrameSchedule {
    Region damage{IntRect(0, 0, pageWidth, pageHeight)};
    IntRect presentation;
    uint32_t *pixels = nullptr;
    user_pixel_scroll presentScroll{};
    bool multipleScrolls = false;
    SkSurface *surface = nullptr;
    SOS2::GPURaster *gpu = nullptr; // paints when set; pixels then hold readbacks

    // GPU frames go to the compositor; pixels go stale.
    bool gpuPresents() const { return gpu && gpu->canPresent(); }

    uint64_t refineAfter = 0;
    bool copiedSincePaint = false;
    bool painting = false;
    bool updatePending = true;

    static IntRect clip(const IntRect &rect) {
        return intersection(rect, IntRect(0, 0, pageWidth, pageHeight));
    }

    bool hasDamage() const { return !damage.isEmpty() || !presentation.isEmpty(); }

    void boundDamage() {
        if (damage.gridSize() > 128) damage = Region(damage.bounds());
    }

    void invalidate(const IntRect &rect) {
        damage.unite(Region(clip(rect)));
        boundDamage();
    }

    void scroll(const IntSize &delta, const IntRect &scrollRect, const IntRect &clipRect) {
        auto rect = clip(intersection(scrollRect, clipRect));
        if (rect.isEmpty() || delta.isZero()) return;
        presentation.unite(rect);
        if (!pixels || painting) {
            invalidate(rect);
            return;
        }
        SOS2::PixelDamage area{unsigned(rect.x()), unsigned(rect.y()), unsigned(rect.width()),
                               unsigned(rect.height())};
        // GPU frames keep no pixel buffer: only the surface moves, and the
        // compositor copies the whole scroll rectangle from it.
        if (!gpuPresents()) surface->notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
        auto copied = gpuPresents() ? SOS2::scrollDestination(pageWidth, pageHeight, area,
                                                              delta.width(), delta.height())
                                    : SOS2::scrollPixels(pixels, pageWidth, pageHeight, area,
                                                         delta.width(), delta.height());
        if (!copied.width) {
            invalidate(rect);
            return;
        }
        if (gpu)
            gpu->scroll(IntRect(copied.x, copied.y, copied.width, copied.height), delta.width(),
                        delta.height());
        // A frame may retain one scroll. Multiple callbacks fall back to exact
        // patches against the unmodified presentation reference, preserving
        // ordering even for overlapping or nested scroll rectangles.
        // GPU frames need none.
        if (!gpuPresents()) {
            if (presentScroll.rect.width) multipleScrolls = true;
            else
                presentScroll = {
                    {area.x, area.y, area.width, area.height}, delta.width(), delta.height()};
        }
        // Pending content damage moves with the copied pixels. Preserve damage
        // outside the scroll clip and repaint newly exposed strips separately.
        Region bounds(rect), moved(damage);
        moved.intersect(bounds);
        damage.subtract(bounds);
        moved.translate(delta);
        moved.intersect(bounds);
        damage.unite(moved);
        Region exposed(bounds);
        exposed.subtract(Region(IntRect(copied.x, copied.y, copied.width, copied.height)));
        damage.unite(exposed);
        boundDamage();
        copiedSincePaint = true;
        refineAfter = 0;
    }

    // Composited pages redraw moved layers themselves; the scroll only tells the
    // kernel which presented pixels to reuse, keeping the patches small.
    void hintScroll(const IntSize &delta, const IntRect &scrollRect, const IntRect &clipRect) {
        auto rect = clip(intersection(scrollRect, clipRect));
        if (rect.isEmpty() || delta.isZero()) return;
        if (presentScroll.rect.width) multipleScrolls = true;
        else
            presentScroll = {{unsigned(rect.x()), unsigned(rect.y()), unsigned(rect.width()),
                              unsigned(rect.height())},
                             delta.width(),
                             delta.height()};
    }
};

class NativeChromeClient final : public EmptyChromeClient {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NativeChromeClient);
    FrameSchedule &schedule;
    Compositors &compositors;

    bool composited() const { return compositors.current->active(); }

public:
    NativeChromeClient(FrameSchedule &value, Compositors &layers)
        : schedule(value), compositors(layers) { }

    // While composited, layer invalidations reach the compositor instead.
    void invalidateRootView(const IntRect &rect) final {
        if (!composited()) schedule.presentation.unite(FrameSchedule::clip(rect));
    }

    void invalidateContentsAndRootView(const IntRect &rect) final {
        if (!composited()) schedule.invalidate(rect);
    }

    void invalidateContentsForSlowScroll(const IntRect &rect) final {
        if (!composited()) schedule.invalidate(rect);
    }

    void scroll(const IntSize &delta, const IntRect &rect, const IntRect &clip) final {
        if (composited()) schedule.hintScroll(delta, rect, clip);
        else schedule.scroll(delta, rect, clip);
    }

    bool scheduleRenderingUpdate() final {
        schedule.updatePending = true;
        return true;
    }

    void triggerRenderingUpdate() final { schedule.updatePending = true; }

    GraphicsLayerFactory *graphicsLayerFactory() const final {
        return &compositors.current->factory();
    }

    void attachRootGraphicsLayer(LocalFrame &, GraphicsLayer *layer) final {
        auto &compositor = *compositors.current;
        bool wasActive = compositor.active();
        compositor.setRoot(layer);
        // Leaving compositing repaints the whole view directly; entering it
        // drops direct-paint damage and its pending refinement, since the
        // compositor draws everything.
        if (wasActive && !layer) schedule.invalidate(IntRect(0, 0, pageWidth, pageHeight));
        if (!wasActive && layer) {
            schedule.damage = Region();
            schedule.refineAfter = 0;
        }
        schedule.updatePending = true;
    }

    // Composite for animated transforms and opacity. will-change and backdrop
    // filters composite regardless; fixed and overflow scrolling stay painted.
    CompositingTriggerFlags allowedCompositingTriggers() const final {
        return AnimationTrigger | AnimatedOpacityTrigger;
    }
};

int main() {
    // MADV_DONTNEED only zeroes SOS2 pages and frees nothing: never purge.
    mi_option_set(mi_option_purge_delay, -1);
    // SOS2 commits memory when it is mapped, so JavaScriptCore's address space
    // reservations (the JIT pool, and on 64-bit the structure heap) are cut to
    // what pages use. A process has 16 threads and WebKit stops a page that
    // cannot start one, so GC marking adds at most 3 threads and the JIT
    // worklist 2, leaving room for workers and image decoders.
    // ScriptController's own JSC::initialize() below then does nothing.
    JSC::initialize([] {
#if CPU(ADDRESS64)
        JSC::Options::structureHeapSizeInKB() = 16 * 1024;
        JSC::Options::jitMemoryReservationSize() = 16 * MB;
#else
        // One region, within the ARMv7 JIT's direct branch range (16 MiB).
        // Pages compile under 1 MiB of code (Wikipedia: 0.9 MiB), and every
        // page maps the whole pool.
        JSC::Options::jitMemoryReservationSize() = 4 * MB;
        // Pages map memory only in the SDK's 128 MiB mmap range: the rest of
        // the quota that ramSize() reports is the newlib break, which the
        // engine's allocator never grows (sdk/runtime/internal.h). The heap
        // grows by the smaller figure.
        JSC::Options::forceRAMSize() = 128 * MB;
#endif
        JSC::Options::numberOfGCMarkers() = std::min(JSC::Options::numberOfGCMarkers(), 4u);
        JSC::Options::minNumberOfWorklistThreads() = 1;
        JSC::Options::maxNumberOfWorklistThreads() = 2;
        // The sos2.jsc= boot argument, such as useDFGJIT=false (LLInt and the
        // baseline JIT) or useJIT=false (LLInt alone); a bad one stops here.
        if (const char* options = sos2_engine_options())
            RELEASE_ASSERT(JSC::Options::setOptions(options));
    });
    ScriptController::initializeMainThread();
    ProcessCapabilities::setCanUseAcceleratedBuffers(false);
    SOS2::installPlatformStrategies();
    ProcessWarming::prewarmGlobally();
    FrameSchedule schedule;
    // Before the page: layers keep pointers to their compositor until destroyed.
    Compositors compositors;
    auto configuration =
        pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    configuration.chromeClient = makeUniqueRef<NativeChromeClient>(schedule, compositors);
    SOS2::configureInput(configuration);
    SOS2::configureNavigation(configuration);
    SOS2::configureStorage(configuration);
    std::get<PageConfiguration::LocalMainFrameCreationParameters>(
        configuration.mainFrameCreationParameters)
        .effectiveSandboxFlags = {};
    auto page = Page::create(WTF::move(configuration));
    auto &settings = page->settings();
    settings.setScriptEnabled(true);
    settings.setImagesEnabled(true);
    settings.setLoadsImagesAutomatically(true);
    // Layer compositing (web_compositor.h).
    settings.setAcceleratedCompositingEnabled(true);
    // Pages never zoom, so the visual viewport is always the layout viewport.
    // With WebCore's visual-viewport mode on, each scroll step moves the layout
    // viewport and lays out and repaints every fixed-position element; without
    // it they are only moved.
    settings.setVisualViewportEnabled(false);
    settings.setHiddenPageDOMTimerThrottlingEnabled(true);
    settings.setHiddenPageCSSAnimationSuspensionEnabled(true);
    settings.setLocalStorageEnabled(true);
    settings.setIndexedDBAPIEnabled(false);
    settings.setSessionStorageEnabled(true);
    settings.setCookieEnabled(true);
    settings.setAsyncClipboardAPIEnabled(false);
    settings.setJavaScriptCanAccessClipboard(false);
    settings.setDefaultFontSize(18);
    settings.setDefaultFixedFontSize(16);
    settings.setStandardFontFamily("DejaVu Serif"_s);
    settings.setSerifFontFamily("DejaVu Serif"_s);
    settings.setSansSerifFontFamily("DejaVu Sans"_s);
    settings.setFixedFontFamily("DejaVu Sans Mono"_s);
    settings.setCursiveFontFamily("DejaVu Sans"_s);
    settings.setFantasyFontFamily("DejaVu Sans"_s);
    auto frame = page->localMainFrame();
    frame->setView(LocalFrameView::create(*frame, IntSize(pageWidth, pageHeight)));
    frame->init();
    // Keep WebKit's root scrolling enabled. Disabling scrollbars also disables
    // wheel scrolling and makes controls below the initial viewport unreachable.
    auto url = SOS2::initialBrowserURL();
    frame->loader().load(FrameLoadRequest(*frame, ResourceRequest(URL(url))));
    std::vector<uint32_t> pixels(pageWidth * pageHeight);
    std::vector<uint32_t> presentedPixels(pageWidth * pageHeight);
    bool hasPresented = false;
    // Every surface draws text as WebKit's own layer tiles do (contrast and
    // gamma from FontRenderOptions), so text keeps its weight across paths.
    auto surfaceProps = FontRenderOptions::singleton().createSurfaceProps();
    auto surface = SkSurfaces::WrapPixels(
        SkImageInfo::Make(pageWidth, pageHeight, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
        pixels.data(), pageWidth * 4, &surfaceProps);
    if (!surface) return 3;
    schedule.pixels = pixels.data();
    schedule.surface = surface.get();
    std::unique_ptr<SOS2::GPURaster> gpu;
    // Skia's GPU backend when the kernel offers a GPU (web_gpu.h), and with it
    // WebKit's compositor (web_gpu_compositor.h).
    gpu = SOS2::GPURaster::create(pageWidth, pageHeight);
    schedule.gpu = gpu.get();
    // No layer exists yet: the first compositing update uses this compositor.
    if (gpu &&
        (compositors.gpu = SOS2::GPUCompositor::create(pageWidth, pageHeight))) {
        // Layer tiles and canvases may then be GPU buffers.
        ProcessCapabilities::setCanUseAcceleratedBuffers(true);
        // Fixed-position elements get layers of their own, which WebCore moves
        // as the page scrolls; painted into the page they are repainted at
        // their old and new place on every scroll step.
        settings.setAcceleratedCompositingForFixedPositionEnabled(true);
        compositors.current = compositors.gpu.get();
    }
    bool blendFallback = false;
    bool handBackAnimations = false;
    // Lost for good: repaint and present everything on the CPU.
    auto loseGPU = [&] {
        gpu.reset();
        schedule.gpu = nullptr;
        hasPresented = false;
        surface->notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
        schedule.invalidate(IntRect(0, 0, pageWidth, pageHeight));
        if (compositors.usesGPU()) {
            // WebKit's layers need the GPU: composite no more. The next update
            // drops them, and WebCore paints the whole view directly from then on.
            compositors.gpu->lose();
            ProcessCapabilities::setCanUseAcceleratedBuffers(false);
            settings.setAcceleratedCompositingEnabled(false);
            handBackAnimations = true;
        }
        compositors.current->redrawAll();
        schedule.updatePending = true;
    };
    SOS2::Input input(page.get());
    bool painted = false;
    bool wasVisible = false;
    uint64_t nextFrame = 0;
    for (;;) {
        // Drive WebKit's actual event loop: timers, promise jobs and DOM events.
        RunLoop::cycle();
        SOS2::handleBrowserCommands(page.get());
        SOS2::HTTP::closeIdleConnections();
        // Bound work per frame even when a device or page produces continuous input.
        for (unsigned budget = 0; budget < 64; ++budget) {
            user_input_event event;
            int received = sos2_next_event(&event);
            if (received < 0) return 5;
            if (!received) break;
            input.handle(event);
        }
        auto surfaceState = sos2_surface_state();
        bool visible = surfaceState & USER_SURFACE_VISIBLE;
        if (page->isVisible() != visible) page->setIsVisible(visible);
        // The display's GPU failed: an idle page would present nothing more
        // and stay blank, so lose the GPU now, as a failed present would.
        if (gpu && !(surfaceState & USER_SURFACE_GPU_FRAMES)) loseGPU();
        // The shell and compositor retain the last presented page. Hidden-page
        // invalidations remain in schedule.damage; visibility alone only needs
        // a rendering update, not a full raster/upload of unchanged content.
        if (visible && !wasVisible) schedule.updatePending = true;
        wasVisible = visible;
        auto now = microseconds();
        if (visible && schedule.refineAfter && now >= schedule.refineAfter) {
            // Skia's clipped curve coverage can differ by a few edge pixels
            // from an unclipped paint. Keep movement cheap, then refine once
            // after 150 ms without further scrolling. Start this interval after
            // the last paint, so a slow frame cannot trigger it mid-gesture.
            schedule.invalidate(IntRect(0, 0, pageWidth, pageHeight));
            schedule.refineAfter = 0;
        }
        auto *compositor = compositors.current;
        if (compositor->active() && compositor->refinementDue(now)) schedule.updatePending = true;
        bool compositeDue = compositor->active() && compositor->needsComposite();
        if (now >= nextFrame &&
            (schedule.updatePending ||
             ((schedule.hasDamage() || compositeDue) && (visible || !painted)))) {
            // Schedule against frame start; work no longer adds a fixed sleep
            // to every frame. WebKit applies hidden-page animation/timer policy.
            nextFrame = now + 16667;
            // Frames that only run the compositor's own animations skip WebCore.
            bool update = schedule.updatePending || schedule.hasDamage() || !compositor->active() ||
                          !compositor->compositesWithoutUpdate();
            schedule.updatePending = false;
            if (update) {
                page->isolatedUpdateRendering();
                // Let WebCore take over both painting and animation when a
                // blend needs its backdrop, keeping GPU raster and presentation.
                if (gpu && compositors.usesGPU()) {
                    bool flatten = needsBlendFallback(page.get());
                    if (flatten != blendFallback) {
                        blendFallback = flatten;
                        setLayerCompositing(page.get(), !flatten);
                        handBackAnimations |= flatten;
                        schedule.invalidate(IntRect(0, 0, pageWidth, pageHeight));
                        schedule.updatePending = true;
                        continue;
                    }
                }
                if (std::exchange(handBackAnimations, false)) {
                    handBackAcceleratedAnimations(page.get());
                    schedule.updatePending = true;
                }
                // Layer backings repaint inside WebCore's layer flush.
                if (compositor->active()) {
                    frame->view()->flushCompositingStateIncludingSubframes();
                    compositor->commit();
                }
            }
            bool composited = compositor->active();
            if ((composited ? compositor->needsComposite() || schedule.presentScroll.rect.width
                            : schedule.hasDamage()) &&
                (visible || !painted)) {
                // Retain the rest of the backing store. Invalidations raised by
                // paint itself remain queued for the following frame.
                auto damage = std::exchange(schedule.damage, Region());
                auto presentation = std::exchange(schedule.presentation, IntRect());
                presentation.unite(damage.bounds());
                auto rectangles = damage.rects();
                // Dense damage paints as one rectangle, sparse damage (e.g. two
                // small animations far apart) rectangle by rectangle: each paint
                // walks the render tree (SOS2::coalesceDamage). The strips a fast
                // scroll exposed paint apart even when dense; the idle refinement
                // reconciles Skia's clip-dependent coverage of curved edges.
                if (composited) {
                    // The compositor redraws the viewport; the comparison below
                    // finds the pixels that changed.
                    presentation = IntRect(0, 0, pageWidth, pageHeight);
                    rectangles.clear();
                    rectangles.append(presentation);
                } else if (!rectangles.isEmpty() &&
                           SOS2::coalesceDamage(rectangles.size(), damage.totalArea(),
                                                damage.bounds().size().unclampedArea(),
                                                !schedule.copiedSincePaint)) {
                    damage = Region(damage.bounds());
                    rectangles.clear();
                    rectangles.append(damage.bounds());
                }
                // On the GPU when the kernel offers one, else into the pixel buffer.
                auto *canvas = (gpu ? &gpu->surface() : surface.get())->getCanvas();
                // The main frame fills every rect it paints with its opaque base
                // background first. Clear only where it returns without painting
                // (no render tree yet, layout pending), so old pixels never stay.
                auto &view = *frame->view();
                bool paintsBackground = frame->contentRenderer() &&
                                        view.layoutContext().inPaintableState() &&
                                        !view.needsLayout();
                schedule.painting = true;
                std::optional<Region> composed;
                if (composited && (composed = compositor->composite(*canvas))) {
                    // Only the compositor's damage changed; compare and present just
                    // that. A scroll hint shifts every presented pixel in its
                    // rectangle, so then the whole viewport is compared.
                    rectangles = composed->rects();
                    presentation = composed->bounds();
                    if (schedule.presentScroll.rect.width) {
                        presentation = IntRect(0, 0, pageWidth, pageHeight);
                        rectangles.clear();
                        rectangles.append(presentation);
                    }
                } else {
                    // A composited tree the compositor cannot draw paints flattened.
                    auto behavior = view.paintBehavior();
                    if (composited)
                        view.setPaintBehavior(behavior | PaintBehavior::FlattenCompositingLayers);
                    for (const auto &rect : rectangles) {
                        canvas->save();
                        canvas->clipRect(
                            SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()));
                        if (!paintsBackground) canvas->clear(SK_ColorWHITE);
                        {
                            GraphicsContextSkia context(*canvas, RenderingMode::Unaccelerated,
                                                        RenderingPurpose::Snapshot);
                            view.paint(context, rect);
                            if (composited) paintLayeredScrollbars(view, context, rect);
                        }
                        canvas->restore();
                    }
                    if (composited) view.setPaintBehavior(behavior);
                }
                schedule.painting = false;
                if (!composited) {
                    if (damage.totalArea() == uint64_t(pageWidth) * pageHeight)
                        schedule.refineAfter = 0;
                    else if (schedule.copiedSincePaint)
                        schedule.refineAfter = microseconds() + 150000;
                    schedule.copiedSincePaint = false;
                }
                // GPU frames go to the compositor from the surface itself;
                // otherwise GPU painting is read back into the pixel buffer.
                bool gpuFrame = schedule.gpuPresents();
                IntRect shown = hasPresented ? presentation : IntRect(0, 0, pageWidth, pageHeight);
                if (gpu &&
                    !(gpuFrame ? gpu->present(std::span<const IntRect>(&shown, 1))
                               : gpu->readBack(rectangles.span(), pixels.data(), pageWidth))) {
                    loseGPU();
                    continue;
                }
                if (gpuFrame) {
                    hasPresented = true;
                    schedule.presentScroll = {};
                    schedule.multipleScrolls = false;
                } else {
                    auto scroll = std::exchange(schedule.presentScroll, user_pixel_scroll{});
                    if (std::exchange(schedule.multipleScrolls, false) || !hasPresented)
                        scroll = {};
                    if (scroll.rect.width)
                        SOS2::scrollPixels(
                            presentedPixels.data(), pageWidth, pageHeight,
                            {scroll.rect.x, scroll.rect.y, scroll.rect.width, scroll.rect.height},
                            scroll.dx, scroll.dy);
                    SOS2::PixelDamage bounds{static_cast<unsigned>(presentation.x()),
                                             static_cast<unsigned>(presentation.y()),
                                             static_cast<unsigned>(presentation.width()),
                                             static_cast<unsigned>(presentation.height())};
                    SOS2::PixelDamageList patches;
                    if (!hasPresented) patches = {{{0, 0, pageWidth, pageHeight}}, 1};
                    else if (scroll.rect.width) {
                        // Both retained images received the same scroll above.
                        // Only clipped paint can change their remaining pixels:
                        // compare exposed strips and fixed content, not the whole
                        // viewport on every touch movement. Multiple scrolls use
                        // the full presentation comparison through the fallback.
                        for (const auto &rect : rectangles)
                            SOS2::appendChangedRegions(
                                patches, pixels.data(), presentedPixels.data(), pageWidth,
                                {unsigned(rect.x()), unsigned(rect.y()), unsigned(rect.width()),
                                 unsigned(rect.height())});
                    } else
                        patches = SOS2::changedRegions(pixels.data(), presentedPixels.data(),
                                                       pageWidth, bounds);
                    static_assert(SOS2::PixelDamageList::limit == USER_FRAME_RECTS);
                    if (patches.count || scroll.rect.width) {
                        user_pixel_rect damageRects[USER_FRAME_RECTS];
                        for (unsigned i = 0; i < patches.count; ++i) {
                            auto r = patches.rects[i];
                            damageRects[i] = {r.x, r.y, r.width, r.height};
                        }
                        if (sos2_present_frame(pixels.data(), &scroll, damageRects, patches.count))
                            return 4;
                        for (unsigned i = 0; i < patches.count; ++i) {
                            auto r = patches.rects[i];
                            for (unsigned y = r.y; y < r.y + r.height; ++y)
                                std::memcpy(presentedPixels.data() + y * pageWidth + r.x,
                                            pixels.data() + y * pageWidth + r.x, r.width * 4);
                        }
                        hasPresented = true;
                    }
                }
            }
        }
        SOS2::publishBrowserState(page.get());
        if (!painted && !frame->loader().isLoading() &&
            !frame->loader().provisionalDocumentLoader() &&
            frame->document()->readyState() == Document::ReadyState::Complete)
            painted = true;
        // Keep timers, input and incremental network I/O responsive without
        // running layout, rasterization or framebuffer copies on an idle page.
        now = microseconds();
        uint64_t delayUs = 8000;
        // A composite is due for the compositor's own animations as well.
        compositeDue = compositors.current->active() && compositors.current->needsComposite();
        if (schedule.updatePending ||
            ((schedule.hasDamage() || compositeDue) && (visible || !painted)))
            delayUs = nextFrame > now ? std::min<uint64_t>(4000, nextFrame - now) : 1;
        struct timespec delay = {0, static_cast<long>(delayUs * 1000)};
        nanosleep(&delay, nullptr);
    }
}
