// -*- coding: utf-8 -*-
// ===========================================================================
// Meta-audit follow-up F47, items F2 / F3 / F4 — phase-49.
//
// Three CRL defects, all of them defence-in-depth rather than bypasses
// (certIssuedSigner() already closes the forgery bypass this programme found
// in phase-42), and all three sharing one root cause: PDFE's CRL handling was
// written as if a CRL were a flat list of serials issued by exactly one
// authority about exactly one certificate — the LEAF.
//
//   F2  Revocation was checked at the LEAF ONLY. A revoked intermediate CA
//       was invisible: its leaves are individually spotless, so an attacker
//       holding a compromised-and-revoked CA key could mint a signer
//       certificate that reported LtvValidated. The CRL proving the CA
//       revoked could be sitting in the document's own /DSS and nothing
//       looked at it.
//
//   F3  issuingDistributionPoint SCOPES a CRL. Phase-42, honestly unable to
//       interpret it, rejected every CRL bearing any critical extension — and
//       IDP is always critical, so every scoped CRL was discarded. Sound but
//       crude: discarding evidence produces "no evidence", which is the state
//       an attacker wants. The fix INTERPRETS the scope instead.
//
//   F4  RFC 5280 §5.3.3 certificateIssuer was modelled as "does this entry
//       carry the extension at all", which is wrong in BOTH directions (see
//       the two tests below that pin each direction).
//
// EVERY test here is paired with a control in which the pre-fix code reached
// the OPPOSITE verdict, so none of them can pass by accident. The pairs are
// named in phase-49-dispositions.md alongside the measured revert-and-observe
// output.
//
// Fixtures: tests/fixtures/crl_chain/ (phase-48, byte-pinned by
// tools/gate/check_crl_chain_fixture.py). Helpers: tests/support/
// CrlChainFixture.h — shared, never copied; copying them is the sibling-drift
// defect this programme keeps hitting.
// ===========================================================================

#include "support/CrlChainFixture.h"

#include "signatures/LtvValidator.h"

#include <openssl/x509v3.h>

using namespace pdfe::core;
using namespace pdfe::signatures;
namespace cc = pdfe_test::crlchain;
using cc::chainDir;

namespace {

constexpr const char* kScratch = "scope_chain";

std::string shown(LtvStatus s)
{
    return ltvStatusToDisplayString(s).toStdString();
}

// Everything a case needs, loaded once and owned for the case's lifetime.
// ASN1_INTEGER* serials point INTO these certificates, so they must outlive
// every CRL minted from them.
struct Corpus {
    X509* root = nullptr;
    X509* inter = nullptr;
    X509* leaf = nullptr;
    X509* crldpLeaf = nullptr;
    EVP_PKEY* rootKey = nullptr;
    EVP_PKEY* interKey = nullptr;
    std::vector<uint8_t> leafDer;
    std::vector<uint8_t> interDer;
    std::vector<uint8_t> crldpLeafDer;

    bool load()
    {
        root = cc::loadDerCert(chainDir() / "root.der");
        inter = cc::loadDerCert(chainDir() / "inter_crlsign.der");
        leaf = cc::loadDerCert(chainDir() / "leaf_crlsign.der");
        crldpLeaf = cc::loadDerCert(chainDir() / "leaf_crldp.der");
        rootKey = cc::loadPemKey(chainDir() / "root_key.pem");
        interKey = cc::loadPemKey(chainDir() / "inter_crlsign_key.pem");
        leafDer = cc::readAllBytes(chainDir() / "leaf_crlsign.der");
        interDer = cc::readAllBytes(chainDir() / "inter_crlsign.der");
        crldpLeafDer = cc::readAllBytes(chainDir() / "leaf_crldp.der");
        return root && inter && leaf && crldpLeaf && rootKey && interKey
               && !leafDer.empty() && !interDer.empty()
               && !crldpLeafDer.empty();
    }
    ~Corpus()
    {
        if (root) X509_free(root);
        if (inter) X509_free(inter);
        if (leaf) X509_free(leaf);
        if (crldpLeaf) X509_free(crldpLeaf);
        if (rootKey) EVP_PKEY_free(rootKey);
        if (interKey) EVP_PKEY_free(interKey);
    }
};

// The distribution point leaf_crldp.der names, pinned in
// tests/fixtures/gen_two_level_ca_signer.py as LEAF_CRLDP_URI. Any IDP naming
// anything else must NOT cover it — that is the whole content of the
// fullName-matching block in idpCoversCert(). CorpusShapeIsAsTheseTestsAssume
// asserts the fixture really carries this string, so the two cannot drift into
// agreeing by accident.
constexpr const char* kLeafCrldpUri = "http://crl.pdfe.test/f47/inter_crlsign.crl";

// Sign with `pfxName`, install the root as a trust anchor, and validate
// against `crls`. Returns the LtvStatus. `pdfName` must be unique per case so
// two cases cannot collide on disk.
::testing::AssertionResult runCaseWith(
    const char* pfxName,
    const char* pdfName,
    const std::vector<std::vector<uint8_t>>& crls,
    const std::vector<uint8_t>& leafDer,
    const std::vector<uint8_t>& interDer,
    LtvStatus& outStatus)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto signedPath = cc::signPdfWith(ctx, chainDir() / pfxName,
                                      kScratch, pdfName, "CrlScopeSig");
    if (signedPath.empty())
        return ::testing::AssertionFailure() << "could not sign " << pdfName;

