// -*- coding: utf-8 -*-
//
// Meta-audit follow-up F47 item F1 (phase-48): RFC 5280 §6.3.3(f) —
// a CRL is evidence only if the certificate that signed it is ENTITLED to sign
// CRLs, i.e. its keyUsage extension (when present) asserts cRLSign.
//
// WHY THIS FILE EXISTS AND WHY IT IS SHAPED THIS WAY
// --------------------------------------------------
// The rule was written in phase-42 and deliberately withdrawn, because the
// entire signing corpus rested on tests/fixtures/test_signer.pfx — a
// SELF-SIGNED END-ENTITY certificate (CA:FALSE; keyUsage critical
// digitalSignature, contentCommitment; no cRLSign) which issues its own CRLs.
// Enforcing the rule against that fixture rejects every CRL in the suite, so
// the rule could not be shipped without a real two-level chain first.
//
// tests/fixtures/crl_chain/ (built by tests/fixtures/gen_two_level_ca_signer.py)
// supplies it, and — critically — supplies it TWICE:
//
//     root
//      ├── inter_crlsign      keyUsage = keyCertSign + cRLSign   → leaf_crlsign
//      └── inter_nocrlsign    keyUsage = keyCertSign             → leaf_nocrlsign
//
// The two intermediates are produced by the same generator code path with the
// same subject pattern, validity window, key size and signature algorithm. The
// ONLY difference is the crl_sign bit. Both genuinely issued their leaf, so
// both survive certIssuedSigner() — the gate that already existed — and the
// cRLSign rule is the only thing that can separate them.
//
// THE DISCRIMINATION REQUIREMENT. A guard no mutation can redden is decoration.
// The positive and negative cases below run the SAME body via runCase(),
// differing only in which chain is fed in, and assert opposite verdicts:
//
//   * delete the `if (!(X509_get_key_usage(issuer) & KU_CRL_SIGN)) continue;`
//     line from crlIsAuthorized() in src/signatures/LtvValidator.cpp
//     → NoCrlSignIssuerRejected flips NoDssData → LtvValidated. RED.
//   * invert it to `if (X509_get_key_usage(issuer) & KU_CRL_SIGN) continue;`
//     → CrlSignIssuerAccepted flips LtvValidated → NoDssData. RED.
//
//   * move the check OUT of crlIsAuthorized() and re-implement it inline in
//     hasValidCrlForCert() only — the half-implementation the phase-48
//     review built, which enforces the rule on "is there evidence" but not on
//     "is the signer revoked" → NoCrlSignIssuerCannotRevoke flips
//     NoDssData → Revoked. RED.
//
// Neither mutation can be absorbed, because IssuerKeyUsageBitsAreAsIntended
// pins the fixture's keyUsage bits directly: if someone "fixes" the negative
// case by regenerating inter_nocrlsign WITH cRLSign, that test goes red first
// and names the real cause.
//
// The CRLs are minted at run time from the committed intermediate private keys
// so their thisUpdate/nextUpdate are wall-clock relative — a committed CRL
// would age out and redden the suite on a calendar boundary.

#include <gtest/gtest.h>

#include "signatures/LtvValidator.h"
#include "support/CrlChainFixture.h"

#include <QString>

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

using namespace pdfe::signatures;
using namespace pdfe::core;
namespace cc = pdfe_test::crlchain;

