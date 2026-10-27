// -*- coding: utf-8 -*-
// Pipeline F phase-10: offline LTV signature validator.
//
// On document re-open, walks /Catalog/DSS via DssReader, builds an
// in-memory OpenSSL X509_STORE (DSS certs + user trust anchors), and
// performs signer-cert chain + revocation checks against the embedded
// data. Network-free.
//
// Falls back to the existing ambient-trust verify path -- the user's anchors
// plus OpenSSL's default certificate paths, never the Windows certificate
// store (GitHub #212) -- when the doc has
// no /DSS — that's the "validated online" status, where revocation can
// only be confirmed by an external (out-of-band) network check.
#pragma once

#include "SignatureData.h"

#include <QString>

#include <cstdint>
#include <vector>

namespace pdfe::core {
class Document;
}

namespace pdfe::signatures {

/// Outcome of LtvValidator::verify(). Distinguishes the LTV-validated
/// case (chain + revocation confirmed against embedded data) from the
/// "no /DSS, online verify only" and "no /DSS, no revocation" cases.
enum class LtvStatus {
    /// /DSS present, chain validated against embedded certs (+ optional
    /// trust anchors), no embedded OCSP/CRL flagged the cert as revoked.
    LtvValidated,
    /// No /DSS dict. Chain validated against the user's trust anchors plus
    /// OpenSSL's default certificate paths. NOT the Windows certificate
    /// store, which PDFE never reads -- see the RECORDED DECISION block at
    /// the top of CertChainValidator.h (GitHub #212).
    OnlineValidated,
    /// No /DSS dict and those sources don't trust the chain. The
    /// digest is fine; a fresh revocation check is the only way to move
    /// this to OnlineValidated.
    NoDssData,
    /// Document has no signature widgets at all.
    NoSignatures,
    /// Digest verification failed. The signed bytes don't match the
    /// PKCS#7 SignedData. Cert chain status is irrelevant.
    InvalidSignature,
    /// At least one OCSP single-response or CRL entry says the signer
    /// cert is revoked. Hard fail regardless of chain trust.
    Revoked,
    /// Chain doesn't terminate at a trusted root (neither OpenSSL's default
    /// certificate paths nor user trust anchors -- the Windows certificate
    /// store is not consulted, GitHub #212). Digest passed, no revocation
    /// evidence.
    ///
    /// PHASE-50 (F52). verify() used to REWRITE this to NoDssData whenever the
    /// document had no /DSS, on a stated "UI contract" that the panel should
    /// read "validated (no revocation data)". The net effect was that a
    /// self-minted, unanchored certificate with no /DSS displayed the word
    /// **Validated**. The rewrite is gone; this status now reaches the UI
    /// intact and renders as "Signature intact — certificate NOT trusted".
    UntrustedChain,
    /// Revocation evidence WAS present in /DSS and was REFUSED by a specific
    /// rule (F53, phase-50). Distinct from NoDssData, which means no evidence
    /// was supplied at all. Every path that produces this used to collapse into
    /// NoDssData, so a forged OCSP responder and an empty /DSS rendered
    /// identically — the single signal that something adversarial is happening
    /// was discarded.
    ///
    /// FAIL-CLOSED INVARIANT. It is reached only from branches that already
    /// declined to award LtvValidated, and it can never be reached instead of
    /// Revoked or UntrustedChain (both are classified first, in step 5, and
    /// return before this branch). Adding it must never turn rejected evidence
    /// into acceptable evidence, nor manufacture a false Revoked.
    ///
    /// PLACEMENT, CORRECTED BY THE PHASE-50 REVIEW. This was described as
    /// "strictly a DOWNGRADE of NoDssData". It is not only that: the arm sits
    /// above BOTH remaining branches, so it displaces OnlineValidated — a PASS
    /// — as well. That happens when /DSS carries revocation artifacts but no
    /// certificates, so the document would otherwise have passed on the system
    /// trust store alone. Intended, and stronger than the original wording
    /// admitted. See the comment on the arm itself in LtvValidator.cpp.
    ///
    /// LtvVerifyResult::message names the rule that did the refusing.
    RevocationEvidenceRejected,
    /// Anything we couldn't classify cleanly (e.g. a MuPDF / OpenSSL
    /// error during the verify). Treat as untrusted in UI.
    Unknown
};

/// Convert LtvStatus to the strings the SignaturePanel surfaces.
QString ltvStatusToDisplayString(LtvStatus status);

namespace detail {

/// Instrumentation for the F59 discriminating control (phase-50).
///
/// The revoked-path-certificate walk's verdict is IDENTICAL whether or not the
/// loop stops at the first revoked link — `revokedPathCert` is a bool that is
/// already true. A test asserting the verdict therefore cannot detect an early
/// break being added or removed. These counters make the walk's *work*
/// observable, which is the only thing F59 changes.
struct PathWalkStats {
    int linkVisits = 0;               ///< loop iterations actually entered
    int certIssuedSignerCalls = 0;    ///< certIssuedSigner() calls the walk made
    int isCertRevokedByCrlCalls = 0;  ///< isCertRevokedByCrl() calls the walk made
};

/// The "is any CA above the signer revoked" path walk, extracted from
/// LtvValidator::verifyWith() step 4 so it can be driven directly with a
/// synthetic path. Behaviour is unchanged from the inline loop it replaces.
///
/// @param pathDer  the path OpenSSL VERIFIED, leaf first, anchor last. The leaf
///                 (index 0) and the anchor (last element) are both excluded
///                 from the walk — RFC 5280 §6.1 does not subject a trust
///                 anchor to revocation checking, and the leaf is handled by
///                 the direct isCertRevokedByCrl() call in verifyWith().
/// @param crlBlobs the DER CRLs to consult (the /DSS /CRLs payload).
/// @param stats    optional; receives the call counts described above.
/// @return true when some intermediate on the path is revoked.
bool walkPathForRevokedCert(const std::vector<std::vector<uint8_t>>& pathDer,
                            const std::vector<std::vector<uint8_t>>& crlBlobs,
                            PathWalkStats* stats);

/// F54 (phase-51) — test seam over SignerIssuerPool::fromCandidates(), the ONLY
/// construction path of the type that gates OCSP responder authentication.
///
/// ocspResponseIsAuthorized() seeds its X509_STORE with X509_V_FLAG_PARTIAL_
/// CHAIN, which makes every store member a trust anchor. Membership must
/// therefore be restricted to certificates PROVEN by signature to have issued
/// the signer. Until phase-51 that restriction lived in a comment on a plain
/// `std::vector<X509*>` parameter; it is now enforced by the parameter's TYPE,
/// whose constructor is private and whose factory runs certIssuedSigner() on
/// every admitted certificate.
///
/// @return the indices into @p candidateDer that the factory ADMITTED. A
///         document-supplied certificate wearing the real issuer's DN but a
///         different key must not appear. Returns empty when @p signerDer does
///         not parse.
std::vector<int> signerIssuerPoolAdmittedIndices(
    const std::vector<std::vector<uint8_t>>& candidateDer,
    const std::vector<uint8_t>& signerDer);

/// F54 (phase-51) — drives the hardened path end to end: build the pool from
/// @p candidateDer against @p signerDer, then run the RFC 6960 §4.2.2.2
/// responder-authentication check on @p ocspDer against it.
/// @return true only when the responder authenticated against a pool member.
///         A rogue responder whose certificate never passed certIssuedSigner()
///         yields false because it never entered the pool.
bool ocspAuthorizedForSigner(
    const std::vector<uint8_t>& ocspDer,
    const std::vector<std::vector<uint8_t>>& candidateDer,
    const std::vector<uint8_t>& signerDer);

} // namespace detail

/// Full result of LtvValidator::verify() — the LtvStatus headline plus
/// the underlying digest verification (re-used from existing
/// SignatureManager API) for callers that want the raw digest message.
struct LtvVerifyResult {
    LtvStatus status = LtvStatus::Unknown;
    /// Human-readable message. Logged by callers, shown in details
    /// dialogs.
    QString message;
    /// The digest verification result from the existing
    /// pdf_check_widget_digest path — preserved so callers don't lose
    /// the existing diagnostic surface.
    SignatureVerifyResult digestResult;
    /// True if the signature carries an embedded RFC 3161 timestamp token
    /// (id-smime-aa-timeStampToken in the PKCS#7 unauthenticated attributes).
    bool hasTimestamp = false;
    /// True if that timestamp's messageImprint actually binds this signature
    /// (imprint == SHA-256 of the signature value). False when the token is
    /// absent, malformed, or attests to different bytes (a forged/mismatched
    /// timestamp). See TsaClient::verifyTimestampToken.
    bool timestampImprintValid = false;
};

/// Result of checking a signature's embedded RFC 3161 timestamp.
struct TimestampCheck {
    bool present = false;       ///< A timestamp attribute was found.
    bool imprintValid = false;  ///< Its messageImprint binds the signature.
};

/**
 * @brief Offline-first signature validator.
 *
 * Two entry points:
 *   - verify() drives the full flow: extract first signature, read
 *     /DSS, validate.
 *   - verifyWith() is the test-friendly seam — caller supplies the
 *     embedded certs / OCSP / CRL bytes directly. The "real" verify()
 *     is a thin wrapper that feeds DssReader output into verifyWith().
 *
 * Pure static interface — no per-instance state. Each call produces a
 * fresh X509_STORE.
 */
class LtvValidator {
public:
    /// Drive the full offline validation pipeline against @p doc.
    /// Returns a populated result; never throws.
    static LtvVerifyResult verify(pdfe::core::Document* doc);

