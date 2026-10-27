#pragma once

#include "OcspCrlData.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace pdfe::signatures {

/**
 * @brief Parses a DER-encoded OCSP response from a local file or memory buffer.
 *
 * Strictly offline — there is no network code in this class. Callers are
 * expected to acquire the OCSP response bytes through some out-of-band channel
 * (a sidecar file shipped with a fixture, or the bytes returned by a UI flow
 * that the user explicitly approved). The parser validates the ASN.1 with
 * OpenSSL's d2i_OCSP_RESPONSE and rejects malformed input cleanly (returns
 * false, no exception, no crash).
 *
 * The parsed @ref OcspResponse retains the raw DER bytes so that downstream
 * code (LtvDssWriter) can embed them into the /DSS /OCSPs stream array
 * verbatim without re-encoding.
 */
class OcspResponseParser {
public:
    /// Parse OCSP response from a file. Returns true on success.
    /// On failure, @p out.statusMessage is set to a human-readable reason
    /// and the rest of @p out is left in its default-constructed state.
    static bool parse(const std::filesystem::path& path, OcspResponse& out);

    /// Parse OCSP response from a memory buffer. Same semantics as parse().
    /// Safe against null pointers and zero-length input.
    static bool parseBytes(const uint8_t* der, std::size_t len,
                           OcspResponse& out);
};

} // namespace pdfe::signatures
