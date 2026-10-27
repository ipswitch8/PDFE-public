#include <gtest/gtest.h>
#include "fonts/PdfFontEmbedder.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <filesystem>
#include <memory>

using namespace pdfe::fonts;
using namespace pdfe::core;

class PdfFontEmbedderTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        embedder_ = std::make_unique<PdfFontEmbedder>(context_.get());

        std::filesystem::path testPdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(testPdf)) {
            hasTestPdf_ = false;
            return;
        }
        doc_ = Document::open(context_, testPdf);
        if (doc_ && doc_->pageCount() > 0) {
            page_ = doc_->page(0);
            hasTestPdf_ = true;
        }
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<PdfFontEmbedder> embedder_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    bool hasTestPdf_ = false;
};

// Test 1: Embed Arial returns resource name starting with "Fsub"
TEST_F(PdfFontEmbedderTest, EmbedArialReturnsResourceName) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::filesystem::path arialPath = "C:\\Windows\\Fonts\\arial.ttf";
    if (!std::filesystem::exists(arialPath)) {
        GTEST_SKIP() << "Arial font not available on system";
    }

    auto result = embedder_->embedFont(page_.get(), doc_.get(), arialPath.string());

    ASSERT_TRUE(result.has_value()) << "embedFont should succeed with valid Arial path";
    EXPECT_FALSE(result->resourceName.empty()) << "Resource name should not be empty";
    EXPECT_TRUE(result->resourceName.starts_with("Fsub"))
        << "Resource name should start with 'Fsub', got: " << result->resourceName;
}

// Test 2: Embed multiple fonts gets different resource names
TEST_F(PdfFontEmbedderTest, EmbedMultipleFontsGetsDifferentNames) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::filesystem::path arialPath = "C:\\Windows\\Fonts\\arial.ttf";
    std::filesystem::path timesPath = "C:\\Windows\\Fonts\\times.ttf";

    if (!std::filesystem::exists(arialPath)) {
        GTEST_SKIP() << "Arial font not available on system";
    }
    if (!std::filesystem::exists(timesPath)) {
        GTEST_SKIP() << "Times font not available on system";
    }

    auto result1 = embedder_->embedFont(page_.get(), doc_.get(), arialPath.string());
    auto result2 = embedder_->embedFont(page_.get(), doc_.get(), timesPath.string());

    ASSERT_TRUE(result1.has_value()) << "First embedFont should succeed";
    ASSERT_TRUE(result2.has_value()) << "Second embedFont should succeed";
    EXPECT_NE(result1->resourceName, result2->resourceName)
        << "Different fonts should get different resource names";
}

// Test 3: Invalid path returns nullopt
TEST_F(PdfFontEmbedderTest, InvalidPathReturnsNullopt) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::string invalidPath = "C:\\nonexistent\\font.ttf";
    auto result = embedder_->embedFont(page_.get(), doc_.get(), invalidPath);

    EXPECT_FALSE(result.has_value()) << "embedFont with invalid path should return nullopt";
}

// Test 4: Null page returns nullopt
TEST_F(PdfFontEmbedderTest, NullPageReturnsNullopt) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::filesystem::path arialPath = "C:\\Windows\\Fonts\\arial.ttf";
    if (!std::filesystem::exists(arialPath)) {
        GTEST_SKIP() << "Arial font not available on system";
    }

    auto result = embedder_->embedFont(nullptr, doc_.get(), arialPath.string());

    EXPECT_FALSE(result.has_value()) << "embedFont with null page should return nullopt";
}

// Test 5: Null document returns nullopt
TEST_F(PdfFontEmbedderTest, NullDocReturnsNullopt) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::filesystem::path arialPath = "C:\\Windows\\Fonts\\arial.ttf";
    if (!std::filesystem::exists(arialPath)) {
        GTEST_SKIP() << "Arial font not available on system";
    }

    auto result = embedder_->embedFont(page_.get(), nullptr, arialPath.string());

    EXPECT_FALSE(result.has_value()) << "embedFont with null document should return nullopt";
}

// Test 6: Empty path returns nullopt
TEST_F(PdfFontEmbedderTest, EmptyPathReturnsNullopt) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    auto result = embedder_->embedFont(page_.get(), doc_.get(), "");

    EXPECT_FALSE(result.has_value()) << "embedFont with empty path should return nullopt";
}
