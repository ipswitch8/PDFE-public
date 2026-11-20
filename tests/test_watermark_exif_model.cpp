// -*- coding: utf-8 -*-
// Phase 4 of pipeline image-placement-state: the decided rules for the batch
// watermark, the image metadata stripper and image extraction. The phase-1
// probes (tests/test_watermark_exif_state.cpp) pin the reported defects; these
// tests pin the rest of each rule (docs/image-placement-model.md, "Watermark,
// metadata stripping and extraction").
//
//   Watermark resources (GitHub #448, #449)
//     WatermarkModel.ExistingWatermarkNamesAreNotReused
//     WatermarkModel.SharedResourcesOfOtherPagesAreNotEdited
//     WatermarkModel.TextIsWrittenAsALiteralString
//   Watermark placement (GitHub #450, #451)
//     WatermarkModel.ImageIsCentredOnRotate270Page
//     WatermarkModel.ImageIsCentredOnRotatedPageWithMediaBoxOrigin
//     WatermarkModel.ImagePositionPresetsPutItInEachCorner
//     WatermarkModel.ImagePositionFollowsTheRenderedPageOnRotatedPage
//     WatermarkModel.TextIsCentredOnRotatedPageAndReadsAcrossIt
//     WatermarkModel.TextPositionTopLeft
//     WatermarkDialogImageMode.RotationAndPositionAreLiveInImageMode
//   Metadata stripping (GitHub #452, #453; criterion 4)
//     ExifStripModel.JpegMetadataIsRemovedAndSoftMaskKept
//     ExifStripModel.RealExifFixtureLosesItsExifAndKeepsItsPixels
//     ExifStripModel.AdobeCmykJpegKeepsItsColours
//     ExifStripModel.MetadataStreamsAreRemovedFromImageAndMask
//     ExifStripModel.StencilMaskWithMetadataKeepsItsStencil
//     ExifStripModel.Jp2MetadataBoxIsRemoved
//     ExifStripModel.ImagesWithNothingToStripAreUntouched
//   Security-audit and claim-check remediation: see the sections at the end of this file.
//   Extraction (GitHub #439)
//     ImageDataExtractModel.AdobeCmykJpegExtractsAsRendered
//     ImageDataExtractModel.GrayAndRgbExtractUnchanged
#include <gtest/gtest.h>
#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "image/ExifStripper.h"
#include "image/ImageManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/JpxSecurityGate.h"
#include "ui/WatermarkDialog.h"
#include "ui/MainWindow.h"
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QMessageBox>
#include <QAction>
#include <QTimer>
#include <QRect>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

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

using Bytes = std::string;

// ------------------------------------------------------------------ fixtures

using PageFill = std::function<void(fz_context*, pdf_document*, pdf_obj* resources)>;

struct PageSpec {
    fz_rect box = fz_make_rect(0, 0, 200, 200);
    int rotate = 0;
    std::string content = "q Q";
    PageFill fill;
};

// One page per spec; when `sharedResources` the pages share ONE indirect
// /Resources dict, filled by the first spec.
void buildPdf(const std::shared_ptr<MuPdfContext>& mc, const std::string& path,
              const std::vector<PageSpec>& specs, bool sharedResources = false) {
    mc->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_create_document(ctx);
        pdf_obj* shared = nullptr;
        if (sharedResources) {
            pdf_obj* res = pdf_new_dict(ctx, doc, 4);
            if (specs[0].fill) specs[0].fill(ctx, doc, res);
            shared = pdf_add_object_drop(ctx, doc, res);
        }
        for (const PageSpec& s : specs) {
            fz_buffer* buf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(s.content.data()), s.content.size());
            pdf_obj* res = shared ? pdf_keep_obj(ctx, shared) : pdf_new_dict(ctx, doc, 4);
            if (!shared && s.fill) s.fill(ctx, doc, res);
            pdf_obj* page = pdf_add_page(ctx, doc, s.box, s.rotate, res, buf);
            pdf_insert_page(ctx, doc, -1, page);
            pdf_drop_obj(ctx, page);
            pdf_drop_obj(ctx, res);
            fz_drop_buffer(ctx, buf);
        }
        pdf_drop_obj(ctx, shared);
        pdf_save_document(ctx, doc, path.c_str(), nullptr);
        pdf_drop_document(ctx, doc);
    });
}

// Adds an image built from encoded bytes (JPEG, JP2) or a pixmap; NEW ref.
pdf_obj* addEncodedImage(fz_context* ctx, pdf_document* doc, const Bytes& data) {
    fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    fz_image* img = nullptr;
    pdf_obj* obj = nullptr;
    fz_try(ctx) {
        img = fz_new_image_from_buffer(ctx, b);
        obj = pdf_add_image(ctx, doc, img);
    }
    fz_always(ctx) { fz_drop_image(ctx, img); fz_drop_buffer(ctx, b); }
    fz_catch(ctx) { fz_rethrow(ctx); }
    return obj;
}

pdf_obj* addPixmapImage(fz_context* ctx, pdf_document* doc, fz_pixmap* pix) {
    fz_image* img = fz_new_image_from_pixmap(ctx, pix, nullptr);
    pdf_obj* obj = nullptr;
    fz_try(ctx) { obj = pdf_add_image(ctx, doc, img); }
    fz_always(ctx) { fz_drop_image(ctx, img); }
    fz_catch(ctx) { fz_rethrow(ctx); }
    return obj;
}

// A flat-colour pixmap; the caller drops it.
fz_pixmap* flatPixmap(fz_context* ctx, fz_colorspace* cs, int w, int h, std::vector<int> comps) {
    fz_pixmap* pix = fz_new_pixmap(ctx, cs, w, h, nullptr, 0);
    unsigned char* s = fz_pixmap_samples(ctx, pix);
    const int stride = fz_pixmap_stride(ctx, pix);
    const int n = fz_pixmap_components(ctx, pix);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < n; ++k) s[y * stride + x * n + k] = static_cast<unsigned char>(comps[k]);
    return pix;
}

// A 32x32 gray soft mask: left half 0 (transparent), right half 255; NEW ref.
pdf_obj* addHalfSoftMask(fz_context* ctx, pdf_document* doc) {
    fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_gray(ctx), 32, 32, nullptr, 0);
    unsigned char* s = fz_pixmap_samples(ctx, pix);
    const int stride = fz_pixmap_stride(ctx, pix);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) s[y * stride + x] = x >= 16 ? 255 : 0;
    pdf_obj* obj = addPixmapImage(ctx, doc, pix);
    fz_drop_pixmap(ctx, pix);
    return obj;
}

// An uncompressed XMP /Metadata stream holding `secret`; NEW ref.
pdf_obj* addXmpStream(fz_context* ctx, pdf_document* doc, const std::string& secret) {
    const std::string xmp = "<x:xmpmeta xmlns:x='adobe:ns:meta/'>" + secret + "</x:xmpmeta>";
    fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(xmp.data()), xmp.size());
    pdf_obj* d = pdf_new_dict(ctx, doc, 2);
    pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(Metadata));
    pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(XML));
    pdf_obj* s = pdf_add_stream(ctx, doc, b, d, 0);
    pdf_drop_obj(ctx, d);
    fz_drop_buffer(ctx, b);
    return s;
}

void putImage(fz_context* ctx, pdf_document* doc, pdf_obj* res, const char* name, pdf_obj* img) {
    pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    if (!xo) xo = pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 2);
    pdf_dict_puts(ctx, xo, name, img);
    (void)doc;
}

// JPEG segment helpers.
Bytes segment(unsigned char marker, const Bytes& payload) {
    Bytes s;
    s.push_back(static_cast<char>(0xFF));
    s.push_back(static_cast<char>(marker));
    const size_t len = payload.size() + 2;
    s.push_back(static_cast<char>((len >> 8) & 0xFF));
    s.push_back(static_cast<char>(len & 0xFF));
    return s + payload;
}

const char* kExifSecret = "PDFE-EXIF-GPS-SECRET";
const char* kXmpSecret = "PDFE-XMP-SECRET";
const char* kCommentSecret = "PDFE-COMMENT-SECRET";
const char* kIptcSecret = "PDFE-IPTC-SECRET";
const char* kTrailingSecret = "PDFE-TRAILING-COMMENT";

// The metadata segments a camera or editor writes: APP1 Exif (a valid empty
// TIFF IFD followed by the secret), APP1 XMP, APP13 Photoshop/IPTC and a
// comment after the header, plus a comment after the scan, before EOI.
Bytes withMetadata(const Bytes& jpeg) {
    Bytes exif("Exif\0\0II*\0\x08\0\0\0\0\0\0\0\0\0", 20);
    exif += kExifSecret;
    const Bytes xmp = Bytes("http://ns.adobe.com/xap/1.0/\0", 29) + "<x:xmpmeta>" + kXmpSecret + "</x:xmpmeta>";
    const Bytes iptc = Bytes("Photoshop 3.0\0", 14) + "8BIM" + kIptcSecret;
    const Bytes front = segment(0xE1, exif) + segment(0xE1, xmp) + segment(0xED, iptc) + segment(0xFE, kCommentSecret);
    // After SOI and the APP0/APP14 header segments the encoder wrote.
    size_t at = 2;
    while (at + 4 <= jpeg.size() && static_cast<unsigned char>(jpeg[at]) == 0xFF) {
        const unsigned char m = static_cast<unsigned char>(jpeg[at + 1]);
        if (m != 0xE0 && m != 0xEE) break;
        const size_t len = (static_cast<unsigned char>(jpeg[at + 2]) << 8) | static_cast<unsigned char>(jpeg[at + 3]);
        at += 2 + len;
    }
    Bytes out = jpeg.substr(0, at) + front + jpeg.substr(at);
    // A comment between the scan and EOI.
    const size_t eoi = out.size() - 2;
    if (static_cast<unsigned char>(out[eoi]) == 0xFF && static_cast<unsigned char>(out[eoi + 1]) == 0xD9)
        out.insert(eoi, segment(0xFE, kTrailingSecret));
    return out;
}

Bytes qtJpeg(QColor left, QColor right) {
    QImage im(32, 32, QImage::Format_RGB32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) im.setPixelColor(x, y, x < 16 ? left : right);
    QByteArray ba;
    QBuffer qb(&ba);
    qb.open(QIODevice::WriteOnly);
    EXPECT_TRUE(im.save(&qb, "JPG", 95));
    return Bytes(ba.constData(), static_cast<size_t>(ba.size()));
}

// A flat CMYK JPEG in the Adobe convention: an APP14 Adobe marker (libjpeg
// writes one for every CMYK JPEG) and inverted samples, so a reader that
// honours the marker sees C=200 M=100 Y=0 K=50. The samples are inverted
// here: fz_new_buffer_from_pixmap_as_jpeg's invert_cmyk argument reaches the
// writer as its "drop the pixmap" flag in MuPDF 1.26 (the arguments of the
// internal jpeg_from_pixmap call are swapped), which frees the caller's
// pixmap, so it must stay 0.
Bytes adobeCmykJpeg(const std::shared_ptr<MuPdfContext>& mc) {
    Bytes out;
    mc->execute([&](fz_context* ctx) {
        fz_pixmap* pix = flatPixmap(ctx, fz_device_cmyk(ctx), 32, 32, {255 - 200, 255 - 100, 255 - 0, 255 - 50});
        fz_buffer* b = nullptr;
        fz_try(ctx) {
            b = fz_new_buffer_from_pixmap_as_jpeg(ctx, pix, fz_default_color_params, 95, 0);
            unsigned char* d = nullptr;
            const size_t n = fz_buffer_storage(ctx, b, &d);
            out.assign(reinterpret_cast<const char*>(d), n);
        }
        fz_always(ctx) { fz_drop_buffer(ctx, b); fz_drop_pixmap(ctx, pix); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return out;
}

bool hasAdobeApp14(const Bytes& jpeg) {
    return jpeg.find(Bytes("\xFF\xEE", 2)) != Bytes::npos && jpeg.find("Adobe") != Bytes::npos;
}

Bytes readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Raw read-back of the first image XObject of page 0 of a saved file.
struct ImageState {
    bool found = false;
    std::set<std::string> keys;     // dictionary keys
    Bytes raw;                      // stream bytes as stored (filters not undone)
    std::string filter;
    std::string colorSpace;
    bool hasSMask = false, smaskHasMetadata = false, imageMask = false;
    Bytes smaskRaw;
};

ImageState readImage(const std::shared_ptr<MuPdfContext>& mc, const std::string& path, const char* name = "Im1") {
    ImageState st;
    mc->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, path.c_str());
        fz_try(ctx) {
            pdf_obj* page = pdf_lookup_page_obj(ctx, doc, 0);
            pdf_obj* res = pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources));
            pdf_obj* img = pdf_dict_gets(ctx, pdf_dict_get(ctx, res, PDF_NAME(XObject)), name);
            if (img) {
                st.found = true;
                for (int i = 0; i < pdf_dict_len(ctx, img); ++i)
                    st.keys.insert(pdf_to_name(ctx, pdf_dict_get_key(ctx, img, i)));
                st.filter = pdf_to_name(ctx, pdf_dict_get(ctx, img, PDF_NAME(Filter)));
                st.colorSpace = pdf_to_name(ctx, pdf_dict_get(ctx, img, PDF_NAME(ColorSpace)));
                st.imageMask = pdf_dict_get_bool(ctx, img, PDF_NAME(ImageMask));
                fz_buffer* b = pdf_load_raw_stream(ctx, img);
                unsigned char* d = nullptr;
                const size_t n = fz_buffer_storage(ctx, b, &d);
                st.raw.assign(reinterpret_cast<const char*>(d), n);
                fz_drop_buffer(ctx, b);
                pdf_obj* sm = pdf_dict_get(ctx, img, PDF_NAME(SMask));
                if (sm) {
                    st.hasSMask = true;
                    st.smaskHasMetadata = pdf_dict_get(ctx, sm, PDF_NAME(Metadata)) != nullptr;
                    fz_buffer* sb = pdf_load_raw_stream(ctx, sm);
                    const size_t sn = fz_buffer_storage(ctx, sb, &d);
                    st.smaskRaw.assign(reinterpret_cast<const char*>(d), sn);
                    fz_drop_buffer(ctx, sb);
                }
            }
        }
        fz_always(ctx) { pdf_drop_document(ctx, doc); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    return st;
}

QImage renderOf(const std::shared_ptr<MuPdfContext>& mc, const std::string& path, int page = 0) {
    auto doc = Document::open(mc, path);
    if (!doc) return {};
    auto p = doc->page(page);
    return p ? p->render(QRectF(), 1.0f) : QImage();
}

std::string textOf(const std::shared_ptr<MuPdfContext>& mc, const std::string& path) {
    auto doc = Document::open(mc, path);
    return doc ? doc->page(0)->extractPlainText() : std::string();
}

bool isBlue(QRgb c) { return qAlpha(c) > 128 && qBlue(c) > 200 && qRed(c) < 80 && qGreen(c) < 80; }
bool isGrey(QRgb c) {
    return qAlpha(c) > 128 && std::abs(qRed(c) - 128) < 40 && std::abs(qGreen(c) - 128) < 40 &&
           std::abs(qBlue(c) - 128) < 40;
}

QRect boxOf(const QImage& img, const std::function<bool(QRgb)>& pred) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (pred(img.pixel(x, y))) {
                x0 = std::min(x0, x); y0 = std::min(y0, y);
                x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
    return x1 < 0 ? QRect() : QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

// Copies the document to `out` with Document::saveAs (the app's save path).
void saveCopy(Document* doc, const std::string& out) { doc->saveAs(out); }

class ModelBase : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        mc_ = std::make_shared<MuPdfContext>();
    }
    std::string path(const std::string& n) { return tmp_.filePath(QString::fromStdString(n)).toStdString(); }
    std::string solidPng(const char* n, int w, int h, QColor c) {
        const std::string p = path(n);
        QImage im(w, h, QImage::Format_RGB32);
        im.fill(c);
        EXPECT_TRUE(im.save(QString::fromStdString(p)));
        return p;
    }
    void imageWatermark(const std::string& in, const std::string& out, const std::string& png,
                        float rotation, const std::string& position,
                        const std::string& range = "all", std::vector<int> pages = {}) {
        WatermarkConfig cfg;
        cfg.opacity = 1.0f;
        cfg.rotation = rotation;
        cfg.position = position;
        WatermarkExtConfig ext;
        ext.mode = "image";
        ext.imagePath = png;
        ext.rangeMode = range;
        ext.explicitPages = std::move(pages);
        BatchEngine(mc_).applyWatermarkExt(in, out, cfg, ext);
    }
    // Saves `in` twice through Document::saveAs (the app's save, which may
    // re-encode Flate streams): as it is, to `control`, and after
    // ExifStripper::stripPageImages, to `out`. Comparing `out` with `control`
    // isolates what the strip changed. Returns the strip count.
    int stripAndControl(const std::string& in, const std::string& out, const std::string& control) {
        {
            auto doc = Document::open(mc_, in);
            EXPECT_NE(doc, nullptr);
            saveCopy(doc.get(), control);
        }
        auto doc = Document::open(mc_, in);
        EXPECT_NE(doc, nullptr);
        auto page = doc->page(0);
        ExifStripper st(mc_);
        const int n = st.stripPageImages(page.get(), doc.get());
        page.reset();
        saveCopy(doc.get(), out);
        return n;
    }
    void textWatermark(const std::string& in, const std::string& out, const std::string& text,
                       float rotation, const std::string& position, float fontSize = 24.0f) {
        WatermarkConfig cfg;
        cfg.text = text;
        cfg.opacity = 1.0f;
        cfg.rotation = rotation;
        cfg.position = position;
        cfg.fontSize = fontSize;
        BatchEngine(mc_).applyWatermark(in, out, cfg);
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> mc_;
};

} // namespace

