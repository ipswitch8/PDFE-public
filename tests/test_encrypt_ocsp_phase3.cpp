// -*- coding: utf-8 -*-
//
// Pipeline b9e14d07 phase-3: SEC-ENCRYPT-META + SEC-OCSP edge cases.
//
// SEC-ENCRYPT-META: encryptMetadata=false roundtrip — metadata readable
//   after encryption.
// SEC-OCSP: OCSP unknown status, stale OCSP data, expired CRL.

#include <gtest/gtest.h>

#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "metadata/MetadataManager.h"
#include "signatures/DssReader.h"
#include "signatures/LtvDssWriter.h"
#include "signatures/LtvValidator.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"
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

using namespace pdfe::encryption;
using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempDir(const char* sub)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_phase3" / sub;
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

// NOTE (SEC-9, phase-48 review): this file used to carry a local
// `buildCrlDer()` minting CRLs from `test_signer.pfx` — a self-signed
// END-ENTITY cert with no `cRLSign`. Its only caller,
// `OcspEdgeCaseTest.ExpiredCrlNotLtvValidated`, also ran with
// useTrustAnchors=false, so its verdict was UntrustedChain and its
// `EXPECT_NE(..., LtvValidated)` could not fail for any input. That test was
// migrated onto the two-level corpus (see below) and the helper removed rather
// than left as a convenient way to mint an unauthorised CRL again.


std::filesystem::path buildSignedFixture(std::shared_ptr<MuPdfContext> ctx,
                                          const char* name)
{
    auto fixtures = std::filesystem::path(fixturesDir());
    auto pfx = fixtures / "test_signer.pfx";
    auto srcPdf = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(srcPdf))
        return {};

    auto out = tempDir("ocsp") / name;
    std::filesystem::remove(out);
    SignatureManager mgr(ctx);

    {
        auto doc = Document::open(ctx, srcPdf);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "OcspSig",
                                  QRectF(100, 500, 200, 50));
        doc->saveAs(out);
    }
    {
        auto doc = Document::open(ctx, out);
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfx.string();
        opts.pfxPassword = "test123";
        opts.reason = "OCSP test";
        opts.location = "PDFE test suite";
        mgr.signField(doc.get(), page.get(), 0, opts);
    }
    return out;
}

} // namespace

// =====================================================================
// SEC-ENCRYPT-META: encryptMetadata=false roundtrip
// =====================================================================

class EncryptMetadataTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        fixture_ = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (!std::filesystem::exists(fixture_))
            GTEST_SKIP() << "test_single.pdf fixture missing";
        encMgr_ = std::make_unique<EncryptionManager>(ctx_);
        metaMgr_ = std::make_unique<pdfe::metadata::MetadataManager>(ctx_);
        td_ = tempDir("encrypt_meta");
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path td_;
    std::unique_ptr<EncryptionManager> encMgr_;
    std::unique_ptr<pdfe::metadata::MetadataManager> metaMgr_;
};

TEST_F(EncryptMetadataTest, EncryptMetadataFalseMetadataReadable)
{
    auto encrypted = td_ / "meta_false.pdf";
    std::filesystem::remove(encrypted);

    // Set metadata on original.
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    metaMgr_->writeField(doc.get(), "info:Title", "SEC-ENCRYPT-META Test");

    // Encrypt with encryptMetadata=false.
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "user123";
    settings.ownerPassword = "owner123";
    settings.encryptMetadata = false;
    encMgr_->saveEncrypted(doc.get(), encrypted, settings);

    // Reopen with password, read metadata.
    auto reopened = Document::open(ctx_, encrypted, "user123");
    ASSERT_NE(reopened, nullptr);

    auto title = metaMgr_->readField(reopened.get(), "info:Title");
    EXPECT_FALSE(title.empty())
        << "SEC-ENCRYPT-META: metadata must be readable when encryptMetadata=false";

    // Verify encryption info reports metadata NOT encrypted.
    auto info = encMgr_->getEncryptionInfo(reopened.get());
    EXPECT_FALSE(info.metadataEncrypted)
        << "encryptMetadata=false must be reported in EncryptionInfo";
}

TEST_F(EncryptMetadataTest, EncryptMetadataTrueDefault)
{
    auto encrypted = td_ / "meta_true.pdf";
    std::filesystem::remove(encrypted);

    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    // Encrypt with defaults (encryptMetadata=true).
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "user123";
    settings.ownerPassword = "owner123";
    encMgr_->saveEncrypted(doc.get(), encrypted, settings);

    auto reopened = Document::open(ctx_, encrypted, "user123");
    ASSERT_NE(reopened, nullptr);

    auto info = encMgr_->getEncryptionInfo(reopened.get());
    EXPECT_TRUE(info.metadataEncrypted)
        << "Default encryptMetadata=true must be reported in EncryptionInfo";
}

// =====================================================================
// SEC-OCSP: OCSP/CRL edge cases via LtvValidator::verifyWith()
// =====================================================================

class OcspEdgeCaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        signedPath_ = buildSignedFixture(ctx_, "ocsp_edge.pdf");
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

