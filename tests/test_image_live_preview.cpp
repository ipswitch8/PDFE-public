// -*- coding: utf-8 -*-
//
// ImageTool live preview: while an image is moved, resized or rotated, the
// image's OWN PIXELS are drawn at the in-progress geometry.
//
// User report (2026-11-22): "when adding an image and repositioning it or using
// the control box to resize or rotate, the image display should be continually
// updated during the action so the user can see the results as they are making
// the adjustments". Before this, ImageTool::paint drew only a dashed outline and
// the handles during a drag; the page tiles kept showing the image at its
// original place until the edit was committed on mouse release.
//
// ORACLE. The inserted image has four solid quadrants (red TL, azure "blue"
// TR, green BL, yellow BR), so position, orientation, crop and mirroring are all visible
// as "which colour is where". ImageTool::paint is rasterised into a transparent
// page-sized image at zoom 1 (the painter contract: page space, fitz Y-down
// points). The page tiles are not part of the raster, so a colour at a point
// can only have come from the live preview.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "image/ImageTool.h"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRectF>
#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

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

// Upright, 200x100 at (100, 150), fitz Y-down points.
const QRectF kRect(100.0, 150.0, 200.0, 100.0);

// "Blue" is azure on purpose: the tool's selection frame and handles are
// drawn in pure Qt::blue, and a sample must never be mistaken for them.
const QColor kRed(255, 0, 0), kBlue(0, 160, 255), kGreen(0, 255, 0), kYellow(255, 255, 0);

bool near(QRgb px, const QColor& c, int tol = 60)
{
    return qAlpha(px) > 200 && std::abs(qRed(px) - c.red()) <= tol &&
           std::abs(qGreen(px) - c.green()) <= tol && std::abs(qBlue(px) - c.blue()) <= tol;
}

bool isAnyQuadrantColour(QRgb px)
{
    return near(px, kRed) || near(px, kBlue) || near(px, kGreen) || near(px, kYellow);
}

std::string describe(QRgb px)
{
    std::ostringstream o;
    o << "argb(" << qAlpha(px) << "," << qRed(px) << "," << qGreen(px) << "," << qBlue(px) << ")";
    return o.str();
}

}  // namespace

class ImageLivePreviewTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());
        base_ = fixture("test_single.pdf");
        ASSERT_FALSE(base_.empty()) << "fixture test_single.pdf not found";

        QImage q(80, 40, QImage::Format_RGB32);
        QPainter p(&q);
        p.fillRect(0, 0, 40, 20, kRed);
        p.fillRect(40, 0, 40, 20, kBlue);
        p.fillRect(0, 20, 40, 20, kGreen);
        p.fillRect(40, 20, 40, 20, kYellow);
        p.end();
        png_ = tmp_.filePath("quadrants.png").toStdString();
        ASSERT_TRUE(q.save(QString::fromStdString(png_)));
    }

    void TearDown() override
    {
        tool_->clearPage();
        page_.reset();
        doc_.reset();
    }

    // Insert the quadrant image at kRect on page 0 and bind the tool to it.
    void openWithInsert()
    {
        doc_ = Document::open(context_, base_);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        InsertImageOptions o;
        o.filePath = png_;
        o.targetRect = kRect;
        o.maintainAspectRatio = false;
        auto r = manager_->insertImage(page_.get(), doc_.get(), o);
        ASSERT_TRUE(r.success) << r.errorMessage;
        name_ = r.imageInfo.xobjectName;
        bindTool();
    }

    // Insert, save, then replace the page content with @p content (raw PDF
    // operators; %NAME% is replaced by the image's XObject name), reopen.
    void openWithContent(const std::function<std::string(double x0, double y1)>& content)
    {
        openWithInsert();
        tool_->clearPage();
        page_.reset();
        const std::string a = tmp_.filePath("a.pdf").toStdString();
        doc_->saveAs(a);
        doc_.reset();
        const std::string out = tmp_.filePath("built.pdf").toStdString();
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdf = pdf_open_document(ctx, a.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdf, 0);
            const fz_rect mb = pdf_bound_page(ctx, pp, FZ_MEDIA_BOX);
            std::string text = content(mb.x0, mb.y1);
            for (size_t at; (at = text.find("%NAME%")) != std::string::npos;) {
                text.replace(at, 6, name_);
            }
            fz_buffer* buf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(text.data()), text.size());
            pdf_obj* stm = pdf_add_stream(ctx, pdf, buf, nullptr, 0);
            fz_drop_buffer(ctx, buf);
            pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
            pdf_drop_obj(ctx, stm);
            pdf_write_options wo = pdf_default_write_options;
            pdf_save_document(ctx, pdf, out.c_str(), &wo);
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdf);
        });
        doc_ = Document::open(context_, out);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        bindTool();
    }

    void bindTool()
    {
        tool_->setPage(page_.get(), doc_.get());
        ASSERT_TRUE(tool_->selectImage(name_)) << "the inserted image must be selectable";
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

    // Rasterise ImageTool::paint at zoom 1 into a transparent page-sized image.
    QImage paintTool()
    {
        QImage canvas(700, 900, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter p(&canvas);
        tool_->paint(p, 1.0f);
        p.end();
        return canvas.convertToFormat(QImage::Format_ARGB32);
    }

    // The rotation handle: found with the tool's real hit-test above the top
    // edge (its exact offset is an implementation detail).
    QPointF rotationHandle()
    {
        const QRectF b = bbox();
        for (double dy = 0; dy < 80; dy += 0.5) {
            const QPointF p(b.center().x(), b.top() - dy);
            if (tool_->hitTestHandleForTest(p, 1.0f) == HandlePosition::Rotation) return p;
        }
        ADD_FAILURE() << "no rotation handle found above the image";
        return b.center();
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<ImageTool> tool_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    std::string base_, png_, name_;
};

// Selected but not dragging: the tool draws only its frame, never the pixels
// (the page tiles show the image). Keeps the oracle honest: colours seen in the
// drag tests below come from the live preview.
TEST_F(ImageLivePreviewTest, NoPixelsArePaintedWhenNotDragging)
{
    openWithInsert();
    const QImage img = paintTool();
    const QRectF b = bbox();
    EXPECT_FALSE(tool_->hasLivePreviewForTest());
    for (QPointF p : {b.center() + QPointF(-50, -25), b.center() + QPointF(50, 25)}) {
        EXPECT_FALSE(isAnyQuadrantColour(img.pixel(p.toPoint())))
            << "image pixels painted with no drag in progress at " << p.x() << "," << p.y();
    }
}

TEST_F(ImageLivePreviewTest, MoveDrawsTheImageAtTheNewPositionAndNotTheOld)
{
    openWithInsert();
    const QRectF before = bbox();
    const QPointF delta(150, 300);
    press(before.center());
    ASSERT_TRUE(tool_->hasLivePreviewForTest()) << "a body drag must capture the preview";
    moveTo(before.center() + delta);

    const QImage img = paintTool();
    const QRectF now = before.translated(delta);
    const double qx = now.width() / 4, qy = now.height() / 4;
    const QPointF c = now.center();
    EXPECT_TRUE(near(img.pixel((c + QPointF(-qx, -qy)).toPoint()), kRed)) << describe(img.pixel((c + QPointF(-qx, -qy)).toPoint()));
    EXPECT_TRUE(near(img.pixel((c + QPointF(qx, -qy)).toPoint()), kBlue));
    EXPECT_TRUE(near(img.pixel((c + QPointF(-qx, qy)).toPoint()), kGreen));
    EXPECT_TRUE(near(img.pixel((c + QPointF(qx, qy)).toPoint()), kYellow));

    // The old position is washed out, not drawn.
    const QPointF oc = before.center();
    for (QPointF off : {QPointF(-qx, -qy), QPointF(qx, -qy), QPointF(-qx, qy), QPointF(qx, qy)}) {
        const QRgb px = img.pixel((oc + off).toPoint());
        EXPECT_FALSE(isAnyQuadrantColour(px)) << "image still drawn at the old position: " << describe(px);
        EXPECT_GT(qAlpha(px), 100) << "the old position should carry the white wash";
        EXPECT_GT(qRed(px), 200);
        EXPECT_GT(qBlue(px), 200);
    }

    release(before.center() + delta);
    EXPECT_FALSE(tool_->hasLivePreviewForTest()) << "release must drop the preview";
}

TEST_F(ImageLivePreviewTest, ResizeStretchesThePreviewToTheNewRect)
{
    openWithInsert();
    const QRectF before = bbox();
    ASSERT_EQ(tool_->hitTestHandleForTest(before.bottomRight(), 1.0f), HandlePosition::BottomRight);
    press(before.bottomRight());
    ASSERT_TRUE(tool_->hasLivePreviewForTest()) << "a handle drag must capture the preview";
    moveTo(before.bottomRight() + QPointF(100, 100));

    const QRectF now = bbox();
    ASSERT_GT(now.width(), before.width() + 50) << "precondition: the resize took effect";
    ASSERT_GT(now.height(), before.height() + 50);
    const QImage img = paintTool();
    // Beyond the original rect, inside the new one: the stretched yellow
    // quadrant. Nothing would be drawn there with an outline-only overlay.
    const QPointF beyond(now.right() - 10, now.bottom() - 10);
    ASSERT_FALSE(before.contains(beyond));
    EXPECT_TRUE(near(img.pixel(beyond.toPoint()), kYellow)) << describe(img.pixel(beyond.toPoint()));
    EXPECT_TRUE(near(img.pixel((now.topLeft() + QPointF(10, 10)).toPoint()), kRed));
    // The quadrant boundary moved with the stretch: 3/8 of the new width is red.
    EXPECT_TRUE(near(img.pixel((now.topLeft() + QPointF(now.width() * 0.375, 10)).toPoint()), kRed));
    EXPECT_TRUE(near(img.pixel((now.topLeft() + QPointF(now.width() * 0.625, 10)).toPoint()), kBlue));
}

TEST_F(ImageLivePreviewTest, RotateDrawsThePreviewAtThePendingAngle)
{
    openWithInsert();
    const QRectF b = bbox();
    const QPointF c = b.center();
    const QPointF h = rotationHandle();
    const double r = c.y() - h.y();
    press(h);
    ASSERT_TRUE(tool_->hasLivePreviewForTest()) << "a rotation-handle drag must capture the preview";
    // From straight above the centre to straight right of it: +90 degrees,
    // visually clockwise.
    moveTo(c + QPointF(r, 0));

    const QImage img = paintTool();
    // Clockwise quarter turn in this Y-down frame maps (x, y) -> (-y, x). The
    // red quadrant (x in [-100,0], y in [-50,0]) lands in x in [0,50],
    // y in [-100,0], and so on. Samples sit at y = +-70, clear of the
    // UNROTATED frame the tool still draws at y = +-50.
    EXPECT_TRUE(near(img.pixel((c + QPointF(25, -70)).toPoint()), kRed)) << describe(img.pixel((c + QPointF(25, -70)).toPoint()));
    EXPECT_TRUE(near(img.pixel((c + QPointF(-25, 70)).toPoint()), kYellow));
    EXPECT_TRUE(near(img.pixel((c + QPointF(25, 70)).toPoint()), kBlue));
    EXPECT_TRUE(near(img.pixel((c + QPointF(-25, -70)).toPoint()), kGreen));
    // Where the UNROTATED image's red quadrant was is now outside the rotated
    // footprint (|x| > 50): only the wash.
    EXPECT_FALSE(isAnyQuadrantColour(img.pixel((c + QPointF(-75, -25)).toPoint())));

    release(c + QPointF(r, 0));
    EXPECT_FALSE(tool_->hasLivePreviewForTest());
}

TEST_F(ImageLivePreviewTest, PreviewShowsOnlyTheCroppedRegion)
{
    openWithInsert();
    // Keep the LEFT half: red over green.
    const QRectF b = bbox();
    ASSERT_TRUE(editor_->cropImage(page_.get(), doc_.get(), name_,
                                   QRectF(b.left(), b.top(), b.width() / 2, b.height()),
                                   0, 0, 0, 0));
    bindTool();
    const QRectF cropped = bbox();
    ASSERT_NEAR(cropped.width(), b.width() / 2, 1.0) << "precondition: the crop took effect";

    const QPointF delta(0, 300);
    press(cropped.center());
    ASSERT_TRUE(tool_->hasLivePreviewForTest());
    moveTo(cropped.center() + delta);
    const QImage img = paintTool();
    const QRectF now = cropped.translated(delta);
    // The cropped-away blue/yellow half must not appear: the visible region's
    // right side is the red/green boundary, not the image's right half.
    EXPECT_TRUE(near(img.pixel((now.topLeft() + QPointF(now.width() - 8, 10)).toPoint()), kRed))
        << describe(img.pixel((now.topLeft() + QPointF(now.width() - 8, 10)).toPoint()));
    EXPECT_TRUE(near(img.pixel((now.bottomLeft() + QPointF(now.width() - 8, -10)).toPoint()), kGreen));
    // Interior only: 8 px in from each edge clears the frame and the handles
    // (8 px squares centred on the edges).
    for (int x = int(now.left()) + 8; x < int(now.right()) - 8; x += 4) {
        for (int y = int(now.top()) + 8; y < int(now.bottom()) - 8; y += 4) {
            const QRgb px = img.pixel(x, y);
            ASSERT_FALSE(near(px, kBlue) || near(px, kYellow))
                << "cropped-away pixels drawn at " << x << "," << y << ": " << describe(px);
        }
    }
}

TEST_F(ImageLivePreviewTest, PreviewKeepsAMirroredPlacementMirrored)
{
    // A left-right mirrored placement of the same image (negative a in cm).
    openWithContent([](double x0, double y1) {
        std::ostringstream o;
        o << "q " << -kRect.width() << " 0 0 " << kRect.height() << " "
          << (x0 + kRect.x() + kRect.width()) << " " << (y1 - kRect.y() - kRect.height())
          << " cm /%NAME% Do Q\n";
        return o.str();
    });
    ASSERT_TRUE(tool_->selectedImage().flippedX) << "precondition: the placement is mirrored";

    const QRectF before = bbox();
    const QPointF delta(100, 350);
    press(before.center());
    ASSERT_TRUE(tool_->hasLivePreviewForTest());
    moveTo(before.center() + delta);
    const QImage img = paintTool();
    const QPointF c = before.translated(delta).center();
    // Mirrored: red is top-RIGHT, blue top-left.
    EXPECT_TRUE(near(img.pixel((c + QPointF(50, -25)).toPoint()), kRed)) << describe(img.pixel((c + QPointF(50, -25)).toPoint()));
    EXPECT_TRUE(near(img.pixel((c + QPointF(-50, -25)).toPoint()), kBlue));
    EXPECT_TRUE(near(img.pixel((c + QPointF(50, 25)).toPoint()), kGreen));
}

TEST_F(ImageLivePreviewTest, EscapeDuringADragDropsThePreview)
{
    openWithInsert();
    const QRectF b = bbox();
    press(b.center());
    moveTo(b.center() + QPointF(40, 40));
    ASSERT_TRUE(tool_->hasLivePreviewForTest());
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool_->keyPressEvent(&esc);
    EXPECT_FALSE(tool_->hasLivePreviewForTest());
    const QImage img = paintTool();
    EXPECT_FALSE(isAnyQuadrantColour(img.pixel((b.center() + QPointF(40, 40)).toPoint())))
        << "no stale preview pixels after Esc";
}
