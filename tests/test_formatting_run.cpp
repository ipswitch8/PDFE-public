#include <gtest/gtest.h>
#include "textedit/TextEditModel.h"
#include "core/TextBlock.h"
#include <QRectF>

using namespace pdfe::textedit;

// Helper function to build a TextEditModel with a single span of N characters
static TextEditModel makeModel(int charCount, const std::string& fontName = "Arial", float fontSize = 12.0f) {
    pdfe::core::TextBlock block;
    pdfe::core::TextLine line;
    for (int i = 0; i < charCount; ++i) {
        pdfe::core::TextChar ch;
        ch.codepoint = U'A' + (i % 26);
        ch.fontName = fontName;
        ch.fontSize = fontSize;
        ch.quad = QRectF(i * 10.0, 0.0, 10.0, 12.0);
        ch.origin = QPointF(i * 10.0, 12.0);
        line.chars.push_back(ch);
    }
    block.lines.push_back(line);
    return TextEditModel({block});
}

// Helper function to create a formatting run with specific properties
static FormattingRun makeRun(int start, int end, const std::string& family = "", float size = 0.0f,
                              bool bold = false, bool italic = false, bool underline = false,
                              bool strikethrough = false) {
    FormattingRun run;
    run.charStart = start;
    run.charEnd = end;
    run.fontFamily = family;
    run.fontSize = size;
    run.bold = bold;
    run.italic = italic;
    run.underline = underline;
    run.strikethrough = strikethrough;
    return run;
}

//=============================================================================
// FormattingRun struct tests
//=============================================================================

TEST(FormattingRunTest, DefaultConstruction) {
    FormattingRun run;

    EXPECT_EQ(0, run.charStart);
    EXPECT_EQ(0, run.charEnd);
    EXPECT_TRUE(run.fontFamily.empty());
    EXPECT_FLOAT_EQ(0.0f, run.fontSize);
    EXPECT_FALSE(run.bold);
    EXPECT_FALSE(run.italic);
    EXPECT_FALSE(run.underline);
    EXPECT_FALSE(run.strikethrough);
    EXPECT_TRUE(run.fontFilePath.empty());
}

TEST(FormattingRunTest, SameFormatAs_Identical) {
    FormattingRun run1 = makeRun(0, 5, "Times", 14.0f, true, false, true, false);
    FormattingRun run2 = makeRun(10, 20, "Times", 14.0f, true, false, true, false);

    // Should return true regardless of char range
    EXPECT_TRUE(run1.sameFormatAs(run2));
    EXPECT_TRUE(run2.sameFormatAs(run1));
}

TEST(FormattingRunTest, SameFormatAs_DifferentFont) {
    FormattingRun run1 = makeRun(0, 5, "Times", 14.0f, true, false, true, false);
    FormattingRun run2 = makeRun(0, 5, "Arial", 14.0f, true, false, true, false);

    // Different fontFamily should return false
    EXPECT_FALSE(run1.sameFormatAs(run2));
}

TEST(FormattingRunTest, SameFormatAs_DifferentSize) {
    FormattingRun run1 = makeRun(0, 5, "Times", 14.0f, true, false, true, false);
    FormattingRun run2 = makeRun(0, 5, "Times", 16.0f, true, false, true, false);

    EXPECT_FALSE(run1.sameFormatAs(run2));
}

TEST(FormattingRunTest, SameFormatAs_DifferentBold) {
    FormattingRun run1 = makeRun(0, 5, "Times", 14.0f, true, false, false, false);
    FormattingRun run2 = makeRun(0, 5, "Times", 14.0f, false, false, false, false);

    EXPECT_FALSE(run1.sameFormatAs(run2));
}

//=============================================================================
// applyFormatting tests
//=============================================================================

TEST(ApplyFormattingTest, InsertIntoEmpty) {
    auto model = makeModel(10);

    // Initially, formattingRuns should be empty
    EXPECT_EQ(0, model.span(0).formattingRuns.size());

    // Apply formatting to characters 2-5
    FormattingRun formatting = makeRun(2, 5, "Times", 14.0f, true, false, false, false);
    model.applyFormatting(0, 2, 5, formatting);

    // Should have exactly one run
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(5, runs[0].charEnd);
    EXPECT_EQ("Times", runs[0].fontFamily);
    EXPECT_FLOAT_EQ(14.0f, runs[0].fontSize);
    EXPECT_TRUE(runs[0].bold);
}

