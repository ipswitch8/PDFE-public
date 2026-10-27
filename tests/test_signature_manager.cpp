/**
 * @file test_signature_manager.cpp
 * @brief Unit tests for SignatureManager.
 */

#include <gtest/gtest.h>
#include "signatures/SignatureManager.h"
#include "signatures/SignatureData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <filesystem>
#include <memory>

using namespace pdfe::signatures;

static std::string fixturesDir() {
    return TEST_FIXTURES_DIR;
}

class SignatureManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        manager_ = std::make_unique<SignatureManager>(context_);
    }

    void TearDown() override {
        manager_.reset();
        document_.reset();
        context_.reset();
    }

    void openTestPdf(const std::string& name) {
        std::string path = fixturesDir() + "/" + name;
        document_ = pdfe::core::Document::open(context_, path);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<SignatureManager> manager_;
    std::unique_ptr<pdfe::core::Document> document_;
};

// Test 1: Construct and destroy
TEST_F(SignatureManagerTest, ConstructDestroy) {
    ASSERT_NE(manager_, nullptr);
}

// Test 2: Bridge available
TEST_F(SignatureManagerTest, BridgeAvailable) {
    EXPECT_TRUE(manager_->isPkcs7BridgeAvailable());
}

// Test 3: Count signatures on document with no signatures
TEST_F(SignatureManagerTest, CountSignatures_NoSignatures) {
    std::string path = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    int count = manager_->countSignatures(document_.get());
    EXPECT_EQ(0, count);
}

// Test 4: List signatures on document with no signatures
TEST_F(SignatureManagerTest, ListSignatures_Empty) {
    std::string path = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto sigs = manager_->listSignatures(document_.get());
    EXPECT_TRUE(sigs.empty());
}

// Test 5: Create signature field
TEST_F(SignatureManagerTest, CreateSignatureField) {
    std::string path = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    SignatureData data = manager_->createSignatureField(
        page.get(), "TestSig1", QRectF(100, 500, 200, 50));

    EXPECT_EQ(0, data.pageIndex);
    EXPECT_GE(data.widgetIndex, 0);
    EXPECT_EQ("TestSig1", data.fieldName);
    EXPECT_FALSE(data.isSigned);
}

// Test 6: Create then count signatures
TEST_F(SignatureManagerTest, CreateField_IncreasesCount) {
    std::string path = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    EXPECT_EQ(0, manager_->countSignatures(document_.get()));

    manager_->createSignatureField(page.get(), "Sig1", QRectF(100, 500, 200, 50));
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));

    manager_->createSignatureField(page.get(), "Sig2", QRectF(100, 400, 200, 50));
    EXPECT_EQ(2, manager_->countSignatures(document_.get()));
}

// Test 7: Create and list signatures
TEST_F(SignatureManagerTest, CreateField_AppearsInList) {
    std::string path = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    manager_->createSignatureField(page.get(), "MySig", QRectF(50, 600, 150, 40));

    auto sigs = manager_->listSignatures(document_.get());
    ASSERT_EQ(1u, sigs.size());
    EXPECT_EQ("MySig", sigs[0].fieldName);
    EXPECT_EQ(0, sigs[0].pageIndex);
    EXPECT_FALSE(sigs[0].isSigned);
}

// Test 8: Remove signature field
TEST_F(SignatureManagerTest, RemoveSignatureField) {
    std::string path = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    auto created = manager_->createSignatureField(
        page.get(), "ToRemove", QRectF(100, 500, 200, 50));
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));

    auto removed = manager_->removeSignatureField(page.get(), created.widgetIndex);
    EXPECT_EQ("ToRemove", removed.fieldName);
    EXPECT_EQ(0, manager_->countSignatures(document_.get()));
}

// Test 9: Restore signature field
TEST_F(SignatureManagerTest, RestoreSignatureField) {
    std::string path = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    auto created = manager_->createSignatureField(
        page.get(), "ToRestore", QRectF(100, 500, 200, 50));
    manager_->removeSignatureField(page.get(), created.widgetIndex);
    EXPECT_EQ(0, manager_->countSignatures(document_.get()));

    int newIdx = manager_->restoreSignatureField(page.get(), created);
    EXPECT_GE(newIdx, 0);
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));
}

