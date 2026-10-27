// test_script_aware_font_selection.cpp - Tests for script-aware font filtering
// and per-word style detection (bold, italic, underline, strikethrough).
//
// Verifies that the font selection pipeline correctly:
// 1. Detects primary script from font glyph coverage
// 2. Detects script from Unicode text
// 3. Applies script mismatch penalties in distanceTo()
// 4. Filters non-Latin fonts when matching Latin text
// 5. Detects per-word bold/italic/underline/strikethrough from image pixels
// 6. Renders per-word styles in BitmapCompositor

#include <gtest/gtest.h>
#include "fonts/FontMetricProfile.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/SubstituteFontSelector.h"
#include "tre/FontMatcher.h"
#include "tre/BitmapCompositor.h"
#include "tre/TreTypes.h"
#include "core/MuPdfContext.h"

#include <QImage>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <filesystem>
#include <string>

using namespace pdfe::fonts;
using namespace pdfe::tre;
using namespace pdfe::core;

static std::string fixturesDir() {
    return TEST_FIXTURES_DIR;
}

// =============================================================================
// 1. FontScript enum and detection from text
// =============================================================================

TEST(ScriptDetection, LatinText_DetectsLatin)
{
    std::u32string text = U"The quick brown fox jumps over the lazy dog";
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Latin);
}

TEST(ScriptDetection, ArabicText_DetectsArabic)
{
    // Arabic characters (U+0627 alif, U+0628 ba, U+062A ta, etc.)
    std::u32string text = {0x0627, 0x0644, 0x0639, 0x0631, 0x0628, 0x064A, 0x0629};
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Arabic);
}

TEST(ScriptDetection, CyrillicText_DetectsCyrillic)
{
    // Cyrillic characters (U+041F Pe, U+0440 er, etc.)
    std::u32string text = {0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442};
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Cyrillic);
}

TEST(ScriptDetection, CJKText_DetectsCJK)
{
    // CJK characters
    std::u32string text = {0x4E16, 0x754C, 0x4F60, 0x597D};
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::CJK);
}

TEST(ScriptDetection, EmptyText_ReturnsUnknown)
{
    std::u32string text;
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Unknown);
}

TEST(ScriptDetection, DevanagariText_DetectsDevanagari)
{
    // Devanagari characters (U+0928 na, U+092E ma, U+0938 sa, etc.)
    std::u32string text = {0x0928, 0x092E, 0x0938, 0x094D, 0x0924, 0x0947};
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Devanagari);
}

TEST(ScriptDetection, MixedLatinArabic_DominantWins)
{
    // 5 Latin letters + 3 Arabic → Latin wins
    std::u32string text = U"Hello";
    text.push_back(0x0627);
    text.push_back(0x0628);
    text.push_back(0x062A);
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Latin);
}

TEST(ScriptDetection, DigitsOnly_ReturnsUnknown)
{
    // Digits and punctuation are script-neutral, should NOT count as Latin
    std::u32string text = U"12345!@#$%";
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Unknown);
}

TEST(ScriptDetection, DigitsWithArabic_DetectsArabic)
{
    // "123" + 5 Arabic chars → digits are neutral, Arabic wins
    std::u32string text = U"123";
    text.push_back(0x0627);
    text.push_back(0x0644);
    text.push_back(0x0639);
    text.push_back(0x0631);
    text.push_back(0x0628);
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Arabic);
}

TEST(ScriptDetection, SpacesAndPunctuation_ReturnsUnknown)
{
    std::u32string text = U"   ... --- !!!";
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Unknown);
}

// =============================================================================
// 2. Script mismatch penalty in distanceTo()
// =============================================================================

TEST(ScriptPenalty, SameScript_NoPenalty)
{
    FontMetricProfile a, b;
    a.primaryScript = FontScript::Latin;
    b.primaryScript = FontScript::Latin;
    a.stemWeightV = b.stemWeightV = 0.1f;
    a.xToCapRatio = b.xToCapRatio = 0.5f;

    float dist = a.distanceTo(b);
    EXPECT_LT(dist, 1.0f) << "Same-script identical fonts should have near-zero distance";
}

TEST(ScriptPenalty, DifferentScript_HeavyPenalty)
{
    FontMetricProfile a, b;
    a.primaryScript = FontScript::Latin;
    b.primaryScript = FontScript::Arabic;
    // Make all metric fields identical
    a.stemWeightV = b.stemWeightV = 0.1f;
    a.xToCapRatio = b.xToCapRatio = 0.5f;
    a.weightClass = b.weightClass = 400;
    a.avgCharWidth = b.avgCharWidth = 0.5f;

    float dist = a.distanceTo(b);
    // sqrt(50) ≈ 7.07
    EXPECT_GT(dist, 7.0f) << "Script mismatch should add heavy penalty (sqrt(50) ≈ 7.07)";
}

TEST(ScriptPenalty, UnknownScript_NoPenalty)
{
    FontMetricProfile a, b;
    a.primaryScript = FontScript::Latin;
    b.primaryScript = FontScript::Unknown;
    a.stemWeightV = b.stemWeightV = 0.1f;
    a.xToCapRatio = b.xToCapRatio = 0.5f;

    float dist = a.distanceTo(b);
    EXPECT_LT(dist, 1.0f) << "Unknown script should not incur penalty";
}

TEST(ScriptPenalty, BothUnknown_NoPenalty)
{
    FontMetricProfile a, b;
    a.primaryScript = FontScript::Unknown;
    b.primaryScript = FontScript::Unknown;
    a.stemWeightV = b.stemWeightV = 0.1f;
    a.xToCapRatio = b.xToCapRatio = 0.5f;

    float dist = a.distanceTo(b);
    EXPECT_LT(dist, 1.0f) << "Both unknown should not incur penalty";
}

