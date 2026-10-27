// -*- coding: utf-8 -*-
//
// Meta-audit follow-up, phase-51: backlog rows F55 and F60(b).
//
// Both rows are about a branch of src/signatures/LtvValidator.cpp that is
// DOCUMENTED in a comment and exercised by NOTHING. A comment is not a control:
// it cannot fail, so it cannot stop the behaviour it describes from changing.
// This file supplies the missing tests, using the two members phase-51 added to
// tests/fixtures/crl_chain/ (see tests/fixtures/gen_two_level_ca_signer.py).
//
// ─────────────────────────────────────────────────────────────────────────
// F55 — the keyUsage-ABSENT branch of crlIsAuthorized()
// ─────────────────────────────────────────────────────────────────────────
// crlIsAuthorized() enforces RFC 5280 §6.3.3(f) with
//
//     if (!(X509_get_key_usage(issuer) & KU_CRL_SIGN)) continue;
//
// and the comment above it says, correctly, that X509_get_key_usage() returns
// UINT32_MAX when the keyUsage extension is ABSENT — every bit set — so a CA
// that states no keyUsage at all is left UNCONSTRAINED, per RFC 5280 §4.2.1.3.
// It then warns: "Do NOT 'tighten' this into a requirement that keyUsage be
// present — that would reject legitimate CAs."
//
// Before this file, nothing enforced that warning. The corpus held two
// intermediates, BOTH carrying keyUsage, so the absent-extension branch was
// unreachable from any test and the exact over-tightening the comment warns
// against would have passed the entire suite. `inter_noku` closes that:
//
//     root
//      ├── inter_crlsign     keyUsage = keyCertSign + cRLSign   (honoured)
//      ├── inter_nocrlsign   keyUsage = keyCertSign             (refused)
//      └── inter_noku        NO keyUsage extension at all       (honoured)
//
// Three intermediates, one variable, all three outcomes of the same line.
//
// THE DISCRIMINATING CONTROL, run in phase-51 and recorded here so a future
// reader can repeat it. Mutate crlIsAuthorized() to demand the extension:
//
//     if (X509_get_key_usage(issuer) == UINT32_MAX) continue;   // ADDED
//     if (!(X509_get_key_usage(issuer) & KU_CRL_SIGN)) continue;
//
//   * NoKeyUsageIssuerAccepted and NoKeyUsageIssuerCanRevoke below → RED
//     (LtvValidated → RevocationEvidenceRejected, Revoked →
//     RevocationEvidenceRejected).
//   * every keyUsage-BEARING case — CrlIssuerKeyUsageTest.CrlSignIssuerAccepted,
//     .CrlSignIssuerCanRevoke, .NoCrlSignIssuerRejected,
//     .NoCrlSignIssuerCannotRevoke — stays GREEN.
//
// That second half is the part that matters: it is what proves these tests
// exercise the ABSENT branch specifically, rather than duplicating coverage the
// phase-48 corpus already had.
//
// ─────────────────────────────────────────────────────────────────────────
// F60(b) — idpCoversCert()'s onlyCA fail-open, PINNED, NOT CHANGED
// ─────────────────────────────────────────────────────────────────────────
// idpCoversCert() reads
//
//     if (covers && idp->onlyCA && X509_check_ca(targetCert) == 0) covers = false;
//
// X509_check_ca() returns 0 for "definitely not a CA" AND for a certificate it
// cannot classify. The call site cannot tell the two apart, so a malformed
// certificate is silently treated as "not a CA".
//
// WHAT THE CONSEQUENCE ACTUALLY IS — and it is the OPPOSITE WAY ROUND from the
// obvious reading. `== 0` sets covers = FALSE, i.e. the CRL is judged NOT to
// cover the certificate. For the "is there evidence" question that fails
// CLOSED (no evidence, no LtvValidated). For the "is this certificate REVOKED"
// question it fails OPEN: a genuine revocation entry, in a CRL signed by the
// certificate's own issuer, is DISCARDED, and the certificate reads as
// not-revoked. That is the direction with security weight, and it is the one
// pinned below.
//
// The backlog frames F60(b) as "neither is a defect today", so this closes to a
// DOCUMENTED-AND-PINNED state. Nothing in src/ changes. The value of the pin is
// that the behaviour is now stated in a form that fails if someone alters it —
// deliberately or, far more likely, as a side effect of touching the onlyCA arm
// for another reason.
//
// The pin is a THREE-WAY comparison, not a single assertion, because a lone
// EXPECT_FALSE would also pass if the whole path walk were broken:
//
//   MalformedCaUnderOnlyCaScopeIsNotRevoked   malformed CA + onlyCA CRL → false
//   WellFormedCaUnderOnlyCaScopeIsRevoked     well-formed CA + onlyCA CRL → true
//   MalformedCaUnderUnscopedCrlIsRevoked      malformed CA + no IDP     → true
//
// The second says the onlyCA machinery works at all; the third says everything
// else about the malformed certificate works — it is issued by the root, the
// root's CRL authenticates, the entry matches. So the ONLY thing that produces
// the `false` in the first is the onlyCA × X509_check_ca()==0 interaction.

