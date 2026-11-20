// -*- coding: utf-8 -*-
// Pipeline image-placement-state, phase 3: the image editor's UI and insert
// rules (docs/image-placement-model.md, "Image actions in the UI" and
// "Inserting an image"). These pin the decisions phase 3 made, beyond the
// phase-1 probes:
//
//   - Image > Crop / Resize keep a FIXED step (central 80% of what is visible;
//     half the size) and keep rotation and crop: Crop composes with an
//     existing crop, Resize keeps the crop and the visible centre.
//   - Rotate Image... is relative: on a 30 degree image it gives 120.
//   - The properties panel: Rotation sets an ABSOLUTE angle, shows an angle
//     that is not a quarter turn as itself in a non-selectable entry, and a
//     programmatic refresh never requests an edit; Height resizes keeping
//     rotation and crop; Crop, Extract and Remove do what the menu does; for a
//     placement inside a Form XObject every control (and every image action)
//     is disabled with a tooltip.
//   - A handle drag on a rotated image keeps the opposite corner where it is
//     on screen.
//   - insertImage: a JPEG source is stored as DCT at jpegQuality with NO
//     metadata (EXIF / APP1, comments) in the stream; a PNG source stays
//     lossless whatever the quality; the insert reports its identity; undo
//     removes the placement and the resource and redo puts both back; undo in
//     the window clears the tool and the panel.
//   - The Insert Image dialog disables the controls that would do nothing
//     (quality for a non-JPEG source; the always-on EXIF strip) with tooltips.
#include <gtest/gtest.h>
#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "image/ImageCommands.h"
#include "app/AppSettings.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "ui/InsertImageDialog.h"
#include "ui/ImagePropertiesPanel.h"
#include "ui/TestDialogSeam.h"
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStatusBar>
#include <QTest>
#include <QStandardItemModel>
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
const QRectF kRect(150.0, 200.0, 200.0, 120.0);

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

float normDeg(float d) {
    d = std::fmod(d, 360.0f);
    return d < 0 ? d + 360.0f : d;
}

QPointF rotateAbout(QPointF p, QPointF c, float radians) {
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    const QPointF d = p - c;
    return c + QPointF(d.x() * cs - d.y() * sn, d.x() * sn + d.y() * cs);
}

void settle() {
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

class PlacingMainWindow : public pdfe::ui::MainWindow {
protected:
    std::optional<InsertImageChoice> promptInsertImageOptions(const QString&, QSize, QSizeF) override {
        InsertImageChoice choice;
        choice.targetRect = kRect;
        choice.jpegQuality = 90;
        choice.stripExif = true;
        choice.maintainAspectRatio = false;
        return choice;
    }
};

// A window holding test_single.pdf with a 64x64 red PNG inserted at kRect
// through Image > Insert, the image tool active and the image selected.
struct Window {
    QTemporaryDir tmp;
    PlacingMainWindow mw;
    ImageTool* tool = nullptr;
    std::string name;

    bool open(const std::string& pdfOverride = std::string()) {
        ensureQApplication();
        qRegisterMetaType<std::string>("std::string");
        const std::string pdf = pdfOverride.empty() ? fixture("test_single.pdf") : pdfOverride;
        if (pdf.empty()) return false;
        mw.openFile(QString::fromStdString(pdf));
        settle();
        if (!mw.documentForTesting()) return false;
        if (!pdfOverride.empty()) {
            auto* toolAction = mw.findChild<QAction*>(QStringLiteral("imageToolAction"));
            if (!toolAction || !toolAction->isEnabled()) return false;
            toolAction->trigger();
            settle();
            auto* canvas = mw.findChild<pdfe::ui::PdfCanvas*>();
            tool = canvas ? canvas->imageTool() : nullptr;
            return tool != nullptr;
        }
        const std::string png = tmp.filePath("red.png").toStdString();
        QImage red(64, 64, QImage::Format_RGB32);
        red.fill(QColor(255, 0, 0));
        if (!red.save(QString::fromStdString(png))) return false;
        pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(png));
        auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
        if (!insert) return false;
        insert->trigger();
        settle();
        auto* canvas = mw.findChild<pdfe::ui::PdfCanvas*>();
        tool = canvas ? canvas->imageTool() : nullptr;
        if (!tool || !tool->hasSelection()) return false;
        name = tool->selectedImage().xobjectName;
        return true;
    }
    std::vector<PageImageInfo> all() {
        Document* doc = mw.documentForTesting();
        ImageManager mgr(doc->context());
        auto page = doc->page(0);
        return mgr.enumerateImages(page.get(), doc);
    }
    PageImageInfo onPage() {
        for (const auto& i : all())
            if (i.xobjectName == name) return i;
        ADD_FAILURE() << "image not on the page";
        return {};
    }
    template <class W> W* child(const char* objectName) {
        return mw.findChild<W*>(QString::fromLatin1(objectName));
    }
    void rotateTo(float degrees) {
        emit tool->rotateRequested(name, degrees);
        settle();
    }
    void trigger(const char* action) {
        auto* a = child<QAction>(action);
        ASSERT_NE(a, nullptr) << action;
        ASSERT_TRUE(a->isEnabled()) << action;
        a->trigger();
        settle();
    }
};

void expectSamePlacementInTool(Window& w) {
    ASSERT_TRUE(w.tool->hasSelection());
    const PageImageInfo page = w.onPage();
    const PageImageInfo tool = w.tool->selectedImage();
    EXPECT_NEAR(tool.rotationRadians, page.rotationRadians, 1e-4f);
    EXPECT_NEAR(tool.boundingBox.x(), page.boundingBox.x(), 0.01);
    EXPECT_NEAR(tool.boundingBox.y(), page.boundingBox.y(), 0.01);
    EXPECT_NEAR(tool.boundingBox.width(), page.boundingBox.width(), 0.01);
    EXPECT_NEAR(tool.boundingBox.height(), page.boundingBox.height(), 0.01);
    for (int k = 0; k < 4; ++k) EXPECT_NEAR(tool.crop[k], page.crop[k], 1e-5f);
}

// ---- raw reads of a saved file ---------------------------------------------
struct RawImage {
    std::string filter;     // first /Filter name
    bool hasMetadata = false;
    QByteArray raw;         // encoded stream bytes
};

