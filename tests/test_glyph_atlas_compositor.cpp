#include <gtest/gtest.h>
#include "tre/GlyphAtlasCompositor.h"
#include "tre/GlyphAtlas.h"
#include "tre/BitmapCompositor.h"
#include "tre/TreTypes.h"
#include "fonts/FontResolver.h"
#include <QImage>

using namespace pdfe::tre;

class GlyphAtlasCompositorTest : public ::testing::Test {
protected:
    FontMatch makeDefaultFontMatch() {
        // Resolve a real Arial-or-equivalent path via FontResolver
        // so the test works on any platform (Windows: arial.ttf;
        // Linux: LiberationSans-Regular.ttf; macOS: Helvetica.ttc).
        FontMatch fm;
        fm.fontFamily = "Arial";
        fm.fontFilePath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
        fm.sizePt = 12.0f;
        fm.color = {0, 0, 0};
        fm.confidence = 0.8f;
        return fm;
    }

    GlyphAtlas makeAtlasWithChars(const std::u32string& chars) {
        // Create a test image with distinct regions for each character
        int charW = 15;
        int charH = 20;
        int imgW = static_cast<int>(chars.size()) * (charW + 5) + 10;
        QImage img(imgW, 50, QImage::Format_ARGB32);
        img.fill(QColor(255, 255, 255));

        std::vector<HocrLine> lines;
        HocrLine line;
        line.bbox = {0, 10, static_cast<double>(imgW), 30};

        HocrWord word;
        word.bbox = line.bbox;

        for (size_t i = 0; i < chars.size(); ++i) {
            int x0 = static_cast<int>(i) * (charW + 5) + 5;
            int y0 = 10;

            // Draw black pixels for the character
            for (int y = y0; y < y0 + charH; ++y) {
                for (int x = x0; x < x0 + charW; ++x) {
                    img.setPixel(x, y, qRgb(0, 0, 0));
                }
            }

            HocrChar ch;
            ch.codepoint = chars[i];
            ch.bbox = {static_cast<double>(x0), static_cast<double>(y0),
                       static_cast<double>(x0 + charW), static_cast<double>(y0 + charH)};
            ch.confidence = 90;
            word.chars.push_back(ch);
        }

        line.words.push_back(word);
        lines.push_back(line);

        GlyphAtlas atlas;
        atlas.build(img, lines, {0, 0, 0}, {255, 255, 255});
        return atlas;
    }
};

TEST_F(GlyphAtlasCompositorTest, AllCharsInAtlas) {
    auto atlas = makeAtlasWithChars(U"ABC");
    auto fm = makeDefaultFontMatch();

    GlyphAtlasCompositor compositor;
    QImage result = compositor.compose(U"ABC", atlas, fm, {255, 255, 255},
                                        200, 50, 300, nullptr);

    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 200);
    EXPECT_EQ(result.height(), 50);
}

TEST_F(GlyphAtlasCompositorTest, NoCharsInAtlasFallsBackToFreeType) {
    GlyphAtlas emptyAtlas;  // No build called, empty atlas
    auto fm = makeDefaultFontMatch();
    BitmapCompositor bc;

    GlyphAtlasCompositor compositor;
    QImage result = compositor.compose(U"XYZ", emptyAtlas, fm, {255, 255, 255},
                                        200, 50, 300, &bc);

    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 200);
    EXPECT_EQ(result.height(), 50);
}

TEST_F(GlyphAtlasCompositorTest, MixedAtlasAndFallback) {
    auto atlas = makeAtlasWithChars(U"AB");
    auto fm = makeDefaultFontMatch();
    BitmapCompositor bc;

    GlyphAtlasCompositor compositor;
    // 'A' and 'B' in atlas, 'X' falls back to FreeType
    QImage result = compositor.compose(U"AXB", atlas, fm, {255, 255, 255},
                                        200, 50, 300, &bc);

    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 200);
    EXPECT_EQ(result.height(), 50);
}

TEST_F(GlyphAtlasCompositorTest, SpacesHandled) {
    auto atlas = makeAtlasWithChars(U"AB");
    auto fm = makeDefaultFontMatch();

    GlyphAtlasCompositor compositor;
    QImage result = compositor.compose(U"A B", atlas, fm, {255, 255, 255},
                                        200, 50, 300, nullptr);

    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 200);
    EXPECT_EQ(result.height(), 50);
}

TEST_F(GlyphAtlasCompositorTest, EmptyText) {
    auto atlas = makeAtlasWithChars(U"ABC");
    auto fm = makeDefaultFontMatch();

    GlyphAtlasCompositor compositor;
    QImage result = compositor.compose(U"", atlas, fm, {255, 255, 255},
                                        200, 50, 300, nullptr);

    EXPECT_TRUE(result.isNull());
}

TEST_F(GlyphAtlasCompositorTest, OutputDimensionsMatch) {
    auto atlas = makeAtlasWithChars(U"Hi");
    auto fm = makeDefaultFontMatch();

    GlyphAtlasCompositor compositor;
    QImage result = compositor.compose(U"Hi", atlas, fm, {255, 255, 255},
                                        300, 80, 300, nullptr);

    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 300);
    EXPECT_EQ(result.height(), 80);
}

TEST_F(GlyphAtlasCompositorTest, NullCompositorStillWorks) {
    // When no BitmapCompositor is provided and chars are missing,
    // the compositor should skip missing chars gracefully
    GlyphAtlas emptyAtlas;
    auto fm = makeDefaultFontMatch();

    GlyphAtlasCompositor compositor;
    QImage result = compositor.compose(U"X", emptyAtlas, fm, {255, 255, 255},
                                        200, 50, 300, nullptr);

    // Should still produce a bitmap (transparent, with no glyphs rendered)
    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 200);
    EXPECT_EQ(result.height(), 50);
}

