#include <gtest/gtest.h>

#include "tre/FontMatcher.h"
#include "tre/TreTypes.h"
#include "tre/CoordMapper.h"
#include "tre/BitmapCompositor.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "core/MuPdfContext.h"

#include <QImage>
#include <QColor>
#include <QRect>
#include <cmath>
#include <filesystem>
#include <numeric>

using namespace pdfe::tre;

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

/// Lowercased basename-ish view of a resolved font path, for family matching.
static std::string lowerPath(const std::string& p) {
    std::string out = p;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

/// Does the resolved path name a face from a known SANS family (Arial or one
/// of its metric-compatible substitutes)?
static bool namesSansFamily(const std::string& lower) {
    return lower.find("arial") != std::string::npos
        || lower.find("liberationsans") != std::string::npos
        || lower.find("arimo") != std::string::npos
        || lower.find("dejavusans") != std::string::npos
        || lower.find("nimbussans") != std::string::npos;
}

/// Does the resolved path name a face from a known SERIF family?
static bool namesSerifFamily(const std::string& lower) {
    return lower.find("times") != std::string::npos
        || lower.find("liberationserif") != std::string::npos
        || lower.find("tinos") != std::string::npos
        || lower.find("dejavuserif") != std::string::npos
        || lower.find("nimbusroman") != std::string::npos
        || lower.find("georgia") != std::string::npos
        || lower.find("garamond") != std::string::npos
        || lower.find("cambria") != std::string::npos;
}

/// Create a test region with specified metrics.
static RegionTextData makeRegionData(float xSizePx, float descPx, float ascPx,
                                      float baselineSlope, int wordConf) {
    RegionTextData region;

    // Create a single line with the specified metrics
    HocrLine line;
    line.bbox = PixelRect{0, 0, 100, static_cast<double>(xSizePx + descPx + ascPx)};
    line.baselineSlope = baselineSlope;
    line.baselineOffset = 0.0f;
    line.xSizePx = xSizePx;
    line.descendersPx = descPx;
    line.ascendersPx = ascPx;

    // Add a test word
    HocrWord word;
    word.text = "test";
    word.bbox = PixelRect{0, 0, 50, static_cast<double>(xSizePx + descPx + ascPx)};
    word.confidence = wordConf;
    word.fontSizePx = xSizePx;

    line.words.push_back(word);
    region.lines.push_back(line);

    region.plainText = "test";
    region.pixelBbox = PixelRect{0, 0, 100, static_cast<double>(xSizePx + descPx + ascPx)};
    region.pdfBbox = PdfRect{0, 0, 100, static_cast<double>(xSizePx + descPx + ascPx)};

    return region;
}

/// Create a test image with text rectangles.
static QImage createTextImage(int w, int h, QColor bgColor, QColor textColor,
                               const std::vector<QRect>& textRects) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(bgColor);

    for (const auto& rect : textRects) {
        for (int y = rect.top(); y < rect.bottom() && y < h; ++y) {
            for (int x = rect.left(); x < rect.right() && x < w; ++x) {
                img.setPixelColor(x, y, textColor);
            }
        }
    }

    return img;
}

/// Create a test region with realistic baseline data.
/// The baselineOffset is relative to the BOTTOM of the line bbox (y1), as per
/// the HOCR spec (negative means baseline is above the bottom).
static RegionTextData makeRegionWithBaseline(float xSizePx, float descPx, float ascPx,
                                              float baselineOffset, float baselineSlope,
                                              float lineY0, float lineHeight,
                                              const std::string& text = "test") {
    RegionTextData region;

    HocrLine line;
    line.bbox = PixelRect{0, lineY0, 100, lineY0 + lineHeight};
    line.baselineSlope = baselineSlope;
    line.baselineOffset = baselineOffset;
    line.xSizePx = xSizePx;
    line.descendersPx = descPx;
    line.ascendersPx = ascPx;

    HocrWord word;
    word.text = text;
    word.bbox = PixelRect{0, lineY0 + 2, 50, lineY0 + lineHeight - 2};
    word.confidence = 90;
    word.fontSizePx = xSizePx;

    line.words.push_back(word);
    region.lines.push_back(line);

    region.plainText = text;
    region.pixelBbox = line.bbox;
    region.pdfBbox = PdfRect{0, 0, 100, lineHeight};

    return region;
}

