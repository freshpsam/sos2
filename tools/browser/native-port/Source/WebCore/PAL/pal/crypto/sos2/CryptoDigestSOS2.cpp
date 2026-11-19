#include "config.h"
#include "CryptoDigest.h"
#define MBEDTLS_CONFIG_FILE "sos2/mbedtls_config.h"
#include <mbedtls/md.h>

namespace PAL {
struct CryptoDigestContext {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CryptoDigestContext);
    mbedtls_md_context_t value;
    unsigned digestSize{0};

    CryptoDigestContext() { mbedtls_md_init(&value); }

    ~CryptoDigestContext() { mbedtls_md_free(&value); }
};

CryptoDigest::CryptoDigest() = default;
CryptoDigest::~CryptoDigest() = default;

std::unique_ptr<CryptoDigest> CryptoDigest::create(Algorithm algorithm) {
    mbedtls_md_type_t type;
    switch (algorithm) {
        case Algorithm::SHA_1: type = MBEDTLS_MD_SHA1; break;
        case Algorithm::DEPRECATED_SHA_224: type = MBEDTLS_MD_SHA224; break;
        case Algorithm::SHA_256: type = MBEDTLS_MD_SHA256; break;
        case Algorithm::SHA_384: type = MBEDTLS_MD_SHA384; break;
        case Algorithm::SHA_512: type = MBEDTLS_MD_SHA512; break;
        default: return nullptr;
    }
    auto result = makeUnique<CryptoDigest>();
    result->m_context = makeUnique<CryptoDigestContext>();
    const auto *info = mbedtls_md_info_from_type(type);
    if (!info || mbedtls_md_setup(&result->m_context->value, info, 0) ||
        mbedtls_md_starts(&result->m_context->value))
        return nullptr;
    result->m_context->digestSize = mbedtls_md_get_size(info);
    return result;
}

void CryptoDigest::addBytes(std::span<const uint8_t> bytes) {
    RELEASE_ASSERT(!mbedtls_md_update(&m_context->value, bytes.data(), bytes.size()));
}

Vector<uint8_t> CryptoDigest::computeHash() {
    Vector<uint8_t> result(m_context->digestSize);
    RELEASE_ASSERT(!mbedtls_md_finish(&m_context->value, result.mutableSpan().data()));
    return result;
}
}