RawImage firstImageRaw(const std::shared_ptr<MuPdfContext>& context, const std::string& file) {
    RawImage out;
    context->execute([&](fz_context* ctx) {
        pdf_document* pdf = pdf_open_document(ctx, file.c_str());
        pdf_page* pp = pdf_load_page(ctx, pdf, 0);
        pdf_obj* xo = pdf_dict_get(ctx, pdf_page_resources(ctx, pp), PDF_NAME(XObject));
        if (xo && pdf_dict_len(ctx, xo) > 0) {
            pdf_obj* o = pdf_dict_get_val(ctx, xo, 0);
            pdf_obj* f = pdf_dict_get(ctx, o, PDF_NAME(Filter));
            if (pdf_is_array(ctx, f)) f = pdf_array_get(ctx, f, 0);
            if (pdf_is_name(ctx, f)) out.filter = pdf_to_name(ctx, f);
            out.hasMetadata = pdf_dict_get(ctx, o, PDF_NAME(Metadata)) != nullptr;
            fz_buffer* b = pdf_load_raw_stream(ctx, o);
            unsigned char* d = nullptr;
            const size_t n = fz_buffer_storage(ctx, b, &d);
            out.raw = QByteArray(reinterpret_cast<const char*>(d), static_cast<qsizetype>(n));
            fz_drop_buffer(ctx, b);
        }
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, pdf);
    });
    return out;
}

std::vector<std::string> xobjectNames(const std::shared_ptr<MuPdfContext>& context,
                                      const std::string& file) {
    std::vector<std::string> names;
    context->execute([&](fz_context* ctx) {
        pdf_document* pdf = pdf_open_document(ctx, file.c_str());
        pdf_page* pp = pdf_load_page(ctx, pdf, 0);
        pdf_obj* xo = pdf_dict_get(ctx, pdf_page_resources(ctx, pp), PDF_NAME(XObject));
        for (int i = 0, n = xo ? pdf_dict_len(ctx, xo) : 0; i < n; ++i)
            names.push_back(pdf_to_name(ctx, pdf_dict_get_key(ctx, xo, i)));
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, pdf);
    });
    return names;
}

// A JPEG carrying an APP1 Exif segment (IFD0: ImageDescription = `secret`)
// and a COM comment segment holding `comment`.
QByteArray jpegWithMetadata(const QImage& img, const std::string& secret, const std::string& comment) {
    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 95);
    QByteArray tiff("II*\0\x08\0\0\0", 8);
    auto u16 = [](QByteArray& b, int v) { b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF)); };
    auto u32 = [&](QByteArray& b, int v) { u16(b, v & 0xFFFF); u16(b, (v >> 16) & 0xFFFF); };
    const int count = static_cast<int>(secret.size()) + 1;
    u16(tiff, 1);                // one entry
    u16(tiff, 0x010E);           // ImageDescription
    u16(tiff, 2);                // ASCII
    u32(tiff, count);
    u32(tiff, 26);               // value offset: after the IFD
    u32(tiff, 0);                // no next IFD
    tiff.append(secret.c_str(), count);
    QByteArray app1("\xFF\xE1", 2);
    const int len1 = 2 + 6 + static_cast<int>(tiff.size());
    app1.append(char((len1 >> 8) & 0xFF));
    app1.append(char(len1 & 0xFF));
    app1.append("Exif\0\0", 6);
    app1.append(tiff);
    QByteArray com("\xFF\xFE", 2);
    const int len2 = 2 + static_cast<int>(comment.size());
    com.append(char((len2 >> 8) & 0xFF));
    com.append(char(len2 & 0xFF));
    com.append(comment.c_str(), static_cast<qsizetype>(comment.size()));
    jpeg.insert(2, app1 + com);
    return jpeg;
}

} // namespace

// ---------------------------------------------------------------------------
// Image > Crop / Resize: the fixed step, decided in phase 3.
// ---------------------------------------------------------------------------
TEST(ImagePlacementStateUiTest, CropActionComposesWithAnExistingCropAndKeepsRotation) {
    Window w;
    ASSERT_TRUE(w.open());
    w.rotateTo(30.0f);
    const PageImageInfo rotated = w.onPage();
    ASSERT_NEAR(normDeg(visualClockwiseDegrees(rotated)), 30.0f, 0.05f) << "oracle: rotated to 30";
    ASSERT_FALSE(rotated.cropped);

    w.trigger("imageCropAction");
    const PageImageInfo once = w.onPage();
    ASSERT_TRUE(once.cropped);
    EXPECT_NEAR(once.crop[0], 0.1f, 1e-3f);
    EXPECT_NEAR(once.crop[2], 0.9f, 1e-3f);
    EXPECT_NEAR(once.crop[1], 0.1f, 1e-3f);
    EXPECT_NEAR(once.crop[3], 0.9f, 1e-3f);
    EXPECT_NEAR(once.rotationRadians, rotated.rotationRadians, 1e-4f) << "Crop changed the rotation";
    EXPECT_NEAR(once.displayWidth, rotated.displayWidth, 0.05f) << "Crop rescaled the image";
    expectSamePlacementInTool(w);

    // The second Crop keeps the central 80% of what is visible now: it
    // composes with the first (0.1..0.9 -> 0.18..0.82), it does not replace it.
    w.trigger("imageCropAction");
    const PageImageInfo twice = w.onPage();
    EXPECT_NEAR(twice.crop[0], 0.18f, 1e-3f);
    EXPECT_NEAR(twice.crop[2], 0.82f, 1e-3f);
    EXPECT_NEAR(twice.crop[1], 0.18f, 1e-3f);
    EXPECT_NEAR(twice.crop[3], 0.82f, 1e-3f);
    EXPECT_NEAR(twice.rotationRadians, rotated.rotationRadians, 1e-4f);
    EXPECT_NEAR(twice.boundingBox.center().x(), rotated.boundingBox.center().x(), 0.05);
    EXPECT_NEAR(twice.boundingBox.center().y(), rotated.boundingBox.center().y(), 0.05);
    expectSamePlacementInTool(w);
}

TEST(ImagePlacementStateUiTest, ResizeActionHalvesTheSizeAndKeepsRotationCropAndCentre) {
    Window w;
    ASSERT_TRUE(w.open());
    w.rotateTo(30.0f);
    w.trigger("imageCropAction");
    const PageImageInfo before = w.onPage();
    ASSERT_TRUE(before.cropped) << "oracle: cropped";

    w.trigger("imageResizeAction");
    const PageImageInfo after = w.onPage();
    EXPECT_NEAR(after.displayWidth, before.displayWidth * 0.5f, 0.05f);
    EXPECT_NEAR(after.displayHeight, before.displayHeight * 0.5f, 0.05f);
    EXPECT_NEAR(after.rotationRadians, before.rotationRadians, 1e-4f) << "Resize changed the rotation";
    ASSERT_TRUE(after.cropped) << "Resize dropped the crop";
    for (int k = 0; k < 4; ++k) EXPECT_NEAR(after.crop[k], before.crop[k], 1e-4f);
    EXPECT_NEAR(after.boundingBox.center().x(), before.boundingBox.center().x(), 0.05)
        << "the visible centre moved";
    EXPECT_NEAR(after.boundingBox.center().y(), before.boundingBox.center().y(), 0.05);
    expectSamePlacementInTool(w);
}

