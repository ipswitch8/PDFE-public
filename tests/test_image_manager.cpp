// PHASE1_HARDENING: skip=0 expect_ge=0 no_throw=1  (2026-05-08)
#include <gtest/gtest.h>
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QImage>
#include <filesystem>
#include <memory>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

class ImageManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);

        pdfPath = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
        pngPath = std::string(TEST_FIXTURES_DIR) + "/test_image_rgb.png";
        singlePdfPath = std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::string pdfPath;
    std::string pngPath;
    std::string singlePdfPath;
};

// Test 1: Construct - manager creation doesn't throw AND produces a
// usable instance whose context is reachable. The bare no-throw form
// would pass even if the constructor produced a half-built object.
TEST_F(ImageManagerTest, Construct) {
    auto mgr = std::make_unique<ImageManager>(context_);
    ASSERT_NE(mgr, nullptr) << "ImageManager construction returned null";
    // The manager must be usable on at least null inputs without
    // crashing — exercises the constructor's wiring beyond the
    // "didn't throw" check.
    EXPECT_NO_THROW({
        auto images = mgr->enumerateImages(nullptr, nullptr);
        EXPECT_TRUE(images.empty())
            << "enumerateImages on null page must return an empty list";
    });
}

// Test 2: EnumerateImages_PdfWithImages - finds 2 images on page 0
TEST_F(ImageManagerTest, EnumerateImages_PdfWithImages) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto images = manager_->enumerateImages(page.get(), doc.get());

    EXPECT_EQ(images.size(), 2);
}

// Test 3: EnumerateImages_ImageNames - names are "Im0" and "Im1"
TEST_F(ImageManagerTest, EnumerateImages_ImageNames) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto images = manager_->enumerateImages(page.get(), doc.get());

    ASSERT_EQ(images.size(), 2);
    EXPECT_EQ(images[0].xobjectName, "Im0");
    EXPECT_EQ(images[1].xobjectName, "Im1");
}

// Test 4: EnumerateImages_ImageDimensions - Im0 is 4x4 pixels, Im1 is 3x3 pixels
TEST_F(ImageManagerTest, EnumerateImages_ImageDimensions) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto images = manager_->enumerateImages(page.get(), doc.get());

    ASSERT_EQ(images.size(), 2);
    EXPECT_EQ(images[0].pixelWidth, 4);
    EXPECT_EQ(images[0].pixelHeight, 4);
    EXPECT_EQ(images[1].pixelWidth, 3);
    EXPECT_EQ(images[1].pixelHeight, 3);
}

// Test 5: EnumerateImages_DisplaySize - Im0 has displayWidth ~200, Im1 ~150
TEST_F(ImageManagerTest, EnumerateImages_DisplaySize) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto images = manager_->enumerateImages(page.get(), doc.get());

    ASSERT_EQ(images.size(), 2);
    EXPECT_NEAR(images[0].displayWidth, 200.0f, 5.0f);
    EXPECT_NEAR(images[0].displayHeight, 200.0f, 5.0f);
    EXPECT_NEAR(images[1].displayWidth, 150.0f, 5.0f);
    EXPECT_NEAR(images[1].displayHeight, 150.0f, 5.0f);
}

// Test 6: EnumerateImages_BoundingBox - bounding boxes are non-empty
TEST_F(ImageManagerTest, EnumerateImages_BoundingBox) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto images = manager_->enumerateImages(page.get(), doc.get());

    ASSERT_EQ(images.size(), 2);
    EXPECT_FALSE(images[0].boundingBox.isEmpty());
    EXPECT_FALSE(images[1].boundingBox.isEmpty());
    EXPECT_GT(images[0].boundingBox.width(), 0.0);
    EXPECT_GT(images[0].boundingBox.height(), 0.0);
    EXPECT_GT(images[1].boundingBox.width(), 0.0);
    EXPECT_GT(images[1].boundingBox.height(), 0.0);
}

