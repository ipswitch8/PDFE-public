// PHASE1_HARDENING: skip=9 expect_ge=0 no_throw=2  (2026-05-08)
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include <filesystem>
#include <memory>

class SaveOperationsTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();

        // Create temp directory for test files
        tempDir_ = std::filesystem::temp_directory_path() / "pdfe_save_test";
        std::filesystem::create_directories(tempDir_);

        // Copy test fixture to temp file for testing
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        std::filesystem::path sourcePdf = fixturesDir / "test_single.pdf";

        if (std::filesystem::exists(sourcePdf)) {
            tempFile_ = tempDir_ / "test_save.pdf";
            std::filesystem::copy_file(sourcePdf, tempFile_,
                std::filesystem::copy_options::overwrite_existing);

            // Open the temp file
            doc_ = pdfe::core::Document::open(context_, tempFile_);
        }
    }

    void TearDown() override {
        doc_.reset();
        context_.reset();

        // Clean up temp directory
        if (std::filesystem::exists(tempDir_)) {
            std::filesystem::remove_all(tempDir_);
        }
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> doc_;
    std::filesystem::path tempDir_;
    std::filesystem::path tempFile_;
};

TEST_F(SaveOperationsTest, SaveCreatesFile) {
    ASSERT_NE(doc_, nullptr) << "test_single.pdf not loaded";

    // Save to the same file (in-place save)
    ASSERT_TRUE(std::filesystem::exists(tempFile_));

    doc_->save(tempFile_);

    // Verify file still exists
    EXPECT_TRUE(std::filesystem::exists(tempFile_));

    // Verify file has content
    EXPECT_GT(std::filesystem::file_size(tempFile_), 0);
}

TEST_F(SaveOperationsTest, SaveAsCreatesNewFile) {
    ASSERT_NE(doc_, nullptr) << "test_single.pdf not loaded";

    // Create a different path for saveAs
    std::filesystem::path newPath = tempDir_ / "test_save_as.pdf";

    ASSERT_FALSE(std::filesystem::exists(newPath));

    // Save to new file
    doc_->saveAs(newPath);

    // Verify new file exists
    EXPECT_TRUE(std::filesystem::exists(newPath));
    EXPECT_GT(std::filesystem::file_size(newPath), 0);

    // Verify original file still exists
    EXPECT_TRUE(std::filesystem::exists(tempFile_));
}

TEST_F(SaveOperationsTest, SavePreservesPageCount) {
    ASSERT_NE(doc_, nullptr) << "test_single.pdf not loaded";

    int originalPageCount = doc_->pageCount();

    // Save the document
    doc_->save(tempFile_);

    // Reopen and verify page count unchanged
    auto reopened = pdfe::core::Document::open(context_, tempFile_);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), originalPageCount);
}

TEST_F(SaveOperationsTest, MarkPageDirty) {
    ASSERT_NE(doc_, nullptr) << "test_single.pdf not loaded";

    // Mark page 0 as dirty. The bare API returns void; assert that
    // a subsequent dirty-aware operation observes the dirty state by
    // re-issuing a display list (the internal cache must be bypassed).
    EXPECT_NO_THROW({
        doc_->markPageDirty(0);
    });
    void* listAfterDirty = doc_->createDisplayList(0);
    ASSERT_NE(listAfterDirty, nullptr)
        << "createDisplayList must succeed after markPageDirty";
    doc_->releaseDisplayList(listAfterDirty);

    // Verify no crash - marking dirty is an internal state change
    // The next createDisplayList will bypass cache
    EXPECT_NO_THROW({
        void* displayList = doc_->createDisplayList(0);
        ASSERT_NE(displayList, nullptr);
        doc_->releaseDisplayList(displayList);
    });
}