TEST(ScriptPenalty, LatinVsArabic_WorstThanLatinVsLatinWithBadMetrics)
{
    // A Latin font with somewhat different metrics should still score better
    // than an Arabic font with identical metrics
    FontMetricProfile query;
    query.primaryScript = FontScript::Latin;
    query.stemWeightV = 0.10f;
    query.xToCapRatio = 0.50f;
    query.weightClass = 400;

    FontMetricProfile latinCandidate;
    latinCandidate.primaryScript = FontScript::Latin;
    latinCandidate.stemWeightV = 0.15f;  // somewhat different
    latinCandidate.xToCapRatio = 0.55f;  // somewhat different
    latinCandidate.weightClass = 500;    // somewhat different

    FontMetricProfile arabicCandidate;
    arabicCandidate.primaryScript = FontScript::Arabic;
    arabicCandidate.stemWeightV = 0.10f;  // identical metrics
    arabicCandidate.xToCapRatio = 0.50f;
    arabicCandidate.weightClass = 400;

    float latinDist = query.distanceTo(latinCandidate);
    float arabicDist = query.distanceTo(arabicCandidate);

    EXPECT_LT(latinDist, arabicDist)
        << "Latin font with imperfect metrics (" << latinDist
        << ") should beat Arabic font with perfect metrics ("
        << arabicDist << ")";
}

// =============================================================================
// 3. System font script detection via EmbeddedFontAnalyzer
// =============================================================================

class ScriptFontAnalysisTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        analyzer_ = std::make_unique<EmbeddedFontAnalyzer>(context_.get());
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<EmbeddedFontAnalyzer> analyzer_;
};

TEST_F(ScriptFontAnalysisTest, Arial_DetectedAsLatin)
{
    auto profile = analyzer_->analyzeFromFile("C:\\Windows\\Fonts\\arial.ttf");
    if (!profile.has_value()) {
        GTEST_SKIP() << "Arial not found at expected path";
    }
    EXPECT_EQ(profile->primaryScript, FontScript::Latin)
        << "Arial should be classified as primarily Latin";
}

TEST_F(ScriptFontAnalysisTest, TimesNewRoman_DetectedAsLatin)
{
    auto profile = analyzer_->analyzeFromFile("C:\\Windows\\Fonts\\times.ttf");
    if (!profile.has_value()) {
        GTEST_SKIP() << "Times New Roman not found";
    }
    EXPECT_EQ(profile->primaryScript, FontScript::Latin)
        << "Times New Roman should be classified as primarily Latin";
}

TEST_F(ScriptFontAnalysisTest, Calibri_DetectedAsLatin)
{
    auto profile = analyzer_->analyzeFromFile("C:\\Windows\\Fonts\\calibri.ttf");
    if (!profile.has_value()) {
        GTEST_SKIP() << "Calibri not found";
    }
    EXPECT_EQ(profile->primaryScript, FontScript::Latin)
        << "Calibri should be classified as primarily Latin";
}

// =============================================================================
// 4. Full pipeline: Latin text should get a Latin font
// =============================================================================

TEST_F(ScriptFontAnalysisTest, FindSubstitute_LatinText_ReturnsLatinFont)
{
#ifndef _WIN32
    // SubstituteFontSelector relies on SystemFontDiscovery enumeration
    // to score candidate fonts. The Linux enumerator is currently a
    // no-op placeholder (see CI-LIMITATION docs), so findSubstitute
    // returns nullopt regardless of which Latin fonts are installed.
    GTEST_SKIP() << "Requires SystemFontDiscovery Linux impl";
#endif
    auto cache = std::make_unique<FontAnalysisCache>();
    auto selector = std::make_unique<SubstituteFontSelector>(
        analyzer_.get(), cache.get());

    // Query: Latin sans-serif regular
    FontMetricProfile query;
    query.weightClass = 400;
    query.isSerif = false;
    query.isItalic = false;
    query.primaryScript = FontScript::Latin;

    std::u32string latinText = U"The quick brown fox jumps over the lazy dog";

    auto result = selector->findSubstitute(query, latinText);
    ASSERT_TRUE(result.has_value()) << "Should find a substitute font";

    // The result should be a Latin-primary font
    EXPECT_EQ(result->profile.primaryScript, FontScript::Latin)
        << "Substitute for Latin text should be a Latin font, got: "
        << result->profile.familyName;
    EXPECT_TRUE(result->profile.hasLatinBasic)
        << "Substitute should have Latin Basic coverage";
}

TEST_F(ScriptFontAnalysisTest, FontMatcher_LatinOcr_ReturnsLatinFont)
{
    auto cache = std::make_unique<FontAnalysisCache>();
    auto selector = std::make_unique<SubstituteFontSelector>(
        analyzer_.get(), cache.get());
    FontMatcher matcher(selector.get(), analyzer_.get());

    ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false;
    props.textColor = {0, 0, 0};

    std::u32string latinText = U"The quick brown fox jumps over the lazy dog";
    auto match = matcher.findMatch(props, latinText);

    EXPECT_FALSE(match.fontFamily.empty())
        << "Should find a font for Latin text";

    // Verify it's not an Arabic/CJK font
    EXPECT_TRUE(
        match.fontFamily.find("Scheherazade") == std::string::npos &&
        match.fontFamily.find("SimSun") == std::string::npos &&
        match.fontFamily.find("Noto Sans Arabic") == std::string::npos
    ) << "Latin text should NOT match Arabic/CJK font, got: " << match.fontFamily;
}

// =============================================================================
// 5. Image-based serif detection via detectSerif()
// =============================================================================