    auto rootPem = cc::derToPemFile(chainDir() / "root.der", kScratch,
                                    "scope_root.pem");
    if (rootPem.empty())
        return ::testing::AssertionFailure() << "could not write root anchor PEM";
    cc::ScopedTrustAnchor anchor(rootPem);
    if (!anchor.added)
        return ::testing::AssertionFailure() << "could not install root anchor";

    auto doc = Document::open(ctx, signedPath);
    if (!doc)
        return ::testing::AssertionFailure() << "could not reopen " << pdfName;

    auto r = LtvValidator::verifyWith(doc.get(), {leafDer, interDer}, {}, crls,
                                      /*useTrustAnchors=*/true);
    outStatus = r.status;
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult runCase(
    const char* pdfName,
    const std::vector<std::vector<uint8_t>>& crls,
    const std::vector<uint8_t>& leafDer,
    const std::vector<uint8_t>& interDer,
    LtvStatus& outStatus)
{
    return runCaseWith("leaf_crlsign.pfx", pdfName, crls, leafDer, interDer,
                       outStatus);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════
// Fixture invariants. If these go red, every comparison below stops being
// controlled and its verdict means nothing — so the failure names itself
// rather than silently turning the controls into decoration.
// ═══════════════════════════════════════════════════════════════════════
TEST(CrlScopeAndChainTest, CorpusShapeIsAsTheseTestsAssume)
{
    Corpus c;
    ASSERT_TRUE(c.load()) << "crl_chain corpus incomplete — root_key.pem is "
                             "emitted by tests/fixtures/gen_two_level_ca_signer.py";

    // The path must be leaf -> inter -> root, with root self-signed, or the
    // "revoke the intermediate" case is not testing a chain at all.
    EXPECT_EQ(X509_NAME_cmp(X509_get_issuer_name(c.leaf),
                            X509_get_subject_name(c.inter)), 0)
        << "leaf_crlsign is not issued by inter_crlsign";
    EXPECT_EQ(X509_NAME_cmp(X509_get_issuer_name(c.inter),
                            X509_get_subject_name(c.root)), 0)
        << "inter_crlsign is not issued by root";
    EXPECT_EQ(X509_NAME_cmp(X509_get_issuer_name(c.root),
                            X509_get_subject_name(c.root)), 0)
        << "root is not self-signed";

    // The root's key must actually be the root's, or every CRL this file
    // mints from it is unauthorised for the wrong reason.
    EXPECT_EQ(X509_check_private_key(c.root, c.rootKey), 1)
        << "root_key.pem does not match root.der";
    EXPECT_EQ(X509_check_private_key(c.inter, c.interKey), 1)
        << "inter_crlsign_key.pem does not match inter_crlsign.der";

    // The root must carry cRLSign, else the F2/F4 cases would fail on the
    // phase-48 keyUsage rule rather than on what they mean to measure.
    EXPECT_TRUE(X509_get_key_usage(c.root) & KU_CRL_SIGN)
        << "root does not assert cRLSign";

    // The CA/user-cert distinction is what the IDP scope tests turn on.
    EXPECT_GT(X509_check_ca(c.inter), 0) << "inter_crlsign is not a CA";
    EXPECT_EQ(X509_check_ca(c.leaf), 0) << "leaf_crlsign must NOT be a CA";

    // No cRLDistributionPoints on the leaf: the DP-name branch of
    // idpCoversCert() must refuse, and these tests must not accidentally
    // depend on a DP match.
    auto* dps = static_cast<STACK_OF(DIST_POINT)*>(
        X509_get_ext_d2i(c.leaf, NID_crl_distribution_points, nullptr, nullptr));
    EXPECT_EQ(dps, nullptr) << "leaf_crlsign unexpectedly carries a CRLDP";
    if (dps) CRL_DIST_POINTS_free(dps);
    ERR_clear_error();

    // ...and leaf_crldp is its opposite number: same issuer, same non-CA
    // shape, but it NAMES exactly one distribution point. The DP tests are a
    // controlled comparison between these two certificates, so both halves
    // have to be what they claim (phase-49 review, finding M3).
    EXPECT_EQ(X509_NAME_cmp(X509_get_issuer_name(c.crldpLeaf),
                            X509_get_subject_name(c.inter)), 0)
        << "leaf_crldp is not issued by inter_crlsign, so it is not comparable "
           "with leaf_crlsign";
    EXPECT_EQ(X509_check_ca(c.crldpLeaf), 0) << "leaf_crldp must NOT be a CA";

    auto* crldpDps = static_cast<STACK_OF(DIST_POINT)*>(
        X509_get_ext_d2i(c.crldpLeaf, NID_crl_distribution_points, nullptr,
                         nullptr));
    ASSERT_NE(crldpDps, nullptr)
        << "leaf_crldp carries NO cRLDistributionPoints, so the fullName-"
           "matching block of idpCoversCert() is still unreachable and every "
           "DP test below is vacuous";
    ASSERT_EQ(sk_DIST_POINT_num(crldpDps), 1);
    DIST_POINT* dp0 = sk_DIST_POINT_value(crldpDps, 0);
    ASSERT_NE(dp0, nullptr);
    ASSERT_NE(dp0->distpoint, nullptr);
    ASSERT_EQ(dp0->distpoint->type, 0) << "expected a fullName, not an RDN";
    ASSERT_EQ(sk_GENERAL_NAME_num(dp0->distpoint->name.fullname), 1);
    GENERAL_NAME* gn0 = sk_GENERAL_NAME_value(dp0->distpoint->name.fullname, 0);
    ASSERT_NE(gn0, nullptr);
    EXPECT_EQ(gn0->type, GEN_URI);
    if (gn0->type == GEN_URI) {
        const std::string got(
            reinterpret_cast<const char*>(
                ASN1_STRING_get0_data(gn0->d.uniformResourceIdentifier)),
            static_cast<size_t>(
                ASN1_STRING_length(gn0->d.uniformResourceIdentifier)));
        EXPECT_EQ(got, std::string(kLeafCrldpUri))
            << "leaf_crldp names a different distribution point than the tests "
               "below mint IDPs for; the DP-match cases would then be measuring "
               "a mismatch and passing for the wrong reason";
    }
    CRL_DIST_POINTS_free(crldpDps);
    ERR_clear_error();
}

// ═══════════════════════════════════════════════════════════════════════
// F2 — revocation is a property of the PATH, not of the leaf.
//
// The pair below differs in EXACTLY ONE byte-level fact: whether the root's
// CRL lists the intermediate's serial. Both cases supply the same two CRLs
// (one from the root, one from the intermediate), the same chain, the same
// anchor. Pre-fix, both reached LtvValidated, because nothing ever asked the
// root's CRL about the intermediate.
// ═══════════════════════════════════════════════════════════════════════
TEST(CrlScopeAndChainTest, RevokedIntermediateIsDetected)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec rootSpec;
    rootSpec.revokedSerials = {X509_get0_serialNumber(c.inter)};
    auto rootCrl = cc::mintCrl(c.rootKey, c.root, rootSpec);
    ASSERT_FALSE(rootCrl.empty());
    auto interCrl = cc::mintCrl(c.interKey, c.inter, {});  // empty: leaf is fine
    ASSERT_FALSE(interCrl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f2_revoked.pdf", {rootCrl, interCrl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::Revoked)
        << "The signer's issuing CA is listed as revoked by its own issuer's "
           "CRL. Everything that CA issued is thereby untrustworthy, so the "
           "document must not validate. Getting LtvValidated here means "
           "revocation is still being checked at the leaf only (F47/F2); got "
        << shown(status);
}

TEST(CrlScopeAndChainTest, UnrevokedIntermediateStillValidates)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // IDENTICAL to the case above except that the root's CRL is empty. If this
    // is not LtvValidated then the case above proves nothing: it would be
    // failing because the path walk rejects everything, not because it found
    // a revocation.
    auto rootCrl = cc::mintCrl(c.rootKey, c.root, {});
    ASSERT_FALSE(rootCrl.empty());
    auto interCrl = cc::mintCrl(c.interKey, c.inter, {});
    ASSERT_FALSE(interCrl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f2_clean.pdf", {rootCrl, interCrl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::LtvValidated)
        << "With a fresh CRL from the signer's CA and a fresh CRL from the "
           "root, neither listing anything, the chain is fully covered by "
           "revocation evidence and must reach full LTV; got " << shown(status);
}

// A revoked intermediate must not be mistakable for a revoked LEAF: the two
// produce different messages, because a user told "the signer is revoked"
// would go and inspect the wrong certificate.
TEST(CrlScopeAndChainTest, RevokedCaAndRevokedLeafAreReportedDistinctly)
{
    Corpus c;
    ASSERT_TRUE(c.load());
    auto ctx = std::make_shared<MuPdfContext>();
    auto rootPem = cc::derToPemFile(chainDir() / "root.der", kScratch,
                                    "scope_root_msg.pem");
    ASSERT_FALSE(rootPem.empty());
    cc::ScopedTrustAnchor anchor(rootPem);
    ASSERT_TRUE(anchor.added);

    auto signedPath = cc::signPdfWith(ctx, chainDir() / "leaf_crlsign.pfx",
                                      kScratch, "scope_f2_msg.pdf", "CrlScopeSig");
    ASSERT_FALSE(signedPath.empty());

    cc::CrlSpec caSpec;
    caSpec.revokedSerials = {X509_get0_serialNumber(c.inter)};
    auto caCrl = cc::mintCrl(c.rootKey, c.root, caSpec);
    cc::CrlSpec leafSpec;
    leafSpec.revokedSerials = {X509_get0_serialNumber(c.leaf)};
    auto leafCrl = cc::mintCrl(c.interKey, c.inter, leafSpec);
    ASSERT_FALSE(caCrl.empty());
    ASSERT_FALSE(leafCrl.empty());

    auto doc = Document::open(ctx, signedPath);
    ASSERT_NE(doc, nullptr);

    auto caCase = LtvValidator::verifyWith(doc.get(), {c.leafDer, c.interDer},
                                           {}, {caCrl}, true);
    auto leafCase = LtvValidator::verifyWith(doc.get(), {c.leafDer, c.interDer},
                                             {}, {leafCrl}, true);
    EXPECT_EQ(caCase.status, LtvStatus::Revoked);
    EXPECT_EQ(leafCase.status, LtvStatus::Revoked);
    EXPECT_NE(caCase.message, leafCase.message)
        << "a revoked CA and a revoked signer produce the same message, so a "
           "user cannot tell which certificate to go and look at";
}

// ═══════════════════════════════════════════════════════════════════════
// F3 — issuingDistributionPoint scope.
//
// The pair that discriminates against the PRE-FIX code is
// InScopeIdpCrlIsAcceptedAsEvidence (pre-fix: NoDssData, because any critical
// extension was rejected outright).
//
// The pair that discriminates against a NAIVE widening — "recognise IDP and
// then ignore what it says" — is OutOfScopeIdpCrlIsNotEvidence and
// OutOfScopeIdpCrlCannotRevoke. Those are the tests that make the widening
// safe rather than merely permissive, and they are what goes red if the
// onlyContainsUserCerts / onlyContainsCACerts comparison is deleted.
// ═══════════════════════════════════════════════════════════════════════
TEST(CrlScopeAndChainTest, InScopeIdpCrlIsAcceptedAsEvidence)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.onlyContainsUserCerts = true;  // the leaf IS a user cert
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f3_inscope.pdf", {crl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::LtvValidated)
        << "A CRL scoped to user certificates COVERS the signer, which is a "
           "user certificate, so it is valid revocation evidence. Getting "
           "NoDssData here means issuingDistributionPoint is still being "
           "refused wholesale as an uninterpretable critical extension "
           "(F47/F3); got " << shown(status);
}

TEST(CrlScopeAndChainTest, OutOfScopeIdpCrlIsNotEvidence)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // IDENTICAL to the case above but for onlyContainsCACerts. The signer is
    // not a CA, so this CRL says nothing whatsoever about it and must not be
    // counted as evidence about it.
    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.onlyContainsCaCerts = true;
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f3_outscope.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A CRL scoped to CA certificates is not authoritative about an "
           "end-entity signer. Getting LtvValidated here means the IDP was "
           "parsed and then ignored — a widening that accepts more than it "
           "checks; got " << shown(status);
}

