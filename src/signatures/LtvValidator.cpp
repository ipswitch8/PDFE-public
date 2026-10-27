// -*- coding: utf-8 -*-
// Pipeline F phase-10: LtvValidator implementation. See header.
#include "LtvValidator.h"

#include "CertChainValidator.h"
#include "DssReader.h"
#include "LtvDssWriter.h"
#include "SignatureManager.h"
#include "TrustAnchorManager.h"
#include "TsaClient.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <openssl/x509.h>
#include <openssl/x509v3.h>   // KU_CRL_SIGN (RFC 5280 §6.3.3(f) enforcement)
#include <openssl/ocsp.h>
#include <openssl/pkcs7.h>
#include <openssl/sha.h>
#include <openssl/objects.h>
#include <openssl/err.h>

#include <QObject>
#include <QtGlobal>   // qWarning() — the F4 fail-closed diagnostic

#include <concepts>
#include <cstddef>
#include <vector>

namespace pdfe::signatures {

// PHASE-50 REVIEW, FIX 2. The switch below was described as
// "compiler-enforced exhaustive". It was not: no PDFE-owned CMake file sets
// -Wall/-Wswitch/-Werror or /W4 //WX, MSVC C4062 is off at the default
// warning level, and the `return QObject::tr("Unknown")` after the switch acts
// as a de facto `default:` — so an enumerator added to LtvStatus and forgotten
// here would compile silently and render "Unknown" to the user.
//
// The enforcement is therefore stated HERE, in the file that needs it, rather
// than as a target-wide flag that would also change the diagnostic contract of
// every other translation unit in pdfe_signatures. Verified by mutation: with
// `case LtvStatus::Revoked` deleted, MSVC fails the build with
// "error C4062: enumerator 'Revoked' in switch of enum 'LtvStatus' is not
// handled"; restoring it builds clean. Note C4062 (not C4061) is the correct
// number: it fires only when there is no `default:` LABEL inside the switch,
// which is exactly the shape this function has and must keep — the trailing
// return is outside the switch and does not suppress it.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(3 : 4062)      // off-by-default at /W1: enable it first…
#  pragma warning(error : 4062)  // …then make an unhandled enumerator fatal.
#elif defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic error "-Wswitch"
#endif
QString ltvStatusToDisplayString(LtvStatus status)
{
    switch (status) {
    case LtvStatus::LtvValidated:     return QObject::tr("Validated (LTV)");
    case LtvStatus::OnlineValidated:  return QObject::tr("Validated (online)");
    case LtvStatus::NoDssData:        return QObject::tr("Validated (no revocation data)");
    case LtvStatus::NoSignatures:     return QObject::tr("No signatures");
    case LtvStatus::InvalidSignature: return QObject::tr("INVALID");
    case LtvStatus::Revoked:          return QObject::tr("REVOKED");
    // PHASE-50 (F52). Was "UNTRUSTED". The product decision recorded in
    // docs/audit/meta-audit-2026-07/followup/phase-50/f52-product-decision.md
    // asks the panel to say what is and is not established, because the two
    // halves are independently true and a bare "UNTRUSTED" reads as though the
    // digest failed. What is established is the digest; what is not is the
    // certificate.
    case LtvStatus::UntrustedChain:
        return QObject::tr("Signature intact — certificate NOT trusted");
    // PHASE-50 (F53). Present-and-refused evidence, as distinct from absent
    // evidence (NoDssData). LtvVerifyResult::message names the rule.
    case LtvStatus::RevocationEvidenceRejected:
        return QObject::tr("Revocation evidence REJECTED");
    case LtvStatus::Unknown:          return QObject::tr("Unknown");
    }
    return QObject::tr("Unknown");
}
#if defined(_MSC_VER)
#  pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace {

constexpr long kOcspMaxAgeSec = 7 * 86400;  // 7 days
constexpr long kOcspDriftSec  = 300;         // 5 minutes clock skew
// CRL analogue of the OCSP freshness rule. Until phase-42 there was none: a CRL
// with no nextUpdate never expired, and a FUTURE-dated CRL was accepted.
constexpr long kCrlMaxAgeSec  = 7 * 86400;  // 7 days
constexpr long kCrlDriftSec   = 300;         // 5 minutes clock skew

// ═══════════════════════════════════════════════════════════════════════
// F53 (phase-50): "the evidence was REFUSED" is not the same claim as "there
// was no evidence", and until this phase they were the same VALUE.
//
// Every rejected-evidence path below — a rogue-responder OCSP, a CRL signed by
// something that is not a proven issuer of the signer, a CRL whose issuer does
// not assert cRLSign (RFC 5280 §6.3.3(f)), a delta / future-dated / stale /
// out-of-scope CRL, a stale OCSP, an unparseable blob — left ocspVerdict == -1
// and hasValidCrl == false, and so landed on exactly the NoDssData a /DSS
// containing nothing at all produces. A document carrying ACTIVELY FORGED
// revocation evidence was presented identically to one carrying none.
//
// This records the FIRST rule that refused something, so classification can
// report it. It is diagnostic only: nothing here can make refused evidence
// count, and nothing here can produce a Revoked verdict. It cannot even be
// consulted unless the verdict was already going to be "no revocation
// evidence".
//
// FIRST, not last, deliberately: the earliest refusal is the one closest to the
// artifact the document actually supplied, and a later, vaguer rule
// overwriting it would lose the specific finding.
struct EvidenceRejection {
    bool any = false;
    QString rule;
    void note(const QString& r) {
        if (!any) { any = true; rule = r; }
    }
};

// Decode a DER blob into an X509*. Returns nullptr on failure (drains
// the OpenSSL error queue so subsequent ops don't see stale errors).
X509* derToX509(const std::vector<uint8_t>& der)
{
    if (der.empty()) return nullptr;
    const unsigned char* p = der.data();
    X509* cert = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
    if (!cert) ERR_clear_error();
    return cert;
}

// Add every cert in @p certs to @p store via X509_STORE_add_cert. The
// store keeps its own ref, so we always free our local handle. Duplicate
// adds are tolerated (OpenSSL returns 0 + sets an error code; we drain).
void addCertsToStore(X509_STORE* store,
                     const std::vector<std::vector<uint8_t>>& certs)
{
    if (!store) return;
    for (const auto& der : certs) {
        X509* cert = derToX509(der);
        if (!cert) continue;
        if (X509_STORE_add_cert(store, cert) != 1) {
            ERR_clear_error();
        }
        X509_free(cert);
    }
}

// NOTE (meta-audit follow-up F23, gap PDF-39): the local verifyChain()
// helper that used to live here — signer + store + untrusted stack -> bool —
// has been replaced by CertChainValidator::validate(), which takes the anchor
// set as an explicit parameter and returns a specific verdict instead of a
// bool.

// Compare two X509_NAME by canonical DER. OpenSSL's X509_NAME_cmp does
// this internally.
bool sameName(X509_NAME* a, X509_NAME* b)
{
    if (!a || !b) return false;
    return X509_NAME_cmp(a, b) == 0;
}

// ═══════════════════════════════════════════════════════════════════════
// SECURITY (phase-42 remediation): who is allowed to speak about revocation.
//
// The revocation step used to pool "every cert in /DSS + any intermediates"
// and treat the whole pool as entitled to authorize revocation evidence — as
// the CRL signer, and (worse) as TRUST ANCHORS seeding the X509_STORE handed
// to OCSP_basic_verify. `/DSS /Certs` is read verbatim out of the PDF by
// DssReader with no filtering whatsoever, so that pool is ATTACKER-SUPPLIED.
//
// This file already states the rule at the chain-validation step: "/DSS-
// embedded certs are NEVER added to the trust store. Doing so would grant
// trust-anchor status to anything the document author chose to embed." The
// revocation step was doing exactly that.
//
// The residual attack (which survived the first CRL fix, because that fix
// checked the CRL's signature against a key it resolved by DN):
//   1. mint a self-signed rogue cert whose SUBJECT DN equals the real
//      issuer's DN, and append it to /DSS /Certs;
//   2. mint a CRL with that issuer DN, signed by the rogue key;
//   3. DN lookup finds the rogue cert, X509_CRL_verify succeeds under the
//      rogue key, and the forged CRL is honoured.
// Forged REVOCATION was unconditional — revocation is classified before the
// chain-trust check, so any signed PDF could be forced to `Revoked` with no
// valid chain at all. Forged NON-revocation yielded `LtvValidated` for a
// genuinely revoked certificate.
//
// The fix: a candidate may authorize revocation data about the signer only if
// it is CRYPTOGRAPHICALLY the signer's issuer — its public key actually
// verifies the signer certificate's signature. A DN is attacker-chosen text;
// a signature is not. The rogue cert fails because the signer certificate was
// signed by the REAL issuer's key, which the attacker does not have.
//
// This also strengthens the OCSP path: the store seeded for OCSP_basic_verify
// now contains only real issuers, so a delegated responder still validates
// (it chains to its genuine issuer) while a rogue anchor cannot be injected.
bool certIssuedSigner(X509* candidate, X509* signerCert)
{
    if (!candidate || !signerCert) return false;
    // Cheap structural gate first: only a cert whose SUBJECT is the signer's
    // stated issuer is even a candidate...
    if (!sameName(X509_get_subject_name(candidate),
                  X509_get_issuer_name(signerCert))) {
        return false;
    }
    // ...and then the part that cannot be forged: did this key actually sign
    // the signer certificate?
    EVP_PKEY* pub = X509_get0_pubkey(candidate);
    if (!pub) { ERR_clear_error(); return false; }
    const int rc = X509_verify(signerCert, pub);
    if (rc != 1) ERR_clear_error();
    return rc == 1;
}

// ═══════════════════════════════════════════════════════════════════════
// F54 (phase-51) — THE PARTIAL_CHAIN PRECONDITION IS NOW A TYPE, NOT A COMMENT.
//
// ocspResponseIsAuthorized() below seeds an X509_STORE with a set of candidate
// issuers and turns on X509_V_FLAG_PARTIAL_CHAIN, which makes EVERY member of
// that store a trust anchor for the responder-authentication path. That is
// correct exactly as long as membership is restricted to certificates PROVEN by
// signature to have issued the signer (certIssuedSigner). If an unfiltered pool
// ever reaches it, a rogue certificate lifted verbatim out of the document's
// /DSS becomes a trust anchor and the whole RFC 6960 §4.2.2.2 check collapses —
// the precise bypass phase-42 closed.
//
// Until this phase the only thing enforcing that was a COMMENT on a
// `const std::vector<X509*>&` parameter, and the one caller's compliance. A
// second caller, or a refactor that moved the filtering step, would have
// re-opened the hole silently and compiled clean.
//
// SignerIssuerPool makes it structural. Its constructor is private and its only
// construction path is fromCandidates(), which runs certIssuedSigner() on every
// admitted certificate. A raw std::vector<X509*> therefore cannot bind to the
// parameter at all — the precondition is discharged by the type system rather
// than by a reader's diligence.
//
// The pool is a NON-OWNING view: the certificates belong to the caller (they
// live in `parsedCands` in verifyWith()), exactly as the vector it replaces did,
// and it must not outlive them.
class SignerIssuerPool {
public:
    /// The ONLY way to obtain a populated pool. Admits a candidate only when its
    /// key actually signed @p signerCert.
    static SignerIssuerPool fromCandidates(const std::vector<X509*>& candidates,
                                           X509* signerCert)
    {
        SignerIssuerPool pool;
        if (!signerCert) return pool;
        pool.signer_ = signerCert;
        for (X509* c : candidates) {
            if (certIssuedSigner(c, signerCert)) pool.certs_.push_back(c);
        }
        return pool;
    }

