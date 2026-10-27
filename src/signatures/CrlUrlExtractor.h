// -*- coding: utf-8 -*-
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <openssl/types.h>

namespace pdfe::signatures {

/**
 * @brief Extract CRL Distribution Point URLs from a signer certificate.
 *
 * Pure parser, no network calls. Walks the cert's CRL Distribution Points
 * extension (RFC 5280 §4.2.1.13) and yields URIs from each
 * DistributionPointName::fullName.
 *
 * SECURITY: Only http:// and https:// scheme URLs are returned. Other
 * schemes (ldap://, ftp://, file:// etc.) are filtered out before any
 * UI confirmation dialog can show them.
 */
class CrlUrlExtractor {
public:
    /// Parse @p cert and append CRL DP URLs to @p urls.
    /// Returns true if the extension was present; false if absent.
    static bool parse(X509* cert, std::vector<std::string>& urls);

    /// Parse a DER-encoded cert blob and emit CRL DP URLs.
    static bool parseDer(const uint8_t* der, std::size_t derLen,
                         std::vector<std::string>& urls);
};

} // namespace pdfe::signatures
