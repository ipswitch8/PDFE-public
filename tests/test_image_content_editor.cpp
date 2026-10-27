#include <gtest/gtest.h>
#include "image/ImageContentEditor.h"
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QRectF>
#include <filesystem>
#include <memory>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

class ImageContentEditorTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);

        pdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
    }

    // Helper to create a temp copy for mutation tests
    std::filesystem::path makeTempCopy(const std::string& suffix) {
        auto tempPath = std::filesystem::temp_directory_path() / ("pdfe_test_editor_" + suffix + ".pdf");
        auto srcDoc = Document::open(context_, pdfPath_);
        srcDoc->saveAs(tempPath);
        return tempPath;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::string pdfPath_;
};

// Test 1: Construct - no throw
TEST_F(ImageContentEditorTest, Construct) {
    EXPECT_NO_THROW({
        auto ed = std::make_unique<ImageContentEditor>(context_);
        EXPECT_NE(ed, nullptr);
    });
}

// Test 2: ResizeImage_Success - returns true for valid arguments
TEST_F(ImageContentEditorTest, ResizeImage_Success) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("resize_ok");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    bool result = editor_->resizeImage(page.get(), doc.get(), "Im0", 300.0f, 250.0f, 50.0f, 50.0f);
    EXPECT_TRUE(result);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 3: ResizeImage_PreservesImageCount - image count unchanged after resize
TEST_F(ImageContentEditorTest, ResizeImage_PreservesImageCount) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("resize_count");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    int countBefore = manager_->imageCount(page.get(), doc.get());

    editor_->resizeImage(page.get(), doc.get(), "Im0", 300.0f, 250.0f, 50.0f, 50.0f);

    int countAfter = manager_->imageCount(page.get(), doc.get());
    EXPECT_EQ(countBefore, countAfter);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 4: ResizeImage_NullPage - returns false
TEST_F(ImageContentEditorTest, ResizeImage_NullPage) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);

    bool result = editor_->resizeImage(nullptr, doc.get(), "Im0", 300.0f, 250.0f, 50.0f, 50.0f);
    EXPECT_FALSE(result);
}

// Test 5: ResizeImage_EmptyName - returns false
TEST_F(ImageContentEditorTest, ResizeImage_EmptyName) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("resize_empty");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    bool result = editor_->resizeImage(page.get(), doc.get(), "", 300.0f, 250.0f, 50.0f, 50.0f);
    EXPECT_FALSE(result);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 6: RotateImage_Success - returns true for valid arguments
TEST_F(ImageContentEditorTest, RotateImage_Success) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("rotate_ok");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    bool result = editor_->rotateImage(page.get(), doc.get(), "Im0", 90.0f, 200.0f, 300.0f, 200.0f, 200.0f);
    EXPECT_TRUE(result);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 7: RotateImage_PreservesImageCount - image count unchanged after rotate
TEST_F(ImageContentEditorTest, RotateImage_PreservesImageCount) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("rotate_count");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    int countBefore = manager_->imageCount(page.get(), doc.get());

    editor_->rotateImage(page.get(), doc.get(), "Im0", 90.0f, 200.0f, 300.0f, 200.0f, 200.0f);

    int countAfter = manager_->imageCount(page.get(), doc.get());
    EXPECT_EQ(countBefore, countAfter);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 8: RotateImage_NullDoc - returns false
TEST_F(ImageContentEditorTest, RotateImage_NullDoc) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    bool result = editor_->rotateImage(page.get(), nullptr, "Im0", 90.0f, 200.0f, 300.0f, 200.0f, 200.0f);
    EXPECT_FALSE(result);
}

// Test 9: CropImage_Success - returns true for valid arguments
TEST_F(ImageContentEditorTest, CropImage_Success) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("crop_ok");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    QRectF cropRect(110.0, 110.0, 80.0, 80.0);
    bool result = editor_->cropImage(page.get(), doc.get(), "Im0", cropRect, 200.0f, 200.0f, 100.0f, 100.0f);
    EXPECT_TRUE(result);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 10: CropImage_PreservesImageCount - image count unchanged after crop
TEST_F(ImageContentEditorTest, CropImage_PreservesImageCount) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("crop_count");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    int countBefore = manager_->imageCount(page.get(), doc.get());

    QRectF cropRect(110.0, 110.0, 80.0, 80.0);
    editor_->cropImage(page.get(), doc.get(), "Im0", cropRect, 200.0f, 200.0f, 100.0f, 100.0f);

    int countAfter = manager_->imageCount(page.get(), doc.get());
    EXPECT_EQ(countBefore, countAfter);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 11: CropImage_EmptyName - returns false
TEST_F(ImageContentEditorTest, CropImage_EmptyName) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("crop_empty");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    QRectF cropRect(110.0, 110.0, 80.0, 80.0);
    bool result = editor_->cropImage(page.get(), doc.get(), "", cropRect, 200.0f, 200.0f, 100.0f, 100.0f);
    EXPECT_FALSE(result);

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 12: NullDoc_AllMethods - all methods return false with null doc
TEST_F(ImageContentEditorTest, NullDoc_AllMethods) {
    if (!std::filesystem::exists(pdfPath_)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath_;
    }

    auto tempPath = makeTempCopy("null_doc");
    auto doc = Document::open(context_, tempPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    EXPECT_FALSE(editor_->resizeImage(page.get(), nullptr, "Im0", 300.0f, 250.0f, 50.0f, 50.0f));
    EXPECT_FALSE(editor_->rotateImage(page.get(), nullptr, "Im0", 90.0f, 200.0f, 300.0f, 200.0f, 200.0f));

    QRectF cropRect(110.0, 110.0, 80.0, 80.0);
    EXPECT_FALSE(editor_->cropImage(page.get(), nullptr, "Im0", cropRect, 200.0f, 200.0f, 100.0f, 100.0f));

    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}