    const std::vector<X509*>& certs() const noexcept { return certs_; }
    X509* signer() const noexcept { return signer_; }
    bool empty() const noexcept { return certs_.empty(); }

private:
    SignerIssuerPool() = default;
    std::vector<X509*> certs_;
    X509* signer_ = nullptr;
};

// ═══════════════════════════════════════════════════════════════════════
// F54 residual (phase-51) — THE SECOND POOL, AND WHY IT MUST BE A SECOND TYPE.
//
// The CRL side consults TWO sets of certificates, and they are NOT the same set
// under a different name:
//
//   * the DIRECT issuers of the target — a SignerIssuerPool, admitted only by
//     certIssuedSigner(). These may sign a CRL that speaks for the target
//     directly (CrlAuthority::Direct).
//   * the PATH ANCESTORS of the target — everything strictly ABOVE the target's
//     direct issuer on the path OpenSSL verified, i.e. path[idx+2..]. These may
//     sign an INDIRECT CRL about the target (CrlAuthority::Indirect, phase-49
//     F47 item F4).
//
// certIssuedSigner() is the WRONG admission rule for the ancestors: a
// grandparent CA did not sign the target certificate, so running the signer-
// issuer filter over the ancestors would empty the pool and delete indirect CRL
// support outright. Conversely the ancestors must never be handed to the direct
// slot: accepting an ancestor's CRL as a DIRECT statement about the leaf is
// exactly the phase-49 review finding H1 — absence of the leaf's serial from a
// grandparent's list is a completeness claim nothing establishes, and honouring
// it yields LtvValidated for a revoked signature.
//
// Until now both sets were `std::vector<X509*>`, so that swap was a silent,
// type-correct edit sitting in adjacent parameters of three functions. Two
// distinct types make it a compile error. This is STRICTLY STRONGER than what
// phase-51 achieved on the OCSP side, which only stopped an unfiltered vector.
//
// PROVENANCE. An ancestor's authority does not come from a predicate we can
// re-run on it in isolation — it comes from WHERE IT SAT on a chain-validated
// path. So the admission rule is structural rather than cryptographic, and the
// only construction path is VerifiedPath::ancestorsAboveIssuerOf() below, whose
// input is the path CertChainValidator actually verified. An arbitrary bag of
// /DSS certificates cannot become one.
//
// NON-OWNING view over the VerifiedPath that produced it; must not outlive it.
class VerifiedPath;

class PathAncestorPool {
public:
    const std::vector<X509*>& certs() const noexcept { return certs_; }
    bool empty() const noexcept { return certs_.empty(); }

private:
    friend class VerifiedPath;
    PathAncestorPool() = default;
    std::vector<X509*> certs_;
};

// The chain-validated path, leaf first / anchor last, parsed once and OWNED.
// Replaces two hand-rolled parse loops plus their matching X509_free sweeps —
// which had to be reached on every exit path — with one RAII holder, and is the
// sole producer of PathAncestorPool.
class VerifiedPath {
public:
    explicit VerifiedPath(const std::vector<std::vector<uint8_t>>& verifiedPathDer)
    {
        // F3 (phase-51 review). The reserve() is load-bearing, not a
        // micro-optimisation. A constructor that throws part-way through does
        // NOT get its destructor run, so a `bad_alloc` out of push_back() would
        // abandon every X509* parsed so far PLUS the one in `c` — the exact leak
        // the RAII holder exists to abolish. Reserving the whole capacity up
        // front means push_back() below cannot allocate and therefore cannot
        // throw; and if reserve() itself throws, certs_ is still empty and
        // nothing has been parsed, so there is nothing to leak. The count is an
        // upper bound (derToX509 may return null and be skipped), which is fine.
        certs_.reserve(verifiedPathDer.size());
        for (const auto& der : verifiedPathDer) {
            if (X509* c = derToX509(der)) certs_.push_back(c);
        }
    }
    ~VerifiedPath() { for (X509* c : certs_) X509_free(c); }

    VerifiedPath(const VerifiedPath&) = delete;
    VerifiedPath& operator=(const VerifiedPath&) = delete;
    VerifiedPath(VerifiedPath&&) = delete;
    VerifiedPath& operator=(VerifiedPath&&) = delete;

    std::size_t size() const noexcept { return certs_.size(); }
    X509* at(std::size_t i) const { return certs_[i]; }

