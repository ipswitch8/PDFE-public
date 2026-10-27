// -*- coding: utf-8 -*-
// Meta-audit follow-up F23 (gap PDF-39): certificate trust path and chain
// validation.
//
// WHAT WAS MISSING
// ----------------
// PDFE had chain validation (LtvValidator, Pkcs7OpenSslBridge) and a trust
// list (TrustAnchorManager), and NO test that a chain to a trusted anchor
// validates while an otherwise identical chain to an untrusted one does not.
// The two obstacles were infrastructure, not product behaviour:
//
//   1. There was no way to name the anchor set. Validation read the
//      QSettings-backed TrustAnchorManager singleton plus OpenSSL's default
//      certificate paths -- never the Windows store, which PDFE does not
//      consult (GitHub #212).
//      A test could only mutate the user's persisted trust list and
//      hope teardown restored it, which makes trust a property of ambient
//      global state and leaks between tests.
//   2. There was no multi-level chain material. Every existing signature test
//      uses a single self-signed cert, which cannot express "intermediate",
//      "expired intermediate" or "cross-certified".
//
// Both are now fixed: TrustAnchorSet + CertChainValidator make the anchor set
// an explicit parameter (src/signatures/), and tests/fixtures/
// gen_cert_chain_corpus.py synthesises the chains.
//
// THE ORACLE
// ----------
// Every assertion below is on a SPECIFIC verdict (ChainVerdict) and, where it
// discriminates, on the raw X509_V_ERR_* code and the depth. "Validation
// failed" is not a result: a typo'd fixture path, an empty anchor set and a
// genuine trust rejection all produce it. Each negative case is paired with a
// CONTROL that differs in exactly ONE input, so the assertion is attributable.
//
// CALENDAR INDEPENDENCE
// ---------------------
// Nothing here races the clock. The corpus's validity windows are pinned
// constants in the generator, and the verification instant is pinned here
// (kVerificationTimeUnix). The expired intermediate is expired BY
// CONSTRUCTION relative to that instant, not by waiting. One test
// additionally verifies at wall-clock time to prove the default path works;
// it can only start failing after 2199-01-01.
//
// See docs/audit/meta-audit-2026-07/followup/phase-23-dispositions.md.

#include <gtest/gtest.h>

#include "signatures/CertChainValidator.h"
#include "signatures/TrustAnchorSet.h"
#include "signatures/TrustAnchorManager.h"

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pdfe::signatures;

namespace {

// Must match VERIFICATION_INSTANT in tests/fixtures/gen_cert_chain_corpus.py:
// 2030-06-15T00:00:00Z. Inside every "far" validity window (notAfter
// 2199-01-01) and after the deliberately-expired one (notAfter 2021-01-01).
constexpr std::int64_t kVerificationTimeUnix = 1907712000;

// 2020-06-01T00:00:00Z — inside EVERY window in the corpus, including the
// expired intermediate's. Used as the control instant that isolates expiry:
// same certificates, same anchors, only the clock differs.
constexpr std::int64_t kPreExpiryTimeUnix = 1590969600;

std::filesystem::path corpusDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / "cert_chains";
}