// Test 7: EnumerateImages_NoImages - test_single.pdf has 0 images
TEST_F(ImageManagerTest, EnumerateImages_NoImages) {
    if (!std::filesystem::exists(singlePdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << singlePdfPath;
    }

    auto doc = Document::open(context_, singlePdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto images = manager_->enumerateImages(page.get(), doc.get());

    EXPECT_EQ(images.size(), 0);
}

// Test 8: GetImageInfo_Valid - returns valid info for index 0
TEST_F(ImageManagerTest, GetImageInfo_Valid) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto info = manager_->getImageInfo(page.get(), doc.get(), 0);

    EXPECT_TRUE(info.isValid());
    EXPECT_EQ(info.xobjectName, "Im0");
    EXPECT_EQ(info.pixelWidth, 4);
    EXPECT_EQ(info.pixelHeight, 4);
}

// Test 9: GetImageInfo_InvalidIndex - returns invalid (isValid() false) for out-of-range
TEST_F(ImageManagerTest, GetImageInfo_InvalidIndex) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Test negative index
    auto infoNeg = manager_->getImageInfo(page.get(), doc.get(), -1);
    EXPECT_FALSE(infoNeg.isValid());

    // Test out of range index
    auto infoOOR = manager_->getImageInfo(page.get(), doc.get(), 999);
    EXPECT_FALSE(infoOOR.isValid());
}

// Test 10: ImageCount - returns 2 for test_pdf_with_images, 0 for test_single
TEST_F(ImageManagerTest, ImageCount) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }
    if (!std::filesystem::exists(singlePdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << singlePdfPath;
    }

    auto docWithImages = Document::open(context_, pdfPath);
    ASSERT_NE(docWithImages, nullptr);
    auto pageWithImages = docWithImages->page(0);
    ASSERT_NE(pageWithImages, nullptr);

    int countWithImages = manager_->imageCount(pageWithImages.get(), docWithImages.get());
    EXPECT_EQ(countWithImages, 2);

    auto docNoImages = Document::open(context_, singlePdfPath);
    ASSERT_NE(docNoImages, nullptr);
    auto pageNoImages = docNoImages->page(0);
    ASSERT_NE(pageNoImages, nullptr);

    int countNoImages = manager_->imageCount(pageNoImages.get(), docNoImages.get());
    EXPECT_EQ(countNoImages, 0);
}

