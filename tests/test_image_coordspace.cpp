// -*- coding: utf-8 -*-
// GitHub #373 (+ #376, #378) — image subsystem coordinate-space unification.
//
// Before #373 the image path mixed THREE spaces: enumeration bboxes were PDF
// user space (Y-up), mouse input was fitz device space (Y-down), and the
// selection overlay painted into a raw widget-space painter. The seams used
// by tests (canvasRectFromPdf / dragByPdfCoord) had no flip either, which
// exactly cancelled the Y-up enumeration — so every prior test round-tripped
// inside one space and could not see the inversion (#378). The user-visible
// results: selection handles offset from the image, and a drag DOWN
// committing a move UP.
//
// These tests are deliberately CROSS-space: each one compares the geometry a
// seam reports against pixels actually rendered from the written content
// stream, so no self-cancelling pair of errors can satisfy them.
//
//   1. InsertedImageRendersWhereTheSeamSaysItIs — the decisive oracle.
//      Pre-fix failure distance: the enumerated y and the rendered y differ
//      by pageH - 2*y - h (hundreds of points), not by a rounding tolerance.
//   2. CommittedBodyDragDownMovesTheRenderedImageDown — drag direction at
//      commit (the "drag down -> image jumps up" defect).
//   3. ClockwiseRotateDragCommitsClockwiseRotation — #376: the rotation sign
//      re-derived with genuine Y-down input, asserted against the committed
//      cm matrix AND the rendered pixel orientation.
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
#include <QMouseEvent>
#include <QPainter>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

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

// Pixel-space bounding box of all pixels satisfying a predicate, plus the
// centroid. Rendered at scale 1.0 a device pixel (x, y) IS the fitz-space
// point (x, y) — Page::render regions are fitz Y-down (see Page.h) — which is
// what makes these oracles cross-space: seam coordinates can be compared to
// pixel coordinates with no further transform.
struct PixBox {
    int minX = 1 << 28, minY = 1 << 28, maxX = -1, maxY = -1;
    long long count = 0;
    double sumX = 0.0, sumY = 0.0;
    bool found() const { return count > 0; }
    double cx() const { return count ? sumX / count : -1.0; }
    double cy() const { return count ? sumY / count : -1.0; }
    int w() const { return maxX >= minX ? maxX - minX + 1 : 0; }
    int h() const { return maxY >= minY ? maxY - minY + 1 : 0; }
};

template <typename Pred>
PixBox locatePixels(const QImage& im, Pred pred) {
    PixBox b;
    for (int y = 0; y < im.height(); ++y) {
        for (int x = 0; x < im.width(); ++x) {
            const QRgb p = im.pixel(x, y);
            if (pred(qRed(p), qGreen(p), qBlue(p))) {
                b.minX = std::min(b.minX, x);
                b.minY = std::min(b.minY, y);
                b.maxX = std::max(b.maxX, x);
                b.maxY = std::max(b.maxY, y);
                b.sumX += x;
                b.sumY += y;
                ++b.count;
            }
        }
    }
    return b;
}

bool isRed(int r, int g, int b)  { return r > 160 && g < 90 && b < 90; }
bool isBlue(int r, int g, int b) { return b > 160 && r < 90 && g < 90; }
bool isRedOrBlue(int r, int g, int b) { return isRed(r, g, b) || isBlue(r, g, b); }

} // namespace

class ImageCoordSpaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        qRegisterMetaType<std::string>("std::string");
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        tool_ = std::make_unique<ImageTool>(manager_.get());
        singlePdf_ = std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
        ASSERT_TRUE(std::filesystem::exists(singlePdf_)) << singlePdf_;
    }

    void TearDown() override {
        for (const auto& p : tempFiles_) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }

    std::filesystem::path tempFile(const std::string& name) {
        auto p = std::filesystem::temp_directory_path() / ("pdfe_coordspace_" + name);
        tempFiles_.push_back(p);
        return p;
    }

    // Write a solid-red PNG fixture (unambiguous against test_single.pdf's
    // black-on-white text).
    std::filesystem::path makeSolidRedPng() {
        QImage img(40, 20, QImage::Format_RGB32);
        img.fill(QColor(255, 0, 0));
        auto p = tempFile("red.png");
        EXPECT_TRUE(img.save(QString::fromStdString(p.string())));
        return p;
    }

    // Write a PNG whose TOP half is red and BOTTOM half is blue — an
    // orientation marker for the rotation-direction oracle.
    std::filesystem::path makeTopRedBottomBluePng() {
        QImage img(40, 40, QImage::Format_RGB32);
        QPainter painter(&img);
        painter.fillRect(0, 0, 40, 20, QColor(255, 0, 0));
        painter.fillRect(0, 20, 40, 20, QColor(0, 0, 255));
        painter.end();
        auto p = tempFile("redblue.png");
        EXPECT_TRUE(img.save(QString::fromStdString(p.string())));
        return p;
    }

    // Save the live document, reopen it fresh, and render page 0 at scale 1.
    // Going through the file asserts the WRITTEN content stream, not any
    // in-memory state.
    QImage saveReopenRender(Document* doc, const std::string& tag,
                            std::vector<PageImageInfo>* imagesOut = nullptr) {
        auto out = tempFile(tag + ".pdf");
        doc->saveAs(out);
        auto doc2 = Document::open(context_, out);
        EXPECT_NE(doc2, nullptr);
        if (!doc2) return {};
        auto page2 = doc2->page(0);
        EXPECT_NE(page2, nullptr);
        if (!page2) return {};
        if (imagesOut)
            *imagesOut = manager_->enumerateImages(page2.get(), doc2.get());
        QImage im = page2->render(page2->boundingBox(), 1.0f);
        EXPECT_FALSE(im.isNull());
        return im;
    }

    // Event helpers (scale 1, coordinates are fitz Y-down page space — the
    // exact space PdfCanvas's screenToPdf dispatch produces).
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
    std::string singlePdf_;
    std::vector<std::filesystem::path> tempFiles_;
};