TEST(CrlScopeAndChainTest, OutOfScopeIdpCrlCannotRevoke)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // The same out-of-scope CRL, this time LISTING the signer's serial. Scope
    // must be honoured in the revocation direction too, or an out-of-scope
    // CRL becomes a denial-of-service primitive. (This is the same
    // both-directions lesson phase-48 learned about the cRLSign rule.)
    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.onlyContainsCaCerts = true;
    spec.revokedSerials = {X509_get0_serialNumber(c.leaf)};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f3_outscope_rev.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "An out-of-scope CRL must be discarded BEFORE its revocation claim "
           "is read, exactly as an unauthorised one is. Getting Revoked here "
           "means scope is enforced on the evidence path but not on the "
           "revocation path; got " << shown(status);
}

TEST(CrlScopeAndChainTest, InScopeIdpCrlCanRevoke)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // ...and the control for the case immediately above: revocation through an
    // IN-scope IDP CRL must still WORK, or OutOfScopeIdpCrlCannotRevoke could
    // be passing because IDP CRLs never revoke anything.
    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.onlyContainsUserCerts = true;
    spec.revokedSerials = {X509_get0_serialNumber(c.leaf)};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f3_inscope_rev.pdf", {crl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::Revoked)
        << "An in-scope IDP CRL listing the signer's serial is authoritative "
           "and must produce Revoked; got " << shown(status);
}