// ---------------------------------------------------------------------------
// Relative rotation.
// ---------------------------------------------------------------------------
TEST(ImagePlacementStateUiTest, RotateImageOnA30DegreeImageGives120) {
    Window w;
    ASSERT_TRUE(w.open());
    w.rotateTo(30.0f);
    ASSERT_NEAR(normDeg(visualClockwiseDegrees(w.onPage())), 30.0f, 0.05f);
    w.trigger("imageRotateAction");
    EXPECT_NEAR(normDeg(visualClockwiseDegrees(w.onPage())), 120.0f, 0.05f);
    w.trigger("imageRotate270Action");
    EXPECT_NEAR(normDeg(visualClockwiseDegrees(w.onPage())), 30.0f, 0.05f) << "120 + 270 = 390 = 30";
    expectSamePlacementInTool(w);
}

TEST(ImagePlacementStateUiTest, RotateActionsKeepTheCrop) {
    Window w;
    ASSERT_TRUE(w.open());
    w.trigger("imageCropAction");
    const PageImageInfo cropped = w.onPage();
    ASSERT_TRUE(cropped.cropped);
    w.trigger("imageRotate180Action");
    const PageImageInfo after = w.onPage();
    EXPECT_NEAR(normDeg(visualClockwiseDegrees(after)), 180.0f, 0.05f);
    ASSERT_TRUE(after.cropped);
    for (int k = 0; k < 4; ++k) EXPECT_NEAR(after.crop[k], cropped.crop[k], 1e-4f);
}

// ---------------------------------------------------------------------------
// The properties panel.
// ---------------------------------------------------------------------------
TEST(ImagePlacementStateUiTest, PanelRotationSetsAnAbsoluteAngle) {
    Window w;
    ASSERT_TRUE(w.open());
    auto* combo = w.child<QComboBox>("imageRotationCombo");
    ASSERT_NE(combo, nullptr);
    ASSERT_TRUE(combo->isEnabled());
    combo->setCurrentIndex(2);
    settle();
    EXPECT_NEAR(normDeg(visualClockwiseDegrees(w.onPage())), 180.0f, 0.05f);
    EXPECT_EQ(combo->currentText(), QString::fromUtf8("180°"));
    combo->setCurrentIndex(1);
    settle();
    EXPECT_NEAR(normDeg(visualClockwiseDegrees(w.onPage())), 90.0f, 0.05f)
        << "the panel's angle is absolute: 90 after 180 is 90, not 270";
    EXPECT_EQ(combo->currentText(), QString::fromUtf8("90°"));
}

// The panel's refresh from the page (setImageInfo) never requests an edit;
// only a user change does (the updatingInfo_ guard).
TEST(ImagePlacementStateUiTest, PanelRefreshRequestsNoEdit) {
    ensureQApplication();
    ImagePropertiesPanel panel;
    QSignalSpy resize(&panel, &ImagePropertiesPanel::resizeRequested);
    QSignalSpy rotate(&panel, &ImagePropertiesPanel::rotationRequested);
    PageImageInfo info;
    info.xobjectName = "Im0";
    info.pixelWidth = info.pixelHeight = 64;
    info.displayWidth = 200.0f;
    info.displayHeight = 120.0f;
    info.rotationRadians = -kPi / 2.0f;   // 90 clockwise
    panel.setImageInfo(info);
    info.rotationRadians = kPi;           // 180
    info.displayWidth = 50.0f;
    panel.setImageInfo(info);
    info.rotationRadians = -kPi / 6.0f;   // 30: the extra entry
    info.displayHeight = 70.0f;
    panel.setImageInfo(info);
    info.rotationRadians = 0.0f;
    panel.setImageInfo(info);
    panel.clearInfo();
    EXPECT_EQ(resize.count(), 0) << "a refresh requested a resize";
    EXPECT_EQ(rotate.count(), 0) << "a refresh requested a rotation";

    panel.setImageInfo(info);
    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("imageRotationCombo"));
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(3);  // the user picks 270
    ASSERT_EQ(rotate.count(), 1);
    EXPECT_FLOAT_EQ(rotate.at(0).at(0).toFloat(), 270.0f);
    auto* width = panel.findChild<QDoubleSpinBox*>(QStringLiteral("imageWidthSpin"));
    ASSERT_NE(width, nullptr);
    width->setValue(80.0);
    ASSERT_EQ(resize.count(), 1);
    EXPECT_FLOAT_EQ(resize.at(0).at(0).toFloat(), 80.0f);
    EXPECT_FLOAT_EQ(resize.at(0).at(1).toFloat(), 70.0f);
}

TEST(ImagePlacementStateUiTest, PanelShowsAnAngleThatIsNotAQuarterTurnWithoutEditing) {
    Window w;
    ASSERT_TRUE(w.open());
    w.rotateTo(30.0f);
    auto* combo = w.child<QComboBox>("imageRotationCombo");
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentText(), QString::fromUtf8("30°")) << "never silently 0";
    ASSERT_EQ(combo->count(), 5);
    EXPECT_EQ(combo->currentIndex(), 4);
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    ASSERT_NE(model, nullptr);
    EXPECT_FALSE(model->item(4)->flags() & Qt::ItemIsEnabled) << "the shown angle is not a choice";
    EXPECT_NEAR(normDeg(visualClockwiseDegrees(w.onPage())), 30.0f, 0.05f)
        << "showing the angle changed the image";

    // Showing the angle requested no edit: ONE undo takes the image back to
    // upright (if the refresh had committed a 0/90/... edit, one undo would
    // only undo that).
    w.trigger("undoAction");
    EXPECT_NEAR(w.onPage().rotationRadians, 0.0f, 1e-4f);
    EXPECT_EQ(combo->currentText(), QString::fromUtf8("0°")) << "the panel follows the undo";
    EXPECT_EQ(combo->count(), 4) << "the extra entry is gone at a quarter turn";
}

