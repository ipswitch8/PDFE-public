// -*- coding: utf-8 -*-
// PHASE1_HARDENING: skip=0 expect_ge=21 no_throw=0  (2026-11-17)
//
// Moving or resizing a ROTATED placed image must keep its rotation (GitHub
// issue: "rotated image reverts to 0 degrees when moved").
//
// Pre-fix chain:
//   * ImageTool::mouseReleaseEvent committed a body drag (and a resize-handle
//     drag) as resizeRequested(name, w, h, x, y): the image's UPRIGHT rect,
//     with no orientation;
//   * ImageContentEditor::resizeImage removed the placement and re-appended
//     `q w 0 0 h e f cm /Im Do Q`, an axis-aligned CTM, so the content lost
//     its rotation;
//   * MainWindow's resizeRequested handler did not re-enumerate the tool (the
//     rotate handler does, GitHub #391(c)), so the overlay kept drawing the old
//     rotated frame and hid the regression.
//
// These tests drive the REAL ImageTool events and the REAL editor, then read
// the placement back by re-enumerating the page, so they test what is on the
// page rather than what the tool believes.
#include <gtest/gtest.h>
#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "image/ImageCommands.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "ui/TestDialogSeam.h"
#include <QAction>
#include <optional>
#include <QApplication>
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

// The fitz Y-down position of a point that is visually rotated with the image
// (see test_image_rotation_handle.cpp: local -> page is a rotation of MINUS
// rotationRadians in this frame).
QPointF rotateAbout(QPointF p, QPointF c, float radians) {
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    const QPointF d = p - c;
    return c + QPointF(d.x() * cs - d.y() * sn, d.x() * sn + d.y() * cs);
}

struct Resize {
    std::string name;
    float w = 0, h = 0, x = 0, y = 0, angle = 0;
    int fires = 0;
};

} // namespace

class ImageMoveKeepsRotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        qRegisterMetaType<std::string>("std::string");
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());
        const std::string path = std::string(TEST_FIXTURES_DIR) + "/test_pdf_with_images.pdf";
        ASSERT_TRUE(std::filesystem::exists(path)) << "Fixture missing: " << path;
        doc_ = Document::open(context_, path);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);
        auto images = manager_->enumerateImages(page_.get(), doc_.get());
        ASSERT_FALSE(images.empty());
        img0_ = images[0];
        ASSERT_NEAR(img0_.rotationRadians, 0.0f, 1e-3f) << "Fixture image starts upright";
    }
    void TearDown() override {
        tool_.reset();
        page_.reset();
        doc_.reset();
    }

    void press(QPointF p) {
        QMouseEvent e(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mousePressEvent(&e, p, 1.0f);
    }
    void moveTo(QPointF p) {
        QMouseEvent e(QEvent::MouseMove, p, p, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mouseMoveEvent(&e, p, 1.0f);
    }
    void release(QPointF p) {
        QMouseEvent e(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mouseReleaseEvent(&e, p, 1.0f);
    }

    // Rotate image 0 to `visualCwDeg` through the production editor, then bind
    // and select it in the tool exactly as MainWindow's rotate handler does.
    PageImageInfo rotateAndSelect(float visualCwDeg) {
        const QPointF c = img0_.boundingBox.center();
        EXPECT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), img0_.xobjectName, visualCwDeg,
                                         static_cast<float>(c.x()), static_cast<float>(c.y()),
                                         img0_.displayWidth, img0_.displayHeight));
        tool_->setPage(page_.get(), doc_.get());
        EXPECT_TRUE(tool_->selectImage(img0_.xobjectName));
        return tool_->selectedImage();
    }

    // The placement as it is now ON THE PAGE.
    PageImageInfo onPage() {
        for (const auto& i : manager_->enumerateImages(page_.get(), doc_.get())) {
            if (i.xobjectName == img0_.xobjectName) return i;
        }
        ADD_FAILURE() << "image " << img0_.xobjectName << " vanished from the page";
        return {};
    }

    // Commit what the tool emitted, through the production editor.
    void commit(const Resize& r) {
        ASSERT_EQ(r.fires, 1) << "the drag must emit exactly one resize/move";
        ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), r.name, r.w, r.h, r.x, r.y, r.angle));
    }

    void capture() {
        QObject::connect(tool_.get(), &ImageTool::resizeRequested,
            [this](const std::string& n, float w, float h, float x, float y, float a) {
                last_.name = n; last_.w = w; last_.h = h; last_.x = x; last_.y = y; last_.angle = a;
                ++last_.fires;
            });
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<ImageTool> tool_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    PageImageInfo img0_;
    Resize last_;
};

