// -*- coding: utf-8 -*-
// Phase 9 (wiring): BlockParamsBuilder — converts a plain-text selection
// + PDF bbox + font size into a ready-to-queue AlignmentWriter::BlockParams.
// Used by MainWindow to translate TextEditTool selection state into the
// AlignmentPipeline queue without a layering dependency.
#include <gtest/gtest.h>
#include "reflow/BlockParamsBuilder.h"
#include "reflow/ReflowTypes.h"

using namespace pdfe::reflow;

TEST(BlockParamsBuilderTest, BboxFieldsArePopulatedFromRect)
{
    auto p = BlockParamsBuilder::build(
        "hello world", QRectF(72.0, 700.0, 400.0, 50.0),
        12.0f, Alignment::Left);
    EXPECT_FLOAT_EQ(p.blockLeftPt, 72.0f);
    EXPECT_FLOAT_EQ(p.blockTopPt, 700.0f);
    EXPECT_FLOAT_EQ(p.blockWidthPt, 400.0f);
}

TEST(BlockParamsBuilderTest, FontSizeDrivesLineHeightByDefault)
{
    auto p = BlockParamsBuilder::build(
        "a b c", QRectF(0, 0, 200, 50),
        10.0f, Alignment::Left);
    EXPECT_FLOAT_EQ(p.fontSizePt, 10.0f);
    EXPECT_FLOAT_EQ(p.lineHeightPt, 12.0f);  // 10 * 1.2
}

TEST(BlockParamsBuilderTest, AlignmentIsPassedThrough)
{
    auto left   = BlockParamsBuilder::build("text", QRectF(0,0,100,20), 12.0f, Alignment::Left);
    auto center = BlockParamsBuilder::build("text", QRectF(0,0,100,20), 12.0f, Alignment::Center);
    auto right  = BlockParamsBuilder::build("text", QRectF(0,0,100,20), 12.0f, Alignment::Right);
    auto just   = BlockParamsBuilder::build("text", QRectF(0,0,100,20), 12.0f, Alignment::Justify);
    EXPECT_EQ(left.alignment,   Alignment::Left);
    EXPECT_EQ(center.alignment, Alignment::Center);
    EXPECT_EQ(right.alignment,  Alignment::Right);
    EXPECT_EQ(just.alignment,   Alignment::Justify);
}

TEST(BlockParamsBuilderTest, TokenizeReflowProducesLines)
{
    // With 6pt char width (12pt font * 0.5) and a narrow 80pt box,
    // "alpha beta gamma delta" must wrap onto multiple lines.
    auto p = BlockParamsBuilder::build(
        "alpha beta gamma delta epsilon zeta eta",
        QRectF(0, 0, 80, 200),
        12.0f, Alignment::Left);
    EXPECT_GT(p.lines.size(), 1u);
}

TEST(BlockParamsBuilderTest, DefaultFontResourceNameIsF1)
{
    auto p = BlockParamsBuilder::build(
        "x", QRectF(0,0,100,20), 12.0f, Alignment::Left);
    EXPECT_EQ(p.fontResourceName, "F1");
}

TEST(BlockParamsBuilderTest, FontResourceNameOverrideHonored)
{
    auto p = BlockParamsBuilder::build(
        "x", QRectF(0,0,100,20), 12.0f, Alignment::Left,
        /*measurer=*/nullptr, /*lineHeightMult=*/1.2f,
        /*fontResourceName=*/"F3");
    EXPECT_EQ(p.fontResourceName, "F3");
}

TEST(BlockParamsBuilderTest, CustomMeasurerUsedWhenProvided)
{
    int callCount = 0;
    auto measurer = [&](const std::string& w) {
        ++callCount;
        return static_cast<float>(w.size()) * 8.0f;
    };
    auto p = BlockParamsBuilder::build(
        "alpha beta gamma", QRectF(0,0,200,20),
        12.0f, Alignment::Left, measurer);
    EXPECT_GT(callCount, 0);
    EXPECT_FALSE(p.lines.empty());
}

TEST(BlockParamsBuilderTest, NonpositiveFontSizeFallsBackToDefault)
{
    auto p = BlockParamsBuilder::build(
        "x", QRectF(0,0,100,20), 0.0f, Alignment::Left);
    EXPECT_FLOAT_EQ(p.fontSizePt, 12.0f);
    EXPECT_FLOAT_EQ(p.lineHeightPt, 14.4f);  // 12 * 1.2
}

TEST(BlockParamsBuilderTest, CustomLineHeightMultiplier)
{
    auto p = BlockParamsBuilder::build(
        "x", QRectF(0,0,100,20), 10.0f, Alignment::Left,
        /*measurer=*/nullptr, /*lineHeightMult=*/1.5f);
    EXPECT_FLOAT_EQ(p.lineHeightPt, 15.0f);
}

TEST(BlockParamsBuilderTest, EmptyTextProducesEmptyLines)
{
    auto p = BlockParamsBuilder::build(
        "", QRectF(0,0,100,20), 12.0f, Alignment::Left);
    EXPECT_TRUE(p.lines.empty());
}
