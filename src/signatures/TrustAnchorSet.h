// -*- coding: utf-8 -*-
// Meta-audit follow-up F23 (gap PDF-39): the trust-store seam.
//
// TrustAnchorManager is a QSettings-backed SINGLETON. That is the right shape
// for "the set of CAs this user has chosen to trust", and wrong for everything
// else: any code that wants to evaluate a chain against a DIFFERENT anchor set
// -- a test, a what-if in the UI, a second document opened with stricter
// policy -- has no way to say so. Its only options are to mutate the user's
// persisted trust list and hope teardown puts it back, or to give up and fall
// back on ambient trust -- OpenSSL's default certificate paths, which is the
// only ambient source there is; the Windows certificate store is not one
// (GitHub #212). Both make the trusted/untrusted distinction a
// property of ambient global state rather than of the call.
//
// TrustAnchorSet is that missing value type: an explicit, copyable set of
// anchor certificates with no persistence and no global state. It is the
// parameter CertChainValidator takes. TrustAnchorManager keeps its singleton
// role but now EXPOSES its contents as one of these (snapshot()), so the
// persisted list and an ad-hoc list are the same kind of thing and travel
// through the same code path.
#pragma once

#include <QByteArray>
#include <QList>

#include <cstdint>
#include <vector>

// OpenSSL's blessed forward-decl header -- keeps <openssl/x509.h> out of our
// public API, same convention as TrustAnchorManager.h.
#include <openssl/types.h>

namespace pdfe::signatures {

/**
 * @brief An explicit, self-contained set of trust-anchor certificates.
 *
 * Plain value type: copyable, no singleton, no QSettings, no filesystem.
 * Constructing one and passing it to CertChainValidator::validate() is the
 * whole seam -- the anchor set becomes an argument instead of ambient state.
 *
 * Certificates are held as DER bytes (the same representation
 * TrustAnchorManager persists), so an anchor can move between the persisted
 * list and an ad-hoc set without re-encoding.
 */
class TrustAnchorSet {
public:
    TrustAnchorSet() = default;

    /// Append a DER-encoded X.509 certificate. Malformed or empty input is
    /// rejected (returns false) rather than silently stored -- a caller that
    /// meant to seed an anchor and typo'd the path must not end up with a
    /// silently empty set that then looks like a genuine "untrusted" verdict.
    bool addDer(const QByteArray& der);

    /// Convenience overload for the std::vector<uint8_t> DER form the rest of
    /// the signature stack passes around.
    bool addDer(const std::vector<uint8_t>& der);

    /// Number of anchors held.
    int size() const { return static_cast<int>(anchorsDer_.size()); }
    bool isEmpty() const { return anchorsDer_.empty(); }

    /// Raw DER of every anchor, in insertion order.
    const QList<QByteArray>& derBytes() const { return anchorsDer_; }

    /// Push every anchor into @p store via X509_STORE_add_cert. Tolerates a
    /// null @p store (no-op). Returns the number of anchors successfully
    /// added; a caller that seeded N anchors and gets back fewer knows the
    /// store is not what it asked for, instead of inferring it from a
    /// downstream verdict.
    int loadIntoStore(X509_STORE* store) const;

private:
    QList<QByteArray> anchorsDer_;
};

} // namespace pdfe::signatures
