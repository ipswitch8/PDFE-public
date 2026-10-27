#include <gtest/gtest.h>
#include "tre/BitmapCompositor.h"
#include "tre/TreTypes.h"
#include <QImage>
#include <QColor>
#include <string>
#include <filesystem>

using namespace pdfe::tre;

static const std::string kArialPath = "C:/Windows/Fonts/arial.ttf";

static bool hasArialFont() {
    return std::filesystem::exists(kArialPath);
}

static int countNonTransparentPixels(const QImage& img) {
    int count = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) > 0) {
                count++;
            }
        }
    }
    return count;
}

static bool hasColorComponent(const QImage& img, uint8_t minR, uint8_t minG, uint8_t minB) {
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            QRgb pixel = img.pixel(x, y);
            if (qAlpha(pixel) > 0) {
                if (qRed(pixel) >= minR && qGreen(pixel) >= minG && qBlue(pixel) >= minB) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool cornersAreTransparent(const QImage& img) {
    // Check all four corners
    return qAlpha(img.pixel(0, 0)) == 0 &&
           qAlpha(img.pixel(img.width() - 1, 0)) == 0 &&
           qAlpha(img.pixel(0, img.height() - 1)) == 0 &&
           qAlpha(img.pixel(img.width() - 1, img.height() - 1)) == 0;
}

class BitmapCompositorTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!hasArialFont()) {
            GTEST_SKIP() << "arial.ttf not found at " << kArialPath;
        }
    }
};

TEST_F(BitmapCompositorTest, ConstructAndDestroy) {
    // Should construct and destroy without throwing
    EXPECT_NO_THROW({
        BitmapCompositor compositor;
    });
}

TEST_F(BitmapCompositorTest, RenderSimpleText) {
    BitmapCompositor compositor;

    std::u32string text = U"Hello";
    RgbColor black{0, 0, 0};

    QImage result = compositor.renderText(text, kArialPath, 12.0f, black, 200, 100, 300);

    ASSERT_EQ(result.width(), 200);
    ASSERT_EQ(result.height(), 100);
    ASSERT_EQ(result.format(), QImage::Format_ARGB32_Premultiplied);

    // Should have some non-transparent pixels where text was rendered
    int nonTransparentPixels = countNonTransparentPixels(result);
    EXPECT_GT(nonTransparentPixels, 0) << "Rendered text should have visible pixels";
}

TEST_F(BitmapCompositorTest, TransparentBackground) {
    BitmapCompositor compositor;

    std::u32string text = U"Test";
    RgbColor black{0, 0, 0};

    QImage result = compositor.renderText(text, kArialPath, 12.0f, black, 200, 100, 300);

    // Check that corners are transparent (text shouldn't extend to edges)
    EXPECT_TRUE(cornersAreTransparent(result))
        << "Image corners should remain transparent";
}

TEST_F(BitmapCompositorTest, TextColorCorrect) {
    BitmapCompositor compositor;

    std::u32string text = U"Red";
    RgbColor red{255, 0, 0};

    QImage result = compositor.renderText(text, kArialPath, 12.0f, red, 200, 100, 300);

    // Should have pixels with red component
    bool hasRedPixels = false;
    for (int y = 0; y < result.height(); ++y) {
        for (int x = 0; x < result.width(); ++x) {
            QRgb pixel = result.pixel(x, y);
            if (qAlpha(pixel) > 0 && qRed(pixel) > 0) {
                hasRedPixels = true;
                break;
            }
        }
        if (hasRedPixels) break;
    }

    EXPECT_TRUE(hasRedPixels) << "Rendered text should have red color component";
}

TEST_F(BitmapCompositorTest, FontSizeAffectsOutput) {
    BitmapCompositor compositor;

    std::u32string text = U"Size";
    RgbColor black{0, 0, 0};

    QImage small = compositor.renderText(text, kArialPath, 8.0f, black, 400, 200, 300);
    QImage large = compositor.renderText(text, kArialPath, 24.0f, black, 400, 200, 300);

    int smallPixels = countNonTransparentPixels(small);
    int largePixels = countNonTransparentPixels(large);

    EXPECT_GT(largePixels, smallPixels)
        << "24pt text should have more pixels than 8pt text";
}

TEST_F(BitmapCompositorTest, DpiAffectsOutput) {
    BitmapCompositor compositor;

    std::u32string text = U"DPI";
    RgbColor black{0, 0, 0};

    QImage lowDpi = compositor.renderText(text, kArialPath, 12.0f, black, 400, 200, 150);
    QImage highDpi = compositor.renderText(text, kArialPath, 12.0f, black, 400, 200, 600);

    int lowDpiPixels = countNonTransparentPixels(lowDpi);
    int highDpiPixels = countNonTransparentPixels(highDpi);

    EXPECT_GT(highDpiPixels, lowDpiPixels)
        << "600 DPI should produce more pixels than 150 DPI for same font size";
}