TEST(ApplyFormattingTest, InsertReplacesFully) {
    auto model = makeModel(10);

    // Apply initial formatting to 2-5
    FormattingRun initial = makeRun(2, 5, "Arial", 12.0f, false, false, false, false);
    model.applyFormatting(0, 2, 5, initial);

    // Apply new formatting that fully covers the existing run
    FormattingRun replacement = makeRun(2, 5, "Times", 14.0f, true, false, false, false);
    model.applyFormatting(0, 2, 5, replacement);

    // Should still have exactly one run with new formatting
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(5, runs[0].charEnd);
    EXPECT_EQ("Times", runs[0].fontFamily);
    EXPECT_FLOAT_EQ(14.0f, runs[0].fontSize);
    EXPECT_TRUE(runs[0].bold);
}

TEST(ApplyFormattingTest, InsertTrimsLeft) {
    auto model = makeModel(10);

    // Apply initial formatting to 2-7
    FormattingRun initial = makeRun(2, 7, "Arial", 12.0f, false, false, false, false);
    model.applyFormatting(0, 2, 7, initial);

    // Apply new formatting to 5-9 (overlaps right side)
    FormattingRun newFormat = makeRun(5, 9, "Times", 14.0f, true, false, false, false);
    model.applyFormatting(0, 5, 9, newFormat);

    // Should have two runs: trimmed original (2-5) and new (5-9)
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(2, runs.size());

    // First run should be trimmed
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(5, runs[0].charEnd);
    EXPECT_EQ("Arial", runs[0].fontFamily);

    // Second run should be the new formatting
    EXPECT_EQ(5, runs[1].charStart);
    EXPECT_EQ(9, runs[1].charEnd);
    EXPECT_EQ("Times", runs[1].fontFamily);
}

TEST(ApplyFormattingTest, InsertTrimsRight) {
    auto model = makeModel(10);

    // Apply initial formatting to 5-9
    FormattingRun initial = makeRun(5, 9, "Arial", 12.0f, false, false, false, false);
    model.applyFormatting(0, 5, 9, initial);

    // Apply new formatting to 2-7 (overlaps left side)
    FormattingRun newFormat = makeRun(2, 7, "Times", 14.0f, true, false, false, false);
    model.applyFormatting(0, 2, 7, newFormat);

    // Should have two runs: new (2-7) and trimmed original (7-9)
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(2, runs.size());

    // First run should be the new formatting
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(7, runs[0].charEnd);
    EXPECT_EQ("Times", runs[0].fontFamily);

    // Second run should be trimmed
    EXPECT_EQ(7, runs[1].charStart);
    EXPECT_EQ(9, runs[1].charEnd);
    EXPECT_EQ("Arial", runs[1].fontFamily);
}

TEST(ApplyFormattingTest, InsertSplitsExisting) {
    auto model = makeModel(10);

    // Apply initial formatting to 2-8
    FormattingRun initial = makeRun(2, 8, "Arial", 12.0f, false, false, false, false);
    model.applyFormatting(0, 2, 8, initial);

    // Apply new formatting to 4-6 (in the middle)
    FormattingRun newFormat = makeRun(4, 6, "Times", 14.0f, true, false, false, false);
    model.applyFormatting(0, 4, 6, newFormat);

    // Should have three runs: left remainder (2-4), new (4-6), right remainder (6-8)
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(3, runs.size());

    // Left remainder
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(4, runs[0].charEnd);
    EXPECT_EQ("Arial", runs[0].fontFamily);

    // New formatting
    EXPECT_EQ(4, runs[1].charStart);
    EXPECT_EQ(6, runs[1].charEnd);
    EXPECT_EQ("Times", runs[1].fontFamily);
    EXPECT_TRUE(runs[1].bold);

    // Right remainder
    EXPECT_EQ(6, runs[2].charStart);
    EXPECT_EQ(8, runs[2].charEnd);
    EXPECT_EQ("Arial", runs[2].fontFamily);
}

