// -*- coding: utf-8 -*-
//
// GitHub #336 — stale Pages-panel thumbnail after right-click Delete.
//
// Symptom: right-click > Delete removes the page (navigation confirms it is
// gone) but the panel keeps showing the old thumbnail; clicking the stale
// thumbnail navigates to the now-shifted next page.
//
// Root cause (investigation docs/audit/ui-panel-fixes-2026-11-investigation.md,
// Bug B): the delete path DOES reach PagePanel::refreshPages() (via
// onDocumentStructureChanged), which rebuilds the item LIST with the correct
// count and calls thumbnailRenderer_->cancelAll() + generateThumbnails(). But
// cancelAll() clears only PENDING tasks, not the image cache. The cache is
// keyed by CacheKey{pageIndex,w,h}; a delete renumbers every page at/after the
// edit point, so the item that lands at a shifted index is served the STALE
// bitmap cached under that index. The item's Qt::UserRole page index is correct
// (navigation works) — only the picture is stale.
//
// Fix: refreshPages() now calls thumbnailRenderer_->invalidate() (a full cache
// clear) alongside cancelAll(), before generateThumbnails(). A full clear is
// correct because a renumber invalidates every index from the edit point on.
//
// This test proves the CACHE FLUSH specifically, not merely the item count
// (which was already correct before the fix and so would not catch this bug).
//
// Key oracle: ThumbnailRenderer::renderCount() — a cache HIT does NOT increment
// it (see test_page_panel_lazy_thumbnails.cpp, which relies on exactly this).
// Pre-fix the shifted indices are served from the stale page-index-keyed cache,
// so after a delete renderCount() stays 0; post-fix the cache is cleared, so the
// rebuilt window re-rasterizes and renderCount() rises. renderCount() is
// content-agnostic — it counts rasterizations, not pixels — which is why it is
// the sound oracle here: test_3page.pdf's pages have no drawn content, so a
// pixel-level "the thumbnail changed" oracle would be vacuous (every page
// rasterizes to the same blank bitmap) and cannot be used.
//
// panelItemCount() == pageCount() is also asserted, but only as a sanity check —
// it held before the fix too, so on its own it does not catch #336.

#include <gtest/gtest.h>

#include "ui/PagePanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PageCommands.h"
#include "rendering/ThumbnailRenderer.h"

#include <QApplication>
#include <QEventLoop>
#include <QList>
#include <QTimer>

#include <filesystem>
#include <memory>
#include <vector>

using namespace pdfe::ui;
using namespace pdfe::core;

namespace {

std::filesystem::path fixture(const std::string& name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

// Pump the event loop until renderCount() has been stable for `quietMs`, or
// until `timeoutMs` elapses. Thumbnail renders are async on a worker pool, so we
// wait for the burst a request triggers to drain rather than for a fixed sleep.
void pumpUntilRendersSettle(int quietMs = 300, int timeoutMs = 15000)
{
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

    std::size_t last = pdfe::rendering::ThumbnailRenderer::renderCount();
    int stableFor = 0;

    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        const std::size_t now = pdfe::rendering::ThumbnailRenderer::renderCount();
        if (now != last) {
            last = now;
            stableFor = 0;
        } else {
            stableFor += 25;
            if (stableFor >= quietMs) loop.quit();
        }
    });
    poll.start(25);
    deadline.start(timeoutMs);
    loop.exec();
}

// Give each page a distinct /Rotate so the delete models a real
// renumber-with-distinguishable-pages scenario (not needed by the renderCount
// oracle, which is content-agnostic, but it makes the fixture faithful to the
// reported bug). Applied through Document directly so no undo entry is created
// (mirrors test_page_panel_internal_reorder.cpp).
void stampDistinctIdentities(Document* doc)
{
    for (int i = 0; i < doc->pageCount(); ++i) {
        doc->rotatePage(i, 90 * i);  // -> 0, 90, 180
    }
}

} // namespace

// Deleting a NON-LAST page must flush the thumbnail image cache so the shifted
// pages are re-rasterized instead of served stale from the page-index-keyed
// cache. Without the fix, refreshPages() rebuilds the row list correctly but the
// cache still serves the old bitmap for the shifted index.
TEST(PagePanelThumbnailCacheFlush, DeleteFlushesStaleThumbnailCache)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    const auto pdf = fixture("test_3page.pdf");
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3) << "this test assumes the 3-page fixture";

    stampDistinctIdentities(doc.get());  // page rotations -> [0, 90, 180]

    // Initial population: a non-shown panel with 3 pages falls back to a leading
    // window that covers all 3, so setDocument()'s generateThumbnails() renders
    // every page's 128x128 thumbnail and caches it under
    // CacheKey{pageIndex,128,128}. Reset BEFORE setDocument so this render is the
    // measured baseline and the cache is freshly populated from it.
    pdfe::rendering::ThumbnailRenderer::resetRenderCount();

    PagePanel panel;
    panel.setDocument(doc.get());
    QApplication::processEvents();
    pumpUntilRendersSettle();

    ASSERT_EQ(panel.panelItemCount(), doc->pageCount());
    const std::size_t rendersOnOpen = pdfe::rendering::ThumbnailRenderer::renderCount();
    ASSERT_GT(rendersOnOpen, 0u)
        << "fixture invalid: no thumbnails rendered on open, so there is nothing "
           "cached to prove the flush against";

    // ── Delete a NON-LAST page via the SAME command the context-menu Delete runs,
    //    then refresh exactly as MainWindow::onDocumentStructureChanged does. ──
    pdfe::rendering::ThumbnailRenderer::resetRenderCount();

    pdfe::commands::DeletePageRangeCommand del(doc.get(), std::vector<int>{1});
    del.execute();
    doc->refreshPageCount();
    ASSERT_EQ(doc->pageCount(), 2);

    panel.refreshPages();
    QApplication::processEvents();
    pumpUntilRendersSettle();

    // (sanity) the row list rebuilt with the new count — true before the fix too,
    // so NOT the assertion that catches #336.
    EXPECT_EQ(panel.panelItemCount(), doc->pageCount());

    // (KEY) the cache was flushed: the rebuilt window re-rasterized the shifted
    // pages rather than serving them from the page-index-keyed cache. A cache hit
    // never increments renderCount(), so pre-fix this is 0; the fix drives it up.
    const std::size_t rendersAfterDelete = pdfe::rendering::ThumbnailRenderer::renderCount();
    EXPECT_GT(rendersAfterDelete, 0u)
        << "no thumbnails re-rendered after delete -> the shifted pages were "
           "served from the stale page-index-keyed cache (GitHub #336). The item "
           "list rebuilt with the right count, but refreshPages() never flushed "
           "the thumbnail image cache.";
    EXPECT_GE(rendersAfterDelete, 2u)
        << "a 2-page document should re-rasterize both rows after a full cache "
           "flush; a lower count means the shifted row was served stale";
}

