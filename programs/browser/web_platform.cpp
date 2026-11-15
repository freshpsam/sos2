/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
// SOS2 platform adapter: private writable sites and native HTTP(S) transport.
// Derived from WebKit's platform adapter scaffolding; engine code is unchanged.

#include <WebCore/BlobRegistry.h>
#include <WebCore/Color.h>
#include <WebCore/LoaderStrategy.h>
#include <WebCore/MediaStrategy.h>
#include <WebCore/PasteboardItemInfo.h>
#include <WebCore/PasteboardStrategy.h>
#include <WebCore/PlatformStrategies.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SubresourceLoader.h>
#include <WebCore/BlobRegistryImpl.h>
#include <WebCore/PolicyContainer.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/NetworkStateNotifier.h>
#include <mutex>
#include <algorithm>
#include <vector>
#include <wtf/RunLoop.h>
#include "web_content.h"
#include <wtf/URL.h>
#include <wtf/text/WTFString.h>
#include <sos2/network.h>

namespace SOS2 {

using namespace WebCore;

namespace {

// WebCore keeps its normal resource, CORS, CSP, decoding and cancellation paths.
// Network requests use ResourceHandle so normal redirects and policy callbacks run.
class LocalLoaderStrategy final : public LoaderStrategy {
    static ResourceError unavailable() {
        return {"SOS2"_s, 1, URL(), "External resource loading is not available"_s};
    }

    std::vector<RefPtr<SubresourceLoader>> pending;
    bool suspended = false;

    void serve(RefPtr<SubresourceLoader> loader) {
        auto at = std::find(pending.begin(), pending.end(), loader);
        if (at == pending.end()) return;
        if (loader->reachedTerminalState()) {
            pending.erase(at);
            return;
        }
        if (suspended || loader->defersLoading()) return;
        pending.erase(at);
        if (loader->request().url().protocolIsData() || !isLocalURL(loader->request().url())) {
            // Use WebKit's data-URL parser and loader, including its MIME checks.
            loader->start();
            return;
        }
        ResourceResponse response;
        Vector<uint8_t> bytes;
        auto error = readLocalResource(loader->request(), response, bytes);
        if (!error.isNull()) {
            loader->didFail(error);
            return;
        }
        loader->deliverResponseAndData(WTF::move(response), SharedBuffer::create(WTF::move(bytes)));
    }

    void schedule(RefPtr<SubresourceLoader> loader) {
        RunLoop::mainSingleton().dispatch(
            [this, loader = WTF::move(loader)]() mutable { serve(WTF::move(loader)); });
    }

public:
    void loadResource(LocalFrame &frame, CachedResource &resource, ResourceRequest &&request,
                      const ResourceLoaderOptions &options,
                      CompletionHandler<void(RefPtr<SubresourceLoader> &&)> &&completion) final {
        // blob: URLs are served by BlobRegistryImpl's ResourceHandle through
        // loader->start(); without them Blob reads and object URLs fail.
        auto &url = request.url();
        if (pending.size() >= 256 ||
            (!url.protocolIsData() && !url.protocolIsBlob() && !url.protocolIsInHTTPFamily()) ||
            url.string().length() > 12 * 1024 * 1024) {
            completion(nullptr);
            return;
        }
        SubresourceLoader::create(
            frame, resource, WTF::move(request), options,
            [this, completion = WTF::move(completion)](RefPtr<SubresourceLoader> &&loader) mutable {
                RefPtr retained = loader;
                if (retained) pending.push_back(retained);
                completion(WTF::move(loader));
                if (retained) schedule(WTF::move(retained));
            });
    }

    void loadResourceSynchronously(FrameLoader &, ResourceLoaderIdentifier, const ResourceRequest &,
                                   ClientCredentialPolicy, const FetchOptions &,
                                   const HTTPHeaderMap &, ResourceError &error, ResourceResponse &,
                                   Vector<uint8_t> &) final {
        error = unavailable();
    }

    void pageLoadCompleted(Page &) final { }

    void browsingContextRemoved(LocalFrame &) final { }

    void remove(ResourceLoader *loader) final {
        std::erase_if(pending, [loader](auto &item) { return item.get() == loader; });
    }

    void setDefersLoading(ResourceLoader &loader, bool defers) final {
        if (!defers)
            for (auto &item : pending)
                if (item.get() == &loader) {
                    schedule(item);
                    break;
                }
    }

    void crossOriginRedirectReceived(ResourceLoader *, const URL &) final { }

    void servePendingRequests(ResourceLoadPriority) final {
        for (auto &item : pending) schedule(item);
    }

    void suspendPendingRequests() final { suspended = true; }

    void resumePendingRequests() final {
        suspended = false;
        servePendingRequests(ResourceLoadPriority::Low);
    }

    void startPingLoad(LocalFrame &, ResourceRequest &, const HTTPHeaderMap &, const FetchOptions &,
                       ContentSecurityPolicyImposition,
                       PingLoadCompletionHandler &&completion) final {
        if (completion) completion(unavailable(), {});
    }

