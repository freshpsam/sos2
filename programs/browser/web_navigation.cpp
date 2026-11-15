#include "config.h"
#include "web_content.h"
#include "web_browser.h"
#include <WebCore/Document.h>
#include <WebCore/DocumentView.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/DocumentWriter.h>
#include <WebCore/EmptyFrameLoaderClient.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/FrameLoadRequest.h>
#include <WebCore/FrameLoaderTypes.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/MIMETypeRegistry.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/SubstituteData.h>
#include <wtf/RunLoop.h>
#include <cstring>

namespace SOS2 {
using namespace WebCore;

class NativeFrameLoaderClient final : public EmptyFrameLoaderClient {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NativeFrameLoaderClient);
    LocalFrame &frame;

public:
    NativeFrameLoaderClient(LocalFrame &frame, FrameLoader &loader)
        : EmptyFrameLoaderClient(loader), frame(frame) { }

    bool hasWebView() const final { return true; }

    bool canHandleRequest(const ResourceRequest &request) const final {
        // blob: documents are loaded by BlobRegistryImpl's ResourceHandle.
        auto &url = request.url();
        return url.protocolIsInHTTPFamily() || url.protocolIsData() || url.protocolIsBlob() ||
               url.isAboutBlank();
    }

    bool canShowMIMEType(const String &type) const final {
        return MIMETypeRegistry::isSupportedNonImageMIMEType(type) ||
               MIMETypeRegistry::isSupportedImageMIMEType(type);
    }

    bool canShowMIMETypeAsHTML(const String &type) const final { return type == "text/html"_s; }

    String userAgent(const URL &) const final {
        // Match the compatibility tokens in the pinned WebKit GLib/WPE port
        // (platform/glib/UserAgentGLib.cpp), retaining our application identity.
        // Missing Version/ or advertising ARM with Safari makes Google select
        // its obsolete WAP tier and reject searches before scripts can run.
        return "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_13_4) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/60.5 Safari/605.1.15 SOS2/1.0"_s;
    }

    void dispatchDecidePolicyForNavigationAction(
        const NavigationAction &, const ResourceRequest &request, const ResourceResponse &,
        FormState *, const String &, std::optional<NavigationIdentifier>,
        std::optional<HitTestResult> &&, bool, NavigationUpgradeToHTTPSBehavior, SandboxFlags,
        PolicyDecisionMode, FramePolicyFunction &&callback) final {
        callback(canHandleRequest(request) ? PolicyAction::Use : PolicyAction::Ignore);
    }

    void dispatchDecidePolicyForNewWindowAction(const NavigationAction &, const ResourceRequest &,
                                                FormState *, const String &,
                                                std::optional<HitTestResult> &&,
                                                FramePolicyFunction &&callback) final {
        callback(PolicyAction::Ignore);
    }

    void dispatchDecidePolicyForResponse(const ResourceResponse &response, const ResourceRequest &,
                                         const String &, FramePolicyFunction &&callback) final {
        callback(canShowMIMEType(response.mimeType()) ? PolicyAction::Use : PolicyAction::Ignore);
    }

    void transitionToCommittedForNewPage(InitializingIframe) final {
        auto size = frame.view() ? frame.view()->size() : IntSize(480, 752);
        frame.createView(size, std::nullopt, IntSize());
    }

    void committedLoad(DocumentLoader *loader, const SharedBuffer &data) final {
        loader->commitData(data);
    }

    // DocumentLoader ends its writer after this callback.
    void finishedLoading(DocumentLoader *) final { }

    void dispatchDidFinishLoad() final {
        if (frame.isMainFrame()) browserCommitted(*frame.document());
    }

    void loadError(const ResourceError &error) {
        if (error.isCancellation()) return;
        if (frame.isMainFrame()) browserFailed(error.failingURL());
        RunLoop::mainSingleton().dispatch([frame = Ref{frame},
                                           document = RefPtr{frame.document()}] {
            // A newer navigation owns the frame. Never replace it with an old error.
            if (frame->loader().provisionalDocumentLoader() || frame->document() != document.get())
                return;
            const char *html =
                "<!doctype html><title>Website unavailable</title><body style='font:20px sans-serif;margin:24px'><h1>Website unavailable</h1><p>The connection failed or the website's certificate could not be verified.</p><p>Check the address, network connection and system clock, then try again.</p>";
            auto content = SharedBuffer::create(
                std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(html), strlen(html)));
            URL blank("about:blank"_s);
            ResourceResponse response(URL(blank), "text/html"_s, content->size(), "UTF-8"_s);
            SubstituteData substitute(WTF::move(content), URL(), WTF::move(response),
                                      SubstituteData::SessionHistoryVisibility::Hidden);
            frame->loader().load(FrameLoadRequest(frame.get(), ResourceRequest(WTF::move(blank)),
                                                  WTF::move(substitute)));
        });
    }

    void dispatchDidFailProvisionalLoad(const ResourceError &error, WillContinueLoading,
                                        WillInternallyHandleFailure) final {
        loadError(error);
    }

    void dispatchDidFailLoad(const ResourceError &error) final { loadError(error); }
};

void configureNavigation(PageConfiguration &configuration) {
    std::get<PageConfiguration::LocalMainFrameCreationParameters>(
        configuration.mainFrameCreationParameters)
        .clientCreator = [](LocalFrame &frame,
                            FrameLoader &loader) -> UniqueRef<LocalFrameLoaderClient> {
        return makeUniqueRefWithoutRefCountedCheck<NativeFrameLoaderClient>(frame, loader);
    };
}
}