#include <gtest/gtest.h>

#include "signatures/LtvValidator.h"
#include "support/CrlChainFixture.h"

#include <QString>

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace pdfe::signatures;
using namespace pdfe::core;
namespace cc = pdfe_test::crlchain;

namespace {

constexpr const char* kScratch = "f55_f60b";

std::filesystem::path chainDir() { return cc::chainDir(); }

std::string shown(LtvStatus s)
{
    return ltvStatusToDisplayString(s).toStdString();
}

// Deliberately the SAME shape as runCase() in tests/test_crl_issuer_keyusage.cpp
// — sign with a leaf, mint a CRL from its issuing intermediate, trust the root,
// ask LtvValidator. Keeping the body identical is what lets the F55 verdicts be
// compared against the phase-48 keyUsage cases as a controlled set: the only
// thing that differs between them is which intermediate is in play.
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
                                      in.signedName, "F55Sig");
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

    // The revoked serial is read from the committed leaf DER, never hard-coded:
    // regenerating the corpus with different serials must not silently turn
    // this into a CRL that revokes nobody.
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
                                    "f55_root.pem");
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

// ── F60(b) helpers ───────────────────────────────────────────────────────
//
// detail::walkPathForRevokedCert() takes the verified path DER directly, so the
// onlyCA scope rule can be driven against an INTERMEDIATE without needing a
// document that OpenSSL would build that path for — which is fortunate, since
// inter_malformed_ca.der is by construction something no path builder would
// accept as a CA. The walk excludes index 0 (the leaf, handled separately by
// verifyWith) and the last element (the trust anchor, which RFC 5280 §6.1 does
// not subject to revocation checking), so index 1 is the certificate under
// test and index 0 is never inspected beyond being parsed.

struct WalkInputs {
    const char* subjectDer;   ///< path[1] — the certificate whose status is asked
    bool onlyCaScope;         ///< emit an issuingDistributionPoint with onlyCA
};

