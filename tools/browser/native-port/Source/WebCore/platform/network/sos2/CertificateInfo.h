#pragma once
#include <WebCore/CertificateSummary.h>
#include <optional>

namespace WebCore {
class ResourceResponse;
class ResourceError;

// Transport verification status. Certificate inspection UI is not implemented.
class CertificateInfo {
public:
    CertificateInfo() = default;

    explicit CertificateInfo(bool verified) : m_verified(verified) { }

    explicit CertificateInfo(const ResourceResponse &) { }

    explicit CertificateInfo(const ResourceError &) { }

    CertificateInfo isolatedCopy() const { return *this; }

    bool isEmpty() const { return !m_verified; }

    bool containsNonRootSHA1SignedCertificate() const { return false; }

    std::optional<CertificateSummary> summary() const { return std::nullopt; }

    bool operator==(const CertificateInfo &) const = default;

private:
    bool m_verified = false;
};
}
