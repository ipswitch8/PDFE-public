#include <gtest/gtest.h>
#include "image/ImageData.h"

using namespace pdfe::image;

// ============================================================================
// PageImageInfo Tests
// ============================================================================

class ImageDataTest : public ::testing::Test {
protected:
    PageImageInfo info;
};

TEST_F(ImageDataTest, DefaultValuesAreZeroOrEmpty) {
    // Assert that all default values are correct
    EXPECT_TRUE(info.xobjectName.empty());
    EXPECT_EQ(info.pageIndex, -1);
    EXPECT_EQ(info.imageIndex, -1);
    EXPECT_EQ(info.pixelWidth, 0);
    EXPECT_EQ(info.pixelHeight, 0);
    EXPECT_EQ(info.bitsPerComponent, 8);
    EXPECT_EQ(info.components, 3);
    EXPECT_TRUE(info.boundingBox.isEmpty());
    EXPECT_EQ(info.displayWidth, 0.0f);
    EXPECT_EQ(info.displayHeight, 0.0f);
    EXPECT_EQ(info.xDpi, 0);
    EXPECT_EQ(info.yDpi, 0);
    EXPECT_FALSE(info.isJpeg);
    EXPECT_FALSE(info.isMask);
    EXPECT_EQ(info.ctm[0], 0.0f);
    EXPECT_EQ(info.ctm[1], 0.0f);
    EXPECT_EQ(info.ctm[2], 0.0f);
    EXPECT_EQ(info.ctm[3], 0.0f);
    EXPECT_EQ(info.ctm[4], 0.0f);
    EXPECT_EQ(info.ctm[5], 0.0f);
}

TEST_F(ImageDataTest, IsValidReturnsFalseWhenEmpty) {
    // Default constructed PageImageInfo should be invalid
    EXPECT_FALSE(info.isValid());
}

TEST_F(ImageDataTest, IsValidReturnsFalseWhenOnlyNameSet) {
    // Only setting name is not sufficient
    info.xobjectName = "Im0";
    EXPECT_FALSE(info.isValid());
}

TEST_F(ImageDataTest, IsValidReturnsFalseWhenOnlyDimensionsSet) {
    // Only setting dimensions without name is not sufficient
    info.pixelWidth = 100;
    info.pixelHeight = 200;
    EXPECT_FALSE(info.isValid());
}

TEST_F(ImageDataTest, IsValidReturnsTrueWhenNameAndDimensionsSet) {
    // Valid when all required fields are set
    info.xobjectName = "Im0";
    info.pixelWidth = 100;
    info.pixelHeight = 200;
    EXPECT_TRUE(info.isValid());
}

TEST_F(ImageDataTest, PixelAreaReturnsWidthTimesHeight) {
    // Test multiplication of pixel dimensions
    info.pixelWidth = 150;
    info.pixelHeight = 250;
    EXPECT_EQ(info.pixelArea(), 37500);
}

TEST_F(ImageDataTest, PixelAreaReturnsZeroForDefaultValues) {
    // Default values should result in zero area
    EXPECT_EQ(info.pixelArea(), 0);
}

TEST_F(ImageDataTest, PixelAreaHandlesLargeValues) {
    // Test with large values to verify int64_t return type
    info.pixelWidth = 10000;
    info.pixelHeight = 10000;
    EXPECT_EQ(info.pixelArea(), 100000000);
}

TEST_F(ImageDataTest, DisplayAreaReturnsWidthTimesHeight) {
    // Test multiplication of display dimensions
    info.displayWidth = 72.0f;
    info.displayHeight = 144.0f;
    EXPECT_FLOAT_EQ(info.displayArea(), 10368.0f);
}

TEST_F(ImageDataTest, DisplayAreaReturnsZeroForDefaultValues) {
    // Default values should result in zero display area
    EXPECT_FLOAT_EQ(info.displayArea(), 0.0f);
}

TEST_F(ImageDataTest, ColorSpaceStringReturnsGrayscaleForOneComponent) {
    // 1 component = Grayscale
    info.components = 1;
    EXPECT_EQ(info.colorSpaceString(), "Grayscale");
}

