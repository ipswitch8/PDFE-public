// -*- coding: utf-8 -*-
// Pipeline F phase-10: offline LTV validator + persistent trust anchors.
//
// AC test plan (phase-10 ACs 5.1-5.5):
//   (1) VerifiesLtvDocumentWithDss: signed PDF + embedded /DSS (cert +
//       OCSP good) → LtvValidated.
//   (2) NoDssDataReturnsNoDssStatus: signed PDF without /DSS →
//       UntrustedChain for the self-signed test signer (or OnlineValidated,
//       depending on OpenSSL's default cert paths; never the Windows store,
//       which PDFE does not read — GitHub #212). Phase-50 F52 corrected this from
//       NoDssData; see the test's own header for why the old expectation
//       encoded the defect.
//   (3) RevokedCertReturnsRevoked: /DSS with OCSP status=revoked →
//       Revoked. Synthesizes an OCSP_RESPONSE with V_OCSP_CERTSTATUS_REVOKED.
//   (4) TrustAnchorRoundTrip: addTrust → listTrusts → removeTrust round-trip.
//   (5) CustomTrustAnchorEnablesValidation: a self-signed cert added to
//       TrustAnchorManager makes its chain validate via verifyWith().
//
// QSettings isolation: every test that touches TrustAnchorManager calls
// clear() in TearDown. Tests run after the Google Test main() that calls
// QCoreApplication::setOrganizationName("PDFE") + setApplicationName("PDFE"),
// so QSettings reads/writes go to PDFE/PDFE — same store the app uses,
// but clear() drops the "TrustAnchors" group at the end.
//
// Runtime: the LtvValidator + LtvDssWriter SetUpTestSuite signs once per
// test program run. Subsequent tests workCopy() the signed source so we
// don't pay PKCS#12 + RSA on every TEST_F.

#include <gtest/gtest.h>

#include "signatures/DssReader.h"
#include "signatures/LtvDssWriter.h"
#include "signatures/LtvValidator.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"
#include "signatures/TrustAnchorManager.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QRectF>
#include <QRegularExpression>
#include <QString>

#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>     // EVP_RSA_gen macro
#include <openssl/x509.h>

#include <cstdint>
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

std::filesystem::path tempWorkDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_ltv_validator";
    std::filesystem::create_directories(d);
    return d;
}

// ---------------------------------------------------------------------------
// Signing fixture — mirrors test_ltv_dss_writer.cpp's pattern. We sign one
// PDF per test suite and copy it into per-test working files.
// ---------------------------------------------------------------------------
bool buildSignedFixture(std::shared_ptr<MuPdfContext> ctx,
                        const std::filesystem::path& outPath)
{
    auto fixtures = std::filesystem::path(fixturesDir());
    auto pfx = fixtures / "test_signer.pfx";
    auto srcPdf = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(srcPdf))
        return false;

    SignatureManager mgr(ctx);

    {
        auto doc = Document::open(ctx, srcPdf);
        if (!doc) return false;
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "LtvSig",
                                  QRectF(100, 500, 200, 50));
        std::filesystem::remove(outPath);
        doc->saveAs(outPath);
    }
    {
        auto doc = Document::open(ctx, outPath);
        if (!doc) return false;
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfx.string();
        opts.pfxPassword = "test123";
        opts.reason = "LTV validator test";
        opts.location = "PDFE test suite";
        mgr.signField(doc.get(), page.get(), 0, opts);
    }
    return true;
}

// Load test signer (cert + key) from the bundled PFX.
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

