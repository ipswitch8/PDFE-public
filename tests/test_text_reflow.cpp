// -*- coding: utf-8 -*-
// Phase 9: TextReflowEngine tests — tokenization, greedy line breaking,
// alignment offset math, and a perf budget test for a 500-char block.
#include <gtest/gtest.h>
#include "reflow/TextReflowEngine.h"
#include "reflow/ReflowTypes.h"

#include <chrono>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::reflow;

namespace {

// A deterministic word measurer for tests: each word's width equals its
// character count times a configurable per-char advance (in points).
auto makeUniformMeasurer(float perCharPt)
{
    return [perCharPt](const std::string& w) -> float {
        return static_cast<float>(w.size()) * perCharPt;
    };
}

std::vector<ReflowLine> reflowPlain(const std::string& text,
                                    float maxW, float charW)
{
    auto measurer = makeUniformMeasurer(charW);
    auto words = TextReflowEngine::tokenize(text, measurer);
    return TextReflowEngine::reflow(words, maxW, /*spaceWidthPt=*/charW);
}

} // namespace

// -------- Tokenize --------

TEST(Tokenize, SplitsOnWhitespaceAndCollapsesRuns)
{
    auto m = makeUniformMeasurer(1.0f);
    auto words = TextReflowEngine::tokenize("  hello   world\t\nfoo ", m);
    ASSERT_EQ(words.size(), 3u);
    EXPECT_EQ(words[0].text, "hello");
    EXPECT_EQ(words[1].text, "world");
    EXPECT_EQ(words[2].text, "foo");
}

TEST(Tokenize, EmptyTextYieldsNoWords)
{
    auto m = makeUniformMeasurer(1.0f);
    EXPECT_TRUE(TextReflowEngine::tokenize("", m).empty());
    EXPECT_TRUE(TextReflowEngine::tokenize("   \t \n ", m).empty());
}

TEST(Tokenize, MeasurerDeterminesWordWidth)
{
    auto m = makeUniformMeasurer(2.5f);
    auto words = TextReflowEngine::tokenize("abc de", m);
    ASSERT_EQ(words.size(), 2u);
    EXPECT_FLOAT_EQ(words[0].width, 3 * 2.5f);
    EXPECT_FLOAT_EQ(words[1].width, 2 * 2.5f);
}

// -------- Greedy reflow --------

TEST(Reflow, SingleWordThatFitsGivesOneLine)
{
    auto lines = reflowPlain("hello", 100.0f, 3.0f);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, "hello");
    EXPECT_EQ(lines[0].wordCount, 1);
    EXPECT_TRUE(lines[0].isLastLine);
}

TEST(Reflow, AllWordsFitOnOneLine)
{
    // 5 chars each, 3 words, space=3pt → total "aaaaa aaaaa aaaaa" needs
    // (5*3 per-char) + (2 spaces) = 15+15+15 + 3+3 = 51pt; width=100 fits.
    auto lines = reflowPlain("aaaaa aaaaa aaaaa", 100.0f, 3.0f);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].wordCount, 3);
    EXPECT_TRUE(lines[0].isLastLine);
}

TEST(Reflow, GreedyBreaksWhenNextWordOverflows)
{
    // width budget = 30pt, per-char 3pt, space 3pt.
    // "hello" = 15pt fits. "world" = 15pt + 3pt space = +18pt → 33pt.
    // Overflow → commit "hello" and start "world".
    auto lines = reflowPlain("hello world", 30.0f, 3.0f);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].text, "hello");
    EXPECT_EQ(lines[1].text, "world");
    EXPECT_TRUE(lines[1].isLastLine);
}

TEST(Reflow, WordLargerThanWidthLandsOnOwnLineAndOverflows)
{
    auto lines = reflowPlain("supercalifragilistic", 10.0f, 2.0f);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, "supercalifragilistic");
}

TEST(Reflow, LastLineFlagSetOnFinalLineOnly)
{
    auto lines = reflowPlain("a b c d e", 10.0f, 3.0f);
    ASSERT_GT(lines.size(), 1u);
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        EXPECT_FALSE(lines[i].isLastLine) << "line " << i << " flagged as last";
    }
    EXPECT_TRUE(lines.back().isLastLine);
}

// -------- Line offset / alignment --------

TEST(LineOffset, LeftAlignmentIsZero)
{
    EXPECT_FLOAT_EQ(TextReflowEngine::lineXOffset(200.0f, 100.0f,
                                                   Alignment::Left, false), 0.0f);
}

