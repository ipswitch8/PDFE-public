// -*- coding: utf-8 -*-
//
// Phase 4 of the thumbnail-driven zoom-hang fix: integration hardening.
//
// The three earlier phases are unit-verified in isolation (worker-pool render,
// viewport-gated lazy loading, zoom-side tile eviction). This test exercises
// them TOGETHER on a large-page-count document in the shape the bug report
// described: open a big PDF, scroll the thumbnail panel through dozens of pages,
// then zoom repeatedly. It asserts the hang-regression proxy recommended by the
// plan — a BOUNDED ThumbnailRenderer::renderCount() — plus no crash across the
// whole sequence. No timing/benchmark harness is introduced (interactive
// latency is not measured here); bounded render work is the proxy.

#include <gtest/gtest.h>

#include "ui/PagePanel.h"
#include "ui/PdfCanvas.h"
#include "rendering/ThumbnailRenderer.h"
#include "rendering/TileCache.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

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

void pumpUntilRendersSettle(int quietMs = 200, int timeoutMs = 20000)
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
        if (now != last) { last = now; stableFor = 0; }
        else { stableFor += 25; if (stableFor >= quietMs) loop.quit(); }
    });
    poll.start(25);
    deadline.start(timeoutMs);
    loop.exec();
}

QScrollBar* panelScrollBar(PagePanel& panel)
{
    auto* list = panel.findChild<QListWidget*>("pagePanelList");
    return list ? list->verticalScrollBar() : nullptr;
}

} // namespace

// Scroll the full range of a large thumbnail panel, then zoom repeatedly. Render
// work stays bounded (each page rasterized ~once, no cache thrash), a second
// full scroll adds no renders (cache reuse), zooming adds no thumbnail renders,
// and nothing crashes.
TEST(ThumbnailZoomIntegration, LargeDocScrollThenZoomStaysBoundedAndStable)
{
    const auto pdf = fixture("test_100page.pdf");
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "test_100page.pdf fixture required and missing: " << pdf.string();

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    const int pageCount = doc->pageCount();
    ASSERT_GE(pageCount, 50);

    PagePanel panel;
    panel.resize(190, 420);
    panel.show();
    QApplication::processEvents();

    pdfe::rendering::ThumbnailRenderer::resetRenderCount();
    panel.setDocument(doc.get());
    QApplication::processEvents();
    pumpUntilRendersSettle();

    QScrollBar* sb = panelScrollBar(panel);
    ASSERT_NE(sb, nullptr);
    ASSERT_GT(sb->maximum(), 0)
        << "panel not scrollable -> no usable viewport geometry in this env";

    // Scroll through the FULL range in ~24 steps (simulating a user dragging
    // through dozens of pages), settling render work at each stop.
    const int steps = 24;
    for (int i = 1; i <= steps; ++i) {
        const int v = static_cast<int>(
            static_cast<long long>(sb->maximum()) * i / steps);
        sb->setValue(v);
        QApplication::processEvents();
        pumpUntilRendersSettle(120, 8000);
    }

    const std::size_t afterScroll =
        pdfe::rendering::ThumbnailRenderer::renderCount();

    // Bounded: every page rasterizes at most ~once. The 256-entry LRU holds all
    // 100 pages, so there is no eviction/thrash; renderCount must not exceed the
    // page count by more than a few margin windows. The pre-fix flood (all pages
    // at open) or a thrashing cache would blow well past this.
    EXPECT_GT(afterScroll, 0u);
    EXPECT_LE(afterScroll,
              static_cast<std::size_t>(pageCount)
                  + 4u * static_cast<std::size_t>(PagePanel::prefetchMargin()))
        << "render work exceeded one-per-page + margin -> flood or cache thrash";

    // Second full scroll pass: everything is cached -> essentially no new renders.
    for (int i = 0; i <= steps; ++i) {
        const int v = static_cast<int>(
            static_cast<long long>(sb->maximum()) * i / steps);
        sb->setValue(v);
        QApplication::processEvents();
        pumpUntilRendersSettle(100, 6000);
    }
    const std::size_t afterSecondPass =
        pdfe::rendering::ThumbnailRenderer::renderCount();
    EXPECT_LE(afterSecondPass,
              afterScroll + static_cast<std::size_t>(PagePanel::prefetchMargin()))
        << "re-scrolling a fully-cached range re-rasterized -> cache not reused";

    // Now the zoom half, sharing the SAME document. A real TileCache, null
    // renderer: setZoom still runs invalidateOtherZoom + updateDocumentLayout
    // over all pages (the O(pageCount) zoom path) without a second worker pool.
    pdfe::rendering::TileCache tileCache;
    PdfCanvas canvas;
    canvas.setDocument(doc.get(), &tileCache, nullptr);
    canvas.resize(800, 600);
    canvas.show();
    QApplication::processEvents();

    const std::size_t beforeZoom =
        pdfe::rendering::ThumbnailRenderer::renderCount();

    const float zooms[] = {1.25f, 1.75f, 1.0f, 2.5f, 0.75f, 1.5f};
    for (float z : zooms) {
        canvas.setZoom(z);                 // must not throw / crash
        QApplication::processEvents();
    }

    // Zooming the canvas must not drive any thumbnail rasterization.
    EXPECT_EQ(pdfe::rendering::ThumbnailRenderer::renderCount(), beforeZoom)
        << "canvas zoom triggered thumbnail renders -> paths are entangled";

    // Reaching here without an exception/crash across scroll+zoom IS the
    // integration verdict.
    SUCCEED();
}
