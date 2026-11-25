// -*- coding: utf-8 -*-
//
// ImageTool and the page edge (ui-tweaks phase-3).
//
//   1. CLAMP. An image can never be moved or resized off its page. The rule,
//      stated at clampMovedRectToPage in ImageTool.h: the image's CENTRE stays
//      inside Page::boundingBox(). Checked as pure math (move, corner and edge
//      resize, edges, corners, rotated images) and through real drags.
//   2. OVERFLOW. The part of a selected or dragged image outside the page is
//      still drawn, shaded translucent red with a red border; the in-page part
//      is not shaded. Checked geometrically (imageOverflowPath) and by pixels
//      of ImageTool::paint, rasterised the way test_image_live_preview.cpp
//      does it: zoom 1, painter in fitz page space, transparent backdrop.
//   3. TOOLTIP. Hovering the excess gives kImageOverflowCropTooltip, through
//      ImageTool::tooltipAt and PdfCanvas's QEvent::ToolTip path; hovering the
//      in-page part does not.
//   4. The overlay, red excess included, is painted only on the tool's page.
//
// The image is solid green so any red seen can only be the overflow marking.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "image/ImageTool.h"
#include "ui/PdfCanvas.h"

#include <QApplication>
#include <QHelpEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRectF>
#include <QTemporaryDir>
#include <QToolTip>
#include <QTransform>

#include <cmath>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

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

constexpr double kPi = 3.14159265358979323846;

// A4, the size of the fixture's pages, for the pure-math tests.
const QRectF kPage(0.0, 0.0, 595.28, 841.89);

// Upright, 200x100 at (100, 150), fitz Y-down points.
const QRectF kRect(100.0, 150.0, 200.0, 100.0);

const QColor kGreen(0, 200, 0);

bool insidePage(QPointF c, const QRectF& page, double eps = 1e-3)
{
    return c.x() >= page.left() - eps && c.x() <= page.right() + eps &&
           c.y() >= page.top() - eps && c.y() <= page.bottom() + eps;
}

::testing::AssertionResult rectNear(const QRectF& a, const QRectF& b, double tol = 1e-3)
{
    if (std::abs(a.left() - b.left()) <= tol && std::abs(a.top() - b.top()) <= tol &&
        std::abs(a.width() - b.width()) <= tol && std::abs(a.height() - b.height()) <= tol) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "(" << a.x() << "," << a.y() << " " << a.width() << "x" << a.height() << ") vs ("
           << b.x() << "," << b.y() << " " << b.width() << "x" << b.height() << ")";
}

std::string describe(QRgb px)
{
    std::ostringstream o;
    o << "argb(" << qAlpha(px) << "," << qRed(px) << "," << qGreen(px) << "," << qBlue(px) << ")";
    return o.str();
}

// The translucent red wash over the green image: red clearly present AND the
// image still visible underneath.
bool isShadedImage(QRgb px)
{
    return qAlpha(px) > 200 && qRed(px) > 60 && qGreen(px) > 60 && qBlue(px) < 60;
}

// The image itself, unmarked.
bool isPlainGreen(QRgb px)
{
    return qAlpha(px) > 200 && qRed(px) < 30 && qGreen(px) > 150 && qBlue(px) < 30;
}

// Any red marking at all: the wash or the border.
bool hasRedMark(QRgb px)
{
    return qAlpha(px) > 0 && qRed(px) > 50 && qRed(px) > qBlue(px) + 40;
}

// The opaque red border.
bool isBorderRed(QRgb px)
{
    return qAlpha(px) > 200 && qRed(px) > 180 && qGreen(px) < 60 && qBlue(px) < 60;
}

const std::vector<HandlePosition> kResizeHandles = {
    HandlePosition::TopLeft,    HandlePosition::TopCenter,   HandlePosition::TopRight,
    HandlePosition::MiddleLeft, HandlePosition::MiddleRight, HandlePosition::BottomLeft,
    HandlePosition::BottomCenter, HandlePosition::BottomRight};

}  // namespace

// ---------------------------------------------------------------------------
// 1a. Clamp rule, pure math.
// ---------------------------------------------------------------------------

TEST(ImagePageClampTest, MoveThatKeepsTheCentreOnThePageIsUnchanged)
{
    for (QPointF d : {QPointF(0, 0), QPointF(50, 60), QPointF(-90, -140), QPointF(290, 590)}) {
        EXPECT_TRUE(rectNear(clampMovedRectToPage(kRect, d, kPage), kRect.translated(d)))
            << "delta " << d.x() << "," << d.y();
    }
}