// Test 11: InsertImage_Success - insert test_image_rgb.png into test_single.pdf copy, verify count increases by 1
TEST_F(ImageManagerTest, InsertImage_Success) {
    if (!std::filesystem::exists(singlePdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << singlePdfPath;
    }
    if (!std::filesystem::exists(pngPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pngPath;
    }

    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_img_insert.pdf";

    auto doc = Document::open(context_, singlePdfPath);
    ASSERT_NE(doc, nullptr);
    doc->saveAs(tempPath);

    auto docCopy = Document::open(context_, tempPath);
    ASSERT_NE(docCopy, nullptr);

    auto page = docCopy->page(0);
    ASSERT_NE(page, nullptr);

    int initialCount = manager_->imageCount(page.get(), docCopy.get());

    InsertImageOptions opts;
    opts.filePath = pngPath;
    opts.targetRect = QRectF(100, 100, 200, 160); // 100x80 image scaled to 200x160

    auto result = manager_->insertImage(page.get(), docCopy.get(), opts);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());

    int finalCount = manager_->imageCount(page.get(), docCopy.get());
    EXPECT_EQ(finalCount, initialCount + 1);

    page.reset();
    docCopy.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 12: InsertImage_InvalidPath - non-existent file returns success=false
TEST_F(ImageManagerTest, InsertImage_InvalidPath) {
    if (!std::filesystem::exists(singlePdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << singlePdfPath;
    }

    auto doc = Document::open(context_, singlePdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = "C:\\nonexistent\\image.png";
    opts.targetRect = QRectF(100, 100, 200, 200);

    auto result = manager_->insertImage(page.get(), doc.get(), opts);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}

// Test 13: InsertImage_EmptyRect - empty target rect returns success=false
TEST_F(ImageManagerTest, InsertImage_EmptyRect) {
    if (!std::filesystem::exists(singlePdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << singlePdfPath;
    }
    if (!std::filesystem::exists(pngPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pngPath;
    }

    auto doc = Document::open(context_, singlePdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = pngPath;
    opts.targetRect = QRectF(); // Empty rect

    auto result = manager_->insertImage(page.get(), doc.get(), opts);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}

// Test 14: InsertImage_ResultInfo - inserted image result has correct pixel dimensions
TEST_F(ImageManagerTest, InsertImage_ResultInfo) {
    if (!std::filesystem::exists(singlePdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << singlePdfPath;
    }
    if (!std::filesystem::exists(pngPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pngPath;
    }

    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_img_insert_result.pdf";

    auto doc = Document::open(context_, singlePdfPath);
    ASSERT_NE(doc, nullptr);
    doc->saveAs(tempPath);

    auto docCopy = Document::open(context_, tempPath);
    ASSERT_NE(docCopy, nullptr);

    auto page = docCopy->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = pngPath;
    opts.targetRect = QRectF(100, 100, 200, 160);

    auto result = manager_->insertImage(page.get(), docCopy.get(), opts);

    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.imageInfo.isValid());
    // test_image_rgb.png is 100x80 pixels
    EXPECT_EQ(result.imageInfo.pixelWidth, 100);
    EXPECT_EQ(result.imageInfo.pixelHeight, 80);

    page.reset();
    docCopy.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 15: RemoveImage_Success - remove image 0 from test_pdf_with_images copy, verify count decreases
TEST_F(ImageManagerTest, RemoveImage_Success) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_img_remove.pdf";

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);
    doc->saveAs(tempPath);

    auto docCopy = Document::open(context_, tempPath);
    ASSERT_NE(docCopy, nullptr);

    auto page = docCopy->page(0);
    ASSERT_NE(page, nullptr);

    int initialCount = manager_->imageCount(page.get(), docCopy.get());
    EXPECT_EQ(initialCount, 2);

    auto result = manager_->removeImage(page.get(), docCopy.get(), 0);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());

    int finalCount = manager_->imageCount(page.get(), docCopy.get());
    EXPECT_EQ(finalCount, initialCount - 1);

    page.reset();
    docCopy.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 16: RemoveImage_InvalidIndex - out-of-range returns success=false
TEST_F(ImageManagerTest, RemoveImage_InvalidIndex) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Test negative index
    auto resultNeg = manager_->removeImage(page.get(), doc.get(), -1);
    EXPECT_FALSE(resultNeg.success);

    // Test out of range index
    auto resultOOR = manager_->removeImage(page.get(), doc.get(), 999);
    EXPECT_FALSE(resultOOR.success);
}

// Test 17: RemoveImage_ResultInfo - result contains info about removed image
TEST_F(ImageManagerTest, RemoveImage_ResultInfo) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_img_remove_info.pdf";

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);
    doc->saveAs(tempPath);

    auto docCopy = Document::open(context_, tempPath);
    ASSERT_NE(docCopy, nullptr);

    auto page = docCopy->page(0);
    ASSERT_NE(page, nullptr);

    auto result = manager_->removeImage(page.get(), docCopy.get(), 0);

    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.imageInfo.isValid());
    EXPECT_EQ(result.imageInfo.xobjectName, "Im0");
    EXPECT_EQ(result.imageInfo.pixelWidth, 4);
    EXPECT_EQ(result.imageInfo.pixelHeight, 4);

    page.reset();
    docCopy.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 18: ExtractImage_Valid - extract image 0 from test_pdf_with_images, verify non-null QImage
TEST_F(ImageManagerTest, ExtractImage_Valid) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    QImage img = manager_->extractImage(page.get(), doc.get(), 0);

    EXPECT_FALSE(img.isNull());
}

// Test 19: ExtractImage_Dimensions - extracted QImage has correct dimensions (4x4 for Im0)
TEST_F(ImageManagerTest, ExtractImage_Dimensions) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    QImage img = manager_->extractImage(page.get(), doc.get(), 0);

    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 4);
    EXPECT_EQ(img.height(), 4);
}

// Test 20: ExtractImage_InvalidIndex - returns null QImage for out-of-range
TEST_F(ImageManagerTest, ExtractImage_InvalidIndex) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Test negative index
    QImage imgNeg = manager_->extractImage(page.get(), doc.get(), -1);
    EXPECT_TRUE(imgNeg.isNull());

    // Test out of range index
    QImage imgOOR = manager_->extractImage(page.get(), doc.get(), 999);
    EXPECT_TRUE(imgOOR.isNull());
}

