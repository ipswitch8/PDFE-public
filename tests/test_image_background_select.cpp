// -*- coding: utf-8 -*-
//
// ui-tweaks-3 phase-1: a DELIBERATE way to select a page-covering image.
//
// ui-tweaks-2 phase-2 made a click on a page-covering image (a scan, or a
// full-page photo; isPageBackgroundImage, >= 90% of the visible page) deselect
// instead of picking it up, so a stray press-drag on a scanned page no longer
// moves the whole page. That left no way to pick such an image up on purpose:
// only the insert flow selected one, by name.
//
// Two deliberate gestures now do:
//   * DOUBLE-CLICK on it with the image tool (PdfCanvas::mouseDoubleClickEvent
//     -> ImageTool::mouseDoubleClickEvent). A single click, a plain drag and a
//     modified press still do not (pinned unchanged in
//     test_image_scan_background.cpp).
//   * Image > "Select Page Background Image" (imageSelectBackgroundAction),
//     which arms the image tool on the current page if needed and calls
//     ImageTool::selectPageBackgroundImage().
// Once selected, the image is an ordinary selection: handles, move, resize,
// rotate and Delete apply.
//
// ORACLES. Selection is read from the tool (hasSelection / selectedImage /
// the imageSelected signal); every edit is read back from the page by
// ImageManager enumeration, and at window level by undo depth and the page's
// content-stream bytes. Each "nothing happens" case has a positive control in
// the same test or fixture.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "image/ImageTool.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "support/LiveContentOfPage.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QStatusBar>
#include <QString>
#include <QTemporaryDir>

#include <array>
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

// An ordinary image on the scanned page, fitz Y-down points.
const QRectF kOrdinary(100.0, 150.0, 200.0, 100.0);
// A point on the scan outside the ordinary image.
const QPointF kOnScan(450.0, 650.0);

bool sameBytes(const std::array<float, 6>& a, const std::array<float, 6>& b)
{
    return std::memcmp(a.data(), b.data(), sizeof(float) * 6) == 0;
}

std::optional<PageImageInfo> findByName(const std::vector<PageImageInfo>& all,
                                        const std::string& name)
{
    for (const auto& i : all) {
        if (i.xobjectName == name) return i;
    }
    return std::nullopt;
}

std::string writePng(QTemporaryDir& tmp, const char* name, QColor c)
{
    QImage q(80, 40, QImage::Format_RGB32);
    q.fill(c);
    const std::string p = tmp.filePath(QString::fromLatin1(name)).toStdString();
    EXPECT_TRUE(q.save(QString::fromStdString(p)));
    return p;
}

}  // namespace

// ===========================================================================
// ImageTool on its own: the scanned fixture plus one ordinary image.
// ===========================================================================

class ImageBackgroundSelectToolTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());
        const std::string base = fixture("test_scanned.pdf");
        ASSERT_FALSE(base.empty()) << "fixture test_scanned.pdf not found";
        doc_ = Document::open(context_, base);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);
        const auto before = images();
        ASSERT_EQ(before.size(), 1u) << "precondition: the fixture holds one full-page scan";
        scanName_ = before[0].xobjectName;
        ASSERT_TRUE(isPageBackgroundImage(before[0], page_->boundingBox()))
            << "precondition: the scan is the page background";

        ordinaryName_ = insert(kOrdinary);
        ASSERT_FALSE(isPageBackgroundImage(*findByName(images(), ordinaryName_),
                                           page_->boundingBox()));
        tool_->setPage(page_.get(), doc_.get());
        scanCtm_ = findByName(images(), scanName_)->pageCtm;

        QObject::connect(tool_.get(), &ImageTool::imageSelected,
                         [this](const PageImageInfo& i) {
                             ++selects_;
                             lastSelected_ = i.xobjectName;
                         });
        QObject::connect(tool_.get(), &ImageTool::resizeRequested,
                         [this](const std::string& n, float w, float h, float x, float y, float) {
                             ++resizes_;
                             lastCommitted_ = n;
                             lastRect_ = QRectF(x, y, w, h);
                         });
        QObject::connect(tool_.get(), &ImageTool::rotateRequested,
                         [this](const std::string& n, float) {
                             ++rotates_;
                             lastCommitted_ = n;
                         });
        QObject::connect(tool_.get(), &ImageTool::removeRequested, [this](int) { ++removes_; });
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

    std::string insert(const QRectF& r)
    {
        InsertImageOptions o;
        o.filePath = writePng(tmp_, ("i" + std::to_string(++pngs_) + ".png").c_str(),
                              QColor(0, 200, 0));
        o.targetRect = r;
        o.maintainAspectRatio = false;
        auto res = manager_->insertImage(page_.get(), doc_.get(), o);
        EXPECT_TRUE(res.success) << res.errorMessage;
        return res.imageInfo.xobjectName;
    }

    void mouse(QEvent::Type t, QPointF p, Qt::MouseButtons buttons)
    {
        QMouseEvent e(t, p, p, Qt::LeftButton, buttons, Qt::NoModifier);
        switch (t) {
        case QEvent::MouseButtonPress: tool_->mousePressEvent(&e, p, 1.0f); break;
        case QEvent::MouseMove: tool_->mouseMoveEvent(&e, p, 1.0f); break;
        case QEvent::MouseButtonRelease: tool_->mouseReleaseEvent(&e, p, 1.0f); break;
        case QEvent::MouseButtonDblClick: tool_->mouseDoubleClickEvent(&e, p, 1.0f); break;
        default: break;
        }
    }
    // What Qt delivers for a double-click: press, release, double-click, release.
    void doubleClick(QPointF p)
    {
        mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
        mouse(QEvent::MouseButtonDblClick, p, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
    }
    void drag(QPointF from, QPointF to)
    {
        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
        QMouseEvent m1(QEvent::MouseMove, (from + to) / 2.0, (from + to) / 2.0, Qt::NoButton,
                       Qt::LeftButton, Qt::NoModifier);
        tool_->mouseMoveEvent(&m1, (from + to) / 2.0, 1.0f);
        QMouseEvent m2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mouseMoveEvent(&m2, to, 1.0f);
        mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
    }

    bool scanSelected() const
    {
        return tool_->hasSelection() && tool_->selectedImage().xobjectName == scanName_;
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageTool> tool_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    std::string scanName_, ordinaryName_, lastSelected_, lastCommitted_;
    std::array<float, 6> scanCtm_{};
    QRectF lastRect_;
    int pngs_ = 0, selects_ = 0, resizes_ = 0, rotates_ = 0, removes_ = 0;
};

// The core gesture: a double-click on the scan selects it (and starts nothing).
TEST_F(ImageBackgroundSelectToolTest, DoubleClickOnTheScanSelectsIt)
{
    ASSERT_FALSE(tool_->hasSelection());
    doubleClick(kOnScan);
    EXPECT_TRUE(scanSelected()) << "a double-click on the scan must select it";
    EXPECT_GE(selects_, 1) << "imageSelected was not emitted";
    EXPECT_EQ(lastSelected_, scanName_);
    EXPECT_EQ(resizes_ + rotates_ + removes_, 0) << "the double-click itself edited something";
    EXPECT_TRUE(sameBytes(findByName(images(), scanName_)->pageCtm, scanCtm_));
}

// Selected that way, the scan is an ordinary selection: a body drag moves it.
TEST_F(ImageBackgroundSelectToolTest, AfterADoubleClickADragMovesTheScan)
{
    doubleClick(kOnScan);
    ASSERT_TRUE(scanSelected());
    const QRectF start = tool_->selectedImage().boundingBox;
    drag(kOnScan, kOnScan + QPointF(-30, -20));
    ASSERT_EQ(resizes_, 1) << "the drag committed nothing";
    EXPECT_EQ(lastCommitted_, scanName_);
    EXPECT_NEAR(lastRect_.x(), start.x() - 30, 0.5);
    EXPECT_NEAR(lastRect_.y(), start.y() - 20, 0.5);
}

// The rule from ui-tweaks-2 is untouched: one click still does not select it.
TEST_F(ImageBackgroundSelectToolTest, ASingleClickOnTheScanStillDoesNotSelectIt)
{
    mouse(QEvent::MouseButtonPress, kOnScan, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, kOnScan, Qt::NoButton);
    EXPECT_FALSE(tool_->hasSelection());
    EXPECT_EQ(selects_, 0);
    // Positive control: the double-click at the same point does.
    doubleClick(kOnScan);
    EXPECT_TRUE(scanSelected());
}

// A double-click on an ordinary image selects THAT image, not the scan under it.
TEST_F(ImageBackgroundSelectToolTest, DoubleClickOnAnOrdinaryImageSelectsThatImage)
{
    doubleClick(kOrdinary.center());
    ASSERT_TRUE(tool_->hasSelection());
    EXPECT_EQ(tool_->selectedImage().xobjectName, ordinaryName_);
    EXPECT_EQ(resizes_ + rotates_ + removes_, 0);
    // And it stays editable as before: a drag moves it.
    drag(kOrdinary.center(), kOrdinary.center() + QPointF(20, 10));
    EXPECT_EQ(resizes_, 1);
    EXPECT_EQ(lastCommitted_, ordinaryName_);
}

// With the ordinary image selected, a double-click on one of its handles is
// the handle press it always was, not a jump to the scan beneath.
TEST_F(ImageBackgroundSelectToolTest, DoubleClickOnTheSelectionsHandleKeepsTheSelection)
{
    ASSERT_TRUE(tool_->selectImage(ordinaryName_));
    const QPointF corner = kOrdinary.bottomRight();
    ASSERT_FALSE(kOrdinary.contains(corner + QPointF(2, 2)));
    doubleClick(corner + QPointF(2, 2));
    ASSERT_TRUE(tool_->hasSelection());
    EXPECT_EQ(tool_->selectedImage().xobjectName, ordinaryName_)
        << "a double-click on the selection's handle selected the scan";
}

// Selected by double-click, every ordinary edit applies to the scan.
TEST_F(ImageBackgroundSelectToolTest, AfterADoubleClickResizeRotateAndDeleteApply)
{
    doubleClick(kOnScan);
    ASSERT_TRUE(scanSelected());
    const QRectF b = tool_->selectedImage().boundingBox;

    // Resize by the bottom-right handle (inward: the scan reaches the page edge).
    drag(b.bottomRight(), b.bottomRight() + QPointF(-40, -30));
    ASSERT_EQ(resizes_, 1) << "the handle drag committed nothing";
    EXPECT_EQ(lastCommitted_, scanName_);
    EXPECT_NEAR(lastRect_.width(), b.width() - 40, 0.5);
    EXPECT_NEAR(lastRect_.height(), b.height() - 30, 0.5);

    // Rotate by the rotation handle (20 px above the top centre at zoom 1).
    // The overlay was resized above; put it back so the handle is where the
    // page has it (the window resyncs the tool after each commit).
    ASSERT_TRUE(resyncImageToolPlacement(*tool_, page_.get(), doc_.get(), scanName_, 0));
    const QRectF c = tool_->selectedImage().boundingBox;
    const QPointF handle(c.center().x(), c.top() - 20.0);
    drag(handle, handle + QPointF(80, 0));
    EXPECT_EQ(rotates_, 1) << "the rotation drag committed nothing";
    EXPECT_EQ(lastCommitted_, scanName_);

    // Delete.
    ASSERT_TRUE(scanSelected());
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    tool_->keyPressEvent(&del);
    EXPECT_EQ(removes_, 1) << "Delete did not request the scan's removal";
}

// The method: picks the background, false when there is none.
TEST_F(ImageBackgroundSelectToolTest, SelectPageBackgroundImageSelectsTheScan)
{
    ASSERT_TRUE(tool_->selectImage(ordinaryName_));
    selects_ = 0;
    EXPECT_TRUE(tool_->selectPageBackgroundImage());
    EXPECT_TRUE(scanSelected());
    EXPECT_EQ(selects_, 1);
    EXPECT_EQ(lastSelected_, scanName_);
    EXPECT_EQ(resizes_ + rotates_ + removes_, 0);
}

TEST_F(ImageBackgroundSelectToolTest, SelectPageBackgroundImageWithNoPageIsFalse)
{
    tool_->clearPage();
    EXPECT_FALSE(tool_->selectPageBackgroundImage());
    EXPECT_FALSE(tool_->hasSelection());
}

// Two page-covering images: the topmost (last drawn) is the one the user sees.
TEST_F(ImageBackgroundSelectToolTest, SelectPageBackgroundImagePicksTheTopmost)
{
    const QRectF box = page_->boundingBox();
    const std::string top = insert(box);
    ASSERT_TRUE(isPageBackgroundImage(*findByName(images(), top), box));
    tool_->setPage(page_.get(), doc_.get());
    EXPECT_TRUE(tool_->selectPageBackgroundImage());
    ASSERT_TRUE(tool_->hasSelection());
    EXPECT_EQ(tool_->selectedImage().xobjectName, top)
        << "with two page-covering images the topmost must be chosen";
    // The double-click agrees.
    tool_->deselect();
    doubleClick(kOnScan);
    ASSERT_TRUE(tool_->hasSelection());
    EXPECT_EQ(tool_->selectedImage().xobjectName, top);
}

// A plain page with no page-covering image: nothing to select, nothing done.
TEST(ImageBackgroundSelectPlainPageTest, NoBackgroundImageMeansNothingIsSelected)
{
    ensureQApplication();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    auto ctx = std::make_shared<MuPdfContext>();
    ImageManager manager(ctx);
    ImageTool tool(&manager);
    const std::string base = fixture("test_3page.pdf");
    ASSERT_FALSE(base.empty());
    auto doc = Document::open(ctx, base);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    InsertImageOptions o;
    o.filePath = writePng(tmp, "p.png", QColor(0, 0, 200));
    o.targetRect = kOrdinary;
    o.maintainAspectRatio = false;
    auto res = manager.insertImage(page.get(), doc.get(), o);
    ASSERT_TRUE(res.success) << res.errorMessage;
    tool.setPage(page.get(), doc.get());
    int selects = 0, edits = 0;
    QObject::connect(&tool, &ImageTool::imageSelected, [&](const PageImageInfo&) { ++selects; });
    QObject::connect(&tool, &ImageTool::resizeRequested,
                     [&](const std::string&, float, float, float, float, float) { ++edits; });

    EXPECT_FALSE(tool.selectPageBackgroundImage());
    EXPECT_FALSE(tool.hasSelection());

    const QPointF empty(450.0, 650.0);
    ASSERT_FALSE(kOrdinary.contains(empty));
    for (auto t : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                   QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
        QMouseEvent e(t, empty, empty, Qt::LeftButton,
                      t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
        if (t == QEvent::MouseButtonPress) tool.mousePressEvent(&e, empty, 1.0f);
        else if (t == QEvent::MouseButtonRelease) tool.mouseReleaseEvent(&e, empty, 1.0f);
        else tool.mouseDoubleClickEvent(&e, empty, 1.0f);
    }
    EXPECT_FALSE(tool.hasSelection()) << "a double-click on an empty plain page selected something";
    EXPECT_EQ(selects, 0);
    EXPECT_EQ(edits, 0);

    // Positive control: a double-click on the ordinary image selects it.
    for (auto t : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                   QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
        const QPointF p = kOrdinary.center();
        QMouseEvent e(t, p, p, Qt::LeftButton,
                      t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
        if (t == QEvent::MouseButtonPress) tool.mousePressEvent(&e, p, 1.0f);
        else if (t == QEvent::MouseButtonRelease) tool.mouseReleaseEvent(&e, p, 1.0f);
        else tool.mouseDoubleClickEvent(&e, p, 1.0f);
    }
    EXPECT_TRUE(tool.hasSelection());
    EXPECT_EQ(edits, 0);
    tool.clearPage();
}

// With an ordinary image selected, a double-click on empty page is the press
// it replaced: "outside any image" deselects.
TEST(ImageBackgroundSelectPlainPageTest, DoubleClickOnEmptyPageDeselectsTheSelection)
{
    ensureQApplication();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    auto ctx = std::make_shared<MuPdfContext>();
    ImageManager manager(ctx);
    ImageTool tool(&manager);
    const std::string base = fixture("test_3page.pdf");
    ASSERT_FALSE(base.empty());
    auto doc = Document::open(ctx, base);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    InsertImageOptions o;
    o.filePath = writePng(tmp, "d.png", QColor(0, 0, 200));
    o.targetRect = kOrdinary;
    o.maintainAspectRatio = false;
    auto res = manager.insertImage(page.get(), doc.get(), o);
    ASSERT_TRUE(res.success) << res.errorMessage;
    tool.setPage(page.get(), doc.get());
    ASSERT_TRUE(tool.selectImage(res.imageInfo.xobjectName));
    int deselects = 0;
    QObject::connect(&tool, &ImageTool::imageDeselected, [&]() { ++deselects; });

    // Only the double-click event itself: no preceding press, so only the
    // double-click handler can do the deselect.
    const QPointF empty(450.0, 650.0);
    ASSERT_FALSE(kOrdinary.contains(empty));
    QMouseEvent e(QEvent::MouseButtonDblClick, empty, empty, Qt::LeftButton, Qt::LeftButton,
                  Qt::NoModifier);
    tool.mouseDoubleClickEvent(&e, empty, 1.0f);
    EXPECT_FALSE(tool.hasSelection()) << "a double-click on empty page kept the selection";
    EXPECT_EQ(deselects, 1);
    tool.clearPage();
}

// ===========================================================================
// Through PdfCanvas: the double-click reaches the tool, for its own page only.
// test_3page.pdf with a page-covering image on pages 0 and 1; tool on page 0.
// ===========================================================================

class ImageBackgroundSelectCanvasTest : public ::testing::Test {
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
        doc_ = Document::open(context_, base);
        ASSERT_NE(doc_, nullptr);
        ASSERT_GE(doc_->pageCount(), 2);
        page0_ = doc_->page(0);
        page1_ = doc_->page(1);
        bg0_ = coverPage(page0_.get(), "bg0.png");
        bg1_ = coverPage(page1_.get(), "bg1.png");
        tool_->setPage(page0_.get(), doc_.get());

        canvas_ = std::make_unique<pdfe::ui::PdfCanvas>();
        canvas_->setObjectName(QStringLiteral("imageBackgroundSelectTestCanvas"));
        canvas_->resize(900, 1300);
        canvas_->setDocument(doc_.get(), nullptr, nullptr);
        canvas_->setZoom(0.5f);
        canvas_->setImageTool(tool_.get());
    }
    void TearDown() override
    {
        if (canvas_) {
            canvas_->clearImageTool();
            canvas_->clearDocument();
            canvas_.reset();
        }
        tool_->clearPage();
        page0_.reset();
        page1_.reset();
        doc_.reset();
    }

    std::string coverPage(Page* page, const char* png)
    {
        InsertImageOptions o;
        o.filePath = writePng(tmp_, png, QColor(220, 220, 200));
        o.targetRect = page->boundingBox();
        o.maintainAspectRatio = false;
        auto res = manager_->insertImage(page, doc_.get(), o);
        EXPECT_TRUE(res.success) << res.errorMessage;
        const auto all = manager_->enumerateImages(page, doc_.get());
        const auto info = findByName(all, res.imageInfo.xobjectName);
        EXPECT_TRUE(info && isPageBackgroundImage(*info, page->boundingBox()))
            << "precondition: the inserted image covers the page";
        return res.imageInfo.xobjectName;
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
    void send(QEvent::Type t, QPoint p, Qt::MouseButtons bs)
    {
        QMouseEvent e(t, QPointF(p), QPointF(canvas_->mapToGlobal(p)), Qt::LeftButton, bs,
                      Qt::NoModifier);
        QCoreApplication::sendEvent(canvas_.get(), &e);
    }
    void canvasDoubleClick(QPoint p)
    {
        send(QEvent::MouseButtonPress, p, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, p, Qt::NoButton);
        send(QEvent::MouseButtonDblClick, p, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, p, Qt::NoButton);
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageTool> tool_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page0_, page1_;
    std::unique_ptr<pdfe::ui::PdfCanvas> canvas_;
    std::string bg0_, bg1_;
};

TEST_F(ImageBackgroundSelectCanvasTest, DoubleClickOnTheToolsPageSelectsItsBackground)
{
    canvasDoubleClick(widgetPos(0, QPointF(300, 400)));
    ASSERT_TRUE(tool_->hasSelection()) << "the canvas did not hand the double-click to the tool";
    EXPECT_EQ(tool_->selectedImage().xobjectName, bg0_);
    EXPECT_EQ(tool_->pageIndex(), 0);
}

TEST_F(ImageBackgroundSelectCanvasTest, DoubleClickOnAnotherPagesBackgroundSelectsNothing)
{
    canvasDoubleClick(widgetPos(1, QPointF(300, 400)));
    EXPECT_FALSE(tool_->hasSelection())
        << "a double-click on page 1 selected something on the tool's page 0";
    EXPECT_EQ(tool_->pageIndex(), 0);
    // Positive control: the same gesture on the tool's page.
    canvasDoubleClick(widgetPos(0, QPointF(300, 400)));
    EXPECT_TRUE(tool_->hasSelection());
}

// With page 0's background selected, a double-click on page 1 deselects it,
// as a press on another page does. Only the double-click event is sent, so
// only the canvas's double-click dispatch can do it.
TEST_F(ImageBackgroundSelectCanvasTest, DoubleClickOnAnotherPageDeselectsTheToolsSelection)
{
    ASSERT_TRUE(tool_->selectPageBackgroundImage());
    ASSERT_EQ(tool_->selectedImage().xobjectName, bg0_);
    send(QEvent::MouseButtonDblClick, widgetPos(1, QPointF(300, 400)), Qt::LeftButton);
    EXPECT_FALSE(tool_->hasSelection())
        << "a double-click on another page left the tool's selection in place";
    EXPECT_EQ(tool_->pageIndex(), 0);
}

// ===========================================================================
// MainWindow: the Image menu action and the double-click end to end.
// ===========================================================================

class ImageBackgroundSelectWindowTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
    }

    void open(pdfe::ui::MainWindow& mw, const char* pdf)
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
        action_ = mw.findChild<QAction*>(QStringLiteral("imageSelectBackgroundAction"));
        ASSERT_NE(action_, nullptr) << "no QAction named imageSelectBackgroundAction";
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
    void send(QEvent::Type t, QPoint p, Qt::MouseButtons bs)
    {
        QMouseEvent e(t, QPointF(p), QPointF(canvas_->mapToGlobal(p)), Qt::LeftButton, bs,
                      Qt::NoModifier);
        QCoreApplication::sendEvent(canvas_, &e);
        settle();
    }
    void canvasDrag(QPoint from, QPoint to)
    {
        send(QEvent::MouseButtonPress, from, Qt::LeftButton);
        send(QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
        send(QEvent::MouseMove, to, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::NoButton);
    }
    void canvasDoubleClick(QPoint p)
    {
        send(QEvent::MouseButtonPress, p, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, p, Qt::NoButton);
        send(QEvent::MouseButtonDblClick, p, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, p, Qt::NoButton);
    }

    QTemporaryDir tmp_;
    Document* doc_ = nullptr;
    pdfe::ui::PdfCanvas* canvas_ = nullptr;
    pdfe::commands::UndoManager* undo_ = nullptr;
    QAction* action_ = nullptr;
    std::unique_ptr<ImageManager> manager_;
};

// The action is in the Image menu, named for automation, and described.
TEST_F(ImageBackgroundSelectWindowTest, TheActionIsInTheImageMenuAndDescribed)
{
    pdfe::ui::MainWindow mw;
    open(mw, "test_scanned.pdf");
    if (HasFatalFailure()) return;
    auto* menu = mw.findChild<QMenu*>(QStringLiteral("imageMenu"));
    ASSERT_NE(menu, nullptr);
    EXPECT_TRUE(menu->actions().contains(action_)) << "not in the Image menu";
    EXPECT_FALSE(action_->text().isEmpty());
    EXPECT_FALSE(action_->statusTip().isEmpty());
    EXPECT_FALSE(action_->toolTip().isEmpty());
    EXPECT_FALSE(action_->whatsThis().isEmpty());
    EXPECT_TRUE(action_->isEnabled()) << "disabled with a document open";
}

// Enabled with a document, disabled again once it is closed.
TEST_F(ImageBackgroundSelectWindowTest, TheActionIsDisabledWhenTheDocumentCloses)
{
    pdfe::ui::MainWindow mw;
    open(mw, "test_scanned.pdf");
    if (HasFatalFailure()) return;
    ASSERT_TRUE(action_->isEnabled());
    auto* close = mw.findChild<QAction*>(QStringLiteral("closeAction"));
    ASSERT_NE(close, nullptr);
    close->trigger();
    settle();
    ASSERT_EQ(mw.documentForTesting(), nullptr) << "precondition: the document closed";
    EXPECT_FALSE(action_->isEnabled()) << "still enabled with no document";
}

// Not armed -> the action arms the image tool on the current page and selects
// the scan; then a drag through the canvas moves it with ONE undo command.
TEST_F(ImageBackgroundSelectWindowTest, TheActionArmsTheToolAndSelectsTheScan)
{
    pdfe::ui::MainWindow mw;
    open(mw, "test_scanned.pdf");
    if (HasFatalFailure()) return;
    const auto before = imagesOn(0);
    ASSERT_EQ(before.size(), 1u);
    const std::string scan = before[0].xobjectName;
    ASSERT_EQ(canvas_->imageTool(), nullptr) << "precondition: the image tool is not armed";

    action_->trigger();
    settle();
    ImageTool* tool = canvas_->imageTool();
    ASSERT_NE(tool, nullptr) << "the action did not arm the image tool";
    EXPECT_EQ(tool->pageIndex(), canvas_->currentPage());
    ASSERT_TRUE(tool->hasSelection()) << "the action did not select the scan";
    EXPECT_EQ(tool->selectedImage().xobjectName, scan);
    auto* remove = mw.findChild<QAction*>(QStringLiteral("imageRemoveAction"));
    ASSERT_NE(remove, nullptr);
    EXPECT_TRUE(remove->isEnabled()) << "the image actions must follow the selection";

    const int depth = undo_->undoStackSize();
    const QRectF b = tool->selectedImage().boundingBox;
    const QPointF from = b.center(), to = b.center() + QPointF(-40, -30);
    canvasDrag(widgetPos(0, from), widgetPos(0, to));
    EXPECT_EQ(undo_->undoStackSize(), depth + 1) << "the drag must push exactly one command";
    const auto moved = findByName(imagesOn(0), scan);
    ASSERT_TRUE(moved.has_value());
    EXPECT_FALSE(sameBytes(moved->pageCtm, before[0].pageCtm)) << "the scan did not move";
    EXPECT_NEAR(moved->boundingBox.x(), before[0].boundingBox.x() - 40, 2.5);
    EXPECT_NEAR(moved->boundingBox.y(), before[0].boundingBox.y() - 30, 2.5);
}

// Selected by the action, Remove Image removes the scan (one undo command).
TEST_F(ImageBackgroundSelectWindowTest, TheActionsSelectionCanBeRemoved)
{
    pdfe::ui::MainWindow mw;
    open(mw, "test_scanned.pdf");
    if (HasFatalFailure()) return;
    ASSERT_EQ(imagesOn(0).size(), 1u);
    action_->trigger();
    settle();
    ASSERT_NE(canvas_->imageTool(), nullptr);
    ASSERT_TRUE(canvas_->imageTool()->hasSelection());
    const int depth = undo_->undoStackSize();
    auto* remove = mw.findChild<QAction*>(QStringLiteral("imageRemoveAction"));
    ASSERT_NE(remove, nullptr);
    remove->trigger();
    settle();
    EXPECT_EQ(undo_->undoStackSize(), depth + 1);
    EXPECT_TRUE(imagesOn(0).empty()) << "the scan was not removed";
}

// A page with no page-covering image: nothing selected, nothing changed, and
// the status bar says why.
TEST_F(ImageBackgroundSelectWindowTest, OnAPageWithoutABackgroundTheActionSaysSoAndDoesNothing)
{
    pdfe::ui::MainWindow mw;
    open(mw, "test_3page.pdf");
    if (HasFatalFailure()) return;
    ASSERT_TRUE(imagesOn(0).empty()) << "precondition: page 0 has no images";
    const int depth = undo_->undoStackSize();
    const std::string bytes = liveContentOfPage(doc_, tmp_.filePath("b.pdf").toStdString(), 0);
    mw.statusBar()->clearMessage();

    action_->trigger();
    settle();
    ImageTool* tool = canvas_->imageTool();
    EXPECT_TRUE(tool == nullptr || !tool->hasSelection()) << "something was selected";
    EXPECT_EQ(undo_->undoStackSize(), depth);
    EXPECT_EQ(liveContentOfPage(doc_, tmp_.filePath("a.pdf").toStdString(), 0), bytes);
    EXPECT_FALSE(mw.statusBar()->currentMessage().isEmpty())
        << "the action gave no feedback on a page without a page-covering image";
}

// The double-click end to end: arm the tool by its toolbar action, double-click
// the scan through the canvas, then drag it.
TEST_F(ImageBackgroundSelectWindowTest, DoubleClickThroughTheCanvasSelectsAndADragMovesTheScan)
{
    pdfe::ui::MainWindow mw;
    open(mw, "test_scanned.pdf");
    if (HasFatalFailure()) return;
    const auto before = imagesOn(0);
    ASSERT_EQ(before.size(), 1u);
    const std::string scan = before[0].xobjectName;
    auto* toolAction = mw.findChild<QAction*>(QStringLiteral("imageToolAction"));
    ASSERT_NE(toolAction, nullptr);
    ASSERT_TRUE(toolAction->isEnabled());
    toolAction->trigger();
    settle();
    ImageTool* tool = canvas_->imageTool();
    ASSERT_NE(tool, nullptr) << "the Image Tool action did not arm the tool";
    ASSERT_FALSE(tool->hasSelection());

    const int depth = undo_->undoStackSize();
    const QPointF at(300.0, 400.0);
    canvasDoubleClick(widgetPos(0, at));
    ASSERT_TRUE(tool->hasSelection()) << "the double-click did not select the scan";
    EXPECT_EQ(tool->selectedImage().xobjectName, scan);
    EXPECT_EQ(undo_->undoStackSize(), depth) << "the double-click itself edited the page";

    canvasDrag(widgetPos(0, at), widgetPos(0, at + QPointF(-40, -30)));
    EXPECT_EQ(undo_->undoStackSize(), depth + 1);
    const auto moved = findByName(imagesOn(0), scan);
    ASSERT_TRUE(moved.has_value());
    EXPECT_NEAR(moved->boundingBox.x(), before[0].boundingBox.x() - 40, 2.5);
}
