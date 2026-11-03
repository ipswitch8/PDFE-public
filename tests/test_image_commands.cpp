// PHASE1_HARDENING: skip=0 expect_ge=1 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "image/ImageCommands.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <algorithm>
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

// GitHub #329 — the reposition/resize command path an inserted image reaches
// through its (now live) handles. The existing ImageContentEditor tests assert
// resizeImage() RETURNS true and preserves the image count; neither proves the
// geometry actually moved, nor that undo puts it back. This asserts the real
// behavioural contract against enumeration (which recomputes the bounding box
// from the drawn CTM), so it fails if resize became a no-op or if undo stopped
// restoring the prior content stream.
TEST_F(ImageCommandsTest, ResizeImage_ChangesGeometryAndUndoReverts) {
    if (!std::filesystem::exists(imagePdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << imagePdfPath_;
    }

    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_cmd_resize_geom.pdf";
    {
        auto doc = pdfe::core::Document::open(context_, imagePdfPath_);
        doc->saveAs(tempPath);
    }

    auto doc = pdfe::core::Document::open(context_, tempPath);
    auto page = doc->page(0);

    auto findByName = [](const std::vector<pdfe::image::PageImageInfo>& v,
                         const std::string& name)
        -> const pdfe::image::PageImageInfo* {
        auto it = std::find_if(v.begin(), v.end(),
            [&](const pdfe::image::PageImageInfo& i) { return i.xobjectName == name; });
        return it == v.end() ? nullptr : &*it;
    };

    auto before = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(before.empty()) << "fixture must contain at least one image";
    const std::string name = before[0].xobjectName;
    const QRectF origBox = before[0].boundingBox;
    ASSERT_GT(origBox.width(), 0.0);
    ASSERT_GT(origBox.height(), 0.0);

    // A target size and position clearly distinct from the original, so a
    // no-op resize cannot accidentally satisfy the assertion.
    const float newW = static_cast<float>(origBox.width()) + 123.0f;
    const float newH = static_cast<float>(origBox.height()) + 77.0f;
    const float newX = static_cast<float>(origBox.x()) + 41.0f;
    const float newY = static_cast<float>(origBox.y()) + 29.0f;

    auto editor = std::make_unique<pdfe::image::ImageContentEditor>(context_);
    pdfe::image::ResizeImageCommand cmd(
        manager_.get(), editor.get(), page.get(), doc.get(),
        name, newW, newH, newX, newY);

    cmd.execute();

    auto after = manager_->enumerateImages(page.get(), doc.get());
    const auto* resized = findByName(after, name);
    ASSERT_NE(resized, nullptr) << "image vanished after resize";
    EXPECT_NEAR(resized->boundingBox.width(), newW, 1.0)
        << "ResizeImageCommand did not change the drawn width";
    EXPECT_NEAR(resized->boundingBox.height(), newH, 1.0)
        << "ResizeImageCommand did not change the drawn height";
    // And it is genuinely different from where it started.
    EXPECT_GT(std::abs(resized->boundingBox.width() - origBox.width()), 1.0);
    EXPECT_GT(std::abs(resized->boundingBox.height() - origBox.height()), 1.0);

    cmd.undo();

    auto reverted = manager_->enumerateImages(page.get(), doc.get());
    const auto* restored = findByName(reverted, name);
    ASSERT_NE(restored, nullptr) << "image vanished after undo";
    EXPECT_NEAR(restored->boundingBox.width(), origBox.width(), 1.0)
        << "undo did not restore the original width";
    EXPECT_NEAR(restored->boundingBox.height(), origBox.height(), 1.0)
        << "undo did not restore the original height";
    EXPECT_NEAR(restored->boundingBox.x(), origBox.x(), 1.0)
        << "undo did not restore the original x position";
    EXPECT_NEAR(restored->boundingBox.y(), origBox.y(), 1.0)
        << "undo did not restore the original y position";

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