// Generic "build OCSP response with status X" — same machinery as
// test_validation_request.cpp / test_ocsp_crl_parser.cpp, but
// parameterized over the cert status so we can fabricate revoked
// responses for AC test 3.
std::vector<uint8_t> buildOcspResponseDerWithStatus(
    EVP_PKEY* key, X509* cert, int certStatus)
{
    std::vector<uint8_t> result;
    if (!key || !cert) return result;

    OCSP_BASICRESP* basic = OCSP_BASICRESP_new();
    if (!basic) return result;

    OCSP_CERTID* certId = OCSP_cert_to_id(EVP_sha1(), cert, cert);
    if (!certId) { OCSP_BASICRESP_free(basic); return result; }

    ASN1_TIME* thisUpd = ASN1_TIME_set(nullptr, time(nullptr));
    ASN1_TIME* nextUpd = ASN1_TIME_set(nullptr, time(nullptr) + 86400);
    if (!thisUpd || !nextUpd) {
        if (thisUpd) ASN1_TIME_free(thisUpd);
        if (nextUpd) ASN1_TIME_free(nextUpd);
        OCSP_CERTID_free(certId); OCSP_BASICRESP_free(basic);
        return result;
    }

    // For revoked status we also need a revocation date.
    ASN1_TIME* revokedAt = nullptr;
    int reason = 0;
    if (certStatus == V_OCSP_CERTSTATUS_REVOKED) {
        revokedAt = ASN1_TIME_set(nullptr, time(nullptr) - 3600);
        reason = OCSP_REVOKED_STATUS_KEYCOMPROMISE;
    }

    OCSP_SINGLERESP* single = OCSP_basic_add1_status(
        basic, certId, certStatus, reason, revokedAt, thisUpd, nextUpd);
    OCSP_CERTID_free(certId);
    ASN1_TIME_free(thisUpd);
    ASN1_TIME_free(nextUpd);
    if (revokedAt) ASN1_TIME_free(revokedAt);
    if (!single) { OCSP_BASICRESP_free(basic); return result; }

    if (OCSP_basic_sign(basic, cert, key, EVP_sha256(), nullptr, 0) != 1) {
        OCSP_BASICRESP_free(basic);
        return result;
    }

    OCSP_RESPONSE* resp = OCSP_response_create(
        OCSP_RESPONSE_STATUS_SUCCESSFUL, basic);
    OCSP_BASICRESP_free(basic);
    if (!resp) return result;

    unsigned char* der = nullptr;
    int derLen = i2d_OCSP_RESPONSE(resp, &der);
    OCSP_RESPONSE_free(resp);
    if (derLen <= 0 || !der) {
        if (der) OPENSSL_free(der);
        return result;
    }
    result.assign(der, der + derLen);
    OPENSSL_free(der);
    return result;
}

// Generate a tiny self-signed cert (RSA-2048, 30-day validity) and write
// the DER bytes to @p outPath. Used by the trust-anchor round-trip test
// so we don't depend on the bundled PFX (which is used by signing tests
// and would race the global TrustAnchorManager state if reused here).
bool writeSelfSignedCertDer(const std::filesystem::path& outPath,
                             EVP_PKEY** outKey = nullptr,
                             X509** outCert = nullptr)
{
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (!pkey) return false;

    X509* cert = X509_new();
    if (!cert) { EVP_PKEY_free(pkey); return false; }

    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60 * 60 * 24 * 30);
    X509_set_pubkey(cert, pkey);

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("PDFE Phase-10 Trust Anchor"),
        -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("PDFE Tests"), -1, -1, 0);
    X509_set_issuer_name(cert, name);

    if (X509_sign(cert, pkey, EVP_sha256()) <= 0) {
        X509_free(cert); EVP_PKEY_free(pkey);
        return false;
    }

    unsigned char* der = nullptr;
    int derLen = i2d_X509(cert, &der);
    if (derLen <= 0 || !der) {
        if (der) OPENSSL_free(der);
        X509_free(cert); EVP_PKEY_free(pkey);
        return false;
    }

    {
        std::ofstream f(outPath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(der), derLen);
    }
    OPENSSL_free(der);

    if (outKey && outCert) {
        *outKey = pkey;
        *outCert = cert;
    } else {
        X509_free(cert);
        EVP_PKEY_free(pkey);
    }
    return true;
}

// Pipeline F phase-10: cleanup helper. Wipe TrustAnchorManager state both
// in-memory and on disk. Tests call this in SetUp + TearDown so they
// don't pollute the user's actual QSettings store.
void resetTrustAnchors()
{
    TrustAnchorManager::instance().clear();
}

} // anonymous namespace

// ===========================================================================
// LtvValidator tests — drive the offline verification pipeline against a
// real signed PDF.
// ===========================================================================

class LtvValidatorTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        sigCtx_ = std::make_shared<MuPdfContext>();
        signedFixture_ = tempWorkDir() / "ltv_validator_signed.pdf";
        try {
            if (!buildSignedFixture(sigCtx_, signedFixture_))
                signedFixture_.clear();
        } catch (const std::exception&) {
            signedFixture_.clear();
        }
    }

    static void TearDownTestSuite() {
        std::error_code ec;
        if (!signedFixture_.empty())
            std::filesystem::remove(signedFixture_, ec);
        sigCtx_.reset();
    }

    void SetUp() override {
        if (signedFixture_.empty() ||
            !std::filesystem::exists(signedFixture_)) {
            GTEST_SKIP() << "Could not produce signed fixture; skipping.";
        }
        ctx = sigCtx_;
        // Ensure no leftover trust anchors from previous tests.
        resetTrustAnchors();
    }

    void TearDown() override {
        resetTrustAnchors();
    }

    std::filesystem::path workCopy(const char* outName) {
        auto dst = tempWorkDir() / outName;
        std::filesystem::remove(dst);
        std::filesystem::copy_file(signedFixture_, dst);
        return dst;
    }

    std::shared_ptr<MuPdfContext> ctx;

    static std::shared_ptr<MuPdfContext> sigCtx_;
    static std::filesystem::path signedFixture_;
};

std::shared_ptr<MuPdfContext> LtvValidatorTest::sigCtx_;
std::filesystem::path LtvValidatorTest::signedFixture_;

// -- AC 5.1: signed PDF + embedded /DSS (cert + good OCSP) → LtvValidated.
// ===========================================================================
// CLASS-LEVEL (GitHub #212/#233). The two message guards below each pin ONE
// code path. This pins the whole display-string surface at once.
//
// ltvStatusToDisplayString() is pure over the enum, so every panel string a
// user can ever see is reachable from here without constructing a document.
// Two defects in this family were user-facing strings naming the Windows
// certificate store, which PDFE does not read; both were found by hand, and
// the second was missed by a literal search for the first. A per-path
// assertion cannot close that -- a new status with a new bad string would be
// guarded by nothing.
//
// Values are swept by INTEGER rather than from a hand-written list, so an
// enum member added tomorrow is covered without anyone remembering to update
// this test. That is the difference between a control and a checklist.
TEST(LtvDisplayStrings, NoStatusStringNamesATrustSourcePdfeDoesNotConsult)
{
    const QRegularExpression storeClaim(
        R"(system\s+(trust\s+)?store|windows\s+(certificate\s+)?store)",
        QRegularExpression::CaseInsensitiveOption);

    int realStatuses = 0;
    for (int raw = 0; raw < 64; ++raw) {
        const QString s =
            ltvStatusToDisplayString(static_cast<LtvStatus>(raw));
        // Out-of-range values fall through to "Unknown"; that is the switch's
        // documented default and carries no claim, so it is not a member.
        if (s == QStringLiteral("Unknown"))
            continue;
        ++realStatuses;

        EXPECT_FALSE(s.isEmpty())
            << "Status " << raw << " produced an empty panel string.";
        EXPECT_FALSE(s.contains(storeClaim))
            << "ltvStatusToDisplayString(" << raw << ") = '"
            << s.toStdString()
            << "' names a trust store PDFE does not consult (GitHub #212).";
    }

    // Non-vacuity, and the reason it is needed: if the cast ever stopped
    // producing real statuses -- an enum renumbering, say -- the loop above
    // would sweep 64 "Unknown"s and assert nothing while staying green.
    EXPECT_GE(realStatuses, 8)
        << "Swept " << realStatuses << " real statuses; expected at least 8. "
           "The sweep is not reaching the enum, so the assertions above "
           "measured nothing.";
}