TEST_F(OcspEdgeCaseTest, OcspUnknownStatusTreatedAsNonGood)
{
    auto ocspDer = buildOcspDer(key_, cert_, V_OCSP_CERTSTATUS_UNKNOWN);
    ASSERT_FALSE(ocspDer.empty()) << "Failed to build OCSP response";

    auto doc = Document::open(ctx_, signedPath_);
    ASSERT_NE(doc, nullptr);

    auto result = LtvValidator::verifyWith(
        doc.get(), certChain(), {ocspDer}, {}, false);

    EXPECT_NE(result.status, LtvStatus::LtvValidated)
        << "SEC-OCSP: OCSP unknown status must not produce LtvValidated";
}

TEST_F(OcspEdgeCaseTest, StaleOcspDataNotLtvValidated)
{
    // thisUpdate = 30 days ago, nextUpdate = 29 days ago (expired).
    time_t now = time(nullptr);
    auto ocspDer = buildOcspDer(key_, cert_, V_OCSP_CERTSTATUS_GOOD,
                                 now - 30 * 86400,
                                 now - 29 * 86400);
    ASSERT_FALSE(ocspDer.empty()) << "Failed to build stale OCSP response";

    auto doc = Document::open(ctx_, signedPath_);
    ASSERT_NE(doc, nullptr);

    auto result = LtvValidator::verifyWith(
        doc.get(), certChain(), {ocspDer}, {}, false);

    EXPECT_NE(result.status, LtvStatus::LtvValidated)
        << "SEC-OCSP: stale OCSP data (nextUpdate in past) must not "
           "produce LtvValidated";
}

// SEC-9 (phase-48 review): migrated off `test_signer.pfx` and off
// useTrustAnchors=false. See the note where `buildCrlDer()` used to live for
// why the old shape could not fail. This is a plain TEST, not a TEST_F, because
// it no longer uses any member of the `test_signer`-based OcspEdgeCaseTest
// fixture; the name is kept so the rule it pins stays traceable.
TEST(OcspEdgeCaseCrlTest, ExpiredCrlNotLtvValidated)
{
    namespace cc = pdfe_test::crlchain;
    auto dir = cc::chainDir();
    auto ctx = std::make_shared<MuPdfContext>();

    auto signedPath = cc::signPdfWith(ctx, dir / "leaf_crlsign.pfx",
                                      "encrypt_ocsp", "expired_crl.pdf",
                                      "OcspCrlSig");
    ASSERT_FALSE(signedPath.empty()) << "could not sign with leaf_crlsign.pfx";

    X509* interCert = cc::loadDerCert(dir / "inter_crlsign.der");
    ASSERT_NE(interCert, nullptr);
    EVP_PKEY* interKey = cc::loadPemKey(dir / "inter_crlsign_key.pem");
    ASSERT_NE(interKey, nullptr);

    // nextUpdate = yesterday.
    cc::CrlSpec spec;
    spec.thisUpdateOffsetSec = -2 * 86400;
    spec.nextUpdateOffsetSec = -86400;
    auto crlDer = cc::mintCrl(interKey, interCert, spec);
    EVP_PKEY_free(interKey);
    X509_free(interCert);
    ASSERT_FALSE(crlDer.empty()) << "Failed to build expired CRL";

    auto leafDer = cc::readAllBytes(dir / "leaf_crlsign.der");
    auto interDer = cc::readAllBytes(dir / "inter_crlsign.der");
    ASSERT_FALSE(leafDer.empty());
    ASSERT_FALSE(interDer.empty());

    auto rootPem = cc::derToPemFile(dir / "root.der", "encrypt_ocsp",
                                    "expired_crl_root.pem");
    ASSERT_FALSE(rootPem.empty());
    cc::ScopedTrustAnchor anchor(rootPem);
    ASSERT_TRUE(anchor.added);

    auto doc = Document::open(ctx, signedPath);
    ASSERT_NE(doc, nullptr);

    // The chain IS trusted, so LtvValidated is genuinely reachable here — it
    // is what CrlChainFreshnessTest.FreshCrlAccepted gets from the identical
    // setup with a fresh CRL. Only the expiry can move this verdict.
    auto result = LtvValidator::verifyWith(doc.get(), {leafDer, interDer},
                                           {}, {crlDer},
                                           /*useTrustAnchors=*/true);

    // PHASE-50 (F53): was NoDssData. Same rule, same reasoning as
    // CrlChainFreshnessTest.ExpiredCrlRejected — the CRL was there and was
    // refused, which is not the same claim as "there was no CRL".
    EXPECT_EQ(result.status, LtvStatus::RevocationEvidenceRejected)
        << "SEC-OCSP: an expired CRL must be discarded as refused evidence; got "
        << ltvStatusToDisplayString(result.status).toStdString();
    EXPECT_NE(result.status, LtvStatus::LtvValidated)
        << "SEC-OCSP: expired CRL (nextUpdate in past) must not "
           "produce LtvValidated";
}