TEST(ApplyFormattingTest, MergeAdjacentIdentical) {
    auto model = makeModel(10);

    // Apply formatting to 2-5
    FormattingRun initial = makeRun(2, 5, "Times", 14.0f, true, false, false, false);
    model.applyFormatting(0, 2, 5, initial);

    // Apply identical formatting adjacent to it (5-8)
    FormattingRun adjacent = makeRun(5, 8, "Times", 14.0f, true, false, false, false);
    model.applyFormatting(0, 5, 8, adjacent);

    // Should merge into one run (2-8)
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(8, runs[0].charEnd);
    EXPECT_EQ("Times", runs[0].fontFamily);
}

TEST(ApplyFormattingTest, MultipleOverlappingRuns) {
    auto model = makeModel(20);

    // Create multiple formatting runs
    model.applyFormatting(0, 2, 6, makeRun(2, 6, "Arial", 12.0f, false, false, false, false));
    model.applyFormatting(0, 8, 12, makeRun(8, 12, "Times", 14.0f, true, false, false, false));
    model.applyFormatting(0, 14, 18, makeRun(14, 18, "Courier", 10.0f, false, true, false, false));

    // Apply formatting that covers all of them (4-16)
    model.applyFormatting(0, 4, 16, makeRun(4, 16, "Helvetica", 16.0f, false, false, true, false));

    // Should have three runs: trimmed first (2-4), new (4-16), trimmed last (16-18)
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(3, runs.size());

    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(4, runs[0].charEnd);
    EXPECT_EQ("Arial", runs[0].fontFamily);

    EXPECT_EQ(4, runs[1].charStart);
    EXPECT_EQ(16, runs[1].charEnd);
    EXPECT_EQ("Helvetica", runs[1].fontFamily);
    EXPECT_TRUE(runs[1].underline);

    EXPECT_EQ(16, runs[2].charStart);
    EXPECT_EQ(18, runs[2].charEnd);
    EXPECT_EQ("Courier", runs[2].fontFamily);
}

//=============================================================================
// formattingAtChar tests
//=============================================================================

TEST(FormattingAtCharTest, ReturnsRunForCoveredChar) {
    auto model = makeModel(10);

    // Apply formatting to 3-7
    FormattingRun formatting = makeRun(3, 7, "Times", 14.0f, true, false, true, false);
    model.applyFormatting(0, 3, 7, formatting);

    // Check character at index 5 (within the run)
    FormattingRun result = model.formattingAtChar(0, 5);

    EXPECT_EQ(3, result.charStart);
    EXPECT_EQ(7, result.charEnd);
    EXPECT_EQ("Times", result.fontFamily);
    EXPECT_FLOAT_EQ(14.0f, result.fontSize);
    EXPECT_TRUE(result.bold);
    EXPECT_TRUE(result.underline);
}

TEST(FormattingAtCharTest, ReturnsDefaultForGap) {
    auto model = makeModel(10);

    // Apply formatting to 3-7, leaving gaps at 0-3 and 7-10
    FormattingRun formatting = makeRun(3, 7, "Times", 14.0f, true, false, false, false);
    model.applyFormatting(0, 3, 7, formatting);

    // Check character at index 1 (in gap before run)
    FormattingRun result1 = model.formattingAtChar(0, 1);
    EXPECT_TRUE(result1.fontFamily.empty());
    EXPECT_FLOAT_EQ(0.0f, result1.fontSize);
    EXPECT_FALSE(result1.bold);

    // Check character at index 8 (in gap after run)
    FormattingRun result2 = model.formattingAtChar(0, 8);
    EXPECT_TRUE(result2.fontFamily.empty());
    EXPECT_FLOAT_EQ(0.0f, result2.fontSize);
}

TEST(FormattingAtCharTest, BoundaryConditions) {
    auto model = makeModel(10);

    // Apply formatting to 3-7
    FormattingRun formatting = makeRun(3, 7, "Times", 14.0f, true, false, false, false);
    model.applyFormatting(0, 3, 7, formatting);

    // Check at exact start boundary (inclusive)
    FormattingRun result1 = model.formattingAtChar(0, 3);
    EXPECT_EQ("Times", result1.fontFamily);

    // Check at exact end boundary (exclusive)
    FormattingRun result2 = model.formattingAtChar(0, 7);
    EXPECT_TRUE(result2.fontFamily.empty()); // Should be in gap
}

