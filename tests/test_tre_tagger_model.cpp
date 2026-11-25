// -*- coding: utf-8 -*-
// Pipeline image-placement-state, phase 6: the rules decided for the OCR image
// write-back (PdfContentWriter / ImageReplacer, GitHub #443-#447) and for the
// content-stream tagger (ContentStreamTagger::tagPageContent, GitHub #438).
// The phase-1 probes are in tests/test_tre_tagger_state.cpp; these pin the
// decided behaviour around them (docs/image-placement-model.md).
//
//   TreReplaceModelTest   frame, matrix write-back (rotation + flip + crop,
//                         oblique), image state kept (SMask, ICC, Mask, OC,
//                         Decode), one placement only (page, shared dict,
//                         form refused), whole-page picture on a rotated scan
//   TaggerModelTest       explicit ranges, automatic placement (interleaved
//                         text and images, the AutoTagger pipeline), existing
//                         marked content and structure tree kept, a range that
//                         cuts a nested pair is not written, rendering kept
//   Security-audit follow-ups (remediation): bounded placement on deep or
//                         crossing nesting, an inline image dictionary with ID
//                         look-alikes, and the write-back size guard
//                         (TreReplaceSizeGuardTest, TreReplaceSizeGuardUnitTest)
#include <gtest/gtest.h>

#include "compliance/AutoTagger.h"
#include "compliance/ContentStreamTagger.h"
#include "compliance/StructureTreeBuilder.h"
#include "compliance/StructureTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PageContentGuard.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/FontResolver.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/SubstituteFontSelector.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "tre/BgColorDetector.h"
#include "tre/BitmapCompositor.h"
#include "tre/CoordMapper.h"
#include "tre/FontMatcher.h"
#include "tre/ImageMasker.h"
#include "tre/ImageReplacer.h"
#include "tre/OcrPipeline.h"
#include "tre/PdfContentWriter.h"
#include "tre/RegionReconstructor.h"
#include "tre/WriteBackGuard.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <functional>
#include <memory>
#include <regex>
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

namespace {

using namespace pdfe::core;

void ensureApp() {
    if (!QApplication::instance()) {
        static int argc = 0;
        static char* argv[] = {nullptr};
        static QApplication app(argc, argv);
    }
}

std::string fixturePath(const char* name) {
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"}) {
        const std::string c = std::string(prefix) + name;
        if (std::filesystem::exists(c)) return c;
    }
    const std::string d = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return std::filesystem::exists(d) ? d : std::string();
}

bool isRed(QRgb c) { return qAlpha(c) > 200 && qRed(c) > 200 && qGreen(c) < 90 && qBlue(c) < 90; }
bool isBlue(QRgb c) { return qAlpha(c) > 200 && qRed(c) < 90 && qGreen(c) < 90 && qBlue(c) > 200; }
bool isMagenta(QRgb c) {
    return qAlpha(c) > 200 && qRed(c) > 200 && qGreen(c) < 90 && qBlue(c) > 200;
}
bool isBackground(QRgb c) {
    return qAlpha(c) < 30 || (qRed(c) > 230 && qGreen(c) > 230 && qBlue(c) > 230);
}

// A raw image XObject (8-bit RGB, unfiltered) filled with `rgb`, optional
// extra dictionary entries applied by `extra`.
pdf_obj* addRawRgbImage(fz_context* ctx, pdf_document* pdoc, int w, int h,
                        const std::function<void(int, int, unsigned char*)>& pixel) {
    std::vector<unsigned char> data(static_cast<size_t>(w * h * 3));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) pixel(x, y, &data[static_cast<size_t>((y * w + x) * 3)]);
    fz_buffer* buf = fz_new_buffer_from_copied_data(ctx, data.data(), data.size());
    pdf_obj* img = pdf_add_stream(ctx, pdoc, buf, nullptr, 0);
    fz_drop_buffer(ctx, buf);
    pdf_dict_put(ctx, img, PDF_NAME(Type), PDF_NAME(XObject));
    pdf_dict_put(ctx, img, PDF_NAME(Subtype), PDF_NAME(Image));
    pdf_dict_put_int(ctx, img, PDF_NAME(Width), w);
    pdf_dict_put_int(ctx, img, PDF_NAME(Height), h);
    pdf_dict_put(ctx, img, PDF_NAME(ColorSpace), PDF_NAME(DeviceRGB));
    pdf_dict_put_int(ctx, img, PDF_NAME(BitsPerComponent), 8);
    return img;
}

pdf_obj* pageXObjects(fz_context* ctx, pdf_page* pp) {
    pdf_obj* res = pdf_page_resources(ctx, pp);
    pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    if (!xo) xo = pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 4);
    return xo;
}

void setContents(fz_context* ctx, pdf_document* pdoc, pdf_obj* pageObj, const std::string& text) {
    fz_buffer* buf = fz_new_buffer_from_copied_data(
        ctx, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    pdf_obj* stream = pdf_add_stream(ctx, pdoc, buf, nullptr, 0);
    fz_drop_buffer(ctx, buf);
    pdf_dict_put(ctx, pageObj, PDF_NAME(Contents), stream);
    pdf_drop_obj(ctx, stream);
}

// "a b c d e f cm" for a placement given in the RENDERED frame of page `pp`
// (unit square -> rendered page), converted to user space.
std::string cmForRendered(fz_context* ctx, pdf_page* pp, const fz_matrix& rendered) {
    fz_rect box;
    fz_matrix t;
    pdf_page_transform(ctx, pp, &box, &t);
    const fz_matrix user = fz_concat(rendered, fz_invert_matrix(t));
    std::ostringstream os;
    os.precision(10);
    os << user.a << ' ' << user.b << ' ' << user.c << ' ' << user.d << ' ' << user.e << ' '
       << user.f << " cm";
    return os.str();
}

using RawEdit = std::function<void(fz_context*, pdf_document*, pdf_page*)>;

} // namespace

// ---------------------------------------------------------------------------
// Shared fixture: test_single.pdf, edited raw, reopened through Document.
// ---------------------------------------------------------------------------
class PlacementDocBase : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApp();
        ASSERT_TRUE(tmp_.isValid());
        base_ = fixturePath("test_single.pdf");
        ASSERT_FALSE(base_.empty()) << "fixture test_single.pdf not found";
        context_ = std::make_shared<MuPdfContext>();
        doc_ = Document::open(context_, base_);
        ASSERT_NE(doc_, nullptr);
        embedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        writer_ = std::make_unique<pdfe::tre::PdfContentWriter>(context_.get(), embedder_.get());
    }
    void TearDown() override {
        page_.reset();
        writer_.reset();
        embedder_.reset();
        doc_.reset();
    }

    std::string path(const std::string& leaf) { return tmp_.filePath(QString::fromStdString(leaf)).toStdString(); }

    // save doc_ -> raw edit of page 0 -> reopen as doc_.
    void edit(const RawEdit& fn) {
        const std::string in = path("in" + std::to_string(++step_) + ".pdf");
        const std::string out = path("out" + std::to_string(step_) + ".pdf");
        page_.reset();
        doc_->saveAs(in);
        bool ok = false;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdoc = pdf_open_document(ctx, in.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdoc, 0);
            fn(ctx, pdoc, pp);
            pdf_write_options wo = pdf_default_write_options;
            pdf_save_document(ctx, pdoc, out.c_str(), &wo);
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdoc);
            ok = true;
        });
        ASSERT_TRUE(ok);
        doc_ = Document::open(context_, out);
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);
    }

    Page* page() {
        page_ = doc_->page(0);
        return page_.get();
    }
    QImage render(int index = 0) {
        page_.reset();
        auto p = doc_->page(index);
        QImage img = p->render(QRectF(), 1.0f);
        page_ = doc_->page(0);
        return img;
    }

    // Raw page-0 content of the saved doc_ (all streams).
    std::string savedContent(int pageIndex = 0) {
        const std::string p = path("content" + std::to_string(++step_) + ".pdf");
        page_.reset();
        doc_->saveAs(p);
        std::string out;
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdoc = pdf_open_document(ctx, p.c_str());
            pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdoc, pageIndex);
            pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));
            const int n = pdf_is_array(ctx, contents) ? pdf_array_len(ctx, contents) : 1;
            for (int i = 0; i < n; ++i) {
                pdf_obj* s = pdf_is_array(ctx, contents) ? pdf_array_get(ctx, contents, i) : contents;
                fz_buffer* b = pdf_load_stream(ctx, s);
                unsigned char* d = nullptr;
                const size_t len = fz_buffer_storage(ctx, b, &d);
                out.append(reinterpret_cast<const char*>(d), len);
                out.push_back('\n');
                fz_drop_buffer(ctx, b);
            }
            pdf_drop_document(ctx, pdoc);
        });
        page_ = doc_->page(0);
        return out;
    }

    // Read something from page 0 of doc_ as saved now.
    template <class F>
    void inspect(F fn) {
        const std::string p = path("inspect" + std::to_string(++step_) + ".pdf");
        page_.reset();
        doc_->saveAs(p);
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdoc = pdf_open_document(ctx, p.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdoc, 0);
            fn(ctx, pp);
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdoc);
        });
        page_ = doc_->page(0);
    }

    QTemporaryDir tmp_;
    std::string base_;
    int step_ = 0;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> embedder_;
    std::unique_ptr<pdfe::tre::PdfContentWriter> writer_;
};

// ===========================================================================
// TreReplaceModelTest
// ===========================================================================
class TreReplaceModelTest : public PlacementDocBase {
protected:
    // A 64x64 red image /ImR drawn by `content` (which replaces the page's).
    void drawRed(const std::function<std::string(fz_context*, pdf_page*)>& content) {
        edit([&](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
            pdf_obj* img = addRawRgbImage(ctx, pdoc, 64, 64, [](int, int, unsigned char* p) {
                p[0] = 255; p[1] = 0; p[2] = 0;
            });
            pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImR", img);
            pdf_drop_obj(ctx, img);
            setContents(ctx, pdoc, pp->obj, content(ctx, pp));
        });
    }

