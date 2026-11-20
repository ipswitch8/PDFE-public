// -*- coding: utf-8 -*-
// RED probes, pipeline image-placement-state, phase 1 (audit; no change under src/).
//
// Each probe asserts the CORRECT behaviour and therefore FAILS on today's code
// for the documented reason. Oracle-sanity conditions are ASSERTed so that no
// failure is a setup failure. Fixtures are built with raw MuPDF; the pixel
// oracle is Page::render (it has alpha: unpainted = transparent).
//
//   W1  WatermarkState.W1_ImageWatermarkKeepsPagesOwnGS0
//       BatchEngine's image watermark writes /GS0 and /WMImg into the page's
//       /Resources with pdf_dict_puts. A page whose content already uses its
//       own /GS0 (CA/ca 0.3) must keep that ExtGState: the original content
//       stays translucent and /GS0 stays 0.3 in the saved file.
//   W2  WatermarkState.W2_InheritedResourcesImageSurvivesWatermark
//       WatermarkState.W2_InheritedResourcesTextSurvivesWatermark
//       A page whose /Resources are INHERITED from the Pages node gets a new
//       EMPTY /Resources on the page dict, shadowing the inherited ones. The
//       page's own image and text must still render after the watermark.
//       WatermarkState.W2_TextWatermarkUsesItsOwnFontNotPagesF1
//       ensureF1Helvetica reuses an existing /F1 whatever it is: with /F1 =
//       Symbol the "DRAFT" watermark is typeset in Symbol and no longer reads
//       DRAFT.
//   W3  WatermarkState.W3_ImageWatermarkIsCentredOnRotatedPage
//       On a /Rotate 90 page the image watermark's rendered centre must be the
//       rendered page centre (pdf_bound_page is rotated, the raw MediaBox
//       origin is not).
//   W4  WatermarkState.W4_ImageWatermarkHonoursRotation
//       WatermarkState.W4_ImageWatermarkHonoursPosition
//       WatermarkDialog keeps the Rotation spin and Position combo enabled in
//       Image mode. EXPECTED BEHAVIOUR CHOSEN: they WORK (not "disabled").
//       The dialog hands cfg.rotation / cfg.position to
//       BatchEngine::applyWatermarkExt, so the engine must apply them to an
//       image watermark: rotation 45 tilts the image, "top-left" moves it.
//   X1  ExifState.X1_SMaskSurvivesStrip
//       ExifState.X1_StencilMaskSurvivesStrip
//       ExifStripper rewrites every image XObject with
//       fz_new_image_from_pixmap(..., nullptr): an /SMask, or an /ImageMask
//       stencil, must survive (transparent region stays transparent).
//   X2  ExifState.X2_CmykColoursSurviveStrip
//       pixmapToQImage maps n==4 to RGBA8888, so a CMYK image's K is read as
//       alpha: the rendered colour must be unchanged within tolerance.
//   X3  NOT PROBED (see findings-watermark-exif.md): the stripper documents a
//       QImage round trip of every image, so "non-JPEG stream untouched" is
//       not the intended behaviour.
//   R1  ExtractState.R1_ExtractedCmykImageMatchesRenderedColour
//       ImageManager::extractImage assumes n==4 is RGBA8888: extracting a
//       CMYK image must give the colour the page renders.
#include <gtest/gtest.h>
#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "image/ExifStripper.h"
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QApplication>
#include <QImage>
#include <QRect>
#include <QTemporaryDir>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

using namespace pdfe::core;
using namespace pdfe::image;
using namespace pdfe::batch;