/// Create a multi-line region for line spacing tests.
static RegionTextData makeMultiLineRegion(const std::vector<float>& lineYPositions,
                                           float xSizePx, float descPx, float ascPx) {
    RegionTextData region;

    for (float yPos : lineYPositions) {
        HocrLine line;
        line.bbox = PixelRect{0, yPos, 100, yPos + xSizePx + descPx + ascPx};
        line.xSizePx = xSizePx;
        line.descendersPx = descPx;
        line.ascendersPx = ascPx;
        line.baselineSlope = 0.0f;
        line.baselineOffset = 0.0f;

        HocrWord word;
        word.text = "line";
        word.bbox = PixelRect{0, yPos, 50, yPos + xSizePx + descPx + ascPx};
        word.confidence = 90;
        word.fontSizePx = xSizePx;

        line.words.push_back(word);
        region.lines.push_back(line);
    }

    region.plainText = "line\nline\nline";
    region.pixelBbox = PixelRect{0, 0, 100, 200};
    region.pdfBbox = PdfRect{0, 0, 100, 200};

    return region;
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

TEST(FontMatcherTest, DetectPropertiesBasicText) {
    // Setup: xSizePx=10, descPx=3, ascPx=12, at 300 DPI
    // Tesseract x_size is the row height (≈ em-square) in pixels.
    // fontSizePt = x_size * 72 / dpi = 10 * 72 / 300 = 2.4
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    const RegionTextData region = makeRegionData(10.0f, 3.0f, 12.0f, 0.0f, 90);
    const QImage img = createTextImage(100, 100, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // Font size: fontSizePt = x_size * 72 / dpi = 10 * 72 / 300 = 2.4
    EXPECT_NEAR(props.fontSizePt, 2.4f, 0.1f);

    // x-height ratio = 10 / (10 + 3 + 12) = 10/25 = 0.4
    EXPECT_NEAR(props.xHeightRatio, 0.4f, 0.01f);

    // Weight should be Light (0.4 is in the Light range: 0.38-0.42)
    EXPECT_EQ(props.weight, FontWeight::Light);

    // Not italic (slope = 0)
    EXPECT_FALSE(props.isItalic);

    // Default to sans-serif
    EXPECT_FALSE(props.isSerif);
}

TEST(FontMatcherTest, FontSizeFromXSize) {
    // Test the font size conversion formula explicitly.
    // Tesseract x_size is the row height (≈ em-square) in pixels.
    // fontSizePt = x_size * 72 / dpi.
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Test case 1: xSizePx=20, DPI=300
    // fontSizePt = 20 * 72 / 300 = 4.8
    {
        const RegionTextData region = makeRegionData(20.0f, 5.0f, 25.0f, 0.0f, 90);
        const QImage img = createTextImage(100, 100, Qt::white, Qt::black, {});
        const ScannedTextProperties props = matcher.detectProperties(region, img, 300);
        EXPECT_NEAR(props.fontSizePt, 4.8f, 0.1f);
    }

    // Test case 2: xSizePx=24, DPI=150
    // fontSizePt = 24 * 72 / 150 = 11.52
    {
        const RegionTextData region = makeRegionData(24.0f, 6.0f, 30.0f, 0.0f, 90);
        const QImage img = createTextImage(100, 100, Qt::white, Qt::black, {});
        const ScannedTextProperties props = matcher.detectProperties(region, img, 150);
        EXPECT_NEAR(props.fontSizePt, 11.52f, 0.1f);
    }
}

TEST(FontMatcherTest, DetectTextColorBlack) {
    // Create white image with black rectangles at word positions
    const std::vector<QRect> textRects = {
        QRect(10, 10, 40, 20),
        QRect(60, 10, 40, 20),
        QRect(10, 40, 40, 20)
    };

    const QImage img = createTextImage(150, 100, Qt::white, Qt::black, textRects);

    std::vector<HocrWord> words;
    for (const auto& rect : textRects) {
        HocrWord word;
        word.text = "test";
        word.bbox = PixelRect{
            static_cast<double>(rect.left()),
            static_cast<double>(rect.top()),
            static_cast<double>(rect.right()),
            static_cast<double>(rect.bottom())
        };
        word.confidence = 90;
        words.push_back(word);
    }

    const RgbColor color = FontMatcher::detectTextColor(img, words);

    // Should detect black (or near-black)
    EXPECT_LT(color.r, 50);
    EXPECT_LT(color.g, 50);
    EXPECT_LT(color.b, 50);
}

TEST(FontMatcherTest, DetectTextColorRed) {
    // Create white image with red rectangles at word positions
    const std::vector<QRect> textRects = {
        QRect(10, 10, 40, 20),
        QRect(60, 10, 40, 20)
    };

    const QImage img = createTextImage(150, 100, Qt::white, Qt::red, textRects);

    std::vector<HocrWord> words;
    for (const auto& rect : textRects) {
        HocrWord word;
        word.text = "test";
        word.bbox = PixelRect{
            static_cast<double>(rect.left()),
            static_cast<double>(rect.top()),
            static_cast<double>(rect.right()),
            static_cast<double>(rect.bottom())
        };
        word.confidence = 90;
        words.push_back(word);
    }

    const RgbColor color = FontMatcher::detectTextColor(img, words);

    // Should detect red (high R, low G and B)
    EXPECT_GT(color.r, 200);
    EXPECT_LT(color.g, 50);
    EXPECT_LT(color.b, 50);
}

TEST(FontMatcherTest, DetectTextColorEmptyWords) {
    // No words provided
    const QImage img = createTextImage(150, 100, Qt::white, Qt::black, {});
    const std::vector<HocrWord> words;

    const RgbColor color = FontMatcher::detectTextColor(img, words);

    // Should return black as default
    EXPECT_EQ(color.r, 0);
    EXPECT_EQ(color.g, 0);
    EXPECT_EQ(color.b, 0);
}

TEST(FontMatcherTest, EstimateWeightRegular) {
    // xHeightRatio = 0.45 should yield Regular (400)
    const FontWeight weight = FontMatcher::estimateWeight(0.45f);
    EXPECT_EQ(weight, FontWeight::Regular);
}

TEST(FontMatcherTest, EstimateWeightBold) {
    // xHeightRatio = 0.58 should yield Bold (700)
    const FontWeight weight = FontMatcher::estimateWeight(0.58f);
    EXPECT_EQ(weight, FontWeight::Bold);
}

TEST(FontMatcherTest, EstimateWeightSemiBold) {
    // xHeightRatio = 0.52 should yield SemiBold (600)
    const FontWeight weight = FontMatcher::estimateWeight(0.52f);
    EXPECT_EQ(weight, FontWeight::SemiBold);
}

TEST(FontMatcherTest, EstimateWeightLight) {
    // xHeightRatio = 0.40 should yield Light (300)
    const FontWeight weight = FontMatcher::estimateWeight(0.40f);
    EXPECT_EQ(weight, FontWeight::Light);
}

TEST(FontMatcherTest, EstimateWeightThin) {
    // xHeightRatio = 0.35 should yield Thin (100)
    const FontWeight weight = FontMatcher::estimateWeight(0.35f);
    EXPECT_EQ(weight, FontWeight::Thin);
}

TEST(FontMatcherTest, FindMatchReturnsFontPath) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false;
    props.textColor = RgbColor{0, 0, 0};
    props.lineSpacingRatio = 1.2f;

    const std::u32string text = U"Test text";

    const FontMatch match = matcher.findMatch(props, text);

    // Should return a non-empty font path (either from selector or fallback)
    EXPECT_FALSE(match.fontFilePath.empty());
    EXPECT_GT(match.sizePt, 0.0f);
}

TEST(FontMatcherTest, FindMatchConfidenceRange) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false;
    props.textColor = RgbColor{0, 0, 0};

    const std::u32string text = U"ABC";

    const FontMatch match = matcher.findMatch(props, text);

    // Confidence should be in [0.0, 1.0]
    EXPECT_GE(match.confidence, 0.0f);
    EXPECT_LE(match.confidence, 1.0f);
}

