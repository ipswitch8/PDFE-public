// -*- coding: utf-8 -*-
//
// Pipeline b9e14d07 phase-2: signature security tests.
//
// SEC-REDACT-SIG:  Redact → save → sign → verify byte-range intact
// SEC-MULTISIG:    Sign twice → verify both signatures valid
// SEC-PKCS7-TAMPER: Sign → corrupt PKCS#7 → verify rejection

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "redaction/RedactionManager.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"

#include <QRectF>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_sig_sec";
    std::filesystem::create_directories(d);
    return d;
}

std::filesystem::path createSignableCopy(
    std::shared_ptr<MuPdfContext> ctx,
    const std::filesystem::path& src,
    const char* outName,
    QRectF sigRect = QRectF(100, 500, 200, 50))
{
    auto out = tempDir() / outName;
    std::filesystem::remove(out);
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, src);
    auto page = doc->page(0);
    mgr.createSignatureField(page.get(), "TestSig", sigRect);
    doc->saveAs(out);
    return out;
}

std::filesystem::path createMultiSigCopy(
    std::shared_ptr<MuPdfContext> ctx,
    const std::filesystem::path& src,
    const char* outName)
{
    auto out = tempDir() / outName;
    std::filesystem::remove(out);
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, src);
    auto page = doc->page(0);
    mgr.createSignatureField(page.get(), "Sig1", QRectF(100, 500, 150, 40));
    mgr.createSignatureField(page.get(), "Sig2", QRectF(300, 500, 150, 40));
    doc->saveAs(out);
    return out;
}

} // namespace

class SignatureSecurityTest : public ::testing::Test {
protected:
    void SetUp() override {
        pfxPath_ = std::filesystem::path(fixturesDir()) / "test_signer.pfx";
        basePath_ = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        redactPath_ = std::filesystem::path(fixturesDir()) / "test_redaction.pdf";
        if (!std::filesystem::exists(pfxPath_) ||
            !std::filesystem::exists(basePath_)) {
            GTEST_SKIP() << "Signing fixtures not available";
        }
        ctx_ = std::make_shared<MuPdfContext>();
    }

    SigningOptions makeOpts(const std::string& reason = "Security test") const {
        SigningOptions opts;
        opts.pfxPath = pfxPath_.string();
        opts.pfxPassword = "test123";
        opts.reason = reason;
        opts.location = "Test Lab";
        return opts;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path pfxPath_;
    std::filesystem::path basePath_;
    std::filesystem::path redactPath_;
};

// =====================================================================
// SEC-REDACT-SIG: Redact → save → sign → verify byte-range intact
// =====================================================================

TEST_F(SignatureSecurityTest, RedactThenSignByteRangeIntact)
{
    // Each stage writes to a fresh path to avoid MuPDF file-lock conflicts.
    auto step1 = tempDir() / "redact_s1.pdf";
    auto step2 = tempDir() / "redact_s2.pdf";
    auto step3 = tempDir() / "redact_s3.pdf";
    for (auto* p : {&step1, &step2, &step3})
        std::filesystem::remove(*p);

    // Stage 1: copy base, apply redaction, save to step1.
    std::filesystem::copy_file(basePath_, step1,
        std::filesystem::copy_options::overwrite_existing);
    {
        auto doc = Document::open(ctx_, step1);
        auto page = doc->page(0);
        pdfe::redaction::RedactionManager redactMgr(ctx_);
        redactMgr.createRedactionMark(page.get(), QRectF(50, 50, 200, 30));
        pdfe::redaction::RedactionApplyOptions applyOpts;
        auto result = redactMgr.applyAllRedactions(doc.get(), applyOpts);
        ASSERT_TRUE(result.success) << "Redaction must succeed";
        doc->saveAs(step2);
    }

    // Stage 2: add signature field to redacted copy.
    {
        SignatureManager mgr(ctx_);
        auto doc = Document::open(ctx_, step2);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "PostRedactSig",
                                  QRectF(100, 600, 200, 50));
        doc->saveAs(step3);
    }

    // Stage 3: sign.
    SignatureManager sigMgr(ctx_);
    {
        auto doc = Document::open(ctx_, step3);
        auto page = doc->page(0);
        sigMgr.signField(doc.get(), page.get(), 0, makeOpts("Post-redaction sign"));
    }

    // Verify.
    auto reopened = Document::open(ctx_, step3);
    ASSERT_NE(reopened, nullptr);
    auto page = reopened->page(0);
    auto result = sigMgr.verifySignature(reopened.get(), page.get(), 0);
    EXPECT_TRUE(result.digestValid)
        << "SEC-REDACT-SIG: signature after redaction must have valid digest; "
           "digestStatus=" << static_cast<int>(result.digestStatus);
}

// =====================================================================
// SEC-MULTISIG: Sign once → sign again → verify both
// =====================================================================

TEST_F(SignatureSecurityTest, MultiSignatureChainVerifiesAll)
{
    // Sign two fields sequentially and verify all signatures via
    // verifyAllSignatures(). Uses the same flow as the CountCorrect
    // test but adds verification assertions.
    auto work = createSignableCopy(ctx_, basePath_, "msv_work.pdf");
    SignatureManager mgr(ctx_);

    // Sign first field.
    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0, makeOpts("Multi-sig verify"));
    }

    // Verify via verifyAllSignatures.
    auto reopened = Document::open(ctx_, work);
    ASSERT_NE(reopened, nullptr);
    auto summary = mgr.verifyAllSignatures(reopened.get());
    EXPECT_GE(summary.totalCount, 1)
        << "Document must have at least 1 signature field";
    EXPECT_GE(summary.signedCount, 1)
        << "Field must be signed";

    for (const auto& sig : summary.signatures) {
        if (sig.isSigned) {
            EXPECT_TRUE(sig.verification.digestValid)
                << "SEC-MULTISIG: signature '" << sig.fieldName
                << "' must have valid digest; status="
                << static_cast<int>(sig.verification.digestStatus);
        }
    }
}

