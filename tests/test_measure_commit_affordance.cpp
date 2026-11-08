// tests/test_measure_commit_affordance.cpp
//
// many-bugs-2026-11 phase-4 — two measurement-tool defects, each driven
// through the tool's REAL mouse/key seams (not a reimplementation) against a
// real page + AnnotationManager.
//
// GitHub #349 — Measure Distance line shifts up-and-left after mouse-up.
//   The commit path built the Line annotation from a bounding RECT only and
//   never populated the endpoints, so the renderer serialised /L from MuPDF's
//   defaults and the committed line no longer matched the preview (which is
//   drawn straight from the clicked PDF points). The fix stores the actual
//   drag endpoints in AnnotationData::lineStart/lineEnd, which serialize to /L.
//   Oracle: the committed annotation's stored /L (read back via getAnnotation)
//   equals the two input points, with no post-mouse-up offset. Pre-fix the /L
//   is the rect-derived default, so this fails.
//
// GitHub #350 — Perimeter/Area had no discoverable complete/stop and a
//   double-click dumped the raw "[PDFE-MEASURE:...]" marker as the visible
//   text. The fix (a) shows only the human-readable value in /Contents and
//   carries the machine tag in the hidden AnnotationData::measureTag field, and
//   (b) adds an explicit completion seam — Enter/Return finishes, Escape
//   cancels — in addition to the double-click. Oracle: completing via the key
//   seam creates exactly ONE FreeText whose visible contents has no
//   "[PDFE-MEASURE:" marker, while the tag is still recoverable from the hidden
//   field.

#include <gtest/gtest.h>

#include "annotations/AnnotationData.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "measure/MeasureTool.h"
#include "measure/MeasureTypes.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>

#include <filesystem>
#include <memory>
#include <string>

using pdfe::annotations::AnnotationData;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;
using pdfe::measure::MeasureMode;
using pdfe::measure::MeasureTool;

namespace {

class MeasureCommitAffordanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        const std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "fixture not available: " << fixture;
        }
        document_ = pdfe::core::Document::open(context_, fixture);
        if (!document_ || document_->pageCount() < 1) {
            GTEST_SKIP() << "document did not open";
        }
        manager_ = std::make_unique<AnnotationManager>(context_);
        pagePtr_ = document_->page(0);
        ASSERT_NE(pagePtr_, nullptr);
        page_ = pagePtr_.get();
    }

    // Press at a PDF point through the tool's real mouse seam. The event's own
    // position is irrelevant — onMousePress takes the PDF point as a parameter —
    // so a fresh local event per click sidesteps Qt6's non-copyable events.
    void pressAt(MeasureTool& tool, const QPointF& pdfPoint) {
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool.onMousePress(&ev, 0, pdfPoint);
    }

    int annotCount() {
        return static_cast<int>(manager_->getAnnotations(page_).size());
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<AnnotationManager> manager_;
    std::unique_ptr<pdfe::core::Page> pagePtr_;
    pdfe::core::Page* page_ = nullptr;
};

