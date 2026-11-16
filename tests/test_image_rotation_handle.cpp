// -*- coding: utf-8 -*-
// PHASE1_HARDENING: skip=0 expect_ge=3 no_throw=0  (2026-11-14)
//
// GitHub #391 (BUG 2) — placed-image rotation handle.
//
// Three defects the pre-fix handle exhibited:
//   (a) the computed angle was quantized to the nearest 90 deg in both the
//       live preview (ImageTool.cpp:361) and the commit (:404), so the handle
//       could never emit an arbitrary angle.
//   (b) the handle emitted a per-drag DELTA, but ImageContentEditor::rotateImage
//       applies its argument as an ABSOLUTE orientation from upright, and the
//       press never recorded the image's current rotation as a base. So the
//       first drag from upright worked (base 0 => delta == absolute) but every
//       later drag applied the snapped delta as a fresh absolute => the ~180
//       jump reported in #391.
//   (c) after a commit MainWindow only called refreshPage(), which does not
//       re-enumerate the ImageTool overlay, so paint() kept drawing the stale
//       pre-rotation orientation (baseDeg reads the cached rotationRadians).
//
// These tests drive the REAL ImageTool event API and the REAL
// ImageContentEditor::rotateImage path (plus the setPage/selectImage re-sync
// the fix wires into MainWindow), so they discriminate the fixed behaviour
// from the pre-fix snap/delta/stale-overlay bugs rather than re-implementing
// the logic under test.
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

// Plain math-convention rotation of a point about a centre — mirrors the
// production rotateAbout(). Tool coordinates are fitz Y-down (GitHub #373):
// a placement whose rotationRadians is theta (CCW-positive Y-up convention)
// maps local -> page by a rotation of MINUS theta in this frame, so a handle
// at a visually-rotated location is obtained by rotating the local handle by
// -rotationRadians (same relation the production hit-test inverse uses).
QPointF rotateAbout(QPointF p, QPointF c, float radians) {
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    const QPointF d = p - c;
    return c + QPointF(d.x() * cs - d.y() * sn, d.x() * sn + d.y() * cs);
}

double angleDegAbout(QPointF p, QPointF c) {
    const QPointF d = p - c;
    return std::atan2(static_cast<double>(d.y()), static_cast<double>(d.x()))
           * 180.0 / 3.14159265358979323846;
}

} // namespace

class ImageRotationHandleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        qRegisterMetaType<std::string>("std::string");
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());
        pdfPath_ = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
    }

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
// (a) GitHub #391(a) — an arbitrary drag angle must NOT be snapped to 90.
// Pre-fix: both preview and commit did std::round(delta/90)*90, so a ~37 deg
// drag emitted 0 (or, past the halfway point, 90). Post-fix the real angle
// flows through unchanged.
// ---------------------------------------------------------------------------
TEST_F(ImageRotationHandleTest, ArbitraryAngle_NotSnappedTo90) {
    ASSERT_TRUE(std::filesystem::exists(pdfPath_)) << "Fixture missing: " << pdfPath_;
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    tool_->setPage(page.get(), doc.get());
    auto images = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(images.empty());
    const PageImageInfo first = images[0];
    ASSERT_NEAR(first.rotationRadians, 0.0f, 1e-3f) << "Fixture image starts upright";

    const QRectF bbox = first.boundingBox;
    const QPointF center = bbox.center();

    press(center);
    release(center);
    ASSERT_TRUE(tool_->hasSelection());

    float gotAngle = 0.0f;
    int fires = 0;
    QObject::connect(tool_.get(), &ImageTool::rotateRequested,
        [&](const std::string&, float angle) { ++fires; gotAngle = angle; });

    // Rotation handle sits visually above the image (fitz Y-down): start vector
    // points straight up (startAngle = -90 deg).
    const QPointF rotHandle(center.x(), bbox.top() - 20.0);
    press(rotHandle);

    // Drag clockwise to ~ -53 deg so the raw delta is ~ +37 deg — plainly not a
    // multiple of 90, and well past neither 0 nor 90.
    const QPointF releasePt(center.x() + 60.0, center.y() - 80.0);
    moveTo(releasePt);
    release(releasePt);

    const double startDeg = angleDegAbout(rotHandle, center);
    const double endDeg = angleDegAbout(releasePt, center);
    const float expected = static_cast<float>(endDeg - startDeg);  // ~ +36.87

    ASSERT_EQ(fires, 1) << "Rotation drag must emit exactly one rotate command";
    EXPECT_NEAR(gotAngle, expected, 2.0f)
        << "Arbitrary drag angle must flow through unsnapped; got "
        << gotAngle << " expected " << expected;
    // Regression guard: a re-introduced 90-snap would force ~90 or ~0.
    EXPECT_GT(std::abs(gotAngle - 90.0f), 5.0f) << "Must not snap to 90";
    EXPECT_GT(std::abs(gotAngle - 0.0f), 5.0f) << "Must not snap to 0";

    page.reset();
    doc.reset();
}

