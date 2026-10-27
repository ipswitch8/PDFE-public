#include <gtest/gtest.h>

#include "tre/BgColorDetector.h"
#include "tre/TreTypes.h"

#include <QColor>
#include <QImage>
#include <cstdlib>

using namespace pdfe::tre;

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

/// Create a solid-color test image.
static QImage createSolidImage(int width, int height, QColor color) {
    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(color);
    return img;
}

/// Create a vertical gradient image (top color to bottom color).
static QImage createVerticalGradient(int width, int height, QColor topColor, QColor bottomColor) {
    QImage img(width, height, QImage::Format_ARGB32);

    for (int y = 0; y < height; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(height - 1);

        const int r = static_cast<int>(topColor.red() * (1.0f - t) + bottomColor.red() * t);
        const int g = static_cast<int>(topColor.green() * (1.0f - t) + bottomColor.green() * t);
        const int b = static_cast<int>(topColor.blue() * (1.0f - t) + bottomColor.blue() * t);

        const QColor lineColor(r, g, b);

        for (int x = 0; x < width; ++x) {
            img.setPixelColor(x, y, lineColor);
        }
    }

    return img;
}

/// Create a horizontal gradient image (left color to right color).
static QImage createHorizontalGradient(int width, int height, QColor leftColor, QColor rightColor) {
    QImage img(width, height, QImage::Format_ARGB32);

    for (int x = 0; x < width; ++x) {
        const float t = static_cast<float>(x) / static_cast<float>(width - 1);

        const int r = static_cast<int>(leftColor.red() * (1.0f - t) + rightColor.red() * t);
        const int g = static_cast<int>(leftColor.green() * (1.0f - t) + rightColor.green() * t);
        const int b = static_cast<int>(leftColor.blue() * (1.0f - t) + rightColor.blue() * t);

        const QColor colColor(r, g, b);

        for (int y = 0; y < height; ++y) {
            img.setPixelColor(x, y, colColor);
        }
    }

    return img;
}

/// Create a noisy image with deterministic random colors.
static QImage createNoisyImage(int width, int height, unsigned int seed = 42) {
    QImage img(width, height, QImage::Format_ARGB32);
    std::srand(seed);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int r = std::rand() % 256;
            const int g = std::rand() % 256;
            const int b = std::rand() % 256;
            img.setPixelColor(x, y, QColor(r, g, b));
        }
    }

    return img;
}

/// Create an image with horizontal black bands (simulating text lines) and white gaps.
static QImage createTextLinesImage(int width, int height,
                                  const std::vector<int>& lineTops,
                                  const std::vector<int>& lineHeights) {
    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(Qt::white);

    for (size_t i = 0; i < lineTops.size() && i < lineHeights.size(); ++i) {
        const int y0 = lineTops[i];
        const int y1 = y0 + lineHeights[i];

        for (int y = y0; y < y1 && y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                img.setPixelColor(x, y, Qt::black);
            }
        }
    }

    return img;
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

TEST(BgColorDetectorTest, DetectWhiteBackground) {
    BgColorDetector detector;

    const QImage img = createSolidImage(200, 200, Qt::white);
    const PixelRect region{10, 10, 190, 190};

    const BackgroundInfo result = detector.detect(img, region);

    EXPECT_TRUE(result.isUniform);
    EXPECT_FALSE(result.hasGradient);
    EXPECT_FALSE(result.isComplex);
    EXPECT_EQ(result.dominantColor.r, 255);
    EXPECT_EQ(result.dominantColor.g, 255);
    EXPECT_EQ(result.dominantColor.b, 255);
    EXPECT_GT(result.confidence, 0.5f);
}

