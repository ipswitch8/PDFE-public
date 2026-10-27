#include <gtest/gtest.h>
#include "tre/ImageSimilarity.h"
#include <QImage>
#include <QPainter>

using namespace pdfe::tre;

// ---------------------------------------------------------------------------
// Helper: create a solid color image
// ---------------------------------------------------------------------------
static QImage makeSolidImage(int w, int h, QColor color)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(color);
    return img;
}

// Helper: create a simple black-text-on-white image
static QImage makeTextImage(int w, int h, const QString& text, int fontSizePx = 20)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter painter(&img);
    painter.setPen(Qt::black);
    QFont font("Arial", fontSizePx);
    painter.setFont(font);
    painter.drawText(img.rect(), Qt::AlignCenter, text);
    painter.end();
    return img;
}

// ---------------------------------------------------------------------------
// toGrayscale
// ---------------------------------------------------------------------------

TEST(ImageSimilarityTest, ToGrayscaleNullReturnsNull) {
    QImage null;
    QImage result = ImageSimilarity::toGrayscale(null);
    EXPECT_TRUE(result.isNull());
}

TEST(ImageSimilarityTest, ToGrayscaleConvertsARGB) {
    QImage color = makeSolidImage(10, 10, Qt::red);
    QImage gray = ImageSimilarity::toGrayscale(color);
    EXPECT_FALSE(gray.isNull());
    EXPECT_EQ(gray.format(), QImage::Format_Grayscale8);
    EXPECT_EQ(gray.width(), 10);
    EXPECT_EQ(gray.height(), 10);
}

// ---------------------------------------------------------------------------
// otsuThreshold
// ---------------------------------------------------------------------------

TEST(ImageSimilarityTest, OtsuThresholdBimodal) {
    // Create a bimodal image: left half black, right half white
    QImage img(100, 50, QImage::Format_Grayscale8);
    for (int y = 0; y < 50; ++y) {
        uchar* row = img.scanLine(y);
        for (int x = 0; x < 50; ++x) row[x] = 20;    // dark
        for (int x = 50; x < 100; ++x) row[x] = 230;  // light
    }
    int thresh = ImageSimilarity::otsuThreshold(img);
    // Should be roughly between the two modes (20 and 230)
    EXPECT_GE(thresh, 20);
    EXPECT_LE(thresh, 230);
}

// ---------------------------------------------------------------------------
// binarizedNCC
// ---------------------------------------------------------------------------

TEST(ImageSimilarityTest, NccIdenticalImagesApproxOne) {
    QImage img = makeTextImage(200, 50, "Hello World");
    float ncc = ImageSimilarity::binarizedNCC(img, img);
    EXPECT_NEAR(ncc, 1.0f, 0.01f);
}

TEST(ImageSimilarityTest, NccBlankImagesReturnZero) {
    // Two identical blank white images -- after binarization, all pixels
    // are the same value, so variance is 0, NCC returns 0.
    QImage white = makeSolidImage(100, 50, Qt::white);
    float ncc = ImageSimilarity::binarizedNCC(white, white);
    EXPECT_FLOAT_EQ(ncc, 0.0f);
}

TEST(ImageSimilarityTest, NccNullImageReturnZero) {
    QImage null;
    QImage valid = makeSolidImage(10, 10, Qt::white);
    EXPECT_FLOAT_EQ(ImageSimilarity::binarizedNCC(null, valid), 0.0f);
    EXPECT_FLOAT_EQ(ImageSimilarity::binarizedNCC(valid, null), 0.0f);
}

TEST(ImageSimilarityTest, NccDifferentSizesHandled) {
    // Two images of different sizes -- should not crash
    QImage small = makeTextImage(100, 30, "Hi");
    QImage large = makeTextImage(200, 60, "Hi");
    float ncc = ImageSimilarity::binarizedNCC(small, large);
    // Just verify it doesn't crash and returns [0,1]
    EXPECT_GE(ncc, 0.0f);
    EXPECT_LE(ncc, 1.0f);
}

// ---------------------------------------------------------------------------
// normalizedMSE
// ---------------------------------------------------------------------------

TEST(ImageSimilarityTest, MseIdenticalIsZero) {
    QImage img = makeTextImage(100, 30, "Test");
    float mse = ImageSimilarity::normalizedMSE(img, img);
    EXPECT_NEAR(mse, 0.0f, 0.001f);
}

TEST(ImageSimilarityTest, MseBlackVsWhiteIsHigh) {
    QImage black = makeSolidImage(50, 50, Qt::black);
    QImage white = makeSolidImage(50, 50, Qt::white);
    float mse = ImageSimilarity::normalizedMSE(black, white);
    EXPECT_GT(mse, 0.9f);
}

// ---------------------------------------------------------------------------
// extractAndScale
// ---------------------------------------------------------------------------

TEST(ImageSimilarityTest, ExtractAndScaleReturnsCorrectSize) {
    QImage source = makeSolidImage(200, 200, Qt::red);
    QRect region(50, 50, 100, 100);
    QSize target(64, 64);
    QImage result = ImageSimilarity::extractAndScale(source, region, target);
    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 64);
    EXPECT_EQ(result.height(), 64);
}

TEST(ImageSimilarityTest, ExtractAndScaleNullReturnsNull) {
    QImage null;
    QImage result = ImageSimilarity::extractAndScale(null, QRect(0, 0, 10, 10), QSize(5, 5));
    EXPECT_TRUE(result.isNull());
}

TEST(ImageSimilarityTest, ExtractAndScaleOutOfBoundsClamps) {
    QImage source = makeSolidImage(100, 100, Qt::blue);
    // Region extends beyond image bounds
    QRect region(80, 80, 50, 50);
    QSize target(20, 20);
    QImage result = ImageSimilarity::extractAndScale(source, region, target);
    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 20);
    EXPECT_EQ(result.height(), 20);
}