// ---------------------------------------------------------------------------
// (b) GitHub #391(b) — the CORE second-drag regression. On an already-rotated
// image a small drag must emit the ABSOLUTE orientation base + delta, NOT the
// bare delta and NOT ~180.
//
// Pre-fix: the handle emits the per-drag delta, snapped to 90, which
// rotateImage then applies as a fresh absolute from upright. A ~50 deg drag
// snaps to 90 and is applied as absolute 90 — ignoring the 30 deg the image
// was already at. This test asserts the emitted value is base + delta.
// ---------------------------------------------------------------------------
TEST_F(ImageRotationHandleTest, SecondDrag_IsAdditiveFromCurrentRotation_Not180) {
    // The tool's event handlers require a bound page/doc, so open the fixture
    // and set it first; then inject a synthetic PRE-ROTATED image via the test
    // seam (setImagesForTest preserves the page/doc setPage bound). This lets
    // the drag exercise the real mousePress/Move/Release path against a known
    // non-zero base rotation.
    ASSERT_TRUE(std::filesystem::exists(pdfPath_)) << "Fixture missing: " << pdfPath_;
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    tool_->setPage(page.get(), doc.get());

    // Pre-rotated placement: 120x80 centred at (300,400). rotationRadians is
    // CCW-positive (Y-up); the tool's current VISUAL clockwise angle is
    // -rotationRadians in degrees. Choose -30 deg of radians => +30 deg visual.
    const QPointF center(300.0, 400.0);
    const float baseRadians = -kPi / 6.0f;                 // visual CW = +30 deg
    const float baseVisualCW = -baseRadians * 180.0f / kPi; // = +30 deg

    PageImageInfo info;
    info.xobjectName = "ImPre";
    info.pixelWidth = 120;
    info.pixelHeight = 80;
    info.displayWidth = 120.0f;
    info.displayHeight = 80.0f;
    info.rotationRadians = baseRadians;
    info.boundingBox = QRectF(center.x() - 60.0, center.y() - 40.0, 120.0, 80.0);

    tool_->setImagesForTest({info}, 0);
    ASSERT_TRUE(tool_->hasSelection());
    ASSERT_NEAR(tool_->selectedImage().rotationRadians, baseRadians, 1e-4f);

    float gotAngle = 0.0f;
    int fires = 0;
    QObject::connect(tool_.get(), &ImageTool::rotateRequested,
        [&](const std::string&, float angle) { ++fires; gotAngle = angle; });

    // The rotation handle now lives at its VISUALLY rotated position: the local
    // handle (straight above the centre) rotated by -rotationRadians.
    const QPointF localRot(center.x(), info.boundingBox.top() - 20.0);
    const QPointF pressPt = rotateAbout(localRot, center, -baseRadians);
    press(pressPt);

    // A moderate further drag. End the pointer so the raw delta is ~ +50 deg.
    const double startDeg = angleDegAbout(pressPt, center);
    const double endDeg = startDeg + 50.0;
    const double endRad = endDeg * 3.14159265358979323846 / 180.0;
    const QPointF releasePt(
        center.x() + 100.0 * std::cos(endRad),
        center.y() + 100.0 * std::sin(endRad));
    moveTo(releasePt);
    release(releasePt);

    const double delta = angleDegAbout(releasePt, center) - startDeg;  // ~ +50
    const float expectedAbsolute = baseVisualCW + static_cast<float>(delta); // ~ +80

    ASSERT_EQ(fires, 1) << "Second drag must still emit exactly one rotate command";
    EXPECT_NEAR(gotAngle, expectedAbsolute, 2.0f)
        << "Emitted angle must be ABSOLUTE (base + delta); got " << gotAngle
        << " expected " << expectedAbsolute << " (base " << baseVisualCW
        << " + delta " << delta << ")";
    // Discriminators: pre-fix emitted the bare snapped delta (round(50/90)*90
    // = 90). Must be neither the bare delta nor ~180.
    EXPECT_GT(std::abs(gotAngle - static_cast<float>(delta)), 5.0f)
        << "Must not emit the bare per-drag delta (ignores current rotation)";
    EXPECT_GT(std::abs(gotAngle - 180.0f), 5.0f)
        << "Must not jump to ~180 (#391 second-drag symptom)";

    page.reset();
    doc.reset();
}

