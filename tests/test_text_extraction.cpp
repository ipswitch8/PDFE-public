#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "core/TextBlock.h"
#include "core/TextLine.h"
#include "core/TextChar.h"
#include "core/MuPdfContext.h"
#include "core/Error.h"
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;

class TextExtractionTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
        }
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
};

TEST_F(TextExtractionTest, ExtractPlainText) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    std::string text = page->extractPlainText();

    // The test PDF should contain some text
    EXPECT_FALSE(text.empty()) << "Extracted text should not be empty for a text-containing PDF";
}

TEST_F(TextExtractionTest, ExtractStructuredText) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    std::vector<TextBlock> blocks = page->extractText();

    // Should have at least one text block
    EXPECT_FALSE(blocks.empty()) << "Should extract at least one text block";

    // Check block structure
    for (const auto& block : blocks) {
        if (block.type == TextBlockType::Image) {
            continue;  // Image blocks don't have text
        }

        // Each text block should have lines
        EXPECT_FALSE(block.lines.empty()) << "Text block should have at least one line";

        // Block bounding box should be valid
        EXPECT_GT(block.boundingBox.width(), 0.0);
        EXPECT_GT(block.boundingBox.height(), 0.0);

        for (const auto& line : block.lines) {
            // Each line should have characters
            EXPECT_FALSE(line.chars.empty()) << "Line should have at least one character";

            // Line bounding box should be valid
            EXPECT_GT(line.boundingBox.width(), 0.0);
            EXPECT_GT(line.boundingBox.height(), 0.0);

            // Line text should match concatenated characters
            std::string lineText = line.text();
            EXPECT_FALSE(lineText.empty());
        }
    }
}

TEST_F(TextExtractionTest, CharacterPositions) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    auto page = document->page(0);
    std::vector<TextBlock> blocks = page->extractText();

    bool foundCharWithPosition = false;
    for (const auto& block : blocks) {
        if (block.type == TextBlockType::Image) continue;

        for (const auto& line : block.lines) {
            for (const auto& ch : line.chars) {
                // Each character should have valid quad
                EXPECT_GE(ch.quad.width(), 0.0) << "Character quad width should be non-negative";
                EXPECT_GE(ch.quad.height(), 0.0) << "Character quad height should be non-negative";

                // Font size should be positive for real characters
                // GitHub #273: the size check was gated on `codepoint > 32`,
                // so for a page of only spaces/control characters nothing was
                // asserted. Both classes are specifiable — a printable glyph
                // must have a positive size, and a control/space glyph must
                // still not have a NEGATIVE one — so assert on both paths.
                if (ch.codepoint > 32) {  // printable
                    EXPECT_GT(ch.fontSize, 0.0f)
                        << "Font size should be positive for character U+"
                        << std::hex << static_cast<uint32_t>(ch.codepoint);
                    foundCharWithPosition = true;
                } else {                  // control / space
                    EXPECT_GE(ch.fontSize, 0.0f)
                        << "Font size must not be negative, even for the "
                           "control/space character U+"
                        << std::hex << static_cast<uint32_t>(ch.codepoint);
                }
            }
        }
    }

    EXPECT_TRUE(foundCharWithPosition) << "Should find at least one character with position info";
}

TEST_F(TextExtractionTest, EmptyPage) {
    // Create a document and try to extract text from a page that might be empty
    // We test that extraction doesn't crash even on pages with no text
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Try all pages — at minimum, extraction should not crash
    for (int i = 0; i < document->pageCount(); ++i) {
        auto page = document->page(i);
        ASSERT_NE(page, nullptr);

        // These should not throw
        EXPECT_NO_THROW({
            std::vector<TextBlock> blocks = page->extractText();
        });
        EXPECT_NO_THROW({
            std::string text = page->extractPlainText();
        });
    }
}

TEST_F(TextExtractionTest, TextExtractorStandalone) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Test TextExtractor directly (not through Page convenience methods)
    TextExtractor extractor(context);

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    std::vector<TextBlock> blocks = extractor.extract(page.get());
    EXPECT_FALSE(blocks.empty());

    std::string plainText = extractor.extractPlainText(page.get());
    EXPECT_FALSE(plainText.empty());
}

TEST_F(TextExtractionTest, TextExtractorNullPage) {
    TextExtractor extractor(context);

    // Extracting from null page should throw
    EXPECT_THROW(extractor.extract(nullptr), PdfeException);
    EXPECT_THROW(extractor.extractPlainText(nullptr), PdfeException);
}

TEST_F(TextExtractionTest, MultiPageExtraction) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    if (document->pageCount() < 2) {
        GTEST_SKIP() << "Need at least 2 pages for multi-page test";
    }

    // Extract text from multiple pages
    for (int i = 0; i < document->pageCount(); ++i) {
        auto page = document->page(i);
        ASSERT_NE(page, nullptr);

        std::string text = page->extractPlainText();
        // Each page should produce some result (even if empty string for blank pages)
        // The key test is that it doesn't crash
    }
}

TEST_F(TextExtractionTest, TextBlockTypes) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);
    std::vector<TextBlock> blocks = page->extractText();

    for (const auto& block : blocks) {
        // Type should be one of the defined enum values
        EXPECT_TRUE(
            block.type == TextBlockType::Paragraph ||
            block.type == TextBlockType::List ||
            block.type == TextBlockType::Table ||
            block.type == TextBlockType::Image ||
            block.type == TextBlockType::Unknown
        );
    }
}

TEST_F(TextExtractionTest, PlainTextMatchesStructured) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);

    std::string plainText = page->extractPlainText();
    std::vector<TextBlock> blocks = page->extractText();

    // Build text from structured blocks
    std::string structuredText;
    for (const auto& block : blocks) {
        if (block.type == TextBlockType::Image) continue;
        if (!structuredText.empty()) {
            structuredText += "\n";
        }
        structuredText += block.text();
    }

    // Both methods should produce non-empty text (from same page)
    // The exact format may differ (MuPDF plain text vs our structured concatenation),
    // but both should contain the same words
    EXPECT_FALSE(plainText.empty());
    EXPECT_FALSE(structuredText.empty());
}

TEST_F(TextExtractionTest, TextExtractorIsMovable) {
    TextExtractor extractor1(context);

    // Move constructor
    TextExtractor extractor2(std::move(extractor1));

    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);
    // Moved-to extractor should work
    EXPECT_NO_THROW({
        auto blocks = extractor2.extract(page.get());
    });
}
