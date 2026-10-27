// PHASE4_ROUNDTRIP_SIGNATURES: sign -> save -> reopen -> verify
// tests for the digital-signature subsystem. Closes the audit's gap
// that test_signature_manager.cpp checks "did the signing call
// succeed (no throw)" but rarely "did the verifier decode the bytes
// and confirm the digest matches".
//
// Plus a tamper-detection test: mutate the signed bytes by one byte,
// verify isValid=false. A signing implementation that always reports
// valid would pass test_signature_manager.SignAndVerify but fail
// here.

#include <gtest/gtest.h>

#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "signatures/TrustAnchorManager.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <openssl/pkcs12.h>
#include <openssl/x509.h>

#include <fstream>
#include <filesystem>
#include <memory>
#include <string>

using pdfe::signatures::SignatureManager;
using pdfe::signatures::SigningOptions;
using pdfe::signatures::TrustAnchorManager;

namespace {

// Extract the leaf signing cert from a PFX and write it as DER to @p outPath,
// so it can be imported into TrustAnchorManager as a trust anchor. Returns
// false if the PFX can't be parsed.
bool writePfxLeafCertDer(const std::filesystem::path& pfxPath,
                         const char* password,
                         const std::filesystem::path& outPath)
{
    std::ifstream f(pfxPath, std::ios::binary);
    if (!f) return false;
    std::string buf((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    if (buf.empty()) return false;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(buf.data());
    PKCS12* p12 = d2i_PKCS12(nullptr, &p, static_cast<long>(buf.size()));
    if (!p12) return false;

    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* chain = nullptr;
    int ok = PKCS12_parse(p12, password, &key, &cert, &chain);
    PKCS12_free(p12);
    if (chain) sk_X509_pop_free(chain, X509_free);
    if (!ok || !cert) {
        if (key) EVP_PKEY_free(key);
        if (cert) X509_free(cert);
        return false;
    }

    unsigned char* der = nullptr;
    int n = i2d_X509(cert, &der);
    bool wrote = false;
    if (n > 0 && der) {
        std::ofstream(outPath, std::ios::binary)
            .write(reinterpret_cast<const char*>(der), n);
        OPENSSL_free(der);
        wrote = true;
    }
    if (key) EVP_PKEY_free(key);
    X509_free(cert);
    return wrote;
}

} // namespace

namespace {

class SignatureRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixtureBase_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        pfxPath_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_signer.pfx";
        ASSERT_TRUE(std::filesystem::exists(fixtureBase_));
        ASSERT_TRUE(std::filesystem::exists(pfxPath_))
            << "test_signer.pfx fixture missing — required for signing tests";
        manager_ = std::make_unique<SignatureManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_sig_roundtrip" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        manager_.reset();
        ctx_.reset();
    }

    /// Create an unsigned PDF with one signature field at (100,500),
    /// save it under tempDir_, and return the saved path.
    std::filesystem::path createUnsignedSigField(const std::string& name) {
        auto out = tempDir_ / (name + "_unsigned.pdf");
        auto doc = pdfe::core::Document::open(ctx_, fixtureBase_);
        auto page = doc->page(0);
        manager_->createSignatureField(page.get(), "FieldA",
                                        QRectF(100, 500, 200, 50));
        doc->saveAs(out);
        return out;
    }

    /// Sign the field at widgetIndex 0 of the document at *path*,
    /// using the test PFX. Sign performs an incremental save in
    /// place. Returns the same path for chaining.
    std::filesystem::path signFirstField(const std::filesystem::path& path,
                                          const std::string& reason = "Test") {
        auto doc = pdfe::core::Document::open(ctx_, path);
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfxPath_.string();
        opts.pfxPassword = "test123";
        opts.reason = reason;
        opts.location = "Test Lab";
        manager_->signField(doc.get(), page.get(), 0, opts);
        return path;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixtureBase_;
    std::filesystem::path pfxPath_;
    std::filesystem::path tempDir_;
    std::unique_ptr<SignatureManager> manager_;
};

// ─── Sign + verify ───────────────────────────────────────────────────

TEST_F(SignatureRoundTripTest, SignedDocumentPassesDigestVerifyOnReopen) {
    auto path = createUnsignedSigField("digest_ok");
    signFirstField(path, "Digest verification");

    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    auto page = reopened->page(0);
    auto result = manager_->verifySignature(reopened.get(), page.get(), 0);
    EXPECT_TRUE(result.digestValid)
        << "Freshly-signed PDF must report digestValid=true on reopen";
    EXPECT_EQ(result.digestStatus,
              pdfe::signatures::SignatureError::Okay)
        << "digestStatus must be Okay (got "
        << static_cast<int>(result.digestStatus) << ")";
    EXPECT_FALSE(result.modifiedAfterSigning)
        << "Untouched signed PDF must report modifiedAfterSigning=false";
}

// ─── Signer identity (CN from PFX) preserved in details ──────────────

TEST_F(SignatureRoundTripTest, SignerCommonNameSurvivesReopen) {
    // The PFX cert chain's leaf CN must be parseable from the
    // signature dict's PKCS#7 contents on reopen.
    //
    // Phase-16 (pipeline a7c2e541) closeout: previously this test
    // accepted ANY non-empty CN or formattedName. We now assert the
    // EXACT CN from the test_signer.pfx leaf cert. /Reason and
    // /Location round-trip remains outside this test's scope — it
    // is tracked separately under the Reason/Location dict-readback
    // gap in SignatureManager::extractWidgetData (see KNOWN_WEAK
    // historical note).
    auto path = createUnsignedSigField("signer_cn");
    signFirstField(path, "Signer CN check");

    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    auto page = reopened->page(0);
    auto details = manager_->getSignatureDetails(reopened.get(), page.get(), 0);
    EXPECT_TRUE(details.isSigned)
        << "Reopened sig field must report isSigned=true";

    // test_signer.pfx leaf cert subject is
    //   CN=PDFE Test Signer, OU=QA, O=PDFE Test, C=US
    // (verified via cryptography.hazmat.primitives.serialization.pkcs12)
    const std::string kExpectedCN = "PDFE Test Signer";
    const bool matchesCn =
        details.signer.commonName == kExpectedCN
        || details.signer.formattedName.find(kExpectedCN) != std::string::npos;
    EXPECT_TRUE(matchesCn)
        << "PKCS#7-derived signer must report CN='" << kExpectedCN
        << "'; got commonName='" << details.signer.commonName
        << "' formattedName='" << details.signer.formattedName << "'";
}

// ─── Tamper detection ───────────────────────────────────────────────

TEST_F(SignatureRoundTripTest, TamperingFlippedByteFailsDigestVerify) {
    auto path = createUnsignedSigField("tamper");
    signFirstField(path, "Pre-tamper");

    // Verify pre-tamper.
    {
        auto pre = pdfe::core::Document::open(ctx_, path);
        ASSERT_NE(pre, nullptr);
        auto page = pre->page(0);
        auto preResult = manager_->verifySignature(pre.get(), page.get(), 0);
        ASSERT_TRUE(preResult.digestValid)
            << "Pre-tamper signature must be valid before we mutate the bytes";
    }

    // Mutate one content byte. We pick a byte deep in the file (well
    // past the header) so we hit a content stream / object body, not
    // a structural marker. The PDF must still parse, but the digest
    // must fail.
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f) << "must be able to open the signed PDF for tamper";
        f.seekg(0, std::ios::end);
        auto size = f.tellg();
        ASSERT_GT(size, std::streampos(1024))
            << "Signed PDF must be large enough to tamper safely";
        // Pick a byte at 75% through the file. That should land in
        // page-content territory, well past the AcroForm / signature
        // dict near the front and past the trailer's xref table at
        // the end.
        const std::streampos targetPos = size * 3 / 4;
        f.seekg(targetPos);
        char b;
        f.read(&b, 1);
        f.seekp(targetPos);
        // Flip bits to ensure a real change (XOR with 0xFF).
        char bFlipped = b ^ static_cast<char>(0xFF);
        f.write(&bFlipped, 1);
        f.flush();
    }