    /// Everything strictly above the DIRECT issuer of certs_[idx] — that is
    /// certs_[idx+2..]. Empty when the path is too short, which is the
    /// fail-closed direction: no ancestors means no indirect CRL is honoured.
    PathAncestorPool ancestorsAboveIssuerOf(std::size_t idx) const
    {
        PathAncestorPool pool;
        for (std::size_t j = idx + 2; j < certs_.size(); ++j) {
            pool.certs_.push_back(certs_[j]);
        }
        return pool;
    }

private:
    std::vector<X509*> certs_;
};

// The two pools are interchangeable in exactly one place: whether a CRL's
// signature verifies under SOME member of a set that is entitled to sign it.
// crlIsAuthorized() answers only that, and the entitlement — direct issuer vs
// path ancestor — is decided by its caller, crlAuthorityFor(), which is where
// the two are kept apart. Constraining it to the two pool types (rather than
// taking a `const std::vector<X509*>&`) means there is NO raw-vector entry point
// into CRL authorisation at all.
template <class Pool>
concept CrlSigningAuthorityPool =
    std::same_as<Pool, SignerIssuerPool> || std::same_as<Pool, PathAncestorPool>;

// Authenticate an OCSP response before trusting its status. Per RFC 6960
// §4.2.2.2 an OCSP response is only authoritative if signed by (a) the CA
// that issued the target certificate, or (b) a delegated responder cert
// issued by that CA and bearing the id-kp-OCSPSigning extended key usage.
// WITHOUT this check a rogue responder could forge a GOOD status for any
// certificate and we would trust it — a full revocation-bypass. We build a
// trust store from the issuer candidates (the only certs entitled to answer
// for this signer) and require OCSP_basic_verify to authenticate the
// responder against it. A rogue response — signed by an unrelated key and
// carrying its own self-signed responder cert — fails the chain-to-trusted-
// issuer check and is rejected here.
bool ocspResponseIsAuthorized(OCSP_BASICRESP* basic,
                              const SignerIssuerPool& issuerCandidates)
{
    if (!basic || issuerCandidates.empty()) return false;

    // F54 DEFENCE IN DEPTH. SignerIssuerPool's private constructor already makes
    // an unfiltered pool unrepresentable, but a cast, a placement trick or a
    // future edit to fromCandidates() could subvert it. Re-run the invariant at
    // the point of use so the violation is caught before an attacker-supplied
    // certificate is promoted to trust-anchor status by PARTIAL_CHAIN. This is a
    // redundant check by construction; if it ever fires, the type has been
    // defeated and the bypass is live.
    //
    // F4 (phase-51 review): this WAS an `assert()`, and that made the failure
    // mode worse than the thing it guarded. Two reasons it is now a fail-closed
    // return:
    //   1. CI builds Debug, so a broken invariant ABORTED the process. This
    //      project has a documented hazard (crt_modal_dialogs_hang_ci) where an
    //      abort in CI surfaces as a HUNG RUN rather than a red test — the
    //      moment you most want a legible failure is the moment you got a hang.
    //      Refusing the response instead yields a clean, attributable test
    //      failure, and the security outcome is the same or safer: an
    //      unauthorised OCSP response is not trusted either way.
    //   2. It ran certIssuedSigner() — a full X509_verify() — over the whole
    //      pool on every Debug OCSP call, doubling the pool verification work
    //      that fromCandidates() had already done. Now that the result is acted
    //      on rather than asserted, the cost buys an actual decision.
    // The diagnostic text is preserved and emitted, so a violation is still
    // LOUD; it is simply loud in a form that a test run can report.
    if (issuerCandidates.signer() == nullptr) {
        qWarning("LtvValidator: SignerIssuerPool with members but no signer — "
                 "invariant broken; refusing the OCSP response (fail closed)");
        return false;
    }
    for (X509* issuer : issuerCandidates.certs()) {
        if (!certIssuedSigner(issuer, issuerCandidates.signer())) {
            qWarning("LtvValidator: SignerIssuerPool admitted a cert that did "
                     "not issue the signer — PARTIAL_CHAIN would make it a "
                     "trust anchor; refusing the OCSP response (fail closed)");
            return false;
        }
    }

    X509_STORE* trust = X509_STORE_new();
    if (!trust) return false;
    STACK_OF(X509)* responders = sk_X509_new_null();
    for (X509* issuer : issuerCandidates.certs()) {
        if (!issuer) continue;
        // Store keeps its own ref; the stack borrows (freed with sk_X509_free,
        // which does NOT free the certs — the caller still owns them).
        X509_STORE_add_cert(trust, issuer);
        if (responders) sk_X509_push(responders, issuer);
    }

    // X509_V_FLAG_PARTIAL_CHAIN — REQUIRED for this store to work at all, and
    // NOT a loosening. Found by phase-48 when the LTV suite was migrated off a
    // self-signed end-entity fixture onto a real root → intermediate → leaf
    // chain: every genuine OCSP response signed by the INTERMEDIATE CA
    // silently failed to authenticate, degrading to NoDssData.
    //
    // Cause: without this flag, X509_verify_cert() insists the path terminate
    // at a SELF-SIGNED certificate. `trust` here holds only the issuer
    // candidates, and an intermediate CA is not self-signed, so no path could
    // ever terminate — the check could only ever succeed for a self-signed
    // issuer, which is exactly (and only) what the old fixture was. Since
    // essentially every real-world signing certificate is issued by an
    // intermediate, PDFE could not honour OCSP evidence for any of them.
    //
    // Why enabling it is correct rather than permissive:
    //   * RFC 6960 §4.2.2.2 asks whether the responder is the CA that issued
    //     the TARGET (or that CA's id-kp-OCSPSigning delegate). The relevant
    //     authority for that question IS the issuing CA, which is what this
    //     store contains — so it belongs at the top of this path by
    //     construction.
    //   * Membership of `trust` is not attacker-controlled: it comes from a
    //     SignerIssuerPool, whose only construction path runs certIssuedSigner()
    //     on every admitted certificate. F54 (phase-51) made that STRUCTURAL —
    //     the parameter type cannot bind a raw std::vector<X509*>, so the
    //     restriction is enforced by the compiler rather than by this comment
    //     plus the caller's diligence, which is all that held it before. A cert
    //     planted in /DSS never reaches here.
    //   * The issuing CA's OWN trustworthiness is established separately, by
    //     CertChainValidator in step 3, against the real anchor set. This
    //     check is not the place that decides it, and pretending otherwise is
    //     what broke it.
    //   * A rogue responder is unaffected: its self-signed cert is not in
    //     `trust`, so its path still terminates nowhere trusted and it is
    //     still rejected. LtvResponderTest.ForgedGoodOcspFromRogueResponder-
    //     Rejected and RogueKeyEvidenceRejectedForEveryEvidenceType pin both
    //     directions.
    if (X509_VERIFY_PARAM* vp = X509_STORE_get0_param(trust)) {
        X509_VERIFY_PARAM_set_flags(vp, X509_V_FLAG_PARTIAL_CHAIN);
    }

    // OCSP_basic_verify: locates the responder cert (from the response's own
    // embedded certs or `responders`), verifies the response signature, then
    // verifies the responder chains to a trusted issuer in `trust` and is
    // authorized to answer for the target (the issuer itself, or an
    // id-kp-OCSPSigning delegate issued by it).
    int rc = OCSP_basic_verify(basic, responders, trust, 0);
    if (rc <= 0) ERR_clear_error();

    if (responders) sk_X509_free(responders);
    X509_STORE_free(trust);
    return rc > 0;
}

// PHASE-50 REVIEW, FIX 1 (OCSP half). Does this basic response contain a
// single-response whose CertID names @p signerCert?
//
// The F53 recorder used to note a "rogue responder" rejection for EVERY
// response that failed ocspResponseIsAuthorized(), which is checked — rightly,
// and unchanged — before anything is read out of the response. But a document
// carrying long-term validation material normally embeds one OCSP response per
// chain link, and the response covering the INTERMEDIATE cannot authenticate
// as a responder for the SIGNER. Honest, ordinary content was being reported
// as a rogue responder.
//
// This probe answers the question "is the response even about this
// certificate?" and nothing else. `status`, `reason` and the three times are
// received because OCSP_resp_find_status() has no shorter form; every one of
// them is DISCARDED. Nothing in this function can make an unauthenticated
// response count as evidence — its only caller uses it to decide whether to
// print a diagnostic, and discards the blob regardless of the answer.
bool ocspResponseNamesCert(OCSP_BASICRESP* basic, X509* signerCert,
                           const SignerIssuerPool& issuerCandidates)
{
    if (!basic || !signerCert) return false;
    for (X509* issuer : issuerCandidates.certs()) {
        if (!issuer) continue;
        OCSP_CERTID* id = OCSP_cert_to_id(EVP_sha1(), signerCert, issuer);
        if (!id) {
            ERR_clear_error();
            continue;
        }
        int status = -1, reason = 0;                    // discarded
        ASN1_GENERALIZEDTIME* thisUpd = nullptr;        // discarded
        ASN1_GENERALIZEDTIME* nextUpd = nullptr;        // discarded
        ASN1_GENERALIZEDTIME* revoked = nullptr;        // discarded
        const int found = OCSP_resp_find_status(basic, id, &status, &reason,
                                                &revoked, &thisUpd, &nextUpd);
        OCSP_CERTID_free(id);
        if (found == 1) return true;
    }
    ERR_clear_error();
    return false;
}

// Walk @p ocspBlobs looking for a single-response that names @p signerCert.
// Returns:
//   - V_OCSP_CERTSTATUS_GOOD / _REVOKED / _UNKNOWN if a matching response was
//     found.
//   - -1 if no OCSP blob in the set covers this cert (caller treats as
//     "no revocation evidence").
// @p rejection (optional, F53) records the first rule that refused a blob.
int evaluateOcspForCert(X509* signerCert,
                         const SignerIssuerPool& issuerCandidates,
                         const std::vector<std::vector<uint8_t>>& ocspBlobs,
                         EvidenceRejection* rejection = nullptr)
{
    if (!signerCert) return -1;

    int verdict = -1;
    for (const auto& der : ocspBlobs) {
        if (der.empty()) continue;
        const unsigned char* p = der.data();
        OCSP_RESPONSE* resp = d2i_OCSP_RESPONSE(nullptr, &p,
                                                 static_cast<long>(der.size()));
        if (!resp) {
            ERR_clear_error();
            if (rejection) rejection->note(QObject::tr(
                "OCSP response could not be parsed (malformed DER)"));
            continue;
        }
        if (OCSP_response_status(resp) != OCSP_RESPONSE_STATUS_SUCCESSFUL) {
            OCSP_RESPONSE_free(resp);
            if (rejection) rejection->note(QObject::tr(
                "OCSP responder returned an unsuccessful response status"));
            continue;
        }
        OCSP_BASICRESP* basic = OCSP_response_get1_basic(resp);
        if (!basic) {
            ERR_clear_error();
            OCSP_RESPONSE_free(resp);
            if (rejection) rejection->note(QObject::tr(
                "OCSP basic response could not be extracted"));
            continue;
        }

        // SECURITY (RFC 6960): authenticate the responder BEFORE reading any
        // status out of the response. An unauthenticated/rogue responder's
        // GOOD is worthless — ignore the whole blob.
        if (!ocspResponseIsAuthorized(basic, issuerCandidates)) {
            // PHASE-50 REVIEW, FIX 1. The check above stays exactly where it
            // is; only the DIAGNOSTIC moves. Report a rogue responder only for
            // a response that actually carries a CertID for this certificate —
            // a response about another link of the chain is normal B-LT
            // content, not evidence anyone forged. The blob is discarded
            // either way, so the fail-closed behaviour is unchanged.
            const bool aboutThisCert =
                ocspResponseNamesCert(basic, signerCert, issuerCandidates);
            OCSP_BASICRESP_free(basic);
            OCSP_RESPONSE_free(resp);
            if (rejection && aboutThisCert) rejection->note(QObject::tr(
                "OCSP response failed responder authorisation "
                "(rogue responder: not a proven issuer of the signer)"));
            continue;
        }

        // Try every plausible issuer until one matches. A self-signed
        // signer matches itself.
        for (X509* issuer : issuerCandidates.certs()) {
            if (!issuer) continue;
            OCSP_CERTID* id = OCSP_cert_to_id(EVP_sha1(), signerCert, issuer);
            if (!id) {
                ERR_clear_error();
                continue;
            }
            int status = -1, reason = 0;
            ASN1_GENERALIZEDTIME* thisUpd = nullptr;
            ASN1_GENERALIZEDTIME* nextUpd = nullptr;
            ASN1_GENERALIZEDTIME* revoked = nullptr;
            int found = OCSP_resp_find_status(basic, id, &status, &reason,
                                                &revoked, &thisUpd, &nextUpd);
            OCSP_CERTID_free(id);
            if (found == 1) {
                // Revoked is permanent — honour it even if the
                // response is stale (a cert doesn't un-revoke).
                if (status == V_OCSP_CERTSTATUS_REVOKED) {
                    OCSP_BASICRESP_free(basic);
                    OCSP_RESPONSE_free(resp);
                    return V_OCSP_CERTSTATUS_REVOKED;
                }
                // For non-revoked statuses, reject stale responses.
                if (OCSP_check_validity(thisUpd, nextUpd,
                                        kOcspDriftSec, kOcspMaxAgeSec) != 1) {
                    ERR_clear_error();
                    if (rejection) rejection->note(QObject::tr(
                        "OCSP response rejected on freshness "
                        "(thisUpdate/nextUpdate outside the accepted window)"));
                    break;
                }
                if (status == V_OCSP_CERTSTATUS_GOOD) {
                    verdict = V_OCSP_CERTSTATUS_GOOD;
                }
                break;
            }
        }

        OCSP_BASICRESP_free(basic);
        OCSP_RESPONSE_free(resp);
    }
    return verdict;
}

// SECURITY (RFC 5280 §6.3.3) -- the CRL half of the responder-authentication
// rule that DCA 0005 established for OCSP.
//
// A CRL is evidence only if its SIGNATURE verifies under a key we already have
// reason to trust for that issuer. Matching the issuer DN authenticates
// NOTHING: a DN is attacker-chosen text, so anyone who can place bytes in a
// document's /DSS can mint a CRL carrying the real issuer's DN. Until
// phase-42 both CRL consumers below did exactly that -- DN match plus a
// freshness window, no `X509_CRL_verify` anywhere -- so a forged CRL was
// accepted as "valid revocation evidence" (-> LtvValidated on a certificate
// that may in fact be revoked) and a forged CRL listing the signer's serial
// was accepted as proof of revocation.
//
// This is the same defect species as DCA 0005 (`OCSP_basic_verify` missing);
// it survived that fix because the fix was written against the one artifact
// type the review named. Found by the class-level oracle
// LtvResponderTest.RogueKeyEvidenceRejectedForEveryEvidenceType (phase-42).
//
// NOTE (phase-42 remediation, made STRUCTURAL by the F54 residual): the
// signature check below is only as good as the key it is checked against. A
// candidate set taken straight from /DSS lets an attacker supply the verifying
// key too, which defeats the whole check.
//
// That precondition used to be stated HERE, in prose, over a plain
// `const std::vector<X509*>&` — the same shape phase-51 had just removed from
// the OCSP path one screen up, left behind on the CRL path. It is no longer a
// convention the one caller happens to honour: the parameter is constrained to
// CrlSigningAuthorityPool, so the only things that bind are SignerIssuerPool
// (whose private constructor admits only certs PROVEN by signature to have
// issued the target) and PathAncestorPool (whose only producer is the
// chain-validated path). An unfiltered `std::vector<X509*>` does not compile.
//
// Which of the two is appropriate is decided by crlAuthorityFor(), and the two
// cannot be swapped there either — see PathAncestorPool's header comment.
template <CrlSigningAuthorityPool Pool>
bool crlIsAuthorized(X509_CRL* crl, const Pool& issuerCandidates)
{
    if (!crl) return false;
    X509_NAME* crlIssuer = X509_CRL_get_issuer(crl);
    for (X509* issuer : issuerCandidates.certs()) {
        if (!issuer) continue;
        // Only a candidate whose SUBJECT is the CRL's issuer may sign it.
        if (!sameName(crlIssuer, X509_get_subject_name(issuer))) continue;
        // RFC 5280 §6.3.3(f) — keyUsage/cRLSign (F47 item F1, phase-48).
        //
        // A certificate whose keyUsage extension is PRESENT is entitled to do
        // only what that extension asserts. If it does not assert cRLSign, it
        // may not sign CRLs, and a CRL bearing its signature is not evidence
        // however well the signature verifies.
        //
        // X509_get_key_usage() returns UINT32_MAX when the extension is
        // ABSENT, so a certificate carrying no keyUsage at all is left
        // unconstrained. That is deliberate and is what RFC 5280 §4.2.1.3
        // means: the restriction exists only when the issuer chose to state
        // one. Do NOT "tighten" this into a requirement that keyUsage be
        // present — that would reject legitimate CAs.
        //
        // HISTORY. Phase-42 wrote this rule, measured it, and withdrew it
        // rather than ship it half-applied: the whole signing corpus rested on
        // tests/fixtures/test_signer.pfx, a SELF-SIGNED END-ENTITY certificate
        // (CA:FALSE; keyUsage critical digitalSignature, contentCommitment; no
        // cRLSign) that issues its own CRLs, so the rule correctly rejected
        // every CRL in the suite. Phase-48 supplies the missing fixture —
        // tests/fixtures/crl_chain/, built by
        // tests/fixtures/gen_two_level_ca_signer.py — which provides a real
        // intermediate CA bearing cRLSign, plus a structurally identical one
        // that lacks it, so both sides of this branch are exercised by tests
        // (CrlIssuerKeyUsageTest in tests/test_crl_issuer_keyusage.cpp).
        //
        // This is defence-in-depth, not the primary bypass fix:
        // certIssuedSigner() already requires the candidate's key to have
        // actually signed the signer certificate. What this additionally
        // blocks is a genuinely issuing CA whose own keyUsage forbids CRL
        // signing.
        if (!(X509_get_key_usage(issuer) & KU_CRL_SIGN)) continue;
        EVP_PKEY* pub = X509_get0_pubkey(issuer);
        if (!pub) { ERR_clear_error(); continue; }
        if (X509_CRL_verify(crl, pub) == 1) return true;
        ERR_clear_error();
    }
    return false;
}

// ---------------------------------------------------------------------------
// issuingDistributionPoint scope interpretation (F47 item F3, phase-49).
//
// WHAT CHANGED AND WHY IT IS A WIDENING.
// Phase-42 rejected a CRL bearing ANY critical extension, on the honest ground
// that we implemented none of them and RFC 5280 §6.3.3 requires rejecting what
// you cannot interpret. issuingDistributionPoint is ALWAYS critical, so the
// practical effect was: every IDP-bearing CRL was refused outright. That is
// sound but crude — it discards legitimate revocation evidence, and "no
// evidence" is exactly the state an attacker wants.
//
// This replaces the blanket refusal with an actual scope interpretation for
// issuingDistributionPoint ONLY. Every other critical extension is still
// refused, unchanged. Because accepting a scoped CRL as if it covered a
// certificate outside its scope is UNSOUND, the rule below is written so that
// anything we cannot positively establish is a refusal, not an acceptance.
//
// WHAT REMAINS REFUSED, deliberately (see phase-49-dispositions.md):
//   * onlySomeReasons — a reason-partitioned CRL is not a complete statement
//     about a certificate. Interpreting it needs the reason-code union logic of
//     RFC 5280 §6.3.3(b)(2) across a SET of CRLs, which we do not model.
//   * onlyContainsAttributeCertificates — we validate public-key certificates.
//   * a distributionPoint given as a nameRelativeToCRLIssuer (an RDN fragment).
//     Resolving it means reconstructing a full DN against the CRL issuer's,
//     and a mis-reconstruction here silently widens scope.
//   * every critical extension that is not issuingDistributionPoint.
//
// Returns true only when the CRL's stated scope demonstrably COVERS
// @p targetCert. A CRL with no IDP at all is unscoped and covers everything
// its issuer is authoritative for, so it returns true.
bool idpCoversCert(X509_CRL* crl, X509* targetCert)
{
    if (!crl || !targetCert) return false;

    ISSUING_DIST_POINT* idp = static_cast<ISSUING_DIST_POINT*>(
        X509_CRL_get_ext_d2i(crl, NID_issuing_distribution_point, nullptr,
                             nullptr));
    if (!idp) {
        // Distinguish "absent" from "present but unparseable". An IDP we
        // cannot decode is a scope statement we cannot honour → refuse.
        const bool present =
            X509_CRL_get_ext_by_NID(crl, NID_issuing_distribution_point, -1) >= 0;
        if (present) { ERR_clear_error(); return false; }
        return true;  // genuinely unscoped
    }

    bool covers = true;

    // Reason-partitioned and attribute-certificate CRLs: unimplemented, refuse.
    if (idp->onlysomereasons) covers = false;
    if (covers && idp->onlyattr)  covers = false;

    // onlyContainsUserCerts / onlyContainsCACerts must agree with what the
    // target certificate actually IS. X509_check_ca() > 0 means CA.
    if (covers && idp->onlyuser && X509_check_ca(targetCert) > 0) covers = false;
    if (covers && idp->onlyCA   && X509_check_ca(targetCert) == 0) covers = false;

    // A named distribution point covers the certificate only if the
    // certificate itself points at that distribution point. A certificate
    // carrying no cRLDistributionPoints names no distribution point, so a
    // DP-scoped CRL is not authoritative for it.
    if (covers && idp->distpoint) {
        if (idp->distpoint->type != 0 || !idp->distpoint->name.fullname) {
            covers = false;  // nameRelativeToCRLIssuer — see note above
        } else {
            STACK_OF(DIST_POINT)* certDps = static_cast<STACK_OF(DIST_POINT)*>(
                X509_get_ext_d2i(targetCert, NID_crl_distribution_points,
                                 nullptr, nullptr));
            bool matched = false;
            if (certDps) {
                for (int i = 0; i < sk_DIST_POINT_num(certDps) && !matched; ++i) {
                    DIST_POINT* dp = sk_DIST_POINT_value(certDps, i);
                    if (!dp || !dp->distpoint) continue;
                    if (dp->distpoint->type != 0 || !dp->distpoint->name.fullname)
                        continue;
                    GENERAL_NAMES* a = idp->distpoint->name.fullname;
                    GENERAL_NAMES* b = dp->distpoint->name.fullname;
                    for (int x = 0; x < sk_GENERAL_NAME_num(a) && !matched; ++x) {
                        for (int y = 0; y < sk_GENERAL_NAME_num(b); ++y) {
                            if (GENERAL_NAME_cmp(sk_GENERAL_NAME_value(a, x),
                                                 sk_GENERAL_NAME_value(b, y)) == 0) {
                                matched = true;
                                break;
                            }
                        }
                    }
                }
                CRL_DIST_POINTS_free(certDps);
            } else {
                ERR_clear_error();
            }
            covers = matched;
        }
    }

    ISSUING_DIST_POINT_free(idp);
    ERR_clear_error();
    return covers;
}

// True when the CRL's issuingDistributionPoint asserts indirectCRL, i.e. the
// CRL claims to carry revocations on behalf of issuers other than itself.
// Used to gate the indirect path (F47 item F4) — a CRL that does NOT say this
// may never speak for another issuer.
bool crlAssertsIndirect(X509_CRL* crl)
{
    if (!crl) return false;
    ISSUING_DIST_POINT* idp = static_cast<ISSUING_DIST_POINT*>(
        X509_CRL_get_ext_d2i(crl, NID_issuing_distribution_point, nullptr,
                             nullptr));
    if (!idp) { ERR_clear_error(); return false; }
    const bool indirect = idp->indirectCRL != 0;
    ISSUING_DIST_POINT_free(idp);
    return indirect;
}

// Structural + freshness acceptability of a CRL, independent of its signature
// (phase-42 remediation). Everything here FAILS CLOSED: an unacceptable CRL is
// ignored, which downgrades to "no revocation evidence", never to a verdict.
//
// Each rule closes a hole that existed before:
//   * DELTA CRLs were neither handled nor rejected. A delta lists only changes
//     since a base CRL, so on its own it satisfied "we have a valid CRL" while
//     hiding every revocation recorded in the base — the exact inverse of what
//     revocation checking is for.
//   * CRITICAL extensions we do not implement were ignored. issuingDistribution
//     Point in particular SCOPES a CRL (to a subset of certificates, to
//     specific reason codes, or marks it indirect); honouring a scoped CRL as
//     if it covered everything is unsound.
//   * thisUpdate was NEVER checked, so a CRL dated in the future was accepted.
//   * an ABSENT nextUpdate meant "never expires" — a CRL from any era counted
//     as fresh evidence forever. There is now a max-age floor either way,
//     mirroring kOcspMaxAgeSec.
//
// PHASE-49 (F47 item F3): acceptability is now CERTIFICATE-RELATIVE. It has to
// be — issuingDistributionPoint scopes a CRL to a subset of certificates, so
// "is this CRL acceptable" has no answer until you say acceptable ABOUT WHAT.
// The @p targetCert parameter is that. Every caller passes the certificate
// whose revocation status is being decided.
bool crlIsAcceptable(X509_CRL* crl, X509* targetCert)
{
    if (!crl || !targetCert) return false;

    // Delta CRL — carries the deltaCRLIndicator extension (always critical).
    if (X509_CRL_get_ext_by_NID(crl, NID_delta_crl, -1) >= 0) return false;

    // Critical extensions. issuingDistributionPoint is now INTERPRETED (see
    // idpCoversCert); every other critical extension remains an outright
    // reject, exactly as phase-42 left it.
    const int nExts = X509_CRL_get_ext_count(crl);
    for (int i = 0; i < nExts; ++i) {
        X509_EXTENSION* ext = X509_CRL_get_ext(crl, i);
        if (!ext || !X509_EXTENSION_get_critical(ext)) continue;
        const int nid = OBJ_obj2nid(X509_EXTENSION_get_object(ext));
        if (nid == NID_issuing_distribution_point) continue;
        return false;
    }

    // Scope. Applies whether or not the IDP was marked critical: a scope
    // statement we can read constrains us regardless of its criticality flag.
    if (!idpCoversCert(crl, targetCert)) return false;

    // thisUpdate must exist, and must not be in the FUTURE beyond clock skew.
    // This is a NEW rule: thisUpdate was previously never looked at, so a CRL
    // dated next month was accepted as evidence about today.
    const ASN1_TIME* thisUpd = X509_CRL_get0_lastUpdate(crl);
    if (!thisUpd) return false;
    {
        time_t skewCeiling = time(nullptr) + kCrlDriftSec;
        if (X509_cmp_time(thisUpd, &skewCeiling) > 0) return false;
    }

    // Freshness.
    //
    // When nextUpdate is present it is authoritative, and "expired" means
    // strictly in the past. That is EXACTLY the pre-existing test
    // (X509_cmp_current_time(nextUpd) < 0), deliberately unchanged and merely
    // relocated here — the expiry boundary is pinned by
    // CrlAtExactExpiryBoundary / CrlJustPastExpiryRejected, which sit one
    // minute either side of now, and it is not one of the defects under repair.
    // Applying a skew window here would silently move that boundary.
    //
    // When nextUpdate is ABSENT, the CRL used to never expire — a list issued
    // at any point in history counted as fresh evidence forever. Fall back to a
    // max age on thisUpdate, mirroring kOcspMaxAgeSec.
    const ASN1_TIME* nextUpd = X509_CRL_get0_nextUpdate(crl);
    if (nextUpd) {
        if (X509_cmp_current_time(nextUpd) < 0) return false;  // expired
    } else {
        time_t floorT = time(nullptr) - kCrlMaxAgeSec;
        if (X509_cmp_time(thisUpd, &floorT) < 0) return false;  // too old
    }
    return true;
}

// ---------------------------------------------------------------------------
// Who is allowed to speak for a certificate's revocation status.
//
// Direct   — the CRL's issuer IS the certificate's own issuer, and its
//            signature verifies under a key PROVEN to have issued the
//            certificate (certIssuedSigner). This is the pre-phase-49 rule and
//            is unchanged.
// Indirect — F47 item F4, phase-49. The CRL's issuer is someone ELSE, and the
//            CRL says so by asserting indirectCRL in its
//            issuingDistributionPoint.
//
// THE INDIRECT RULE IS DELIBERATELY NARROWER THAN RFC 5280 §5.2.5. RFC 5280
// authorises an indirect CRL issuer via the cRLIssuer field of the target
// certificate's cRLDistributionPoints — the certificate's own issuer nominates
// who may revoke it. PDFE cannot use that route yet: nothing establishes trust
// in an arbitrary nominated third party, and the only certificates available to
// check one against come out of the document's own /DSS, which is
// attacker-controlled. Accepting one would be accepting more than we check.
//
// What IS accepted is an indirect CRL issued by a certificate ALREADY ABOVE the
// target on the path OpenSSL verified (@p pathAncestors) — the target's
// grandparent CA and up. That is safe on its own terms: such a CA already holds
// total authority over this subtree (it can revoke the target's issuer outright,
// which invalidates the target anyway), it is reached only through verified
// signatures rather than through an attacker-supplied DN, and it must still
// carry cRLSign and actually sign the CRL. A third-party indirect CRL issuer
// that is NOT an ancestor remains refused; see phase-49-dispositions.md.
enum class CrlAuthority { None, Direct, Indirect };

//
// F54 RESIDUAL (this phase): @p directIssuers and @p pathAncestors are the two
// semantically different sets described above, and they now have different
// TYPES. Passing one where the other belongs — which was a silent, type-correct
// edit while both were `std::vector<X509*>` — is a compile error. Getting it
// wrong in the pathAncestors→directIssuers direction IS the phase-49 review
// finding H1 (an ancestor's CRL counted as a direct statement about the leaf).
CrlAuthority crlAuthorityFor(X509_CRL* crl, X509* targetCert,
                             const SignerIssuerPool& directIssuers,
                             const PathAncestorPool& pathAncestors)
{
    if (!crl || !targetCert) return CrlAuthority::None;
    X509_NAME* crlIssuer = X509_CRL_get_issuer(crl);
    if (sameName(crlIssuer, X509_get_issuer_name(targetCert))
        && crlIsAuthorized(crl, directIssuers)) {
        return CrlAuthority::Direct;
    }
    // Indirect: the CRL must SAY it is indirect before it may speak for
    // anyone but itself, and the signer must be a verified ancestor.
    if (crlAssertsIndirect(crl) && crlIsAuthorized(crl, pathAncestors)) {
        return CrlAuthority::Indirect;
    }
    return CrlAuthority::None;
}

// PHASE-50 REVIEW, FIX 1 (the F53 recorder accused honest documents).
//
// CrlAuthority::None conflates two situations that could not be further apart:
//
//   (a) the CRL PURPORTS to speak for this certificate and we refused it — it
//       is forged, or its issuer's keyUsage omits cRLSign, or its signature
//       does not verify under a proven issuer. THIS is what F53 exists to
//       report.
//
//   (b) the CRL is entirely genuine and simply ISN'T ABOUT this certificate:
//       sameName() fails at the DN test, a normal CRL does not assert
//       indirectCRL, and it lists nothing attributed to our issuer. It lands
//       on None as well.
//
// (b) is not an exception — it is the NORMAL content of any multi-level
// document carrying long-term validation material, which embeds one CRL per
// chain link. The root's CRL covers the intermediate, so evaluating the SIGNER
// against it takes branch (b) every single time. Recording that as a rejection put
// "CRL failed issuer authorisation (RFC 5280 §6.3.3)…" on the message of a
// fully valid document, and turned an honest partially-covered document
// (root CRL only) into "Revocation evidence REJECTED" — the string reserved
// for something adversarial.
//
// The separating predicate is crlClaimsAuthorityOver(), defined just below
// crlEntryRevokesCert() because it consults it.

// RFC 5280 §5.3.3 certificateIssuer, with the CARRY-FORWARD semantics the
// standard actually specifies (F47 item F4, phase-49).
//
// The certificateIssuer entry extension names the issuer of the certificate an
// entry is about. Crucially, "if this extension is not present on the first
// entry, the certificate issuer defaults to the CRL issuer" and thereafter
// "the certificate issuer for an entry is the certificate issuer of the
// PRECEDING entry" — the value carries forward down the list until another
// entry overrides it.
//
// The pre-phase-49 code modelled none of that. It asked only whether the
// matching entry carried the extension at all, and treated any entry that did
// as "about someone else". Two distinct defects fell out, in OPPOSITE
// directions, and both are fixed here:
//
//   FALSE NEGATIVE — an entry whose certificateIssuer explicitly names OUR
//   OWN issuer (perfectly legal, and the normal shape in an indirect CRL) was
//   discarded. A genuine revocation was silently not seen.
//
//   FALSE POSITIVE — after an entry naming ANOTHER issuer, every following
//   entry that omits the extension belongs to that other issuer too. The old
//   code read "no extension" as "ours" and would report our certificate
//   revoked on the strength of somebody else's entry.
//
// Entries are walked in ENCODED order, which is the order carry-forward is
// defined over. X509_CRL_get0_by_serial() cannot be used here: it consults a
// sorted/hashed lookup and hands back a single entry with no notion of what
// preceded it.
bool crlEntryRevokesCert(X509_CRL* crl, X509* targetCert)
{
    if (!crl || !targetCert) return false;
    const ASN1_INTEGER* targetSerial = X509_get0_serialNumber(targetCert);
    X509_NAME* targetIssuer = X509_get_issuer_name(targetCert);
    if (!targetSerial || !targetIssuer) return false;

    // F60(a), phase-51: the encoded order of `entries` IS the attribution rule.
    // Nothing on this path may call X509_CRL_get0_by_serial() or otherwise sort
    // the stack — sk_X509_REVOKED_sort() reorders it IN PLACE, which silently
    // re-attributes every bare entry to whatever the sort puts in front of it.
    // Pinned by F60EncodedEntryOrder.CarryForwardAttributionSurvivesOnlyIn-
    // EncodedOrder in tests/test_f54_f60a_ocsp_crl_invariants.cpp, which was
    // observed RED against exactly that mutation.
    STACK_OF(X509_REVOKED)* entries = X509_CRL_get_REVOKED(crl);
    if (!entries) return false;

    // Defaults to the CRL's own issuer until an entry says otherwise.
    X509_NAME* effectiveIssuer = X509_CRL_get_issuer(crl);
    // Owns whatever the last certificateIssuer extension decoded to, because
    // effectiveIssuer may point into it for several iterations.
    GENERAL_NAMES* heldNames = nullptr;
    bool revoked = false;

    for (int i = 0; i < sk_X509_REVOKED_num(entries); ++i) {
        X509_REVOKED* rev = sk_X509_REVOKED_value(entries, i);
        if (!rev) continue;

        if (X509_REVOKED_get_ext_by_NID(rev, NID_certificate_issuer, -1) >= 0) {
            GENERAL_NAMES* gns = static_cast<GENERAL_NAMES*>(
                X509_REVOKED_get_ext_d2i(rev, NID_certificate_issuer, nullptr,
                                         nullptr));
            X509_NAME* named = nullptr;
            if (gns) {
                for (int g = 0; g < sk_GENERAL_NAME_num(gns); ++g) {
                    GENERAL_NAME* gn = sk_GENERAL_NAME_value(gns, g);
                    if (gn && gn->type == GEN_DIRNAME) {
                        named = gn->d.directoryName;
                        break;
                    }
                }
            } else {
                ERR_clear_error();
            }
            if (named) {
                if (heldNames) GENERAL_NAMES_free(heldNames);
                heldNames = gns;
                effectiveIssuer = named;
            } else {
                // An unreadable or non-directoryName certificateIssuer is a
                // statement we cannot interpret. Fail CLOSED for attribution:
                // treat this and every following entry as NOT ours rather than
                // guessing, so an unparseable name can never revoke us.
                if (gns) GENERAL_NAMES_free(gns);
                effectiveIssuer = nullptr;
            }
        }

        if (ASN1_INTEGER_cmp(X509_REVOKED_get0_serialNumber(rev),
                             targetSerial) != 0) {
            continue;
        }
        if (!sameName(effectiveIssuer, targetIssuer)) continue;

        // reasonCode removeFromCRL (8) is an UN-revocation, not a revocation.
        // Preserved verbatim from phase-42, which fixed exactly this: the
        // pre-phase-42 test treated it as a live revocation.
        bool lifted = false;
        ASN1_ENUMERATED* reason = static_cast<ASN1_ENUMERATED*>(
            X509_REVOKED_get_ext_d2i(rev, NID_crl_reason, nullptr, nullptr));
        if (reason) {
            lifted = ASN1_ENUMERATED_get(reason) == CRL_REASON_REMOVE_FROM_CRL;
            ASN1_ENUMERATED_free(reason);
        } else {
            ERR_clear_error();
        }
        if (!lifted) { revoked = true; break; }
    }

    if (heldNames) GENERAL_NAMES_free(heldNames);
    ERR_clear_error();
    return revoked;
}

// PHASE-50 REVIEW, FIX 1 — the separating predicate promised above.
//
// Does this CRL make any claim AT ALL about @p targetCert? It asks only that,
// never whether the claim is any good: crlAuthorityFor() keeps sole ownership
// of the authorisation decision, and a CRL that fails it is discarded exactly
// as before whatever this answers. Nothing here can make refused evidence
// count, and its only caller uses it to decide whether to print a diagnostic.
//
// A CRL claims to cover the target in exactly two ways:
//
//   1. It is issued in the TARGET'S ISSUER'S NAME. Such a CRL purports to
//      enumerate that CA's revocations, which is a claim about the target's
//      whole cohort. Note this deliberately does NOT require the name to be
//      honestly held — a forgery carrying the real issuer's DN makes precisely
//      this claim, and reporting it is the entire point of F53.
//
//   2. It carries an ENTRY naming the target's serial and attributed (by RFC
//      5280 §5.3.3 carry-forward) to the target's issuer. That is an explicit,
//      self-attributing claim that THIS certificate is revoked. It catches the
//      CRL that speaks about the target without being issued in its issuer's
//      name — the ancestor or sibling CRL that tries to revoke the signer, with
//      or without declaring itself indirect. Those are refusals worth
//      reporting, and dropping them would have been a real regression; the
//      committed corpus caught exactly that when this predicate was first
//      written with the narrower test (CrlScopeAndChainTest
//      .NonIndirectCrlFromAncestorCannotSpeakForLeaf went red).
//
// Asserting indirectCRL is NOT on its own sufficient. An indirect CRL that
// neither is issued in our issuer's name nor lists us has said nothing about
// us, and is the same benign background content as case (b) above.
bool crlClaimsAuthorityOver(X509_CRL* crl, X509* targetCert)
{
    if (!crl || !targetCert) return false;
    if (sameName(X509_CRL_get_issuer(crl), X509_get_issuer_name(targetCert))) {
        return true;
    }
    return crlEntryRevokesCert(crl, targetCert);
}

// Walk @p crlBlobs looking for an authorised, acceptable CRL that lists
// @p targetCert as revoked. Returns true on match.
// @p rejection (optional, F53) records the first rule that refused a CRL. This
// is the ONLY CRL consumer that records.
//
// KNOWN, DELIBERATE INCOMPLETENESS — PHASE-50 REVIEW, FIX 3. The original
// rationale for not recording from hasValidCrlForCert() said it "would report
// a refusal that did not happen". That is true of most of its refusals, which
// this function refuses identically and has already recorded, but it is NOT
// true of one: hasValidCrlForCert() requires CrlAuthority::Direct while this
// function also accepts Indirect. An ancestor-issued indirect CRL that
// legitimately covers the signer and lists nothing is therefore HONOURED for
// the revocation direction and REFUSED for the credit direction. That refusal
// is real, and it is currently invisible: the document lands on NoDssData,
// which renders "Validated (no revocation data)".
//
// It is left unreported ON PURPOSE. Recording it would push an honest document
// carrying a genuine indirect CRL onto RevocationEvidenceRejected — "something
// adversarial is happening" — which is precisely the false accusation FIX 1
// removed from the two cases above. The asymmetry itself is deliberate policy
// and load-bearing (see hasValidCrlForCert's contract: absence-of-entry from
// an indirect CRL is a completeness claim nothing establishes). What was wrong
// was asserting the recorder had no gaps. It has this one, it is documented
// here and in phase-50-dispositions.md, and it fails in the safe direction:
// the status understates what PDFE knows, never overstates it.
bool isCertRevokedByCrl(X509* targetCert,
                        const SignerIssuerPool& directIssuers,
                        const PathAncestorPool& pathAncestors,
                        const std::vector<std::vector<uint8_t>>& crlBlobs,
                        EvidenceRejection* rejection = nullptr)
{
    if (!targetCert) return false;

    for (const auto& der : crlBlobs) {
        if (der.empty()) continue;
        const unsigned char* p = der.data();
        X509_CRL* crl = d2i_X509_CRL(nullptr, &p,
                                       static_cast<long>(der.size()));
        if (!crl) {
            ERR_clear_error();
            if (rejection) rejection->note(QObject::tr(
                "CRL could not be parsed (malformed DER)"));
            continue;
        }
        // Authenticate the CRL BEFORE reading any revocation claim out of it.
        const CrlAuthority auth =
            crlAuthorityFor(crl, targetCert, directIssuers, pathAncestors);
        if (auth == CrlAuthority::None) {
            // PHASE-50 REVIEW, FIX 1. Report the refusal only if the CRL
            // actually CLAIMED to cover this certificate; a genuine CRL about
            // another link of the chain is not "rejected evidence" and must
            // pass in silence. The security decision is unchanged — the blob
            // is discarded either way. See crlClaimsAuthorityOver().
            const bool claimedThisCert = crlClaimsAuthorityOver(crl, targetCert);
            X509_CRL_free(crl);
            if (rejection && claimedThisCert) rejection->note(QObject::tr(
                "CRL failed issuer authorisation (RFC 5280 §6.3.3): its "
                "signature was not verified by a proven issuer of the signer, "
                "or that issuer's keyUsage does not assert cRLSign"));
            continue;
        }
        // Structure + freshness + SCOPE (delta, critical exts, IDP coverage,
        // future-dating, staleness).
        if (!crlIsAcceptable(crl, targetCert)) {
            X509_CRL_free(crl);
            if (rejection) rejection->note(QObject::tr(
                "CRL rejected as unacceptable: a delta CRL, an unhandled "
                "critical extension, a scope that does not cover this "
                "certificate, or a future-dated or expired validity window"));
            continue;
        }
        const bool revoked = crlEntryRevokesCert(crl, targetCert);
        X509_CRL_free(crl);
        if (revoked) return true;
    }
    return false;
}

// Do we hold a CRL that constitutes positive revocation evidence ABOUT
// @p targetCert? Note this is deliberately NOT "a CRL exists": it must be
// authorised, structurally acceptable, fresh, and in SCOPE for this
// certificate. A CRL scoped away from it says nothing about it.
//
// ONLY A **DIRECT** CRL COUNTS HERE, AND THE ASYMMETRY WITH isCertRevokedByCrl
// IS THE WHOLE POINT (phase-49 review finding H1).
//
// The two directions are not symmetric because the two claims are not
// symmetric. A revocation claim is POSITIVE and self-attributing: an indirect
// CRL that lists our serial and names our issuer in its per-entry
// certificateIssuer (crlEntryRevokesCert enforces both) has told us something
// specific and checkable about US. But the ABSENCE of our serial from somebody
// else's list is a claim of COMPLETENESS — "I enumerate every revoked
// certificate issued by your CA" — and nothing in an indirect CRL establishes
// that. idpCoversCert() cannot supply it either: an IDP whose only assertion is
// indirectCRL sets none of the `covers = false` conditions, so it answers true
// for every certificate in the world.
//
// Concretely, without this restriction: take a hierarchy's legitimate
// root-issued combined CRL, omit the intermediate's own CRL carrying the
// signer's revocation, and every artifact is genuine and correctly signed while
// revokedByCrl is false and hasValidCrl is true — LtvValidated for a revoked
// signature. Requiring Direct restores the invariant this function's contract
// above already claims, and costs nothing in the revocation direction:
// isCertRevokedByCrl keeps its Indirect branch.
// F54 residual (this phase): the `== CrlAuthority::Direct` test below is the
// whole of the H1 fix and is UNCHANGED. The pool types make it harder to defeat
// by accident — pathAncestors can no longer be slid into the directIssuers slot
// to turn an Indirect CRL into a Direct one — but they do not replace it, and
// nothing here relaxes it to `!= CrlAuthority::None`. Pinned by
// CrlScopeAndChainTest (tests/test_crl_scope_and_chain.cpp:733).
bool hasValidCrlForCert(X509* targetCert,
                        const SignerIssuerPool& directIssuers,
                        const PathAncestorPool& pathAncestors,
                        const std::vector<std::vector<uint8_t>>& crlBlobs)
{
    if (!targetCert) return false;
    for (const auto& der : crlBlobs) {
        if (der.empty()) continue;
        const unsigned char* p = der.data();
        X509_CRL* crl = d2i_X509_CRL(nullptr, &p,
                                       static_cast<long>(der.size()));
        if (!crl) { ERR_clear_error(); continue; }
        // An unauthenticated CRL is not evidence -- see crlIsAuthorized().
        // Without this, a rogue-signed CRL carrying the real issuer's DN made
        // hasRevocationEvidence true and the document read as LtvValidated.
        const bool authorized =
            crlAuthorityFor(crl, targetCert, directIssuers, pathAncestors)
            == CrlAuthority::Direct;
        // Structure + freshness + scope subsumes the old ad-hoc nextUpdate
        // test, and additionally rejects delta CRLs (which would otherwise
        // satisfy "we have revocation evidence" while listing none of the base
        // CRL's revocations), CRLs bearing critical extensions we cannot
        // interpret, CRLs scoped away from this certificate, future-dated
        // CRLs, and CRLs with no nextUpdate at all (previously: never expires).
        const bool acceptable = authorized && crlIsAcceptable(crl, targetCert);
        X509_CRL_free(crl);
        if (acceptable) return true;
    }
    return false;
}

// Extract all cert chains for every signed signature widget. We only
// validate the FIRST signature in this phase — multi-signer LTV is a
// separate problem (each signature has its own VRI entry; iterating is
// straightforward but adds UI complexity not in scope for phase-10).
struct ExtractedChain {
    std::vector<uint8_t> signerDer;                   // leaf cert
    std::vector<std::vector<uint8_t>> intermediates;  // optional intermediates
    bool valid = false;
};

ExtractedChain extractFirstChain(pdfe::core::Document* doc)
{
    ExtractedChain out;
    SignatureInputs si;
    if (!LtvDssWriter::extractFirstSignatureInputs(doc, si)) {
        return out;
    }
    if (si.certChain.empty()) return out;
    out.signerDer = si.certChain.front();
    if (si.certChain.size() > 1) {
        out.intermediates.assign(si.certChain.begin() + 1, si.certChain.end());
    }
    out.valid = !out.signerDer.empty();
    return out;
}

} // namespace