namespace {

void ensureQApplication() {
    if (!QApplication::instance()) {
        static int argc = 0;
        static char* argv[] = { nullptr };
        static QApplication app(argc, argv);
    }
}

using ResourceFill = std::function<void(fz_context*, pdf_document*, pdf_obj* resources)>;

struct PageSpec {
    float w = 200.0f, h = 200.0f;
    int rotate = 0;
    std::string content;
    ResourceFill fill;            // populates the resource dictionary
    bool inheritResources = false; // move /Resources from the page to the Pages node
};

// Add a solid-colour or arbitrary pixmap as an image XObject; returns a NEW ref.
pdf_obj* addPixmapImage(fz_context* ctx, pdf_document* doc, fz_pixmap* pix) {
    fz_image* img = fz_new_image_from_pixmap(ctx, pix, nullptr);
    pdf_obj* obj = nullptr;
    fz_try(ctx) { obj = pdf_add_image(ctx, doc, img); }
    fz_always(ctx) { fz_drop_image(ctx, img); }
    fz_catch(ctx) { fz_rethrow(ctx); }
    return obj;
}

void buildPdf(const std::shared_ptr<MuPdfContext>& mc, const std::string& path, const PageSpec& s) {
    mc->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_create_document(ctx);
        fz_buffer* buf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(s.content.data()), s.content.size());
        pdf_obj* res = pdf_new_dict(ctx, doc, 4);
        if (s.fill) s.fill(ctx, doc, res);
        pdf_obj* page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, s.w, s.h), s.rotate, res, buf);
        pdf_insert_page(ctx, doc, -1, page);
        if (s.inheritResources) {
            pdf_obj* pobj = pdf_lookup_page_obj(ctx, doc, 0);
            pdf_obj* parent = pdf_dict_get(ctx, pobj, PDF_NAME(Parent));
            pdf_dict_put(ctx, parent, PDF_NAME(Resources), res);
            pdf_dict_del(ctx, pobj, PDF_NAME(Resources));
        }
        pdf_save_document(ctx, doc, path.c_str(), nullptr);
        fz_drop_buffer(ctx, buf);
        pdf_drop_obj(ctx, res);
        pdf_drop_obj(ctx, page);
        pdf_drop_document(ctx, doc);
    });
}

// Raw MuPDF read-back of the saved file (Document::fzDocument() is private).
// Returns CA of the page's /Resources /ExtGState /GS0, or -1 when absent.
float readGs0CA(const std::shared_ptr<MuPdfContext>& mc, const std::string& path) {
    float v = -1.0f;
    mc->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, path.c_str());
        pdf_obj* page = pdf_lookup_page_obj(ctx, doc, 0);
        pdf_obj* res = pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources));
        pdf_obj* gs = pdf_dict_get(ctx, res, PDF_NAME(ExtGState));
        pdf_obj* gs0 = pdf_dict_gets(ctx, gs, "GS0");
        if (gs0) v = pdf_to_real(ctx, pdf_dict_get(ctx, gs0, PDF_NAME(CA)));
        pdf_drop_document(ctx, doc);
    });
    return v;
}

// True when the first image XObject of page 0 (own or inherited resources)
// carries the given key (e.g. SMask).
bool firstImageHas(const std::shared_ptr<MuPdfContext>& mc, const std::string& path, pdf_obj* key) {
    bool has = false;
    mc->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, path.c_str());
        pdf_obj* page = pdf_lookup_page_obj(ctx, doc, 0);
        pdf_obj* res = pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources));
        pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
        if (xo && pdf_dict_len(ctx, xo) > 0) has = pdf_dict_get(ctx, pdf_dict_get_val(ctx, xo, 0), key) != nullptr;
        pdf_drop_document(ctx, doc);
    });
    return has;
}

QImage renderOf(const std::shared_ptr<MuPdfContext>& mc, const std::string& path) {
    auto doc = Document::open(mc, path);
    if (!doc) return {};
    auto page = doc->page(0);
    return page ? page->render(QRectF(), 1.0f) : QImage();
}

bool isBlue(QRgb c) { return qAlpha(c) > 128 && qBlue(c) > 200 && qRed(c) < 80 && qGreen(c) < 80; }

// Bounding box of opaque blue pixels; null rect when none.
QRect blueBox(const QImage& img) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (isBlue(img.pixel(x, y))) {
                x0 = std::min(x0, x); y0 = std::min(y0, y);
                x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
    return x1 < 0 ? QRect() : QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}


