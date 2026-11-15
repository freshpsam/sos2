#pragma once
#include <string>
#include <string_view>

namespace WTF {
class URL;
}

namespace WebCore {
struct PageConfiguration;
class ResourceRequest;
}

namespace SOS2 {
void configureStorage(WebCore::PageConfiguration &);
std::string requestCookies(const WebCore::ResourceRequest &, bool crossSiteRedirect);
void responseCookie(const WebCore::ResourceRequest &, std::string_view, bool crossSiteRedirect);
bool differentCookieSites(const WTF::URL &, const WTF::URL &);
}