namespace {

// The corpus helpers (load, mint, sign, scoped trust anchor) live in
// tests/support/CrlChainFixture.h so the CRL-freshness tests migrated off
// test_signer.pfx share ONE definition with this file rather than a copy.
constexpr const char* kScratch = "keyusage";

std::filesystem::path chainDir() { return cc::chainDir(); }

// One end-to-end case: sign with `leafPfx`, mint a CRL from `interKeyPem` /
// `interDer`, hand both to LtvValidator with `root.der` trusted, and return
// the verdict. The positive and negative tests execute this SAME body and
// differ only in the four fixture names below (plus a distinct scratch
// filename, so the runs cannot collide in the temp directory). Nothing
// else varies: same trust anchor, same CRL validity window, same call into
// verifyWith().
//
// `revokeSigner` is the ONE additional degree of freedom, added by the
// phase-48 review's SEC-2 finding: it lists the signer's own serial in the
// minted CRL, which moves the question from "is this CRL evidence" to "may
// this CRL REVOKE". Both directions must be gated by the same rule, and
// before NoCrlSignIssuerCannotRevoke existed only the first was pinned.
struct CaseInputs {
    const char* leafPfx;
    const char* leafDer;
    const char* interDer;
    const char* interKeyPem;
    const char* signedName;
};

::testing::AssertionResult runCase(const CaseInputs& in, LtvStatus& outStatus,
                                   bool revokeSigner = false)
{
    auto ctx = std::make_shared<MuPdfContext>();

    auto signedPath = cc::signPdfWith(ctx, chainDir() / in.leafPfx, kScratch,
                                      in.signedName, "CrlKuSig");
    if (signedPath.empty() || !std::filesystem::exists(signedPath))
        return ::testing::AssertionFailure() << "could not sign with " << in.leafPfx;

    X509* interCert = cc::loadDerCert(chainDir() / in.interDer);
    if (!interCert)
        return ::testing::AssertionFailure() << "could not load " << in.interDer;
    EVP_PKEY* interKey = cc::loadPemKey(chainDir() / in.interKeyPem);
    if (!interKey) {
        X509_free(interCert);
        return ::testing::AssertionFailure() << "could not load " << in.interKeyPem;
    }

    // The serial to revoke is read from the committed leaf DER, never
    // hard-coded: regenerating the corpus with different serials must not
    // silently turn this into a CRL that revokes nobody.
    X509* leafCert = nullptr;
    cc::CrlSpec spec;
    spec.nextUpdateOffsetSec = 86400;
    if (revokeSigner) {
        leafCert = cc::loadDerCert(chainDir() / in.leafDer);
        if (!leafCert) {
            EVP_PKEY_free(interKey);
            X509_free(interCert);
            return ::testing::AssertionFailure() << "could not load " << in.leafDer;
        }
        spec.revokedSerials.push_back(X509_get0_serialNumber(leafCert));
    }

    auto crl = cc::mintCrl(interKey, interCert, spec);
    EVP_PKEY_free(interKey);
    X509_free(interCert);
    if (leafCert) X509_free(leafCert);
    if (crl.empty())
        return ::testing::AssertionFailure() << "could not mint CRL for " << in.interDer;

    auto leafDer = cc::readAllBytes(chainDir() / in.leafDer);
    auto interDer = cc::readAllBytes(chainDir() / in.interDer);
    if (leafDer.empty() || interDer.empty())
        return ::testing::AssertionFailure() << "missing DER material";

    auto rootPem = cc::derToPemFile(chainDir() / "root.der", kScratch,
                                    "crl_ku_root.pem");
    if (rootPem.empty())
        return ::testing::AssertionFailure() << "could not write root anchor PEM";
    cc::ScopedTrustAnchor anchor(rootPem);
    if (!anchor.added)
        return ::testing::AssertionFailure() << "could not install root trust anchor";

    auto doc = Document::open(ctx, signedPath);
    if (!doc)
        return ::testing::AssertionFailure() << "could not reopen signed PDF";

    auto r = LtvValidator::verifyWith(doc.get(), {leafDer, interDer}, {}, {crl},
                                       /*useTrustAnchors=*/true);
    outStatus = r.status;
    return ::testing::AssertionSuccess();
}

std::string shown(LtvStatus s)
{
    return ltvStatusToDisplayString(s).toStdString();
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// The fixture's own invariants. If these go red, the two cases below are
// no longer a controlled comparison and their verdicts mean nothing — so
// this test exists to make that failure mode name itself rather than
// silently turning the control into decoration.
// ─────────────────────────────────────────────────────────────────────
TEST(CrlIssuerKeyUsageTest, IssuerKeyUsageBitsAreAsIntended)
{
    X509* withCrlSign = cc::loadDerCert(chainDir() / "inter_crlsign.der");
    X509* withoutCrlSign = cc::loadDerCert(chainDir() / "inter_nocrlsign.der");
    ASSERT_NE(withCrlSign, nullptr) << "inter_crlsign.der missing or unparseable";
    ASSERT_NE(withoutCrlSign, nullptr) << "inter_nocrlsign.der missing or unparseable";

    const uint32_t kuYes = X509_get_key_usage(withCrlSign);
    const uint32_t kuNo = X509_get_key_usage(withoutCrlSign);

    // Both must actually CARRY a keyUsage extension — an absent extension
    // reads as UINT32_MAX (unconstrained), which would make the negative
    // case pass for the wrong reason.
    EXPECT_NE(kuYes, UINT32_MAX)
        << "inter_crlsign has no keyUsage extension; the rule would not apply";
    EXPECT_NE(kuNo, UINT32_MAX)
        << "inter_nocrlsign has no keyUsage extension; the negative control "
           "would be vacuous";

    EXPECT_TRUE(kuYes & KU_CRL_SIGN) << "inter_crlsign must assert cRLSign";
    EXPECT_FALSE(kuNo & KU_CRL_SIGN) << "inter_nocrlsign must NOT assert cRLSign";

    // Everything else that matters must be IDENTICAL, or the pair is not a
    // control. keyCertSign in particular: both must be able to have issued
    // their leaf, so certIssuedSigner() cannot be what separates them.
    EXPECT_TRUE(kuYes & KU_KEY_CERT_SIGN);
    EXPECT_TRUE(kuNo & KU_KEY_CERT_SIGN);
    EXPECT_EQ(kuYes & ~static_cast<uint32_t>(KU_CRL_SIGN),
              kuNo & ~static_cast<uint32_t>(KU_CRL_SIGN))
        << "the two intermediates differ in more than the cRLSign bit; "
           "the comparison below is no longer controlled";

    X509_free(withCrlSign);
    X509_free(withoutCrlSign);
}

// ─────────────────────────────────────────────────────────────────────
// F1 POSITIVE: an intermediate CA that carries cRLSign is ACCEPTED, so its
// CRL counts as revocation evidence and the document reaches full LTV.
// ─────────────────────────────────────────────────────────────────────
TEST(CrlIssuerKeyUsageTest, CrlSignIssuerAccepted)
{
    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase({"leaf_crlsign.pfx", "leaf_crlsign.der",
                         "inter_crlsign.der", "inter_crlsign_key.pem",
                         "crl_ku_pos.pdf"},
                        status));
    EXPECT_EQ(status, LtvStatus::LtvValidated)
        << "A fresh CRL signed by the signer's genuine issuing CA, which "
           "asserts cRLSign, is valid revocation evidence per RFC 5280 "
           "§6.3.3(f) and must produce full LTV; got " << shown(status);
}

// ─────────────────────────────────────────────────────────────────────
// F1 NEGATIVE (the control): the SAME body, the SAME structure, an issuer
// that genuinely issued the signer — but whose keyUsage omits cRLSign. Its
// CRL must be discarded, leaving no revocation evidence at all.
//
// Note this FAILS CLOSED: the verdict degrades to NoDssData (no evidence),
// never to LtvValidated. It does not become Revoked either — the CRL is not
// consulted at all, in either direction.
// ─────────────────────────────────────────────────────────────────────
TEST(CrlIssuerKeyUsageTest, NoCrlSignIssuerRejected)
{
    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase({"leaf_nocrlsign.pfx", "leaf_nocrlsign.der",
                         "inter_nocrlsign.der", "inter_nocrlsign_key.pem",
                         "crl_ku_neg.pdf"},
                        status));
    // PHASE-50 (F53): was NoDssData. §6.3.3(f) is a REFUSAL of evidence that
    // was supplied, which is now reported as such — see
    // LtvStatusReportingPhase50.RejectedCrlIsDistinguishableFromAbsentEvidence,
    // whose whole point is that this case and BothChainsAreOtherwiseEquivalent's
    // no-CRL case used to be the same value.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A CRL signed by a CA whose keyUsage does not assert cRLSign is "
           "not authorised to sign CRLs (RFC 5280 §6.3.3(f)); it must be "
           "discarded, leaving no revocation evidence. Getting LtvValidated "
           "here means the keyUsage check in crlIsAuthorized() is missing or "
           "ineffective; got " << shown(status);
}

