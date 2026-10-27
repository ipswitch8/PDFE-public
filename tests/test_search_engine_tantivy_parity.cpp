// -*- coding: utf-8 -*-
// Pipeline G phase-8: end-to-end tests covering the SearchEngine ↔
// Tantivy hybrid path. The unit-level Tantivy contract is locked by
// test_tantivy_search.cpp; the DocumentSearchIndex lifecycle by
// test_document_search_index.cpp. This suite covers the integration
// glue that the SearchPanel relies on:
//
//   1. searchSpecificPages() against a Tantivy candidate set produces
//      the same MuPDF quad hits as a full-scan search().
//   2. Page-level reindex after a synthesized text change reflects in
//      subsequent queries.
//   3. On a multi-page fixture, the Tantivy candidate set only contains
//      pages whose page-level text actually contains the term.
//   4. Empty / whitespace queries surface zero hits without crashing.
//   5. perf-benchmarks AC: TantivyIndex.query() on a 100-page synthetic
//      index completes in < 50 ms (per phase-8 AC #7).
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/SearchEngine.h"
#include "core/SearchResult.h"
#include "search/DocumentSearchIndex.h"
#include "search/TantivyIndex.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <string>

using namespace pdfe::core;
using namespace pdfe::search;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path freshIndexDir(const char* name)
{
    auto dir = std::filesystem::temp_directory_path()
             / "pdfe_tantivy_parity" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

std::set<int> pagesWithHits(const SearchResult& r)
{
    std::set<int> out;
    for (const auto& h : r.hits) out.insert(h.pageIndex);
    return out;
}

} // namespace

class SearchEngineTantivyParityTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx = std::make_shared<MuPdfContext>();
        auto src = fixturesDir() / "test_3page.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
    }

    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
};

// AC #5(1) + AC #5(2): build the index on a real fixture, run a query
// for a known word, then verify that searchSpecificPages on the same
// candidate set yields the same per-page hit set as the full-scan
// search() — i.e. the Tantivy-routed path agrees with the legacy path.
TEST_F(SearchEngineTantivyParityTest,
       SearchSpecificPagesAgreesWithFullScanOnTantivyCandidates)
{
    // Pick a token that's almost certainly present in any PDF body —
    // the word "page" is in test_3page.pdf's per-page text.
    const std::string needle = "page";

    SearchEngine engine(doc.get());
    SearchResult fullScan = engine.search(needle, SearchFlags::None);

    DocumentSearchIndex idx(doc.get());
    auto tantivyHits = idx.searchPages(needle, 100);

    // If the index sees no candidates, full scan must also be empty for
    // the parity claim to be meaningful. test_3page.pdf has the word
    // "page" on every page so we expect both sides non-empty.
    ASSERT_FALSE(tantivyHits.empty())
        << "Tantivy expected to surface candidate pages for 'page' "
           "on test_3page.pdf";
    ASSERT_FALSE(fullScan.hits.empty())
        << "Full scan expected to find 'page' on test_3page.pdf";

    std::vector<int> candidates;
    candidates.reserve(tantivyHits.size());
    for (const auto& h : tantivyHits) candidates.push_back(h.pageIndex);

    SearchResult hybrid = engine.searchSpecificPages(
        needle, candidates, SearchFlags::None);

    // Per-page hit sets must agree. Total hit counts must match too —
    // searchSpecificPages must not lose hits relative to full scan when
    // the candidate set covers every page that had any hit.
    auto pagesFull = pagesWithHits(fullScan);
    auto pagesHyb  = pagesWithHits(hybrid);
    EXPECT_EQ(pagesFull, pagesHyb)
        << "page-level hit sets disagree between full-scan and "
           "Tantivy-hybrid paths";
    EXPECT_EQ(fullScan.hits.size(), hybrid.hits.size())
        << "total hit count differs between full-scan and "
           "Tantivy-hybrid paths";

    // Stronger AC #5(2) check: the actual quad rectangles must agree.
    // searchSpecificPages and search() walk the same MuPDF code path
    // per page, so the quads themselves should be identical (subject
    // to ordering by pageIndex + match offset).
    auto sortKey = [](const pdfe::core::SearchHit& h) {
        return std::make_tuple(h.pageIndex, h.matchStart, h.matchEnd);
    };
    std::vector<pdfe::core::SearchHit> fs(fullScan.hits.begin(),
                                          fullScan.hits.end());
    std::vector<pdfe::core::SearchHit> hy(hybrid.hits.begin(),
                                          hybrid.hits.end());
    std::sort(fs.begin(), fs.end(),
              [&](const pdfe::core::SearchHit& a,
                  const pdfe::core::SearchHit& b) {
                  return sortKey(a) < sortKey(b);
              });
    std::sort(hy.begin(), hy.end(),
              [&](const pdfe::core::SearchHit& a,
                  const pdfe::core::SearchHit& b) {
                  return sortKey(a) < sortKey(b);
              });
    ASSERT_EQ(fs.size(), hy.size());
    for (size_t i = 0; i < fs.size(); ++i) {
        EXPECT_EQ(fs[i].pageIndex, hy[i].pageIndex);
        ASSERT_EQ(fs[i].quads.size(), hy[i].quads.size())
            << "quad-vector size mismatch at hit " << i;
        for (size_t q = 0; q < fs[i].quads.size(); ++q) {
            EXPECT_NEAR(fs[i].quads[q].x(),
                        hy[i].quads[q].x(), 1e-3);
            EXPECT_NEAR(fs[i].quads[q].y(),
                        hy[i].quads[q].y(), 1e-3);
            EXPECT_NEAR(fs[i].quads[q].width(),
                        hy[i].quads[q].width(), 1e-3);
            EXPECT_NEAR(fs[i].quads[q].height(),
                        hy[i].quads[q].height(), 1e-3);
        }
    }
}

