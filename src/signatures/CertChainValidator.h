// -*- coding: utf-8 -*-
// Meta-audit follow-up F23 (gap PDF-39): certificate trust-path validation
// with a SPECIFIC verdict.
//
// Before this class, every chain check in the signature stack collapsed
// X509_verify_cert()'s rich failure taxonomy into a bool at the point of use
// (LtvValidator::verifyChain) or into four PDF-level codes buried inside a
// MuPDF callback (Pkcs7OpenSslBridge). Two consequences, both bad:
//
//   1. Nothing could distinguish "this chain does not reach a trusted root"
//      from "this chain reaches a trusted root through an EXPIRED
//      intermediate" from "the leaf is revoked". A test asserting only
//      "validation failed" passes for all three -- including for a typo'd
//      fixture path that produced no chain at all. Removing exactly that
//      class of unfalsifiable oracle is what this audit programme is for.
//   2. The anchor set was ambient: TrustAnchorManager's persisted singleton
//      plus X509_STORE_set_default_paths(). There was no way to ask "would
//      this chain validate against THIS set of anchors?"
//
// CertChainValidator fixes both. The anchor set is an explicit parameter
// (TrustAnchorSet), OpenSSL's default cert paths are opt-IN rather than
// unconditional (and are NOT the Windows store -- see the DECISION below), the
// verification instant is pinnable, and the result carries the specific
// verdict plus the raw X509_V_ERR_* code and the depth it occurred at.
//
// ===========================================================================
// RECORDED DECISION — PDFE DOES NOT CONSULT THE WINDOWS CERTIFICATE STORE
// GitHub #212. Decided 2026-09-18. This is deliberate, not an oversight.
// ===========================================================================
//
// WHAT THE CODE DOES TODAY, MEASURED
// ----------------------------------
// No file under src/ calls CertOpenStore, CertOpenSystemStore or any other
// wincrypt/Crypt32 entry point for signature trust. The ONLY sources of trust
// in the whole signature stack are:
//
//   1. TrustAnchorSet / TrustAnchorManager — certificates the USER explicitly
//      imported, persisted in QSettings, and
//   2. X509_STORE_set_default_paths(), reached via
//      ChainValidationRequest::useOpenSslDefaultPaths.
//
// (2) is the trap this block exists to disarm. It was named `useSystemStore`,
// and both production callers (LtvValidator::verifyChain,
// Pkcs7OpenSslBridge::openssl_check_certificate) set it true, so the source
// read as though PDFE already honoured the machine's trust configuration. It
// does not. X509_STORE_set_default_paths() loads OPENSSLDIR/certs plus
// SSL_CERT_FILE / SSL_CERT_DIR. Those are POSIX conventions; Windows keeps its
// roots in the registry-backed certificate store, which OpenSSL never reads.
// On the vcpkg OpenSSL build PDFE links, OPENSSLDIR/certs is not even present
// on a stock machine, so the call is typically a no-op that loads nothing.
//
// The consequence, stated plainly: a certificate that Windows trusts — one
// sitting in the machine ROOT store right now — is reported UntrustedRoot by
// PDFE unless the user imported it as a PDFE trust anchor.
//
// THE DECISION: KEEP IT THAT WAY, FOR NOW
// ---------------------------------------
//   * PDFE's trust model is an EXPLICIT, user-controlled, portable anchor set.
//     It is auditable, it is identical on every machine, and a test can state
//     it exactly (which is what CertChainValidator was built for). The Windows
//     ROOT store is machine policy PDFE does not control: it is mutable by any
//     administrator or enterprise GPO, and it carries hundreds of CAs the user
//     never chose for document signing.
//   * Adopting it would NOT buy Acrobat parity, which is the reason usually
//     given for wanting it. Acrobat's document-signature trust comes from the
//     Adobe Approved Trust List (AATL) and the EU Trusted List — curated
//     DOCUMENT-SIGNING trust lists. The Windows ROOT store is the TLS/code
//     root set. Wiring it in would make PDFE trust a broader and DIFFERENT set
//     than Acrobat, while still disagreeing with Acrobat. Real parity means
//     ingesting AATL/EUTL, which is a separate feature with its own update and
//     revocation story.
//   * A correct Windows-store integration is not a small thing: CertOpenStore
//     on "ROOT" plus "CA" plus the enterprise and GPO store provider variants,
//     CTL and disallowed-certificate (`Disallowed` store) handling so a
//     Microsoft-distrusted root is not silently honoured, store-change
//     notification, and per-purpose EKU filtering (szOID_KP_DOCUMENT_SIGNING)
//     so a TLS-only root cannot vouch for a document. A half-built version of
//     that is worse than none: it would broaden trust without the distrust
//     half that makes the OS store safe to rely on.
//
// WHAT WAS DONE INSTEAD, SO THIS IS NOT MERELY AN OPINION IN A COMMENT
// -------------------------------------------------------------------
//   * The flag was renamed to useOpenSslDefaultPaths so no reader can infer
//     OS-store consultation from it, and LtvValidator's comment claiming the
//     system store rescues public-CA chains offline was corrected — on Windows
//     it never did.
//   * tests/test_cert_store_trust_policy.cpp PINS the behaviour: it pulls a
//     real, currently-valid root certificate out of the live Windows ROOT
//     store, and asserts PDFE reports it UntrustedRoot with
//     useOpenSslDefaultPaths ON — paired with a positive control that the very
//     same certificate IS Trusted once added to a TrustAnchorSet, so the
//     negative is attributable to the trust SOURCE and not to a bad fixture.
//
// IF THIS DECISION IS EVER REVERSED, that test is where it will announce
// itself: it goes red the moment PDFE starts honouring the OS store, and it
// must be rewritten deliberately rather than deleted.
#pragma once