// Test 21: ExtractImageToFile - saves image to temp file, verify file exists and is non-empty
TEST_F(ImageManagerTest, ExtractImageToFile) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto tempFile = std::filesystem::temp_directory_path() / "pdfe_test_extract_img.png";

    bool result = manager_->extractImageToFile(page.get(), doc.get(), 0, tempFile.string());

    EXPECT_TRUE(result);
    EXPECT_TRUE(std::filesystem::exists(tempFile));

    if (std::filesystem::exists(tempFile)) {
        EXPECT_GT(std::filesystem::file_size(tempFile), 0);
        std::filesystem::remove(tempFile);
    }
}

// Test 22: SaveImageRef_Valid - returns non-null for existing image
TEST_F(ImageManagerTest, SaveImageRef_Valid) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    void* ref = manager_->saveImageRef(page.get(), doc.get(), "Im0");

    EXPECT_NE(ref, nullptr);

    if (ref != nullptr) {
        manager_->releaseRef(ref);
    }
}

// Test 23: SaveContentsRef_Valid - returns non-null for page with contents
TEST_F(ImageManagerTest, SaveContentsRef_Valid) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    void* ref = manager_->saveContentsRef(page.get(), doc.get());

    EXPECT_NE(ref, nullptr);

    if (ref != nullptr) {
        manager_->releaseRef(ref);
    }
}

// Test 24: ReleaseRef_Null - releasing null doesn't throw
TEST_F(ImageManagerTest, ReleaseRef_Null) {
    EXPECT_NO_THROW({
        manager_->releaseRef(nullptr);
    });
}

// Test 25: NullPageOrDoc - all methods handle null gracefully
TEST_F(ImageManagerTest, NullPageOrDoc) {
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "Test fixture not available: " << pdfPath;
    }

    auto doc = Document::open(context_, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Test null page
    auto imagesNullPage = manager_->enumerateImages(nullptr, doc.get());
    EXPECT_EQ(imagesNullPage.size(), 0);

    auto infoNullPage = manager_->getImageInfo(nullptr, doc.get(), 0);
    EXPECT_FALSE(infoNullPage.isValid());

    int countNullPage = manager_->imageCount(nullptr, doc.get());
    EXPECT_EQ(countNullPage, 0);

    // Test null document
    auto imagesNullDoc = manager_->enumerateImages(page.get(), nullptr);
    EXPECT_EQ(imagesNullDoc.size(), 0);

    auto infoNullDoc = manager_->getImageInfo(page.get(), nullptr, 0);
    EXPECT_FALSE(infoNullDoc.isValid());

    int countNullDoc = manager_->imageCount(page.get(), nullptr);
    EXPECT_EQ(countNullDoc, 0);

    // Test extract with null
    QImage imgNullPage = manager_->extractImage(nullptr, doc.get(), 0);
    EXPECT_TRUE(imgNullPage.isNull());

    QImage imgNullDoc = manager_->extractImage(page.get(), nullptr, 0);
    EXPECT_TRUE(imgNullDoc.isNull());

    // Test extractToFile with null
    auto tempFile = std::filesystem::temp_directory_path() / "pdfe_test_null_extract.png";
    bool resultNullPage = manager_->extractImageToFile(nullptr, doc.get(), 0, tempFile.string());
    EXPECT_FALSE(resultNullPage);

    bool resultNullDoc = manager_->extractImageToFile(page.get(), nullptr, 0, tempFile.string());
    EXPECT_FALSE(resultNullDoc);

    // Test insert with null
    InsertImageOptions opts;
    opts.filePath = pngPath;
    opts.targetRect = QRectF(100, 100, 200, 200);

    auto insertNullPage = manager_->insertImage(nullptr, doc.get(), opts);
    EXPECT_FALSE(insertNullPage.success);

    auto insertNullDoc = manager_->insertImage(page.get(), nullptr, opts);
    EXPECT_FALSE(insertNullDoc.success);

    // Test remove with null
    auto removeNullPage = manager_->removeImage(nullptr, doc.get(), 0);
    EXPECT_FALSE(removeNullPage.success);

    auto removeNullDoc = manager_->removeImage(page.get(), nullptr, 0);
    EXPECT_FALSE(removeNullDoc.success);
}