// Test 10: Null document handling
TEST_F(SignatureManagerTest, CountSignatures_NullDoc) {
    EXPECT_EQ(0, manager_->countSignatures(nullptr));
}

// Test 11: Null document list
TEST_F(SignatureManagerTest, ListSignatures_NullDoc) {
    auto sigs = manager_->listSignatures(nullptr);
    EXPECT_TRUE(sigs.empty());
}

// Test 12: Null page throws
TEST_F(SignatureManagerTest, CreateField_NullPage_Throws) {
    EXPECT_THROW(
        manager_->createSignatureField(nullptr, "Sig", QRectF(0, 0, 100, 50)),
        std::exception);
}

// Test 13: Get signature details
TEST_F(SignatureManagerTest, GetSignatureDetails) {
    std::string path = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    manager_->createSignatureField(page.get(), "DetailSig", QRectF(100, 500, 200, 50));

    auto details = manager_->getSignatureDetails(document_.get(), page.get(), 0);
    EXPECT_EQ("DetailSig", details.fieldName);
    EXPECT_FALSE(details.isSigned);
    EXPECT_EQ(0, details.pageIndex);
}

// Test 14: Sign a field (requires saving to disk first)
TEST_F(SignatureManagerTest, SignField) {
    std::string pfxPath = fixturesDir() + "/test_signer.pfx";
    std::string pdfPath = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(pfxPath) || !std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Required test fixtures not available";
    }

    // pdf_sign_signature does incremental save internally, so we need a writable file
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_sign_test.pdf";
    auto tempPath2 = std::filesystem::temp_directory_path() / "pdfe_sign_test2.pdf";

    // Create field and saveAs to a new file (avoids Windows file-lock on save+rename)
    {
        auto doc = pdfe::core::Document::open(context_, pdfPath);
        auto page = doc->page(0);
        manager_->createSignatureField(page.get(), "SignMe", QRectF(100, 500, 200, 50));
        doc->saveAs(tempPath);
    }

    // Reopen saved file and sign
    {
        auto doc = pdfe::core::Document::open(context_, tempPath);
        auto page = doc->page(0);

        SigningOptions opts;
        opts.pfxPath = pfxPath;
        opts.pfxPassword = "test123";
        opts.reason = "Testing";
        opts.location = "Test Lab";

        EXPECT_NO_THROW(
            manager_->signField(doc.get(), page.get(), 0, opts));
    }

    std::filesystem::remove(tempPath);
    std::filesystem::remove(tempPath2);
}

// Test 15: Sign and verify roundtrip
TEST_F(SignatureManagerTest, SignAndVerify) {
    std::string pfxPath = fixturesDir() + "/test_signer.pfx";
    std::string pdfPath = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(pfxPath) || !std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Required test fixtures not available";
    }

    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_signverify_test.pdf";

    // Create field and saveAs to temp file
    {
        auto doc = pdfe::core::Document::open(context_, pdfPath);
        auto page = doc->page(0);
        manager_->createSignatureField(page.get(), "VerifySig", QRectF(100, 500, 200, 50));
        doc->saveAs(tempPath);
    }

    // Reopen and sign
    {
        auto doc = pdfe::core::Document::open(context_, tempPath);
        auto page = doc->page(0);

        SigningOptions opts;
        opts.pfxPath = pfxPath;
        opts.pfxPassword = "test123";
        opts.reason = "Verification Test";
        opts.location = "Test Lab";

        manager_->signField(doc.get(), page.get(), 0, opts);
    }

    // Reopen again to verify (pdf_sign_signature does incremental save,
    // making the in-memory state stale)
    {
        auto doc = pdfe::core::Document::open(context_, tempPath);
        auto page = doc->page(0);

        auto result = manager_->verifySignature(doc.get(), page.get(), 0);
        EXPECT_TRUE(result.digestValid);
        EXPECT_EQ(SignatureError::Okay, result.digestStatus);
    }

    std::filesystem::remove(tempPath);
}