class ProbeBase : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        mc_ = std::make_shared<MuPdfContext>();
    }
    std::string path(const char* n) { return tmp_.filePath(n).toStdString(); }
    std::string solidPng(const char* n, int w, int h, QColor c) {
        const std::string p = path(n);
        QImage im(w, h, QImage::Format_RGB32);
        im.fill(c);
        EXPECT_TRUE(im.save(QString::fromStdString(p)));
        return p;
    }
    void watermarkImage(const std::string& in, const std::string& out, const std::string& png,
                        float rotation = 45.0f, const std::string& position = "center") {
        WatermarkConfig cfg;
        cfg.opacity = 1.0f;
        cfg.rotation = rotation;
        cfg.position = position;
        WatermarkExtConfig ext;
        ext.mode = "image";
        ext.imagePath = png;
        BatchEngine(mc_).applyWatermarkExt(in, out, cfg, ext);
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> mc_;
};

// A 16x16 RGB image, left half alpha 0, right half opaque red (alpha -> /SMask).
ResourceFill smaskImage() {
    return [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), 16, 16, nullptr, 1);
        unsigned char* s = fz_pixmap_samples(ctx, pix);
        const int stride = fz_pixmap_stride(ctx, pix);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                unsigned char* p = s + y * stride + x * 4;
                const bool right = x >= 8;
                p[0] = right ? 255 : 0; p[1] = 0; p[2] = 0; p[3] = right ? 255 : 0;
            }
        pdf_obj* img = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        pdf_obj* xo = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, xo, "Im1", img);
        pdf_dict_put_drop(ctx, res, PDF_NAME(XObject), xo);
        pdf_drop_obj(ctx, img);
    };
}

// A 16x16 DeviceCMYK image of one flat colour C=200 M=100 Y=0 K=50.
ResourceFill cmykImage() {
    return [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_cmyk(ctx), 16, 16, nullptr, 0);
        unsigned char* s = fz_pixmap_samples(ctx, pix);
        const int stride = fz_pixmap_stride(ctx, pix);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                unsigned char* p = s + y * stride + x * 4;
                p[0] = 200; p[1] = 100; p[2] = 0; p[3] = 50;
            }
        pdf_obj* img = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        pdf_obj* xo = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, xo, "Im1", img);
        pdf_dict_put_drop(ctx, res, PDF_NAME(XObject), xo);
        pdf_drop_obj(ctx, img);
    };
}

const char* kImageContent = "q 160 0 0 160 20 20 cm /Im1 Do Q";

} // namespace

class WatermarkState : public ProbeBase {};
class ExifState : public ProbeBase {};
class ExtractState : public ProbeBase {};

// ---------------------------------------------------------------- W1
TEST_F(WatermarkState, W1_ImageWatermarkKeepsPagesOwnGS0) {
    PageSpec s;
    s.content = "/GS0 gs 1 0 0 rg 20 20 60 60 re f";
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* gs0 = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put_real(ctx, gs0, PDF_NAME(CA), 0.3);
        pdf_dict_put_real(ctx, gs0, PDF_NAME(ca), 0.3);
        pdf_obj* egs = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts_drop(ctx, egs, "GS0", gs0);
        pdf_dict_put_drop(ctx, res, PDF_NAME(ExtGState), egs);
    };
    const std::string in = path("w1_in.pdf"), out = path("w1_out.pdf");
    buildPdf(mc_, in, s);
    // Oracle: the page's own square is translucent (alpha ~ 0.3 * 255 = 77) at
    // fitz (30,170), which the centred 80x80 watermark (60..140) does not cover.
    const QRgb before = renderOf(mc_, in).pixel(30, 170);
    ASSERT_GT(qAlpha(before), 50);
    ASSERT_LT(qAlpha(before), 110) << "oracle: original content must be translucent";
    ASSERT_NEAR(readGs0CA(mc_, in), 0.3f, 0.01f);
    const std::string png = solidPng("wm.png", 64, 64, QColor(0, 0, 255));
    ASSERT_NO_THROW(watermarkImage(in, out, png));
    const QImage after = renderOf(mc_, out);
    ASSERT_FALSE(after.isNull());
    ASSERT_TRUE(isBlue(after.pixel(100, 100))) << "oracle: the watermark itself must render";
    EXPECT_NEAR(readGs0CA(mc_, out), 0.3f, 0.01f)
        << "the page's own /GS0 ExtGState was overwritten by the watermark";
    EXPECT_LT(qAlpha(after.pixel(30, 170)), 110)
        << "existing content lost its translucency (alpha now " << qAlpha(after.pixel(30, 170)) << ")";
}

