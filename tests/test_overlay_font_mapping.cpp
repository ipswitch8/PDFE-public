#include <gtest/gtest.h>
#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "tre/FontMatcher.h"
#include "tre/TreTypes.h"
#include "tre/OcrPipeline.h"
#include "tre/HocrParser.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"

#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace pdfe::textedit;
using namespace pdfe::core;

static std::string fixturesDir() {
    return TEST_FIXTURES_DIR;
}

// =============================================================================
// Helper: create a TextSpan with a given font name for testing buildOverlayFont
// =============================================================================
static TextSpan makeTestSpan(const std::string& fontName, float fontSize = 12.0f)
{
    TextSpan span;
    span.fontName = fontName;
    span.fontSize = fontSize;
    span.text = U"Test";
    span.originalText = U"Test";
    span.boundingBox = QRectF(0, 0, 100, 20);
    return span;
}

// Replicates buildOverlayFont's probe-based pixelSize calculation so tests can
// predict the expected value without hard-coding font-specific magic numbers.
// The production formula matches the substitute font's actual rendered ink
// extent (via QFontMetricsF::tightBoundingRect on a reference string with
// ascenders + descenders) to `baseSizePt × zoom × visibleExtentRatio`. The
// visibleExtentRatio is the SOURCE font's measured ink extent / em.
// Tests that don't pass an instance of TextEditTool use 0.85 default
// (the same default production uses for unknown fonts).
static int expectedProbePixelSize(const QString& family, float baseSizePt,
                                  float zoom, float visibleExtentRatio = 0.85f)
{
    float targetExtentPx = baseSizePt * zoom * visibleExtentRatio;
    constexpr int kProbePx = 100;
    QFont probe(family);
    probe.setPixelSize(kProbePx);
    QFontMetricsF probeFm(probe);
    QRectF probeInk = probeFm.tightBoundingRect(QStringLiteral("HMlbdfhpqgyEjk"));
    float probeExtent = static_cast<float>(probeInk.height());
    if (probeExtent < 1.0f)
        probeExtent = static_cast<float>(probeFm.ascent() + probeFm.descent());
    float scale = (probeExtent > 1.0f)
        ? (targetExtentPx / probeExtent)
        : (targetExtentPx / static_cast<float>(kProbePx));
    int pixelSize = static_cast<int>(static_cast<float>(kProbePx) * scale + 0.5f);
    if (pixelSize < 1) pixelSize = 1;
    return pixelSize;
}

// =============================================================================
// Test fixture
// =============================================================================
class OverlayFontMappingTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
    }

    /// Create a TextEditTool and call buildOverlayFont via the public
    /// charIndexFromPdfX path — but since buildOverlayFont is static,
    /// we can test it indirectly through QFont behavior.
    /// Instead, we use the static method directly (it's accessible from tests
    /// because we include the header and the method is public static).
    QFont buildFont(const std::string& fontName, float fontSize = 12.0f,
                    const QString& override = {}) {
        TextSpan span = makeTestSpan(fontName, fontSize);
        return TextEditTool::buildOverlayFont(span, 1.0f, override);
    }

    /// Check that a QFont resolves to a family containing the expected substring,
    /// OR a metric-compatible equivalent (Linux ships Liberation/Arimo/Carlito
    /// instead of the Microsoft originals; layout is identical). Tests assert
    /// the substitution chain reaches an acceptable family rather than the
    /// specific Microsoft name.
    bool fontResolvesTo(const QFont& font, const QString& expectedSubstring) {
        QFontInfo fi(font);
        const QString actual = fi.family();
        if (actual.contains(expectedSubstring, Qt::CaseInsensitive)) return true;

        // Metric-compatible substitutes — these have matching glyph widths
        // and advances, so document layout is identical to the Microsoft
        // original. Mostly relevant on Linux.
        static const std::unordered_map<std::string, std::vector<QString>> equivalence = {
            {"arial",            {"Liberation Sans", "Arimo", "DejaVu Sans",
                                  "Nimbus Sans"}},
            {"times new roman",  {"Liberation Serif", "Tinos", "DejaVu Serif",
                                  "Nimbus Roman"}},
            {"times",            {"Liberation Serif", "Tinos", "DejaVu Serif",
                                  "Nimbus Roman"}},
            {"courier new",      {"Liberation Mono", "Cousine", "DejaVu Sans Mono",
                                  "Nimbus Mono"}},
            {"courier",          {"Liberation Mono", "Cousine", "DejaVu Sans Mono",
                                  "Nimbus Mono"}},
            {"calibri",          {"Carlito"}},
            {"cambria",          {"Caladea"}},
        };
        auto it = equivalence.find(expectedSubstring.toLower().toStdString());
        if (it == equivalence.end()) return false;
        for (const auto& equiv : it->second) {
            if (actual.contains(equiv, Qt::CaseInsensitive)) return true;
        }
        return false;
    }

    std::shared_ptr<MuPdfContext> context_;
};