    // replaceWithFontMatch painting `sel` (rendered frame) magenta.
    void magentaEdit(const QRectF& sel) {
        using namespace pdfe::tre;
        auto ocr = std::make_unique<OcrPipeline>(context_.get(), doc_.get());
        auto coord = std::make_unique<CoordMapper>(context_.get(), doc_.get());
        auto emb = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        auto writer = std::make_unique<PdfContentWriter>(context_.get(), emb.get());
        auto cache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
        auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(analyzer.get(), cache.get());
        auto matcher = std::make_unique<FontMatcher>(selector.get(), analyzer.get());
        auto bgd = std::make_unique<BgColorDetector>();
        auto masker = std::make_unique<ImageMasker>();
        auto compositor = std::make_unique<BitmapCompositor>();
        ImageReplacer replacer(ocr.get(), coord.get(), bgd.get(), matcher.get(), masker.get(),
                               compositor.get(), writer.get());
        FontMatch fm;
        fm.fontFamily = "Arial";
        fm.fontStyle = "Regular";
        fm.fontFilePath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
        ASSERT_FALSE(fm.fontFilePath.empty()) << "no Arial-equivalent font";
        fm.sizePt = 12.0f;
        fm.color = RgbColor{255, 0, 255};
        fm.confidence = 0.8f;
        BackgroundInfo bg;
        bg.dominantColor = RgbColor{255, 0, 255};
        bg.isUniform = true;
        bg.confidence = 0.9f;
        PdfRect pr;
        pr.x0 = sel.left(); pr.y0 = sel.top(); pr.x1 = sel.right(); pr.y1 = sel.bottom();
        const ReconstructionResult r = replacer.replaceWithFontMatch(
            page(), doc_.get(), 0, pr, "WWWWWWWW", fm, bg, RegionId{3}, {}, "WWWWWWWW");
        ASSERT_TRUE(r.success) << r.errorMessage;
    }

    // Edited pixels only inside `sel` (+margin); red outside it unchanged.
    void expectEditOnlyInside(const QImage& before, const QImage& after, const QRectF& sel,
                              double margin, const char* what) {
        const QRectF allowed = sel.adjusted(-margin, -margin, margin, margin);
        int inside = 0, outside = 0, redKept = 0, redBefore = 0;
        for (int y = 0; y < after.height(); ++y)
            for (int x = 0; x < after.width(); ++x) {
                const QPointF c(x + 0.5, y + 0.5);
                if (isMagenta(after.pixel(x, y))) (allowed.contains(c) ? inside : outside)++;
                if (!allowed.adjusted(-2, -2, 2, 2).contains(c) && isRed(before.pixel(x, y))) {
                    ++redBefore;
                    if (isRed(after.pixel(x, y))) ++redKept;
                }
            }
        EXPECT_GT(inside, int(sel.width() * sel.height() * 0.5)) << what << ": too little was edited inside the rect";
        EXPECT_EQ(outside, 0) << what << ": edited pixels landed outside the edit rect";
        ASSERT_GT(redBefore, 0);
        EXPECT_GE(double(redKept) / redBefore, 0.99) << what << ": the image outside the rect changed";
    }
};

// GitHub #443: MediaBox origin and /Rotate 270 together; placementFitz is in the
// rendered frame, the same as ImageManager's and the render's.
TEST_F(TreReplaceModelTest, PlacementFrameFollowsMediaBoxOriginAndRotate270Together) {
    drawRed([](fz_context* ctx, pdf_page* pp) {
        pdf_dict_put_rect(ctx, pp->obj, PDF_NAME(MediaBox), fz_make_rect(30, 40, 30 + 595, 40 + 842));
        pdf_dict_put_int(ctx, pp->obj, PDF_NAME(Rotate), 270);
        return std::string("q 200 0 0 120 130 340 cm /ImR Do Q\n");
    });
    const QImage shown = render();
    int n = 0;
    QRect red;
    {
        int x0 = shown.width(), y0 = shown.height(), x1 = -1, y1 = -1;
        for (int y = 0; y < shown.height(); ++y)
            for (int x = 0; x < shown.width(); ++x)
                if (isRed(shown.pixel(x, y))) {
                    ++n; x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
        red = QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    }
    ASSERT_GT(n, 0);
    ASSERT_NEAR(red.width(), 120, 3) << "oracle: /Rotate 270 swaps the extent";
    pdfe::image::ImageManager manager(context_);
    const auto imgs = manager.enumerateImages(page(), doc_.get());
    ASSERT_EQ(imgs.size(), 1u);
    const auto got = writer_->findImageXObjectForRegion(page(), doc_.get(), QRectF(red).adjusted(5, 5, -5, -5));
    ASSERT_TRUE(got.has_value());
    // ImageManager's boundingBox is the upright rect in the image's own frame
    // (rotate it by -rotationRadians for the outline); placementFitz is the
    // axis-aligned extent on the page. Both are in the rendered frame, so they
    // share the centre.
    EXPECT_NEAR(got->placementFitz.center().x(), imgs[0].boundingBox.center().x(), 1.0);
    EXPECT_NEAR(got->placementFitz.center().y(), imgs[0].boundingBox.center().y(), 1.0);
    EXPECT_NEAR(got->placementFitz.x(), red.x(), 2.0);
    EXPECT_NEAR(got->placementFitz.y(), red.y(), 2.0);
    EXPECT_NEAR(got->placementFitz.width(), red.width(), 2.0);
    EXPECT_NEAR(got->placementFitz.height(), red.height(), 2.0);
    EXPECT_EQ(got->occurrence, 0);
    EXPECT_FALSE(got->inFormXObject);
    EXPECT_TRUE(got->hasPageCtm);
}

// GitHub #444: a quarter turn with a crop (clip inside the cm).
TEST_F(TreReplaceModelTest, WriteBackOnARotatedAndCroppedImageLandsInTheRect) {
    drawRed([](fz_context* ctx, pdf_page* pp) {
        // Rendered: image x axis runs DOWN the page (90 degrees clockwise).
        const fz_matrix r = fz_make_matrix(0, 200, 120, 0, 150, 200);
        return "q " + cmForRendered(ctx, pp, r) + " 0.1 0.1 0.8 0.8 re W n /ImR Do Q\n";
    });
    const QImage before = render();
    const QRectF sel(190, 260, 40, 30);
    ASSERT_TRUE(isRed(before.pixel(int(sel.center().x()), int(sel.center().y()))));
    magentaEdit(sel);
    expectEditOnlyInside(before, render(), sel, 6.0, "rotated 90 + cropped");
}

// GitHub #444: a horizontal mirror.
TEST_F(TreReplaceModelTest, WriteBackOnAMirroredImageLandsInTheRect) {
    drawRed([](fz_context* ctx, pdf_page* pp) {
        const fz_matrix r = fz_make_matrix(-200, 0, 0, -120, 350, 320);
        return "q " + cmForRendered(ctx, pp, r) + " /ImR Do Q\n";
    });
    const QImage before = render();
    const QRectF sel(160, 210, 50, 30);
    ASSERT_TRUE(isRed(before.pixel(int(sel.center().x()), int(sel.center().y()))));
    magentaEdit(sel);
    expectEditOnlyInside(before, render(), sel, 6.0, "mirrored");
}

// GitHub #444: a 270-degree turn combined with a flip.
TEST_F(TreReplaceModelTest, WriteBackOnARotatedAndFlippedImageLandsInTheRect) {
    drawRed([](fz_context* ctx, pdf_page* pp) {
        const fz_matrix r = fz_make_matrix(0, -200, -120, 0, 270, 400);
        return "q " + cmForRendered(ctx, pp, r) + " /ImR Do Q\n";
    });
    const QImage before = render();
    const QRectF sel(200, 330, 40, 40);
    ASSERT_TRUE(isRed(before.pixel(int(sel.center().x()), int(sel.center().y()))));
    magentaEdit(sel);
    expectEditOnlyInside(before, render(), sel, 6.0, "rotated 270 + flipped");
}

// GitHub #444: an oblique (30 degree) placement goes through the matrix too.
TEST_F(TreReplaceModelTest, WriteBackOnAnObliqueImageLandsInTheRect) {
    drawRed([](fz_context* ctx, pdf_page* pp) {
        const double a = 30.0 * M_PI / 180.0;
        const fz_matrix r = fz_make_matrix(float(240 * std::cos(a)), float(240 * std::sin(a)),
                                           float(160 * std::sin(a)), float(-160 * std::cos(a)),
                                           200, 300);
        return "q " + cmForRendered(ctx, pp, r) + " /ImR Do Q\n";
    });
    const QImage before = render();
    const QRectF sel(250, 300, 40, 30);
    ASSERT_TRUE(isRed(before.pixel(int(sel.center().x()), int(sel.center().y()))));
    magentaEdit(sel);
    expectEditOnlyInside(before, render(), sel, 6.0, "oblique 30 degrees");
}

// GitHub #445: SMask, an ICCBased RGB colour space, /OC, /Intent and
// /Interpolate are kept; /Decode is not copied (the pixels are decoded).
TEST_F(TreReplaceModelTest, ReplaceKeepsSoftMaskIccOptionalContentAndRenderingIntent) {
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        pdf_obj* img = addRawRgbImage(ctx, pdoc, 32, 32, [](int, int, unsigned char* p) {
            p[0] = 255; p[1] = 0; p[2] = 0;
        });
        std::vector<unsigned char> alpha(32 * 32);
        for (int i = 0; i < 32 * 32; ++i) alpha[static_cast<size_t>(i)] = (i % 32) < 16 ? 0 : 255;
        fz_buffer* mb = fz_new_buffer_from_copied_data(ctx, alpha.data(), alpha.size());
        pdf_obj* sm = pdf_add_stream(ctx, pdoc, mb, nullptr, 0);
        fz_drop_buffer(ctx, mb);
        pdf_dict_put(ctx, sm, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, sm, PDF_NAME(Subtype), PDF_NAME(Image));
        pdf_dict_put_int(ctx, sm, PDF_NAME(Width), 32);
        pdf_dict_put_int(ctx, sm, PDF_NAME(Height), 32);
        pdf_dict_put(ctx, sm, PDF_NAME(ColorSpace), PDF_NAME(DeviceGray));
        pdf_dict_put_int(ctx, sm, PDF_NAME(BitsPerComponent), 8);
        pdf_dict_put(ctx, img, PDF_NAME(SMask), sm);
        // ICCBased, N 3 (this build draws it as DeviceRGB; the profile is kept).
        const char prof[] = "not-a-real-profile";
        fz_buffer* pb = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(prof), sizeof(prof));
        pdf_obj* icc = pdf_add_stream(ctx, pdoc, pb, nullptr, 0);
        fz_drop_buffer(ctx, pb);
        pdf_dict_put_int(ctx, icc, PDF_NAME(N), 3);
        pdf_dict_put(ctx, icc, PDF_NAME(Alternate), PDF_NAME(DeviceRGB));
        pdf_obj* cs = pdf_new_array(ctx, pdoc, 2);
        pdf_array_push(ctx, cs, PDF_NAME(ICCBased));
        pdf_array_push(ctx, cs, icc);
        pdf_dict_put(ctx, img, PDF_NAME(ColorSpace), cs);
        // A visible OC group.
        pdf_obj* ocg = pdf_add_new_dict(ctx, pdoc, 2);
        pdf_dict_put(ctx, ocg, PDF_NAME(Type), PDF_NAME(OCG));
        pdf_dict_put_text_string(ctx, ocg, PDF_NAME(Name), "Layer");
        pdf_dict_put(ctx, img, PDF_NAME(OC), ocg);
        pdf_dict_put(ctx, img, PDF_NAME(Intent), PDF_NAME(Perceptual));
        pdf_dict_put_bool(ctx, img, PDF_NAME(Interpolate), 1);
        pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImS", img);
        pdf_drop_obj(ctx, ocg);
        pdf_drop_obj(ctx, cs);
        pdf_drop_obj(ctx, icc);
        pdf_drop_obj(ctx, sm);
        pdf_drop_obj(ctx, img);
        setContents(ctx, pdoc, pp->obj, "q 200 0 0 120 150 400 cm /ImS Do Q\n");
    });
    const QImage before = render();
    const auto target = writer_->findImageXObjectForRegion(page(), doc_.get(), QRectF(160, 330, 180, 100));
    ASSERT_TRUE(target.has_value());
    QImage same(32, 32, QImage::Format_RGB888);
    same.fill(QColor(255, 0, 0));
    ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), same, *target));

    bool smask = false, icc3 = false, oc = false, intent = false, interp = false, decode = true;
    inspect([&](fz_context* ctx, pdf_page* pp) {
        pdf_obj* img = pdf_dict_gets(ctx, pageXObjects(ctx, pp), "ImS");
        smask = pdf_is_stream(ctx, pdf_dict_get(ctx, img, PDF_NAME(SMask)));
        pdf_obj* cs = pdf_dict_get(ctx, img, PDF_NAME(ColorSpace));
        icc3 = pdf_is_array(ctx, cs) && pdf_name_eq(ctx, pdf_array_get(ctx, cs, 0), PDF_NAME(ICCBased)) &&
               pdf_dict_get_int(ctx, pdf_array_get(ctx, cs, 1), PDF_NAME(N)) == 3;
        oc = pdf_is_dict(ctx, pdf_dict_get(ctx, img, PDF_NAME(OC)));
        intent = pdf_name_eq(ctx, pdf_dict_get(ctx, img, PDF_NAME(Intent)), PDF_NAME(Perceptual));
        interp = pdf_to_bool(ctx, pdf_dict_get(ctx, img, PDF_NAME(Interpolate)));
        decode = pdf_dict_get(ctx, img, PDF_NAME(Decode)) != nullptr;
    });
    EXPECT_TRUE(smask);
    EXPECT_TRUE(icc3) << "the ICCBased RGB colour space was dropped";
    EXPECT_TRUE(oc);
    EXPECT_TRUE(intent);
    EXPECT_TRUE(interp);
    EXPECT_FALSE(decode);
    const QImage after = render();
    EXPECT_TRUE(isBackground(after.pixel(190, 380))) << "the masked half is painted";
    EXPECT_TRUE(isRed(after.pixel(300, 380)));
    EXPECT_EQ(before.pixel(190, 380), after.pixel(190, 380));
}