// GitHub #349: the committed Distance line's stored /L must equal the clicked
// endpoints — no offset introduced at mouse-up.
TEST_F(MeasureCommitAffordanceTest, DistanceCommitStoresTheActualDragEndpoints) {
    MeasureTool tool(manager_.get(), MeasureMode::Distance);
    tool.setPage(page_);

    // Two distinct on-page PDF points. Neither is the origin, so the pre-fix
    // rect-derived default (which is not these points) cannot accidentally
    // satisfy the assertion.
    const QPointF p0(100.0, 120.0);
    const QPointF p1(300.0, 260.0);

    int committedIndex = -1;
    AnnotationType committedType = AnnotationType::Unknown;
    tool.annotationCreated = [&](int, AnnotationData data) {
        committedIndex = data.annotIndex;
        committedType = data.type;
    };

    // Distance is a two-click gesture: press at p0, drag, press at p1 (commit).
    pressAt(tool, p0);
    {
        QMouseEvent move(QEvent::MouseMove, QPointF(0, 0), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        tool.onMouseMove(&move, 0, p1);
    }
    pressAt(tool, p1);

    ASSERT_NE(committedIndex, -1) << "the second click must commit a Line annotation";
    EXPECT_EQ(committedType, AnnotationType::Line);

    // Read the STORED /L back out of the annotation — the renderer's source of
    // truth, not the in-memory struct we handed the callback.
    const AnnotationData stored = manager_->getAnnotation(page_, committedIndex);
    EXPECT_NEAR(stored.lineStart.x(), p0.x(), 1e-3)
        << "GitHub #349: stored /L start x drifted from the clicked point";
    EXPECT_NEAR(stored.lineStart.y(), p0.y(), 1e-3)
        << "GitHub #349: stored /L start y drifted from the clicked point";
    EXPECT_NEAR(stored.lineEnd.x(), p1.x(), 1e-3)
        << "GitHub #349: stored /L end x drifted from the clicked point";
    EXPECT_NEAR(stored.lineEnd.y(), p1.y(), 1e-3)
        << "GitHub #349: stored /L end y drifted from the clicked point — the "
           "line was built from the bounding rect only and /L fell back to "
           "defaults";
}

// GitHub #350: Perimeter completes via the explicit Enter/Return seam; the
// visible contents is the human value with no marker, and the machine tag is
// still recoverable from the hidden field. Exactly one annotation is created.
TEST_F(MeasureCommitAffordanceTest, PerimeterCompletesViaKeySeamWithHiddenTag) {
    MeasureTool tool(manager_.get(), MeasureMode::Perimeter);
    tool.setPage(page_);

    bool created = false;
    AnnotationData committed;
    tool.annotationCreated = [&](int, AnnotationData data) {
        created = true;
        committed = data;
    };

    const int before = annotCount();

    // Accumulate three points for the perimeter.
    const QPointF pts[3] = {QPointF(100, 100), QPointF(300, 120),
                            QPointF(260, 300)};
    for (const QPointF& p : pts) {
        pressAt(tool, p);
    }

    // Finish through the DISCOVERABLE keyboard affordance (Enter), proving the
    // completion seam exists and is wired — not the old double-click-only path.
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    tool.onKeyPress(&enter);
    EXPECT_TRUE(enter.isAccepted())
        << "GitHub #350: the completion key must be consumed, not passed to the "
           "canvas as a scroll/paginate";

    ASSERT_TRUE(created)
        << "GitHub #350: Enter/Return must complete the perimeter measurement";
    EXPECT_EQ(committed.type, AnnotationType::FreeText);

    // (a) VISIBLE contents — read back from the stored annotation — must be the
    // human value with NO machine marker.
    const AnnotationData stored =
        manager_->getAnnotation(page_, committed.annotIndex);
    EXPECT_EQ(stored.contents.find("[PDFE-MEASURE:"), std::string::npos)
        << "GitHub #350: the raw marker leaked into the visible text box: '"
        << stored.contents << "'";
    EXPECT_FALSE(stored.contents.empty())
        << "the visible text box must still show the human-readable value";

    // (b) The machine tag survives in the HIDDEN field and is still parseable.
    EXPECT_NE(committed.measureTag.find("[PDFE-MEASURE:"), std::string::npos)
        << "GitHub #350: the machine tag must be recoverable from the hidden "
           "measureTag field";
    std::string mode;
    float value = -1.0f;
    std::string unit;
    EXPECT_TRUE(MeasureTool::parseContents(committed.measureTag, mode, value, unit))
        << "the hidden tag must parse back to mode/value/unit";
    EXPECT_EQ(mode, "perimeter");

    // No stray second (literal-text) annotation box: exactly one was added.
    EXPECT_EQ(annotCount(), before + 1)
        << "GitHub #350: completing the measurement must not spawn a stray "
           "literal-text annotation box";
}

// GitHub #350: Escape is a discoverable cancel — it discards the in-progress
// measurement and creates nothing.
TEST_F(MeasureCommitAffordanceTest, EscapeCancelsPerimeterWithoutCommitting) {
    MeasureTool tool(manager_.get(), MeasureMode::Perimeter);
    tool.setPage(page_);

    bool created = false;
    tool.annotationCreated = [&](int, AnnotationData) { created = true; };

    const int before = annotCount();
    const QPointF pts[3] = {QPointF(100, 100), QPointF(300, 120),
                            QPointF(260, 300)};
    for (const QPointF& p : pts) {
        pressAt(tool, p);
    }

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool.onKeyPress(&esc);
    EXPECT_TRUE(esc.isAccepted());

    EXPECT_FALSE(created) << "Escape must not commit an annotation";
    EXPECT_EQ(annotCount(), before) << "Escape must leave the page unchanged";

    // After cancelling, a subsequent Enter has nothing to finish.
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    tool.onKeyPress(&enter);
    EXPECT_FALSE(created);
    EXPECT_EQ(annotCount(), before);
}

}  // namespace
