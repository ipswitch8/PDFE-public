// -*- coding: utf-8 -*-
//
// GitHub #333 — within-document page-thumbnail reorder.
//
// Before the fix, PageListWidget was IconMode + Static + InternalMove with only
// a currentRow()-delta dropEvent and no dragEnter/dragMove overrides. That
// combination never ACCEPTS the same-document drag, so it bubbled to
// MainWindow::dropEvent whose same-document branch deliberately no-ops it: no
// drop indicator, no reorder, no undo entry. The fix handles the internal drag
// explicitly in PageListWidget (accepting it so it does not bubble) and computes
// an explicit insert-before target instead of the unreliable currentRow() delta.
//
// These tests exercise the NEW decision logic, not widget configuration:
//
//  * InternalDropDispatch_* drive the real performInternalReorder() dispatch via
//    the PagePanel::simulateInternalDrop seam and assert pageMoved(from,to) fires
//    with MovePageCommand's insert-before semantics, then apply that exact
//    (from,to) through a real MovePageCommand and assert the DOCUMENT order
//    changed to match (distinct per-page identities, compared as a whole).
//
//  * RealDropEventReordersInternalDrag posts a genuine QDragEnterEvent +
//    QDropEvent carrying this document's own #325 token to the list and asserts
//    the drag is ACCEPTED (so it will not bubble to the same-document no-op) and
//    pageMoved fires. On the pre-fix code the list never accepts the internal
//    drag and emits nothing, so this reddens against the old PagePanel.
//
// It does NOT weaken or delete tests/test_ui_page_reorder_drag.py (the xfail
// real-OS-gesture test) nor the #326 configuration guard in
// test_page_panel_q_invokable.cpp.

#include <gtest/gtest.h>
#include "ui/PagePanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PageCommands.h"

#include <QApplication>
#include <QListWidget>
#include <QSignalSpy>
#include <QMimeData>
#include <QScopedPointer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QPoint>
#include <QPointF>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

using namespace pdfe::ui;
using namespace pdfe::core;

namespace {

struct ReorderFixture {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<PagePanel> panel;
};

bool makeFixture(ReorderFixture& f, const std::string& pdfName = "test_3page.pdf")
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

// test_3page.pdf shares ONE inherited /MediaBox across all pages (see
// test_page_commands.cpp), so size cannot tell pages apart. Stamp a distinct
// /Rotate on each page so a permutation is observable. Applied through Document
// directly so no undo entry is created.
void stampDistinctIdentities(Document* doc)
{
    for (int i = 0; i < doc->pageCount(); ++i) {
        doc->rotatePage(i, 90 * i);  // -> 0, 90, 180
    }
}

std::vector<int> identities(Document* doc)
{
    std::vector<int> ids;
    for (int i = 0; i < doc->pageCount(); ++i) {
        ids.push_back(doc->pageInfo(i).rotation);
    }
    return ids;
}

} // namespace

// ─── The internal-drop dispatch: insert-before semantics + document reorder ──

TEST(PagePanelInternalReorder, InternalDropDispatch_ForwardMoveEmitsAndReorders)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    ReorderFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    stampDistinctIdentities(f.doc.get());
    const std::vector<int> before = identities(f.doc.get());  // [0, 90, 180]
    const std::set<int> uniq(before.begin(), before.end());
    ASSERT_EQ(uniq.size(), before.size())
        << "page identities must be distinct or no permutation is observable";

    QSignalSpy spy(f.panel.get(), &PagePanel::pageMoved);
    ASSERT_TRUE(spy.isValid());

    // Drag page 0, drop it with the indicator BEFORE index 2. MovePageCommand's
    // convention: remove 0, re-insert at (0 < 2 ? 2-1 : 2) = position 1.
    const int to = f.panel->simulateInternalDrop(/*fromRow=*/0, /*insertBefore=*/2);
    EXPECT_EQ(to, 2) << "the raw insert-before index is handed to pageMoved";

    ASSERT_EQ(spy.count(), 1) << "an order-changing internal drop must emit pageMoved";
    const auto args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), 0);
    EXPECT_EQ(args.at(1).toInt(), 2);

    // Apply the EMITTED (from,to) through a real MovePageCommand and confirm the
    // document order matches the visually-expected result: page 0 lands between
    // the old pages 1 and 2 -> [90, 0, 180].
    pdfe::commands::MovePageCommand cmd(f.doc.get(), args.at(0).toInt(), args.at(1).toInt());
    cmd.execute();
    EXPECT_EQ(identities(f.doc.get()), (std::vector<int>{90, 0, 180}))
        << "the (from,to) the drop emitted did not reorder the document as the "
           "drop indicator promised";
}