// ---------------------------------------------------------------------------
// 1. The decisive cross-space oracle: insert at a fitz-space rect, then the
//    RENDERED pixels, the ENUMERATED bbox and the HANDLE hit-test must all
//    agree on where the image is — in the same Y-down frame.
//
//    Pre-fix: insertImage wrote the fitz-space y straight into the Y-up cm
//    (image renders at y ~ pageH - y - h = hundreds of points away from the
//    requested spot), and the enumerated bbox disagreed with the pixels by
//    the same flip distance. Tolerance here is 3 pt; the pre-fix error for
//    this page is |pageH - 2*150 - 100| >> 3.
// ---------------------------------------------------------------------------
TEST_F(ImageCoordSpaceTest, InsertedImageRendersWhereTheSeamSaysItIs) {
    auto doc = Document::open(context_, singlePdf_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Premise: the flip distance must be observable on this page, and the
    // fixture must start image-free so the red rect is OURS.
    const double pageH = page->boundingBox().height();
    ASSERT_GT(pageH, 400.0) << "page too short to distinguish a Y flip";
    ASSERT_TRUE(manager_->enumerateImages(page.get(), doc.get()).empty())
        << "test_single.pdf must have no images of its own";

    InsertImageOptions opts;
    opts.filePath = makeSolidRedPng().string();
    opts.targetRect = QRectF(100.0, 150.0, 200.0, 100.0);  // fitz Y-down
    opts.maintainAspectRatio = false;
    auto res = manager_->insertImage(page.get(), doc.get(), opts);
    ASSERT_TRUE(res.success) << res.errorMessage;

    // Enumerated bbox (what the tool, the overlay and imageSelectionDiag all
    // consume) must equal the requested fitz rect.
    auto images = manager_->enumerateImages(page.get(), doc.get());
    ASSERT_EQ(images.size(), 1u);
    const QRectF bbox = images[0].boundingBox;
    EXPECT_NEAR(bbox.x(), 100.0, 1.0);
    EXPECT_NEAR(bbox.y(), 150.0, 1.0)
        << "enumerated bbox y must be fitz Y-down (distance from page TOP)";
    EXPECT_NEAR(bbox.width(), 200.0, 1.0);
    EXPECT_NEAR(bbox.height(), 100.0, 1.0);

    // Render the WRITTEN file and locate the red rectangle in pixels.
    QImage im = saveReopenRender(doc.get(), "insert_oracle");
    ASSERT_FALSE(im.isNull());
    PixBox px = locatePixels(im, isRed);
    ASSERT_GT(px.count, 2000) << "inserted red image not found in the render";

    // THE cross-space assertion: pixels where the seam says the image is.
    EXPECT_NEAR(px.minX, bbox.x(), 3.0);
    EXPECT_NEAR(px.minY, bbox.y(), 3.0)
        << "rendered image top (" << px.minY << ") disagrees with the "
        << "enumerated bbox top (" << bbox.y() << ") — the pre-#373 flip "
        << "distance on this page is ~" << (pageH - 2 * 150.0 - 100.0) << " pt";
    EXPECT_NEAR(px.w(), bbox.width(), 4.0);
    EXPECT_NEAR(px.h(), bbox.height(), 4.0);

    // Handle cross-space check (#373 handle-offset defect): the tool's REAL
    // hit-test must report the corner handles at the RENDERED corners. The
    // hit-test consumes the same bbox the paint path draws (PdfCanvas now
    // hands paint a page-space painter), so pixels->handles closes the loop.
    tool_->setPage(page.get(), doc.get());
    ASSERT_TRUE(tool_->selectLast());
    EXPECT_EQ(tool_->hitTestHandleForTest(QPointF(px.minX, px.minY), 1.0f),
              HandlePosition::TopLeft)
        << "TopLeft handle is not at the rendered top-left corner";
    EXPECT_EQ(tool_->hitTestHandleForTest(QPointF(px.maxX, px.maxY), 1.0f),
              HandlePosition::BottomRight)
        << "BottomRight handle is not at the rendered bottom-right corner";

    page.reset();
    doc.reset();
}

// ---------------------------------------------------------------------------
// 2. Committed-move direction: a REAL body drag DOWN (+50 in fitz y, through
//    the same mouse-event path PdfCanvas dispatches) must move the RENDERED
//    image DOWN by 50 — and the written cm f (user-space bottom, Y-up) must
//    DECREASE.
//
//    Pre-fix: the overlay tracked the mouse (two Y-down errors cancelling)
//    but the commit wrote the Y-down y into the Y-up cm, so the image jumped
//    UP — this is the user-reported inversion.
// ---------------------------------------------------------------------------
TEST_F(ImageCoordSpaceTest, CommittedBodyDragDownMovesTheRenderedImageDown) {
    auto doc = Document::open(context_, singlePdf_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = makeSolidRedPng().string();
    opts.targetRect = QRectF(100.0, 150.0, 200.0, 100.0);
    opts.maintainAspectRatio = false;
    auto res = manager_->insertImage(page.get(), doc.get(), opts);
    ASSERT_TRUE(res.success) << res.errorMessage;
    const std::string name = res.imageInfo.xobjectName;

    tool_->setPage(page.get(), doc.get());
    ASSERT_TRUE(tool_->selectImage(name));

    // Premise-assert the pre-move state from the written file: rendered top
    // at y ~ 150 and the enumerated cm f recorded.
    std::vector<PageImageInfo> beforeImages;
    QImage before = saveReopenRender(doc.get(), "move_before", &beforeImages);
    ASSERT_FALSE(before.isNull());
    PixBox pxBefore = locatePixels(before, isRed);
    ASSERT_GT(pxBefore.count, 2000);
    ASSERT_NEAR(pxBefore.minY, 150.0, 3.0) << "pre-move premise violated";
    ASSERT_EQ(beforeImages.size(), 1u);
    const float fBefore = beforeImages[0].ctm[5];

    // Wire the tool to the editor exactly as MainWindow does, then drag the
    // body DOWN by +50 fitz y (device-down positive).
    QObject::connect(tool_.get(), &ImageTool::resizeRequested,
        [&](const std::string& n, float w, float h, float x, float y) {
            editor_->resizeImage(page.get(), doc.get(), n, w, h, x, y);
        });
    const QPointF start = tool_->selectedImage().boundingBox.center();
    const QPointF end = start + QPointF(0.0, 50.0);
    press(start);
    ASSERT_TRUE(tool_->hasSelection());
    moveTo(end);
    release(end);

    // Re-enumerate + re-render from the WRITTEN file.
    std::vector<PageImageInfo> afterImages;
    QImage after = saveReopenRender(doc.get(), "move_after", &afterImages);
    ASSERT_FALSE(after.isNull());
    ASSERT_EQ(afterImages.size(), 1u);

    // fitz y increased by ~50 (the image is lower on the page)...
    EXPECT_NEAR(afterImages[0].boundingBox.y(), 150.0 + 50.0, 2.0)
        << "enumerated position did not move down by the drag delta";
    // ...the user-space cm f decreased by ~50 (nearer the page bottom)...
    EXPECT_NEAR(afterImages[0].ctm[5], fBefore - 50.0f, 2.0f)
        << "committed cm f must DECREASE when the image moves visually down";
    // ...and the pixels moved DOWN. Pre-fix the commit moved them UP, so the
    // sign of (after - before) is the discriminator, not just the magnitude.
    PixBox pxAfter = locatePixels(after, isRed);
    ASSERT_GT(pxAfter.count, 2000);
    EXPECT_NEAR(pxAfter.minY - pxBefore.minY, 50.0, 3.0)
        << "drag DOWN must move the rendered image DOWN (drag-inversion bug)";
    EXPECT_NEAR(pxAfter.minX, pxBefore.minX, 2.0);

    page.reset();
    doc.reset();
}

// ---------------------------------------------------------------------------
// 3. GitHub #376 — rotation sign with genuine Y-down input, end to end: a
//    drag that encircles CLOCKWISE on screen must commit a CLOCKWISE visual
//    rotation in the file.
//
//    Direction is asserted three independent ways:
//      (a) the emitted command angle is +90 (clockwise-positive convention);
//      (b) the committed cm matrix has b < 0 (atan2(b,a) ~ -pi/2 = CW 90 in
//          the Y-up user space the content stream is written in);
//      (c) the rendered pixels: the image's RED top half ends up on the
//          visual RIGHT (top -> right is exactly CW 90; a CCW commit — the
//          pre-fix #341 negation fed with Y-down input — puts it LEFT).
// ---------------------------------------------------------------------------
TEST_F(ImageCoordSpaceTest, ClockwiseRotateDragCommitsClockwiseRotation) {
    auto doc = Document::open(context_, singlePdf_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = makeTopRedBottomBluePng().string();
    opts.targetRect = QRectF(150.0, 150.0, 120.0, 120.0);
    opts.maintainAspectRatio = false;
    auto res = manager_->insertImage(page.get(), doc.get(), opts);
    ASSERT_TRUE(res.success) << res.errorMessage;
    const std::string name = res.imageInfo.xobjectName;

    // Premise from the written file: red on TOP of blue, horizontally aligned.
    QImage before = saveReopenRender(doc.get(), "rot_before");
    ASSERT_FALSE(before.isNull());
    {
        PixBox red = locatePixels(before, isRed);
        PixBox blue = locatePixels(before, isBlue);
        ASSERT_GT(red.count, 1000);
        ASSERT_GT(blue.count, 1000);
        ASSERT_LT(red.cy(), blue.cy()) << "premise: red half starts on TOP";
        ASSERT_NEAR(red.cx(), blue.cx(), 2.0) << "premise: halves start x-aligned";
    }

    tool_->setPage(page.get(), doc.get());
    ASSERT_TRUE(tool_->selectImage(name));
    const QRectF bbox = tool_->selectedImage().boundingBox;
    const QPointF center = bbox.center();

    float gotAngle = 0.0f;
    int rotateFires = 0;
    QObject::connect(tool_.get(), &ImageTool::rotateRequested,
        [&](const std::string& n, float angle) {
            ++rotateFires;
            gotAngle = angle;
            editor_->rotateImage(page.get(), doc.get(), n, angle,
                                 static_cast<float>(center.x()),
                                 static_cast<float>(center.y()),
                                 tool_->selectedImage().displayWidth,
                                 tool_->selectedImage().displayHeight);
        });

    // The rotation handle sits 20 pt visually ABOVE the top edge (Y-down:
    // smaller y). Premise-assert the real hit-test agrees before dragging.
    const QPointF rotHandle(center.x(), bbox.top() - 20.0);
    ASSERT_EQ(tool_->hitTestHandleForTest(rotHandle, 1.0f), HandlePosition::Rotation)
        << "premise: rotation handle must be where the drag starts";

    // Encircle CLOCKWISE on screen: start ABOVE the centre, release to the
    // RIGHT of the centre (up -> right is visually clockwise). Premise on the
    // input points themselves: in Y-down atan2 the start vector (0, -y) is
    // -90 deg and the end vector (+x, ~0) is ~0 deg — the angle GREW, which
    // in a Y-down frame IS the clockwise direction.
    press(rotHandle);
    const QPointF releasePt(center.x() + 100.0, center.y() - 1.0);
    moveTo(releasePt);
    release(releasePt);

    ASSERT_EQ(rotateFires, 1) << "rotation drag must commit exactly once";
    EXPECT_NEAR(gotAngle, 90.0f, 0.001f)
        << "#376: a visually-clockwise drag must emit +90 (CW-positive "
           "convention); the pre-fix negation emitted -90 for this input";

    // The committed file: cm matrix direction + visual orientation.
    std::vector<PageImageInfo> afterImages;
    QImage after = saveReopenRender(doc.get(), "rot_after", &afterImages);
    ASSERT_FALSE(after.isNull());
    ASSERT_EQ(afterImages.size(), 1u);
    const auto& rot = afterImages[0];
    // (b) cm encodes CW: atan2(b, a) is CCW-positive in user space, so a
    // CW-90 placement has rotationRadians ~ -pi/2 and ctm.b < 0.
    EXPECT_NEAR(rot.rotationRadians, -kPi / 2.0f, 0.05f)
        << "committed cm must encode CLOCKWISE 90 (atan2(b,a) ~ -pi/2), got "
        << rot.rotationRadians;
    EXPECT_LT(rot.ctm[1], -1.0f)
        << "cm b component must be negative for a clockwise placement";
    // (c) visual: the red (formerly top) half is now on the RIGHT.
    PixBox red = locatePixels(after, isRed);
    PixBox blue = locatePixels(after, isBlue);
    ASSERT_GT(red.count, 1000) << "rotated image lost its red half";
    ASSERT_GT(blue.count, 1000) << "rotated image lost its blue half";
    EXPECT_GT(red.cx(), blue.cx() + 20.0)
        << "CW 90 must carry the top (red) half to the RIGHT; it is not — "
           "the committed rotation ran the wrong way";
    EXPECT_NEAR(red.cy(), blue.cy(), 2.0)
        << "after a quarter turn the halves must be y-aligned";

    page.reset();
    doc.reset();
}