    /// Test seam: validate using caller-supplied embedded data.
    /// @p embeddedCerts is treated as the /DSS /Certs payload (added to
    /// the X509_STORE before chain verify). @p ocsps and @p crls are
    /// scanned for the signer cert. Pass empty vectors to simulate
    /// "DSS exists but no revocation data".
    ///
    /// @param useTrustAnchors  If true, TrustAnchorManager::loadIntoStore()
    ///                         is called on the X509_STORE before verify.
    ///                         Tests that want isolation from the user's
    ///                         actual trust list pass false.
    static LtvVerifyResult verifyWith(
        pdfe::core::Document* doc,
        const std::vector<std::vector<uint8_t>>& embeddedCerts,
        const std::vector<std::vector<uint8_t>>& ocsps,
        const std::vector<std::vector<uint8_t>>& crls,
        bool useTrustAnchors = true);

    /// Extract and check an embedded RFC 3161 timestamp from a signature's
    /// raw PKCS#7 (/Contents) bytes. Locates the id-smime-aa-timeStampToken
    /// unauthenticated attribute, computes the expected message imprint as the
    /// SHA-256 of the SignerInfo's signature value (matching the embed side in
    /// Pkcs7OpenSslBridge), and reports whether the token's messageImprint
    /// binds it. This is diagnostic (non-fatal): verifyWith() uses it to
    /// populate LtvVerifyResult::hasTimestamp / ::timestampImprintValid rather
    /// than to reject the signature. Exposed for direct testing. Never throws.
    static TimestampCheck checkEmbeddedTimestamp(
        const std::vector<uint8_t>& pkcs7Der);
};

} // namespace pdfe::signatures
