#include "config.h"
#include "CryptoAlgorithmRegistry.h"
#include "CryptoAlgorithmSHA1.h"
#include "CryptoAlgorithmSHA256.h"
#include "CryptoAlgorithmSHA384.h"
#include "CryptoAlgorithmSHA512.h"
#include "CryptoKeyEC.h"
#include "CryptoKeyRSA.h"
#include "CryptoKeyRSAComponents.h"
#include "ExceptionOr.h"

namespace WebCore {
void CryptoAlgorithmRegistry::platformRegisterAlgorithms() {
    // These use the native PAL digest implementation. Unregistered ciphers and
    // asymmetric algorithms reject Web Crypto operations with NotSupportedError.
    registerAlgorithm<CryptoAlgorithmSHA1>();
    registerAlgorithm<CryptoAlgorithmSHA256>();
    registerAlgorithm<CryptoAlgorithmSHA384>();
    registerAlgorithm<CryptoAlgorithmSHA512>();
}

bool CryptoKeyEC::platformSupportedCurve(NamedCurve) { return false; }

std::optional<CryptoKeyPair> CryptoKeyEC::platformGeneratePair(CryptoAlgorithmIdentifier,
                                                               NamedCurve, bool,
                                                               CryptoKeyUsageBitmap) {
    return std::nullopt;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportRaw(CryptoAlgorithmIdentifier, NamedCurve,
                                                   Vector<uint8_t> &&, bool, CryptoKeyUsageBitmap) {
    return nullptr;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportJWKPublic(CryptoAlgorithmIdentifier, NamedCurve,
                                                         Vector<uint8_t> &&, Vector<uint8_t> &&,
                                                         bool, CryptoKeyUsageBitmap) {
    return nullptr;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportJWKPrivate(CryptoAlgorithmIdentifier, NamedCurve,
                                                          Vector<uint8_t> &&, Vector<uint8_t> &&,
                                                          Vector<uint8_t> &&, bool,
                                                          CryptoKeyUsageBitmap) {
    return nullptr;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportSpki(CryptoAlgorithmIdentifier, NamedCurve,
                                                    Vector<uint8_t> &&, bool,
                                                    CryptoKeyUsageBitmap) {
    return nullptr;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportPkcs8(CryptoAlgorithmIdentifier, NamedCurve,
                                                     Vector<uint8_t> &&, bool,
                                                     CryptoKeyUsageBitmap) {
    return nullptr;
}

Vector<uint8_t> CryptoKeyEC::platformExportRaw() const { return {}; }

Vector<uint8_t> CryptoKeyEC::platformExportSpki() const { return {}; }

Vector<uint8_t> CryptoKeyEC::platformExportPkcs8() const { return {}; }

bool CryptoKeyEC::platformAddFieldElements(JsonWebKey &) const { return false; }

CryptoKeyRSA::CryptoKeyRSA(CryptoAlgorithmIdentifier identifier, CryptoAlgorithmIdentifier hash,
                           bool hasHash, CryptoKeyType type, PlatformRSAKeyContainer &&key,
                           bool extractable, CryptoKeyUsageBitmap usage)
    : CryptoKey(identifier, type, extractable, usage), m_platformKey(WTF::move(key)),
      m_restrictedToSpecificHash(hasHash), m_hash(hash) { }

bool CryptoKeyRSA::isRestrictedToHash(CryptoAlgorithmIdentifier &identifier) const {
    identifier = m_hash;
    return m_restrictedToSpecificHash;
}

size_t CryptoKeyRSA::keySizeInBits() const { return 0; }

auto CryptoKeyRSA::algorithm() const -> KeyAlgorithm { RELEASE_ASSERT_NOT_REACHED(); }

RefPtr<CryptoKeyRSA> CryptoKeyRSA::create(CryptoAlgorithmIdentifier, CryptoAlgorithmIdentifier,
                                          bool, const CryptoKeyRSAComponents &, bool,
                                          CryptoKeyUsageBitmap) {
    return nullptr;
}

void CryptoKeyRSA::generatePair(CryptoAlgorithmIdentifier, CryptoAlgorithmIdentifier, bool,
                                unsigned, const Vector<uint8_t> &, bool, CryptoKeyUsageBitmap,
                                KeyPairCallback &&, VoidCallback &&failure,
                                ScriptExecutionContext *) {
    failure();
}

RefPtr<CryptoKeyRSA> CryptoKeyRSA::importSpki(CryptoAlgorithmIdentifier,
                                              std::optional<CryptoAlgorithmIdentifier>,
                                              Vector<uint8_t> &&, bool, CryptoKeyUsageBitmap) {
    return nullptr;
}

RefPtr<CryptoKeyRSA> CryptoKeyRSA::importPkcs8(CryptoAlgorithmIdentifier,
                                               std::optional<CryptoAlgorithmIdentifier>,
                                               Vector<uint8_t> &&, bool, CryptoKeyUsageBitmap) {
    return nullptr;
}

ExceptionOr<Vector<uint8_t>> CryptoKeyRSA::exportSpki() const {
    return Exception{ExceptionCode::NotSupportedError};
}

ExceptionOr<Vector<uint8_t>> CryptoKeyRSA::exportPkcs8() const {
    return Exception{ExceptionCode::NotSupportedError};
}

std::unique_ptr<CryptoKeyRSAComponents> CryptoKeyRSA::exportData() const { return nullptr; }
}