// ─────────────────────────────────────────────────────────────────────
// F47/SEC-2 (phase-48 review): the REVOCATION direction of §6.3.3(f).
//
// WHY THIS TEST IS NOT REDUNDANT WITH NoCrlSignIssuerRejected. The rule is
// consulted on two independent paths in LtvValidator.cpp:
//
//   hasValidCrlForCert()  → "is there revocation EVIDENCE"  → LtvValidated
//   isCertRevokedByCrl()    → "is the signer REVOKED"         → Revoked
//
// Both call crlIsAuthorized(). Every other test in this file mints an EMPTY
// CRL, and every revoked-serial CRL elsewhere in the suite is signed either by
// the cRLSign-bearing intermediate or by a rogue key — so before this test,
// NOTHING constructed a CRL that both LISTS the signer's serial AND is signed
// by the non-cRLSign intermediate. The phase-48 review proved the consequence
// by mutation: move the keyUsage check out of the shared crlIsAuthorized() and
// re-implement it inline in hasValidCrlForCert() alone — enforcement on
// "evidence" but NOT on "revoked" — and the entire signature/LTV suite stayed
// green, 32/32. A half-implemented security rule was indistinguishable from
// the whole one.
//
// The consequence of that half-implementation is not a downgrade, it is a
// DENIAL OF SERVICE with a forged cause: a CA that is not entitled to sign
// CRLs could mark any certificate it issued as revoked, and PDFE would report
// the signature as Revoked on that unauthorised say-so.
//
// The assertion is therefore NoDssData, and specifically NOT Revoked: the CRL
// must be discarded outright, not consulted-and-believed.
// ─────────────────────────────────────────────────────────────────────
TEST(CrlIssuerKeyUsageTest, NoCrlSignIssuerCannotRevoke)
{
    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase({"leaf_nocrlsign.pfx", "leaf_nocrlsign.der",
                         "inter_nocrlsign.der", "inter_nocrlsign_key.pem",
                         "crl_ku_revoke.pdf"},
                        status, /*revokeSigner=*/true));
    // PHASE-50 (F53): was NoDssData. The "specifically NOT Revoked" property
    // this test exists for is unchanged and is still asserted, because
    // EXPECT_EQ against RevocationEvidenceRejected excludes Revoked exactly as
    // EXPECT_EQ against NoDssData did. What changes is only that the discarded
    // CRL is now reported as discarded instead of as absent.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A CRL that lists the signer's serial but is signed by a CA whose "
           "keyUsage does not assert cRLSign is not authorised to revoke "
           "anything (RFC 5280 §6.3.3(f)). It must be discarded before its "
           "revocation claim is read. Getting Revoked here means the keyUsage "
           "check is enforced only on the evidence path (hasValidCrlForCert) "
           "and not on the revocation path (isCertRevokedByCrl); got "
        << shown(status);
}