TEST(FontMatcherTest, FindMatchSansFont) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false; // Sans-serif
    props.textColor = RgbColor{0, 0, 0};

    const std::u32string text = U"Sans text";

    const FontMatch match = matcher.findMatch(props, text);

    // Should return a font path (fallback to arial.ttf or
    // metric-compatible Linux substitute if no better match)
    EXPECT_FALSE(match.fontFilePath.empty());

    // GitHub #273: this was gated on `if (match.confidence < 0.5f)`, so only
    // the FALLBACK outcome was ever checked. On any host where the resolver
    // found a good sans match (confidence >= 0.5) the test asserted nothing
    // at all about the path it returned — which is most developer machines.
    //
    // Both outcomes are specifiable, so both are now asserted. The request was
    // for a SANS face (props.isSerif = false), and that is the property that
    // must hold either way.
    const std::string lower = lowerPath(match.fontFilePath);
    if (match.confidence < 0.5f) {
        EXPECT_TRUE(namesSansFamily(lower))
            << "Sans fallback should resolve to Arial or a metric-compatible "
               "substitute, got: " << match.fontFilePath;
    } else {
        // A high-confidence match may be any installed sans face (Segoe UI,
        // Calibri, ...), so the family cannot be enumerated — but resolving a
        // SERIF face for a sans request is wrong at any confidence.
        EXPECT_FALSE(namesSerifFamily(lower))
            << "a sans-serif request resolved to a serif face at confidence "
            << match.confidence << ": " << match.fontFilePath;
    }
}

TEST(FontMatcherTest, FindMatchSerifFont) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = true; // Serif
    props.textColor = RgbColor{0, 0, 0};

    const std::u32string text = U"Serif text";

    const FontMatch match = matcher.findMatch(props, text);

    // Should return a font path (fallback to times.ttf or
    // metric-compatible Linux substitute if no better match)
    EXPECT_FALSE(match.fontFilePath.empty());

    // GitHub #273 — the serif mirror of the sans case above. Only the fallback
    // branch was checked; a high-confidence match asserted nothing. The
    // request was for a SERIF face, and that holds either way.
    const std::string lower = lowerPath(match.fontFilePath);
    if (match.confidence < 0.5f) {
        EXPECT_TRUE(namesSerifFamily(lower))
            << "Serif fallback should resolve to Times New Roman or a "
               "metric-compatible substitute, got: " << match.fontFilePath;
    } else {
        // A high-confidence serif match may be any installed serif face, so
        // the family cannot be enumerated — but resolving a face from the
        // known SANS list for a serif request is wrong at any confidence.
        EXPECT_FALSE(namesSansFamily(lower))
            << "a serif request resolved to a sans face at confidence "
            << match.confidence << ": " << match.fontFilePath;
    }
}

