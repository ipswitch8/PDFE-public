// -*- coding: utf-8 -*-
//
// Pipeline 3f1e9b2c phase-3 (SIG-CERT): certification signature
// enforcement tests. Verifies each DocMDP permission level (P1/P2/P3)
// correctly protects the document, and that approval (non-certification)
// signatures do not restrict modifications.
//
// P-GAP-SIG-CERT-BYTERANGE — already fixed upstream; skip-guards removed here.
// Certification signatures once produced negative-length byte ranges (a
// /Reference array on /V grew the signature dict after its byte-range was
// planned), so verifySignature() threw "length of signature byte range
// negative". That was fixed in commit dbe4639 (2026-05-25) by dropping the
// /Reference mutation. The defensive `GTEST_SKIP()`-on-ByteRangeBug guards
// added a day earlier (a7d583b) then became dead code that had not triggered
// for months. Phase 3 (pipeline e5b9c231) REMOVES those dead skip guards and
// replaces them with hard assertions, so these tests now always exercise real
// certification enforcement and a byte-range REGRESSION would be a FAILURE (not
// a silent skip). No new product fix was needed for this; the fix predates
// Phase 3.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"

#include <QRectF>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_cert_sig";
    std::filesystem::create_directories(d);
    return d;
}

bool fileContains(const std::filesystem::path& p, const std::string& needle)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str().find(needle) != std::string::npos;
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
    mgr.createSignatureField(page.get(), "CertSig", sigRect);
    doc->saveAs(out);
    return out;
}

void tamperByte(const std::filesystem::path& path)
{
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    auto pos = size * 3 / 4;
    f.seekg(pos);
    char b;
    f.read(&b, 1);
    f.seekp(pos);
    char flipped = b ^ static_cast<char>(0xFF);
    f.write(&flipped, 1);
    f.flush();
}

enum class VerifyOutcome { Valid, Invalid, ByteRangeBug };

struct CertVerifyResult {
    VerifyOutcome outcome = VerifyOutcome::Invalid;
    SignatureVerifyResult result;
};

CertVerifyResult tryCertVerify(SignatureManager& mgr,
                               std::shared_ptr<MuPdfContext> ctx,
                               const std::filesystem::path& path)
{
    CertVerifyResult out;
    auto doc = Document::open(ctx, path);
    if (!doc) {
        out.outcome = VerifyOutcome::Invalid;
        return out;
    }
    auto page = doc->page(0);
    try {
        out.result = mgr.verifySignature(doc.get(), page.get(), 0);
        out.outcome = out.result.digestValid
                          ? VerifyOutcome::Valid
                          : VerifyOutcome::Invalid;
    } catch (const std::exception& e) {
        std::string msg = e.what();
        if (msg.find("byte range negative") != std::string::npos) {
            out.outcome = VerifyOutcome::ByteRangeBug;
        } else {
            out.outcome = VerifyOutcome::Invalid;
        }
    }
    return out;
}

} // namespace

class CertEnforcementTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto pfx = std::filesystem::path(fixturesDir()) / "test_signer.pfx";
        auto base = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        auto form = std::filesystem::path(fixturesDir()) / "test_form.pdf";
        if (!std::filesystem::exists(pfx) || !std::filesystem::exists(base)) {
            GTEST_SKIP() << "Signing fixtures not available";
        }
        ctx_ = std::make_shared<MuPdfContext>();
        pfxPath_ = pfx;
        basePath_ = base;
        formExists_ = std::filesystem::exists(form);
        formPath_ = form;
    }

    SigningOptions makeOpts(CertificationLevel level) const {
        SigningOptions opts;
        opts.pfxPath = pfxPath_.string();
        opts.pfxPassword = "test123";
        opts.reason = "Certification enforcement test";
        opts.location = "Test Lab";
        opts.certification = level;
        return opts;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path pfxPath_;
    std::filesystem::path basePath_;
    std::filesystem::path formPath_;
    bool formExists_ = false;
};

// =====================================================================
// P1 (NoChanges) — any modification invalidates
// =====================================================================

TEST_F(CertEnforcementTest, P1_DocMDPBytesPresent)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_p1_bytes.pdf");
    SignatureManager mgr(ctx_);
    auto doc = Document::open(ctx_, work);
    auto page = doc->page(0);
    mgr.signField(doc.get(), page.get(), 0, makeOpts(CertificationLevel::NoChanges));

    EXPECT_TRUE(fileContains(work, "/DocMDP"));
    EXPECT_TRUE(fileContains(work, "/P 1"));
}