// Test 16: Verify all signatures
TEST_F(SignatureManagerTest, VerifyAllSignatures) {
    std::string pfxPath = fixturesDir() + "/test_signer.pfx";
    std::string pdfPath = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(pfxPath) || !std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Required test fixtures not available";
    }

    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_verifyall_test.pdf";

    // Create two fields and saveAs to temp file
    {
        auto doc = pdfe::core::Document::open(context_, pdfPath);
        auto page = doc->page(0);
        manager_->createSignatureField(page.get(), "AllSig1", QRectF(100, 500, 200, 50));
        manager_->createSignatureField(page.get(), "AllSig2", QRectF(100, 400, 200, 50));
        doc->saveAs(tempPath);
    }

    // Reopen and sign first field only
    {
        auto doc = pdfe::core::Document::open(context_, tempPath);
        auto page = doc->page(0);

        SigningOptions opts;
        opts.pfxPath = pfxPath;
        opts.pfxPassword = "test123";
        manager_->signField(doc.get(), page.get(), 0, opts);
    }

    // Reopen to verify (signField does incremental save, making in-memory state stale)
    {
        auto doc = pdfe::core::Document::open(context_, tempPath);
        auto summary = manager_->verifyAllSignatures(doc.get());
        EXPECT_EQ(2, summary.totalCount);
        EXPECT_EQ(1, summary.signedCount);
        EXPECT_EQ(1, summary.unsignedCount);
    }

    std::filesystem::remove(tempPath);
}

// Test 17: Sign with empty PFX path throws
TEST_F(SignatureManagerTest, SignField_EmptyPfx_Throws) {
    std::string pdfPath = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    manager_->createSignatureField(page.get(), "Sig", QRectF(100, 500, 200, 50));

    SigningOptions opts;
    // Empty pfxPath
    EXPECT_THROW(
        manager_->signField(document_.get(), page.get(), 0, opts),
        std::exception);
}

// Test 18: Sign with wrong password throws
TEST_F(SignatureManagerTest, SignField_WrongPassword_Throws) {
    std::string pfxPath = fixturesDir() + "/test_signer.pfx";
    std::string pdfPath = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(pfxPath) || !std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Required test fixtures not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    manager_->createSignatureField(page.get(), "Sig", QRectF(100, 500, 200, 50));

    SigningOptions opts;
    opts.pfxPath = pfxPath;
    opts.pfxPassword = "wrongpass";

    EXPECT_ANY_THROW(
        manager_->signField(document_.get(), page.get(), 0, opts));
}

// Test 19: Verify unsigned field
TEST_F(SignatureManagerTest, VerifyUnsignedField) {
    std::string pdfPath = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    manager_->createSignatureField(page.get(), "Unsigned", QRectF(100, 500, 200, 50));

    auto result = manager_->verifySignature(document_.get(), page.get(), 0);
    EXPECT_EQ(SignatureError::NotSigned, result.digestStatus);
    EXPECT_EQ(SignatureError::NotSigned, result.certificateStatus);
    EXPECT_FALSE(result.digestValid);
    EXPECT_FALSE(result.certificateValid);
}

// Test 20: Incremental save (with field creation, no signing needed)
TEST_F(SignatureManagerTest, SaveIncremental) {
    std::string pdfPath = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    // Copy test file to temp location (incremental save modifies in place)
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_incremental.pdf";
    std::filesystem::copy_file(pdfPath, tempPath,
                                std::filesystem::copy_options::overwrite_existing);

    auto origSize = std::filesystem::file_size(tempPath);

    auto tempDoc = pdfe::core::Document::open(context_, tempPath);
    auto page = tempDoc->page(0);

    manager_->createSignatureField(page.get(), "IncrSig", QRectF(100, 500, 200, 50));

    // Phase-53 review, finding 3. This used to call
    // SignatureManager::saveIncremental(), a second
    // pdf_save_document(do_incremental=1) implementation with no staging, no
    // certification check, and no caller anywhere in src/ — these tests were the
    // only thing keeping it alive. It is gone; the maintained route is
    // Document::saveIncrementalStaged(), which runs the DocMDP save guard and
    // publishes by atomic rename.
    //
    // The page must be released FIRST, and that is a property of the hardened
    // route rather than an accommodation: publication renames over the target,
    // Windows will not rename over a file MuPDF holds open, and a live
    // pdfe::core::Page keeps the fz_document alive via fz_keep_document. Every
    // production caller drops its pages the same way (MainWindow's
    // drain -> write -> rebind scope). The assertions below are unchanged.
    page.reset();

    EXPECT_NO_THROW(tempDoc->saveIncrementalStaged());

    // Close before checking file size
    tempDoc.reset();

    // Verify file size increased (incremental append adds data)
    auto newSize = std::filesystem::file_size(tempPath);
    EXPECT_GT(newSize, origSize);

    std::filesystem::remove(tempPath);
}

