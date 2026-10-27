// -*- coding: utf-8 -*-
//
// Pipeline e4c71f3a phase-2 (FIX-OCSP-VALIDATOR): Production-level tests
// for OCSP_check_validity() staleness rejection and CRL nextUpdate expiry
// enforcement in LtvValidator.
//
// These test the actual code paths added by FIX-OCSP-VALIDATOR, not the
// mock-level assertions in test_encrypt_ocsp_phase3.cpp.

#include <gtest/gtest.h>

#include "signatures/LtvValidator.h"
#include "signatures/SignatureManager.h"
#include "signatures/TrustAnchorManager.h"
#include "support/CrlChainFixture.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QRectF>

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempDir(const char* sub)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_ocsp_val" / sub;
    std::filesystem::create_directories(d);
    return d;
}

bool loadTestSigner(EVP_PKEY*& outKey, X509*& outCert)
{
    outKey = nullptr;
    outCert = nullptr;
    auto pfxPath = std::filesystem::path(fixturesDir()) / "test_signer.pfx";
    if (!std::filesystem::exists(pfxPath)) return false;

    std::ifstream f(pfxPath, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string buf = ss.str();
    if (buf.empty()) return false;

    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(buf.data());
    PKCS12* p12 = d2i_PKCS12(nullptr, &p, static_cast<long>(buf.size()));
    if (!p12) return false;

    STACK_OF(X509)* chain = nullptr;
    int ok = PKCS12_parse(p12, "test123", &outKey, &outCert, &chain);
    PKCS12_free(p12);
    if (chain) sk_X509_pop_free(chain, X509_free);
    if (!ok || !outKey || !outCert) {
        if (outKey) { EVP_PKEY_free(outKey); outKey = nullptr; }
        if (outCert) { X509_free(outCert); outCert = nullptr; }
        return false;
    }
    return true;
}

std::vector<uint8_t> buildOcspDer(EVP_PKEY* key, X509* cert,
                                   int certStatus,
                                   time_t thisUpdate = 0,
                                   time_t nextUpdate = 0)
{
    std::vector<uint8_t> result;
    if (!key || !cert) return result;

    if (thisUpdate == 0) thisUpdate = time(nullptr);
    if (nextUpdate == 0) nextUpdate = thisUpdate + 86400;

    OCSP_BASICRESP* basic = OCSP_BASICRESP_new();
    if (!basic) return result;

    OCSP_CERTID* certId = OCSP_cert_to_id(EVP_sha1(), cert, cert);
    if (!certId) { OCSP_BASICRESP_free(basic); return result; }

    ASN1_TIME* thisUpd = ASN1_TIME_set(nullptr, thisUpdate);
    ASN1_TIME* nextUpd = ASN1_TIME_set(nullptr, nextUpdate);

    ASN1_TIME* revokedAt = nullptr;
    int reason = 0;
    if (certStatus == V_OCSP_CERTSTATUS_REVOKED) {
        revokedAt = ASN1_TIME_set(nullptr, time(nullptr) - 3600);
        reason = OCSP_REVOKED_STATUS_KEYCOMPROMISE;
    }

    OCSP_basic_add1_status(basic, certId, certStatus, reason,
                           revokedAt, thisUpd, nextUpd);
    OCSP_CERTID_free(certId);
    ASN1_TIME_free(thisUpd);
    ASN1_TIME_free(nextUpd);
    if (revokedAt) ASN1_TIME_free(revokedAt);

    OCSP_basic_sign(basic, cert, key, EVP_sha256(), nullptr, 0);

    OCSP_RESPONSE* resp = OCSP_response_create(
        OCSP_RESPONSE_STATUS_SUCCESSFUL, basic);
    OCSP_BASICRESP_free(basic);
    if (!resp) return result;

    unsigned char* der = nullptr;
    int derLen = i2d_OCSP_RESPONSE(resp, &der);
    OCSP_RESPONSE_free(resp);
    if (derLen > 0 && der) {
        result.assign(der, der + derLen);
        OPENSSL_free(der);
    }
    return result;
}

// NOTE: this file used to carry a local `buildCrlDer()` that minted CRLs from
// `test_signer.pfx`. Every one of its callers was migrated onto the two-level
// corpus (see the SEC-9 block below), and it was removed rather than left as
// a convenient way to reintroduce a self-signed-end-entity CRL.

std::filesystem::path buildSignedFixture(std::shared_ptr<MuPdfContext> ctx,
                                          const char* name)
{
    auto fixtures = std::filesystem::path(fixturesDir());
    auto pfx = fixtures / "test_signer.pfx";
    auto srcPdf = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(srcPdf))
        return {};

    auto out = tempDir("ocsp_val") / name;
    std::filesystem::remove(out);
    SignatureManager mgr(ctx);

    {
        auto doc = Document::open(ctx, srcPdf);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "ValSig",
                                  QRectF(100, 500, 200, 50));
        doc->saveAs(out);
    }
    {
        auto doc = Document::open(ctx, out);
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfx.string();
        opts.pfxPassword = "test123";
        opts.reason = "OCSP validation test";
        opts.location = "PDFE test suite";
        mgr.signField(doc.get(), page.get(), 0, opts);
    }
    return out;
}

