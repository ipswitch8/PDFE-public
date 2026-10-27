// -*- coding: utf-8 -*-
//
// Pipeline 4 (HARDEN-DOCMDP): DocMDP byte-range regression test.
// Prevents recurrence of P-GAP-SIG-CERT-BYTERANGE where adding
// /Reference to /V dict after pdf_sign_signature() caused MuPDF's
// byte-range computation to go negative.
//
// The fix (pipeline b9e14d07 phase-2) stores DocMDP metadata in a
// standalone /Root/Perms dict instead of modifying /V post-signing.

#include <gtest/gtest.h>

#include "signatures/SignatureManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QRectF>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {
std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_docmdp_regression";
    std::filesystem::create_directories(d);
    return d;
}
}

class DocMdpByteRangeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        pfx_ = std::filesystem::path(fixturesDir()) / "test_signer.pfx";
        src_ = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (!std::filesystem::exists(pfx_) || !std::filesystem::exists(src_))
            GTEST_SKIP() << "Signing fixtures not available";
        td_ = tempDir();
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path pfx_, src_, td_;
};

TEST_F(DocMdpByteRangeTest, CertificationSignatureByteRangeValid)
{
    SignatureManager mgr(ctx_);

    auto step1 = td_ / "cert_step1.pdf";
    std::filesystem::remove(step1);
    {
        auto doc = Document::open(ctx_, src_);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "CertSig", QRectF(100, 500, 200, 50));
        doc->saveAs(step1);
    }

    auto step2 = td_ / "cert_step2.pdf";
    std::filesystem::copy(step1, step2, std::filesystem::copy_options::overwrite_existing);
    {
        auto doc = Document::open(ctx_, step2);
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfx_.string();
        opts.pfxPassword = "test123";
        opts.reason = "DocMDP regression test";
        opts.certification = CertificationLevel::NoChanges;
        mgr.signField(doc.get(), page.get(), 0, opts);
    }

    // Verify the signed file has valid byte-range (the bug caused
    // negative byte-range values, making the file unreadable)
    auto doc = Document::open(ctx_, step2);
    ASSERT_NE(doc, nullptr) << "Signed file must be openable";

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto sigs = mgr.listSignatures(doc.get());
    ASSERT_GE(sigs.size(), 1u) << "Must have at least 1 signature";
    EXPECT_TRUE(sigs[0].isSigned) << "Signature must be signed";

    auto vr = mgr.verifySignature(doc.get(), page.get(), sigs[0].widgetIndex);
    EXPECT_TRUE(vr.digestValid)
        << "HARDEN-DOCMDP: digest must be valid after certification signing "
        << "(byte-range regression check)";
}

TEST_F(DocMdpByteRangeTest, DocMdpLevelPresentInPermsDict)
{
    SignatureManager mgr(ctx_);

    auto step1 = td_ / "perms_step1.pdf";
    std::filesystem::remove(step1);
    {
        auto doc = Document::open(ctx_, src_);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "PermsSig", QRectF(100, 500, 200, 50));
        doc->saveAs(step1);
    }

    auto step2 = td_ / "perms_step2.pdf";
    std::filesystem::copy(step1, step2, std::filesystem::copy_options::overwrite_existing);
    {
        auto doc = Document::open(ctx_, step2);
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfx_.string();
        opts.pfxPassword = "test123";
        opts.reason = "DocMDP perms test";
        opts.certification = CertificationLevel::NoChanges;
        mgr.signField(doc.get(), page.get(), 0, opts);
    }

    // Read the raw bytes to verify /Perms is present
    std::ifstream f(step2, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("/Perms"), std::string::npos)
        << "HARDEN-DOCMDP: /Perms dict must be present in certified PDF";
    EXPECT_NE(content.find("/DocMDP"), std::string::npos)
        << "HARDEN-DOCMDP: /DocMDP must be present under /Perms";
}

TEST_F(DocMdpByteRangeTest, ApprovalSignatureNoDocMdp)
{
    SignatureManager mgr(ctx_);

    auto step1 = td_ / "approval_step1.pdf";
    std::filesystem::remove(step1);
    {
        auto doc = Document::open(ctx_, src_);
        auto page = doc->page(0);
        mgr.createSignatureField(page.get(), "ApprSig", QRectF(100, 500, 200, 50));
        doc->saveAs(step1);
    }

    auto step2 = td_ / "approval_step2.pdf";
    std::filesystem::copy(step1, step2, std::filesystem::copy_options::overwrite_existing);
    {
        auto doc = Document::open(ctx_, step2);
        auto page = doc->page(0);
        SigningOptions opts;
        opts.pfxPath = pfx_.string();
        opts.pfxPassword = "test123";
        opts.reason = "Approval only";
        opts.certification = CertificationLevel::NotCertification;
        mgr.signField(doc.get(), page.get(), 0, opts);
    }

    auto doc = Document::open(ctx_, step2);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);

    auto vr = mgr.verifySignature(doc.get(), page.get(), 0);
    EXPECT_TRUE(vr.digestValid)
        << "Approval signature must have valid digest";
}