class WatermarkModel : public ModelBase {};
class ExifStripModel : public ModelBase {};
class ImageDataExtractModel : public ModelBase {};

// ============================================================ watermark: resources

// The page already uses the names the watermark prefers: an ExtGState
// PdfeWmGS (alpha 0.3), an image PdfeWmImg (red) and a font PdfeWmF (Symbol).
// The watermark picks other names, and the page's own resources still draw.
TEST_F(WatermarkModel, ExistingWatermarkNamesAreNotReused) {
    PageSpec s;
    s.content = "q /PdfeWmGS gs 0 1 0 rg 10 170 20 20 re f Q "
                "q 30 0 0 30 160 10 cm /PdfeWmImg Do Q "
                "BT /PdfeWmF 20 Tf 10 100 Td (HELLO) Tj ET";
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* gs = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put_real(ctx, gs, PDF_NAME(CA), 0.3);
        pdf_dict_put_real(ctx, gs, PDF_NAME(ca), 0.3);
        pdf_dict_put_drop(ctx, res, PDF_NAME(ExtGState), pdf_new_dict(ctx, doc, 1));
        pdf_dict_puts_drop(ctx, pdf_dict_get(ctx, res, PDF_NAME(ExtGState)), "PdfeWmGS", gs);
        fz_pixmap* pix = flatPixmap(ctx, fz_device_rgb(ctx), 8, 8, {255, 0, 0});
        pdf_obj* img = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        putImage(ctx, doc, res, "PdfeWmImg", img);
        pdf_drop_obj(ctx, img);
        pdf_obj* f = pdf_new_dict(ctx, doc, 4);
        pdf_dict_put(ctx, f, PDF_NAME(Type), PDF_NAME(Font));
        pdf_dict_put(ctx, f, PDF_NAME(Subtype), PDF_NAME(Type1));
        pdf_dict_put_name(ctx, f, PDF_NAME(BaseFont), "Symbol");
        pdf_dict_put_drop(ctx, res, PDF_NAME(Font), pdf_new_dict(ctx, doc, 1));
        pdf_dict_puts_drop(ctx, pdf_dict_get(ctx, res, PDF_NAME(Font)), "PdfeWmF", f);
    };
    const std::string in = path("names_in.pdf"), outImg = path("names_img.pdf"), outTxt = path("names_txt.pdf");
    buildPdf(mc_, in, {s});
    const QImage before = renderOf(mc_, in);
    const std::string textBefore = textOf(mc_, in);
    ASSERT_LT(qAlpha(before.pixel(20, 20)), 110) << "oracle: own ExtGState makes the square translucent";
    ASSERT_GT(qRed(before.pixel(175, 175)), 200) << "oracle: own image is red";
    ASSERT_EQ(textBefore.find("HELLO"), std::string::npos) << "oracle: own font is Symbol";

    const std::string png = solidPng("wm.png", 64, 64, QColor(0, 0, 255));
    ASSERT_NO_THROW(imageWatermark(in, outImg, png, 0.0f, "center"));
    const QImage after = renderOf(mc_, outImg);
    ASSERT_TRUE(isBlue(after.pixel(100, 100))) << "oracle: the watermark renders";
    EXPECT_LT(qAlpha(after.pixel(20, 20)), 110) << "the page's PdfeWmGS was replaced";
    EXPECT_GT(qRed(after.pixel(175, 175)), 200) << "the page's PdfeWmImg was replaced";
    EXPECT_FALSE(isBlue(after.pixel(175, 175)));

    ASSERT_NO_THROW(textWatermark(in, outTxt, "DRAFT", 0.0f, "center"));
    const std::string textAfter = textOf(mc_, outTxt);
    EXPECT_NE(textAfter.find("DRAFT"), std::string::npos) << "watermark set in the page's PdfeWmF: " << textAfter;
    EXPECT_EQ(textAfter.find("HELLO"), std::string::npos) << "the page's own PdfeWmF was replaced: " << textAfter;
}

// Two pages share one indirect /Resources dict. Watermarking page 1 only must
// leave page 2's resources and render exactly as they were.
TEST_F(WatermarkModel, SharedResourcesOfOtherPagesAreNotEdited) {
    PageSpec s;
    s.content = "/GS0 gs 1 0 0 rg 20 20 60 60 re f";
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* gs = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put_real(ctx, gs, PDF_NAME(CA), 0.3);
        pdf_dict_put_real(ctx, gs, PDF_NAME(ca), 0.3);
        pdf_dict_put_drop(ctx, res, PDF_NAME(ExtGState), pdf_new_dict(ctx, doc, 1));
        pdf_dict_puts_drop(ctx, pdf_dict_get(ctx, res, PDF_NAME(ExtGState)), "GS0", gs);
    };
    const std::string in = path("shared_in.pdf"), out = path("shared_out.pdf");
    buildPdf(mc_, in, {s, s}, /*sharedResources=*/true);
    const QImage page2Before = renderOf(mc_, in, 1);
    const std::string png = solidPng("wm.png", 64, 64, QColor(0, 0, 255));
    ASSERT_NO_THROW(imageWatermark(in, out, png, 0.0f, "center", "explicit", {0}));
    ASSERT_TRUE(isBlue(renderOf(mc_, out, 0).pixel(100, 100))) << "oracle: page 1 is watermarked";
    EXPECT_EQ(renderOf(mc_, out, 1), page2Before) << "page 2 changed";

    int page2WmEntries = -1;
    float page1Gs0 = -1.0f;
    mc_->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, out.c_str());
        pdf_obj* p2 = pdf_lookup_page_obj(ctx, doc, 1);
        pdf_obj* res2 = pdf_dict_get_inheritable(ctx, p2, PDF_NAME(Resources));
        page2WmEntries = 0;
        for (pdf_obj* cat : {PDF_NAME(ExtGState), PDF_NAME(XObject), PDF_NAME(Font)}) {
            pdf_obj* d = pdf_dict_get(ctx, res2, cat);
            for (int i = 0; i < pdf_dict_len(ctx, d); ++i)
                if (std::string(pdf_to_name(ctx, pdf_dict_get_key(ctx, d, i))).rfind("PdfeWm", 0) == 0)
                    ++page2WmEntries;
        }
        pdf_obj* p1 = pdf_lookup_page_obj(ctx, doc, 0);
        pdf_obj* res1 = pdf_dict_get_inheritable(ctx, p1, PDF_NAME(Resources));
        page1Gs0 = pdf_to_real(ctx, pdf_dict_get(ctx, pdf_dict_gets(ctx, pdf_dict_get(ctx, res1, PDF_NAME(ExtGState)), "GS0"), PDF_NAME(CA)));
        pdf_drop_document(ctx, doc);
    });
    EXPECT_EQ(page2WmEntries, 0) << "the watermark's resources were added to page 2's shared dict";
    EXPECT_NEAR(page1Gs0, 0.3f, 0.01f) << "page 1 lost its own GS0";
}

// Parentheses and backslashes in the text are escaped, not PDF syntax.
TEST_F(WatermarkModel, TextIsWrittenAsALiteralString) {
    const std::string in = path("lit_in.pdf"), out = path("lit_out.pdf");
    buildPdf(mc_, in, {PageSpec{}});
    ASSERT_NO_THROW(textWatermark(in, out, "A(B)C\\D) Tj (X", 0.0f, "center", 12.0f));
    EXPECT_NE(textOf(mc_, out).find("A(B)C\\D) Tj (X"), std::string::npos) << textOf(mc_, out);
}

// ============================================================ watermark: placement

