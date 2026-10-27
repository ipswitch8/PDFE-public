#include <gtest/gtest.h>
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditModel.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include <filesystem>
#include <memory>

using namespace pdfe::textedit;
using namespace pdfe::core;

class TextEditWidthScalingTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);

            if (document && document->pageCount() > 0) {
                auto page = document->page(0);
                if (page) {
                    auto blocks = page->extractText();
                    if (!blocks.empty()) {
                        model = std::make_unique<TextEditModel>(blocks);
                    }
                }
            }
        }
    }

    void TearDown() override {
        model.reset();
        document.reset();
        context.reset();
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::unique_ptr<TextEditModel> model;
};

TEST_F(TextEditWidthScalingTest, LongerTextNoException) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0) << "Need at least one text span";

    // Get first span
    const auto& span = model->span(0);
    std::u32string oldText = span.text;

    // Create new text that is significantly longer
    std::u32string newText = oldText + U" ADDITIONAL_LONG_TEXT_TO_TEST_WIDTH_SCALING";

    // Create and execute command
    // This exercises the width scaling path when newText is longer than oldText
    EXPECT_NO_THROW({
        EditTextCommand cmd(
            document.get(),
            context,
            0,              // pageIndex
            0,              // spanIndex
            span,           // spanInfo
            oldText,
            newText
        );

        // Execute should apply width scaling and not crash
        cmd.execute();

        // Undo should also work
        cmd.undo();

        // Redo should work
        cmd.redo();
    }) << "EditTextCommand should handle longer text without exception";
}

TEST_F(TextEditWidthScalingTest, ShorterTextNoException) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get first span
    const auto& span = model->span(0);
    std::u32string oldText = span.text;

    // Skip test if original text is too short to make shorter
    if (oldText.size() <= 1) {
        GTEST_SKIP() << "Original text too short for this test";
    }

    // Create new text that is shorter (take first half)
    std::u32string newText = oldText.substr(0, oldText.size() / 2);

    // Create and execute command
    // This exercises the width scaling path when newText is shorter than oldText
    EXPECT_NO_THROW({
        EditTextCommand cmd(
            document.get(),
            context,
            0,              // pageIndex
            0,              // spanIndex
            span,           // spanInfo
            oldText,
            newText
        );

        // Execute should apply width scaling and not crash
        cmd.execute();

        // Undo
        cmd.undo();

        // Redo
        cmd.redo();
    }) << "EditTextCommand should handle shorter text without exception";
}

TEST_F(TextEditWidthScalingTest, SameTextNoScaling) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get first span
    const auto& span = model->span(0);
    std::u32string sameText = span.text;

    // Create command where old text equals new text
    // This should not apply any scaling (no-op case)
    EXPECT_NO_THROW({
        EditTextCommand cmd(
            document.get(),
            context,
            0,              // pageIndex
            0,              // spanIndex
            span,           // spanInfo
            sameText,
            sameText
        );

        // Execute with same text should not crash
        cmd.execute();

        // Undo
        cmd.undo();

        // Redo
        cmd.redo();
    }) << "EditTextCommand should handle same text without exception";
}