#include "TrustAnchorSet.h"

#include <QString>

#include <cstdint>
#include <vector>

namespace pdfe::signatures {

/// Specific outcome of a chain validation. Deliberately finer-grained than a
/// bool: a caller (or a test) that asserts Trusted-vs-UntrustedRoot is making
/// a claim a broken fixture cannot accidentally satisfy.
enum class ChainVerdict {
    /// A complete path from the leaf to one of the supplied anchors was built
    /// and every certificate on it verified. X509_V_OK.
    Trusted,
    /// No path to a trusted anchor could be built, or one was built and the
    /// terminating certificate is not trusted. The chain is structurally
    /// fine; the ANCHOR SET is what rejects it.
    UntrustedRoot,
    /// A certificate on the path is outside its validity window (past
    /// notAfter) at the verification instant. Distinct from UntrustedRoot:
    /// the path EXISTS and reaches an anchor, it is just stale.
    ExpiredCertificate,
    /// A certificate on the path is not yet valid (before notBefore) at the
    /// verification instant.
    NotYetValid,
    /// A supplied CRL lists a certificate on the path as revoked.
    Revoked,
    /// A CRL check was requested but no usable CRL covers the leaf's issuer.
    /// Reported rather than silently treated as "good".
    NoRevocationInformation,
    /// The leaf did not parse, or no leaf was supplied. Never conflate with
    /// UntrustedRoot -- an unparseable input is a caller bug, not a trust
    /// decision.
    MalformedInput,
    /// Verified and failed for a reason this enum does not model. The raw
    /// X509_V_ERR_* code in ChainValidationResult::opensslError identifies it.
    Unknown
};

/// Human-readable name of a verdict. Stable, non-localised -- intended for
/// logs, gtest failure messages and disposition documents, not the UI.
QString chainVerdictToString(ChainVerdict verdict);

/// Everything a validation needs. Defaults are the SAFE, CONTROLLED ones:
/// no system store, no CRL checking, verify at wall-clock time.
///
/// (The sentence above is left EXACTLY as it was: the phase-7 criterion for
/// GitHub #233 names it as already accurate and warns that rewriting it would
/// damage correct documentation. This paragraph is an addition, not an edit.
/// "No system store" is true in the strongest sense -- no setting in this
/// struct reaches the Windows certificate store, because it is never a trust
/// source at all, not merely one that is switched off. See the RECORDED
/// DECISION at the top of this header, GitHub #212.)
struct ChainValidationRequest {
    /// DER of the end-entity certificate whose path is being validated.
    std::vector<uint8_t> leafDer;

    /// DER of certificates that may be used to BUILD the path but are not
    /// themselves trusted -- intermediates, cross-certificates, anything
    /// harvested from a document. Never a source of trust.
    std::vector<std::vector<uint8_t>> untrustedDer;

    /// The anchor set. This -- and, if @ref useOpenSslDefaultPaths is set,
    /// OpenSSL's default certificate paths -- is the ONLY source of trust.
    /// The Windows certificate store is deliberately not one; see the
    /// DECISION block at the top of this header (GitHub #212).
    TrustAnchorSet anchors;

