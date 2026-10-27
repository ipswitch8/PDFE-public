// test_unified_text_edit.cpp
// Tests for unified text edit auto-routing: Page::hasTextContent(),
// TextExtractor::hasText(), and tessdata path resolution.

#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <memory>

using namespace pdfe::core;

class UnifiedTextEditTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        fixturesDir = std::filesystem::path(TEST_FIXTURES_DIR);
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path fixturesDir;
};

TEST_F(UnifiedTextEditTest, HasTextContent_NativeTextPage) {
    auto testPdf = fixturesDir / "test_single.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto document = Document::open(context, testPdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    EXPECT_TRUE(page->hasTextContent())
        << "A native text PDF page should report hasTextContent() == true";
}

TEST_F(UnifiedTextEditTest, HasTextContent_ScannedPage) {
    auto testPdf = fixturesDir / "test_scanned.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto document = Document::open(context, testPdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    EXPECT_FALSE(page->hasTextContent())
        << "A scanned/image-only PDF page should report hasTextContent() == false";
}

TEST_F(UnifiedTextEditTest, TextExtractorHasText_Delegates) {
    auto testPdf = fixturesDir / "test_single.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto document = Document::open(context, testPdf);
    ASSERT_NE(document, nullptr);
    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    TextExtractor extractor(context);
    EXPECT_EQ(extractor.hasText(page.get()), page->hasTextContent())
        << "TextExtractor::hasText() should match Page::hasTextContent()";
}

TEST_F(UnifiedTextEditTest, TextExtractorHasText_NullPage) {
    TextExtractor extractor(context);
    EXPECT_FALSE(extractor.hasText(nullptr))
        << "TextExtractor::hasText(nullptr) should return false";
}

TEST_F(UnifiedTextEditTest, TessdataFixtureExists) {
    auto tessdataPath = fixturesDir / "tessdata" / "eng.traineddata";
    EXPECT_TRUE(std::filesystem::exists(tessdataPath))
        << "eng.traineddata should exist in test fixtures at: " << tessdataPath;
}