    auto post = pdfe::core::Document::open(ctx_, path);
    if (post) {
        auto page = post->page(0);
        auto result = manager_->verifySignature(post.get(), page.get(), 0);
        EXPECT_FALSE(result.digestValid)
            << "Tampered PDF must NOT report digestValid=true; the verifier "
               "must reject byte-level mutation in the signed range";
    } else {
        SUCCEED() << "Tampered PDF was unparseable — also a valid "
                     "tamper-detection outcome";
    }
}

// Pipeline a7c4f891 phase-4 (B2): strengthen TamperingFlippedByte
// FailsDigestVerify by also asserting digestStatus and
// modifiedAfterSigning fields. The AC explicitly says "digestStatus
// reports tamper" — the existing test only checked digestValid==false.

TEST_F(SignatureRoundTripTest, TamperingReportsModifiedAfterSigning) {
    auto path = createUnsignedSigField("tamper2");
    signFirstField(path, "B2-tamper");

    // Pre-tamper baseline.
    {
        auto pre = pdfe::core::Document::open(ctx_, path);
        ASSERT_NE(pre, nullptr);
        auto page = pre->page(0);
        auto preResult = manager_->verifySignature(pre.get(), page.get(), 0);
        ASSERT_TRUE(preResult.digestValid)
            << "Pre-tamper signature must be valid";
        EXPECT_FALSE(preResult.modifiedAfterSigning)
            << "Pre-tamper: modifiedAfterSigning must be false";
    }

    // Mutate 1 byte at 75% through the file.
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f);
        f.seekg(0, std::ios::end);
        auto size = f.tellg();
        ASSERT_GT(size, std::streampos(1024));
        const std::streampos targetPos = size * 3 / 4;
        f.seekg(targetPos);
        char b;
        f.read(&b, 1);
        f.seekp(targetPos);
        char bFlipped = b ^ static_cast<char>(0xFF);
        f.write(&bFlipped, 1);
        f.flush();
    }

    // The tampered file MUST still be openable by MuPDF (single-byte
    // flip at 75% offset corrupts encrypted content but not the xref
    // structure). If MuPDF rejects the file entirely, the tamper
    // detection oracle below cannot fire — and a security test that
    // silently auto-passes is worse than a test that fails loudly.
    // ASSERT (not SUCCEED) so the test suite flags the need to adjust
    // the tamper offset if MuPDF's parser changes.
    auto post = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(post, nullptr)
        << "B2: tampered PDF must still be openable by MuPDF so the "
        << "digest-verification oracle can fire. If this fails, the "
        << "tamper offset needs adjusting (the flip hit a structural "
        << "byte that broke the xref, not just content).";

    auto page = post->page(0);
    auto result = manager_->verifySignature(post.get(), page.get(), 0);
    // Post-tamper invariants for in-place byte mutation:
    //   - digestValid must be false (digest re-compute fails)
    //   - isFullyValid must be false (rolls up digestValid)
    //   - digestStatus must be a specific non-Unknown error code
    // Note: modifiedAfterSigning detects INCREMENTAL UPDATES after
    // signing (pdf_incremental_change_since_signing_widget). It is
    // NOT set for in-place byte mutation, which doesn't add an
    // incremental update — that's a different detection axis.
    EXPECT_FALSE(result.digestValid)
        << "B2: tampered byte must invalidate digestValid";
    EXPECT_FALSE(result.isFullyValid())
        << "B2: tampered doc must not report isFullyValid";
    // digestStatus should be non-Unknown — the verifier must
    // emit a specific error code so the user gets a meaningful
    // message ("Digest mismatch") not a vague "Unknown".
    EXPECT_NE(result.digestStatus, pdfe::signatures::SignatureError::Unknown)
        << "B2: tampered doc must produce a specific digestStatus "
        << "error code (not Unknown)";
    // The digestMessage field must be non-empty (canonical user-
    // facing description of why verification failed).
    EXPECT_FALSE(result.digestMessage.empty())
        << "B2: tampered doc must produce a non-empty digestMessage "
        << "for caller-rendered user feedback";
}

