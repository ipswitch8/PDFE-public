// tests/test_substitution_info_model.cpp
//
// SAVE-08 Phase 1 — data-model tests for SubstitutionInfo and its
// nested RunSegment vector. These lock the contract that the
// per-run extension preserves all legacy fields and stores
// segments verbatim without validation at the model layer.
//
// Phase-2 (per-run emission) and Phase-3 (caller migration) build
// on this model; failure of any test here blocks both subsequent
// phases.

#include <gtest/gtest.h>

#include "fonts/FontSubstitutionEngine.h"

using pdfe::fonts::RunSegment;
using pdfe::fonts::SubstitutionInfo;

// ─────────────────────────────────────────────────────────────────────
// SubstitutionInfo legacy contract: existing callers see no changes.
// ─────────────────────────────────────────────────────────────────────
TEST(SubstitutionInfoModel, DefaultConstructedHasEmptyRunSegments) {
    SubstitutionInfo info;
    EXPECT_TRUE(info.runSegments.empty())
        << "Default-constructed SubstitutionInfo must start with no "
           "run segments — empty triggers the legacy single-font "
           "emission path (UI-TE-SAVE-08 backwards-compat invariant).";
}

TEST(SubstitutionInfoModel, LegacyFieldsRemainAccessibleAndUnchanged) {
    SubstitutionInfo info;
    info.spanIndex = 7;
    info.substituteResName = "Fsub3";
    info.horizontalScale = 1.25f;
    info.originalFontSize = 11.5f;
    info.originalFontName = "Helvetica";

    // Confirm round-trip — phase-1 must not have stomped these fields.
    EXPECT_EQ(info.spanIndex, 7);
    EXPECT_EQ(info.substituteResName, "Fsub3");
    EXPECT_FLOAT_EQ(info.horizontalScale, 1.25f);
    EXPECT_FLOAT_EQ(info.originalFontSize, 11.5f);
    EXPECT_EQ(info.originalFontName, "Helvetica");
    EXPECT_TRUE(info.runSegments.empty())
        << "Setting legacy fields must not synthesize implicit run "
           "segments; emission stays on the legacy path.";
}

// ─────────────────────────────────────────────────────────────────────
// RunSegment storage: charStart/charEnd/fontResName/fontSize round-trip.
// ─────────────────────────────────────────────────────────────────────
TEST(SubstitutionInfoModel, RunSegmentStoresAllFields) {
    RunSegment seg;
    seg.charStart = 4;
    seg.charEnd = 10;
    seg.fontResName = "Fbold1";
    seg.fontSize = 14.0f;

    EXPECT_EQ(seg.charStart, 4);
    EXPECT_EQ(seg.charEnd, 10);
    EXPECT_EQ(seg.fontResName, "Fbold1");
    EXPECT_FLOAT_EQ(seg.fontSize, 14.0f);
}

TEST(SubstitutionInfoModel, MultipleRunSegmentsPushedWithoutCollision) {
    SubstitutionInfo info;
    info.spanIndex = 0;

    info.runSegments.push_back({0, 5, "Fregular", 12.0f});
    info.runSegments.push_back({5, 10, "Fbold", 12.0f});
    info.runSegments.push_back({10, 15, "Fitalic", 12.0f});

    ASSERT_EQ(info.runSegments.size(), 3u);
    EXPECT_EQ(info.runSegments[0].fontResName, "Fregular");
    EXPECT_EQ(info.runSegments[1].fontResName, "Fbold");
    EXPECT_EQ(info.runSegments[2].fontResName, "Fitalic");
    // Char-range bookkeeping survives push_back ordering.
    EXPECT_EQ(info.runSegments[0].charEnd, info.runSegments[1].charStart);
    EXPECT_EQ(info.runSegments[1].charEnd, info.runSegments[2].charStart);
}

// ─────────────────────────────────────────────────────────────────────
// Boundary conditions — model layer stores verbatim, no validation.
// Validation (e.g. rejecting overlaps) is the emission layer's job.
// ─────────────────────────────────────────────────────────────────────
TEST(SubstitutionInfoModel, EmptySegmentCharStartEqualsCharEnd) {
    SubstitutionInfo info;
    info.runSegments.push_back({3, 3, "Fregular", 12.0f});  // empty slice
    ASSERT_EQ(info.runSegments.size(), 1u);
    EXPECT_EQ(info.runSegments[0].charStart, 3);
    EXPECT_EQ(info.runSegments[0].charEnd, 3)
        << "Empty segments (charStart==charEnd) must be storable at "
           "the model layer; phase-2 emission is responsible for "
           "skipping them.";
}