TEST_F(WatermarkModel, ImageIsCentredOnRotate270Page) {
    PageSpec s;
    s.box = fz_make_rect(0, 0, 200, 300);
    s.rotate = 270;
    const std::string in = path("r270_in.pdf"), out = path("r270_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NO_THROW(imageWatermark(in, out, solidPng("wm.png", 64, 64, QColor(0, 0, 255)), 0.0f, "center"));
    const QImage img = renderOf(mc_, out);
    ASSERT_EQ(img.width(), 300);
    ASSERT_EQ(img.height(), 200);
    const QRect b = boxOf(img, isBlue);
    ASSERT_FALSE(b.isNull());
    EXPECT_NEAR(QRectF(b).center().x(), 150.0, 2.0);
    EXPECT_NEAR(QRectF(b).center().y(), 100.0, 2.0);
    EXPECT_NEAR(b.width(), 120, 2) << "40% of the rendered width";
    EXPECT_NEAR(b.height(), 120, 2) << "aspect kept, upright as seen";
}

TEST_F(WatermarkModel, ImageIsCentredOnRotatedPageWithMediaBoxOrigin) {
    PageSpec s;
    s.box = fz_make_rect(100, 100, 300, 400);
    s.rotate = 90;
    const std::string in = path("org_in.pdf"), out = path("org_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NO_THROW(imageWatermark(in, out, solidPng("wm.png", 64, 64, QColor(0, 0, 255)), 0.0f, "center"));
    const QImage img = renderOf(mc_, out);
    ASSERT_EQ(img.width(), 300);
    const QRect b = boxOf(img, isBlue);
    ASSERT_FALSE(b.isNull());
    EXPECT_NEAR(QRectF(b).center().x(), 150.0, 2.0);
    EXPECT_NEAR(QRectF(b).center().y(), 100.0, 2.0);
}

// 200x200 page, 64x16 image -> 80x20 bar, margin 5% of 200 = 10.
TEST_F(WatermarkModel, ImagePositionPresetsPutItInEachCorner) {
    const std::string in = path("pos_in.pdf");
    buildPdf(mc_, in, {PageSpec{}});
    const std::string png = solidPng("wm.png", 64, 16, QColor(0, 0, 255));
    struct Case { const char* pos; double cx, cy; };
    for (const Case& c : {Case{"top-left", 50, 20}, Case{"top-right", 150, 20},
                          Case{"bottom-left", 50, 180}, Case{"bottom-right", 150, 180},
                          Case{"center", 100, 100}}) {
        const std::string out = path(std::string("pos_") + c.pos + ".pdf");
        ASSERT_NO_THROW(imageWatermark(in, out, png, 0.0f, c.pos));
        const QRect b = boxOf(renderOf(mc_, out), isBlue);
        ASSERT_FALSE(b.isNull()) << c.pos;
        EXPECT_NEAR(QRectF(b).center().x(), c.cx, 2.0) << c.pos;
        EXPECT_NEAR(QRectF(b).center().y(), c.cy, 2.0) << c.pos;
    }
    // A rotated bar at a corner: its rotated bounding box keeps the margin.
    const std::string out = path("pos_tl_45.pdf");
    ASSERT_NO_THROW(imageWatermark(in, out, png, 45.0f, "top-left"));
    const QRect b = boxOf(renderOf(mc_, out), isBlue);
    ASSERT_FALSE(b.isNull());
    EXPECT_NEAR(b.left(), 10, 3);
    EXPECT_NEAR(b.top(), 10, 3);
    EXPECT_GT(b.height(), 50) << "rotation applies at a corner too";
}

// On a /Rotate 90 page the preset names the corner of the page AS SEEN.
TEST_F(WatermarkModel, ImagePositionFollowsTheRenderedPageOnRotatedPage) {
    PageSpec s;
    s.box = fz_make_rect(0, 0, 200, 300);
    s.rotate = 90;
    const std::string in = path("rpos_in.pdf"), out = path("rpos_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NO_THROW(imageWatermark(in, out, solidPng("wm.png", 64, 16, QColor(0, 0, 255)), 0.0f, "top-left"));
    const QImage img = renderOf(mc_, out);
    ASSERT_EQ(img.width(), 300);
    const QRect b = boxOf(img, isBlue);
    ASSERT_FALSE(b.isNull());
    // 120x30 bar, margin 10: centre (70, 25), wider than tall as seen.
    EXPECT_NEAR(QRectF(b).center().x(), 70.0, 2.0);
    EXPECT_NEAR(QRectF(b).center().y(), 25.0, 2.0);
    EXPECT_GT(b.width(), b.height());
}

TEST_F(WatermarkModel, TextIsCentredOnRotatedPageAndReadsAcrossIt) {
    PageSpec s;
    s.box = fz_make_rect(0, 0, 200, 300);
    s.rotate = 90;
    const std::string in = path("trot_in.pdf"), out = path("trot_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NO_THROW(textWatermark(in, out, "WATERMARK", 0.0f, "center", 24.0f));
    const QImage img = renderOf(mc_, out);
    ASSERT_EQ(img.width(), 300);
    const QRect b = boxOf(img, isGrey);
    ASSERT_FALSE(b.isNull()) << "oracle: grey text ink";
    EXPECT_NEAR(QRectF(b).center().x(), 150.0, 6.0);
    EXPECT_NEAR(QRectF(b).center().y(), 100.0, 6.0);
    EXPECT_GT(b.width(), 2 * b.height()) << "the text runs across the page as seen";
}

TEST_F(WatermarkModel, TextPositionTopLeft) {
    const std::string in = path("tpos_in.pdf"), out = path("tpos_out.pdf");
    buildPdf(mc_, in, {PageSpec{}});
    ASSERT_NO_THROW(textWatermark(in, out, "WM", 0.0f, "top-left", 20.0f));
    const QRect b = boxOf(renderOf(mc_, out), isGrey);
    ASSERT_FALSE(b.isNull());
    EXPECT_GE(b.left(), 9);
    EXPECT_LE(b.left(), 16);
    EXPECT_GE(b.top(), 9);
    EXPECT_LE(b.top(), 18);
}

TEST(WatermarkDialogImageMode, RotationAndPositionAreLiveInImageMode) {
    ensureQApplication();
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::ui::WatermarkDialog dlg(ctx, nullptr, nullptr);
    auto* mode = dlg.findChild<QComboBox*>("watermarkDialogModeCombo");
    auto* rotation = dlg.findChild<QDoubleSpinBox*>("watermarkDialogRotationSpin");
    auto* position = dlg.findChild<QComboBox*>("watermarkDialogPositionCombo");
    ASSERT_NE(mode, nullptr);
    ASSERT_NE(rotation, nullptr);
    ASSERT_NE(position, nullptr);
    mode->setCurrentIndex(mode->findData(QStringLiteral("image")));
    ASSERT_EQ(mode->currentData().toString(), QStringLiteral("image"));
    EXPECT_TRUE(rotation->isEnabled() && rotation->isVisibleTo(&dlg));
    EXPECT_TRUE(position->isEnabled() && position->isVisibleTo(&dlg));
    EXPECT_TRUE(rotation->toolTip().contains(QStringLiteral("image")));
    EXPECT_TRUE(position->toolTip().contains(QStringLiteral("image")));
    EXPECT_FALSE(rotation->accessibleName().isEmpty());
    EXPECT_FALSE(position->accessibleName().isEmpty());
}

// ============================================================ metadata stripping

// A JPEG carrying APP1 Exif, APP1 XMP, APP13 IPTC and two comments, with an
// /SMask and a /Metadata stream on the image dictionary.
TEST_F(ExifStripModel, JpegMetadataIsRemovedAndSoftMaskKept) {
    const Bytes jpeg = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addEncodedImage(ctx, doc, jpeg);
        pdf_dict_put_drop(ctx, img, PDF_NAME(SMask), addHalfSoftMask(ctx, doc));
        pdf_dict_put_drop(ctx, img, PDF_NAME(Metadata), addXmpStream(ctx, doc, "PDFE-DICT-XMP-SECRET"));
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("jpeg_in.pdf"), out = path("jpeg_out.pdf");
    buildPdf(mc_, in, {s});
    const ImageState b = readImage(mc_, in);
    ASSERT_EQ(b.filter, "DCTDecode") << "oracle: the JPEG is stored as is";
    for (const char* secret : {kExifSecret, kXmpSecret, kCommentSecret, kIptcSecret, kTrailingSecret})
        ASSERT_NE(b.raw.find(secret), Bytes::npos) << "oracle: fixture carries " << secret;
    ASSERT_TRUE(b.keys.count("Metadata") && b.hasSMask);
    ASSERT_NE(readFile(in).find("PDFE-DICT-XMP-SECRET"), Bytes::npos);
    const QImage before = renderOf(mc_, in);
    ASSERT_LT(qAlpha(before.pixel(60, 100)), 30) << "oracle: masked half transparent";
    ASSERT_GT(qAlpha(before.pixel(140, 100)), 200);

    const std::string control = out + ".control.pdf";
    EXPECT_EQ(stripAndControl(in, out, control), 1);
    const ImageState a = readImage(mc_, out);
    ASSERT_TRUE(a.found);
    for (const char* secret : {kExifSecret, kXmpSecret, kCommentSecret, kIptcSecret, kTrailingSecret})
        EXPECT_EQ(a.raw.find(secret), Bytes::npos) << secret << " survived in the JPEG";
    EXPECT_EQ(a.raw.find(Bytes("\xFF\xE1", 2)), Bytes::npos) << "an APP1 segment survived";
    EXPECT_EQ(a.raw.find(Bytes("\xFF\xFE", 2)), Bytes::npos) << "a COM segment survived";
    EXPECT_EQ(a.raw.find(Bytes("\xFF\xED", 2)), Bytes::npos) << "an APP13 segment survived";
    EXPECT_EQ(a.keys.count("Metadata"), 0u) << "the /Metadata entry survived";
    const Bytes file = readFile(out);
    for (const char* secret : {kExifSecret, kXmpSecret, kCommentSecret, kIptcSecret, kTrailingSecret, "PDFE-DICT-XMP-SECRET"})
        EXPECT_EQ(file.find(secret), Bytes::npos) << secret << " is still in the saved file";
    // Lossless: the scan data and everything that is not metadata is unchanged.
    EXPECT_EQ(a.filter, "DCTDecode");
    const size_t sosB = b.raw.find(Bytes("\xFF\xDA", 2)), sosA = a.raw.find(Bytes("\xFF\xDA", 2));
    ASSERT_NE(sosA, Bytes::npos);
    EXPECT_EQ(a.raw.substr(sosA, a.raw.size() - sosA), b.raw.substr(sosB, b.raw.size() - sosB - 2 - 4 - std::strlen(kTrailingSecret)) + "\xFF\xD9")
        << "the compressed image data changed";
    EXPECT_TRUE(a.hasSMask) << "the soft mask was dropped";
    EXPECT_EQ(a.smaskRaw, readImage(mc_, control).smaskRaw) << "the soft mask changed";
    EXPECT_EQ(renderOf(mc_, out), before) << "the strip changed what the page shows";
}

// The suite's real camera-style fixture (EXIF with GPS and Make tags).
TEST_F(ExifStripModel, RealExifFixtureLosesItsExifAndKeepsItsPixels) {
    const Bytes jpeg = readFile(std::string(TEST_FIXTURES_DIR) + "/test_image_with_exif.jpg");
    ASSERT_NE(jpeg.find("Exif"), Bytes::npos) << "oracle: the fixture carries EXIF";
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addEncodedImage(ctx, doc, jpeg);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("realexif_in.pdf"), out = path("realexif_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NE(readImage(mc_, in).raw.find("Exif"), Bytes::npos) << "oracle: stored with its EXIF";
    const QImage before = renderOf(mc_, in);
    const std::string control = out + ".control.pdf";
    EXPECT_EQ(stripAndControl(in, out, control), 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.raw.find("Exif"), Bytes::npos);
    EXPECT_EQ(a.raw.find(Bytes("\xFF\xE1", 2)), Bytes::npos);
    EXPECT_EQ(readFile(out).find("Exif"), Bytes::npos) << "the original JPEG is still in the saved file";
    EXPECT_EQ(renderOf(mc_, out), before);
}

// An Adobe (inverted) CMYK JPEG with metadata keeps its exact colours: the
// APP14 Adobe marker stays, only metadata goes.
TEST_F(ExifStripModel, AdobeCmykJpegKeepsItsColours) {
    const Bytes plain = adobeCmykJpeg(mc_);
    ASSERT_TRUE(hasAdobeApp14(plain)) << "oracle: MuPDF wrote an Adobe APP14 marker";
    const Bytes jpeg = withMetadata(plain);
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addEncodedImage(ctx, doc, jpeg);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("cmyk_in.pdf"), out = path("cmyk_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_EQ(readImage(mc_, in).filter, "DCTDecode");
    const QImage before = renderOf(mc_, in);
    const QRgb want = before.pixel(100, 100);
    // C=200 M=100 Y=0 K=50 shows as a mid blue; an Adobe-inversion mistake
    // shows its complement.
    ASSERT_GT(qBlue(want), qRed(want) + 80) << "oracle: the CMYK JPEG renders blue";
    const std::string control = out + ".control.pdf";
    EXPECT_EQ(stripAndControl(in, out, control), 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_TRUE(hasAdobeApp14(a.raw)) << "the Adobe colour-transform marker was removed";
    EXPECT_EQ(a.raw.find(kExifSecret), Bytes::npos);
    EXPECT_EQ(renderOf(mc_, out), before) << "CMYK colours changed";
}

// /Metadata on a Flate image and on its soft mask are removed; the mask stays.
TEST_F(ExifStripModel, MetadataStreamsAreRemovedFromImageAndMask) {
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        fz_pixmap* pix = flatPixmap(ctx, fz_device_rgb(ctx), 32, 32, {255, 0, 0});
        pdf_obj* img = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        pdf_obj* sm = addHalfSoftMask(ctx, doc);
        pdf_dict_put_drop(ctx, sm, PDF_NAME(Metadata), addXmpStream(ctx, doc, "PDFE-SMASK-XMP-SECRET"));
        pdf_dict_put_drop(ctx, img, PDF_NAME(SMask), sm);
        pdf_dict_put_drop(ctx, img, PDF_NAME(Metadata), addXmpStream(ctx, doc, "PDFE-IMAGE-XMP-SECRET"));
        pdf_dict_put_dict(ctx, img, PDF_NAME(PieceInfo), 1);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("meta_in.pdf"), out = path("meta_out.pdf");
    buildPdf(mc_, in, {s});
    const ImageState b = readImage(mc_, in);
    ASSERT_TRUE(b.keys.count("Metadata") && b.smaskHasMetadata && b.keys.count("PieceInfo"));
    const QImage before = renderOf(mc_, in);
    const std::string control = out + ".control.pdf";
    EXPECT_EQ(stripAndControl(in, out, control), 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.keys.count("Metadata"), 0u);
    EXPECT_EQ(a.keys.count("PieceInfo"), 0u);
    EXPECT_TRUE(a.hasSMask);
    EXPECT_FALSE(a.smaskHasMetadata) << "the soft mask's /Metadata survived";
    const ImageState c = readImage(mc_, control);
    EXPECT_EQ(a.raw, c.raw) << "the image samples were rewritten";
    EXPECT_EQ(a.smaskRaw, c.smaskRaw) << "the soft mask samples were rewritten";
    const Bytes file = readFile(out);
    EXPECT_EQ(file.find("PDFE-SMASK-XMP-SECRET"), Bytes::npos);
    EXPECT_EQ(file.find("PDFE-IMAGE-XMP-SECRET"), Bytes::npos);
    EXPECT_EQ(renderOf(mc_, out), before);
}

TEST_F(ExifStripModel, StencilMaskWithMetadataKeepsItsStencil) {
    PageSpec s;
    s.content = "1 0 0 rg q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
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
        pdf_dict_put_drop(ctx, d, PDF_NAME(Metadata), addXmpStream(ctx, doc, "PDFE-STENCIL-SECRET"));
        pdf_obj* img = pdf_add_stream(ctx, doc, b, d, 0);
        fz_drop_buffer(ctx, b);
        pdf_drop_obj(ctx, d);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("stencil_in.pdf"), out = path("stencil_out.pdf");
    buildPdf(mc_, in, {s});
    const QImage before = renderOf(mc_, in);
    ASSERT_LT(qAlpha(before.pixel(60, 100)), 30);
    ASSERT_GT(qRed(before.pixel(140, 100)), 200);
    {
        auto doc = Document::open(mc_, in);
        auto page = doc->page(0);
        ExifStripper st(mc_);
        EXPECT_TRUE(st.stripImage(page.get(), doc.get(), "Im1"));
        page.reset();
        saveCopy(doc.get(), out);
    }
    const ImageState a = readImage(mc_, out);
    EXPECT_TRUE(a.imageMask) << "the stencil became an ordinary image";
    EXPECT_EQ(a.keys.count("Metadata"), 0u);
    EXPECT_EQ(readFile(out).find("PDFE-STENCIL-SECRET"), Bytes::npos);
    EXPECT_EQ(renderOf(mc_, out), before);
}

// A JP2 file with an xml box (XMP) between its header and its codestream.
TEST_F(ExifStripModel, Jp2MetadataBoxIsRemoved) {
    const Bytes jp2 = readFile(std::string(TEST_FIXTURES_DIR) + "/jpx_legit_a4_300dpi.jp2");
    ASSERT_GT(jp2.size(), 100u) << "oracle: fixture present";
    // Insert after the jp2h box.
    const size_t jp2h = jp2.find("jp2h");
    ASSERT_NE(jp2h, Bytes::npos);
    const size_t boxStart = jp2h - 4;
    const size_t boxLen = (static_cast<unsigned char>(jp2[boxStart]) << 24) | (static_cast<unsigned char>(jp2[boxStart + 1]) << 16) |
                          (static_cast<unsigned char>(jp2[boxStart + 2]) << 8) | static_cast<unsigned char>(jp2[boxStart + 3]);
    const Bytes xmlBody = "<x:xmpmeta>PDFE-JP2-XMP-SECRET</x:xmpmeta>";
    const uint32_t xl = static_cast<uint32_t>(xmlBody.size() + 8);
    Bytes xmlBox;
    xmlBox.push_back(static_cast<char>(xl >> 24)); xmlBox.push_back(static_cast<char>(xl >> 16));
    xmlBox.push_back(static_cast<char>(xl >> 8)); xmlBox.push_back(static_cast<char>(xl));
    xmlBox += "xml " + xmlBody;
    const Bytes withXml = jp2.substr(0, boxStart + boxLen) + xmlBox + jp2.substr(boxStart + boxLen);

    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addEncodedImage(ctx, doc, withXml);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("jp2_in.pdf"), out = path("jp2_out.pdf");
    buildPdf(mc_, in, {s});
    const ImageState b = readImage(mc_, in);
    ASSERT_EQ(b.filter, "JPXDecode") << "oracle: stored as JPX";
    ASSERT_NE(b.raw.find("PDFE-JP2-XMP-SECRET"), Bytes::npos);
    const QImage before = renderOf(mc_, in);
    const std::string control = out + ".control.pdf";
    EXPECT_EQ(stripAndControl(in, out, control), 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.filter, "JPXDecode");
    EXPECT_EQ(a.raw, jp2) << "only the xml box should have gone";
    EXPECT_EQ(readFile(out).find("PDFE-JP2-XMP-SECRET"), Bytes::npos);
    EXPECT_EQ(renderOf(mc_, out), before);
}

// Nothing to strip -> nothing changes: a Flate image with a soft mask and a
// JPEG with only a JFIF header keep their exact bytes and dictionary keys,
// and both are counted as metadata-free.
TEST_F(ExifStripModel, ImagesWithNothingToStripAreUntouched) {
    const Bytes jpeg = qtJpeg(QColor(0, 0, 255), QColor(0, 255, 0));
    PageSpec s;
    s.content = "q 80 0 0 80 10 10 cm /Im1 Do Q q 80 0 0 80 110 110 cm /Im2 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        fz_pixmap* pix = flatPixmap(ctx, fz_device_rgb(ctx), 32, 32, {255, 0, 0});
        pdf_obj* img = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        pdf_dict_put_drop(ctx, img, PDF_NAME(SMask), addHalfSoftMask(ctx, doc));
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
        pdf_obj* j = addEncodedImage(ctx, doc, jpeg);
        putImage(ctx, doc, res, "Im2", j);
        pdf_drop_obj(ctx, j);
    };
    const std::string in = path("clean_in.pdf"), out = path("clean_out.pdf");
    buildPdf(mc_, in, {s});
    const ImageState in2 = readImage(mc_, in, "Im2");
    ASSERT_EQ(in2.filter, "DCTDecode");
    ASSERT_EQ(in2.raw.find(Bytes("\xFF\xE1", 2)), Bytes::npos) << "oracle: no APP1 in the plain JPEG";
    ASSERT_EQ(in2.raw.find(Bytes("\xFF\xFE", 2)), Bytes::npos) << "oracle: no COM in the plain JPEG";
    const QImage before = renderOf(mc_, in);
    const std::string control = out + ".control.pdf";
    EXPECT_EQ(stripAndControl(in, out, control), 2) << "both images are metadata-free";
    const ImageState b1 = readImage(mc_, control, "Im1"), b2 = readImage(mc_, control, "Im2");
    const ImageState a1 = readImage(mc_, out, "Im1"), a2 = readImage(mc_, out, "Im2");
    ASSERT_TRUE(b1.found && b2.found && a1.found && a2.found);
    EXPECT_EQ(b2.raw, in2.raw) << "oracle: the app's save keeps DCT data as it is";
    EXPECT_EQ(a1.raw, b1.raw);
    EXPECT_EQ(a1.keys, b1.keys);
    EXPECT_EQ(a1.smaskRaw, b1.smaskRaw);
    EXPECT_EQ(a2.raw, b2.raw) << "a JPEG with nothing to strip was rewritten";
    EXPECT_EQ(a2.keys, b2.keys);
    EXPECT_EQ(renderOf(mc_, out), before);
}

// ============================================================ extraction

TEST_F(ImageDataExtractModel, AdobeCmykJpegExtractsAsRendered) {
    const Bytes jpeg = adobeCmykJpeg(mc_);
    ASSERT_TRUE(hasAdobeApp14(jpeg));
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addEncodedImage(ctx, doc, jpeg);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("xcmyk.pdf");
    buildPdf(mc_, in, {s});
    auto doc = Document::open(mc_, in);
    auto page = doc->page(0);
    const QRgb want = page->render(QRectF(), 1.0f).pixel(100, 100);
    ImageManager mgr(mc_);
    const QImage ex = mgr.extractImage(page.get(), doc.get(), 0);
    ASSERT_FALSE(ex.isNull());
    const QColor got = ex.pixelColor(16, 16);
    EXPECT_NEAR(got.red(), qRed(want), 12);
    EXPECT_NEAR(got.green(), qGreen(want), 12);
    EXPECT_NEAR(got.blue(), qBlue(want), 12);
    EXPECT_EQ(got.alpha(), 255);
}

TEST_F(ImageDataExtractModel, GrayAndRgbExtractUnchanged) {
    PageSpec s;
    s.content = "q 80 0 0 80 10 10 cm /Im1 Do Q q 80 0 0 80 110 110 cm /Im2 Do Q";
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        fz_pixmap* g = flatPixmap(ctx, fz_device_gray(ctx), 8, 8, {77});
        pdf_obj* gi = addPixmapImage(ctx, doc, g);
        fz_drop_pixmap(ctx, g);
        putImage(ctx, doc, res, "Im1", gi);
        pdf_drop_obj(ctx, gi);
        fz_pixmap* c = flatPixmap(ctx, fz_device_rgb(ctx), 8, 8, {10, 20, 30});
        pdf_obj* ci = addPixmapImage(ctx, doc, c);
        fz_drop_pixmap(ctx, c);
        putImage(ctx, doc, res, "Im2", ci);
        pdf_drop_obj(ctx, ci);
    };
    const std::string in = path("xgr.pdf");
    buildPdf(mc_, in, {s});
    auto doc = Document::open(mc_, in);
    auto page = doc->page(0);
    ImageManager mgr(mc_);
    const auto infos = mgr.enumerateImages(page.get(), doc.get());
    ASSERT_EQ(infos.size(), 2u);
    for (int i = 0; i < 2; ++i) {
        const QImage ex = mgr.extractImage(page.get(), doc.get(), i);
        ASSERT_FALSE(ex.isNull());
        if (infos[i].xobjectName == "Im1") {
            EXPECT_EQ(ex.format(), QImage::Format_Grayscale8);
            EXPECT_EQ(qGray(ex.pixel(2, 2)), 77);
        } else {
            EXPECT_EQ(ex.format(), QImage::Format_RGB888);
            EXPECT_EQ(ex.pixelColor(2, 2), QColor(10, 20, 30));
        }
    }
}

// ============================================================ karen remediation (phase 4)

namespace {

// A JPEG whose APP1 Exif segment declares a length 12 bytes SHORTER than its
// payload: the last 12 payload bytes (the end of the secret, no 0xFF in them)
// sit where the next marker must be. libjpeg skips them as "extraneous bytes
// before marker" and decodes; a segment parser cannot trust the stream.
Bytes withBadExifLength(const Bytes& jpeg) {
    Bytes payload("Exif\0\0II*\0\x08\0\0\0\0\0\0\0\0\0", 20);
    payload += kExifSecret;
    const size_t declared = payload.size() + 2 - 12;
    Bytes seg;
    seg.push_back(static_cast<char>(0xFF));
    seg.push_back(static_cast<char>(0xE1));
    seg.push_back(static_cast<char>((declared >> 8) & 0xFF));
    seg.push_back(static_cast<char>(declared & 0xFF));
    seg += payload;
    size_t at = 2;
    while (at + 4 <= jpeg.size() && static_cast<unsigned char>(jpeg[at]) == 0xFF) {
        const unsigned char m = static_cast<unsigned char>(jpeg[at + 1]);
        if (m != 0xE0 && m != 0xEE) break;
        at += 2 + ((static_cast<unsigned char>(jpeg[at + 2]) << 8) | static_cast<unsigned char>(jpeg[at + 3]));
    }
    return jpeg.substr(0, at) + seg + jpeg.substr(at);
}

PageFill oneEncodedImage(const Bytes& data, bool softMask = false) {
    return [data, softMask](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addEncodedImage(ctx, doc, data);
        if (softMask) pdf_dict_put_drop(ctx, img, PDF_NAME(SMask), addHalfSoftMask(ctx, doc));
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
}

// A DCT image XObject built by hand from `data` (no parsing by MuPDF), with
// the given /Width (0 makes it undecodable).
pdf_obj* addRawDctImage(fz_context* ctx, pdf_document* doc, const Bytes& data, int width) {
    fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    pdf_obj* d = pdf_new_dict(ctx, doc, 8);
    pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject));
    pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Image));
    pdf_dict_put_int(ctx, d, PDF_NAME(Width), width);
    pdf_dict_put_int(ctx, d, PDF_NAME(Height), 32);
    pdf_dict_put_int(ctx, d, PDF_NAME(BitsPerComponent), 8);
    pdf_dict_put(ctx, d, PDF_NAME(ColorSpace), PDF_NAME(DeviceRGB));
    pdf_dict_put(ctx, d, PDF_NAME(Filter), PDF_NAME(DCTDecode));
    pdf_obj* img = pdf_add_stream(ctx, doc, b, d, 1);
    fz_drop_buffer(ctx, b);
    pdf_drop_obj(ctx, d);
    return img;
}

bool holdsAnySecret(const Bytes& b) {
    for (const char* s : {kExifSecret, kXmpSecret, kCommentSecret, kIptcSecret, kTrailingSecret, "PDFE-MPF-TRAILER-SECRET"})
        if (b.find(s) != Bytes::npos) return true;
    return false;
}

} // namespace

// (a) Anything after EOI (MPF images, thumbnails, appended EXIF) is dropped.
TEST_F(ExifStripModel, TrailerAfterEoiIsRemoved) {
    const Bytes plain = qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0));
    ASSERT_EQ(static_cast<unsigned char>(plain[plain.size() - 1]), 0xD9) << "oracle: Qt JPEG ends with EOI";
    const Bytes trailer = segment(0xE1, Bytes("Exif\0\0", 6) + kExifSecret) + Bytes("MPF\0", 4) + "PDFE-MPF-TRAILER-SECRET";
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = oneEncodedImage(plain + trailer);
    const std::string in = path("trailer_in.pdf"), out = path("trailer_out.pdf");
    buildPdf(mc_, in, {s});
    const ImageState b = readImage(mc_, in);
    ASSERT_EQ(b.filter, "DCTDecode");
    ASSERT_NE(b.raw.find("PDFE-MPF-TRAILER-SECRET"), Bytes::npos) << "oracle: trailer stored";
    ASSERT_NE(b.raw.find(kExifSecret), Bytes::npos);
    const QImage before = renderOf(mc_, in);
    const std::string control = out + ".control.pdf";
    EXPECT_EQ(stripAndControl(in, out, control), 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.filter, "DCTDecode");
    EXPECT_EQ(a.raw, plain) << "the JPEG must end at its EOI";
    EXPECT_FALSE(holdsAnySecret(readFile(out))) << "a trailer secret is still in the saved file";
    EXPECT_EQ(renderOf(mc_, out), before);
}

