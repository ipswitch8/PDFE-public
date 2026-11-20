// -*- coding: utf-8 -*-
// PHASE1_HARDENING: skip=0 expect_ge=9 no_throw=0  (2026-11-17)
//
// An edit to a placed image must keep every property it was not asked to
// change, the outline must show what is on the page, and an image control must
// do what it says. GitHub #417 and the discrepancies the audit of the
// placement editors found (docs/image-placement-model.md):
//
//   D1 (GitHub #417) cropping a rotated image reset its rotation and clipped
//      in page axes;
//   D2 moving, resizing or rotating a cropped image dropped the crop;
//   D3 the outline and hit area of a cropped image were the uncropped image;
//   D4 editing one placement of an image drawn twice rewrote every placement;
//   D5 "Rotate Image..." set an absolute 90 degrees instead of rotating
//      further;
//   D6 the Image Properties panel's controls were enabled but connected to
//      nothing, and its rotation box showed 0 for a rotated image.
//
// The image is a solid red square inserted onto a blank page, so the rendered
// page is its own oracle: red where the image shows, white where it does not.
// Placement facts are read back from the page (enumeration or render), never
// from a tool's cache.
#include <gtest/gtest.h>
#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/PageContentGuard.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "ui/TestDialogSeam.h"
#include "support/ImageWindowFixture.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QImage>
#include <QRectF>
#include <QTemporaryDir>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

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

std::string fixture(const char* name) {
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"}) {
        const std::string c = std::string(prefix) + name;
        if (std::filesystem::exists(c)) return c;
    }
    const std::string d = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return std::filesystem::exists(d) ? d : std::string();
}

float angularDistance(float a, float b) {
    float d = std::fmod(std::abs(a - b), 2.0f * kPi);
    return d > kPi ? 2.0f * kPi - d : d;
}

// The image rect used throughout: upright, 200x120 at (150, 200), fitz Y-down.
const QRectF kRect(150.0, 200.0, 200.0, 120.0);

} // namespace

class ImagePlacementStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        qRegisterMetaType<std::string>("std::string");
        ASSERT_TRUE(tmp_.isValid());
        png_ = tmp_.filePath("red.png").toStdString();
        QImage red(64, 64, QImage::Format_RGB32);
        red.fill(QColor(255, 0, 0));
        ASSERT_TRUE(red.save(QString::fromStdString(png_)));
        const std::string pdf = fixture("test_single.pdf");
        ASSERT_FALSE(pdf.empty()) << "fixture test_single.pdf not found";
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        doc_ = Document::open(context_, pdf);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);
        InsertImageOptions o;
        o.filePath = png_;
        o.targetRect = kRect;
        o.maintainAspectRatio = false;
        auto r = manager_->insertImage(page_.get(), doc_.get(), o);
        ASSERT_TRUE(r.success) << r.errorMessage;
        reload();
        auto imgs = images();
        ASSERT_EQ(imgs.size(), 1u) << "test_single.pdf must start with no images";
        name_ = imgs[0].xobjectName;
        ASSERT_TRUE(isRed(kRect.center())) << "oracle: the inserted image must render red";
        ASSERT_TRUE(isWhite(kRect.topLeft() - QPointF(20, 20))) << "oracle: background is white";
    }

    void reload() { page_ = doc_->page(0); }
    std::vector<PageImageInfo> images() {
        reload();
        return manager_->enumerateImages(page_.get(), doc_.get());
    }
    PageImageInfo only() {
        auto v = images();
        EXPECT_EQ(v.size(), 1u);
        return v.empty() ? PageImageInfo{} : v[0];
    }
    QRgb pixel(QPointF p) {
        reload();
        const QImage img = page_->render(QRectF(), 1.0f);
        const int x = static_cast<int>(std::lround(p.x()));
        const int y = static_cast<int>(std::lround(p.y()));
        if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) return qRgb(0, 0, 255);
        return img.pixel(x, y);
    }
    // Page::render keeps an alpha channel, so nothing-painted is transparent,
    // not white: background means transparent or white, image means opaque red.
    bool isRed(QPointF p) {
        const QRgb c = pixel(p);
        return qAlpha(c) > 200 && qRed(c) > 200 && qGreen(c) < 90 && qBlue(c) < 90;
    }
    bool isWhite(QPointF p) {
        const QRgb c = pixel(p);
        return qAlpha(c) < 30 || (qRed(c) > 230 && qGreen(c) > 230 && qBlue(c) > 230);
    }

    // Central 50% crop of the UPRIGHT rect, as Image > Crop does it.
    void cropCentreHalf(const PageImageInfo& img) {
        const QRectF b = img.boundingBox;
        const QRectF crop(b.x() + b.width() * 0.25, b.y() + b.height() * 0.25,
                          b.width() * 0.5, b.height() * 0.5);
        ASSERT_TRUE(editor_->cropImage(page_.get(), doc_.get(), img.xobjectName, crop,
                                       img.displayWidth, img.displayHeight,
                                       static_cast<float>(b.x()), static_cast<float>(b.y())));
    }

    QTemporaryDir tmp_;
    std::string png_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    std::string name_;
};

