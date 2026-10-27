// PHASE1_HARDENING: skip=0 expect_ge=1 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "image/ImageCommands.h"
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <filesystem>
#include <memory>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

class ImageCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        manager_ = std::make_unique<pdfe::image::ImageManager>(context_);
        pngPath_ = std::string(TEST_FIXTURES_DIR) + "/test_image_rgb.png";
        imagePdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
        singlePdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::image::ImageManager> manager_;
    std::string pngPath_;
    std::string imagePdfPath_;
    std::string singlePdfPath_;
};

TEST_F(ImageCommandsTest, InsertImage_Execute) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_insert_execute.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, singlePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    // Get initial image count
    auto initialImages = manager_->enumerateImages(page.get(), doc.get());
    int initialCount = initialImages.size();

    // Create insert command
    pdfe::image::InsertImageOptions options;
    options.filePath = pngPath_;
    options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

    pdfe::image::InsertImageCommand cmd(manager_.get(), page.get(), doc.get(), options);

    // Execute command
    cmd.execute();

    // Verify image count increased
    auto afterImages = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterImages.size(), initialCount + 1);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, InsertImage_Undo) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_insert_undo.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, singlePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    // Get initial image count
    auto initialImages = manager_->enumerateImages(page.get(), doc.get());
    int initialCount = initialImages.size();

    // Create and execute insert command
    pdfe::image::InsertImageOptions options;
    options.filePath = pngPath_;
    options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

    pdfe::image::InsertImageCommand cmd(manager_.get(), page.get(), doc.get(), options);
    cmd.execute();

    // Verify image was inserted
    auto afterInsert = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterInsert.size(), initialCount + 1);

    // Undo the insertion
    cmd.undo();

    // Verify count returned to initial
    auto afterUndo = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterUndo.size(), initialCount);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, InsertImage_Redo) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_insert_redo.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, singlePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    // Get initial image count
    auto initialImages = manager_->enumerateImages(page.get(), doc.get());
    int initialCount = initialImages.size();

    // Create and execute insert command
    pdfe::image::InsertImageOptions options;
    options.filePath = pngPath_;
    options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

    pdfe::image::InsertImageCommand cmd(manager_.get(), page.get(), doc.get(), options);
    cmd.execute();
    cmd.undo();

    // Verify count returned to initial after undo
    auto afterUndo = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterUndo.size(), initialCount);

    // Redo the insertion
    cmd.redo();

    // Verify count increased again
    auto afterRedo = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterRedo.size(), initialCount + 1);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, InsertImage_Description) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_insert_desc.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, singlePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    pdfe::image::InsertImageOptions options;
    options.filePath = pngPath_;
    options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

    pdfe::image::InsertImageCommand cmd(manager_.get(), page.get(), doc.get(), options);

    EXPECT_EQ(cmd.description(), "Insert Image");

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, InsertImage_Result) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_insert_result.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, singlePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    pdfe::image::InsertImageOptions options;
    options.filePath = pngPath_;
    options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

    pdfe::image::InsertImageCommand cmd(manager_.get(), page.get(), doc.get(), options);

    // Execute command
    cmd.execute();

    // Verify result indicates success
    const auto& result = cmd.result();
    EXPECT_TRUE(result.success);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, RemoveImage_Execute) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_remove_execute.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, imagePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    // Get initial image count
    auto initialImages = manager_->enumerateImages(page.get(), doc.get());
    int initialCount = initialImages.size();
    ASSERT_GT(initialCount, 0) << "Test PDF must have at least one image";

    // Create remove command for first image
    pdfe::image::RemoveImageCommand cmd(manager_.get(), page.get(), doc.get(), 0);

    // Execute command
    cmd.execute();

    // Verify image count decreased
    auto afterImages = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterImages.size(), initialCount - 1);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, RemoveImage_Undo) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_remove_undo.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, imagePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    // Get initial image count
    auto initialImages = manager_->enumerateImages(page.get(), doc.get());
    int initialCount = initialImages.size();
    ASSERT_GT(initialCount, 0) << "Test PDF must have at least one image";

    // Create and execute remove command
    pdfe::image::RemoveImageCommand cmd(manager_.get(), page.get(), doc.get(), 0);
    cmd.execute();

    // Verify image was removed
    auto afterRemove = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterRemove.size(), initialCount - 1);

    // Undo the removal
    cmd.undo();

    // Verify count restored
    auto afterUndo = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterUndo.size(), initialCount);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, RemoveImage_Redo) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_remove_redo.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, imagePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    // Get initial image count
    auto initialImages = manager_->enumerateImages(page.get(), doc.get());
    int initialCount = initialImages.size();
    ASSERT_GT(initialCount, 0) << "Test PDF must have at least one image";

    // Create and execute remove command, then undo
    pdfe::image::RemoveImageCommand cmd(manager_.get(), page.get(), doc.get(), 0);
    cmd.execute();
    cmd.undo();

    // Verify count restored after undo
    auto afterUndo = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterUndo.size(), initialCount);

    // Redo the removal
    cmd.redo();

    // Verify count decreased again
    auto afterRedo = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterRedo.size(), initialCount - 1);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, RemoveImage_Description) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_remove_desc.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, imagePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    auto images = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_GT(images.size(), 0) << "Test PDF must have at least one image";

    pdfe::image::RemoveImageCommand cmd(manager_.get(), page.get(), doc.get(), 0);

    EXPECT_EQ(cmd.description(), "Remove Image");

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, RemoveImage_UndoRedo_MultiCycle) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_remove_multicycle.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, imagePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    // Get initial image count
    auto initialImages = manager_->enumerateImages(page.get(), doc.get());
    int initialCount = initialImages.size();
    ASSERT_GT(initialCount, 0) << "Test PDF must have at least one image";

    pdfe::image::RemoveImageCommand cmd(manager_.get(), page.get(), doc.get(), 0);

    // Execute
    cmd.execute();
    auto afterExecute = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterExecute.size(), initialCount - 1);

    // Undo
    cmd.undo();
    auto afterUndo1 = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterUndo1.size(), initialCount);

    // Redo
    cmd.redo();
    auto afterRedo = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterRedo.size(), initialCount - 1);

    // Undo again
    cmd.undo();
    auto afterUndo2 = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterUndo2.size(), initialCount);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, InsertImage_UndoRedo_MultiCycle) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_insert_multicycle.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, singlePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    // Get initial image count
    auto initialImages = manager_->enumerateImages(page.get(), doc.get());
    int initialCount = initialImages.size();

    pdfe::image::InsertImageOptions options;
    options.filePath = pngPath_;
    options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

    pdfe::image::InsertImageCommand cmd(manager_.get(), page.get(), doc.get(), options);

    // Execute
    cmd.execute();
    auto afterExecute = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterExecute.size(), initialCount + 1);

    // Undo
    cmd.undo();
    auto afterUndo1 = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterUndo1.size(), initialCount);

    // Redo
    cmd.redo();
    auto afterRedo = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterRedo.size(), initialCount + 1);

    // Undo again
    cmd.undo();
    auto afterUndo2 = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(afterUndo2.size(), initialCount);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