// GitHub #445: a stencil /Mask stream and a colour-key /Mask array are kept;
// a /Decode-inverted image replaced with its own decoded pixels looks the same.
TEST_F(TreReplaceModelTest, ReplaceKeepsStencilAndColourKeyMasksAndHonoursDecode) {
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        // ImK: blue where x < 8 (colour-keyed out), red elsewhere.
        pdf_obj* key = addRawRgbImage(ctx, pdoc, 16, 16, [](int x, int, unsigned char* p) {
            p[0] = x < 8 ? 0 : 255; p[1] = 0; p[2] = x < 8 ? 255 : 0;
        });
        pdf_obj* range = pdf_new_array(ctx, pdoc, 6);
        for (int v : {0, 0, 0, 0, 255, 255}) pdf_array_push_int(ctx, range, v);
        pdf_dict_put(ctx, key, PDF_NAME(Mask), range);
        // ImD: stored cyan, /Decode inverts it to red.
        pdf_obj* dec = addRawRgbImage(ctx, pdoc, 16, 16, [](int, int, unsigned char* p) {
            p[0] = 0; p[1] = 255; p[2] = 255;
        });
        pdf_obj* darr = pdf_new_array(ctx, pdoc, 6);
        for (int v : {1, 0, 1, 0, 1, 0}) pdf_array_push_int(ctx, darr, v);
        pdf_dict_put(ctx, dec, PDF_NAME(Decode), darr);
        // ImM: red, with a stencil /Mask hiding one half.
        pdf_obj* stencilImg = addRawRgbImage(ctx, pdoc, 16, 16, [](int, int, unsigned char* p) {
            p[0] = 255; p[1] = 0; p[2] = 0;
        });
        std::vector<unsigned char> bits(16 * 2);
        for (int row = 0; row < 16; ++row) {
            const unsigned char v = row < 8 ? 0xFF : 0x00;
            bits[static_cast<size_t>(row * 2)] = v;
            bits[static_cast<size_t>(row * 2 + 1)] = v;
        }
        fz_buffer* sb = fz_new_buffer_from_copied_data(ctx, bits.data(), bits.size());
        pdf_obj* stencil = pdf_add_stream(ctx, pdoc, sb, nullptr, 0);
        fz_drop_buffer(ctx, sb);
        pdf_dict_put(ctx, stencil, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, stencil, PDF_NAME(Subtype), PDF_NAME(Image));
        pdf_dict_put_int(ctx, stencil, PDF_NAME(Width), 16);
        pdf_dict_put_int(ctx, stencil, PDF_NAME(Height), 16);
        pdf_dict_put_bool(ctx, stencil, PDF_NAME(ImageMask), 1);
        pdf_dict_put_int(ctx, stencil, PDF_NAME(BitsPerComponent), 1);
        pdf_dict_put(ctx, stencilImg, PDF_NAME(Mask), stencil);
        pdf_obj* xo = pageXObjects(ctx, pp);
        pdf_dict_puts(ctx, xo, "ImK", key);
        pdf_dict_puts(ctx, xo, "ImD", dec);
        pdf_dict_puts(ctx, xo, "ImM", stencilImg);
        for (pdf_obj* o : {key, range, dec, darr, stencilImg, stencil}) pdf_drop_obj(ctx, o);
        setContents(ctx, pdoc, pp->obj,
                    "q 160 0 0 160 50 600 cm /ImK Do Q\n"
                    "q 160 0 0 160 250 600 cm /ImD Do Q\n"
                    "q 160 0 0 160 50 300 cm /ImM Do Q\n");
    });
    const QImage before = render();
    // Oracles: keyed half transparent, decoded image red, stencil top hidden.
    ASSERT_TRUE(isBackground(before.pixel(80, 160)));
    ASSERT_TRUE(isRed(before.pixel(180, 160)));
    ASSERT_TRUE(isRed(before.pixel(330, 160)));
    // The stencil hides one half of ImM (mask rows 0-7 against 8-15).
    const bool topShown = isRed(before.pixel(130, 420));
    const bool bottomShown = isRed(before.pixel(130, 520));
    ASSERT_NE(topShown, bottomShown) << "oracle: the stencil hides exactly one half";
    ASSERT_TRUE(isBackground(before.pixel(130, topShown ? 520 : 420)));

    for (const QRectF& region : {QRectF(60, 90, 140, 140), QRectF(260, 90, 140, 140), QRectF(60, 390, 140, 140)}) {
        const auto t = writer_->findImageXObjectForRegion(page(), doc_.get(), region);
        ASSERT_TRUE(t.has_value());
        const QImage own = writer_->getOriginalImage(page(), doc_.get(), *t);  // decoded pixels
        ASSERT_FALSE(own.isNull());
        ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), own, *t)) << t->xobjectName;
    }
    const QImage after = render();
    EXPECT_TRUE(isBackground(after.pixel(80, 160))) << "the colour-key /Mask was dropped";
    EXPECT_TRUE(isRed(after.pixel(180, 160)));
    EXPECT_TRUE(isRed(after.pixel(330, 160))) << "the decoded colours were re-inverted";
    EXPECT_TRUE(isBackground(after.pixel(130, topShown ? 520 : 420))) << "the stencil /Mask was dropped";
    EXPECT_TRUE(isRed(after.pixel(130, topShown ? 420 : 520)));
}

// GitHub #446: the sole placement is replaced in place, under its own name;
// the page content is not touched.
TEST_F(TreReplaceModelTest, SolePlacementIsReplacedUnderItsOwnNameWithoutTouchingTheContent) {
    drawRed([](fz_context*, pdf_page*) { return std::string("q 200 0 0 120 150 400 cm /ImR Do Q\n"); });
    const std::string contentBefore = savedContent();
    const auto t = writer_->findImageXObjectForRegion(page(), doc_.get(), QRectF(160, 330, 180, 100));
    ASSERT_TRUE(t.has_value());
    QImage blue(64, 64, QImage::Format_RGB888);
    blue.fill(QColor(0, 0, 255));
    ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), blue, *t));
    EXPECT_EQ(savedContent(), contentBefore);
    EXPECT_TRUE(isBlue(render().pixel(250, 380)));
}