TEST_F(BitmapCompositorTest, EmptyTextReturnsTransparent) {
    BitmapCompositor compositor;

    std::u32string emptyText = U"";
    RgbColor black{0, 0, 0};

    QImage result = compositor.renderText(emptyText, kArialPath, 12.0f, black, 200, 100, 300);

    ASSERT_EQ(result.width(), 200);
    ASSERT_EQ(result.height(), 100);

    // All pixels should be fully transparent
    int nonTransparentPixels = countNonTransparentPixels(result);
    EXPECT_EQ(nonTransparentPixels, 0) << "Empty text should produce fully transparent image";
}

TEST_F(BitmapCompositorTest, MoveConstructor) {
    BitmapCompositor compositor1;

    // Move construct
    BitmapCompositor compositor2(std::move(compositor1));

    // Moved-to compositor should still work
    std::u32string text = U"Moved";
    RgbColor black{0, 0, 0};

    QImage result = compositor2.renderText(text, kArialPath, 12.0f, black, 200, 100, 300);

    int nonTransparentPixels = countNonTransparentPixels(result);
    EXPECT_GT(nonTransparentPixels, 0) << "Moved compositor should still render text";
}

TEST_F(BitmapCompositorTest, WordWrapping) {
    BitmapCompositor compositor;

    // Very long text in a narrow target should wrap
    std::u32string longText = U"This is a very long text that should definitely wrap to multiple lines when rendered in a narrow target area.";
    RgbColor black{0, 0, 0};

    QImage result = compositor.renderText(longText, kArialPath, 12.0f, black, 150, 300, 300);

    // Check that text rendered beyond the first line by looking at vertical distribution
    bool hasPixelsInUpperHalf = false;
    bool hasPixelsInLowerHalf = false;

    int midHeight = result.height() / 2;

    for (int y = 0; y < midHeight; ++y) {
        for (int x = 0; x < result.width(); ++x) {
            if (qAlpha(result.pixel(x, y)) > 0) {
                hasPixelsInUpperHalf = true;
                break;
            }
        }
    }

    for (int y = midHeight; y < result.height(); ++y) {
        for (int x = 0; x < result.width(); ++x) {
            if (qAlpha(result.pixel(x, y)) > 0) {
                hasPixelsInLowerHalf = true;
                break;
            }
        }
    }

    EXPECT_TRUE(hasPixelsInUpperHalf) << "Should have text in upper half";
    EXPECT_TRUE(hasPixelsInLowerHalf) << "Long text should wrap to lower half";
}

TEST_F(BitmapCompositorTest, FontMatchOverload) {
    BitmapCompositor compositor;

    std::u32string text = U"Match";

    // Create FontMatch struct
    FontMatch fontMatch;
    fontMatch.fontFilePath = kArialPath;
    fontMatch.sizePt = 12.0f;
    fontMatch.color = RgbColor{0, 0, 0};

    QImage result = compositor.renderText(text, fontMatch, 200, 100, 300);

    ASSERT_EQ(result.width(), 200);
    ASSERT_EQ(result.height(), 100);

    int nonTransparentPixels = countNonTransparentPixels(result);
    EXPECT_GT(nonTransparentPixels, 0) << "FontMatch overload should render text";

    // Compare with direct call - should produce similar results
    QImage direct = compositor.renderText(text, kArialPath, 12.0f,
                                          RgbColor{0, 0, 0}, 200, 100, 300);

    int directPixels = countNonTransparentPixels(direct);

    // Allow some variance due to potential floating point differences
    EXPECT_NEAR(nonTransparentPixels, directPixels, directPixels * 0.05)
        << "FontMatch and direct calls should produce similar output";
}

// ---------------------------------------------------------------------------
// Multi-font rendering tests (Phase G)
// ---------------------------------------------------------------------------

static const std::string kArialBoldPath = "C:/Windows/Fonts/arialbd.ttf";
static const std::string kCourierNewPath = "C:/Windows/Fonts/cour.ttf";
static const std::string kTimesNewRomanPath = "C:/Windows/Fonts/times.ttf";

static bool hasMultipleFonts() {
    return std::filesystem::exists(kArialPath)
        && std::filesystem::exists(kCourierNewPath);
}

