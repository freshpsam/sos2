#pragma once
#include <WebCore/ResourceRequestBase.h>

namespace WebCore {
struct ResourceRequestPlatformData {
    ResourceRequestBase::RequestData requestData;
};

using ResourceRequestData = Variant<ResourceRequestBase::RequestData, ResourceRequestPlatformData>;

class ResourceRequest : public ResourceRequestBase {
public:
    ResourceRequest()
        : ResourceRequestBase(URL(), ResourceRequestCachePolicy::UseProtocolCachePolicy) { }

    explicit ResourceRequest(String &&url) : ResourceRequest(URL({}, WTF::move(url))) { }

    ResourceRequest(URL &&url)
        : ResourceRequestBase(WTF::move(url), ResourceRequestCachePolicy::UseProtocolCachePolicy) {
    }

    ResourceRequest(
        URL &&url, const String &referrer,
        ResourceRequestCachePolicy policy = ResourceRequestCachePolicy::UseProtocolCachePolicy)
        : ResourceRequestBase(WTF::move(url), policy) {
        setHTTPReferrer(referrer);
    }

    ResourceRequest(ResourceRequestBase &&base) : ResourceRequestBase(WTF::move(base)) { }

    explicit ResourceRequest(ResourceRequestPlatformData &&data)
        : ResourceRequestBase(WTF::move(data.requestData)) { }

    bool encodingRequiresPlatformData() const { return false; }

    WEBCORE_EXPORT void updateFromDelegatePreservingOldProperties(const ResourceRequest &);

    static ResourceRequest fromResourceRequestData(ResourceRequestData &&data) {
        if (std::holds_alternative<RequestData>(data))
            return ResourceRequest(WTF::move(std::get<RequestData>(data)));
        return ResourceRequest(WTF::move(std::get<ResourceRequestPlatformData>(data)));
    }

    ResourceRequestData getRequestDataToSerialize() const { return m_requestData; }

private:
    friend class ResourceRequestBase;

    void doUpdatePlatformRequest() { }

    void doUpdateResourceRequest() { }

    void doUpdatePlatformHTTPBody() { }

    void doUpdateResourceHTTPBody() { }

    void doPlatformSetAsIsolatedCopy(const ResourceRequest &) { }
};
}

namespace WTF {
template <> struct MarkableTraits<WebCore::ResourceRequest> {
    static bool isEmptyValue(const WebCore::ResourceRequest &request) { return request.isNull(); }

    static WebCore::ResourceRequest emptyValue() { return {}; }
};
}
