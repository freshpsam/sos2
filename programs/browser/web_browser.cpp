#include "config.h"
#include "web_browser.h"
#include "web_content.h"
#include <WebCore/Document.h>
#include <WebCore/DocumentView.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/FrameLoadRequest.h>
#include <WebCore/FrameDestructionObserverInlines.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/Page.h>
#include <sos2/services.h>
#include <wtf/text/CString.h>
#include <wtf/Vector.h>
#include <algorithm>
#include <cstring>

namespace SOS2 {
using namespace WTF;
static String failedURL;
// Addresses only: each renderer owns its history, with no saved documents or
// page images. Back reloads the target through the normal navigation path.
static Vector<URL> history;
static String pendingBack;
static URL firstURL;
static bool forcePublish = false;
static user_browser_state previous{};

static bool valid(const URL &url) {
    return url.isValid() && (url.protocolIsInHTTPFamily() || url.isAboutBlank()) &&
           url.user().isEmpty() && url.password().isEmpty() &&
           url.string().utf8().length() <= BROWSER_URL_MAX;
}

URL initialBrowserURL() {
    URL result("about:blank"_s);
    user_browser_command command{};
    if (sos2_browser_command(&command) > 0 && command.action == BROWSER_NAVIGATE) {
        URL candidate(String::fromUTF8(command.url));
        if (valid(candidate)) result = WTF::move(candidate);
        else {
            failedURL = String::fromUTF8(command.url);
            result = URL("about:blank"_s);
        }
    }
    firstURL = result;
    return result;
}

void browserFailed(const URL &url) {
    failedURL = url.string();
    pendingBack = {};
}

void browserCommitted(WebCore::Document &document) {
    if (!firstURL.isValid()) return;
    const auto &url = document.url();
    // An internal failure document must not replace the last requested address.
    if (url.isAboutBlank() && !failedURL.isEmpty()) return;
    failedURL = {};
    if (!valid(url)) return;
    if (firstURL.isValid() &&
        (!history.isEmpty() || !url.isAboutBlank() || firstURL.isAboutBlank())) {
        auto *loader = document.frame() ? document.frame()->loader().documentLoader() : nullptr;
        if (!pendingBack.isEmpty() && loader && loader->originalURL().string() == pendingBack &&
            !history.isEmpty())
            history.last() = url; // A redirect while going back replaces that address.
        else if (history.isEmpty() || history.last() != url) history.append(url);
        pendingBack = {};
    }
}

static URL displayedURL(WebCore::LocalFrame &frame) {
    auto *provisional = frame.loader().provisionalDocumentLoader();
    if (provisional && !provisional->url().isAboutBlank()) return provisional->url();
    return failedURL.isEmpty() ? frame.document()->url() : URL(failedURL);
}

static bool canGoBack(const URL &current) {
    if (history.isEmpty()) return false;
    return history.size() > 1 || (pendingBack.isEmpty() && current != history.last());
}

void handleBrowserCommands(WebCore::Page &page) {
    user_browser_command command{};
    if (sos2_browser_command(&command) <= 0) return;
    forcePublish = true;
    auto frame = page.localMainFrame();
    if (!frame || !frame->document()) return;
    URL target;
    if (command.action == BROWSER_BACK) {
        auto current = displayedURL(*frame);
        if (!canGoBack(current)) return;
        // An unfinished/failed new navigation has not entered the history yet.
        // Return to its predecessor without discarding that predecessor too.
        if (!pendingBack.isEmpty() || current == history.last()) history.removeLast();
        target = history.last();
        pendingBack = target.string();
    } else if (command.action == BROWSER_NAVIGATE) {
        target = URL(String::fromUTF8(command.url));
        if (!valid(target)) return;
        pendingBack = {};
    } else return;
    failedURL = {};
    frame->loader().load(
        WebCore::FrameLoadRequest(*frame, WebCore::ResourceRequest(WTF::move(target))));
}

void publishBrowserState(WebCore::Page &page) {
    auto frame = page.localMainFrame();
    if (!frame || !frame->document()) return;
    user_browser_state state{};
    URL url = displayedURL(*frame);
    if (canGoBack(url)) state.flags |= BROWSER_CAN_GO_BACK;
    auto encoded = url.string().utf8(), title = frame->document()->title().utf8();
    if (encoded.length() <= BROWSER_URL_MAX) memcpy(state.url, encoded.data(), encoded.length());
    size_t length = std::min<size_t>(title.length(), BROWSER_TITLE_MAX);
    memcpy(state.title, title.data(), length);
    if (forcePublish || memcmp(&previous, &state, sizeof(state))) {
        if (!sos2_browser_state(&state)) {
            previous = state;
            forcePublish = false;
        }
    }
}
}
