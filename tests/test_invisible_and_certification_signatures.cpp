// -*- coding: utf-8 -*-
// Invisible signatures (zero-area + no appearance stream) and
// certification signatures (/Perms /DocMDP) — parity items beyond
// the basic approval-signature flow already covered by
// test_signature_manager.cpp.
#include <gtest/gtest.h>
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace pdfe::signatures;
using namespace pdfe::core;

namespace {

std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempWork(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_sig_ext";
    std::filesystem::create_directories(d);
    return d / name;
}

bool fileContains(const std::filesystem::path& p, const std::string& needle)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss; ss << in.rdbuf();
    return ss.str().find(needle) != std::string::npos;
}

std::filesystem::path setupSignableFixture(std::shared_ptr<MuPdfContext> ctx,
                                            const char* outName,
                                            QRectF rect)
{
    auto src = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    auto out = tempWork(outName);
    std::filesystem::remove(out);
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, src);
    auto page = doc->page(0);
    mgr.createSignatureField(page.get(), "TestSig", rect);
    doc->saveAs(out);
    return out;
}

} // namespace

class InvisibleSignatureTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::filesystem::exists(
                std::filesystem::path(fixturesDir()) / "test_signer.pfx") ||
            !std::filesystem::exists(
                std::filesystem::path(fixturesDir()) / "test_single.pdf")) {
            GTEST_SKIP() << "Signing fixtures not available";
        }
        ctx = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx;
};

TEST_F(InvisibleSignatureTest, InvisibleFlagSignsSuccessfully)
{
    auto workPath = setupSignableFixture(ctx, "invisible_work.pdf",
                                         QRectF(0, 0, 0, 0));
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, workPath);
    auto page = doc->page(0);

    SigningOptions opts;
    opts.pfxPath = fixturesDir() + "/test_signer.pfx";
    opts.pfxPassword = "test123";
    opts.reason = "Invisible signature test";
    opts.invisible = true;

    EXPECT_NO_THROW(mgr.signField(doc.get(), page.get(), 0, opts));
}

TEST_F(InvisibleSignatureTest, SignedFileContainsSignatureDict)
{
    auto workPath = setupSignableFixture(ctx, "invisible_marker.pdf",
                                         QRectF(0, 0, 0, 0));
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, workPath);
    auto page = doc->page(0);

    SigningOptions opts;
    opts.pfxPath = fixturesDir() + "/test_signer.pfx";
    opts.pfxPassword = "test123";
    opts.invisible = true;
    mgr.signField(doc.get(), page.get(), 0, opts);

    // The on-disk file must have a /Sig signature dict regardless of
    // visibility — invisibility only suppresses /AP, not /V.
    EXPECT_TRUE(fileContains(workPath, "/Type/Sig")
             || fileContains(workPath, "/Type /Sig"));
}

TEST_F(InvisibleSignatureTest, VisibleFlagStillWorksForComparison)
{
    auto workPath = setupSignableFixture(ctx, "visible_compare.pdf",
                                         QRectF(100, 500, 200, 50));
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, workPath);
    auto page = doc->page(0);

    SigningOptions opts;
    opts.pfxPath = fixturesDir() + "/test_signer.pfx";
    opts.pfxPassword = "test123";
    opts.invisible = false;  // default behavior

    EXPECT_NO_THROW(mgr.signField(doc.get(), page.get(), 0, opts));
}

class CertificationSignatureTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::filesystem::exists(
                std::filesystem::path(fixturesDir()) / "test_signer.pfx") ||
            !std::filesystem::exists(
                std::filesystem::path(fixturesDir()) / "test_single.pdf")) {
            GTEST_SKIP() << "Signing fixtures not available";
        }
        ctx = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx;
};

TEST_F(CertificationSignatureTest, NoChangesLevelWritesDocMDP)
{
    auto workPath = setupSignableFixture(ctx, "cert_nochanges.pdf",
                                         QRectF(100, 500, 200, 50));
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, workPath);
    auto page = doc->page(0);

    SigningOptions opts;
    opts.pfxPath = fixturesDir() + "/test_signer.pfx";
    opts.pfxPassword = "test123";
    opts.certification = CertificationLevel::NoChanges;
    mgr.signField(doc.get(), page.get(), 0, opts);

    EXPECT_TRUE(fileContains(workPath, "/DocMDP"));
    EXPECT_TRUE(fileContains(workPath, "/TransformMethod"));
    EXPECT_TRUE(fileContains(workPath, "/Perms"));
}

TEST_F(CertificationSignatureTest, FormFillOnlyLevelEmbedsPEquals2)
{
    auto workPath = setupSignableFixture(ctx, "cert_formfill.pdf",
                                         QRectF(100, 500, 200, 50));
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, workPath);
    auto page = doc->page(0);

    SigningOptions opts;
    opts.pfxPath = fixturesDir() + "/test_signer.pfx";
    opts.pfxPassword = "test123";
    opts.certification = CertificationLevel::FormFillOnly;
    mgr.signField(doc.get(), page.get(), 0, opts);

    EXPECT_TRUE(fileContains(workPath, "/DocMDP"));
    // /P 2 indicates FormFillOnly per PDF spec.
    EXPECT_TRUE(fileContains(workPath, "/P 2"));
}

TEST_F(CertificationSignatureTest, AnnotationsAllowedLevelEmbedsPEquals3)
{
    auto workPath = setupSignableFixture(ctx, "cert_annot.pdf",
                                         QRectF(100, 500, 200, 50));
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, workPath);
    auto page = doc->page(0);

    SigningOptions opts;
    opts.pfxPath = fixturesDir() + "/test_signer.pfx";
    opts.pfxPassword = "test123";
    opts.certification = CertificationLevel::AnnotationsAllowed;
    mgr.signField(doc.get(), page.get(), 0, opts);

    EXPECT_TRUE(fileContains(workPath, "/P 3"));
}

TEST_F(CertificationSignatureTest, NotCertificationOmitsDocMDP)
{
    auto workPath = setupSignableFixture(ctx, "cert_none.pdf",
                                         QRectF(100, 500, 200, 50));
    SignatureManager mgr(ctx);
    auto doc = Document::open(ctx, workPath);
    auto page = doc->page(0);

    SigningOptions opts;
    opts.pfxPath = fixturesDir() + "/test_signer.pfx";
    opts.pfxPassword = "test123";
    opts.certification = CertificationLevel::NotCertification;
    mgr.signField(doc.get(), page.get(), 0, opts);

    // An ordinary approval signature must NOT stamp /DocMDP.
    EXPECT_FALSE(fileContains(workPath, "/DocMDP"));
}

TEST(CertificationLevelEnumTest, ValuesMatchPdfSpec)
{
    // PDF spec fixes these numeric values for /TransformParams /P.
    EXPECT_EQ(static_cast<int>(CertificationLevel::NotCertification), 0);
    EXPECT_EQ(static_cast<int>(CertificationLevel::NoChanges),        1);
    EXPECT_EQ(static_cast<int>(CertificationLevel::FormFillOnly),     2);
    EXPECT_EQ(static_cast<int>(CertificationLevel::AnnotationsAllowed), 3);
}