TEST_F(SaveOperationsTest, SaveClearsDirtyPages) {
    ASSERT_NE(doc_, nullptr) << "test_single.pdf not loaded";

    // Mark page dirty
    doc_->markPageDirty(0);

    // Save the document
    doc_->save(tempFile_);

    // After save, createDisplayList should work normally
    // (Internal dirty flags should be cleared or handled properly)
    EXPECT_NO_THROW({
        void* displayList = doc_->createDisplayList(0);
        ASSERT_NE(displayList, nullptr);
        doc_->releaseDisplayList(displayList);
    });

    // Pipeline a7c2e541 phase-16: confirm the saved file is real on
    // disk and can be reopened — a save that silently produces a
    // 0-byte file would still pass createDisplayList on the original
    // in-memory document.
    ASSERT_TRUE(std::filesystem::exists(tempFile_));
    ASSERT_GT(std::filesystem::file_size(tempFile_), 0u);
    auto reopened = pdfe::core::Document::open(context_, tempFile_);
    ASSERT_NE(reopened, nullptr);
}

TEST_F(SaveOperationsTest, SaveAsPreservesPageInfo) {
    ASSERT_NE(doc_, nullptr) << "test_single.pdf not loaded";

    // Get page info before save
    auto pageInfo = doc_->pageInfo(0);
    float originalWidth = pageInfo.widthPt;
    float originalHeight = pageInfo.heightPt;

    // Save to new file
    std::filesystem::path newPath = tempDir_ / "test_preserve_info.pdf";
    doc_->saveAs(newPath);

    // Open new file and verify page info
    auto reopened = pdfe::core::Document::open(context_, newPath);
    ASSERT_NE(reopened, nullptr);

    auto reopenedPageInfo = reopened->pageInfo(0);
    EXPECT_NEAR(reopenedPageInfo.widthPt, originalWidth, 0.1f);
    EXPECT_NEAR(reopenedPageInfo.heightPt, originalHeight, 0.1f);
}

TEST_F(SaveOperationsTest, MultipleMarkPageDirty) {
    ASSERT_NE(doc_, nullptr) << "test_single.pdf not loaded";

    // Mark same page dirty multiple times
    EXPECT_NO_THROW({
        doc_->markPageDirty(0);
        doc_->markPageDirty(0);
        doc_->markPageDirty(0);
    });

    // Save should handle this correctly
    EXPECT_NO_THROW({
        doc_->save(tempFile_);
    });
    // Verify the save actually produced a non-empty, reopenable file —
    // bare EXPECT_NO_THROW on save() can't catch a save that silently
    // writes 0 bytes or corrupts the output.
    ASSERT_TRUE(std::filesystem::exists(tempFile_));
    ASSERT_GT(std::filesystem::file_size(tempFile_), 0u)
        << "save() after multiple markPageDirty calls produced an empty file";
    auto reopened = pdfe::core::Document::open(context_, tempFile_);
    ASSERT_NE(reopened, nullptr) << "saved file must reopen successfully";
}

TEST_F(SaveOperationsTest, SaveAfterMultipleOperations) {
    ASSERT_NE(doc_, nullptr) << "test_single.pdf not loaded";

    int originalPageCount = doc_->pageCount();

    // Mark page dirty
    doc_->markPageDirty(0);

    // Create display list
    void* displayList = doc_->createDisplayList(0);
    ASSERT_NE(displayList, nullptr);
    doc_->releaseDisplayList(displayList);

    // Mark dirty again
    doc_->markPageDirty(0);

    // Save the document
    doc_->save(tempFile_);

    // Verify save succeeded
    EXPECT_TRUE(std::filesystem::exists(tempFile_));

    // Reopen and verify
    auto reopened = pdfe::core::Document::open(context_, tempFile_);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), originalPageCount);
}

TEST_F(SaveOperationsTest, SaveToNonExistentDirectory) {
    ASSERT_NE(doc_, nullptr) << "test_single.pdf not loaded";

    // Try to save to a directory that doesn't exist
    std::filesystem::path invalidPath = tempDir_ / "nonexistent_dir" / "test.pdf";

    // This should throw or handle gracefully
    // Behavior depends on implementation - at minimum, shouldn't crash
    try {
        doc_->saveAs(invalidPath);
        // If it succeeds, parent dir was auto-created
    } catch (const std::exception& e) {
        // Expected to throw - directory doesn't exist
        SUCCEED() << "Correctly threw exception for invalid path: " << e.what();
    }
}
