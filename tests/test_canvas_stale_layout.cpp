// -*- coding: utf-8 -*-
// Deterministic regression for the PdfCanvas stale-layout guard.
//
// PdfCanvas::findPageAtY(docY) resolves a document-space Y coordinate to a
// page index by walking pageLayouts_. pageLayouts_ is only rebuilt when
// updateDocumentLayout()/refreshAllPages() runs, so after a *structural
// shrink* (deletePage) it can still hold entries for pages the document no
// longer has — it is longer than document_->pageCount() until the next
// rebuild. Returning such a stale index to a mouse/tooltip handler that then
// calls the THROWING accessors document_->pageInfo(idx)/page(idx) would let
// the throw escape the Qt event handler → propagate out of the event loop →
// std::terminate.
//
// The fix validates every resolved index against the LIVE document via a
// liveIndex lambda: findPageAtY returns the index only if
// (idx >= 0 && idx < pageCount()), else -1. All callers guard `pageIdx >= 0`,
// so -1 makes them skip (a no-op, not a crash).
//
// This test forces the stale condition (build layout for 10 pages, delete the
// last page WITHOUT rebuilding the layout) and proves findPageAtY returns -1
// for the removed page rather than the old out-of-range index.

#include <gtest/gtest.h>

#include "ui/PdfCanvas.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QApplication>

#include <filesystem>
#include <memory>

using namespace pdfe::ui;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

} // namespace

class CanvasStaleLayoutTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fixture_ = fixturesDir() / "test_10page.pdf";
        if (!std::filesystem::exists(fixture_)) {
            GTEST_SKIP() << "test_10page.pdf fixture not available";
        }
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        doc_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(doc_, nullptr);
        ASSERT_GE(doc_->pageCount(), 2) << "fixture must be multi-page";
    }

    std::filesystem::path fixture_;
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::core::Document> doc_;
};

// findPageAtY must never return an index the live document no longer has,
// even when pageLayouts_ is stale (longer than pageCount) after a shrink.
TEST_F(CanvasStaleLayoutTest, StaleLayoutAfterShrinkReturnsMinusOne)
{
    PdfCanvas canvas;

    // setDocument() runs updateDocumentLayout() synchronously, building
    // pageLayouts_ from the current pageCount (no show/resize needed). A null
    // TileCache/TileRenderer is fine: layout construction does not touch them.
    canvas.setDocument(doc_.get(), nullptr, nullptr);

    // Flush the deferred (QueuedConnection) re-layout setDocument posts, so the
    // layout is settled at 10 pages BEFORE we shrink. After this we must NOT
    // pump the event loop again, or the deferred rebuild would un-stale it.
    QApplication::processEvents();

    const int originalCount = doc_->pageCount();
    const int lastIndex = originalCount - 1;

    // A Y beyond the bottom of all pages deterministically resolves to the
    // last page via findPageAtY's "beyond all pages" branch, independent of
    // exact page heights. With the full layout this is the valid last index.
    const int deepY = 10'000'000;
    const int before = canvas.findPageAtDocY(deepY);
    EXPECT_EQ(before, lastIndex)
        << "With a full 10-page layout, a Y past the end must resolve to the "
           "last page index.";

    // SHRINK the document WITHOUT telling the canvas: pageCount drops to
    // originalCount-1, but pageLayouts_ still has an entry for the removed
    // page (do NOT call refreshAllPages()/updateDocumentLayout()).
    doc_->deletePage(lastIndex);
    ASSERT_EQ(doc_->pageCount(), originalCount - 1);

    // pageLayouts_ is now stale (still 10 entries, back().pageIndex == 9),
    // but the document only has 9 pages. The guard must reject the stale
    // last-page index and return -1 rather than the out-of-range 9.
    const int after = canvas.findPageAtDocY(deepY);
    EXPECT_EQ(after, -1)
        << "After a shrink that leaves pageLayouts_ stale, findPageAtY must "
           "return -1 for a page the live document no longer has — not the "
           "old/out-of-range index.";

    // Stronger invariant: whatever findPageAtY returns, it is never an index
    // the live document does not have. Either -1 (skip) or a valid index.
    const int newCount = doc_->pageCount();
    EXPECT_TRUE(after == -1 || (after >= 0 && after < newCount))
        << "findPageAtY returned " << after << " but the document now has only "
        << newCount << " pages (valid range 0.." << (newCount - 1) << ").";

    // Also spot-check a mid-document Y that lands inside the (still-present)
    // stale band for the removed page's coordinates: any resolution must obey
    // the same live-range invariant.
    for (int probeY : {0, 500, 5'000, 50'000, 500'000, deepY}) {
        const int idx = canvas.findPageAtDocY(probeY);
        EXPECT_TRUE(idx == -1 || (idx >= 0 && idx < newCount))
            << "probeY=" << probeY << " resolved to out-of-range index " << idx
            << " (live pageCount=" << newCount << ").";
    }

    canvas.clearDocument();
}