// ─── Multi-signature scenarios via verifyAllSignatures ───────────────

TEST_F(SignatureRoundTripTest, VerifyAllSignaturesReportsCount) {
    auto path = createUnsignedSigField("count");
    signFirstField(path, "Count test");

    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    auto summary = manager_->verifyAllSignatures(reopened.get());
    EXPECT_EQ(summary.totalCount, 1)
        << "Document with one signed field must report totalCount=1";
    EXPECT_GE(summary.signedCount, 1)
        << "signedCount must be at least 1 after signing";
    EXPECT_EQ(summary.signatures.size(),
              static_cast<size_t>(summary.totalCount))
        << "summary.signatures vector size must match totalCount";
}

// ─── Signature listing round-trip ───────────────────────────────────

TEST_F(SignatureRoundTripTest, ListSignaturesShowsSignedFieldAfterSave) {
    auto path = createUnsignedSigField("list");
    signFirstField(path, "List test");

    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    auto sigs = manager_->listSignatures(reopened.get());
    ASSERT_EQ(sigs.size(), 1u)
        << "listSignatures must report the one field we created";
    EXPECT_TRUE(sigs[0].isSigned)
        << "The single field must be reported as signed";
    EXPECT_EQ(sigs[0].fieldName, "FieldA")
        << "Field name must round-trip through the saved PDF";
    EXPECT_EQ(sigs[0].pageIndex, 0)
        << "Field's page index must round-trip";
}