// The scope rule must follow the extension's MEANING, not its criticality
// flag. A non-critical IDP is malformed per RFC 5280 but still states a
// scope, and honouring a scoped CRL as unscoped because a flag was clear
// would be a trivially forgeable widening.
TEST(CrlScopeAndChainTest, NonCriticalIdpIsStillHonoured)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.onlyContainsCaCerts = true;
    spec.idp.critical = false;
    spec.revokedSerials = {X509_get0_serialNumber(c.leaf)};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f3_noncrit.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A NON-critical issuingDistributionPoint still scopes the CRL. "
           "Getting Revoked here means the scope check is gated on the "
           "criticality flag, so clearing one bit re-opens the whole rule; "
           "got " << shown(status);
}

// Every critical extension OTHER than issuingDistributionPoint must still be
// an outright refusal. This is the test that fails if F3's widening is
// implemented as "stop rejecting critical extensions" rather than as "except
// this one, which is now interpreted".
TEST(CrlScopeAndChainTest, OtherCriticalExtensionsAreStillRefused)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // A delta CRL — deltaCRLIndicator, always critical, still unimplemented.
    cc::CrlSpec spec;
    spec.revokedSerials = {X509_get0_serialNumber(c.leaf)};
    auto der = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(der.empty());
    // Re-open, add deltaCRLIndicator, re-sign.
    const unsigned char* p = der.data();
    X509_CRL* crl = d2i_X509_CRL(nullptr, &p, static_cast<long>(der.size()));
    ASSERT_NE(crl, nullptr);
    ASN1_INTEGER* base = ASN1_INTEGER_new();
    ASN1_INTEGER_set(base, 1);
    ASSERT_EQ(X509_CRL_add1_ext_i2d(crl, NID_delta_crl, base, 1, 0), 1);
    ASN1_INTEGER_free(base);
    ASSERT_NE(X509_CRL_sign(crl, c.interKey, EVP_sha256()), 0);
    unsigned char* out = nullptr;
    const int len = i2d_X509_CRL(crl, &out);
    X509_CRL_free(crl);
    ASSERT_GT(len, 0);
    std::vector<uint8_t> deltaCrl(out, out + len);
    OPENSSL_free(out);

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f3_delta.pdf", {deltaCrl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A delta CRL lists only CHANGES since a base CRL and remains "
           "unimplemented; it must still be refused outright. Getting Revoked "
           "or LtvValidated here means F3 relaxed the critical-extension rule "
           "wholesale instead of interpreting one extension; got "
        << shown(status);
}