// Helper: create a synthetic image with a "character" drawn as a vertical
// stroke. If addSerifs is true, the baseline row has wider horizontal extent
// (simulating serif terminals).
static QImage createSyntheticCharImage(int width, int height,
                                        int charX, int charWidth,
                                        int baselineY, int serifExtra,
                                        bool addSerifs)
{
    // White background
    QImage img(width, height, QImage::Format_RGB32);
    img.fill(Qt::white);

    // Draw a vertical stroke (stem) from top area to baseline
    const int stemX0 = charX;
    const int stemX1 = charX + charWidth;
    const int stemTop = baselineY - (height / 2);

    for (int y = std::max(0, stemTop); y <= baselineY && y < height; ++y) {
        for (int x = stemX0; x < stemX1 && x < width; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }

    // Add serifs: wider strokes at baseline
    if (addSerifs) {
        const int serifX0 = std::max(0, charX - serifExtra);
        const int serifX1 = std::min(width, charX + charWidth + serifExtra);
        // 2 rows of serif at baseline
        for (int dy = 0; dy < 2 && (baselineY + dy) < height; ++dy) {
            for (int x = serifX0; x < serifX1; ++x) {
                img.setPixelColor(x, baselineY + dy, Qt::black);
            }
        }
    }

    return img;
}

// Helper: build HocrLine with one word containing a tall char (no char-level boxes)
static HocrLine makeTestLine(float x0, float y0, float x1, float y1,
                              float baselineOffset, const std::string& wordText,
                              int confidence = 90)
{
    HocrLine line;
    line.bbox = {x0, y0, x1, y1};
    line.baselineSlope = 0.0f;
    line.baselineOffset = baselineOffset;

    HocrWord word;
    word.text = wordText;
    word.bbox = {x0, y0, x1, y1};
    word.confidence = confidence;
    line.words.push_back(word);

    return line;
}

// Helper: build HocrLine with per-character bounding boxes.
// Each charSpec is {codepoint, x0, y0, x1, y1}.
struct CharSpec {
    char32_t codepoint;
    float x0, y0, x1, y1;
    int confidence = 90;
};

static HocrLine makeTestLineWithChars(float lineX0, float lineY0,
                                       float lineX1, float lineY1,
                                       float baselineOffset,
                                       const std::string& wordText,
                                       const std::vector<CharSpec>& charSpecs,
                                       int wordConfidence = 90)
{
    HocrLine line;
    line.bbox = {lineX0, lineY0, lineX1, lineY1};
    line.baselineSlope = 0.0f;
    line.baselineOffset = baselineOffset;

    HocrWord word;
    word.text = wordText;
    word.bbox = {lineX0, lineY0, lineX1, lineY1};
    word.confidence = wordConfidence;

    for (const auto& cs : charSpecs) {
        HocrChar ch;
        ch.codepoint = cs.codepoint;
        ch.bbox = {cs.x0, cs.y0, cs.x1, cs.y1};
        ch.confidence = cs.confidence;
        word.chars.push_back(ch);
    }
    line.words.push_back(word);

    return line;
}

TEST(SerifDetection, SerifLikeImage_ReturnsTrue)
{
    // Create synthetic image 200x100 with a serif-like character at x=80
    // Baseline at y=70, serif extends 6px beyond 4px stem → 10px wider
    const int baselineY = 70;
    const int stemWidth = 4;
    const int serifExtra = 6;
    QImage img = createSyntheticCharImage(200, 100, 80, stemWidth,
                                           baselineY, serifExtra, true);

    // HOCR line with baseline offset placing baseline at y=70
    // Line bbox top = 30, so baselineOffset = 70 - 30 = 40
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLine(70.0f, 30.0f, 100.0f, 80.0f, 40.0f, "Ilt"));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_TRUE(result) << "Image with wide baseline strokes should be detected as serif";
}

TEST(SerifDetection, SansSerifImage_ReturnsFalse)
{
    // Create synthetic image with uniform stroke width (no serifs)
    const int baselineY = 70;
    const int stemWidth = 4;
    QImage img = createSyntheticCharImage(200, 100, 80, stemWidth,
                                           baselineY, 0, false);

    std::vector<HocrLine> lines;
    lines.push_back(makeTestLine(70.0f, 30.0f, 100.0f, 80.0f, 40.0f, "Ilt"));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_FALSE(result) << "Image with uniform stroke should be detected as sans-serif";
}

TEST(SerifDetection, EmptyLines_ReturnsFalse)
{
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    std::vector<HocrLine> lines;
    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_FALSE(result) << "Empty lines should default to sans-serif";
}

// ---------------------------------------------------------------------------
// GitHub #249 -- "measured as sans" and "could not measure" must stop being
// the same answer.
//
// detectSerif() returns false for both, and its own doc says so ("false if
// sans-serif or indeterminate"). That conflation is harmless while nothing
// acts categorically on the result -- and stops being harmless the moment
// something does: FontMatcher now PARTITIONS font candidates so serif
// mismatches rank last, and doing that on an unmeasured value would force
// sans-only results for every region whose serif-ness could not be read.
//
// These two tests are a pair ON PURPOSE. Either alone is satisfiable by a
// constant: `determined = true` always passes the first, `false` always passes
// the second. Only together do they pin that the flag tracks whether a
// measurement actually happened.
// ---------------------------------------------------------------------------

TEST(SerifDetection, SansSerifImage_IsReportedAsDETERMINED)
{
    const int baselineY = 70;
    const int stemWidth = 4;
    QImage img = createSyntheticCharImage(200, 100, 80, stemWidth,
                                           baselineY, 0, false);

    std::vector<HocrLine> lines;
    lines.push_back(makeTestLine(70.0f, 30.0f, 100.0f, 80.0f, 40.0f, "Ilt"));

    bool determined = true;  // seeded WRONG, so a no-op write cannot pass
    const bool result = FontMatcher::detectSerif(img, lines, &determined);

    EXPECT_FALSE(result) << "uniform stroke width is sans-serif";
    EXPECT_TRUE(determined)
        << "a real measurement that concludes 'sans' must be reported as "
           "DETERMINED -- otherwise the serif partition (GitHub #249) is "
           "disabled for every genuinely sans-serif region, which is most of "
           "them";
}

TEST(SerifDetection, EmptyLines_IsReportedAsNOTDetermined)
{
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    std::vector<HocrLine> lines;  // nothing to measure

    bool determined = true;  // seeded WRONG: the function must clear it
    const bool result = FontMatcher::detectSerif(img, lines, &determined);

    EXPECT_FALSE(result) << "no lines -> the documented sans-serif default";
    EXPECT_FALSE(determined)
        << "'could not measure' must NOT be reported as determined. It "
           "returns the same false as a real sans-serif measurement, and "
           "callers that act categorically on serif-ness need to tell them "
           "apart (GitHub #249)";
}

TEST(SerifDetection, NullImage_ReturnsFalse)
{
    QImage nullImage;
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLine(0, 0, 100, 50, 40, "Hello"));

    bool result = FontMatcher::detectSerif(nullImage, lines);
    EXPECT_FALSE(result) << "Null image should default to sans-serif";
}

// Boundary test: small serif overhang on narrow stem should NOT be classified as serif.
// stemWidth=6, serifExtra=1 → per-stroke measures ~6px stem, ~8px baseline → ratio ~0.33
TEST(SerifDetection, BoundarySmallOverhang_ReturnsFalse)
{
    const int baselineY = 70;
    const int stemWidth = 6;
    const int serifExtra = 1;  // Tiny overhang — within noise range
    QImage img = createSyntheticCharImage(200, 100, 80, stemWidth,
                                           baselineY, serifExtra, true);

    std::vector<HocrLine> lines;
    lines.push_back(makeTestLine(70.0f, 30.0f, 120.0f, 80.0f, 40.0f, "Ilt"));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_FALSE(result)
        << "Small overhang on narrow stem (noise range) should NOT be classified as serif";
}

