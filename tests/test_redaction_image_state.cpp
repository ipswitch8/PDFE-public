// -*- coding: utf-8 -*-
// Pipeline image-placement-state, phase-1 (audit): RED probes. Each test
// asserts the CORRECT behaviour and therefore FAILS on today's code, for the
// reason documented beside it. Nothing here is disabled or expected-fail.
//
//   RD1 (RedactionImageState.PartialOverlapKeepsTheRestOfTheImage,
//        .PartialOverlapBlanksTheRedactedPixels,
//        .RedactedPixelsAreNotRecoverableFromTheSavedFile)
//       The UI's "Apply Redactions" (MainWindow::onApplyRedactions) calls
//       RedactionManager::applyAllRedactions with default
//       RedactionApplyOptions, whose imageMethod is Remove. A mark covering a
//       50x50 corner of a 400x300 image therefore deletes the WHOLE image.
//       Asserts: a pixel far from the mark is still the image colour, a pixel
//       inside the mark is not, and the saved file's image XObject has the
//       marked region blanked (and no object in the file still carries the
//       original pixels there).
//   RD2 (RedactionImageState.PerMarkImageMethodIsHonoured)
//       RedactionData::imageMethod is never read. A mark restored with
//       imageMethod=None must leave its image alone while a mark with Remove
//       removes its image; today both get the one global method.
//   RD3 (FlattenImageState.FlattenFormFlattensOnlyTheGivenPage)
//       FormManager::flattenForm(page) calls pdf_bake_document(.., 0, 1),
//       which bakes the widgets of EVERY page. Asserts page 1 keeps its
//       widgets after page 0 is flattened.
//   RD4 (no probe) RedactionManager::flattenAnnotations is documented as
//       document-wide and has no page-scoped API; see the findings file.
//   FL1 (FlattenImageState.FlattenedImageStampIsEnumerableByTheImageTool)
//       After flattenAnnotations an image stamp is page content inside a Form
//       XObject; ImageManager::enumerateImages does not recurse into forms, so
//       the image tool cannot select it. Asserts it is listed.
#include <gtest/gtest.h>
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionData.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QImage>
#include <QRectF>
#include <QTemporaryDir>
#include <cmath>
#include <filesystem>
#include <memory>
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
using pdfe::redaction::RedactionData;
using pdfe::redaction::RedactionImageMethod;

namespace {

std::string fixture(const char* name) {
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/", "../../tests/fixtures/"}) {
        const std::string c = std::string(prefix) + name;
        if (std::filesystem::exists(c)) return c;
    }
    const std::string d = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return std::filesystem::exists(d) ? d : std::string();
}

bool isRedPx(QRgb c) {
    return qAlpha(c) > 200 && qRed(c) > 200 && qGreen(c) < 90 && qBlue(c) < 90;
}

// Result of scanning every Image XObject in a saved file with raw MuPDF.
struct SavedImages {
    int imageObjects = 0;           // all Image XObjects anywhere in the file
    int imagesWithRedCorner = 0;    // ... whose pixel (0,0) is still red
    int pageImageCount = 0;         // Image XObjects in page 0 resources
    bool pageImageLoaded = false;
    int pageImageW = 0, pageImageH = 0;
    QRgb pageImageTopLeft = 0;      // sample (1,1) of the page's image
    QRgb pageImageFar = 0;          // sample at 3/4 w, 3/4 h
};