// Read a DER fixture. Returns empty on failure; every caller ASSERTs
// non-empty, because a silently-missing fixture would otherwise present as a
// perfectly plausible "untrusted chain" verdict — exactly the false green
// this phase exists to prevent.
std::vector<uint8_t> readDer(const std::string& name)
{
    const auto path = corpusDir() / name;
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

class CertChainTrustTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        rootA_        = readDer("rootA.der");
        interA_       = readDer("interA.der");
        leafA_        = readDer("leafA.der");
        rootB_        = readDer("rootB.der");
        interAExpired_= readDer("interA_expired.der");
        leafAExpired_ = readDer("leafA_expired.der");
        rootC_        = readDer("rootC.der");
        interACross_  = readDer("interA_cross.der");
        leafARevoked_ = readDer("leafA_revoked.der");
        crlRevoked_   = readDer("crlA_revoked.der");
        crlEmpty_     = readDer("crlA_empty.der");

        ASSERT_FALSE(rootA_.empty())         << "missing " << (corpusDir() / "rootA.der").string()
                                             << " — regenerate with: python tests/fixtures/gen_cert_chain_corpus.py";
        ASSERT_FALSE(interA_.empty())        << "missing interA.der";
        ASSERT_FALSE(leafA_.empty())         << "missing leafA.der";
        ASSERT_FALSE(rootB_.empty())         << "missing rootB.der";
        ASSERT_FALSE(interAExpired_.empty()) << "missing interA_expired.der";
        ASSERT_FALSE(leafAExpired_.empty())  << "missing leafA_expired.der";
        ASSERT_FALSE(rootC_.empty())         << "missing rootC.der";
        ASSERT_FALSE(interACross_.empty())   << "missing interA_cross.der";
        ASSERT_FALSE(leafARevoked_.empty())  << "missing leafA_revoked.der";
        ASSERT_FALSE(crlRevoked_.empty())    << "missing crlA_revoked.der";
        ASSERT_FALSE(crlEmpty_.empty())      << "missing crlA_empty.der";

        // The seam's whole promise is that none of this touches the user's
        // persisted trust list. Record its size so TearDown can prove it.
        persistedAnchorsAtStart_ =
            TrustAnchorManager::instance().listTrusts().size();
    }

    void TearDown() override
    {
        // If any test in this file had to mutate the singleton to get its
        // answer, the seam would not be a seam. This is the assertion that
        // holds the design honest.
        EXPECT_EQ(TrustAnchorManager::instance().listTrusts().size(),
                  persistedAnchorsAtStart_)
            << "a test in this file mutated the persisted trust list; the "
               "whole point of TrustAnchorSet is that it never has to.";
    }

    // Anchor set holding exactly the given DER certificates. Asserts every
    // one was accepted, so an anchor set that silently ended up empty cannot
    // masquerade as a genuine "untrusted" verdict.
    static TrustAnchorSet anchorsOf(
        const std::vector<std::vector<uint8_t>>& ders)
    {
        TrustAnchorSet set;
        for (const auto& d : ders) {
            EXPECT_TRUE(set.addDer(d)) << "anchor rejected by TrustAnchorSet";
        }
        EXPECT_EQ(set.size(), static_cast<int>(ders.size()));
        return set;
    }

    // The base request every test starts from: leafA under interA, verified
    // at the pinned instant, useOpenSslDefaultPaths OFF (and, as always, no
    // Windows store — PDFE never reads it). Tests then change exactly one
    // field, which is what makes each pairing a control.
    ChainValidationRequest baseRequest() const
    {
        ChainValidationRequest req;
        req.leafDer = leafA_;
        req.untrustedDer = { interA_ };
        req.verificationTimeUnix = kVerificationTimeUnix;
        req.useOpenSslDefaultPaths = false;
        return req;
    }

    std::vector<uint8_t> rootA_, interA_, leafA_, rootB_;
    std::vector<uint8_t> interAExpired_, leafAExpired_;
    std::vector<uint8_t> rootC_, interACross_;
    std::vector<uint8_t> leafARevoked_, crlRevoked_, crlEmpty_;
    qsizetype persistedAnchorsAtStart_ = 0;
};

// ===========================================================================
// AC-3 #1: a chain to a TRUSTED anchor validates.
// ===========================================================================
TEST_F(CertChainTrustTest, ChainToTrustedAnchorValidates)
{
    ChainValidationRequest req = baseRequest();
    req.anchors = anchorsOf({ rootA_ });

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::Trusted)
        << "got " << chainVerdictToString(res.verdict).toStdString()
        << ": " << res.message.toStdString();
    EXPECT_EQ(res.opensslError, X509_V_OK);
    EXPECT_TRUE(res.trusted());
    // leaf + intermediate + root. A path shorter than 3 would mean OpenSSL
    // short-circuited somewhere and the "trusted" verdict is not about the
    // chain we think we tested.
    EXPECT_EQ(res.chainLength, 3);
    EXPECT_EQ(res.anchorsLoaded, 1);
}

