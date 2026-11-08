// -*- coding: utf-8 -*-
//
// GitHub #347 — page Undo/Redo doesn't refresh thumbnails.
// GitHub #348 — image insert doesn't refresh thumbnail.
//
// Both are the SAME shape of bug the #336 delete/reorder fix closed: a page's
// Pages-panel thumbnail is served from the page-index-keyed cache and goes stale
// because nothing invalidates it.
//
//   #347: MainWindow::onUndo/onRedo rebuilt the panel ONLY when the page COUNT
//   changed. Count-preserving page-structure commands (Move, Rotate) took the
//   else branch that refreshes the canvas but not the panel, so undo/redo of a
//   move or rotate left the panel thumbnails stale. The slots now also rebuild
//   when the undone/redone command reports Command::affectsPageStructure().
//
//   #348: MainWindow::onInsertImage() called only canvas_->refreshPage(), never
//   pagePanel_->invalidateThumbnail(). It now invalidates the edited page's
//   thumbnail on the forward path, and undo/redo invalidate it too via
//   InsertImageCommand::affectedThumbnailPage().
//
// Oracle (as in test_page_panel_thumbnail_cache_flush.cpp, GitHub #336):
// ThumbnailRenderer::renderCount() — a cache HIT does NOT increment it. So after
// the operation, a refreshed/invalidated panel re-rasterizes the affected page
// and renderCount() rises; a stale panel served from the cache leaves it at 0.
// renderCount() is content-agnostic (it counts rasterizations, not pixels),
// which is why it is sound even on fixtures whose pages render to similar
// bitmaps.
//
// These tests drive the REAL MainWindow slots (the rotate/undo/redo actions and
// the Insert Image action), not the PagePanel seam in isolation — the whole
// point of #347/#348 is the wiring from those slots to the panel refresh.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "ui/MainWindow.h"
#include "ui/PagePanel.h"
#include "ui/TestDialogSeam.h"
#include "rendering/ThumbnailRenderer.h"

#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QTimer>

#include <filesystem>
#include <optional>
#include <string>

using pdfe::ui::MainWindow;
using pdfe::ui::PagePanel;

namespace {

std::filesystem::path fixture(const std::string& name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

void settle()
{
    QApplication::processEvents();
    QApplication::processEvents();
}

// Pump the event loop until renderCount() has been stable for `quietMs`, or
// until `timeoutMs` elapses (thumbnail renders are async on a worker pool).
// Copied from the #336 cache-flush test — same oracle, same drain strategy.
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

// A MainWindow that answers the modal Insert Image options dialog in code, so
// the headless insert path can run to completion (mirrors
// test_insert_image_activates_tool.cpp).
class ChoosingMainWindow : public MainWindow {
protected:
    std::optional<InsertImageChoice> promptInsertImageOptions(
        const QString&, QSize, QSizeF) override
    {
        InsertImageChoice choice;
        choice.targetRect = QRectF(72.0, 72.0, 200.0, 120.0);
        choice.jpegQuality = 80;
        choice.stripExif = true;
        choice.maintainAspectRatio = false;
        return choice;
    }
};

} // namespace

// GitHub #347 — undo of a count-preserving ROTATE must rebuild the panel
// thumbnails. Pre-fix onUndo's page-count-delta gate skipped the panel refresh
// (count unchanged), leaving the rotated page's thumbnail stale.
TEST(PageThumbnailRefreshUndoInsert, UndoOfRotateRefreshesThumbnails)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    const auto pdf = fixture("test_3page.pdf");
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << "test_3page.pdf unavailable";

    MainWindow mw;
    mw.openFile(QString::fromStdString(pdf.string()));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* panel = mw.findChild<PagePanel*>();
    ASSERT_NE(panel, nullptr) << "MainWindow has no PagePanel";

    auto* rotateCW = mw.findChild<QAction*>(QStringLiteral("rotatePageCWAction"));
    auto* undo = mw.findChild<QAction*>(QStringLiteral("undoAction"));
    ASSERT_NE(rotateCW, nullptr);
    ASSERT_NE(undo, nullptr);

