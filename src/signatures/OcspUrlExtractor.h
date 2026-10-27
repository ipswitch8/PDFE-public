// -*- coding: utf-8 -*-
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pull in the OpenSSL X509 typedef without dragging the full x509 API.
// <openssl/types.h> is the canonical header for the opaque type aliases.
#include <openssl/types.h>

namespace pdfe::signatures {

/**
 * @brief Extract OCSP responder URLs from a signer certificate's
 *        Authority Information Access (AIA) extension.
 *
 * This is a *pure parser*. It does NOT make any network calls. It
 * inspects the X.509 certificate's AIA extension (RFC 5280 §4.2.2.1)
 * and yields URLs whose accessMethod OID is id-ad-ocsp (1.3.6.1.5.5.7.48.1).
 *
 * SECURITY: Only http:// and https:// scheme URLs are returned. ldap://,
 * ftp:// and other schemes are filtered out. This matches the policy in
 * TsaClient::isAllowedTsaUrl().
 *
 * Pipeline F phase-9: drives the user-confirmed network fetch UI.
 */
class OcspUrlExtractor {
public:
    /// Parse @p cert and append OCSP responder URLs to @p urls.
    /// Returns true if the extension was present; false (with @p urls
    /// untouched) if absent or empty. The boolean is for callers that
    /// want to distinguish "no AIA extension" from "AIA present but
    /// no OCSP entries" — both produce an empty @p urls vector but only
    /// the former returns false.
    static bool parse(X509* cert, std::vector<std::string>& urls);

    /// Same, but parses a DER-encoded cert blob in-process. Convenient
    /// when the caller already has the cert as bytes (e.g. from the
    /// /DSS /Certs stream array). Returns false if the DER fails to
    /// parse OR no AIA extension is present.
    static bool parseDer(const uint8_t* der, std::size_t derLen,
                         std::vector<std::string>& urls);
};

} // namespace pdfe::signatures
