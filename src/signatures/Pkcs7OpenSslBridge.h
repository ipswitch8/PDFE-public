#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward declare MuPDF types (named structs only -- pdf_pkcs7_distinguished_name
// is an anonymous typedef so CANNOT be forward-declared)
extern "C" {
struct fz_context;
struct pdf_pkcs7_signer;
struct pdf_pkcs7_verifier;
}

namespace pdfe::signatures {

class TrustAnchorSet;

/// F26 — how verifyWithChain's TRUST decision is to be made.
///
/// Until F26 there was no answer to that question: verifyWithChain built its
/// own inline X509_STORE and called X509_STORE_set_default_paths() on it, so
/// its verdict came from the machine's ambient store and from nothing else.
/// That was a SECOND, uncontrolled trust decision sitting beside the one F23
/// brought under control (CertChainValidator + TrustAnchorSet), and the user's
/// explicit trust anchors had no bearing on it at all.
///
/// Defaults reproduce the PRODUCTION behaviour of the site F23 did rewire
/// (openssl_check_certificate): the user's persisted anchors PLUS OpenSSL's
/// default certificate paths (X509_STORE_set_default_paths -- NOT the Windows
/// certificate store, which PDFE never reads; GitHub #212). A test — or any
/// caller wanting a hermetic answer — supplies an
/// explicit anchor set and turns the system store off, and then the verdict is
/// a property of the CALL rather than of the machine.
///
/// (That second sentence is restored EXACTLY as it was. The phase-7 criterion
/// for #233 names it as already accurate. Only the FIRST clause changed, and
/// it had to: it claimed the production default was "anchors PLUS the system
/// store", which asserts a consultation that does not happen and is precisely
/// what the criterion does require correcting.)
struct VerifyWithChainOptions {
    /// Anchor set to trust. nullptr means "the user's persisted anchors"
    /// (TrustAnchorManager::instance().snapshot()) — never "no anchors".
    const TrustAnchorSet* anchors = nullptr;

    /// Opt in to OpenSSL's default certificate paths
    /// (X509_STORE_set_default_paths). True by default so production behaviour
    /// is unchanged; a controlled test sets it false.
    ///
    /// GitHub #212 — renamed from `useSystemStore`. This is NOT the Windows
    /// certificate store and never was; PDFE deliberately does not consult it.
    /// The full recorded decision, with the measurement behind it, is the
    /// DECISION block at the top of CertChainValidator.h.
    bool useOpenSslDefaultPaths = true;

    /// Unix seconds to verify at; 0 means "now". Pinning this is what makes a
    /// fixture-driven verdict independent of the calendar.
    std::int64_t verificationTimeUnix = 0;
};

/// Result of verifying a PKCS#7 signature blob, including the DER-encoded
/// certificate chain pulled from the signed data. Used by the long-term
/// validation (LTV) pipeline to feed the /DSS dictionary in LtvDssWriter.
///
/// NOTE (meta-audit F20, gap PDF-34): this used to name an ETSI baseline
/// profile. PDFE reaches none of them — the container built in
/// Pkcs7OpenSslBridge.cpp omits the mandatory ESS signing-certificate-v2
/// attribute and carries a prohibited signing-time attribute, and the emitted
/// /SubFilter is `adbe.pkcs7.detached`. See docs/signature-profile-support.md.
struct VerifyWithChainResult {
    /// True if the signature digest verified against the embedded content.
    /// Independent of trust-chain evaluation.
    bool digestValid = false;
    /// True iff the signer cert chained to an anchor in the CONTROLLED anchor
    /// set the call was given (see VerifyWithChainOptions). False does not mean
    /// "invalid" — it usually just means "self-signed test cert" or "that root
    /// is not one of this user's trust anchors".
    ///
    /// F26: this used to be decided by an inline X509_STORE seeded from
    /// X509_STORE_set_default_paths() alone. It now comes from
    /// CertChainValidator, the same implementation LtvValidator and
    /// openssl_check_certificate use.
    bool chainTrusted = false;

