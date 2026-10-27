// test_embedded_font_analyzer_multiscript.cpp — Defect #8: multi-script fonts.
//
// Pipeline e7d3c2a1 Phase 7. The anti-pattern: EmbeddedFontAnalyzer collapsed a
// font to a SINGLE primaryScript by glyph-count dominance, so a genuinely
// multi-script font (e.g. NotoSans, or Arial which carries Latin+Arabic+Cyrillic)
// was reduced to "Latin" — and FontMatcher then penalized it for rendering the
// Arabic text it can perfectly handle.
//
// The fix: classifyCoverage() builds a FULL supportedScripts set from glyph-range
// coverage (every script with real glyph coverage, not just the plurality), and
// FontMetricProfile::coversScript()/distanceTo() check membership rather than
// equality with the single primaryScript.
//
// Mock-descriptor cases run unconditionally (membership semantics). Real-font
// cases prove the set is derived from ACTUAL glyph-range analysis — not a
// font-name allowlist — and are guarded only on font availability (same pattern
// as the existing ScriptFontAnalysisTest suite).

#include <gtest/gtest.h>

#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontMetricProfile.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <set>

using namespace pdfe::fonts;
using namespace pdfe::core;

namespace {

FontMetricProfile makeMatchProfile(const std::set<FontScript>& scripts,
                                    FontScript primary)
{
    FontMetricProfile p;
    p.postScriptName = "Mock";
    p.familyName = "Mock";
    p.glyphCount = 512;
    p.weightClass = 400;
    p.stemWeightV = 0.10f;
    p.xToCapRatio = 0.50f;
    p.avgCharWidth = 0.50f;
    p.primaryScript = primary;
    p.supportedScripts = scripts;
    return p;
}

// A synthetic text query carrying just the needed script (empty supportedScripts
// — the legacy/needed-side profile). Metrics match makeMatchProfile so a covered
// script gives distance 0 and an uncovered one isolates the +8 penalty.
FontMetricProfile makeTextQuery(FontScript script)
{
    FontMetricProfile q;
    q.postScriptName = "Text";
    q.familyName = "Text";
    q.glyphCount = 512;
    q.weightClass = 400;
    q.stemWeightV = 0.10f;
    q.xToCapRatio = 0.50f;
    q.avgCharWidth = 0.50f;
    q.primaryScript = script;
    return q;
}

FontMetricProfile makeArabicQuery() { return makeTextQuery(FontScript::Arabic); }

const char* kArial = "C:\\Windows\\Fonts\\arial.ttf";
const char* kTimes = "C:\\Windows\\Fonts\\times.ttf";

} // namespace

class EmbeddedFontMultiscriptTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        analyzer_ = std::make_unique<EmbeddedFontAnalyzer>(context_.get());
    }
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<EmbeddedFontAnalyzer> analyzer_;
};

// ---------------------------------------------------------------------------
// (a) A NotoSans-equivalent multi-script descriptor reports a supportedScripts
//     set containing Latin, Arabic AND CJK — and coversScript() honours every
//     member, not just the dominant primaryScript.
// ---------------------------------------------------------------------------
TEST_F(EmbeddedFontMultiscriptTest, MultiScriptDescriptor_CoversAllMemberScripts)
{
    auto noto = makeMatchProfile(
        {FontScript::Latin, FontScript::Arabic, FontScript::CJK},
        FontScript::Latin /* Latin is the glyph-count plurality */);

    EXPECT_TRUE(noto.coversScript(FontScript::Latin));
    EXPECT_TRUE(noto.coversScript(FontScript::Arabic));
    EXPECT_TRUE(noto.coversScript(FontScript::CJK));
    // A script it does NOT carry is not covered.
    EXPECT_FALSE(noto.coversScript(FontScript::Hebrew));
    // Unknown is always "covered" (fallback sentinel).
    EXPECT_TRUE(noto.coversScript(FontScript::Unknown));
}

// ---------------------------------------------------------------------------
// (b) A mono-script Latin descriptor covers ONLY Latin.
// ---------------------------------------------------------------------------
TEST_F(EmbeddedFontMultiscriptTest, MonoScriptDescriptor_CoversOnlyLatin)
{
    auto latinOnly = makeMatchProfile({FontScript::Latin}, FontScript::Latin);
    EXPECT_TRUE(latinOnly.coversScript(FontScript::Latin));
    EXPECT_FALSE(latinOnly.coversScript(FontScript::Arabic));
    EXPECT_FALSE(latinOnly.coversScript(FontScript::CJK));
}