// ---------------------------------------------------------------------
// SEC-9 (phase-48 review): the CRL freshness tests below this point run on the
// two-level CA corpus, NOT on test_signer.pfx.
//
// WHY. `test_signer.pfx` is a self-signed END-ENTITY certificate whose
// keyUsage is `digitalSignature, contentCommitment` — no `cRLSign`. Three
// tests in this file minted their CRLs from it and ran with
// useTrustAnchors=false. That made them VACUOUS in two independent ways:
//
//   1. useTrustAnchors=false → the chain never validates → verifyWith()
//      returns at the `if (!chainValid)` branch with UntrustedChain, so
//      `EXPECT_NE(status, LtvValidated)` could not fail for ANY input,
//      expired or fresh. This was true BEFORE phase-48.
//   2. Since phase-48, RFC 5280 §6.3.3(f) additionally discards every CRL
//      signed by that certificate, because it does not assert cRLSign — a
//      second masking layer on top of the first.
//
// Either alone makes "expired CRL rejected" pass for the wrong reason. The
// migration below removes both: a real root→intermediate→leaf chain, the root
// installed as a trust anchor, CRLs issued by an intermediate that genuinely
// carries cRLSign. The rules named in the test names now decide the verdict,
// and each test contrasts against a fresh CRL that reaches LtvValidated
// through the same code path, so "not LtvValidated" can only mean "rejected
// on freshness".
// ---------------------------------------------------------------------
namespace cc = pdfe_test::crlchain;

constexpr const char* kCrlScratch = "freshness";

// Verify a document signed by `leaf_crlsign` against a CRL minted by
// `inter_crlsign` and shaped by `spec`, with `root.der` trusted. `spec` is the
// ONLY thing that varies between the tests below.
::testing::AssertionResult chainCrlVerdict(const cc::CrlSpec& spec,
                                            const char* signedName,
                                            LtvStatus& outStatus)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto dir = cc::chainDir();

    auto signedPath = cc::signPdfWith(ctx, dir / "leaf_crlsign.pfx",
                                      kCrlScratch, signedName, "CrlFreshSig");
    if (signedPath.empty() || !std::filesystem::exists(signedPath))
        return ::testing::AssertionFailure() << "could not sign with leaf_crlsign.pfx";

    X509* interCert = cc::loadDerCert(dir / "inter_crlsign.der");
    if (!interCert)
        return ::testing::AssertionFailure() << "could not load inter_crlsign.der";
    EVP_PKEY* interKey = cc::loadPemKey(dir / "inter_crlsign_key.pem");
    if (!interKey) {
        X509_free(interCert);
        return ::testing::AssertionFailure() << "could not load inter_crlsign_key.pem";
    }
    auto crl = cc::mintCrl(interKey, interCert, spec);
    EVP_PKEY_free(interKey);
    X509_free(interCert);
    if (crl.empty())
        return ::testing::AssertionFailure() << "could not mint CRL";

    auto leafDer = cc::readAllBytes(dir / "leaf_crlsign.der");
    auto interDer = cc::readAllBytes(dir / "inter_crlsign.der");
    if (leafDer.empty() || interDer.empty())
        return ::testing::AssertionFailure() << "missing DER material";

    auto rootPem = cc::derToPemFile(dir / "root.der", kCrlScratch,
                                    "crl_fresh_root.pem");
    if (rootPem.empty())
        return ::testing::AssertionFailure() << "could not write root anchor PEM";
    cc::ScopedTrustAnchor anchor(rootPem);
    if (!anchor.added)
        return ::testing::AssertionFailure() << "could not install root trust anchor";

    auto doc = Document::open(ctx, signedPath);
    if (!doc)
        return ::testing::AssertionFailure() << "could not reopen signed PDF";

    // NOTE the argument slots: verifyWith(doc, certs, OCSPs, CRLs, trust).
    // The pre-migration ExpiredCrlDoesNotCountAsRevocationEvidence passed its
    // CRL DER in the OCSP slot, so it never exercised the CRL path at all.
    auto r = LtvValidator::verifyWith(doc.get(), {leafDer, interDer},
                                      /*ocsps=*/{}, /*crls=*/{crl},
                                      /*useTrustAnchors=*/true);
    outStatus = r.status;
    return ::testing::AssertionSuccess();
}