// ═══════════════════════════════════════════════════════════════════════
// F4 — RFC 5280 §5.3.3 certificateIssuer, and indirect CRLs.
//
// The pre-fix rule was `revoked = !entryHasCertificateIssuerExtension`, which
// is wrong in both directions. Each direction gets its own test, because a
// fix for one that leaves the other broken is precisely the half-fix shape
// phase-48 caught.
// ═══════════════════════════════════════════════════════════════════════
TEST(CrlScopeAndChainTest, EntryNamingOurOwnIssuerDoesRevoke)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // A perfectly ordinary revocation, written the explicit way: the entry
    // spells out the certificate issuer instead of relying on the default.
    // Naming your own issuer is legal and is the NORMAL shape inside an
    // indirect CRL.
    //
    // PRE-FIX: the entry carried certificateIssuer, therefore it was
    // classified "about someone else", therefore a genuine revocation was
    // silently NOT SEEN. A false negative on a revocation check.
    cc::CrlSpec spec;
    spec.entries = {{X509_get0_serialNumber(c.leaf),
                     X509_get_subject_name(c.inter)}};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f4_ownissuer.pdf", {crl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::Revoked)
        << "An entry whose certificateIssuer names the signer's OWN issuer is "
           "a revocation of the signer. Getting NoDssData here means any "
           "presence of certificateIssuer is still being read as 'about "
           "somebody else' (F47/F4); got " << shown(status);
}

TEST(CrlScopeAndChainTest, CarryForwardStopsAnotherIssuersEntryRevokingUs)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // Two entries, in this ENCODED order:
    //   [0] certificateIssuer = root,  serial = root's
    //   [1] no certificateIssuer,      serial = the SIGNER's
    //
    // RFC 5280 §5.3.3: entry [1] inherits entry [0]'s certificate issuer. It
    // is an entry about a certificate issued by the ROOT, and the signer was
    // not issued by the root, so it does not revoke the signer.
    //
    // PRE-FIX: entry [1] carries no certificateIssuer extension, so it was
    // read as "ours" and the signer was reported REVOKED on the strength of
    // somebody else's list. A false positive — a denial-of-service primitive
    // available to anyone who can append two entries to a CRL.
    cc::CrlSpec spec;
    spec.sortEntries = false;  // carry-forward is defined over encoded order
    spec.entries = {
        {X509_get0_serialNumber(c.root), X509_get_subject_name(c.root)},
        {X509_get0_serialNumber(c.leaf), nullptr},
    };
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f4_carry.pdf", {crl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::LtvValidated)
        << "An entry that INHERITS a preceding entry's certificateIssuer is "
           "about that other issuer's certificate, not ours. Getting Revoked "
           "here means certificateIssuer carry-forward (RFC 5280 §5.3.3) is "
           "not implemented and a third party's entry can revoke our signer; "
           "got " << shown(status);
}

// The control for the case above: with the SAME two-entry shape and the SAME
// encoded order, but entry [0] naming our own issuer, the carry-forward makes
// entry [1] ours and the revocation MUST land. Without this, the test above
// could be passing because two-entry CRLs never revoke anything.
TEST(CrlScopeAndChainTest, CarryForwardFromOurOwnIssuerStillRevokes)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.sortEntries = false;
    spec.entries = {
        {X509_get0_serialNumber(c.root), X509_get_subject_name(c.inter)},
        {X509_get0_serialNumber(c.leaf), nullptr},
    };
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f4_carry_ours.pdf", {crl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::Revoked)
        << "Entry [1] inherits entry [0]'s certificateIssuer, which here is "
           "the signer's own issuer, so it revokes the signer; got "
        << shown(status);
}

TEST(CrlScopeAndChainTest, IndirectCrlFromAncestorRevokesLeaf)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // An indirect CRL: issued and signed by the ROOT, carrying an entry about
    // a certificate issued by the INTERMEDIATE, and saying so via
    // indirectCRL in its issuingDistributionPoint.
    //
    // PRE-FIX: the CRL's issuer DN (root) did not equal the signer's issuer DN
    // (intermediate), so the CRL was skipped before anything was read out of
    // it. A genuine revocation, correctly published, was invisible.
    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.indirectCrl = true;
    spec.entries = {{X509_get0_serialNumber(c.leaf),
                     X509_get_subject_name(c.inter)}};
    auto crl = cc::mintCrl(c.rootKey, c.root, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f4_indirect.pdf", {crl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::Revoked)
        << "An indirect CRL issued by a verified ancestor CA, asserting "
           "indirectCRL and naming the signer's issuer per-entry, is "
           "authoritative about the signer; got " << shown(status);
}