    /// DER of CRLs to consult. Non-empty enables X509_V_FLAG_CRL_CHECK on the
    /// leaf. Empty means no revocation checking is performed at all (the
    /// caller is expected to be doing OCSP elsewhere, as LtvValidator does).
    std::vector<std::vector<uint8_t>> crlsDer;

    /// Opt IN to X509_STORE_set_default_paths(). Default false so that a
    /// caller asking about a controlled anchor set gets an answer about that
    /// anchor set and nothing else. The production callers in the signature
    /// stack set this true; tests that need a hermetic verdict leave it false.
    ///
    /// NAMED FOR WHAT IT DOES. This field was called `useSystemStore` until
    /// GitHub #212. That name says "the machine's trust configuration", and on
    /// Windows — the only platform PDFE ships on — that is exactly what it is
    /// NOT. See the DECISION block at the top of this header: on Windows
    /// X509_STORE_set_default_paths() consults OPENSSLDIR/certs and the
    /// SSL_CERT_FILE / SSL_CERT_DIR environment variables. It does not open
    /// the Windows certificate store, and on a default vcpkg OpenSSL build
    /// OPENSSLDIR/certs does not exist, so setting this true normally adds
    /// ZERO anchors. The flag is retained (it is meaningful on POSIX, and it
    /// is the honest place to hang the Windows-store decision) but it must not
    /// be read as "PDFE trusts what Windows trusts".
    bool useOpenSslDefaultPaths = false;

    /// Unix seconds to verify at. 0 means "now". Pinning this is what makes a
    /// certificate test independent of the calendar: the relationship between
    /// a fixture's validity window and the verification instant becomes two
    /// constants rather than a race with the clock.
    std::int64_t verificationTimeUnix = 0;
};

/// Outcome, with enough detail to assert on precisely.
struct ChainValidationResult {
    ChainVerdict verdict = ChainVerdict::Unknown;
    /// Raw X509_V_ERR_* (X509_V_OK == 0 on success).
    int opensslError = 0;
    /// Depth the error occurred at: 0 is the leaf, 1 its issuer, and so on.
    /// -1 when no verification was reached.
    int errorDepth = -1;
    /// Number of certificates in the path OpenSSL built (leaf..anchor
    /// inclusive). 0 when no path was built.
    int chainLength = 0;
    /// DER of every certificate on the VERIFIED path, leaf FIRST and anchor
    /// LAST — i.e. pathDer[0] is @ref ChainValidationRequest::leafDer and
    /// pathDer.back() is the anchor the path terminated at.
    ///
    /// **EMPTY UNLESS @ref trusted().** This is a security contract, not an
    /// optimisation (phase-49 review finding M1). On a failed verification
    /// OpenSSL still exposes a partially built chain whose upper elements came
    /// from the caller's untrusted material and may never have had a signature
    /// checked; handing that out under a name that says "verified" is how an
    /// unverified certificate ends up authorising a CRL. So when trusted() is
    /// true, pathDer.size() == chainLength; otherwise pathDer is empty while
    /// chainLength still reports how far OpenSSL got.
    ///
    /// WHY THIS IS EXPOSED (F47 item F2, phase-49). Revocation checking used
    /// to be a LEAF-ONLY exercise: LtvValidator matched CRLs against the
    /// signer's issuer DN and stopped, so a revoked INTERMEDIATE was invisible
    /// and its still-unrevoked leaf validated cleanly. Checking the rest of
    /// the path requires knowing what the path actually is, and only OpenSSL
    /// knows that — it is the component that chose which of the untrusted
    /// certificates to use. Re-deriving it in the caller by matching DNs would
    /// reintroduce exactly the DN-is-attacker-text mistake this file exists to
    /// avoid; these are the certificates whose signatures were VERIFIED.
    std::vector<std::vector<uint8_t>> pathDer;
    /// Number of anchors that actually made it into the store. A caller that
    /// passed anchors and sees 0 here knows the set was the problem.
    int anchorsLoaded = 0;
    /// Non-localised diagnostic, including OpenSSL's own error string.
    QString message;

    bool trusted() const { return verdict == ChainVerdict::Trusted; }
};

/**
 * @brief Validates a certificate path against an EXPLICIT anchor set.
 *
 * Stateless. Every call builds a fresh X509_STORE, so no verification can
 * leak trust into another. Never throws.
 */
class CertChainValidator {
public:
    static ChainValidationResult validate(const ChainValidationRequest& req);
};

} // namespace pdfe::signatures
