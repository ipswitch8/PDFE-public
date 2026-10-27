// -*- coding: utf-8 -*-
//
// GitHub #226 -- shift+click on PagePanel selected only the clicked page.
//
// The list has always been QAbstractItemView::ExtendedSelection, so Qt built
// the anchor..clicked range correctly. The range was destroyed on the way
// BACK: the click emits pageSelected(clicked), MainWindow::onPageSelected
// routes to PagePanel::setCurrentPage(), and setCurrentRow() carries an
// implicit ClearAndSelect that collapsed the range to the single current row.
//
// The QSignalBlocker around that call suppressed the SIGNAL but not the
// selection change, which is why the collapse left no trace to notice.
//
// These tests drive that exact round trip -- establish a range, then deliver
// the setCurrentPage() the feedback path delivers -- and assert on the whole
// selected-index SET. A liveness oracle ("something is still selected") would
// pass against the defect, because after the collapse exactly one page is
// still selected.

#include <gtest/gtest.h>
#include "ui/PagePanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QCoreApplication>
#include <QList>
#include <filesystem>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::core;

namespace {

struct PanelFixture {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<PagePanel> panel;
};

bool makePanel(PanelFixture& f)
{
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdf = fixtures / "test_3page.pdf";
    if (!std::filesystem::exists(pdf)) return false;

    f.ctx = std::make_shared<MuPdfContext>();
    f.doc = Document::open(f.ctx, pdf);
    if (!f.doc || f.doc->pageCount() <= 0) return false;

    f.panel = std::make_unique<PagePanel>();
    f.panel->setDocument(f.doc.get());
    QCoreApplication::processEvents();
    return true;
}

} // namespace

// The regression proper. Reverting setCurrentPage() to a bare setCurrentRow()
// reddens this: the expected set becomes {2} instead of {0,1,2}.
TEST(PagePanelRangeSelection, CurrentPageFeedbackDoesNotCollapseARange)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PanelFixture f;
    if (!makePanel(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    ASSERT_EQ(f.doc->pageCount(), 3);

    // Stand in for the user shift+clicking page 2 with page 0 as anchor.
    // selectPagesProgrammatic drives the same QItemSelectionModel a mouse
    // range-select drives, so the state under test is the real one.
    ASSERT_TRUE(f.panel->selectPagesProgrammatic({0, 1, 2}));

    const QList<int> built = f.panel->selectedPageIndices();
    ASSERT_EQ(built, (QList<int>{0, 1, 2}))
        << "the range was never established, so the collapse this test exists "
           "to catch could not be observed either way";

    // The feedback the click produces: MainWindow tells the panel which page
    // is now current -- the one that was clicked.
    f.panel->setCurrentPage(2);
    QCoreApplication::processEvents();

    EXPECT_EQ(f.panel->selectedPageIndices(), (QList<int>{0, 1, 2}))
        << "setCurrentPage() collapsed a multi-page selection; this is #226";
}

// A range the current page is NOT part of must also survive -- the guard keys
// on the selection being real, not on which page happens to be current.
TEST(PagePanelRangeSelection, ARangeSurvivesCurrentPageMovingWithinIt)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PanelFixture f;
    if (!makePanel(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    ASSERT_TRUE(f.panel->selectPagesProgrammatic({0, 1}));
    ASSERT_EQ(f.panel->selectedPageIndices(), (QList<int>{0, 1}));

    f.panel->setCurrentPage(0);
    QCoreApplication::processEvents();

    EXPECT_EQ(f.panel->selectedPageIndices(), (QList<int>{0, 1}));
}

// The other half of the behaviour, which the fix must NOT break: with no
// multi-selection to protect, setCurrentPage() still selects the page. Opening
// a document relies on this to land on page 0, so without this test the fix
// could silently leave a freshly-opened document with nothing selected.
TEST(PagePanelRangeSelection, CurrentPageStillSelectsWhenThereIsNoRange)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PanelFixture f;
    if (!makePanel(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    f.panel->setCurrentPage(1);
    QCoreApplication::processEvents();

    EXPECT_EQ(f.panel->selectedPageIndices(), (QList<int>{1}))
        << "setCurrentPage() no longer selects the page when there is no "
           "range to preserve; document-open would land on nothing";
}
