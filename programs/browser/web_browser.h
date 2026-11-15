#pragma once
#include <wtf/URL.h>

namespace WebCore {
class Page;
class Document;
}

namespace SOS2 {
WTF::URL initialBrowserURL();
void browserCommitted(WebCore::Document &);
void browserFailed(const WTF::URL &);
void handleBrowserCommands(WebCore::Page &);
void publishBrowserState(WebCore::Page &);
}