// The revocation path must still WORK when the issuer is entitled — otherwise
// NoCrlSignIssuerCannotRevoke above could pass because revocation-by-CRL is
// broken outright rather than because the keyUsage rule fired. Same body, same
// revoked serial, the only change being which intermediate signs the CRL.
TEST(CrlIssuerKeyUsageTest, CrlSignIssuerCanRevoke)
{
    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase({"leaf_crlsign.pfx", "leaf_crlsign.der",
                         "inter_crlsign.der", "inter_crlsign_key.pem",
                         "crl_ku_revoke_pos.pdf"},
                        status, /*revokeSigner=*/true));
    EXPECT_EQ(status, LtvStatus::Revoked)
        << "A CRL listing the signer's serial, signed by the genuine issuing "
           "CA that asserts cRLSign, IS authoritative and must produce "
           "Revoked. If this is not Revoked, the negative case above proves "
           "nothing about keyUsage; got " << shown(status);
}

// ─────────────────────────────────────────────────────────────────────
// THE CLASS-LEVEL CONTROL (DCA 0014). It asserts a property of the FIXTURE
// CORPUS rather than of one code path: the two chains are indistinguishable
// except through the rule under test, so any divergence between them is
// ATTRIBUTABLE to that rule and to nothing else.
//
// PHASE-10 CORRECTION — THIS CONTROL USED TO BE UNREACHABLE BY THE RULE IT
// GUARDS. As first written it ran only the no-revocation-data half below:
// verifyWith(doc, {leaf, inter}, {}, {}) — BOTH the OCSP and the CRL argument
// empty. With no CRL supplied, crlIsAuthorized() is never called, so the
// KU_CRL_SIGN check DCA 0014 exists to protect could be deleted outright and
// this test would stay green. It pinned the equivalence premise and called
// that the whole job; but "the inputs are equivalent" is only half a control,
// and it is the half that cannot fail when the rule dies.
//
// The two halves are now both asserted, in this order and in one test so they
// cannot drift apart:
//
//   (1) EQUIVALENCE — no revocation data: both chains reach the SAME verdict.
//       Without this, a divergence in (2) could come from a broken chain, a
//       bad digest or an untrusted root rather than from keyUsage.
//   (2) DIVERGENCE — the same CRL minted from each chain's OWN intermediate
//       (exactly as runCase() does at :141): the verdicts must now SPLIT, and
//       split in the specified direction. This is the half a mutation reddens.
//
// Deleting the KU_CRL_SIGN check from crlIsAuthorized() leaves (1) green and
// reddens (2) — which is what makes this a control rather than a description.
// ─────────────────────────────────────────────────────────────────────
TEST(CrlIssuerKeyUsageTest, BothChainsAreOtherwiseEquivalent)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto rootPem = cc::derToPemFile(chainDir() / "root.der", kScratch,
                                    "crl_ku_root_equiv.pem");
    ASSERT_FALSE(rootPem.empty());
    cc::ScopedTrustAnchor anchor(rootPem);
    ASSERT_TRUE(anchor.added);

    struct Chain {
        const char* pfx;
        const char* leaf;
        const char* inter;
        const char* interKey;
        const char* name;
        LtvStatus withCrl;  // the verdict once that chain's OWN CRL is supplied
    };
    const Chain chains[] = {
        {"leaf_crlsign.pfx", "leaf_crlsign.der", "inter_crlsign.der",
         "inter_crlsign_key.pem", "crl_ku_eq_pos.pdf", LtvStatus::LtvValidated},
        {"leaf_nocrlsign.pfx", "leaf_nocrlsign.der", "inter_nocrlsign.der",
         "inter_nocrlsign_key.pem", "crl_ku_eq_neg.pdf",
         LtvStatus::RevocationEvidenceRejected},
    };

    for (const auto& c : chains) {
        auto signedPath = cc::signPdfWith(ctx, chainDir() / c.pfx, kScratch,
                                          c.name, "CrlKuSig");
        ASSERT_FALSE(signedPath.empty()) << "could not sign with " << c.pfx;
        auto leafDer = cc::readAllBytes(chainDir() / c.leaf);
        auto interDer = cc::readAllBytes(chainDir() / c.inter);
        ASSERT_FALSE(leafDer.empty());
        ASSERT_FALSE(interDer.empty());
        auto doc = Document::open(ctx, signedPath);
        ASSERT_NE(doc, nullptr);

        // (1) EQUIVALENCE. NO revocation data at all. Both chains must land on
        // the same verdict, proving the chain/digest/trust preconditions are
        // equal and that only the CRL rule can tell them apart.
        auto bare = LtvValidator::verifyWith(doc.get(), {leafDer, interDer}, {},
                                             {}, /*useTrustAnchors=*/true);
        EXPECT_EQ(bare.status, LtvStatus::NoDssData)
            << c.pfx << ": with no revocation data both chains must reach "
               "NoDssData (trusted chain, no evidence); got "
            << shown(bare.status);

        // (2) DIVERGENCE. Mint an empty, in-date CRL from THIS chain's own
        // intermediate — the identical construction for both rows, differing
        // only in which key/cert signs it, i.e. only in the cRLSign bit that
        // IssuerKeyUsageBitsAreAsIntended has already pinned as the sole
        // difference between the two intermediates.
        X509* interCert = cc::loadDerCert(chainDir() / c.inter);
        ASSERT_NE(interCert, nullptr) << "could not load " << c.inter;
        EVP_PKEY* interKey = cc::loadPemKey(chainDir() / c.interKey);
        if (!interKey) X509_free(interCert);
        ASSERT_NE(interKey, nullptr) << "could not load " << c.interKey;

        cc::CrlSpec spec;
        spec.nextUpdateOffsetSec = 86400;
        auto crl = cc::mintCrl(interKey, interCert, spec);
        EVP_PKEY_free(interKey);
        X509_free(interCert);
        ASSERT_FALSE(crl.empty()) << "could not mint CRL for " << c.inter;

        auto withCrl = LtvValidator::verifyWith(doc.get(), {leafDer, interDer},
                                                {}, {crl},
                                                /*useTrustAnchors=*/true);
        EXPECT_EQ(withCrl.status, c.withCrl)
            << c.pfx << ": supplying a fresh CRL minted by this chain's own "
               "intermediate must move the verdict to " << shown(c.withCrl)
            << ". The two rows are byte-equivalent apart from the issuer's "
               "cRLSign bit and both reached " << shown(bare.status)
            << " without a CRL, so if they do NOT diverge here the RFC 5280 "
               "§6.3.3(f) keyUsage check in crlIsAuthorized() is not running; "
               "got " << shown(withCrl.status);

        // The divergence must be a real split, not both rows agreeing on the
        // same (possibly wrong) value — pinned explicitly so a future edit that
        // makes both expectations identical fails here instead of silently
        // turning this back into a one-sided test.
        EXPECT_NE(withCrl.status, bare.status)
            << c.pfx << ": supplying revocation evidence changed nothing; the "
               "CRL was not consulted on either path";
    }

    static_assert(LtvStatus::LtvValidated != LtvStatus::RevocationEvidenceRejected,
                  "the two rows above must expect DIFFERENT verdicts, or this "
                  "is not a divergence control");
}
