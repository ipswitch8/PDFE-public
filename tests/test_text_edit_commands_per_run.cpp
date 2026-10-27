// tests/test_text_edit_commands_per_run.cpp
//
// SAVE-08 Phase 3 — caller migration tests.
//
// Verifies that EditTextCommand's caller-side substitution builder
// (buildSubstitutionFromFormattingRuns) produces SubstitutionInfo
// records whose runSegments mirror the input FormattingRuns one-for-
// one, in charStart order, with the documented fallback rules:
//
//   1. Multi-run input  -> multi-segment SubstitutionInfo
//   2. Single-run input -> single-segment SubstitutionInfo
//   3. Empty input      -> zero-segment SubstitutionInfo (legacy path)
//   4. Run with empty fontFilePath  -> falls back to span fontName
//   5. Run with unresolved path     -> falls back to span fontName
//   6. charStart/charEnd/fontSize indices preserved verbatim
//   7. Out-of-order input is sorted by charStart before emission
//
// These tests exercise the production helper directly so they don't
// need a live PDF document — the integration with the rest of the
// emission pipeline is locked by the phase-2 tests in
// test_content_stream_per_run_emission.cpp.

#include <gtest/gtest.h>

#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"
#include "fonts/FontSubstitutionEngine.h"

#include <unordered_map>
#include <vector>

using pdfe::fonts::SubstitutionInfo;
using pdfe::textedit::FormattingRun;
using pdfe::textedit::TextSpan;
using pdfe::textedit::buildSubstitutionFromFormattingRuns;

namespace {

TextSpan makeSpan(const std::string& fontName = "OriginalFont",
                  float fontSize = 12.0f) {
    TextSpan s;
    s.fontName = fontName;
    s.fontSize = fontSize;
    return s;
}

FormattingRun makeRun(int charStart, int charEnd,
                      const std::string& fontFilePath = {},
                      float fontSize = 0.0f) {
    FormattingRun r;
    r.charStart = charStart;
    r.charEnd = charEnd;
    r.fontFilePath = fontFilePath;
    r.fontSize = fontSize;
    return r;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// 1. Multi-run input → multi-segment SubstitutionInfo.
// ─────────────────────────────────────────────────────────────────────
TEST(TextEditCommandsPerRun, TwoRunsProduceTwoRunSegments) {
    auto span = makeSpan("HelvBaseline", 12.0f);

    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(0, 5, "C:/fonts/regular.ttf", 12.0f));
    runs.push_back(makeRun(5, 10, "C:/fonts/bold.ttf", 12.0f));

    std::unordered_map<std::string, std::string> path2res = {
        {"C:/fonts/regular.ttf", "Fsub1"},
        {"C:/fonts/bold.ttf",    "Fsub2"},
    };

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    ASSERT_EQ(sub.runSegments.size(), 2u)
        << "Two FormattingRuns must yield two RunSegments — the "
           "dominant-run shortcut (replaced in phase-3) is gone.";

    EXPECT_EQ(sub.runSegments[0].charStart, 0);
    EXPECT_EQ(sub.runSegments[0].charEnd, 5);
    EXPECT_EQ(sub.runSegments[0].fontResName, "Fsub1");
    EXPECT_FLOAT_EQ(sub.runSegments[0].fontSize, 12.0f);

    EXPECT_EQ(sub.runSegments[1].charStart, 5);
    EXPECT_EQ(sub.runSegments[1].charEnd, 10);
    EXPECT_EQ(sub.runSegments[1].fontResName, "Fsub2");
    EXPECT_FLOAT_EQ(sub.runSegments[1].fontSize, 12.0f);
}

TEST(TextEditCommandsPerRun, ThreeDistinctFontsAllSurviveAsSegments) {
    auto span = makeSpan("Original", 11.0f);

    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(0, 3, "regular.ttf", 11.0f));
    runs.push_back(makeRun(3, 6, "bold.ttf",    11.0f));
    runs.push_back(makeRun(6, 9, "italic.ttf",  11.0f));

    std::unordered_map<std::string, std::string> path2res = {
        {"regular.ttf", "Fa"},
        {"bold.ttf",    "Fb"},
        {"italic.ttf",  "Fc"},
    };

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    ASSERT_EQ(sub.runSegments.size(), 3u);
    EXPECT_EQ(sub.runSegments[0].fontResName, "Fa");
    EXPECT_EQ(sub.runSegments[1].fontResName, "Fb");
    EXPECT_EQ(sub.runSegments[2].fontResName, "Fc");
}

// ─────────────────────────────────────────────────────────────────────
// 2. Single-run input → single-segment SubstitutionInfo.
// ─────────────────────────────────────────────────────────────────────
TEST(TextEditCommandsPerRun, SingleRunProducesSingleRunSegment) {
    auto span = makeSpan("Times", 10.0f);

    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(0, 7, "times.ttf", 10.0f));

    std::unordered_map<std::string, std::string> path2res = {
        {"times.ttf", "Fsub3"},
    };

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    ASSERT_EQ(sub.runSegments.size(), 1u);
    EXPECT_EQ(sub.runSegments[0].charStart, 0);
    EXPECT_EQ(sub.runSegments[0].charEnd, 7);
    EXPECT_EQ(sub.runSegments[0].fontResName, "Fsub3");
}