// (b) A bad segment length: the data is decoded and stored as fresh Flate
// pixels with no metadata; the soft mask stays; the picture is unchanged.
TEST_F(ExifStripModel, BadSegmentLengthIsReencodedWithoutMetadata) {
    const Bytes jpeg = withBadExifLength(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = oneEncodedImage(jpeg, /*softMask=*/true);
    const std::string in = path("badlen_in.pdf"), out = path("badlen_out.pdf");
    buildPdf(mc_, in, {s});
    const ImageState b = readImage(mc_, in);
    ASSERT_EQ(b.filter, "DCTDecode");
    ASSERT_NE(b.raw.find(kExifSecret), Bytes::npos) << "oracle: the secret is stored";
    const QImage before = renderOf(mc_, in);
    ASSERT_GT(qBlue(before.pixel(60, 100)) + qRed(before.pixel(140, 100)), 0) << "oracle: MuPDF draws it";
    ASSERT_LT(qAlpha(before.pixel(60, 100)), 30) << "oracle: soft mask in effect";
    ExifStripper::StripReport report;
    {
        auto doc = Document::open(mc_, in);
        auto page = doc->page(0);
        ExifStripper st(mc_);
        report = st.stripPageImagesReport(page.get(), doc.get());
        page.reset();
        saveCopy(doc.get(), out);
    }
    EXPECT_EQ(report.stripped, 1);
    EXPECT_EQ(report.failed, 0);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.filter, "FlateDecode") << "re-encoded as fresh pixels";
    EXPECT_EQ(a.colorSpace, "DeviceRGB");
    EXPECT_TRUE(a.hasSMask) << "the soft mask was dropped";
    EXPECT_FALSE(holdsAnySecret(readFile(out))) << "EXIF bytes survived the fallback";
    EXPECT_EQ(renderOf(mc_, out), before) << "the fallback changed the picture";
}

// (b) The same for an Adobe CMYK JPEG: it stays CMYK and keeps its colours.
TEST_F(ExifStripModel, BadSegmentLengthCmykStaysCmyk) {
    const Bytes jpeg = withBadExifLength(adobeCmykJpeg(mc_));
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = oneEncodedImage(jpeg);
    const std::string in = path("badcmyk_in.pdf"), out = path("badcmyk_out.pdf");
    buildPdf(mc_, in, {s});
    const QImage before = renderOf(mc_, in);
    const QRgb want = before.pixel(100, 100);
    ASSERT_GT(qBlue(want), qRed(want) + 80) << "oracle: renders blue";
    const std::string control = out + ".control.pdf";
    EXPECT_EQ(stripAndControl(in, out, control), 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.filter, "FlateDecode");
    EXPECT_EQ(a.colorSpace, "DeviceCMYK") << "CMYK must not be turned into RGB";
    EXPECT_FALSE(holdsAnySecret(readFile(out)));
    const QRgb got = renderOf(mc_, out).pixel(100, 100);
    EXPECT_NEAR(qRed(got), qRed(want), 2);
    EXPECT_NEAR(qGreen(got), qGreen(want), 2);
    EXPECT_NEAR(qBlue(got), qBlue(want), 2);
}

