// -*- coding: utf-8 -*-
//
// Drop-location indicator geometry for the Pages panel.
//
// THE DEFECT (user report): while dragging a page thumbnail to reorder, the
// drop-location indicator was a full-height VERTICAL bar at a thumbnail's edge.
// A vertical bar spanning a whole item does not tell the user whether the
// dragged page will land ABOVE or BELOW the existing page at that spot — the
// insertion direction is ambiguous.
//
// THE FIX: PageListWidget::dropIndicatorRect() now returns a HORIZONTAL line in
// the gap BETWEEN thumbnails at the insertion boundary (above the insert-before
// item; below the last item on append). A horizontal line in the gap reads
// unambiguously as "the page is inserted HERE".
//
// This asserts the GEOMETRY through the PagePanel::dropIndicatorRectForTesting
// seam (which forces a deterministic single-column layout so visualItemRect is
// valid in a headless panel). The discriminating oracle is ORIENTATION:
//   * fixed (horizontal line): width  > height   (spans the item width, ~3px tall)
//   * pre-fix (vertical bar):  height > width    (spans the item height, ~3px wide)
// so this test FAILS against the old vertical-bar geometry — it is not vacuous.
// A null rect (no geometry) also fails, so a seam that silently produced nothing
// would not pass either.
#include <gtest/gtest.h>

#include "ui/PagePanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QApplication>
#include <QCoreApplication>
#include <QRect>

#include <filesystem>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::core;

namespace {

struct DropIndicatorFixture {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<PagePanel> panel;
};

bool makeFixture(DropIndicatorFixture& f)
{
    const auto pdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    if (!std::filesystem::exists(pdf)) return false;
    f.ctx = std::make_shared<MuPdfContext>();
    f.doc = Document::open(f.ctx, pdf);
    if (!f.doc || f.doc->pageCount() < 3) return false;
    f.panel = std::make_unique<PagePanel>();
    f.panel->setDocument(f.doc.get());
    QCoreApplication::processEvents();
    return true;
}

} // namespace

// The indicator for inserting BEFORE a middle page is a horizontal line (wider
// than tall) sitting ABOVE that page's cell — not the old full-height bar.
TEST(PagePanelDropIndicator, InsertBeforeMiddlePageIsHorizontalLineAboveIt)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    DropIndicatorFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    const QRect r = f.panel->dropIndicatorRectForTesting(/*insertIndex=*/1);
    ASSERT_FALSE(r.isNull()) << "no indicator geometry produced for insertIndex=1";

    // KEY discriminator: horizontal (width > height). Pre-fix this was a vertical
    // bar (height > width), so the old geometry fails here.
    EXPECT_GT(r.width(), r.height())
        << "drop indicator must be a HORIZONTAL line (w>h); got "
        << r.width() << "x" << r.height();
    // It spans roughly the thumbnail width and is only a few px tall.
    EXPECT_GE(r.width(), 32) << "indicator should span the item width";
    EXPECT_LE(r.height(), 8) << "indicator should be a thin line, not a full cell";
}

// The append indicator (insertIndex == pageCount) is also a horizontal line, and
// it sits BELOW the previous insert-before line (further down the column) — i.e.
// appending marks the gap after the last page, not before it.
TEST(PagePanelDropIndicator, AppendIsHorizontalLineBelowInsertBefore)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    DropIndicatorFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    const int n = f.doc->pageCount();
    const QRect lastBefore = f.panel->dropIndicatorRectForTesting(/*insertIndex=*/n - 1);
    const QRect append     = f.panel->dropIndicatorRectForTesting(/*insertIndex=*/n);
    ASSERT_FALSE(lastBefore.isNull());
    ASSERT_FALSE(append.isNull());

    EXPECT_GT(append.width(), append.height())
        << "append indicator must be a HORIZONTAL line; got "
        << append.width() << "x" << append.height();
    // The append line is below the "insert before the last page" line.
    EXPECT_GT(append.top(), lastBefore.top())
        << "append marker (gap after last page) must sit below the insert-before "
           "marker for the last page";
}

// Sensitivity floor: the indicator is a THIN line, never a full-cell highlight.
// A regression back to the full-height bar (height ~= icon size, 128px) trips this.
TEST(PagePanelDropIndicator, IndicatorIsThinNotAFullCellBar)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    DropIndicatorFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    const QRect r = f.panel->dropIndicatorRectForTesting(/*insertIndex=*/2);
    ASSERT_FALSE(r.isNull());
    EXPECT_LT(r.height(), 32)
        << "a thin insertion line must be far shorter than a thumbnail cell "
           "(a full-height vertical bar would be ~128px tall)";
}