TEST(ImagePlacementStateUiTest, PanelHeightResizesKeepingRotationAndCrop) {
    Window w;
    ASSERT_TRUE(w.open());
    w.rotateTo(90.0f);
    w.trigger("imageCropAction");
    const PageImageInfo before = w.onPage();
    auto* height = w.child<QDoubleSpinBox>("imageHeightSpin");
    ASSERT_NE(height, nullptr);
    ASSERT_TRUE(height->isEnabled());
    EXPECT_NEAR(height->value(), before.displayHeight, 0.05) << "the panel shows the full height";
    height->setValue(60.0);
    settle();
    const PageImageInfo after = w.onPage();
    EXPECT_NEAR(after.displayHeight, 60.0f, 0.1f);
    EXPECT_NEAR(after.displayWidth, before.displayWidth, 0.1f) << "Height changed the width";
    EXPECT_NEAR(after.rotationRadians, before.rotationRadians, 1e-4f);
    ASSERT_TRUE(after.cropped);
    for (int k = 0; k < 4; ++k) EXPECT_NEAR(after.crop[k], before.crop[k], 1e-4f);
    EXPECT_NEAR(after.boundingBox.center().x(), before.boundingBox.center().x(), 0.05);
    EXPECT_NEAR(after.boundingBox.center().y(), before.boundingBox.center().y(), 0.05);
    EXPECT_NEAR(height->value(), 60.0, 0.05);
    expectSamePlacementInTool(w);
}

TEST(ImagePlacementStateUiTest, PanelCropButtonCropsLikeTheMenu) {
    Window w;
    ASSERT_TRUE(w.open());
    auto* crop = w.child<QPushButton>("imageCropButton");
    ASSERT_NE(crop, nullptr);
    ASSERT_TRUE(crop->isEnabled());
    crop->click();
    settle();
    const PageImageInfo after = w.onPage();
    ASSERT_TRUE(after.cropped);
    EXPECT_NEAR(after.crop[0], 0.1f, 1e-3f);
    EXPECT_NEAR(after.crop[2], 0.9f, 1e-3f);
    expectSamePlacementInTool(w);
}

TEST(ImagePlacementStateUiTest, PanelExtractWritesTheImage) {
    Window w;
    ASSERT_TRUE(w.open());
    auto* extract = w.child<QPushButton>("imageExtractButton");
    ASSERT_NE(extract, nullptr);
    ASSERT_TRUE(extract->isEnabled());
    const QString out = w.tmp.filePath(QStringLiteral("extracted.png"));
    ASSERT_FALSE(QFileInfo::exists(out));
    pdfe::ui::testseam::enqueueFileDialogPath(out);
    extract->click();
    settle();
    ASSERT_TRUE(QFileInfo::exists(out)) << "Extract wrote nothing";
    const QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 64);
    EXPECT_EQ(img.height(), 64);
    const QRgb c = img.pixel(32, 32);
    EXPECT_GT(qRed(c), 200);
    EXPECT_LT(qGreen(c), 60);
    EXPECT_LT(qBlue(c), 60);
}

TEST(ImagePlacementStateUiTest, PanelRemoveRemovesTheImageAndClearsThePanel) {
    Window w;
    ASSERT_TRUE(w.open());
    auto* remove = w.child<QPushButton>("imageRemoveButton");
    ASSERT_NE(remove, nullptr);
    ASSERT_TRUE(remove->isEnabled());
    remove->click();
    settle();
    EXPECT_TRUE(w.all().empty()) << "Remove left the image on the page";
    EXPECT_FALSE(w.tool->hasSelection()) << "the tool still selects a removed image";
    EXPECT_FALSE(remove->isEnabled()) << "the panel still offers controls for a removed image";
    auto* action = w.child<QAction>("imageRemoveAction");
    ASSERT_NE(action, nullptr);
    EXPECT_FALSE(action->isEnabled());
}

TEST(ImagePlacementStateUiTest, PanelAndActionsAreDisabledForAFormPlacement) {
    ensureQApplication();
    QTemporaryDir tmp;
    auto context = std::make_shared<MuPdfContext>();
    ImageManager mgr(context);
    const std::string base = fixture("test_single.pdf");
    ASSERT_FALSE(base.empty());
    const std::string png = tmp.filePath("red.png").toStdString();
    QImage red(64, 64, QImage::Format_RGB32);
    red.fill(QColor(255, 0, 0));
    ASSERT_TRUE(red.save(QString::fromStdString(png)));
    const std::string inserted = tmp.filePath("a.pdf").toStdString();
    std::string name;
    {
        auto doc = Document::open(context, base);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        InsertImageOptions o;
        o.filePath = png;
        o.targetRect = kRect;
        o.maintainAspectRatio = false;
        auto r = mgr.insertImage(page.get(), doc.get(), o);
        ASSERT_TRUE(r.success);
        name = r.imageInfo.xobjectName;
        page.reset();
        doc->saveAs(inserted);
    }
    // Move the image into a Form XObject drawn by the page.
    const std::string formFile = tmp.filePath("form.pdf").toStdString();
    context->execute([&](fz_context* ctx) {
        pdf_document* pdf = pdf_open_document(ctx, inserted.c_str());
        pdf_page* pp = pdf_load_page(ctx, pdf, 0);
        const fz_rect mb = pdf_bound_page(ctx, pp, FZ_MEDIA_BOX);
        pdf_obj* res = pdf_page_resources(ctx, pp);
        pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
        pdf_obj* img = pdf_keep_obj(ctx, pdf_dict_gets(ctx, xo, name.c_str()));
        std::ostringstream body;
        body << "q " << kRect.width() << " 0 0 " << kRect.height() << " " << (mb.x0 + kRect.x())
             << " " << (mb.y1 - kRect.y() - kRect.height()) << " cm /" << name << " Do Q\n";
        const std::string text = body.str();
        fz_buffer* buf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(text.data()), text.size());
        pdf_obj* form = pdf_add_stream(ctx, pdf, buf, nullptr, 0);
        fz_drop_buffer(ctx, buf);
        pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
        pdf_dict_put_rect(ctx, form, PDF_NAME(BBox), mb);
        pdf_obj* fres = pdf_new_dict(ctx, pdf, 1);
        pdf_obj* fxo = pdf_new_dict(ctx, pdf, 1);
        pdf_dict_puts(ctx, fxo, name.c_str(), img);
        pdf_dict_put_drop(ctx, fres, PDF_NAME(XObject), fxo);
        pdf_dict_put_drop(ctx, form, PDF_NAME(Resources), fres);
        pdf_drop_obj(ctx, img);
        pdf_dict_dels(ctx, xo, name.c_str());
        pdf_dict_puts(ctx, xo, "Fm0", form);
        pdf_drop_obj(ctx, form);
        const std::string page = "/Fm0 Do\n";
        fz_buffer* pb = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(page.data()), page.size());
        pdf_obj* stm = pdf_add_stream(ctx, pdf, pb, nullptr, 0);
        fz_drop_buffer(ctx, pb);
        pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stm);
        pdf_drop_obj(ctx, stm);
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, pdf, formFile.c_str(), &wo);
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, pdf);
    });

    Window w;
    ASSERT_TRUE(w.open(formFile));
    w.name = name;
    ASSERT_TRUE(w.tool->selectPlacement(name, -1)) << "oracle: the form placement is selectable";
    settle();
    ASSERT_TRUE(w.tool->selectedImage().inFormXObject);
    for (const char* control : {"imageWidthSpin", "imageHeightSpin", "imageRotationCombo",
                                "imageCropButton", "imageExtractButton", "imageRemoveButton"}) {
        auto* widget = w.child<QWidget>(control);
        ASSERT_NE(widget, nullptr) << control;
        EXPECT_FALSE(widget->isEnabled()) << control << " is enabled for a form placement";
        EXPECT_TRUE(widget->toolTip().contains(QStringLiteral("Form XObject")))
            << control << " does not say why it is disabled";
    }
    for (const char* action : {"imageRemoveAction", "imageRotateAction", "imageRotate180Action",
                               "imageRotate270Action", "imageCropAction", "imageResizeAction"}) {
        auto* a = w.child<QAction>(action);
        ASSERT_NE(a, nullptr) << action;
        EXPECT_FALSE(a->isEnabled()) << action << " is enabled for a form placement";
        EXPECT_TRUE(a->toolTip().contains(QStringLiteral("Form XObject"))) << action;
    }
}

