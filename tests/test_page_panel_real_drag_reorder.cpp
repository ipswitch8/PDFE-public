// -*- coding: utf-8 -*-
//
// GitHub #335 — a REAL page-thumbnail drag reorder must reorder the document
// AND leave the panel in sync, with no clearOrRemove() model deletion.
//
// Why #333's test could not catch this: PagePanelInternalReorder's
// RealDropEventReordersInternalDrag drives the drop through
// deliverSyntheticInternalDrag, which calls the protected dropEvent DIRECTLY. A
// real gesture instead goes through QAbstractItemView::mouseMoveEvent →
// startDrag() → QDrag::exec(). The break #335 reports lives on that path:
// before the fix PageListWidget had no startDrag override, so Qt's
// QListView::startDrag → QIconModeViewBase::filterStartDrag ran, and because our
// dropEvent accepts the internal drop without delegating to
// QAbstractItemView::dropEvent, Qt's dd->dropEventMoved stayed false — so on the
// MoveAction return filterStartDrag executed dd->clearOrRemove()
// (qlistview.cpp:2915-2917 → qabstractitemview.cpp:4557 model->removeRows),
// silently deleting the dragged page; refreshPages() also rebuilt the list
// re-entrantly from inside exec(). Net: "nothing happens" / desync.
//
// These tests go through the REAL startDrag() override (via the
// PagePanel::deliverRealDragReorder seam, which selects the source row and
// invokes startDrag; the override's exec is replaced by a deterministic drop
// delivery so no blocking OS drag loop is entered). They assert BOTH that the
// document reordered AND that panelItemCount() stays == pageCount() — the count
// invariant is the assertion that would catch the clearOrRemove desync.

#include <gtest/gtest.h>
#include "ui/PagePanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PageCommands.h"

#include <QApplication>
#include <QListWidget>
#include <QSignalSpy>
#include <QPoint>
#include <QCoreApplication>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

using namespace pdfe::ui;
using namespace pdfe::core;

namespace {

struct DragFixture {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<PagePanel> panel;
};

bool makeFixture(DragFixture& f, const std::string& pdfName = "test_3page.pdf")
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

// test_3page.pdf shares ONE inherited /MediaBox across all pages, so size cannot
// tell pages apart. Stamp a distinct /Rotate per page so a permutation is
// observable. Applied through Document directly so no undo entry is created.
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

// ─── The real initiation path: reorder happens, panel stays in sync ──────────
//
// Wires pageMoved exactly as MainWindow does (MovePageCommand + refreshPages),
// so the within-document reorder and the re-entrant panel rebuild both run
// against the drag loop, then drives the REAL startDrag() override.
TEST(PagePanelRealDragReorder, RealStartDragReordersDocumentAndKeepsPanelInSync)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    DragFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    stampDistinctIdentities(f.doc.get());
    const std::vector<int> before = identities(f.doc.get());  // [0, 90, 180]
    const std::set<int> uniqBefore(before.begin(), before.end());
    ASSERT_EQ(uniqBefore.size(), before.size())
        << "page identities must be distinct or no permutation is observable";

    Document* doc = f.doc.get();
    PagePanel* panel = f.panel.get();

    // Mimic MainWindow::onPageMoved → onDocumentStructureChanged: apply the move
    // to the document and refresh the panel. This runs synchronously from inside
    // the (simulated) QDrag::exec() loop, so refreshPages() must defer its list
    // rebuild to the next event-loop turn rather than clear() the list under the
    // drag.
    QObject::connect(panel, &PagePanel::pageMoved, panel,
                     [doc, panel](int from, int to) {
                         pdfe::commands::MovePageCommand cmd(doc, from, to);
                         cmd.execute();
                         panel->refreshPages();
                     });

    QSignalSpy spy(panel, &PagePanel::pageMoved);
    ASSERT_TRUE(spy.isValid());

    // A drop far past the last item resolves to "append" (insert-before ==
    // pageCount) regardless of headless layout: a deterministic, order-changing
    // target for a drag starting at row 0.
    const bool consumed = panel->deliverRealDragReorder(/*fromRow=*/0,
                                                         QPoint(100000, 100000));
    EXPECT_TRUE(consumed)
        << "a real same-document drag must be recognised + consumed as internal "
           "by the startDrag/dropEvent path, not bubbled or dropped";

    ASSERT_EQ(spy.count(), 1) << "a real drag reorder must emit pageMoved once";
    const auto args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), 0);
    EXPECT_EQ(args.at(1).toInt(), 3) << "append == insert-before pageCount";

    // The document actually reordered (catches "the gesture does nothing"):
    // remove page 0, re-insert after the last -> [B, C, A].
    EXPECT_EQ(identities(doc), (std::vector<int>{90, 180, 0}))
        << "the real drag did not reorder the document";

    // Flush the deferred panel rebuild, then assert the panel is in sync. A
    // clearOrRemove() deletion (the #335 defect) would leave the panel short a
    // row or the document short a page — this count invariant catches it.
    QCoreApplication::processEvents();
    EXPECT_EQ(panel->panelItemCount(), doc->pageCount())
        << "panel item count desynced from the document — a page was deleted "
           "(clearOrRemove) or the list rebuild corrupted the count";

    // No page was lost: the post-drag multiset of identities equals the original.
    const std::vector<int> after = identities(doc);
    const std::set<int> uniqAfter(after.begin(), after.end());
    EXPECT_EQ(uniqAfter, uniqBefore)
        << "a page identity disappeared — the dragged page was deleted, not moved";
    EXPECT_EQ(doc->pageCount(), static_cast<int>(before.size()))
        << "page count changed across a reorder — a reorder must not add/remove pages";
}

// ─── Sensitivity control: the count invariant above is NOT vacuous ───────────
//
// Reproduces, purely in the test using the public list API, the one extra thing
// the pre-fix base path did that the fix removes: clearOrRemove()'s
// model->removeRow on the dragged source row (qabstractitemview.cpp:4571) after
// the drop. If that tail ran, panelItemCount() would drop below pageCount(). This
// proves the primary test's count assertion has teeth — it is exactly what would
// redden if anyone reinstated the base QListView::startDrag path.
TEST(PagePanelRealDragReorder, ClearOrRemoveTailWouldDesyncPanel_SensitivityControl)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    DragFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    Document* doc = f.doc.get();
    PagePanel* panel = f.panel.get();

    auto* list = panel->findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(panel->panelItemCount(), doc->pageCount());

    // Simulate the clearOrRemove() model deletion the base path would run.
    list->model()->removeRow(0);

    EXPECT_LT(panel->panelItemCount(), doc->pageCount())
        << "if the base clearOrRemove() tail ran, the panel desyncs from the "
           "document — this is what the fix prevents and the primary test asserts "
           "against";
}