TEST(ImagePageClampTest, MovePastEachEdgeStopsWithTheCentreOnThatEdge)
{
    const QPointF c = kRect.center();  // (200, 200)
    // Right.
    QRectF r = clampMovedRectToPage(kRect, QPointF(5000, 10), kPage);
    EXPECT_NEAR(r.center().x(), kPage.right(), 1e-6);
    EXPECT_NEAR(r.center().y(), c.y() + 10, 1e-6) << "the other axis keeps the full drag";
    EXPECT_EQ(r.size(), kRect.size());
    // Left.
    r = clampMovedRectToPage(kRect, QPointF(-5000, -10), kPage);
    EXPECT_NEAR(r.center().x(), kPage.left(), 1e-6);
    EXPECT_NEAR(r.center().y(), c.y() - 10, 1e-6);
    // Top.
    r = clampMovedRectToPage(kRect, QPointF(20, -5000), kPage);
    EXPECT_NEAR(r.center().y(), kPage.top(), 1e-6);
    EXPECT_NEAR(r.center().x(), c.x() + 20, 1e-6);
    // Bottom.
    r = clampMovedRectToPage(kRect, QPointF(-20, 5000), kPage);
    EXPECT_NEAR(r.center().y(), kPage.bottom(), 1e-6);
    EXPECT_NEAR(r.center().x(), c.x() - 20, 1e-6);
    // Half the image may hang off: the rect itself crosses the edge.
    EXPECT_GT(r.bottom(), kPage.bottom());
    EXPECT_LT(r.top(), kPage.bottom()) << "the image never leaves the page entirely";
}

TEST(ImagePageClampTest, MovePastACornerPinsTheCentreToThatCorner)
{
    struct Case { QPointF delta; QPointF corner; };
    for (const Case& k : {Case{QPointF(-9000, -9000), kPage.topLeft()},
                          Case{QPointF(9000, -9000), kPage.topRight()},
                          Case{QPointF(-9000, 9000), kPage.bottomLeft()},
                          Case{QPointF(9000, 9000), kPage.bottomRight()}}) {
        const QRectF r = clampMovedRectToPage(kRect, k.delta, kPage);
        EXPECT_NEAR(r.center().x(), k.corner.x(), 1e-6);
        EXPECT_NEAR(r.center().y(), k.corner.y(), 1e-6);
        EXPECT_EQ(r.size(), kRect.size());
        EXPECT_TRUE(r.intersects(kPage)) << "a quarter of the image stays on the page";
    }
}

TEST(ImagePageClampTest, AnImageAlreadyOffThePageCanComeBackButNotGoFurther)
{
    // Centre at (-100, 200): off the left edge before the drag.
    const QRectF off(-200.0, 150.0, 200.0, 100.0);
    // Further left: held where it started.
    QRectF r = clampMovedRectToPage(off, QPointF(-300, 0), kPage);
    EXPECT_NEAR(r.center().x(), -100.0, 1e-6);
    // Back toward the page: free.
    r = clampMovedRectToPage(off, QPointF(250, 0), kPage);
    EXPECT_NEAR(r.center().x(), 150.0, 1e-6);
    // Along the edge it already sits off: free in y.
    r = clampMovedRectToPage(off, QPointF(0, 300), kPage);
    EXPECT_NEAR(r.center().y(), 500.0, 1e-6);
}

TEST(ImagePageClampTest, ResizeThatKeepsTheCentreOnThePageMatchesTheUnclampedResize)
{
    for (float deg : {0.0f, 30.0f, 90.0f, -135.0f}) {
        const float rot = static_cast<float>(deg * kPi / 180.0);
        for (HandlePosition h : kResizeHandles) {
            for (QPointF d : {QPointF(30, 20), QPointF(-40, 15), QPointF(-20, -60)}) {
                const QRectF want = resizeRectForHandle(h, kRect, d, rot);
                ASSERT_TRUE(insidePage(want.center(), kPage)) << "precondition";
                EXPECT_TRUE(rectNear(clampResizedRectToPage(h, kRect, d, rot, kPage), want))
                    << "handle " << int(h) << " deg " << deg;
            }
        }
    }
}

