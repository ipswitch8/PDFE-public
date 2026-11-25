// -*- coding: utf-8 -*-
//
// ui-tweaks-2 phase-2: the add-image flow must not drag the scanned page.
//
// User report: on a scanned page, Insert Image activates the image tool and
// selects the new image (MainWindow::onInsertImage). A press-drag OUTSIDE the
// new image's box then selected the full-page scan and moved, resized or
// rotated it. ImageTool::hitTestImage hit-tested every placement, and on a
// scanned page every point of the page is inside the scan's box, so the
// "clicked outside any image - deselect" branch could never be reached.
//
// The fix (ImageTool.h, isPageBackgroundImage): a placement that covers
// essentially the whole visible page is the page's BACKGROUND. A click does
// not pick it up (it deselects instead) unless it is already the selection;
// an explicit selectImage/selectPlacement still selects it.
//
// Two further defects found on the way (phase2-investigation.md):
//   * PdfCanvas dispatched presses on ANY page to the image tool, which
//     hit-tested them against its OWN page's images; and it measured a drag
//     that left the tool's page against whatever page the pointer was over.
//   * The MainWindow resize/rotate/remove/crop handlers and the image actions
//     committed to canvas_->currentPage() instead of the tool's page.
//
// ORACLES. Placement identity is read back from the page (ImageManager
// enumeration) and compared BYTE-FOR-BYTE (the pageCtm floats), and at the
// window level the undo depth and the page's content stream bytes are compared
// before and after. Every "nothing happened" assertion has a positive control
// in the same file showing the same gesture DOES commit on an ordinary image.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "image/ImageTool.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "support/LiveContentOfPage.h"
#include "ui/TestDialogSeam.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRectF>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;
using pdfe::testsupport::liveContentOfPage;

namespace {

void ensureQApplication()
{
    if (!QApplication::instance()) {
        static int argc = 0;
        static char* argv[] = {nullptr};
        static QApplication app(argc, argv);
    }
}

std::string fixture(const char* name)
{
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"}) {
        const std::string c = std::string(prefix) + name;
        if (std::filesystem::exists(c)) return c;
    }
    const std::string d = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return std::filesystem::exists(d) ? d : std::string();
}

void settle()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

// The inserted image, fitz Y-down points (as the Insert Image dialog gives it).
const QRectF kInserted(100.0, 150.0, 200.0, 100.0);
// A second, ordinary image on the same scanned page.
const QRectF kOther(350.0, 420.0, 120.0, 80.0);
// A point on the scan that is outside both images.
const QPointF kOnScan(450.0, 650.0);

bool sameBytes(const std::array<float, 6>& a, const std::array<float, 6>& b)
{
    return std::memcmp(a.data(), b.data(), sizeof(float) * 6) == 0;
}

std::string describeCtm(const std::array<float, 6>& m)
{
    std::string s = "[";
    for (float v : m) s += std::to_string(v) + " ";
    return s + "]";
}

std::optional<PageImageInfo> findByName(const std::vector<PageImageInfo>& all,
                                        const std::string& name)
{
    for (const auto& i : all) {
        if (i.xobjectName == name) return i;
    }
    return std::nullopt;
}

}  // namespace

// ===========================================================================
// ImageTool on the scanned fixture, with the commits wired to the real editor
// exactly as MainWindow wires them (so a wrong drag really moves the scan).
// ===========================================================================

class ImageScanBackgroundTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());
        const std::string base = fixture("test_scanned.pdf");
        ASSERT_FALSE(base.empty()) << "fixture test_scanned.pdf not found";

        QImage q(80, 40, QImage::Format_RGB32);
        q.fill(QColor(0, 200, 0));
        png_ = tmp_.filePath("green.png").toStdString();
        ASSERT_TRUE(q.save(QString::fromStdString(png_)));

        doc_ = Document::open(context_, base);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);

        const auto before = images();
        ASSERT_EQ(before.size(), 1u) << "precondition: the fixture holds one full-page scan";
        scanName_ = before[0].xobjectName;
        ASSERT_NEAR(before[0].boundingBox.width(), page_->boundingBox().width(), 1.0)
            << "precondition: the scan spans the page";
        ASSERT_NEAR(before[0].boundingBox.height(), page_->boundingBox().height(), 1.0);

        name_ = insert(kInserted, &occ_);
        bindAsAfterInsert();
        wireCommits();
        scanCtm_ = scan().pageCtm;
    }

    void TearDown() override
    {
        tool_->clearPage();
        page_.reset();
        doc_.reset();
    }

    std::vector<PageImageInfo> images() const
    {
        return manager_->enumerateImages(page_.get(), doc_.get());
    }

    PageImageInfo scan() const
    {
        const auto s = findByName(images(), scanName_);
        EXPECT_TRUE(s.has_value()) << "the scan is no longer on the page";
        return s.value_or(PageImageInfo{});
    }

    PageImageInfo placed(const std::string& name) const
    {
        const auto s = findByName(images(), name);
        EXPECT_TRUE(s.has_value()) << name << " is no longer on the page";
        return s.value_or(PageImageInfo{});
    }

    std::string insert(const QRectF& r, int* occurrence = nullptr)
    {
        InsertImageOptions o;
        o.filePath = png_;
        o.targetRect = r;
        o.maintainAspectRatio = false;
        auto res = manager_->insertImage(page_.get(), doc_.get(), o);
        EXPECT_TRUE(res.success) << res.errorMessage;
        if (occurrence) *occurrence = res.imageInfo.occurrence;
        return res.imageInfo.xobjectName;
    }

    // What MainWindow::onInsertImage leaves: tool bound to the page (fresh
    // enumeration) and the just-inserted placement selected by identity.
    void bindAsAfterInsert()
    {
        tool_->setPage(page_.get(), doc_.get());
        ASSERT_TRUE(tool_->selectPlacement(name_, occ_) || tool_->selectImage(name_))
            << "the inserted image must be selectable";
        ASSERT_EQ(tool_->selectedImage().xobjectName, name_);
    }

    void wireCommits()
    {
        QObject::connect(tool_.get(), &ImageTool::imageSelected,
                         [this](const PageImageInfo& i) {
                             ++selects_;
                             if (i.xobjectName == scanName_) ++scanSelects_;
                             lastSelected_ = i.xobjectName;
                         });
        QObject::connect(tool_.get(), &ImageTool::imageDeselected, [this]() { ++deselects_; });
        QObject::connect(tool_.get(), &ImageTool::removeRequested, [this](int) { ++removes_; });
        QObject::connect(tool_.get(), &ImageTool::resizeRequested,
                         [this](const std::string& n, float w, float h, float x, float y,
                                float deg) {
                             ++resizes_;
                             lastCommitted_ = n;
                             const PageImageInfo sel = tool_->selectedImage();
                             const int occ = sel.xobjectName == n ? sel.occurrence : 0;
                             editor_->resizeImage(page_.get(), doc_.get(), PlacementId{n, occ}, w,
                                                  h, x, y, deg);
                         });
        QObject::connect(tool_.get(), &ImageTool::rotateRequested,
                         [this](const std::string& n, float deg) {
                             ++rotates_;
                             lastCommitted_ = n;
                             const PageImageInfo sel = tool_->selectedImage();
                             const int occ = sel.xobjectName == n ? sel.occurrence : 0;
                             editor_->rotateImage(page_.get(), doc_.get(), PlacementId{n, occ}, deg,
                                                  float(sel.boundingBox.center().x()),
                                                  float(sel.boundingBox.center().y()),
                                                  sel.displayWidth, sel.displayHeight);
                         });
    }

    void press(QPointF p, Qt::KeyboardModifiers m = Qt::NoModifier)
    {
        QMouseEvent e(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton, m);
        tool_->mousePressEvent(&e, p, 1.0f);
    }
    void moveTo(QPointF p, Qt::KeyboardModifiers m = Qt::NoModifier)
    {
        QMouseEvent e(QEvent::MouseMove, p, p, Qt::NoButton, Qt::LeftButton, m);
        tool_->mouseMoveEvent(&e, p, 1.0f);
    }
    void release(QPointF p, Qt::KeyboardModifiers m = Qt::NoModifier)
    {
        QMouseEvent e(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::LeftButton, m);
        tool_->mouseReleaseEvent(&e, p, 1.0f);
    }
    void drag(QPointF from, QPointF to, Qt::KeyboardModifiers m = Qt::NoModifier)
    {
        press(from, m);
        moveTo((from + to) / 2.0, m);
        moveTo(to, m);
        release(to, m);
    }
    void key(int k)
    {
        QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier);
        tool_->keyPressEvent(&e);
    }

    int edits() const { return resizes_ + rotates_ + removes_; }

    // The scan is exactly where the fixture put it and no edit was requested.
    void expectScanUntouched(const char* what)
    {
        const PageImageInfo s = scan();
        EXPECT_TRUE(sameBytes(s.pageCtm, scanCtm_))
            << what << ": the scan's placement changed from " << describeCtm(scanCtm_) << " to "
            << describeCtm(s.pageCtm);
        EXPECT_EQ(resizes_, 0) << what << ": a move/resize was requested (last for '"
                               << lastCommitted_ << "', scan is '" << scanName_ << "')";
        EXPECT_EQ(rotates_, 0) << what << ": a rotation was requested";
        EXPECT_EQ(removes_, 0) << what << ": a removal was requested";
        EXPECT_FALSE(tool_->hasSelection() && tool_->selectedImage().xobjectName == scanName_)
            << what << ": the scan became the selection";
    }

    // Select by click, move, resize by a corner handle, rotate by the rotation
    // handle: each through the real mouse path, each read back from the page.
    void exerciseOrdinaryImage(const std::string& name)
    {
        const PageImageInfo start = placed(name);
        // Select by click (from "something else selected" or nothing).
        if (!(tool_->hasSelection() && tool_->selectedImage().xobjectName == name)) {
            const int s0 = selects_;
            press(start.boundingBox.center());
            release(start.boundingBox.center());
            EXPECT_EQ(selects_, s0 + 1) << name << ": a click did not select it";
            EXPECT_EQ(lastSelected_, name);
        }
        ASSERT_TRUE(tool_->hasSelection());
        ASSERT_EQ(tool_->selectedImage().xobjectName, name);

        // Move.
        const int r0 = resizes_;
        drag(start.boundingBox.center(), start.boundingBox.center() + QPointF(30, 20));
        EXPECT_EQ(resizes_, r0 + 1) << name << ": the body drag committed nothing";
        const PageImageInfo moved = placed(name);
        EXPECT_NEAR(moved.boundingBox.x(), start.boundingBox.x() + 30, 0.5) << name;
        EXPECT_NEAR(moved.boundingBox.y(), start.boundingBox.y() + 20, 0.5) << name;
        ASSERT_TRUE(resyncImageToolPlacement(*tool_, page_.get(), doc_.get(), name,
                                             moved.occurrence));

        // Resize by the bottom-right handle.
        const QRectF b = tool_->selectedImage().boundingBox;
        drag(b.bottomRight(), b.bottomRight() + QPointF(20, 10));
        EXPECT_EQ(resizes_, r0 + 2) << name << ": the handle drag committed nothing";
        const PageImageInfo resized = placed(name);
        EXPECT_NEAR(resized.boundingBox.width(), b.width() + 20, 0.5) << name;
        EXPECT_NEAR(resized.boundingBox.height(), b.height() + 10, 0.5) << name;
        ASSERT_TRUE(resyncImageToolPlacement(*tool_, page_.get(), doc_.get(), name,
                                             resized.occurrence));

        // Rotate by the rotation handle (20 px above the top centre at zoom 1).
        const QRectF c = tool_->selectedImage().boundingBox;
        const QPointF handle(c.center().x(), c.top() - 20.0);
        const int t0 = rotates_;
        drag(handle, handle + QPointF(60, 0));
        EXPECT_EQ(rotates_, t0 + 1) << name << ": the rotation drag committed nothing";
        EXPECT_GT(std::abs(placed(name).rotationRadians), 0.05f) << name << " did not rotate";
        ASSERT_TRUE(resyncImageToolPlacement(*tool_, page_.get(), doc_.get(), name,
                                             placed(name).occurrence));
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<ImageTool> tool_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    std::string png_, name_, scanName_, lastSelected_, lastCommitted_;
    int occ_ = 0;
    std::array<float, 6> scanCtm_{};
    int scanSelects_ = 0;  // imageSelected for the scan (bindAsAfterInsert selects the insert)
    int selects_ = 0, deselects_ = 0, resizes_ = 0, rotates_ = 0, removes_ = 0;
};