// GitHub #446: a /XObject dictionary shared with another page is not edited
// under the shared name; the other page keeps the original pixels.
TEST_F(TreReplaceModelTest, AnImageInADictSharedWithAnotherPageChangesOnlyOnThisPage) {
    drawRed([](fz_context*, pdf_page*) { return std::string("q 200 0 0 120 150 400 cm /ImR Do Q\n"); });
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        pdf_obj* res = pdf_dict_get(ctx, pp->obj, PDF_NAME(Resources));
        const std::string text = "q 200 0 0 120 150 400 cm /ImR Do Q\n";
        fz_buffer* buf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(text.data()), text.size());
        pdf_obj* np = pdf_add_page(ctx, pdoc, fz_make_rect(0, 0, 595.28f, 841.89f), 0, res, buf);
        fz_drop_buffer(ctx, buf);
        pdf_insert_page(ctx, pdoc, -1, np);
        pdf_drop_obj(ctx, np);
    });
    ASSERT_EQ(doc_->pageCount(), 2);
    ASSERT_TRUE(isRed(render(1).pixel(250, 380))) << "precondition: page 2 draws the image";
    const auto t = writer_->findImageXObjectForRegion(page(), doc_.get(), QRectF(160, 330, 180, 100));
    ASSERT_TRUE(t.has_value());
    QImage blue(64, 64, QImage::Format_RGB888);
    blue.fill(QColor(0, 0, 255));
    ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), blue, *t));
    EXPECT_TRUE(isBlue(render(0).pixel(250, 380)));
    EXPECT_TRUE(isRed(render(1).pixel(250, 380))) << "the other page sharing the dict changed too";
}

// GitHub #446: the repoint is undone by restoring the page content snapshot.
TEST_F(TreReplaceModelTest, RepointedPlacementIsUndoneByTheContentSnapshot) {
    drawRed([](fz_context*, pdf_page*) {
        return std::string("q 200 0 0 120 150 400 cm /ImR Do Q\nq 200 0 0 120 150 100 cm /ImR Do Q\n");
    });
    void* snapshot = writer_->saveContentsRef(page(), doc_.get());
    ASSERT_NE(snapshot, nullptr);
    const auto t = writer_->findImageXObjectForRegion(page(), doc_.get(), QRectF(160, 330, 180, 100));
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->occurrence, 0);
    QImage blue(64, 64, QImage::Format_RGB888);
    blue.fill(QColor(0, 0, 255));
    ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), blue, *t));
    QImage after = render();
    EXPECT_TRUE(isBlue(after.pixel(250, 380)));
    EXPECT_TRUE(isRed(after.pixel(250, 680))) << "the other copy changed";
    ASSERT_TRUE(writer_->restoreContentsRef(page(), doc_.get(), snapshot));
    after = render();
    EXPECT_TRUE(isRed(after.pixel(250, 380))) << "undo did not bring the original back";
    EXPECT_TRUE(isRed(after.pixel(250, 680)));
    writer_->releaseContentsRef(snapshot);
}

// The phase-2 rule: an image inside a Form XObject is reported, never written.
TEST_F(TreReplaceModelTest, AnImageInsideAFormIsReportedButNotWritten) {
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        pdf_obj* img = addRawRgbImage(ctx, pdoc, 16, 16, [](int, int, unsigned char* p) {
            p[0] = 255; p[1] = 0; p[2] = 0;
        });
        const std::string formText = "q 200 0 0 120 0 0 cm /ImF Do Q\n";
        fz_buffer* fb = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(formText.data()), formText.size());
        pdf_obj* form = pdf_add_stream(ctx, pdoc, fb, nullptr, 0);
        fz_drop_buffer(ctx, fb);
        pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
        pdf_dict_put_rect(ctx, form, PDF_NAME(BBox), fz_make_rect(0, 0, 200, 120));
        pdf_obj* fres = pdf_dict_put_dict(ctx, form, PDF_NAME(Resources), 1);
        pdf_obj* fxo = pdf_dict_put_dict(ctx, fres, PDF_NAME(XObject), 1);
        pdf_dict_puts(ctx, fxo, "ImF", img);
        pdf_dict_puts(ctx, pageXObjects(ctx, pp), "Fm0", form);
        pdf_drop_obj(ctx, form);
        pdf_drop_obj(ctx, img);
        setContents(ctx, pdoc, pp->obj, "q 1 0 0 1 150 400 cm /Fm0 Do Q\n");
    });
    const QImage before = render();
    ASSERT_TRUE(isRed(before.pixel(250, 380)));
    const auto t = writer_->findImageXObjectForRegion(page(), doc_.get(), QRectF(160, 330, 180, 100));
    ASSERT_TRUE(t.has_value()) << "the form's image is what the region shows";
    EXPECT_TRUE(t->inFormXObject);
    EXPECT_EQ(t->occurrence, -1);
    QImage blue(16, 16, QImage::Format_RGB888);
    blue.fill(QColor(0, 0, 255));
    EXPECT_FALSE(writer_->replacePageImage(page(), doc_.get(), blue, *t));
    EXPECT_EQ(render(), before) << "a refused replace changed the page";
    const auto main = writer_->findPageImageXObject(page(), doc_.get());
    EXPECT_TRUE(!main.has_value() || !main->inFormXObject)
        << "the page's main image must be one of its own";
}

// GitHub #447 on a scan: the image covers a /Rotate 90 page. A picture of the
// page written back through the two-argument form lands the right way round.
TEST_F(TreReplaceModelTest, WholePagePictureOfARotatedScanIsWrittenTheRightWayRound) {
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        pdf_obj* img = addRawRgbImage(ctx, pdoc, 120, 170, [](int x, int, unsigned char* p) {
            p[0] = x < 60 ? 255 : 0; p[1] = 0; p[2] = x < 60 ? 0 : 255;
        });
        pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImP", img);
        pdf_drop_obj(ctx, img);
        pdf_dict_put_int(ctx, pp->obj, PDF_NAME(Rotate), 90);
        setContents(ctx, pdoc, pp->obj, "q 595.28 0 0 841.89 0 0 cm /ImP Do Q\n");
    });
    const QImage picture = render();
    ASSERT_GT(picture.width(), picture.height()) << "precondition: the rendered page is landscape";
    // Oracle: on the rendered page the image's left half (red) is at the top.
    ASSERT_TRUE(isRed(picture.pixel(picture.width() / 2, picture.height() / 4)));
    ASSERT_TRUE(isBlue(picture.pixel(picture.width() / 2, picture.height() * 3 / 4)));
    ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), picture));
    const QImage after = render();
    EXPECT_TRUE(isRed(after.pixel(after.width() / 2, after.height() / 4)))
        << "the picture was written into the image unrotated";
    EXPECT_TRUE(isBlue(after.pixel(after.width() / 2, after.height() * 3 / 4)));
}

// CONTROL for #447: an upright scan covering the page keeps the historical path
// (the picture is the image).
TEST_F(TreReplaceModelTest, WholePagePictureOfAnUprightScanIsTheImage) {
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        pdf_obj* img = addRawRgbImage(ctx, pdoc, 300, 424, [](int x, int, unsigned char* p) {
            p[0] = x < 150 ? 255 : 0; p[1] = 0; p[2] = x < 150 ? 0 : 255;
        });
        pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImP", img);
        pdf_drop_obj(ctx, img);
        setContents(ctx, pdoc, pp->obj, "q 595.28 0 0 841.89 0 0 cm /ImP Do Q\n");
    });
    QImage picture(300, 424, QImage::Format_RGB888);
    picture.fill(QColor(0, 0, 255));
    ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), picture));
    const QImage after = render();
    EXPECT_TRUE(isBlue(after.pixel(100, 400)));
    EXPECT_TRUE(isBlue(after.pixel(500, 400)));
    int w = 0, h = 0;
    inspect([&](fz_context* ctx, pdf_page* pp) {
        pdf_obj* img = pdf_dict_gets(ctx, pageXObjects(ctx, pp), "ImP");
        w = pdf_dict_get_int(ctx, img, PDF_NAME(Width));
        h = pdf_dict_get_int(ctx, img, PDF_NAME(Height));
    });
    EXPECT_EQ(w, 300);
    EXPECT_EQ(h, 424);
}

// ===========================================================================
// TaggerModelTest
// ===========================================================================
class TaggerModelTest : public PlacementDocBase {
protected:
    // Text, image, text, image (18 operators: 5 + 4 + 5 + 4).
    void interleavedPage() {
        edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
            pdf_obj* img = addRawRgbImage(ctx, pdoc, 16, 16, [](int, int, unsigned char* p) {
                p[0] = 255; p[1] = 0; p[2] = 0;
            });
            pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImT", img);
            pdf_drop_obj(ctx, img);
            setContents(ctx, pdoc, pp->obj,
                        "BT /F1 12 Tf 72 760 Td (First paragraph) Tj ET\n"
                        "q 100 0 0 60 72 600 cm /ImT Do Q\n"
                        "BT /F1 12 Tf 72 560 Td (Second paragraph) Tj ET\n"
                        "q 100 0 0 60 72 400 cm /ImT Do Q\n");
        });
    }

    static bool segment(const std::string& c, const char* tag, int mcid, std::string* out) {
        const std::regex bdc(std::string("/") + tag + "\\s*<<\\s*/MCID\\s+" + std::to_string(mcid) +
                             "\\s*>>\\s*BDC");
        std::smatch m;
        if (!std::regex_search(c, m, bdc)) return false;
        const size_t start = static_cast<size_t>(m.position(0) + m.length(0));
        const std::regex word("\\b(BDC|BMC|EMC)\\b");
        int depth = 1;
        const std::string rest = c.substr(start);
        for (std::sregex_iterator it(rest.begin(), rest.end(), word), end; it != end; ++it) {
            depth += ((*it)[1] == "EMC") ? -1 : 1;
            if (depth == 0) {
                *out = rest.substr(0, static_cast<size_t>(it->position(0)));
                return true;
            }
        }
        return false;
    }
    static int count(const std::string& c, const char* re) {
        const std::regex r(re);
        return static_cast<int>(std::distance(std::sregex_iterator(c.begin(), c.end(), r),
                                              std::sregex_iterator()));
    }
    static bool hasDo(const std::string& s) { return count(s, "(^|\\s)Do(?=\\s|$)") > 0; }
    static void expectBalanced(const std::string& c) {
        EXPECT_EQ(count(c, "\\b(BDC|BMC)\\b"), count(c, "\\bEMC\\b")) << c;
        EXPECT_EQ(count(c, "(^|\\s)q(?=\\s|$)"), count(c, "(^|\\s)Q(?=\\s|$)")) << c;
    }

    void tag(const std::vector<pdfe::compliance::MarkedContentRegion>& regions) {
        page_.reset();
        pdfe::compliance::ContentStreamTagger tagger(context_);
        tagger.tagPageContent(doc_.get(), 0, regions);
        page_ = doc_->page(0);
    }
};