    /// F26 — raw X509_V_ERR_* behind `chainTrusted == false` (X509_V_OK == 0
    /// when trusted, and when no signer was present to evaluate). Carried so a
    /// caller or a test can assert WHY a chain was rejected rather than only
    /// that it was: "untrusted root", "expired" and "no chain extracted at all"
    /// are three different findings that a bare false conflates.
    int chainOpensslError = 0;
    /// DER-encoded cert chain. Index 0 is the signer (leaf); subsequent
    /// entries are intermediates / root in unspecified order.
    std::vector<std::vector<uint8_t>> certChain;
    /// Free-form human-readable status, useful for logging and tests.
    std::string statusMessage;
};

/**
 * @brief Bridge for PKCS7 signing/verification using OpenSSL.
 *
 * Implements the MuPDF pdf_pkcs7_signer and pdf_pkcs7_verifier vtable
 * interfaces using OpenSSL directly, since MuPDF's pkcs7-openssl helpers
 * are not compiled into the vcpkg libmupdf.lib.
 */
class Pkcs7OpenSslBridge {
public:
    Pkcs7OpenSslBridge();
    ~Pkcs7OpenSslBridge();

    // Non-copyable, non-movable
    Pkcs7OpenSslBridge(const Pkcs7OpenSslBridge&) = delete;
    Pkcs7OpenSslBridge& operator=(const Pkcs7OpenSslBridge&) = delete;

    /// Check if OpenSSL is available and initialized.
    bool isAvailable() const;

    /// Read a PFX/PKCS12 file and return a MuPDF-compatible signer.
    /// Caller must call dropSigner() when done.
    /// @note Parameters are const char* (not std::string&) because this function
    /// uses fz_throw (longjmp) for errors. Temporary std::string objects on the
    /// stack between setjmp and longjmp cause MSVC Release crashes (SEH 0xc0000005).
    pdf_pkcs7_signer* readPfx(fz_context* ctx, const char* pfxPath,
                               const char* password);

    /// Create a MuPDF-compatible verifier.
    /// Caller must call dropVerifier() when done.
    pdf_pkcs7_verifier* createVerifier(fz_context* ctx);

    /// Drop (free) a signer created by readPfx().
    static void dropSigner(fz_context* ctx, pdf_pkcs7_signer* signer);

    /// Drop (free) a verifier created by createVerifier().
    static void dropVerifier(fz_context* ctx, pdf_pkcs7_verifier* verifier);

    /// Configure TSA timestamping on a signer. Must be called after readPfx()
    /// and before the signer is used for signing.
    static void configureTsa(pdf_pkcs7_signer* signer,
                              const std::string& tsaUrl);

    /// Verify a raw PKCS#7 SignedData blob and return the verified signer
    /// chain alongside the verify result. Designed for the LTV pipeline:
    /// the chain is what gets embedded into /DSS. The signed-content stream
    /// can be omitted (digest-only result will reflect that — it will be
    /// false because there was nothing to digest against).
    ///
    /// @param pkcs7Der        DER bytes of the PKCS#7 SignedData blob.
    /// @param pkcs7DerLen     Length of @p pkcs7Der.
    /// @param signedContent   Optional pointer to the bytes that were signed
    ///                        (for digest verification). May be nullptr.
    /// @param signedContentLen Length of @p signedContent.
    /// @param opts             F26 — the anchor set the trust verdict is made
    ///                         against. Defaults to production behaviour (the
    ///                         user's persisted anchors + OpenSSL's default
    ///                         certificate paths; never the Windows store).
    /// @return Result with chain populated even when verification fails,
    ///         provided the PKCS#7 was parseable.
    static VerifyWithChainResult verifyWithChain(
        const uint8_t* pkcs7Der, size_t pkcs7DerLen,
        const uint8_t* signedContent = nullptr,
        size_t signedContentLen = 0,
        const VerifyWithChainOptions& opts = VerifyWithChainOptions{});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::signatures
