#include "config.h"
#include "Cookie.h"
#include "NetworkStorageSession.h"
#include "NetworkingContext.h"
#include "ResourceError.h"
#include <wtf/CompletionHandler.h>

namespace WebCore {
// Cookies remain disabled until an origin-aware storage provider is installed.
std::pair<String, bool> NetworkStorageSession::cookiesForDOM(
    const URL &, const SameSiteInfo &, const URL &, std::optional<FrameIdentifier>,
    std::optional<PageIdentifier>, IncludeSecureCookies, ApplyTrackingPrevention,
    ShouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker) const {
    return {};
}

std::pair<String, bool> NetworkStorageSession::cookieRequestHeaderFieldValue(
    const URL &, const SameSiteInfo &, const URL &, std::optional<FrameIdentifier>,
    std::optional<PageIdentifier>, IncludeSecureCookies, ApplyTrackingPrevention,
    ShouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker) const {
    return {};
}

void NetworkStorageSession::setCookiesFromDOM(
    const URL &, const SameSiteInfo &, const URL &, std::optional<FrameIdentifier>,
    std::optional<PageIdentifier>, ApplyTrackingPrevention, RequiresScriptTrackingPrivacy,
    const String &, ShouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker) const { }

bool NetworkStorageSession::getRawCookies(const URL &, const SameSiteInfo &, const URL &,
                                          std::optional<FrameIdentifier>,
                                          std::optional<PageIdentifier>, ApplyTrackingPrevention,
                                          ShouldRelaxThirdPartyCookieBlocking,
                                          Vector<Cookie> &) const {
    return false;
}

void NetworkStorageSession::setCookie(const Cookie &) { }

void NetworkStorageSession::deleteCookie(const URL &, const URL &, const String &,
                                         CompletionHandler<void()> &&completion) const {
    completion();
}
}