// ===========================================================================
// AC-3 #2: a chain to an UNTRUSTED anchor does NOT validate.
//
// THE DISCRIMINATING CONTROL: this request is byte-identical to the one above
// except for the anchor set. Same leaf, same intermediate, same instant, same
// (disabled) OpenSSL default paths. So the difference in verdict is attributable to the
// anchors and to nothing else.
// ===========================================================================
TEST_F(CertChainTrustTest, ChainToUntrustedAnchorIsRejected)
{
    ChainValidationRequest trustedReq = baseRequest();
    trustedReq.anchors = anchorsOf({ rootA_ });
    const ChainValidationResult trustedRes =
        CertChainValidator::validate(trustedReq);
    ASSERT_EQ(trustedRes.verdict, ChainVerdict::Trusted)
        << "control arm did not validate, so the negative arm proves nothing: "
        << trustedRes.message.toStdString();

    // ONE field differs.
    ChainValidationRequest untrustedReq = baseRequest();
    untrustedReq.anchors = anchorsOf({ rootB_ });
    const ChainValidationResult res = CertChainValidator::validate(untrustedReq);

    EXPECT_EQ(res.verdict, ChainVerdict::UntrustedRoot)
        << "got " << chainVerdictToString(res.verdict).toStdString()
        << ": " << res.message.toStdString();
    EXPECT_NE(res.opensslError, X509_V_OK);
    // The specific reason: the path could not be completed to anything in the
    // store. Asserting the code rules out "failed for some other reason that
    // happens to also be a failure".
    EXPECT_TRUE(res.opensslError == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT ||
                res.opensslError == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY)
        << "unexpected X509_V_ERR " << res.opensslError << ": "
        << res.message.toStdString();
    // Depth 1 = the intermediate is where the walk ran out of issuers.
    EXPECT_EQ(res.errorDepth, 1);
    // rootB WAS loaded. The rejection is a trust decision, not an empty store.
    EXPECT_EQ(res.anchorsLoaded, 1);
    EXPECT_FALSE(res.trusted());
}

// ===========================================================================
// pathDer is THE VERIFIED PATH, and is empty when nothing was verified.
//
// Phase-49 added ChainValidationResult::pathDer for the revocation walk, and
// its header contract says "the certificates whose signatures were VERIFIED".
// The re-encode loop ran unconditionally, so on the failure path it handed out
// X509_STORE_CTX_get0_chain()'s PARTIALLY BUILT chain — whose upper elements
// came from the caller's untrusted material (for LtvValidator, the document's
// own attacker-controlled /DSS) and may never have had a signature checked.
// Measured at the time: trusted=0, chainLength=2, pathDer.size()=2.
//
// Not a live bypass — LtvValidator gates every positive verdict behind
// `if (!chainValid)` — but the entire security argument for the F2 path walk
// and for the indirect-CRL rule is the phrase "the verified path", and
// ancestorsAbove() feeds these certificates to crlIsAuthorized() with no
// re-verification. The pair below makes the contract true by construction
// instead of by every future caller remembering (phase-49 review, finding M1).
// ===========================================================================
TEST_F(CertChainTrustTest, PathDerIsThePathWhenVerificationSucceeds)
{
    ChainValidationRequest req = baseRequest();
    req.anchors = anchorsOf({ rootA_ });

    const ChainValidationResult res = CertChainValidator::validate(req);

    ASSERT_TRUE(res.trusted()) << res.message.toStdString();
    ASSERT_EQ(res.chainLength, 3);
    ASSERT_EQ(static_cast<int>(res.pathDer.size()), res.chainLength)
        << "a verified path must be reported in full: a short path silently "
           "shortens the revocation walk in LtvValidator, which fails OPEN";
    // Leaf first, anchor last — the ordering every caller indexes by.
    EXPECT_EQ(res.pathDer.front(), leafA_);
    EXPECT_EQ(res.pathDer[1], interA_);
    EXPECT_EQ(res.pathDer.back(), rootA_);
}

TEST_F(CertChainTrustTest, PathDerIsEmptyWhenVerificationFails)
{
    // ONE field differs from the case above: the anchor set. Same leaf, same
    // untrusted material, same instant.
    ChainValidationRequest req = baseRequest();
    req.anchors = anchorsOf({ rootB_ });

    const ChainValidationResult res = CertChainValidator::validate(req);

    ASSERT_FALSE(res.trusted()) << res.message.toStdString();
    // OpenSSL still built something — that is the point. chainLength reports
    // how far it got, and is deliberately still populated.
    EXPECT_GT(res.chainLength, 0)
        << "if OpenSSL built no partial chain at all this case cannot "
           "discriminate; pathDer would be empty either way";
    EXPECT_TRUE(res.pathDer.empty())
        << "pathDer holds " << res.pathDer.size() << " certificate(s) after a "
           "FAILED verification. Its documented contract is 'the certificates "
           "whose signatures were verified', and LtvValidator hands exactly "
           "these to crlIsAuthorized() without re-verifying them — so a "
           "partially built path here is untrusted /DSS material travelling "
           "under a name that says otherwise";
}