// D1 (GitHub #417): cropping a rotated image keeps the rotation.
TEST_F(ImagePlacementStateTest, CropKeepsRotation) {
    const QPointF c = kRect.center();
    ASSERT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), name_, 30.0f,
                                     float(c.x()), float(c.y()), float(kRect.width()), float(kRect.height())));
    const PageImageInfo rotated = only();
    ASSERT_GT(std::abs(rotated.rotationRadians), 0.4f);
    cropCentreHalf(rotated);
    const PageImageInfo after = only();
    EXPECT_LT(angularDistance(after.rotationRadians, rotated.rotationRadians), 1e-3f)
        << "cropping reset the rotation (was " << rotated.rotationRadians << ", now "
        << after.rotationRadians << ")";
}

// D1 (GitHub #417): the crop is in the image's own axes. A point inside the
// kept region of a rotated image, taken in the image's own frame, stays red;
// a point the crop removes, also in the image's frame, goes white.
TEST_F(ImagePlacementStateTest, CropOfRotatedImageClipsInTheImagesAxes) {
    const QPointF c = kRect.center();
    ASSERT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), name_, 30.0f,
                                     float(c.x()), float(c.y()), float(kRect.width()), float(kRect.height())));
    const PageImageInfo rotated = only();
    cropCentreHalf(rotated);
    // Image-frame point at local (0.40w, 0) from the centre: removed by a
    // central-half crop (|0.40| > 0.25). Map it to the page by the visual
    // rotation (fitz Y-down: rotate by -rotationRadians).
    const float t = -rotated.rotationRadians;
    auto toPage = [&](double lx, double ly) {
        return QPointF(c.x() + lx * std::cos(t) - ly * std::sin(t),
                       c.y() + lx * std::sin(t) + ly * std::cos(t));
    };
    EXPECT_TRUE(isWhite(toPage(0.40 * kRect.width(), 0.0)))
        << "a point the crop removes (in the image's own axes) is still drawn";
    EXPECT_TRUE(isRed(toPage(0.20 * kRect.width(), 0.20 * kRect.height())))
        << "a point inside the kept region (in the image's own axes) is not drawn";
}

// D2: moving a cropped image keeps the crop.
TEST_F(ImagePlacementStateTest, MoveKeepsCrop) {
    cropCentreHalf(only());
    ASSERT_TRUE(isWhite(kRect.topLeft() + QPointF(10, 10))) << "precondition: the crop took effect";
    const QPointF d(80.0, 60.0);
    const PageImageInfo cropped = only();
    // Move the image by d, keeping its full (uncropped) placement size.
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_,
                                     cropped.displayWidth, cropped.displayHeight,
                                     float(kRect.x() + d.x()), float(kRect.y() + d.y())));
    EXPECT_TRUE(isWhite(kRect.topLeft() + d + QPointF(10, 10)))
        << "after a move the cropped-away corner is drawn again: the crop was dropped";
    EXPECT_TRUE(isRed(kRect.center() + d)) << "the kept centre must move with the image";
}

// D2: rotating a cropped image keeps the crop.
TEST_F(ImagePlacementStateTest, RotateKeepsCrop) {
    cropCentreHalf(only());
    const QPointF c = kRect.center();
    ASSERT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), name_, 90.0f,
                                     float(c.x()), float(c.y()), float(kRect.width()), float(kRect.height())));
    // After a quarter turn the uncropped image would cover a point 0.45*w
    // from the centre along the page's vertical axis; the cropped one does not.
    EXPECT_TRUE(isWhite(c + QPointF(0.0, 0.45 * kRect.width())))
        << "after a rotate the cropped-away region is drawn again: the crop was dropped";
    EXPECT_TRUE(isRed(c)) << "the kept centre must still be drawn";
}

// D3: the outline of a cropped image is the visible region.
TEST_F(ImagePlacementStateTest, OutlineOfACroppedImageIsTheVisibleRegion) {
    cropCentreHalf(only());
    const PageImageInfo cropped = only();
    EXPECT_NEAR(cropped.boundingBox.width(), kRect.width() * 0.5, 0.5)
        << "the outline still spans the cropped-away part of the image";
    EXPECT_NEAR(cropped.boundingBox.height(), kRect.height() * 0.5, 0.5);
    EXPECT_NEAR(cropped.boundingBox.center().x(), kRect.center().x(), 0.5);
    EXPECT_NEAR(cropped.boundingBox.center().y(), kRect.center().y(), 0.5);
}

