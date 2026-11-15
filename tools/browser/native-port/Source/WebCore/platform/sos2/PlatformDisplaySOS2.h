#pragma once

#include "PlatformDisplay.h"

namespace WebCore {

// The shared display of a page with a GPU: Mesa's surfaceless EGL platform over
// the page's virgl context, which the SDK's libdrm shim serves. Pages without a
// GPU never create it, so WebCore's accelerated paths stay off for them.
class PlatformDisplaySOS2 final : public PlatformDisplay {
public:
    // Creates the display and makes it the shared one. False when EGL cannot
    // start; nothing is shared then.
    WEBCORE_EXPORT static bool initialize();
    ~PlatformDisplaySOS2();

private:
    explicit PlatformDisplaySOS2(Ref<GLDisplay> &&);

    Type type() const final { return Type::Surfaceless; }
};

} // namespace WebCore