namespace detail {

// F47 item F2 (phase-49) — REVOCATION IS A PROPERTY OF THE PATH, NOT OF THE
// LEAF. Extracted verbatim from verifyWith() step 4 by phase-50 so the F59 loop
// guard has a discriminating control: the walk's VERDICT is identical whether
// or not it stops early, so only its call counts can witness the change.
//
// Everything the leaf-only checks did was leaf-only, and so was every
// revocation check PDFE had ever performed. A CA whose own certificate has been
// revoked signs leaves that are individually spotless, so an attacker holding a
// compromised-and-revoked intermediate's key could mint a signer certificate
// that validated cleanly and reported LtvValidated. The CRLs proving the
// intermediate revoked could even be sitting in the document's own /DSS:
// nothing looked at them.
//
// ChainValidationRequest::crlsDer was left empty (and still is) rather than
// handing the CRLs to X509_verify_cert with X509_V_FLAG_CRL_CHECK_ALL. That
// route was considered and rejected: CRL_CHECK_ALL demands a CRL for EVERY
// certificate on the path and returns X509_V_ERR_UNABLE_TO_GET_CRL when one is
// missing, which would turn every existing signed PDF that ships partial
// revocation data from "validated" into "untrusted". That is a fail-closed
// regression against real documents, not a security improvement. This walk
// keeps the established soft-fail shape — a missing CRL still means "no
// evidence" — while making a PRESENT one that says "revoked" impossible to
// ignore.
//
// The anchor itself (the last element) is excluded: RFC 5280 §6.1 does not
// subject a trust anchor to revocation checking, and its own CRL would have to
// be signed by itself.
//
// F59 (phase-50): the loop guard now also tests `!revoked`. Before, the guard
// tested only the caller's leaf verdict, so once a revoked CA was found the
// walk kept running certIssuedSigner() — a full signature verification — and
// isCertRevokedByCrl() over every remaining link, for a result that could not
// change. Behaviourally identical, which is exactly why it needed a counter to
// pin it.
bool walkPathForRevokedCert(const std::vector<std::vector<uint8_t>>& pathDer,
                            const std::vector<std::vector<uint8_t>>& crlBlobs,
                            PathWalkStats* stats)
{
    // Owns the parsed certificates for the whole walk and frees them on every
    // exit path from THIS function — return, or an exception unwinding through
    // it — because by then the VerifiedPath is fully constructed and its
    // destructor runs. (The one path a destructor cannot cover is a throw from
    // inside the constructor itself, before the object exists; that is closed
    // separately by the reserve() in VerifiedPath's constructor — see F3
    // there.) F54 residual: previously a manual loop + a manual sweep.
    // ancestorsAboveIssuerOf() is the sole producer of a PathAncestorPool —
    // path[idx+2..], the only certificates permitted to issue an INDIRECT CRL
    // about link idx; see crlAuthorityFor().
    const VerifiedPath path(pathDer);

    bool revoked = false;
    for (std::size_t i = 1; i + 1 < path.size() && !revoked; ++i) {
        if (stats) ++stats->linkVisits;
        X509* subject = path.at(i);
        X509* parent  = path.at(i + 1);
        // Belt and braces: OpenSSL already verified this link, but the same
        // "prove it, do not trust the DN" rule applies here as at the leaf, and
        // it costs one signature check. That check IS the pool's constructor
        // now — fromCandidates() runs certIssuedSigner(parent, subject) and
        // admits nothing else, so an empty pool means exactly what the explicit
        // `!certIssuedSigner(...) continue` it replaces meant, at the same cost
        // of one verification per link.
        if (stats) ++stats->certIssuedSignerCalls;
        const SignerIssuerPool parentOnly =
            SignerIssuerPool::fromCandidates({parent}, subject);
        if (parentOnly.empty()) continue;
        if (stats) ++stats->isCertRevokedByCrlCalls;
        // Rejections are NOT recorded here: these refusals are about
        // intermediates, and reporting "revocation evidence rejected" to a user
        // looking at the SIGNER would point them at the wrong certificate.
        if (isCertRevokedByCrl(subject, parentOnly,
                               path.ancestorsAboveIssuerOf(i), crlBlobs)) {
            revoked = true;
        }
    }

    return revoked;
}

// ═══════════════════════════════════════════════════════════════════════
// F54 (phase-51) test seams.
//
// The hardening itself is a COMPILE-TIME fact — SignerIssuerPool's private
// constructor — and a compile-time fact cannot be reddened by a runtime test.
// What CAN be, and what actually carries the security claim, is the behaviour of
// the pool's single construction path: it must admit a genuine issuer and refuse
// a document-supplied certificate that merely wears the right DN. These two
// functions expose exactly that, and nothing else. Neither is used in production
// code; both take and return only bytes, so this header stays free of OpenSSL.
// ═══════════════════════════════════════════════════════════════════════

std::vector<int> signerIssuerPoolAdmittedIndices(
    const std::vector<std::vector<uint8_t>>& candidateDer,
    const std::vector<uint8_t>& signerDer)
{
    std::vector<int> admitted;
    X509* signer = derToX509(signerDer);
    if (!signer) return admitted;

    std::vector<X509*> parsed;          // owns
    std::vector<int> parsedIndex;       // parsed[k] came from candidateDer[i]
    for (size_t i = 0; i < candidateDer.size(); ++i) {
        if (X509* c = derToX509(candidateDer[i])) {
            parsed.push_back(c);
            parsedIndex.push_back(static_cast<int>(i));
        }
    }

    const SignerIssuerPool pool = SignerIssuerPool::fromCandidates(parsed, signer);
    for (X509* member : pool.certs()) {
        for (size_t k = 0; k < parsed.size(); ++k) {
            if (parsed[k] == member) { admitted.push_back(parsedIndex[k]); break; }
        }
    }

    for (X509* c : parsed) X509_free(c);
    X509_free(signer);
    return admitted;
}

bool ocspAuthorizedForSigner(const std::vector<uint8_t>& ocspDer,
                             const std::vector<std::vector<uint8_t>>& candidateDer,
                             const std::vector<uint8_t>& signerDer)
{
    X509* signer = derToX509(signerDer);
    if (!signer) return false;

    std::vector<X509*> parsed;
    for (const auto& der : candidateDer) {
        if (X509* c = derToX509(der)) parsed.push_back(c);
    }
    const SignerIssuerPool pool = SignerIssuerPool::fromCandidates(parsed, signer);

    bool authorized = false;
    if (!ocspDer.empty()) {
        const unsigned char* p = ocspDer.data();
        OCSP_RESPONSE* resp = d2i_OCSP_RESPONSE(nullptr, &p,
                                                static_cast<long>(ocspDer.size()));
        if (!resp) {
            ERR_clear_error();
        } else {
            OCSP_BASICRESP* basic = OCSP_response_get1_basic(resp);
            if (!basic) ERR_clear_error();
            else {
                authorized = ocspResponseIsAuthorized(basic, pool);
                OCSP_BASICRESP_free(basic);
            }
            OCSP_RESPONSE_free(resp);
        }
    }

    for (X509* c : parsed) X509_free(c);
    X509_free(signer);
    return authorized;
}

} // namespace detail