TEST(ImagePageClampTest, CornerResizePastThePageCornerStopsWithTheCentreOnIt)
{
    // Bottom-right handle dragged far beyond the bottom-right page corner:
    // the centre moves by delta/2, so it stops exactly on that corner, with
    // the top-left (the anchor) where it was.
    const QRectF orig(400.0, 600.0, 150.0, 100.0);
    QRectF r = clampResizedRectToPage(HandlePosition::BottomRight, orig, QPointF(5000, 5000), 0.0f, kPage);
    EXPECT_NEAR(r.center().x(), kPage.right(), 1e-6);
    EXPECT_NEAR(r.center().y(), kPage.bottom(), 1e-6);
    EXPECT_NEAR(r.left(), orig.left(), 1e-6);
    EXPECT_NEAR(r.top(), orig.top(), 1e-6);

    // Top-left handle past the top-left page corner.
    const QRectF nearTl(10.0, 10.0, 200.0, 100.0);
    r = clampResizedRectToPage(HandlePosition::TopLeft, nearTl, QPointF(-5000, -5000), 0.0f, kPage);
    EXPECT_NEAR(r.center().x(), 0.0, 1e-6);
    EXPECT_NEAR(r.center().y(), 0.0, 1e-6);
    EXPECT_NEAR(r.right(), nearTl.right(), 1e-6);
    EXPECT_NEAR(r.bottom(), nearTl.bottom(), 1e-6);
}

TEST(ImagePageClampTest, EdgeResizePastAnEdgeStopsWithTheCentreOnIt)
{
    const QRectF orig(400.0, 300.0, 150.0, 100.0);  // centre (475, 350)
    QRectF r = clampResizedRectToPage(HandlePosition::MiddleRight, orig, QPointF(5000, 77), 0.0f, kPage);
    EXPECT_NEAR(r.center().x(), kPage.right(), 1e-6);
    EXPECT_NEAR(r.left(), orig.left(), 1e-6);
    EXPECT_NEAR(r.top(), orig.top(), 1e-6) << "an edge handle never moves the other axis";
    EXPECT_NEAR(r.height(), orig.height(), 1e-6);

    r = clampResizedRectToPage(HandlePosition::TopCenter, orig, QPointF(0, -5000), 0.0f, kPage);
    EXPECT_NEAR(r.center().y(), kPage.top(), 1e-6);
    EXPECT_NEAR(r.bottom(), orig.bottom(), 1e-6);
}

TEST(ImagePageClampTest, RotatedEdgeResizeIsClampedAlongTheImagesOwnAxis)
{
    // rotationRadians = +90 deg (visually counter-clockwise): the image's own
    // x axis points UP the page, so its right-edge handle sits above the
    // centre and dragging it up lengthens the image upward.
    const float rot = static_cast<float>(kPi / 2.0);
    const QRectF orig(100.0, 150.0, 200.0, 100.0);  // centre (200, 200)
    const QRectF free = resizeRectForHandle(HandlePosition::MiddleRight, orig, QPointF(0, -100), rot);
    ASSERT_NEAR(free.width(), 300.0, 1e-3) << "precondition: upward drag lengthens the image";
    ASSERT_LT(free.center().y(), 200.0);

    const QRectF r = clampResizedRectToPage(HandlePosition::MiddleRight, orig, QPointF(13, -5000), rot, kPage);
    EXPECT_NEAR(r.center().y(), kPage.top(), 1e-3) << "centre stopped on the top edge";
    EXPECT_NEAR(r.center().x(), 200.0, 1e-3) << "the cross-axis part of the drag never moves an edge handle";
    EXPECT_NEAR(r.width(), 200.0 + 2 * 200.0, 1e-3) << "grew by exactly 2 * (distance to the edge)";
    EXPECT_NEAR(r.height(), 100.0, 1e-3);
}