// (b) Cut off inside the scan: every segment before the cut was seen, so the
// metadata goes and the stream is kept as DCT.
TEST_F(ExifStripModel, JpegCutOffInTheScanLosesItsMetadata) {
    const Bytes full = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    const size_t sos = full.find(Bytes("\xFF\xDA", 2));
    ASSERT_NE(sos, Bytes::npos);
    const Bytes cut = full.substr(0, sos + (full.size() - sos) / 2);
    ASSERT_EQ(cut.find(kTrailingSecret), Bytes::npos) << "oracle: the cut drops the trailing comment";
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addRawDctImage(ctx, doc, cut, 32);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("cutscan_in.pdf"), out = path("cutscan_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NE(readImage(mc_, in).raw.find(kExifSecret), Bytes::npos);
    const std::string control = out + ".control.pdf";
    EXPECT_EQ(stripAndControl(in, out, control), 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.filter, "DCTDecode");
    EXPECT_FALSE(holdsAnySecret(readFile(out)));
}

// (b) Cut off before the scan: not a JPEG that parses, so it is decoded. If
// MuPDF can decode it the result holds no metadata; if not, it is reported as
// NOT stripped, not counted, and stripImage says false. Either way it is never
// reported stripped while a secret remains.
TEST_F(ExifStripModel, JpegCutOffBeforeTheScanIsNeverReportedCleanWithMetadata) {
    const Bytes full = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    const size_t sof = full.find(Bytes("\xFF\xC0", 2));
    ASSERT_NE(sof, Bytes::npos);
    const Bytes cut = full.substr(0, sof + 6);   // inside the SOF segment
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addRawDctImage(ctx, doc, cut, 32);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("cuthdr_in.pdf"), out = path("cuthdr_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NE(readImage(mc_, in).raw.find(kExifSecret), Bytes::npos);
    ExifStripper::StripReport report;
    bool single = true;
    {
        auto doc = Document::open(mc_, in);
        auto page = doc->page(0);
        ExifStripper st(mc_);
        report = st.stripPageImagesReport(page.get(), doc.get());
        single = st.stripImage(page.get(), doc.get(), "Im1");
        page.reset();
        saveCopy(doc.get(), out);
    }
    EXPECT_EQ(report.total(), 1);
    EXPECT_EQ(report.alreadyClean, 0) << "a cut-off JPEG with metadata was reported clean";
    RecordProperty("outcome", report.failed == 1 ? "reported-not-stripped" : "reencoded");
    std::fprintf(stderr, "cut-before-scan outcome: %s\n", report.failed == 1 ? "reported-not-stripped" : "reencoded");
    if (report.failed == 1) {
        EXPECT_EQ(report.metadataFree(), 0);
        EXPECT_FALSE(single);
    } else {
        EXPECT_EQ(report.stripped, 1);
        EXPECT_FALSE(holdsAnySecret(readFile(out))) << "reported stripped, but a secret remains";
    }
}

// (b)+(c) An image that cannot be decoded at all (malformed data, /Width 0) is
// reported as not stripped, is not counted, and stripImage says false.
TEST_F(ExifStripModel, UndecodableMalformedImageIsReportedNotStripped) {
    const Bytes jpeg = withBadExifLength(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    PageSpec s;
    s.content = "q Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addRawDctImage(ctx, doc, jpeg, 0);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("undecodable.pdf");
    buildPdf(mc_, in, {s});
    auto doc = Document::open(mc_, in);
    auto page = doc->page(0);
    ExifStripper st(mc_);
    EXPECT_EQ(st.stripImageResult(page.get(), doc.get(), "Im1"), ExifStripper::ImageResult::Failed);
    EXPECT_FALSE(st.stripImage(page.get(), doc.get(), "Im1"));
    EXPECT_EQ(st.stripPageImages(page.get(), doc.get()), 0) << "an image that kept its metadata was counted";
    const ExifStripper::StripReport r = st.stripPageImagesReport(page.get(), doc.get());
    EXPECT_EQ(r.failed, 1);
    EXPECT_EQ(r.stripped, 0);
    EXPECT_EQ(r.alreadyClean, 0);
}

// (c) The counts per outcome, and the message the UI shows for them.
TEST_F(ExifStripModel, ReportCountsEachOutcomeAndSaysSo) {
    const Bytes exifJpeg = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    const Bytes broken = withBadExifLength(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    PageSpec s;
    s.content = "q Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* a = addEncodedImage(ctx, doc, exifJpeg);
        putImage(ctx, doc, res, "Im1", a);
        pdf_drop_obj(ctx, a);
        fz_pixmap* pix = flatPixmap(ctx, fz_device_rgb(ctx), 8, 8, {1, 2, 3});
        pdf_obj* b = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        putImage(ctx, doc, res, "Im2", b);
        pdf_drop_obj(ctx, b);
        pdf_obj* c = addRawDctImage(ctx, doc, broken, 0);
        putImage(ctx, doc, res, "Im3", c);
        pdf_drop_obj(ctx, c);
    };
    const std::string in = path("outcomes.pdf");
    buildPdf(mc_, in, {s, PageSpec{}});
    auto doc = Document::open(mc_, in);
    ExifStripper st(mc_);
    const ExifStripper::StripReport r = st.stripAllImagesReport(doc.get());
    EXPECT_EQ(r.stripped, 1);
    EXPECT_EQ(r.alreadyClean, 1);
    EXPECT_EQ(r.failed, 1);
    EXPECT_EQ(ExifStripper::describe(r).toStdString(),
              "Metadata removed from 1 image(s). 1 image(s) had no metadata to remove. "
              "1 image(s) could not be processed; their metadata may remain.");
    // A second pass: the stripped image is now clean, the broken one still fails.
    const ExifStripper::StripReport again = st.stripAllImagesReport(doc.get());
    EXPECT_EQ(again.stripped, 0);
    EXPECT_EQ(again.alreadyClean, 2);
    EXPECT_EQ(again.failed, 1);
    EXPECT_EQ(st.stripAllImages(doc.get()), 2) << "stripAllImages counts metadata-free images only";

    ExifStripper::StripReport none;
    EXPECT_EQ(ExifStripper::describe(none).toStdString(), "No images found to strip metadata from.");
    ExifStripper::StripReport clean;
    clean.alreadyClean = 2;
    EXPECT_EQ(ExifStripper::describe(clean).toStdString(),
              "Metadata removed from 0 image(s). 2 image(s) had no metadata to remove.");
}

// (d) Rotation defaults per mode: text 45 degrees, image upright (0), until
// the user sets one; the user's Rotation and Position reach config() and the
// engine.
TEST(WatermarkDialogImageMode, ImageModeDefaultsToUprightTextTo45) {
    ensureQApplication();
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::ui::WatermarkDialog dlg(ctx, nullptr, nullptr);
    auto* mode = dlg.findChild<QComboBox*>("watermarkDialogModeCombo");
    ASSERT_NE(mode, nullptr);
    EXPECT_FLOAT_EQ(dlg.config().rotation, 45.0f) << "text default";
    mode->setCurrentIndex(mode->findData(QStringLiteral("image")));
    EXPECT_FLOAT_EQ(dlg.config().rotation, 0.0f) << "an image watermark defaults to upright";
    EXPECT_EQ(dlg.extConfig().mode, "image");
    mode->setCurrentIndex(mode->findData(QStringLiteral("text")));
    EXPECT_FLOAT_EQ(dlg.config().rotation, 45.0f) << "back to the text default";
}

TEST_F(WatermarkModel, DialogConfigReachesTheEngine) {
    const std::string in = path("dlg_in.pdf");
    buildPdf(mc_, in, {PageSpec{}});
    const std::string png = solidPng("wm.png", 64, 16, QColor(0, 0, 255));
    auto apply = [&](pdfe::ui::WatermarkDialog& dlg, const std::string& out) {
        WatermarkConfig cfg = dlg.config();
        cfg.opacity = 1.0f;
        BatchEngine(mc_).applyWatermarkExt(in, out, cfg, dlg.extConfig());
        return boxOf(renderOf(mc_, out), isBlue);
    };
    {
        // Image mode, defaults: upright and centred.
        pdfe::ui::WatermarkDialog dlg(mc_, nullptr, nullptr);
        auto* mode = dlg.findChild<QComboBox*>("watermarkDialogModeCombo");
        auto* image = dlg.findChild<QLineEdit*>("watermarkDialogImagePath");
        ASSERT_TRUE(mode && image);
        mode->setCurrentIndex(mode->findData(QStringLiteral("image")));
        image->setText(QString::fromStdString(png));
        const QRect b = apply(dlg, path("dlg_default.pdf"));
        ASSERT_FALSE(b.isNull());
        EXPECT_NEAR(b.height(), 20, 3) << "default image watermark is not upright";
        EXPECT_NEAR(QRectF(b).center().x(), 100.0, 3.0);
    }
    {
        // The user sets Rotation 45 and Position Top Left, then switches modes
        // and back: the user's rotation is kept and both reach the engine.
        pdfe::ui::WatermarkDialog dlg(mc_, nullptr, nullptr);
        auto* mode = dlg.findChild<QComboBox*>("watermarkDialogModeCombo");
        auto* image = dlg.findChild<QLineEdit*>("watermarkDialogImagePath");
        auto* rotation = dlg.findChild<QDoubleSpinBox*>("watermarkDialogRotationSpin");
        auto* position = dlg.findChild<QComboBox*>("watermarkDialogPositionCombo");
        ASSERT_TRUE(mode && image && rotation && position);
        mode->setCurrentIndex(mode->findData(QStringLiteral("image")));
        rotation->setValue(45.0);
        position->setCurrentIndex(position->findData(QStringLiteral("top-left")));
        mode->setCurrentIndex(mode->findData(QStringLiteral("text")));
        mode->setCurrentIndex(mode->findData(QStringLiteral("image")));
        EXPECT_FLOAT_EQ(dlg.config().rotation, 45.0f) << "a user-set rotation was reset by a mode switch";
        EXPECT_EQ(dlg.config().position, "top-left");
        image->setText(QString::fromStdString(png));
        const QRect b = apply(dlg, path("dlg_user.pdf"));
        ASSERT_FALSE(b.isNull());
        EXPECT_GT(b.height(), 50) << "user rotation did not reach the engine";
        EXPECT_LT(QRectF(b).center().x(), 80.0) << "user position did not reach the engine";
        EXPECT_LT(QRectF(b).center().y(), 80.0);
    }
}

// ============================================================ security-audit remediation (phase 4)
//
//   1. Kept JPEG segments cut to their standard size
//     ExifStripModel.JfifThumbnailAndTailAreCutAndJfxxDropped
//     ExifStripModel.AdobeSegmentPaddingIsCut
//     ExifStripModel.IccChunkWithBadHeaderIsDroppedValidOneKept
//   2. JPEG 2000 box whitelist
//     ExifStripModel.Jp2PrivateBoxesAreRemovedAtTopLevelAndInHeader
//     ExifStripModel.MalformedJp2IsNotReplacedWhileJpxDecodingIsOff
//   3. Fallback size cap
//     ExifStripModel.HugeDictionaryDimensionsAreNotDecoded
//     ExifStripModel.HugeJpegFrameIsNotDecoded
//   4. Images the strip visits
//     ExifStripModel.FormXObjectImagesAreStripped
//     ExifStripModel.AlternateImagesAreStripped
//     ExifStripModel.MaskOfAMaskIsStripped
//     ExifStripModel.PageThumbnailIsRemoved
//     ExifStripModel.InlineJpegImagesAreReportedNotProcessed
//   5. A JPEG filter that is not the last filter
//     ExifStripModel.BuriedDctFilterIsNeverReportedClean

namespace {

// Sets the process-wide JPX decode gate for one test, and restores it.
struct ScopedJpxDecoding {
    explicit ScopedJpxDecoding(bool on) : saved(pdfe::core::isJpxDecodingEnabled()) {
        pdfe::core::setJpxDecodingEnabled(on);
    }
    ~ScopedJpxDecoding() { pdfe::core::setJpxDecodingEnabled(saved); }
    bool saved;
};

size_t segLen(const Bytes& b, size_t at) {
    return (static_cast<unsigned char>(b[at + 2]) << 8) | static_cast<unsigned char>(b[at + 3]);
}

// Offset of the first segment with marker m (walking segments from SOI), or npos.
size_t findSegment(const Bytes& jpeg, unsigned char m) {
    size_t at = 2;
    while (at + 4 <= jpeg.size() && static_cast<unsigned char>(jpeg[at]) == 0xFF) {
        const unsigned char cur = static_cast<unsigned char>(jpeg[at + 1]);
        if (cur == m) return at;
        if (cur == 0xDA) break;
        at += 2 + segLen(jpeg, at);
    }
    return Bytes::npos;
}

void putBE32(Bytes& b, size_t at, uint32_t v) {
    b[at] = static_cast<char>(v >> 24);
    b[at + 1] = static_cast<char>(v >> 16);
    b[at + 2] = static_cast<char>(v >> 8);
    b[at + 3] = static_cast<char>(v);
}

Bytes jp2Box(const char* type, const Bytes& body) {
    Bytes b(4, '\0');
    putBE32(b, 0, static_cast<uint32_t>(body.size() + 8));
    return b + type + body;
}

ExifStripper::StripReport stripPageAndSave(const std::shared_ptr<MuPdfContext>& mc, const std::string& in,
                                           const std::string& out) {
    ExifStripper::StripReport r;
    auto doc = Document::open(mc, in);
    EXPECT_NE(doc, nullptr);
    if (!doc) return r;
    auto page = doc->page(0);
    ExifStripper st(mc);
    r = st.stripPageImagesReport(page.get(), doc.get());
    page.reset();
    saveCopy(doc.get(), out);
    return r;
}

// A Form XObject with the given content and resources (borrowed); NEW ref.
pdf_obj* addForm(fz_context* ctx, pdf_document* doc, const std::string& content, pdf_obj* res) {
    fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(content.data()),
                                                  content.size());
    pdf_obj* d = pdf_new_dict(ctx, doc, 5);
    pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject));
    pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Form));
    pdf_dict_put_rect(ctx, d, PDF_NAME(BBox), fz_make_rect(0, 0, 200, 200));
    pdf_dict_put(ctx, d, PDF_NAME(Resources), res);
    pdf_obj* f = pdf_add_stream(ctx, doc, b, d, 0);
    pdf_drop_obj(ctx, d);
    fz_drop_buffer(ctx, b);
    return f;
}

// A gray 8x8 Flate image whose /Metadata holds `secret`; NEW ref.
pdf_obj* addFlateImageWithXmp(fz_context* ctx, pdf_document* doc, int gray, const std::string& secret) {
    fz_pixmap* pix = flatPixmap(ctx, fz_device_gray(ctx), 8, 8, {gray});
    pdf_obj* img = addPixmapImage(ctx, doc, pix);
    fz_drop_pixmap(ctx, pix);
    pdf_dict_put_drop(ctx, img, PDF_NAME(Metadata), addXmpStream(ctx, doc, secret));
    return img;
}

} // namespace

// 1. A JFIF APP0 carrying a 4x2 RGB thumbnail (whose bytes spell a secret)
// and bytes appended after it, plus a JFXX extension APP0: afterwards the
// APP0 is the bare 14-byte JFIF header with a 0x0 thumbnail, the JFXX segment
// is gone, everything else is byte-identical and the render is unchanged.
TEST_F(ExifStripModel, JfifThumbnailAndTailAreCutAndJfxxDropped) {
    const Bytes plain = qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0));
    const size_t app0 = findSegment(plain, 0xE0);
    ASSERT_EQ(app0, 2u) << "oracle: Qt writes JFIF APP0 first";
    ASSERT_EQ(segLen(plain, app0), 16u) << "oracle: a bare JFIF header";
    const Bytes header = plain.substr(app0 + 4, 14);
    ASSERT_EQ(header.substr(0, 5), Bytes("JFIF\0", 5));
    const Bytes thumb = "PDFE-JFIF-THUMB-SECRET!!";    // 4 x 2 x 3 bytes
    ASSERT_EQ(thumb.size(), 24u);
    Bytes jfif = header;
    jfif[12] = 4;
    jfif[13] = 2;
    jfif += thumb + "PDFE-JFIF-TAIL-SECRET";
    const Bytes jfxx = Bytes("JFXX\0\x10", 6) + "PDFE-JFXX-SECRET";
    const Bytes rest = plain.substr(app0 + 2 + 16);
    const Bytes jpeg = plain.substr(0, 2) + segment(0xE0, jfif) + segment(0xE0, jfxx) + rest;
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = oneEncodedImage(jpeg);
    const std::string in = path("jfif_in.pdf"), out = path("jfif_out.pdf");
    buildPdf(mc_, in, {s});
    const ImageState b = readImage(mc_, in);
    ASSERT_EQ(b.filter, "DCTDecode");
    ASSERT_NE(b.raw.find("PDFE-JFIF-THUMB-SECRET"), Bytes::npos) << "oracle: stored with its thumbnail";
    const QImage before = renderOf(mc_, in);
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 1) << "a cut JFIF segment must count as stripped";
    const ImageState a = readImage(mc_, out);
    const Bytes file = readFile(out);
    for (const char* secret : {"PDFE-JFIF-THUMB-SECRET", "PDFE-JFIF-TAIL-SECRET", "PDFE-JFXX-SECRET"})
        EXPECT_EQ(file.find(secret), Bytes::npos) << secret << " is still in the saved file";
    ASSERT_GE(a.raw.size(), 20u);
    EXPECT_EQ(segLen(a.raw, 2), 16u) << "the JFIF length was not rewritten to 16";
    EXPECT_EQ(static_cast<unsigned char>(a.raw[2 + 4 + 12]), 0) << "thumbnail width not 0";
    EXPECT_EQ(static_cast<unsigned char>(a.raw[2 + 4 + 13]), 0) << "thumbnail height not 0";
    EXPECT_EQ(a.raw, plain) << "only the thumbnail, the tail and the JFXX segment should have gone";
    EXPECT_EQ(renderOf(mc_, out), before);
}

