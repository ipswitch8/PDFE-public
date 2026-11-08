// tests/test_select_tool_cross_page_move.cpp
//
// GitHub #343 — dragging a selected object (e.g. a Stamp) so the pointer
// crosses onto a DIFFERENT page crashed PDFE.
//
// Mechanism:
//   SelectTool::onMouseMove deliberately ignores the hover page, so a drag that
//   crosses a page boundary keeps editing the annotation it started on.
//   SelectTool::onMouseRelease, however, used to emit
//       annotationMoved(pageIndex /* = hover/target page */, selectedAnnotIndex_, …)
//   where selectedAnnotIndex_ names the annotation on the SOURCE page. The
//   MainWindow handler then called AnnotationManager::getAnnotation(targetPage,
//   selectedAnnotIndex_) — an index valid on the source page but out of range on
//   the target page — which fz_throws "Annotation index out of range". That
//   exception unwinds out of a Qt mouse-event handler and terminates the process
//   (the same class of crash as GitHub #234).
//
// The fix emits the annotation's OWN page (currentPageIndex_, the page it was
// selected on) so the handler always indexes the correct page; MainWindow also
// bounds-checks before getAnnotation. These tests pin the emitted page index.
//
// Driving the REAL SelectTool press/move/release seam (not a reimplementation)
// is the point: the bug lived entirely in which page index onMouseRelease
// chose to emit.

#include <gtest/gtest.h>

#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QRectF>

#include <filesystem>
#include <memory>

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

class SelectToolCrossPageMoveTest : public ::testing::Test {
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
        ASSERT_GE(document_->pageCount(), 2);

        manager_ = std::make_unique<AnnotationManager>(ctx_);
        page0_ = document_->page(0);
        page1_ = document_->page(1);
        ASSERT_NE(page0_, nullptr);
        ASSERT_NE(page1_, nullptr);

        // Two annotations on page 0. Selecting index 1 is what makes the
        // pre-fix crash vector concrete: page 1 carries NO annotations, so an
        // emitted (targetPage=1, index=1) is out of range there.
        manager_->createAnnotation(page0_.get(), AnnotationType::Stamp,
                                    QRectF(80, 80, 120, 60));
        manager_->createAnnotation(page0_.get(), AnnotationType::Stamp,
                                    QRectF(80, 300, 120, 60));

        tool_ = std::make_unique<SelectTool>(manager_.get());
        tool_->setPage(page0_.get());

        // Prime lastZoom_ so hit-testing tolerances are sane.
        QImage img(800, 1000, QImage::Format_ARGB32);
        QPainter p(&img);
        tool_->onPaintOverlay(p, 0, 1.0f, QPointF(0, 0), 792.0f);
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<AnnotationManager> manager_;
    std::unique_ptr<pdfe::core::Page> page0_;
    std::unique_ptr<pdfe::core::Page> page1_;
    std::unique_ptr<SelectTool> tool_;
};

// Cross-page drag: press on page 0, release while the pointer is over page 1.
// The emitted move MUST carry the SOURCE page (0), not the hover page (1).
TEST_F(SelectToolCrossPageMoveTest, ReleaseOnADifferentPageEmitsTheSourcePage) {
    // Select the second annotation (index 1) on page 0 by its centre.
    const QRectF rect1 = manager_->getAnnotation(page0_.get(), 1).rect;
    const QPointF pressPt = rect1.center();

    QMouseEvent press(QEvent::MouseButtonPress, pressPt, pressPt,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->onMousePress(&press, 0, pressPt);
    ASSERT_EQ(tool_->selectedAnnotIndex(), 1)
        << "precondition: the drag must start with annotation 1 selected on "
           "page 0, else this proves nothing";

    int emittedPage = -999;
    int emittedIndex = -999;
    bool fired = false;
    tool_->annotationMoved = [&](int pg, int idx, QRectF, QRectF) {
        emittedPage = pg;
        emittedIndex = idx;
        fired = true;
    };

    // Move far enough to count as a real move, and release reporting the HOVER
    // page as page 1 (the pointer wandered onto the next page).
    const QPointF releasePt(pressPt.x() + 40, pressPt.y() + 40);
    QMouseEvent release(QEvent::MouseButtonRelease, releasePt, releasePt,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->onMouseRelease(&release, /*pageIndex=hover=*/1, releasePt);

    ASSERT_TRUE(fired) << "the move must have been emitted";
    EXPECT_EQ(emittedPage, 0)
        << "GitHub #343: onMouseRelease emitted the hover/target page instead "
           "of the annotation's own source page; the handler then indexes the "
           "wrong page out of range and crashes";
    EXPECT_EQ(emittedIndex, 1);

    // The emitted (page, index) must resolve — i.e. the MainWindow handler's
    // getAnnotation() would not throw. The source page owns index 1.
    EXPECT_NO_THROW(manager_->getAnnotation(page0_.get(), emittedIndex));
    EXPECT_LT(emittedIndex,
              static_cast<int>(manager_->getAnnotations(page0_.get()).size()));

    // Document the crash vector: on the hover page the emitted index was out of
    // range, which is exactly what used to be dereferenced.
    EXPECT_GE(emittedIndex,
              static_cast<int>(manager_->getAnnotations(page1_.get()).size()))
        << "fixture precondition: page 1 must have fewer annotations than the "
           "emitted index for the pre-fix crash to be reproducible";
}

// Within-page move still works: press and release on the same page emit that
// page, so the ordinary (non-crashing) path is unchanged by the fix.
TEST_F(SelectToolCrossPageMoveTest, SamePageMoveStillEmitsThatPage) {
    const QRectF rect0 = manager_->getAnnotation(page0_.get(), 0).rect;
    const QPointF pressPt = rect0.center();

    QMouseEvent press(QEvent::MouseButtonPress, pressPt, pressPt,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->onMousePress(&press, 0, pressPt);
    ASSERT_EQ(tool_->selectedAnnotIndex(), 0);

    int emittedPage = -999;
    bool fired = false;
    QRectF oldR, newR;
    tool_->annotationMoved = [&](int pg, int, QRectF o, QRectF n) {
        emittedPage = pg;
        oldR = o;
        newR = n;
        fired = true;
    };

    const QPointF releasePt(pressPt.x() + 25, pressPt.y() + 15);
    QMouseEvent release(QEvent::MouseButtonRelease, releasePt, releasePt,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool_->onMouseRelease(&release, 0, releasePt);

    ASSERT_TRUE(fired);
    EXPECT_EQ(emittedPage, 0);
    // The move actually translated the rect (sanity: not a no-op).
    EXPECT_NE(oldR.topLeft(), newR.topLeft());
}

}  // namespace