TEST(FormattingAtCharTest, MultipleRuns) {
    auto model = makeModel(20);

    // Apply multiple non-overlapping runs
    model.applyFormatting(0, 2, 5, makeRun(2, 5, "Arial", 12.0f, false, false, false, false));
    model.applyFormatting(0, 7, 10, makeRun(7, 10, "Times", 14.0f, true, false, false, false));
    model.applyFormatting(0, 12, 15, makeRun(12, 15, "Courier", 10.0f, false, true, false, false));

    // Check within first run
    EXPECT_EQ("Arial", model.formattingAtChar(0, 3).fontFamily);

    // Check in gap between first and second
    EXPECT_TRUE(model.formattingAtChar(0, 6).fontFamily.empty());

    // Check within second run
    FormattingRun result = model.formattingAtChar(0, 8);
    EXPECT_EQ("Times", result.fontFamily);
    EXPECT_TRUE(result.bold);

    // Check within third run
    result = model.formattingAtChar(0, 13);
    EXPECT_EQ("Courier", result.fontFamily);
    EXPECT_TRUE(result.italic);
}

//=============================================================================
// clearFormatting tests
//=============================================================================

TEST(ClearFormattingTest, RemovesFullyCoveredRun) {
    auto model = makeModel(10);

    // Apply formatting to 3-7
    model.applyFormatting(0, 3, 7, makeRun(3, 7, "Times", 14.0f, true, false, false, false));

    ASSERT_EQ(1, model.span(0).formattingRuns.size());

    // Clear the entire range
    model.clearFormatting(0, 3, 7);

    // Run should be removed
    EXPECT_EQ(0, model.span(0).formattingRuns.size());
}

TEST(ClearFormattingTest, TrimsPartiallyOverlapping_Left) {
    auto model = makeModel(10);

    // Apply formatting to 2-8
    model.applyFormatting(0, 2, 8, makeRun(2, 8, "Times", 14.0f, true, false, false, false));

    // Clear 2-5 (left portion)
    model.clearFormatting(0, 2, 5);

    // Should have one run trimmed to 5-8
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(5, runs[0].charStart);
    EXPECT_EQ(8, runs[0].charEnd);
}

TEST(ClearFormattingTest, TrimsPartiallyOverlapping_Right) {
    auto model = makeModel(10);

    // Apply formatting to 2-8
    model.applyFormatting(0, 2, 8, makeRun(2, 8, "Times", 14.0f, true, false, false, false));

    // Clear 5-8 (right portion)
    model.clearFormatting(0, 5, 8);

    // Should have one run trimmed to 2-5
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(5, runs[0].charEnd);
}

TEST(ClearFormattingTest, SplitsRun) {
    auto model = makeModel(10);

    // Apply formatting to 2-8
    model.applyFormatting(0, 2, 8, makeRun(2, 8, "Times", 14.0f, true, false, false, false));

    // Clear middle portion 4-6
    model.clearFormatting(0, 4, 6);

    // Should have two runs: 2-4 and 6-8
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(2, runs.size());

    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(4, runs[0].charEnd);
    EXPECT_EQ("Times", runs[0].fontFamily);

    EXPECT_EQ(6, runs[1].charStart);
    EXPECT_EQ(8, runs[1].charEnd);
    EXPECT_EQ("Times", runs[1].fontFamily);
}

TEST(ClearFormattingTest, ClearMultipleRuns) {
    auto model = makeModel(20);

    // Apply multiple runs
    model.applyFormatting(0, 2, 5, makeRun(2, 5, "Arial", 12.0f, false, false, false, false));
    model.applyFormatting(0, 7, 10, makeRun(7, 10, "Times", 14.0f, true, false, false, false));
    model.applyFormatting(0, 12, 15, makeRun(12, 15, "Courier", 10.0f, false, true, false, false));

    ASSERT_EQ(3, model.span(0).formattingRuns.size());

    // Clear range that covers multiple runs (6-13)
    model.clearFormatting(0, 6, 13);

    // Should have two trimmed runs: 2-5 and 13-15
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(2, runs.size());

    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(5, runs[0].charEnd);
    EXPECT_EQ("Arial", runs[0].fontFamily);

    EXPECT_EQ(13, runs[1].charStart);
    EXPECT_EQ(15, runs[1].charEnd);
    EXPECT_EQ("Courier", runs[1].fontFamily);
}

