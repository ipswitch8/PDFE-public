#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pdfe::signatures {

/// Parsed contents of an OCSP response loaded from disk. Populated by
/// OcspResponseParser::parse(). The raw DER bytes are retained verbatim so
/// they can be embedded into a /DSS /OCSPs stream without re-encoding.
///
/// Fields default to "unknown" / zero so a parser that succeeds at the
/// outer ASN.1 layer but cannot extract a status (e.g. an OCSP response
/// with no embedded BasicOCSPResponse) still leaves the struct in a
/// well-defined state instead of producing UB.
struct OcspResponse {
    /// Raw DER bytes of the OCSP response, byte-for-byte from the file.
    /// Always populated on parse() == true.
    std::vector<uint8_t> derBytes;

    /// Producer's "thisUpdate" timestamp from the first single-response,
    /// converted to Unix epoch seconds. 0 if not extractable.
    int64_t thisUpdate = 0;

    /// Producer's "nextUpdate" timestamp (Unix seconds). 0 if absent.
    int64_t nextUpdate = 0;

    /// V_OCSP_CERTSTATUS_GOOD / _REVOKED / _UNKNOWN from openssl/ocsp.h, or
    /// -1 if not extractable.
    int certStatus = -1;

    /// Human-readable status (e.g. "good", "revoked", "unknown",
    /// "successful (no single-response)"). Useful for logs and tests.
    std::string statusMessage;
};

/// Parsed contents of a CRL loaded from disk. Populated by CrlParser::parse().
/// Like OcspResponse, raw DER bytes are retained for direct /DSS embedding.
struct CrlInfo {
    /// Raw DER bytes of the CRL, byte-for-byte from the file.
    std::vector<uint8_t> derBytes;

    /// Issuer DN in RFC 2253 format (e.g. "CN=Test CA,O=PDFE,C=US").
    std::string issuerDN;

    /// CRL "thisUpdate" timestamp, Unix epoch seconds. 0 if absent.
    int64_t thisUpdate = 0;

    /// CRL "nextUpdate" timestamp, Unix epoch seconds. 0 if absent.
    int64_t nextUpdate = 0;

    /// Human-readable status. Useful for logs and tests.
    std::string statusMessage;
};

} // namespace pdfe::signatures
