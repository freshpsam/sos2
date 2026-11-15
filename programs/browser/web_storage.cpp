#include "config.h"
#include "web_state.h"
#include "web_storage.h"
#include <WebCore/CookieJar.h>
#include <WebCore/Cookie.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/Page.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/PublicSuffixStore.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/Storage.h>
#include <WebCore/StorageArea.h>
#include <WebCore/StorageEventDispatcher.h>
#include <WebCore/StorageMap.h>
#include <WebCore/StorageNamespace.h>
#include <WebCore/StorageNamespaceProvider.h>
#include <WebCore/StorageSessionProvider.h>
#include <WebCore/StorageType.h>
#include <pal/SessionID.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Function.h>
#include <wtf/text/CString.h>
#include <map>
#include <ctime>
#include <sys/stat.h>

namespace SOS2 {
using namespace WebCore;

namespace {
std::string utf8(const String &value) {
    auto bytes = value.utf8();
    return {bytes.data(), bytes.length()};
}

std::u16string utf16(const String &value) {
    std::u16string result(value.length(), u'\0');
    for (unsigned i = 0; i < value.length(); ++i) result[i] = value[i];
    return result;
}

String string(const std::u16string &value) {
    return String(std::span<const char16_t>(value.data(), value.size()));
}

State::Address address(const URL &url) {
    if (!url.isValid() || !url.protocolIsInHTTPFamily()) return {};
    State::Address result;
    result.scheme = utf8(url.protocol().toString());
    result.host = utf8(url.host().toString());
    result.port = url.port().value_or(url.protocolIs("https"_s) ? 443 : 80);
    result.path = utf8(url.path().toString());
    if (result.path.empty()) result.path = "/";
    return result;
}

State::Store &store() {
    static auto *instance = [] {
        mkdir("/data/browser", 0700);
        return new State::Store("/data/browser/state.sqlite3");
    }();
    return *instance;
}

State::CookieContext documentContext(const Document &document, const URL &url) {
    State::CookieContext context;
    context.url = address(url);
    context.firstParty = address(document.topOrigin().toURL());
    context.dom = true;
    context.sameSite = State::sameSite(context.url, context.firstParty);
    return context;
}

State::CookieContext requestContext(const ResourceRequest &request, bool crossSiteRedirect) {
    State::CookieContext context;
    context.url = address(request.url());
    context.firstParty = address(request.firstPartyForCookies());
    context.topLevel = request.isTopSite();
    context.credentials = request.allowCookies();
    if (!context.firstParty.valid() && context.topLevel) context.firstParty = context.url;
    context.sameSite = request.isSameSite() && !crossSiteRedirect;
    auto method = request.httpMethod();
    context.safeMethod =
        method == "GET"_s || method == "HEAD"_s || method == "OPTIONS"_s || method == "TRACE"_s;
    return context;
}

class NoStorageSession final : public StorageSessionProvider {
    NetworkStorageSession *storageSession() const final { return nullptr; }
};

class NativeCookieJar final : public CookieJar {
public:
    NativeCookieJar() : CookieJar(adoptRef(*new NoStorageSession)) { }

    String cookies(Document &document, const URL &url) const final {
        return String::fromUTF8(store().cookies(documentContext(document, url), time(nullptr)));
    }

    void setCookies(Document &document, const URL &url, const String &value) final {
        store().setCookie(documentContext(document, url), utf8(value), time(nullptr));
    }

    bool cookiesEnabled(Document &document) final {
        return store().available() && address(document.cookieURL()).valid();
    }

    void remoteCookiesEnabled(const Document &,
                              CompletionHandler<void(bool)> &&completion) const final {
        completion(false);
    }

    std::pair<String, SecureCookiesAccessed>
    cookieRequestHeaderFieldValue(const URL &firstParty, const SameSiteInfo &sameSite,
                                  const URL &url, std::optional<FrameIdentifier>,
                                  std::optional<PageIdentifier>, IncludeSecureCookies) const final {
        State::CookieContext context;
        context.url = address(url);
        context.firstParty = address(firstParty);
        context.sameSite = sameSite.isSameSite;
        context.topLevel = sameSite.isTopSite;
        context.safeMethod = sameSite.isSafeHTTPMethod;
        return {String::fromUTF8(store().cookies(context, time(nullptr))),
                context.url.secure() ? SecureCookiesAccessed::Yes : SecureCookiesAccessed::No};
    }

    // The asynchronous Cookie Store/inspector APIs are not exposed by this port.
    bool getRawCookies(Document &, const URL &, Vector<Cookie> &) const final { return false; }

    void setRawCookie(const Document &, const Cookie &, ShouldPartitionCookie) final { }

    void deleteCookie(const Document &, const URL &, const String &,
                      CompletionHandler<void()> &&completion) final {
        completion();
    }