::testing::AssertionResult runWalk(const WalkInputs& in, bool& outRevoked)
{
    X509* rootCert = cc::loadDerCert(chainDir() / "root.der");
    if (!rootCert)
        return ::testing::AssertionFailure() << "could not load root.der";
    EVP_PKEY* rootKey = cc::loadPemKey(chainDir() / "root_key.pem");
    if (!rootKey) {
        X509_free(rootCert);
        return ::testing::AssertionFailure() << "could not load root_key.pem";
    }
    X509* subject = cc::loadDerCert(chainDir() / in.subjectDer);
    if (!subject) {
        EVP_PKEY_free(rootKey);
        X509_free(rootCert);
        return ::testing::AssertionFailure() << "could not load " << in.subjectDer;
    }

    cc::CrlSpec spec;
    spec.nextUpdateOffsetSec = 86400;
    spec.revokedSerials.push_back(X509_get0_serialNumber(subject));
    if (in.onlyCaScope) {
        spec.hasIdp = true;
        spec.idp.onlyContainsCaCerts = true;
    }
    auto crl = cc::mintCrl(rootKey, rootCert, spec);
    EVP_PKEY_free(rootKey);
    X509_free(rootCert);
    X509_free(subject);
    if (crl.empty())
        return ::testing::AssertionFailure() << "could not mint the root CRL";

    // path[0] is excluded from the walk by design; any well-formed leaf serves.
    auto leafDer = cc::readAllBytes(chainDir() / "leaf_crlsign.der");
    auto subjectDer = cc::readAllBytes(chainDir() / in.subjectDer);
    auto rootDer = cc::readAllBytes(chainDir() / "root.der");
    if (leafDer.empty() || subjectDer.empty() || rootDer.empty())
        return ::testing::AssertionFailure() << "missing DER material";

    detail::PathWalkStats stats;
    outRevoked = detail::walkPathForRevokedCert(
        {leafDer, subjectDer, rootDer}, {crl}, &stats);
    if (stats.linkVisits != 1) {
        return ::testing::AssertionFailure()
            << "the walk visited " << stats.linkVisits << " link(s), expected "
               "exactly 1 (path[1]); the comparison below would not be about "
               "the certificate it names";
    }
    return ::testing::AssertionSuccess();
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// F55 — the fixture's own invariants.
//
// If these go red the two behavioural tests below stop being about the
// absent-keyUsage branch, and would keep passing while meaning something
// else. This is the same guard IssuerKeyUsageBitsAreAsIntended provides for
// the phase-48 pair.
// ─────────────────────────────────────────────────────────────────────
TEST(F55NoKeyUsageIssuer, InterNokuHasNoKeyUsageExtensionAtAll)
{
    X509* noku = cc::loadDerCert(chainDir() / "inter_noku.der");
    ASSERT_NE(noku, nullptr) << "inter_noku.der missing or unparseable";

    // ABSENT, not present-and-empty. Only the extension index can tell those
    // apart: a keyUsage extension asserting no bits at all is PRESENT, reads as
    // 0 rather than UINT32_MAX, and would be REFUSED — the opposite verdict to
    // the one this file pins. Checking the returned bits alone would not
    // distinguish the fixture we built from the fixture we did not.
    EXPECT_LT(X509_get_ext_by_NID(noku, NID_key_usage, -1), 0)
        << "inter_noku carries a keyUsage extension; the absent-extension "
           "branch of crlIsAuthorized() is then not what these tests exercise";

    // The premise the LtvValidator.cpp comment rests on, measured rather than
    // quoted: OpenSSL reports an absent keyUsage as every bit set.
    EXPECT_EQ(X509_get_key_usage(noku), UINT32_MAX)
        << "X509_get_key_usage() did not return UINT32_MAX for a certificate "
           "with no keyUsage extension. The comment in crlIsAuthorized() — and "
           "this whole test file — rests on that; if it has changed, the "
           "unconstrained-CA branch needs re-deriving, not re-asserting";
    EXPECT_TRUE(X509_get_key_usage(noku) & KU_CRL_SIGN);

    // It must still be a usable CA, or the chain would fail for an unrelated
    // reason and the CRL verdict below would never be reached.
    EXPECT_GT(X509_check_ca(noku), 0)
        << "inter_noku must be a valid CA; otherwise the chain, not the "
           "keyUsage rule, is what decides the tests below";

    X509_free(noku);
}

TEST(F55NoKeyUsageIssuer, ItDiffersFromTheOtherIntermediatesOnlyInKeyUsage)
{
    X509* noku = cc::loadDerCert(chainDir() / "inter_noku.der");
    X509* yes = cc::loadDerCert(chainDir() / "inter_crlsign.der");
    ASSERT_NE(noku, nullptr);
    ASSERT_NE(yes, nullptr);

    // Same issuer, same validity window, same basicConstraints. If any of these
    // drifted, `inter_noku` would no longer be a one-variable variant of the
    // phase-48 pair and comparing their verdicts would prove nothing.
    EXPECT_EQ(X509_NAME_cmp(X509_get_issuer_name(noku),
                            X509_get_issuer_name(yes)), 0)
        << "the two intermediates are not issued by the same root";
    EXPECT_EQ(ASN1_TIME_compare(X509_get0_notBefore(noku),
                                X509_get0_notBefore(yes)), 0);
    EXPECT_EQ(ASN1_TIME_compare(X509_get0_notAfter(noku),
                                X509_get0_notAfter(yes)), 0);
    EXPECT_GE(X509_get_ext_by_NID(yes, NID_key_usage, -1), 0)
        << "inter_crlsign must CARRY keyUsage, or the pair is not a contrast";

    X509_free(noku);
    X509_free(yes);
}

// ─────────────────────────────────────────────────────────────────────
// F55 POSITIVE: a CA that states NO keyUsage is unconstrained (RFC 5280
// §4.2.1.3), so its CRL is valid revocation evidence and the document
// reaches full LTV.
//
// This is the test that dies under the over-tightening mutation the
// crlIsAuthorized() comment warns about — and the ONLY one that does.
// ─────────────────────────────────────────────────────────────────────
TEST(F55NoKeyUsageIssuer, NoKeyUsageIssuerAccepted)
{
    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase({"leaf_noku.pfx", "leaf_noku.der",
                         "inter_noku.der", "inter_noku_key.pem",
                         "f55_noku_pos.pdf"},
                        status));
    EXPECT_EQ(status, LtvStatus::LtvValidated)
        << "A CA that asserts NO keyUsage extension is UNCONSTRAINED per RFC "
           "5280 §4.2.1.3, and OpenSSL reports that as X509_get_key_usage() == "
           "UINT32_MAX. Its CRL is therefore valid revocation evidence and the "
           "document must reach full LTV. Getting RevocationEvidenceRejected "
           "here means crlIsAuthorized() has been tightened into requiring "
           "keyUsage to be PRESENT — the exact change its comment forbids, "
           "because it rejects legitimate real-world CAs; got " << shown(status);
}