TimestampCheck LtvValidator::checkEmbeddedTimestamp(
    const std::vector<uint8_t>& pkcs7Der)
{
    TimestampCheck out;
    if (pkcs7Der.empty()) return out;

    const unsigned char* p = pkcs7Der.data();
    PKCS7* p7 = d2i_PKCS7(nullptr, &p, static_cast<long>(pkcs7Der.size()));
    if (!p7) { ERR_clear_error(); return out; }

    STACK_OF(PKCS7_SIGNER_INFO)* sis = PKCS7_get_signer_info(p7);
    if (sis && sk_PKCS7_SIGNER_INFO_num(sis) > 0) {
        PKCS7_SIGNER_INFO* si = sk_PKCS7_SIGNER_INFO_value(sis, 0);
        if (si && si->enc_digest && si->enc_digest->data &&
            si->enc_digest->length > 0 && si->unauth_attr) {
            // Expected imprint = SHA-256 of the signature value — exactly what
            // the embed side (Pkcs7OpenSslBridge::embedTimestampToken) hashed.
            unsigned char imprint[SHA256_DIGEST_LENGTH];
            SHA256(si->enc_digest->data,
                   static_cast<size_t>(si->enc_digest->length), imprint);

            // Find the id-smime-aa-timeStampToken unauthenticated attribute.
            ASN1_OBJECT* tsOid = OBJ_txt2obj("1.2.840.113549.1.9.16.2.14", 1);
            const int n = X509at_get_attr_count(si->unauth_attr);
            for (int i = 0; i < n; ++i) {
                X509_ATTRIBUTE* attr = X509at_get_attr(si->unauth_attr, i);
                if (!attr) continue;
                if (OBJ_cmp(X509_ATTRIBUTE_get0_object(attr), tsOid) != 0)
                    continue;
                ASN1_TYPE* t = X509_ATTRIBUTE_get0_type(attr, 0);
                if (!t) continue;
                out.present = true;
                // The token was embedded verbatim as a V_ASN1_SEQUENCE whose
                // bytes ARE the DER TimeStampToken (see the embed side).
                if (t->type == V_ASN1_SEQUENCE && t->value.sequence &&
                    t->value.sequence->data && t->value.sequence->length > 0) {
                    std::vector<uint8_t> tokenDer(
                        t->value.sequence->data,
                        t->value.sequence->data + t->value.sequence->length);
                    // No TSA certificate: this path wants the RFC 3161
                    // imprint binding only (it reads `parsed &&
                    // imprintMatches` below and never `signatureValid`).
                    // `NotEstablished` is the honest value — nothing here has
                    // established any TSA's trust — and it is inert while the
                    // certificate is null, since no anchor is installed. F61.
                    auto v = TsaClient::verifyTimestampToken(
                        tokenDer, imprint, SHA256_DIGEST_LENGTH, nullptr,
                        TsaClient::TsaCertTrust::NotEstablished);
                    out.imprintValid = v.parsed && v.imprintMatches;
                }
                break;
            }
            if (tsOid) ASN1_OBJECT_free(tsOid);
        }
    }

    PKCS7_free(p7);
    return out;
}