// An empty anchor set with useOpenSslDefaultPaths off must also reject — and
// this is what proves the seam is real: no ambient trust source the machine
// offers, whatever it contains, can rescue this chain. (On Windows the OS
// certificate store is not among those sources at all; GitHub #212.)
// Renamed from EmptyAnchorSetWithoutSystemStoreRejects (GitHub #233): the
// flag this test clears is useOpenSslDefaultPaths, which is not the system
// store and never was. The old name asserted the very equation the DECISION
// block in CertChainValidator.h exists to disarm, and a name survives every
// grep aimed at comments.
TEST_F(CertChainTrustTest, EmptyAnchorSetWithoutDefaultPathsRejects)
{
    ChainValidationRequest req = baseRequest();  // anchors default-constructed
    ASSERT_TRUE(req.anchors.isEmpty());

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::UntrustedRoot)
        << res.message.toStdString();
    EXPECT_EQ(res.anchorsLoaded, 0);
    EXPECT_FALSE(res.trusted());
}

// SECURITY INVARIANT: untrustedDer is path-building material, never a source
// of trust. This is the "document self-certifies" attack — LtvValidator's
// comment has always claimed /DSS-embedded certs cannot terminate a chain,
// and this is the assertion that holds it. A self-signed certificate offered
// ONLY as untrusted material must be rejected, no matter that it is present.
//
// Discovered while mutation-testing this file (see phase-23-dispositions.md,
// M0): adding a non-self-signed intermediate to the trust store does NOT make
// it an anchor, because OpenSSL still walks on to a self-signed root. So a
// mutation that leaked untrustedDer into the store was invisible to every
// other test here. A self-signed leaf is the case that catches it.
TEST_F(CertChainTrustTest, SelfSignedCertOfferedAsUntrustedMaterialIsNotAnAnchor)
{
    ChainValidationRequest req;
    req.leafDer = rootA_;               // self-signed CA, as the leaf
    req.untrustedDer = { rootA_ };      // offered, but not trusted
    req.anchors = anchorsOf({ rootB_ }); // the only trust source, unrelated
    req.useOpenSslDefaultPaths = false;
    req.verificationTimeUnix = kVerificationTimeUnix;

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::UntrustedRoot)
        << "a certificate supplied as untrusted material must never be able "
           "to terminate its own chain: "
        << res.message.toStdString();
    EXPECT_EQ(res.opensslError, X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT);

    // CONTROL: the same certificate as an ANCHOR does validate. Only the
    // field it was supplied in differs, so the rejection above is about
    // trust source and not about the certificate.
    ChainValidationRequest controlReq = req;
    controlReq.anchors = anchorsOf({ rootA_ });
    const ChainValidationResult controlRes =
        CertChainValidator::validate(controlReq);
    EXPECT_EQ(controlRes.verdict, ChainVerdict::Trusted)
        << controlRes.message.toStdString();
}

// A malformed leaf must NOT be reported as a trust failure. Conflating the
// two is how a broken fixture path becomes a plausible-looking green.
TEST_F(CertChainTrustTest, MalformedLeafIsNotAnUntrustedVerdict)
{
    ChainValidationRequest req = baseRequest();
    req.anchors = anchorsOf({ rootA_ });
    req.leafDer = { 0x30, 0x82, 0xde, 0xad };  // not a certificate

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::MalformedInput)
        << res.message.toStdString();
    EXPECT_NE(res.verdict, ChainVerdict::UntrustedRoot);
}

// ===========================================================================
// AC-3 #3: an EXPIRED INTERMEDIATE is rejected.
//
// THE DISCRIMINATING CONTROL: the same certificates and the same anchor set,
// verified at kPreExpiryTimeUnix (inside the intermediate's window) instead
// of kVerificationTimeUnix (outside it). Only the clock differs, so the
// rejection is attributable to expiry rather than to chain structure.
// ===========================================================================
TEST_F(CertChainTrustTest, ExpiredIntermediateIsRejected)
{
    ChainValidationRequest req;
    req.leafDer = leafAExpired_;
    req.untrustedDer = { interAExpired_ };
    req.anchors = anchorsOf({ rootA_ });
    req.useOpenSslDefaultPaths = false;
    req.verificationTimeUnix = kVerificationTimeUnix;

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::ExpiredCertificate)
        << "got " << chainVerdictToString(res.verdict).toStdString()
        << ": " << res.message.toStdString();
    EXPECT_EQ(res.opensslError, X509_V_ERR_CERT_HAS_EXPIRED);
    // Depth 1 = the INTERMEDIATE expired, not the leaf. The leaf's own
    // notAfter is 2199-01-01, so a depth-0 result would mean the corpus is
    // not testing what this test claims.
    EXPECT_EQ(res.errorDepth, 1);
    // The path to the anchor EXISTS — this is not an untrusted-root failure.
    EXPECT_NE(res.verdict, ChainVerdict::UntrustedRoot);
}

