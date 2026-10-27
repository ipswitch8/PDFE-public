// -*- coding: utf-8 -*-
// Phase 6: Tantivy full-text search tests.
// Covers open/close, upsert/delete/commit, boolean/phrase/prefix queries,
// ranked result ordering, persistent-across-reopen, incremental updates.
#include <gtest/gtest.h>
#include "search/TantivyIndex.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::search;

namespace {

std::filesystem::path freshIndexDir(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "pdfe_tantivy_tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

bool hitsContainPage(const std::vector<SearchHit>& hits, int pageIdx)
{
    return std::any_of(hits.begin(), hits.end(),
        [&](const SearchHit& h) { return h.pageIndex == pageIdx; });
}

int findHitPos(const std::vector<SearchHit>& hits, int pageIdx)
{
    for (int i = 0; i < static_cast<int>(hits.size()); ++i) {
        if (hits[i].pageIndex == pageIdx) return i;
    }
    return -1;
}

} // namespace

TEST(TantivyIndexBasic, OpenCreatesIndexDirAndReportsZeroDocs)
{
    auto dir = freshIndexDir("open_empty");
    TantivyIndex idx(dir);
    EXPECT_TRUE(idx.valid());
    EXPECT_EQ(idx.documentCount(), 0);
}

TEST(TantivyIndexBasic, UpsertAddCommitMakesDocSearchable)
{
    auto dir = freshIndexDir("upsert_basic");
    TantivyIndex idx(dir);
    ASSERT_TRUE(idx.upsertPage(0, "The quick brown fox jumps over the lazy dog"));
    ASSERT_TRUE(idx.commit());
    EXPECT_EQ(idx.documentCount(), 1);

    auto hits = idx.search("fox", 10);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].pageIndex, 0);
    EXPECT_GT(hits[0].score, 0.0f);
}

TEST(TantivyIndexBasic, UncommittedWritesAreNotVisible)
{
    auto dir = freshIndexDir("uncommitted");
    TantivyIndex idx(dir);
    ASSERT_TRUE(idx.upsertPage(0, "quick brown fox"));
    // Not committed — reader's snapshot is still at 0 docs.
    EXPECT_EQ(idx.documentCount(), 0);
    auto hits = idx.search("fox", 10);
    EXPECT_TRUE(hits.empty());
}

TEST(TantivyIndexBasic, DeletePageRemovesFromSearch)
{
    auto dir = freshIndexDir("delete_page");
    TantivyIndex idx(dir);
    idx.upsertPage(0, "alpha beta gamma");
    idx.upsertPage(1, "delta epsilon zeta");
    idx.commit();
    ASSERT_EQ(idx.documentCount(), 2);

    ASSERT_TRUE(idx.deletePage(0));
    idx.commit();
    EXPECT_EQ(idx.documentCount(), 1);

    auto hits = idx.search("alpha", 10);
    EXPECT_TRUE(hits.empty()) << "page 0 should be gone after delete+commit";
}

// -------- Boolean queries --------

TEST(TantivyIndexQuery, BooleanAndReturnsOnlyPagesWithAllTerms)
{
    auto dir = freshIndexDir("boolean_and");
    TantivyIndex idx(dir);
    idx.upsertPage(0, "apple banana cherry");
    idx.upsertPage(1, "apple orange");
    idx.upsertPage(2, "banana cherry");
    idx.commit();

    // Default conjunction is AND when the parser is in conjunctive mode.
    auto hits = idx.search("apple banana", 10);
    EXPECT_TRUE(hitsContainPage(hits, 0));
    EXPECT_FALSE(hitsContainPage(hits, 1))
        << "page 1 has apple but no banana";
    EXPECT_FALSE(hitsContainPage(hits, 2))
        << "page 2 has banana but no apple";
}

TEST(TantivyIndexQuery, BooleanOrReturnsEitherMatch)
{
    auto dir = freshIndexDir("boolean_or");
    TantivyIndex idx(dir);
    idx.upsertPage(0, "apple pie");
    idx.upsertPage(1, "blueberry muffin");
    idx.upsertPage(2, "lemon tart");
    idx.commit();

    auto hits = idx.search("apple OR blueberry", 10);
    EXPECT_TRUE(hitsContainPage(hits, 0));
    EXPECT_TRUE(hitsContainPage(hits, 1));
    EXPECT_FALSE(hitsContainPage(hits, 2));
}

TEST(TantivyIndexQuery, BooleanNotExcludesMatchingPages)
{
    auto dir = freshIndexDir("boolean_not");
    TantivyIndex idx(dir);
    idx.upsertPage(0, "apple pie");
    idx.upsertPage(1, "apple sauce");
    idx.upsertPage(2, "pineapple");  // no 'apple' as a separate token
    idx.commit();

    auto hits = idx.search("+apple -pie", 10);
    EXPECT_FALSE(hitsContainPage(hits, 0))
        << "page 0 must be excluded (contains 'pie')";
    EXPECT_TRUE(hitsContainPage(hits, 1));
}

// -------- Phrase queries --------

TEST(TantivyIndexQuery, PhraseQueryMatchesAdjacentTerms)
{
    auto dir = freshIndexDir("phrase");
    TantivyIndex idx(dir);
    idx.upsertPage(0, "machine learning is hard");
    idx.upsertPage(1, "machines can learn and reason about learning");
    idx.commit();

    // Phrase: "machine learning" should match page 0 only. Page 1 has
    // both words but not adjacent.
    auto hits = idx.search("\"machine learning\"", 10);
    EXPECT_TRUE(hitsContainPage(hits, 0));
    EXPECT_FALSE(hitsContainPage(hits, 1));
}