// ---------------------------------------------------------------- W2
static PageSpec inheritedSpec() {
    PageSpec s;
    // Image at fitz x 10..50 / y 150..190 (PDF y 10..50); text on the top band.
    s.content = "q 40 0 0 40 10 10 cm /Im1 Do Q BT /F1 40 Tf 10 150 Td (HELLO) Tj ET";
    s.inheritResources = true;
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), 8, 8, nullptr, 0);
        unsigned char* sm = fz_pixmap_samples(ctx, pix);
        const int stride = fz_pixmap_stride(ctx, pix);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                unsigned char* p = sm + y * stride + x * 3;
                p[0] = 255; p[1] = 0; p[2] = 0;
            }
        pdf_obj* img = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        pdf_obj* xo = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, xo, "Im1", img);
        pdf_dict_put_drop(ctx, res, PDF_NAME(XObject), xo);
        pdf_drop_obj(ctx, img);
        pdf_obj* f1 = pdf_new_dict(ctx, doc, 4);
        pdf_dict_put(ctx, f1, PDF_NAME(Type), PDF_NAME(Font));
        pdf_dict_put(ctx, f1, PDF_NAME(Subtype), PDF_NAME(Type1));
        pdf_dict_put_name(ctx, f1, PDF_NAME(BaseFont), "Symbol");
        pdf_obj* fonts = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts_drop(ctx, fonts, "F1", f1);
        pdf_dict_put_drop(ctx, res, PDF_NAME(Font), fonts);
    };
    return s;
}

TEST_F(WatermarkState, W2_InheritedResourcesImageSurvivesWatermark) {
    const std::string in = path("w2i_in.pdf"), out = path("w2i_out.pdf");
    buildPdf(mc_, in, inheritedSpec());
    const QImage before = renderOf(mc_, in);
    ASSERT_FALSE(before.isNull());
    ASSERT_EQ(qRed(before.pixel(30, 170)) > 200 && qAlpha(before.pixel(30, 170)) > 200, true)
        << "oracle: the inherited-resource image renders red before the watermark";
    const std::string png = solidPng("wm.png", 64, 64, QColor(0, 0, 255));
    ASSERT_NO_THROW(watermarkImage(in, out, png));
    const QImage after = renderOf(mc_, out);
    ASSERT_FALSE(after.isNull());
    ASSERT_TRUE(isBlue(after.pixel(100, 100))) << "oracle: the watermark itself must render";
    const QRgb c = after.pixel(30, 170);
    EXPECT_TRUE(qRed(c) > 200 && qAlpha(c) > 200 && qBlue(c) < 80)
        << "the page's inherited image vanished after the watermark";
}

TEST_F(WatermarkState, W2_InheritedResourcesTextSurvivesWatermark) {
    // The inherited font is Symbol, so the page text reads as Greek letters. If
    // the watermark shadows the inherited /Resources the font is lost and MuPDF
    // falls back to a default font: the same operators then read "HELLO".
    const std::string in = path("w2t_in.pdf"), out = path("w2t_out.pdf");
    buildPdf(mc_, in, inheritedSpec());
    std::string textBefore;
    {
        auto d = Document::open(mc_, in);
        ASSERT_NE(d, nullptr);
        textBefore = d->page(0)->extractPlainText();
    }
    ASSERT_FALSE(textBefore.empty()) << "oracle: the page has text";
    ASSERT_EQ(textBefore.find("HELLO"), std::string::npos)
        << "oracle: the inherited Symbol font is in effect before the watermark: " << textBefore;
    const std::string png = solidPng("wm.png", 64, 64, QColor(0, 0, 255));
    ASSERT_NO_THROW(watermarkImage(in, out, png));
    auto d = Document::open(mc_, out);
    ASSERT_NE(d, nullptr);
    ASSERT_TRUE(isBlue(d->page(0)->render(QRectF(), 1.0f).pixel(100, 100)))
        << "oracle: the watermark itself must render";
    EXPECT_EQ(d->page(0)->extractPlainText(), textBefore)
        << "the page's inherited font was lost after the watermark";
}