using pdfe::compliance::MarkedContentRegion;
using pdfe::compliance::StructureTag;

// Explicit operator ranges: each region encloses exactly its own operators.
TEST_F(TaggerModelTest, ExplicitRangesWrapEachRegionInItsOwnSequence) {
    interleavedPage();
    tag({{StructureTag::P, 0, 0, 5}, {StructureTag::Figure, 1, 5, 9},
         {StructureTag::P, 2, 9, 14}, {StructureTag::Figure, 3, 14, 18}});
    const std::string c = savedContent();
    std::string p0, f1, p2, f3;
    ASSERT_TRUE(segment(c, "P", 0, &p0));
    ASSERT_TRUE(segment(c, "Figure", 1, &f1));
    ASSERT_TRUE(segment(c, "P", 2, &p2));
    ASSERT_TRUE(segment(c, "Figure", 3, &f3));
    EXPECT_NE(p0.find("First paragraph"), std::string::npos);
    EXPECT_EQ(p0.find("Second"), std::string::npos);
    EXPECT_FALSE(hasDo(p0));
    EXPECT_TRUE(hasDo(f1));
    EXPECT_NE(f1.find("72 600 cm"), std::string::npos);
    EXPECT_NE(p2.find("Second paragraph"), std::string::npos);
    EXPECT_FALSE(hasDo(p2));
    EXPECT_TRUE(hasDo(f3));
    EXPECT_NE(f3.find("72 400 cm"), std::string::npos);
    expectBalanced(c);
    // Content order is kept: P0 < F1 < P2 < F3.
    EXPECT_LT(c.find("/MCID 0"), c.find("/MCID 1"));
    EXPECT_LT(c.find("/MCID 1"), c.find("/MCID 2"));
    EXPECT_LT(c.find("/MCID 2"), c.find("/MCID 3"));
}

// No ranges (what the production callers pass): each image goes to the next
// Figure, the text to the other regions, in content order.
TEST_F(TaggerModelTest, WithoutRangesEachFigureGetsItsImageAndTextNeverEnclosesOne) {
    interleavedPage();
    tag({{StructureTag::H1, 0, 0, 0}, {StructureTag::Figure, 1, 0, 0},
         {StructureTag::P, 2, 0, 0}, {StructureTag::Figure, 3, 0, 0}});
    const std::string c = savedContent();
    std::string h0, f1, p2, f3;
    ASSERT_TRUE(segment(c, "H1", 0, &h0));
    ASSERT_TRUE(segment(c, "Figure", 1, &f1));
    ASSERT_TRUE(segment(c, "P", 2, &p2));
    ASSERT_TRUE(segment(c, "Figure", 3, &f3));
    EXPECT_NE(h0.find("First paragraph"), std::string::npos) << c;
    EXPECT_NE(p2.find("Second paragraph"), std::string::npos) << c;
    EXPECT_FALSE(hasDo(h0));
    EXPECT_FALSE(hasDo(p2));
    EXPECT_NE(f1.find("72 600 cm"), std::string::npos) << c;
    EXPECT_NE(f3.find("72 400 cm"), std::string::npos) << c;
    expectBalanced(c);
}

// The production pipeline: AutoTagger regions -> tagPageContent.
TEST_F(TaggerModelTest, AutoTaggerRegionsPointFiguresAtTheImages) {
    interleavedPage();
    pdfe::compliance::AutoTagger autoTagger(context_);
    page_.reset();
    const auto root = autoTagger.tagDocument(doc_.get());
    std::vector<MarkedContentRegion> regions;
    int figures = 0;
    for (const auto& child : root.children) {
        if (child->pageIndex != 0 || child->tag == StructureTag::Artifact) continue;
        MarkedContentRegion r;
        r.tag = child->tag;
        r.mcid = child->mcid;
        regions.push_back(r);
        if (r.tag == StructureTag::Figure) ++figures;
    }
    ASSERT_EQ(figures, 2) << "precondition: AutoTagger finds both images";
    tag(regions);
    const std::string c = savedContent();
    for (const auto& r : regions) {
        std::string seg;
        ASSERT_TRUE(segment(c, pdfe::compliance::structureTagToName(r.tag), r.mcid, &seg)) << c;
        if (r.tag == StructureTag::Figure) {
            EXPECT_TRUE(hasDo(seg)) << "Figure MCID " << r.mcid << " encloses no image:\n" << c;
        } else {
            EXPECT_FALSE(hasDo(seg)) << "MCID " << r.mcid << " encloses an image:\n" << c;
        }
    }
    expectBalanced(c);
}

// Content already inside a marked-content sequence is left as it is.
TEST_F(TaggerModelTest, ExistingMarkedContentIsKeptAndNotNestedInto) {
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        pdf_obj* img = addRawRgbImage(ctx, pdoc, 16, 16, [](int, int, unsigned char* p) {
            p[0] = 255; p[1] = 0; p[2] = 0;
        });
        pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImT", img);
        pdf_drop_obj(ctx, img);
        setContents(ctx, pdoc, pp->obj,
                    "/Span <</MCID 9>> BDC BT /F1 12 Tf 72 760 Td (Kept) Tj ET EMC\n"
                    "q 100 0 0 60 72 600 cm /ImT Do Q\n");
    });
    tag({{StructureTag::P, 0, 0, 0}, {StructureTag::Figure, 1, 0, 0}});
    const std::string c = savedContent();
    std::string span, fig;
    ASSERT_TRUE(segment(c, "Span", 9, &span)) << c;
    EXPECT_NE(span.find("Kept"), std::string::npos);
    EXPECT_EQ(span.find("MCID"), std::string::npos) << "a new sequence was nested into an existing one";
    ASSERT_TRUE(segment(c, "Figure", 1, &fig));
    EXPECT_TRUE(hasDo(fig));
    expectBalanced(c);
}

// A range that would cut a q/Q pair is not written; the stream stays valid.
TEST_F(TaggerModelTest, ARangeThatCutsANestedPairIsNotWritten) {
    interleavedPage();
    tag({{StructureTag::Figure, 1, 5, 7}});  // "q" and "cm" without their Q
    const std::string c = savedContent();
    std::string fig;
    ASSERT_TRUE(segment(c, "Figure", 1, &fig));
    EXPECT_FALSE(hasDo(fig));
    // Not written: the sequence is empty (a sequence holding "q ... cm" without
    // its Q would cross the q/Q nesting).
    EXPECT_EQ(fig.find_first_not_of(" \t\r\n"), std::string::npos) << "[" << fig << "]";
    expectBalanced(c);
    // The page still has all its operators, in order.
    EXPECT_LT(c.find("First paragraph"), c.find("72 600 cm"));
    EXPECT_LT(c.find("72 600 cm"), c.find("Second paragraph"));
}

// Tagging changes no pixel.
TEST_F(TaggerModelTest, TaggingKeepsTheRendering) {
    interleavedPage();
    const QImage before = render();
    tag({{StructureTag::P, 0, 0, 0}, {StructureTag::Figure, 1, 0, 0},
         {StructureTag::P, 2, 0, 0}, {StructureTag::Figure, 3, 0, 0}});
    EXPECT_EQ(render(), before);
}

// An existing structure tree and its marked content survive tagPageContent.
TEST_F(TaggerModelTest, ExistingStructureTreeIsPreserved) {
    const std::string tagged = fixturePath("test_tagged_rolemap.pdf");
    ASSERT_FALSE(tagged.empty()) << "fixture test_tagged_rolemap.pdf not found";
    page_.reset();
    doc_ = Document::open(context_, tagged);
    ASSERT_NE(doc_, nullptr);
    const std::string before = savedContent();
    const int mcidsBefore = count(before, "/MCID\\s+\\d+");
    ASSERT_GT(mcidsBefore, 0) << "precondition: the fixture is tagged";
    int rootBefore = 0, rootAfter = 0;
    inspect([&](fz_context* ctx, pdf_page* pp) {
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, pp->doc), PDF_NAME(Root));
        rootBefore = pdf_to_num(ctx, pdf_dict_get(ctx, root, PDF_NAME(StructTreeRoot)));
    });
    ASSERT_GT(rootBefore, 0);
    tag({{StructureTag::P, 50, 0, 0}});
    const std::string after = savedContent();
    inspect([&](fz_context* ctx, pdf_page* pp) {
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, pp->doc), PDF_NAME(Root));
        rootAfter = pdf_to_num(ctx, pdf_dict_get(ctx, root, PDF_NAME(StructTreeRoot)));
    });
    EXPECT_EQ(rootAfter, rootBefore);
    for (const std::string& id : {std::string("0"), std::string("1"), std::string("2")}) {
        EXPECT_EQ(count(before, ("/MCID\\s+" + id + "\\b").c_str()),
                  count(after, ("/MCID\\s+" + id + "\\b").c_str()))
            << "MCID " << id << " changed";
    }
    expectBalanced(after);
}