TEST(FontMatcherTest, MatchRegionEndToEnd) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Create test data
    const RegionTextData region = makeRegionData(10.0f, 3.0f, 12.0f, 0.0f, 90);
    const std::vector<QRect> textRects = {QRect(0, 0, 50, 25)};
    const QImage img = createTextImage(100, 100, Qt::white, Qt::black, textRects);
    const int dpi = 300;

    const FontMatch match = matcher.matchRegion(region, img, dpi);

    // Verify basic results
    EXPECT_FALSE(match.fontFilePath.empty());
    EXPECT_GT(match.sizePt, 0.0f);
    EXPECT_GE(match.confidence, 0.0f);
    EXPECT_LE(match.confidence, 1.0f);

    // Verify detected properties: fontSizePt = x_size * 72 / dpi = 10 * 72 / 300 = 2.4
    EXPECT_NEAR(match.detectedProperties.fontSizePt, 2.4f, 0.1f);
    EXPECT_EQ(match.detectedProperties.weight, FontWeight::Light);
    EXPECT_FALSE(match.detectedProperties.isItalic);
}

TEST(FontMatcherTest, MatchRegionEmptyData) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Empty region data
    RegionTextData region;
    region.plainText = "";

    const QImage img = createTextImage(100, 100, Qt::white, Qt::black, {});
    const int dpi = 300;

    const FontMatch match = matcher.matchRegion(region, img, dpi);

    // Should gracefully handle empty data and return a fallback
    EXPECT_FALSE(match.fontFilePath.empty());
    EXPECT_GT(match.sizePt, 0.0f); // Should use default font size
}

TEST(FontMatcherTest, FallbackToArial) {
    // Test that when selector returns nullopt, we fallback to arial.ttf
    // This test may need to skip if arial.ttf is not available

    const std::filesystem::path arialPath("C:/Windows/Fonts/arial.ttf");
    if (!std::filesystem::exists(arialPath)) {
        GTEST_SKIP() << "arial.ttf not found at " << arialPath;
    }

    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false;
    props.textColor = RgbColor{0, 0, 0};

    // Use obscure text that's unlikely to be matched by system fonts
    const std::u32string text = U"";

    const FontMatch match = matcher.findMatch(props, text);

    // If confidence is low, it should be a fallback.
    // Compare case-insensitively because the production code now
    // queries DirectWrite (via FontResolver) and DirectWrite returns
    // the Windows-canonical uppercase path "C:\WINDOWS\FONTS\ARIAL.TTF"
    // rather than the lowercase string we used to hardcode. NTFS is
    // case-insensitive so either form is equally valid.
    // GitHub #273: gated on low confidence, so the Arial expectation never
    // ran on a host that resolved a confident match. This test is named
    // FallbackToArial and the request is for a sans face, so both outcomes
    // are specifiable.
    const std::string lower = lowerPath(match.fontFilePath);
    if (match.confidence < 0.5f) {
        EXPECT_NE(lower.find("arial"), std::string::npos)
            << "Expected an Arial-family fallback path, got: "
            << match.fontFilePath;
    } else {
        EXPECT_FALSE(namesSerifFamily(lower))
            << "a sans request resolved to a serif face at confidence "
            << match.confidence << ": " << match.fontFilePath;
    }
}

TEST(FontMatcherTest, LineSpacingDetection) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Create region with two lines at y=0 and y=30
    // xSizePx=10, so spacing ratio = 30/10 = 3.0
    const std::vector<float> lineYPositions = {0.0f, 30.0f};
    const RegionTextData region = makeMultiLineRegion(lineYPositions, 10.0f, 3.0f, 12.0f);

    const QImage img = createTextImage(100, 100, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // Expected spacing ratio = 30 / 10 = 3.0
    EXPECT_NEAR(props.lineSpacingRatio, 3.0f, 0.1f);
}

TEST(FontMatcherTest, LineSpacingSingleLine) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Single line - should default to 1.2
    const RegionTextData region = makeRegionData(10.0f, 3.0f, 12.0f, 0.0f, 90);
    const QImage img = createTextImage(100, 100, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // Should default to 1.2 for single line
    EXPECT_NEAR(props.lineSpacingRatio, 1.2f, 0.01f);
}

TEST(FontMatcherTest, ItalicDetection) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Create region with italic slope > 0.02
    const RegionTextData region = makeRegionData(10.0f, 3.0f, 12.0f, 0.15f, 90);
    const QImage img = createTextImage(100, 100, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // Should detect as italic
    EXPECT_TRUE(props.isItalic);
}

TEST(FontMatcherTest, NonItalicDetection) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Create region with small slope < 0.02
    const RegionTextData region = makeRegionData(10.0f, 3.0f, 12.0f, 0.01f, 90);
    const QImage img = createTextImage(100, 100, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // Should not detect as italic
    EXPECT_FALSE(props.isItalic);
}

// ---------------------------------------------------------------------------
// Baseline-to-ascender averaging tests
// ---------------------------------------------------------------------------