// THE REPRODUCTION. Before the fix the press selected the scan and the drag
// moved it by the drag delta (resizeRequested for the scan's name).
TEST_F(ImageScanBackgroundTest, DragOutsideTheInsertedImageNeverMovesTheScan)
{
    ASSERT_FALSE(kInserted.contains(kOnScan)) << "precondition: the press is outside the image";
    drag(kOnScan, kOnScan + QPointF(50, 40));
    expectScanUntouched("press-drag outside the inserted image");
    EXPECT_EQ(selects_, 0) << "the press selected '" << lastSelected_ << "'";
    EXPECT_FALSE(tool_->hasSelection()) << "the press must deselect the inserted image";
    EXPECT_EQ(deselects_, 1) << "the press must deselect (imageDeselected) instead";
}

// The scan's own handle positions (its corners, edge midpoints and the
// rotation handle above its top edge). Each pressed twice: before the fix
// the first press selected the scan, the second grabbed its handle.
TEST_F(ImageScanBackgroundTest, PressesInTheScansHandleAreasLeaveItAlone)
{
    const QRectF s = scan().boundingBox;
    const double in = 3.0;  // inside the page, well within handle tolerance
    const std::vector<QPointF> spots = {
        {s.left() + in, s.top() + in},          {s.center().x(), s.top() + in},
        {s.right() - in, s.top() + in},         {s.left() + in, s.center().y()},
        {s.right() - in, s.center().y()},       {s.left() + in, s.bottom() - in},
        {s.center().x(), s.bottom() - in},      {s.right() - in, s.bottom() - in},
        {s.center().x(), s.top() - 20.0 + in},  // the rotation handle
    };
    for (const QPointF& p : spots) {
        bindAsAfterInsert();
        for (int i = 0; i < 2; ++i) {
            drag(p, p + QPointF(-40, -35));
        }
        expectScanUntouched("press in a scan handle area");
    }
    EXPECT_EQ(scanSelects_, 0) << "a press selected the scan";
}

TEST_F(ImageScanBackgroundTest, ModifiedPressesOnTheScanLeaveItAlone)
{
    for (Qt::KeyboardModifiers m :
         {Qt::KeyboardModifiers(Qt::ShiftModifier), Qt::KeyboardModifiers(Qt::ControlModifier),
          Qt::KeyboardModifiers(Qt::AltModifier), Qt::KeyboardModifiers(Qt::MetaModifier),
          Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::ControlModifier)}) {
        bindAsAfterInsert();
        drag(kOnScan, kOnScan + QPointF(60, -45), m);
        drag(kOnScan, kOnScan + QPointF(60, -45), m);
        expectScanUntouched("modified press-drag on the scan");
    }
    EXPECT_EQ(scanSelects_, 0) << "a modified press selected the scan";
}

TEST_F(ImageScanBackgroundTest, DeleteAfterAClickOnTheScanRemovesNothing)
{
    press(kOnScan);
    release(kOnScan);
    key(Qt::Key_Delete);
    key(Qt::Key_Backspace);
    EXPECT_EQ(removes_, 0) << "Delete after a click on the scan asked to remove an image";
    expectScanUntouched("Delete after a background click");
    EXPECT_EQ(images().size(), 2u);

    // Positive control: with an ordinary image selected, Delete does ask.
    bindAsAfterInsert();
    key(Qt::Key_Delete);
    EXPECT_EQ(removes_, 1) << "oracle: Delete on the selected inserted image must ask to remove";
}

TEST_F(ImageScanBackgroundTest, TheInsertedImageStaysSelectableMovableResizableRotatable)
{
    exerciseOrdinaryImage(name_);
    // Click away (onto the scan): deselects; click it again: reselects.
    press(kOnScan);
    release(kOnScan);
    EXPECT_FALSE(tool_->hasSelection());
    const PageImageInfo now = placed(name_);
    const int s0 = selects_;
    press(now.boundingBox.center());
    release(now.boundingBox.center());
    EXPECT_EQ(selects_, s0 + 1);
    EXPECT_EQ(lastSelected_, name_);
    EXPECT_TRUE(sameBytes(scan().pageCtm, scanCtm_)) << "editing the inserted image moved the scan";
}

TEST_F(ImageScanBackgroundTest, OtherOrdinaryImagesOnTheScannedPageStayEditable)
{
    const std::string other = insert(kOther);
    bindAsAfterInsert();
    exerciseOrdinaryImage(other);
    exerciseOrdinaryImage(name_);
    EXPECT_TRUE(sameBytes(scan().pageCtm, scanCtm_)) << "editing ordinary images moved the scan";
}

TEST_F(ImageScanBackgroundTest, HoverOverTheScanShowsNoGrabCursor)
{
    const std::string other = insert(kOther);
    bindAsAfterInsert();
    EXPECT_EQ(tool_->cursorAt(kOnScan, 1.0f).shape(), Qt::ArrowCursor)
        << "the scan is offered as something to pick up";
    EXPECT_EQ(tool_->cursorAt(kInserted.center(), 1.0f).shape(), Qt::OpenHandCursor);
    EXPECT_EQ(tool_->cursorAt(kOther.center(), 1.0f).shape(), Qt::PointingHandCursor)
        << "an ordinary unselected image must still offer itself";
}