// The safety control for the widening above. Same CRL, same signer, same
// ancestor — but WITHOUT the indirectCRL assertion. A CRL that does not
// declare itself indirect may not speak for another issuer, because
// otherwise any CA anywhere above any certificate could revoke it silently
// and RFC 5280's indirection contract would be unenforced.
TEST(CrlScopeAndChainTest, NonIndirectCrlFromAncestorCannotSpeakForLeaf)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;  // note: no IDP at all, so no indirectCRL assertion
    spec.entries = {{X509_get0_serialNumber(c.leaf),
                     X509_get_subject_name(c.inter)}};
    auto crl = cc::mintCrl(c.rootKey, c.root, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f4_notindirect.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A CRL that does not assert indirectCRL may only speak about "
           "certificates its own issuer issued. Getting Revoked here means "
           "the indirect path was opened to every CRL rather than to CRLs "
           "that declare themselves indirect; got " << shown(status);
}

// The other half of the indirect gate: asserting indirectCRL is not enough
// on its own. The signer must be a verified ancestor. Here the intermediate
// that did NOT issue the signer mints an indirect CRL about it — it has no
// authority over this certificate and must be ignored.
TEST(CrlScopeAndChainTest, IndirectCrlFromANonAncestorIsRefused)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // inter_nocrlsign is a genuine CA under the same root, and it is NOT on
    // the signer's path. (It also lacks cRLSign, which is a second,
    // independent reason to refuse — both must hold, and that is the point:
    // authority is not transitive between siblings.)
    X509* sibling = cc::loadDerCert(chainDir() / "inter_nocrlsign.der");
    EVP_PKEY* siblingKey = cc::loadPemKey(chainDir() / "inter_nocrlsign_key.pem");
    ASSERT_NE(sibling, nullptr);
    ASSERT_NE(siblingKey, nullptr);

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.indirectCrl = true;
    spec.entries = {{X509_get0_serialNumber(c.leaf),
                     X509_get_subject_name(c.inter)}};
    auto crl = cc::mintCrl(siblingKey, sibling, spec);
    ASSERT_FALSE(crl.empty());
    X509_free(sibling);
    EVP_PKEY_free(siblingKey);

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_f4_sibling.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "An indirect CRL from a CA that is not on the signer's verified "
           "path has no authority over the signer, however well it is signed. "
           "Getting Revoked here means asserting indirectCRL is sufficient to "
           "revoke anyone; got " << shown(status);
}

// ═══════════════════════════════════════════════════════════════════════
// H1 (phase-49 review) — AN INDIRECT CRL IS EVIDENCE OF REVOCATION, NEVER
// EVIDENCE OF NON-REVOCATION.
//
// The indirect widening above (F4) opened crlAuthorityFor() to CRLs issued by
// a verified ancestor. hasValidCrlForCert() accepted `!= CrlAuthority::None`,
// so an ancestor's indirect CRL counted as COMPLETE evidence about the leaf —
// but nothing in it establishes that its scope covers the leaf. An IDP whose
// only assertion is indirectCRL sets none of idpCoversCert()'s `covers = false`
// conditions, so that function answers true for every certificate in existence.
//
// The two directions are asymmetric on purpose, and the pair below is what
// pins the asymmetry: a revocation claim is positive and self-attributing
// (crlEntryRevokesCert still requires serial AND certificateIssuer to match),
// whereas the ABSENCE of a serial from a third party's list is a claim of
// completeness that nobody made.
//
// THE ATTACK THIS CLOSES, using only genuine correctly-signed artifacts: the
// signer's certificate has been revoked by its issuing CA. The document author
// embeds the hierarchy's legitimate root-issued combined CRL and simply omits
// the intermediate's CRL carrying the revocation. Pre-fix that document read
// LtvValidated.
// ═══════════════════════════════════════════════════════════════════════
TEST(CrlScopeAndChainTest, AncestorIndirectCrlListingNothingIsNotEvidenceAboutLeaf)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    // ONE CRL: root-issued, EMPTY, asserting indirectCRL and nothing else.
    // Nothing from the signer's own issuer at all.
    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.indirectCrl = true;
    auto crl = cc::mintCrl(c.rootKey, c.root, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_h1_indirect_empty.pdf", {crl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::NoDssData)
        << "An ancestor's indirect CRL that lists nothing says nothing about "
           "whether the signer is revoked: it never claimed to enumerate the "
           "signer's issuer's revocations. Getting LtvValidated here means "
           "hasValidCrlForCert() is accepting CrlAuthority::Indirect as "
           "complete evidence, which lets an attacker prove non-revocation by "
           "supplying a third party's genuine list and withholding the one "
           "that matters; got " << shown(status);
}