TEST(LineOffset, CenterIsHalfOfSlack)
{
    EXPECT_FLOAT_EQ(TextReflowEngine::lineXOffset(200.0f, 100.0f,
                                                   Alignment::Center, false), 50.0f);
}

TEST(LineOffset, RightPushesToRightEdge)
{
    EXPECT_FLOAT_EQ(TextReflowEngine::lineXOffset(200.0f, 100.0f,
                                                   Alignment::Right, false), 100.0f);
}

TEST(LineOffset, JustifyLastLineIsLeft)
{
    EXPECT_FLOAT_EQ(TextReflowEngine::lineXOffset(200.0f, 100.0f,
                                                   Alignment::Justify, true), 0.0f);
}

// -------- Justify extra spacing --------

TEST(JustifyExtra, OnlyAppliesToJustifyAlignment)
{
    EXPECT_FLOAT_EQ(
        TextReflowEngine::justifyExtraSpace(200.0f, 100.0f, 5, Alignment::Left, false),
        0.0f);
    EXPECT_FLOAT_EQ(
        TextReflowEngine::justifyExtraSpace(200.0f, 100.0f, 5, Alignment::Center, false),
        0.0f);
}

TEST(JustifyExtra, LastLineGetsNoExtraSpacing)
{
    EXPECT_FLOAT_EQ(
        TextReflowEngine::justifyExtraSpace(200.0f, 100.0f, 5, Alignment::Justify, true),
        0.0f);
}

TEST(JustifyExtra, DistributesSlackAcrossWordGaps)
{
    // blockWidth=200, lineWidth=100, 5 words → 4 gaps.
    // extra per gap = (200-100) / 4 = 25.
    EXPECT_FLOAT_EQ(
        TextReflowEngine::justifyExtraSpace(200.0f, 100.0f, 5, Alignment::Justify, false),
        25.0f);
}

TEST(JustifyExtra, SingleWordLineNoExtraSpace)
{
    EXPECT_FLOAT_EQ(
        TextReflowEngine::justifyExtraSpace(200.0f, 100.0f, 1, Alignment::Justify, false),
        0.0f);
}

// -------- Reflow on text growth + shrink --------

TEST(ReflowAdaptive, InsertGrowsLineCount)
{
    auto lines1 = reflowPlain("alpha beta gamma", 40.0f, 3.0f);
    auto lines2 = reflowPlain("alpha beta gamma delta epsilon zeta",
                              40.0f, 3.0f);
    EXPECT_GT(lines2.size(), lines1.size())
        << "adding more text should produce more lines";
}

TEST(ReflowAdaptive, DeleteShrinksLineCount)
{
    auto lines1 = reflowPlain("alpha beta gamma delta epsilon zeta",
                              40.0f, 3.0f);
    auto lines2 = reflowPlain("alpha beta", 40.0f, 3.0f);
    EXPECT_LT(lines2.size(), lines1.size());
}

TEST(ReflowAdaptive, FontSizeDoublingRoughlyHalvesLineCount)
{
    // Same text, width budget, but per-char width doubles → each word
    // takes twice the space → roughly half as many words per line.
    const std::string text =
        "one two three four five six seven eight nine ten";
    auto small = reflowPlain(text, 40.0f, 2.0f);
    auto big   = reflowPlain(text, 40.0f, 4.0f);
    // Expect big has more lines; within ±1 of 2x factor.
    EXPECT_GT(big.size(), small.size());
    EXPECT_LE(big.size(), small.size() * 2 + 1);
}

// -------- Perf budget --------

TEST(ReflowPerf, FiveHundredCharBlockUnderOneHundredMilliseconds)
{
    // Build a block of ~500 characters (≈80 words).
    std::ostringstream os;
    const std::string word = "lorem ";
    while (os.str().size() < 500) os << word;
    const std::string text = os.str();

    auto measurer = makeUniformMeasurer(4.0f);   // simulate HarfBuzz
    auto words = TextReflowEngine::tokenize(text, measurer);

    auto t0 = std::chrono::steady_clock::now();
    auto lines = TextReflowEngine::reflow(words, 400.0f, 4.0f);
    auto t1 = std::chrono::steady_clock::now();

    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    EXPECT_LT(elapsedMs, 100)
        << "500-char reflow took " << elapsedMs << " ms (budget 100 ms)";
    EXPECT_GT(lines.size(), 0u);
}
