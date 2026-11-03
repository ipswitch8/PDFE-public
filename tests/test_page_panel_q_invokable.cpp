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

// ---------------------------------------------------------------------------
// GitHub #326 — the drag-reorder ROOT-CAUSE configuration guard.
//
// The defect was a one-line misconfiguration: the thumbnail QListWidget had a
// non-Static movement() (Snap). In IconMode a non-Static movement makes
// QListView handle the drag gesture itself — free-positioning the icon's rect
// — and NEVER fall through to the InternalMove drop subsystem, so a REAL drag
// never reaches PageListWidget::dropEvent → onPageDropped → pageMoved →
// MovePageCommand, and the document order is left untouched.
//
// The three properties below are exactly what must hold for a real drag gesture
// to route through the drop handler and reorder the Document:
//   * movement()        == Static        (so InternalMove, not free icons, wins)
//   * dragDropMode()    == InternalMove   (reorder rows within the view)
//   * defaultDropAction == MoveAction     (a reorder, not a copy)
//
// This is a DETERMINISTIC guard on the live widget's real configuration. It is
// the furthest-reachable real seam for the gesture half of #326 in a headless
// test: the full synthetic OS-level drag cannot be driven against a Qt6
// QListWidget on this build (KNOWN_WEAK P10-INFRA-1 — SendInput does not make
// Qt6 START the InternalMove QDrag), which is why the model/command half is
// pinned separately at the command level (test_page_commands.cpp) and the
// end-to-end gesture is exercised by the xfail'd pywinauto test. If anyone
// reverts movement() to Snap/Free, the real drag silently stops reordering the
// document again — and this test reddens.
TEST(PagePanelQInvokable, DragReorderConfiguration_RoutesRealDragThroughDropHandler)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    auto* list = f.panel->findChild<QListWidget*>();
    ASSERT_NE(list, nullptr) << "PagePanel has no internal QListWidget";

    EXPECT_EQ(list->movement(), QListView::Static)
        << "movement() must be Static or a real drag never reaches the drop "
           "handler — this is the GitHub #326 root cause";
    EXPECT_EQ(list->dragDropMode(), QAbstractItemView::InternalMove)
        << "the list must be in InternalMove mode to reorder rows on drop";
    EXPECT_EQ(list->defaultDropAction(), Qt::MoveAction)
        << "a reorder drop must default to MoveAction, not CopyAction";
}

// ---------------------------------------------------------------------------
// GitHub #325 — PagePanel::parsePageMime round-trip and rejection.
//
// parsePageMime decodes the application/x-pdfe-page payload at the drop side
// (MainWindow::dropEvent). It is a pure function, so it is unit-testable with
// no widget or document. A malformed payload must be REJECTED, not silently
// decoded to a bogus (token, index) that the drop then acts on.
// ---------------------------------------------------------------------------
TEST(PagePanelCrossDocMime, ParsePageMime_ValidPayloadRoundTrips)
{
    QString token;
    int page = -1;
    EXPECT_TRUE(PagePanel::parsePageMime(QByteArray("sometoken\n7"), token, page));
    EXPECT_EQ(token, QStringLiteral("sometoken"));
    EXPECT_EQ(page, 7);

    // page 0 is valid (first page).
    token.clear(); page = -1;
    EXPECT_TRUE(PagePanel::parsePageMime(QByteArray("3-abc123\n0"), token, page));
    EXPECT_EQ(token, QStringLiteral("3-abc123"));
    EXPECT_EQ(page, 0);
}

TEST(PagePanelCrossDocMime, ParsePageMime_MalformedPayloadsReturnFalse)
{
    QString token;
    int page = 0;
    // No newline separator.
    EXPECT_FALSE(PagePanel::parsePageMime(QByteArray("tokenonly"), token, page));
    // Empty token (newline first).
    EXPECT_FALSE(PagePanel::parsePageMime(QByteArray("\n5"), token, page));
    // Non-numeric page index.
    EXPECT_FALSE(PagePanel::parsePageMime(QByteArray("tok\nnotanumber"), token, page));
    // Negative page index.
    EXPECT_FALSE(PagePanel::parsePageMime(QByteArray("tok\n-1"), token, page));
    // Empty input.
    EXPECT_FALSE(PagePanel::parsePageMime(QByteArray(), token, page));
    // Missing page (trailing newline, empty index).
    EXPECT_FALSE(PagePanel::parsePageMime(QByteArray("tok\n"), token, page));
}

// ---------------------------------------------------------------------------
// GitHub #325 — drag-source registry lifecycle.
//
// register → stable token; re-register same doc → SAME token; resolve → the
// live Document*; unregister → resolve returns nullptr (the closed-source-
// window safety path that keeps MainWindow::dropEvent from dereferencing a
// gone document); distinct documents get distinct, non-colliding tokens.
// ---------------------------------------------------------------------------
TEST(PagePanelCrossDocRegistry, RegisterResolveUnregisterLifecycle)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PagePanelFixture a;
    PagePanelFixture b;
    if (!makeFixture(a) || !makeFixture(b))
        GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    Document* docA = a.doc.get();
    Document* docB = b.doc.get();
    ASSERT_NE(docA, docB);

    // A null document yields no token and resolves to nothing.
    EXPECT_TRUE(PagePanel::registerDragSource(nullptr).isEmpty());

    const QString tokenA = PagePanel::registerDragSource(docA);
    ASSERT_FALSE(tokenA.isEmpty());

    // Idempotent: a second register of the SAME document returns the SAME token.
    EXPECT_EQ(PagePanel::registerDragSource(docA), tokenA)
        << "re-registering a live document must return its existing token";

    // Resolve returns the live Document*.
    EXPECT_EQ(PagePanel::resolveDragSource(tokenA), docA);

    // A distinct document gets a DISTINCT token (serial+pointer scheme, no
    // collision) and resolves to itself.
    const QString tokenB = PagePanel::registerDragSource(docB);
    ASSERT_FALSE(tokenB.isEmpty());
    EXPECT_NE(tokenB, tokenA)
        << "distinct documents must not collide on the same token";
    EXPECT_EQ(PagePanel::resolveDragSource(tokenB), docB);
    EXPECT_EQ(PagePanel::resolveDragSource(tokenA), docA)
        << "registering B must not disturb A's mapping";

    // Unregister A: its token no longer resolves (the safety path); B survives.
    PagePanel::unregisterDragSource(docA);
    EXPECT_EQ(PagePanel::resolveDragSource(tokenA), nullptr)
        << "after unregister, a stale token must resolve to nullptr, never a "
           "dangling pointer";
    EXPECT_EQ(PagePanel::resolveDragSource(tokenB), docB);

    // An unknown token resolves to nullptr; unregister of an unknown doc is a
    // safe no-op.
    EXPECT_EQ(PagePanel::resolveDragSource(QStringLiteral("never-registered")),
              nullptr);
    PagePanel::unregisterDragSource(docB);
    PagePanel::unregisterDragSource(docA);  // double unregister: no-op, no crash
}

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