// (a) The reported bug: rotate, then drag the body. The content must keep its
// rotation, and its centre must move by the drag.
TEST_F(ImageMoveKeepsRotationTest, MoveKeepsRotationAndMovesCentre) {
    const PageImageInfo rotated = rotateAndSelect(37.0f);
    ASSERT_GT(std::abs(rotated.rotationRadians), 0.5f) << "precondition: the rotate committed";
    const QPointF c0 = rotated.boundingBox.center();

    capture();
    const QPointF delta(60.0, 45.0);
    press(c0);
    moveTo(c0 + delta);
    release(c0 + delta);
    commit(last_);

    const PageImageInfo after = onPage();
    EXPECT_NEAR(after.rotationRadians, rotated.rotationRadians, 1e-3f)
        << "moving a rotated image reset its rotation (was " << rotated.rotationRadians
        << " rad, now " << after.rotationRadians << ")";
    EXPECT_NEAR(after.boundingBox.center().x(), (c0 + delta).x(), 0.05);
    EXPECT_NEAR(after.boundingBox.center().y(), (c0 + delta).y(), 0.05);
    EXPECT_NEAR(after.displayWidth, rotated.displayWidth, 0.05f);
    EXPECT_NEAR(after.displayHeight, rotated.displayHeight, 0.05f);
}

// (b) Resize-handle drag on a rotated image keeps the rotation too.
TEST_F(ImageMoveKeepsRotationTest, ResizeHandleKeepsRotation) {
    const PageImageInfo rotated = rotateAndSelect(-25.0f);
    const QRectF bb = rotated.boundingBox;
    capture();
    // The bottom-right handle, at its visually rotated position, dragged outwards.
    const QPointF handle = rotateAbout(bb.bottomRight(), bb.center(), -rotated.rotationRadians);
    press(handle);
    moveTo(handle + QPointF(30.0, 20.0));
    release(handle + QPointF(30.0, 20.0));
    commit(last_);

    const PageImageInfo after = onPage();
    EXPECT_NEAR(after.rotationRadians, rotated.rotationRadians, 1e-3f)
        << "resizing a rotated image reset its rotation";
    EXPECT_NEAR(after.displayWidth, last_.w, 0.05f);
    EXPECT_NEAR(after.displayHeight, last_.h, 0.05f);
}

// (c) No regression for upright images: the move is still a plain
// translation with an axis-aligned CTM.
TEST_F(ImageMoveKeepsRotationTest, UprightMoveIsStillAxisAligned) {
    tool_->setPage(page_.get(), doc_.get());
    ASSERT_TRUE(tool_->selectImage(img0_.xobjectName));
    const QPointF c0 = img0_.boundingBox.center();
    capture();
    press(c0);
    moveTo(c0 + QPointF(-40.0, 25.0));
    release(c0 + QPointF(-40.0, 25.0));
    EXPECT_NEAR(last_.angle, 0.0f, 1e-6f) << "an upright image must emit angle 0";
    commit(last_);

    const PageImageInfo after = onPage();
    EXPECT_NEAR(after.ctm[1], 0.0f, 1e-4f);
    EXPECT_NEAR(after.ctm[2], 0.0f, 1e-4f);
    EXPECT_NEAR(after.ctm[0], img0_.displayWidth, 0.05f);
    EXPECT_NEAR(after.ctm[3], img0_.displayHeight, 0.05f);
    EXPECT_NEAR(after.boundingBox.center().x(), c0.x() - 40.0, 0.05);
    EXPECT_NEAR(after.boundingBox.center().y(), c0.y() + 25.0, 0.05);
}