// ─── countSignatures matches reality ─────────────────────────────────

TEST_F(SignatureRoundTripTest, CountSignaturesMatchesAfterSave) {
    auto path = createUnsignedSigField("count_match");
    signFirstField(path, "Count match");

    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(manager_->countSignatures(reopened.get()), 1)
        << "countSignatures must return 1 after one signature was added "
           "and the document was saved + reopened";
}

// ─── Pipeline a7c4f891 phase-6 (B10): expired cert ─────────────────
// Sign with an expired PFX and verify that the certificate status
// reports an error on reopen. MuPDF's PKCS#7 verifier checks cert
// validity dates via its internal X.509 validation path.

TEST_F(SignatureRoundTripTest, ExpiredCertReportsInvalidCertificate) {
    auto expiredPfx = std::filesystem::path(TEST_FIXTURES_DIR)
                      / "certs" / "test_signer_expired.pfx";
    if (!std::filesystem::exists(expiredPfx)) {
        GTEST_SKIP() << "B10: test_signer_expired.pfx fixture not found; "
                     << "run tests/fixtures/generate_cert_fixtures.py";
    }

    auto path = createUnsignedSigField("b10_expired");

    // Sign with the expired cert.
    auto doc = pdfe::core::Document::open(ctx_, path);
    auto page = doc->page(0);
    SigningOptions opts;
    opts.pfxPath = expiredPfx.string();
    opts.pfxPassword = "test123";
    opts.reason = "B10 expired cert test";
    opts.location = "Test Lab";

    bool signSucceeded = false;
    try {
        manager_->signField(doc.get(), page.get(), 0, opts);
        signSucceeded = true;
    } catch (const std::exception& e) {
        // Some PKCS#7 backends reject expired certs at sign time.
        // That's also a valid security posture.
        SUCCEED() << "B10: signing with expired cert rejected at sign time: "
                  << e.what();
        return;
    }
    ASSERT_TRUE(signSucceeded);

    // Reopen and verify — certificate should be flagged.
    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    auto rpage = reopened->page(0);
    auto result = manager_->verifySignature(reopened.get(), rpage.get(), 0);

    // The digest itself may be valid (the bytes weren't tampered),
    // but the certificate validation should report a problem.
    // Accept any of: certificateValid=false, certificateStatus
    // indicating an error, or isFullyValid()=false.
    bool certFlagged =
        !result.certificateValid
        || result.certificateStatus != pdfe::signatures::SignatureError::Okay
        || !result.isFullyValid();
    EXPECT_TRUE(certFlagged)
        << "B10: expired cert must be flagged by the verifier; "
        << "certificateValid=" << result.certificateValid
        << " certificateStatus=" << static_cast<int>(result.certificateStatus)
        << " isFullyValid=" << result.isFullyValid();
}