    void preconnectTo(FrameLoader &, ResourceRequest &&, StoredCredentialsPolicy,
                      ShouldPreconnectAsFirstParty,
                      PreconnectCompletionHandler &&completion) final {
        if (completion) completion(unavailable());
    }

    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }

    bool isOnLine() const final {
        user_net_info info{};
        return !sos2_net_info(&info) && (info.flags & NET_ADDRESS);
    }

    void addOnlineStateChangeListener(Function<void(bool)> &&listener) final {
        NetworkStateNotifier::singleton().addListener(WTF::move(listener));
    }

    void isResourceLoadFinished(CachedResource &,
                                CompletionHandler<void(bool)> &&completion) final {
        completion(true);
    }

    ResourceError cancelledError(const ResourceRequest &request) const final {
        return {"SOS2"_s, 3, request.url(), "Load cancelled"_s, ResourceError::Type::Cancellation};
    }

    ResourceError blockedError(const ResourceRequest &) const final { return unavailable(); }

    bool isBlockedError(const ResourceError &) const final { return false; }

    ResourceError blockedByContentBlockerError(const ResourceRequest &) const final {
        return unavailable();
    }

    ResourceError cannotShowURLError(const ResourceRequest &) const final { return unavailable(); }

    ResourceError interruptedForPolicyChangeError(const ResourceRequest &request) const final {
        return cancelledError(request);
    }
#if ENABLE(CONTENT_FILTERING)
    ResourceError blockedByContentFilterError(const ResourceRequest &) const final {
        return unavailable();
    }
#endif
    ResourceError cannotShowMIMETypeError(const ResourceResponse &) const final {
        return unavailable();
    }

    ResourceError fileDoesNotExistError(const ResourceResponse &) const final {
        return unavailable();
    }

    ResourceError httpsUpgradeRedirectLoopError(const ResourceRequest &) const final {
        return unavailable();
    }

    ResourceError httpNavigationWithHTTPSOnlyError(const ResourceRequest &) const final {
        return unavailable();
    }

    bool isHttpNavigationWithHTTPSOnlyError(const ResourceError &) const final { return false; }

    ResourceError pluginWillHandleLoadError(const ResourceResponse &) const final {
        return unavailable();
    }
};

class LocalPasteboardStrategy final : public PasteboardStrategy {
public:
#if PLATFORM(IOS_FAMILY)
    void writeToPasteboard(const PasteboardURL &, const String &, const PasteboardContext *) final {
    }

    void writeToPasteboard(const PasteboardWebContent &, const String &,
                           const PasteboardContext *) final { }

    void writeToPasteboard(const PasteboardImage &, const String &,
                           const PasteboardContext *) final { }

    void writeToPasteboard(const String &, const String &, const String &,
                           const PasteboardContext *) final { }

    void updateSupportedTypeIdentifiers(const Vector<String> &, const String &,
                                        const PasteboardContext *) final { }
#endif
#if PLATFORM(COCOA)
    void getTypes(Vector<String> &, const String &, const PasteboardContext *) final { }

    RefPtr<SharedBuffer> bufferForType(const String &, const String &,
                                       const PasteboardContext *) final {
        return nullptr;
    }

    void getPathnamesForType(Vector<String> &, const String &, const String &,
                             const PasteboardContext *) final { }

    String stringForType(const String &, const String &, const PasteboardContext *) final {
        return {};
    }

    Vector<String> allStringsForType(const String &, const String &,
                                     const PasteboardContext *) final {
        return {};
    }

    int64_t changeCount(const String &, const PasteboardContext *) final { return 0; }

    Color color(const String &, const PasteboardContext *) final { return {}; }

    URL url(const String &, const PasteboardContext *) final { return {}; }

    int getNumberOfFiles(const String &, const PasteboardContext *) final { return 0; }

    int64_t addTypes(const Vector<String> &, const String &, const PasteboardContext *) final {
        return 0;
    }

    int64_t setTypes(const Vector<String> &, const String &, const PasteboardContext *) final {
        return 0;
    }

    int64_t setBufferForType(SharedBuffer *, const String &, const String &,
                             const PasteboardContext *) final {
        return 0;
    }

    int64_t setURL(const PasteboardURL &, const String &, const PasteboardContext *) final {
        return 0;
    }

    int64_t setColor(const Color &, const String &, const PasteboardContext *) final { return 0; }

    int64_t setStringForType(const String &, const String &, const String &,
                             const PasteboardContext *) final {
        return 0;
    }

    int64_t writeWebArchive(LegacyWebArchive &, const String &) final { return 0; }

    bool containsURLStringSuitableForLoading(const String &, const PasteboardContext *) final {
        return false;
    }

    String urlStringSuitableForLoading(const String &, String &, const PasteboardContext *) final {
        return {};
    }
#endif
    String readStringFromPasteboard(size_t, const String &, const String &,
                                    const PasteboardContext *) final {
        return {};
    }

    RefPtr<SharedBuffer> readBufferFromPasteboard(std::optional<size_t>, const String &,
                                                  const String &, const PasteboardContext *) final {
        return nullptr;
    }