// ===========================================================================
// ImageExtractDecisionTest: the phase-1 NOT-PROBED row "extracting an image
// ignores its soft mask", decided in phase 6 (docs/image-placement-model.md).
// Extract returns the image XObject's own samples; its /SMask is a separate
// image XObject and is not applied. This pins that decision.
// ===========================================================================
class ImageExtractDecisionTest : public PlacementDocBase {};

TEST_F(ImageExtractDecisionTest, SoftMaskedImageExtractsItsOwnSamplesWithoutTheMask) {
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        pdf_obj* img = addRawRgbImage(ctx, pdoc, 32, 32, [](int, int, unsigned char* p) {
            p[0] = 255; p[1] = 0; p[2] = 0;
        });
        std::vector<unsigned char> alpha(32 * 32);
        for (int i = 0; i < 32 * 32; ++i) alpha[static_cast<size_t>(i)] = (i % 32) < 16 ? 0 : 255;
        fz_buffer* mb = fz_new_buffer_from_copied_data(ctx, alpha.data(), alpha.size());
        pdf_obj* sm = pdf_add_stream(ctx, pdoc, mb, nullptr, 0);
        fz_drop_buffer(ctx, mb);
        pdf_dict_put(ctx, sm, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, sm, PDF_NAME(Subtype), PDF_NAME(Image));
        pdf_dict_put_int(ctx, sm, PDF_NAME(Width), 32);
        pdf_dict_put_int(ctx, sm, PDF_NAME(Height), 32);
        pdf_dict_put(ctx, sm, PDF_NAME(ColorSpace), PDF_NAME(DeviceGray));
        pdf_dict_put_int(ctx, sm, PDF_NAME(BitsPerComponent), 8);
        pdf_dict_put(ctx, img, PDF_NAME(SMask), sm);
        pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImS", img);
        pdf_drop_obj(ctx, sm);
        pdf_drop_obj(ctx, img);
        setContents(ctx, pdoc, pp->obj, "q 200 0 0 120 150 400 cm /ImS Do Q\n");
    });
    ASSERT_TRUE(isBackground(render().pixel(190, 380))) << "oracle: the mask hides the left half";
    pdfe::image::ImageManager manager(context_);
    const QImage got = manager.extractImage(page(), doc_.get(), 0);
    ASSERT_FALSE(got.isNull());
    ASSERT_EQ(got.width(), 32);
    // Both halves come out as the stored red samples, opaque: the mask is a
    // separate image and is not applied (decided).
    EXPECT_TRUE(isRed(got.pixel(4, 16)));
    EXPECT_TRUE(isRed(got.pixel(28, 16)));
}

// ===========================================================================
// Security-audit follow-ups (phase 6 remediation): bounded work in the
// tagger's automatic placement, the inline-image dictionary lexed to its real
// ID operator, and the write-back size guard.
// ===========================================================================

namespace {

// One BT ... ET text object and one wrapped image draw per level, inside
// `depth` nested q ... Q pairs.
std::string deeplyNestedContent(int depth) {
    std::string c;
    c.reserve(static_cast<size_t>(depth) * 64);
    for (int i = 0; i < depth; ++i) c += "q BT (t) Tj ET q 9 0 0 9 0 0 cm /ImT Do Q\n";
    for (int i = 0; i < depth; ++i) c += "Q\n";
    return c;
}

int countToken(const std::string& c, const std::string& token) {
    int n = 0;
    for (size_t at = c.find(token); at != std::string::npos; at = c.find(token, at + 1)) ++n;
    return n;
}

const char kInlineImageWithIdLookalikes[] =
    "BI /W 1 /H 1 /CS /G /BPC 8 /IDx (one EI two) ID \x80 EI";

}  // namespace

// 20000 nested q ... Q pairs holding 20000 text objects and 20000 image draws.
// The old placement rescanned the span after every growth step, once per text
// atom, and took many seconds here. It must finish well inside 2 s and still
// place the regions (this is not the budget fallback).
TEST_F(TaggerModelTest, DeeplyNestedContentIsPlacedInBoundedTime) {
    constexpr int kDepth = 20000;
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        pdf_obj* img = addRawRgbImage(ctx, pdoc, 4, 4, [](int, int, unsigned char* p) {
            p[0] = 255; p[1] = 0; p[2] = 0;
        });
        pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImT", img);
        pdf_drop_obj(ctx, img);
        setContents(ctx, pdoc, pp->obj, deeplyNestedContent(kDepth));
    });
    std::vector<MarkedContentRegion> regions;
    for (int i = 0; i < 100; ++i)
        regions.push_back({i % 2 == 0 ? StructureTag::Figure : StructureTag::P, i, 0, 0});
    const auto t0 = std::chrono::steady_clock::now();
    tag(regions);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // Build-config-aware bound: a Debug build runs this 20000-deep placement
    // several times slower than Release (where it finishes well inside 2 s). The
    // old O(n^2) rescan took many seconds and would blow even the generous Debug
    // bound, so a quadratic regression is still caught in both configurations.
#ifdef NDEBUG
    constexpr double kPlacementBudgetSec = 2.0;
#else
    constexpr double kPlacementBudgetSec = 8.0;
#endif
    EXPECT_LT(seconds, kPlacementBudgetSec)
        << "automatic placement is not bounded on deep nesting";

    const std::string c = savedContent();
    // Placed, not left untagged: Figure 0 holds the first image draw, P 1 the
    // first text object and no image.
    const size_t fig = c.find("/Figure <</MCID 0>> BDC");
    const size_t para = c.find("/P <</MCID 1>> BDC");
    ASSERT_NE(fig, std::string::npos);
    ASSERT_NE(para, std::string::npos);
    const size_t figEnd = c.find("EMC", fig);
    const size_t paraEnd = c.find("EMC", para);
    ASSERT_NE(figEnd, std::string::npos);
    ASSERT_NE(paraEnd, std::string::npos);
    EXPECT_NE(c.substr(fig, figEnd - fig).find("/ImT Do"), std::string::npos);
    EXPECT_NE(c.substr(para, paraEnd - para).find("(t) Tj"), std::string::npos);
    EXPECT_EQ(c.substr(para, paraEnd - para).find(" Do"), std::string::npos);
    // Every operator kept; every sequence closed.
    EXPECT_EQ(countToken(c, "(t) Tj"), kDepth);
    EXPECT_EQ(countToken(c, "/ImT Do"), kDepth);
    EXPECT_EQ(countToken(c, " BDC"), 100);
    EXPECT_EQ(countToken(c, "EMC"), 100);
}

// A crafted chain of crossing q/Q and BT/ET pairs, in which every text atom's
// hull grows one link at a time to the whole page: quadratic growth steps
// whatever the lookup. The budget stops it; the page is left untagged (each
// region gets its empty sequence) and keeps every operator.
TEST_F(TaggerModelTest, CrossingPairChainHitsTheBudgetAndIsLeftUntagged) {
    constexpr int kLinks = 20000;
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        std::string c = "q BT (t) Tj Q\n";
        for (int i = 1; i < kLinks; ++i) c += "q ET BT (t) Tj Q\n";
        c += "ET\n";
        setContents(ctx, pdoc, pp->obj, c);
    });
    const auto t0 = std::chrono::steady_clock::now();
    tag({{StructureTag::P, 0, 0, 0}, {StructureTag::P, 1, 0, 0}});
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(seconds, 2.0) << "the placement budget did not bound a crossing chain";
    const std::string c = savedContent();
    EXPECT_EQ(countToken(c, "(t) Tj"), kLinks);
    for (int mcid : {0, 1}) {
        const std::string empty = "/P <</MCID " + std::to_string(mcid) + ">> BDC\nEMC";
        EXPECT_NE(c.find(empty), std::string::npos) << "MCID " << mcid << " is not empty";
    }
    EXPECT_EQ(countToken(c, " BDC"), 2);
    EXPECT_EQ(countToken(c, "EMC"), 2);
}

// The inline image's dictionary holds a name starting with ID (/IDx) and a
// string holding " EI ". The image runs from BI to the EI after the real ID
// operator; the old byte search stopped at /IDx and ended the image inside
// the string, so every later operator index shifted.
TEST_F(TaggerModelTest, InlineImageDictionaryWithIdLookalikesIsOneOperator) {
    const std::string inlineImage = kInlineImageWithIdLookalikes;
    edit([&](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        setContents(ctx, pdoc, pp->obj,
                    "q 10 0 0 10 72 700 cm " + inlineImage + " Q\n"
                    "BT /F1 12 Tf 72 600 Td (After) Tj ET\n");
    });
    // Explicit ranges: q cm BI Q are operators 0-3, the text object 4-8.
    tag({{StructureTag::Figure, 0, 0, 4}, {StructureTag::P, 1, 4, 9}});
    const std::string c = savedContent();
    std::string fig, para;
    ASSERT_TRUE(segment(c, "Figure", 0, &fig)) << c;
    ASSERT_TRUE(segment(c, "P", 1, &para)) << c;
    EXPECT_NE(fig.find(inlineImage + " Q"), std::string::npos) << "[" << fig << "]";
    EXPECT_NE(para.find("(After) Tj ET"), std::string::npos) << "[" << para << "]";
    EXPECT_EQ(para.find("EI"), std::string::npos) << "[" << para << "]";
    expectBalanced(c);
}

// The same page placed automatically: the inline image, with its wrapper,
// goes to the Figure whole; the text goes to the P.
TEST_F(TaggerModelTest, InlineImageDictionaryWithIdLookalikesIsPlacedWhole) {
    const std::string inlineImage = kInlineImageWithIdLookalikes;
    edit([&](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        setContents(ctx, pdoc, pp->obj,
                    "q 10 0 0 10 72 700 cm " + inlineImage + " Q\n"
                    "BT /F1 12 Tf 72 600 Td (After) Tj ET\n");
    });
    tag({{StructureTag::Figure, 0, 0, 0}, {StructureTag::P, 1, 0, 0}});
    const std::string c = savedContent();
    std::string fig, para;
    ASSERT_TRUE(segment(c, "Figure", 0, &fig)) << c;
    ASSERT_TRUE(segment(c, "P", 1, &para)) << c;
    EXPECT_NE(fig.find("q 10 0 0 10 72 700 cm " + inlineImage + " Q"), std::string::npos)
        << "[" << fig << "]";
    EXPECT_NE(para.find("(After) Tj ET"), std::string::npos) << "[" << para << "]";
    expectBalanced(c);
}