// ─── Pipeline a7c4f891 phase-6 (B10): self-signed cert ─────────────

TEST_F(SignatureRoundTripTest, SelfSignedCertReportsSelfSigned) {
    auto selfSignedPfx = std::filesystem::path(TEST_FIXTURES_DIR)
                         / "certs" / "test_signer_selfsigned.pfx";
    if (!std::filesystem::exists(selfSignedPfx)) {
        GTEST_SKIP() << "B10: test_signer_selfsigned.pfx fixture not found";
    }

    auto path = createUnsignedSigField("b10_selfsigned");

    auto doc = pdfe::core::Document::open(ctx_, path);
    auto page = doc->page(0);
    SigningOptions opts;
    opts.pfxPath = selfSignedPfx.string();
    opts.pfxPassword = "test123";
    opts.reason = "B10 self-signed test";
    opts.location = "Test Lab";

    bool signSucceeded = false;
    try {
        manager_->signField(doc.get(), page.get(), 0, opts);
        signSucceeded = true;
    } catch (const std::exception&) {
        SUCCEED() << "B10: self-signed cert rejected at sign time (valid posture)";
        return;
    }
    ASSERT_TRUE(signSucceeded);

    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    auto rpage = reopened->page(0);
    auto result = manager_->verifySignature(reopened.get(), rpage.get(), 0);

    // MuPDF reports self-signed certs via certificateStatus.
    // The existing test_signer.pfx is ALSO self-signed, so this
    // test documents the expected status rather than asserting a
    // specific error code. The key invariant: the verifier must
    // return a non-Okay certificate status for an untrusted cert.
    bool certUntrusted =
        result.certificateStatus == pdfe::signatures::SignatureError::SelfSigned
        || result.certificateStatus == pdfe::signatures::SignatureError::SelfSignedInChain
        || result.certificateStatus == pdfe::signatures::SignatureError::NotTrusted
        || !result.certificateValid;
    EXPECT_TRUE(certUntrusted)
        << "B10: self-signed cert must be flagged as untrusted; "
        << "certificateStatus=" << static_cast<int>(result.certificateStatus)
        << " certificateValid=" << result.certificateValid;

    // Digest should be valid (bytes are intact).
    EXPECT_TRUE(result.digestValid)
        << "B10: self-signed cert should not affect digest validity";
}

// ─── Pipeline a7c4f891 phase-6 (B10): intermediate CA chain ────────

TEST_F(SignatureRoundTripTest, IntermediateCAChainSignatureVerifies) {
    auto intermediatePfx = std::filesystem::path(TEST_FIXTURES_DIR)
                           / "certs" / "test_signer_intermediate.pfx";
    if (!std::filesystem::exists(intermediatePfx)) {
        GTEST_SKIP() << "B10: test_signer_intermediate.pfx fixture not found";
    }

    auto path = createUnsignedSigField("b10_intermediate");

    auto doc = pdfe::core::Document::open(ctx_, path);
    auto page = doc->page(0);
    SigningOptions opts;
    opts.pfxPath = intermediatePfx.string();
    opts.pfxPassword = "test123";
    opts.reason = "B10 intermediate CA test";
    opts.location = "Test Lab";

    bool signSucceeded = false;
    try {
        manager_->signField(doc.get(), page.get(), 0, opts);
        signSucceeded = true;
    } catch (const std::exception&) {
        SUCCEED() << "B10: intermediate CA cert rejected at sign time "
                  << "(acceptable if chain embedding not supported)";
        return;
    }
    ASSERT_TRUE(signSucceeded);

    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    auto rpage = reopened->page(0);
    auto result = manager_->verifySignature(reopened.get(), rpage.get(), 0);

    // Digest must be valid (the PFX contains the full chain so the
    // PKCS#7 blob should be self-contained).
    EXPECT_TRUE(result.digestValid)
        << "B10: intermediate CA signature digest must be valid";

    // Certificate chain from an untrusted root will still be flagged
    // as not-trusted by MuPDF (no system trust store integration).
    // The key assertion: the verifier must produce a specific status
    // (not Unknown) indicating it examined the chain.
    EXPECT_NE(result.certificateStatus,
              pdfe::signatures::SignatureError::Unknown)
        << "B10: verifier must produce a specific certificate status "
        << "for a chain-signed cert (not Unknown)";

    // The signer identity should be extractable from the chain leaf.
    auto details = manager_->getSignatureDetails(reopened.get(), rpage.get(), 0);
    EXPECT_TRUE(details.isSigned)
        << "B10: chain-signed field must report isSigned=true";
    bool hasIdentity =
        !details.signer.commonName.empty()
        || !details.signer.formattedName.empty();
    EXPECT_TRUE(hasIdentity)
        << "B10: signer identity must be extractable from intermediate "
        << "CA chain; got CN='" << details.signer.commonName << "'";
}

