#include <gtest/gtest.h>
#include "core/PageOperations.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Error.h"
#include <filesystem>
#include <memory>

using namespace pdfe::core;

class PageOperationsTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        // Set up test PDF path
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        // Open test document if it exists
        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
        }

        // Set up output path in temp directory
        outputPath = std::filesystem::temp_directory_path() / "pdfe_test_extract.pdf";
    }

    void TearDown() override {
        // Clean up output file if it exists
        if (std::filesystem::exists(outputPath)) {
            std::filesystem::remove(outputPath);
        }
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::filesystem::path outputPath;
};

TEST_F(PageOperationsTest, ExtractPages_SinglePage) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Extract only the first page (index 0)
    std::vector<int> pagesToExtract = {0};

    extractPagesToFile(context, document.get(), pagesToExtract, outputPath);

    // Verify the output file was created
    ASSERT_TRUE(std::filesystem::exists(outputPath));

    // Open the extracted document and verify it has 1 page
    auto extractedDoc = Document::open(context, outputPath);
    ASSERT_NE(extractedDoc, nullptr);
    EXPECT_EQ(extractedDoc->pageCount(), 1);
}

TEST_F(PageOperationsTest, ExtractPages_Multiple) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Extract pages 0 and 2 (1st and 3rd pages)
    std::vector<int> pagesToExtract = {0, 2};

    extractPagesToFile(context, document.get(), pagesToExtract, outputPath);

    // Verify the output file was created
    ASSERT_TRUE(std::filesystem::exists(outputPath));

    // Open the extracted document and verify it has 2 pages
    auto extractedDoc = Document::open(context, outputPath);
    ASSERT_NE(extractedDoc, nullptr);
    EXPECT_EQ(extractedDoc->pageCount(), 2);
}

TEST_F(PageOperationsTest, ExtractPages_All) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Extract all 3 pages
    std::vector<int> pagesToExtract = {0, 1, 2};

    extractPagesToFile(context, document.get(), pagesToExtract, outputPath);

    // Verify the output file was created
    ASSERT_TRUE(std::filesystem::exists(outputPath));

    // Open the extracted document and verify it has 3 pages
    auto extractedDoc = Document::open(context, outputPath);
    ASSERT_NE(extractedDoc, nullptr);
    EXPECT_EQ(extractedDoc->pageCount(), 3);
}

TEST_F(PageOperationsTest, ExtractPages_OutputFileCreated) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Ensure output file doesn't exist before extraction
    ASSERT_FALSE(std::filesystem::exists(outputPath));

    // Extract a single page
    std::vector<int> pagesToExtract = {1};
    extractPagesToFile(context, document.get(), pagesToExtract, outputPath);

    // Verify the output file now exists and has non-zero size
    ASSERT_TRUE(std::filesystem::exists(outputPath));
    EXPECT_GT(std::filesystem::file_size(outputPath), 0);
}

TEST_F(PageOperationsTest, ExtractPages_InvalidIndex) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Try to extract with out-of-range index (document has 3 pages: 0, 1, 2)
    std::vector<int> pagesToExtract = {5};

    EXPECT_THROW(
        extractPagesToFile(context, document.get(), pagesToExtract, outputPath),
        PdfeException
    );
}

TEST_F(PageOperationsTest, ExtractPages_NullContext) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    std::vector<int> pagesToExtract = {0};

    EXPECT_THROW(
        extractPagesToFile(nullptr, document.get(), pagesToExtract, outputPath),
        PdfeException
    );
}

TEST_F(PageOperationsTest, ExtractPages_NullDocument) {
    std::vector<int> pagesToExtract = {0};

    EXPECT_THROW(
        extractPagesToFile(context, nullptr, pagesToExtract, outputPath),
        PdfeException
    );
}

TEST_F(PageOperationsTest, ExtractPages_EmptyPageList) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    std::vector<int> pagesToExtract; // Empty list

    EXPECT_THROW(
        extractPagesToFile(context, document.get(), pagesToExtract, outputPath),
        PdfeException
    );
}

TEST_F(PageOperationsTest, ExtractPages_NegativeIndex) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    std::vector<int> pagesToExtract = {-1};

    EXPECT_THROW(
        extractPagesToFile(context, document.get(), pagesToExtract, outputPath),
        PdfeException
    );
}

TEST_F(PageOperationsTest, ExtractPages_ReverseOrder) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Extract pages in reverse order (3rd, 2nd, 1st)
    std::vector<int> pagesToExtract = {2, 1, 0};

    extractPagesToFile(context, document.get(), pagesToExtract, outputPath);

    // Verify the output file was created
    ASSERT_TRUE(std::filesystem::exists(outputPath));

    // Open the extracted document and verify it has 3 pages
    auto extractedDoc = Document::open(context, outputPath);
    ASSERT_NE(extractedDoc, nullptr);
    EXPECT_EQ(extractedDoc->pageCount(), 3);
}
