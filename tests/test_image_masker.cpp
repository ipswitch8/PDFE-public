#include <gtest/gtest.h>
#include "tre/ImageMasker.h"
#include "tre/TreTypes.h"
#include <QImage>
#include <QColor>

using namespace pdfe::tre;

namespace {

// Helper to create a solid-color QImage
QImage createSolidImage(int width, int height, const QColor& color) {
    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(color);
    return img;
}

// Helper to check if a pixel is approximately equal to a color (with tolerance)
bool isColorApprox(const QColor& actual, const QColor& expected, int tolerance = 5) {
    return std::abs(actual.red() - expected.red()) <= tolerance &&
           std::abs(actual.green() - expected.green()) <= tolerance &&
           std::abs(actual.blue() - expected.blue()) <= tolerance;
}

// Helper to count pixels in a region that match a color (approximately)
int countMatchingPixels(const QImage& img, const PixelRect& region,
                        const QColor& color, int tolerance = 5) {
    int count = 0;
    int x0 = static_cast<int>(region.x0);
    int y0 = static_cast<int>(region.y0);
    int x1 = static_cast<int>(region.x1);
    int y1 = static_cast<int>(region.y1);

    for (int y = y0; y < y1 && y < img.height(); ++y) {
        for (int x = x0; x < x1 && x < img.width(); ++x) {
            QColor pixelColor(img.pixel(x, y));
            if (isColorApprox(pixelColor, color, tolerance)) {
                count++;
            }
        }
    }
    return count;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test: MaskRegionSolidFill
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, MaskRegionSolidFill) {
    ImageMasker masker;

    // Create a red 100x100 image
    QImage source = createSolidImage(100, 100, QColor(255, 0, 0));

    // Mask center 40x40 region with white
    PixelRect region{30, 30, 70, 70};
    RgbColor white{255, 255, 255};

    QImage result = masker.maskRegion(source, region, white, 1);

    // Check that center pixels are approximately white
    // (with feathering, they might not be exactly white)
    QColor centerPixel(result.pixel(50, 50));
    EXPECT_NEAR(centerPixel.red(), 255, 20);
    EXPECT_NEAR(centerPixel.green(), 255, 20);
    EXPECT_NEAR(centerPixel.blue(), 255, 20);

    // Check that a pixel well inside the masked region is white
    int whiteCount = countMatchingPixels(result, PixelRect{40, 40, 60, 60}, QColor(255, 255, 255), 10);
    EXPECT_GT(whiteCount, 350); // Most of the 20x20 center area should be white
}

// ---------------------------------------------------------------------------
// Test: MaskRegionPreservesOutside
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, MaskRegionPreservesOutside) {
    ImageMasker masker;

    // Create a blue 100x100 image
    QImage source = createSolidImage(100, 100, QColor(0, 0, 255));

    // Mask center region with green
    PixelRect region{30, 30, 70, 70};
    RgbColor green{0, 255, 0};

    QImage result = masker.maskRegion(source, region, green, 1);

    // Check corners are still blue (outside masked region)
    QColor topLeft(result.pixel(5, 5));
    QColor topRight(result.pixel(95, 5));
    QColor bottomLeft(result.pixel(5, 95));
    QColor bottomRight(result.pixel(95, 95));

    EXPECT_TRUE(isColorApprox(topLeft, QColor(0, 0, 255), 5));
    EXPECT_TRUE(isColorApprox(topRight, QColor(0, 0, 255), 5));
    EXPECT_TRUE(isColorApprox(bottomLeft, QColor(0, 0, 255), 5));
    EXPECT_TRUE(isColorApprox(bottomRight, QColor(0, 0, 255), 5));
}

// ---------------------------------------------------------------------------
// Test: FeatheredEdgesBlended
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, FeatheredEdgesBlended) {
    ImageMasker masker;

    // Create a black 100x100 image
    QImage source = createSolidImage(100, 100, QColor(0, 0, 0));

    // Mask center region with white, with feathering
    PixelRect region{30, 30, 70, 70};
    RgbColor white{255, 255, 255};

    QImage result = masker.maskRegion(source, region, white, 3);

    // Check edge pixel (should be blended, not pure black or white)
    QColor edgePixel(result.pixel(30, 50)); // Left edge of masked region
    EXPECT_GT(edgePixel.red(), 20);    // Not pure black
    EXPECT_LT(edgePixel.red(), 235);   // Not pure white
    EXPECT_GT(edgePixel.green(), 20);
    EXPECT_LT(edgePixel.green(), 235);
    EXPECT_GT(edgePixel.blue(), 20);
    EXPECT_LT(edgePixel.blue(), 235);
}