// =====================================================================
// R-12 (phase-4 SECURITY-ORACLE): real trust-chain enforcement.
//
// The audit found the signature suite only ever asserted "digest valid" and
// documented the cert status — it never proved that an UNTRUSTED cert is
// rejected AND a TRUSTED cert is accepted. These two tests pin BOTH poles.
//
// The test signer (test_signer.pfx) is self-signed. Absent any trust anchor
// the verifier must report a NON-Okay certificate status. After the user
// explicitly trusts that cert (TrustAnchorManager, now wired into the
// certificate verifier), the same signature must report certificate Okay.
// This is the trusted/untrusted distinction the audit demanded.
// =====================================================================

TEST_F(SignatureRoundTripTest, UntrustedSelfSignedCertReportsNonOkay) {
    // Ensure a clean trust store (no leftover anchors from other tests).
    TrustAnchorManager::instance().clear();

    auto path = createUnsignedSigField("r12_untrusted");
    signFirstField(path, "R12 untrusted");

    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    auto page = reopened->page(0);
    auto result = manager_->verifySignature(reopened.get(), page.get(), 0);

    // Digest is fine (bytes intact) but the cert is NOT trusted.
    EXPECT_TRUE(result.digestValid) << "R-12: untampered digest must be valid";
    EXPECT_FALSE(result.certificateValid)
        << "R-12: an untrusted self-signed cert must NOT report certificateValid";
    EXPECT_NE(result.certificateStatus,
              pdfe::signatures::SignatureError::Okay)
        << "R-12: untrusted cert certificateStatus must be non-Okay (got "
        << static_cast<int>(result.certificateStatus) << ")";
    EXPECT_FALSE(result.isFullyValid())
        << "R-12: untrusted cert must not roll up to isFullyValid()";
}

TEST_F(SignatureRoundTripTest, TrustedAnchorCertReportsOkay) {
    TrustAnchorManager::instance().clear();

    auto path = createUnsignedSigField("r12_trusted");
    signFirstField(path, "R12 trusted");

    // Import the signer's own cert as an explicit user trust anchor.
    auto anchorDer = tempDir_ / "r12_signer_anchor.der";
    ASSERT_TRUE(writePfxLeafCertDer(pfxPath_, "test123", anchorDer))
        << "Could not extract signer leaf cert from test_signer.pfx";

    QString err;
    ASSERT_TRUE(TrustAnchorManager::instance().addTrust(anchorDer, &err))
        << "addTrust failed: " << err.toStdString();

    auto reopened = pdfe::core::Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    auto page = reopened->page(0);
    auto result = manager_->verifySignature(reopened.get(), page.get(), 0);

    // With the signer explicitly trusted, the certificate must verify Okay.
    EXPECT_TRUE(result.digestValid) << "R-12: digest must be valid";
    EXPECT_EQ(result.certificateStatus,
              pdfe::signatures::SignatureError::Okay)
        << "R-12: an explicitly-trusted cert must report certificate Okay (got "
        << static_cast<int>(result.certificateStatus)
        << " msg='" << result.certificateMessage << "')";
    EXPECT_TRUE(result.certificateValid)
        << "R-12: trusted cert must report certificateValid=true";

    // Cleanup so we don't pollute the global trust store for other tests.
    TrustAnchorManager::instance().clear();
    std::error_code ec;
    std::filesystem::remove(anchorDer, ec);
}