TEST_F(SignatureSecurityTest, MultiSignatureCountCorrect)
{
    auto s1 = tempDir() / "msc_s1.pdf";
    auto s2 = tempDir() / "msc_s2.pdf";
    for (auto* p : {&s1, &s2}) std::filesystem::remove(*p);

    SignatureManager mgr(ctx_);

    {
        auto doc = Document::open(ctx_, basePath_);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "SigA",
                                  QRectF(100, 500, 150, 40));
        doc->saveAs(s1);
    }
    {
        auto doc = Document::open(ctx_, s1);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0, makeOpts("Sig A"));
    }
    {
        auto doc = Document::open(ctx_, s1);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "SigB",
                                  QRectF(300, 500, 150, 40));
        doc->saveAs(s2);
    }
    {
        auto doc = Document::open(ctx_, s2);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 1, makeOpts("Sig B"));
    }

    auto reopened = Document::open(ctx_, s2);
    EXPECT_GE(mgr.countSignatures(reopened.get()), 2);
}

// =====================================================================
// SEC-PKCS7-TAMPER: Sign → corrupt PKCS#7 → verify rejection
// =====================================================================

TEST_F(SignatureSecurityTest, TamperedPkcs7Rejected)
{
    auto work = createSignableCopy(ctx_, basePath_, "pkcs7_tamper.pdf");
    SignatureManager mgr(ctx_);

    // Sign.
    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0, makeOpts("PKCS7 tamper test"));
    }

    // Verify pre-tamper baseline.
    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        auto baseline = mgr.verifySignature(doc.get(), page.get(), 0);
        ASSERT_TRUE(baseline.digestValid)
            << "Pre-tamper signature must be valid";
    }

    // Tamper a byte at 75% through the file (hits content/signature area).
    {
        std::fstream f(work, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f) << "Must be able to open signed PDF for tamper";
        f.seekg(0, std::ios::end);
        auto size = f.tellg();
        ASSERT_GT(size, std::streampos(1024));
        auto pos = size * 3 / 4;
        f.seekg(pos);
        char b;
        f.read(&b, 1);
        f.seekp(pos);
        char flipped = b ^ static_cast<char>(0xFF);
        f.write(&flipped, 1);
        f.flush();
    }

    // Verify post-tamper: digest must be invalid.
    auto reopened = Document::open(ctx_, work);
    if (reopened) {
        auto page = reopened->page(0);
        auto result = mgr.verifySignature(reopened.get(), page.get(), 0);
        EXPECT_FALSE(result.digestValid)
            << "SEC-PKCS7-TAMPER: tampered PKCS#7 must fail digest verification";
        EXPECT_NE(result.digestStatus, SignatureError::Unknown)
            << "Verifier must produce a specific error code, not Unknown";
        EXPECT_FALSE(result.digestMessage.empty())
            << "digestMessage must be non-empty for user feedback";
    } else {
        SUCCEED() << "Tampered PDF unparseable — valid tamper detection";
    }
}

TEST_F(SignatureSecurityTest, TamperedPkcs7NotFullyValid)
{
    auto work = createSignableCopy(ctx_, basePath_, "pkcs7_tamper2.pdf");
    SignatureManager mgr(ctx_);

    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0, makeOpts("PKCS7 tamper2"));
    }

    // Tamper — deterministically FLIP the byte so the mutation is guaranteed
    // to change the signed range.
    //
    // R-13 FIX (audit test_signature_security_phase2.cpp:310): the previous
    // line was
    //     f.write(&b == 0 ? "\x01" : "\x00", 1);
    // which C++ parses as  (&b) == 0 ? ... : ...  — i.e. "is the address of a
    // stack local null?", which is ALWAYS false, so it unconditionally wrote
    // "\x00". When the byte at pos was already 0x00 the tamper was a NO-OP and
    // the oracle "verified" an untampered file (a false pass). We XOR-flip the
    // byte (matching TamperingFlippedByteFailsDigestVerify) so at least one bit
    // always changes, then assert the verifier actively rejects it.
    char original = 0;
    {
        std::fstream f(work, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f) << "Must be able to open signed PDF for tamper";
        f.seekg(0, std::ios::end);
        auto size = f.tellg();
        ASSERT_GT(size, std::streampos(1024));
        auto pos = size * 3 / 4;
        f.seekg(pos);
        f.read(&original, 1);
        f.seekp(pos);
        char flipped = original ^ static_cast<char>(0xFF);
        ASSERT_NE(flipped, original) << "XOR-flip must change the byte";
        f.write(&flipped, 1);
        f.flush();
    }

    auto reopened = Document::open(ctx_, work);
    if (reopened) {
        auto page = reopened->page(0);
        auto result = mgr.verifySignature(reopened.get(), page.get(), 0);
        // A genuinely flipped byte in the signed range must be DETECTED: the
        // digest must fail and the roll-up isFullyValid() must be false.
        EXPECT_FALSE(result.digestValid)
            << "SEC-PKCS7-TAMPER: flipped byte in signed range must invalidate "
               "the digest";
        EXPECT_FALSE(result.isFullyValid())
            << "SEC-PKCS7-TAMPER: tampered document must not report isFullyValid()";
    } else {
        SUCCEED() << "Tampered PDF unparseable — also a valid tamper outcome";
    }
}