// ---------------------------------------------------------------------------
// Handle drag on a rotated image: the opposite corner stays put on screen.
// ---------------------------------------------------------------------------
TEST(ImagePlacementStateUiTest, TopLeftHandleDragOfRotatedImageKeepsTheOppositeCorner) {
    auto context = std::make_shared<MuPdfContext>();
    ImageManager mgr(context);
    PageImageInfo info;
    info.xobjectName = "Im0";
    info.displayWidth = 200.0f;
    info.displayHeight = 120.0f;
    info.boundingBox = kRect;
    info.placementBox = kRect;
    info.rotationRadians = -kPi / 6.0f;  // 30 degrees clockwise on screen
    info.pixelWidth = info.pixelHeight = 64;
    ImageTool tool(&mgr);
    // A page is needed for the mouse path; the drag never reads it.
    const std::string base = fixture("test_single.pdf");
    ASSERT_FALSE(base.empty());
    auto doc = Document::open(context, base);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    tool.setPage(page.get(), doc.get());
    tool.setImagesForTest({info}, 0);
    int fires = 0;
    float w = 0, h = 0, x = 0, y = 0;
    QObject::connect(&tool, &ImageTool::resizeRequested,
        [&](const std::string&, float nw, float nh, float nx, float ny, float) {
            w = nw; h = nh; x = nx; y = ny; ++fires;
        });
    const QPointF c = kRect.center();
    const QPointF from = rotateAbout(kRect.topLeft(), c, -info.rotationRadians);
    const QPointF to = rotateAbout(kRect.topLeft() + QPointF(-20.0, -10.0), c, -info.rotationRadians);
    auto mouse = [&](QEvent::Type t, QPointF p) {
        QMouseEvent e(t, p, p, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        if (t == QEvent::MouseButtonPress) tool.mousePressEvent(&e, p, 1.0f);
        else if (t == QEvent::MouseMove) tool.mouseMoveEvent(&e, p, 1.0f);
        else tool.mouseReleaseEvent(&e, p, 1.0f);
    };
    mouse(QEvent::MouseButtonPress, from);
    mouse(QEvent::MouseMove, to);
    mouse(QEvent::MouseButtonRelease, to);
    ASSERT_EQ(fires, 1);
    EXPECT_NEAR(w, 220.0f, 0.01f) << "own width";
    EXPECT_NEAR(h, 130.0f, 0.01f) << "own height";
    // The bottom-right corner, on screen, before and after.
    const QPointF brBefore = rotateAbout(kRect.bottomRight(), c, -info.rotationRadians);
    const QRectF r(x, y, w, h);
    const QPointF brAfter = rotateAbout(r.bottomRight(), r.center(), -info.rotationRadians);
    EXPECT_NEAR(brAfter.x(), brBefore.x(), 0.01) << "the opposite corner moved";
    EXPECT_NEAR(brAfter.y(), brBefore.y(), 0.01);
}

// ---------------------------------------------------------------------------
// Insert: encoding, metadata, identity, undo.
// ---------------------------------------------------------------------------
class ImagePlacementStateInsertTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        base_ = fixture("test_single.pdf");
        ASSERT_FALSE(base_.empty());
    }
    std::string path(const char* leaf) { return tmp_.filePath(leaf).toStdString(); }
    // Insert `file` into a fresh copy of the base and save; returns the result.
    ImageOperationResult insertAndSave(const std::string& file, int quality, bool strip,
                                       const std::string& out) {
        auto doc = Document::open(context_, base_);
        EXPECT_NE(doc, nullptr);
        if (!doc) return {};
        auto page = doc->page(0);
        InsertImageOptions o;
        o.filePath = file;
        o.targetRect = kRect;
        o.maintainAspectRatio = false;
        o.jpegQuality = quality;
        o.stripExif = strip;
        auto r = manager_->insertImage(page.get(), doc.get(), o);
        page.reset();
        doc->saveAs(out);
        return r;
    }

    QTemporaryDir tmp_;
    std::string base_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
};