TEST_F(WatermarkState, W2_TextWatermarkUsesItsOwnFontNotPagesF1) {
    PageSpec s;
    s.content = "q Q";
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* f1 = pdf_new_dict(ctx, doc, 4);
        pdf_dict_put(ctx, f1, PDF_NAME(Type), PDF_NAME(Font));
        pdf_dict_put(ctx, f1, PDF_NAME(Subtype), PDF_NAME(Type1));
        pdf_dict_put_name(ctx, f1, PDF_NAME(BaseFont), "Symbol");
        pdf_obj* fonts = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts_drop(ctx, fonts, "F1", f1);
        pdf_dict_put_drop(ctx, res, PDF_NAME(Font), fonts);
    };
    const std::string in = path("w2f_in.pdf"), out = path("w2f_out.pdf");
    buildPdf(mc_, in, s);
    {
        auto d = Document::open(mc_, in);
        ASSERT_NE(d, nullptr);
        ASSERT_EQ(d->page(0)->extractPlainText().find("DRAFT"), std::string::npos);
    }
    WatermarkConfig cfg;
    cfg.text = "DRAFT";
    ASSERT_NO_THROW(BatchEngine(mc_).applyWatermark(in, out, cfg));
    auto d = Document::open(mc_, out);
    ASSERT_NE(d, nullptr);
    const std::string txt = d->page(0)->extractPlainText();
    ASSERT_FALSE(txt.empty()) << "oracle: the text watermark produced extractable text";
    EXPECT_NE(txt.find("DRAFT"), std::string::npos)
        << "the watermark was typeset in the page's own /F1 (Symbol); text reads: " << txt;
}

// ---------------------------------------------------------------- W3
TEST_F(WatermarkState, W3_ImageWatermarkIsCentredOnRotatedPage) {
    PageSpec s;
    s.w = 200; s.h = 300; s.rotate = 90;
    s.content = "q Q";
    const std::string in = path("w3_in.pdf"), out = path("w3_out.pdf");
    buildPdf(mc_, in, s);
    const std::string png = solidPng("wm.png", 64, 64, QColor(0, 0, 255));
    ASSERT_NO_THROW(watermarkImage(in, out, png));
    const QImage after = renderOf(mc_, out);
    ASSERT_FALSE(after.isNull());
    ASSERT_EQ(after.width(), 300) << "oracle: a /Rotate 90 page renders 300x200";
    ASSERT_EQ(after.height(), 200);
    const QRect box = blueBox(after);
    ASSERT_FALSE(box.isNull()) << "oracle: the watermark is at least partly visible";
    const QPointF c = QRectF(box).center();
    EXPECT_NEAR(c.x(), 150.0, 4.0) << "rendered watermark box " << box.x() << "," << box.y()
                                   << " " << box.width() << "x" << box.height();
    EXPECT_NEAR(c.y(), 100.0, 4.0);
}

// ---------------------------------------------------------------- W4
TEST_F(WatermarkState, W4_ImageWatermarkHonoursRotation) {
    PageSpec s;
    s.content = "q Q";
    const std::string in = path("w4r_in.pdf");
    buildPdf(mc_, in, s);
    const std::string png = solidPng("wm.png", 64, 16, QColor(0, 0, 255)); // wide bar
    const std::string flat = path("w4r_0.pdf"), tilted = path("w4r_45.pdf");
    ASSERT_NO_THROW(watermarkImage(in, flat, png, 0.0f));
    ASSERT_NO_THROW(watermarkImage(in, tilted, png, 45.0f));
    const QRect b0 = blueBox(renderOf(mc_, flat));
    ASSERT_FALSE(b0.isNull());
    ASSERT_NEAR(b0.height(), 20, 3) << "oracle: unrotated 80x20 bar";
    ASSERT_NEAR(b0.width(), 80, 3);
    const QRect b45 = blueBox(renderOf(mc_, tilted));
    ASSERT_FALSE(b45.isNull());
    EXPECT_GT(b45.height(), 50) << "rotation 45 had no effect on the image watermark (box "
                                << b45.width() << "x" << b45.height() << ")";
}

