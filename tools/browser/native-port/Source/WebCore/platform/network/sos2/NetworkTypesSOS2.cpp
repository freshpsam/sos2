#include "config.h"
#include "ResourceRequest.h"
#include "FormData.h"
#include "NetworkStateNotifier.h"
#include <sos2/network.h>
#include <wtf/RunLoop.h>

namespace WebCore {
void NetworkStateNotifier::updateStateWithoutNotifying() {
    user_net_info info{};
    m_isOnLine = !sos2_net_info(&info) && (info.flags & NET_ADDRESS);
}

void NetworkStateNotifier::startObserving() {
    static auto *timer = new Timer([this] { updateState(); });
    timer->startRepeating(1_s);
}

void ResourceRequest::updateFromDelegatePreservingOldProperties(const ResourceRequest &request) {
    auto oldPriority = priority();
    RefPtr<FormData> body = httpBody();
    bool hidden = hiddenFromInspector();
    auto oldRequester = requester();
    auto initiator = initiatorIdentifier();
    auto inspectorNode = inspectorInitiatorNodeIdentifier();
    *this = request;
    setPriority(oldPriority);
    setHTTPBody(WTF::move(body));
    setHiddenFromInspector(hidden);
    setRequester(oldRequester);
    setInitiatorIdentifier(initiator);
    if (inspectorNode) setInspectorInitiatorNodeIdentifier(*inspectorNode);
}
}
