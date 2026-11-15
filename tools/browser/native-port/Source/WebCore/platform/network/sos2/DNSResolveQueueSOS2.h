#pragma once
#include "DNSResolveQueue.h"
#include <wtf/CompletionHandler.h>

namespace WebCore {
// The local-page runtime has no network transport. Resolve requests fail
// explicitly instead of inventing addresses or delegating to a host resolver.
class DNSResolveQueuePlatform final : public DNSResolveQueue {
public:
    void resolve(const String &, uint64_t, DNSCompletionHandler &&completion) final {
        completion(makeUnexpected(DNSError::CannotResolve));
    }

    void stopResolve(uint64_t) final { }

private:
    void updateIsUsingProxy() final { m_isUsingProxy = true; }

    void platformResolve(const String &) final { decrementRequestCount(); }
};
}
