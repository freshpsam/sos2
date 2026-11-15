#pragma once
#include <WebCore/ResourceErrorBase.h>
#include <WebCore/CertificateInfo.h>

namespace WebCore {
class ResourceError : public ResourceErrorBase {
public:
    ResourceError(Type type = Type::Null) : ResourceErrorBase(type) { }

    ResourceError(const String &domain, int code, const URL &url, const String &description,
                  Type type = Type::General, IsSanitized sanitized = IsSanitized::No)
        : ResourceErrorBase(domain, code, url, description, type, sanitized) { }

    ErrorRecoveryMethod errorRecoveryMethod() const { return ErrorRecoveryMethod::NoRecovery; }

private:
    friend class ResourceErrorBase;

    void doPlatformIsolatedCopy(const ResourceError &) { }
};
}