TEST_F(ImageDataTest, ColorSpaceStringReturnsRGBForThreeComponents) {
    // 3 components = RGB (default)
    info.components = 3;
    EXPECT_EQ(info.colorSpaceString(), "RGB");
}

TEST_F(ImageDataTest, ColorSpaceStringReturnsCMYKForFourComponents) {
    // 4 components = CMYK
    info.components = 4;
    EXPECT_EQ(info.colorSpaceString(), "CMYK");
}

TEST_F(ImageDataTest, ColorSpaceStringReturnsUnknownForOtherValues) {
    // Any other value should return "Unknown"
    info.components = 0;
    EXPECT_EQ(info.colorSpaceString(), "Unknown");

    info.components = 2;
    EXPECT_EQ(info.colorSpaceString(), "Unknown");

    info.components = 5;
    EXPECT_EQ(info.colorSpaceString(), "Unknown");

    info.components = -1;
    EXPECT_EQ(info.colorSpaceString(), "Unknown");
}

TEST_F(ImageDataTest, CompressionStringReturnsJPEGWhenIsJpegTrue) {
    // When isJpeg is true, should return "JPEG"
    info.isJpeg = true;
    EXPECT_EQ(info.compressionString(), "JPEG");
}

TEST_F(ImageDataTest, CompressionStringReturnsLosslessWhenIsJpegFalse) {
    // When isJpeg is false (default), should return "Lossless"
    info.isJpeg = false;
    EXPECT_EQ(info.compressionString(), "Lossless");
}

// ============================================================================
// ImageTransform Tests
// ============================================================================

class ImageTransformTest : public ::testing::Test {
protected:
    ImageTransform transform;
};

TEST_F(ImageTransformTest, DefaultValuesAreIdentity) {
    // Default transform should be identity
    EXPECT_FLOAT_EQ(transform.scaleX, 1.0f);
    EXPECT_FLOAT_EQ(transform.scaleY, 1.0f);
    EXPECT_FLOAT_EQ(transform.rotation, 0.0f);
    EXPECT_FLOAT_EQ(transform.translateX, 0.0f);
    EXPECT_FLOAT_EQ(transform.translateY, 0.0f);
    EXPECT_TRUE(transform.cropRect.isEmpty());
}

TEST_F(ImageTransformTest, IsIdentityReturnsTrueForDefaultTransform) {
    // Default transform should be identity
    EXPECT_TRUE(transform.isIdentity());
}

TEST_F(ImageTransformTest, IsIdentityReturnsFalseWhenScaleXChanged) {
    // Changing scaleX should make it non-identity
    transform.scaleX = 2.0f;
    EXPECT_FALSE(transform.isIdentity());
}

TEST_F(ImageTransformTest, IsIdentityReturnsFalseWhenScaleYChanged) {
    // Changing scaleY should make it non-identity
    transform.scaleY = 0.5f;
    EXPECT_FALSE(transform.isIdentity());
}

TEST_F(ImageTransformTest, IsIdentityReturnsFalseWhenRotationChanged) {
    // Changing rotation should make it non-identity
    transform.rotation = 90.0f;
    EXPECT_FALSE(transform.isIdentity());
}

TEST_F(ImageTransformTest, IsIdentityReturnsFalseWhenTranslateXChanged) {
    // Changing translateX should make it non-identity
    transform.translateX = 10.0f;
    EXPECT_FALSE(transform.isIdentity());
}

TEST_F(ImageTransformTest, IsIdentityReturnsFalseWhenTranslateYChanged) {
    // Changing translateY should make it non-identity
    transform.translateY = -5.0f;
    EXPECT_FALSE(transform.isIdentity());
}

TEST_F(ImageTransformTest, IsIdentityReturnsFalseWhenCropRectSet) {
    // Setting a crop rectangle should make it non-identity
    transform.cropRect = QRectF(0, 0, 100, 100);
    EXPECT_FALSE(transform.isIdentity());
}

TEST_F(ImageTransformTest, HasCropReturnsFalseForDefaultTransform) {
    // Default transform has no crop
    EXPECT_FALSE(transform.hasCrop());
}

TEST_F(ImageTransformTest, HasCropReturnsTrueWhenCropRectIsNonEmpty) {
    // Setting a valid crop rectangle should return true
    transform.cropRect = QRectF(10, 10, 50, 50);
    EXPECT_TRUE(transform.hasCrop());
}

