// tests/test_text_edit_pipeline.cpp
// Unit tests for ContentStreamEditor text injection pipeline
// Tests the complete flow: extract → modify → apply → re-extract

#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "textedit/TextEditModel.h"
#include "textedit/ContentStreamEditor.h"
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <filesystem>

// Test fixture for ContentStreamEditor pipeline tests
class TextEditPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize MuPDF context
        context_ = std::make_shared<pdfe::core::MuPdfContext>();

        // Open test PDF with known text content
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_single.pdf not found";
        }
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr) << "Failed to open test PDF";
        ASSERT_GT(document_->pageCount(), 0) << "Test PDF has no pages";

        // Open multi-span test PDF
        std::filesystem::path multiSpanPath =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_multispan.pdf";
        if (std::filesystem::exists(multiSpanPath)) {
            multiSpanDoc_ = pdfe::core::Document::open(context_, multiSpanPath);
        }
    }

    void TearDown() override {
        multiSpanDoc_.reset();
        document_.reset();
        context_.reset();
    }

    // Helper: Extract text spans from a page
    std::vector<pdfe::textedit::TextSpan> extractSpans(pdfe::core::Page* page) {
        pdfe::core::TextExtractor extractor(context_);
        auto blocks = extractor.extract(page);
        pdfe::textedit::TextEditModel model(blocks);

        std::vector<pdfe::textedit::TextSpan> spans;
        for (int i = 0; i < model.spanCount(); ++i) {
            spans.push_back(model.span(i));
        }
        return spans;
    }

    // Helper: Extract plain text from page
    std::string extractPlainText(pdfe::core::Page* page) {
        pdfe::core::TextExtractor extractor(context_);
        return extractor.extractPlainText(page);
    }

    // Helper: Apply edits and reload page
    std::unique_ptr<pdfe::core::Page> applyEditsAndReload(
        pdfe::core::Page* page,
        const std::vector<pdfe::textedit::TextSpan>& allSpans,
        const std::vector<std::pair<int, std::u32string>>& modifications)
    {
        pdfe::textedit::ContentStreamEditor editor(context_.get());
        editor.applyEdits(page, document_.get(), allSpans, modifications);

        // Reload page to see changes (page's fz_page is invalidated after edit)
        return document_->page(0);
    }

    // Helper: Apply edits on a specified document and reload page
    std::unique_ptr<pdfe::core::Page> applyEditsOnDoc(
        pdfe::core::Document* doc,
        pdfe::core::Page* page,
        const std::vector<pdfe::textedit::TextSpan>& allSpans,
        const std::vector<std::pair<int, std::u32string>>& modifications)
    {
        pdfe::textedit::ContentStreamEditor editor(context_.get());
        editor.applyEdits(page, doc, allSpans, modifications);
        return doc->page(0);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::core::Document> multiSpanDoc_;
};

// Test 1: FontNameMappingWorks
// Verify that PostScript font names from extraction can be used with ContentStreamEditor
// without crashes, and that replacement text appears after applying edits.
TEST_F(TextEditPipelineTest, FontNameMappingWorks) {
    // Extract initial page
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Extract text spans
    auto spans = extractSpans(page.get());
    ASSERT_GT(spans.size(), 0) << "Test PDF should have at least one text span";

    // Verify we got font names (should be PostScript names like "AAAAAA+SomeFont")
    const auto& firstSpan = spans[0];
    EXPECT_FALSE(firstSpan.fontName.empty()) << "Span should have a font name";

    // Record original text for comparison
    std::u32string originalText = firstSpan.text;
    EXPECT_GT(originalText.length(), 0) << "First span should have text";

    // Apply edit: replace first span with "TEST_REPLACEMENT"
    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, U"TEST_REPLACEMENT"});

    // Apply edits (should not crash despite font name mapping complexity)
    ASSERT_NO_THROW({
        page = applyEditsAndReload(page.get(), spans, modifications);
    });
    ASSERT_NE(page, nullptr);

    // Re-extract text and verify replacement appears
    std::string newPlainText = extractPlainText(page.get());
    EXPECT_NE(newPlainText.find("TEST_REPLACEMENT"), std::string::npos)
        << "Replacement text should appear in extracted text. Got: " << newPlainText;
}

// Test 2: EditedTextAppearsInExtraction
// Replace first span's text with "REPLACED" and verify it appears in re-extraction.
TEST_F(TextEditPipelineTest, EditedTextAppearsInExtraction) {
    // Extract initial page
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Extract text spans
    auto spans = extractSpans(page.get());
    ASSERT_GT(spans.size(), 0) << "Test PDF should have at least one text span";

    // Record original text
    std::string originalPlainText = extractPlainText(page.get());
    EXPECT_GT(originalPlainText.length(), 0) << "Original page should have text";

    // Apply edit: replace first span with "REPLACED"
    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, U"REPLACED"});

    page = applyEditsAndReload(page.get(), spans, modifications);
    ASSERT_NE(page, nullptr);

    // Re-extract text
    std::string newPlainText = extractPlainText(page.get());

    // Verify "REPLACED" appears in extraction
    EXPECT_NE(newPlainText.find("REPLACED"), std::string::npos)
        << "Edited text 'REPLACED' should appear in re-extraction. Got: " << newPlainText;

    // Verify original first span text is gone (assuming it was different)
    if (!spans[0].text.empty()) {
        // Convert u32string to string for comparison
        std::string originalFirstSpan;
        for (char32_t ch : spans[0].text) {
            if (ch < 128) originalFirstSpan += static_cast<char>(ch);
        }

        if (originalFirstSpan != "REPLACED") {
            EXPECT_EQ(newPlainText.find(originalFirstSpan), std::string::npos)
                << "Original first span text should be removed";
        }
    }
}