TEST(BgColorDetectorTest, DetectColoredBackground) {
    BgColorDetector detector;

    const QImage img = createSolidImage(200, 200, QColor(100, 150, 200));
    const PixelRect region{10, 10, 190, 190};

    const BackgroundInfo result = detector.detect(img, region);

    EXPECT_TRUE(result.isUniform);
    EXPECT_FALSE(result.hasGradient);
    EXPECT_FALSE(result.isComplex);
    EXPECT_EQ(result.dominantColor.r, 100);
    EXPECT_EQ(result.dominantColor.g, 150);
    EXPECT_EQ(result.dominantColor.b, 200);
    EXPECT_GT(result.confidence, 0.5f);
}

TEST(BgColorDetectorTest, DetectVerticalGradientBackground) {
    BgColorDetector detector;

    const QImage img = createVerticalGradient(200, 200, Qt::white, QColor(128, 128, 128));
    const PixelRect region{10, 10, 190, 190};

    const BackgroundInfo result = detector.detect(img, region);

    EXPECT_FALSE(result.isUniform);
    EXPECT_TRUE(result.hasGradient);
    EXPECT_FALSE(result.isComplex);
    EXPECT_GT(result.confidence, 0.0f);
}

TEST(BgColorDetectorTest, DetectHorizontalGradientBackground) {
    BgColorDetector detector;

    const QImage img = createHorizontalGradient(200, 200, Qt::white, QColor(128, 128, 128));
    const PixelRect region{10, 10, 190, 190};

    const BackgroundInfo result = detector.detect(img, region);

    EXPECT_FALSE(result.isUniform);
    EXPECT_TRUE(result.hasGradient);
    EXPECT_FALSE(result.isComplex);
}

TEST(BgColorDetectorTest, DetectComplexBackground) {
    BgColorDetector detector;

    const QImage img = createNoisyImage(200, 200, 42);
    const PixelRect region{10, 10, 190, 190};

    const BackgroundInfo result = detector.detect(img, region);

    EXPECT_FALSE(result.isUniform);
    EXPECT_FALSE(result.hasGradient);
    EXPECT_TRUE(result.isComplex);
}

TEST(BgColorDetectorTest, InterLineGapSampling) {
    BgColorDetector detector;

    // Create image with black horizontal bands (simulating text) and white gaps
    const std::vector<int> lineTops = {20, 60, 100, 140};
    const std::vector<int> lineHeights = {15, 15, 15, 15};
    const QImage img = createTextLinesImage(200, 200, lineTops, lineHeights);

    const PixelRect region{0, 0, 200, 200};

    // Create HocrLine objects representing the text lines
    std::vector<HocrLine> lines;
    for (size_t i = 0; i < lineTops.size() && i < lineHeights.size(); ++i) {
        HocrLine line;
        line.bbox.x0 = 0;
        line.bbox.y0 = lineTops[i];
        line.bbox.x1 = 200;
        line.bbox.y1 = lineTops[i] + lineHeights[i];
        lines.push_back(line);
    }

    const BackgroundInfo result = detector.detect(img, region, lines);

    // Should detect white background from the gaps, not black from the text lines
    EXPECT_TRUE(result.isUniform);
    EXPECT_FALSE(result.hasGradient);
    EXPECT_FALSE(result.isComplex);

    // Dominant color should be white or very close to white
    EXPECT_GT(result.dominantColor.r, 200);
    EXPECT_GT(result.dominantColor.g, 200);
    EXPECT_GT(result.dominantColor.b, 200);

    // Confidence should be higher due to gap sampling
    EXPECT_GT(result.confidence, 0.5f);
}

TEST(BgColorDetectorTest, EdgeSamplingOnly) {
    BgColorDetector detector;

    const QImage img = createSolidImage(200, 200, QColor(100, 150, 200));
    const PixelRect region{10, 10, 190, 190};

    // Use the two-argument overload (no lines)
    const BackgroundInfo result = detector.detect(img, region);

    EXPECT_TRUE(result.isUniform);
    EXPECT_EQ(result.dominantColor.r, 100);
    EXPECT_EQ(result.dominantColor.g, 150);
    EXPECT_EQ(result.dominantColor.b, 200);
    EXPECT_GT(result.confidence, 0.0f);
}