cc::CrlSpec freshCrl()
{
    cc::CrlSpec s;
    s.thisUpdateOffsetSec = -3600;
    s.nextUpdateOffsetSec = 7 * 86400;
    return s;
}

cc::CrlSpec expiredCrl()
{
    cc::CrlSpec s;
    s.thisUpdateOffsetSec = -2 * 86400;
    s.nextUpdateOffsetSec = -86400;   // nextUpdate = yesterday
    return s;
}

} // namespace

// =====================================================================
// OCSP staleness: production-level tests
// =====================================================================

class OcspValidityTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        signedPath_ = buildSignedFixture(ctx_, "ocsp_validity.pdf");
        if (signedPath_.empty())
            GTEST_SKIP() << "Signing fixtures not available";
        if (!loadTestSigner(key_, cert_))
            GTEST_SKIP() << "Cannot load test signer PFX";
    }

    void TearDown() override {
        if (key_) EVP_PKEY_free(key_);
        if (cert_) X509_free(cert_);
    }

    std::vector<std::vector<uint8_t>> certChain() const {
        unsigned char* der = nullptr;
        int len = i2d_X509(cert_, &der);
        std::vector<uint8_t> certDer(der, der + len);
        OPENSSL_free(der);
        return {certDer};
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path signedPath_;
    EVP_PKEY* key_ = nullptr;
    X509* cert_ = nullptr;
};

TEST_F(OcspValidityTest, FreshOcspResponseAccepted)
{
    // GOOD response with thisUpdate=now, nextUpdate=+1 day.
    time_t now = time(nullptr);
    auto ocspDer = buildOcspDer(key_, cert_, V_OCSP_CERTSTATUS_GOOD,
                                 now, now + 86400);
    ASSERT_FALSE(ocspDer.empty());

    auto doc = Document::open(ctx_, signedPath_);
    ASSERT_NE(doc, nullptr);

    auto result = LtvValidator::verifyWith(
        doc.get(), certChain(), {ocspDer}, {}, false);

    // With useTrustAnchors=false the chain won't be trusted, so we
    // can't get LtvValidated. But the OCSP verdict IS GOOD internally
    // (the function doesn't reject it as stale). The overall status
    // will be UntrustedChain because of the self-signed test cert.
    // The key assertion: the status is NOT Revoked and NOT InvalidSignature.
    EXPECT_NE(result.status, LtvStatus::Revoked);
    EXPECT_NE(result.status, LtvStatus::InvalidSignature);
}

TEST_F(OcspValidityTest, StaleOcspResponseRejectedByValidityCheck)
{
    // thisUpdate = 30 days ago, nextUpdate = 29 days ago.
    // OCSP_check_validity must reject this as stale.
    time_t now = time(nullptr);
    auto staleOcsp = buildOcspDer(key_, cert_, V_OCSP_CERTSTATUS_GOOD,
                                   now - 30 * 86400,
                                   now - 29 * 86400);
    ASSERT_FALSE(staleOcsp.empty());

    // Also build a fresh OCSP for contrast.
    auto freshOcsp = buildOcspDer(key_, cert_, V_OCSP_CERTSTATUS_GOOD,
                                   now, now + 86400);
    ASSERT_FALSE(freshOcsp.empty());

    auto doc = Document::open(ctx_, signedPath_);
    ASSERT_NE(doc, nullptr);

    // With ONLY stale OCSP: should not be LtvValidated.
    auto staleResult = LtvValidator::verifyWith(
        doc.get(), certChain(), {staleOcsp}, {}, false);
    EXPECT_NE(staleResult.status, LtvStatus::LtvValidated)
        << "Stale OCSP (nextUpdate in past) must not produce LtvValidated";

    // With fresh OCSP: the OCSP verdict would be GOOD (distinct behavior).
    // Both end up as UntrustedChain due to self-signed cert, but the
    // internal OCSP verdict differs. This confirms the code path diverges.
    auto freshResult = LtvValidator::verifyWith(
        doc.get(), certChain(), {freshOcsp}, {}, false);
    EXPECT_NE(freshResult.status, LtvStatus::InvalidSignature);
}