// The exception: a scan selected EXPLICITLY (selectImage) is the selection,
// and the selection is always editable.
TEST_F(ImageScanBackgroundTest, AnExplicitlySelectedScanIsStillEditable)
{
    ASSERT_TRUE(tool_->selectImage(scanName_));
    ASSERT_EQ(tool_->selectedImage().xobjectName, scanName_);
    drag(kOnScan, kOnScan + QPointF(-30, -20));
    EXPECT_EQ(resizes_, 1) << "the explicitly selected scan could not be moved";
    EXPECT_EQ(lastCommitted_, scanName_);
    EXPECT_FALSE(sameBytes(scan().pageCtm, scanCtm_)) << "oracle: the scan did move";
}

// A page without a scan keeps click-to-select, including for an image that is
// large (80% of the page) but not the page's background.
TEST(ImageScanBackgroundPlainPageTest, ANonScanPageStillSelectsItsImagesByClick)
{
    ensureQApplication();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    auto context = std::make_shared<MuPdfContext>();
    ImageManager manager(context);
    ImageTool tool(&manager);
    const std::string base = fixture("test_single.pdf");
    ASSERT_FALSE(base.empty());
    QImage q(80, 40, QImage::Format_RGB32);
    q.fill(QColor(0, 200, 0));
    const std::string png = tmp.filePath("g.png").toStdString();
    ASSERT_TRUE(q.save(QString::fromStdString(png)));
    auto doc = Document::open(context, base);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    const QRectF pb = page->boundingBox();
    ASSERT_TRUE(manager.enumerateImages(page.get(), doc.get()).empty()) << "precondition";

    // 0.9 x 0.89 of the page = 80.1% coverage.
    const QRectF big(pb.left() + pb.width() * 0.05, pb.top() + pb.height() * 0.05,
                     pb.width() * 0.9, pb.height() * 0.89);
    for (const QRectF& r : {kInserted, big}) {
        InsertImageOptions o;
        o.filePath = png;
        o.targetRect = r;
        o.maintainAspectRatio = false;
        auto res = manager.insertImage(page.get(), doc.get(), o);
        ASSERT_TRUE(res.success) << res.errorMessage;
    }
    tool.setPage(page.get(), doc.get());
    int selects = 0;
    QObject::connect(&tool, &ImageTool::imageSelected, [&](const PageImageInfo&) { ++selects; });
    auto click = [&](QPointF p) {
        QMouseEvent pe(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton,
                       Qt::NoModifier);
        tool.mousePressEvent(&pe, p, 1.0f);
        QMouseEvent re(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::LeftButton,
                       Qt::NoModifier);
        tool.mouseReleaseEvent(&re, p, 1.0f);
    };
    // The big image is drawn last, so it is on top everywhere it covers.
    const QPointF onBig(big.right() - 20, big.bottom() - 20);
    click(onBig);
    EXPECT_EQ(selects, 1) << "a large ordinary image was not selected by a click";
    ASSERT_TRUE(tool.hasSelection());
    EXPECT_NEAR(tool.selectedImage().boundingBox.width(), big.width(), 1.0);
    EXPECT_EQ(tool.cursorAt(onBig, 1.0f).shape(), Qt::OpenHandCursor);
    tool.deselect();
    EXPECT_EQ(tool.cursorAt(onBig, 1.0f).shape(), Qt::PointingHandCursor);
    tool.clearPage();
}

// ===========================================================================
// PdfCanvas: a press is for the image tool only on the tool's own page, and a
// drag is measured on the tool's page wherever the pointer goes.
// ===========================================================================

class ImageToolCanvasPageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());
        const std::string base = fixture("test_3page.pdf");
        ASSERT_FALSE(base.empty()) << "fixture test_3page.pdf not found";
        QImage q(80, 40, QImage::Format_RGB32);
        q.fill(QColor(0, 200, 0));
        const std::string png = tmp_.filePath("g.png").toStdString();
        ASSERT_TRUE(q.save(QString::fromStdString(png)));
        doc_ = Document::open(context_, base);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        InsertImageOptions o;
        o.filePath = png;
        o.targetRect = kInserted;
        o.maintainAspectRatio = false;
        auto res = manager_->insertImage(page_.get(), doc_.get(), o);
        ASSERT_TRUE(res.success) << res.errorMessage;
        name_ = res.imageInfo.xobjectName;
        tool_->setPage(page_.get(), doc_.get());
        ASSERT_TRUE(tool_->selectImage(name_));
        ctm_ = tool_->selectedImage().pageCtm;

        QObject::connect(tool_.get(), &ImageTool::resizeRequested,
                         [this](const std::string&, float w, float h, float x, float y, float) {
                             ++resizes_;
                             lastRect_ = QRectF(x, y, w, h);
                         });
        QObject::connect(tool_.get(), &ImageTool::rotateRequested,
                         [this](const std::string&, float) { ++rotates_; });

        canvas_ = std::make_unique<pdfe::ui::PdfCanvas>();
        canvas_->setObjectName(QStringLiteral("imageScanBackgroundTestCanvas"));
        canvas_->resize(900, 1300);
        canvas_->setDocument(doc_.get(), nullptr, nullptr);
        canvas_->setZoom(0.5f);
        canvas_->setImageTool(tool_.get());
        ASSERT_GE(doc_->pageCount(), 2);
    }
    void TearDown() override
    {
        if (canvas_) {
            canvas_->clearImageTool();
            canvas_->clearDocument();
            canvas_.reset();
        }
        tool_->clearPage();
        page_.reset();
        doc_.reset();
    }

    QPoint widgetPos(int pageIndex, QPointF pdf) const
    {
        QPoint origin;
        int w = 0, h = 0;
        EXPECT_TRUE(canvas_->getPageScreenRect(pageIndex, origin, w, h));
        const QPoint p = (QPointF(origin) + pdf * canvas_->zoom()).toPoint();
        EXPECT_TRUE(canvas_->rect().contains(p)) << "the point is off the canvas widget";
        return p;
    }

    void send(QEvent::Type t, QPoint p, Qt::MouseButton b, Qt::MouseButtons bs)
    {
        QMouseEvent e(t, QPointF(p), QPointF(canvas_->mapToGlobal(p)), b, bs, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas_.get(), &e);
    }
    void canvasDrag(QPoint from, QPoint to)
    {
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageTool> tool_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    std::unique_ptr<pdfe::ui::PdfCanvas> canvas_;
    std::string name_;
    std::array<float, 6> ctm_{};
    int resizes_ = 0, rotates_ = 0;
    QRectF lastRect_;
};