TEST_F(LtvValidatorTest, VerifiesLtvDocumentWithDss)
{
    auto work = workCopy("ltv_validates.pdf");

    // Sign + embed /DSS with cert + good-status OCSP response.
    SignatureInputs inputs;
    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(LtvDssWriter::extractFirstSignatureInputs(doc.get(), inputs));
    }

    // Build a "good" OCSP response signed by the test signer (self-issued
    // is fine for our purposes — verifyWith() doesn't gate on OCSP signer
    // trust, only the certStatus value).
    EVP_PKEY* key = nullptr; X509* cert = nullptr;
    ASSERT_TRUE(loadTestSigner(key, cert));
    auto ocspDer = buildOcspResponseDerWithStatus(key, cert,
                                                    V_OCSP_CERTSTATUS_GOOD);
    ASSERT_FALSE(ocspDer.empty());
    EVP_PKEY_free(key); X509_free(cert);

    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        LtvDssWriter writer;
        ASSERT_TRUE(writer.write(doc.get(), inputs.contentsHex,
                                  inputs.certChain,
                                  /*ocsps=*/{ocspDer}));
    }

    // Re-open and verify. The test signer is self-signed — its root is not
    // among PDFE's trust sources (the user's anchors and OpenSSL's default
    // certificate paths; never the Windows store, GitHub #212), so we must
    // explicitly trust it before
    // calling verify(). With the phase-10 security fix (/DSS certs are
    // intermediates only, never trust anchors), this is required to
    // reach LtvValidated. Without TrustAnchorManager, the verdict would
    // be UntrustedChain — which is the correct behaviour for a doc whose
    // signer chain doesn't terminate at a known root.
    auto pfxDer = std::filesystem::path(fixturesDir())
                  / "test_signer_root.der";
    {
        // Write the test signer cert to a temp .der file so addTrust()
        // can ingest it.
        EVP_PKEY* k = nullptr; X509* c = nullptr;
        ASSERT_TRUE(loadTestSigner(k, c));
        unsigned char* der = nullptr;
        int n = i2d_X509(c, &der);
        ASSERT_GT(n, 0);
        std::ofstream(pfxDer, std::ios::binary)
            .write(reinterpret_cast<const char*>(der), n);
        OPENSSL_free(der);
        EVP_PKEY_free(k); X509_free(c);
    }
    QString trustErr;
    ASSERT_TRUE(TrustAnchorManager::instance().addTrust(pfxDer, &trustErr))
        << "Trust anchor add failed: " << trustErr.toStdString();

    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    auto result = LtvValidator::verify(doc.get());
    EXPECT_EQ(result.status, LtvStatus::LtvValidated)
        << "Expected LtvValidated; got " << static_cast<int>(result.status)
        << " message=" << result.message.toStdString();

    // GitHub #212/#233, the /DSS arm. Same guard as the no-/DSS test: this
    // function's sibling branch used to emit "Validated against system trust
    // store", a false ASSURANCE that the OS had vouched for the certificate.
    ASSERT_FALSE(result.message.isEmpty())
        << "No message to inspect — the assertion below would be vacuous.";
    EXPECT_FALSE(result.message.contains(
        QRegularExpression(R"(system\s+(trust\s+)?store)",
                           QRegularExpression::CaseInsensitiveOption)))
        << "User-facing LTV message claims a system trust store was "
           "consulted; PDFE does not consult one. Got: "
        << result.message.toStdString();

    std::error_code ec;
    std::filesystem::remove(pfxDer, ec);
}

