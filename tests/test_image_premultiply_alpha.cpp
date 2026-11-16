// Regression tests for the transparency/alpha defects fixed in pipeline
// `transparency-insert-fix` phase-2:
//
//   Defect A — ImageManager::insertImage dropped alpha for transparent inputs
//              whose QImage format was neither RGB32 nor ARGB32 (e.g. an
//              Indexed8 PNG with a transparent palette entry), AND wrote
//              straight (non-premultiplied) colour samples.
//   Defect B — AnnotationManager::setStampImageAppearance used a plain `int`
//              pixel index (32-bit overflow for large stamps) and also wrote
//              straight colour; the #392 size_t promotion was never ported.
//   Defect C — ExifStripper's QImage->fz_pixmap copy wrote straight colour.
//
// The shared root cause of the colour defect: MuPDF fz_pixmaps with alpha store
// PREMULTIPLIED colour by convention. pdf_add_image un-premultiplies on save
// (vcpkg MuPDF source/pdf/pdf-image.c: `inva = a ? 255*256/a : 0;
// *d++ = (*s++ * inva) >> 8`) and splits alpha into a grayscale /SMask. Copying
// STRAIGHT samples into the alpha pixmap makes that un-premultiply blow out
// coloured semi-transparent pixels. The fix converts alpha-bearing source
// images to QImage::Format_ARGB32_Premultiplied before the copy.
//
// These tests use only synthetic, in-memory images and the committed
// tests/fixtures/test_single.pdf — no sensitive paths.

#include <gtest/gtest.h>
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "image/ImageSampleIndex.h"
#include "annotations/AnnotationData.h"
#include "annotations/AnnotationManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QImage>
#include <QRectF>
#include <QVector>
#include <QRgb>
#include <QTemporaryDir>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

namespace {

std::string singlePagePdf() {
    return std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
}

// Load the first /XObject of /Subtype /Image found on page 0 of `doc` whose
// width matches `wantWidth`, run it through MuPDF's decode, and return a
// freshly-allocated RGB pixmap (caller drops it). Also reports whether the
// image stream carries a non-null /SMask. Returns nullptr if none found.
struct DecodedImage {
    int width = 0;
    int height = 0;
    int n = 0;
    bool hasSMask = false;
    std::vector<unsigned char> samples;  // n bytes/pixel, row-major
};

// Insert modifies the Document in memory; Page::fzPage() is private to tests, so
// we round-trip through a saved PDF (lossless flate) and inspect that instead.
bool decodeSavedImage(const std::shared_ptr<MuPdfContext>& context,
                      const std::string& pdfPath,
                      int wantWidth, DecodedImage& out) {
    bool found = false;
    context->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_open_document(ctx, pdfPath.c_str());
        if (!pdfDoc) return;
        pdf_page* pdfPage = pdf_load_page(ctx, pdfDoc, 0);
        if (!pdfPage) { pdf_drop_document(ctx, pdfDoc); return; }
        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        pdf_obj* xobjs = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (!xobjs) {
            fz_drop_page(ctx, reinterpret_cast<fz_page*>(pdfPage));
            pdf_drop_document(ctx, pdfDoc);
            return;
        }

        const int len = pdf_dict_len(ctx, xobjs);
        for (int i = 0; i < len && !found; ++i) {
            pdf_obj* val = pdf_dict_get_val(ctx, xobjs, i);
            pdf_obj* subtype = pdf_dict_get(ctx, val, PDF_NAME(Subtype));
            if (!pdf_name_eq(ctx, subtype, PDF_NAME(Image))) continue;

            const int w = pdf_dict_get_int(ctx, val, PDF_NAME(Width));
            if (wantWidth > 0 && w != wantWidth) continue;

            pdf_obj* smask = pdf_dict_get(ctx, val, PDF_NAME(SMask));
            out.hasSMask = (smask != nullptr);

            fz_image* img = pdf_load_image(ctx, pdfDoc, val);
            fz_pixmap* pix = fz_get_pixmap_from_image(ctx, img, nullptr, nullptr,
                                                      nullptr, nullptr);
            out.width = pix->w;
            out.height = pix->h;
            out.n = pix->n;
            const size_t total = static_cast<size_t>(pix->h) * pix->stride;
            out.samples.assign(pix->samples, pix->samples + total);
            // normalize to tightly-packed n bytes/pixel for pixel(0,0) reads via
            // the stride we stored; we keep stride implicitly as w*n is usual.
            // Re-pack row 0 explicitly to be safe:
            if (pix->stride != static_cast<ptrdiff_t>(pix->w) * pix->n) {
                std::vector<unsigned char> packed(
                    static_cast<size_t>(pix->w) * pix->h * pix->n);
                for (int y = 0; y < pix->h; ++y) {
                    std::memcpy(&packed[static_cast<size_t>(y) * pix->w * pix->n],
                                pix->samples + static_cast<size_t>(y) * pix->stride,
                                static_cast<size_t>(pix->w) * pix->n);
                }
                out.samples.swap(packed);
            }
            fz_drop_pixmap(ctx, pix);
            fz_drop_image(ctx, img);
            found = true;
        }
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(pdfPage));
        pdf_drop_document(ctx, pdfDoc);
    });
    return found;
}