// Test 21: Preview signature appearance
TEST_F(SignatureManagerTest, PreviewAppearance) {
    std::string pfxPath = fixturesDir() + "/test_signer.pfx";
    if (!std::filesystem::exists(pfxPath)) {
        GTEST_SKIP() << "test_signer.pfx not available";
    }

    SigningOptions opts;
    opts.pfxPath = pfxPath;
    opts.pfxPassword = "test123";
    opts.reason = "Test";
    opts.location = "Lab";

    // Preview uses pdf_preview_signature_as_pixmap which may produce varying formats
    QImage preview;
    EXPECT_NO_THROW(preview = manager_->previewSignatureAppearance(200, 100, opts));

    // If preview was generated, verify it has reasonable dimensions
    if (!preview.isNull()) {
        EXPECT_GT(preview.width(), 0);
        EXPECT_GT(preview.height(), 0);
    }
}

// Test 22: Clear signature on unsigned field (no-op, should not throw)
TEST_F(SignatureManagerTest, ClearSignature_Unsigned) {
    std::string pdfPath = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    manager_->createSignatureField(page.get(), "ClearMe", QRectF(100, 500, 200, 50));

    // Clearing an unsigned field should be a safe no-op
    EXPECT_NO_THROW(manager_->clearSignature(page.get(), 0));

    // Field should still exist and remain unsigned
    auto details = manager_->getSignatureDetails(document_.get(), page.get(), 0);
    EXPECT_FALSE(details.isSigned);
    EXPECT_EQ(1, manager_->countSignatures(document_.get()));
}

// Test 23: Multiple pages with signatures
TEST_F(SignatureManagerTest, MultiPageSignatures) {
    std::string pdfPath = fixturesDir() + "/test_3page.pdf";
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "test_3page.pdf not available";
    }

    openTestPdf("test_3page.pdf");
    ASSERT_GE(document_->pageCount(), 2);

    auto page0 = document_->page(0);
    auto page1 = document_->page(1);

    manager_->createSignatureField(page0.get(), "Page1Sig", QRectF(100, 500, 200, 50));
    manager_->createSignatureField(page1.get(), "Page2Sig", QRectF(100, 500, 200, 50));

    auto sigs = manager_->listSignatures(document_.get());
    ASSERT_EQ(2u, sigs.size());
    EXPECT_EQ(0, sigs[0].pageIndex);
    EXPECT_EQ(1, sigs[1].pageIndex);
}

// Test 24 was SaveIncremental_NullDoc_Throws. Its entire subject was
// SignatureManager::saveIncremental()'s own `if (!doc) throw` line, and that
// function was removed in the phase-53 review remediation (finding 3 — dead,
// unstaged, uncertification-checked duplicate of
// Document::saveIncrementalStaged; see the tombstone in SignatureManager.h).
// A null-argument check cannot outlive the function it guarded, so the test
// goes with it rather than being re-pointed: Document::saveIncrementalStaged()
// is a member function and has no null-document argument to reject.

// Test 25: Remove nonexistent widget throws
TEST_F(SignatureManagerTest, RemoveField_InvalidIndex_Throws) {
    std::string pdfPath = fixturesDir() + "/test_single.pdf";
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    openTestPdf("test_single.pdf");
    auto page = document_->page(0);

    EXPECT_ANY_THROW(
        manager_->removeSignatureField(page.get(), 99));
}