// ---------------------------------------------------------------------------
// The write-back size guard (ImageReplacer).
// ---------------------------------------------------------------------------
class TreReplaceSizeGuardTest : public TreReplaceModelTest {
protected:
    // replaceWithFontMatch painting `sel` magenta; the result is returned.
    pdfe::tre::ReconstructionResult tryMagentaEdit(const QRectF& sel) {
        using namespace pdfe::tre;
        auto ocr = std::make_unique<OcrPipeline>(context_.get(), doc_.get());
        auto coord = std::make_unique<CoordMapper>(context_.get(), doc_.get());
        auto emb = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        auto writer = std::make_unique<PdfContentWriter>(context_.get(), emb.get());
        auto cache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
        auto selector =
            std::make_unique<pdfe::fonts::SubstituteFontSelector>(analyzer.get(), cache.get());
        auto matcher = std::make_unique<FontMatcher>(selector.get(), analyzer.get());
        auto bgd = std::make_unique<BgColorDetector>();
        auto masker = std::make_unique<ImageMasker>();
        auto compositor = std::make_unique<BitmapCompositor>();
        ImageReplacer replacer(ocr.get(), coord.get(), bgd.get(), matcher.get(), masker.get(),
                               compositor.get(), writer.get());
        FontMatch fm;
        fm.fontFamily = "Arial";
        fm.fontStyle = "Regular";
        fm.fontFilePath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
        fm.sizePt = 12.0f;
        fm.color = RgbColor{255, 0, 255};
        fm.confidence = 0.8f;
        BackgroundInfo bg;
        bg.dominantColor = RgbColor{255, 0, 255};
        bg.isUniform = true;
        bg.confidence = 0.9f;
        PdfRect pr;
        pr.x0 = sel.left(); pr.y0 = sel.top(); pr.x1 = sel.right(); pr.y1 = sel.bottom();
        return replacer.replaceWithFontMatch(page(), doc_.get(), 0, pr, "WWWWWWWW", fm, bg,
                                             RegionId{3}, {}, "WWWWWWWW");
    }

    // The page's ImR as saved: its size and its first sample.
    void imageState(int* w, int* h, int* firstRed) {
        inspect([&](fz_context* ctx, pdf_page* pp) {
            pdf_obj* img = pdf_dict_gets(ctx, pageXObjects(ctx, pp), "ImR");
            *w = pdf_dict_get_int(ctx, img, PDF_NAME(Width));
            *h = pdf_dict_get_int(ctx, img, PDF_NAME(Height));
            fz_buffer* b = pdf_load_stream(ctx, img);
            unsigned char* d = nullptr;
            const size_t len = fz_buffer_storage(ctx, b, &d);
            *firstRed = len > 0 ? d[0] : -1;
            fz_drop_buffer(ctx, b);
        });
    }

    void expectRefusedAndUntouched(const std::string& placement, const char* what) {
        drawRed([&](fz_context*, pdf_page*) { return placement; });
        ASSERT_FALSE(pdfe::fonts::FontResolver::resolveFontPath("Arial").empty())
            << "no Arial-equivalent font";
        const std::string contentBefore = savedContent();
        const pdfe::tre::ReconstructionResult r = tryMagentaEdit(QRectF(160, 330, 180, 100));
        EXPECT_FALSE(r.success) << what << ": an edit with an implausible write-back size succeeded";
        EXPECT_NE(r.errorMessage.find("implausible size"), std::string::npos)
            << what << ": [" << r.errorMessage << "]";
        int w = 0, h = 0, red = 0;
        imageState(&w, &h, &red);
        EXPECT_EQ(w, 64) << what;
        EXPECT_EQ(h, 64) << what;
        EXPECT_EQ(red, 255) << what << ": the image's pixels changed";
        EXPECT_EQ(savedContent(), contentBefore) << what;
    }
};

// Upright placement 200 pt by 1e-7 pt: invertible, and the edit maps to a
// height of billions of image pixels. Refused; nothing allocated or written.
TEST_F(TreReplaceSizeGuardTest, UprightPlacementWithATinyScaleIsRefused) {
    expectRefusedAndUntouched("q 200 0 0 0.0000001 150 400 cm /ImR Do Q\n", "upright 1e-7");
}

// Mirrored, so the write-back goes through the placement matrix.
TEST_F(TreReplaceSizeGuardTest, MirroredPlacementWithATinyScaleIsRefused) {
    expectRefusedAndUntouched("q -200 0 0 0.0000001 350 400 cm /ImR Do Q\n", "mirrored 1e-7");
}

// A non-finite placement matrix does not reach the write-back through a page
// (the image finder reports no matrix for it, and the placement falls back to
// the page rect), so the guard's handling of NaN and infinity is pinned on the
// guard itself. ImageReplacer checks every value with it before any int
// conversion or allocation.
TEST(TreReplaceSizeGuardUnitTest, NonFiniteAndHugeValuesAreRejected) {
    using namespace pdfe::tre::writeback;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(intSafe(nan));
    EXPECT_FALSE(intSafe(inf));
    EXPECT_FALSE(intSafe(-inf));
    EXPECT_FALSE(intSafe(1e10));
    EXPECT_FALSE(intSafe(-1e10));
    EXPECT_FALSE(intSafe(30.0 * 64.0 / 1e-7));  // 30 pt of a 64 px image placed 1e-7 pt high
    EXPECT_TRUE(intSafe(0.0));
    EXPECT_TRUE(intSafe(-250.5));
    EXPECT_TRUE(intSafe(16384.0));

    EXPECT_FALSE(transformIsFinite(QTransform(nan, 0, 0, 1, 0, 0)));
    EXPECT_FALSE(transformIsFinite(QTransform(1, 0, 0, 1, inf, 0)));
    EXPECT_TRUE(transformIsFinite(QTransform(0, 2, -3, 0, 10, 20)));

    const QSize small(64, 64);
    EXPECT_TRUE(sizeIsSane(1, 1, small));
    EXPECT_TRUE(sizeIsSane(16384, 6000, small));
    EXPECT_FALSE(sizeIsSane(16385, 1, small)) << "a side over 16384";
    EXPECT_FALSE(sizeIsSane(1, 16385, small)) << "a side over 16384";
    EXPECT_FALSE(sizeIsSane(16384, 16384, small)) << "over 100 million pixels";
    EXPECT_FALSE(sizeIsSane(0, 10, small));
    EXPECT_FALSE(sizeIsSane(10, -1, small));
    // A target already larger than the caps raises them to its own size.
    const QSize huge(20000, 12000);
    EXPECT_TRUE(sizeIsSane(20000, 12000, huge));
    EXPECT_FALSE(sizeIsSane(20001, 12000, huge));
}

// CONTROL: an ordinary upright placement is still edited.
TEST_F(TreReplaceSizeGuardTest, OrdinaryPlacementIsStillEdited) {
    drawRed([](fz_context*, pdf_page*) {
        return std::string("q 200 0 0 120 150 400 cm /ImR Do Q\n");
    });
    ASSERT_FALSE(pdfe::fonts::FontResolver::resolveFontPath("Arial").empty())
        << "no Arial-equivalent font";
    const QImage before = render();
    const QRectF sel(170, 340, 60, 30);
    ASSERT_TRUE(isRed(before.pixel(int(sel.center().x()), int(sel.center().y()))));
    const pdfe::tre::ReconstructionResult r = tryMagentaEdit(sel);
    ASSERT_TRUE(r.success) << r.errorMessage;
    expectEditOnlyInside(before, render(), sel, 6.0, "upright control");
}

// ===========================================================================
// Claim-check remediation (phase 6, round 2): documented rules that had no
// test of their own. Overlapping explicit ranges, ranges that cut BT/ET or
// BDC/EMC, the byte-exact repoint, and a source image with /Decode replaced.
// ===========================================================================

// An explicit range that overlaps an earlier region is not written: it gets
// its own empty sequence at the end, and the earlier region is intact.
TEST_F(TaggerModelTest, AnExplicitRangeOverlappingAnEarlierRegionIsNotWritten) {
    interleavedPage();
    // [0, 9): the first text object and the first image draw (balanced).
    // [5, 14): the first image draw and the second text object (balanced on
    // its own, so only the overlap can refuse it).
    tag({{StructureTag::P, 0, 0, 9}, {StructureTag::Figure, 1, 5, 14}});
    const std::string c = savedContent();
    std::string p0, f1;
    ASSERT_TRUE(segment(c, "P", 0, &p0)) << c;
    ASSERT_TRUE(segment(c, "Figure", 1, &f1)) << c;
    // The earlier region is intact: exactly its own operators.
    EXPECT_NE(p0.find("First paragraph"), std::string::npos) << c;
    EXPECT_NE(p0.find("72 600 cm"), std::string::npos) << c;
    EXPECT_TRUE(hasDo(p0)) << c;
    EXPECT_EQ(p0.find("Second paragraph"), std::string::npos) << c;
    // The overlapping one is not written: one BDC for MCID 1, empty, after
    // all of the page's own content.
    EXPECT_EQ(count(c, "/MCID\\s+1\\b"), 1) << c;
    EXPECT_EQ(f1.find_first_not_of(" \t\r\n"), std::string::npos) << "[" << f1 << "]";
    EXPECT_GT(c.find("/MCID 1"), c.find("72 400 cm")) << c;
    EXPECT_EQ(count(c, "\\bBDC\\b"), 2) << c;
    // Nothing duplicated or lost; content order kept.
    EXPECT_EQ(count(c, "First paragraph"), 1) << c;
    EXPECT_EQ(count(c, "Second paragraph"), 1) << c;
    EXPECT_EQ(count(c, "72 600 cm"), 1) << c;
    EXPECT_EQ(count(c, "72 400 cm"), 1) << c;
    EXPECT_LT(c.find("72 600 cm"), c.find("Second paragraph"));
    EXPECT_LT(c.find("Second paragraph"), c.find("72 400 cm"));
    expectBalanced(c);
}