// Boundary test: clear serif overhang on narrow stem must be classified as serif.
// stemWidth=6, serifExtra=4 → per-stroke measures ~6px stem, ~14px baseline → ratio >0.35
TEST(SerifDetection, BoundaryClearSerif_ReturnsTrue)
{
    const int baselineY = 70;
    const int stemWidth = 6;
    const int serifExtra = 4;  // Clear serif overhang
    QImage img = createSyntheticCharImage(200, 100, 80, stemWidth,
                                           baselineY, serifExtra, true);

    std::vector<HocrLine> lines;
    lines.push_back(makeTestLine(70.0f, 30.0f, 120.0f, 80.0f, 40.0f, "Ilt"));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_TRUE(result)
        << "Clear serif overhang on narrow stem should be classified as serif";
}

TEST(SerifDetection, NoTallChars_ReturnsFalse)
{
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    // Words with only 'o', 'a', 'e' — no tall chars for serif detection
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLine(10, 10, 100, 50, 30, "oae"));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_FALSE(result) << "Without tall characters, should default to sans-serif";
}

TEST(SerifDetection, LowConfidence_Skipped)
{
    // Serif-like image, but word has low confidence — should be skipped
    const int baselineY = 70;
    QImage img = createSyntheticCharImage(200, 100, 80, 4, baselineY, 6, true);

    std::vector<HocrLine> lines;
    // confidence=30 (below 50 threshold)
    lines.push_back(makeTestLine(70, 30, 100, 80, 40, "Ilt", 30));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_FALSE(result)
        << "Low-confidence words should be skipped, defaulting to sans-serif";
}

TEST(SerifDetection, TinyWordHeight_Skipped)
{
    // Word bbox too small (height < 8px) — should be skipped
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    std::vector<HocrLine> lines;
    lines.push_back(makeTestLine(10, 40, 50, 45, 3, "Il"));  // 5px tall

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_FALSE(result)
        << "Words smaller than 8px should be skipped";
}

TEST(SerifDetection, FontMatch_SerifQuery_PrefersSerif)
{
    // Verify that when detectSerif returns true, the FontMatcher query profile
    // has isSerif=true, which influences the font selection
    auto context = std::make_shared<MuPdfContext>();
    auto analyzer = std::make_unique<EmbeddedFontAnalyzer>(context.get());
    auto cache = std::make_unique<FontAnalysisCache>();
    auto selector = std::make_unique<SubstituteFontSelector>(
        analyzer.get(), cache.get());
    FontMatcher matcher(selector.get(), analyzer.get());

    // Query with serif=true should prefer serif fonts
    ScannedTextProperties serifProps;
    serifProps.fontSizePt = 12.0f;
    serifProps.weight = FontWeight::Regular;
    serifProps.isItalic = false;
    serifProps.isSerif = true;
    serifProps.textColor = {0, 0, 0};

    ScannedTextProperties sansProps;
    sansProps.fontSizePt = 12.0f;
    sansProps.weight = FontWeight::Regular;
    sansProps.isItalic = false;
    sansProps.isSerif = false;
    sansProps.textColor = {0, 0, 0};

    std::u32string text = U"The quick brown fox";
    auto serifMatch = matcher.findMatch(serifProps, text);
    auto sansMatch = matcher.findMatch(sansProps, text);

    EXPECT_FALSE(serifMatch.fontFamily.empty());
    EXPECT_FALSE(sansMatch.fontFamily.empty());

    // They should potentially return different fonts (serif vs sans-serif)
    // At minimum, verify both succeed
    std::cout << "  Serif query matched: " << serifMatch.fontFamily << std::endl;
    std::cout << "  Sans query matched:  " << sansMatch.fontFamily << std::endl;

    // The serif match should not be the same sans-serif font used for sans query
    // (unless the system has very limited fonts). This is a soft check.
    if (serifMatch.fontFamily != sansMatch.fontFamily) {
        SUCCEED() << "Serif and sans-serif queries returned different fonts";
    } else {
        // Not a failure, just log it — some systems may not have enough fonts
        std::cout << "  NOTE: Same font for both queries (limited system fonts?)"
                  << std::endl;
    }
}

// =============================================================================
// 5b. Per-character serif detection tests
// =============================================================================

TEST(SerifDetection, PerChar_SerifChar_ReturnsTrue)
{
    // Single 'I' character with serif: stem=4px, serif adds 6px each side
    const int baselineY = 70;
    const int stemWidth = 4;
    const int serifExtra = 6;
    QImage img = createSyntheticCharImage(200, 100, 80, stemWidth,
                                           baselineY, serifExtra, true);

    // Per-character bbox covers just the 'I'
    std::vector<CharSpec> chars = {
        {U'I', 70.0f, 30.0f, 100.0f, 80.0f}
    };
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLineWithChars(70, 30, 100, 80, 40.0f, "I", chars));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_TRUE(result) << "Per-char serif detection on 'I' with serifs should return true";
}

TEST(SerifDetection, PerChar_SansSerifChar_ReturnsFalse)
{
    // Single 'I' character without serif: uniform stroke
    const int baselineY = 70;
    const int stemWidth = 4;
    QImage img = createSyntheticCharImage(200, 100, 80, stemWidth,
                                           baselineY, 0, false);

    std::vector<CharSpec> chars = {
        {U'I', 70.0f, 30.0f, 100.0f, 80.0f}
    };
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLineWithChars(70, 30, 100, 80, 40.0f, "I", chars));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_FALSE(result) << "Per-char serif detection on 'I' without serifs should return false";
}

