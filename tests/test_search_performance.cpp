#include <gtest/gtest.h>
#include "core/SearchEngine.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/SearchResult.h"
#include "core/Error.h"
#include <memory>
#include <filesystem>
#include <string>
#include <chrono>

using namespace pdfe::core;

/// Test fixture for search performance verification.
/// These tests verify that search works correctly after the reserve() optimization.
class SearchPerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_single.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
        }
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
};

TEST_F(SearchPerformanceTest, SearchPage_FindsText) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    SearchEngine engine(document.get());

    // A3-16: the cascade of "if that word isn't there try another" ending in
    // `EXPECT_GE(totalMatches(), 0)` asserted nothing at all. test_single.pdf is a
    // committed, pinned fixture, so the token it contains is knowable and is
    // asserted directly.
    const SearchResult result = engine.search("test");
    ASSERT_GT(result.totalMatches(), 0)
        << "test_single.pdf contains the word 'test'";
    EXPECT_EQ(result.query, "test");
    for (const auto& hit : result.hits)
        EXPECT_FALSE(hit.quads.empty()) << "hit without a highlight quad";

    // The half of A3-16 that says a file named "performance" asserted no budget.
    // This is a wall-clock CEILING, not a benchmark: the measured Debug cost of
    // 100 searches of this one-page fixture is a few milliseconds, so the ceiling
    // cannot flake on a loaded runner, but a hang, a per-hit document reopen or an
    // accidental quadratic rescan blows straight through it.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        const SearchResult r = engine.search("test");
        ASSERT_EQ(r.totalMatches(), result.totalMatches())
            << "repeat " << i << " returned a different count — search is not "
               "deterministic over an unchanged document";
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 15000) << "100 searches of a 1-page document took " << ms
                         << " ms";
}

TEST_F(SearchPerformanceTest, SearchPage_NotFound) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    SearchEngine engine(document.get());

    // Search for text that definitely doesn't exist
    SearchResult result = engine.search("xyznonexistent12345");

    // Should return empty results
    EXPECT_EQ(result.totalMatches(), 0);
    EXPECT_TRUE(result.hits.empty());
    EXPECT_EQ(result.query, "xyznonexistent12345");
}

TEST_F(SearchPerformanceTest, SearchPage_QuadsNotEmpty) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    SearchEngine engine(document.get());

    // A3-16: the "try another word" cascade made the loop below vacuous whenever
    // every candidate missed — a for-loop over an empty vector asserts nothing.
    // The fixture authors "Single Page Test Document", so the token is known.
    SearchResult result = engine.search("Document");
    ASSERT_GT(result.totalMatches(), 0)
        << "test_single.pdf authors the word 'Document'; without at least one hit "
           "the per-quad checks below would be vacuous";

    for (const auto& hit : result.hits) {
        // Each hit should have at least one quad (bounding rectangle)
        EXPECT_FALSE(hit.quads.empty()) << "Search hit should have non-empty quads vector";

        // Verify quads have valid dimensions
        for (const auto& quad : hit.quads) {
            EXPECT_GT(quad.width(), 0.0) << "Quad width should be positive";
            EXPECT_GT(quad.height(), 0.0) << "Quad height should be positive";
        }
    }
}

TEST_F(SearchPerformanceTest, SearchPage_CaseInsensitive) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    SearchEngine engine(document.get());

    // Search for a word in lowercase (case-insensitive by default)
    SearchResult resultLower = engine.search("test", SearchFlags::None);

    // Search for the same word in uppercase (still case-insensitive)
    SearchResult resultUpper = engine.search("TEST", SearchFlags::None);

    // Both should be case-insensitive
    EXPECT_FALSE(resultLower.caseSensitive);
    EXPECT_FALSE(resultUpper.caseSensitive);

    // A3-16: `EXPECT_GE(totalMatches(), 0)` could not fail. The actual contract of
    // a case-INSENSITIVE search is that the casing of the QUERY is irrelevant, so
    // the two searches above must agree exactly.
    ASSERT_GT(resultLower.totalMatches(), 0)
        << "test_single.pdf contains the word 'test'";
    EXPECT_EQ(resultUpper.totalMatches(), resultLower.totalMatches())
        << "a case-insensitive search must not depend on the query's casing";

    // And the case-SENSITIVE search must actually behave differently. The fixture
    // authors the string "Single Page Test Document", so the only casing that may
    // match under the flag is "Test".
    SearchResult resultCaseSensitive = engine.search("Test", SearchFlags::CaseSensitive);
    EXPECT_TRUE(resultCaseSensitive.caseSensitive);
    EXPECT_GT(resultCaseSensitive.totalMatches(), 0)
        << "the authored casing 'Test' must match under the case-sensitive flag";
    EXPECT_EQ(engine.search("test", SearchFlags::CaseSensitive).totalMatches(), 0)
        << "all-lowercase 'test' does not occur in 'Single Page Test Document', "
           "so a case-sensitive search must find nothing";
    EXPECT_EQ(engine.search("TEST", SearchFlags::CaseSensitive).totalMatches(), 0)
        << "nor does all-uppercase 'TEST'";
}