// -- AC 5.2: no /DSS dict.
//
// PHASE-50 (F52): THIS TEST ENCODED THE DEFECT. It asserted NoDssData for a
// SELF-SIGNED signer that reaches no trust anchor, and
// ltvStatusToDisplayString(NoDssData) is "Validated (no revocation data)" —
// so the assertion was pinning the panel saying **Validated** about a
// certificate nothing vouches for. Its own comment named the state as
// "validated by digest, no revocation data, no trusted root", which is three
// facts collapsed into one word.
//
// verify() no longer rewrites UntrustedChain -> NoDssData, so the honest
// verdict for this fixture is UntrustedChain. OnlineValidated remains
// acceptable for the rare host whose OpenSSL cert directory really does
// accept the cert.
// The added display-string assertion is what makes the difference visible:
// it is the F52 acceptance criterion applied to this fixture.
TEST_F(LtvValidatorTest, NoDssDataReturnsNoDssStatus)
{
    auto work = workCopy("ltv_no_dss.pdf");

    // Sanity: confirm /DSS is NOT present on the freshly-signed source.
    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        DssEmbedded dss;
        DssReader::readDssEmbedded(doc.get(), dss);
        ASSERT_FALSE(dss.hasDss);
    }

    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);
    auto result = LtvValidator::verify(doc.get());

    // Self-signed test cert won't chain through the user's anchors or
    // OpenSSL's default certificate paths -- the only two trust sources PDFE
    // has (the Windows certificate store is never consulted; GitHub #212) --
    // so the honest verdict is UntrustedChain. If the host's OpenSSL cert
    // directory unexpectedly accepts the cert (rare), OnlineValidated is
    // also acceptable.
    EXPECT_TRUE(result.status == LtvStatus::UntrustedChain ||
                result.status == LtvStatus::OnlineValidated)
        << "Expected UntrustedChain or OnlineValidated; got "
        << static_cast<int>(result.status)
        << " message=" << result.message.toStdString();
    if (result.status == LtvStatus::UntrustedChain) {
        EXPECT_FALSE(ltvStatusToDisplayString(result.status)
                         .contains("Validated", Qt::CaseInsensitive))
            << "F52: an untrusted self-signed signer with no /DSS must not "
               "produce a panel string containing the word 'Validated'; got "
            << ltvStatusToDisplayString(result.status).toStdString();
        // And positively: this arm's message states the recorded decision, so
        // a user seeing a Windows-trusted certificate reported untrusted is
        // told why rather than left to read it as a defect (GitHub #212).
        EXPECT_TRUE(result.message.contains("does not consult",
                                            Qt::CaseInsensitive))
            << "UntrustedChain message should say the Windows store is not "
               "consulted; got "
            << result.message.toStdString();
    }
    // GitHub #212/#233. The user-facing string must not tell the user their
    // certificate was weighed against the Windows certificate store, because
    // PDFE never reads it. This is asserted UNCONDITIONALLY, over whichever
    // of the two acceptable arms ran, so it cannot go vacuous on a host that
    // produces the other verdict -- an `if (UntrustedChain)` guard here would
    // silently stop testing anything on such a host.
    ASSERT_FALSE(result.message.isEmpty())
        << "No message to inspect — the assertion below would be vacuous.";
    // Matched by PATTERN, not by the one literal that was wrong first. A
    // plain contains("system store") check misses "Validated against system
    // TRUST store" -- which is a real string this file used to produce on the
    // sibling code path, and a false ASSURANCE rather than a false denial.
    EXPECT_FALSE(result.message.contains(
        QRegularExpression(R"(system\s+(trust\s+)?store)",
                           QRegularExpression::CaseInsensitiveOption)))
        << "User-facing LTV message claims a system trust store was "
           "consulted; PDFE does not consult one. Got: "
        << result.message.toStdString();

    EXPECT_NE(result.status, LtvStatus::LtvValidated)
        << "No /DSS but reported LtvValidated — must not.";
    EXPECT_NE(result.status, LtvStatus::InvalidSignature)
        << "Digest should still verify on a freshly-signed PDF.";
}

// -- AC 5.3: /DSS with revoked OCSP → Revoked. Synthesize an OCSP
// response carrying V_OCSP_CERTSTATUS_REVOKED and feed it through
// verifyWith() (the seam tests use to skip /DSS embedding overhead and
// exercise the revocation classifier directly).
TEST_F(LtvValidatorTest, RevokedCertReturnsRevoked)
{
    auto work = workCopy("ltv_revoked.pdf");

    SignatureInputs inputs;
    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(LtvDssWriter::extractFirstSignatureInputs(doc.get(), inputs));
    }

    EVP_PKEY* key = nullptr; X509* cert = nullptr;
    ASSERT_TRUE(loadTestSigner(key, cert));
    auto ocspDer = buildOcspResponseDerWithStatus(key, cert,
                                                    V_OCSP_CERTSTATUS_REVOKED);
    ASSERT_FALSE(ocspDer.empty());
    EVP_PKEY_free(key); X509_free(cert);

    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    // Use verifyWith() so we don't need to round-trip through LtvDssWriter
    // first — simpler, and exercises the same revocation-classification
    // code path.
    auto result = LtvValidator::verifyWith(doc.get(),
                                            inputs.certChain,
                                            /*ocsps=*/{ocspDer},
                                            /*crls=*/{},
                                            /*useTrustAnchors=*/false);
    EXPECT_EQ(result.status, LtvStatus::Revoked)
        << "Expected Revoked; got " << static_cast<int>(result.status)
        << " message=" << result.message.toStdString();
}

// ===========================================================================
// TrustAnchorManager tests — persistence + OpenSSL store integration.
// ===========================================================================

class TrustAnchorManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Always start clean. QSettings is global so tests must be
        // hygienic — see file header.
        resetTrustAnchors();
    }

    void TearDown() override {
        resetTrustAnchors();
    }
};