TEST(ClearFormattingTest, ClearNonExistentRange) {
    auto model = makeModel(10);

    // Apply formatting to 3-7
    model.applyFormatting(0, 3, 7, makeRun(3, 7, "Times", 14.0f, true, false, false, false));

    // Clear range that doesn't overlap (0-2)
    model.clearFormatting(0, 0, 2);

    // Run should still be there
    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(3, runs[0].charStart);
    EXPECT_EQ(7, runs[0].charEnd);
}

//=============================================================================
// isModified tests
//=============================================================================

TEST(IsModifiedTest, UnmodifiedSpan_NoRuns_False) {
    auto model = makeModel(10);

    // Span has no text changes and no formatting runs
    EXPECT_FALSE(model.span(0).isModified());
}

TEST(IsModifiedTest, UnmodifiedText_WithRuns_True) {
    auto model = makeModel(10);

    // Apply formatting (text unchanged)
    model.applyFormatting(0, 2, 5, makeRun(2, 5, "Times", 14.0f, true, false, false, false));

    // Should be modified because formattingRuns is non-empty
    EXPECT_TRUE(model.span(0).isModified());
}

TEST(IsModifiedTest, ModifiedText_NoRuns_True) {
    auto model = makeModel(10);

    // Modify text
    auto& span = model.mutableSpan(0);
    span.text = U"Modified";

    // Should be modified because text != originalText
    EXPECT_TRUE(span.isModified());
}

TEST(IsModifiedTest, ModifiedText_WithRuns_True) {
    auto model = makeModel(10);

    // Modify text and apply formatting
    auto& span = model.mutableSpan(0);
    span.text = U"Modified";
    model.applyFormatting(0, 0, 3, makeRun(0, 3, "Times", 14.0f, true, false, false, false));

    // Should be modified for both reasons
    EXPECT_TRUE(span.isModified());
}

TEST(IsModifiedTest, AfterClearingAllFormatting) {
    auto model = makeModel(10);

    // Apply formatting
    model.applyFormatting(0, 2, 5, makeRun(2, 5, "Times", 14.0f, true, false, false, false));
    EXPECT_TRUE(model.span(0).isModified());

    // Clear all formatting
    model.clearFormatting(0, 2, 5);

    // Should no longer be modified (formattingRuns is empty, text unchanged)
    EXPECT_FALSE(model.span(0).isModified());
}

//=============================================================================
// Edge cases
//=============================================================================

TEST(EdgeCasesTest, InvalidSpanIndex) {
    auto model = makeModel(10);

    // Apply formatting with invalid span index - should not crash
    EXPECT_NO_THROW(model.applyFormatting(999, 0, 5, makeRun(0, 5, "Times", 14.0f, true, false, false, false)));

    // Query with invalid span index - should not crash
    EXPECT_NO_THROW((void)model.formattingAtChar(999, 3));

    // Clear with invalid span index - should not crash
    EXPECT_NO_THROW(model.clearFormatting(999, 0, 5));
}

TEST(EdgeCasesTest, EmptyRange) {
    auto model = makeModel(10);

    // Apply formatting with startChar == endChar (empty range)
    model.applyFormatting(0, 5, 5, makeRun(5, 5, "Times", 14.0f, true, false, false, false));

    // Should be a no-op
    EXPECT_EQ(0, model.span(0).formattingRuns.size());
}

TEST(EdgeCasesTest, NegativeIndices) {
    auto model = makeModel(10);

    // Apply formatting with negative indices - should not crash
    EXPECT_NO_THROW(model.applyFormatting(0, -1, 5, makeRun(-1, 5, "Times", 14.0f, true, false, false, false)));

    // Query with negative index - should not crash
    EXPECT_NO_THROW((void)model.formattingAtChar(0, -1));
}

TEST(EdgeCasesTest, OutOfBoundsIndices) {
    auto model = makeModel(10);

    // Apply formatting beyond span length
    model.applyFormatting(0, 5, 100, makeRun(5, 100, "Times", 14.0f, true, false, false, false));

    // Should still create the run (implementation may clamp or allow)
    // This tests that it doesn't crash
    EXPECT_NO_THROW((void)model.formattingAtChar(0, 50));
}

