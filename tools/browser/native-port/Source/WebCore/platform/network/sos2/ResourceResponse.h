#pragma once
#include <WebCore/ResourceResponseBase.h>

namespace WebCore {
class ResourceResponse : public ResourceResponseBase {
public:
    ResourceResponse() = default;

    ResourceResponse(URL &&url, String &&mimeType, long long expectedLength, String &&encoding)
        : ResourceResponseBase(WTF::move(url), WTF::move(mimeType), expectedLength,
                               WTF::move(encoding)) { }

    ResourceResponse(ResourceResponseBase &&base) : ResourceResponseBase(WTF::move(base)) { }

    String platformSuggestedFilename() const { return {}; }

private:
    friend class ResourceResponseBase;

    void doUpdateResourceResponse() { }
};
}
