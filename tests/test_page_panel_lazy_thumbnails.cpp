// -*- coding: utf-8 -*-
//
// Phase 2 of the thumbnail-driven zoom-hang fix: PagePanel must stop requesting
// a full-page thumbnail rasterization for EVERY page at document-open time.
//
// Root cause recap: opening a large PDF called requestThumbnail() for all N
// pages up front. Combined with the (now-fixed, phase 1) UI-thread rasterizer,
// that flooded the event loop and starved the main canvas's zoom. Phase 1 moved
// rendering to a worker pool; phase 2 bounds HOW MANY are requested: only the
// visible range plus a small margin, extended on scroll, cancelled when pages
// scroll far away.
//
// The oracle is ThumbnailRenderer::renderCount() — the process-wide count of
// REAL rasterizations (a cache hit or cancelled task never increments it). The
// pre-phase-2 behavior rendered all N; the fix renders only a bounded window.
//
// Row creation is unchanged: generateThumbnails() still adds a placeholder row
// per page synchronously, so panelItemCount() == pageCount() (GitHub #136) still
// holds — asserted here so the lazy-IMAGE change cannot silently regress the
// lazy-nothing ROW contract the UI tests depend on.

#include <gtest/gtest.h>

#include "ui/PagePanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "rendering/ThumbnailRenderer.h"

#include <QApplication>
#include <QEventLoop>
#include <QScrollBar>
#include <QListWidget>
#include <QTimer>

#include <filesystem>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::core;

namespace {

std::filesystem::path fixture(const std::string& name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

// Pump the event loop until renderCount() has been stable for `quietMs`, or
// until `timeoutMs` elapses. Worker renders are async, so we wait for the burst
// triggered by a request/scroll to drain rather than for a fixed sleep.
void pumpUntilRendersSettle(int quietMs = 250, int timeoutMs = 15000)
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

// A QListWidget child drives the panel's scrollbar; find it.
QScrollBar* panelScrollBar(PagePanel& panel)
{
    auto* list = panel.findChild<QListWidget*>("pagePanelList");
    return list ? list->verticalScrollBar() : nullptr;
}

} // namespace

// At document-open with a large page count, the panel requests only a bounded
// window of thumbnails, NOT one per page. Every ROW is still created.
TEST(PagePanelLazyThumbnails, OpenRequestsBoundedWindowNotEveryPage)
{
    const auto pdf = fixture("test_100page.pdf");
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "test_100page.pdf fixture is required and missing: " << pdf.string();

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    const int pageCount = doc->pageCount();
    ASSERT_GE(pageCount, 50) << "fixture must have >= 50 pages to prove the bound";

    PagePanel panel;
    // A small shown viewport so only a handful of rows are visible — the bound
    // must come from the visible window, not from the panel happening to be
    // large enough to show everything.
    panel.resize(180, 360);
    panel.show();
    QApplication::processEvents();

    pdfe::rendering::ThumbnailRenderer::resetRenderCount();
    panel.setDocument(doc.get());
    QApplication::processEvents();
    pumpUntilRendersSettle();

    // Row contract (GitHub #136) preserved: one row per page, synchronously.
    EXPECT_EQ(panel.panelItemCount(), pageCount);

    const std::size_t rendered = pdfe::rendering::ThumbnailRenderer::renderCount();
    // Some thumbnails must have rendered (the visible ones)...
    EXPECT_GT(rendered, 0u) << "no thumbnails rendered at all -> lazy loading "
                               "requested nothing visible";
    // ...but NOT all N. The pre-fix behavior rendered every page.
    EXPECT_LT(rendered, static_cast<std::size_t>(pageCount))
        << "every page rendered at open -> lazy windowing did not take effect";
    // The request window is (visible rows + 2*margin), falling back to
    // fallbackWindow() when the viewport has no geometry. For a 360px-tall
    // panel the visible extent is at most the fallback window, so this bound
    // (window + both margins + a small slack) holds and is far below N=100.
    const std::size_t openBound =
        static_cast<std::size_t>(PagePanel::fallbackWindow())
        + 2u * static_cast<std::size_t>(PagePanel::prefetchMargin()) + 8u;
    EXPECT_LE(rendered, openBound)
        << "far more than a visible window rendered at open (" << rendered
        << " > " << openBound << ")";
}

// Scrolling into a previously-unseen range renders those pages; scrolling back
// to an already-rendered range renders nothing new (LRU cache reuse).
TEST(PagePanelLazyThumbnails, ScrollRendersNewRangeThenReusesCache)
{
    const auto pdf = fixture("test_100page.pdf");
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "test_100page.pdf fixture is required and missing: " << pdf.string();

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 50);

    PagePanel panel;
    panel.resize(180, 360);
    panel.show();
    QApplication::processEvents();

    pdfe::rendering::ThumbnailRenderer::resetRenderCount();
    panel.setDocument(doc.get());
    QApplication::processEvents();
    pumpUntilRendersSettle();

    QScrollBar* sb = panelScrollBar(panel);
    ASSERT_NE(sb, nullptr);
    ASSERT_GT(sb->maximum(), 0)
        << "panel is not scrollable -> cannot exercise scroll-driven loading "
           "(no usable viewport geometry in this environment)";

    const std::size_t afterOpen =
        pdfe::rendering::ThumbnailRenderer::renderCount();

    // Jump to the bottom: the last pages were never in the initial window.
    sb->setValue(sb->maximum());
    QApplication::processEvents();
    pumpUntilRendersSettle();
    const std::size_t afterScrollDown =
        pdfe::rendering::ThumbnailRenderer::renderCount();

    EXPECT_GT(afterScrollDown, afterOpen)
        << "scrolling to a new range rendered no new thumbnails";
    // Still bounded: a single jump renders the bottom window, not every page in
    // between (those windows were skipped).
    EXPECT_LT(afterScrollDown, static_cast<std::size_t>(doc->pageCount()))
        << "scrolling rendered the entire document rather than a window";

    // Back to the top: those pages are already cached -> no new rasterizations.
    sb->setValue(0);
    QApplication::processEvents();
    pumpUntilRendersSettle();
    const std::size_t afterScrollBack =
        pdfe::rendering::ThumbnailRenderer::renderCount();

    EXPECT_EQ(afterScrollBack, afterScrollDown)
        << "returning to an already-rendered range re-rasterized -> cache not "
           "reused under the lazy-request path";
}
