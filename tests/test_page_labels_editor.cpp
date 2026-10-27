// -*- coding: utf-8 -*-
// Pipeline E phase-6 / Bucket 3c: page-label editor.
//
// Verifies the PageLabelManager read/write API and the labelFor()
// function, including a save+reopen round-trip that proves /PageLabels
// actually lands in the saved PDF.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PageLabels.h"

#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::core;

// ---------------- Pure-function tests ----------------

TEST(PageLabelFormat, DecimalIsSimple)
{
    EXPECT_EQ(PageLabelManager::formatNumber(1, PageLabelStyle::Decimal), "1");
    EXPECT_EQ(PageLabelManager::formatNumber(42, PageLabelStyle::Decimal), "42");
}

TEST(PageLabelFormat, RomanUpperAndLower)
{
    EXPECT_EQ(PageLabelManager::formatNumber(1, PageLabelStyle::RomanUpper), "I");
    EXPECT_EQ(PageLabelManager::formatNumber(4, PageLabelStyle::RomanUpper), "IV");
    EXPECT_EQ(PageLabelManager::formatNumber(9, PageLabelStyle::RomanUpper), "IX");
    EXPECT_EQ(PageLabelManager::formatNumber(40, PageLabelStyle::RomanUpper), "XL");
    EXPECT_EQ(PageLabelManager::formatNumber(1994, PageLabelStyle::RomanUpper),
              "MCMXCIV");
    EXPECT_EQ(PageLabelManager::formatNumber(3, PageLabelStyle::RomanLower), "iii");
    EXPECT_EQ(PageLabelManager::formatNumber(58, PageLabelStyle::RomanLower),
              "lviii");
}

TEST(PageLabelFormat, AlphaUpperAndLower)
{
    EXPECT_EQ(PageLabelManager::formatNumber(1, PageLabelStyle::AlphaUpper), "A");
    EXPECT_EQ(PageLabelManager::formatNumber(26, PageLabelStyle::AlphaUpper), "Z");
    EXPECT_EQ(PageLabelManager::formatNumber(27, PageLabelStyle::AlphaUpper), "AA");
    EXPECT_EQ(PageLabelManager::formatNumber(28, PageLabelStyle::AlphaUpper), "BB");
    EXPECT_EQ(PageLabelManager::formatNumber(53, PageLabelStyle::AlphaUpper),
              "AAA");
    EXPECT_EQ(PageLabelManager::formatNumber(1, PageLabelStyle::AlphaLower), "a");
}

TEST(PageLabelFormat, NoneStyleProducesEmpty)
{
    EXPECT_EQ(PageLabelManager::formatNumber(7, PageLabelStyle::None), "");
}

TEST(PageLabelStyleConversion, RoundTripsAllStyles)
{
    using S = PageLabelStyle;
    for (S s : {S::None, S::Decimal, S::RomanUpper, S::RomanLower,
                S::AlphaUpper, S::AlphaLower}) {
        char c = PageLabelManager::styleToPdfChar(s);
        EXPECT_EQ(PageLabelManager::pdfCharToStyle(c), s)
            << "Round-trip failed for style " << static_cast<int>(s);
    }
}

TEST(PageLabelLabelFor, MultipleRangesPickActive)
{
    std::vector<PageLabelRange> ranges = {
        // Pages 0-2: i, ii, iii
        {0, PageLabelStyle::RomanLower, "", 1},
        // Pages 3-: 1, 2, 3, ...
        {3, PageLabelStyle::Decimal,    "", 1},
        // Pages 6-: A-1, A-2, A-3
        {6, PageLabelStyle::Decimal,    "A-", 1},
    };
    EXPECT_EQ(PageLabelManager::labelFor(0, ranges), "i");
    EXPECT_EQ(PageLabelManager::labelFor(1, ranges), "ii");
    EXPECT_EQ(PageLabelManager::labelFor(2, ranges), "iii");
    EXPECT_EQ(PageLabelManager::labelFor(3, ranges), "1");
    EXPECT_EQ(PageLabelManager::labelFor(5, ranges), "3");
    EXPECT_EQ(PageLabelManager::labelFor(6, ranges), "A-1");
    EXPECT_EQ(PageLabelManager::labelFor(8, ranges), "A-3");
}

TEST(PageLabelLabelFor, EmptyRangesReturnsEmpty)
{
    EXPECT_EQ(PageLabelManager::labelFor(0, {}), "");
}

// ---------------- Document round-trip ----------------

class PageLabelDocTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        auto src = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(workDir.isValid());
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    QTemporaryDir workDir;
};

TEST_F(PageLabelDocTest, WriteAndReadRangesInMemory)
{
    std::vector<PageLabelRange> ranges = {
        {0, PageLabelStyle::RomanUpper, "", 1},
        {1, PageLabelStyle::Decimal,    "Ch-", 5},
    };
    PageLabelManager::write(doc.get(), ranges);

    auto readBack = PageLabelManager::read(doc.get());
    ASSERT_EQ(readBack.size(), 2u);
    EXPECT_EQ(readBack[0].firstPage, 0);
    EXPECT_EQ(readBack[0].style, PageLabelStyle::RomanUpper);
    EXPECT_EQ(readBack[0].prefix, "");
    EXPECT_EQ(readBack[0].startValue, 1);
    EXPECT_EQ(readBack[1].firstPage, 1);
    EXPECT_EQ(readBack[1].style, PageLabelStyle::Decimal);
    EXPECT_EQ(readBack[1].prefix, "Ch-");
    EXPECT_EQ(readBack[1].startValue, 5);
}

TEST_F(PageLabelDocTest, SaveAndReopenPreservesPageLabels)
{
    std::vector<PageLabelRange> ranges = {
        {0, PageLabelStyle::RomanLower, "",      1},
        {2, PageLabelStyle::Decimal,    "Page-", 1},
    };
    PageLabelManager::write(doc.get(), ranges);

    std::string outPath = (std::filesystem::path(
        workDir.path().toStdString()) / "labelled.pdf").string();
    ASSERT_NO_THROW(doc->saveAs(outPath));

    doc.reset();
    auto reopened = Document::open(ctx, outPath);
    ASSERT_NE(reopened, nullptr);
    auto readBack = PageLabelManager::read(reopened.get());
    ASSERT_EQ(readBack.size(), 2u);
    EXPECT_EQ(readBack[0].style, PageLabelStyle::RomanLower);
    EXPECT_EQ(readBack[1].style, PageLabelStyle::Decimal);
    EXPECT_EQ(readBack[1].prefix, "Page-");

    EXPECT_EQ(PageLabelManager::labelFor(0, readBack), "i");
    EXPECT_EQ(PageLabelManager::labelFor(1, readBack), "ii");
    EXPECT_EQ(PageLabelManager::labelFor(2, readBack), "Page-1");
}

TEST_F(PageLabelDocTest, EmptyRangesRemovesPageLabels)
{
    PageLabelManager::write(doc.get(), {
        {0, PageLabelStyle::RomanLower, "", 1}});
    ASSERT_FALSE(PageLabelManager::read(doc.get()).empty());
    PageLabelManager::write(doc.get(), {});
    EXPECT_TRUE(PageLabelManager::read(doc.get()).empty());
}