    URL readURLFromPasteboard(size_t, const String &, String &, const PasteboardContext *) final {
        return {};
    }

    std::optional<PasteboardItemInfo> informationForItemAtIndex(size_t, const String &, int64_t,
                                                                const PasteboardContext *) final {
        return std::nullopt;
    }

    std::optional<Vector<PasteboardItemInfo>>
    allPasteboardItemInfo(const String &, int64_t, const PasteboardContext *) final {
        return std::nullopt;
    }

    int getPasteboardItemsCount(const String &, const PasteboardContext *) final { return 0; }

    Vector<String> typesSafeForDOMToReadAndWrite(const String &, const String &,
                                                 const PasteboardContext *) final {
        return {};
    }

    int64_t writeCustomData(const Vector<PasteboardCustomData> &, const String &,
                            const PasteboardContext *) final {
        return 0;
    }

    bool containsStringSafeForDOMToReadForType(const String &, const String &,
                                               const PasteboardContext *) final {
        return false;
    }
#if PLATFORM(GTK) || PLATFORM(WPE)
    Vector<String> types(const String &) final { return {}; }

    String readTextFromClipboard(const String &, const String &) final { return {}; }

    Vector<String> readFilePathsFromClipboard(const String &) final { return {}; }

    RefPtr<SharedBuffer> readBufferFromClipboard(const String &, const String &) final {
        return nullptr;
    }

    void writeToClipboard(const String &, SelectionData &&) final { }

    void clearClipboard(const String &) final { }

    int64_t changeCount(const String &) final { return 0; }
#elif USE(LIBWPE)
    void getTypes(Vector<String> &) final { }

    void writeToPasteboard(const PasteboardWebContent &) final { }

    void writeToPasteboard(const String &, const String &) final { }
#endif
};

class LocalBlobRegistry final : public BlobRegistry {
    BlobRegistryImpl registry;

public:
    void registerInternalFileBlobURL(const URL &u, Ref<BlobDataFileReference> &&f, const String &,
                                     const String &type) final {
        registry.registerInternalFileBlobURL(u, WTF::move(f), type);
    }

    void registerInternalBlobURL(const URL &u, Vector<BlobPart> &&data, const String &type) final {
        registry.registerInternalBlobURL(u, WTF::move(data), type);
    }

    void registerBlobURL(const URL &u, const URL &source, const PolicyContainer &policy,
                         const std::optional<SecurityOriginData> &origin) final {
        registry.registerBlobURL(u, source, policy, origin);
    }

    void registerInternalBlobURLOptionallyFileBacked(const URL &u, const URL &source,
                                                     RefPtr<BlobDataFileReference> &&f,
                                                     const String &type) final {
        registry.registerInternalBlobURLOptionallyFileBacked(u, source, WTF::move(f), type,
                                                             PolicyContainer());
    }

    void registerInternalBlobURLForSlice(const URL &u, const URL &source, long long start,
                                         long long end, const String &type) final {
        registry.registerInternalBlobURLForSlice(u, source, start, end, type);
    }

    void unregisterBlobURL(const URL &u, const std::optional<SecurityOriginData> &origin) final {
        registry.unregisterBlobURL(u, origin);
    }

    void registerBlobURLHandle(const URL &u,
                               const std::optional<SecurityOriginData> &origin) final {
        registry.registerBlobURLHandle(u, origin);
    }

    void unregisterBlobURLHandle(const URL &u,
                                 const std::optional<SecurityOriginData> &origin) final {
        registry.unregisterBlobURLHandle(u, origin);
    }

    String blobType(const URL &u) final { return registry.blobType(u); }

    unsigned long long blobSize(const URL &u) final { return registry.blobSize(u); }

    void writeBlobsToTemporaryFilesForIndexedDB(
        const Vector<String> &, CompletionHandler<void(Vector<String> &&)> &&completion) final {
        completion({});
    }

    BlobRegistryImpl *blobRegistryImpl() final { return &registry; }
};

class LocalMediaStrategy final : public MediaStrategy {
public:
#if ENABLE(WEB_AUDIO)
    Ref<AudioDestination> createAudioDestination(const AudioDestinationCreationOptions &) final {
        RELEASE_ASSERT_NOT_REACHED();
    }
#endif
};

class LocalPlatformStrategies final : public PlatformStrategies {
private:
    LoaderStrategy *createLoaderStrategy() final { return new LocalLoaderStrategy; }

    PasteboardStrategy *createPasteboardStrategy() final { return new LocalPasteboardStrategy; }

    MediaStrategy *createMediaStrategy() final { return new LocalMediaStrategy; }

    BlobRegistry *createBlobRegistry() final { return new LocalBlobRegistry; }
#if ENABLE(DECLARATIVE_WEB_PUSH)
    PushStrategy *createPushStrategy() final { return nullptr; }
#endif
};

} // anonymous namespace

void installPlatformStrategies() {
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] { setPlatformStrategies(new LocalPlatformStrategies); });
}

} // namespace SOS2