// ─────────────────────────────────────────────────────────────────────
// F55, the REVOCATION direction. Phase-48's SEC-2 finding was that
// §6.3.3(f) is consulted on two independent paths (hasValidCrlForCert →
// "is there evidence", isCertRevokedByCrl → "is the signer revoked") and
// that enforcing it on one only left the suite entirely green. The same
// asymmetry exists for the absent-keyUsage branch, in the same two places,
// so both are pinned rather than just the first.
// ─────────────────────────────────────────────────────────────────────
TEST(F55NoKeyUsageIssuer, NoKeyUsageIssuerCanRevoke)
{
    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase({"leaf_noku.pfx", "leaf_noku.der",
                         "inter_noku.der", "inter_noku_key.pem",
                         "f55_noku_revoke.pdf"},
                        status, /*revokeSigner=*/true));
    EXPECT_EQ(status, LtvStatus::Revoked)
        << "An unconstrained CA — no keyUsage extension — that lists its own "
           "signer's serial in a CRL it signed IS authoritative, and the "
           "signature must read as Revoked. Anything else means the CRL was "
           "discarded before its revocation claim was read, i.e. the "
           "absent-keyUsage case is being treated as unauthorised on the "
           "revocation path; got " << shown(status);
}

// ─────────────────────────────────────────────────────────────────────
// F60(b) — the PRIMITIVE the fail-open rests on, measured.
//
// The whole disposition turns on X509_check_ca() returning 0 for a
// certificate it cannot classify, which is the same value it returns for
// "definitely not a CA". This measures it rather than assuming it: if a
// future OpenSSL distinguished the two, the disposition would need
// re-deriving and this is where that surfaces.
// ─────────────────────────────────────────────────────────────────────
TEST(F60bMalformedCaScope, MalformedCaIsIndistinguishableFromNotACa)
{
    X509* bad = cc::loadDerCert(chainDir() / "inter_malformed_ca.der");
    X509* good = cc::loadDerCert(chainDir() / "inter_crlsign.der");
    X509* leaf = cc::loadDerCert(chainDir() / "leaf_crlsign.der");
    ASSERT_NE(bad, nullptr) << "inter_malformed_ca.der missing or unparseable";
    ASSERT_NE(good, nullptr);
    ASSERT_NE(leaf, nullptr);

    // The malformed certificate: basicConstraints is SEQUENCE { INTEGER 0 },
    // i.e. cA absent (FALSE) WITH a pathLenConstraint, which RFC 5280 §4.2.1.9
    // forbids outright — while keyUsage asserts keyCertSign and cRLSign, so the
    // certificate claims to be a CA in one extension and denies it in another.
    // (Python `cryptography` refuses to model this combination at all; the
    // generator emits the octets verbatim for that reason.)
    EXPECT_EQ(X509_check_ca(bad), 0)
        << "X509_check_ca() no longer returns 0 for the malformed CA. F60(b)'s "
           "entire premise is that 0 conflates 'not a CA' with 'cannot "
           "classify'; a different value here means the conflation is gone and "
           "the disposition must be re-derived rather than this expectation "
           "adjusted";

    // keyCertSign IS asserted, so ku_reject() is provably not what produced the
    // 0 above — the basicConstraints encoding is.
    EXPECT_TRUE(X509_get_key_usage(bad) & KU_KEY_CERT_SIGN)
        << "the malformed CA must assert keyCertSign, or X509_check_ca() would "
           "return 0 for an ordinary keyUsage reason and the fixture would not "
           "be about malformedness at all";
    EXPECT_TRUE(X509_get_key_usage(bad) & KU_CRL_SIGN);

    // The two controls: a real CA classifies as one, a real end-entity does not.
    EXPECT_GT(X509_check_ca(good), 0) << "inter_crlsign must classify as a CA";
    EXPECT_EQ(X509_check_ca(leaf), 0)
        << "a genuine end-entity must also yield 0 — that identity of return "
           "value between it and the malformed CA IS the finding";

    X509_free(bad);
    X509_free(good);
    X509_free(leaf);
}