// (c') The editor's angle-0 output is unchanged from the axis-aligned form, so
// every existing caller that passes no angle is unaffected.
TEST_F(ImageMoveKeepsRotationTest, EditorDefaultAngleKeepsAxisAlignedCtm) {
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), img0_.xobjectName,
                                     100.0f, 50.0f, 72.0f, 144.0f));
    const PageImageInfo after = onPage();
    EXPECT_NEAR(after.ctm[0], 100.0f, 1e-3f);
    EXPECT_NEAR(after.ctm[1], 0.0f, 1e-4f);
    EXPECT_NEAR(after.ctm[2], 0.0f, 1e-4f);
    EXPECT_NEAR(after.ctm[3], 50.0f, 1e-3f);
    EXPECT_NEAR(after.boundingBox.x(), 72.0, 0.01);
    EXPECT_NEAR(after.boundingBox.y(), 144.0, 0.01);
}

// (d) Undo of the move restores the rotated placement, and redo re-applies
// the rotated move.
TEST_F(ImageMoveKeepsRotationTest, UndoAndRedoOfMoveKeepRotation) {
    const PageImageInfo rotated = rotateAndSelect(52.0f);
    const QPointF c0 = rotated.boundingBox.center();
    capture();
    press(c0);
    moveTo(c0 + QPointF(30.0, -20.0));
    release(c0 + QPointF(30.0, -20.0));
    ASSERT_EQ(last_.fires, 1);

    ResizeImageCommand cmd(manager_.get(), editor_.get(), page_.get(), doc_.get(),
                           last_.name, last_.w, last_.h, last_.x, last_.y, last_.angle);
    cmd.execute();
    const PageImageInfo moved = onPage();
    EXPECT_NEAR(moved.rotationRadians, rotated.rotationRadians, 1e-3f);

    cmd.undo();
    const PageImageInfo undone = onPage();
    EXPECT_NEAR(undone.rotationRadians, rotated.rotationRadians, 1e-3f);
    EXPECT_NEAR(undone.boundingBox.center().x(), c0.x(), 0.05);
    EXPECT_NEAR(undone.boundingBox.center().y(), c0.y(), 0.05);

    cmd.redo();
    const PageImageInfo redone = onPage();
    EXPECT_NEAR(redone.rotationRadians, rotated.rotationRadians, 1e-3f);
    EXPECT_NEAR(redone.boundingBox.center().x(), c0.x() + 30.0, 0.05);
    EXPECT_NEAR(redone.boundingBox.center().y(), c0.y() - 20.0, 0.05);
}

// (e) After a committed move the overlay's orientation (what paint() reads)
// equals the content's. With the re-sync MainWindow now performs after a
// resize/move, the tool's cached selection is re-read from the page.
TEST_F(ImageMoveKeepsRotationTest, OverlayMatchesContentAfterMove) {
    const PageImageInfo rotated = rotateAndSelect(64.0f);
    const QPointF c0 = rotated.boundingBox.center();
    capture();
    press(c0);
    moveTo(c0 + QPointF(-35.0, 30.0));
    release(c0 + QPointF(-35.0, 30.0));
    commit(last_);

    // MainWindow's resizeRequested handler: refresh, then re-enumerate and
    // reselect by identity.
    ASSERT_TRUE(resyncImageToolSelection(*tool_, page_.get(), doc_.get(), img0_.xobjectName));

    const PageImageInfo content = onPage();
    EXPECT_NEAR(tool_->selectedImage().rotationRadians, content.rotationRadians, 1e-4f)
        << "the overlay and the content disagree on the rotation";
    EXPECT_NEAR(content.rotationRadians, rotated.rotationRadians, 1e-3f)
        << "the content lost its rotation";
    EXPECT_NEAR(tool_->selectedImage().boundingBox.center().x(), content.boundingBox.center().x(), 0.05);
}

