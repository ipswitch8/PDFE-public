// -*- coding: utf-8 -*-
// Bates numbering via {bates:PREFIX:START[:WIDTH]} header/footer token.
// Expands per-page so headers/footers can stamp unique incrementing
// identifiers the way the legal industry expects them.
#include <gtest/gtest.h>
#include "headerfooter/HeaderFooterEngine.h"

using pdfe::headerfooter::HeaderFooterEngine;

namespace {
std::string resolve(const std::string& raw, int pageIdx, int total = 10)
{
    return HeaderFooterEngine::resolveTokens(
        raw, pageIdx, total, "author", "file.pdf", "2026-04-22");
}
} // namespace

TEST(BatesToken, BasicDefaultWidth)
{
    // {bates:PREFIX:START} with default WIDTH=6
    EXPECT_EQ(resolve("{bates:ACME:1000}", 0), "ACME001000");
    EXPECT_EQ(resolve("{bates:ACME:1000}", 5), "ACME001005");
}

TEST(BatesToken, ExplicitWidth)
{
    EXPECT_EQ(resolve("{bates:X:1:4}", 0), "X0001");
    EXPECT_EQ(resolve("{bates:X:1:4}", 99), "X0100");
}

TEST(BatesToken, OverflowsGracefully)
{
    // Width=2 but number needs 3 digits → just use the raw number.
    EXPECT_EQ(resolve("{bates:X:99:2}", 2), "X101");
}

TEST(BatesToken, EmptyPrefixAllowed)
{
    EXPECT_EQ(resolve("{bates::1000}", 0), "001000");
}

TEST(BatesToken, MultipleTokensInOneString)
{
    EXPECT_EQ(resolve("Doc {bates:A:1:3} of {bates:Z:100:4}", 2),
              "Doc A003 of Z0102");
}

TEST(BatesToken, InvalidTokenLeftAsIs)
{
    // No colon after "bates:" means not well-formed — leave in place.
    EXPECT_EQ(resolve("{bates:no-colon}", 0), "{bates:no-colon}");
    // Non-numeric start.
    EXPECT_EQ(resolve("{bates:X:abc:3}", 0), "{bates:X:abc:3}");
}

TEST(BatesToken, ClampsInsaneWidthToDefault)
{
    // width=0 / huge widths silently clamp to default 6.
    EXPECT_EQ(resolve("{bates:Y:1:0}", 0), "Y000001");
    EXPECT_EQ(resolve("{bates:Y:1:99}", 0), "Y000001");
}

TEST(BatesToken, CoexistsWithOtherTokens)
{
    // {page}, {pages}, {bates:...} all in one string.
    EXPECT_EQ(resolve("p.{page}/{pages} id:{bates:L:500}", 2),
              "p.3/10 id:L000502");
}

TEST(BatesToken, PageIndexZeroStartsAtStart)
{
    // Page index 0 (first page) gets START exactly.
    EXPECT_EQ(resolve("{bates:P:42}", 0), "P000042");
}

TEST(BatesToken, UnclosedTokenLeftAsIs)
{
    // Missing closing brace — append rest unchanged.
    EXPECT_EQ(resolve("prefix {bates:X:1 suffix", 0),
              "prefix {bates:X:1 suffix");
}