SavedImages scanSaved(MuPdfContext& mc, const std::string& path) {
    SavedImages out;
    mc.execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_open_document(ctx, path.c_str());
        auto sample = [&](pdf_obj* obj, int x, int y, bool& ok) -> QRgb {
            QRgb px = 0;
            fz_image* im = nullptr;
            fz_pixmap* pm = nullptr;
            fz_pixmap* conv = nullptr;
            fz_var(im);
            fz_var(pm);
            fz_var(conv);
            fz_try(ctx) {
                im = pdf_load_image(ctx, pdoc, obj);
                pm = fz_get_pixmap_from_image(ctx, im, nullptr, nullptr, nullptr, nullptr);
                fz_pixmap* rgb = pm;
                if (pm->n != 3 && pm->n != 4) {
                    conv = fz_convert_pixmap(ctx, pm, fz_device_rgb(ctx), nullptr, nullptr,
                                             fz_default_color_params, 0);
                    rgb = conv;
                }
                if (x >= rgb->w) x = rgb->w - 1;
                if (y >= rgb->h) y = rgb->h - 1;
                const unsigned char* p = rgb->samples + y * rgb->stride + x * rgb->n;
                px = qRgb(p[0], p[1], p[2]);
                out.pageImageW = rgb->w;
                out.pageImageH = rgb->h;
                ok = true;
            }
            fz_always(ctx) {
                fz_drop_pixmap(ctx, conv);
                fz_drop_pixmap(ctx, pm);
                fz_drop_image(ctx, im);
            }
            fz_catch(ctx) { ok = false; }
            return px;
        };
        const int n = pdf_xref_len(ctx, pdoc);
        for (int i = 1; i < n; ++i) {
            pdf_obj* o = nullptr;
            fz_try(ctx) { o = pdf_load_object(ctx, pdoc, i); }
            fz_catch(ctx) { o = nullptr; }
            if (!o) continue;
            if (pdf_is_dict(ctx, o) &&
                pdf_name_eq(ctx, pdf_dict_get(ctx, o, PDF_NAME(Subtype)), PDF_NAME(Image))) {
                ++out.imageObjects;
                bool ok = false;
                const QRgb c = sample(o, 0, 0, ok);
                if (ok && isRedPx(c)) ++out.imagesWithRedCorner;
            }
            pdf_drop_obj(ctx, o);
        }
        pdf_page* pp = pdf_load_page(ctx, pdoc, 0);
        pdf_obj* xo = pdf_dict_get(ctx, pdf_page_resources(ctx, pp), PDF_NAME(XObject));
        const int m = xo ? pdf_dict_len(ctx, xo) : 0;
        for (int i = 0; i < m; ++i) {
            pdf_obj* v = pdf_dict_get_val(ctx, xo, i);
            if (!pdf_name_eq(ctx, pdf_dict_get(ctx, v, PDF_NAME(Subtype)), PDF_NAME(Image))) continue;
            ++out.pageImageCount;
            if (out.pageImageLoaded) continue;
            bool ok = false;
            out.pageImageTopLeft = sample(v, 1, 1, ok);
            if (ok) {
                bool ok2 = false;
                out.pageImageFar = sample(v, out.pageImageW * 3 / 4, out.pageImageH * 3 / 4, ok2);
                out.pageImageLoaded = ok2;
            }
        }
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, pdoc);
    });
    return out;
}

} // namespace

class RedactionImageStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tmp_.isValid());
        ctx_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(ctx_);
        redact_ = std::make_unique<pdfe::redaction::RedactionManager>(ctx_);
        const std::string pdf = fixture("test_single.pdf");
        ASSERT_FALSE(pdf.empty()) << "fixture test_single.pdf not found";
        doc_ = Document::open(ctx_, pdf);
        ASSERT_NE(doc_, nullptr);
    }

    // Insert a solid red image of the given placement (fitz Y-down).
    void insertRed(const QRectF& rect, const char* file) {
        const std::string png = tmp_.filePath(file).toStdString();
        QImage red(64, 64, QImage::Format_RGB32);
        red.fill(QColor(255, 0, 0));
        ASSERT_TRUE(red.save(QString::fromStdString(png)));
        auto page = doc_->page(0);
        InsertImageOptions o;
        o.filePath = png;
        o.targetRect = rect;
        o.maintainAspectRatio = false;
        auto r = manager_->insertImage(page.get(), doc_.get(), o);
        ASSERT_TRUE(r.success) << r.errorMessage;
    }

    QRgb pixel(QPointF p) {
        auto page = doc_->page(0);
        const QImage img = page->render(QRectF(), 1.0f);
        const int x = static_cast<int>(std::lround(p.x()));
        const int y = static_cast<int>(std::lround(p.y()));
        if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) return qRgb(0, 0, 255);
        return img.pixel(x, y);
    }
    bool isRed(QPointF p) { return isRedPx(pixel(p)); }

    // Exactly what MainWindow::onApplyRedactions passes: default options but
    // rewriteDocument=false.
    pdfe::redaction::RedactionApplyResult applyAsUiDoes() {
        pdfe::redaction::RedactionApplyOptions options;
        options.rewriteDocument = false;
        return redact_->applyAllRedactions(doc_.get(), options);
    }

    QTemporaryDir tmp_;
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<pdfe::redaction::RedactionManager> redact_;
    std::unique_ptr<Document> doc_;
};

// 400x300 image at (100,100); the mark covers its top-left 50x50 corner.
static const QRectF kImage(100.0, 100.0, 400.0, 300.0);
static const QRectF kMark(100.0, 100.0, 50.0, 50.0);

class RedactionImageState : public RedactionImageStateTest {};