// ---------------------------------------------------------------------------
// Class-level: a placement edit must not drop a property it was not asked to
// change. Every route that commits a move or resize, at a spread of angles
// including the atan2 wrap near +/-180 and exact quarter turns: the rotation
// read back from the page is the rotation before the edit, and the size on the
// page is the size requested.
// ---------------------------------------------------------------------------
enum class Route { BodyMove, ResizeHandle, ResizeAction };

struct EditCase {
    Route route;
    float visualCwDeg;
};

class ImageEditKeepsRotationTest : public ImageMoveKeepsRotationTest,
                                   public ::testing::WithParamInterface<EditCase> {};

namespace {
float angularDistance(float a, float b) {
    float d = std::fmod(std::abs(a - b), 2.0f * kPi);
    return d > kPi ? 2.0f * kPi - d : d;
}
} // namespace

TEST_P(ImageEditKeepsRotationTest, RotationSurvivesEveryMoveAndResizeRoute) {
    const EditCase p = GetParam();
    const PageImageInfo rotated = rotateAndSelect(p.visualCwDeg);
    const QRectF bb = rotated.boundingBox;
    capture();

    switch (p.route) {
    case Route::BodyMove:
        press(bb.center());
        moveTo(bb.center() + QPointF(22.0, -17.0));
        release(bb.center() + QPointF(22.0, -17.0));
        break;
    case Route::ResizeHandle: {
        const QPointF h = rotateAbout(bb.bottomRight(), bb.center(), -rotated.rotationRadians);
        press(h);
        moveTo(h + QPointF(15.0, 10.0));
        release(h + QPointF(15.0, 10.0));
        break;
    }
    case Route::ResizeAction:
        // What MainWindow::onImageResize emits: half size at the same origin,
        // with the image's current orientation.
        emit tool_->resizeRequested(rotated.xobjectName, rotated.displayWidth * 0.5f,
                                    rotated.displayHeight * 0.5f,
                                    static_cast<float>(bb.x()), static_cast<float>(bb.y()),
                                    visualClockwiseDegrees(rotated));
        break;
    }
    commit(last_);

    const PageImageInfo after = onPage();
    EXPECT_LT(angularDistance(after.rotationRadians, rotated.rotationRadians), 1e-3f)
        << "route " << static_cast<int>(p.route) << " at " << p.visualCwDeg
        << " deg: rotation " << rotated.rotationRadians << " became " << after.rotationRadians;
    EXPECT_NEAR(after.displayWidth, last_.w, 0.05f);
    EXPECT_NEAR(after.displayHeight, last_.h, 0.05f);
}

INSTANTIATE_TEST_SUITE_P(
    AllRoutesAndAngles, ImageEditKeepsRotationTest,
    ::testing::Values(
        EditCase{Route::BodyMove, 37.0f},     EditCase{Route::BodyMove, -90.0f},
        EditCase{Route::BodyMove, 135.0f},    EditCase{Route::BodyMove, 179.5f},
        EditCase{Route::BodyMove, -179.5f},   EditCase{Route::BodyMove, 0.5f},
        EditCase{Route::ResizeHandle, 37.0f}, EditCase{Route::ResizeHandle, 90.0f},
        EditCase{Route::ResizeHandle, -135.0f},
        EditCase{Route::ResizeAction, 37.0f}, EditCase{Route::ResizeAction, 180.0f},
        EditCase{Route::ResizeAction, -60.0f}));