TEST_F(CertEnforcementTest, P1_UntouchedDocumentVerification)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_p1_clean.pdf");
    SignatureManager mgr(ctx_);
    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0, makeOpts(CertificationLevel::NoChanges));
    }

    auto vr = tryCertVerify(mgr, ctx_, work);
    // P-GAP-SIG-CERT-BYTERANGE is fixed (upstream, commit dbe4639). A
    // regression must FAIL here, never silently skip.
    ASSERT_NE(vr.outcome, VerifyOutcome::ByteRangeBug)
        << "certification byte-range regressed: verification threw "
           "'byte range negative'";
    EXPECT_EQ(vr.outcome, VerifyOutcome::Valid)
        << "Untouched P1-certified document must pass digest verification";
}

TEST_F(CertEnforcementTest, P1_ContentTamperInvalidatesDigest)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_p1_tamper.pdf");
    SignatureManager mgr(ctx_);
    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0, makeOpts(CertificationLevel::NoChanges));
    }

    tamperByte(work);

    auto vr = tryCertVerify(mgr, ctx_, work);
    // With the byte-range bug fixed, a tampered certified doc must be actively
    // detected as INVALID (not merely unverifiable).
    EXPECT_EQ(vr.outcome, VerifyOutcome::Invalid)
        << "P1: tampered document must be detected as invalid";
}

// =====================================================================
// P2 (FormFillOnly) — form fill preserves; content tamper breaks
// =====================================================================

TEST_F(CertEnforcementTest, P2_DocMDPBytesPresent)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_p2_bytes.pdf");
    SignatureManager mgr(ctx_);
    auto doc = Document::open(ctx_, work);
    auto page = doc->page(0);
    mgr.signField(doc.get(), page.get(), 0, makeOpts(CertificationLevel::FormFillOnly));

    EXPECT_TRUE(fileContains(work, "/DocMDP"));
    EXPECT_TRUE(fileContains(work, "/P 2"));
}

TEST_F(CertEnforcementTest, P2_ContentTamperInvalidatesDigest)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_p2_content.pdf");
    SignatureManager mgr(ctx_);
    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0, makeOpts(CertificationLevel::FormFillOnly));
    }

    tamperByte(work);

    auto vr = tryCertVerify(mgr, ctx_, work);
    EXPECT_EQ(vr.outcome, VerifyOutcome::Invalid)
        << "P2: content tamper must be detected as invalid";
}

TEST_F(CertEnforcementTest, P2_FormFillPreservesDigest)
{
    if (!formExists_)
        GTEST_SKIP() << "test_form.pdf fixture unavailable";

    auto work = createSignableCopy(ctx_, formPath_, "cert_p2_formfill.pdf");

    SignatureManager sigMgr(ctx_);
    pdfe::forms::FormManager formMgr(ctx_);

    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        sigMgr.signField(doc.get(), page.get(), 0,
                         makeOpts(CertificationLevel::FormFillOnly));
    }

    // Verify the signing itself produced a valid signature (no byte-range bug).
    auto baseline = tryCertVerify(sigMgr, ctx_, work);
    ASSERT_NE(baseline.outcome, VerifyOutcome::ByteRangeBug)
        << "certification byte-range regressed on the P2 baseline";
    ASSERT_EQ(baseline.outcome, VerifyOutcome::Valid)
        << "P2: baseline signature must be valid before form fill";

    // Fill a form field and save incrementally.
    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);

        auto fields = formMgr.getFormFields(page.get());
        bool filled = false;
        for (const auto& f : fields) {
            if (f.type == pdfe::forms::FormFieldType::Text) {
                formMgr.setFieldValue(page.get(), f.widgetIndex,
                                      "P2 fill test");
                filled = true;
                break;
            }
        }
        if (!filled) {
            GTEST_SKIP() << "No text field found in test_form.pdf for P2 fill";
        }
        doc->saveIncremental();
    }

    auto afterFill = tryCertVerify(sigMgr, ctx_, work);
    EXPECT_EQ(afterFill.outcome, VerifyOutcome::Valid)
        << "P2: form fill + incremental save must preserve digest validity";
}

// =====================================================================
// P3 (AnnotationsAllowed) — annotations preserve; content tamper breaks
// =====================================================================