class RedactionImageStateCorner : public RedactionImageStateTest {
protected:
    void SetUp() override {
        RedactionImageStateTest::SetUp();
        if (::testing::Test::HasFatalFailure()) return;
        insertRed(kImage, "big.png");
        ASSERT_TRUE(isRed(kImage.center())) << "oracle: image renders red";
        ASSERT_TRUE(isRed(kMark.center())) << "oracle: marked corner is red before redaction";
        auto page = doc_->page(0);
        redact_->createRedactionMark(page.get(), kMark);
        ASSERT_EQ(redact_->getRedactionMarks(page.get()).size(), 1u);
    }
};

// RD1: the part of the image outside the mark survives.
TEST_F(RedactionImageStateCorner, PartialOverlapKeepsTheRestOfTheImage) {
    auto r = applyAsUiDoes();
    ASSERT_TRUE(r.success) << r.errorMessage;
    EXPECT_TRUE(isRed(QPointF(kImage.right() - 20, kImage.bottom() - 20)))
        << "a pixel far from the mark is no longer the image colour: the whole image was "
           "deleted (default RedactionImageMethod::Remove)";
    EXPECT_TRUE(isRed(kImage.center())) << "the image centre was deleted by a corner mark";
}

// RD1: the pixels under the mark are not the image colour.
TEST_F(RedactionImageStateCorner, PartialOverlapBlanksTheRedactedPixels) {
    auto r = applyAsUiDoes();
    ASSERT_TRUE(r.success) << r.errorMessage;
    ASSERT_TRUE(isRed(QPointF(kImage.right() - 20, kImage.bottom() - 20)))
        << "precondition: the unmarked part of the image must remain (see "
           "PartialOverlapKeepsTheRestOfTheImage)";
    EXPECT_FALSE(isRed(kMark.center())) << "marked pixel still shows the image";
}

// RD1 (security): save, reopen with raw MuPDF; the image XObject that remains
// has its marked region blanked and no object in the file still carries the
// original pixels in the marked corner.
TEST_F(RedactionImageStateCorner, RedactedPixelsAreNotRecoverableFromTheSavedFile) {
    auto r = applyAsUiDoes();
    ASSERT_TRUE(r.success) << r.errorMessage;
    const std::string out = tmp_.filePath("redacted.pdf").toStdString();
    doc_->saveAs(out);
    const SavedImages s = scanSaved(*ctx_, out);
    ASSERT_EQ(s.pageImageCount, 1)
        << "the page must still reference its (partially redacted) image XObject; found "
        << s.pageImageCount << " (the whole image was removed)";
    ASSERT_TRUE(s.pageImageLoaded);
    // Source pixel (1,1) lies inside the 50x50 mark (the mark covers ~12.5% of
    // the width and ~16.7% of the height of the image); 3/4,3/4 is far outside.
    EXPECT_FALSE(isRedPx(s.pageImageTopLeft)) << "marked region of the image XObject is not blanked";
    EXPECT_TRUE(isRedPx(s.pageImageFar)) << "unmarked region of the image XObject was lost";
    EXPECT_EQ(s.imagesWithRedCorner, 0)
        << "an image object in the saved file still carries the original marked pixels";
}

// RD2: per-mark RedactionData::imageMethod is honoured. Two images; the mark
// over A is restored with None, the mark over B with Remove.
TEST_F(RedactionImageState, PerMarkImageMethodIsHonoured) {
    const QRectF imgA(60.0, 100.0, 200.0, 150.0);
    const QRectF imgB(300.0, 100.0, 200.0, 150.0);
    insertRed(imgA, "a.png");
    insertRed(imgB, "b.png");
    ASSERT_TRUE(isRed(imgA.center()));
    ASSERT_TRUE(isRed(imgB.center()));
    auto page = doc_->page(0);
    RedactionData ma;
    ma.rect = QRectF(imgA.x(), imgA.y(), 40, 40);
    ma.pageIndex = 0;
    ma.imageMethod = RedactionImageMethod::None;
    RedactionData mb;
    mb.rect = QRectF(imgB.x(), imgB.y(), 40, 40);
    mb.pageIndex = 0;
    mb.imageMethod = RedactionImageMethod::Remove;
    redact_->restoreRedactionMark(page.get(), ma);
    redact_->restoreRedactionMark(page.get(), mb);
    ASSERT_EQ(redact_->getRedactionMarks(page.get()).size(), 2u);
    page.reset();
    auto r = applyAsUiDoes();
    ASSERT_TRUE(r.success) << r.errorMessage;
    EXPECT_TRUE(isRed(imgA.center()))
        << "image A (mark imageMethod=None) was altered: the per-mark method is ignored";
    EXPECT_FALSE(isRed(imgB.center()))
        << "image B (mark imageMethod=Remove) survives";
}

