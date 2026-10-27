// test_font_matcher_per_run_script.cpp — Defect #7: per-run script detection.
//
// Pipeline e7d3c2a1 Phase 7. The anti-pattern: FontMatcher collapsed a whole
// region to ONE dominant script (detectScript over the entire region) and used
// that single primaryScript to drive font matching. A minority-script run inside
// a majority-script region (e.g. an Arabic numeral group inside Latin body text)
// was therefore matched as the WRONG script, so the correct font for that run
// incurred a spurious script-mismatch penalty.
//
// The fix: segment text into per-script runs (FontMatcher::segmentScriptRuns),
// detect each run's OWN script (FontMatcher::scriptForRun), and drive the
// distanceTo() script penalty from the per-run script
// (FontMetricProfile::distanceTo(other, queryScriptOverride)) — so each run is
// matched against fonts appropriate for ITS script, not the region majority.
//
// These are deterministic unit tests over the per-run primitives; no PDF / system
// fonts required. The on-fixture exercise lives in test_font_script_integration.

#include <gtest/gtest.h>

#include "tre/FontMatcher.h"
#include "fonts/FontMetricProfile.h"

#include <string>
#include <vector>

using namespace pdfe::tre;
using namespace pdfe::fonts;

namespace {

// A candidate-font profile with identical continuous metrics to the query, so
// distanceTo() isolates ONLY the script penalty. `scripts` becomes the font's
// glyph-coverage set (defect #8 membership); `primary` its dominant script.
FontMetricProfile makeFontProfile(const std::set<FontScript>& scripts,
                                   FontScript primary)
{
    FontMetricProfile p;
    p.postScriptName = "Candidate";
    p.familyName = "Candidate";
    p.glyphCount = 256;
    p.weightClass = 400;
    p.stemWeightV = 0.10f;
    p.xToCapRatio = 0.50f;
    p.avgCharWidth = 0.50f;
    p.primaryScript = primary;
    p.supportedScripts = scripts;
    return p;
}

// A synthetic per-run query profile carrying just the run's script (identical
// continuous metrics to makeFontProfile, so a covered script => distance 0).
FontMetricProfile makeQueryProfile(FontScript runScript)
{
    FontMetricProfile q;
    q.postScriptName = "Query";
    q.familyName = "Query";
    q.glyphCount = 256;
    q.weightClass = 400;
    q.stemWeightV = 0.10f;
    q.xToCapRatio = 0.50f;
    q.avgCharWidth = 0.50f;
    q.primaryScript = runScript;
    return q;
}

// "Hello " (6 Latin+space) then 3 Arabic letters (alif, lam, ayn).
std::u32string latinThenArabic()
{
    std::u32string s = U"Hello ";
    s.push_back(0x0627);
    s.push_back(0x0644);
    s.push_back(0x0639);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Case 1: pure-Latin region — one Latin run.
// ---------------------------------------------------------------------------
TEST(FontMatcherPerRunScript, PureLatinRegion_SingleLatinRun)
{
    const std::u32string text = U"The quick brown fox";
    const auto runs = FontMatcher::segmentScriptRuns(text);

    ASSERT_EQ(runs.size(), 1u) << "Single-script region must be one run";
    EXPECT_EQ(runs[0].script, FontScript::Latin);
    EXPECT_EQ(runs[0].start, 0u);
    EXPECT_EQ(runs[0].end, text.size());
    EXPECT_EQ(FontMatcher::scriptForRun(text, runs[0].start, runs[0].end),
              FontScript::Latin);
}

// ---------------------------------------------------------------------------
// Case 2: pure-Arabic region — one Arabic run.
// ---------------------------------------------------------------------------
TEST(FontMatcherPerRunScript, PureArabicRegion_SingleArabicRun)
{
    std::u32string text;
    for (char32_t c : {0x0627, 0x0644, 0x0639, 0x0631, 0x0628, 0x064A, 0x0629})
        text.push_back(c);

    const auto runs = FontMatcher::segmentScriptRuns(text);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].script, FontScript::Arabic);
    EXPECT_EQ(FontMatcher::scriptForRun(text, 0, text.size()),
              FontScript::Arabic);
}