// D4: editing one placement of an image drawn twice leaves the other alone.
TEST_F(ImagePlacementStateTest, EditingOnePlacementLeavesTheOtherCopy) {
    // Draw the SAME XObject a second time, 250 points lower. Document keeps
    // its MuPDF handle private, so: save, append the second draw with raw
    // MuPDF, save, and reopen through Document.
    const QRectF second = kRect.translated(0.0, 250.0);
    const std::string once = tmp_.filePath("once.pdf").toStdString();
    const std::string twice = tmp_.filePath("twice.pdf").toStdString();
    page_.reset();
    doc_->saveAs(once);
    bool appended = false;
    context_->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_open_document(ctx, once.c_str());
        pdf_page* pp = pdf_load_page(ctx, pdoc, 0);
        const fz_rect mb = pdf_bound_page(ctx, pp, FZ_MEDIA_BOX);
        std::ostringstream ops;
        ops << "q " << second.width() << " 0 0 " << second.height() << " "
            << (mb.x0 + second.x()) << " " << (mb.y1 - second.y() - second.height())
            << " cm /" << name_ << " Do Q\n";
        const std::string text = ops.str();
        appendContentIsolatingAmbientCtm(ctx, pdoc, pp->obj, text.c_str(), text.size());
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, pdoc, twice.c_str(), &wo);
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, pdoc);
        appended = true;
    });
    ASSERT_TRUE(appended);
    doc_ = Document::open(context_, twice);
    ASSERT_NE(doc_, nullptr);
    reload();
    ASSERT_EQ(images().size(), 2u) << "precondition: the image is drawn twice";
    ASSERT_TRUE(isRed(second.center()));

    // Move the FIRST placement only.
    ASSERT_TRUE(editor_->resizeImage(page_.get(), doc_.get(), name_,
                                     float(kRect.width()), float(kRect.height()),
                                     float(kRect.x() + 30.0), float(kRect.y())));
    EXPECT_EQ(images().size(), 2u) << "editing one placement removed the other copy";
    EXPECT_TRUE(isRed(second.center())) << "the untouched second copy is gone";
}

// ---------------------------------------------------------------------------
// D5 and D6 run through a real MainWindow.
// ---------------------------------------------------------------------------
namespace {

void settle() {
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

using pdfe::testsupport::WindowFixture;

} // namespace

// D5: "Rotate Image..." rotates a further 90 degrees each time.
TEST(ImagePlacementStateWindowTest, RotateImageActionRotatesFurtherEachTime) {
    WindowFixture f;
    ASSERT_TRUE(f.open());
    auto* rotate = f.mw.findChild<QAction*>(QStringLiteral("imageRotateAction"));
    ASSERT_NE(rotate, nullptr);
    ASSERT_TRUE(rotate->isEnabled());
    rotate->trigger();
    settle();
    const float once = f.onPage().rotationRadians;
    EXPECT_LT(angularDistance(std::abs(once), kPi / 2.0f), 1e-3f) << "first Rotate Image: a quarter turn";
    rotate->trigger();
    settle();
    const float twice = f.onPage().rotationRadians;
    EXPECT_LT(angularDistance(twice, kPi), 1e-3f)
        << "Rotate Image twice must be a half turn; got " << twice << " rad (the action "
           "set an absolute 90 degrees again)";
}

// D6: the Image Properties panel's width box resizes the image on the page.
TEST(ImagePlacementStateWindowTest, PropertiesPanelWidthResizesTheImage) {
    WindowFixture f;
    ASSERT_TRUE(f.open());
    auto* width = f.mw.findChild<QDoubleSpinBox*>(QStringLiteral("imageWidthSpin"));
    ASSERT_NE(width, nullptr);
    ASSERT_TRUE(width->isEnabled()) << "precondition: the panel offers the control";
    width->setValue(100.0);
    settle();
    EXPECT_NEAR(f.onPage().displayWidth, 100.0f, 0.5f)
        << "the panel's Width box is enabled but changing it did nothing to the image";
}

// D6: the panel's rotation box shows the image's real rotation.
TEST(ImagePlacementStateWindowTest, PropertiesPanelShowsTheRealRotation) {
    WindowFixture f;
    ASSERT_TRUE(f.open());
    emit f.tool->rotateRequested(f.name, 90.0f);
    settle();
    ASSERT_LT(angularDistance(std::abs(f.onPage().rotationRadians), kPi / 2.0f), 1e-3f);
    auto* combo = f.mw.findChild<QComboBox*>(QStringLiteral("imageRotationCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentText(), QStringLiteral("90°"))
        << "the panel shows " << combo->currentText().toStdString() << " for an image rotated 90 degrees";
}