    void getCookiesAsync(
        Document &, const URL &, const CookieStoreGetOptions &,
        CompletionHandler<void(std::optional<Vector<Cookie>> &&)> &&completion) const final {
        completion(std::nullopt);
    }

    void setCookieAsync(Document &, const URL &, const Cookie &,
                        CompletionHandler<void(bool)> &&completion) const final {
        completion(false);
    }
};

size_t volatileBytes = 0;

class NativeArea final : public StorageArea {
    Ref<SecurityOrigin> origin;
    StorageType type;
    StorageMap map;
    std::string identity;
    size_t bytes = 0;
    bool ready, loaded = false;
    int64_t version = 0;

    bool persistent() const { return type == StorageType::Local; }

    bool load() {
        State::Items entries;
        if (!store().read(identity, entries)) return false;
        StorageMap next(map.quota());
        size_t total = 0;
        for (auto &[key, value] : entries) {
            next.setItemIgnoringQuota(string(key), string(value));
            total += (key.size() + value.size()) * 2;
        }
        map = std::move(next);
        bytes = total;
        return true;
    }

    // Pages share local storage: reload this origin's items after a page,
    // possibly another one, changed them. Reads may lag by up to 250 ms, but
    // a change starts from what is stored. False until the items were read.
    bool refresh(State::Store::Look look = State::Store::Look::Recent) {
        if (!ready || !persistent()) return ready;
        auto current = store().version(identity, look);
        if ((!loaded || current != version) && load()) {
            version = current;
            loaded = true;
        }
        return loaded;
    }

    // This page's own change is already in the map.
    void stored() { version = store().version(identity, State::Store::Look::Cached); }

    void changed(LocalFrame &source, const String &key, const String &oldValue,
                 const String &newValue) {
        auto *document = source.document();
        auto *page = source.page();
        if (!document || !page) return;
        Function<bool(Storage &)> isSource = [&](Storage &storage) {
            return storage.frame() == &source || &storage.area() != this;
        };
        if (type == StorageType::Session)
            StorageEventDispatcher::dispatchSessionStorageEvents(
                key, oldValue, newValue, *page, origin, document->url().string(), isSource);
        else
            StorageEventDispatcher::dispatchLocalStorageEvents(
                key, oldValue, newValue, nullptr, origin, document->url().string(), isSource);
    }

public:
    NativeArea(const SecurityOrigin &origin, StorageType type, unsigned quota,
               bool disabled = false)
        : origin(origin.isolatedCopy()), type(type),
          map(std::min<unsigned>(quota, State::originQuota)),
          identity(address(origin.toURL()).origin()),
          ready(!disabled && quota && !origin.isOpaque() && !identity.empty()) {
        refresh(State::Store::Look::Now);
    }

    ~NativeArea() {
        if (!persistent()) volatileBytes -= bytes;
    }

    unsigned length() final {
        refresh();
        return map.length();
    }

    String key(unsigned index) final {
        refresh();
        return map.key(index);
    }

    String item(const String &key) final {
        refresh();
        return map.getItem(key);
    }

    bool contains(const String &key) final {
        refresh();
        return map.contains(key);
    }

    StorageType storageType() const final { return type; }

    size_t memoryBytesUsedByCache() final { return bytes; }

    void setItem(LocalFrame &source, const String &key, const String &value,
                 bool &quotaException) final {
        quotaException = !refresh(State::Store::Look::Now);
        if (quotaException) return;
        auto old = map.getItem(key);
        if (!old.isNull() && old == value) return;
        size_t oldBytes = old.isNull() ? 0 : (key.length() + size_t(old.length())) * 2;
        size_t newBytes = (key.length() + size_t(value.length())) * 2;
        if (newBytes > State::originQuota || bytes - oldBytes + newBytes > State::originQuota ||
            (old.isNull() && map.length() >= 4096) ||
            (!persistent() && volatileBytes - oldBytes + newBytes > State::profileQuota)) {
            quotaException = true;
            return;
        }
        StorageMap next = map;
        String ignored;
        next.setItem(key, value, ignored, quotaException);
        if (quotaException) return;
        if (persistent()) {
            if (!store().set(identity, utf16(key), utf16(value))) {
                quotaException = true;
                return;
            }
            stored();
        } else volatileBytes = volatileBytes - oldBytes + newBytes;
        bytes = bytes - oldBytes + newBytes;
        map = std::move(next);
        changed(source, key, old, value);
    }

    void removeItem(LocalFrame &source, const String &key) final {
        if (!refresh(State::Store::Look::Now)) return;
        auto old = map.getItem(key);
        if (old.isNull()) return;
        if (persistent()) {
            if (!store().remove(identity, utf16(key))) return;
            stored();
        }
        size_t size = (key.length() + size_t(old.length())) * 2;
        bytes -= size;
        if (!persistent()) volatileBytes -= size;
        String ignored;
        map.removeItem(key, ignored);
        changed(source, key, old, nullString());
    }

