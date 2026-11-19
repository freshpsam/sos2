#include "config.h"
#include "http_client.h"
#include "web_content.h"
#include "web_storage.h"
#include <WebCore/FormData.h>
#include <WebCore/DNS.h>
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/HTTPParsers.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/NetworkingContext.h>
#include <WebCore/ResourceHandle.h>
#include <WebCore/SecurityOrigin.h>
#include "platform/network/ResourceHandleInternal.h"
#include "platform/WebCorePersistentCoders.h"
#include <WebCore/ResourceHandleClient.h>
#include <WebCore/ResourceError.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/Timer.h>
#include <wtf/CompletionHandler.h>
#include <wtf/text/CString.h>
#include <wtf/persistence/PersistentEncoder.h>
#include <wtf/persistence/PersistentDecoder.h>
#include <memory>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace WebCore {
namespace {
class NetworkLoad;
std::unordered_map<ResourceHandle *, std::shared_ptr<NetworkLoad>> loads;
unsigned activeLoads = 0;

bool allowed(const URL &url) {
    return url.isValid() && url.protocolIsInHTTPFamily() && url.user().isEmpty() &&
           url.password().isEmpty() && !url.host().endsWithIgnoringASCIICase(".sos2.local"_s) &&
           url.string().length() <= 8192 && portAllowed(url) && !isIPAddressDisallowed(url);
}

std::string text(const String &value) {
    auto utf8 = value.utf8();
    return {utf8.data(), utf8.length()};
}

struct CacheEntry {
    std::string key, url;
    ResourceResponse response;
    Vector<uint8_t> body;
    MonotonicTime stored;
    WallTime wall;
    Seconds age, lifetime;
};

// A conservative private HTTP cache, bounded to eight 256 KiB decoded bodies.
// Explicit freshness only: no stale reuse, credentialled responses or Vary.
// WebCore retains responsibility for CSP/CORS and response-policy decisions.
std::vector<CacheEntry> cache;
constexpr size_t cacheBodyLimit = 256 * 1024;

void expireCache() {
    auto mono = MonotonicTime::now();
    auto wall = WallTime::now();
    std::erase_if(cache, [&](auto &entry) {
        auto resident = mono - entry.stored;
        return entry.age + resident >= entry.lifetime ||
               std::abs(((wall - entry.wall) - resident).value()) > 2;
    });
}

class NetworkLoad : public std::enable_shared_from_this<NetworkLoad> {
public:
    ResourceHandle *handle;
    ResourceRequest request;
    Timer timer;
    bool deferred = false, cancelled = false, waiting = false;
    bool crossSiteRedirect = false;
    unsigned redirects = 0;
    MonotonicTime created = MonotonicTime::now();
    // The 120 s cap starts with the first transport or cache hit. Waiting in
    // the six-load queue is not charged: loads behind slow ones must still start.
    std::optional<MonotonicTime> started;
    std::unique_ptr<SOS2::HTTP::Client> transport;
    bool receivedResponse = false;
    std::string cacheKey;
    std::optional<CacheEntry> cached, saving;

    NetworkLoad(ResourceHandle &handle, bool deferred)
        : handle(&handle), request(handle.firstRequest()), timer([this] { tick(); }),
          deferred(deferred) { }

    ~NetworkLoad() { releaseTransport(); }

    void releaseTransport() {
        if (transport) {
            transport.reset();
            --activeLoads;
        }
    }

    void stop() {
        cancelled = true;
        timer.stop();
        releaseTransport();
        cached.reset();
        saving.reset();
    }

    void fail(const String &message, int code = 1) {
        stop();
        if (handle->client())
            handle->client()->didFail(handle, {"SOS2Network"_s, code, request.url(), message});
    }