TEST_F(GlyphAtlasCompositorTest, NewlinesSkippedGracefully) {
    auto atlas = makeAtlasWithChars(U"AB");
    auto fm = makeDefaultFontMatch();

    GlyphAtlasCompositor compositor;
    // Text with newlines -- newlines should be skipped, not crash
    QImage result = compositor.compose(U"A\nB", atlas, fm, {255, 255, 255},
                                        200, 50, 300, nullptr);

    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 200);
    EXPECT_EQ(result.height(), 50);
}

TEST_F(GlyphAtlasCompositorTest, TabsAdvancePen) {
    auto atlas = makeAtlasWithChars(U"AB");
    auto fm = makeDefaultFontMatch();

    GlyphAtlasCompositor compositor;
    // Tab between A and B should advance pen by 4x space width
    QImage result = compositor.compose(U"A\tB", atlas, fm, {255, 255, 255},
                                        400, 50, 300, nullptr);

    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 400);
    EXPECT_EQ(result.height(), 50);
}

TEST_F(GlyphAtlasCompositorTest, FreeTypeFallbackNotClippedAtTop) {
    // Regression test: FreeType fallback used to render into a full-height canvas
    // (e.g. 80px) then position at baselineY - 80 = -20, clipping the character.
    // After the cropToContent fix, the FreeType glyph should be cropped to its
    // visible pixels and placed within the visible area (drawY >= 0).
    auto atlas = makeAtlasWithChars(U"AB");
    auto fm = makeDefaultFontMatch();
    BitmapCompositor bc;

    GlyphAtlasCompositor compositor;
    // 'X' not in atlas → FreeType fallback. Target height 80px is much taller
    // than the atlas glyphs (~20px). Without cropping, drawY would be negative.
    QImage result = compositor.compose(U"AXB", atlas, fm, {255, 255, 255},
                                        300, 80, 300, &bc);

    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 300);
    EXPECT_EQ(result.height(), 80);

    // Verify the FreeType 'X' character has visible pixels.
    // The atlas 'A' and 'B' are at baselineY=60, height=20, so drawY=40.
    // The FreeType 'X' should also be near this position after cropping.
    bool hasVisiblePixelsInMiddle = false;
    for (int y = 20; y < 70; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(result.constScanLine(y));
        for (int x = 0; x < 300; ++x) {
            if (qAlpha(line[x]) > 0) {
                hasVisiblePixelsInMiddle = true;
                break;
            }
        }
        if (hasVisiblePixelsInMiddle) break;
    }
    EXPECT_TRUE(hasVisiblePixelsInMiddle) << "FreeType fallback character should have visible pixels in the middle region";

    // Verify no visible pixels in top rows (would indicate the old clipping bug)
    bool hasPixelsInTopClipZone = false;
    for (int y = 0; y < 5; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(result.constScanLine(y));
        for (int x = 0; x < 300; ++x) {
            if (qAlpha(line[x]) > 0) {
                hasPixelsInTopClipZone = true;
                break;
            }
        }
        if (hasPixelsInTopClipZone) break;
    }
    EXPECT_FALSE(hasPixelsInTopClipZone) << "No characters should render in the top 5 rows";
}

TEST_F(GlyphAtlasCompositorTest, FreeTypeFallbackWidthReasonable) {
    // The FreeType fallback character should advance penX by a reasonable amount
    // (the cropped width), not by the full oversized canvas width.
    auto atlas = makeAtlasWithChars(U"AB");
    auto fm = makeDefaultFontMatch();
    BitmapCompositor bc;

    GlyphAtlasCompositor compositor;

    // Compare: AXB (mixed) vs AB (atlas only)
    QImage mixed = compositor.compose(U"AXB", atlas, fm, {255, 255, 255},
                                       300, 50, 300, &bc);
    QImage atlasOnly = compositor.compose(U"AB", atlas, fm, {255, 255, 255},
                                           300, 50, 300, nullptr);

    EXPECT_FALSE(mixed.isNull());
    EXPECT_FALSE(atlasOnly.isNull());

    // Find rightmost visible pixel in each
    auto findRightmost = [](const QImage& img) -> int {
        int rightmost = -1;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = img.width() - 1; x >= 0; --x) {
                if (qAlpha(line[x]) > 0) {
                    rightmost = std::max(rightmost, x);
                    break;
                }
            }
        }
        return rightmost;
    };

    int mixedRight = findRightmost(mixed);
    int atlasRight = findRightmost(atlasOnly);

    // AXB should be wider than AB (X adds width), but not excessively so
    EXPECT_GT(mixedRight, atlasRight) << "AXB should be wider than AB";
    // The X shouldn't be wider than 3 atlas char widths (reasonable bound)
    EXPECT_LT(mixedRight, atlasRight + 15 * 3) << "FreeType X width should be reasonable";
}

TEST_F(GlyphAtlasCompositorTest, ZeroDimensionsReturnNull) {
    auto atlas = makeAtlasWithChars(U"A");
    auto fm = makeDefaultFontMatch();

    GlyphAtlasCompositor compositor;

    QImage result1 = compositor.compose(U"A", atlas, fm, {255, 255, 255},
                                         0, 50, 300, nullptr);
    EXPECT_TRUE(result1.isNull());

    QImage result2 = compositor.compose(U"A", atlas, fm, {255, 255, 255},
                                         200, 0, 300, nullptr);
    EXPECT_TRUE(result2.isNull());
}
