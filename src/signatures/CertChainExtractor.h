#pragma once

#include <cstdint>
#include <vector>

// Forward-declare OpenSSL types to keep heavy headers out of the .h.
struct pkcs7_st;
typedef struct pkcs7_st PKCS7;

namespace pdfe::signatures {

/**
 * @brief Extracts the X.509 certificate chain from a PKCS#7 SignedData blob.
 *
 * Pure parsing utility — no network access, no trust evaluation. Used by the
 * long-term-validation (LTV) pipeline to gather the certs that need to be
 * embedded in the document's /DSS dictionary.
 *
 * NOTE (meta-audit F20, gap PDF-34): this comment used to name an ETSI
 * baseline profile. PDFE reaches none of them — it emits
 * `adbe.pkcs7.detached`, never `ETSI.CAdES.detached`, and the PKCS#7 container
 * does not satisfy CAdES-BES. /DSS embedding is ISO 32000-2 §12.8.4.3
 * validation data on its own terms. Profile support is stated in exactly one
 * place: docs/signature-profile-support.md (guarded by
 * tests/test_pades_profiles.cpp).
 *
 * Chain length 0..N is supported. Each output entry is the DER encoding of a
 * single X.509 certificate, suitable for writing as a PDF stream object.
 */
class CertChainExtractor {
public:
    /// Parse a raw PKCS#7 SignedData buffer (DER bytes) and return the chain.
    /// On parse failure or empty input, returns an empty vector.
    static std::vector<std::vector<uint8_t>> extractChain(
        const uint8_t* pkcs7Der, size_t pkcs7DerLen);

    /// Same as above, but operating on an already-parsed PKCS7 structure.
    /// Caller retains ownership of @p p7.
    static std::vector<std::vector<uint8_t>> extractChain(PKCS7* p7);
};

} // namespace pdfe::signatures