TEST(BgColorDetectorTest, EmptyRegionReturnsDefault) {
    BgColorDetector detector;

    const QImage img = createSolidImage(200, 200, Qt::white);
    const PixelRect emptyRegion{50, 50, 50, 50}; // Zero-area region

    const BackgroundInfo result = detector.detect(img, emptyRegion);

    // Should return reasonable defaults without crashing
    EXPECT_TRUE(result.isUniform);
    EXPECT_EQ(result.dominantColor.r, 255);
    EXPECT_EQ(result.dominantColor.g, 255);
    EXPECT_EQ(result.dominantColor.b, 255);
    EXPECT_EQ(result.confidence, 0.0f);
}

TEST(BgColorDetectorTest, ConfidenceHighWithGaps) {
    BgColorDetector detector;

    const QImage img = createSolidImage(200, 200, Qt::white);
    const PixelRect region{0, 0, 200, 200};

    // Create HocrLines with gaps
    std::vector<HocrLine> lines;
    for (int i = 0; i < 5; ++i) {
        HocrLine line;
        line.bbox.x0 = 0;
        line.bbox.y0 = i * 40;
        line.bbox.x1 = 200;
        line.bbox.y1 = i * 40 + 15;
        lines.push_back(line);
    }

    const BackgroundInfo resultWithGaps = detector.detect(img, region, lines);
    const BackgroundInfo resultEdgeOnly = detector.detect(img, region);

    // Confidence should be higher with gap sampling
    EXPECT_GT(resultWithGaps.confidence, resultEdgeOnly.confidence);
}

TEST(BgColorDetectorTest, ConfidenceLowEdgeOnly) {
    BgColorDetector detector;

    const QImage img = createSolidImage(200, 200, Qt::white);
    const PixelRect region{10, 10, 190, 190};

    const BackgroundInfo result = detector.detect(img, region);

    // Edge-only should still have reasonable confidence, but not maximum
    EXPECT_GT(result.confidence, 0.3f);
    EXPECT_LT(result.confidence, 1.0f);
}

TEST(BgColorDetectorTest, SmallRegionHandled) {
    BgColorDetector detector;

    const QImage img = createSolidImage(200, 200, Qt::white);
    const PixelRect smallRegion{50, 50, 60, 60}; // 10x10 pixel region

    const BackgroundInfo result = detector.detect(img, smallRegion);

    // Should not crash and should produce valid result
    EXPECT_TRUE(result.isUniform);
    EXPECT_EQ(result.dominantColor.r, 255);
    EXPECT_EQ(result.dominantColor.g, 255);
    EXPECT_EQ(result.dominantColor.b, 255);
}

TEST(BgColorDetectorTest, RegionClampedToImage) {
    BgColorDetector detector;

    const QImage img = createSolidImage(200, 200, QColor(100, 150, 200));

    // Region extends past image bounds
    const PixelRect oversizedRegion{-50, -50, 300, 300};

    const BackgroundInfo result = detector.detect(img, oversizedRegion);

    // Should not crash and should clamp to valid image area
    EXPECT_TRUE(result.isUniform);
    EXPECT_EQ(result.dominantColor.r, 100);
    EXPECT_EQ(result.dominantColor.g, 150);
    EXPECT_EQ(result.dominantColor.b, 200);
}

TEST(BgColorDetectorTest, NearWhiteIsUniform) {
    BgColorDetector detector;

    // Create image with off-white color (within uniformity threshold)
    const QImage img = createSolidImage(200, 200, QColor(250, 252, 248));
    const PixelRect region{10, 10, 190, 190};

    const BackgroundInfo result = detector.detect(img, region);

    // Should still be classified as uniform
    EXPECT_TRUE(result.isUniform);
    EXPECT_FALSE(result.hasGradient);
    EXPECT_FALSE(result.isComplex);

    // Color should be close to the off-white
    EXPECT_GE(result.dominantColor.r, 245);
    EXPECT_GE(result.dominantColor.g, 245);
    EXPECT_GE(result.dominantColor.b, 245);
}