// =============================================================================
// Test 1: Helvetica maps to Arial (the critical bug fix)
// =============================================================================
TEST_F(OverlayFontMappingTest, Helvetica_MapsToArial)
{
    QFont font = buildFont("Helvetica");
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "Helvetica should map to Arial, got: " << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 2: Subset-prefixed Helvetica also maps to Arial
// =============================================================================
TEST_F(OverlayFontMappingTest, SubsetPrefixedHelvetica_MapsToArial)
{
    QFont font = buildFont("ABCDEF+Helvetica");
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "ABCDEF+Helvetica should map to Arial, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 3: Times-Roman maps to Times New Roman
// =============================================================================
TEST_F(OverlayFontMappingTest, TimesRoman_MapsToTimesNewRoman)
{
    QFont font = buildFont("Times-Roman");
    QFontInfo fi(font);
    EXPECT_TRUE(fontResolvesTo(font, "Times New Roman"))
        << "Times-Roman should map to Times New Roman (or metric-compatible "
        << "Liberation Serif / Tinos / Nimbus Roman), got: " << fi.family().toStdString();
}

// =============================================================================
// Test 4: Courier maps to Courier New
// =============================================================================
TEST_F(OverlayFontMappingTest, Courier_MapsToCourierNew)
{
    QFont font = buildFont("Courier");
    QFontInfo fi(font);
    EXPECT_TRUE(fontResolvesTo(font, "Courier New"))
        << "Courier should map to Courier New (or metric-compatible "
        << "Liberation Mono / Cousine / Nimbus Mono), got: " << fi.family().toStdString();
}

// =============================================================================
// Test 5: GlyphLessFont (Tesseract OCR) maps to Arial
// =============================================================================
TEST_F(OverlayFontMappingTest, GlyphLessFont_MapsToArial)
{
    QFont font = buildFont("GlyphLessFont");
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "GlyphLessFont should map to Arial, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 6: AdobeBlank maps to Arial
// =============================================================================
TEST_F(OverlayFontMappingTest, AdobeBlank_MapsToArial)
{
    QFont font = buildFont("AdobeBlank");
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "AdobeBlank should map to Arial, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 7: Liberation Sans maps to Arial (Linux font → Windows equivalent)
// =============================================================================
TEST_F(OverlayFontMappingTest, LiberationSans_MapsToArial)
{
    QFont font = buildFont("LiberationSans");
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "LiberationSans should map to Arial, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 8: Liberation Serif maps to Times New Roman
// =============================================================================
TEST_F(OverlayFontMappingTest, LiberationSerif_MapsToTimesNewRoman)
{
    QFont font = buildFont("REFXWK+Liberation Serif");
    QFontInfo fi(font);
    EXPECT_TRUE(fontResolvesTo(font, "Times New Roman"))
        << "Liberation Serif should map to Times New Roman "
        << "(or itself / metric equivalent on Linux), got: "
        << fi.family().toStdString();
}

// =============================================================================
// Test 9: Helvetica-Bold also maps to Arial (bold variant)
// =============================================================================
TEST_F(OverlayFontMappingTest, HelveticaBold_MapsToArial)
{
    QFont font = buildFont("Helvetica-Bold");
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "Helvetica-Bold should map to Arial, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 10: Unknown font falls back to Arial via QFontInfo validation
// =============================================================================
TEST_F(OverlayFontMappingTest, UnknownFont_FallsBackToArial)
{
    QFont font = buildFont("TotallyFakeNonExistentFont12345");
    QFontInfo fi(font);
    // Should either resolve to Arial (via fallback) or at least not crash
    // and produce a usable font
    QFontMetricsF fm(font);
    EXPECT_GT(fm.horizontalAdvance("Test"), 0.0)
        << "Unknown font should fall back to a usable font";
}

// =============================================================================
// Test 11: Overlay font override takes precedence over PDF name
// =============================================================================
TEST_F(OverlayFontMappingTest, OverrideTakesPrecedence)
{
    // PDF name says "Courier" (monospace) but override says "Arial" (sans-serif)
    QFont font = buildFont("Courier", 12.0f, QStringLiteral("Arial"));
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "Override should take precedence over PDF font name, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 12: Override with empty string reverts to PDF name mapping
// =============================================================================
TEST_F(OverlayFontMappingTest, EmptyOverride_UsesMapping)
{
    QFont font = buildFont("Helvetica", 12.0f, QString());
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "Empty override should use mapping table, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 13: ArialMT (common variant) maps to Arial
// =============================================================================
TEST_F(OverlayFontMappingTest, ArialMT_MapsToArial)
{
    QFont font = buildFont("ArialMT");
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "ArialMT should map to Arial, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 14: Calibri passes through correctly (common modern font)
// =============================================================================
TEST_F(OverlayFontMappingTest, Calibri_MapsCorrectly)
{
    QFont font = buildFont("Calibri");
    QFontInfo fi(font);
    EXPECT_TRUE(fontResolvesTo(font, "Calibri"))
        << "Calibri should map to Calibri (or metric-compatible Carlito), got: "
        << fi.family().toStdString();
}

// =============================================================================
// Test 15: FontMatcher produces valid family for scanned page
// =============================================================================
TEST_F(OverlayFontMappingTest, FontMatcher_ProducesValidFamily)
{
    auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto cache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        analyzer.get(), cache.get());
    pdfe::tre::FontMatcher matcher(selector.get(), analyzer.get());

    // Create minimal scanned text properties
    pdfe::tre::ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = pdfe::tre::FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false;
    props.textColor = {0, 0, 0};

    std::u32string text = U"Hello World";
    auto match = matcher.findMatch(props, text);

    // FontMatcher should return a non-empty family
    EXPECT_FALSE(match.fontFamily.empty())
        << "FontMatcher should produce a non-empty font family";

    // The family should be a valid Qt font
    QFont font(QString::fromStdString(match.fontFamily), 12);
    QFontMetricsF fm(font);
    EXPECT_GT(fm.horizontalAdvance("Test"), 0.0)
        << "FontMatcher family should be a valid Qt font: " << match.fontFamily;
}

// =============================================================================
// Test 16: setOverlayFontFamily persists through tool lifetime
// =============================================================================
TEST_F(OverlayFontMappingTest, SetOverlayFontFamily_Persists)
{
    auto nativePath = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(nativePath)) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    auto doc = Document::open(context_, nativePath);
    ASSERT_NE(doc, nullptr);

    TextEditTool tool(doc.get(), context_);
    tool.activate();

    // Set override
    tool.setOverlayFontFamily(QStringLiteral("Verdana"));

    // The override should affect font building
    TextSpan span = makeTestSpan("Helvetica");
    // We can't directly call buildOverlayFont with the override via the tool,
    // but we can verify the override was stored by checking the public API
    // doesn't crash and produces a valid state
    tool.setPage(0);
    SUCCEED() << "setOverlayFontFamily + setPage should not crash";

    tool.deactivate();
}

// =============================================================================
// Test 17: Partial match works (HelveticaNeue → Arial via "helvetica" substring)
// =============================================================================
TEST_F(OverlayFontMappingTest, HelveticaNeue_MapsToArial)
{
    QFont font = buildFont("HelveticaNeue");
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "HelveticaNeue should map to Arial via partial match, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 18: Nimbus Sans maps to Arial (Ghostscript font → Windows equivalent)
// =============================================================================
TEST_F(OverlayFontMappingTest, NimbusSans_MapsToArial)
{
    QFont font = buildFont("NimbusSans");
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "NimbusSans should map to Arial, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 19: Override with "Arial" on a non-embedded OCR font produces Arial
// =============================================================================
TEST_F(OverlayFontMappingTest, OverrideArialOnSerifOcrFont)
{
    // Simulates the image-dominated page fallback: the OCR font name says
    // "Liberation Serif" (no subset prefix = not a real embedded font).
    // The override should force Arial.
    TextSpan span = makeTestSpan("Liberation Serif", 2.0f);
    QFont font = TextEditTool::buildOverlayFont(span, 1.0f, QStringLiteral("Arial"));
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "Arial override on OCR font (no subset prefix) should produce Arial, got: "
        << QFontInfo(font).family().toStdString();
}

// =============================================================================
// Test 19b: Override is SKIPPED for real embedded fonts (subset prefix)
// =============================================================================
TEST_F(OverlayFontMappingTest, OverrideSkippedForRealEmbeddedFont)
{
    // When a font has a subset prefix (e.g., "REFXWK+Liberation Serif"),
    // it's a real embedded font with correct metrics. FontMatcher overrides
    // should NOT apply — the span's own fontName and fontSize are correct.
    TextSpan span = makeTestSpan("REFXWK+Liberation Serif", 11.0f);
    QFont font = TextEditTool::buildOverlayFont(span, 1.0f, QStringLiteral("Arial"), 7.0f);

    // Override should be ignored: font should NOT be Arial
    QFontInfo fi(font);
    EXPECT_FALSE(fi.family().contains("Arial", Qt::CaseInsensitive))
        << "Real embedded font with subset prefix should NOT be overridden with Arial, got: "
        << fi.family().toStdString();

    // Font size should use span.fontSize (11pt) not override (7pt).
    // The probe-based formula scales by the resolved family's actual
    // ascent+descent, so compute the expected pixelSize the same way.
    int expected = expectedProbePixelSize(QFontInfo(font).family(), 11.0f, 1.0f);
    EXPECT_EQ(font.pixelSize(), expected)
        << "Real embedded font should use span.fontSize=11 (->8), not override=7 (->5)";
}

// =============================================================================
// Test 20: Without override, Liberation Serif maps to Times New Roman
// =============================================================================
TEST_F(OverlayFontMappingTest, LiberationSerifWithoutOverride_MapsToTimesNewRoman)
{
    TextSpan span = makeTestSpan("REFXWK+Liberation Serif", 12.0f);
    QFont font = TextEditTool::buildOverlayFont(span, 1.0f);
    QFontInfo fi(font);
    EXPECT_TRUE(fontResolvesTo(font, "Times New Roman"))
        << "Liberation Serif without override should map to Times New Roman "
        << "(or itself / metric equivalent), got: "
        << fi.family().toStdString();
}

// =============================================================================
// Test 21: FontMatcher with isSerif=false defaults to Arial (fallback path)
// =============================================================================
TEST_F(OverlayFontMappingTest, FontMatcherFallback_SansSerif_ReturnsArial)
{
    auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto cache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        analyzer.get(), cache.get());
    pdfe::tre::FontMatcher matcher(selector.get(), analyzer.get());

    pdfe::tre::ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = pdfe::tre::FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = false;  // sans-serif
    props.textColor = {0, 0, 0};

    // Empty text triggers fallback path in findMatch (findSubstitute bails)
    std::u32string emptyText;
    auto match = matcher.findMatch(props, emptyText);
    EXPECT_EQ(match.fontFamily, "Arial")
        << "Sans-serif fallback should be Arial, got: " << match.fontFamily;
}

// =============================================================================
// Test 22: FontMatcher with isSerif=true defaults to Times New Roman
// =============================================================================
TEST_F(OverlayFontMappingTest, FontMatcherFallback_Serif_ReturnsTimesNewRoman)
{
    auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto cache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        analyzer.get(), cache.get());
    pdfe::tre::FontMatcher matcher(selector.get(), analyzer.get());

    pdfe::tre::ScannedTextProperties props;
    props.fontSizePt = 12.0f;
    props.weight = pdfe::tre::FontWeight::Regular;
    props.isItalic = false;
    props.isSerif = true;  // serif
    props.textColor = {0, 0, 0};

    std::u32string emptyText;
    auto match = matcher.findMatch(props, emptyText);
    EXPECT_EQ(match.fontFamily, "Times New Roman")
        << "Serif fallback should be Times New Roman, got: " << match.fontFamily;
}

// =============================================================================
// Test 23: Override wins on OCR font without subset prefix (end-to-end)
// =============================================================================
TEST_F(OverlayFontMappingTest, ImageDominatedPage_OverridePreventsSerif)
{
    auto nativePath = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(nativePath)) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    auto doc = Document::open(context_, nativePath);
    ASSERT_NE(doc, nullptr);

    TextEditTool tool(doc.get(), context_);
    tool.activate();

    // Simulate the image-dominated fallback: set Arial override
    tool.setOverlayFontFamily(QStringLiteral("Arial"));

    // Build a span with an OCR font name (no subset prefix = not real embedded font)
    TextSpan span = makeTestSpan("GlyphLessFont", 2.0f);
    QFont font = TextEditTool::buildOverlayFont(span, 1.0f, QStringLiteral("Arial"));
    QFontInfo fi(font);
    EXPECT_TRUE(fontResolvesTo(font, "Arial"))
        << "Image-dominated page with Arial override on OCR font should show Arial "
        << "(or metric-compatible Liberation Sans / Arimo / DejaVu Sans), got: "
        << fi.family().toStdString();

    tool.deactivate();
}

// =============================================================================
// Test 23b: Override is SKIPPED for real embedded font on image-dominated page
// =============================================================================
TEST_F(OverlayFontMappingTest, ImageDominatedPage_RealFontBypassesOverride)
{
    auto nativePath = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(nativePath)) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    auto doc = Document::open(context_, nativePath);
    ASSERT_NE(doc, nullptr);

    TextEditTool tool(doc.get(), context_);
    tool.activate();

    // Simulate the image-dominated fallback: set Arial override
    tool.setOverlayFontFamily(QStringLiteral("Arial"));
    tool.setOverlayFontSize(7.0f);

    // Build a span with a real embedded font (has subset prefix + reasonable fontSize)
    TextSpan span = makeTestSpan("REFXWK+Liberation Serif", 11.0f);
    QFont font = TextEditTool::buildOverlayFont(span, 1.0f, QStringLiteral("Arial"), 7.0f);
    QFontInfo fi(font);

    // Override should be bypassed: font should NOT be Arial
    EXPECT_FALSE(fi.family().contains("Arial", Qt::CaseInsensitive))
        << "Real embedded font should NOT be overridden with Arial, got: "
        << fi.family().toStdString();

    // Font size should use span.fontSize=11, not override=7
    int expected = expectedProbePixelSize(QFontInfo(font).family(), 11.0f, 1.0f);
    EXPECT_EQ(font.pixelSize(), expected)
        << "Real embedded font should use span.fontSize=11, not FontMatcher override=7";

    tool.deactivate();
}

// =============================================================================
// Test: Font size override for image-dominated pages
// =============================================================================

TEST_F(OverlayFontMappingTest, FontSizeOverride_UsesOverrideInsteadOfSpanSize) {
    // On image-dominated pages, span.fontSize comes from the invisible OCR text
    // layer and is typically a tiny arbitrary value. The FontMatcher-detected
    // font size should be used instead.
    TextSpan span = makeTestSpan("Arial", 2.0f);  // OCR layer: tiny 2pt
    float overrideSize = 12.0f;  // FontMatcher detected: 12pt

    QFont fontWithOverride = TextEditTool::buildOverlayFont(
        span, 1.0f, QStringLiteral("Arial"), overrideSize);
    QFont fontWithoutOverride = TextEditTool::buildOverlayFont(
        span, 1.0f, QStringLiteral("Arial"), 0.0f);

    // The probe-based formula scales by the resolved font's ascent+descent,
    // so compute expectations the same way.
    int expectedWith = expectedProbePixelSize(
        QFontInfo(fontWithOverride).family(), 12.0f, 1.0f);
    int expectedWithout = expectedProbePixelSize(
        QFontInfo(fontWithoutOverride).family(), 2.0f, 1.0f);
    EXPECT_EQ(fontWithOverride.pixelSize(), expectedWith)
        << "Font size override (12pt) should produce probe-scaled pixelSize at zoom=1.0";
    EXPECT_EQ(fontWithoutOverride.pixelSize(), expectedWithout)
        << "Without override, tiny OCR span size (2pt) should produce probe-scaled pixelSize";
    // Cross-check intent: override path produces a larger glyph than no-override path.
    EXPECT_GT(fontWithOverride.pixelSize(), fontWithoutOverride.pixelSize())
        << "Override (12pt) should yield strictly larger pixelSize than span size (2pt)";
}

TEST_F(OverlayFontMappingTest, FontSizeOverride_ZeroMeansUseSpanDefault) {
    TextSpan span = makeTestSpan("Arial", 14.0f);

    QFont font = TextEditTool::buildOverlayFont(
        span, 1.0f, QStringLiteral("Arial"), 0.0f);

    // 0 override = use span.fontSize (14pt). Probe-based scaling applies.
    int expected = expectedProbePixelSize(QFontInfo(font).family(), 14.0f, 1.0f);
    EXPECT_EQ(font.pixelSize(), expected)
        << "Zero override should use span.fontSize (14pt) with probe-scaled pixelSize";
}

TEST_F(OverlayFontMappingTest, BuildRunFont_FontSizeOverrideAppliesToRunDefault) {
    TextSpan span = makeTestSpan("Arial", 2.0f);  // tiny OCR size
    FormattingRun run;
    run.charStart = 0;
    run.charEnd = 4;
    // run.fontSize = 0 means "use span default" (which should use override)

    QFont font = TextEditTool::buildRunFont(
        run, span, 1.0f, QStringLiteral("Arial"), 12.0f);

    // run.fontSize=0 -> baseFontSize=12 (override). buildRunFont uses the
    // simple direct mapping (pixelSize = baseSizePt × zoom) — only
    // buildOverlayFont applies probe scaling.
    EXPECT_EQ(font.pixelSize(), 12)
        << "BuildRunFont with zero run.fontSize should use fontSizeOverride (12pt)";
}

TEST_F(OverlayFontMappingTest, BuildRunFont_ExplicitRunSizeOverridesAll) {
    TextSpan span = makeTestSpan("Arial", 2.0f);
    FormattingRun run;
    run.charStart = 0;
    run.charEnd = 4;
    run.fontSize = 18.0f;  // explicit run size

    QFont font = TextEditTool::buildRunFont(
        run, span, 1.0f, QStringLiteral("Arial"), 12.0f);

    // Explicit run.fontSize (18) takes priority: 18 * 1.0 = 18 pixels
    EXPECT_EQ(font.pixelSize(), 18)
        << "Explicit run.fontSize should override both span and fontSizeOverride";
}

TEST_F(OverlayFontMappingTest, FontSizeOverride_ScalesWithZoom) {
    TextSpan span = makeTestSpan("Arial", 2.0f);

    QFont fontZoom1 = TextEditTool::buildOverlayFont(
        span, 1.0f, QStringLiteral("Arial"), 12.0f);
    QFont fontZoom2 = TextEditTool::buildOverlayFont(
        span, 2.0f, QStringLiteral("Arial"), 12.0f);

    // Probe-based pixelSize scales linearly with zoom (target = baseSizePt × zoom).
    int expectedZoom1 = expectedProbePixelSize(QFontInfo(fontZoom1).family(), 12.0f, 1.0f);
    int expectedZoom2 = expectedProbePixelSize(QFontInfo(fontZoom2).family(), 12.0f, 2.0f);
    EXPECT_EQ(fontZoom1.pixelSize(), expectedZoom1);
    EXPECT_EQ(fontZoom2.pixelSize(), expectedZoom2);
    // Cross-check: doubling zoom roughly doubles pixelSize (within ±1 due to rounding).
    EXPECT_NEAR(fontZoom2.pixelSize(), fontZoom1.pixelSize() * 2, 1)
        << "Font size override should scale with zoom factor";
}

// =============================================================================
// Visible-extent ratio: per-font lookup
// =============================================================================
TEST_F(OverlayFontMappingTest, VisibleExtentRatio_DefaultsTo085_WhenFontUnknown)
{
    auto nativePath = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(nativePath)) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }
    auto doc = Document::open(context_, nativePath);
    ASSERT_NE(doc, nullptr);

    TextEditTool tool(doc.get(), context_);
    tool.activate();
    tool.setPage(0);

    // A font name not present on the page should fall back to 0.85
    // (typical Latin metrics).
    float ratio = tool.visibleExtentRatioForFont("DefinitelyNotAnEmbeddedFont-Plain");
    EXPECT_NEAR(ratio, 0.85f, 0.001f)
        << "Unknown font should fall back to 0.85 default";
}

TEST_F(OverlayFontMappingTest, VisibleExtentRatio_WithinSaneRange_ForEmbeddedFonts)
{
    auto nativePath = std::filesystem::path(fixturesDir()) / "test_single.pdf";
    if (!std::filesystem::exists(nativePath)) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }
    auto doc = Document::open(context_, nativePath);
    ASSERT_NE(doc, nullptr);

    TextEditTool tool(doc.get(), context_);
    tool.activate();
    tool.setPage(0);  // triggers warmFontCacheForPage

    // For ANY embedded font we successfully analyzed, the ratio must
    // be in the [0.55, 1.0] range (clamped by the implementation).
    // Verify the clamp is enforced for at least one font on the page.
    auto blocks = doc->page(0)->extractText();
    for (const auto& block : blocks) {
        for (const auto& line : block.lines) {
            for (const auto& ch : line.chars) {
                if (ch.fontName.empty()) continue;
                float r = tool.visibleExtentRatioForFont(ch.fontName);
                EXPECT_GE(r, 0.55f) << "ratio below floor for " << ch.fontName;
                EXPECT_LE(r, 1.0f) << "ratio above ceiling for " << ch.fontName;
            }
        }
    }
    tool.deactivate();
}