// The control that keeps the case above from being vacuous: the signer's OWN
// issuer's empty CRL — the Direct case — still IS evidence, and still reaches
// full LTV. Same document, same anchor, same empty CRL shape; the only
// difference is who issued it.
TEST(CrlScopeAndChainTest, DirectIssuerCrlListingNothingIsEvidenceAboutLeaf)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    auto crl = cc::mintCrl(c.interKey, c.inter, {});
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_h1_direct_empty.pdf", {crl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::LtvValidated)
        << "An empty, fresh, in-scope CRL from the signer's OWN issuer is a "
           "complete statement that the signer is not revoked, and must still "
           "reach full LTV. If this is NoDssData the case above proves nothing "
           "— the Direct path would be broken rather than the Indirect path "
           "restricted; got " << shown(status);
}

// ═══════════════════════════════════════════════════════════════════════
// M3 (phase-49 review) — the refusals phase-49-dispositions.md §2 claimed but
// no test exercised.
//
// idpCoversCert() refuses five things. Phase-49 shipped tests for two of them
// (onlyContainsCACerts mismatch, and delta CRLs via the critical-extension
// rule). The other three, and the ~36-line distributionPoint fullName-matching
// block — the most intricate new code in the change — had ZERO coverage, and
// CrlSpec::Idp could not even express them. A partial idpCoversCert() handling
// only onlyuser/onlyCA passed the entire suite.
//
// Every case below is paired with a control that differs in exactly the one
// fact under test, so none of them can pass by being refused for some other
// reason. (Refusal is the fail-closed direction, which is precisely why an
// unpaired refusal test is nearly free to pass.)
// ═══════════════════════════════════════════════════════════════════════

// onlySomeReasons — the refusal the brief singled out. A reason-partitioned
// CRL enumerates revocations for SOME reason codes only. Its silence about a
// certificate is not a statement that the certificate is unrevoked; RFC 5280
// §6.3.3(b)(2) requires unioning the reason codes across a SET of CRLs to
// conclude anything, which PDFE does not model.
TEST(CrlScopeAndChainTest, ReasonScopedCrlIsNotEvidence)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.onlySomeReasonsBits = 0x02;  // one reason bit; which one is moot
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_m3_reasons.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A reason-partitioned CRL is not a complete statement about a "
           "certificate, so its silence is not evidence of non-revocation. "
           "Getting LtvValidated here means idpCoversCert() parses the IDP and "
           "ignores onlySomeReasons — the exact partial implementation that "
           "passed the whole suite before this test existed; got "
        << shown(status);
}

// CONTROL for the above: byte-identical CRL minus the onlySomeReasons bits.
// The IDP is still there, still critical, still from the same issuer.
TEST(CrlScopeAndChainTest, UnreasonScopedCrlIsEvidence)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;  // an IDP asserting nothing at all: unscoped
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_m3_noreasons.pdf", {crl},
                        c.leafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::LtvValidated)
        << "The same CRL WITHOUT onlySomeReasons is unscoped and must count. "
           "If this is NoDssData then ReasonScopedCrlIsNotEvidence is passing "
           "because IDP-bearing CRLs never count, not because of the reason "
           "partition; got " << shown(status);
}

// ...and the revocation direction, which is where a scope rule enforced on one
// side only becomes a denial-of-service primitive.
TEST(CrlScopeAndChainTest, ReasonScopedCrlCannotRevoke)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.onlySomeReasonsBits = 0x02;
    spec.revokedSerials = {X509_get0_serialNumber(c.leaf)};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_m3_reasons_rev.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A CRL we refuse to interpret must be discarded BEFORE its "
           "revocation claim is read, exactly as an unauthorised one is; got "
        << shown(status);
}

// onlyContainsAttributeCertificates — PDFE validates public-key certificates.
// A CRL scoped to attribute certificates is about a different kind of object
// entirely, so it neither covers nor revokes the signer.
TEST(CrlScopeAndChainTest, AttributeCertificateScopedCrlIsNotEvidence)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.onlyAttributeCerts = true;
    spec.revokedSerials = {X509_get0_serialNumber(c.leaf)};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_m3_attrcert.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A CRL declaring itself to cover attribute certificates says "
           "nothing about a public-key signing certificate, in either "
           "direction. Getting Revoked here means onlyattr is unread and a "
           "serial collision across two different object namespaces can revoke "
           "the signer; got " << shown(status);
}

// ═══════════════════════════════════════════════════════════════════════
// The distributionPoint fullName block. Three cases, turning on the ONE fact
// that separates leaf_crlsign from leaf_crldp: whether the certificate names
// a distribution point, and whether it is the same one.
// ═══════════════════════════════════════════════════════════════════════

// A DP-scoped CRL is authoritative only for certificates that point AT that
// distribution point. leaf_crlsign names none, so it is not covered.
TEST(CrlScopeAndChainTest, DpScopedCrlDoesNotCoverACertWithNoCrldp)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.distributionPointUri = kLeafCrldpUri;
    spec.revokedSerials = {X509_get0_serialNumber(c.leaf)};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_m3_dp_nocrldp.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "A certificate carrying no cRLDistributionPoints names no "
           "distribution point, so a DP-scoped CRL is not authoritative for "
           "it. Getting Revoked or LtvValidated here means the distpoint "
           "branch of idpCoversCert() is not being entered at all; got "
        << shown(status);
}