    bool begin() {
        if (!allowed(request.url())) {
            fail("Unsupported network URL"_s);
            return false;
        }
        SOS2::HTTP::Request native;
        auto &url = request.url();
        native.tls = url.protocolIs("https"_s);
        native.host = text(url.host().toString());
        native.port = url.port().value_or(native.tls ? 443 : 80);
        native.method = text(request.httpMethod());
        native.target = text(url.path().toString());
        if (native.target.empty()) native.target = "/";
        if (url.hasQuery()) native.target += "?" + text(url.query().toString());
        for (auto &header : request.httpHeaderFields())
            if (!equalIgnoringASCIICase(header.key, "Cookie"_s))
                native.headers.emplace_back(text(header.key), text(header.value));
        auto cookies = SOS2::requestCookies(request, crossSiteRedirect);
        if (!cookies.empty()) native.headers.emplace_back("Cookie", std::move(cookies));
        expireCache();
        cacheKey.clear();
        bool cacheable = native.method == "GET" && !request.httpBody();
        for (auto &header : native.headers) {
            auto name = SOS2::HTTP::lower(header.first);
            if (name == "cookie" || name == "authorization" || name == "range" ||
                name == "cache-control" || name == "pragma" || name.starts_with("if-"))
                cacheable = false;
        }
        auto policy = request.cachePolicy();
        if (policy != ResourceRequestCachePolicy::UseProtocolCachePolicy &&
            policy != ResourceRequestCachePolicy::ReturnCacheDataElseLoad &&
            policy != ResourceRequestCachePolicy::ReturnCacheDataDontLoad)
            cacheable = false;
        if (native.method != "GET" && native.method != "HEAD") cache.clear();
        else if (!cacheable) {
            auto url = text(request.url().string());
            std::erase_if(cache, [&](auto &entry) { return entry.url == url; });
        }
        if (cacheable) {
            cacheKey = text(request.url().string()) + '\n' +
                       text(request.firstPartyForCookies().string()) + '\n';
            auto headers = native.headers;
            for (auto &header : headers) header.first = SOS2::HTTP::lower(header.first);
            std::sort(headers.begin(), headers.end());
            for (auto &header : headers) cacheKey += header.first + ": " + header.second + '\n';
            for (auto at = cache.begin(); at != cache.end(); ++at)
                if (at->key == cacheKey) {
                    cached = *at;
                    auto recent = std::move(*at);
                    cache.erase(at);
                    cache.push_back(std::move(recent));
                    cached->response.setSource(ResourceResponse::Source::MemoryCache);
                    auto age = cached->age + (MonotonicTime::now() - cached->stored);
                    cached->response.setHTTPHeaderField(
                        HTTPHeaderName::Age, String::number(static_cast<unsigned>(age.value())));
                    receivedResponse = false;
                    return true;
                }
        }
        if (policy == ResourceRequestCachePolicy::ReturnCacheDataDontLoad) {
            fail("No fresh cached response available"_s);
            return false;
        }
        if (RefPtr body = request.httpBody()) {
            for (auto &element : body->elements())
                if (!std::holds_alternative<Vector<uint8_t>>(element.data)) {
                    fail("File and blob uploads are unavailable"_s);
                    return false;
                }
            if (body->lengthInBytes() > SOS2::HTTP::maximumBody) {
                fail("Upload exceeds resource limit"_s);
                return false;
            }
            auto bytes = body->flatten();
            native.body.assign(bytes.begin(), bytes.end());
        }
        receivedResponse = false;
        transport = std::make_unique<SOS2::HTTP::Client>(std::move(native));
        ++activeLoads;
        return true;
    }