// ---------------------------------------------------------------------------
// (c) distanceTo() — a multi-script font covering Arabic is NOT penalized for
//     Arabic text; a Latin-only font IS. This is the membership check (#8).
// ---------------------------------------------------------------------------
TEST_F(EmbeddedFontMultiscriptTest, DistanceTo_MultiScriptCoversArabic_NoPenalty)
{
    auto arabicText = makeArabicQuery();
    auto noto = makeMatchProfile(
        {FontScript::Latin, FontScript::Arabic, FontScript::CJK},
        FontScript::Latin);
    auto latinOnly = makeMatchProfile({FontScript::Latin}, FontScript::Latin);

    // NotoSans covers Arabic even though its primaryScript is Latin => no penalty.
    EXPECT_FLOAT_EQ(arabicText.distanceTo(noto), 0.0f)
        << "Multi-script font covering Arabic must not be penalized for Arabic text";
    // Latin-only font lacks Arabic coverage => +8 penalty.
    EXPECT_FLOAT_EQ(arabicText.distanceTo(latinOnly), 8.0f)
        << "Latin-only font must be penalized for Arabic text";
}

// ---------------------------------------------------------------------------
// (d) REAL font, glyph-range derived: Arial carries Latin AND Arabic glyph
//     ranges. "Arial" is NOT in any script name-override list, so the Arabic
//     membership can only have come from the cmap glyph-count analysis — proving
//     supportedScripts is built from actual coverage, not a name allowlist.
// ---------------------------------------------------------------------------
TEST_F(EmbeddedFontMultiscriptTest, RealArial_SupportedScriptsDerivedFromGlyphRanges)
{
    if (!std::filesystem::exists(kArial))
        GTEST_SKIP() << "arial.ttf not present on this host";

    auto profile = analyzer_->analyzeFromFile(kArial);
    ASSERT_TRUE(profile.has_value());

    // Dominant script stays Latin (the plurality) ...
    EXPECT_EQ(profile->primaryScript, FontScript::Latin);
    // ... but the supported set is multi-script, derived from glyph coverage.
    EXPECT_GT(profile->supportedScripts.count(FontScript::Latin), 0u)
        << "Arial must report Latin coverage";
    EXPECT_GT(profile->supportedScripts.count(FontScript::Arabic), 0u)
        << "Arial carries Arabic glyphs — must be in supportedScripts (glyph-derived, not name)";
    EXPECT_TRUE(profile->coversScript(FontScript::Arabic))
        << "coversScript(Arabic) must be true for Arial";

    // And therefore Arial is NOT penalized for Arabic text (the #8 payoff),
    // whereas a Latin-only font would be.
    auto arabicText = makeArabicQuery();
    auto latinOnly = makeMatchProfile({FontScript::Latin}, FontScript::Latin);
    EXPECT_LT(arabicText.distanceTo(*profile), arabicText.distanceTo(latinOnly))
        << "Multi-script Arial should out-score a Latin-only font for Arabic text";
}

// ---------------------------------------------------------------------------
// (e) REAL font, uncovered-script penalty: Times New Roman covers Latin (and on
//     modern Windows also Arabic/Cyrillic/Greek/Hebrew) but carries NO CJK
//     ideographs. So it is (correctly) penalized for CJK text and NOT for Latin —
//     proving the penalty keys off real glyph coverage. Because both queries
//     share identical metrics, the CJK-vs-Latin distance gap is exactly the +8
//     script penalty, with no fragile assumption about which scripts Times adds.
// ---------------------------------------------------------------------------
TEST_F(EmbeddedFontMultiscriptTest, RealTimes_UncoveredScript_Penalized)
{
    if (!std::filesystem::exists(kTimes))
        GTEST_SKIP() << "times.ttf not present on this host";

    auto profile = analyzer_->analyzeFromFile(kTimes);
    ASSERT_TRUE(profile.has_value());

    // Times genuinely covers Latin but not CJK (no Han ideographs ship with it).
    EXPECT_GT(profile->supportedScripts.count(FontScript::Latin), 0u)
        << "Times must report Latin coverage";
    EXPECT_EQ(profile->supportedScripts.count(FontScript::CJK), 0u)
        << "Times carries no CJK glyphs — must NOT claim CJK coverage";
    EXPECT_TRUE(profile->coversScript(FontScript::Latin));
    EXPECT_FALSE(profile->coversScript(FontScript::CJK));

    // Identical-metric queries differing ONLY in needed script: the CJK query
    // triggers the +8 penalty (uncovered), the Latin query does not (covered).
    const float latinDist = makeTextQuery(FontScript::Latin).distanceTo(*profile);
    const float cjkDist = makeTextQuery(FontScript::CJK).distanceTo(*profile);
    EXPECT_NEAR(cjkDist - latinDist, 8.0f, 1e-3f)
        << "CJK (uncovered) must add exactly the +8 script penalty vs Latin (covered)";
    EXPECT_GT(cjkDist, latinDist)
        << "Times must be penalized more for CJK than for Latin";
}