TEST(PagePanelInternalReorder, InternalDropDispatch_BackwardMoveReordersToFront)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    ReorderFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    stampDistinctIdentities(f.doc.get());  // [0, 90, 180]

    QSignalSpy spy(f.panel.get(), &PagePanel::pageMoved);

    // Drag page 2, drop with the indicator BEFORE index 0 (to the very front).
    const int to = f.panel->simulateInternalDrop(/*fromRow=*/2, /*insertBefore=*/0);
    EXPECT_EQ(to, 0);
    ASSERT_EQ(spy.count(), 1);
    const auto args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), 2);
    EXPECT_EQ(args.at(1).toInt(), 0);

    pdfe::commands::MovePageCommand cmd(f.doc.get(), 2, 0);
    cmd.execute();
    EXPECT_EQ(identities(f.doc.get()), (std::vector<int>{180, 0, 90}))
        << "dropping page 2 at the front must yield [C, A, B]";
}

TEST(PagePanelInternalReorder, InternalDropDispatch_AppendPastEndReorders)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    ReorderFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    stampDistinctIdentities(f.doc.get());  // [0, 90, 180]

    QSignalSpy spy(f.panel.get(), &PagePanel::pageMoved);

    // Drop page 0 past the last item: insert-before index == pageCount (3).
    const int to = f.panel->simulateInternalDrop(/*fromRow=*/0, /*insertBefore=*/3);
    EXPECT_EQ(to, 3);
    ASSERT_EQ(spy.count(), 1);

    pdfe::commands::MovePageCommand cmd(f.doc.get(), 0, 3);
    cmd.execute();
    EXPECT_EQ(identities(f.doc.get()), (std::vector<int>{90, 180, 0}))
        << "dropping page 0 at the end must yield [B, C, A]";
}

// ─── The no-op guard: a drop that does not change order emits nothing ────────

TEST(PagePanelInternalReorder, InternalDropDispatch_SameSlotDropsAreNoOps)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    ReorderFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    QSignalSpy spy(f.panel.get(), &PagePanel::pageMoved);

    // Dropping page 1 before itself (insert-before 1) or immediately after it
    // (insert-before 2) leaves the order unchanged; both must be suppressed so
    // no empty "Move Page" undo entry is pushed.
    EXPECT_EQ(f.panel->simulateInternalDrop(1, 1), -1);
    EXPECT_EQ(f.panel->simulateInternalDrop(1, 2), -1);
    EXPECT_EQ(spy.count(), 0) << "a no-op drop must not emit pageMoved";

    // A genuine move still fires.
    EXPECT_EQ(f.panel->simulateInternalDrop(1, 0), 0);
    EXPECT_EQ(spy.count(), 1);
}

TEST(PagePanelInternalReorder, InternalDropDispatch_OutOfRangeSourceIsRejected)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    ReorderFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    QSignalSpy spy(f.panel.get(), &PagePanel::pageMoved);
    EXPECT_EQ(f.panel->simulateInternalDrop(-1, 1), -1);
    EXPECT_EQ(f.panel->simulateInternalDrop(99, 1), -1);
    EXPECT_EQ(spy.count(), 0);
}

// ─── The real event path: a same-document QDropEvent is accepted + reorders ──
//
// This posts a genuine QDragEnterEvent/QDropEvent to the list carrying THIS
// document's own #325 token (the payload a real same-panel drag produces).
// On the pre-fix PagePanel the list has no dragEnter override and a
// currentRow()-delta dropEvent, so the drag is never accepted and nothing is
// emitted — this test reddens there. On the fixed code the internal drag is
// recognised by its token, accepted (so it will NOT bubble to the same-document
// no-op), and pageMoved fires.
TEST(PagePanelInternalReorder, RealDropEventReordersInternalDrag)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    ReorderFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    QSignalSpy spy(f.panel.get(), &PagePanel::pageMoved);
    ASSERT_TRUE(spy.isValid());

    // Drive the REAL dragEnter/drop overrides via the panel seam. It builds this
    // document's own #325 payload (so isInternalDrag() must recognise the drag as
    // internal by its token) and calls PageListWidget's actual dragEnterEvent +
    // dropEvent — the same methods, with the same event objects, Qt invokes for a
    // live drag. Qt will not route a manually-sent QDropEvent to a view's
    // protected handlers, so the handlers are called directly; the only thing
    // bypassed is Qt's own event-delivery plumbing, which is not under test.
    //
    // A position well past every item makes dropInsertIndex() resolve to "append"
    // (insert-before == pageCount) regardless of headless layout — a
    // deterministic, order-changing target for a drag starting at row 0. A return
    // of true means both handlers accepted and CONSUMED the drag (it would not
    // bubble to MainWindow::dropEvent's same-document no-op — the #333 defect).
    const bool consumed =
        f.panel->deliverSyntheticInternalDrag(/*fromRow=*/0, QPoint(100000, 100000));
    EXPECT_TRUE(consumed)
        << "a same-document drag must be recognised as internal and consumed by "
           "the list's dragEnter/drop handlers, not bubbled to the same-doc no-op";

    ASSERT_EQ(spy.count(), 1)
        << "a real same-document drop must route through pageMoved";
    const auto args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), 0) << "the dragged source row is carried in the payload";
    EXPECT_NE(args.at(1).toInt(), 0)
        << "the drop target must differ from the source (a real reorder)";
}