// ─────────────────────────────────────────────────────────────────────
// 3. Empty FormattingRuns → zero-segment SubstitutionInfo.
//    Phase-2 emission still uses the legacy single-Tf path when
//    runSegments.empty() — no behavioral change for unformatted edits.
// ─────────────────────────────────────────────────────────────────────
TEST(TextEditCommandsPerRun, EmptyFormattingRunsProducesNoSegments) {
    auto span = makeSpan("Helvetica", 12.0f);

    std::vector<FormattingRun> runs;  // empty
    std::unordered_map<std::string, std::string> path2res;

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    EXPECT_TRUE(sub.runSegments.empty())
        << "Empty FormattingRuns must yield zero RunSegments so the "
           "phase-2 emission falls through to the legacy single-Tf "
           "path; otherwise the 30+ dominant-run regression tests "
           "would diverge.";
    EXPECT_EQ(sub.spanIndex, 0);
    EXPECT_EQ(sub.originalFontName, "Helvetica");
    EXPECT_FLOAT_EQ(sub.originalFontSize, 12.0f);
    EXPECT_TRUE(sub.substituteResName.empty())
        << "With no segments there is nothing to derive "
           "substituteResName from; leaving it empty is the legacy "
           "fontSwitches behavior (no-op substitution).";
}

// ─────────────────────────────────────────────────────────────────────
// 4. Run with empty fontFilePath → fallback to span's original font.
// ─────────────────────────────────────────────────────────────────────
TEST(TextEditCommandsPerRun, RunWithEmptyFontFilePathFallsBackToSpanFont) {
    auto span = makeSpan("OriginalSpanFont", 14.0f);

    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(0, 5, /*fontFilePath=*/"", /*fontSize=*/0.0f));

    std::unordered_map<std::string, std::string> path2res;

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    ASSERT_EQ(sub.runSegments.size(), 1u)
        << "A run with empty fontFilePath must NOT be silently "
           "dropped — phase-2 emission would skip the entire span "
           "and the user's text would vanish on save.";
    EXPECT_EQ(sub.runSegments[0].fontResName, "OriginalSpanFont")
        << "Fallback contract: empty path → span.fontName.";
    EXPECT_FLOAT_EQ(sub.runSegments[0].fontSize, 14.0f)
        << "Fallback contract: zero fontSize → span.fontSize.";
}

TEST(TextEditCommandsPerRun, RunWithUnresolvedPathFallsBackToSpanFont) {
    // The path is set but pathToResName has no matching entry —
    // simulates embedFont() returning std::nullopt (e.g., font file
    // unreadable, format unsupported).
    auto span = makeSpan("OriginalFont", 13.0f);

    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(0, 5, "C:/missing/font.ttf", 13.0f));

    std::unordered_map<std::string, std::string> path2res;  // empty

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    ASSERT_EQ(sub.runSegments.size(), 1u);
    EXPECT_EQ(sub.runSegments[0].fontResName, "OriginalFont")
        << "Embed failure must not silently drop text — fall back to "
           "the span's original font so the saved content stream "
           "still emits a Tj for these characters.";
}

// ─────────────────────────────────────────────────────────────────────
// 5. charStart/charEnd/fontSize round-trip from FormattingRun input.
// ─────────────────────────────────────────────────────────────────────
TEST(TextEditCommandsPerRun, RunCharRangeAndFontSizeMatchInput) {
    auto span = makeSpan("Span", 10.0f);

    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(0,  4, "a.ttf", 11.5f));
    runs.push_back(makeRun(4, 12, "b.ttf", 14.0f));
    runs.push_back(makeRun(12, 13, "c.ttf", 8.0f));

    std::unordered_map<std::string, std::string> path2res = {
        {"a.ttf", "Fa"}, {"b.ttf", "Fb"}, {"c.ttf", "Fc"},
    };

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    ASSERT_EQ(sub.runSegments.size(), 3u);

    EXPECT_EQ(sub.runSegments[0].charStart, 0);
    EXPECT_EQ(sub.runSegments[0].charEnd, 4);
    EXPECT_FLOAT_EQ(sub.runSegments[0].fontSize, 11.5f);

    EXPECT_EQ(sub.runSegments[1].charStart, 4);
    EXPECT_EQ(sub.runSegments[1].charEnd, 12);
    EXPECT_FLOAT_EQ(sub.runSegments[1].fontSize, 14.0f);

    EXPECT_EQ(sub.runSegments[2].charStart, 12);
    EXPECT_EQ(sub.runSegments[2].charEnd, 13);
    EXPECT_FLOAT_EQ(sub.runSegments[2].fontSize, 8.0f);
}