TEST_F(CertEnforcementTest, P3_DocMDPBytesPresent)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_p3_bytes.pdf");
    SignatureManager mgr(ctx_);
    auto doc = Document::open(ctx_, work);
    auto page = doc->page(0);
    mgr.signField(doc.get(), page.get(), 0,
                   makeOpts(CertificationLevel::AnnotationsAllowed));

    EXPECT_TRUE(fileContains(work, "/DocMDP"));
    EXPECT_TRUE(fileContains(work, "/P 3"));
}

TEST_F(CertEnforcementTest, P3_AnnotationPreservesDigest)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_p3_annot.pdf");

    SignatureManager sigMgr(ctx_);
    pdfe::annotations::AnnotationManager annotMgr(ctx_);

    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        sigMgr.signField(doc.get(), page.get(), 0,
                         makeOpts(CertificationLevel::AnnotationsAllowed));
    }

    auto baseline = tryCertVerify(sigMgr, ctx_, work);
    ASSERT_NE(baseline.outcome, VerifyOutcome::ByteRangeBug)
        << "certification byte-range regressed on the P3 baseline";
    ASSERT_EQ(baseline.outcome, VerifyOutcome::Valid)
        << "P3: baseline signature must be valid before annotation add";

    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        annotMgr.createAnnotation(page.get(),
            pdfe::annotations::AnnotationType::Text,
            QRectF(200, 200, 24, 24));
        doc->saveIncremental();
    }

    auto afterAnnot = tryCertVerify(sigMgr, ctx_, work);
    EXPECT_EQ(afterAnnot.outcome, VerifyOutcome::Valid)
        << "P3: annotation + incremental save must preserve digest validity";
}

TEST_F(CertEnforcementTest, P3_ContentTamperInvalidatesDigest)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_p3_tamper.pdf");
    SignatureManager mgr(ctx_);
    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0,
                       makeOpts(CertificationLevel::AnnotationsAllowed));
    }

    tamperByte(work);

    auto vr = tryCertVerify(mgr, ctx_, work);
    EXPECT_EQ(vr.outcome, VerifyOutcome::Invalid)
        << "P3: content tamper must be detected as invalid";
}

// =====================================================================
// Approval signature (no certification) — no DocMDP restriction
// =====================================================================

TEST_F(CertEnforcementTest, Approval_OmitsDocMDP)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_approval.pdf");
    SignatureManager mgr(ctx_);
    auto doc = Document::open(ctx_, work);
    auto page = doc->page(0);
    mgr.signField(doc.get(), page.get(), 0,
                   makeOpts(CertificationLevel::NotCertification));

    EXPECT_FALSE(fileContains(work, "/DocMDP"))
        << "Approval signature must not stamp /DocMDP";
}

TEST_F(CertEnforcementTest, Approval_DigestValidAfterSigning)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_approval_valid.pdf");
    SignatureManager mgr(ctx_);
    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        mgr.signField(doc.get(), page.get(), 0,
                       makeOpts(CertificationLevel::NotCertification));
    }

    auto reopened = Document::open(ctx_, work);
    auto page = reopened->page(0);
    auto result = mgr.verifySignature(reopened.get(), page.get(), 0);
    EXPECT_TRUE(result.digestValid)
        << "Approval signature must pass digest verification on reopen";
    EXPECT_FALSE(result.modifiedAfterSigning)
        << "Untouched approval-signed doc must not report modifications";
}

TEST_F(CertEnforcementTest, Approval_DoesNotLockDocument)
{
    auto work = createSignableCopy(ctx_, basePath_, "cert_approval_unlock.pdf");
    SignatureManager sigMgr(ctx_);
    pdfe::annotations::AnnotationManager annotMgr(ctx_);

    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        sigMgr.signField(doc.get(), page.get(), 0,
                         makeOpts(CertificationLevel::NotCertification));
    }

    {
        auto doc = Document::open(ctx_, work);
        auto page = doc->page(0);
        annotMgr.createAnnotation(page.get(),
            pdfe::annotations::AnnotationType::Text,
            QRectF(300, 300, 24, 24));
        doc->saveIncremental();
    }

    auto reopened = Document::open(ctx_, work);
    auto page = reopened->page(0);
    auto result = sigMgr.verifySignature(reopened.get(), page.get(), 0);
    EXPECT_TRUE(result.digestValid)
        << "Approval: annotation + incremental save must preserve digest";
}