// ---------------------------------------------------------------------------
// (c) GitHub #391(c) — the selection outline must follow the image after a
// committed rotate. paint() draws the frame at baseDeg = -rotationRadians of
// the CACHED selected image, so the overlay only moves if the tool is
// re-enumerated after the commit. This proves the re-sync (setPage +
// selectImage) the fix wires into MainWindow updates the cached orientation
// paint() consumes, and that WITHOUT it the cache stays stale.
// ---------------------------------------------------------------------------
TEST_F(ImageRotationHandleTest, OutlineFollowsImageAfterCommit) {
    ASSERT_TRUE(std::filesystem::exists(pdfPath_)) << "Fixture missing: " << pdfPath_;
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    tool_->setPage(page.get(), doc.get());
    auto before = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_FALSE(before.empty());
    const PageImageInfo img0 = before[0];
    ASSERT_TRUE(tool_->selectImage(img0.xobjectName));
    ASSERT_NEAR(tool_->selectedImage().rotationRadians, 0.0f, 1e-3f)
        << "Image starts upright, so paint would draw an axis-aligned outline";

    const QPointF c = img0.boundingBox.center();
    // Commit a real 90-degree rotation through the production editor path.
    ASSERT_TRUE(editor_->rotateImage(
        page.get(), doc.get(), img0.xobjectName, 90.0f,
        static_cast<float>(c.x()), static_cast<float>(c.y()),
        img0.displayWidth, img0.displayHeight))
        << "rotateImage must succeed on the fixture image";

    // Pre-resync: the tool still holds the pre-rotation enumeration, so the
    // cached rotation (what paint's baseDeg reads) is stale at ~0 — this is the
    // #391(c) symptom: the outline does NOT follow the image.
    EXPECT_NEAR(tool_->selectedImage().rotationRadians, 0.0f, 1e-3f)
        << "Without a re-sync the cached orientation stays stale (pre-fix behaviour)";

    // The fix: re-enumerate the page and reselect by identity (exactly what
    // MainWindow's rotateRequested handler now does after refreshPage).
    tool_->setPage(page.get(), doc.get());
    ASSERT_TRUE(tool_->selectImage(img0.xobjectName))
        << "Rotated image must remain selectable by its XObject name";

    // Post-resync: the cached rotationRadians — the value paint()'s baseDeg
    // consumes — reflects the committed ~90-degree orientation, so a second
    // paint would draw the outline at the new orientation.
    EXPECT_NEAR(std::abs(tool_->selectedImage().rotationRadians), kPi / 2.0f, 0.1f)
        << "After re-sync the outline orientation must match the committed rotation";

    page.reset();
    doc.reset();
}
