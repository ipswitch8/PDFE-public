#pragma once

#include "OcspCrlData.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace pdfe::signatures {

/**
 * @brief Parses a DER-encoded X.509 CRL from a local file or memory buffer.
 *
 * Strictly offline. No network code, no socket calls, no QNetworkAccessManager.
 * Validates structural integrity via OpenSSL's d2i_X509_CRL and rejects
 * malformed input cleanly. The raw DER bytes are retained on @ref CrlInfo so
 * downstream code (LtvDssWriter) can embed them into the /DSS /CRLs stream
 * array verbatim.
 *
 * Issuer DN is extracted in RFC 2253 form for human-readable display and
 * matching against signer cert issuers.
 */
class CrlParser {
public:
    /// Parse CRL from a file. Returns true on success.
    /// On failure, @p out.statusMessage is set to a human-readable reason
    /// and the rest of @p out is left in its default-constructed state.
    static bool parse(const std::filesystem::path& path, CrlInfo& out);

    /// Parse CRL from a memory buffer. Same semantics as parse().
    /// Safe against null pointers and zero-length input.
    static bool parseBytes(const uint8_t* der, std::size_t len, CrlInfo& out);
};

} // namespace pdfe::signatures