// ---------------------------------------------------------------------------
// Test: BoxBlurSinglePass
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, BoxBlurSinglePass) {
    // Create a 50x50 grayscale image with a white rectangle in the center
    QImage mask(50, 50, QImage::Format_Grayscale8);
    mask.fill(0);

    // Fill center 20x20 with white
    for (int y = 15; y < 35; ++y) {
        uchar* line = mask.scanLine(y);
        for (int x = 15; x < 35; ++x) {
            line[x] = 255;
        }
    }

    // Apply single blur pass
    QImage blurred = ImageMasker::boxBlur(mask, 1);

    // Check that edge pixels are slightly blurred
    uchar edgeValue = blurred.constScanLine(15)[15]; // Top-left corner of white rect
    EXPECT_GT(edgeValue, 0);   // Not pure black
    EXPECT_LT(edgeValue, 255); // Not pure white

    // Center should still be mostly white
    uchar centerValue = blurred.constScanLine(25)[25];
    EXPECT_GT(centerValue, 200);
}

// ---------------------------------------------------------------------------
// Test: BoxBlurThreePasses
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, BoxBlurThreePasses) {
    // Create a 50x50 grayscale image with a white rectangle in the center
    QImage mask(50, 50, QImage::Format_Grayscale8);
    mask.fill(0);

    // Fill center 20x20 with white
    for (int y = 15; y < 35; ++y) {
        uchar* line = mask.scanLine(y);
        for (int x = 15; x < 35; ++x) {
            line[x] = 255;
        }
    }

    // Apply three blur passes
    QImage blurred = ImageMasker::boxBlur(mask, 3);

    // Edge should be more blurred than single pass
    uchar edgeValue = blurred.constScanLine(15)[15];
    EXPECT_GT(edgeValue, 0);
    EXPECT_LT(edgeValue, 200); // Should be more blurred than single pass

    // Check a pixel further from the edge
    uchar nearEdge = blurred.constScanLine(17)[17];
    EXPECT_GT(nearEdge, edgeValue); // Should be brighter than corner
}

// ---------------------------------------------------------------------------
// Test: GradientMaskFill
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, GradientMaskFill) {
    ImageMasker masker;

    // Create a black 100x100 image
    QImage source = createSolidImage(100, 100, QColor(0, 0, 0));

    // Define a gradient: red (TL), green (TR), blue (BL), white (BR)
    GradientInfo gradient;
    gradient.topLeft = RgbColor{255, 0, 0};      // Red
    gradient.topRight = RgbColor{0, 255, 0};     // Green
    gradient.bottomLeft = RgbColor{0, 0, 255};   // Blue
    gradient.bottomRight = RgbColor{255, 255, 255}; // White

    PixelRect region{20, 20, 80, 80};

    QImage result = masker.maskRegionGradient(source, region, gradient, 1);

    // Check center pixel (should be a mix of all four colors)
    QColor centerPixel(result.pixel(50, 50));
    // Center should have components from all corners
    EXPECT_GT(centerPixel.red(), 50);
    EXPECT_GT(centerPixel.green(), 50);
    EXPECT_GT(centerPixel.blue(), 50);

    // Check top-left corner of region (should be closer to red)
    QColor tlPixel(result.pixel(25, 25));
    EXPECT_GT(tlPixel.red(), 150); // Mostly red
}

// ---------------------------------------------------------------------------
// Test: MaskAllRegionsMultiple
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, MaskAllRegionsMultiple) {
    ImageMasker masker;

    // Create a blue 200x200 image
    QImage source = createSolidImage(200, 200, QColor(0, 0, 255));

    // Define two non-overlapping regions
    std::vector<std::pair<PixelRect, BackgroundInfo>> regions;

    BackgroundInfo bg1;
    bg1.dominantColor = RgbColor{255, 0, 0}; // Red
    bg1.isUniform = true;
    bg1.hasGradient = false;
    regions.emplace_back(PixelRect{10, 10, 50, 50}, bg1);

    BackgroundInfo bg2;
    bg2.dominantColor = RgbColor{0, 255, 0}; // Green
    bg2.isUniform = true;
    bg2.hasGradient = false;
    regions.emplace_back(PixelRect{100, 100, 140, 140}, bg2);

    QImage result = masker.maskAllRegions(source, regions);

    // Check first region is red
    QColor region1Pixel(result.pixel(30, 30));
    EXPECT_TRUE(isColorApprox(region1Pixel, QColor(255, 0, 0), 20));

    // Check second region is green
    QColor region2Pixel(result.pixel(120, 120));
    EXPECT_TRUE(isColorApprox(region2Pixel, QColor(0, 255, 0), 20));

    // Check area between regions is still blue
    QColor betweenPixel(result.pixel(60, 60));
    EXPECT_TRUE(isColorApprox(betweenPixel, QColor(0, 0, 255), 10));
}

