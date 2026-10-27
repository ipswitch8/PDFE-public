// -*- coding: utf-8 -*-
//
// Pipeline a7c2e541 phase-17 (coverage expansion).
//
// AC2-coverage: addresses tests/docs/coverage_gaps.md entry #3.
//
// Verifies the Q_INVOKABLE movePageProgrammatic hook added to
// PagePanel in phase-17.  Closes KNOWN_WEAK P10-INFRA-1 on the
// test-automation side (pyautogui mouseDown/moveTo/mouseUp drag does
// not reliably trigger Qt6 QListWidget InternalMove on this build).

#include <gtest/gtest.h>
#include "ui/PagePanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QCoreApplication>
#include <QListWidget>
#include <QSet>
#include <QSignalSpy>
#include <filesystem>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::core;

namespace {

struct PagePanelFixture {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<PagePanel> panel;
};

bool makeFixture(PagePanelFixture& f, const std::string& pdfName = "test_3page.pdf")
{
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdf = fixtures / pdfName;
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

TEST(PagePanelQInvokable, MovePageProgrammatic_NoDocumentReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanel panel;
    EXPECT_FALSE(panel.movePageProgrammatic(0, 1));
}

TEST(PagePanelQInvokable, MovePageProgrammatic_NegativeIndicesReturnFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    EXPECT_FALSE(f.panel->movePageProgrammatic(-1, 0));
    EXPECT_FALSE(f.panel->movePageProgrammatic(0, -1));
    EXPECT_FALSE(f.panel->movePageProgrammatic(-5, -10));
}

TEST(PagePanelQInvokable, MovePageProgrammatic_OutOfRangeReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    const int pc = f.doc->pageCount();
    EXPECT_FALSE(f.panel->movePageProgrammatic(pc, 0));
    EXPECT_FALSE(f.panel->movePageProgrammatic(0, pc));
    EXPECT_FALSE(f.panel->movePageProgrammatic(pc + 100, pc + 200));
}

TEST(PagePanelQInvokable, MovePageProgrammatic_EqualIndicesReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    EXPECT_FALSE(f.panel->movePageProgrammatic(0, 0));
    EXPECT_FALSE(f.panel->movePageProgrammatic(1, 1));
}

TEST(PagePanelQInvokable, MovePageProgrammatic_ValidMoveEmitsPageMovedSignal)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    QSignalSpy spy(f.panel.get(), &PagePanel::pageMoved);
    ASSERT_TRUE(spy.isValid());

    EXPECT_TRUE(f.panel->movePageProgrammatic(0, 2));
    ASSERT_EQ(spy.count(), 1);
    const auto& args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), 0);
    EXPECT_EQ(args.at(1).toInt(), 2);
}

TEST(PagePanelQInvokable, MovePageProgrammatic_MultipleMovesEmitMultipleSignals)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    QSignalSpy spy(f.panel.get(), &PagePanel::pageMoved);

    EXPECT_TRUE(f.panel->movePageProgrammatic(0, 2));
    EXPECT_TRUE(f.panel->movePageProgrammatic(1, 0));
    EXPECT_TRUE(f.panel->movePageProgrammatic(2, 1));

    EXPECT_EQ(spy.count(), 3);
}

// Pipeline b5e3a9f2 phase-6 (P-GAP-73): tests for the new
// selectPagesProgrammatic Q_INVOKABLE hook.

TEST(PagePanelQInvokable, SelectPagesProgrammatic_EmptyListReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanel panel;
    EXPECT_FALSE(panel.selectPagesProgrammatic(QList<int>{}));
}

TEST(PagePanelQInvokable, SelectPagesProgrammatic_OutOfRangeReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    // 3-page doc — index 5 is out of range.
    EXPECT_FALSE(f.panel->selectPagesProgrammatic(QList<int>{5}));
    EXPECT_FALSE(f.panel->selectPagesProgrammatic(QList<int>{0, 5}));
    EXPECT_FALSE(f.panel->selectPagesProgrammatic(QList<int>{-1}));
}

TEST(PagePanelQInvokable, SelectPagesProgrammatic_SingleIndexSelectsIt)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    EXPECT_TRUE(f.panel->selectPagesProgrammatic(QList<int>{1}));

    // Verify selection landed on row 1 via the internal QListWidget.
    auto* list = f.panel->findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    auto selected = list->selectedItems();
    EXPECT_EQ(selected.size(), 1);
    EXPECT_EQ(list->row(selected.first()), 1);
}

TEST(PagePanelQInvokable, SelectPagesProgrammatic_MultipleNonContiguousIndices)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    // Non-contiguous selection: pages 0 and 2 (skipping 1) — this is
    // the case that pyautogui Ctrl+click consistently fails on Qt6
    // ExtendedSelection, hence the programmatic seam.
    EXPECT_TRUE(f.panel->selectPagesProgrammatic(QList<int>{0, 2}));

    auto* list = f.panel->findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    auto selected = list->selectedItems();
    EXPECT_EQ(selected.size(), 2);

    QSet<int> rows;
    for (auto* item : selected) rows.insert(list->row(item));
    EXPECT_TRUE(rows.contains(0));
    EXPECT_TRUE(rows.contains(2));
    EXPECT_FALSE(rows.contains(1));
}

TEST(PagePanelQInvokable, SelectPagesProgrammatic_ClearsPriorSelection)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    // First select page 0, then re-select pages 1 and 2 — the
    // original selection on row 0 must be cleared.
    EXPECT_TRUE(f.panel->selectPagesProgrammatic(QList<int>{0}));
    EXPECT_TRUE(f.panel->selectPagesProgrammatic(QList<int>{1, 2}));

    auto* list = f.panel->findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    auto selected = list->selectedItems();
    EXPECT_EQ(selected.size(), 2);

    QSet<int> rows;
    for (auto* item : selected) rows.insert(list->row(item));
    EXPECT_FALSE(rows.contains(0)) << "prior row-0 selection must be cleared";
    EXPECT_TRUE(rows.contains(1));
    EXPECT_TRUE(rows.contains(2));
}