    void clear(LocalFrame &source) final {
        if (!refresh(State::Store::Look::Now) || !map.length()) return;
        if (persistent()) {
            if (!store().clear(identity)) return;
            stored();
        } else volatileBytes -= bytes;
        bytes = 0;
        map.clear();
        changed(source, nullString(), nullString(), nullString());
    }

    Ref<NativeArea> clone() {
        auto result = adoptRef(*new NativeArea(origin, type, map.quota(), !ready));
        if (!persistent() && volatileBytes + bytes <= State::profileQuota) {
            result->map = map;
            result->bytes = bytes;
            volatileBytes += bytes;
        }
        return result;
    }
};

class NativeNamespace final : public StorageNamespace {
    StorageType type;
    unsigned quota;
    PAL::SessionID session;
    RefPtr<SecurityOrigin> top;
    std::map<std::string, Ref<NativeArea>> areas;

public:
    NativeNamespace(StorageType type, unsigned quota, PAL::SessionID session,
                    RefPtr<SecurityOrigin> top = nullptr)
        : type(type), quota(quota), session(session), top(std::move(top)) { }

    Ref<StorageArea> storageArea(const SecurityOrigin &origin) final {
        auto key = utf8(origin.toRawString());
        auto at = areas.find(key);
        if (at != areas.end()) return at->second.copyRef();
        auto area = adoptRef(*new NativeArea(origin, type, quota, areas.size() >= 256));
        if (areas.size() < 256) areas.emplace(std::move(key), area.copyRef());
        return area;
    }

    const SecurityOrigin *topLevelOrigin() const final { return top.get(); }

    PAL::SessionID sessionID() const final { return session; }

    void setSessionIDForTesting(PAL::SessionID value) final { session = value; }

    uint64_t storageAreaMapCountForTesting() const final { return areas.size(); }

    Ref<StorageNamespace> copy(Page &) final {
        auto result = adoptRef(*new NativeNamespace(type, quota, session, top));
        for (auto &[key, area] : areas) result->areas.emplace(key, area->clone());
        return result;
    }
};

class NativeStorageProvider final : public StorageNamespaceProvider {
    std::map<std::pair<Page *, std::string>, Ref<StorageNamespace>> sessions;

    Ref<StorageNamespace> createLocalStorageNamespace(unsigned quota,
                                                      PAL::SessionID session) final {
        return adoptRef(*new NativeNamespace(StorageType::Local, quota, session));
    }

    Ref<StorageNamespace> createTransientLocalStorageNamespace(SecurityOrigin &top, unsigned quota,
                                                               PAL::SessionID session) final {
        return adoptRef(
            *new NativeNamespace(StorageType::TransientLocal, quota, session, top.isolatedCopy()));
    }

public:
    RefPtr<StorageNamespace> sessionStorageNamespace(const SecurityOrigin &top, Page &page,
                                                     ShouldCreateNamespace create) final {
        auto key = std::make_pair(&page, utf8(top.toRawString()));
        auto at = sessions.find(key);
        if (at != sessions.end()) return at->second.copyRef();
        if (create == ShouldCreateNamespace::No) return nullptr;
        auto space = adoptRef(*new NativeNamespace(
            StorageType::Session, sessions.size() < 256 ? sessionStorageQuota() : 0,
            page.sessionID(), top.isolatedCopy()));
        if (sessions.size() < 256) sessions.emplace(std::move(key), space.copyRef());
        return space;
    }
};
}

void configureStorage(PageConfiguration &configuration) {
    configuration.cookieJar = adoptRef(*new NativeCookieJar);
    configuration.storageNamespaceProvider = adoptRef(*new NativeStorageProvider);
}

std::string requestCookies(const ResourceRequest &request, bool crossSiteRedirect) {
    return store().cookies(requestContext(request, crossSiteRedirect), time(nullptr));
}

void responseCookie(const ResourceRequest &request, std::string_view cookie,
                    bool crossSiteRedirect) {
    store().setCookie(requestContext(request, crossSiteRedirect), cookie, time(nullptr));
}

bool differentCookieSites(const URL &a, const URL &b) {
    return !State::sameSite(address(a), address(b));
}
}

namespace WebCore {
// Keep WebCore's SameSite/document.domain decisions and the cookie jar on the
// same pinned rules, including private registries and reserved local origins.
bool PublicSuffixStore::platformIsPublicSuffix(StringView domain) const {
    auto bytes = domain.convertToASCIILowercase().utf8();
    return SOS2::State::publicSuffix({bytes.data(), bytes.length()});
}

String PublicSuffixStore::platformTopPrivatelyControlledDomain(StringView host) const {
    auto bytes = host.utf8();
    std::string_view domain(bytes.data(), bytes.length());
    while (domain.starts_with('.')) domain.remove_prefix(1);
    if (domain.empty() || SOS2::State::publicSuffix(domain)) return emptyString();
    return String::fromUTF8(SOS2::State::registrableDomain(domain));
}
}