TEST_F(ImageTransformTest, HasCropReturnsFalseWhenCropRectIsEmpty) {
    // Explicitly setting an empty crop rect
    transform.cropRect = QRectF();
    EXPECT_FALSE(transform.hasCrop());
}

// ============================================================================
// InsertImageOptions Tests
// ============================================================================

class InsertImageOptionsTest : public ::testing::Test {
protected:
    InsertImageOptions options;
};

TEST_F(InsertImageOptionsTest, DefaultValuesAreCorrect) {
    // Verify all default values
    EXPECT_TRUE(options.filePath.empty());
    EXPECT_TRUE(options.targetRect.isEmpty());
    EXPECT_EQ(options.jpegQuality, 85);
    EXPECT_TRUE(options.stripExif);
    EXPECT_TRUE(options.maintainAspectRatio);
}

TEST_F(InsertImageOptionsTest, FilePathCanBeSet) {
    // Test that filePath can be assigned
    options.filePath = "/path/to/image.jpg";
    EXPECT_EQ(options.filePath, "/path/to/image.jpg");
}

TEST_F(InsertImageOptionsTest, TargetRectCanBeSet) {
    // Test that targetRect can be assigned
    options.targetRect = QRectF(0, 0, 200, 300);
    EXPECT_FALSE(options.targetRect.isEmpty());
    EXPECT_FLOAT_EQ(options.targetRect.width(), 200.0);
    EXPECT_FLOAT_EQ(options.targetRect.height(), 300.0);
}

TEST_F(InsertImageOptionsTest, JpegQualityCanBeSet) {
    // Test that jpegQuality can be changed
    options.jpegQuality = 95;
    EXPECT_EQ(options.jpegQuality, 95);
}

TEST_F(InsertImageOptionsTest, StripExifCanBeSet) {
    // Test that stripExif can be changed
    options.stripExif = false;
    EXPECT_FALSE(options.stripExif);
}

TEST_F(InsertImageOptionsTest, MaintainAspectRatioCanBeSet) {
    // Test that maintainAspectRatio can be changed
    options.maintainAspectRatio = false;
    EXPECT_FALSE(options.maintainAspectRatio);
}

// ============================================================================
// ImageOperationResult Tests
// ============================================================================

class ImageOperationResultTest : public ::testing::Test {
protected:
    ImageOperationResult result;
};

TEST_F(ImageOperationResultTest, DefaultValuesAreCorrect) {
    // Verify all default values
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());
    EXPECT_FALSE(result.imageInfo.isValid());
}

TEST_F(ImageOperationResultTest, SuccessCanBeSet) {
    // Test that success can be set to true
    result.success = true;
    EXPECT_TRUE(result.success);
}

TEST_F(ImageOperationResultTest, ErrorMessageCanBeSet) {
    // Test that errorMessage can be assigned
    result.errorMessage = "File not found";
    EXPECT_EQ(result.errorMessage, "File not found");
}

TEST_F(ImageOperationResultTest, ImageInfoCanBeSet) {
    // Test that imageInfo can be populated
    result.imageInfo.xobjectName = "Im1";
    result.imageInfo.pixelWidth = 800;
    result.imageInfo.pixelHeight = 600;

    EXPECT_TRUE(result.imageInfo.isValid());
    EXPECT_EQ(result.imageInfo.xobjectName, "Im1");
    EXPECT_EQ(result.imageInfo.pixelWidth, 800);
    EXPECT_EQ(result.imageInfo.pixelHeight, 600);
}

TEST_F(ImageOperationResultTest, SuccessfulOperationHasNoError) {
    // Successful operation should have success=true and empty error
    result.success = true;
    result.errorMessage = "";
    result.imageInfo.xobjectName = "Im2";
    result.imageInfo.pixelWidth = 1024;
    result.imageInfo.pixelHeight = 768;

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());
    EXPECT_TRUE(result.imageInfo.isValid());
}

TEST_F(ImageOperationResultTest, FailedOperationHasErrorMessage) {
    // Failed operation should have success=false and error message
    result.success = false;
    result.errorMessage = "Invalid image format";

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_EQ(result.errorMessage, "Invalid image format");
}