TEST_F(CertChainTrustTest, ExpiredIntermediateControlValidatesBeforeExpiry)
{
    ChainValidationRequest req;
    req.leafDer = leafAExpired_;
    req.untrustedDer = { interAExpired_ };
    req.anchors = anchorsOf({ rootA_ });
    req.useOpenSslDefaultPaths = false;
    // ONE field differs from the test above.
    req.verificationTimeUnix = kPreExpiryTimeUnix;

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::Trusted)
        << "the same chain must validate inside the intermediate's window; "
           "if it does not, the expiry test above is not isolating expiry: "
        << res.message.toStdString();
    EXPECT_EQ(res.opensslError, X509_V_OK);
    EXPECT_EQ(res.chainLength, 3);
}

// ===========================================================================
// AC-3 #4: a CROSS-CERTIFIED path resolves.
//
// interA_cross carries interA's subject DN and public key but is issued by
// rootC. So leafA — issued by interA — reaches rootC through it, even though
// rootA is nowhere in the anchor set.
//
// THE DISCRIMINATING CONTROL: remove the cross-certificate from the untrusted
// pool and nothing else. The path must then fail, proving the success above
// went THROUGH the cross-certificate rather than around it.
// ===========================================================================
TEST_F(CertChainTrustTest, CrossCertifiedPathResolves)
{
    ChainValidationRequest req = baseRequest();
    req.untrustedDer = { interACross_ };
    req.anchors = anchorsOf({ rootC_ });   // rootA deliberately absent

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::Trusted)
        << "got " << chainVerdictToString(res.verdict).toStdString()
        << ": " << res.message.toStdString();
    EXPECT_EQ(res.opensslError, X509_V_OK);
    EXPECT_EQ(res.chainLength, 3);  // leafA -> interA_cross -> rootC
}

TEST_F(CertChainTrustTest, CrossCertifiedPathControlFailsWithoutCrossCert)
{
    ChainValidationRequest req = baseRequest();
    // ONE field differs: the ordinary interA replaces the cross-certificate.
    req.untrustedDer = { interA_ };
    req.anchors = anchorsOf({ rootC_ });

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::UntrustedRoot)
        << "without the cross-certificate leafA must NOT reach rootC; if it "
           "does, the cross-certified test above is not proving anything: "
        << res.message.toStdString();
    EXPECT_NE(res.opensslError, X509_V_OK);
}

// The same leaf reaching a DIFFERENT anchor via a different path — rootA
// directly — confirms interA_cross is genuinely an alternative route rather
// than the only one that works.
TEST_F(CertChainTrustTest, CrossCertifiedLeafAlsoResolvesViaOriginalRoot)
{
    ChainValidationRequest req = baseRequest();
    req.untrustedDer = { interA_ };
    req.anchors = anchorsOf({ rootA_ });

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::Trusted) << res.message.toStdString();
}

// ===========================================================================
// Revocation: the corpus's CRL material is not decoration.
//
// THE DISCRIMINATING CONTROL: a structurally identical CRL from the same
// issuer that revokes nothing. Without it, a "revoked" verdict could be an
// artefact of CRL checking being switched on at all.
// ===========================================================================
TEST_F(CertChainTrustTest, RevokedLeafIsRejectedByCrl)
{
    ChainValidationRequest req;
    req.leafDer = leafARevoked_;
    req.untrustedDer = { interA_ };
    req.anchors = anchorsOf({ rootA_ });
    req.crlsDer = { crlRevoked_ };
    req.useOpenSslDefaultPaths = false;
    req.verificationTimeUnix = kVerificationTimeUnix;

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::Revoked)
        << "got " << chainVerdictToString(res.verdict).toStdString()
        << ": " << res.message.toStdString();
    EXPECT_EQ(res.opensslError, X509_V_ERR_CERT_REVOKED);
    EXPECT_EQ(res.errorDepth, 0);  // the leaf is the revoked cert
}

