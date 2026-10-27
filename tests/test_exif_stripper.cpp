// Copyright (c) 2025 PDFE Project
// Unit tests for ExifStripper class

#include <gtest/gtest.h>
#include "image/ExifStripper.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QImage>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;
using namespace pdfe::image;

class ExifStripperTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();

        // Build test fixture paths
        std::filesystem::path fixturesDir(TEST_FIXTURES_DIR);
        pdfPath_ = fixturesDir / "test_pdf_with_images.pdf";
        jpegPath_ = fixturesDir / "test_image_with_exif.jpg";
        pngPath_ = fixturesDir / "test_image_rgb.png";

        // Verify test fixtures exist
        ASSERT_TRUE(std::filesystem::exists(pdfPath_))
            << "Test fixture not found: " << pdfPath_;
        ASSERT_TRUE(std::filesystem::exists(jpegPath_))
            << "Test fixture not found: " << jpegPath_;
        ASSERT_TRUE(std::filesystem::exists(pngPath_))
            << "Test fixture not found: " << pngPath_;
    }

    void TearDown() override {
        context_.reset();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path pdfPath_;
    std::filesystem::path jpegPath_;
    std::filesystem::path pngPath_;
};

// Test 1: Constructor does not throw
TEST_F(ExifStripperTest, Construct_NoThrow) {
    EXPECT_NO_THROW({
        ExifStripper stripper(context_);
    });
}

// Test 2: Static stripExif returns non-null for valid image
TEST_F(ExifStripperTest, StripExif_Static_NonNull) {
    // Arrange
    QImage sourceImage(QString::fromStdString(jpegPath_.string()));
    ASSERT_FALSE(sourceImage.isNull()) << "Failed to load test JPEG";

    // Act
    QImage result = ExifStripper::stripExif(sourceImage);

    // Assert
    EXPECT_FALSE(result.isNull()) << "stripExif returned null image";
}

// Test 3: Static stripExif preserves dimensions
TEST_F(ExifStripperTest, StripExif_Static_PreservesDimensions) {
    // Arrange
    QImage sourceImage(QString::fromStdString(jpegPath_.string()));
    ASSERT_FALSE(sourceImage.isNull()) << "Failed to load test JPEG";
    int originalWidth = sourceImage.width();
    int originalHeight = sourceImage.height();

    // Act
    QImage result = ExifStripper::stripExif(sourceImage);

    // Assert
    EXPECT_EQ(originalWidth, result.width())
        << "Width changed after EXIF strip";
    EXPECT_EQ(originalHeight, result.height())
        << "Height changed after EXIF strip";
}

// Test 3b: Static stripExif REMOVES embedded metadata (GPS/EXIF).
// Truth oracle for the privacy guarantee + kills mutation M4 (the
// exif-strip-noop survivor). QImage::save->loadFromData PRESERVES text()
// metadata (tEXt chunks), so a naive PNG round-trip leaks embedded GPS/Make;
// stripExif must rebuild a metadata-free image. RED against the pre-fix
// round-trip-only implementation AND against the M4 mutant (return original);
// GREEN against the real strip.
TEST_F(ExifStripperTest, StripExif_Static_RemovesGps) {
    QImage img(QString::fromStdString(jpegPath_.string()));
    ASSERT_FALSE(img.isNull()) << "Failed to load test JPEG";

    // Metadata a privacy-stripper must remove (as it would arrive on a QImage
    // decoded from a source carrying GPS/camera tags).
    img.setText("Make", "TestCamera");
    img.setText("GPSLatitude", "40.7128");
    img.setText("GPSLongitude", "-74.0060");
    ASSERT_FALSE(img.text("GPSLatitude").isEmpty());

    QImage clean = ExifStripper::stripExif(img);
    ASSERT_FALSE(clean.isNull()) << "stripExif returned null";

    EXPECT_TRUE(clean.textKeys().isEmpty())
        << "stripExif left embedded metadata keys: "
        << clean.textKeys().join(",").toStdString();
    EXPECT_TRUE(clean.text("GPSLatitude").isEmpty()) << "GPS latitude survived strip";
    EXPECT_TRUE(clean.text("GPSLongitude").isEmpty()) << "GPS longitude survived strip";
    EXPECT_TRUE(clean.text("Make").isEmpty()) << "camera Make survived strip";
    // Pixels must be preserved (strip metadata, not the image).
    EXPECT_EQ(clean.size(), img.size());
}