// -- AC 5.4: addTrust → listTrusts contains the cert → removeTrust →
// listTrusts is empty.
TEST_F(TrustAnchorManagerTest, TrustAnchorRoundTrip)
{
    auto certPath = tempWorkDir() / "trust_anchor_roundtrip.der";
    ASSERT_TRUE(writeSelfSignedCertDer(certPath));

    auto& mgr = TrustAnchorManager::instance();

    // Pre-condition: list is empty.
    EXPECT_TRUE(mgr.listTrusts().isEmpty());

    QString err;
    ASSERT_TRUE(mgr.addTrust(certPath, &err))
        << "addTrust failed: " << err.toStdString();

    auto list = mgr.listTrusts();
    ASSERT_EQ(list.size(), 1);
    EXPECT_FALSE(list.first().fingerprintSha256.isEmpty());
    EXPECT_EQ(list.first().fingerprintSha256.size(), 64); // 256-bit hex.
    EXPECT_FALSE(list.first().subjectCN.isEmpty());

    // Pull fingerprint, removeTrust by fingerprint.
    const QString fp = list.first().fingerprintSha256;
    EXPECT_TRUE(mgr.removeTrust(fp));
    EXPECT_TRUE(mgr.listTrusts().isEmpty());

    // Removing again should report not-found.
    EXPECT_FALSE(mgr.removeTrust(fp));

    std::filesystem::remove(certPath);
}

// -- AC 5.5: a custom trust anchor loaded into the OpenSSL store enables
// chain validation against it.
//
// Strategy: generate a self-signed cert, add it to TrustAnchorManager,
// call loadIntoStore(), then run X509_verify_cert against the same cert.
// A self-signed cert that lives in the trust store must chain to itself
// successfully — which proves loadIntoStore() actually delivered the
// cert into the OpenSSL store.
//
// We don't drive a full LtvValidator::verifyWith() through this path
// because that would require a signed PDF whose signer chain terminates
// at our generated anchor — too much PKI plumbing for what the AC is
// asking ("custom trust anchor loaded into store and chain validates
// against it").
TEST_F(TrustAnchorManagerTest, CustomTrustAnchorEnablesValidation)
{
    // Step 1: produce a self-signed trust-anchor cert.
    EVP_PKEY* anchorKey = nullptr; X509* anchorCert = nullptr;
    auto certDerPath = tempWorkDir() / "trust_anchor_validate.der";
    ASSERT_TRUE(writeSelfSignedCertDer(certDerPath, &anchorKey, &anchorCert));

    // Step 2: import the cert into TrustAnchorManager.
    auto& mgr = TrustAnchorManager::instance();
    QString err;
    ASSERT_TRUE(mgr.addTrust(certDerPath, &err))
        << "addTrust failed: " << err.toStdString();
    ASSERT_EQ(mgr.listTrusts().size(), 1);

    // Step 3: feed the trust list into a fresh X509_STORE and verify.
    X509_STORE* store = X509_STORE_new();
    ASSERT_NE(store, nullptr);
    mgr.loadIntoStore(store);

    X509_STORE_CTX* sc = X509_STORE_CTX_new();
    ASSERT_NE(sc, nullptr);
    ASSERT_EQ(X509_STORE_CTX_init(sc, store, anchorCert, nullptr), 1);
    int rc = X509_verify_cert(sc);
    EXPECT_EQ(rc, 1)
        << "Custom trust anchor should validate its own self-signed cert. "
        << "X509_STORE_CTX_get_error="
        << X509_verify_cert_error_string(X509_STORE_CTX_get_error(sc));
    X509_STORE_CTX_free(sc);
    X509_STORE_free(store);

    // Sanity: a fresh store WITHOUT the trust anchor must NOT validate
    // (this disambiguates "loadIntoStore worked" from "the X509_STORE
    // happened to contain this cert already").
    X509_STORE* emptyStore = X509_STORE_new();
    ASSERT_NE(emptyStore, nullptr);
    X509_STORE_CTX* sc2 = X509_STORE_CTX_new();
    ASSERT_NE(sc2, nullptr);
    ASSERT_EQ(X509_STORE_CTX_init(sc2, emptyStore, anchorCert, nullptr), 1);
    int rc2 = X509_verify_cert(sc2);
    EXPECT_NE(rc2, 1)
        << "Anchor cert should NOT validate against an empty store — "
        << "if it does, the test isn't actually proving loadIntoStore.";
    X509_STORE_CTX_free(sc2);
    X509_STORE_free(emptyStore);

    // Cleanup.
    EVP_PKEY_free(anchorKey);
    X509_free(anchorCert);
    std::filesystem::remove(certDerPath);
}