// AC #5(3): reindexPage after a synthesized text change reflects in
// subsequent queries. We can't actually mutate page text without going
// through the editing pipeline, so instead we exercise the underlying
// upsertPage path directly: replace page 0's indexed text with a known
// token, commit, and verify the new term is searchable while the old
// one is gone.
TEST_F(SearchEngineTantivyParityTest, UpsertPageReplacesIndexedText)
{
    auto dir = freshIndexDir("upsert_replace");
    TantivyIndex idx(dir);

    idx.upsertPage(0, "original alpha keyword");
    idx.upsertPage(1, "second page beta");
    idx.commit();
    ASSERT_FALSE(idx.search("alpha", 10).empty());

    // Replace page 0 — the Tantivy delete-then-add semantic must drop
    // 'alpha' and surface 'gamma' on the next query.
    idx.upsertPage(0, "rewritten gamma keyword");
    idx.commit();

    EXPECT_TRUE(idx.search("alpha", 10).empty())
        << "old page-0 token must disappear after upsertPage replacement";
    auto gammaHits = idx.search("gamma", 10);
    ASSERT_FALSE(gammaHits.empty());
    EXPECT_EQ(gammaHits[0].pageIndex, 0);
    // Page 1 untouched.
    auto betaHits = idx.search("beta", 10);
    ASSERT_FALSE(betaHits.empty());
    EXPECT_EQ(betaHits[0].pageIndex, 1);
}

// AC #5(5): on a synthetic multi-page index, a query for a unique
// term returns only the pages containing that term.
TEST_F(SearchEngineTantivyParityTest,
       QueryReturnsOnlyPagesContainingTheTerm)
{
    auto dir = freshIndexDir("multi_page_filter");
    TantivyIndex idx(dir);

    // 5 pages, only pages 1 and 3 contain the rare term.
    idx.upsertPage(0, "the cat sat on the mat");
    idx.upsertPage(1, "encyclopedia of obscure facts");
    idx.upsertPage(2, "lorem ipsum dolor sit amet");
    idx.upsertPage(3, "encyclopedia and dictionary");
    idx.upsertPage(4, "the rain in spain");
    idx.commit();

    auto hits = idx.search("encyclopedia", 10);
    std::set<int> hitPages;
    for (const auto& h : hits) hitPages.insert(h.pageIndex);

    EXPECT_EQ(hitPages.size(), 2u);
    EXPECT_TRUE(hitPages.count(1));
    EXPECT_TRUE(hitPages.count(3));
    EXPECT_FALSE(hitPages.count(0));
    EXPECT_FALSE(hitPages.count(2));
    EXPECT_FALSE(hitPages.count(4));
}

// AC #5(4): empty / whitespace queries return zero hits without
// crashing. Already covered by TantivyIndexRobustness::EmptyQuery...
// at the unit-test layer; this version walks through DocumentSearchIndex
// to lock the same invariant at the integration layer.
TEST_F(SearchEngineTantivyParityTest, EmptyQueryViaDocumentSearchIndexReturnsEmpty)
{
    DocumentSearchIndex idx(doc.get());
    auto empty = idx.searchPages("", 10);
    EXPECT_TRUE(empty.empty());

    // Whitespace-only query also yields nothing.
    auto whitespace = idx.searchPages("   ", 10);
    EXPECT_TRUE(whitespace.empty());
}