// A range that would cut a BT/ET pair is not written.
TEST_F(TaggerModelTest, ARangeThatCutsABtEtPairIsNotWritten) {
    interleavedPage();
    // [0, 4): BT Tf Td Tj without its ET. No q/Q or BDC/EMC inside, so only
    // the BT/ET pairing can refuse it.
    tag({{StructureTag::P, 0, 0, 4}});
    const std::string c = savedContent();
    std::string p0;
    ASSERT_TRUE(segment(c, "P", 0, &p0)) << c;
    EXPECT_EQ(p0.find_first_not_of(" \t\r\n"), std::string::npos) << "[" << p0 << "]";
    EXPECT_EQ(count(c, "\\bBT\\b"), count(c, "\\bET\\b")) << c;
    EXPECT_EQ(count(c, "First paragraph"), 1) << c;
    // The empty sequence comes after the page's own content.
    EXPECT_GT(c.find("/MCID 0"), c.find("72 400 cm")) << c;
    expectBalanced(c);
}

// A range that would cut a BDC/EMC pair (an existing sequence) is not written,
// and the existing sequence is kept as it was.
TEST_F(TaggerModelTest, ARangeThatCutsABdcEmcPairIsNotWritten) {
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        pdf_obj* img = addRawRgbImage(ctx, pdoc, 16, 16, [](int, int, unsigned char* p) {
            p[0] = 255; p[1] = 0; p[2] = 0;
        });
        pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImT", img);
        pdf_drop_obj(ctx, img);
        // Operators: 0 BDC, 1 q, 2 cm, 3 Do, 4 Q, 5 EMC, 6-10 the text object.
        setContents(ctx, pdoc, pp->obj,
                    "/Span <</MCID 9>> BDC q 100 0 0 60 72 600 cm /ImT Do Q EMC\n"
                    "BT /F1 12 Tf 72 560 Td (After) Tj ET\n");
    });
    // [0, 5): BDC q cm Do Q without the EMC. q/Q balanced, no BT, so only the
    // BDC/EMC pairing can refuse it.
    tag({{StructureTag::Figure, 1, 0, 5}});
    const std::string c = savedContent();
    std::string fig, span;
    ASSERT_TRUE(segment(c, "Figure", 1, &fig)) << c;
    EXPECT_EQ(fig.find_first_not_of(" \t\r\n"), std::string::npos) << "[" << fig << "]";
    ASSERT_TRUE(segment(c, "Span", 9, &span)) << c;
    EXPECT_TRUE(hasDo(span)) << c;
    EXPECT_NE(span.find("72 600 cm"), std::string::npos) << c;
    EXPECT_EQ(span.find("MCID"), std::string::npos) << c;
    EXPECT_EQ(count(c, "\\bBDC\\b"), 2) << c;
    EXPECT_GT(c.find("/MCID 1"), c.find("(After)")) << c;
    expectBalanced(c);
}

// GitHub #446: a repoint changes only the targeted Do's name operand. Every
// other byte of the page content (numbers written in their own form, a Do
// outside a recognised wrapper) and the drawing order stay.
TEST_F(TreReplaceModelTest, RepointChangesOnlyTheTargetedDoNameInTheContent) {
    // Three draws of /ImR: a wrapper with numbers in a non-canonical form, a Do
    // after two cm's (no recognised wrapper), and a plain wrapper.
    drawRed([](fz_context*, pdf_page*) {
        return std::string("q 200.00 0 0 120.000 150 400 cm /ImR Do Q\n"
                           "q 1 0 0 1 0 0 cm 200 0 0 120 150 100 cm /ImR Do Q\n"
                           "q 200 0 0 120 150 650 cm /ImR Do Q\n");
    });
    QImage blue(64, 64, QImage::Format_RGB888);
    blue.fill(QColor(0, 0, 255));

    // First placement (rendered y 322..442).
    const std::string before1 = savedContent();
    ASSERT_EQ(before1.find("PdfeTreIm"), std::string::npos);
    const auto t1 = writer_->findImageXObjectForRegion(page(), doc_.get(), QRectF(160, 330, 180, 100));
    ASSERT_TRUE(t1.has_value());
    ASSERT_EQ(t1->xobjectName, "ImR");
    ASSERT_EQ(t1->occurrence, 0);
    ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), blue, *t1));
    const std::string after1 = savedContent();
    std::string expected1 = before1;
    const size_t at1 = expected1.find("/ImR Do");
    ASSERT_NE(at1, std::string::npos);
    expected1.replace(at1, 4, "/PdfeTreIm1");
    EXPECT_EQ(after1, expected1) << "the repoint changed more than the Do's name";

    // Second placement, the Do outside a recognised wrapper (rendered y 622..742).
    const auto t2 = writer_->findImageXObjectForRegion(page(), doc_.get(), QRectF(160, 630, 180, 100));
    ASSERT_TRUE(t2.has_value());
    ASSERT_EQ(t2->xobjectName, "ImR");
    ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), blue, *t2));
    const std::string after2 = savedContent();
    std::string expected2 = after1;
    const size_t at2 = expected2.find("/ImR Do");
    ASSERT_NE(at2, std::string::npos);
    ASSERT_GT(at2, expected2.find("150 100 cm")) << "precondition: the next ImR is the second draw";
    expected2.replace(at2, 4, "/PdfeTreIm2");
    EXPECT_EQ(after2, expected2) << "the repoint changed more than the Do's name";

    // The new names are image entries; the third copy is untouched.
    bool fresh1 = false, fresh2 = false, old = false;
    inspect([&](fz_context* ctx, pdf_page* pp) {
        pdf_obj* xo = pageXObjects(ctx, pp);
        auto isImage = [&](const char* n) {
            return pdf_name_eq(ctx, pdf_dict_get(ctx, pdf_dict_gets(ctx, xo, n), PDF_NAME(Subtype)),
                               PDF_NAME(Image)) != 0;
        };
        fresh1 = isImage("PdfeTreIm1");
        fresh2 = isImage("PdfeTreIm2");
        old = isImage("ImR");
    });
    EXPECT_TRUE(fresh1);
    EXPECT_TRUE(fresh2);
    EXPECT_TRUE(old);
    const QImage after = render();
    EXPECT_TRUE(isBlue(after.pixel(250, 380)));
    EXPECT_TRUE(isBlue(after.pixel(250, 680)));
    EXPECT_TRUE(isRed(after.pixel(250, 130))) << "the third copy changed";
}

// GitHub #445: a source image WITH /Decode. The replacement carries no /Decode
// (the new samples are the colours to show) and renders the intended colours.
TEST_F(TreReplaceModelTest, ReplacingADecodeImageWritesNoDecodeAndShowsTheNewColours) {
    edit([](fz_context* ctx, pdf_document* pdoc, pdf_page* pp) {
        // Stored cyan; /Decode [1 0 1 0 1 0] inverts it to red.
        pdf_obj* dec = addRawRgbImage(ctx, pdoc, 16, 16, [](int, int, unsigned char* p) {
            p[0] = 0; p[1] = 255; p[2] = 255;
        });
        pdf_obj* darr = pdf_new_array(ctx, pdoc, 6);
        for (int v : {1, 0, 1, 0, 1, 0}) pdf_array_push_int(ctx, darr, v);
        pdf_dict_put(ctx, dec, PDF_NAME(Decode), darr);
        pdf_dict_puts(ctx, pageXObjects(ctx, pp), "ImD", dec);
        pdf_drop_obj(ctx, darr);
        pdf_drop_obj(ctx, dec);
        setContents(ctx, pdoc, pp->obj, "q 200 0 0 120 150 400 cm /ImD Do Q\n");
    });
    bool decodeBefore = false;
    inspect([&](fz_context* ctx, pdf_page* pp) {
        pdf_obj* img = pdf_dict_gets(ctx, pageXObjects(ctx, pp), "ImD");
        decodeBefore = pdf_is_array(ctx, pdf_dict_get(ctx, img, PDF_NAME(Decode))) != 0;
    });
    ASSERT_TRUE(decodeBefore) << "precondition: the source image has /Decode";
    ASSERT_TRUE(isRed(render().pixel(250, 380))) << "oracle: /Decode turns the cyan samples red";

    const auto t = writer_->findImageXObjectForRegion(page(), doc_.get(), QRectF(160, 330, 180, 100));
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->xobjectName, "ImD");
    // Blue left half, green right half. Under the old inverting /Decode they
    // would show as yellow and magenta.
    QImage pic(16, 16, QImage::Format_RGB888);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            pic.setPixelColor(x, y, x < 8 ? QColor(0, 0, 255) : QColor(0, 255, 0));
    ASSERT_TRUE(writer_->replacePageImage(page(), doc_.get(), pic, *t));

    bool decodeAfter = true, image = false;
    inspect([&](fz_context* ctx, pdf_page* pp) {
        pdf_obj* img = pdf_dict_gets(ctx, pageXObjects(ctx, pp), "ImD");
        image = pdf_name_eq(ctx, pdf_dict_get(ctx, img, PDF_NAME(Subtype)), PDF_NAME(Image)) != 0;
        decodeAfter = pdf_dict_get(ctx, img, PDF_NAME(Decode)) != nullptr;
    });
    ASSERT_TRUE(image) << "the sole placement is replaced under its own name";
    EXPECT_FALSE(decodeAfter) << "the replacement carries the old /Decode";
    const QImage after = render();
    const QRgb left = after.pixel(190, 380), right = after.pixel(310, 380);
    EXPECT_TRUE(isBlue(left)) << std::hex << left;
    EXPECT_TRUE(qAlpha(right) > 200 && qRed(right) < 90 && qGreen(right) > 200 && qBlue(right) < 90)
        << std::hex << right;
}
