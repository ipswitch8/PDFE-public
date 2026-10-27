#include "CertChainExtractor.h"

#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/safestack.h>

#include <cstring>

namespace pdfe::signatures {

namespace {

// Append the DER encoding of @p cert to @p out. No-op on null/encode failure.
void appendCertDer(X509* cert, std::vector<std::vector<uint8_t>>& out)
{
    if (!cert) return;
    unsigned char* der = nullptr;
    int len = i2d_X509(cert, &der);
    if (len <= 0 || !der) {
        if (der) OPENSSL_free(der);
        return;
    }
    out.emplace_back(der, der + len);
    OPENSSL_free(der);
}

} // namespace

std::vector<std::vector<uint8_t>> CertChainExtractor::extractChain(
    const uint8_t* pkcs7Der, size_t pkcs7DerLen)
{
    if (!pkcs7Der || pkcs7DerLen == 0) return {};

    const unsigned char* p = pkcs7Der;
    PKCS7* p7 = d2i_PKCS7(nullptr, &p, static_cast<long>(pkcs7DerLen));
    if (!p7) return {};

    auto result = extractChain(p7);
    PKCS7_free(p7);
    return result;
}

std::vector<std::vector<uint8_t>> CertChainExtractor::extractChain(PKCS7* p7)
{
    std::vector<std::vector<uint8_t>> result;
    if (!p7) return result;

    // PKCS7_get0_signers gives us the actual signer cert(s) — typically one.
    STACK_OF(X509)* signers = PKCS7_get0_signers(p7, nullptr, 0);

    // PKCS7_get0_certificates: all certs embedded in the SignedData.certificates
    // field. This is the canonical source for the full chain. PKCS7_get0_signers
    // intersects against signer-info, so we use the union: signers first
    // (preserves "leaf is index 0" convention), then any remaining intermediates.
    STACK_OF(X509)* allCerts = nullptr;
    if (PKCS7_type_is_signed(p7) && p7->d.sign) {
        allCerts = p7->d.sign->cert;
    }

    // Track what we've already added by pointer identity (cheap dedup).
    auto alreadyAdded = [&result](X509* cert) -> bool {
        if (!cert) return true;
        unsigned char* der = nullptr;
        int len = i2d_X509(cert, &der);
        if (len <= 0 || !der) {
            if (der) OPENSSL_free(der);
            return false;
        }
        bool found = false;
        for (const auto& existing : result) {
            if (existing.size() == static_cast<size_t>(len) &&
                std::memcmp(existing.data(), der, len) == 0) {
                found = true;
                break;
            }
        }
        OPENSSL_free(der);
        return found;
    };

    // 1) Signers first — leaf cert(s).
    if (signers) {
        for (int i = 0; i < sk_X509_num(signers); ++i) {
            X509* cert = sk_X509_value(signers, i);
            if (cert && !alreadyAdded(cert)) {
                appendCertDer(cert, result);
            }
        }
    }

    // 2) Then any other certs (intermediates / root).
    if (allCerts) {
        for (int i = 0; i < sk_X509_num(allCerts); ++i) {
            X509* cert = sk_X509_value(allCerts, i);
            if (cert && !alreadyAdded(cert)) {
                appendCertDer(cert, result);
            }
        }
    }

    if (signers) sk_X509_free(signers);
    return result;
}

} // namespace pdfe::signatures
