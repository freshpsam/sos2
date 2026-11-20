#include "config.h"
#include "web_gpu_compositor.h"

#include <WebCore/CoordinatedImageBackingStore.h>
#include <WebCore/GraphicsLayerCoordinated.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/SkiaPaintingEngine.h>
#include <WebCore/TextureMapper.h>
#include <WebCore/TextureMapperGLHeaders.h>
#include <WebCore/TextureMapperLayer.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <EGL/egl.h>
#include <cstring>

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
      m_sceneRoot(CoordinatedPlatformLayer::create()),
      // Tiles are painted on this thread (no painting workers on SOS2).
      m_paintingEngine(SkiaPaintingEngine::create()) {
    Locker locker{m_sceneRoot->lock()};
    m_sceneRoot->setAnchorPoint(FloatPoint3D(0, 0, 0));
    m_sceneRoot->setSize(FloatSize(width, height));
}

GPUCompositor::~GPUCompositor() {
    // As WPE's scene state does: layers outliving the compositor lose their client.
    for (auto &layer : std::exchange(m_layers, {})) layer->invalidateClient();
    if (m_framebuffer) {
        glDeleteFramebuffers(1, &m_framebuffer);
        glDeleteRenderbuffers(1, &m_stencil);
    }
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
                [&] { return CoordinatedImageBackingStore::create(WTF::move(image)); })
        .iterator->value;
}

// WPE's CoordinatedSceneState::flush, after WebCore's layer flush.
void GPUCompositor::commit() {
    // Now, even when the page is hidden and no composite follows.
    releaseDetached();
}

// TextureMapper clips through its target's stencil buffer, which Skia's
// framebuffer lacks: it draws into a framebuffer of its own on the surface's
// texture, with a stencil buffer as BitmapTexture::initializeStencil makes.
bool GPUCompositor::bindTarget(unsigned target, unsigned texture) {
    if (!m_framebuffer) {
        glGenFramebuffers(1, &m_framebuffer);
        glGenRenderbuffers(1, &m_stencil);
        glBindRenderbuffer(GL_RENDERBUFFER, m_stencil);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, m_viewport.width(),
                              m_viewport.height());
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    if (texture == m_targetTexture) return true;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, texture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_stencil);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    m_targetTexture = complete ? texture : 0;
    return complete;
}

// Skia leaves GL state that TextureMapper expects at its defaults: the blend
// equation, the color mask, sampler objects and vertex attribute divisors.
// Skia's instanced path draws share the default VAO with TextureMapper, whose
// non-instanced draws need every divisor back at zero.
void GPUCompositor::resetSkiaState() {
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    static bool gles3 = [] {
        auto *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
        return version && !strncmp(version, "OpenGL ES ", 10) && version[10] >= '3';
    }();
    static auto bindSampler = gles3
        ? reinterpret_cast<void (*)(GLuint, GLuint)>(eglGetProcAddress("glBindSampler"))
        : nullptr;
    if (bindSampler)
        for (GLuint unit = 0; unit < 4; ++unit) bindSampler(unit, 0);
    // These are the GLES instancing entry points Skia itself uses, including
    // GLES 2 implementations that expose instancing through an extension.
    static auto vertexAttribDivisor = [] {
        const char *name = gles3 ? "glVertexAttribDivisor" : nullptr;
        if (!name) {
            auto *extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
            if (extensions && strstr(extensions, "GL_EXT_instanced_arrays"))
                name = "glVertexAttribDivisorEXT";
            else if (extensions && strstr(extensions, "GL_ANGLE_instanced_arrays"))
                name = "glVertexAttribDivisorANGLE";
        }
        return name ? reinterpret_cast<void (*)(GLuint, GLuint)>(eglGetProcAddress(name)) : nullptr;
    }();
    if (vertexAttribDivisor) {
        GLint attributes = 0;
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
        for (GLint attribute = 0; attribute < attributes; ++attribute)
            vertexAttribDivisor(static_cast<GLuint>(attribute), 0);
    }
}

Composition GPUCompositor::composite(SkCanvas &canvas) {
    // Without the surface's texture, the caller paints the whole view flattened.
    Composition flattened{Region(IntRect(IntPoint(), m_viewport)), true};
    if (m_lost) return flattened;
    auto *surface = canvas.getSurface();
    auto *skia = PlatformDisplay::sharedDisplay().skiaGrContext();
    GrGLTextureInfo texture;
    if (!surface || !skia) return flattened;
    auto backend =
        SkSurfaces::GetBackendTexture(surface, SkSurfaces::BackendHandleAccess::kFlushWrite);
    if (!backend.isValid() || !GrBackendTextures::GetGLTextureInfo(backend, &texture))
        return flattened;
    // WPE's ThreadedCompositor::paintToCurrentGLContext: TextureMapper's GL
    // work, from the layer flush's tile uploads on, follows Skia's pending
    // work, and Skia then relearns the GL state it changed.
    skia->flushAndSubmit();
    // A rendering update applies the layers' changes and painted tiles; a frame
    // that only runs animations just draws again (as ThreadedCompositor does).
    if (std::exchange(m_changed, false)) {
        OptionSet<CompositionReason> reasons{CompositionReason::RenderingUpdate};
        // WPE's CoordinatedSceneState::flushCompositingState, on this thread.
        releaseDetached();
        m_sceneRoot->flushCompositingState(reasons);
        for (auto &layer : copyToVector(m_layers)) layer->flushCompositingState(reasons);
    }
    if (!m_textureMapper) m_textureMapper = TextureMapper::create();
    if (!bindTarget(texture.fTarget, texture.fID)) {
        skia->resetContext();
        return flattened;
    }
    resetSkiaState();
    glViewport(0, 0, m_viewport.width(), m_viewport.height());
    // No damage tracking: the whole surface is drawn, on white like the view.
    glDisable(GL_SCISSOR_TEST);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    auto &root = m_sceneRoot->ensureTarget();
    root.setTransform(TransformationMatrix());
    m_animating = root.applyAnimationsRecursively(MonotonicTime::now());
    // The surface keeps its top row first, as TextureMapper's flipped painting.
    m_textureMapper->beginPainting(TextureMapper::FlipY::Yes);
    m_textureMapper->beginClip(TransformationMatrix(),
                               FloatRoundedRect(FloatRect(FloatPoint(), FloatSize(m_viewport))));
    root.paint(*m_textureMapper);
    m_textureMapper->endClip();
    m_textureMapper->endPainting();
    skia->resetContext();
    return {Region(IntRect(IntPoint(), m_viewport))};
}

void GPUCompositor::lose() {
    m_lost = true;
    m_animating = false;
    // Skia's context is abandoned, or the GPU is gone. GL calls into it still
    // fail or are ignored (sdk/drm), so everything is freed now, as usual:
    // leaving compositing drops the detached layers, then TextureMapper and
    // the framebuffer go, while the page lives on with CPU compositing.
    setRoot(nullptr);
    m_imageBackingStores.clear();
    m_textureMapper = nullptr;
    if (m_framebuffer) {
        glDeleteFramebuffers(1, &m_framebuffer);
        glDeleteRenderbuffers(1, &m_stencil);
        m_framebuffer = m_stencil = m_targetTexture = 0;
    }
}

}