// (e') The re-sync is what makes the overlay agree with the page. The Resize
// action changes the size ON THE PAGE without touching the tool's cached
// selection, so only a real re-read gives the overlay the committed size.
TEST_F(ImageMoveKeepsRotationTest, OverlayIsReReadFromThePageAfterResizeAction) {
    const PageImageInfo rotated = rotateAndSelect(41.0f);
    capture();
    emit tool_->resizeRequested(rotated.xobjectName, rotated.displayWidth * 0.5f,
                                rotated.displayHeight * 0.5f,
                                static_cast<float>(rotated.boundingBox.x()),
                                static_cast<float>(rotated.boundingBox.y()),
                                visualClockwiseDegrees(rotated));
    commit(last_);
    ASSERT_NEAR(tool_->selectedImage().displayWidth, rotated.displayWidth, 1e-3f)
        << "precondition: before the re-sync the tool still holds the pre-edit size";

    ASSERT_TRUE(resyncImageToolSelection(*tool_, page_.get(), doc_.get(), img0_.xobjectName));

    const PageImageInfo content = onPage();
    EXPECT_NEAR(tool_->selectedImage().displayWidth, content.displayWidth, 1e-3f);
    EXPECT_NEAR(tool_->selectedImage().displayHeight, content.displayHeight, 1e-3f);
    EXPECT_NEAR(tool_->selectedImage().rotationRadians, content.rotationRadians, 1e-4f);
    EXPECT_NEAR(content.displayWidth, rotated.displayWidth * 0.5f, 0.05f);
}


// ---------------------------------------------------------------------------
// End to end through MainWindow's own handlers: insert an image, rotate it,
// drag it, then use Image > Resize. Each commit runs the production
// rotateRequested / resizeRequested handlers, and the image is read back from
// the window's document. The overlay (the tool's selection, what paint() draws)
// must agree with the page after every step; for the Resize action that is
// only true if MainWindow re-reads the tool after the commit.
// ---------------------------------------------------------------------------
namespace {

std::string findUiFixture(const char* name) {
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"}) {
        const std::string candidate = std::string(prefix) + name;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    const std::string fromDir = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return std::filesystem::exists(fromDir) ? fromDir : std::string();
}

void settleEvents() {
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

class InsertingMainWindow : public pdfe::ui::MainWindow {
protected:
    std::optional<InsertImageChoice> promptInsertImageOptions(const QString&, QSize, QSizeF) override {
        InsertImageChoice choice;
        choice.targetRect = QRectF(120.0, 160.0, 200.0, 100.0);
        choice.jpegQuality = 80;
        choice.stripExif = true;
        choice.maintainAspectRatio = false;
        return choice;
    }
};

PageImageInfo placementOnPage(Document* doc, const std::string& name) {
    ImageManager mgr(doc->context());
    auto page = doc->page(0);
    for (const auto& i : mgr.enumerateImages(page.get(), doc))
        if (i.xobjectName == name) return i;
    ADD_FAILURE() << "image " << name << " is not on the page";
    return {};
}

} // namespace

TEST(ImageMoveKeepsRotationMainWindowTest, RotateMoveAndResizeThroughTheWindowKeepRotation) {
    ensureQApplication();
    qRegisterMetaType<std::string>("std::string");
    const std::string pdf = findUiFixture("test_single.pdf");
    const std::string png = findUiFixture("test_image_rgb.png");
    ASSERT_FALSE(pdf.empty()) << "fixture test_single.pdf not found";
    ASSERT_FALSE(png.empty()) << "fixture test_image_rgb.png not found";

    InsertingMainWindow mw;
    mw.openFile(QString::fromStdString(pdf));
    settleEvents();
    Document* doc = mw.documentForTesting();
    ASSERT_NE(doc, nullptr);
    auto* canvas = mw.findChild<pdfe::ui::PdfCanvas*>();
    ASSERT_NE(canvas, nullptr);

    pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(png));
    auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
    ASSERT_NE(insert, nullptr);
    insert->trigger();
    settleEvents();
    ImageTool* tool = canvas->imageTool();
    ASSERT_NE(tool, nullptr);
    ASSERT_TRUE(tool->hasSelection());
    const std::string name = tool->selectedImage().xobjectName;