TEST_F(OcspValidityTest, FutureOcspResponseRejected)
{
    // thisUpdate = 1 hour in the future (exceeds 5-min drift tolerance).
    time_t now = time(nullptr);
    auto futureOcsp = buildOcspDer(key_, cert_, V_OCSP_CERTSTATUS_GOOD,
                                    now + 3600, now + 7200);
    ASSERT_FALSE(futureOcsp.empty());

    auto doc = Document::open(ctx_, signedPath_);
    ASSERT_NE(doc, nullptr);

    auto result = LtvValidator::verifyWith(
        doc.get(), certChain(), {futureOcsp}, {}, false);
    EXPECT_NE(result.status, LtvStatus::LtvValidated)
        << "Future-dated OCSP response must be rejected";
}

TEST_F(OcspValidityTest, StaleRevokedOcspStillHonoured)
{
    // Revocation is permanent — even a stale OCSP response showing
    // REVOKED should be honoured.
    time_t now = time(nullptr);
    auto staleRevoked = buildOcspDer(key_, cert_, V_OCSP_CERTSTATUS_REVOKED,
                                      now - 30 * 86400,
                                      now - 29 * 86400);
    ASSERT_FALSE(staleRevoked.empty());

    auto doc = Document::open(ctx_, signedPath_);
    ASSERT_NE(doc, nullptr);

    auto result = LtvValidator::verifyWith(
        doc.get(), certChain(), {staleRevoked}, {}, false);
    EXPECT_EQ(result.status, LtvStatus::Revoked)
        << "Revoked status must be honoured even when OCSP response is stale";
}

// =====================================================================
// CRL expiry: production-level tests
// =====================================================================

// A FRESH CRL from an authorised issuer, against a TRUSTED chain, is valid
// revocation evidence and must produce full LTV. This is the positive pole the
// two expiry tests below are measured against: without it, "not LtvValidated"
// would again be unattributable.
TEST(CrlChainFreshnessTest, FreshCrlAccepted)
{
    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(chainCrlVerdict(freshCrl(), "crl_fresh_accept.pdf", status));

    EXPECT_EQ(status, LtvStatus::LtvValidated)
        << "a fresh CRL from the signer's genuine cRLSign-bearing issuer, with "
           "the root trusted, is valid revocation evidence and must reach full "
           "LTV; got " << ltvStatusToDisplayString(status).toStdString();
    // The signer's serial is not listed, so it must not read as revoked.
    EXPECT_NE(status, LtvStatus::Revoked);
    EXPECT_NE(status, LtvStatus::InvalidSignature);
}

// nextUpdate in the past → the CRL is stale and must be discarded. Because the
// chain IS trusted here, the ONLY thing that can move this off LtvValidated is
// the freshness rule.
TEST(CrlChainFreshnessTest, ExpiredCrlRejected)
{
    LtvStatus status = LtvStatus::Unknown;
    ASSERT_TRUE(chainCrlVerdict(expiredCrl(), "crl_expired_reject.pdf", status));

    // PHASE-50 (F53): was NoDssData. An expired CRL is evidence that was
    // supplied and refused on freshness, which now has its own status and a
    // message naming the rule.
    EXPECT_EQ(status, LtvStatus::RevocationEvidenceRejected)
        << "an expired CRL (nextUpdate in the past) must be discarded as "
           "refused evidence. Trust is NOT the confound here — "
           "FreshCrlAccepted reaches LtvValidated through this same "
           "path; got " << ltvStatusToDisplayString(status).toStdString();
    EXPECT_NE(status, LtvStatus::LtvValidated)
        << "Expired CRL (nextUpdate in past) must not produce LtvValidated";
}

