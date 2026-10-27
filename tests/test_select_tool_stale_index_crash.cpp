// tests/test_select_tool_stale_index_crash.cpp
//
// GitHub #234 — PDFE terminated on an unhandled C++ exception unwinding out of
// QWidget::paintEvent.
//
// Chain, identical to the byte offset in three crash dumps taken 2026-09-19:
//
//   SelectTool::onPaintOverlay        (re-queries the model for each selected index)
//     AnnotationManager::getAnnotation  (getNthAnnot -> fz_throw "index out of range")
//       MuPdfContext::execute           (converts any fz error to MuPdfException)
//         -> unwinds through PdfCanvas::paintEvent -> Qt terminates the process
//
// Deleting an annotation shifts every higher index down by one, so a tool that
// cached indices is left naming something that no longer exists. SelectTool's
// OWN delete path is careful (clears the selection first, deletes in reverse
// index order); deletions that originate elsewhere -- the comment panel, a menu
// action, an undo -- never reached it, because AnnotationTool had no
// annotations-changed notification at all.
//
// WHY THE PAINT-PATH ASSERTION IS THE LOAD-BEARING ONE. Adding
// onAnnotationsChanged() fixes the paths we know about. It cannot fix the ones
// nobody has wired yet, and this defect exists precisely because such a path
// was missed. So the invariant under test is the stronger one: *a paint handler
// that re-queries a model it does not own must survive that model having moved*
// -- with no notification, no cooperation, nothing.
//
// These tests therefore deliberately do NOT call onAnnotationsChanged() before
// painting. Calling it would test the notification and leave the crash itself
// unguarded: the test would pass even if the try/catch in onPaintOverlay were
// deleted. A test that cannot fail when the fix is removed is decoration.

#include <gtest/gtest.h>

#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QRectF>

#include <filesystem>
#include <memory>

#include "annotations/AnnotationData.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "annotations/SelectTool.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;
using pdfe::annotations::SelectTool;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

class SelectToolStaleIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        const std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "fixture not available: " << fixture;
        }
        document_ = Document::open(ctx_, fixture);
        ASSERT_NE(document_, nullptr);
        ASSERT_GT(document_->pageCount(), 0);

        manager_ = std::make_unique<AnnotationManager>(ctx_);
        pageOwner_ = document_->page(0);
        page_ = pageOwner_.get();
        ASSERT_NE(page_, nullptr);

        // Two annotations, so deleting the second leaves index 1 out of range
        // while index 0 stays valid -- that asymmetry is what lets the pruning
        // assertion distinguish "dropped the bad one" from "dropped everything".
        manager_->createAnnotation(page_, AnnotationType::Square,
                                   QRectF(50, 50, 100, 40));
        manager_->createAnnotation(page_, AnnotationType::Square,
                                   QRectF(50, 150, 100, 40));
    }

    // A painter over a real image: onPaintOverlay draws, and we want the draw
    // calls to actually execute rather than short-circuit on a null device.
    QImage canvas_{400, 400, QImage::Format_ARGB32};
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<AnnotationManager> manager_;
    std::shared_ptr<pdfe::core::Page> pageOwner_;
    pdfe::core::Page* page_ = nullptr;
};

// THE CRASH. Without the guard in onPaintOverlay this throws MuPdfException,
// which in the real application unwinds out of QWidget::paintEvent and kills
// the process.
TEST_F(SelectToolStaleIndexTest, PaintingASelectionDeletedElsewhereDoesNotThrow) {
    SelectTool tool(manager_.get());
    tool.setPage(page_);
    tool.selectByIndex(0, 1);
    ASSERT_EQ(tool.selectedAnnotIndices().size(), 1u)
        << "precondition: the tool must actually hold a selection, otherwise "
           "the paint path short-circuits and this test proves nothing";

    // Delete it the way the comment panel / a menu action would -- straight
    // through the manager, with the tool never told.
    manager_->deleteAnnotation(page_, 1);

    QPainter painter(&canvas_);
    EXPECT_NO_THROW({
        tool.onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);
    }) << "GitHub #234: a stale annotation index must not throw out of the "
          "paint path -- Qt terminates the process when an exception unwinds "
          "through an event handler";
}

// The stale index must be PRUNED, not merely skipped. Skipping and keeping it
// would re-throw (and be re-caught) on every repaint, turning one dead index
// into an unbounded stream of exceptions hidden behind a catch.
TEST_F(SelectToolStaleIndexTest, AStaleIndexIsDroppedFromTheSelection) {
    SelectTool tool(manager_.get());
    tool.setPage(page_);
    tool.selectByIndex(0, 1);
    manager_->deleteAnnotation(page_, 1);

    QPainter painter(&canvas_);
    tool.onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);

    EXPECT_TRUE(tool.selectedAnnotIndices().empty())
        << "the index that failed to resolve is still in the selection, so the "
           "next repaint will throw again";
    EXPECT_LT(tool.selectedAnnotIndex(), 0)
        << "the scalar selection must follow the vector to empty, or the rest "
           "of the UI keeps offering actions against a deleted annotation";
}

// A surviving index must still be drawn. This is what stops the fix degrading
// into "catch everything and render nothing" -- the mutant that makes
// onPaintOverlay an early `return` passes both tests above and fails this one.
TEST_F(SelectToolStaleIndexTest, AValidSelectionStillPaintsAfterASiblingIsDeleted) {
    SelectTool tool(manager_.get());
    tool.setPage(page_);
    tool.selectByIndex(0, 0);          // index 0 survives the delete below
    manager_->deleteAnnotation(page_, 1);

    canvas_.fill(Qt::transparent);
    {
        QPainter painter(&canvas_);
        tool.onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);
    }

    bool painted = false;
    for (int y = 0; y < canvas_.height() && !painted; ++y) {
        for (int x = 0; x < canvas_.width(); ++x) {
            if (qAlpha(canvas_.pixel(x, y)) != 0) { painted = true; break; }
        }
    }
    EXPECT_TRUE(painted)
        << "nothing was drawn: the still-valid selection was discarded along "
           "with the stale one";
    EXPECT_EQ(tool.selectedAnnotIndices().size(), 1u)
        << "the valid index must be retained";
}

// The notification hook itself. Separate from the crash tests on purpose: this
// one would still pass if the paint-path guard were removed, so it is NOT
// evidence about the crash -- only about the tool honouring the new contract.
TEST_F(SelectToolStaleIndexTest, OnAnnotationsChangedDropsTheCachedSelection) {
    SelectTool tool(manager_.get());
    tool.setPage(page_);
    tool.selectByIndex(0, 1);
    ASSERT_FALSE(tool.selectedAnnotIndices().empty());

    tool.onAnnotationsChanged();

    EXPECT_TRUE(tool.selectedAnnotIndices().empty());
    EXPECT_LT(tool.selectedAnnotIndex(), 0);
}

}  // namespace