// 1. An Adobe APP14 segment padded with a secret: cut to its 12-byte payload,
// the CMYK colours unchanged.
TEST_F(ExifStripModel, AdobeSegmentPaddingIsCut) {
    const Bytes plain = adobeCmykJpeg(mc_);
    const size_t app14 = findSegment(plain, 0xEE);
    ASSERT_NE(app14, Bytes::npos) << "oracle: an Adobe APP14 segment";
    ASSERT_EQ(segLen(plain, app14), 14u) << "oracle: the standard Adobe payload";
    const Bytes payload = plain.substr(app14 + 4, 12) + "PDFE-ADOBE-PAD-SECRET";
    const Bytes jpeg = plain.substr(0, app14) + segment(0xEE, payload) + plain.substr(app14 + 2 + 14);
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = oneEncodedImage(jpeg);
    const std::string in = path("adobe_in.pdf"), out = path("adobe_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NE(readImage(mc_, in).raw.find("PDFE-ADOBE-PAD-SECRET"), Bytes::npos);
    const QImage before = renderOf(mc_, in);
    const QRgb want = before.pixel(100, 100);
    ASSERT_GT(qBlue(want), qRed(want) + 80) << "oracle: the CMYK JPEG renders blue";
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(readFile(out).find("PDFE-ADOBE-PAD-SECRET"), Bytes::npos) << "the padding survived";
    EXPECT_EQ(a.raw, plain) << "the Adobe segment must be its 12-byte payload, the rest unchanged";
    EXPECT_EQ(renderOf(mc_, out), before) << "CMYK colours changed";
}

// 1. APP2 ICC_PROFILE: a chunk whose header is invalid (sequence 0, count 0)
// is dropped; a valid one is kept whole, its body included (a known limit).
TEST_F(ExifStripModel, IccChunkWithBadHeaderIsDroppedValidOneKept) {
    const Bytes plain = qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0));
    const size_t after = 2 + 2 + segLen(plain, 2);          // after the JFIF APP0
    const Bytes bad = segment(0xE2, Bytes("ICC_PROFILE\0\0\0", 14) + "PDFE-ICC-BAD-HEADER-SECRET");
    const Bytes good = segment(0xE2, Bytes("ICC_PROFILE\0\x01\x01", 14) + "PDFE-ICC-BODY-KEPT");
    const Bytes jpeg = plain.substr(0, after) + bad + good + plain.substr(after);
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = oneEncodedImage(jpeg);
    const std::string in = path("icc_in.pdf"), out = path("icc_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NE(readImage(mc_, in).raw.find("PDFE-ICC-BAD-HEADER-SECRET"), Bytes::npos);
    const QImage before = renderOf(mc_, in);
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.raw.find("PDFE-ICC-BAD-HEADER-SECRET"), Bytes::npos) << "an invalid ICC chunk was kept";
    EXPECT_EQ(a.raw, plain.substr(0, after) + good + plain.substr(after)) << "the valid ICC chunk must stay whole";
    EXPECT_EQ(renderOf(mc_, out), before);
}

// 2. A private box at top level and one inside jp2h: both go (whitelist), the
// file is the original JP2 again, and it still decodes to the same picture.
TEST_F(ExifStripModel, Jp2PrivateBoxesAreRemovedAtTopLevelAndInHeader) {
    const Bytes jp2 = readFile(std::string(TEST_FIXTURES_DIR) + "/jpx_legit_a4_300dpi.jp2");
    ASSERT_GT(jp2.size(), 100u) << "oracle: fixture present";
    const size_t boxStart = jp2.find("jp2h") - 4;
    const size_t boxLen = (static_cast<unsigned char>(jp2[boxStart]) << 24) | (static_cast<unsigned char>(jp2[boxStart + 1]) << 16) |
                          (static_cast<unsigned char>(jp2[boxStart + 2]) << 8) | static_cast<unsigned char>(jp2[boxStart + 3]);
    const Bytes nested = jp2Box("priv", "PDFE-JP2-NESTED-SECRET");
    const Bytes top = jp2Box("priv", "PDFE-JP2-TOP-SECRET") + jp2Box("free", "PDFE-JP2-FREE-SECRET");
    Bytes withPriv = jp2.substr(0, boxStart + boxLen) + nested + top + jp2.substr(boxStart + boxLen);
    putBE32(withPriv, boxStart, static_cast<uint32_t>(boxLen + nested.size()));
    ScopedJpxDecoding jpxOn(true);
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = oneEncodedImage(withPriv);
    const std::string in = path("jp2priv_in.pdf"), out = path("jp2priv_out.pdf");
    buildPdf(mc_, in, {s});
    const ImageState b = readImage(mc_, in);
    ASSERT_EQ(b.filter, "JPXDecode");
    ASSERT_NE(b.raw.find("PDFE-JP2-NESTED-SECRET"), Bytes::npos);
    const QImage before = renderOf(mc_, in);
    {
        ScopedJpxDecoding jpxOff(false);
        ASSERT_NE(renderOf(mc_, in), before) << "oracle: the JP2 really decodes (not the placeholder)";
    }
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.filter, "JPXDecode");
    const Bytes file = readFile(out);
    for (const char* secret : {"PDFE-JP2-NESTED-SECRET", "PDFE-JP2-TOP-SECRET", "PDFE-JP2-FREE-SECRET"})
        EXPECT_EQ(file.find(secret), Bytes::npos) << secret << " is still in the saved file";
    EXPECT_EQ(a.raw, jp2) << "only the private and free boxes should have gone";
    EXPECT_EQ(renderOf(mc_, out), before);
}

// 2/3. A JP2 with a broken box structure goes to the fallback; while JPX
// decoding is off MuPDF would hand back a 1x1 placeholder, which must never
// replace the image: it is reported as not processed and left as it is.
TEST_F(ExifStripModel, MalformedJp2IsNotReplacedWhileJpxDecodingIsOff) {
    const Bytes jp2 = readFile(std::string(TEST_FIXTURES_DIR) + "/jpx_legit_a4_300dpi.jp2");
    Bytes broken = jp2 + jp2Box("priv", "PDFE-JP2-BROKEN-SECRET");
    putBE32(broken, jp2.size(), 4096);                      // runs past the end
    ScopedJpxDecoding jpxOff(false);
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = oneEncodedImage(broken);
    const std::string in = path("jp2bad_in.pdf"), out = path("jp2bad_out.pdf");
    buildPdf(mc_, in, {s});
    const ImageState b = readImage(mc_, in);
    ASSERT_EQ(b.filter, "JPXDecode");
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.failed, 1) << "a JP2 that cannot be decoded must be reported as not processed";
    EXPECT_EQ(r.stripped, 0);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.filter, "JPXDecode") << "the image was replaced";
    EXPECT_EQ(a.raw, b.raw);
}

// 3. Malformed JPEG data whose dictionary claims 12000 x 12000 RGB (144
// megapixels, over the 100-megapixel cap): reported as not processed without
// decoding (it would allocate about a gigabyte), and fast.
TEST_F(ExifStripModel, HugeDictionaryDimensionsAreNotDecoded) {
    const Bytes jpeg = withBadExifLength(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    PageSpec s;
    s.content = "q Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addRawDctImage(ctx, doc, jpeg, 12000);
        pdf_dict_put_int(ctx, img, PDF_NAME(Height), 12000);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("bomb.pdf");
    buildPdf(mc_, in, {s});
    auto doc = Document::open(mc_, in);
    auto page = doc->page(0);
    ExifStripper st(mc_);
    const auto t0 = std::chrono::steady_clock::now();
    const ExifStripper::ImageResult res = st.stripImageResult(page.get(), doc.get(), "Im1");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(res, ExifStripper::ImageResult::Failed) << "a 144-megapixel image was decoded";
    EXPECT_LT(ms, 1000) << "the over-size image took " << ms << " ms: it was decoded";
    EXPECT_NE(ExifStripper::describe(st.stripPageImagesReport(page.get(), doc.get())).indexOf("could not be processed"), -1);
}

// 3. The JPEG decoder works from the frame header, not the dictionary: a
// malformed JPEG whose SOF claims 12000 x 12000 (dictionary 32 x 32) is not
// decoded either.
TEST_F(ExifStripModel, HugeJpegFrameIsNotDecoded) {
    Bytes jpeg = withBadExifLength(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    const size_t sof = jpeg.find(Bytes("\xFF\xC0", 2));
    ASSERT_NE(sof, Bytes::npos);
    jpeg[sof + 5] = static_cast<char>(12000 >> 8);
    jpeg[sof + 6] = static_cast<char>(12000 & 0xFF);
    jpeg[sof + 7] = static_cast<char>(12000 >> 8);
    jpeg[sof + 8] = static_cast<char>(12000 & 0xFF);
    PageSpec s;
    s.content = "q Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addRawDctImage(ctx, doc, jpeg, 32);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("sofbomb.pdf");
    buildPdf(mc_, in, {s});
    auto doc = Document::open(mc_, in);
    auto page = doc->page(0);
    ExifStripper st(mc_);
    EXPECT_EQ(st.stripImageResult(page.get(), doc.get(), "Im1"), ExifStripper::ImageResult::Failed);
}

// 4. Images inside Form XObjects: Fm1 draws a JPEG with EXIF and the form
// Fm2, which draws a Flate image with XMP. Fm2 is also listed on the page
// (shared) and lists Fm1 in its own resources (a cycle). Both images are
// stripped once each; the picture is unchanged.
TEST_F(ExifStripModel, FormXObjectImagesAreStripped) {
    const Bytes jpeg = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    PageSpec s;
    s.content = "/Fm1 Do";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* im1 = addEncodedImage(ctx, doc, jpeg);
        pdf_obj* im2 = addFlateImageWithXmp(ctx, doc, 90, "PDFE-FORM2-XMP-SECRET");
        pdf_obj* res2 = pdf_new_dict(ctx, doc, 1);
        putImage(ctx, doc, res2, "Im2", im2);
        pdf_obj* fm2 = addForm(ctx, doc, "q 80 0 0 80 110 110 cm /Im2 Do Q", res2);
        pdf_obj* res1 = pdf_new_dict(ctx, doc, 1);
        putImage(ctx, doc, res1, "Im1", im1);
        putImage(ctx, doc, res1, "Fm2", fm2);
        pdf_obj* fm1 = addForm(ctx, doc, "q 80 0 0 80 10 10 cm /Im1 Do Q /Fm2 Do", res1);
        putImage(ctx, doc, res2, "Fm1", fm1);                // cycle, in resources only
        putImage(ctx, doc, res, "Fm1", fm1);
        putImage(ctx, doc, res, "Fm2", fm2);                 // shared
        for (pdf_obj* o : {im1, im2, res2, fm2, res1, fm1}) pdf_drop_obj(ctx, o);
    };
    const std::string in = path("form_in.pdf"), out = path("form_out.pdf");
    buildPdf(mc_, in, {s});
    const Bytes fileIn = readFile(in);
    ASSERT_NE(fileIn.find("PDFE-FORM2-XMP-SECRET"), Bytes::npos);
    ASSERT_NE(fileIn.find(kExifSecret), Bytes::npos);
    const QImage before = renderOf(mc_, in);
    ASSERT_GT(qBlue(before.pixel(20, 160)), 200) << "oracle: the form draws Im1";
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 2) << "the images inside the forms were not stripped (once each)";
    EXPECT_EQ(r.alreadyClean, 0);
    EXPECT_EQ(r.failed, 0);
    const Bytes file = readFile(out);
    EXPECT_EQ(file.find("PDFE-FORM2-XMP-SECRET"), Bytes::npos);
    EXPECT_FALSE(holdsAnySecret(file)) << "EXIF of the image inside the form survived";
    EXPECT_EQ(renderOf(mc_, out), before);
}

// 4. An image's /Alternates: the alternate JPEG's EXIF and the alternate's
// /Metadata are removed.
TEST_F(ExifStripModel, AlternateImagesAreStripped) {
    const Bytes jpeg = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        fz_pixmap* pix = flatPixmap(ctx, fz_device_rgb(ctx), 32, 32, {0, 0, 255});
        pdf_obj* img = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        pdf_obj* alt = addEncodedImage(ctx, doc, jpeg);
        pdf_dict_put_drop(ctx, alt, PDF_NAME(Metadata), addXmpStream(ctx, doc, "PDFE-ALT-XMP-SECRET"));
        pdf_obj* entry = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put(ctx, entry, PDF_NAME(Image), alt);
        pdf_dict_puts_drop(ctx, entry, "DefaultForPrinting", PDF_TRUE);
        pdf_obj* arr = pdf_new_array(ctx, doc, 1);
        pdf_array_push_drop(ctx, arr, entry);
        pdf_dict_puts_drop(ctx, img, "Alternates", arr);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, alt);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("alt_in.pdf"), out = path("alt_out.pdf");
    buildPdf(mc_, in, {s});
    const Bytes fileIn = readFile(in);
    ASSERT_NE(fileIn.find("PDFE-ALT-XMP-SECRET"), Bytes::npos);
    ASSERT_NE(fileIn.find(kExifSecret), Bytes::npos) << "oracle: the alternate JPEG carries EXIF";
    const QImage before = renderOf(mc_, in);
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 1);
    const Bytes file = readFile(out);
    EXPECT_EQ(file.find("PDFE-ALT-XMP-SECRET"), Bytes::npos) << "the alternate's /Metadata survived";
    EXPECT_FALSE(holdsAnySecret(file)) << "the alternate JPEG's EXIF survived";
    EXPECT_NE(file.find("Alternates"), Bytes::npos) << "the alternate itself is kept";
    EXPECT_EQ(renderOf(mc_, out), before);
}

// 4. A soft mask's own /SMask (not drawn, but stored) loses its metadata too.
TEST_F(ExifStripModel, MaskOfAMaskIsStripped) {
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        fz_pixmap* pix = flatPixmap(ctx, fz_device_rgb(ctx), 32, 32, {255, 0, 0});
        pdf_obj* img = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        pdf_obj* sm = addHalfSoftMask(ctx, doc);
        pdf_dict_put_drop(ctx, sm, PDF_NAME(SMask), addFlateImageWithXmp(ctx, doc, 200, "PDFE-MASK-OF-MASK-SECRET"));
        pdf_dict_put_drop(ctx, img, PDF_NAME(SMask), sm);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("mm_in.pdf"), out = path("mm_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NE(readFile(in).find("PDFE-MASK-OF-MASK-SECRET"), Bytes::npos);
    const QImage before = renderOf(mc_, in);
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 1);
    EXPECT_EQ(readFile(out).find("PDFE-MASK-OF-MASK-SECRET"), Bytes::npos) << "the mask's mask kept its /Metadata";
    EXPECT_EQ(renderOf(mc_, out), before);
}