TEST_F(ImagePlacementStateInsertTest, JpegSourceIsStoredAsDctWithNoMetadata) {
    QImage img(48, 32, QImage::Format_RGB32);
    img.fill(QColor(20, 120, 220));
    const std::string secret = "PDFE-TEST-GPS-51.5N-0.12W";
    const std::string comment = "PDFE-TEST-COMMENT";
    const QByteArray jpeg = jpegWithMetadata(img, secret, comment);
    ASSERT_TRUE(jpeg.contains(QByteArray::fromStdString(secret))) << "oracle: source carries EXIF";
    ASSERT_TRUE(jpeg.contains(QByteArray("Exif\0\0", 6)));
    ASSERT_TRUE(jpeg.contains(QByteArray::fromStdString(comment)));
    const std::string file = path("meta.jpg");
    {
        QFile f(QString::fromStdString(file));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        ASSERT_EQ(f.write(jpeg), jpeg.size());
    }
    ASSERT_FALSE(QImage(QString::fromStdString(file)).isNull()) << "oracle: a valid JPEG";

    for (bool strip : {true, false}) {
        const std::string out = path(strip ? "strip.pdf" : "nostrip.pdf");
        const auto r = insertAndSave(file, 80, strip, out);
        ASSERT_TRUE(r.success) << r.errorMessage;
        const RawImage raw = firstImageRaw(context_, out);
        EXPECT_EQ(raw.filter, "DCTDecode") << "a JPEG source is stored as JPEG";
        ASSERT_GE(raw.raw.size(), 4);
        EXPECT_EQ(static_cast<uchar>(raw.raw[0]), 0xFF);
        EXPECT_EQ(static_cast<uchar>(raw.raw[1]), 0xD8);
        EXPECT_FALSE(raw.raw.contains(QByteArray("Exif\0\0", 6)))
            << "the inserted stream carries an EXIF segment (stripExif=" << strip << ")";
        EXPECT_FALSE(raw.raw.contains(QByteArray("\xFF\xE1", 2))) << "an APP1 segment survived";
        EXPECT_FALSE(raw.raw.contains(QByteArray::fromStdString(secret)));
        EXPECT_FALSE(raw.raw.contains(QByteArray::fromStdString(comment)))
            << "the source's comment segment survived";
        EXPECT_FALSE(raw.hasMetadata) << "the image carries a Metadata stream";
        QFile saved(QString::fromStdString(out));
        ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
        EXPECT_FALSE(saved.readAll().contains(QByteArray::fromStdString(secret)))
            << "the EXIF text is somewhere in the saved file";
    }
}

TEST_F(ImagePlacementStateInsertTest, PngSourceStaysLosslessWhateverTheQuality) {
    QImage img(64, 64, QImage::Format_RGB32);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) img.setPixel(x, y, qRgb((x * 37) & 255, (y * 59) & 255, (x ^ y) & 255));
    const std::string png = path("art.png");
    ASSERT_TRUE(img.save(QString::fromStdString(png)));
    const std::string out = path("png.pdf");
    const auto r = insertAndSave(png, 5, true, out);
    ASSERT_TRUE(r.success) << r.errorMessage;
    EXPECT_FALSE(r.imageInfo.isJpeg);
    const RawImage raw = firstImageRaw(context_, out);
    EXPECT_NE(raw.filter, "DCTDecode") << "a PNG was stored as JPEG";
    auto doc = Document::open(context_, out);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    const QImage back = manager_->extractImage(page.get(), doc.get(), 0);
    ASSERT_EQ(back.size(), img.size());
    int diff = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if ((back.pixel(x, y) & 0xFFFFFF) != (img.pixel(x, y) & 0xFFFFFF)) ++diff;
    EXPECT_EQ(diff, 0) << "the PNG's pixels changed: it was not stored losslessly";
}

TEST_F(ImagePlacementStateInsertTest, InsertReportsItsIdentityAndUndoRedoRoundTrips) {
    const std::string png = path("red.png");
    QImage red(64, 64, QImage::Format_RGB32);
    red.fill(QColor(255, 0, 0));
    ASSERT_TRUE(red.save(QString::fromStdString(png)));
    auto doc = Document::open(context_, base_);
    ASSERT_NE(doc, nullptr);
    // A first image so the second insert is not index 0.
    {
        auto page = doc->page(0);
        InsertImageOptions o;
        o.filePath = png;
        o.targetRect = QRectF(20, 20, 50, 50);
        ASSERT_TRUE(manager_->insertImage(page.get(), doc.get(), o).success);
    }
    auto cmdPage = doc->page(0);
    InsertImageOptions o;
    o.filePath = png;
    o.targetRect = kRect;
    o.maintainAspectRatio = false;
    InsertImageCommand cmd(manager_.get(), cmdPage.get(), doc.get(), o);
    cmd.execute();
    ASSERT_TRUE(cmd.result().success);
    const std::string name = cmd.result().imageInfo.xobjectName;
    EXPECT_EQ(cmd.result().imageInfo.occurrence, 0);
    EXPECT_EQ(cmd.result().imageInfo.imageIndex, 1);
    auto enumerate = [&]() {
        auto p = doc->page(0);
        return manager_->enumerateImages(p.get(), doc.get());
    };
    auto listed = enumerate();
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[1].xobjectName, name);
    EXPECT_EQ(listed[1].occurrence, cmd.result().imageInfo.occurrence);

    cmd.undo();
    listed = enumerate();
    ASSERT_EQ(listed.size(), 1u) << "undo removed more or less than the inserted placement";
    EXPECT_NE(listed[0].xobjectName, name);
    const std::string afterUndo = path("undo.pdf");
    doc->saveAs(afterUndo);
    auto names = xobjectNames(context_, afterUndo);
    EXPECT_EQ(names.size(), 1u) << "undo left the inserted resource, or took the other one";

    cmd.redo();
    ASSERT_TRUE(cmd.result().success);
    listed = enumerate();
    ASSERT_EQ(listed.size(), 2u) << "redo did not re-insert";
    const std::string afterRedo = path("redo.pdf");
    doc->saveAs(afterRedo);
    EXPECT_EQ(xobjectNames(context_, afterRedo).size(), 2u);
}

TEST(ImagePlacementStateUiTest, UndoOfAnInsertClearsTheToolAndThePanel) {
    Window w;
    ASSERT_TRUE(w.open());
    ASSERT_EQ(w.all().size(), 1u);
    auto* remove = w.child<QPushButton>("imageRemoveButton");
    ASSERT_NE(remove, nullptr);
    ASSERT_TRUE(remove->isEnabled());
    w.trigger("undoAction");
    EXPECT_TRUE(w.all().empty()) << "undo of the insert left the image";
    EXPECT_FALSE(w.tool->hasSelection());
    EXPECT_FALSE(remove->isEnabled()) << "the panel still shows the undone image";
}