// A stamp's image lives in its /AP/N appearance Form XObject's
// /Resources/XObject, not in the page resources. Walk the page's annotations in
// the saved PDF, find the first appearance Image XObject, decode it and report
// its /SMask presence and samples. Returns false if none found.
bool decodeStampApImage(const std::shared_ptr<MuPdfContext>& context,
                        const std::string& pdfPath,
                        int wantWidth, DecodedImage& out) {
    bool found = false;
    context->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_open_document(ctx, pdfPath.c_str());
        if (!pdfDoc) return;
        pdf_page* pdfPage = pdf_load_page(ctx, pdfDoc, 0);
        if (!pdfPage) { pdf_drop_document(ctx, pdfDoc); return; }

        for (pdf_annot* a = pdf_first_annot(ctx, pdfPage);
             a && !found; a = pdf_next_annot(ctx, a)) {
            pdf_obj* annotObj = pdf_annot_obj(ctx, a);
            pdf_obj* ap = pdf_dict_get(ctx, annotObj, PDF_NAME(AP));
            pdf_obj* n = pdf_dict_get(ctx, ap, PDF_NAME(N));
            if (!n) continue;
            pdf_obj* res = pdf_dict_get(ctx, n, PDF_NAME(Resources));
            pdf_obj* xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
            if (!xobjs) continue;

            const int len = pdf_dict_len(ctx, xobjs);
            for (int i = 0; i < len && !found; ++i) {
                pdf_obj* val = pdf_dict_get_val(ctx, xobjs, i);
                pdf_obj* subtype = pdf_dict_get(ctx, val, PDF_NAME(Subtype));
                if (!pdf_name_eq(ctx, subtype, PDF_NAME(Image))) continue;
                const int w = pdf_dict_get_int(ctx, val, PDF_NAME(Width));
                if (wantWidth > 0 && w != wantWidth) continue;

                pdf_obj* smask = pdf_dict_get(ctx, val, PDF_NAME(SMask));
                out.hasSMask = (smask != nullptr);

                fz_image* img = pdf_load_image(ctx, pdfDoc, val);
                fz_pixmap* pix = fz_get_pixmap_from_image(ctx, img, nullptr,
                                                          nullptr, nullptr, nullptr);
                out.width = pix->w;
                out.height = pix->h;
                out.n = pix->n;
                std::vector<unsigned char> packed(
                    static_cast<size_t>(pix->w) * pix->h * pix->n);
                for (int yy = 0; yy < pix->h; ++yy) {
                    std::memcpy(&packed[static_cast<size_t>(yy) * pix->w * pix->n],
                                pix->samples + static_cast<size_t>(yy) * pix->stride,
                                static_cast<size_t>(pix->w) * pix->n);
                }
                out.samples.swap(packed);
                fz_drop_pixmap(ctx, pix);
                fz_drop_image(ctx, img);
                found = true;
            }
        }
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(pdfPage));
        pdf_drop_document(ctx, pdfDoc);
    });
    return found;
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 1 (Defect A/B/C colour contract) — premultiplied colour round-trip.
//
// Insert a uniform semi-transparent coloured image (R=200,G=100,B=50,A=128)
// through the real ImageManager::insertImage path, then decode the embedded
// image XObject and read back the stored (un-premultiplied) red channel.
//
//   FIXED  : insertImage premultiplies -> pixmap red = round(200*128/255)=100;
//            pdf_add_image un-premultiplies: (100*510)>>8 = 199 (~original 200).
//   PRE-FIX: straight red = 200 -> un-premultiply (200*510)>>8 = 398 -> clamp 255.
//
// So the decoded red is ~199 after the fix and 255 before it: assert red < 220.
// ---------------------------------------------------------------------------
TEST(ImagePremultiplyAlpha, InsertedColourIsPremultipliedNotBlownOut) {
    auto context = std::make_shared<MuPdfContext>();
    auto manager = std::make_unique<ImageManager>(context);

    ASSERT_TRUE(std::filesystem::exists(singlePagePdf())) << singlePagePdf();
    auto doc = Document::open(context, singlePagePdf());
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    const int W = 8, H = 8;
    QImage src(W, H, QImage::Format_ARGB32);  // straight alpha
    src.fill(qRgba(200, 100, 50, 128));       // R,G,B,A straight

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString pngPath = tmp.filePath("premul_src.png");
    ASSERT_TRUE(src.save(pngPath, "PNG"));

    InsertImageOptions opts;
    opts.filePath = pngPath.toStdString();
    opts.targetRect = QRectF(72, 72, 64, 64);
    auto result = manager->insertImage(page.get(), doc.get(), opts);
    ASSERT_TRUE(result.success) << result.errorMessage;

    const std::string outPdf = tmp.filePath("premul_out.pdf").toStdString();
    ASSERT_NO_THROW(doc->saveAs(outPdf));

    DecodedImage dec;
    ASSERT_TRUE(decodeSavedImage(context, outPdf, W, dec))
        << "inserted image XObject not found in saved PDF";
    ASSERT_GE(dec.n, 3);
    ASSERT_TRUE(dec.hasSMask)
        << "a semi-transparent image must emit an /SMask";

    const int red0 = dec.samples[0];
    // Fixed path ~199; pre-fix straight-copy path 255 (clamped). Gate at 220.
    EXPECT_LT(red0, 220)
        << "decoded red=" << red0
        << " — straight (non-premultiplied) colour was written and MuPDF's "
           "un-premultiply blew it out (expected ~199 for premultiplied input)";
    EXPECT_GT(red0, 180) << "decoded red=" << red0 << " (expected ~199)";
}