// The positive half: leaf_crldp DOES name that distribution point, so the same
// shape of CRL covers it and is complete evidence about it.
TEST(CrlScopeAndChainTest, DpScopedCrlCoversACertWhoseCrldpMatches)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.distributionPointUri = kLeafCrldpUri;
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCaseWith("leaf_crldp.pfx", "scope_m3_dp_match.pdf", {crl},
                            c.crldpLeafDer, c.interDer, status));
    EXPECT_EQ(status, LtvStatus::LtvValidated)
        << "The signer's own cRLDistributionPoints names exactly this "
           "distribution point, so the CRL covers it and is complete evidence. "
           "Getting NoDssData here means GENERAL_NAME_cmp never matches and "
           "every DP-scoped CRL is discarded — a refusal that would make the "
           "two negative DP cases pass for the wrong reason; got "
        << shown(status);
}

// ...and the discriminator for the name comparison itself. Identical to the
// case above but for one character in the URI: the CRL must stop covering the
// certificate. Without this, `matched = true` unconditionally would pass.
TEST(CrlScopeAndChainTest, DpScopedCrlWithADifferentUriDoesNotCover)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.distributionPointUri =
        std::string(kLeafCrldpUri) + ".other";  // one suffix differs
    spec.revokedSerials = {X509_get0_serialNumber(c.crldpLeaf)};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCaseWith("leaf_crldp.pfx", "scope_m3_dp_mismatch.pdf", {crl},
                            c.crldpLeafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "The CRL is scoped to a DIFFERENT distribution point than the one "
           "the signer names, so it is not authoritative for the signer even "
           "though it lists its serial. Getting Revoked here means the "
           "fullName comparison is not actually comparing names; got "
        << shown(status);
}

// A distributionPoint given as nameRelativeToCRLIssuer is an RDN FRAGMENT, and
// resolving it means reconstructing a full DN against the CRL issuer's. A
// mis-reconstruction silently WIDENS scope, so it is refused outright.
TEST(CrlScopeAndChainTest, NameRelativeToCrlIssuerDistributionPointIsRefused)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    spec.hasIdp = true;
    spec.idp.distributionPointRelativeCn = "PDFE F47 DP";
    spec.revokedSerials = {X509_get0_serialNumber(c.crldpLeaf)};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCaseWith("leaf_crldp.pfx", "scope_m3_dp_relative.pdf", {crl},
                            c.crldpLeafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "nameRelativeToCRLIssuer is a scope statement PDFE does not resolve, "
           "and an unresolved scope must fail closed. Getting LtvValidated or "
           "Revoked here means an RDN fragment is being treated as 'no "
           "distribution point at all', i.e. as unscoped; got " << shown(status);
}

// An issuingDistributionPoint that is PRESENT but will not decode. This is the
// branch that distinguishes "absent, therefore unscoped, therefore covers
// everything" from "stated, but unreadable". Reading an unreadable scope as
// absent is a one-byte widening available to anyone who can corrupt a CRL.
TEST(CrlScopeAndChainTest, UndecodableIdpIsRefused)
{
    Corpus c;
    ASSERT_TRUE(c.load());

    cc::CrlSpec spec;
    // Not a valid ISSUING_DIST_POINT: a bare ASN.1 BOOLEAN.
    spec.rawIdpExtensionDer = {0x01, 0x01, 0xFF};
    spec.revokedSerials = {X509_get0_serialNumber(c.leaf)};
    auto crl = cc::mintCrl(c.interKey, c.inter, spec);
    ASSERT_FALSE(crl.empty());

    // The extension really must be present and really must not decode, or this
    // test measures nothing.
    const unsigned char* p = crl.data();
    X509_CRL* parsed = d2i_X509_CRL(nullptr, &p, static_cast<long>(crl.size()));
    ASSERT_NE(parsed, nullptr);
    EXPECT_GE(X509_CRL_get_ext_by_NID(parsed, NID_issuing_distribution_point, -1), 0)
        << "the raw IDP extension was not installed on the CRL";
    auto* decoded = static_cast<ISSUING_DIST_POINT*>(X509_CRL_get_ext_d2i(
        parsed, NID_issuing_distribution_point, nullptr, nullptr));
    EXPECT_EQ(decoded, nullptr) << "the 'undecodable' IDP decoded after all";
    if (decoded) ISSUING_DIST_POINT_free(decoded);
    X509_CRL_free(parsed);
    ERR_clear_error();

    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(runCase("scope_m3_idp_garbage.pdf", {crl},
                        c.leafDer, c.interDer, status));
    // PHASE-50 (F53): was NoDssData. This CRL was PRESENT and a named rule
    // refused it. NoDssData now means only "no evidence was supplied at
    // all"; a refusal has its own status and its own message.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "An issuingDistributionPoint that is present but cannot be decoded "
           "is a scope statement we cannot honour, and must be refused rather "
           "than read as 'unscoped'. Getting Revoked here means the "
           "present-but-unparseable branch is gone and a corrupted IDP widens "
           "the CRL to everything; got " << shown(status);
}
