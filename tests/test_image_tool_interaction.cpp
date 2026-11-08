// -*- coding: utf-8 -*-
// PHASE1_HARDENING: skip=0 expect_ge=3 no_throw=0  (2026-11-06)
//
// GitHub #340 / #341 / #342 — interactive image-tool manipulation.
//
// These drive the REAL ImageTool event API (mousePress/Move/Release and the
// #342 hit-test seam) against a REAL enumerated image from the shared fixture
// test_pdf_with_images.pdf, so the production move/rotate/hit-test code paths
// are exercised — not a re-implementation of the logic under test. Only the
// #342 rotated-handle case uses the injection seam (setImagesForTest), because
// a deterministic known-rotation placement is the cleanest way to assert the
// rotated hit-test; a companion test drives a REAL 90-degree rotate through
// ImageContentEditor + re-enumeration to cover the ImageManager bbox/rotation
// derivation itself.
//
// Each test is a real discriminator against the pre-fix behaviour:
//   #340 pre-fix: body-drag release emitted nothing (0 signals).
//   #341 pre-fix: a clockwise drag emitted -90 (opposite the command convention).
//   #342 pre-fix: a handle at a rotated corner hit-tested to None; a rotated CTM
//                 enumerated to a degenerate (~0 width) bounding box.
#include <gtest/gtest.h>
#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QApplication>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QPointF>
#include <QRectF>
#include <filesystem>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

namespace {

constexpr float kPi = 3.14159265358979323846f;

void ensureQApplication() {
    if (!QApplication::instance()) {
        static int argc = 0;
        static char* argv[] = { nullptr };
        static QApplication app(argc, argv);
    }
}

// Forward CCW-positive rotation of a point about a centre — mirrors the
// production rotateAbout() so a test can place a point at an image's VISUALLY
// rotated handle location (then let the tool inverse-rotate it back).
QPointF rotateAbout(QPointF p, QPointF c, float radians) {
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    const QPointF d = p - c;
    return c + QPointF(d.x() * cs - d.y() * sn, d.x() * sn + d.y() * cs);
}

} // namespace

class ImageToolInteractionTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        // resizeRequested/rotateRequested carry a std::string; register it so
        // QSignalSpy can connect and count those emissions.
        qRegisterMetaType<std::string>("std::string");
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());
        pdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
    }

    // Open the fixture, set it on the tool, and return the first enumerated
    // image. The fixture is asserted (not skipped) to carry an image so an
    // empty fixture reddens rather than silently passing.
    PageImageInfo openFixtureAndSelectFirst(std::unique_ptr<Document>& docOut,
                                            std::unique_ptr<Page>& pageOut) {
        EXPECT_TRUE(std::filesystem::exists(pdfPath_))
            << "Fixture missing: " << pdfPath_;
        docOut = Document::open(context_, pdfPath_);
        EXPECT_NE(docOut, nullptr);
        pageOut = docOut->page(0);
        EXPECT_NE(pageOut, nullptr);
        tool_->setPage(pageOut.get(), docOut.get());
        auto images = manager_->enumerateImages(pageOut.get(), docOut.get());
        EXPECT_FALSE(images.empty()) << "Fixture must enumerate at least one image";
        return images.empty() ? PageImageInfo{} : images[0];
    }

    // Event dispatch helpers (named locals so we never take the address of a
    // temporary QMouseEvent). All at scale 1.0.
    void press(QPointF p) {
        QMouseEvent e(QEvent::MouseButtonPress, p, p,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mousePressEvent(&e, p, 1.0f);
    }
    void moveTo(QPointF p) {
        QMouseEvent e(QEvent::MouseMove, p, p,
                      Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mouseMoveEvent(&e, p, 1.0f);
    }
    void release(QPointF p) {
        QMouseEvent e(QEvent::MouseButtonRelease, p, p,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mouseReleaseEvent(&e, p, 1.0f);
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<ImageTool> tool_;
    std::string pdfPath_;
};

// ---------------------------------------------------------------------------
// GitHub #340 — a body drag must commit the move (pre-fix: 0 signals).
// ---------------------------------------------------------------------------
TEST_F(ImageToolInteractionTest, BodyDrag_EmitsMoveWithTranslatedOrigin) {
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    const PageImageInfo first = openFixtureAndSelectFirst(doc, page);
    ASSERT_FALSE(first.xobjectName.empty());

    const QRectF orig = first.boundingBox;
    const QPointF start = orig.center();
    const QPointF end = start + QPointF(40.0, 30.0);

    // Capture resize-command arguments via a direct-connected lambda (reliable
    // for the std::string-carrying signal), and count via QSignalSpy.
    QSignalSpy resizeSpy(tool_.get(), &ImageTool::resizeRequested);
    int fires = 0;
    std::string gotName;
    float gotW = 0, gotH = 0, gotX = 0, gotY = 0;
    QObject::connect(tool_.get(), &ImageTool::resizeRequested,
        [&](const std::string& n, float w, float h, float x, float y) {
            ++fires; gotName = n; gotW = w; gotH = h; gotX = x; gotY = y;
        });

    press(start);
    ASSERT_TRUE(tool_->hasSelection()) << "Press on the image body must select it";
    moveTo(end);
    release(end);

    // Pre-fix: body-drag release emitted nothing, so this is 0.
    EXPECT_EQ(resizeSpy.count(), 1) << "A real body drag must emit exactly one move/resize command";
    ASSERT_EQ(fires, 1);
    EXPECT_EQ(gotName, first.xobjectName);
    // Size is unchanged by a move.
    EXPECT_NEAR(gotW, static_cast<float>(orig.width()), 0.5f);
    EXPECT_NEAR(gotH, static_cast<float>(orig.height()), 0.5f);
    // Origin is translated by the drag delta (+40, +30).
    EXPECT_NEAR(gotX, static_cast<float>(orig.x()) + 40.0f, 0.5f);
    EXPECT_NEAR(gotY, static_cast<float>(orig.y()) + 30.0f, 0.5f);
}

// A pure click-to-select (no movement) must NOT commit a move — guards the
// kMinMoveDistance threshold the fix introduced.
TEST_F(ImageToolInteractionTest, BodyClickWithoutDrag_EmitsNoMove) {
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    const PageImageInfo first = openFixtureAndSelectFirst(doc, page);
    ASSERT_FALSE(first.xobjectName.empty());

    const QPointF c = first.boundingBox.center();
    QSignalSpy resizeSpy(tool_.get(), &ImageTool::resizeRequested);

    press(c);
    release(c);

    ASSERT_TRUE(tool_->hasSelection());
    EXPECT_EQ(resizeSpy.count(), 0) << "A click with no drag must not commit a move";
}

// ---------------------------------------------------------------------------
// GitHub #341 — a visually-clockwise rotate drag must emit +90 (pre-fix: -90).
// ---------------------------------------------------------------------------
TEST_F(ImageToolInteractionTest, RotateDrag_ClockwiseEmitsPositiveNinety) {
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    const PageImageInfo first = openFixtureAndSelectFirst(doc, page);
    ASSERT_FALSE(first.xobjectName.empty());

    const QRectF bbox = first.boundingBox;
    const QPointF center = bbox.center();

    // Select the image first (handles only hit-test once something is selected).
    press(center);
    release(center);
    ASSERT_TRUE(tool_->hasSelection());

    float gotAngle = 0.0f;
    int fires = 0;
    QObject::connect(tool_.get(), &ImageTool::rotateRequested,
        [&](const std::string&, float angle) { ++fires; gotAngle = angle; });
    QSignalSpy rotateSpy(tool_.get(), &ImageTool::rotateRequested);

    // Press the rotation handle (top-centre, 20px above the top edge at scale 1).
    const QPointF rotHandle(center.x(), bbox.top() - 20.0);
    press(rotHandle);

    // Drag a quarter turn. The press vector points straight "down" from centre
    // (startAngle = -90 deg). Releasing to the left with a slight negative y lands
    // near endAngle = -180 deg, so the clockwise-positive delta (startAngle -
    // endAngle) snaps to +90. Pre-fix (endAngle - startAngle) this was -90.
    const QPointF releasePt(center.x() - 100.0, center.y() - 1.0);
    moveTo(releasePt);
    release(releasePt);

    ASSERT_EQ(rotateSpy.count(), 1) << "Rotation drag must emit exactly one rotate command";
    ASSERT_EQ(fires, 1);
    EXPECT_NEAR(gotAngle, 90.0f, 0.001f)
        << "Clockwise drag must emit +90 (toolbar/command convention), not -90";
}

// ---------------------------------------------------------------------------
// GitHub #342 (part 3/4) — a handle at a rotated corner must be hittable.
// Uses the injection seam for a deterministic 90-degree placement.
// ---------------------------------------------------------------------------
TEST_F(ImageToolInteractionTest, RotatedImage_HandleHittableAtRotatedPosition) {
    // 100x60 placement centred at (300,400), rotated 90 deg CCW.
    const QPointF center(300.0, 400.0);
    PageImageInfo info;
    info.xobjectName = "ImRot";
    info.pixelWidth = 100;
    info.pixelHeight = 60;
    info.displayWidth = 100.0f;
    info.displayHeight = 60.0f;
    info.rotationRadians = kPi / 2.0f;
    info.boundingBox = QRectF(center.x() - 50.0, center.y() - 30.0, 100.0, 60.0);

    tool_->setImagesForTest({info}, 0);
    ASSERT_TRUE(tool_->hasSelection());
    ASSERT_NEAR(tool_->selectedImage().rotationRadians, kPi / 2.0f, 1e-4f);

    // Local (un-rotated) handle positions.
    const QPointF localTR = info.boundingBox.topRight();
    const QPointF localRot(center.x(), info.boundingBox.top() - 20.0);

    // Page-space positions after the placement's +90 rotation.
    const QPointF pageTR = rotateAbout(localTR, center, info.rotationRadians);
    const QPointF pageRot = rotateAbout(localRot, center, info.rotationRadians);

    // Post-fix: the handles live at the rotated positions.
    EXPECT_EQ(tool_->hitTestHandleForTest(pageTR, 1.0f), HandlePosition::TopRight)
        << "Resize corner must be grabbable at its visually-rotated location";
    EXPECT_EQ(tool_->hitTestHandleForTest(pageRot, 1.0f), HandlePosition::Rotation)
        << "Rotation handle must be grabbable at its visually-rotated location";

    // Sensitivity / discriminator: pre-fix the hit-test was axis-aligned, so it
    // would have matched the UN-rotated corner (and MISSED the rotated one above).
    // Post-fix the un-rotated position no longer registers as TopRight.
    EXPECT_NE(tool_->hitTestHandleForTest(localTR, 1.0f), HandlePosition::TopRight)
        << "With rotation applied, the un-rotated corner must no longer be the hit "
           "(pre-fix this WAS TopRight, and the rotated position returned None)";
}

// ---------------------------------------------------------------------------
// GitHub #342 (part 1/2) — a real rotated CTM must enumerate to a non-degenerate
// bounding box and a correct rotationRadians (pre-fix: ~0 width, no rotation).
// ---------------------------------------------------------------------------
TEST_F(ImageToolInteractionTest, RealRotation_EnumeratesOrientedBoundingBox) {
    ASSERT_TRUE(std::filesystem::exists(pdfPath_)) << "Fixture missing: " << pdfPath_;
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto before = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(before.empty());
    const PageImageInfo img0 = before[0];
    ASSERT_NEAR(img0.rotationRadians, 0.0f, 1e-3f) << "Fixture image starts axis-aligned";

    const QPointF c = img0.boundingBox.center();
    // Rotate 90 degrees clockwise (toolbar convention: positive = clockwise).
    ASSERT_TRUE(editor_->rotateImage(
        page.get(), doc.get(), img0.xobjectName, 90.0f,
        static_cast<float>(c.x()), static_cast<float>(c.y()),
        img0.displayWidth, img0.displayHeight))
        << "rotateImage must succeed on the fixture image";

    // The rotate removes the old placement and re-appends a new one, so the
    // rotated image's content-stream order (and thus its enumerated index) can
    // change. Find it by XObject name rather than assuming index 0.
    auto after = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(after.empty());
    PageImageInfo rotated;
    bool found = false;
    for (const auto& im : after) {
        if (im.xobjectName == img0.xobjectName) { rotated = im; found = true; break; }
    }
    ASSERT_TRUE(found) << "Rotated image " << img0.xobjectName
                       << " must still be enumerable after rotation";

    // Pre-fix the bounding box was built from ctm.a/ctm.d only, which collapse to
    // ~0 for a 90-degree CTM; the box must instead carry the full display extent.
    EXPECT_GT(rotated.boundingBox.width(), 10.0)
        << "Rotated placement must not yield a degenerate (near-zero-width) bbox";
    EXPECT_GT(rotated.boundingBox.height(), 10.0);
    EXPECT_NEAR(static_cast<float>(rotated.boundingBox.width()), rotated.displayWidth, 1.0f);
    EXPECT_NEAR(static_cast<float>(rotated.boundingBox.height()), rotated.displayHeight, 1.0f);

    // rotationRadians must reflect the ~90-degree placement (sign is CW in PDF
    // space; a 90-deg clockwise rotate maps to about -pi/2).
    EXPECT_NEAR(std::abs(rotated.rotationRadians), kPi / 2.0f, 0.1f)
        << "A real 90-degree rotation must enumerate a ~90-degree rotationRadians";

    page.reset();
    doc.reset();
}