// AC #5(3) layer test: DocumentSearchIndex::reindexPage runs through
// the background worker, the page count is preserved across N
// reindexes, and queries still work after waitForPendingReindex().
// Tantivy holds a single-writer lock per index dir so we cannot open
// a second handle on the same dir to seed off-canonical content;
// instead we exercise the reindexPage code path directly and verify
// the threading mechanism + idempotent upsert semantic.
TEST_F(SearchEngineTantivyParityTest,
       ReindexPageOnDocumentSearchIndexPreservesCountAndQueryability)
{
    DocumentSearchIndex idx(doc.get());

    // Build the index lazily on first search.
    auto warm = idx.searchPages("page", 10);
    const auto countBefore = idx.documentCount();
    ASSERT_GT(countBefore, 0)
        << "lazy build must populate the index on first searchPages()";

    // Drive reindexPage on every page. The first call dispatches a
    // background worker; subsequent calls join the prior worker before
    // dispatching their own (serializes writes). A final
    // waitForPendingReindex() guarantees the last worker has committed.
    const int total = doc->pageCount();
    for (int i = 0; i < total; ++i) {
        idx.reindexPage(i);
    }
    idx.waitForPendingReindex();

    // Document count must be unchanged — reindexPage replaces in
    // place, not adds.
    EXPECT_EQ(idx.documentCount(), countBefore)
        << "reindexPage must replace the per-page entry, not append";

    // Search must still work. A query that hit before must still hit.
    auto after = idx.searchPages("page", 10);
    EXPECT_FALSE(after.empty())
        << "search must work after a full reindex pass; the background "
           "worker either failed to commit or deadlocked";
}

// AC #1: spec-named interface (build/query/invalidatePage). These are
// thin adapters over searchPages/buildFromScratch/reindexPage but the
// spec calls out these three method names explicitly, so we lock them.
TEST_F(SearchEngineTantivyParityTest, SpecNamedInterfaceBuildQueryInvalidatePage)
{
    DocumentSearchIndex idx(doc.get());

    // build() does a full from-scratch rebuild and is idempotent.
    idx.build(doc.get());
    const auto countAfterBuild = idx.documentCount();
    EXPECT_GT(countAfterBuild, 0);

    // query() returns only page indices for a token present in the
    // fixture body. test_3page.pdf has the word "page" on every page.
    auto pages = idx.query("page");
    EXPECT_FALSE(pages.empty());
    for (int p : pages) {
        EXPECT_GE(p, 0);
        EXPECT_LT(p, doc->pageCount());
    }

    // invalidatePage(0) routes to the background reindex and must not
    // change the total document count.
    idx.invalidatePage(0);
    idx.waitForPendingReindex();
    EXPECT_EQ(idx.documentCount(), countAfterBuild);

    // build() called against the wrong Document* is a no-op (does not
    // crash, does not silently steal another doc's index dir).
    idx.build(nullptr);
    EXPECT_EQ(idx.documentCount(), countAfterBuild)
        << "build(nullptr) must not rebuild; it should be a no-op";
}

// AC #7 (perf-benchmarks): TantivyIndex query on a 100-page synthetic
// index completes in < 50 ms. We synthesize a 100-page index here
// because no checked-in 100-page fixture exists; the Tantivy contract
// is what's being measured (the per-page candidate lookup), not MuPDF's
// extractPlainText timing.
TEST_F(SearchEngineTantivyParityTest, HundredPageQueryUnderFiftyMilliseconds)
{
    auto dir = freshIndexDir("perf_100");
    TantivyIndex idx(dir);

    for (int i = 0; i < 100; ++i) {
        std::string body = "page body " + std::to_string(i)
                         + " quick brown fox";
        if (i % 5 == 0) body += " RAREWORDXYZ";
        idx.upsertPage(i, body);
    }
    idx.commit();
    ASSERT_EQ(idx.documentCount(), 100);

    // Warm the reader once so the first hit doesn't pay open-time cost.
    (void)idx.search("body", 10);

    auto t0 = std::chrono::steady_clock::now();
    auto hits = idx.search("RAREWORDXYZ", 100);
    auto t1 = std::chrono::steady_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    EXPECT_LT(ms, 50) << "query took " << ms << " ms; AC #7 budget is 50";
    EXPECT_GE(hits.size(), 20u)
        << "expected ~20 pages with RAREWORDXYZ on 100-page index";
}