// ─────────────────────────────────────────────────────────────────────
// 6. Out-of-order input is sorted by charStart so the emission walk
//    follows visual reading order regardless of caller insertion order.
// ─────────────────────────────────────────────────────────────────────
TEST(TextEditCommandsPerRun, OutOfOrderInputSortedByCharStart) {
    auto span = makeSpan("Span", 12.0f);

    // Insert in reverse (last-run first, first-run last).
    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(10, 15, "c.ttf", 12.0f));
    runs.push_back(makeRun(5, 10,  "b.ttf", 12.0f));
    runs.push_back(makeRun(0, 5,   "a.ttf", 12.0f));

    std::unordered_map<std::string, std::string> path2res = {
        {"a.ttf", "Fa"}, {"b.ttf", "Fb"}, {"c.ttf", "Fc"},
    };

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    ASSERT_EQ(sub.runSegments.size(), 3u);
    // After sort: charStart 0, 5, 10.
    EXPECT_EQ(sub.runSegments[0].charStart, 0);
    EXPECT_EQ(sub.runSegments[0].fontResName, "Fa");
    EXPECT_EQ(sub.runSegments[1].charStart, 5);
    EXPECT_EQ(sub.runSegments[1].fontResName, "Fb");
    EXPECT_EQ(sub.runSegments[2].charStart, 10);
    EXPECT_EQ(sub.runSegments[2].fontResName, "Fc");
}

// ─────────────────────────────────────────────────────────────────────
// 7. SubstitutionInfo metadata (spanIndex, originalFont*) is populated
//    from the span regardless of the runs.
// ─────────────────────────────────────────────────────────────────────
TEST(TextEditCommandsPerRun, MetadataDerivedFromSpanNotRuns) {
    auto span = makeSpan("MySpanFont", 9.5f);

    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(0, 3, "x.ttf", 24.0f));  // run size differs

    std::unordered_map<std::string, std::string> path2res = {
        {"x.ttf", "Fx"},
    };

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    EXPECT_EQ(sub.spanIndex, 0);
    EXPECT_EQ(sub.originalFontName, "MySpanFont")
        << "originalFontName must come from the span — not the run — "
           "because the legacy fontSwitches map is keyed by the span's "
           "PDF resource name when the per-run path is bypassed.";
    EXPECT_FLOAT_EQ(sub.originalFontSize, 9.5f);
    EXPECT_FLOAT_EQ(sub.horizontalScale, 1.0f);
    // First-run resName fallback for downstream legacy consumers.
    EXPECT_EQ(sub.substituteResName, "Fx");
}

// ─────────────────────────────────────────────────────────────────────
// 8. Mixed scenario: some runs resolved, some unresolved, some empty
//    path. Each must independently fall back as documented; ordering
//    after sort is preserved.
// ─────────────────────────────────────────────────────────────────────
TEST(TextEditCommandsPerRun, MixedResolutionFallsBackPerSegment) {
    auto span = makeSpan("FallbackFont", 12.0f);

    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(0, 4,   "resolved.ttf", 12.0f));  // resolved
    runs.push_back(makeRun(4, 8,   "missing.ttf",  12.0f));  // unresolved
    runs.push_back(makeRun(8, 12,  /*path=*/"",    12.0f));  // empty path

    std::unordered_map<std::string, std::string> path2res = {
        {"resolved.ttf", "Fres"},
    };

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    ASSERT_EQ(sub.runSegments.size(), 3u);
    EXPECT_EQ(sub.runSegments[0].fontResName, "Fres");
    EXPECT_EQ(sub.runSegments[1].fontResName, "FallbackFont")
        << "Unresolved path → span fallback (not silent drop).";
    EXPECT_EQ(sub.runSegments[2].fontResName, "FallbackFont")
        << "Empty path → span fallback.";
}

// ─────────────────────────────────────────────────────────────────────
// 9. Run.fontSize == 0.0 → falls back to span.fontSize.
//    Run.fontSize > 0 is preserved verbatim.
// ─────────────────────────────────────────────────────────────────────
TEST(TextEditCommandsPerRun, ZeroFontSizeFallsBackToSpanFontSize) {
    auto span = makeSpan("Span", 12.5f);

    std::vector<FormattingRun> runs;
    runs.push_back(makeRun(0, 5, "a.ttf", /*fontSize=*/0.0f));   // fallback
    runs.push_back(makeRun(5, 10, "b.ttf", /*fontSize=*/16.0f)); // explicit

    std::unordered_map<std::string, std::string> path2res = {
        {"a.ttf", "Fa"}, {"b.ttf", "Fb"},
    };

    SubstitutionInfo sub =
        buildSubstitutionFromFormattingRuns(runs, path2res, span);

    ASSERT_EQ(sub.runSegments.size(), 2u);
    EXPECT_FLOAT_EQ(sub.runSegments[0].fontSize, 12.5f)
        << "Zero run.fontSize → span.fontSize fallback.";
    EXPECT_FLOAT_EQ(sub.runSegments[1].fontSize, 16.0f)
        << "Non-zero run.fontSize preserved verbatim.";
}