TEST(FontMatcherTest, BaselineAveragingWithRealBaseline) {
    // Simulate a realistic HOCR line at 300 DPI.
    // Line bbox: y0=200, y1=240 (height=40px)
    // Baseline offset: -7 (relative to y1), so baseline_y = 240-7 = 233
    // Ascender height: 233 - 200 = 33px
    // Em from baseline: 33 / 0.78 = 42.31px
    // Baseline estimate: 42.31 * 72 / 300 = 10.15pt
    //
    // x_size = 28.5 (from Tesseract row fitting)
    // x_size estimate: 28.5 * 72 / 300 = 6.84pt
    //
    // Average: (6.84 + 10.15) / 2 ≈ 8.50pt

    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    const RegionTextData region = makeRegionWithBaseline(
        28.5f,  // xSizePx
        5.5f,   // descPx
        7.0f,   // ascPx
        -7.0f,  // baselineOffset (negative = above bbox bottom)
        0.002f, // baselineSlope
        200.0f, // lineY0
        40.0f   // lineHeight
    );

    const QImage img = createTextImage(200, 300, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // x_size estimate alone would be 6.84pt.
    // With baseline averaging, should be higher.
    EXPECT_GT(props.fontSizePt, 7.0f);

    // The average of x_size (6.84) and baseline (~10.15) should be ~8.5
    EXPECT_NEAR(props.fontSizePt, 8.50f, 0.5f);
}

TEST(FontMatcherTest, BaselineAveragingSkippedForZeroOffset) {
    // When baselineOffset=0, the baseline ends up at bbox bottom (y1),
    // making ascender = full line height. This fails the validity check
    // (>95% of line height), so only x_size is used.

    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Same as existing test setup with baselineOffset=0
    const RegionTextData region = makeRegionData(10.0f, 3.0f, 12.0f, 0.0f, 90);
    const QImage img = createTextImage(100, 100, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // Should use x_size only: 10 * 72 / 300 = 2.4
    EXPECT_NEAR(props.fontSizePt, 2.4f, 0.1f);
}

TEST(FontMatcherTest, BaselineAveragingSkippedForCJK) {
    // CJK text should skip baseline averaging because CJK doesn't have
    // a meaningful baseline concept. Only x_size should be used.

    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Create region with meaningful baseline data but CJK text
    RegionTextData region = makeRegionWithBaseline(
        28.5f,          // xSizePx
        5.5f,           // descPx
        7.0f,           // ascPx
        -7.0f,          // baselineOffset
        0.0f,           // baselineSlope
        200.0f,         // lineY0
        40.0f,          // lineHeight
        "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95" // "中文测试" in UTF-8
    );

    const QImage img = createTextImage(200, 300, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // Should use x_size only: 28.5 * 72 / 300 = 6.84
    EXPECT_NEAR(props.fontSizePt, 6.84f, 0.1f);
}

TEST(FontMatcherTest, BaselineAveragingMultipleLines) {
    // Test with two lines having different baseline data.
    // The baseline estimates should be averaged across lines, then
    // averaged with the x_size estimate.

    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    RegionTextData region;
    region.plainText = "Hello World\nSecond line";

    // Line 1: bbox y0=100, y1=140 (h=40), baseline offset=-8
    // baseline_y = 140 + (-8) = 132, ascender = 132 - 100 = 32
    // em = 32/0.78 = 41.03, pt = 41.03 * 72 / 300 = 9.85
    HocrLine line1;
    line1.bbox = PixelRect{0, 100, 200, 140};
    line1.baselineSlope = 0.0f;
    line1.baselineOffset = -8.0f;
    line1.xSizePx = 28.0f;
    line1.descendersPx = 5.0f;
    line1.ascendersPx = 7.0f;
    HocrWord word1;
    word1.text = "Hello World";
    word1.bbox = PixelRect{0, 102, 100, 138};
    word1.confidence = 90;
    line1.words.push_back(word1);
    region.lines.push_back(line1);

    // Line 2: bbox y0=160, y1=200 (h=40), baseline offset=-6
    // baseline_y = 200 + (-6) = 194, ascender = 194 - 160 = 34
    // em = 34/0.78 = 43.59, pt = 43.59 * 72 / 300 = 10.46
    HocrLine line2;
    line2.bbox = PixelRect{0, 160, 200, 200};
    line2.baselineSlope = 0.0f;
    line2.baselineOffset = -6.0f;
    line2.xSizePx = 29.0f;
    line2.descendersPx = 5.0f;
    line2.ascendersPx = 7.0f;
    HocrWord word2;
    word2.text = "Second line";
    word2.bbox = PixelRect{0, 162, 100, 198};
    word2.confidence = 90;
    line2.words.push_back(word2);
    region.lines.push_back(line2);

    region.pixelBbox = PixelRect{0, 100, 200, 200};
    region.pdfBbox = PdfRect{0, 0, 200, 100};

    const QImage img = createTextImage(200, 300, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // x_size avg: (28+29)/2 = 28.5 → 28.5*72/300 = 6.84pt
    // baseline avg: (9.85+10.46)/2 = 10.16pt
    // combined: (6.84 + 10.16) / 2 = 8.50
    EXPECT_NEAR(props.fontSizePt, 8.50f, 0.5f);
}

TEST(FontMatcherTest, BaselineAveragingDegenerateBaselineSkipped) {
    // When the baseline offset would put the ascender at <30% of line
    // height, the baseline estimate is rejected as degenerate.

    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Degenerate: offset=-1 (baseline almost at bbox bottom)
    // baseline_y = 240 + (-1) = 239, ascender = 239-200 = 39
    // 39 / 40 = 97.5% of line height → exceeds 95% threshold → skipped
    const RegionTextData region = makeRegionWithBaseline(
        28.5f, 5.5f, 7.0f,
        -1.0f,  // Almost at bbox bottom
        0.0f, 200.0f, 40.0f
    );

    const QImage img = createTextImage(200, 300, Qt::white, Qt::black, {});
    const int dpi = 300;

    const ScannedTextProperties props = matcher.detectProperties(region, img, dpi);

    // Should fall back to x_size only: 28.5 * 72 / 300 = 6.84
    EXPECT_NEAR(props.fontSizePt, 6.84f, 0.1f);
}

// ---------------------------------------------------------------------------
// Phase 2: Enriched query profile tests
// ---------------------------------------------------------------------------

TEST(FontMatcherQueryEnrichment, XToCapRatioFromDetectedProperties) {
    // When xHeightRatio and ascenderRatio are set, findMatch should
    // produce a query profile with non-zero xToCapRatio.
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false;
    props.xHeightRatio = 0.5f;
    props.ascenderRatio = 0.7f;
    props.textColor = {0, 0, 0};

    // Expected: approxCap = 0.5 + (0.7 - 0.5) = 0.7
    // xToCapRatio = 0.5 / 0.7 ≈ 0.714
    const std::u32string text = U"Hello";
    FontMatch result = matcher.findMatch(props, text);

    // Verify the match returned something (font found)
    EXPECT_FALSE(result.fontFilePath.empty());
    // The enriched profile was used internally -- we can't inspect it directly,
    // but the match should succeed since Arial covers "Hello"
    EXPECT_GT(result.confidence, 0.0f);
}

TEST(FontMatcherQueryEnrichment, ItalicSetsNonZeroAngle) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = FontWeight::Regular;
    props.isItalic = true;
    props.isSerif = false;
    props.textColor = {0, 0, 0};

    const std::u32string text = U"Hello";
    FontMatch result = matcher.findMatch(props, text);

    // Verify a match was found
    EXPECT_FALSE(result.fontFilePath.empty());
    EXPECT_GT(result.confidence, 0.0f);
}

// ---------------------------------------------------------------------------
// Phase 6: Visual re-ranking integration tests
// ---------------------------------------------------------------------------

TEST(FontMatcherVisualRanking, NoCompositorSkipsVisualRanking) {
    // Without setting a compositor, matchRegion should use metric-only path
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);
    // Do NOT call setBitmapCompositor

    RegionTextData region = makeRegionData(28.5f, 5.5f, 7.0f, 0.0f, 95);
    const QImage img = createTextImage(200, 300, Qt::white, Qt::black, {});

    FontMatch result = matcher.matchRegion(region, img, 300);
    EXPECT_FALSE(result.fontFilePath.empty());
    EXPECT_GT(result.confidence, 0.0f);
}

TEST(FontMatcherVisualRanking, WithCompositorAlwaysUsesVisualRanking) {
    // Visual ranking runs unconditionally when compositor is set
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    BitmapCompositor compositor;
    matcher.setBitmapCompositor(&compositor);

    // Use a known-pair font (Helvetica → Arial) which gets high confidence
    RegionTextData region = makeRegionData(28.5f, 5.5f, 7.0f, 0.0f, 95);
    const QImage img = createTextImage(200, 300, Qt::white, Qt::black, {});

    FontMatch result = matcher.matchRegion(region, img, 300);
    EXPECT_FALSE(result.fontFilePath.empty());
    // Should succeed regardless of visual ranking path
    EXPECT_GT(result.confidence, 0.0f);
}

TEST(FontMatcherVisualRanking, WithCompositorAndLowConfidence) {
    // This tests the full visual re-ranking path
    if (!std::filesystem::exists("C:\\Windows\\Fonts\\arial.ttf")) {
        GTEST_SKIP() << "Arial not available";
    }

    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    BitmapCompositor compositor;
    matcher.setBitmapCompositor(&compositor);

    // Create region data with a non-standard font name to force system font scoring
    // (not a known pair, so confidence will be lower)
    RegionTextData region = makeRegionData(28.5f, 5.5f, 7.0f, 0.0f, 95);
    const QImage img = createTextImage(200, 300, Qt::white, Qt::black, {});

    FontMatch result = matcher.matchRegion(region, img, 300);
    EXPECT_FALSE(result.fontFilePath.empty());
    // Should still find a valid font
    EXPECT_GT(result.confidence, 0.0f);
}

// ---------------------------------------------------------------------------
// buildQueryProfile weight-class mapping tests (Karen QA - zero coverage)
//
// buildQueryProfile() is a public static that maps ScannedTextProperties into
// a FontMetricProfile used by distanceTo(). The weightClass field must map
// FontWeight enum values to OS/2 usWeightClass integers so that distanceTo()
// weight penalties are meaningful.
// ---------------------------------------------------------------------------

TEST(FontMatcherBuildQueryProfile, WeightClass_Thin_Maps100) {
    ScannedTextProperties props;
    props.weight = FontWeight::Thin;
    props.isItalic = false;
    props.isSerif = false;

    auto profile = FontMatcher::buildQueryProfile(props, U"abc");

    EXPECT_EQ(profile.weightClass, static_cast<int>(FontWeight::Thin));
}

TEST(FontMatcherBuildQueryProfile, WeightClass_Light_Maps300) {
    ScannedTextProperties props;
    props.weight = FontWeight::Light;
    props.isItalic = false;
    props.isSerif = false;

    auto profile = FontMatcher::buildQueryProfile(props, U"abc");

    EXPECT_EQ(profile.weightClass, static_cast<int>(FontWeight::Light));
}

TEST(FontMatcherBuildQueryProfile, WeightClass_Regular_Maps400) {
    ScannedTextProperties props;
    props.weight = FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false;

    auto profile = FontMatcher::buildQueryProfile(props, U"abc");

    EXPECT_EQ(profile.weightClass, static_cast<int>(FontWeight::Regular));
}

TEST(FontMatcherBuildQueryProfile, WeightClass_SemiBold_Maps600) {
    ScannedTextProperties props;
    props.weight = FontWeight::SemiBold;
    props.isItalic = false;
    props.isSerif = false;

    auto profile = FontMatcher::buildQueryProfile(props, U"abc");

    EXPECT_EQ(profile.weightClass, static_cast<int>(FontWeight::SemiBold));
}

TEST(FontMatcherBuildQueryProfile, WeightClass_Bold_Maps700) {
    ScannedTextProperties props;
    props.weight = FontWeight::Bold;
    props.isItalic = false;
    props.isSerif = false;

    auto profile = FontMatcher::buildQueryProfile(props, U"abc");

    EXPECT_EQ(profile.weightClass, static_cast<int>(FontWeight::Bold));
}

// distanceTo() normalises weight class difference by /500; confirm the
// penalty between Regular(400) and Bold(700) is proportional and non-zero.
TEST(FontMatcherBuildQueryProfile, WeightClassDistance_RegularVsBold_IsProportional) {
    using pdfe::fonts::FontMetricProfile;

    ScannedTextProperties regularProps;
    regularProps.weight = FontWeight::Regular;

    ScannedTextProperties boldProps;
    boldProps.weight = FontWeight::Bold;

    auto regularProfile = FontMatcher::buildQueryProfile(regularProps, U"abc");
    auto boldProfile    = FontMatcher::buildQueryProfile(boldProps,    U"abc");

    // Make all other fields identical so only weightClass differs
    // (both profiles already have zeroed continuous metrics from buildQueryProfile)
    float dist = regularProfile.distanceTo(boldProfile);

    // Expected: diff = (700-400)/500 = 0.6; contribution = 10 * 0.6^2 = 3.6
    // sqrt(3.6) ≈ 1.897
    EXPECT_NEAR(dist, std::sqrt(3.6f), 0.01f);
    EXPECT_GT(dist, 0.0f);
}

// Regular vs SemiBold should produce a smaller distance than Regular vs Bold.
TEST(FontMatcherBuildQueryProfile, WeightClassDistance_RegularVsSemiBold_LessThanBold) {
    using pdfe::fonts::FontMetricProfile;

    ScannedTextProperties regularProps;
    regularProps.weight = FontWeight::Regular;

    ScannedTextProperties semiBoldProps;
    semiBoldProps.weight = FontWeight::SemiBold;

    ScannedTextProperties boldProps;
    boldProps.weight = FontWeight::Bold;

    auto regularProfile  = FontMatcher::buildQueryProfile(regularProps,  U"abc");
    auto semiBoldProfile = FontMatcher::buildQueryProfile(semiBoldProps, U"abc");
    auto boldProfile     = FontMatcher::buildQueryProfile(boldProps,     U"abc");

    float distSemi = regularProfile.distanceTo(semiBoldProfile);
    float distBold = regularProfile.distanceTo(boldProfile);

    EXPECT_LT(distSemi, distBold)
        << "Regular vs SemiBold distance should be less than Regular vs Bold";
}

// ---------------------------------------------------------------------------
// Bold weight override tests (Karen QA - zero coverage)
//
// detectProperties() overrides the xHeightRatio-based weight to Regular when
// the pixel analysis (per-word bold detection) finds <20% bold words.
// The override fires only when the metric weight is SemiBold or higher.
// We test this through detectProperties() by constructing synthetic page
// images and HOCR data with known word dimensions.
//
// Strategy: create a region where xHeightRatio >= 0.50 (SemiBold or Bold),
// but the word bboxes in the image have thin strokes (< bold threshold).
// detectProperties() should downgrade the weight to Regular.
// ---------------------------------------------------------------------------

// Helper: create a solid-filled word region at the given rect.
// Thin strokes = words that are mostly background with a thin dark line.
static QImage makeThinStrokeImage(int width, int height,
                                   const std::vector<QRect>& wordRects)
{
    // White background
    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(Qt::white);

    // Draw a single thin horizontal line through the middle of each word rect
    // (represents thin stroke weight — not bold).
    for (const QRect& r : wordRects) {
        int midY = r.top() + r.height() / 2;
        for (int x = r.left(); x < r.right() && x < width; ++x) {
            if (midY >= 0 && midY < height)
                img.setPixelColor(x, midY, Qt::black);
        }
    }
    return img;
}

// When xHeightRatio says Bold (>=0.55) but all word stem widths are thin
// (below the bold-detection threshold), the weight must be downgraded to Regular.
TEST(FontMatcherBoldOverride, HighXHeightRatio_ThinStrokes_DowngradesToRegular) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // Construct a region whose xHeightRatio >= 0.55 (Bold by estimateWeight).
    // xSizePx=20, descPx=1, ascPx=1  →  ratio = 20/(20+1+1) = 20/22 ≈ 0.909
    RegionTextData region = makeRegionData(20.0f, 1.0f, 1.0f, 0.0f, 90);

    // Confirm the metric alone would give Bold
    ASSERT_EQ(FontMatcher::estimateWeight(20.0f / 22.0f), FontWeight::Bold);

    // Thin-stroke image: each word has only a 1-pixel-high line (not bold)
    const std::vector<QRect> wordRects = {
        QRect(0, 0, 50, 22),
    };
    QImage img = makeThinStrokeImage(100, 50, wordRects);

    const ScannedTextProperties props = matcher.detectProperties(region, img, 300);

    // Bold fraction < 0.2 → downgrade to Regular
    EXPECT_EQ(props.weight, FontWeight::Regular)
        << "Weight should be downgraded to Regular when pixel analysis finds thin strokes";
}

// When xHeightRatio says Regular (0.42-0.50), the override must NOT fire
// (the override only downgrades SemiBold/Bold, never upgrades).
TEST(FontMatcherBoldOverride, RegularXHeightRatio_NoOverride) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // xSizePx=10, descPx=3, ascPx=12  → ratio = 10/25 = 0.40 (Light by estimateWeight)
    // Use ratio in Regular range: xSizePx=10, descPx=2, ascPx=10  → 10/22 ≈ 0.455 (Regular)
    RegionTextData region = makeRegionData(10.0f, 2.0f, 10.0f, 0.0f, 90);

    // Confirm the metric gives Regular
    ASSERT_EQ(FontMatcher::estimateWeight(10.0f / 22.0f), FontWeight::Regular);

    const QImage img = makeThinStrokeImage(100, 50, {QRect(0, 0, 50, 22)});

    const ScannedTextProperties props = matcher.detectProperties(region, img, 300);

    // Override does not fire for Regular weight — must stay Regular
    EXPECT_EQ(props.weight, FontWeight::Regular)
        << "Regular weight must not be changed by bold override logic";
}

