// -*- coding: utf-8 -*-
//
// GitHub #394 (weak-oracle-hardening, phase-2) — the MISSING SelectTool
// rotate-handle drag oracle, mirroring tests/test_image_rotation_handle.cpp's
// discriminators but for the annotation SelectTool rotate path.
//
// SelectTool's production rotate logic is already CORRECT:
//   * onMousePress on the rotate handle captures dragStartRotation_ =
//     rotationDegrees_ (SelectTool.cpp:321) — the angle the annotation already
//     carries, not zero.
//   * onMouseMove/onMouseRelease set
//     rotationDegrees_ = dragStartRotation_ + (currentAngle - startAngle)
//     (SelectTool.cpp:402 / :501) — ADDITIVE from the current orientation, with
//     NO 90-degree snapping.
//
// Nothing guarded that against regression. These tests drive the REAL event API
// (onMousePress/onMouseMove/onMouseRelease) through a rotate-handle drag at an
// arbitrary (non-90-multiple) angle starting from an already-rotated annotation,
// and read the result back through the REAL accessors (rotationDegrees() and the
// annotationRotated callback payload). They discriminate:
//   (a) a reintroduced 90-degree snap (final would jump to a multiple of 90),
//   (b) a per-drag-delta-as-absolute regression (final would be the raw delta,
//       ignoring the stored base angle),
//   (c) a reset-to-upright on the second drag (final would ignore drag 1).
//
// The matching src-mutation proof (reintroduce std::round(deg/90)*90 into the
// onMouseRelease rotate branch) is recorded in the phase-2 closeout.

#include <gtest/gtest.h>
#include "annotations/SelectTool.h"
#include "annotations/AnnotationManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <filesystem>
#include <cmath>
#include <memory>

using namespace pdfe::annotations;
using namespace pdfe::core;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Mirror SelectTool::computeRotation EXACTLY: atan2(dx, -dy) in degrees. This
// only computes the geometric angle of a point about the centre — the same
// arithmetic the production hit path uses — it does NOT re-implement the
// product's snap-vs-no-snap or additive decision, which is what the assertions
// below actually test (same approach as test_image_rotation_handle.cpp).
double rotationAngleDeg(QPointF cur, QPointF center) {
    return std::atan2(cur.x() - center.x(), -(cur.y() - center.y()))
           * 180.0 / kPi;
}

// The rotate handle sits at the UNROTATED top-centre minus 25/zoom points
// (hitTestHandle / handleCentroidsDebug). At zoom 1.0 that is (cx, top-25).
QPointF rotateHandlePoint(const QRectF& rect) {
    return QPointF(rect.center().x(), rect.top() - 25.0);
}

// A point `deg` clockwise-from-straight-up around `center` at radius `r`,
// i.e. the inverse of rotationAngleDeg: rotationAngleDeg(pointAtAngle(c,d,r),c)==d.
QPointF pointAtAngle(QPointF center, double deg, double r) {
    const double rad = deg * kPi / 180.0;
    return QPointF(center.x() + r * std::sin(rad),
                   center.y() - r * std::cos(rad));
}

} // namespace

class SelectToolRotationHandleTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_single.pdf";
        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
            manager = std::make_unique<AnnotationManager>(context);
            if (document && document->pageCount() > 0) {
                pagePtr = document->page(0);
                page = pagePtr.get();
            }
        }
    }

    void press(QPointF p) {
        QMouseEvent e(QEvent::MouseButtonPress, p, p,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool->onMousePress(&e, 0, p);
    }
    void moveTo(QPointF p) {
        QMouseEvent e(QEvent::MouseMove, p, p,
                      Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        tool->onMouseMove(&e, 0, p);
    }
    void release(QPointF p) {
        QMouseEvent e(QEvent::MouseButtonRelease, p, p,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool->onMouseRelease(&e, 0, p);
    }

    // Create a FreeText annotation, give it a stored non-zero rotation, build a
    // SelectTool bound to the page, paint once (sets lastZoom_ = 1.0 for the
    // handle hit-test geometry), and click-select it so the tool adopts the
    // stored angle — exactly the setup
    // SelectToolEnhancedTest.SelectingARotatedAnnotationAdoptsItsStoredAngle uses.
    QRectF makeRotatedSelection(double baseDegrees) {
        const QRectF rect(100.0, 100.0, 200.0, 60.0);
        manager->createAnnotation(page, AnnotationType::FreeText, rect);
        auto annots = manager->getAnnotations(page);
        EXPECT_GT(annots.size(), 0u);
        annotIndex = static_cast<int>(annots.size()) - 1;
        QRectF stored = annots[annotIndex].rect;

        manager->setRotation(page, annotIndex, baseDegrees, stored);

        tool = std::make_unique<SelectTool>(manager.get());
        tool->setPage(page);
        QImage image(800, 600, QImage::Format_ARGB32);
        QPainter painter(&image);
        tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 792.0f);
        painter.end();

        auto refreshed = manager->getAnnotations(page);
        QRectF actualRect = refreshed[annotIndex].rect;
        const QPointF centre = actualRect.center();
        press(centre);
        release(centre);
        return actualRect;
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
    std::unique_ptr<AnnotationManager> manager;
    std::unique_ptr<Page> pagePtr;
    Page* page = nullptr;
    std::unique_ptr<SelectTool> tool;
    int annotIndex = -1;
};

// (a) An arbitrary (~37-degree) rotate-handle drag starting from a NON-ZERO base
// rotation must yield base + delta — unsnapped, and NOT the raw delta alone.
TEST_F(SelectToolRotationHandleTest, ArbitraryAngleAdditiveFromNonZeroBase_NotSnapped) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available: " << testPdf.string();
    }

    const double base = 33.0;  // non-zero, non-90-multiple stored angle
    const QRectF rect = makeRotatedSelection(base);
    ASSERT_EQ(tool->selectedAnnotIndex(), annotIndex)
        << "click did not select the annotation";
    ASSERT_NEAR(tool->rotationDegrees(), base, 1e-3)
        << "selection must adopt the annotation's stored angle before the drag";

    double gotDeg = 0.0;
    int fires = 0, gotPage = -1, gotAnnot = -1;
    tool->annotationRotated = [&](int pg, int a, double deg) {
        ++fires; gotPage = pg; gotAnnot = a; gotDeg = deg;
    };

    const QPointF centre = rect.center();
    const QPointF handle = rotateHandlePoint(rect);
    const double startAngle = rotationAngleDeg(handle, centre);  // ~0 (straight up)
    const double delta = 37.0;
    const QPointF endPt = pointAtAngle(centre, startAngle + delta, 60.0);

    press(handle);
    moveTo(endPt);
    release(endPt);

    const double endAngle = rotationAngleDeg(endPt, centre);
    const double expected = base + (endAngle - startAngle);  // ~ 70

    ASSERT_EQ(fires, 1) << "rotate drag must emit exactly one annotationRotated";
    EXPECT_EQ(gotPage, 0);
    EXPECT_EQ(gotAnnot, annotIndex);
    EXPECT_NEAR(gotDeg, expected, 1.0)
        << "rotate must be base + delta; got " << gotDeg << " expected " << expected;
    EXPECT_NEAR(tool->rotationDegrees(), expected, 1.0);

    // Discriminators (mirror test_image_rotation_handle.cpp):
    EXPECT_GT(std::abs(gotDeg - 90.0), 5.0) << "must not snap to 90";
    EXPECT_GT(std::abs(gotDeg - 0.0), 5.0) << "must not snap to 0";
    EXPECT_GT(std::abs(gotDeg - (endAngle - startAngle)), 5.0)
        << "must be base+delta, not the bare per-drag delta (ignores stored base)";
}

// (b) A SECOND consecutive rotate-handle drag on the SAME already-rotated
// selection must accumulate: final == base + delta1 + delta2 — NOT reset to
// upright (delta2 alone) and NOT base-dropped (delta1 + delta2).
TEST_F(SelectToolRotationHandleTest, SecondConsecutiveDragIsAdditive) {
    if (!document || !page) {
        GTEST_SKIP() << "Test PDF not available: " << testPdf.string();
    }

    const double base = 33.0;
    const QRectF rect = makeRotatedSelection(base);
    ASSERT_EQ(tool->selectedAnnotIndex(), annotIndex);
    ASSERT_NEAR(tool->rotationDegrees(), base, 1e-3);

    double gotDeg = 0.0;
    int fires = 0;
    tool->annotationRotated = [&](int, int, double deg) { ++fires; gotDeg = deg; };

    const QPointF centre = rect.center();
    const QPointF handle = rotateHandlePoint(rect);
    const double startAngle = rotationAngleDeg(handle, centre);  // ~0

    // Drag 1: +37 deg -> ~70.
    const double delta1 = 37.0;
    const QPointF end1 = pointAtAngle(centre, startAngle + delta1, 60.0);
    press(handle);
    moveTo(end1);
    release(end1);
    ASSERT_EQ(fires, 1);
    const double afterFirst = base + (rotationAngleDeg(end1, centre) - startAngle);
    ASSERT_NEAR(tool->rotationDegrees(), afterFirst, 1.0)
        << "first drag did not land additively before the second drag";

    // Drag 2: +41 deg on the SAME selection -> ~111 (accumulated).
    const double delta2 = 41.0;
    const QPointF end2 = pointAtAngle(centre, startAngle + delta2, 60.0);
    press(handle);  // handle geometry unchanged (rect is unchanged by rotation)
    moveTo(end2);
    release(end2);
    ASSERT_EQ(fires, 2) << "second drag must emit a second annotationRotated";

    const double drag2Angle = rotationAngleDeg(end2, centre) - startAngle;
    const double expectedFinal = afterFirst + drag2Angle;  // ~ 111

    EXPECT_NEAR(gotDeg, expectedFinal, 1.0)
        << "final angle must accumulate both drags; got " << gotDeg
        << " expected " << expectedFinal;
    EXPECT_NEAR(tool->rotationDegrees(), expectedFinal, 1.0);

    // Discriminators:
    EXPECT_GT(std::abs(gotDeg - (base + drag2Angle)), 5.0)
        << "second drag lost drag 1 (final == base + delta2 only)";
    EXPECT_GT(std::abs(gotDeg - drag2Angle), 5.0)
        << "second drag reset to upright (final == delta2 alone)";
    EXPECT_GT(std::abs(gotDeg - (delta1 + drag2Angle)), 5.0)
        << "second drag dropped the stored base (final == delta1 + delta2)";
    EXPECT_GT(std::abs(gotDeg - 90.0), 5.0) << "must not snap to 90";
    EXPECT_GT(std::abs(gotDeg - 180.0), 5.0) << "must not snap to 180";
}