// ---------------------------------------------------------------------------
// Case 3: mixed Latin+Arabic region — segments into two correct runs, and each
// run, matched with ITS own script, picks the right-script font with no penalty.
// ---------------------------------------------------------------------------
TEST(FontMatcherPerRunScript, MixedLatinArabic_BothRunsCorrect)
{
    const std::u32string text = latinThenArabic();  // "Hello " + 3 Arabic
    const auto runs = FontMatcher::segmentScriptRuns(text);

    ASSERT_EQ(runs.size(), 2u) << "Mixed region must split into 2 runs";
    EXPECT_EQ(runs[0].script, FontScript::Latin);
    EXPECT_EQ(runs[1].script, FontScript::Arabic);
    // The trailing space attaches to the leading Latin run (script-neutral).
    EXPECT_EQ(runs[0].end, 6u);
    EXPECT_EQ(runs[1].start, 6u);

    // Per-run script detection.
    EXPECT_EQ(FontMatcher::scriptForRun(text, runs[0].start, runs[0].end),
              FontScript::Latin);
    EXPECT_EQ(FontMatcher::scriptForRun(text, runs[1].start, runs[1].end),
              FontScript::Arabic);

    // Each run matched against the correct-script font => zero script penalty.
    auto latinFont = makeFontProfile({FontScript::Latin}, FontScript::Latin);
    auto arabicFont = makeFontProfile({FontScript::Arabic}, FontScript::Arabic);

    auto latinQuery = makeQueryProfile(FontScript::Latin);
    auto arabicQuery = makeQueryProfile(FontScript::Arabic);

    EXPECT_FLOAT_EQ(latinQuery.distanceTo(latinFont, FontScript::Latin), 0.0f);
    EXPECT_FLOAT_EQ(arabicQuery.distanceTo(arabicFont, FontScript::Arabic), 0.0f);
    // ...and cross-matched runs DO incur the penalty.
    EXPECT_FLOAT_EQ(arabicQuery.distanceTo(latinFont, FontScript::Arabic), 8.0f);
    EXPECT_FLOAT_EQ(latinQuery.distanceTo(arabicFont, FontScript::Latin), 8.0f);
}

// ---------------------------------------------------------------------------
// Case 4: single Arabic run in a Latin-DOMINANT region — the heart of the bug.
// Region detectScript() returns Latin (the majority), but the Arabic run's own
// script must be Arabic, so an Arabic font incurs NO penalty for that run.
// ---------------------------------------------------------------------------
TEST(FontMatcherPerRunScript, SingleArabicRunInLatinDominantRegion_NoFalsePenalty)
{
    // Long Latin body with a short Arabic insert in the middle.
    std::u32string text = U"the invoice total is ";
    const size_t arabicStart = text.size();
    text.push_back(0x0627);
    text.push_back(0x0644);
    text.push_back(0x0631);
    const size_t arabicEnd = text.size();
    text += U" due now";

    // The whole-region (old) verdict is Latin-dominant...
    EXPECT_EQ(FontMatcher::detectScript(text), FontScript::Latin)
        << "Region majority is Latin — the trap the old code fell into";

    // ...but the Arabic run is detected as Arabic.
    EXPECT_EQ(FontMatcher::scriptForRun(text, arabicStart, arabicEnd),
              FontScript::Arabic);

    const auto runs = FontMatcher::segmentScriptRuns(text);
    ASSERT_EQ(runs.size(), 3u) << "Latin | Arabic | Latin";
    EXPECT_EQ(runs[0].script, FontScript::Latin);
    EXPECT_EQ(runs[1].script, FontScript::Arabic);
    EXPECT_EQ(runs[2].script, FontScript::Latin);

    // Matching the Arabic run against an Arabic font must NOT be penalized even
    // though the surrounding region is majority Latin (the false penalty).
    auto arabicFont = makeFontProfile({FontScript::Arabic}, FontScript::Arabic);
    auto arabicQuery = makeQueryProfile(FontScript::Arabic);
    EXPECT_FLOAT_EQ(arabicQuery.distanceTo(arabicFont, FontScript::Arabic), 0.0f)
        << "Arabic run must not be penalized against an Arabic font";

    // And a multi-script font that COVERS Arabic (defect #8) is likewise unpenalized.
    auto notoLike = makeFontProfile(
        {FontScript::Latin, FontScript::Arabic, FontScript::CJK},
        FontScript::Latin);
    EXPECT_FLOAT_EQ(arabicQuery.distanceTo(notoLike, FontScript::Arabic), 0.0f)
        << "Multi-script font covering Arabic must not be penalized for the Arabic run";
}