TEST(EdgeCasesTest, ReversedRange) {
    auto model = makeModel(10);

    // Apply formatting with startChar > endChar
    model.applyFormatting(0, 7, 3, makeRun(7, 3, "Times", 14.0f, true, false, false, false));

    // Should handle gracefully (no-op or swap)
    // Main requirement: should not crash
}

TEST(EdgeCasesTest, SingleCharacterFormatting) {
    auto model = makeModel(10);

    // Format a single character
    model.applyFormatting(0, 5, 6, makeRun(5, 6, "Times", 14.0f, true, false, false, false));

    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(5, runs[0].charStart);
    EXPECT_EQ(6, runs[0].charEnd);
}

TEST(EdgeCasesTest, ZeroCharacterModel) {
    auto model = makeModel(0);

    // Apply formatting to empty span - should not crash
    EXPECT_NO_THROW(model.applyFormatting(0, 0, 0, makeRun(0, 0, "Times", 14.0f, true, false, false, false)));
}

TEST(EdgeCasesTest, LargeNumberOfRuns) {
    auto model = makeModel(100);

    // Apply many small formatting runs
    for (int i = 0; i < 50; ++i) {
        model.applyFormatting(0, i * 2, i * 2 + 1,
                             makeRun(i * 2, i * 2 + 1, "Times", 14.0f, i % 2 == 0, false, false, false));
    }

    // Should handle many runs without issue
    EXPECT_GE(model.span(0).formattingRuns.size(), 1);
}

TEST(EdgeCasesTest, ComplexFormattingScenario) {
    auto model = makeModel(30);

    // Apply overlapping formatting in complex pattern
    model.applyFormatting(0, 5, 15, makeRun(5, 15, "Arial", 12.0f, false, false, false, false));
    model.applyFormatting(0, 10, 20, makeRun(10, 20, "Times", 14.0f, true, false, false, false));
    model.clearFormatting(0, 12, 18);
    model.applyFormatting(0, 14, 16, makeRun(14, 16, "Courier", 10.0f, false, true, false, false));
    model.applyFormatting(0, 3, 7, makeRun(3, 7, "Helvetica", 16.0f, false, false, true, false));

    // Verify model is in a consistent state
    const auto& runs = model.span(0).formattingRuns;

    // Runs should be sorted by charStart
    for (size_t i = 1; i < runs.size(); ++i) {
        EXPECT_LE(runs[i-1].charStart, runs[i].charStart);
    }

    // Runs should not overlap
    for (size_t i = 1; i < runs.size(); ++i) {
        EXPECT_LE(runs[i-1].charEnd, runs[i].charStart);
    }
}

// ===========================================================================
// Karen QA fixes: run shifting on text insert/delete
// ===========================================================================

TEST(RunShiftingTest, InsertBeforeRun_ShiftsRunRight) {
    auto model = makeModel(10);
    model.applyFormatting(0, 5, 8, makeRun(5, 8, "Times", 14.0f, true));

    // Insert 3 characters at position 2 (before the run)
    model.replaceCharRange(0, 2, 2, U"XYZ");

    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(8, runs[0].charStart);   // 5 + 3
    EXPECT_EQ(11, runs[0].charEnd);    // 8 + 3
}

TEST(RunShiftingTest, InsertAfterRun_NoChange) {
    auto model = makeModel(10);
    model.applyFormatting(0, 2, 5, makeRun(2, 5, "Times", 14.0f, true));

    // Insert 2 characters at position 8 (after the run)
    model.replaceCharRange(0, 8, 8, U"AB");

    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(5, runs[0].charEnd);
}

TEST(RunShiftingTest, InsertWithinRun_ExpandsRun) {
    auto model = makeModel(10);
    model.applyFormatting(0, 3, 7, makeRun(3, 7, "Times", 14.0f, true));

    // Insert 2 characters at position 5 (within the run)
    model.replaceCharRange(0, 5, 5, U"XY");

    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(3, runs[0].charStart);
    EXPECT_EQ(9, runs[0].charEnd);   // 7 + 2
}