TEST(SubstitutionInfoModel, ZeroLengthSegmentAtStart) {
    SubstitutionInfo info;
    info.runSegments.push_back({0, 0, "Fregular", 12.0f});
    info.runSegments.push_back({0, 5, "Fbold", 12.0f});
    EXPECT_EQ(info.runSegments.size(), 2u);
    EXPECT_EQ(info.runSegments[0].charEnd, 0);
    EXPECT_EQ(info.runSegments[1].charEnd, 5);
}

TEST(SubstitutionInfoModel, OverlappingRangesStoredButNotValidated) {
    // Overlap is malformed input — the model accepts it without
    // diagnostics so callers can build pathological inputs for
    // emission-layer tests later. Validation belongs in phase-2.
    SubstitutionInfo info;
    info.runSegments.push_back({0, 8, "Fregular", 12.0f});
    info.runSegments.push_back({3, 6, "Fbold",    12.0f});  // overlaps prior

    ASSERT_EQ(info.runSegments.size(), 2u);
    EXPECT_EQ(info.runSegments[0].charEnd, 8);
    EXPECT_EQ(info.runSegments[1].charStart, 3)
        << "Model layer must accept overlapping ranges without "
           "rejection; emission-layer is the validation point.";
}

TEST(SubstitutionInfoModel, RunSegmentsCanCarryDifferentFontSizes) {
    SubstitutionInfo info;
    info.runSegments.push_back({0, 4,  "Fregular", 11.0f});
    info.runSegments.push_back({4, 8,  "Fbig",     18.0f});
    info.runSegments.push_back({8, 12, "Fsmall",   8.5f});
    EXPECT_FLOAT_EQ(info.runSegments[0].fontSize, 11.0f);
    EXPECT_FLOAT_EQ(info.runSegments[1].fontSize, 18.0f);
    EXPECT_FLOAT_EQ(info.runSegments[2].fontSize, 8.5f);
}

TEST(SubstitutionInfoModel, DefaultConstructedRunSegmentIsZeroed) {
    RunSegment seg;
    EXPECT_EQ(seg.charStart, 0);
    EXPECT_EQ(seg.charEnd, 0);
    EXPECT_TRUE(seg.fontResName.empty());
    EXPECT_FLOAT_EQ(seg.fontSize, 0.0f)
        << "RunSegment field defaults must be zero-equivalent so the "
           "emission layer's empty-segment skip predicate "
           "(charStart == charEnd) holds for default-constructed "
           "segments without further initialization.";
}

// ─────────────────────────────────────────────────────────────────────
// Copy / move semantics: SubstitutionInfo with runSegments must
// preserve the vector through the standard value-semantics ops the
// pipeline uses (build a SubstitutionInfo, push into a
// std::vector<SubstitutionInfo>, etc.).
// ─────────────────────────────────────────────────────────────────────
TEST(SubstitutionInfoModel, CopyPreservesRunSegments) {
    SubstitutionInfo a;
    a.spanIndex = 2;
    a.runSegments.push_back({0, 3, "Fa", 10.0f});
    a.runSegments.push_back({3, 7, "Fb", 12.0f});

    SubstitutionInfo b = a;
    EXPECT_EQ(b.spanIndex, 2);
    ASSERT_EQ(b.runSegments.size(), 2u);
    EXPECT_EQ(b.runSegments[0].fontResName, "Fa");
    EXPECT_EQ(b.runSegments[1].fontResName, "Fb");
    EXPECT_FLOAT_EQ(b.runSegments[1].fontSize, 12.0f);
}

TEST(SubstitutionInfoModel, MovePreservesRunSegments) {
    SubstitutionInfo a;
    a.runSegments.push_back({0, 5, "Fa", 12.0f});
    SubstitutionInfo b = std::move(a);
    ASSERT_EQ(b.runSegments.size(), 1u);
    EXPECT_EQ(b.runSegments[0].fontResName, "Fa");
}