// ---------------------------------------------------------------------------
// Case 5: distanceTo() per-run override — matching vs mismatching script.
// ---------------------------------------------------------------------------
TEST(FontMatcherPerRunScript, DistanceTo_PerRunScript_MatchingVsMismatching)
{
    auto query = makeQueryProfile(FontScript::Latin);  // region/profile hint = Latin
    auto arabicFont = makeFontProfile({FontScript::Arabic}, FontScript::Arabic);
    auto latinFont = makeFontProfile({FontScript::Latin}, FontScript::Latin);

    // Override says THIS run is Arabic, regardless of the profile's Latin hint:
    //   - Arabic font (covers Arabic) => no penalty.
    //   - Latin font (no Arabic)      => +8 penalty.
    EXPECT_FLOAT_EQ(query.distanceTo(arabicFont, FontScript::Arabic), 0.0f);
    EXPECT_FLOAT_EQ(query.distanceTo(latinFont, FontScript::Arabic), 8.0f);

    // With NO override the legacy region hint (Latin) drives the penalty:
    //   - Latin font => no penalty, Arabic font => +8.
    EXPECT_FLOAT_EQ(query.distanceTo(latinFont), 0.0f);
    EXPECT_FLOAT_EQ(query.distanceTo(arabicFont), 8.0f);
}

// ---------------------------------------------------------------------------
// Case 6: script-neutral codepoints (digits, spaces, punctuation) do NOT split
// a run — "ABC 123, DEF" is one Latin run, not three.
// ---------------------------------------------------------------------------
TEST(FontMatcherPerRunScript, NeutralCharsDoNotFragmentRuns)
{
    const std::u32string text = U"ABC 123, DEF-456";
    const auto runs = FontMatcher::segmentScriptRuns(text);

    ASSERT_EQ(runs.size(), 1u)
        << "Neutral chars must extend the surrounding run, not split it";
    EXPECT_EQ(runs[0].script, FontScript::Latin);
    EXPECT_EQ(runs[0].end, text.size());
}

// ---------------------------------------------------------------------------
// Case 7: empty / neutral-only text yields no scripted run, and the empty
// region query is treated as Unknown (no penalty, fallback matching allowed).
// ---------------------------------------------------------------------------
TEST(FontMatcherPerRunScript, NeutralOnlyText_NoScriptedRun)
{
    const std::u32string neutral = U"123 ... --- 456";
    const auto runs = FontMatcher::segmentScriptRuns(neutral);
    // A single run may exist but its script is Unknown (no scripted codepoints).
    for (const auto& r : runs)
        EXPECT_EQ(r.script, FontScript::Unknown);
    EXPECT_EQ(FontMatcher::scriptForRun(neutral, 0, neutral.size()),
              FontScript::Unknown);

    EXPECT_TRUE(FontMatcher::segmentScriptRuns(std::u32string()).empty())
        << "Empty text has no runs";
}