TEST_F(BitmapCompositorTest, MultiFontEmptyRunsFallsBackToSingleFont) {
    BitmapCompositor compositor;
    std::u32string text = U"Hello World";
    FontMatch match;
    match.fontFilePath = kArialPath;
    match.sizePt = 14.0f;
    match.color = {0, 0, 0};

    std::vector<FontRun> emptyRuns;
    QImage multi = compositor.renderMultiFontText(text, match, emptyRuns, 300, 100, 300);
    QImage single = compositor.renderText(text, match, 300, 100, 300);

    int multiPx = countNonTransparentPixels(multi);
    int singlePx = countNonTransparentPixels(single);

    EXPECT_GT(multiPx, 0);
    EXPECT_NEAR(multiPx, singlePx, singlePx * 0.05)
        << "Empty font runs should produce same output as single-font render";
}

TEST_F(BitmapCompositorTest, MultiFontSingleRunRendersText) {
    BitmapCompositor compositor;
    std::u32string text = U"Hello World";
    FontMatch match;
    match.fontFilePath = kArialPath;
    match.sizePt = 14.0f;
    match.color = {0, 0, 0};

    FontRun run;
    run.charStart = 0;
    run.charEnd = 11;
    run.fontFilePath = kArialPath;
    run.fontSizePt = 14.0f;
    std::vector<FontRun> runs = {run};

    QImage result = compositor.renderMultiFontText(text, match, runs, 300, 100, 300);

    ASSERT_FALSE(result.isNull());
    EXPECT_GT(countNonTransparentPixels(result), 0)
        << "Single FontRun covering entire text should produce visible output";
}

TEST_F(BitmapCompositorTest, MultiFontTwoDifferentFonts) {
    if (!hasMultipleFonts()) {
        GTEST_SKIP() << "Need arial.ttf and cour.ttf for multi-font test";
    }

    BitmapCompositor compositor;
    std::u32string text = U"Hello World";  // 11 chars
    FontMatch match;
    match.fontFilePath = kArialPath;
    match.sizePt = 14.0f;
    match.color = {0, 0, 0};

    // "Hello" in Arial, " World" in Courier New
    FontRun run1;
    run1.charStart = 0;
    run1.charEnd = 5;
    run1.fontFilePath = kArialPath;
    run1.fontSizePt = 14.0f;

    FontRun run2;
    run2.charStart = 6;
    run2.charEnd = 11;
    run2.fontFilePath = kCourierNewPath;
    run2.fontSizePt = 14.0f;

    std::vector<FontRun> runs = {run1, run2};
    QImage result = compositor.renderMultiFontText(text, match, runs, 400, 100, 300);

    ASSERT_FALSE(result.isNull());
    EXPECT_GT(countNonTransparentPixels(result), 0)
        << "Two different fonts should produce visible output";
}

TEST_F(BitmapCompositorTest, MultiFontDifferentSizes) {
    BitmapCompositor compositor;
    std::u32string text = U"BigSmall";  // 8 chars
    FontMatch match;
    match.fontFilePath = kArialPath;
    match.sizePt = 12.0f;
    match.color = {0, 0, 0};

    FontRun bigRun;
    bigRun.charStart = 0;
    bigRun.charEnd = 3;  // "Big"
    bigRun.fontFilePath = kArialPath;
    bigRun.fontSizePt = 24.0f;

    FontRun smallRun;
    smallRun.charStart = 3;
    smallRun.charEnd = 8;  // "Small"
    smallRun.fontFilePath = kArialPath;
    smallRun.fontSizePt = 8.0f;

    std::vector<FontRun> runs = {bigRun, smallRun};
    QImage result = compositor.renderMultiFontText(text, match, runs, 400, 100, 300);

    ASSERT_FALSE(result.isNull());
    EXPECT_GT(countNonTransparentPixels(result), 0)
        << "Different sizes should produce visible output";
}