// ---------------------------------------------------------------------------
// RD3 / FL1
// ---------------------------------------------------------------------------
class FlattenImageState : public RedactionImageStateTest {};

// RD3: flattenForm(page 0) must not flatten page 1's widgets.
TEST_F(FlattenImageState, FlattenFormFlattensOnlyTheGivenPage) {
    const std::string src = fixture("test_form.pdf");
    ASSERT_FALSE(src.empty());
    const std::string two = tmp_.filePath("two.pdf").toStdString();
    bool built = false;
    ctx_->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_open_document(ctx, src.c_str());
        // pdf_graft_page copies content but not annotations, so give the new
        // page its own copies of page 0's widget dictionaries.
        pdf_graft_page(ctx, pdoc, -1, pdoc, 0);
        pdf_obj* p0 = pdf_lookup_page_obj(ctx, pdoc, 0);
        pdf_obj* p1 = pdf_lookup_page_obj(ctx, pdoc, 1);
        pdf_obj* src = pdf_dict_get(ctx, p0, PDF_NAME(Annots));
        pdf_obj* dst = pdf_new_array(ctx, pdoc, pdf_array_len(ctx, src));
        pdf_dict_put_drop(ctx, p1, PDF_NAME(Annots), dst);
        for (int i = 0; i < pdf_array_len(ctx, src); ++i) {
            pdf_obj* w = pdf_array_get(ctx, src, i);
            pdf_obj* copy = pdf_deep_copy_obj(ctx, w);
            pdf_dict_put(ctx, copy, PDF_NAME(P), p1);
            pdf_array_push_drop(ctx, dst, pdf_add_object_drop(ctx, pdoc, copy));
        }
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, pdoc, two.c_str(), &wo);
        pdf_drop_document(ctx, pdoc);
        built = true;
    });
    ASSERT_TRUE(built);
    doc_ = Document::open(ctx_, two);
    ASSERT_NE(doc_, nullptr);
    ASSERT_EQ(doc_->pageCount(), 2);
    pdfe::forms::FormManager forms(ctx_);
    {
        auto p0 = doc_->page(0);
        auto p1 = doc_->page(1);
        ASSERT_TRUE(forms.hasFormFields(p0.get())) << "precondition: page 0 has widgets";
        ASSERT_TRUE(forms.hasFormFields(p1.get())) << "precondition: page 1 has widgets";
    }
    {
        auto p0 = doc_->page(0);
        forms.flattenForm(p0.get());
    }
    const std::string out = tmp_.filePath("flat.pdf").toStdString();
    doc_->saveAs(out);
    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr);
    ASSERT_EQ(reopened->pageCount(), 2);
    auto q0 = reopened->page(0);
    auto q1 = reopened->page(1);
    ASSERT_FALSE(forms.hasFormFields(q0.get())) << "precondition: page 0 was flattened";
    EXPECT_TRUE(forms.hasFormFields(q1.get()))
        << "page 1's widgets were flattened too: pdf_bake_document bakes every page";
}

// FL1: a flattened image stamp is still an image the image tool can list.
TEST_F(FlattenImageState, FlattenedImageStampIsEnumerableByTheImageTool) {
    pdfe::annotations::AnnotationManager annots(ctx_);
    QImage img(40, 20, QImage::Format_RGB32);
    img.fill(qRgb(255, 0, 0));
    const QRectF rect(100.0, 100.0, 200.0, 100.0);
    {
        auto page = doc_->page(0);
        auto data = annots.createAnnotation(page.get(), pdfe::annotations::AnnotationType::Stamp, rect);
        ASSERT_GE(data.annotIndex, 0);
        annots.setStampImageAppearance(page.get(), data.annotIndex, img);
        ASSERT_TRUE(annots.stampAppearanceHasImageXObjectForTesting(page.get(), data.annotIndex));
    }
    ASSERT_TRUE(isRed(rect.center())) << "oracle: the stamp renders red before flatten";
    ASSERT_TRUE(redact_->flattenAnnotations(doc_.get()));
    ASSERT_TRUE(isRed(rect.center())) << "precondition: the baked stamp is still painted";
    auto page = doc_->page(0);
    EXPECT_TRUE(annots.getAnnotations(page.get()).empty()) << "precondition: annotation baked away";
    const auto imgs = manager_->enumerateImages(page.get(), doc_.get());
    EXPECT_EQ(imgs.size(), 1u)
        << "the flattened stamp image is page content inside a Form XObject and "
           "enumerateImages does not recurse into forms: the image tool cannot select it";
}