// ---------------------------------------------------------------------------
// The Insert Image dialog offers only controls that do something.
// ---------------------------------------------------------------------------
TEST(ImagePlacementStateUiTest, InsertDialogDisablesControlsThatWouldDoNothing) {
    ensureQApplication();
    QTemporaryDir tmp;
    QImage img(16, 16, QImage::Format_RGB32);
    img.fill(Qt::green);
    const QString png = tmp.filePath(QStringLiteral("a.png"));
    const QString jpg = tmp.filePath(QStringLiteral("a.jpg"));
    ASSERT_TRUE(img.save(png));
    ASSERT_TRUE(img.save(jpg, "JPEG"));
    const QSizeF a4(595.0, 842.0);
    {
        pdfe::ui::InsertImageDialog dlg(png, img.size(), a4);
        auto* quality = dlg.findChild<QSpinBox*>(QStringLiteral("insertImageJpegQualitySpin"));
        ASSERT_NE(quality, nullptr);
        EXPECT_FALSE(quality->isEnabled()) << "JPEG quality is offered for a PNG, where it does nothing";
        EXPECT_FALSE(quality->toolTip().isEmpty());
        auto* strip = dlg.findChild<QCheckBox*>(QStringLiteral("insertImageStripExifCheck"));
        ASSERT_NE(strip, nullptr);
        EXPECT_TRUE(strip->isChecked());
        EXPECT_FALSE(strip->isEnabled()) << "metadata is never copied, so the box is not a choice";
        EXPECT_TRUE(strip->toolTip().contains(QStringLiteral("always")));
    }
    {
        pdfe::ui::InsertImageDialog dlg(jpg, img.size(), a4);
        auto* quality = dlg.findChild<QSpinBox*>(QStringLiteral("insertImageJpegQualitySpin"));
        ASSERT_NE(quality, nullptr);
        EXPECT_TRUE(quality->isEnabled()) << "JPEG quality is not offered for a JPEG";
    }
}

// ---------------------------------------------------------------------------
// Claim-check remediation: Extract's network policy and failure report, the
// Width/Height commit rule, content-based format detection, the quality
// clamp and the EXIF-turned default rect.
// ---------------------------------------------------------------------------
namespace {

QString statusOf(pdfe::ui::MainWindow& mw) {
    return mw.statusBar() ? mw.statusBar()->currentMessage() : QString();
}

// Restores the user's network-path setting whatever the test does.
struct NetworkSettingGuard {
    pdfe::app::AppSettings settings;
    bool saved = settings.allowUserNetworkPaths();
    explicit NetworkSettingGuard(bool allow) { settings.setAllowUserNetworkPaths(allow); }
    ~NetworkSettingGuard() { settings.setAllowUserNetworkPaths(saved); }
};

// 64 wide x 32 high JPEG carrying EXIF Orientation 6 (shown 32 x 64).
QByteArray exifOrientation6Jpeg() {
    QImage wide(64, 32, QImage::Format_RGB32);
    wide.fill(QColor(255, 0, 0));
    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    wide.save(&buf, "JPEG", 100);
    const unsigned char app1[] = {
        0xFF, 0xE1, 0x00, 0x22,
        'E', 'x', 'i', 'f', 0, 0,
        'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x01, 0x00,
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00};
    jpeg.insert(2, reinterpret_cast<const char*>(app1), sizeof(app1));
    return jpeg;
}

bool writeBytes(const QString& file, const QByteArray& bytes) {
    QFile f(file);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

} // namespace

TEST(ImagePlacementStateUiTest, ExtractToANetworkPathIsRefusedWhenTheyAreTurnedOff) {
    NetworkSettingGuard network(/*allow=*/false);
    // Well-formed and classified as remote (the Save As policy test's target).
    const QString target = QStringLiteral("\\\\localhost\\C$\\Temp\\pdfe_p3_extract.png");
    QFile::remove(target);
    {
        Window w;
        ASSERT_TRUE(w.open());
        auto* extract = w.child<QPushButton>("imageExtractButton");
        ASSERT_NE(extract, nullptr);
        ASSERT_TRUE(extract->isEnabled());
        pdfe::ui::testseam::enqueueFileDialogPath(target);
        extract->click();
        settle();
        EXPECT_TRUE(statusOf(w.mw).contains(QStringLiteral("network locations are turned off")))
            << "no refusal; status was \"" << statusOf(w.mw).toStdString() << "\"";
        EXPECT_FALSE(QFileInfo::exists(target)) << "the refused extract wrote a file anyway";
        EXPECT_TRUE(pdfe::ui::testseam::takeFileDialogPath().isEmpty()) << "the seam path was not used";
    }
    QFile::remove(target);
}

TEST(ImagePlacementStateUiTest, ExtractThatCannotWriteSaysSo) {
    Window w;
    ASSERT_TRUE(w.open());
    auto* extract = w.child<QPushButton>("imageExtractButton");
    ASSERT_NE(extract, nullptr);
    const QString target = w.tmp.filePath(QStringLiteral("no-such-dir/extracted.png"));
    ASSERT_FALSE(QFileInfo::exists(QFileInfo(target).path())) << "oracle: the folder is missing";
    pdfe::ui::testseam::enqueueFileDialogPath(target);
    extract->click();
    settle();
    EXPECT_FALSE(QFileInfo::exists(target));
    EXPECT_TRUE(statusOf(w.mw).contains(QStringLiteral("could not be extracted")))
        << "a failed extract was not reported; status was \"" << statusOf(w.mw).toStdString() << "\"";
}

TEST(ImagePlacementStateUiTest, ExtractSuccessIsReported) {
    Window w;
    ASSERT_TRUE(w.open());
    const QString target = w.tmp.filePath(QStringLiteral("ok.png"));
    pdfe::ui::testseam::enqueueFileDialogPath(target);
    w.child<QPushButton>("imageExtractButton")->click();
    settle();
    ASSERT_TRUE(QFileInfo::exists(target));
    EXPECT_TRUE(statusOf(w.mw).contains(QStringLiteral("Image extracted to ok.png")));
}

// Typing commits ONE resize, on Enter (Width) or focus-out (Height), not one
// per keystroke: each keystroke would otherwise be its own undo entry.
TEST(ImagePlacementStateUiTest, TypingAWidthOrHeightCommitsOnceOnEnterOrFocusOut) {
    Window w;
    ASSERT_TRUE(w.open());
    auto* width = w.child<QDoubleSpinBox>("imageWidthSpin");
    auto* height = w.child<QDoubleSpinBox>("imageHeightSpin");
    ASSERT_NE(width, nullptr);
    ASSERT_NE(height, nullptr);
    ASSERT_NEAR(w.onPage().displayWidth, 200.0f, 0.05f);

    width->selectAll();
    QTest::keyClicks(width, QStringLiteral("150"));
    settle();
    EXPECT_NEAR(w.onPage().displayWidth, 200.0f, 0.05f)
        << "a keystroke committed a resize before Enter";
    QTest::keyClick(width, Qt::Key_Return);
    settle();
    EXPECT_NEAR(w.onPage().displayWidth, 150.0f, 0.1f) << "Enter did not commit the typed width";
    // Exactly one entry: one undo goes back to the original 200, not to an
    // intermediate 15 or 1.
    w.trigger("undoAction");
    EXPECT_NEAR(w.onPage().displayWidth, 200.0f, 0.1f)
        << "typing \"150\" made more than one undo entry";
    ASSERT_EQ(w.all().size(), 1u) << "one undo removed the insert too: no resize entry existed";

    height->selectAll();
    QTest::keyClicks(height, QStringLiteral("75"));
    settle();
    EXPECT_NEAR(w.onPage().displayHeight, 120.0f, 0.05f)
        << "a keystroke committed a resize before focus-out";
    QFocusEvent out(QEvent::FocusOut, Qt::TabFocusReason);
    QApplication::sendEvent(height, &out);
    settle();
    EXPECT_NEAR(w.onPage().displayHeight, 75.0f, 0.1f) << "focus-out did not commit the typed height";
    w.trigger("undoAction");
    EXPECT_NEAR(w.onPage().displayHeight, 120.0f, 0.1f)
        << "typing \"75\" made more than one undo entry";
    EXPECT_EQ(w.all().size(), 1u);
}

// The format is read from the bytes, not the name.
TEST_F(ImagePlacementStateInsertTest, SourceFormatIsReadFromContentNotExtension) {
    QImage img(48, 48, QImage::Format_RGB32);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x) img.setPixel(x, y, qRgb((x * 41) & 255, (y * 23) & 255, 90));
    // PNG bytes named .jpg
    const std::string pngAsJpg = path("png-content.jpg");
    ASSERT_TRUE(img.save(QString::fromStdString(pngAsJpg), "PNG"));
    // JPEG bytes named .png
    const std::string jpgAsPng = path("jpeg-content.png");
    ASSERT_TRUE(img.save(QString::fromStdString(jpgAsPng), "JPEG", 90));
    {
        QFile f(QString::fromStdString(pngAsJpg));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        ASSERT_TRUE(f.read(4) == QByteArray("\x89PNG", 4)) << "oracle: PNG bytes";
    }
    {
        QFile f(QString::fromStdString(jpgAsPng));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        ASSERT_TRUE(f.read(2) == QByteArray("\xFF\xD8", 2)) << "oracle: JPEG bytes";
    }