// Test 3: MultipleSpansEdited
// Edit two different spans with different replacement text and verify both appear.
TEST_F(TextEditPipelineTest, MultipleSpansEdited) {
    if (!multiSpanDoc_) {
        GTEST_SKIP() << "test_multispan.pdf not available";
    }

    auto page = multiSpanDoc_->page(0);
    ASSERT_NE(page, nullptr);

    auto spans = extractSpans(page.get());
    ASSERT_GE(spans.size(), 2u) << "Multi-span PDF should have at least 2 spans";

    // Apply edits: replace first two spans
    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, U"FIRST_EDIT"});
    modifications.push_back({1, U"SECOND_EDIT"});

    page = applyEditsOnDoc(multiSpanDoc_.get(), page.get(), spans, modifications);
    ASSERT_NE(page, nullptr);

    // Re-extract text
    pdfe::core::TextExtractor ext(context_);
    std::string newPlainText = ext.extractPlainText(page.get());

    // Verify both replacements appear
    EXPECT_NE(newPlainText.find("FIRST_EDIT"), std::string::npos)
        << "First edited text should appear. Got: " << newPlainText;
    EXPECT_NE(newPlainText.find("SECOND_EDIT"), std::string::npos)
        << "Second edited text should appear. Got: " << newPlainText;
}

// Test 4: EmptyReplacementRemovesText
// Replace a span with empty string and verify original text no longer appears.
TEST_F(TextEditPipelineTest, EmptyReplacementRemovesText) {
    // Extract initial page
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Extract text spans
    auto spans = extractSpans(page.get());
    ASSERT_GT(spans.size(), 0) << "Test PDF should have at least one text span";

    // Record original first span text
    std::u32string originalText = spans[0].text;
    EXPECT_GT(originalText.length(), 0) << "First span should have text to remove";

    // Convert to ASCII string for later verification
    std::string originalTextAscii;
    for (char32_t ch : originalText) {
        if (ch < 128) originalTextAscii += static_cast<char>(ch);
    }

    // Get original full text
    std::string originalPlainText = extractPlainText(page.get());

    // Apply edit: replace first span with empty string
    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, U""});

    page = applyEditsAndReload(page.get(), spans, modifications);
    ASSERT_NE(page, nullptr);

    // Re-extract text
    std::string newPlainText = extractPlainText(page.get());

    // Verify the original first span text no longer appears
    // (unless it appears elsewhere on the page)
    if (!originalTextAscii.empty() && originalPlainText.find(originalTextAscii) != std::string::npos) {
        // Original text was in the document
        // After removing first span, it should either be gone or appear less frequently
        size_t originalCount = 0;
        size_t newCount = 0;
        size_t pos = 0;

        while ((pos = originalPlainText.find(originalTextAscii, pos)) != std::string::npos) {
            originalCount++;
            pos += originalTextAscii.length();
        }

        pos = 0;
        while ((pos = newPlainText.find(originalTextAscii, pos)) != std::string::npos) {
            newCount++;
            pos += originalTextAscii.length();
        }

        EXPECT_LT(newCount, originalCount)
            << "Empty replacement should reduce occurrences of original text";
    }
}

// Test 5: TextPositionPreserved
// Replace text with same-length text and verify bounding box position is preserved.
TEST_F(TextEditPipelineTest, TextPositionPreserved) {
    // Extract initial page
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Extract text spans
    auto spans = extractSpans(page.get());
    ASSERT_GT(spans.size(), 0) << "Test PDF should have at least one text span";

    // Record original bounding box
    QRectF originalBBox = spans[0].boundingBox;
    EXPECT_GT(originalBBox.width(), 0) << "Original span should have width";
    EXPECT_GT(originalBBox.height(), 0) << "Original span should have height";

    // Get original text length
    size_t originalLength = spans[0].text.length();
    EXPECT_GT(originalLength, 0) << "Original span should have text";

    // Create replacement text of same length
    std::u32string replacementText;
    for (size_t i = 0; i < originalLength; ++i) {
        replacementText += U'X';  // Use 'X' characters
    }

    // Apply edit
    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, replacementText});

    page = applyEditsAndReload(page.get(), spans, modifications);
    ASSERT_NE(page, nullptr);

    // Re-extract spans
    auto newSpans = extractSpans(page.get());
    ASSERT_GT(newSpans.size(), 0) << "Re-extraction should find spans";

    // Find the span at approximately the same position
    // (it should be the first span if order is preserved)
    QRectF newBBox = newSpans[0].boundingBox;

    // Verify position is preserved within tolerance (5pt)
    const double tolerance = 5.0;

    EXPECT_NEAR(newBBox.x(), originalBBox.x(), tolerance)
        << "X position should be preserved within 5pt";
    EXPECT_NEAR(newBBox.y(), originalBBox.y(), tolerance)
        << "Y position should be preserved within 5pt";

    // Width might vary slightly due to font metrics, but should be close
    EXPECT_NEAR(newBBox.width(), originalBBox.width(), originalBBox.width() * 0.5)
        << "Width should be approximately preserved (within 50%)";

    // Height should be very close (same font size)
    EXPECT_NEAR(newBBox.height(), originalBBox.height(), tolerance)
        << "Height should be preserved within 5pt";
}