TEST(SerifDetection, PerChar_PreferredOverWordLevel)
{
    // When char-level boxes are available, per-char analysis should be used.
    // Create a word "Io" where 'I' has no serifs but the 'o' is wide at baseline.
    // Word-level analysis would see the 'o' contamination; per-char should not.
    const int baselineY = 70;
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    // Draw narrow 'I' (sans-serif) at x=40-44
    for (int y = 35; y <= baselineY; ++y) {
        for (int x = 40; x < 44; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }
    // Draw wide 'o' at x=60-80 (round shape, naturally wider at baseline)
    for (int y = 45; y <= baselineY; ++y) {
        for (int x = 60; x < 80; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }

    // Per-char boxes: only 'I' is a diagnostic char, 'o' is ignored
    std::vector<CharSpec> chars = {
        {U'I', 38.0f, 30.0f, 46.0f, 80.0f},
        {U'o', 58.0f, 40.0f, 82.0f, 80.0f}
    };
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLineWithChars(38, 30, 82, 80, 40.0f, "Io", chars));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_FALSE(result)
        << "Per-char analysis should isolate 'I' from 'o', detecting sans-serif";
}

TEST(SerifDetection, PerChar_FallsBackToWordLevel)
{
    // When no char-level boxes exist, falls back to word-level analysis.
    // This is the existing behavior — sans-serif uniform stroke.
    const int baselineY = 70;
    const int stemWidth = 4;
    QImage img = createSyntheticCharImage(200, 100, 80, stemWidth,
                                           baselineY, 0, false);

    // No chars populated — word-level fallback
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLine(70.0f, 30.0f, 100.0f, 80.0f, 40.0f, "Ilt"));

    bool result = FontMatcher::detectSerif(img, lines);
    EXPECT_FALSE(result) << "Word-level fallback with uniform stroke should return false";
}

TEST(SerifDetection, PerChar_SkipsNonDiagnosticChars)
{
    // Only diagnostic chars (I, l, T, etc.) should be measured.
    // Round chars like 'a', 'o', 'e' should be skipped entirely.
    // Note: this defaults to sans-serif via two mechanisms: (1) per-char skips
    // non-diagnostic chars so no ratios are collected, and (2) hasCharData=true
    // suppresses word-level fallback. Word text "aoe" also lacks tall chars,
    // so word-level would also skip it — both paths agree.
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    // Draw wide 'a' shape at baseline (naturally wider — would cause false positive)
    for (int y = 45; y <= 70; ++y) {
        for (int x = 40; x < 60; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }

    std::vector<CharSpec> chars = {
        {U'a', 38.0f, 30.0f, 62.0f, 80.0f},
        {U'o', 70.0f, 30.0f, 90.0f, 80.0f},
        {U'e', 100.0f, 30.0f, 120.0f, 80.0f}
    };
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLineWithChars(38, 30, 120, 80, 40.0f, "aoe", chars));

    bool result = FontMatcher::detectSerif(img, lines);
    // No diagnostic chars → no per-char ratios collected, no word-level fallback
    // because word has no tall characters either. Should default to sans-serif.
    EXPECT_FALSE(result) << "Non-diagnostic chars should be skipped, defaulting to sans-serif";
}

TEST(SerifDetection, PerChar_MultipleChars_MedianUsed)
{
    // Multiple diagnostic characters: 2 sans-serif + 1 serif → median = sans-serif
    QImage img(400, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    // Draw three 'I'-like stems at different positions
    // #1: no serif (x=40-44)
    for (int y = 35; y <= 70; ++y)
        for (int x = 40; x < 44; ++x)
            img.setPixelColor(x, y, Qt::black);

    // #2: no serif (x=140-144)
    for (int y = 35; y <= 70; ++y)
        for (int x = 140; x < 144; ++x)
            img.setPixelColor(x, y, Qt::black);

    // #3: with serif (x=240-244, serif extends to 234-250)
    for (int y = 35; y <= 70; ++y)
        for (int x = 240; x < 244; ++x)
            img.setPixelColor(x, y, Qt::black);
    for (int x = 234; x < 250; ++x) {
        img.setPixelColor(x, 70, Qt::black);
        img.setPixelColor(x, 71, Qt::black);
    }

    std::vector<CharSpec> chars = {
        {U'I', 38.0f, 30.0f, 46.0f, 80.0f},
        {U'l', 138.0f, 30.0f, 146.0f, 80.0f},
        {U'T', 232.0f, 30.0f, 252.0f, 80.0f}
    };
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLineWithChars(38, 30, 252, 80, 40.0f, "IlT", chars));

    bool result = FontMatcher::detectSerif(img, lines);
    // 2 out of 3 are sans-serif, median should be sans-serif
    EXPECT_FALSE(result) << "Median of 2 sans + 1 serif should classify as sans-serif";
}

TEST(SerifDetection, PerChar_LowConfidenceChar_Skipped)
{
    // Char with confidence < 40 should be skipped even if diagnostic
    const int baselineY = 70;
    QImage img = createSyntheticCharImage(200, 100, 80, 4, baselineY, 6, true);

    std::vector<CharSpec> chars = {
        {U'I', 70.0f, 30.0f, 100.0f, 80.0f, 30}  // low confidence
    };
    std::vector<HocrLine> lines;
    lines.push_back(makeTestLineWithChars(70, 30, 100, 80, 40.0f, "I", chars));

    bool result = FontMatcher::detectSerif(img, lines);
    // Low confidence char skipped, no word-level fallback (chars were present),
    // no ratios → defaults to sans-serif
    EXPECT_FALSE(result) << "Low-confidence char should be skipped";
}

// =============================================================================
// 6. Per-word style detection tests
// =============================================================================