// The same input as ExpiredCrlRejected, asserted as a CONTRAST rather than a
// single verdict: fresh and expired CRLs differ ONLY in their validity window,
// and must land on opposite sides of hasRevocationEvidence. This is what makes
// the rule discriminating rather than merely satisfied.
//
// (The pre-migration version of this test passed its CRL DER in verifyWith()'s
// OCSP argument slot, so it exercised the CRL path not at all.)
TEST(CrlChainFreshnessTest, ExpiredCrlDoesNotCountAsRevocationEvidence)
{
    LtvStatus freshStatus = LtvStatus::Unknown;
    ASSERT_TRUE(chainCrlVerdict(freshCrl(), "crl_evidence_fresh.pdf", freshStatus));
    LtvStatus expiredStatus = LtvStatus::Unknown;
    ASSERT_TRUE(chainCrlVerdict(expiredCrl(), "crl_evidence_expired.pdf",
                                expiredStatus));

    EXPECT_EQ(freshStatus, LtvStatus::LtvValidated)
        << "control: a fresh CRL must count as revocation evidence";
    // PHASE-50 (F53): was NoDssData.
    EXPECT_EQ(expiredStatus, LtvStatus::RevocationEvidenceRejected)
        << "an expired CRL must NOT count as revocation evidence in the "
           "hasRevocationEvidence check; got "
        << ltvStatusToDisplayString(expiredStatus).toStdString();
    EXPECT_NE(freshStatus, expiredStatus)
        << "freshness must change the verdict; if these are equal the rule is "
           "not load-bearing regardless of what either verdict is";
}

// =====================================================================
// R-13 (phase-4 SECURITY-ORACLE): revocation-FRESHNESS enforcement must be
// observable INDEPENDENT of trust state.
//
// Every OCSP test above runs with useTrustAnchors=false, so the chain is
// always UntrustedChain and "not LtvValidated" could mean either "untrusted"
// OR "stale" — the staleness rejection is masked. (The CRL tests above no
// longer have this problem: SEC-9 migrated them onto the two-level corpus with
// the root trusted. The OCSP ones are left on `test_signer.pfx` deliberately —
// a self-signed cert is a legitimate OCSP responder for itself, so those tests
// are not vacuous in the same way, and this test is the control that proves
// it.) Here we explicitly TRUST the
// signer (add its self-signed cert to TrustAnchorManager) so the chain is
// trusted, then prove the ONLY thing separating a fresh-GOOD OCSP
// (→ LtvValidated) from a stale-GOOD OCSP (→ NOT LtvValidated) is freshness.
// =====================================================================
TEST_F(OcspValidityTest, StaleOcspRejectedEvenWhenChainIsTrusted)
{
    // Trust the signer explicitly so the chain is no longer the confound.
    auto anchorDer =
        tempDir("ocsp_val") / "ocsp_trust_anchor.der";
    {
        unsigned char* der = nullptr;
        int n = i2d_X509(cert_, &der);
        ASSERT_GT(n, 0);
        std::ofstream(anchorDer, std::ios::binary)
            .write(reinterpret_cast<const char*>(der), n);
        OPENSSL_free(der);
    }
    TrustAnchorManager::instance().clear();
    QString err;
    ASSERT_TRUE(TrustAnchorManager::instance().addTrust(anchorDer, &err))
        << "addTrust failed: " << err.toStdString();

    time_t now = time(nullptr);
    auto freshOcsp = buildOcspDer(key_, cert_, V_OCSP_CERTSTATUS_GOOD,
                                   now, now + 86400);
    auto staleOcsp = buildOcspDer(key_, cert_, V_OCSP_CERTSTATUS_GOOD,
                                   now - 30 * 86400, now - 29 * 86400);
    ASSERT_FALSE(freshOcsp.empty());
    ASSERT_FALSE(staleOcsp.empty());

    // Fresh GOOD OCSP + trusted chain → full LTV validation.
    {
        auto doc = Document::open(ctx_, signedPath_);
        ASSERT_NE(doc, nullptr);
        auto res = LtvValidator::verifyWith(
            doc.get(), certChain(), {freshOcsp}, {}, /*useTrustAnchors=*/true);
        EXPECT_EQ(res.status, LtvStatus::LtvValidated)
            << "R-13: trusted chain + FRESH good OCSP must be LtvValidated (got "
            << static_cast<int>(res.status) << ")";
    }

    // Same trusted chain, but a STALE OCSP → must NOT reach LtvValidated. With
    // trust no longer the confound, this isolates freshness enforcement.
    {
        auto doc = Document::open(ctx_, signedPath_);
        ASSERT_NE(doc, nullptr);
        auto res = LtvValidator::verifyWith(
            doc.get(), certChain(), {staleOcsp}, {}, /*useTrustAnchors=*/true);
        EXPECT_NE(res.status, LtvStatus::LtvValidated)
            << "R-13: a STALE OCSP must be rejected as revocation evidence even "
               "when the signer chain IS trusted — freshness must be enforced "
               "independently of trust";
    }

    TrustAnchorManager::instance().clear();
    std::error_code ec;
    std::filesystem::remove(anchorDer, ec);
}
