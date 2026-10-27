#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/SearchEngine.h"
#include "core/SearchResult.h"
#include "core/MuPdfContext.h"
#include "core/Error.h"
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;

class SearchTest : public ::testing::Test {
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

TEST_F(SearchTest, BasicSearch) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    SearchEngine engine(document.get());
    SearchResult result = engine.search("test");

    // Query should be stored
    EXPECT_EQ(result.query, "test");

    // A3-16: this used to be `EXPECT_GE(result.totalMatches(), 0)` — the count is
    // a vector size, so the assertion could not fail and the whole test proved
    // only that search returned. The behavioural contract is instead: a token the
    // fixture DOES contain is found, a token it does not is not, and every hit
    // carries a usable highlight quad on a real page.
    ASSERT_GT(result.totalMatches(), 0)
        << "test_3page.pdf contains the word 'test'";
    for (const auto& hit : result.hits) {
        EXPECT_GE(hit.pageIndex, 0);
        EXPECT_LT(hit.pageIndex, document->pageCount());
        EXPECT_FALSE(hit.quads.empty()) << "hit without a highlight quad";
    }
    EXPECT_EQ(engine.search("zzzznotinthisdocumentzzzz").totalMatches(), 0)
        << "an absent token must produce no hits";
}

TEST_F(SearchTest, SearchWithCaseSensitive) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());

    // Case-insensitive search (default)
    SearchResult result1 = engine.search("test", SearchFlags::None);

    // Case-sensitive search
    SearchResult result2 = engine.search("test", SearchFlags::CaseSensitive);

    EXPECT_FALSE(result1.caseSensitive) << "Case-insensitive by default";
    EXPECT_TRUE(result2.caseSensitive) << "Case-sensitive flag stored correctly";

    // A3-16 / A3-56 (`partial-contract-assert`): asserting only that the FLAG was
    // stored is what let the flag be ignored by the engine for the whole life of
    // the feature. The behaviour is what matters — a case-sensitive search must
    // be a SUBSET of the case-insensitive one, and a casing that does not occur
    // in the document must find nothing at all.
    EXPECT_LE(result2.totalMatches(), result1.totalMatches())
        << "case-sensitive results must be a subset of case-insensitive results";
    const SearchResult impossible =
        engine.search("tEsT", SearchFlags::CaseSensitive);
    EXPECT_EQ(impossible.totalMatches(), 0)
        << "the mixed casing 'tEsT' does not occur in test_3page.pdf, so a "
           "case-SENSITIVE search for it must find nothing";
    EXPECT_GT(engine.search("tEsT", SearchFlags::None).totalMatches(), 0)
        << "...while the case-INSENSITIVE search for the same string still hits";
}

TEST_F(SearchTest, SearchResultHitStructure) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());
    SearchResult result = engine.search("test");

    // Check hit structure
    for (const auto& hit : result.hits) {
        // Page index should be valid
        EXPECT_GE(hit.pageIndex, 0);
        EXPECT_LT(hit.pageIndex, document->pageCount());

        // Should have at least one quad
        EXPECT_FALSE(hit.quads.empty()) << "Hit should have highlight rectangles";

        // Quads should have positive dimensions
        for (const auto& quad : hit.quads) {
            EXPECT_GT(quad.width(), 0.0) << "Quad width should be positive";
            EXPECT_GT(quad.height(), 0.0) << "Quad height should be positive";
        }

        // Context should not be empty
        EXPECT_FALSE(hit.context.empty()) << "Hit should have context text";

        // Match bounds should be valid
        EXPECT_LE(hit.matchStart, hit.matchEnd);
    }
}

TEST_F(SearchTest, EmptyQuery) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());
    SearchResult result = engine.search("");

    // Empty query should return no results
    EXPECT_EQ(result.totalMatches(), 0);
    EXPECT_TRUE(result.hits.empty());
}

TEST_F(SearchTest, SearchNonExistentText) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());

    // Search for text that definitely doesn't exist
    SearchResult result = engine.search("xyzabc123notfound");

    // Should complete without error
    EXPECT_EQ(result.totalMatches(), 0);
    EXPECT_TRUE(result.hits.empty());
}

TEST_F(SearchTest, SearchEngineNullDocument) {
    // Creating SearchEngine with null document should throw
    EXPECT_THROW(SearchEngine engine(nullptr), PdfeException);
}

TEST_F(SearchTest, MultiPageSearch) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    if (document->pageCount() < 2) {
        GTEST_SKIP() << "Need at least 2 pages for multi-page test";
    }

    SearchEngine engine(document.get());
    SearchResult result = engine.search("test");

    // If we find results, check they're on valid pages
    for (const auto& hit : result.hits) {
        EXPECT_GE(hit.pageIndex, 0);
        EXPECT_LT(hit.pageIndex, document->pageCount());
    }
}

TEST_F(SearchTest, SearchFlags) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());

    // Test flag combinations
    SearchFlags flags1 = SearchFlags::CaseSensitive | SearchFlags::WholeWord;
    EXPECT_TRUE(hasFlag(flags1, SearchFlags::CaseSensitive));
    EXPECT_TRUE(hasFlag(flags1, SearchFlags::WholeWord));
    EXPECT_FALSE(hasFlag(flags1, SearchFlags::UseRegex));

    // Search with combined flags should not crash
    EXPECT_NO_THROW({
        SearchResult result = engine.search("test", flags1);
    });
}

TEST_F(SearchTest, TotalMatchesCount) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());
    SearchResult result = engine.search("test");

    // totalMatches() should match hits.size()
    EXPECT_EQ(result.totalMatches(), static_cast<int>(result.hits.size()));
}

TEST_F(SearchTest, SearchContextExtraction) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());
    SearchResult result = engine.search("test");

    // Each hit should have context
    for (const auto& hit : result.hits) {
        EXPECT_FALSE(hit.context.empty());

        // Context should be reasonably sized (not too short, not too long)
        EXPECT_GT(hit.context.size(), 3) << "Context should have meaningful content";
        EXPECT_LT(hit.context.size(), 200) << "Context should not be excessively long";
    }
}

TEST_F(SearchTest, SearchPreservesQuery) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());

    std::string query = "test query";
    SearchResult result = engine.search(query);

    EXPECT_EQ(result.query, query);
}

TEST_F(SearchTest, CancelSearchBeforeStart) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());

    // Cancelling before search should not crash
    EXPECT_NO_THROW(engine.cancelSearch());
}

TEST_F(SearchTest, QuadConversion) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    SearchEngine engine(document.get());
    SearchResult result = engine.search("test");

    // Check that quads are properly converted to QRectF
    for (const auto& hit : result.hits) {
        for (const auto& quad : hit.quads) {
            // Should have valid bounds
            EXPECT_TRUE(quad.isValid() || quad.width() >= 0);
            EXPECT_GE(quad.x(), 0.0) << "X coordinate should be non-negative";
            EXPECT_GE(quad.y(), 0.0) << "Y coordinate should be non-negative";
        }
    }
}