    void tick() {
        auto self = shared_from_this();
        Ref protectedHandle{*handle};
        if (cancelled) return;
        if (!handle->client()) {
            stop();
            return;
        }
        if (started && MonotonicTime::now() - *started > 120_s) {
            fail("Resource load timed out"_s, -4);
            return;
        }
        if (deferred || waiting) return;
        if (!transport && !cached) {
            if (activeLoads >= 6) return;
            if (!begin()) return;
            if (!started) started = MonotonicTime::now();
        }
        if (cached) {
            if (!receivedResponse) {
                waiting = true;
                receivedResponse = true;
                handle->checkTAO(cached->response);
                handle->didReceiveResponse(ResourceResponse(cached->response), [self] {
                    if (!self->cancelled) self->waiting = false;
                });
                return;
            }
            auto data = SharedBuffer::create(WTF::move(cached->body));
            cached.reset();
            if (data->size()) handle->client()->didReceiveBuffer(handle, data.get(), data->size());
            if (cancelled || !handle->client()) return;
            cancelled = true;
            timer.stop();
            NetworkLoadMetrics metrics;
            metrics.complete = true;
            handle->client()->didFinishLoading(handle, metrics);
            return;
        }
        transport->poll();
        if (!transport->error().empty()) {
            auto message = String::fromUTF8(transport->error());
            int code = transport->errorCode();
            fail(message, code);
            return;
        }
        if (receivedResponse) {
            auto chunk = transport->takeData();
            if (!chunk.empty()) {
                if (saving) {
                    if (chunk.size() > cacheBodyLimit - saving->body.size()) saving.reset();
                    else saving->body.append(std::span<const uint8_t>(chunk.data(), chunk.size()));
                }
                Vector<uint8_t> bytes;
                bytes.append(std::span<const uint8_t>(chunk.data(), chunk.size()));
                auto data = SharedBuffer::create(WTF::move(bytes));
                handle->client()->didReceiveBuffer(handle, data.get(), data->size());
                if (cancelled || !handle->client()) return;
            }
            if (transport->done()) {
                if (saving) {
                    // Include download time in response age. Only completed,
                    // successfully decoded bodies can enter the cache.
                    auto now = MonotonicTime::now();
                    saving->age += now - saving->stored;
                    saving->stored = now;
                    saving->wall = WallTime::now();
                    if (saving->age < saving->lifetime) {
                        std::erase_if(cache, [&](auto &entry) { return entry.key == saving->key; });
                        if (cache.size() == 8) cache.erase(cache.begin());
                        cache.push_back(WTF::move(*saving));
                    }
                    saving.reset();
                }
                releaseTransport();
                cancelled = true;
                timer.stop();
                NetworkLoadMetrics metrics;
                metrics.complete = true;
                handle->client()->didFinishLoading(handle, metrics);
            }
            return;
        }
        if (!transport->headersReady()) return;
        auto &result = transport->response();
        // Each Set-Cookie is independent: commas can be part of an Expires date.
        for (auto &header : result.headers)
            if (header.first == "set-cookie")
                SOS2::responseCookie(request, header.second, crossSiteRedirect);
        auto type = String::fromUTF8(result.header("content-type"));
        String mime = extractMIMETypeFromMediaType(type);
        if (mime.isEmpty()) mime = "application/octet-stream"_s;
        ResourceResponse response(URL(request.url()), WTF::move(mime), -1,
                                  extractCharsetFromMediaType(type).toString());
        response.setHTTPStatusCode(result.status);
        response.setHTTPStatusText(String::fromUTF8(result.reason));
        response.setHTTPVersion(result.http2 ? "HTTP/2"_s : "HTTP/1.1"_s);
        response.setSource(ResourceResponse::Source::Network);
        for (auto &h : result.headers)
            response.addHTTPHeaderField(String::fromUTF8(h.first), String::fromUTF8(h.second));
        if (request.url().protocolIs("https"_s)) response.setCertificateInfo(CertificateInfo(true));
        if (!cacheKey.empty() && result.status == 200 && response.date() &&
            response.cacheControlMaxAge() &&
            (response.httpHeaderField(HTTPHeaderName::Age).isEmpty() || response.age()) &&
            !response.cacheControlContainsNoCache() && !response.cacheControlContainsNoStore() &&
            response.httpHeaderField(HTTPHeaderName::Vary).isEmpty() &&
            response.httpHeaderField(HTTPHeaderName::SetCookie).isEmpty()) {
            auto now = MonotonicTime::now();
            auto wall = WallTime::now();
            auto age =
                std::max(wall - *response.date(), response.age().value_or(0_s) + (now - created));
            auto lifetime = std::min(*response.cacheControlMaxAge(), 60_s);
            if (age < lifetime)
                saving = CacheEntry{
                    cacheKey, text(request.url().string()), response, {}, now, wall, age, lifetime};
        }
        handle->checkTAO(response);
        auto location = result.header("location");
        if ((result.status == 301 || result.status == 302 || result.status == 303 ||
             result.status == 307 || result.status == 308) &&
            !location.empty()) {
            if (++redirects > 10) {
                fail("Too many redirects"_s);
                return;
            }
            ResourceRequest next = request;
            URL target(request.url(), String::fromUTF8(location));
            if (!allowed(target) ||
                (request.url().protocolIs("https"_s) && !target.protocolIs("https"_s))) {
                fail("Unsafe redirect target"_s);
                return;
            }
            crossSiteRedirect =
                crossSiteRedirect || SOS2::differentCookieSites(request.url(), target);
            next.setURL(URL(target));
            next.redirectAsGETIfNeeded(request, response);
            if (!protocolHostAndPortAreEqual(request.url(), target)) {
                next.clearHTTPAuthorization();
                next.removeHTTPHeaderField(HTTPHeaderName::Cookie);
                handle->markAsHavingCrossOriginRedirect();
            }
            handle->incrementRedirectCount();
            waiting = true;
            releaseTransport();
            // WebCore applies fetch redirect mode, CORS, CSP and referrer policy
            // before it approves a new request. Never follow a redirect internally.
            handle->client()->willSendRequestAsync(handle, WTF::move(next), WTF::move(response),
                                                   [self](ResourceRequest &&approved) {
                                                       if (self->cancelled) return;
                                                       if (approved.isNull()) {
                                                           self->stop();
                                                           return;
                                                       }
                                                       self->request = WTF::move(approved);
                                                       self->waiting = false;
                                                   });
            return;
        }
        waiting = true;
        receivedResponse = true;
        handle->didReceiveResponse(WTF::move(response), [self]() {
            if (self->cancelled || !self->handle->client()) return;
            self->waiting = false;
        });
    }
};
}