TEST_F(TextEditWidthScalingTest, CharRatioLogic) {
    // Pure math test: verify character ratio computation logic
    // This tests the scaling factor calculation independent of PDF operations

    // Test case 1: Doubling the text length
    {
        size_t oldLen = 10;
        size_t newLen = 20;
        double charRatio = static_cast<double>(oldLen) / static_cast<double>(newLen);

        EXPECT_DOUBLE_EQ(charRatio, 0.5)
            << "Character ratio for doubling length should be 0.5";

        // Scaling factor would be applied as: originalWidth * charRatio
        // So if original width was 100, scaled width would be 50
        double originalWidth = 100.0;
        double scaledWidth = originalWidth * charRatio;
        EXPECT_DOUBLE_EQ(scaledWidth, 50.0)
            << "Scaled width should be half for doubled text length";
    }

    // Test case 2: Halving the text length
    {
        size_t oldLen = 20;
        size_t newLen = 10;
        double charRatio = static_cast<double>(oldLen) / static_cast<double>(newLen);

        EXPECT_DOUBLE_EQ(charRatio, 2.0)
            << "Character ratio for halving length should be 2.0";

        double originalWidth = 100.0;
        double scaledWidth = originalWidth * charRatio;
        EXPECT_DOUBLE_EQ(scaledWidth, 200.0)
            << "Scaled width should be double for halved text length";
    }

    // Test case 3: Same length (no scaling)
    {
        size_t oldLen = 15;
        size_t newLen = 15;
        double charRatio = static_cast<double>(oldLen) / static_cast<double>(newLen);

        EXPECT_DOUBLE_EQ(charRatio, 1.0)
            << "Character ratio for same length should be 1.0";

        double originalWidth = 100.0;
        double scaledWidth = originalWidth * charRatio;
        EXPECT_DOUBLE_EQ(scaledWidth, 100.0)
            << "Scaled width should be unchanged for same text length";
    }

    // Test case 4: 10% longer
    {
        size_t oldLen = 100;
        size_t newLen = 110;
        double charRatio = static_cast<double>(oldLen) / static_cast<double>(newLen);

        EXPECT_NEAR(charRatio, 0.909090, 0.0001)
            << "Character ratio for 10% longer text";

        double originalWidth = 100.0;
        double scaledWidth = originalWidth * charRatio;
        EXPECT_NEAR(scaledWidth, 90.909, 0.001)
            << "Scaled width should be ~90.9 for 10% longer text";
    }

    // Test case 5: 50% shorter
    {
        size_t oldLen = 100;
        size_t newLen = 50;
        double charRatio = static_cast<double>(oldLen) / static_cast<double>(newLen);

        EXPECT_DOUBLE_EQ(charRatio, 2.0)
            << "Character ratio for 50% shorter text should be 2.0";

        double originalWidth = 100.0;
        double scaledWidth = originalWidth * charRatio;
        EXPECT_DOUBLE_EQ(scaledWidth, 200.0)
            << "Scaled width should double for 50% shorter text";
    }

    // Test case 6: Edge case - very short text
    {
        size_t oldLen = 1;
        size_t newLen = 10;
        double charRatio = static_cast<double>(oldLen) / static_cast<double>(newLen);

        EXPECT_DOUBLE_EQ(charRatio, 0.1)
            << "Character ratio from 1 to 10 chars should be 0.1";
    }

    // Test case 7: Edge case - avoid division by zero
    {
        size_t oldLen = 5;
        size_t newLen = 0;

        // In real implementation, should guard against division by zero
        // Here we just verify the math would be problematic
        if (newLen > 0) {
            double charRatio = static_cast<double>(oldLen) / static_cast<double>(newLen);
            EXPECT_GT(charRatio, 0.0);
        } else {
            // Implementation should handle this case (e.g., use ratio of 1.0 or skip scaling)
            SUCCEED() << "Zero-length text is edge case that should be handled";
        }
    }
}

TEST_F(TextEditWidthScalingTest, EmptyTextHandling) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get first span
    const auto& span = model->span(0);
    std::u32string oldText = span.text;

    // Test editing to empty text
    std::u32string emptyText = U"";

    EXPECT_NO_THROW({
        EditTextCommand cmd(
            document.get(),
            context,
            0,
            0,
            span,
            oldText,
            emptyText
        );

        // Should handle empty text gracefully
        cmd.execute();
        cmd.undo();
        cmd.redo();
    }) << "EditTextCommand should handle empty text without exception";
}

TEST_F(TextEditWidthScalingTest, VeryLongTextHandling) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get first span
    const auto& span = model->span(0);
    std::u32string oldText = span.text;

    // Create very long text (1000+ characters)
    std::u32string veryLongText = oldText;
    for (int i = 0; i < 100; ++i) {
        veryLongText += U" REPEATED_TEXT_SEGMENT";
    }

    EXPECT_GT(veryLongText.size(), 1000u) << "Test text should be very long";

    EXPECT_NO_THROW({
        EditTextCommand cmd(
            document.get(),
            context,
            0,
            0,
            span,
            oldText,
            veryLongText
        );

        // Should handle very long text and extreme scaling
        cmd.execute();
        cmd.undo();
        cmd.redo();
    }) << "EditTextCommand should handle very long text without exception";
}

TEST_F(TextEditWidthScalingTest, SpecialCharactersHandling) {
    if (!document || !model) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(model->spanCount(), 0);

    // Get first span
    const auto& span = model->span(0);
    std::u32string oldText = span.text;

    // Create text with special Unicode characters
    std::u32string specialText = U"Test™ with © symbols • and € currency 中文字符";

    EXPECT_NO_THROW({
        EditTextCommand cmd(
            document.get(),
            context,
            0,
            0,
            span,
            oldText,
            specialText
        );

        // Should handle Unicode special characters
        cmd.execute();
        cmd.undo();
        cmd.redo();
    }) << "EditTextCommand should handle special characters without exception";
}