TEST_F(WatermarkState, W4_ImageWatermarkHonoursPosition) {
    PageSpec s;
    s.content = "q Q";
    const std::string in = path("w4p_in.pdf");
    buildPdf(mc_, in, s);
    const std::string png = solidPng("wm.png", 64, 16, QColor(0, 0, 255));
    const std::string centre = path("w4p_c.pdf"), tl = path("w4p_tl.pdf");
    ASSERT_NO_THROW(watermarkImage(in, centre, png, 0.0f, "center"));
    ASSERT_NO_THROW(watermarkImage(in, tl, png, 0.0f, "top-left"));
    const QRect bc = blueBox(renderOf(mc_, centre));
    ASSERT_FALSE(bc.isNull());
    ASSERT_NEAR(QRectF(bc).center().x(), 100.0, 3.0) << "oracle: centre position is centred";
    const QRect bt = blueBox(renderOf(mc_, tl));
    ASSERT_FALSE(bt.isNull());
    EXPECT_LT(QRectF(bt).center().x(), 80.0) << "Position 'top-left' ignored (box centre x "
                                             << QRectF(bt).center().x() << ")";
    EXPECT_LT(QRectF(bt).center().y(), 80.0) << "Position 'top-left' ignored (box centre y "
                                             << QRectF(bt).center().y() << ")";
}

// ---------------------------------------------------------------- X1
TEST_F(ExifState, X1_SMaskSurvivesStrip) {
    PageSpec s;
    s.content = kImageContent;
    s.fill = smaskImage();
    const std::string in = path("x1.pdf");
    buildPdf(mc_, in, s);
    ASSERT_TRUE(firstImageHas(mc_, in, PDF_NAME(SMask))) << "oracle: fixture image carries an /SMask";
    auto doc = Document::open(mc_, in);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    auto probe = [&](int x, int y) { return page->render(QRectF(), 1.0f).pixel(x, y); };
    ASSERT_LT(qAlpha(probe(60, 100)), 30) << "oracle: left half transparent";
    ASSERT_GT(qAlpha(probe(140, 100)), 200) << "oracle: right half opaque";
    ExifStripper st(mc_);
    ASSERT_EQ(st.stripPageImages(page.get(), doc.get()), 1) << "oracle: the stripper rewrote the image";
    page = doc->page(0);
    const QImage after = page->render(QRectF(), 1.0f);
    ASSERT_GT(qAlpha(after.pixel(140, 100)), 200) << "oracle: opaque half still paints";
    EXPECT_LT(qAlpha(after.pixel(60, 100)), 30) << "the soft mask was dropped: transparent half now alpha "
                                                << qAlpha(after.pixel(60, 100));
}

TEST_F(ExifState, X1_StencilMaskSurvivesStrip) {
    PageSpec s;
    s.content = std::string("1 0 0 rg ") + kImageContent;
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        // 1-bit stencil: bit 1 (left half) = not painted, bit 0 (right half) = painted.
        unsigned char bits[8];
        std::memset(bits, 0xF0, sizeof(bits));
        fz_buffer* b = fz_new_buffer_from_copied_data(ctx, bits, sizeof(bits));
        pdf_obj* d = pdf_new_dict(ctx, doc, 8);
        pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Image));
        pdf_dict_put_int(ctx, d, PDF_NAME(Width), 8);
        pdf_dict_put_int(ctx, d, PDF_NAME(Height), 8);
        pdf_dict_put_int(ctx, d, PDF_NAME(BitsPerComponent), 1);
        pdf_dict_put_bool(ctx, d, PDF_NAME(ImageMask), 1);
        pdf_obj* img = pdf_add_stream(ctx, doc, b, d, 0);
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, d);
        pdf_obj* xo = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, xo, "Im1", img);
        pdf_dict_put_drop(ctx, res, PDF_NAME(XObject), xo);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("x1m.pdf");
    buildPdf(mc_, in, s);
    ASSERT_TRUE(firstImageHas(mc_, in, PDF_NAME(ImageMask))) << "oracle: fixture is a stencil mask";
    auto doc = Document::open(mc_, in);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    const QImage b = page->render(QRectF(), 1.0f);
    ASSERT_LT(qAlpha(b.pixel(60, 100)), 30) << "oracle: masked-out half transparent";
    ASSERT_TRUE(qAlpha(b.pixel(140, 100)) > 200 && qRed(b.pixel(140, 100)) > 200 && qGreen(b.pixel(140, 100)) < 80)
        << "oracle: painted half is the red fill colour";
    ExifStripper st(mc_);
    ASSERT_EQ(st.stripPageImages(page.get(), doc.get()), 1) << "oracle: the stripper rewrote the image";
    page = doc->page(0);
    const QImage a = page->render(QRectF(), 1.0f);
    EXPECT_LT(qAlpha(a.pixel(60, 100)), 30) << "the stencil mask was flattened: masked-out half now alpha "
                                            << qAlpha(a.pixel(60, 100));
    EXPECT_TRUE(qAlpha(a.pixel(140, 100)) > 200 && qRed(a.pixel(140, 100)) > 200 && qGreen(a.pixel(140, 100)) < 80)
        << "the painted half no longer takes the fill colour";
}