    // Rotate through MainWindow's rotateRequested handler.
    emit tool->rotateRequested(name, 37.0f);
    settleEvents();
    const PageImageInfo rotated = placementOnPage(doc, name);
    ASSERT_GT(std::abs(rotated.rotationRadians), 0.5f) << "precondition: the rotate committed";

    // Drag the body with the tool's real mouse handlers; the commit runs
    // through MainWindow's resizeRequested handler.
    const QPointF c0 = tool->selectedImage().boundingBox.center();
    const QPointF delta(50.0, 30.0);
    auto mouse = [&](QEvent::Type t, QPointF pt) {
        QMouseEvent e(t, pt, pt, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        if (t == QEvent::MouseButtonPress) tool->mousePressEvent(&e, pt, 1.0f);
        else if (t == QEvent::MouseMove) tool->mouseMoveEvent(&e, pt, 1.0f);
        else tool->mouseReleaseEvent(&e, pt, 1.0f);
    };
    mouse(QEvent::MouseButtonPress, c0);
    mouse(QEvent::MouseMove, c0 + delta);
    mouse(QEvent::MouseButtonRelease, c0 + delta);
    settleEvents();

    const PageImageInfo moved = placementOnPage(doc, name);
    EXPECT_NEAR(moved.rotationRadians, rotated.rotationRadians, 1e-3f)
        << "GitHub #416: moving the rotated image reset its rotation";
    EXPECT_NEAR(moved.boundingBox.center().x(), (c0 + delta).x(), 0.1);
    EXPECT_NEAR(moved.boundingBox.center().y(), (c0 + delta).y(), 0.1);
    ASSERT_EQ(canvas->imageTool(), tool);
    EXPECT_NEAR(tool->selectedImage().rotationRadians, moved.rotationRadians, 1e-4f)
        << "after the move the outline and the image disagree on the rotation";

    // Image > Resize: halves the image. Only a re-read of the tool after the
    // commit gives the overlay the new size.
    auto* resize = mw.findChild<QAction*>(QStringLiteral("imageResizeAction"));
    ASSERT_NE(resize, nullptr);
    ASSERT_TRUE(resize->isEnabled());
    resize->trigger();
    settleEvents();

    const PageImageInfo resized = placementOnPage(doc, name);
    EXPECT_NEAR(resized.rotationRadians, rotated.rotationRadians, 1e-3f)
        << "Image > Resize reset the rotation";
    EXPECT_NEAR(resized.displayWidth, moved.displayWidth * 0.5f, 0.1f);
    EXPECT_NEAR(tool->selectedImage().displayWidth, resized.displayWidth, 1e-3f)
        << "after Image > Resize the outline still shows the old size: MainWindow did "
           "not re-read the image tool from the page";
    EXPECT_NEAR(tool->selectedImage().rotationRadians, resized.rotationRadians, 1e-4f);
}

// The re-sync must work when the name it is given is a reference INTO the
// tool's own image list. That is what every signal handler receives: the tool
// emits img.xobjectName, and Qt passes it by reference. setPage clears and
// refills that list, so a helper that used the reference after setPage read
// freed memory and lost the selection after every real drag (found by
// RotateMoveAndResizeThroughTheWindowKeepRotation; GitHub #416). In a normal
// build the freed bytes usually still hold the name, so this test documents the
// contract and goes red under AddressSanitizer; the end-to-end test above is
// the one that goes red in a plain build (flip F4).
TEST_F(ImageMoveKeepsRotationTest, ResyncSurvivesANameThatPointsIntoTheToolsOwnList) {
    rotateAndSelect(23.0f);
    const std::string expected = tool_->selectedImage().xobjectName;
    const std::string& intoTheList = tool_->selectedImage().xobjectName;
    EXPECT_TRUE(resyncImageToolSelection(*tool_, page_.get(), doc_.get(), intoTheList))
        << "the re-sync lost the selection when given a name owned by the tool";
    ASSERT_TRUE(tool_->hasSelection());
    EXPECT_EQ(tool_->selectedImage().xobjectName, expected);
}