// GitHub #339: the test above drives the delete through DeletePageRangeCommand +
// refreshPages() DIRECTLY. That skips the real right-click > Delete chain: the
// context menu's "Delete Selected" action must emit pagesDeleteRequested, and
// the host wires that signal to the delete + structure-changed + refreshPages
// sequence. This test drives that REAL chain — the panel's own
// deleteSelectedAction dispatch (via the #339 seam that bypasses only the
// un-driveable modal menu.exec()) emits pagesDeleteRequested, a connected
// handler performs the delete + refresh exactly as MainWindow does, and the
// SAME renderCount cache-flush oracle then proves the thumbnail cache was
// flushed. A regression that unwired Delete from its signal, or that let the
// signal-driven refresh skip the cache flush, fails here — neither is caught by
// the direct-command test above.
TEST(PagePanelThumbnailCacheFlush, ContextMenuDeleteSignalDrivesRefreshAndFlushesCache)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    const auto pdf = fixture("test_3page.pdf");
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3) << "this test assumes the 3-page fixture";

    stampDistinctIdentities(doc.get());

    pdfe::rendering::ThumbnailRenderer::resetRenderCount();

    PagePanel panel;
    panel.setDocument(doc.get());
    QApplication::processEvents();
    pumpUntilRendersSettle();

    ASSERT_EQ(panel.panelItemCount(), doc->pageCount());
    ASSERT_GT(pdfe::rendering::ThumbnailRenderer::renderCount(), 0u)
        << "fixture invalid: no thumbnails rendered on open";

    // Wire pagesDeleteRequested EXACTLY as MainWindow does: delete the requested
    // pages and refresh the panel (MainWindow::onDocumentStructureChanged ->
    // refreshPages). The whole point is that refreshPages() runs because the
    // SIGNAL fired, not because the test called it.
    QList<int> deliveredIndices;
    int deliveries = 0;
    QObject::connect(&panel, &PagePanel::pagesDeleteRequested,
                     &panel, [&](QList<int> indices) {
        ++deliveries;
        deliveredIndices = indices;
        pdfe::commands::DeletePageRangeCommand del(
            doc.get(), std::vector<int>(indices.begin(), indices.end()));
        del.execute();
        doc->refreshPageCount();
        panel.refreshPages();
    });

    // Select a NON-LAST page (index 1) through the panel's own selection seam,
    // so selectedPageIndices() — which the real dispatch reads — returns it.
    ASSERT_TRUE(panel.selectPagesProgrammatic(QList<int>{1}))
        << "could not select page 1 for the delete";

    pdfe::rendering::ThumbnailRenderer::resetRenderCount();

    // Drive the REAL action dispatch (the link the direct-command test skips).
    const bool dispatched =
        panel.triggerContextMenuActionForTest(QStringLiteral("deleteSelectedAction"));
    ASSERT_TRUE(dispatched)
        << "the Delete Selected action did not dispatch — the context-menu "
           "action->signal wiring is broken (GitHub #339)";

    QApplication::processEvents();
    pumpUntilRendersSettle();

    // The signal actually fired, carrying the selected page index.
    EXPECT_EQ(deliveries, 1) << "pagesDeleteRequested must fire exactly once";
    ASSERT_EQ(deliveredIndices.size(), 1);
    EXPECT_EQ(deliveredIndices.first(), 1)
        << "the delete signal carried the wrong page index";

    // The signal-driven delete reached refreshPages AND flushed the cache.
    EXPECT_EQ(doc->pageCount(), 2) << "the signal-driven delete did not remove a page";
    EXPECT_EQ(panel.panelItemCount(), doc->pageCount());
    const std::size_t rendersAfter = pdfe::rendering::ThumbnailRenderer::renderCount();
    EXPECT_GT(rendersAfter, 0u)
        << "no thumbnails re-rendered after the signal-driven delete -> the "
           "shifted pages were served from the stale page-index-keyed cache "
           "(GitHub #336/#339): the refresh ran but never flushed the cache, or "
           "the signal never reached the refresh.";
}