LtvVerifyResult LtvValidator::verifyWith(
    pdfe::core::Document* doc,
    const std::vector<std::vector<uint8_t>>& embeddedCerts,
    const std::vector<std::vector<uint8_t>>& ocsps,
    const std::vector<std::vector<uint8_t>>& crls,
    bool useTrustAnchors)
{
    LtvVerifyResult result;

    if (!doc) {
        result.status = LtvStatus::Unknown;
        result.message = QObject::tr("No document");
        return result;
    }

    // Step 1: pull the first signed signature's cert chain + raw PKCS#7.
    auto extracted = extractFirstChain(doc);
    if (!extracted.valid) {
        result.status = LtvStatus::NoSignatures;
        result.message = QObject::tr("No signed signature in document");
        return result;
    }

    // Step 2: digest verification — independent of cert trust. If the
    // bytes don't match, nothing else matters. SignatureManager wraps
    // pdf_check_widget_digest, which is the canonical MuPDF path; reuse
    // it instead of duplicating the byte-range carving here.
    //
    // NOTE: do NOT reject on `modifiedAfterSigning`. /DSS embedding is
    // itself an incremental update after signing — that's the entire
    // point of post-signature LTV augmentation (ISO 32000-2 §12.8.4.3;
    // this comment used to name an ETSI baseline profile, which PDFE
    // does not reach — see docs/signature-profile-support.md, F20 /
    // gap PDF-34). The
    // signature's covered byte range is preserved (LtvDssWriter checks
    // that invariant); MuPDF still flags the /DSS append as a change.
    // For LTV verification we only care that the digest matches.
    bool digestValidLocal = false;
    try {
        SignatureManager mgr(doc->context());
        auto sigs = mgr.listSignatures(doc);  // Walks all pages already.
        for (const auto& sig : sigs) {
            if (!sig.isSigned) continue;
            auto p = doc->page(sig.pageIndex);
            if (!p) continue;
            auto vr = mgr.verifySignature(doc, p.get(), sig.widgetIndex);
            result.digestResult = vr;
            digestValidLocal = vr.digestValid;
            if (digestValidLocal) break;  // First signed widget wins.
        }
    } catch (...) {
        digestValidLocal = false;
    }

    if (!digestValidLocal) {
        result.status = LtvStatus::InvalidSignature;
        result.message = QObject::tr("Signature digest does not match content");
        return result;
    }

    // Step 2b: surface any embedded RFC 3161 timestamp and whether it actually
    // binds this signature. Read the signature's raw PKCS#7 and check the
    // id-smime-aa-timeStampToken attribute's message imprint. Non-fatal on its
    // own — the flags let callers (and tests) detect a missing or forged
    // (mismatched-imprint) timestamp that would otherwise pass silently.
    {
        SignatureInputs si;
        if (LtvDssWriter::extractFirstSignatureInputs(doc, si)) {
            auto ts = checkEmbeddedTimestamp(si.contentsBin);
            result.hasTimestamp = ts.present;
            result.timestampImprintValid = ts.imprintValid;
        }
    }

    // Step 3: chain validation.
    //
    // Meta-audit follow-up F23 (gap PDF-39): this used to build the
    // X509_STORE inline and collapse X509_verify_cert() into a bool. It now
    // goes through CertChainValidator, which takes the anchor set as an
    // explicit TrustAnchorSet parameter and returns a specific verdict.
    // Trust sources are unchanged: the user's anchors (when
    // @p useTrustAnchors) plus the system default paths.
    //
    // SECURITY (unchanged, and the reason embeddedCerts go where they go):
    // /DSS-embedded certs are NEVER added to the trust store. Doing so would
    // grant trust-anchor status to anything the document author chose to
    // embed — the document could self-certify any chain. They are supplied
    // as untrusted path-building material only, so they can complete a chain
    // but can never terminate one.
    X509* signerCert = derToX509(extracted.signerDer);
    if (!signerCert) {
        result.status = LtvStatus::Unknown;
        result.message = QObject::tr("Signer certificate failed to parse");
        return result;
    }

    ChainValidationRequest chainReq;
    chainReq.leafDer = extracted.signerDer;
    for (const auto& der : embeddedCerts) chainReq.untrustedDer.push_back(der);
    for (const auto& der : extracted.intermediates) chainReq.untrustedDer.push_back(der);
    if (useTrustAnchors) {
        chainReq.anchors = TrustAnchorManager::instance().snapshot();
    }
    // OpenSSL's default cert paths as a fallback.
    //
    // GitHub #212 — THIS COMMENT USED TO CLAIM MORE THAN THE CALL DELIVERS.
    // It read: "System store as a fallback — without this, signed-by-public-CA
    // PDFs that ship /DSS but not the root would fail offline." That is false
    // on Windows, the only platform PDFE ships on. The flag sets
    // X509_STORE_set_default_paths(), which reads OPENSSLDIR/certs and
    // SSL_CERT_FILE / SSL_CERT_DIR — never the Windows certificate store — and
    // OPENSSLDIR/certs does not exist on a stock vcpkg build. So a
    // signed-by-public-CA PDF that ships /DSS but not the root DOES fail here
    // unless the user imported that root as a PDFE trust anchor. That is the
    // deliberate, recorded behaviour (see the DECISION block in
    // CertChainValidator.h), not a bug — but the old comment would have sent a
    // maintainer debugging a fallback that was never doing anything.
    //
    // The line is kept because it is correct and useful on POSIX, and because
    // TrustAnchorManager's anchors are still consulted first by
    // X509_verify_cert when issuer DNs collide.
    chainReq.useOpenSslDefaultPaths = true;
    // CRL/OCSP revocation is evaluated separately in step 4 below against
    // the embedded /DSS material, so no CRLs are handed to the path check.

    const ChainValidationResult chainRes = CertChainValidator::validate(chainReq);
    const bool chainValid = chainRes.trusted();

    // Step 4: revocation evaluation — only meaningful if we have OCSP/CRL
    // bytes to look at.
    int ocspVerdict = -1;
    bool revokedByCrl = false;
    bool revokedPathCert = false;   // a CA above the signer is revoked (F2)
    bool hasValidCrl = false;
    EvidenceRejection rejection;    // F53: which rule refused what, if any
    {
        // Parse every candidate we were handed: /DSS certs + any intermediates,
        // plus the signer itself (which only survives the filter below when it
        // is genuinely self-signed). `parsedCands` OWNS these.
        std::vector<X509*> parsedCands;
        auto pushIssuerDer = [&](const std::vector<uint8_t>& der) {
            X509* c = derToX509(der);
            if (c) parsedCands.push_back(c);
        };
        for (const auto& der : embeddedCerts) pushIssuerDer(der);
        for (const auto& der : extracted.intermediates) pushIssuerDer(der);
        if (X509* selfDup = X509_dup(signerCert)) parsedCands.push_back(selfDup);

        // SECURITY (phase-42 remediation): every one of those certs came out of
        // the PDF, so NONE of them may authorize revocation data on the
        // strength of its DN alone. Keep only the certs whose key ACTUALLY
        // signed the signer certificate — see certIssuedSigner(). A rogue cert
        // planted in /DSS with the real issuer's DN does not survive this, and
        // it is the whole of the residual bypass. Non-owning view.
        //
        // F54 (phase-51): the filtering step IS the pool's constructor now. It
        // is no longer possible to build this set any other way, nor to hand an
        // unfiltered vector to the OCSP path, which is where the filter is
        // load-bearing (X509_V_FLAG_PARTIAL_CHAIN makes every member an anchor).
        const SignerIssuerPool authorizedIssuers =
            SignerIssuerPool::fromCandidates(parsedCands, signerCert);

        ocspVerdict = evaluateOcspForCert(signerCert, authorizedIssuers, ocsps,
                                          &rejection);

        // The path OpenSSL actually VERIFIED, leaf first, anchor last. Owned
        // here (RAII since the F54 residual). Empty when no path could be
        // built, in which case the ancestor pool is empty, no indirect CRL is
        // honoured, detail::walkPathForRevokedCert() below does nothing and the
        // verdict falls through to UntrustedChain — the fail-closed direction.
        const VerifiedPath verifiedPath(chainRes.pathDer);
        // Everything ABOVE the signer's direct issuer, i.e. path[2..]. These
        // are the only certificates permitted to issue an INDIRECT CRL about
        // the signer — see crlAuthorityFor(). Its type keeps it out of the
        // direct-issuer slot; see PathAncestorPool.
        const PathAncestorPool signerAncestors =
            verifiedPath.ancestorsAboveIssuerOf(0);

        // Match the signer's issuer DN against any CRL we hold -- and require
        // that CRL to be signed by a PROVEN issuer of the signer (phase-42).
        revokedByCrl = isCertRevokedByCrl(signerCert, authorizedIssuers,
                                          signerAncestors, crls, &rejection);
        hasValidCrl = hasValidCrlForCert(signerCert, authorizedIssuers,
                                         signerAncestors, crls);

        // F47 item F2, phase-49 — the path walk. Extracted to
        // detail::walkPathForRevokedCert() by phase-50 (F59); see that function
        // for the full rationale and for the loop-guard fix. Skipping it when
        // the leaf is already revoked reproduces the old guard's `!revokedByCrl`
        // condition exactly.
        if (!revokedByCrl) {
            revokedPathCert =
                detail::walkPathForRevokedCert(chainRes.pathDer, crls, nullptr);
        }

        for (X509* c : parsedCands) X509_free(c);
    }

    // Step 5: classify.
    if (ocspVerdict == V_OCSP_CERTSTATUS_REVOKED || revokedByCrl) {
        result.status = LtvStatus::Revoked;
        result.message = QObject::tr("Signer certificate is revoked");
        X509_free(signerCert);
        return result;
    }

    // A revoked CA above the signer invalidates everything it issued, so the
    // document is no more trustworthy than a directly revoked signer. It gets
    // its own message because "the signer is revoked" would be a false
    // statement about the signer, and a user acting on it would go looking at
    // the wrong certificate.
    if (revokedPathCert) {
        result.status = LtvStatus::Revoked;
        result.message = QObject::tr(
            "A certificate authority in the signer's chain is revoked");
        X509_free(signerCert);
        return result;
    }

    if (!chainValid) {
        result.status = LtvStatus::UntrustedChain;
        result.message = QObject::tr("Signer chain does not terminate at a trusted root");
        // PHASE-50 REVIEW, FIX 4. This return sits ABOVE the rejection arm
        // below, so a document with BOTH a forged responder and an untrusted
        // chain used to report only the chain problem and drop the forgery
        // signal entirely. Fail-closed, but F53-incomplete: the forgery is the
        // more alarming of the two facts and the user never saw it. Appended
        // the same way LtvValidated appends it — the STATUS is untouched,
        // because an untrusted chain is already the stronger finding.
        if (rejection.any) {
            result.message = QObject::tr("%1. Note: %2")
                                 .arg(result.message, rejection.rule);
        }
        X509_free(signerCert);
        return result;
    }

    // /DSS-with-OCSP path — full LTV. Only count valid (non-stale,
    // non-expired) revocation evidence.
    const bool hasRevocationEvidence = (ocspVerdict == V_OCSP_CERTSTATUS_GOOD)
                                       || hasValidCrl;
    if (!embeddedCerts.empty() && hasRevocationEvidence) {
        result.status = LtvStatus::LtvValidated;
        result.message = QObject::tr("LTV-validated against embedded /DSS data");
        // F53 corner the status alone cannot express: OTHER evidence in the
        // same /DSS was refused. The verdict stays LtvValidated — accepted
        // evidence is accepted, and downgrading on the strength of a refused
        // artifact would let an attacker suppress a valid signature by adding
        // junk. But the refusal is still the most interesting thing PDFE knows
        // about this document, so it is appended rather than dropped.
        if (rejection.any) {
            result.message = QObject::tr("%1. Note: %2")
                                 .arg(result.message, rejection.rule);
        }
    } else if (rejection.any) {
        // F53 (phase-50). Evidence was supplied and a named rule refused it.
        // This branch sits BELOW the LtvValidated test, so it can never
        // downgrade a document whose evidence was ACCEPTED, and BELOW the
        // Revoked and UntrustedChain returns above, so a refusal can never
        // manufacture a revocation nor mask an untrusted chain.
        //
        // PLACEMENT INVARIANT, CORRECTED BY THE PHASE-50 REVIEW (FIX 1). The
        // dispositions originally claimed this arm "only refines NoDssData".
        // That is not what the code does. It sits above BOTH remaining arms:
        //   * NoDssData      — /DSS has certs but no usable revocation data;
        //   * OnlineValidated — /DSS has NO certs at all, so the document
        //                       passed on ambient trust alone: the user's
        //                       anchors plus OpenSSL's default certificate
        //                       paths. Never the Windows store (#212).
        // A /DSS carrying CRLs but no certificates therefore displaces a PASS
        // status, not merely an "absent evidence" one. That is the intended
        // behaviour — evidence was present and a named rule refused it, which
        // is strictly more than the ambient-trust pass established — but it is
        // a stronger claim than the docs made, and it is now written down.
        // Reaching it at all still requires a rejection that survives FIX 1's
        // relevance test, i.e. an artifact that CLAIMED to cover the signer.
        result.status = LtvStatus::RevocationEvidenceRejected;
        result.message = rejection.rule;
    } else if (!embeddedCerts.empty()) {
        // /DSS exists but has only certs, no OCSP/CRL.
        result.status = LtvStatus::NoDssData;
        result.message = QObject::tr("/DSS present but no revocation data");
    } else {
        // Pass on ambient trust only: the user's anchors plus OpenSSL's
        // default certificate paths. NOT the Windows certificate store,
        // which PDFE never reads (GitHub #212) -- naming it here would be a
        // false ASSURANCE, telling the user the OS vouched for a certificate
        // it was never asked about.
        result.status = LtvStatus::OnlineValidated;
        result.message = QObject::tr(
            "Validated against your trust anchors (not the Windows "
            "certificate store, which PDFE does not consult)");
    }

    X509_free(signerCert);
    return result;
}

