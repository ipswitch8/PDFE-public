// -*- coding: utf-8 -*-
// Phase 9 continuation: AlignmentWriter emits the content-stream
// fragment with per-line Tm offsets and the %%PDFE-ALIGN marker that
// lets reopen-and-edit recover the paragraph's alignment.
#include <gtest/gtest.h>
#include "reflow/AlignmentWriter.h"
#include "reflow/TextReflowEngine.h"
#include "reflow/ReflowTypes.h"

#include <sstream>
#include <string>

using namespace pdfe::reflow;

namespace {

std::vector<ReflowLine> buildLines(const std::string& text,
                                    float maxW, float charW)
{
    auto measurer = [charW](const std::string& w) {
        return static_cast<float>(w.size()) * charW;
    };
    auto words = TextReflowEngine::tokenize(text, measurer);
    return TextReflowEngine::reflow(words, maxW, /*spaceWidthPt=*/charW);
}

int countOccurrences(const std::string& hay, const std::string& needle)
{
    int n = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

// -------- Marker round-trip --------

TEST(AlignmentMarker, EmitsExpectedWord)
{
    EXPECT_EQ(AlignmentWriter::alignmentMarker(Alignment::Left),
              "%%PDFE-ALIGN: left\n");
    EXPECT_EQ(AlignmentWriter::alignmentMarker(Alignment::Center),
              "%%PDFE-ALIGN: center\n");
    EXPECT_EQ(AlignmentWriter::alignmentMarker(Alignment::Right),
              "%%PDFE-ALIGN: right\n");
    EXPECT_EQ(AlignmentWriter::alignmentMarker(Alignment::Justify),
              "%%PDFE-ALIGN: justify\n");
}

TEST(AlignmentMarker, ParsesAllFourValues)
{
    EXPECT_EQ(AlignmentWriter::parseAlignmentMarker(
                  "%%PDFE-ALIGN: left\n..."), Alignment::Left);
    EXPECT_EQ(AlignmentWriter::parseAlignmentMarker(
                  "%%PDFE-ALIGN: center\n..."), Alignment::Center);
    EXPECT_EQ(AlignmentWriter::parseAlignmentMarker(
                  "%%PDFE-ALIGN: right\n..."), Alignment::Right);
    EXPECT_EQ(AlignmentWriter::parseAlignmentMarker(
                  "%%PDFE-ALIGN: justify\n..."), Alignment::Justify);
}

TEST(AlignmentMarker, TolerantToLeadingWhitespaceAndCRLF)
{
    EXPECT_EQ(AlignmentWriter::parseAlignmentMarker(
                  "%%PDFE-ALIGN:   center\r\n..."), Alignment::Center);
}

TEST(AlignmentMarker, AbsentMarkerFallsBackToLeft)
{
    EXPECT_EQ(AlignmentWriter::parseAlignmentMarker(
                  "BT /F1 12 Tf ..."), Alignment::Left);
    EXPECT_EQ(AlignmentWriter::parseAlignmentMarker(""), Alignment::Left);
}

TEST(AlignmentMarker, UnknownWordFallsBackToLeft)
{
    EXPECT_EQ(AlignmentWriter::parseAlignmentMarker(
                  "%%PDFE-ALIGN: diagonal\n"), Alignment::Left);
}

// -------- PDF literal escaping --------

TEST(PdfLiteralEscape, EscapesOpenAndCloseParens)
{
    EXPECT_EQ(AlignmentWriter::escapePdfLiteral("(hello)"),
              "\\(hello\\)");
}

TEST(PdfLiteralEscape, EscapesBackslash)
{
    EXPECT_EQ(AlignmentWriter::escapePdfLiteral("a\\b"),
              "a\\\\b");
}

TEST(PdfLiteralEscape, LeavesPlainTextUnchanged)
{
    EXPECT_EQ(AlignmentWriter::escapePdfLiteral("plain ASCII text"),
              "plain ASCII text");
}

// -------- Emit: structural --------

TEST(AlignmentEmit, LeadsWithMarkerAndOpensGraphicsState)
{
    AlignmentWriter::BlockParams p;
    p.lines = buildLines("hello world", 100.0f, 6.0f);
    p.alignment = Alignment::Left;
    p.blockLeftPt  = 72.0f;
    p.blockTopPt   = 700.0f;
    p.blockWidthPt = 400.0f;
    p.lineHeightPt = 14.0f;
    p.fontSizePt   = 12.0f;

    auto body = AlignmentWriter::emitFragment(p);
    // Marker first.
    EXPECT_EQ(body.substr(0, 13), "%%PDFE-ALIGN:");
    // q / Q frame the whole fragment.
    EXPECT_NE(body.find("\nq\n"), std::string::npos);
    EXPECT_NE(body.find("\nQ\n"), std::string::npos);
    // One BT per line; one ET per BT.
    EXPECT_EQ(countOccurrences(body, "BT\n"), countOccurrences(body, "ET\n"));
    EXPECT_GT(countOccurrences(body, "BT\n"), 0);
}

TEST(AlignmentEmit, EmptyLinesProducesMarkerOnly)
{
    AlignmentWriter::BlockParams p;
    p.alignment = Alignment::Right;
    auto body = AlignmentWriter::emitFragment(p);
    EXPECT_EQ(body, "%%PDFE-ALIGN: right\n");
    EXPECT_EQ(body.find("BT"), std::string::npos);
}

TEST(AlignmentEmit, EmitsFontResourceRef)
{
    AlignmentWriter::BlockParams p;
    p.lines = buildLines("single line", 200.0f, 6.0f);
    p.fontResourceName = "F2";
    p.fontSizePt = 14.0f;
    auto body = AlignmentWriter::emitFragment(p);
    EXPECT_NE(body.find("/F2 14"), std::string::npos)
        << "expected '/F2 14 Tf' for font resource + size";
}

// -------- Emit: alignment-dependent Tm offsets --------

TEST(AlignmentEmit, LeftBlockEmitsZeroXOffset)
{
    AlignmentWriter::BlockParams p;
    p.lines = buildLines("one two three", 100.0f, 6.0f);
    p.alignment = Alignment::Left;
    p.blockLeftPt  = 72.0f;
    p.blockTopPt   = 700.0f;
    p.blockWidthPt = 400.0f;
    auto body = AlignmentWriter::emitFragment(p);
    // Each line's Tm should translate to blockLeftPt + 0 = 72.
    EXPECT_NE(body.find("1 0 0 1 72."), std::string::npos)
        << "Left alignment → x = blockLeftPt";
}

TEST(AlignmentEmit, RightBlockPushesLineToRightEdge)
{
    AlignmentWriter::BlockParams p;
    p.lines = buildLines("short", 300.0f, 6.0f);   // one short line
    p.alignment = Alignment::Right;
    p.blockLeftPt  = 0.0f;
    p.blockTopPt   = 700.0f;
    p.blockWidthPt = 300.0f;
    auto body = AlignmentWriter::emitFragment(p);
    // "short" is 5*6 = 30pt → slack = 300 - 30 = 270 → x = 270.
    EXPECT_NE(body.find("1 0 0 1 270."), std::string::npos);
}

TEST(AlignmentEmit, CenterBlockHalvesTheSlack)
{
    AlignmentWriter::BlockParams p;
    p.lines = buildLines("short", 300.0f, 6.0f);
    p.alignment = Alignment::Center;
    p.blockLeftPt  = 0.0f;
    p.blockTopPt   = 700.0f;
    p.blockWidthPt = 300.0f;
    auto body = AlignmentWriter::emitFragment(p);
    // slack/2 = 135.
    EXPECT_NE(body.find("1 0 0 1 135."), std::string::npos);
}

TEST(AlignmentEmit, JustifyEmitsTwWordSpacingOnNonLastLines)
{
    AlignmentWriter::BlockParams p;
    // Force multiple lines: narrow block, multi-word input.
    p.lines = buildLines(
        "word1 word2 word3 word4 word5 word6 word7 word8", 50.0f, 3.0f);
    ASSERT_GT(p.lines.size(), 1u);
    p.alignment = Alignment::Justify;
    p.blockWidthPt = 50.0f;
    auto body = AlignmentWriter::emitFragment(p);
    // Last line resets Tw to 0; at least one non-last line has a
    // positive Tw value.
    EXPECT_NE(body.find(" Tw\n"), std::string::npos);
    EXPECT_NE(body.find("0 Tw\n"), std::string::npos);
}

TEST(AlignmentEmit, BaselineStepsDownPerLine)
{
    AlignmentWriter::BlockParams p;
    p.lines = buildLines("alpha", 100.0f, 6.0f);
    p.lines.push_back(ReflowLine{.text="beta", .width=24.0f, .wordCount=1, .isLastLine=true});
    p.lines[0].isLastLine = false;
    p.blockLeftPt = 0.0f;
    p.blockTopPt = 100.0f;
    p.blockWidthPt = 100.0f;
    p.lineHeightPt = 20.0f;
    auto body = AlignmentWriter::emitFragment(p);
    // First baseline: top - lineHeight = 100 - 20 = 80.
    // Second baseline: 80 - 20 = 60.
    EXPECT_NE(body.find("1 0 0 1 0.000 80.000 Tm"), std::string::npos);
    EXPECT_NE(body.find("1 0 0 1 0.000 60.000 Tm"), std::string::npos);
}

// -------- Round-trip: emit → parse marker → same alignment --------

TEST(AlignmentRoundTrip, ParsingEmittedFragmentRecoversAlignment)
{
    for (auto align : {Alignment::Left, Alignment::Center,
                       Alignment::Right, Alignment::Justify}) {
        AlignmentWriter::BlockParams p;
        p.alignment = align;
        p.lines = buildLines("one two three", 100.0f, 6.0f);
        auto body = AlignmentWriter::emitFragment(p);
        EXPECT_EQ(AlignmentWriter::parseAlignmentMarker(body), align)
            << "round-trip failed for alignment index "
            << static_cast<int>(align);
    }
}