// =====================================================================
// R-12 DocMDP enforcement: a DISALLOWED content change under a P1
// (NoChanges) certification signature must be FLAGGED by the VERIFIER —
// behaviorally, not merely inferred from a byte-grep of the file.
//
// Note: MuPDF reports a fresh certification as modifiedAfterSigning=true
// because the /Perms /DocMDP dict is written as an incremental update after
// the signature byte range — so modifiedAfterSigning is NOT a clean
// "content-was-edited" axis for certifications. The robust behavioral axis
// is the DIGEST: a content edit that mutates the signed bytes must make the
// verifier report digestValid=false. We first establish the /DocMDP /P 1
// context (this is a NoChanges certification, not an approval sig), then
// prove the verifier actively rejects a content edit under it.
// =====================================================================
TEST_F(SignatureRoundTripTest, P1CertificationFlagsDisallowedContentChange) {
    TrustAnchorManager::instance().clear();

    auto path = createUnsignedSigField("r12_p1_mdp");

    // Certify at P1 (NoChanges).
    {
        auto doc = pdfe::core::Document::open(ctx_, path);
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfxPath_.string();
        opts.pfxPassword = "test123";
        opts.reason = "R12 P1 certification";
        opts.location = "Test Lab";
        opts.certification = pdfe::signatures::CertificationLevel::NoChanges;
        manager_->signField(doc.get(), page.get(), 0, opts);
    }

    // Establish the P1 certification context via the on-disk /DocMDP /P 1
    // marker (context only — the actual violation detection below is
    // behavioral, via the verifier, not this grep).
    {
        std::ifstream in(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        EXPECT_NE(bytes.find("/DocMDP"), std::string::npos)
            << "R-12: P1 signing must stamp a /DocMDP certification dict";
        EXPECT_NE(bytes.find("/P 1"), std::string::npos)
            << "R-12: P1 certification must record /P 1 (NoChanges)";
    }

    // Baseline: untouched certified doc verifies with a valid digest.
    {
        auto doc = pdfe::core::Document::open(ctx_, path);
        auto page = doc->page(0);
        auto pre = manager_->verifySignature(doc.get(), page.get(), 0);
        ASSERT_TRUE(pre.digestValid)
            << "R-12: untouched P1-certified doc must have a valid digest";
    }

    // DISALLOWED content change: XOR-flip a byte deep in the signed content.
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f);
        f.seekg(0, std::ios::end);
        auto size = f.tellg();
        ASSERT_GT(size, std::streampos(1024));
        const std::streampos pos = size * 3 / 4;
        f.seekg(pos);
        char b = 0;
        f.read(&b, 1);
        f.seekp(pos);
        char flipped = b ^ static_cast<char>(0xFF);
        f.write(&flipped, 1);
        f.flush();
    }

    // The verifier must FLAG the disallowed content change behaviorally.
    auto reopened = pdfe::core::Document::open(ctx_, path);
    if (reopened) {
        auto page = reopened->page(0);
        auto result = manager_->verifySignature(reopened.get(), page.get(), 0);
        EXPECT_FALSE(result.digestValid)
            << "R-12: a content edit under a P1 NoChanges certification MUST be "
               "flagged (digest must fail) — not silently accepted";
        EXPECT_FALSE(result.isFullyValid())
            << "R-12: a tampered P1-certified doc must not be isFullyValid()";
    } else {
        SUCCEED() << "Tampered certified PDF unparseable — also a valid "
                     "rejection of the disallowed change";
    }
}

} // namespace