// ---------------------------------------------------------------------------
// Test: CompositeTextBitmapAlpha
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, CompositeTextBitmapAlpha) {
    ImageMasker masker;

    // Create a blue 100x100 source image
    QImage source = createSolidImage(100, 100, QColor(0, 0, 255));

    // Create a semi-transparent red 40x40 text bitmap (non-premultiplied format)
    QImage textBitmap(40, 40, QImage::Format_ARGB32);
    textBitmap.fill(qRgba(255, 0, 0, 128)); // Semi-transparent red

    // Composite at position (30, 30)
    PixelRect region{30, 30, 70, 70};
    RgbColor white{255, 255, 255};

    QImage result = masker.compositeTextBitmap(source, textBitmap, region, white);

    // The region should first be filled with white, then red blended over it
    // So we expect a pink-ish color (white background + semi-transparent red)
    QColor centerPixel(result.pixel(50, 50));

    // Should have red component from the text
    EXPECT_GT(centerPixel.red(), 200);
    // Should have some white showing through
    EXPECT_GT(centerPixel.green(), 100);
    EXPECT_GT(centerPixel.blue(), 100);
}

// ---------------------------------------------------------------------------
// Test: CompositePreservesOutside
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, CompositePreservesOutside) {
    ImageMasker masker;

    // Create a yellow 100x100 source image
    QImage source = createSolidImage(100, 100, QColor(255, 255, 0));

    // Create a red 20x20 text bitmap
    QImage textBitmap(20, 20, QImage::Format_ARGB32);
    textBitmap.fill(qRgba(255, 0, 0, 255));

    // Composite at position (40, 40)
    PixelRect region{40, 40, 60, 60};
    RgbColor white{255, 255, 255};

    QImage result = masker.compositeTextBitmap(source, textBitmap, region, white);

    // Check corners are still yellow (outside composited region)
    QColor topLeft(result.pixel(5, 5));
    QColor bottomRight(result.pixel(95, 95));

    EXPECT_TRUE(isColorApprox(topLeft, QColor(255, 255, 0), 5));
    EXPECT_TRUE(isColorApprox(bottomRight, QColor(255, 255, 0), 5));
}

// ---------------------------------------------------------------------------
// Test: BoundsClampingNoOverflow
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, BoundsClampingNoOverflow) {
    ImageMasker masker;

    // Create a 100x100 image
    QImage source = createSolidImage(100, 100, QColor(128, 128, 128));

    // Region extends 50 pixels past image edge
    PixelRect region{80, 80, 150, 150};
    RgbColor red{255, 0, 0};

    // Should not crash
    QImage result = masker.maskRegion(source, region, red, 2);

    EXPECT_EQ(result.width(), 100);
    EXPECT_EQ(result.height(), 100);

    // Check that bottom-right corner is red-ish (masked)
    QColor cornerPixel(result.pixel(95, 95));
    EXPECT_GT(cornerPixel.red(), 200);
}

// ---------------------------------------------------------------------------
// Test: EmptyRegionNoOp
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, EmptyRegionNoOp) {
    ImageMasker masker;

    // Create a green 50x50 image
    QImage source = createSolidImage(50, 50, QColor(0, 255, 0));

    // Empty region (zero area)
    PixelRect emptyRegion{0, 0, 0, 0};
    RgbColor red{255, 0, 0};

    QImage result = masker.maskRegion(source, emptyRegion, red, 2);

    // Should return unchanged image (all pixels still green)
    for (int y = 0; y < result.height(); ++y) {
        for (int x = 0; x < result.width(); ++x) {
            QColor pixel(result.pixel(x, y));
            EXPECT_TRUE(isColorApprox(pixel, QColor(0, 255, 0), 5));
        }
    }
}

// ---------------------------------------------------------------------------
// Test: LargeImagePerformance
// ---------------------------------------------------------------------------

TEST(ImageMaskerTest, LargeImagePerformance) {
    ImageMasker masker;

    // Create a large 2550x3300 image (typical scanned page at 300 DPI)
    QImage source = createSolidImage(2550, 3300, QColor(240, 240, 240));

    // Mask a region in the center
    PixelRect region{1000, 1500, 1550, 1800};
    RgbColor white{255, 255, 255};

    // Should complete without timeout
    QImage result = masker.maskRegion(source, region, white, 3);

    EXPECT_EQ(result.width(), 2550);
    EXPECT_EQ(result.height(), 3300);

    // Verify masking actually happened
    QColor centerPixel(result.pixel(1275, 1650));
    EXPECT_NEAR(centerPixel.red(), 255, 20);
}