// Test 4: Static stripExif returns null for null input
TEST_F(ExifStripperTest, StripExif_Static_NullImage) {
    // Arrange
    QImage nullImage;
    ASSERT_TRUE(nullImage.isNull()) << "Image should be null";

    // Act
    QImage result = ExifStripper::stripExif(nullImage);

    // Assert
    EXPECT_TRUE(result.isNull())
        << "stripExif should return null for null input";
}

// Test 5: stripImage returns true for valid XObject name
TEST_F(ExifStripperTest, StripImage_ValidName_ReturnsTrue) {
    // Arrange - Create temp copy to avoid file locking issues
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_strip_valid.pdf";
    {
        auto srcDoc = Document::open(context_, pdfPath_);
        srcDoc->saveAs(tempPath);
    }

    auto doc = Document::open(context_, tempPath);
    auto page = doc->page(0);
    ExifStripper stripper(context_);

    // Act - "Im0" should be first image XObject in test PDF
    bool result = stripper.stripImage(page.get(), doc.get(), "Im0");

    // Assert
    EXPECT_TRUE(result) << "stripImage failed for valid XObject name 'Im0'";

    // Cleanup
    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 6: stripImage returns false for invalid XObject name
TEST_F(ExifStripperTest, StripImage_InvalidName_ReturnsFalse) {
    // Arrange - Create temp copy
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_strip_invalid.pdf";
    {
        auto srcDoc = Document::open(context_, pdfPath_);
        srcDoc->saveAs(tempPath);
    }

    auto doc = Document::open(context_, tempPath);
    auto page = doc->page(0);
    ExifStripper stripper(context_);

    // Act - Use non-existent XObject name
    bool result = stripper.stripImage(page.get(), doc.get(), "nonexistent");

    // Assert
    EXPECT_FALSE(result)
        << "stripImage should return false for non-existent XObject";

    // Cleanup
    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 7: stripImage returns false for null page
TEST_F(ExifStripperTest, StripImage_NullPage_ReturnsFalse) {
    // Arrange - Create temp copy
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_strip_nullpage.pdf";
    {
        auto srcDoc = Document::open(context_, pdfPath_);
        srcDoc->saveAs(tempPath);
    }

    auto doc = Document::open(context_, tempPath);
    ExifStripper stripper(context_);

    // Act - Pass null page pointer
    bool result = stripper.stripImage(nullptr, doc.get(), "Im0");

    // Assert
    EXPECT_FALSE(result)
        << "stripImage should return false for null page";

    // Cleanup
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 8: stripPageImages returns correct count for page with images
TEST_F(ExifStripperTest, StripPageImages_ReturnsCount) {
    // Arrange - Create temp copy
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_strip_page.pdf";
    {
        auto srcDoc = Document::open(context_, pdfPath_);
        srcDoc->saveAs(tempPath);
    }

    auto doc = Document::open(context_, tempPath);
    auto page = doc->page(0);
    ExifStripper stripper(context_);

    // Act - test_pdf_with_images has 2 images on page 0
    int count = stripper.stripPageImages(page.get(), doc.get());

    // Assert
    EXPECT_EQ(2, count)
        << "stripPageImages should return 2 for test PDF with 2 images";

    // Cleanup
    page.reset();
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 9: stripPageImages returns 0 for null page
TEST_F(ExifStripperTest, StripPageImages_NullPage_ReturnsZero) {
    // Arrange - Create temp copy
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_strip_page_null.pdf";
    {
        auto srcDoc = Document::open(context_, pdfPath_);
        srcDoc->saveAs(tempPath);
    }

    auto doc = Document::open(context_, tempPath);
    ExifStripper stripper(context_);

    // Act - Pass null page pointer
    int count = stripper.stripPageImages(nullptr, doc.get());

    // Assert
    EXPECT_EQ(0, count)
        << "stripPageImages should return 0 for null page";

    // Cleanup
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}

// Test 10: stripAllImages returns total count for document
TEST_F(ExifStripperTest, StripAllImages_ReturnsTotal) {
    // Arrange - Create temp copy
    auto tempPath = std::filesystem::temp_directory_path() / "pdfe_test_strip_all.pdf";
    {
        auto srcDoc = Document::open(context_, pdfPath_);
        srcDoc->saveAs(tempPath);
    }

    auto doc = Document::open(context_, tempPath);
    ExifStripper stripper(context_);

    // Act - test_pdf_with_images has 1 page with 2 images = 2 total
    int count = stripper.stripAllImages(doc.get());

    // Assert
    EXPECT_EQ(2, count)
        << "stripAllImages should return 2 for single-page doc with 2 images";

    // Cleanup
    doc.reset();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
}
