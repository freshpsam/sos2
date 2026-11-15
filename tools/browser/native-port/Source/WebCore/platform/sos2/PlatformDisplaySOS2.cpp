#include "config.h"
#include "PlatformDisplaySOS2.h"

#include "GLContext.h"
#include "GLDisplay.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace WebCore {

bool PlatformDisplaySOS2::initialize() {
    if (sharedDisplayIfExists()) return true;
    const char *extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!GLContext::isExtensionSupported(extensions, "EGL_MESA_platform_surfaceless")) return false;
    auto display = GLDisplay::create(
        eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr));
    if (!display) return false;
    setSharedDisplay(
        std::unique_ptr<PlatformDisplay>(new PlatformDisplaySOS2(display.releaseNonNull())));
    return true;
}

PlatformDisplaySOS2::PlatformDisplaySOS2(Ref<GLDisplay> &&display)
    : PlatformDisplay(WTF::move(display)) { }

PlatformDisplaySOS2::~PlatformDisplaySOS2() = default;

} // namespace WebCore