// ---------------------------------------------------------------------------
// Test 1b (conversion-level contract, fully deterministic) — Qt's
// Format_ARGB32_Premultiplied yields premultiplied BGRA samples, which is the
// normalization the three fixes apply. Straight (200,100,50)@a=128 becomes
// (100,50,25) premultiplied (±1 rounding). This documents the exact arithmetic.
// ---------------------------------------------------------------------------
TEST(ImagePremultiplyAlpha, Argb32PremultipliedConversionIsPremultiplied) {
    QImage straight(1, 1, QImage::Format_ARGB32);
    straight.setPixel(0, 0, qRgba(200, 100, 50, 128));

    QImage premul = straight.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const uchar* sl = premul.constScanLine(0);
    std::uint32_t pixel;
    std::memcpy(&pixel, sl, 4);
    const int b = pixel & 0xFF;
    const int g = (pixel >> 8) & 0xFF;
    const int r = (pixel >> 16) & 0xFF;
    const int a = (pixel >> 24) & 0xFF;

    EXPECT_EQ(a, 128);
    EXPECT_NEAR(r, 100, 1);  // round(200*128/255)=100
    EXPECT_NEAR(g, 50, 1);   // round(100*128/255)=50
    EXPECT_NEAR(b, 25, 1);   // round(50*128/255)=25
    // Pre-fix straight copy would see 200/100/50 here.
}

// ---------------------------------------------------------------------------
// Test 2 (Defect B) — the stamp pixel index must be computed in 64-bit.
//
// Both ImageManager::insertImage and AnnotationManager::setStampImageAppearance
// now compute their pixel index through the SHARED helper
// pdfe::image::imageSampleIndex(). This test exercises that exact product
// function (not a reimplementation): with width=50000, y=50000, x=0, n=4 the
// true value is 10,000,000,000, far beyond INT32_MAX (2,147,483,647). Reverting
// the helper body to `int` arithmetic makes this test fail (the value wraps);
// the size_t body passes. Pure arithmetic — no multi-GB allocation.
// ---------------------------------------------------------------------------
TEST(ImagePremultiplyAlpha, StampPixelIndexIsComputedIn64Bit) {
    const int width = 50000;
    const int y = 50000;
    const int x = 0;
    const int n = 4;

    // Call the REAL shared helper the product uses at both raster-copy sites.
    const std::size_t idx = pdfe::image::imageSampleIndex(y, width, x, n);

    EXPECT_EQ(idx, static_cast<std::size_t>(10000000000ULL))
        << "imageSampleIndex overflowed — the helper is not computing in size_t "
           "(a reverted int-typed body wraps here)";
    EXPECT_GT(idx, static_cast<std::size_t>(INT32_MAX));
}

