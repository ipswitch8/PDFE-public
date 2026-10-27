// -*- coding: utf-8 -*-
// Phase 6 UI-wiring: DocumentSearchIndex tests — per-document Tantivy
// index lifecycle (lazy build from a Document's pages, cached in a temp
// dir, invalidation, reindex-single-page).
#include <gtest/gtest.h>
#include "search/DocumentSearchIndex.h"
#include "search/TantivyIndex.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <memory>

using namespace pdfe::search;
using namespace pdfe::core;

class DocumentSearchIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto src = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
    }

    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
};

TEST_F(DocumentSearchIndexTest, LazyBuildOnFirstSearch)
{
    DocumentSearchIndex idx(doc.get());
    // documentCount() reports 0 before any search has triggered a build.
    EXPECT_EQ(idx.documentCount(), 0);

    // First search triggers the build.
    auto hits = idx.searchPages("text", 10);
    (void)hits;
    EXPECT_GT(idx.documentCount(), 0)
        << "searchPages() should build the index on first call";
}

TEST_F(DocumentSearchIndexTest, IndexDirIsPerDocument)
{
    // Two indexes for the same document resolve to the same dir.
    DocumentSearchIndex a(doc.get());
    DocumentSearchIndex b(doc.get());
    EXPECT_EQ(a.indexDir(), b.indexDir())
        << "Index dir must be stable for a given document path";
}

TEST_F(DocumentSearchIndexTest, InvalidateClearsCachedIndex)
{
    DocumentSearchIndex idx(doc.get());
    auto hits1 = idx.searchPages("page", 10);
    (void)hits1;
    EXPECT_GT(idx.documentCount(), 0);

    idx.invalidate();
    EXPECT_EQ(idx.documentCount(), 0);

    // After invalidation, a fresh search rebuilds the index.
    auto hits2 = idx.searchPages("page", 10);
    (void)hits2;
    EXPECT_GT(idx.documentCount(), 0);
}

TEST_F(DocumentSearchIndexTest, ReindexPageIsNoOpBeforeFirstBuild)
{
    DocumentSearchIndex idx(doc.get());
    // Does not throw; simply does nothing because the index hasn't built.
    EXPECT_NO_THROW(idx.reindexPage(0));
    EXPECT_EQ(idx.documentCount(), 0);
}

TEST_F(DocumentSearchIndexTest, NullDocumentRejectedAtConstruct)
{
    EXPECT_THROW(DocumentSearchIndex(nullptr), std::invalid_argument);
}