// ─────────────────────────────────────────────────────────────────────
// F60(b) — THE PIN. This asserts the CURRENT behaviour deliberately.
//
// ACCEPTED, TESTED INVARIANT — NOT A BUG REPORT. The phase-51 disposition
// for F60(b) records that neither the fail-open nor a fail-closed variant
// is a defect today: reaching this state needs a certificate that is
// simultaneously malformed enough to defeat classification and sound enough
// to have been issued by a trusted CA and to sit on a verified path. Do NOT
// "fix" this test by changing idpCoversCert(); if the behaviour is ever
// deliberately changed, change this expectation IN THE SAME COMMIT and say
// which way and why.
// ─────────────────────────────────────────────────────────────────────
TEST(F60bMalformedCaScope, MalformedCaUnderOnlyCaScopeIsNotRevoked)
{
    bool revoked = true;
    ASSERT_TRUE(runWalk({"inter_malformed_ca.der", /*onlyCaScope=*/true}, revoked));
    EXPECT_FALSE(revoked)
        << "PINNED CURRENT BEHAVIOUR (F60(b)). An issuingDistributionPoint "
           "asserting onlyContainsCACerts is judged NOT to cover a certificate "
           "for which X509_check_ca() returns 0 — and it returns 0 for a "
           "malformed certificate exactly as it does for a non-CA. The genuine "
           "revocation entry in the root's own CRL is therefore discarded and "
           "the certificate reads as not-revoked: a fail-OPEN on the revocation "
           "question. If this is now true, idpCoversCert()'s onlyCA arm has "
           "changed; that may well be an improvement, but it is a deliberate "
           "decision and must be recorded as one";
}

// Control 1 — the onlyCA machinery works. Without this, the EXPECT_FALSE above
// would also pass if scoped CRLs were ignored wholesale, or if the path walk
// never found anything revoked under any conditions.
TEST(F60bMalformedCaScope, WellFormedCaUnderOnlyCaScopeIsRevoked)
{
    bool revoked = false;
    ASSERT_TRUE(runWalk({"inter_crlsign.der", /*onlyCaScope=*/true}, revoked));
    EXPECT_TRUE(revoked)
        << "An onlyContainsCACerts CRL from the root, listing a WELL-FORMED "
           "intermediate CA's serial, must revoke it. If this is false the "
           "onlyCA scope rule refuses everything, and the pinned case above is "
           "passing for a reason that has nothing to do with malformedness";
}

// Control 2 — everything else about the malformed certificate works: the root
// issued it, the root's CRL authenticates under §6.3.3(f), the serial matches,
// the freshness window is fine. Only the IDP differs from the pinned case, so
// the onlyCA × X509_check_ca() interaction is isolated as the sole cause.
TEST(F60bMalformedCaScope, MalformedCaUnderUnscopedCrlIsRevoked)
{
    bool revoked = false;
    ASSERT_TRUE(runWalk({"inter_malformed_ca.der", /*onlyCaScope=*/false}, revoked));
    EXPECT_TRUE(revoked)
        << "With NO issuingDistributionPoint the same CRL, the same signer and "
           "the same serial must revoke the malformed certificate. If this is "
           "false, the malformed certificate is being rejected somewhere else "
           "entirely and MalformedCaUnderOnlyCaScopeIsNotRevoked is not "
           "measuring the scope rule";
}