// Before the fix the press on page 1 was hit-tested against page 0's images:
// at the same page-local point as page 0's image, it picked that image up.
TEST_F(ImageToolCanvasPageTest, APressOnAnotherPageDoesNotReachTheToolsImages)
{
    ASSERT_EQ(tool_->pageIndex(), 0);
    canvasDrag(widgetPos(1, kInserted.center()), widgetPos(1, kInserted.center() + QPointF(60, 40)));
    EXPECT_EQ(resizes_, 0) << "a drag on page 1 moved page 0's image";
    EXPECT_EQ(rotates_, 0);
    EXPECT_FALSE(tool_->hasSelection()) << "a press on another page must deselect";
    const auto now = findByName(manager_->enumerateImages(page_.get(), doc_.get()), name_);
    ASSERT_TRUE(now.has_value());
    EXPECT_TRUE(sameBytes(now->pageCtm, ctm_));

    // Positive control: the same drag on page 0 does move it.
    ASSERT_TRUE(tool_->selectImage(name_));
    canvasDrag(widgetPos(0, kInserted.center()), widgetPos(0, kInserted.center() + QPointF(60, 40)));
    EXPECT_EQ(resizes_, 1) << "oracle: a drag on the tool's page must move its image";
    EXPECT_NEAR(lastRect_.x(), kInserted.x() + 60, 2.5);
}

// A drag that starts on the tool's page and ends over the next page is a drag
// DOWN past page 0's bottom (held by the page clamp: centre on the bottom
// edge). Before the fix the release was measured on page 1, i.e. as a drag UP.
TEST_F(ImageToolCanvasPageTest, ADragThatLeavesTheToolsPageIsMeasuredOnTheToolsPage)
{
    const QRectF pageBox = page_->mediaBox();
    canvasDrag(widgetPos(0, kInserted.center()), widgetPos(1, QPointF(kInserted.center().x(), 60)));
    ASSERT_EQ(resizes_, 1);
    EXPECT_NEAR(lastRect_.center().y(), pageBox.bottom(), 2.5)
        << "the release over page 1 was measured in page 1's coordinates";
    EXPECT_NEAR(lastRect_.center().x(), kInserted.center().x(), 2.5);
}

// ===========================================================================
// MainWindow: the whole flow, Insert Image -> drag through the canvas.
// ===========================================================================

namespace {

class ScanChoosingMainWindow : public pdfe::ui::MainWindow {
public:
    int promptCount = 0;

protected:
    std::optional<InsertImageChoice> promptInsertImageOptions(const QString&, QSize,
                                                              QSizeF) override
    {
        ++promptCount;
        InsertImageChoice choice;
        choice.targetRect = kInserted;
        choice.jpegQuality = 70;
        choice.stripExif = true;
        choice.maintainAspectRatio = false;
        return choice;
    }
};

}  // namespace

class ImageScanBackgroundWindowTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        image_ = fixture("test_image_rgb.png");
        ASSERT_FALSE(image_.empty()) << "fixture test_image_rgb.png not found";
    }

    // Open @p pdf, insert through the real action (the file dialog answered by
    // the seam, the options by the override), leave the window as the user has
    // it afterwards.
    void openAndInsert(ScanChoosingMainWindow& mw, const char* pdf)
    {
        const std::string path = fixture(pdf);
        ASSERT_FALSE(path.empty()) << pdf << " not found";
        mw.resize(1300, 1100);
        mw.show();
        mw.openFile(QString::fromStdString(path));
        settle();
        doc_ = mw.documentForTesting();
        ASSERT_NE(doc_, nullptr);
        canvas_ = mw.canvasForTesting();
        ASSERT_NE(canvas_, nullptr);
        undo_ = mw.findChild<pdfe::commands::UndoManager*>();
        ASSERT_NE(undo_, nullptr);
        manager_ = std::make_unique<ImageManager>(doc_->context());
        scanBefore_.clear();
        {
            auto p0 = doc_->page(0);
            for (const auto& i : manager_->enumerateImages(p0.get(), doc_)) scanBefore_.push_back(i);
        }
        auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
        ASSERT_NE(insert, nullptr);
        pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(image_));
        insert->trigger();
        settle();
        ASSERT_EQ(mw.promptCount, 1) << "the insert path did not run";
        tool_ = canvas_->imageTool();
        ASSERT_NE(tool_, nullptr) << "the image tool is not active after inserting";
        ASSERT_TRUE(tool_->hasSelection()) << "the inserted image is not selected";
        name_ = tool_->selectedImage().xobjectName;
        ASSERT_EQ(tool_->pageIndex(), 0);
    }

    std::vector<PageImageInfo> imagesOn(int index)
    {
        auto p = doc_->page(index);
        return manager_->enumerateImages(p.get(), doc_);
    }

    QPoint widgetPos(int pageIndex, QPointF pdf) const
    {
        QPoint origin;
        int w = 0, h = 0;
        EXPECT_TRUE(canvas_->getPageScreenRect(pageIndex, origin, w, h));
        const QPoint p = (QPointF(origin) + pdf * canvas_->zoom()).toPoint();
        EXPECT_TRUE(canvas_->rect().contains(p))
            << "the point is off the canvas widget (" << p.x() << "," << p.y() << ")";
        return p;
    }

    void send(QEvent::Type t, QPoint p, Qt::MouseButton b, Qt::MouseButtons bs)
    {
        QMouseEvent e(t, QPointF(p), QPointF(canvas_->mapToGlobal(p)), b, bs, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas_, &e);
        settle();
    }
    void canvasDrag(QPoint from, QPoint to)
    {
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    }

    // Tool-space gestures, for the handler tests (the tool's real signals).
    void toolDrag(QPointF from, QPointF to)
    {
        QMouseEvent p(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
        tool_->mousePressEvent(&p, from, 1.0f);
        QMouseEvent m(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mouseMoveEvent(&m, to, 1.0f);
        QMouseEvent r(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
        tool_->mouseReleaseEvent(&r, to, 1.0f);
        settle();
    }

    // Leave the inserted image selected on page 0 while the canvas's current
    // page is 1 (the user scrolled on before dragging).
    void scrollToPage1(ScanChoosingMainWindow& mw)
    {
        (void)mw;
        canvas_->goToPage(1);
        settle();
        ASSERT_EQ(canvas_->currentPage(), 1) << "precondition: the current page moved on";
        ASSERT_EQ(tool_->pageIndex(), 0) << "precondition: the tool is still on page 0";
        ASSERT_TRUE(tool_->hasSelection());
        ASSERT_EQ(tool_->selectedImage().xobjectName, name_);
        ASSERT_TRUE(imagesOn(1).empty()) << "precondition: page 1 has no images";
    }

    QTemporaryDir tmp_;
    std::string image_, name_;
    Document* doc_ = nullptr;
    pdfe::ui::PdfCanvas* canvas_ = nullptr;
    pdfe::commands::UndoManager* undo_ = nullptr;
    ImageTool* tool_ = nullptr;
    std::unique_ptr<ImageManager> manager_;
    std::vector<PageImageInfo> scanBefore_;
};

// THE REPRODUCTION, end to end: Insert Image on the scanned fixture, then a
// press-drag on the page outside the new image, through PdfCanvas. Nothing may
// change: undo depth, the page's content bytes, the scan's placement. Then
// Delete and the image actions, which before the fix acted on the scan the
// press had selected.
TEST_F(ImageScanBackgroundWindowTest, DragOutsideTheInsertedImageThroughTheCanvasChangesNothing)
{
    ScanChoosingMainWindow mw;
    openAndInsert(mw, "test_scanned.pdf");
    if (HasFatalFailure()) return;
    ASSERT_EQ(scanBefore_.size(), 1u) << "precondition: one scan before the insert";
    const std::string scanName = scanBefore_[0].xobjectName;
    ASSERT_NE(scanName, name_);

    const int depthBefore = undo_->undoStackSize();
    ASSERT_GE(depthBefore, 1) << "the insert itself is on the undo stack";
    const std::string bytesBefore =
        liveContentOfPage(doc_, tmp_.filePath("before.pdf").toStdString(), 0);
    const auto scanCtm = findByName(imagesOn(0), scanName)->pageCtm;
    auto* removeAction = mw.findChild<QAction*>(QStringLiteral("imageRemoveAction"));
    ASSERT_NE(removeAction, nullptr);
    ASSERT_TRUE(removeAction->isEnabled()) << "precondition: the inserted image is selected";

    const QPointF from(450.0, 330.0), to(500.0, 380.0);
    ASSERT_FALSE(kInserted.contains(from));
    canvasDrag(widgetPos(0, from), widgetPos(0, to));

    EXPECT_EQ(undo_->undoStackSize(), depthBefore) << "the drag pushed an undo command";
    EXPECT_EQ(liveContentOfPage(doc_, tmp_.filePath("after.pdf").toStdString(), 0), bytesBefore)
        << "the drag changed the page's content stream";
    const auto scanNow = findByName(imagesOn(0), scanName);
    ASSERT_TRUE(scanNow.has_value());
    EXPECT_TRUE(sameBytes(scanNow->pageCtm, scanCtm))
        << "the scan moved: " << describeCtm(scanCtm) << " -> " << describeCtm(scanNow->pageCtm);
    EXPECT_FALSE(removeAction->isEnabled()) << "the image actions must follow the deselect";
    // Fatal: with the scan selected (the bug), the actions below act on the
    // full-page scan and a modal can open, which would hang a headless run.
    ASSERT_FALSE(tool_->hasSelection()) << "the press must deselect, not select the scan";

    // Delete, and every image action, after that click: still nothing.
    // Delete is handed to the tool the way PdfCanvas::keyPressEvent forwards
    // it. (Sent through the application instead, nothing is selected on the
    // canvas any more, so the canvas does not claim Del and the window's
    // "Delete Selected Pages" shortcut takes it behind its confirmation
    // modal - the idle-canvas rule pinned in test_delete_key_routing.cpp.)
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    tool_->keyPressEvent(&del);
    settle();
    for (const char* a : {"imageRemoveAction", "imageRotateAction", "imageRotate180Action",
                          "imageRotate270Action", "imageCropAction", "imageResizeAction"}) {
        auto* act = mw.findChild<QAction*>(QString::fromLatin1(a));
        ASSERT_NE(act, nullptr) << a;
        act->trigger();
        settle();
    }
    EXPECT_EQ(undo_->undoStackSize(), depthBefore) << "Delete or an image action edited the page";
    EXPECT_EQ(liveContentOfPage(doc_, tmp_.filePath("after2.pdf").toStdString(), 0), bytesBefore);
    ASSERT_EQ(imagesOn(0).size(), 2u) << "an image was removed";
}

// The handlers commit to the TOOL's page. Before the fix each used
// canvas_->currentPage(), so with the user scrolled on to page 1 a drag on
// page 0's image wrote to page 1 (where the name resolves to nothing, or - on
// a scanned document whose every page draws "Im0" - to the wrong scan).
TEST_F(ImageScanBackgroundWindowTest, MoveCommitsToTheToolsPageNotTheCurrentPage)
{
    ScanChoosingMainWindow mw;
    openAndInsert(mw, "test_3page.pdf");
    if (HasFatalFailure()) return;
    scrollToPage1(mw);
    if (HasFatalFailure()) return;
    const QRectF b = tool_->selectedImage().boundingBox;
    const int depth = undo_->undoStackSize();
    toolDrag(b.center(), b.center() + QPointF(40, 30));
    const auto now = findByName(imagesOn(0), name_);
    ASSERT_TRUE(now.has_value());
    EXPECT_NEAR(now->boundingBox.x(), b.x() + 40, 0.5) << "the move did not reach page 0";
    EXPECT_NEAR(now->boundingBox.y(), b.y() + 30, 0.5);
    EXPECT_EQ(undo_->undoStackSize(), depth + 1);
    EXPECT_TRUE(imagesOn(1).empty()) << "the move wrote to page 1";
    EXPECT_EQ(tool_->pageIndex(), 0) << "the resync rebound the tool to the current page";
}

TEST_F(ImageScanBackgroundWindowTest, RotateCommitsToTheToolsPageNotTheCurrentPage)
{
    ScanChoosingMainWindow mw;
    openAndInsert(mw, "test_3page.pdf");
    if (HasFatalFailure()) return;
    scrollToPage1(mw);
    if (HasFatalFailure()) return;
    const QRectF b = tool_->selectedImage().boundingBox;
    const QPointF handle(b.center().x(), b.top() - 20.0);
    const int depth = undo_->undoStackSize();
    toolDrag(handle, handle + QPointF(80, 0));
    const auto now = findByName(imagesOn(0), name_);
    ASSERT_TRUE(now.has_value());
    EXPECT_GT(std::abs(now->rotationRadians), 0.05f) << "the rotation did not reach page 0";
    EXPECT_EQ(undo_->undoStackSize(), depth + 1);
    EXPECT_TRUE(imagesOn(1).empty()) << "the rotation wrote to page 1";
}

TEST_F(ImageScanBackgroundWindowTest, RemoveCommitsToTheToolsPageNotTheCurrentPage)
{
    ScanChoosingMainWindow mw;
    openAndInsert(mw, "test_3page.pdf");
    if (HasFatalFailure()) return;
    scrollToPage1(mw);
    if (HasFatalFailure()) return;
    ASSERT_EQ(imagesOn(0).size(), 1u);
    const int depth = undo_->undoStackSize();
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    tool_->keyPressEvent(&del);
    settle();
    EXPECT_TRUE(imagesOn(0).empty()) << "Delete did not remove page 0's selected image";
    EXPECT_EQ(undo_->undoStackSize(), depth + 1);
}

// The image actions find the selection on the tool's page too (they used
// canvas_->currentPage() and silently did nothing once the user scrolled on).
TEST_F(ImageScanBackgroundWindowTest, ImageActionsActOnTheToolsPageNotTheCurrentPage)
{
    ScanChoosingMainWindow mw;
    openAndInsert(mw, "test_3page.pdf");
    if (HasFatalFailure()) return;
    scrollToPage1(mw);
    if (HasFatalFailure()) return;
    const int depth = undo_->undoStackSize();
    const QRectF before = findByName(imagesOn(0), name_)->boundingBox;

    auto* rotate = mw.findChild<QAction*>(QStringLiteral("imageRotateAction"));
    ASSERT_NE(rotate, nullptr);
    ASSERT_TRUE(rotate->isEnabled());
    rotate->trigger();
    settle();
    auto now = findByName(imagesOn(0), name_);
    ASSERT_TRUE(now.has_value());
    EXPECT_NEAR(visualClockwiseDegrees(*now), 90.0f, 0.5f) << "Rotate did not reach page 0";
    EXPECT_EQ(undo_->undoStackSize(), depth + 1);

    auto* crop = mw.findChild<QAction*>(QStringLiteral("imageCropAction"));
    ASSERT_NE(crop, nullptr);
    crop->trigger();
    settle();
    now = findByName(imagesOn(0), name_);
    ASSERT_TRUE(now.has_value());
    EXPECT_TRUE(now->cropped) << "Crop did not reach page 0";
    EXPECT_EQ(undo_->undoStackSize(), depth + 2);

    auto* resize = mw.findChild<QAction*>(QStringLiteral("imageResizeAction"));
    ASSERT_NE(resize, nullptr);
    resize->trigger();
    settle();
    now = findByName(imagesOn(0), name_);
    ASSERT_TRUE(now.has_value());
    EXPECT_LT(now->boundingBox.width() * now->boundingBox.height(),
              0.5 * before.width() * before.height())
        << "Resize did not reach page 0";
    EXPECT_EQ(undo_->undoStackSize(), depth + 3);
    EXPECT_TRUE(imagesOn(1).empty()) << "an action wrote to page 1";
}

// Extract (the properties panel's button) reads the selection from the
// tool's page as well.
TEST_F(ImageScanBackgroundWindowTest, ExtractReadsTheToolsPageNotTheCurrentPage)
{
    ScanChoosingMainWindow mw;
    openAndInsert(mw, "test_3page.pdf");
    if (HasFatalFailure()) return;
    scrollToPage1(mw);
    if (HasFatalFailure()) return;
    auto* extract = mw.findChild<QPushButton*>(QStringLiteral("imageExtractButton"));
    ASSERT_NE(extract, nullptr);
    const QString out = tmp_.filePath(QStringLiteral("extracted.png"));
    ASSERT_FALSE(QFileInfo::exists(out));
    pdfe::ui::testseam::enqueueFileDialogPath(out);
    extract->click();
    settle();
    // Drain the seam whatever happened, so a failure here cannot leak the
    // queued path into a later test's dialog.
    (void)pdfe::ui::testseam::takeFileDialogPath();
    ASSERT_TRUE(QFileInfo::exists(out)) << "Extract wrote nothing: it looked on page 1";
    QImage got(out);
    EXPECT_FALSE(got.isNull());
}

// ===========================================================================
// The predicate, pure: its boundary and its geometry.
// ===========================================================================

namespace {

const QRectF kLetter(0.0, 0.0, 612.0, 792.0);

PageImageInfo infoAt(const QRectF& r, float rotationRadians = 0.0f)
{
    PageImageInfo i;
    i.xobjectName = "probe";
    i.boundingBox = r;
    i.placementBox = r;
    i.rotationRadians = rotationRadians;
    return i;
}

}  // namespace

TEST(PageBackgroundImagePredicateTest, ThresholdIsNinetyPercent)
{
    EXPECT_DOUBLE_EQ(kPageBackgroundCoverage, 0.90);
}

TEST(PageBackgroundImagePredicateTest, JustBelowTheThresholdIsNotBackground)
{
    // Full width, 0.8999 of the height.
    const PageImageInfo i = infoAt(QRectF(0.0, 0.0, 612.0, 792.0 * 0.8999));
    EXPECT_NEAR(pageCoverageFraction(i, kLetter), 0.8999, 1e-9);
    EXPECT_FALSE(isPageBackgroundImage(i, kLetter));
}

TEST(PageBackgroundImagePredicateTest, JustAboveTheThresholdIsBackground)
{
    const PageImageInfo i = infoAt(QRectF(0.0, 0.0, 612.0, 792.0 * 0.9001));
    EXPECT_NEAR(pageCoverageFraction(i, kLetter), 0.9001, 1e-9);
    EXPECT_TRUE(isPageBackgroundImage(i, kLetter));
}

TEST(PageBackgroundImagePredicateTest, ExactlyAtTheThresholdIsBackground)
{
    const PageImageInfo i = infoAt(QRectF(0.0, 0.0, 612.0 * 0.9, 792.0));
    EXPECT_NEAR(pageCoverageFraction(i, kLetter), 0.90, 1e-12);
    EXPECT_TRUE(isPageBackgroundImage(i, kLetter));
}

TEST(PageBackgroundImagePredicateTest, CoverageIsClampedToTheVisiblePage)
{
    // Twice the page in each direction, centred: covers all of it.
    EXPECT_DOUBLE_EQ(pageCoverageFraction(infoAt(QRectF(-306, -396, 1224, 1584)), kLetter), 1.0);
    // A page-sized image shifted half a page right: half of it is off the
    // page, and off-page area is not coverage.
    const PageImageInfo half = infoAt(QRectF(306, 0, 612, 792));
    EXPECT_NEAR(pageCoverageFraction(half, kLetter), 0.5, 1e-9);
    EXPECT_FALSE(isPageBackgroundImage(half, kLetter));
    // Wholly off the page.
    EXPECT_DOUBLE_EQ(pageCoverageFraction(infoAt(QRectF(700, 0, 612, 792)), kLetter), 0.0);
    // Letter scanned onto A4 (595 x 842): wider than the page, shorter.
    const QRectF a4(0, 0, 595.28, 841.89);
    const PageImageInfo letterOnA4 = infoAt(QRectF(-8.36, 24.945, 612, 792));
    EXPECT_NEAR(pageCoverageFraction(letterOnA4, a4), 792.0 / 841.89, 1e-6);
    EXPECT_TRUE(isPageBackgroundImage(letterOnA4, a4));
}

TEST(PageBackgroundImagePredicateTest, AnOrientedQuadIsMeasuredAsDrawn)
{
    constexpr float kHalfPi = 1.57079632679f;
    // A landscape-framed scan turned a quarter: drawn upright over the page.
    const QRectF turned(306.0 - 396.0, 396.0 - 306.0, 792.0, 612.0);
    EXPECT_NEAR(pageCoverageFraction(infoAt(turned, kHalfPi), kLetter), 1.0, 1e-6);
    EXPECT_TRUE(isPageBackgroundImage(infoAt(turned, kHalfPi), kLetter));
    // The same rect unturned covers only 612 x 612 of the page.
    EXPECT_NEAR(pageCoverageFraction(infoAt(turned), kLetter), 612.0 * 612.0 / (612.0 * 792.0),
                1e-9);
    // A page-sized image turned 45 degrees leaves the corners bare: its
    // axis-aligned box would claim 1.0, the drawn quad does not.
    const double c45 = pageCoverageFraction(infoAt(kLetter, kHalfPi / 2.0f), kLetter);
    EXPECT_LT(c45, 0.9);
    EXPECT_GT(c45, 0.5);
    EXPECT_FALSE(isPageBackgroundImage(infoAt(kLetter, kHalfPi / 2.0f), kLetter));
}

TEST(PageBackgroundImagePredicateTest, EmptyPageOrImageIsNeverBackground)
{
    EXPECT_FALSE(isPageBackgroundImage(infoAt(kLetter), QRectF()));
    EXPECT_FALSE(isPageBackgroundImage(infoAt(QRectF()), kLetter));
    EXPECT_DOUBLE_EQ(pageCoverageFraction(infoAt(QRectF()), kLetter), 0.0);
}

// The scan fixtures through the real enumeration and Page::boundingBox.
TEST(PageBackgroundImagePredicateTest, TheScanFixturesClassifyAsExpected)
{
    auto context = std::make_shared<MuPdfContext>();
    ImageManager manager(context);
    struct Case { const char* file; std::vector<bool> background; };
    for (const Case& k : {Case{"test_scanned.pdf", {true}}, Case{"scan_small_page.pdf", {true}},
                          Case{"mixed_size_scan.pdf", {true}},
                          Case{"scan_offpage_image.pdf", {false, false}}}) {
        const std::string path = fixture(k.file);
        ASSERT_FALSE(path.empty()) << k.file;
        auto doc = Document::open(context, path);
        ASSERT_NE(doc, nullptr) << k.file;
        auto page = doc->page(0);
        const auto imgs = manager.enumerateImages(page.get(), doc.get());
        ASSERT_EQ(imgs.size(), k.background.size()) << k.file;
        for (size_t i = 0; i < imgs.size(); ++i) {
            EXPECT_EQ(isPageBackgroundImage(imgs[i], page->boundingBox()), k.background[i])
                << k.file << " image " << i << " coverage "
                << pageCoverageFraction(imgs[i], page->boundingBox());
        }
    }
}