// Helper: create a synthetic image with a bold-like thick vertical stroke
static QImage createBoldStrokeImage(int width, int height,
                                     int strokeX, int strokeWidth,
                                     int strokeTop, int strokeBottom)
{
    QImage img(width, height, QImage::Format_RGB32);
    img.fill(Qt::white);
    for (int y = strokeTop; y <= strokeBottom && y < height; ++y) {
        for (int x = strokeX; x < strokeX + strokeWidth && x < width; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }
    return img;
}

// Helper: create an HocrLine+HocrWord covering a region
static HocrLine makeWordLine(float x0, float y0, float x1, float y1,
                               float baselineOffset, const std::string& wordText,
                               int confidence = 90)
{
    HocrLine line;
    line.bbox = {x0, y0, x1, y1};
    line.baselineSlope = 0.0f;
    line.baselineOffset = baselineOffset;
    HocrWord word;
    word.text = wordText;
    word.bbox = {x0, y0, x1, y1};
    word.confidence = confidence;
    line.words.push_back(word);
    return line;
}

// --- Bold Detection Tests ---

TEST(BoldDetection, ThickStemWord_True)
{
    // Create 200x100 image with a thick vertical stroke (12px wide)
    // Word bbox: x0=40, y0=10, x1=80, y1=80 (height=70)
    // Stroke at x=50, width=12, from y=10 to y=70
    // stemWidth/wordHeight = 12/70 ≈ 0.17 > 0.11 threshold
    QImage img = createBoldStrokeImage(200, 100, 50, 12, 10, 70);
    HocrLine line = makeWordLine(40, 10, 80, 80, 60, "Il");

    bool result = FontMatcher::detectWordBold(img, line.words[0], line);
    EXPECT_TRUE(result) << "Thick stem (12px in 70px word) should detect as bold";
}

TEST(BoldDetection, ThinStemWord_False)
{
    // Thin stroke: 3px wide in 70px tall word
    // stemWidth/wordHeight = 3/70 ≈ 0.043 < 0.11 threshold
    QImage img = createBoldStrokeImage(200, 100, 58, 3, 10, 70);
    HocrLine line = makeWordLine(40, 10, 80, 80, 60, "Il");

    bool result = FontMatcher::detectWordBold(img, line.words[0], line);
    EXPECT_FALSE(result) << "Thin stem (3px in 70px word) should not detect as bold";
}

TEST(BoldDetection, EmptyImage_False)
{
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    HocrLine line = makeWordLine(40, 10, 80, 80, 60, "Il");

    bool result = FontMatcher::detectWordBold(img, line.words[0], line);
    EXPECT_FALSE(result) << "Empty image should not detect bold";
}

TEST(BoldDetection, NoTallChars_False)
{
    // Word text has no tall characters (I, l, t, h, d, b, f, k, T, H, etc.)
    QImage img = createBoldStrokeImage(200, 100, 50, 12, 10, 70);
    HocrLine line = makeWordLine(40, 10, 80, 80, 60, "oae");

    bool result = FontMatcher::detectWordBold(img, line.words[0], line);
    EXPECT_FALSE(result) << "Word without tall characters should not be tested for bold";
}

// --- Italic Detection Tests ---

TEST(ItalicDetection, SlantedWord_True)
{
    // Create an image with rightward-leaning stroke (italic-like)
    // Top quarter centroid shifted right relative to bottom quarter
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    // Draw a slanted stroke: bottom at x=50, top at x=60 (10px displacement)
    // Word bbox: y0=10, y1=80 (height=70)
    int wordTop = 10;
    int wordBottom = 80;
    int baseX = 50;
    for (int y = wordTop; y <= wordBottom && y < 100; ++y) {
        float t = static_cast<float>(y - wordTop) / static_cast<float>(wordBottom - wordTop);
        int x = baseX + static_cast<int>((1.0f - t) * 25.0f); // top shifted right by 25px (~15° centroid angle)
        for (int dx = 0; dx < 4 && (x + dx) < 200; ++dx) {
            img.setPixelColor(x + dx, y, Qt::black);
        }
    }
    HocrLine line = makeWordLine(40, 10, 80, 80, 60, "Il");

    bool result = FontMatcher::detectWordItalic(img, line.words[0], line);
    EXPECT_TRUE(result) << "Rightward-leaning stroke should detect as italic";
}

TEST(ItalicDetection, UprightWord_False)
{
    // Perfectly vertical stroke — no slant
    QImage img = createBoldStrokeImage(200, 100, 58, 4, 10, 70);
    HocrLine line = makeWordLine(40, 10, 80, 80, 60, "Il");

    bool result = FontMatcher::detectWordItalic(img, line.words[0], line);
    EXPECT_FALSE(result) << "Vertical stroke should not detect as italic";
}

TEST(ItalicDetection, BoundaryBelowThreshold_False)
{
    // ~8° centroid angle (below 10° threshold) — should NOT detect as italic.
    // Uses 12px displacement over 70px height for ~9° centroid angle.
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    int wordTop = 10, wordBottom = 80, baseX = 50;
    for (int y = wordTop; y <= wordBottom && y < 100; ++y) {
        float t = static_cast<float>(y - wordTop) / static_cast<float>(wordBottom - wordTop);
        int x = baseX + static_cast<int>((1.0f - t) * 12.0f);
        for (int dx = 0; dx < 4 && (x + dx) < 200; ++dx)
            img.setPixelColor(x + dx, y, Qt::black);
    }
    HocrLine line = makeWordLine(40, 10, 80, 80, 60, "Il");
    bool result = FontMatcher::detectWordItalic(img, line.words[0], line);
    EXPECT_FALSE(result) << "~9 degree centroid angle should be below 10 degree threshold";
}

TEST(ItalicDetection, BoundaryAboveThreshold_True)
{
    // ~12° centroid angle (above 10° threshold) — should detect as italic.
    // Uses 18px displacement over 70px height for ~12° centroid angle.
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    int wordTop = 10, wordBottom = 80, baseX = 50;
    for (int y = wordTop; y <= wordBottom && y < 100; ++y) {
        float t = static_cast<float>(y - wordTop) / static_cast<float>(wordBottom - wordTop);
        int x = baseX + static_cast<int>((1.0f - t) * 18.0f);
        for (int dx = 0; dx < 4 && (x + dx) < 200; ++dx)
            img.setPixelColor(x + dx, y, Qt::black);
    }
    HocrLine line = makeWordLine(40, 10, 80, 80, 60, "Il");
    bool result = FontMatcher::detectWordItalic(img, line.words[0], line);
    EXPECT_TRUE(result) << "~12 degree centroid angle should be above 10 degree threshold";
}

TEST(ItalicDetection, EmptyImage_False)
{
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    HocrLine line = makeWordLine(40, 10, 80, 80, 60, "Il");

    bool result = FontMatcher::detectWordItalic(img, line.words[0], line);
    EXPECT_FALSE(result) << "Empty image should not detect italic";
}

// --- Underline Detection Tests ---

TEST(UnderlineDetection, WithLine_True)
{
    // Create image with text stroke + horizontal line below baseline
    QImage img(300, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    // Draw a "character" stem from y=20 to y=60
    for (int y = 20; y <= 60; ++y) {
        for (int x = 30; x < 34; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }
    // Baseline at y=60, line bbox y0=10, y1=80 (height=70)
    // Underline position = baseline + 15% of lineHeight = 60 + 0.15*70 = 70.5 → y=70-72
    for (int y = 70; y <= 72; ++y) {
        for (int x = 20; x < 80; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }

    HocrLine line;
    line.bbox = {20, 10, 80, 80};
    line.baselineSlope = 0.0f;
    line.baselineOffset = 50.0f; // baseline Y = y0 + offset = 10 + 50 = 60
    HocrWord word;
    word.text = "Hello";
    word.bbox = {20, 10, 80, 80};
    word.confidence = 90;
    line.words.push_back(word);

    FontMatcher::detectUnderlinedWords(img, line);
    EXPECT_TRUE(line.words[0].style.isUnderlined)
        << "Horizontal line below baseline should detect as underlined";
}

TEST(UnderlineDetection, NoLine_False)
{
    // Just a character stem, no underline
    QImage img(300, 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    for (int y = 20; y <= 60; ++y) {
        for (int x = 30; x < 34; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }

    HocrLine line;
    line.bbox = {20, 10, 80, 80};
    line.baselineSlope = 0.0f;
    line.baselineOffset = 50.0f;
    HocrWord word;
    word.text = "Hello";
    word.bbox = {20, 10, 80, 80};
    word.confidence = 90;
    line.words.push_back(word);

    FontMatcher::detectUnderlinedWords(img, line);
    EXPECT_FALSE(line.words[0].style.isUnderlined)
        << "No horizontal line should not detect underline";
}

TEST(UnderlineDetection, Empty_False)
{
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    HocrLine line;
    line.bbox = {20, 10, 80, 80};
    line.baselineSlope = 0.0f;
    line.baselineOffset = 50.0f;
    // No words in the line
    FontMatcher::detectUnderlinedWords(img, line);
    EXPECT_TRUE(line.words.empty()) << "Empty word list should be handled gracefully";
}

// --- Strikethrough Detection Tests ---

TEST(StrikethroughDetection, WithLine_True)
{
    // Create image with text stroke + horizontal line through mid-text
    QImage img(300, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    // Draw a "character" stem from y=20 to y=60
    for (int y = 20; y <= 60; ++y) {
        for (int x = 30; x < 34; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }
    // Baseline at y=60, line bbox y0=10, y1=80
    // Mid-text Y = (10 + 60) / 2 = 35
    // Strikethrough line at mid-text area
    for (int y = 34; y <= 36; ++y) {
        for (int x = 20; x < 80; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }

    HocrLine line;
    line.bbox = {20, 10, 80, 80};
    line.baselineSlope = 0.0f;
    line.baselineOffset = 50.0f; // baseline Y = 10 + 50 = 60
    HocrWord word;
    word.text = "Hello";
    word.bbox = {20, 10, 80, 80};
    word.confidence = 90;
    line.words.push_back(word);

    FontMatcher::detectStrikethroughWords(img, line);
    EXPECT_TRUE(line.words[0].style.isStrikethrough)
        << "Horizontal line through mid-text should detect as strikethrough";
}

TEST(StrikethroughDetection, NoLine_False)
{
    // Just a character stem, no strikethrough
    QImage img(300, 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    for (int y = 20; y <= 60; ++y) {
        for (int x = 30; x < 34; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }

    HocrLine line;
    line.bbox = {20, 10, 80, 80};
    line.baselineSlope = 0.0f;
    line.baselineOffset = 50.0f;
    HocrWord word;
    word.text = "Hello";
    word.bbox = {20, 10, 80, 80};
    word.confidence = 90;
    line.words.push_back(word);

    FontMatcher::detectStrikethroughWords(img, line);
    EXPECT_FALSE(line.words[0].style.isStrikethrough)
        << "No horizontal line should not detect strikethrough";
}

TEST(StrikethroughDetection, Empty_False)
{
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    HocrLine line;
    line.bbox = {20, 10, 80, 80};
    line.baselineSlope = 0.0f;
    line.baselineOffset = 50.0f;
    FontMatcher::detectStrikethroughWords(img, line);
    EXPECT_TRUE(line.words.empty()) << "Empty word list should be handled gracefully";
}

// --- Mixed Styles Tests ---

TEST(MixedStyles, BoldAndItalicWords)
{
    // Two words side-by-side: first bold (thick stem), second italic (slanted)
    QImage img(400, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    // Word 1 (bold): thick vertical stem at x=50, 12px wide, y=10 to y=70
    for (int y = 10; y <= 70; ++y) {
        for (int x = 50; x < 62; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }

    // Word 2 (italic): slanted stem at x=200 area, y=10 to y=70
    for (int y = 10; y <= 70; ++y) {
        float t = static_cast<float>(y - 10) / 60.0f;
        int x = 200 + static_cast<int>((1.0f - t) * 25.0f); // 25px displacement (~14° centroid angle)
        for (int dx = 0; dx < 4 && (x + dx) < 400; ++dx) {
            img.setPixelColor(x + dx, y, Qt::black);
        }
    }

    HocrLine line;
    line.bbox = {30, 5, 250, 85};
    line.baselineSlope = 0.0f;
    line.baselineOffset = 65.0f; // baseline Y = 5 + 65 = 70

    HocrWord w1;
    w1.text = "Il";
    w1.bbox = {30, 10, 80, 80};
    w1.confidence = 95;
    line.words.push_back(w1);

    HocrWord w2;
    w2.text = "Il";
    w2.bbox = {180, 10, 230, 80};
    w2.confidence = 95;
    line.words.push_back(w2);

    std::vector<HocrLine> lines = {line};
    FontMatcher::detectWordStyles(img, lines);

    EXPECT_TRUE(lines[0].words[0].style.isBold)
        << "First word with thick stem should be bold";
    EXPECT_TRUE(lines[0].words[1].style.isItalic)
        << "Second word with slanted stem should be italic";
}

TEST(MixedStyles, UnderlinedPhrase)
{
    // 3 words in a line; middle word underlined, adjacent words not
    QImage img(400, 100, QImage::Format_RGB32);
    img.fill(Qt::white);

    // Draw character stems for all 3 words
    auto drawStem = [&](int x, int top, int bottom) {
        for (int y = top; y <= bottom; ++y) {
            for (int dx = 0; dx < 3; ++dx) {
                if (x + dx < 400)
                    img.setPixelColor(x + dx, y, Qt::black);
            }
        }
    };
    drawStem(40, 20, 60);   // word 1
    drawStem(150, 20, 60);  // word 2
    drawStem(260, 20, 60);  // word 3

    // Underline only under word 2 (x=120 to x=200), baseline area around y=70
    // baseline Y = 10 + 50 = 60; lineHeight = 70; underline pos = 60 + 0.15*70 ≈ 70
    for (int y = 70; y <= 72; ++y) {
        for (int x = 120; x < 200; ++x) {
            img.setPixelColor(x, y, Qt::black);
        }
    }

    HocrLine line;
    line.bbox = {20, 10, 320, 80};
    line.baselineSlope = 0.0f;
    line.baselineOffset = 50.0f;

    HocrWord w1, w2, w3;
    w1.text = "Hello"; w1.bbox = {20, 10, 100, 80}; w1.confidence = 90;
    w2.text = "World"; w2.bbox = {120, 10, 200, 80}; w2.confidence = 90;
    w3.text = "Test";  w3.bbox = {220, 10, 300, 80}; w3.confidence = 90;
    line.words = {w1, w2, w3};

    FontMatcher::detectUnderlinedWords(img, line);

    EXPECT_FALSE(line.words[0].style.isUnderlined)
        << "First word should not be underlined";
    EXPECT_TRUE(line.words[1].style.isUnderlined)
        << "Middle word with underline pixels should be underlined";
    EXPECT_FALSE(line.words[2].style.isUnderlined)
        << "Third word should not be underlined";
}

// --- WordStyleFlags Default Test ---

TEST(WordStyleFlags, DefaultsAllFalse)
{
    WordStyleFlags flags;
    EXPECT_FALSE(flags.isBold);
    EXPECT_FALSE(flags.isItalic);
    EXPECT_FALSE(flags.isUnderlined);
    EXPECT_FALSE(flags.isStrikethrough);
}

// --- Orchestrator Test ---

TEST(DetectWordStyles, NullImage_NoExceptions)
{
    QImage nullImg;
    std::vector<HocrLine> lines;
    lines.push_back(makeWordLine(10, 10, 100, 80, 60, "Hello"));

    // Should not throw on null image
    EXPECT_NO_THROW(FontMatcher::detectWordStyles(nullImg, lines));
    // All flags should remain false
    EXPECT_FALSE(lines[0].words[0].style.isBold);
    EXPECT_FALSE(lines[0].words[0].style.isItalic);
    EXPECT_FALSE(lines[0].words[0].style.isUnderlined);
    EXPECT_FALSE(lines[0].words[0].style.isStrikethrough);
}

TEST(DetectWordStyles, EmptyLines_NoExceptions)
{
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    std::vector<HocrLine> lines;

    EXPECT_NO_THROW(FontMatcher::detectWordStyles(img, lines));
    EXPECT_TRUE(lines.empty());
}

// --- BitmapCompositor StyledRuns Test ---

static const std::string kArialFontPath = "C:/Windows/Fonts/arial.ttf";

TEST(BitmapCompositorStyled, StyledRunsBoldUnderline_ProducesNonEmptyBitmap)
{
    if (!std::filesystem::exists(kArialFontPath)) {
        GTEST_SKIP() << "arial.ttf not available";
    }

    BitmapCompositor compositor;
    std::u32string text = U"Hello World";

    FontMatch fm;
    fm.fontFilePath = kArialFontPath;
    fm.fontFamily = "Arial";
    fm.sizePt = 14.0f;
    fm.color = {0, 0, 0};

    // "Hello" bold, "World" underlined
    std::vector<StyledRun> runs;
    StyledRun boldRun;
    boldRun.charStart = 0;
    boldRun.charEnd = 5;
    boldRun.style.isBold = true;
    runs.push_back(boldRun);

    StyledRun ulRun;
    ulRun.charStart = 6;
    ulRun.charEnd = 11;
    ulRun.style.isUnderlined = true;
    runs.push_back(ulRun);

    QImage result = compositor.renderStyledText(text, fm, runs, 300, 100, 300);

    ASSERT_FALSE(result.isNull());
    ASSERT_EQ(result.width(), 300);
    ASSERT_EQ(result.height(), 100);

    // Should have rendered pixels
    int nonTransparent = 0;
    for (int y = 0; y < result.height(); ++y) {
        for (int x = 0; x < result.width(); ++x) {
            if (qAlpha(result.pixel(x, y)) > 0)
                nonTransparent++;
        }
    }
    EXPECT_GT(nonTransparent, 50)
        << "Styled text rendering should produce visible pixels";
}

TEST(BitmapCompositorStyled, EmptyRuns_FallsBackToRegular)
{
    if (!std::filesystem::exists(kArialFontPath)) {
        GTEST_SKIP() << "arial.ttf not available";
    }

    BitmapCompositor compositor;
    std::u32string text = U"Test";

    FontMatch fm;
    fm.fontFilePath = kArialFontPath;
    fm.fontFamily = "Arial";
    fm.sizePt = 12.0f;
    fm.color = {0, 0, 0};

    std::vector<StyledRun> emptyRuns;
    QImage styled = compositor.renderStyledText(text, fm, emptyRuns, 200, 80, 300);
    QImage regular = compositor.renderText(text, fm, 200, 80, 300);

    ASSERT_FALSE(styled.isNull());
    ASSERT_FALSE(regular.isNull());
    EXPECT_EQ(styled.size(), regular.size())
        << "Empty styled runs should produce same-sized output as regular";
}

TEST(BitmapCompositorStyled, ItalicStrikethroughRun)
{
    if (!std::filesystem::exists(kArialFontPath)) {
        GTEST_SKIP() << "arial.ttf not available";
    }

    BitmapCompositor compositor;
    std::u32string text = U"Fancy";

    FontMatch fm;
    fm.fontFilePath = kArialFontPath;
    fm.fontFamily = "Arial";
    fm.sizePt = 16.0f;
    fm.color = {0, 0, 0};

    std::vector<StyledRun> runs;
    StyledRun run;
    run.charStart = 0;
    run.charEnd = 5;
    run.style.isItalic = true;
    run.style.isStrikethrough = true;
    runs.push_back(run);

    QImage result = compositor.renderStyledText(text, fm, runs, 200, 80, 300);
    ASSERT_FALSE(result.isNull());

    int nonTransparent = 0;
    for (int y = 0; y < result.height(); ++y) {
        for (int x = 0; x < result.width(); ++x) {
            if (qAlpha(result.pixel(x, y)) > 0)
                nonTransparent++;
        }
    }
    EXPECT_GT(nonTransparent, 30)
        << "Italic+strikethrough text should produce visible pixels";
}