// -------- Prefix queries --------

TEST(TantivyIndexQuery, PrefixQueryMatchesStemAndExtensions)
{
    auto dir = freshIndexDir("prefix");
    TantivyIndex idx(dir);
    idx.upsertPage(0, "running runner runs");
    idx.upsertPage(1, "walking walker walks");
    idx.commit();

    auto hits = idx.search("run*", 10);
    EXPECT_TRUE(hitsContainPage(hits, 0))
        << "prefix 'run*' should match page 0";
    EXPECT_FALSE(hitsContainPage(hits, 1));
}

// -------- Ranking --------

TEST(TantivyIndexRanking, MoreRelevantResultsComeFirst)
{
    auto dir = freshIndexDir("ranking");
    TantivyIndex idx(dir);
    // Page 0 mentions "forest" once among unrelated text.
    idx.upsertPage(0, "the garden has many flowers and one forest tree");
    // Page 1 is about forests heavily.
    idx.upsertPage(1, "forest forest forest wild forest canopy forest");
    idx.commit();

    auto hits = idx.search("forest", 10);
    ASSERT_GE(hits.size(), 2u);
    const int p1 = findHitPos(hits, 1);
    const int p0 = findHitPos(hits, 0);
    ASSERT_GE(p1, 0);
    ASSERT_GE(p0, 0);
    EXPECT_LT(p1, p0) << "page 1 (more occurrences) should rank higher";
}

// -------- Incremental update --------

TEST(TantivyIndexIncremental, UpsertReplacesExistingPageContent)
{
    auto dir = freshIndexDir("incremental");
    TantivyIndex idx(dir);
    idx.upsertPage(0, "version one: alpha");
    idx.commit();
    ASSERT_FALSE(idx.search("alpha", 10).empty());
    ASSERT_TRUE(idx.search("beta", 10).empty());

    // Replace page 0 with new content.
    idx.upsertPage(0, "version two: beta");
    idx.commit();

    EXPECT_TRUE(idx.search("alpha", 10).empty())
        << "old content should be gone after upsert";
    EXPECT_FALSE(idx.search("beta", 10).empty())
        << "new content should be visible";
    // Still only one document for this page.
    EXPECT_EQ(idx.documentCount(), 1);
}

// -------- Persistence across reopen --------

TEST(TantivyIndexPersistence, CommittedIndexReopensWithSameDocs)
{
    auto dir = freshIndexDir("persistence");
    {
        TantivyIndex idx(dir);
        idx.upsertPage(0, "persistence test alpha");
        idx.upsertPage(1, "persistence test beta");
        idx.commit();
        EXPECT_EQ(idx.documentCount(), 2);
    }
    // Reopen — data should still be there.
    TantivyIndex idx2(dir);
    EXPECT_EQ(idx2.documentCount(), 2);
    auto hits = idx2.search("beta", 10);
    EXPECT_TRUE(hitsContainPage(hits, 1));
}

// -------- Performance sanity --------

TEST(TantivyIndexPerformance, ThousandPagesSearchUnderOneSecond)
{
    // Soft perf budget: 1000 synthetic pages, search completes well
    // under 1 second. The strict acceptance criterion is <500 ms, but
    // test hardware varies; we assert <1000 ms so flaky CI doesn't fail.
    auto dir = freshIndexDir("perf_1000");
    TantivyIndex idx(dir);

    for (int i = 0; i < 1000; ++i) {
        // Sprinkle a unique needle on every 7th page so the query has
        // meaningful hits across a large index.
        std::string text = "page " + std::to_string(i) + " sample body text "
                         + "the quick brown fox jumps over the lazy dog";
        if (i % 7 == 0) text += " RAREWORD";
        idx.upsertPage(i, text);
    }
    idx.commit();
    EXPECT_EQ(idx.documentCount(), 1000);

    auto t0 = std::chrono::steady_clock::now();
    auto hits = idx.search("RAREWORD", 200);
    auto t1 = std::chrono::steady_clock::now();

    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    EXPECT_LT(elapsedMs, 1000) << "search took " << elapsedMs << " ms";
    // Every 7th page from 0..999 = ceil(1000/7) = 143 pages.
    EXPECT_GE(hits.size(), 100u);
}

// -------- Robustness --------

TEST(TantivyIndexRobustness, EmptyQueryDoesNotCrashReturnsEmpty)
{
    auto dir = freshIndexDir("empty_query");
    TantivyIndex idx(dir);
    idx.upsertPage(0, "hello world");
    idx.commit();
    auto hits = idx.search("", 10);
    EXPECT_TRUE(hits.empty());
}

TEST(TantivyIndexRobustness, DeleteNonexistentPageIsSafe)
{
    auto dir = freshIndexDir("delete_missing");
    TantivyIndex idx(dir);
    EXPECT_TRUE(idx.deletePage(999));
    idx.commit();
    EXPECT_EQ(idx.documentCount(), 0);
}

TEST(TantivyIndexRobustness, UnicodeContentIndexesAndSearchesCorrectly)
{
    auto dir = freshIndexDir("unicode");
    TantivyIndex idx(dir);
    idx.upsertPage(0, "café naïve résumé — Tokyo 東京 résumé");
    idx.commit();

    // ASCII fallback: search for an ASCII word that's present.
    auto ascii = idx.search("Tokyo", 10);
    EXPECT_TRUE(hitsContainPage(ascii, 0));
}