// 4. A page /Thumb (a picture of the page as it was) is removed and reported.
TEST_F(ExifStripModel, PageThumbnailIsRemoved) {
    PageSpec s;
    s.content = "q Q";
    const std::string base = path("thumb_base.pdf"), in = path("thumb_in.pdf"), out = path("thumb_out.pdf");
    buildPdf(mc_, base, {s});
    mc_->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, base.c_str());
        fz_try(ctx) {
            static const char px[] = "PDFE-THUMB-SECRET-PIXELS";   // 8 x 1 RGB
            fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(px), 24);
            pdf_obj* d = pdf_new_dict(ctx, doc, 4);
            pdf_dict_put_int(ctx, d, PDF_NAME(Width), 8);
            pdf_dict_put_int(ctx, d, PDF_NAME(Height), 1);
            pdf_dict_put_int(ctx, d, PDF_NAME(BitsPerComponent), 8);
            pdf_dict_put(ctx, d, PDF_NAME(ColorSpace), PDF_NAME(DeviceRGB));
            pdf_obj* thumb = pdf_add_stream(ctx, doc, b, d, 0);
            pdf_dict_put_drop(ctx, pdf_lookup_page_obj(ctx, doc, 0), PDF_NAME(Thumb), thumb);
            pdf_drop_obj(ctx, d);
            fz_drop_buffer(ctx, b);
            pdf_save_document(ctx, doc, in.c_str(), nullptr);
        }
        fz_always(ctx) { pdf_drop_document(ctx, doc); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    ASSERT_NE(readFile(in).find("PDFE-THUMB-SECRET-PIXELS"), Bytes::npos) << "oracle: thumbnail stored";
    ExifStripper::StripReport r;
    {
        auto doc = Document::open(mc_, in);
        ExifStripper st(mc_);
        r = st.stripAllImagesReport(doc.get());
        saveCopy(doc.get(), out);
    }
    EXPECT_EQ(r.thumbnailsRemoved, 1);
    EXPECT_EQ(r.total(), 0);
    EXPECT_EQ(ExifStripper::describe(r).toStdString(), "Metadata removed from 0 image(s). 1 page thumbnail(s) removed.");
    EXPECT_EQ(readFile(out).find("PDFE-THUMB-SECRET-PIXELS"), Bytes::npos) << "the page thumbnail survived";
    // The app's save may compress the stream, so the plain-text check alone
    // is not enough: the page must have no /Thumb at all.
    bool hasThumb = true;
    mc_->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, out.c_str());
        fz_try(ctx) { hasThumb = pdf_dict_get(ctx, pdf_lookup_page_obj(ctx, doc, 0), PDF_NAME(Thumb)) != nullptr; }
        fz_always(ctx) { pdf_drop_document(ctx, doc); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    });
    EXPECT_FALSE(hasThumb) << "the saved page still has a /Thumb";
}

// 4. Inline images are not rewritten. One with JPEG data (on the page, and
// one inside a form) is counted as not processed and the message says so; an
// inline image with raw samples has no room for metadata and is not counted.
TEST_F(ExifStripModel, InlineJpegImagesAreReportedNotProcessed) {
    const Bytes jpeg = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    const Bytes inlineJpeg = "q 80 0 0 80 10 10 cm BI /W 32 /H 32 /BPC 8 /CS /RGB /F /DCT ID " + jpeg + "\nEI Q\n";
    const Bytes inlineRaw = Bytes("q 20 0 0 20 150 150 cm BI /W 2 /H 1 /BPC 8 /CS /G ID ") + Bytes("\x10\x20", 2) + "\nEI Q\n";
    PageSpec s;
    s.content = inlineJpeg + inlineRaw + "/Fm1 Do";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* fres = pdf_new_dict(ctx, doc, 1);
        pdf_obj* fm = addForm(ctx, doc, "q 80 0 0 80 110 110 cm BI /W 32 /H 32 /BPC 8 /CS /RGB /F /DCT ID " + jpeg + "\nEI Q", fres);
        putImage(ctx, doc, res, "Fm1", fm);
        pdf_drop_obj(ctx, fm);
        pdf_drop_obj(ctx, fres);
    };
    const std::string in = path("inline.pdf");
    buildPdf(mc_, in, {s});
    const QImage shown = renderOf(mc_, in);
    ASSERT_GT(qBlue(shown.pixel(20, 160)), 200) << "oracle: the inline JPEG is drawn";
    auto doc = Document::open(mc_, in);
    auto page = doc->page(0);
    ExifStripper st(mc_);
    const ExifStripper::StripReport r = st.stripPageImagesReport(page.get(), doc.get());
    EXPECT_EQ(r.inlineNotProcessed, 2) << "inline JPEG images on the page and in the form";
    EXPECT_EQ(r.total(), 0);
    EXPECT_EQ(ExifStripper::describe(r).toStdString(),
              "Metadata removed from 0 image(s). 2 inline JPEG image(s) were not processed; "
              "their metadata may remain.");
}

// 5. A DCT filter that is not the last filter: the stored bytes are not the
// JPEG, so it cannot be cut losslessly. It is never reported clean while left
// untouched: it is re-encoded with no metadata, or reported not processed.
TEST_F(ExifStripModel, BuriedDctFilterIsNeverReportedClean) {
    const Bytes jpeg = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    PageSpec s;
    s.content = "q 160 0 0 160 20 20 cm /Im1 Do Q";
    s.fill = [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addRawDctImage(ctx, doc, jpeg, 32);
        pdf_obj* filters = pdf_new_array(ctx, doc, 2);
        pdf_array_push(ctx, filters, PDF_NAME(DCTDecode));
        pdf_array_push(ctx, filters, PDF_NAME(ASCIIHexDecode));
        pdf_dict_put_drop(ctx, img, PDF_NAME(Filter), filters);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    };
    const std::string in = path("buried_in.pdf"), out = path("buried_out.pdf");
    buildPdf(mc_, in, {s});
    ASSERT_NE(readImage(mc_, in).raw.find(kExifSecret), Bytes::npos) << "oracle: stored with its EXIF";
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.total(), 1);
    EXPECT_EQ(r.alreadyClean, 0) << "an untouched JPEG with EXIF was reported clean";
    RecordProperty("outcome", r.failed == 1 ? "reported-not-processed" : "reencoded");
    if (r.stripped == 1) EXPECT_FALSE(holdsAnySecret(readFile(out))) << "reported stripped, but a secret remains";
}

// ============================================================ claim-check remediation (phase 4)
//
//   F1 Tools > Strip Image Metadata: warning vs information box
//     ExifStripUi.FailedImageShowsAWarningWithTheSummary
//     ExifStripUi.InlineJpegShowsAWarning
//     ExifStripUi.CleanDocumentShowsAnInformationBox
//   F2 the informational dictionary entries
//     ExifStripModel.InformationalEntriesAreDeletedFromTheSavedImage
//   F3 full rewrite, so an in-place (incremental-capable) save scrubs the bytes
//     ExifStripModel.EveryStripCallMarksTheDocumentForFullRewrite
//     ExifStripModel.SaveToTheSameFileDoesNotKeepTheOriginalExif
//   Extraction of other colour spaces, and of a pixmap with alpha
//     ImageDataExtractModel.IccBasedCmykExtractsAsRendered
//     ImageDataExtractModel.LabExtractsAsRendered
//     ImageDataExtractModel.DeviceNExtractsAsRendered
//     ImageDataExtractModel.JpxWithAlphaExtractsAsPremultipliedRgba
//   JP2 box lengths
//     ExifStripModel.Jp2XlBoxPathsAreHandled
//     ExifStripModel.Jp2LengthZeroBoxIsDropped

namespace {

// A page with one image built by `fill` (see PageSpec), saved to `path`.
void onePagePdf(const std::shared_ptr<MuPdfContext>& mc, const std::string& path, const PageFill& fill,
                const std::string& content = "q 160 0 0 160 20 20 cm /Im1 Do Q") {
    PageSpec s;
    s.content = content;
    s.fill = fill;
    buildPdf(mc, path, {s});
}

// The visible QMessageBox named stripImageMetadataResultDialog, read and closed.
struct StripBoxWatch {
    QString text;
    QMessageBox::Icon icon = QMessageBox::NoIcon;
    int seen = 0;
};

void runStripAction(pdfe::ui::MainWindow* mw, StripBoxWatch& w) {
    QTimer watcher;
    QObject::connect(&watcher, &QTimer::timeout, [&]() {
        for (QWidget* top : QApplication::topLevelWidgets()) {
            auto* mb = qobject_cast<QMessageBox*>(top);
            if (mb && mb->objectName() == QLatin1String("stripImageMetadataResultDialog") && mb->isVisible()) {
                if (w.seen++ == 0) {
                    w.text = mb->text();
                    w.icon = mb->icon();
                }
                mb->done(QMessageBox::Ok);
            }
        }
    });
    watcher.start(20);
    auto* action = mw->findChild<QAction*>(QStringLiteral("stripExifAction"));
    ASSERT_NE(action, nullptr) << "no stripExifAction";
    ASSERT_TRUE(action->isEnabled()) << "Strip Image Metadata is disabled with a document open";
    action->trigger();
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    watcher.stop();
}

class ExifStripUi : public ModelBase {
protected:
    StripBoxWatch stripThroughUi(const std::string& pdf) {
        StripBoxWatch w;
        auto mw = std::make_unique<pdfe::ui::MainWindow>();
        mw->openFile(QString::fromStdString(pdf));
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        EXPECT_NE(mw->documentForTesting(), nullptr) << "the document did not open";
        if (mw->documentForTesting()) runStripAction(mw.get(), w);
        mw.reset();
        return w;
    }
};

pdf_obj* addRawImage(fz_context* ctx, pdf_document* doc, const Bytes& samples, int w, int h, pdf_obj* cs) {
    fz_buffer* b = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(samples.data()),
                                                  samples.size());
    pdf_obj* d = pdf_new_dict(ctx, doc, 8);
    pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject));
    pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Image));
    pdf_dict_put_int(ctx, d, PDF_NAME(Width), w);
    pdf_dict_put_int(ctx, d, PDF_NAME(Height), h);
    pdf_dict_put_int(ctx, d, PDF_NAME(BitsPerComponent), 8);
    pdf_dict_put(ctx, d, PDF_NAME(ColorSpace), cs);
    pdf_obj* img = pdf_add_stream(ctx, doc, b, d, 0);
    pdf_drop_obj(ctx, d);
    fz_drop_buffer(ctx, b);
    return img;
}

Bytes flatSamples(int w, int h, std::vector<int> comps) {
    Bytes s;
    for (int i = 0; i < w * h; ++i)
        for (int c : comps) s.push_back(static_cast<char>(c));
    return s;
}

// Extracted colour at (4,4) against the page render at the image centre.
void expectExtractMatchesRender(const std::shared_ptr<MuPdfContext>& mc, const std::string& in, int tol) {
    auto doc = Document::open(mc, in);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    const QRgb want = page->render(QRectF(), 1.0f).pixel(100, 100);
    ASSERT_EQ(qAlpha(want), 255) << "oracle: the image paints opaquely";
    ImageManager mgr(mc);
    const QImage ex = mgr.extractImage(page.get(), doc.get(), 0);
    ASSERT_FALSE(ex.isNull()) << "extraction failed";
    const QColor got = ex.pixelColor(4, 4);
    EXPECT_NEAR(got.red(), qRed(want), tol);
    EXPECT_NEAR(got.green(), qGreen(want), tol);
    EXPECT_NEAR(got.blue(), qBlue(want), tol);
    EXPECT_EQ(got.alpha(), 255);
}

} // namespace

// F1. One image cannot be processed: the result is a WARNING box carrying
// describe()'s text.
TEST_F(ExifStripUi, FailedImageShowsAWarningWithTheSummary) {
    const Bytes broken = withBadExifLength(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    const std::string in = path("ui_failed.pdf");
    onePagePdf(mc_, in, [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addRawDctImage(ctx, doc, broken, 0);   // undecodable
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    }, "q Q");
    const StripBoxWatch w = stripThroughUi(in);
    ASSERT_EQ(w.seen, 1) << "no result box was shown";
    EXPECT_EQ(w.icon, QMessageBox::Warning) << "a failed image must be reported with a warning";
    EXPECT_EQ(w.text.toStdString(),
              "Metadata removed from 0 image(s). 1 image(s) could not be processed; their metadata may remain.");
}

// F1. Inline JPEG images are never processed: also a warning.
TEST_F(ExifStripUi, InlineJpegShowsAWarning) {
    const Bytes jpeg = qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0));
    const std::string in = path("ui_inline.pdf");
    PageSpec s;
    s.content = "q 80 0 0 80 10 10 cm BI /W 32 /H 32 /BPC 8 /CS /RGB /F /DCT ID " + jpeg + "\nEI Q\n";
    buildPdf(mc_, in, {s});
    const StripBoxWatch w = stripThroughUi(in);
    ASSERT_EQ(w.seen, 1) << "no result box was shown";
    EXPECT_EQ(w.icon, QMessageBox::Warning) << "skipped inline JPEG images must be reported with a warning";
    EXPECT_EQ(w.text.toStdString(),
              "Metadata removed from 0 image(s). 1 inline JPEG image(s) were not processed; their metadata may remain.");
}

// F1. Everything processed: an INFORMATION box.
TEST_F(ExifStripUi, CleanDocumentShowsAnInformationBox) {
    const Bytes jpeg = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    const std::string in = path("ui_clean.pdf");
    onePagePdf(mc_, in, oneEncodedImage(jpeg));
    const StripBoxWatch w = stripThroughUi(in);
    ASSERT_EQ(w.seen, 1) << "no result box was shown";
    EXPECT_EQ(w.icon, QMessageBox::Information);
    EXPECT_EQ(w.text.toStdString(), "Metadata removed from 1 image(s).");
}

// F2. /Metadata, /PieceInfo, /LastModified, /OPI, /Name and /ID are deleted;
// the OPI file path is not in the saved file.
TEST_F(ExifStripModel, InformationalEntriesAreDeletedFromTheSavedImage) {
    const char* opiPath = "C:/PDFE-OPI-SECRET/originals/hires-scan.tif";
    const std::string in = path("info_in.pdf"), out = path("info_out.pdf");
    onePagePdf(mc_, in, [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        fz_pixmap* pix = flatPixmap(ctx, fz_device_rgb(ctx), 8, 8, {10, 200, 30});
        pdf_obj* img = addPixmapImage(ctx, doc, pix);
        fz_drop_pixmap(ctx, pix);
        pdf_dict_put_drop(ctx, img, PDF_NAME(Metadata), addXmpStream(ctx, doc, "PDFE-INFO-XMP-SECRET"));
        pdf_obj* piece = pdf_dict_put_dict(ctx, img, PDF_NAME(PieceInfo), 1);
        pdf_dict_puts_drop(ctx, piece, "PDFEApp", pdf_new_text_string(ctx, "PDFE-PIECEINFO-SECRET"));
        pdf_dict_puts_drop(ctx, img, "LastModified", pdf_new_text_string(ctx, "D:20260101000000Z"));
        pdf_obj* opi = pdf_new_dict(ctx, doc, 1);
        pdf_obj* v13 = pdf_dict_puts_dict(ctx, opi, "1.3", 2);
        pdf_dict_put_text_string(ctx, v13, PDF_NAME(F), opiPath);
        pdf_dict_puts_drop(ctx, img, "OPI", opi);
        pdf_dict_put_name(ctx, img, PDF_NAME(Name), "PDFEOldName");
        pdf_dict_put_text_string(ctx, img, PDF_NAME(ID), "PDFE-WEBCAPTURE-ID");
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    });
    const ImageState b = readImage(mc_, in);
    for (const char* k : {"Metadata", "PieceInfo", "LastModified", "OPI", "Name", "ID"})
        ASSERT_EQ(b.keys.count(k), 1u) << "oracle: the fixture sets /" << k;
    ASSERT_NE(readFile(in).find(opiPath), Bytes::npos) << "oracle: the OPI path is stored";
    const QImage before = renderOf(mc_, in);
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 1);
    const ImageState a = readImage(mc_, out);
    ASSERT_TRUE(a.found);
    for (const char* k : {"Metadata", "PieceInfo", "LastModified", "OPI", "Name", "ID"})
        EXPECT_EQ(a.keys.count(k), 0u) << "/" << k << " survived in the saved image dictionary";
    const Bytes file = readFile(out);
    for (const char* secret : {opiPath, "PDFE-INFO-XMP-SECRET", "PDFE-PIECEINFO-SECRET", "PDFEOldName", "PDFE-WEBCAPTURE-ID"})
        EXPECT_EQ(file.find(secret), Bytes::npos) << secret << " is still in the saved file";
    EXPECT_EQ(renderOf(mc_, out), before);
}