    const auto rp = insertAndSave(pngAsJpg, 20, true, path("a.pdf"));
    ASSERT_TRUE(rp.success) << rp.errorMessage;
    EXPECT_FALSE(rp.imageInfo.isJpeg) << "PNG content named .jpg was stored as JPEG";
    EXPECT_NE(firstImageRaw(context_, path("a.pdf")).filter, "DCTDecode");

    const auto rj = insertAndSave(jpgAsPng, 20, true, path("b.pdf"));
    ASSERT_TRUE(rj.success) << rj.errorMessage;
    EXPECT_TRUE(rj.imageInfo.isJpeg) << "JPEG content named .png was stored losslessly";
    EXPECT_EQ(firstImageRaw(context_, path("b.pdf")).filter, "DCTDecode");

    const QSizeF a4(595.0, 842.0);
    {
        pdfe::ui::InsertImageDialog dlg(QString::fromStdString(pngAsJpg), img.size(), a4);
        auto* q = dlg.findChild<QSpinBox*>(QStringLiteral("insertImageJpegQualitySpin"));
        ASSERT_NE(q, nullptr);
        EXPECT_FALSE(q->isEnabled()) << "quality offered for PNG content named .jpg";
    }
    {
        pdfe::ui::InsertImageDialog dlg(QString::fromStdString(jpgAsPng), img.size(), a4);
        auto* q = dlg.findChild<QSpinBox*>(QStringLiteral("insertImageJpegQualitySpin"));
        ASSERT_NE(q, nullptr);
        EXPECT_TRUE(q->isEnabled()) << "quality not offered for JPEG content named .png";
    }
}

// jpegQuality is clamped to 0..100: -20 encodes as 0 and 150 as 100.
TEST_F(ImagePlacementStateInsertTest, JpegQualityIsClampedToZeroToHundred) {
    QImage noise(96, 96, QImage::Format_RGB32);
    quint32 seed = 777;
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 96; ++x) {
            seed = seed * 1664525u + 1013904223u;
            noise.setPixel(x, y, qRgb((seed >> 8) & 255, (seed >> 16) & 255, (seed >> 24) & 255));
        }
    const std::string jpg = path("noise.jpg");
    ASSERT_TRUE(noise.save(QString::fromStdString(jpg), "JPEG", 95));
    auto raw = [&](int q, const char* leaf) {
        const auto r = insertAndSave(jpg, q, true, path(leaf));
        EXPECT_TRUE(r.success);
        return firstImageRaw(context_, path(leaf)).raw;
    };
    const QByteArray q0 = raw(0, "q0.pdf"), qNeg = raw(-20, "qneg.pdf");
    const QByteArray q100 = raw(100, "q100.pdf"), qBig = raw(150, "qbig.pdf");
    ASSERT_FALSE(q0.isEmpty());
    ASSERT_NE(q0, q100) << "oracle: 0 and 100 encode differently";
    EXPECT_EQ(qNeg, q0) << "jpegQuality -20 was not clamped to 0";
    EXPECT_EQ(qBig, q100) << "jpegQuality 150 was not clamped to 100";
}

// MainWindow sizes the default insert rect from the EXIF-turned image.
namespace {
class SizeCapturingWindow : public pdfe::ui::MainWindow {
public:
    QSize seen;
    bool asked = false;
protected:
    std::optional<InsertImageChoice> promptInsertImageOptions(const QString&, QSize source, QSizeF) override {
        seen = source;
        asked = true;
        return std::nullopt;  // cancel: only the size passed in matters
    }
};
} // namespace

TEST(ImagePlacementStateUiTest, DefaultInsertRectUsesTheExifTurnedSize) {
    ensureQApplication();
    QTemporaryDir tmp;
    const QString file = tmp.filePath(QStringLiteral("turned.jpg"));
    ASSERT_TRUE(writeBytes(file, exifOrientation6Jpeg()));
    {
        QImageReader rd(file);
        ASSERT_EQ(rd.size(), QSize(64, 32)) << "oracle: stored 64x32";
        ASSERT_EQ(rd.transformation(), QImageIOHandler::TransformationRotate90) << "oracle: Orientation 6";
    }
    const std::string pdf = fixture("test_single.pdf");
    ASSERT_FALSE(pdf.empty());
    SizeCapturingWindow mw;
    mw.openFile(QString::fromStdString(pdf));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr);
    pdfe::ui::testseam::enqueueFileDialogPath(file);
    auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
    ASSERT_NE(insert, nullptr);
    insert->trigger();
    settle();
    ASSERT_TRUE(mw.asked);
    EXPECT_EQ(mw.seen, QSize(32, 64)) << "the default rect was sized from the sideways pixels";
}
