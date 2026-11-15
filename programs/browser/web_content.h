#pragma once
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <wtf/Vector.h>
#include <wtf/URL.h>

namespace SOS2 {
WTF::URL entryURL();
bool isLocalURL(const WTF::URL &);
WebCore::ResourceError readLocalResource(const WebCore::ResourceRequest &,
                                         WebCore::ResourceResponse &, WTF::Vector<uint8_t> &);
}