// F3. Each public strip call marks the document for a full rewrite.
TEST_F(ExifStripModel, EveryStripCallMarksTheDocumentForFullRewrite) {
    const Bytes jpeg = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    const std::string in = path("rewrite_flag.pdf");
    onePagePdf(mc_, in, oneEncodedImage(jpeg));
    ExifStripper st(mc_);
    const std::vector<std::pair<const char*, std::function<void(Document*, Page*)>>> calls = {
        {"stripImageResult", [&](Document* d, Page* p) { st.stripImageResult(p, d, "Im1"); }},
        {"stripImage", [&](Document* d, Page* p) { st.stripImage(p, d, "Im1"); }},
        {"stripPageImagesReport", [&](Document* d, Page* p) { st.stripPageImagesReport(p, d); }},
        {"stripPageImages", [&](Document* d, Page* p) { st.stripPageImages(p, d); }},
        {"stripAllImagesReport", [&](Document* d, Page*) { st.stripAllImagesReport(d); }},
        {"stripAllImages", [&](Document* d, Page*) { st.stripAllImages(d); }},
    };
    for (const auto& [name, call] : calls) {
        auto doc = Document::open(mc_, in);
        ASSERT_NE(doc, nullptr);
        ASSERT_FALSE(doc->sessionRequiresFullRewrite()) << "oracle: a freshly opened document";
        auto page = doc->page(0);
        call(doc.get(), page.get());
        EXPECT_TRUE(doc->sessionRequiresFullRewrite()) << name << " did not mark the document for a full rewrite";
    }
}

// F3. Strip, then the app's ordinary save to the SAME file (Document::save,
// which appends incrementally unless a full rewrite is required): none of the
// original metadata bytes survive in the file.
TEST_F(ExifStripModel, SaveToTheSameFileDoesNotKeepTheOriginalExif) {
    const Bytes jpeg = withMetadata(qtJpeg(QColor(0, 0, 255), QColor(255, 0, 0)));
    const std::string in = path("inplace.pdf");
    onePagePdf(mc_, in, [&](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* img = addEncodedImage(ctx, doc, jpeg);
        pdf_dict_put_drop(ctx, img, PDF_NAME(Metadata), addXmpStream(ctx, doc, "PDFE-INPLACE-XMP-SECRET"));
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    });
    ASSERT_TRUE(holdsAnySecret(readFile(in))) << "oracle: the file carries EXIF";
    {
        auto doc = Document::open(mc_, in);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ExifStripper st(mc_);
        EXPECT_EQ(st.stripPageImages(page.get(), doc.get()), 1);
        page.reset();
        doc->save(in);
    }
    const Bytes file = readFile(in);
    EXPECT_FALSE(holdsAnySecret(file)) << "the original EXIF bytes survived an in-place save";
    EXPECT_EQ(file.find("PDFE-INPLACE-XMP-SECRET"), Bytes::npos) << "the removed /Metadata survived an in-place save";
    auto reopened = Document::open(mc_, in);
    ASSERT_NE(reopened, nullptr) << "the saved file does not open";
    EXPECT_FALSE(reopened->page(0)->render(QRectF(), 1.0f).isNull());
}

// Extraction: an ICCBased /N 4 image is converted to RGB the way the page
// renders it. PDFE's MuPDF is built without an ICC engine (FZ_ENABLE_ICC=0 in
// vcpkg-overlay-ports/libmupdf), so an ICCBased space resolves to the device
// space its /N names and the profile bytes are not read; a placeholder body
// is enough.
TEST_F(ImageDataExtractModel, IccBasedCmykExtractsAsRendered) {
    const std::string in = path("x_icc.pdf");
    onePagePdf(mc_, in, [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        static const char body[] = "PDFE placeholder ICC profile body";
        fz_buffer* icc = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(body), sizeof(body) - 1);
        pdf_obj* d = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put_int(ctx, d, PDF_NAME(N), 4);
        pdf_dict_put(ctx, d, PDF_NAME(Alternate), PDF_NAME(DeviceCMYK));
        pdf_obj* prof = pdf_add_stream(ctx, doc, icc, d, 0);
        fz_drop_buffer(ctx, icc);
        pdf_drop_obj(ctx, d);
        pdf_obj* cs = pdf_new_array(ctx, doc, 2);
        pdf_array_push(ctx, cs, PDF_NAME(ICCBased));
        pdf_array_push_drop(ctx, cs, prof);
        pdf_obj* img = addRawImage(ctx, doc, flatSamples(8, 8, {200, 100, 0, 50}), 8, 8, cs);
        pdf_drop_obj(ctx, cs);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    });
    expectExtractMatchesRender(mc_, in, 12);
}

// Extraction: a Lab image.
TEST_F(ImageDataExtractModel, LabExtractsAsRendered) {
    const std::string in = path("x_lab.pdf");
    onePagePdf(mc_, in, [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        pdf_obj* params = pdf_new_dict(ctx, doc, 2);
        pdf_obj* wp = pdf_dict_put_array(ctx, params, PDF_NAME(WhitePoint), 3);
        pdf_array_push_real(ctx, wp, 0.9505);
        pdf_array_push_real(ctx, wp, 1.0);
        pdf_array_push_real(ctx, wp, 1.089);
        pdf_obj* cs = pdf_new_array(ctx, doc, 2);
        pdf_array_push(ctx, cs, PDF_NAME(Lab));
        pdf_array_push_drop(ctx, cs, params);
        // L 60, a +40, b -30 (8-bit over [0 100], [-100 100], [-100 100]).
        pdf_obj* img = addRawImage(ctx, doc, flatSamples(8, 8, {153, 179, 89}), 8, 8, cs);
        pdf_drop_obj(ctx, cs);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    });
    expectExtractMatchesRender(mc_, in, 12);
}

// Extraction: a DeviceN [Cyan Magenta] image with a CMYK alternate.
TEST_F(ImageDataExtractModel, DeviceNExtractsAsRendered) {
    const std::string in = path("x_devn.pdf");
    onePagePdf(mc_, in, [](fz_context* ctx, pdf_document* doc, pdf_obj* res) {
        const char ps[] = "{ 0 0 }";
        fz_buffer* fb = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(ps), sizeof(ps) - 1);
        pdf_obj* fd = pdf_new_dict(ctx, doc, 3);
        pdf_dict_put_int(ctx, fd, PDF_NAME(FunctionType), 4);
        pdf_obj* dom = pdf_dict_put_array(ctx, fd, PDF_NAME(Domain), 4);
        for (int k = 0; k < 2; ++k) { pdf_array_push_int(ctx, dom, 0); pdf_array_push_int(ctx, dom, 1); }
        pdf_obj* rng = pdf_dict_put_array(ctx, fd, PDF_NAME(Range), 8);
        for (int k = 0; k < 4; ++k) { pdf_array_push_int(ctx, rng, 0); pdf_array_push_int(ctx, rng, 1); }
        pdf_obj* fn = pdf_add_stream(ctx, doc, fb, fd, 0);
        pdf_drop_obj(ctx, fd);
        fz_drop_buffer(ctx, fb);
        pdf_obj* names = pdf_new_array(ctx, doc, 2);
        pdf_array_push_name(ctx, names, "Cyan");
        pdf_array_push_name(ctx, names, "Magenta");
        pdf_obj* cs = pdf_new_array(ctx, doc, 4);
        pdf_array_push(ctx, cs, PDF_NAME(DeviceN));
        pdf_array_push_drop(ctx, cs, names);
        pdf_array_push(ctx, cs, PDF_NAME(DeviceCMYK));
        pdf_array_push_drop(ctx, cs, fn);
        pdf_obj* img = addRawImage(ctx, doc, flatSamples(8, 8, {200, 100}), 8, 8, cs);
        pdf_drop_obj(ctx, cs);
        putImage(ctx, doc, res, "Im1", img);
        pdf_drop_obj(ctx, img);
    });
    expectExtractMatchesRender(mc_, in, 12);
}

// Extraction: a pixmap with alpha (a JP2 with an alpha channel; fixture
// jpx_rgba_halfalpha_8x8.jp2, made with Pillow: left half opaque blue, right
// half RGBA 200,100,0,128, lossless) comes out as premultiplied RGBA, so the
// half-transparent colour reads back as 200,100,0 at alpha 128, not halved.
TEST_F(ImageDataExtractModel, JpxWithAlphaExtractsAsPremultipliedRgba) {
    const Bytes jp2 = readFile(std::string(TEST_FIXTURES_DIR) + "/jpx_rgba_halfalpha_8x8.jp2");
    ASSERT_GT(jp2.size(), 100u) << "oracle: fixture present";
    ScopedJpxDecoding jpxOn(true);
    const std::string in = path("x_rgba.pdf");
    onePagePdf(mc_, in, oneEncodedImage(jp2));
    ASSERT_EQ(readImage(mc_, in).filter, "JPXDecode");
    auto doc = Document::open(mc_, in);
    auto page = doc->page(0);
    ImageManager mgr(mc_);
    const QImage ex = mgr.extractImage(page.get(), doc.get(), 0);
    ASSERT_FALSE(ex.isNull());
    ASSERT_EQ(ex.size(), QSize(8, 8));
    EXPECT_EQ(ex.format(), QImage::Format_RGBA8888_Premultiplied);
    const QColor opaque = ex.pixelColor(1, 4), half = ex.pixelColor(6, 4);
    EXPECT_EQ(opaque, QColor(0, 0, 255, 255));
    EXPECT_NEAR(half.alpha(), 128, 2);
    EXPECT_NEAR(half.red(), 200, 4) << "the colour was read as straight alpha (halved)";
    EXPECT_NEAR(half.green(), 100, 4);
    EXPECT_NEAR(half.blue(), 0, 4);
}

// JP2: a dropped private box written with an XLBox (length 1 + 64-bit
// length), and a jp2h superbox in XLBox form whose private child is dropped
// (the 64-bit length is patched).
TEST_F(ExifStripModel, Jp2XlBoxPathsAreHandled) {
    const Bytes jp2 = readFile(std::string(TEST_FIXTURES_DIR) + "/jpx_legit_a4_300dpi.jp2");
    ASSERT_GT(jp2.size(), 100u);
    const size_t boxStart = jp2.find("jp2h") - 4;
    const size_t boxLen = (static_cast<unsigned char>(jp2[boxStart]) << 24) | (static_cast<unsigned char>(jp2[boxStart + 1]) << 16) |
                          (static_cast<unsigned char>(jp2[boxStart + 2]) << 8) | static_cast<unsigned char>(jp2[boxStart + 3]);
    auto xlHeader = [](const char* type, uint64_t len) {
        Bytes h(4, '\0');
        putBE32(h, 0, 1);
        h += type;
        for (int k = 7; k >= 0; --k) h.push_back(static_cast<char>(len >> (8 * k)));
        return h;
    };
    const Bytes children = jp2.substr(boxStart + 8, boxLen - 8);
    const Bytes nested = jp2Box("priv", "PDFE-JP2-XL-NESTED-SECRET");
    const Bytes xlPrivBody = "PDFE-JP2-XLBOX-SECRET";
    const Bytes xlPriv = xlHeader("priv", 16 + xlPrivBody.size()) + xlPrivBody;
    const Bytes input = jp2.substr(0, boxStart) + xlHeader("jp2h", 16 + children.size() + nested.size()) + children +
                        nested + xlPriv + jp2.substr(boxStart + boxLen);
    const Bytes expected = jp2.substr(0, boxStart) + xlHeader("jp2h", 16 + children.size()) + children +
                           jp2.substr(boxStart + boxLen);
    ScopedJpxDecoding jpxOn(true);
    const std::string in = path("jp2xl_in.pdf"), out = path("jp2xl_out.pdf");
    onePagePdf(mc_, in, oneEncodedImage(input));
    ASSERT_EQ(readImage(mc_, in).filter, "JPXDecode");
    const QImage before = renderOf(mc_, in);
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 1);
    EXPECT_EQ(r.failed, 0);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.filter, "JPXDecode") << "the XLBox structure was treated as malformed";
    EXPECT_EQ(a.raw, expected) << "XLBox jp2h not patched, or the XLBox private box not dropped";
    const Bytes file = readFile(out);
    EXPECT_EQ(file.find("PDFE-JP2-XLBOX-SECRET"), Bytes::npos);
    EXPECT_EQ(file.find("PDFE-JP2-XL-NESTED-SECRET"), Bytes::npos);
    EXPECT_EQ(renderOf(mc_, out), before);
}

// JP2: a private box of length 0 (it runs to the end of the file) after the
// codestream is dropped; the rest is the original file.
TEST_F(ExifStripModel, Jp2LengthZeroBoxIsDropped) {
    const Bytes jp2 = readFile(std::string(TEST_FIXTURES_DIR) + "/jpx_legit_a4_300dpi.jp2");
    ASSERT_GT(jp2.size(), 100u);
    Bytes tail = jp2Box("priv", "PDFE-JP2-LEN0-SECRET");
    putBE32(tail, 0, 0);
    ScopedJpxDecoding jpxOn(true);
    const std::string in = path("jp2len0_in.pdf"), out = path("jp2len0_out.pdf");
    onePagePdf(mc_, in, oneEncodedImage(jp2 + tail));
    ASSERT_NE(readImage(mc_, in).raw.find("PDFE-JP2-LEN0-SECRET"), Bytes::npos);
    const QImage before = renderOf(mc_, in);
    const ExifStripper::StripReport r = stripPageAndSave(mc_, in, out);
    EXPECT_EQ(r.stripped, 1);
    const ImageState a = readImage(mc_, out);
    EXPECT_EQ(a.filter, "JPXDecode");
    EXPECT_EQ(a.raw, jp2) << "the length-0 private box was not dropped cleanly";
    EXPECT_EQ(readFile(out).find("PDFE-JP2-LEN0-SECRET"), Bytes::npos);
    EXPECT_EQ(renderOf(mc_, out), before);
}