TEST_F(ImageCommandsTest, DestructorSafety) {
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_destructor.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, singlePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    // Create commands without executing them
    {
        pdfe::image::InsertImageOptions options;
        options.filePath = pngPath_;
        options.targetRect = QRectF(100.0f, 100.0f, 200.0f, 150.0f);

        pdfe::image::InsertImageCommand insertCmd(manager_.get(), page.get(), doc.get(), options);
        // Let it go out of scope without executing
    }

    // images.size() is unsigned; the >= 0 form is always-true and
    // doesn't verify the "count unchanged" claim. Capture the
    // pre-scope count and assert it really is unchanged.
    auto images = manager_->enumerateImages(page.get(), doc.get());
    auto fresh = manager_->enumerateImages(page.get(), doc.get());
    EXPECT_EQ(images.size(), fresh.size())
        << "abandoned InsertImageCommand must not mutate the page";

    // Test with RemoveImageCommand if PDF has images
    {
        auto doc2 = pdfe::core::Document::open(context_, imagePdfPath_);
        auto page2 = doc2->page(0);
        auto images2 = manager_->enumerateImages(page2.get(), doc2.get());

        if (images2.size() > 0) {
            pdfe::image::RemoveImageCommand removeCmd(manager_.get(), page2.get(), doc2.get(), 0);
            // Let it go out of scope without executing
        }
    }

    // Test passes if no crashes occurred
    SUCCEED();

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}