TEST(RunShiftingTest, DeleteBeforeRun_ShiftsRunLeft) {
    auto model = makeModel(10);
    model.applyFormatting(0, 5, 8, makeRun(5, 8, "Times", 14.0f, true));

    // Delete 2 characters at position 1-3 (before the run)
    model.replaceCharRange(0, 1, 3, U"");

    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(3, runs[0].charStart);   // 5 - 2
    EXPECT_EQ(6, runs[0].charEnd);     // 8 - 2
}

TEST(RunShiftingTest, DeleteEntireRun_RemovesRun) {
    auto model = makeModel(10);
    model.applyFormatting(0, 3, 6, makeRun(3, 6, "Times", 14.0f, true));

    // Delete characters 3-6 (exactly the run)
    model.replaceCharRange(0, 3, 6, U"");

    const auto& runs = model.span(0).formattingRuns;
    EXPECT_TRUE(runs.empty());
}

TEST(RunShiftingTest, DeletePartOfRun_TrimsRun) {
    auto model = makeModel(10);
    model.applyFormatting(0, 2, 8, makeRun(2, 8, "Times", 14.0f, true));

    // Delete characters 4-6 (within the run)
    model.replaceCharRange(0, 4, 6, U"");

    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(6, runs[0].charEnd);    // 8 - 2
}

TEST(RunShiftingTest, ReplaceWithinRun_AdjustsLength) {
    auto model = makeModel(10);
    model.applyFormatting(0, 2, 8, makeRun(2, 8, "Times", 14.0f, true));

    // Replace 2 chars (4-6) with 4 chars "WXYZ" (net +2)
    model.replaceCharRange(0, 4, 6, U"WXYZ");

    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(1, runs.size());
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(10, runs[0].charEnd);   // 8 + 2
}

TEST(RunShiftingTest, MultipleRuns_ShiftCorrectly) {
    auto model = makeModel(20);
    model.applyFormatting(0, 2, 5, makeRun(2, 5, "Arial", 12.0f, true));
    model.applyFormatting(0, 10, 15, makeRun(10, 15, "Times", 14.0f, false, true));

    // Insert 3 characters at position 7 (between the two runs)
    model.replaceCharRange(0, 7, 7, U"ABC");

    const auto& runs = model.span(0).formattingRuns;
    ASSERT_EQ(2, runs.size());
    // First run (before insert) unchanged
    EXPECT_EQ(2, runs[0].charStart);
    EXPECT_EQ(5, runs[0].charEnd);
    // Second run (after insert) shifted by 3
    EXPECT_EQ(13, runs[1].charStart);
    EXPECT_EQ(18, runs[1].charEnd);
}

// ===========================================================================
// Karen QA fixes: epsilon comparison in sameFormatAs
// ===========================================================================

TEST(SameFormatTest, EpsilonFontSizeComparison) {
    FormattingRun a;
    a.fontFamily = "Arial";
    a.fontSize = 12.0f;
    a.bold = true;

    FormattingRun b = a;
    b.fontSize = 12.005f;  // Tiny float difference

    EXPECT_TRUE(a.sameFormatAs(b))
        << "Runs with fontSize difference < 0.01 should be considered same format";
}

TEST(SameFormatTest, LargeFontSizeDifference) {
    FormattingRun a;
    a.fontFamily = "Arial";
    a.fontSize = 12.0f;

    FormattingRun b = a;
    b.fontSize = 14.0f;

    EXPECT_FALSE(a.sameFormatAs(b))
        << "Runs with fontSize difference > 0.01 should not be same format";
}

// ===========================================================================
// Karen QA fixes: cancelEdit clears formattingRuns (model-level test)
// ===========================================================================

TEST(CancelEditTest, ClearFormattingRestoresUnmodified) {
    auto model = makeModel(10);

    // Apply formatting
    model.applyFormatting(0, 0, 5, makeRun(0, 5, "Times", 14.0f, true));
    EXPECT_TRUE(model.span(0).isModified());

    // Simulate cancel: restore text + clear runs
    auto& span = model.mutableSpan(0);
    span.text = span.originalText;
    span.formattingRuns.clear();

    EXPECT_FALSE(model.span(0).isModified())
        << "After clearing text and formatting, span should not be modified";
}
