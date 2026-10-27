// -*- coding: utf-8 -*-
// Pipeline F phase-10: persistent trust-anchor store for LTV verification.
//
// Lets the user "trust" a CA (or self-signed) certificate by file path.
// Every entry is persisted to QSettings so trust survives restart, and
// loaded into an in-memory OpenSSL X509_STORE at verification time so
// LtvValidator and Pkcs7OpenSslBridge can chain against it.
//
// The class is a singleton because the QSettings storage is global and
// the OpenSSL store has to reflect "the current set of user trust
// anchors" — instance proliferation would just create stale snapshots.
#pragma once

#include "TrustAnchorSet.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <filesystem>

// Pull in just OpenSSL's typedef header so X509_STORE* is a valid type
// in our public API without callers needing the full <openssl/x509.h>.
// types.h is the OpenSSL-blessed forward-decl point and is light.
#include <openssl/types.h>

namespace pdfe::signatures {

/// One trusted root certificate. Stored verbatim as DER bytes so we can
/// re-feed it to OpenSSL without re-parsing PEM each time. Subject /
/// issuer CN are extracted at add-time for UI display.
struct TrustAnchor {
    /// Hex SHA-256 fingerprint of the cert DER. Uppercase, no separators.
    /// Used as the unique identity across QSettings round-trips and as
    /// the removeTrust() handle.
    QString fingerprintSha256;
    /// Common Name from the subject DN. May be empty if the cert has no
    /// CN attribute (rare but legal).
    QString subjectCN;
    /// Common Name from the issuer DN. For self-signed roots this matches
    /// subjectCN.
    QString issuerCN;
    /// File path the user originally imported from. Pure provenance —
    /// the cert bytes are stored independently in QSettings and the
    /// file is not re-read on subsequent loads.
    QString filePath;
    /// Raw DER bytes of the X.509 cert. Always populated.
    QByteArray derBytes;
};

/**
 * @brief Persistent list of user-trusted root certificates.
 *
 * Singleton, lazy-initialized from QSettings on first call to instance().
 * Persistence format is the QApplication-default QSettings backend (on
 * Windows that's HKCU\\Software\\PDFE\\PDFE; on Linux ~/.config/PDFE/PDFE.conf
 * — same as AppSettings).
 *
 * The QSettings group is "TrustAnchors". Each anchor is stored as
 *
 *   TrustAnchors/<fingerprint>/path        : QString
 *   TrustAnchors/<fingerprint>/subject     : QString
 *   TrustAnchors/<fingerprint>/issuer      : QString
 *   TrustAnchors/<fingerprint>/der_b64     : QString  (Base64 of DER)
 *
 * with the cert keyed under its fingerprint. Storing the raw DER avoids
 * any dependency on the PEM file remaining at @c filePath after import.
 */
class TrustAnchorManager {
public:
    /// Singleton accessor. Lazy-loads from QSettings on first call.
    /// Subsequent calls return the same instance.
    static TrustAnchorManager& instance();

    /// Add a trust anchor from a PEM or DER file. Returns true on
    /// success. On failure, @p errorOut (if non-null) is set to a
    /// human-readable reason. The cert is parsed via OpenSSL; any
    /// parse failure rejects the import.
    ///
    /// Notes:
    ///   - Expired certs are accepted. The whole point of trust anchors
    ///     is that the user explicitly trusts something the system may
    ///     not.
    ///   - Adding a cert that's already present is a no-op (returns
    ///     true; @p errorOut may say "already trusted" for clarity).
    bool addTrust(const std::filesystem::path& certPath,
                  QString* errorOut = nullptr);

    /// Remove a trust anchor by fingerprint. Returns true if the
    /// fingerprint was present and removed; false if not found.
    bool removeTrust(const QString& fingerprintSha256);

    /// Snapshot of the current trust list. Returns a copy so callers
    /// can iterate without locking.
    QList<TrustAnchor> listTrusts() const;

    /// Push every trust anchor into @p store via X509_STORE_add_cert.
    /// Tolerates a null @p store (no-op). Errors from individual adds
    /// are silently ignored — the goal is "best-effort enrich the
    /// store"; a malformed entry shouldn't fail the whole verify.
    ///
    /// Implemented as snapshot().loadIntoStore(store) so the persisted
    /// list and an ad-hoc TrustAnchorSet reach OpenSSL by exactly one
    /// code path.
    void loadIntoStore(X509_STORE* store) const;

    /// Meta-audit follow-up F23 (gap PDF-39): the persisted anchors as a
    /// detached, copyable value.
    ///
    /// This is the seam that lets chain validation take the anchor set as
    /// a PARAMETER (CertChainValidator::validate) instead of reaching into
    /// this singleton. Callers that want the user's real trust list pass
    /// snapshot(); callers that want a controlled set — tests, policy
    /// what-ifs — build their own TrustAnchorSet and never touch QSettings
    /// at all. Nothing has to mutate global state and hope teardown
    /// restores it.
    TrustAnchorSet snapshot() const;

    /// Wipe all anchors from QSettings + memory. For tests.
    void clear();

    // Non-copyable, non-movable (singleton).
    TrustAnchorManager(const TrustAnchorManager&) = delete;
    TrustAnchorManager& operator=(const TrustAnchorManager&) = delete;

private:
    TrustAnchorManager();

    void loadFromSettings();
    void writeAnchorToSettings(const TrustAnchor& anchor);
    void removeAnchorFromSettings(const QString& fingerprintSha256);

    /// Parse a PEM or DER file into raw DER bytes. Auto-detects format by
    /// trying PEM first (since DER files routinely look like garbage to
    /// PEM_read_bio_X509 and we get a clean failure to fall back from).
    static bool loadCertBytes(const std::filesystem::path& path,
                               QByteArray& derOut, QString* errorOut);

    /// Fill @p anchor (sans filePath) from raw DER bytes. Returns false
    /// if the bytes don't parse as X.509.
    static bool buildAnchorFromDer(const QByteArray& der,
                                    TrustAnchor& anchor);

    QList<TrustAnchor> anchors_;
};

} // namespace pdfe::signatures