TEST(ImagePageClampTest, RotatedResizeNeverPutsTheCentreOffThePage)
{
    const QRectF orig(420.0, 700.0, 160.0, 120.0);  // centre (500, 760), near the bottom-right
    int clamped = 0;
    for (float deg : {0.0f, 17.0f, 30.0f, 45.0f, 90.0f, 135.0f, 200.0f, -60.0f}) {
        const float rot = static_cast<float>(deg * kPi / 180.0);
        for (HandlePosition h : kResizeHandles) {
            for (double dx : {-3000.0, -400.0, 0.0, 400.0, 3000.0}) {
                for (double dy : {-3000.0, -400.0, 0.0, 400.0, 3000.0}) {
                    const QPointF d(dx, dy);
                    const QRectF r = clampResizedRectToPage(h, orig, d, rot, kPage);
                    EXPECT_TRUE(insidePage(r.center(), kPage))
                        << "handle " << int(h) << " deg " << deg << " delta " << dx << "," << dy
                        << " centre " << r.center().x() << "," << r.center().y();
                    EXPECT_GE(r.width(), 10.0 - 1e-3);
                    EXPECT_GE(r.height(), 10.0 - 1e-3);
                    const QRectF free = resizeRectForHandle(h, orig, d, rot);
                    if (insidePage(free.center(), kPage)) {
                        EXPECT_TRUE(rectNear(r, free)) << "a legal resize must not be altered";
                    } else {
                        ++clamped;
                    }
                }
            }
        }
    }
    EXPECT_GT(clamped, 100) << "oracle: the sweep must actually exercise the clamp";
}

// ---------------------------------------------------------------------------
// 2a. Overflow geometry, pure.
// ---------------------------------------------------------------------------

TEST(ImageOverflowPathTest, EmptyWhenTheImageIsOnThePage)
{
    EXPECT_TRUE(imageOverflowPath(kRect, 0.0f, kPage).isEmpty());
    EXPECT_TRUE(imageOverflowPath(kRect, 33.0f, kPage).isEmpty());
    // Touching the edge exactly is not overflow.
    EXPECT_TRUE(imageOverflowPath(QRectF(0, 0, 100, 100), 0.0f, kPage).isEmpty());
    EXPECT_TRUE(imageOverflowPath(kRect, 0.0f, QRectF()).isEmpty()) << "no page, no overflow";
}

TEST(ImageOverflowPathTest, IsTheExcessOnly)
{
    const QRectF b(500.0, 300.0, 200.0, 100.0);  // right edge at 700, page at 595.28
    const QPainterPath p = imageOverflowPath(b, 0.0f, kPage);
    ASSERT_FALSE(p.isEmpty());
    EXPECT_TRUE(rectNear(p.boundingRect(), QRectF(QPointF(kPage.right(), 300.0), QPointF(700.0, 400.0)), 1e-2));
    EXPECT_TRUE(p.contains(QPointF(650, 350)));
    EXPECT_FALSE(p.contains(QPointF(550, 350))) << "the in-page part is not overflow";
    EXPECT_FALSE(p.contains(QPointF(650, 450))) << "beside the image is not overflow";
}

TEST(ImageOverflowPathTest, FollowsARotatedQuadNotItsBoundingBox)
{
    // A 200x200 square centred on the right edge, turned 45 degrees: a
    // diamond whose right tip is at x = 595.28 + 141.4.
    const QRectF b(kPage.right() - 100.0, 300.0, 200.0, 200.0);
    const QPainterPath p = imageOverflowPath(b, 45.0f, kPage);
    ASSERT_FALSE(p.isEmpty());
    const QPointF c = b.center();
    EXPECT_TRUE(p.contains(c + QPointF(100, 0))) << "inside the diamond, off the page";
    EXPECT_FALSE(p.contains(c + QPointF(90, -90))) << "inside the upright box, outside the diamond";
    EXPECT_FALSE(p.contains(c + QPointF(-60, 0))) << "on the page";
    EXPECT_NEAR(p.boundingRect().right(), c.x() + 100.0 * std::sqrt(2.0), 0.5);
    EXPECT_NEAR(p.boundingRect().left(), kPage.right(), 0.5);
}

// ---------------------------------------------------------------------------
// Fixture for the tool-level tests: a solid green image on a real page.
// ---------------------------------------------------------------------------

class ImagePageBoundsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());
        base_ = fixture("test_3page.pdf");
        ASSERT_FALSE(base_.empty()) << "fixture test_3page.pdf not found";

        QImage q(80, 40, QImage::Format_RGB32);
        q.fill(kGreen);
        png_ = tmp_.filePath("green.png").toStdString();
        ASSERT_TRUE(q.save(QString::fromStdString(png_)));

        doc_ = Document::open(context_, base_);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);
        pageRect_ = page_->boundingBox();
        ASSERT_NEAR(pageRect_.width(), 595.28, 0.5) << "precondition: A4 page";
        InsertImageOptions o;
        o.filePath = png_;
        o.targetRect = kRect;
        o.maintainAspectRatio = false;
        auto r = manager_->insertImage(page_.get(), doc_.get(), o);
        ASSERT_TRUE(r.success) << r.errorMessage;
        name_ = r.imageInfo.xobjectName;
        bindTool();
    }

    void TearDown() override
    {
        tool_->clearPage();
        page_.reset();
        doc_.reset();
    }

    void bindTool()
    {
        tool_->setPage(page_.get(), doc_.get());
        ASSERT_TRUE(tool_->selectImage(name_)) << "the inserted image must be selectable";
    }

    // Re-place the image (committed, as a finished drag would) and re-bind.
    void place(const QRectF& r, float clockwiseDeg = 0.0f)
    {
        if (clockwiseDeg == 0.0f) {
            ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_,
                                             float(r.width()), float(r.height()),
                                             float(r.x()), float(r.y()), 0.0f));
        } else {
            ASSERT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), name_, clockwiseDeg,
                                             float(r.center().x()), float(r.center().y()),
                                             float(r.width()), float(r.height())));
        }
        bindTool();
        ASSERT_TRUE(rectNear(bbox(), r, 0.5)) << "precondition: placed where asked";
    }

    QRectF bbox() const { return tool_->selectedImage().boundingBox; }

    void press(QPointF p)
    {
        QMouseEvent e(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mousePressEvent(&e, p, 1.0f);
    }
    void moveTo(QPointF p)
    {
        QMouseEvent e(QEvent::MouseMove, p, p, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mouseMoveEvent(&e, p, 1.0f);
    }
    void release(QPointF p)
    {
        QMouseEvent e(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool_->mouseReleaseEvent(&e, p, 1.0f);
    }

    // ImageTool::paint at zoom 1 into a transparent image larger than the
    // page, so the excess beyond the right and bottom edges is visible.
    QImage paintTool()
    {
        QImage canvas(900, 1100, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter p(&canvas);
        tool_->paint(p, 1.0f);
        p.end();
        return canvas.convertToFormat(QImage::Format_ARGB32);
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<ImageTool> tool_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    QRectF pageRect_;
    std::string base_, png_, name_;
};

// ---------------------------------------------------------------------------
// 1b. Clamp through real drags.
// ---------------------------------------------------------------------------

TEST_F(ImagePageBoundsTest, MoveWithinThePageFollowsThePointerExactly)
{
    const QRectF before = bbox();
    press(before.center());
    moveTo(before.center() + QPointF(123, 456));
    EXPECT_TRUE(rectNear(bbox(), before.translated(123, 456), 1e-3));
}

TEST_F(ImagePageBoundsTest, MoveFarPastTheRightEdgeStopsAndCommitsWithTheCentreOnIt)
{
    float cx = -1, cy = -1;
    int fires = 0;
    QObject::connect(tool_.get(), &ImageTool::resizeRequested,
                     [&](const std::string&, float w, float h, float x, float y, float) {
                         ++fires;
                         cx = x + w / 2;
                         cy = y + h / 2;
                     });
    const QRectF before = bbox();
    press(before.center());
    moveTo(before.center() + QPointF(4000, 30));
    EXPECT_NEAR(bbox().center().x(), pageRect_.right(), 1e-3);
    EXPECT_NEAR(bbox().center().y(), before.center().y() + 30, 1e-3);
    release(before.center() + QPointF(4000, 30));
    ASSERT_EQ(fires, 1);
    EXPECT_NEAR(cx, pageRect_.right(), 1e-2) << "the committed move is the clamped one";
    EXPECT_NEAR(cy, before.center().y() + 30, 1e-2);
}

TEST_F(ImagePageBoundsTest, MoveFarOffTheTopLeftCornerPinsTheCentreToIt)
{
    const QRectF before = bbox();
    press(before.center());
    moveTo(before.center() + QPointF(-4000, -4000));
    EXPECT_NEAR(bbox().center().x(), pageRect_.left(), 1e-3);
    EXPECT_NEAR(bbox().center().y(), pageRect_.top(), 1e-3);
    EXPECT_EQ(bbox().size(), before.size());
}

TEST_F(ImagePageBoundsTest, ResizeHandleDraggedFarOutsideStopsAtTheRule)
{
    const QRectF before = bbox();
    ASSERT_EQ(tool_->hitTestHandleForTest(before.bottomRight(), 1.0f), HandlePosition::BottomRight);
    press(before.bottomRight());
    moveTo(before.bottomRight() + QPointF(5000, 5000));
    EXPECT_NEAR(bbox().center().x(), pageRect_.right(), 1e-3);
    EXPECT_NEAR(bbox().center().y(), pageRect_.bottom(), 1e-3);
    EXPECT_NEAR(bbox().left(), before.left(), 1e-3) << "the anchor corner stays put";
    EXPECT_NEAR(bbox().top(), before.top(), 1e-3);
}

TEST_F(ImagePageBoundsTest, RotatedImageMovedFarOutsideKeepsItsCentreOnThePage)
{
    place(QRectF(300.0, 400.0, 200.0, 100.0), 30.0f);
    ASSERT_NE(tool_->selectedImage().rotationRadians, 0.0f) << "precondition: rotated";
    const QRectF before = bbox();
    press(before.center());
    moveTo(before.center() + QPointF(3000, 3000));
    EXPECT_NEAR(bbox().center().x(), pageRect_.right(), 1e-3);
    EXPECT_NEAR(bbox().center().y(), pageRect_.bottom(), 1e-3);
    EXPECT_EQ(bbox().size(), before.size());
    // Part of the turned image is still on the page, part is overflow.
    const QPainterPath over = tool_->overflowRegion();
    ASSERT_FALSE(over.isEmpty());
    EXPECT_TRUE(pageRect_.contains(bbox().center() - QPointF(5, 5)));
    EXPECT_FALSE(over.contains(bbox().center() - QPointF(5, 5)));
}

TEST_F(ImagePageBoundsTest, RotatedImageResizeKeepsItsCentreOnThePage)
{
    place(QRectF(380.0, 600.0, 200.0, 100.0), 30.0f);
    const QRectF before = bbox();
    const float rot = tool_->selectedImage().rotationRadians;
    // The bottom-right handle in page space: the corner turned with the image.
    QTransform t;
    t.translate(before.center().x(), before.center().y());
    t.rotate(visualClockwiseDegrees(tool_->selectedImage()));
    t.translate(-before.center().x(), -before.center().y());
    const QPointF handle = t.map(before.bottomRight());
    ASSERT_EQ(tool_->hitTestHandleForTest(handle, 1.0f), HandlePosition::BottomRight);
    press(handle);
    moveTo(handle + QPointF(4000, 4000));
    EXPECT_TRUE(insidePage(bbox().center(), pageRect_))
        << "centre " << bbox().center().x() << "," << bbox().center().y();
    EXPECT_GT(bbox().width(), before.width()) << "the resize still took effect";
    EXPECT_NEAR(tool_->selectedImage().rotationRadians, rot, 1e-6);
}

// ---------------------------------------------------------------------------
// 2b. Overflow painting.
// ---------------------------------------------------------------------------

TEST_F(ImagePageBoundsTest, DragAcrossTheEdgeShadesOnlyTheExcessRed)
{
    const QRectF before = bbox();  // x 100..300, y 150..250
    press(before.center());
    moveTo(before.center() + QPointF(390, 0));  // x 490..690, centre 590: legal, 95 pt over
    ASSERT_TRUE(tool_->hasLivePreviewForTest());
    ASSERT_NEAR(bbox().right(), 690.0, 1e-3) << "precondition: the move is legal, not clamped";
    const QImage img = paintTool();
    const int y = 200;
    for (int x : {610, 640, 680}) {
        EXPECT_TRUE(isShadedImage(img.pixel(x, y))) << "excess at x=" << x << ": " << describe(img.pixel(x, y));
    }
    for (int x : {500, 550, 593}) {
        EXPECT_TRUE(isPlainGreen(img.pixel(x, y))) << "in-page at x=" << x << ": " << describe(img.pixel(x, y));
    }
    // The border runs along the page edge, on the outside of it.
    EXPECT_TRUE(isBorderRed(img.pixel(596, y))) << describe(img.pixel(596, y));
    EXPECT_FALSE(hasRedMark(img.pixel(594, y))) << "no red on the page side: " << describe(img.pixel(594, y));
}

TEST_F(ImagePageBoundsTest, SelectedOverflowingImageShowsItsExcessShadedAndNothingOnThePage)
{
    place(QRectF(480.0, 300.0, 200.0, 100.0));  // x 480..680, centre 580 on the page
    ASSERT_FALSE(tool_->hasLivePreviewForTest()) << "not dragging";
    const QImage img = paintTool();
    for (int x : {610, 640, 670}) {
        EXPECT_TRUE(isShadedImage(img.pixel(x, 350))) << "excess at x=" << x << ": " << describe(img.pixel(x, 350));
    }
    // In-page interior: the tool draws nothing there (the page tiles show the
    // image), so no pixels and no red at all.
    for (int x = 490; x <= 590; x += 5) {
        for (int y = 310; y <= 390; y += 5) {
            const QRgb px = img.pixel(x, y);
            ASSERT_FALSE(hasRedMark(px)) << "red on the page at " << x << "," << y << ": " << describe(px);
            ASSERT_FALSE(isPlainGreen(px)) << "in-page pixels drawn by the overlay at " << x << "," << y;
        }
    }
}

TEST_F(ImagePageBoundsTest, NoRedAnywhereWhileTheImageStaysOnThePage)
{
    auto redCount = [](const QImage& img) {
        int n = 0;
        for (int y = 0; y < img.height(); y += 2)
            for (int x = 0; x < img.width(); x += 2)
                if (hasRedMark(img.pixel(x, y))) ++n;
        return n;
    };
    EXPECT_TRUE(tool_->overflowRegion().isEmpty());
    EXPECT_EQ(redCount(paintTool()), 0) << "selected, on the page";
    const QRectF before = bbox();
    press(before.center());
    moveTo(before.center() + QPointF(200, 300));
    EXPECT_EQ(redCount(paintTool()), 0) << "dragging, on the page";
}

TEST_F(ImagePageBoundsTest, RotatedOverflowIsShadedInsideTheTurnedImageOnly)
{
    // 200x200 square centred 40 pt inside the right edge, turned 45 degrees.
    const QRectF sq(pageRect_.right() - 140.0, 400.0, 200.0, 200.0);
    place(sq, 45.0f);
    const QPointF c = bbox().center();
    const QImage img = paintTool();
    const QPoint inDiamond = (c + QPointF(110, 0)).toPoint();     // off page, inside the diamond
    const QPoint inBoxCorner = (c + QPointF(85, -85)).toPoint();  // off page, outside the diamond
    const QPoint onPage = (c + QPointF(-20, 0)).toPoint();
    ASSERT_GT(inDiamond.x(), pageRect_.right() + 2);
    ASSERT_GT(inBoxCorner.x(), pageRect_.right() + 2);
    EXPECT_TRUE(isShadedImage(img.pixel(inDiamond))) << describe(img.pixel(inDiamond));
    EXPECT_FALSE(hasRedMark(img.pixel(inBoxCorner))) << describe(img.pixel(inBoxCorner));
    EXPECT_FALSE(hasRedMark(img.pixel(onPage))) << describe(img.pixel(onPage));
}

// ---------------------------------------------------------------------------
// 3. Tooltip.
// ---------------------------------------------------------------------------

TEST_F(ImagePageBoundsTest, TooltipOverTheExcessSaysItWillBeCroppedOnSave)
{
    const QString want = QString::fromUtf8(kImageOverflowCropTooltip);
    ASSERT_TRUE(want.contains(QStringLiteral("cropped")));
    ASSERT_TRUE(want.contains(QStringLiteral("saved")));
    place(QRectF(480.0, 300.0, 200.0, 100.0));
    EXPECT_EQ(tool_->tooltipAt(QPointF(640, 350)), want);
    EXPECT_TRUE(tool_->tooltipAt(QPointF(540, 350)).isEmpty()) << "in-page part";
    EXPECT_TRUE(tool_->tooltipAt(QPointF(640, 450)).isEmpty()) << "beside the image, off the page";
    tool_->deselect();
    EXPECT_TRUE(tool_->tooltipAt(QPointF(640, 350)).isEmpty()) << "no selection, no warning";
}

TEST_F(ImagePageBoundsTest, TooltipFollowsTheLiveDragAndARotatedQuad)
{
    const QString want = QString::fromUtf8(kImageOverflowCropTooltip);
    EXPECT_TRUE(tool_->tooltipAt(QPointF(640, 200)).isEmpty()) << "not over the image yet";
    const QRectF before = bbox();
    press(before.center());
    moveTo(before.center() + QPointF(390, 0));
    EXPECT_EQ(tool_->tooltipAt(QPointF(640, 200)), want) << "the excess of the in-progress rect";
    release(before.center() + QPointF(390, 0));

    const QRectF sq(pageRect_.right() - 140.0, 400.0, 200.0, 200.0);
    place(sq, 45.0f);
    const QPointF c = bbox().center();
    EXPECT_EQ(tool_->tooltipAt(c + QPointF(110, 0)), want);
    EXPECT_TRUE(tool_->tooltipAt(c + QPointF(85, -85)).isEmpty()) << "outside the turned image";
    EXPECT_TRUE(tool_->tooltipAt(c + QPointF(-20, 0)).isEmpty()) << "in-page part";
}

// The canvas: tooltip through its QEvent::ToolTip path, and the overlay drawn
// only on the tool's page.
class ImagePageBoundsCanvasTest : public ImagePageBoundsTest {
protected:
    void SetUp() override
    {
        ImagePageBoundsTest::SetUp();
        if (HasFatalFailure()) return;
        canvas_ = std::make_unique<pdfe::ui::PdfCanvas>();
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
        ImagePageBoundsTest::TearDown();
    }

    // Widget position of page-space point @p pdf on page @p pageIndex.
    QPoint widgetPos(int pageIndex, QPointF pdf) const
    {
        QPoint origin;
        int w = 0, h = 0;
        EXPECT_TRUE(canvas_->getPageScreenRect(pageIndex, origin, w, h));
        return (QPointF(origin) + pdf * canvas_->zoom()).toPoint();
    }

    std::unique_ptr<pdfe::ui::PdfCanvas> canvas_;
};

TEST_F(ImagePageBoundsCanvasTest, ToolTipEventOverTheExcessShowsTheCropWarning)
{
    const QString want = QString::fromUtf8(kImageOverflowCropTooltip);
    place(QRectF(480.0, 300.0, 200.0, 100.0));
    const QPoint excess = widgetPos(0, QPointF(640, 350));
    const QPoint inPage = widgetPos(0, QPointF(540, 350));
    EXPECT_EQ(canvas_->imageToolTooltipAt(excess), want);
    EXPECT_TRUE(canvas_->imageToolTooltipAt(inPage).isEmpty());

    // The real event path. A stand-in for MainWindow's annotation callback
    // shows where the in-page hover goes: past the image tool, to it.
    const QString other = QStringLiteral("annotation tooltip");
    canvas_->tooltipRequested = [other](int, QPointF) { return other; };
    canvas_->show();
    QHelpEvent onExcess(QEvent::ToolTip, excess, canvas_->mapToGlobal(excess));
    QCoreApplication::sendEvent(canvas_.get(), &onExcess);
    EXPECT_EQ(QToolTip::text(), want) << "the crop warning, ahead of the annotation callback";
    QHelpEvent onPage(QEvent::ToolTip, inPage, canvas_->mapToGlobal(inPage));
    QCoreApplication::sendEvent(canvas_.get(), &onPage);
    EXPECT_EQ(QToolTip::text(), other) << "the in-page part gets no crop warning";
    QToolTip::hideText();
    canvas_->tooltipRequested = nullptr;
}

TEST_F(ImagePageBoundsCanvasTest, OverlayAndItsRedExcessAppearOnlyOnTheToolsPage)
{
    place(QRectF(480.0, 300.0, 200.0, 100.0));
    const QPointF excessPdf(640, 350);
    auto redAt = [&](const QImage& g, int pageIndex) {
        const QPoint p = widgetPos(pageIndex, excessPdf);
        EXPECT_TRUE(QRect(QPoint(0, 0), g.size()).contains(p)) << "sample off the widget";
        return hasRedMark(g.pixel(p));
    };
    QImage g = canvas_->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    EXPECT_TRUE(redAt(g, 0)) << "the tool's page shows the excess";
    EXPECT_FALSE(redAt(g, 1)) << "another visible page must not get a copy of the overlay";

    // Control: bound to page 1 with the same placement, the marking moves
    // there, so the sample on page 1 can see it.
    auto page1 = doc_->page(1);
    std::vector<PageImageInfo> imgs{tool_->selectedImage()};
    tool_->setPage(page1.get(), doc_.get());
    tool_->setImagesForTest(imgs, 0);
    g = canvas_->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    EXPECT_TRUE(redAt(g, 1));
    EXPECT_FALSE(redAt(g, 0));
    tool_->clearPage();
}