TEST_F(CertChainTrustTest, RevocationControlEmptyCrlLeavesChainTrusted)
{
    ChainValidationRequest req;
    req.leafDer = leafARevoked_;
    req.untrustedDer = { interA_ };
    req.anchors = anchorsOf({ rootA_ });
    // ONE field differs: same issuer, same freshness window, no entries.
    req.crlsDer = { crlEmpty_ };
    req.useOpenSslDefaultPaths = false;
    req.verificationTimeUnix = kVerificationTimeUnix;

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::Trusted)
        << "with CRL checking ON but the serial absent, the chain must be "
           "trusted; otherwise the revoked verdict above is an artefact of "
           "enabling CRL checking: "
        << res.message.toStdString();
    EXPECT_EQ(res.opensslError, X509_V_OK);
}

// ===========================================================================
// Calendar independence, asserted rather than asserted-about.
// ===========================================================================

// The corpus's valid material must also validate at WALL-CLOCK time, so the
// pinned-instant tests above are not hiding a corpus that only works in a
// simulated past. notAfter is 2199-01-01, so this cannot age out.
TEST_F(CertChainTrustTest, TrustedChainAlsoValidatesAtWallClockTime)
{
    ChainValidationRequest req = baseRequest();
    req.anchors = anchorsOf({ rootA_ });
    req.verificationTimeUnix = 0;   // 0 == "now"

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::Trusted)
        << "the corpus validity windows run to 2199-01-01; a failure here "
           "means the fixtures were regenerated with different pins: "
        << res.message.toStdString();
}

// The expired intermediate must be expired at wall-clock time too — expired
// BY CONSTRUCTION (notAfter 2021-01-01), never by waiting.
TEST_F(CertChainTrustTest, ExpiredIntermediateIsExpiredAtWallClockTimeToo)
{
    ChainValidationRequest req;
    req.leafDer = leafAExpired_;
    req.untrustedDer = { interAExpired_ };
    req.anchors = anchorsOf({ rootA_ });
    req.useOpenSslDefaultPaths = false;
    req.verificationTimeUnix = 0;

    const ChainValidationResult res = CertChainValidator::validate(req);

    EXPECT_EQ(res.verdict, ChainVerdict::ExpiredCertificate)
        << res.message.toStdString();
    EXPECT_EQ(res.errorDepth, 1);
}

// ===========================================================================
// TrustAnchorSet itself — the value type the seam is built on.
// ===========================================================================
TEST_F(CertChainTrustTest, TrustAnchorSetRejectsNonCertificateBytes)
{
    TrustAnchorSet set;
    EXPECT_FALSE(set.addDer(QByteArray()));
    EXPECT_FALSE(set.addDer(std::vector<uint8_t>{}));
    EXPECT_FALSE(set.addDer(std::vector<uint8_t>{ 0x30, 0x03, 0x02, 0x01, 0x00 }));
    EXPECT_TRUE(set.isEmpty())
        << "garbage was accepted as a trust anchor; a caller that typo'd an "
           "anchor would then see a genuine-looking untrusted verdict.";

    EXPECT_TRUE(set.addDer(rootA_));
    EXPECT_EQ(set.size(), 1);
}

TEST_F(CertChainTrustTest, TrustAnchorSetIsIndependentOfThePersistedList)
{
    // Build a set with an anchor that is NOT in the user's persisted list,
    // use it, and confirm the persisted list is untouched. This is the
    // property that lets these tests run without QSettings isolation at all.
    const qsizetype before = TrustAnchorManager::instance().listTrusts().size();

    ChainValidationRequest req = baseRequest();
    req.anchors = anchorsOf({ rootA_ });
    const ChainValidationResult res = CertChainValidator::validate(req);
    ASSERT_EQ(res.verdict, ChainVerdict::Trusted) << res.message.toStdString();

    EXPECT_EQ(TrustAnchorManager::instance().listTrusts().size(), before);
    // And the manager's own snapshot() — the production path — still reports
    // exactly the persisted anchors, not ours.
    EXPECT_EQ(TrustAnchorManager::instance().snapshot().size(),
              static_cast<int>(before));
}

} // namespace