ResourceHandle::~ResourceHandle() { cancel(); }

ResourceHandleInternal::~ResourceHandleInternal() = default;

bool ResourceHandle::start() {
    if (loads.size() >= 128) {
        scheduleFailure(BlockedFailure);
        return true;
    }
    auto load = std::make_shared<NetworkLoad>(*this, d->m_defersLoading);
    loads.emplace(this, load);
    load->timer.startRepeating(10_ms);
    return true;
}

void ResourceHandle::cancel() {
    d->m_failureTimer.stop();
    auto at = loads.find(this);
    if (at != loads.end()) {
        at->second->stop();
        loads.erase(at);
    }
    d->m_client = nullptr;
}

void ResourceHandle::platformSetDefersLoading(bool value) {
    auto at = loads.find(this);
    if (at != loads.end()) at->second->deferred = value;
}

void ResourceHandle::platformLoadResourceSynchronously(NetworkingContext *,
                                                       const ResourceRequest &request,
                                                       StoredCredentialsPolicy, SecurityOrigin *,
                                                       ResourceError &error, ResourceResponse &,
                                                       Vector<uint8_t> &data) {
    data.clear();
    error = {"SOS2Network"_s, 1, request.url(), "Synchronous network requests are unavailable"_s};
}

void ResourceHandle::receivedCredential(const AuthenticationChallenge &, const Credential &) {
    scheduleFailure(BlockedFailure);
}

void ResourceHandle::receivedRequestToContinueWithoutCredential(const AuthenticationChallenge &) {
    scheduleFailure(BlockedFailure);
}

void ResourceHandle::receivedCancellation(const AuthenticationChallenge &) { cancel(); }

void ResourceHandle::receivedRequestToPerformDefaultHandling(const AuthenticationChallenge &) {
    scheduleFailure(BlockedFailure);
}

void ResourceHandle::receivedChallengeRejection(const AuthenticationChallenge &) {
    scheduleFailure(BlockedFailure);
}
}

namespace WTF::Persistence {
// A past verification result is never a persistent TLS identity. SOS2 has no
// certificate inspection/archive format yet; disk data cannot restore trust.
void Coder<WebCore::CertificateInfo>::encodeForPersistence(Encoder &encoder,
                                                           const WebCore::CertificateInfo &) {
    encoder << uint8_t(0);
}

std::optional<WebCore::CertificateInfo>
Coder<WebCore::CertificateInfo>::decodeForPersistence(Decoder &decoder) {
    std::optional<uint8_t> version;
    decoder >> version;
    if (!version || *version) return std::nullopt;
    return WebCore::CertificateInfo();
}
}
