#include "config.h"
#include "PlatformScreen.h"
#include "DestinationColorSpace.h"
#include "FloatRect.h"
// Last: the native ABI header defines generic macros (FILE_READ, SYS_*).
#include <user_abi.h>

namespace WebCore {
// The screen is the page viewport below the desktop header, so screen.height,
// device-height media queries and innerHeight agree.
int screenDepth(Widget *) { return 24; }

int screenDepthPerComponent(Widget *) { return 8; }

bool screenIsMonochrome(Widget *) { return false; }

bool screenHasInvertedColors() { return false; }

FloatRect screenRect(Widget *) { return {0, 0, WEB_WIDTH, WEB_HEIGHT}; }

FloatRect screenAvailableRect(Widget *) { return {0, 0, WEB_WIDTH, WEB_HEIGHT}; }

bool screenSupportsExtendedColor(Widget *) { return false; }

DestinationColorSpace screenColorSpace(Widget *) { return DestinationColorSpace::SRGB(); }
}