LtvVerifyResult LtvValidator::verify(pdfe::core::Document* doc)
{
    LtvVerifyResult result;
    if (!doc) {
        result.status = LtvStatus::Unknown;
        result.message = QObject::tr("No document");
        return result;
    }

    DssEmbedded dss;
    DssReader::readDssEmbedded(doc, dss);

    if (!dss.hasDss) {
        // Phase-10 contract: when there's no /DSS we still want a
        // verdict. Run verifyWith() with empty embedded data so the
        // X509_STORE falls back to system-paths-only and the result
        // surfaces as OnlineValidated / NoDssData / UntrustedChain.
        auto r = verifyWith(doc, /*certs=*/{}, /*ocsps=*/{}, /*crls=*/{},
                            /*useTrustAnchors=*/true);
        // verifyWith() returns OnlineValidated when chain validates but
        // there are no /DSS certs. That's exactly the "no DSS, online
        // verify worked" case.
        //
        // ═══════════════════════════════════════════════════════════════
        // F52 (phase-50): THE REWRITE THAT USED TO LIVE HERE IS GONE.
        //
        // This block used to read `r.status = LtvStatus::NoDssData;` under a
        // comment calling that "the 'validated (no revocation data)' UI
        // state" — and ltvStatusToDisplayString(NoDssData) is literally
        // "Validated (no revocation data)". End to end: an attacker signs a
        // document with a self-minted certificate and ships it with no /DSS.
        // The digest verifies — over their own bytes. The chain reaches no
        // trust anchor. The panel said **Validated**. The honest string was
        // written into r.message by this very block and then discarded by
        // MainWindow, which passed only the status to the panel.
        //
        // UntrustedChain now survives to the UI intact and renders as
        // "Signature intact — certificate NOT trusted". Only the MESSAGE is
        // specialised here, because the no-/DSS case genuinely has a more
        // specific detail to give than verifyWith()'s generic one.
        //
        // ACCEPTED COST, decided by the user and recorded in
        // docs/audit/meta-audit-2026-07/followup/phase-50/f52-product-decision.md:
        // users who deliberately sign with self-signed certificates will now
        // see a warning where they previously saw "Validated". That is the
        // correct reading of what PDFE actually established about those
        // documents, and it was accepted knowingly.
        // ═══════════════════════════════════════════════════════════════
        if (r.status == LtvStatus::UntrustedChain) {
            // The wording deliberately does NOT say "system store". PDFE does
            // not consult the Windows certificate store (GitHub #212); saying
            // it did would tell the user their certificate had been checked
            // against a trust source that was never read, and would make the
            // most common case -- a cert Windows trusts, reported untrusted
            // here -- look like a bug rather than the recorded decision it is.
            // Pinned by LtvValidatorTest.NoDssDataReturnsNoDssStatus in
            // tests/test_ltv_validator.cpp, which asserts unconditionally
            // that this string never names the system store.
            r.message = QObject::tr(
                "Digest verified. No /DSS, and the chain is not trusted by "
                "your configured trust anchors. PDFE does not consult the "
                "Windows certificate store.");
        }
        return r;
    }

    return verifyWith(doc, dss.certs, dss.ocsps, dss.crls,
                       /*useTrustAnchors=*/true);
}

} // namespace pdfe::signatures