    pumpUntilRendersSettle();  // let the initial thumbnail burst drain

    // Rotate the current page (forward path refreshes the panel).
    ASSERT_TRUE(rotateCW->isEnabled()) << "rotate action disabled after open";
    rotateCW->trigger();
    settle();
    pumpUntilRendersSettle();
    ASSERT_TRUE(undo->isEnabled())
        << "the rotate did not create an undo entry — the test cannot proceed";

    // Now UNDO the rotate and measure whether the panel re-rasterized.
    pdfe::rendering::ThumbnailRenderer::resetRenderCount();
    undo->trigger();
    settle();
    pumpUntilRendersSettle();

    EXPECT_GT(pdfe::rendering::ThumbnailRenderer::renderCount(), 0u)
        << "undo of a page ROTATE did not refresh the Pages-panel thumbnails "
           "(GitHub #347): the canvas updated but the panel served the rotated "
           "page's thumbnail from the stale page-index-keyed cache. The "
           "count-preserving command slipped through the page-count-delta gate.";
}

// GitHub #348 — inserting an image must invalidate that page's thumbnail on the
// forward path. Pre-fix onInsertImage called only canvas_->refreshPage().
TEST(PageThumbnailRefreshUndoInsert, InsertImageRefreshesThumbnail)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    const auto pdf = fixture("test_3page.pdf");
    const auto img = fixture("test_image_rgb.png");
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << "test_3page.pdf unavailable";
    if (!std::filesystem::exists(img)) GTEST_SKIP() << "test_image_rgb.png unavailable";

    ChoosingMainWindow mw;
    mw.openFile(QString::fromStdString(pdf.string()));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* panel = mw.findChild<PagePanel*>();
    auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(insert, nullptr);

    pumpUntilRendersSettle();

    // Insert an image on the current page via the REAL action.
    pdfe::rendering::ThumbnailRenderer::resetRenderCount();
    pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(img.string()));
    insert->trigger();
    settle();
    pumpUntilRendersSettle();

    EXPECT_GT(pdfe::rendering::ThumbnailRenderer::renderCount(), 0u)
        << "inserting an image did not invalidate the page's Pages-panel "
           "thumbnail (GitHub #348): onInsertImage refreshed only the canvas, so "
           "the panel kept serving the pre-insert thumbnail from the "
           "page-index-keyed cache.";
}

// GitHub #348 — undo of an image insert must ALSO invalidate the page's
// thumbnail (via InsertImageCommand::affectedThumbnailPage() in onUndo).
TEST(PageThumbnailRefreshUndoInsert, UndoOfInsertImageRefreshesThumbnail)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    const auto pdf = fixture("test_3page.pdf");
    const auto img = fixture("test_image_rgb.png");
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << "test_3page.pdf unavailable";
    if (!std::filesystem::exists(img)) GTEST_SKIP() << "test_image_rgb.png unavailable";

    ChoosingMainWindow mw;
    mw.openFile(QString::fromStdString(pdf.string()));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* panel = mw.findChild<PagePanel*>();
    auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
    auto* undo = mw.findChild<QAction*>(QStringLiteral("undoAction"));
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(insert, nullptr);
    ASSERT_NE(undo, nullptr);

    pumpUntilRendersSettle();
    pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(img.string()));
    insert->trigger();
    settle();
    pumpUntilRendersSettle();
    ASSERT_TRUE(undo->isEnabled())
        << "the image insert did not create an undo entry";

    // Undo the insert and measure the panel re-rasterization.
    pdfe::rendering::ThumbnailRenderer::resetRenderCount();
    undo->trigger();
    settle();
    pumpUntilRendersSettle();

    EXPECT_GT(pdfe::rendering::ThumbnailRenderer::renderCount(), 0u)
        << "undo of an image insert did not invalidate the page's thumbnail "
           "(GitHub #348): the count-preserving content undo took the branch "
           "that refreshes neither the panel nor the affected page's thumbnail.";
}