TEST_F(BitmapCompositorTest, MultiFontBoldItalicStyle) {
    BitmapCompositor compositor;
    std::u32string text = U"NormalBoldItalic";  // 16 chars
    FontMatch match;
    match.fontFilePath = kArialPath;
    match.sizePt = 14.0f;
    match.color = {0, 0, 0};

    FontRun normalRun;
    normalRun.charStart = 0;
    normalRun.charEnd = 6;  // "Normal"
    normalRun.fontFilePath = kArialPath;
    normalRun.fontSizePt = 14.0f;
    normalRun.style = {};

    FontRun boldRun;
    boldRun.charStart = 6;
    boldRun.charEnd = 10;  // "Bold"
    boldRun.fontFilePath = kArialPath;
    boldRun.fontSizePt = 14.0f;
    boldRun.style.isBold = true;

    FontRun italicRun;
    italicRun.charStart = 10;
    italicRun.charEnd = 16;  // "Italic"
    italicRun.fontFilePath = kArialPath;
    italicRun.fontSizePt = 14.0f;
    italicRun.style.isItalic = true;

    std::vector<FontRun> runs = {normalRun, boldRun, italicRun};
    QImage styled = compositor.renderMultiFontText(text, match, runs, 500, 100, 300);

    // Compare with unstyled version
    FontRun plainRun;
    plainRun.charStart = 0;
    plainRun.charEnd = 16;
    plainRun.fontFilePath = kArialPath;
    plainRun.fontSizePt = 14.0f;
    plainRun.style = {};
    std::vector<FontRun> plainRuns = {plainRun};
    QImage plain = compositor.renderMultiFontText(text, match, plainRuns, 500, 100, 300);

    int styledPx = countNonTransparentPixels(styled);
    int plainPx = countNonTransparentPixels(plain);

    EXPECT_GT(styledPx, 0);
    EXPECT_GT(plainPx, 0);
    // Bold typically produces more filled pixels
    EXPECT_NE(styledPx, plainPx)
        << "Bold+italic runs should produce different pixel count from plain";
}

TEST_F(BitmapCompositorTest, MultiFontUnderlineStrikethrough) {
    BitmapCompositor compositor;
    std::u32string text = U"Hello World";
    FontMatch match;
    match.fontFilePath = kArialPath;
    match.sizePt = 14.0f;
    match.color = {0, 0, 0};

    FontRun underlineRun;
    underlineRun.charStart = 0;
    underlineRun.charEnd = 5;
    underlineRun.fontFilePath = kArialPath;
    underlineRun.fontSizePt = 14.0f;
    underlineRun.style.isUnderlined = true;

    FontRun strikeRun;
    strikeRun.charStart = 6;
    strikeRun.charEnd = 11;
    strikeRun.fontFilePath = kArialPath;
    strikeRun.fontSizePt = 14.0f;
    strikeRun.style.isStrikethrough = true;

    std::vector<FontRun> runs = {underlineRun, strikeRun};
    QImage decorated = compositor.renderMultiFontText(text, match, runs, 400, 100, 300);

    // Compare with plain
    FontRun plainRun;
    plainRun.charStart = 0;
    plainRun.charEnd = 11;
    plainRun.fontFilePath = kArialPath;
    plainRun.fontSizePt = 14.0f;
    std::vector<FontRun> plainRuns = {plainRun};
    QImage plain = compositor.renderMultiFontText(text, match, plainRuns, 400, 100, 300);

    int decoratedPx = countNonTransparentPixels(decorated);
    int plainPx = countNonTransparentPixels(plain);

    EXPECT_GT(decoratedPx, plainPx)
        << "Underline+strikethrough should produce more pixels than plain text";
}

TEST_F(BitmapCompositorTest, MultiFontGapUsesDefaultFont) {
    BitmapCompositor compositor;
    std::u32string text = U"ABCDEF";  // 6 chars
    FontMatch match;
    match.fontFilePath = kArialPath;
    match.sizePt = 14.0f;
    match.color = {0, 0, 0};

    // Only cover chars 0-2, leave 3-5 to default font
    FontRun partialRun;
    partialRun.charStart = 0;
    partialRun.charEnd = 3;
    partialRun.fontFilePath = kArialPath;
    partialRun.fontSizePt = 14.0f;

    std::vector<FontRun> runs = {partialRun};
    QImage result = compositor.renderMultiFontText(text, match, runs, 300, 100, 300);

    ASSERT_FALSE(result.isNull());
    EXPECT_GT(countNonTransparentPixels(result), 0)
        << "Gap characters should use default font and still render";
}

TEST_F(BitmapCompositorTest, MultiFontEmptyTextReturnsTransparent) {
    BitmapCompositor compositor;
    std::u32string text;
    FontMatch match;
    match.fontFilePath = kArialPath;
    match.sizePt = 14.0f;
    match.color = {0, 0, 0};

    FontRun run;
    run.charStart = 0;
    run.charEnd = 5;
    run.fontFilePath = kArialPath;
    run.fontSizePt = 14.0f;

    std::vector<FontRun> runs = {run};
    QImage result = compositor.renderMultiFontText(text, match, runs, 300, 100, 300);

    ASSERT_FALSE(result.isNull());
    EXPECT_EQ(countNonTransparentPixels(result), 0)
        << "Empty text with font runs should produce transparent image";
}