// ---------------------------------------------------------------- X2
TEST_F(ExifState, X2_CmykColoursSurviveStrip) {
    PageSpec s;
    s.content = kImageContent;
    s.fill = cmykImage();
    const std::string in = path("x2.pdf");
    buildPdf(mc_, in, s);
    auto doc = Document::open(mc_, in);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    const QRgb want = page->render(QRectF(), 1.0f).pixel(100, 100);
    ASSERT_EQ(qAlpha(want), 255) << "oracle: the CMYK image paints opaquely";
    ExifStripper st(mc_);
    ASSERT_EQ(st.stripPageImages(page.get(), doc.get()), 1) << "oracle: the stripper rewrote the image";
    page = doc->page(0);
    const QRgb got = page->render(QRectF(), 1.0f).pixel(100, 100);
    const int tol = 12;
    EXPECT_TRUE(std::abs(qRed(got) - qRed(want)) <= tol && std::abs(qGreen(got) - qGreen(want)) <= tol &&
                std::abs(qBlue(got) - qBlue(want)) <= tol && qAlpha(got) >= 250)
        << "CMYK colour corrupted: rgba(" << qRed(want) << "," << qGreen(want) << "," << qBlue(want)
        << ",255) -> rgba(" << qRed(got) << "," << qGreen(got) << "," << qBlue(got) << "," << qAlpha(got) << ")";
}

// ---------------------------------------------------------------- R1
TEST_F(ExtractState, R1_ExtractedCmykImageMatchesRenderedColour) {
    PageSpec s;
    s.content = kImageContent;
    s.fill = cmykImage();
    const std::string in = path("r1.pdf");
    buildPdf(mc_, in, s);
    auto doc = Document::open(mc_, in);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    const QRgb want = page->render(QRectF(), 1.0f).pixel(100, 100);
    ASSERT_EQ(qAlpha(want), 255) << "oracle: the CMYK image paints opaquely";
    ImageManager mgr(mc_);
    ASSERT_EQ(mgr.enumerateImages(page.get(), doc.get()).size(), 1u) << "oracle: the image is enumerated";
    const QImage ex = mgr.extractImage(page.get(), doc.get(), 0);
    ASSERT_FALSE(ex.isNull()) << "oracle: extraction returned an image";
    ASSERT_GT(ex.width(), 4);
    const QColor got = ex.pixelColor(4, 4);
    const int tol = 12;
    EXPECT_TRUE(std::abs(got.red() - qRed(want)) <= tol && std::abs(got.green() - qGreen(want)) <= tol &&
                std::abs(got.blue() - qBlue(want)) <= tol && got.alpha() >= 250)
        << "extracted CMYK colour is wrong: page renders rgb(" << qRed(want) << "," << qGreen(want) << ","
        << qBlue(want) << "), extract gives rgba(" << got.red() << "," << got.green() << "," << got.blue()
        << "," << got.alpha() << ")";
}