// When words are empty (totalWordCount == 0), the override guard prevents
// a division-by-zero and the weight remains unchanged.
TEST(FontMatcherBoldOverride, EmptyRegion_NoOverrideAndNocrash) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    RegionTextData region;
    region.plainText = "";

    QImage img(10, 10, QImage::Format_ARGB32);
    img.fill(Qt::white);

    // Must not crash or produce undefined behaviour
    EXPECT_NO_FATAL_FAILURE({
        const ScannedTextProperties props = matcher.detectProperties(region, img, 300);
        // Weight defaults to whatever estimateWeight gives for zero xHeightRatio (Thin)
        (void)props;
    });
}

// Confirm override fires for SemiBold as well (threshold is >=SemiBold).
TEST(FontMatcherBoldOverride, SemiBoldXHeightRatio_ThinStrokes_DowngradesToRegular) {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(&context);
    pdfe::fonts::SubstituteFontSelector selector(&analyzer, &cache);
    FontMatcher matcher(&selector, &analyzer);

    // xSizePx=12, descPx=1, ascPx=10  → ratio = 12/23 ≈ 0.522 (SemiBold range 0.50-0.55)
    RegionTextData region = makeRegionData(12.0f, 1.0f, 10.0f, 0.0f, 90);

    // Confirm metric gives SemiBold
    ASSERT_EQ(FontMatcher::estimateWeight(12.0f / 23.0f), FontWeight::SemiBold);

    const QImage img = makeThinStrokeImage(100, 50, {QRect(0, 0, 50, 23)});

    const ScannedTextProperties props = matcher.detectProperties(region, img, 300);

    EXPECT_EQ(props.weight, FontWeight::Regular)
        << "SemiBold should also be downgraded when pixel analysis disagrees";
}