// ---------------------------------------------------------------------------
// Test 3 (Defect A) — a transparent non-RGB32/ARGB32 source keeps its alpha.
//
// An Indexed8 image with a transparent palette entry has hasAlphaChannel()==true
// but format Format_Indexed8. The pre-fix normalization converted any
// non-RGB32/ARGB32 format to opaque RGB32, dropping the alpha, so the inserted
// PDF image had NO /SMask. The fix converts alpha-bearing inputs to
// Format_ARGB32_Premultiplied, preserving alpha -> /SMask present.
// ---------------------------------------------------------------------------
TEST(ImagePremultiplyAlpha, TransparentIndexedInputKeepsAlphaSMask) {
    auto context = std::make_shared<MuPdfContext>();
    auto manager = std::make_unique<ImageManager>(context);

    auto doc = Document::open(context, singlePagePdf());
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    const int W = 10, H = 10;
    QImage idx(W, H, QImage::Format_Indexed8);
    QVector<QRgb> table(2);
    table[0] = qRgba(200, 100, 50, 255);  // opaque colour
    table[1] = qRgba(0, 0, 0, 0);         // fully transparent
    idx.setColorTable(table);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            idx.setPixel(x, y, (x < W / 2) ? 0 : 1);  // half transparent

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString pngPath = tmp.filePath("indexed_alpha.png");
    ASSERT_TRUE(idx.save(pngPath, "PNG"));

    // Precondition: the reloaded image must be a transparent, non-RGB32/ARGB32
    // format, else the pre-fix drop would not have triggered and the test is
    // vacuous. (A palette PNG reloads as Format_Indexed8.)
    QImage reloaded(pngPath);
    ASSERT_FALSE(reloaded.isNull());
    ASSERT_TRUE(reloaded.hasAlphaChannel())
        << "fixture did not reload with alpha (format=" << reloaded.format() << ")";
    ASSERT_NE(reloaded.format(), QImage::Format_RGB32);
    ASSERT_NE(reloaded.format(), QImage::Format_ARGB32)
        << "fixture reloaded as ARGB32; the format-drop trigger would be vacuous";

    InsertImageOptions opts;
    opts.filePath = pngPath.toStdString();
    opts.targetRect = QRectF(72, 72, 80, 80);
    auto result = manager->insertImage(page.get(), doc.get(), opts);
    ASSERT_TRUE(result.success) << result.errorMessage;

    const std::string outPdf = tmp.filePath("indexed_out.pdf").toStdString();
    ASSERT_NO_THROW(doc->saveAs(outPdf));

    DecodedImage dec;
    ASSERT_TRUE(decodeSavedImage(context, outPdf, W, dec))
        << "inserted image XObject not found in saved PDF";
    EXPECT_TRUE(dec.hasSMask)
        << "transparent Indexed8 input lost its alpha (no /SMask): the format "
           "normalization dropped the alpha channel";
}

// ---------------------------------------------------------------------------
// Test 4 (Defect B — stamp premultiply, END TO END) — route a uniform
// straight-alpha image (200,100,50) @ alpha=128 THROUGH
// AnnotationManager::setStampImageAppearance, save, then decode the stamp's
// /AP/N appearance Image XObject and read back the stored red channel.
//
//   FIXED  : setStampImageAppearance premultiplies -> pixmap red=100 ->
//            pdf_add_image un-premultiplies: (100*510)>>8 = 199 (~original 200).
//   PRE-FIX: straight red=200 -> un-premultiply (200*510)>>8 = 398 -> clamp 255.
//
// Asserts the decoded red is ~199 (fixed) and NOT the blown-out pre-fix value.
// Reverting only AnnotationManager's premultiply hunk makes this fail.
// ---------------------------------------------------------------------------
TEST(ImagePremultiplyAlpha, StampAppearanceColourIsPremultipliedNotBlownOut) {
    using namespace pdfe::annotations;
    auto context = std::make_shared<MuPdfContext>();
    auto mgr = std::make_unique<AnnotationManager>(context);

    ASSERT_TRUE(std::filesystem::exists(singlePagePdf())) << singlePagePdf();
    auto doc = Document::open(context, singlePagePdf());
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    const int W = 8, H = 8;
    QImage src(W, H, QImage::Format_ARGB32);   // straight alpha
    src.fill(qRgba(200, 100, 50, 128));

    AnnotationData stamp =
        mgr->createAnnotation(page.get(), AnnotationType::Stamp,
                              QRectF(72, 72, 64, 64));
    ASSERT_GE(stamp.annotIndex, 0);
    ASSERT_NO_THROW(mgr->setStampImageAppearance(page.get(), stamp.annotIndex, src));

    // Sanity: the appearance really carries an image XObject (not a text stamp).
    ASSERT_TRUE(mgr->stampAppearanceHasImageXObjectForTesting(page.get(),
                                                              stamp.annotIndex));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string outPdf = tmp.filePath("stamp_out.pdf").toStdString();
    ASSERT_NO_THROW(doc->saveAs(outPdf));

    DecodedImage dec;
    ASSERT_TRUE(decodeStampApImage(context, outPdf, W, dec))
        << "stamp appearance Image XObject not found in saved PDF";
    ASSERT_GE(dec.n, 3);
    ASSERT_TRUE(dec.hasSMask)
        << "a semi-transparent stamp image must emit an /SMask";

    const int red0 = dec.samples[0];
    EXPECT_LT(red0, 220)
        << "decoded stamp red=" << red0
        << " — straight (non-premultiplied) colour was written and MuPDF's "
           "un-premultiply blew it out (expected ~199 for premultiplied input)";
    EXPECT_GT(red0, 180) << "decoded stamp red=" << red0 << " (expected ~199)";
}
