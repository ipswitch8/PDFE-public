// -*- coding: utf-8 -*-
// Pipeline F phase-10: read /Catalog/DSS back out of a signed PDF for
// offline (LTV) signature verification. Companion to LtvDssWriter.
//
// Strictly offline. No network calls. Walks the in-memory pdf_document
// via friend access to pdfe::core::Document and copies stream bytes
// into vectors that signature-verification code can feed to OpenSSL.
#pragma once

#include <QHash>
#include <QString>

#include <cstdint>
#include <vector>

namespace pdfe::core {
class Document;
}

namespace pdfe::signatures {

/// All DER blobs read out of /Catalog/DSS. The fields mirror the PDF
/// dictionary structure (top-level + per-VRI breakdown). Each byte
/// vector is a verbatim copy of the corresponding stream.
struct DssEmbedded {
    /// /Catalog/DSS/Certs — DER-encoded X.509 certs.
    std::vector<std::vector<uint8_t>> certs;
    /// /Catalog/DSS/OCSPs — DER-encoded OCSP responses.
    std::vector<std::vector<uint8_t>> ocsps;
    /// /Catalog/DSS/CRLs — DER-encoded CRLs.
    std::vector<std::vector<uint8_t>> crls;

    /// Per-VRI breakdown. Key is the uppercase-hex SHA-1 of the
    /// signature's /Contents hex string (matches LtvDssWriter's vriKey).
    /// Each entry holds its own /Cert, /OCSP, /CRL stream payloads.
    /// vriEntries[<key>].certs etc.
    QHash<QString, DssEmbedded> vriEntries;

    /// True iff /Catalog/DSS exists in the document. False means this
    /// PDF is not LTV-augmented; callers should fall back to live
    /// verification.
    bool hasDss = false;

    /// Convenience: total bytes of all top-level streams. Useful for
    /// logs and tests; the actual verification cares only about counts.
    std::size_t totalBytes() const {
        std::size_t n = 0;
        for (const auto& v : certs) n += v.size();
        for (const auto& v : ocsps) n += v.size();
        for (const auto& v : crls) n += v.size();
        return n;
    }
};

/**
 * @brief Reads a /DSS dictionary out of a signed PDF.
 *
 * Friend of pdfe::core::Document so it can reach the underlying
 * fz_document/pdf_document handle without a public accessor. The class
 * is a thin static helper; no per-instance state.
 *
 * Verification flow:
 *
 *   DssEmbedded dss;
 *   if (!DssReader::readDssEmbedded(doc, dss) || !dss.hasDss) {
 *       // No /DSS — fall back to live revocation lookup.
 *   } else {
 *       // Build X509_STORE from dss.certs, run OCSP/CRL checks against
 *       // dss.ocsps / dss.crls.
 *   }
 */
class DssReader {
public:
    /// Walk @p doc /Catalog/DSS and populate @p out. Returns true if a
    /// /DSS dict was found (out.hasDss == true). Returns false on null
    /// inputs or hard MuPDF errors; in that case out is in a default
    /// state.
    static bool readDssEmbedded(pdfe::core::Document* doc, DssEmbedded& out);
};

} // namespace pdfe::signatures
