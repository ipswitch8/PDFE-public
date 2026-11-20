// -*- coding: utf-8 -*-
// Pipeline image-placement-state, phase-1 (audit). RED probes: every probe
// asserts the CORRECT behaviour, so it FAILS on today's code, for the reason
// documented next to it. Nothing here is DISABLED_ or skipped. Preconditions are
// ASSERTed (they pass today); the behaviour under audit is EXPECTed.
//
// The image is a solid red 64x64 PNG inserted onto tests/fixtures/test_single.pdf,
// so Page::render is its own oracle (alpha channel: unpainted = transparent).
// Document::fzDocument() is private, so each scenario is built by
// save -> raw MuPDF edit -> reopen through Document.
//
//   T1  src/tre/PdfContentWriter.cpp enumerateDrawnImages / drawnImageFilter
//       flips Y with fz_bound_page height and ignores the MediaBox origin and
//       /Rotate, so placementFitz is in a different frame from ImageManager's.
//         TreReplaceStateTest.PlacementFrameFollowsMediaBoxOrigin
//           MediaBox [50 100 ...]: placementFitz must equal ImageManager's
//           boundingBox for the same image (and the red pixels it renders as).
//         TreReplaceStateTest.PlacementFrameFollowsPageRotate
//           /Rotate 90: placementFitz must equal the rendered red extent.
//
//   T2  src/tre/ImageReplacer.cpp buildWriteBackImage maps the edit rect into
//       image pixels assuming an upright, unflipped placement.
//         TreReplaceStateTest.WriteBackLandsWhereEditedOnUprightImageControl
//           (control, passes) the same edit on an upright image.
//         TreReplaceStateTest.WriteBackLandsWhereEditedOnRotatedImage
//           image rotated 90 degrees (ImageContentEditor::rotateImage): a
//           replaceWithFontMatch over a small page rect must change pixels
//           inside that rect and nowhere else.
//         TreReplaceStateTest.WriteBackLandsWhereEditedOnFlippedImage
//           the same for a vertically flipped placement (cm with d < 0).
//
//   T3  PdfContentWriter::replacePageImage converts to RGB888 and adds a new
//       image: the original /SMask (/Mask, /Decode, ICC, /OC) is dropped.
//         TreReplaceStateTest.ReplaceKeepsTheImageSoftMask
//           replace an SMask image with its own colour pixels: the masked-out
//           half must stay transparent.
//
//   T4  replacePageImage puts the new image under the shared XObject name, so
//       EVERY Do of that name changes.
//         TreReplaceStateTest.ReplaceTargetsOnlyTheEditedPlacement
//           an image drawn twice; replace with the target resolved for the
//           first placement: the second placement must keep its pixels.
//
//   T5  RegionReconstructor (RegionReconstructor.cpp:138,173-175) writes a
//       whole-page render into the largest drawn image through the
//       two-argument replacePageImage, even when that image is small.
//         TreReplaceStateTest.WholePageRenderIsNotWrittenIntoASmallerImage
//           small image + text elsewhere: after the call the image's rect must
//           still show the image, not a shrunken copy of the page.
//
//   G1  src/compliance/ContentStreamTagger.cpp tagPageContent (~l.123-168)
//       wraps the ENTIRE stream in region 0's BDC/EMC and emits the rest as empty
//       "BDC EMC", so a /Figure MCID points at nothing.
//         TaggerStateTest.FigureMcidEnclosesTheImageDo
//         TaggerStateTest.TextMcidDoesNotEncloseTheImageDo
#include <gtest/gtest.h>

#include "compliance/ContentStreamTagger.h"
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
#include "image/ImageContentEditor.h"
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

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QTemporaryDir>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
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
using pdfe::image::ImageContentEditor;
using pdfe::image::ImageManager;
using pdfe::image::InsertImageOptions;

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

// Upright 200x120 placement at (150, 200), fitz Y-down.
const QRectF kRect(150.0, 200.0, 200.0, 120.0);

bool alphaIsBackground(QRgb c) {
    return qAlpha(c) < 30 || (qRed(c) > 230 && qGreen(c) > 230 && qBlue(c) > 230);
}
bool pixRed(QRgb c) {
    return qAlpha(c) > 200 && qRed(c) > 200 && qGreen(c) < 90 && qBlue(c) < 90;
}
bool pixMagenta(QRgb c) {
    return qAlpha(c) > 200 && qRed(c) > 200 && qGreen(c) < 90 && qBlue(c) > 200;
}

// Bounding box (page pixels == points at scale 1) of the pixels `pred` accepts.
template <class Pred>
QRect pixelBounds(const QImage& img, Pred pred, int* count = nullptr) {
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1, n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (pred(img.pixel(x, y))) {
                ++n;
                x0 = std::min(x0, x); y0 = std::min(y0, y);
                x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
    if (count) *count = n;
    return n ? QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1) : QRect();
}

// Raw MuPDF edit of page 0: open `in`, run edit(ctx, pdoc, page, mediaBox), save
// to `out`.
template <class F>
bool rawEdit(const MuPdfContext& mc, const std::string& in, const std::string& out, F edit) {
    bool ok = false;
    mc.execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_open_document(ctx, in.c_str());
        pdf_page* pp = pdf_load_page(ctx, pdoc, 0);
        const fz_rect mb = pdf_bound_page(ctx, pp, FZ_MEDIA_BOX);
        edit(ctx, pdoc, pp, mb);
        pdf_write_options wo = pdf_default_write_options;
        pdf_save_document(ctx, pdoc, out.c_str(), &wo);
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, pdoc);
        ok = true;
    });
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Shared fixture
// ---------------------------------------------------------------------------
class TreStateBase : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApplication();
        ASSERT_TRUE(tmp_.isValid());
        png_ = tmp_.filePath("red.png").toStdString();
        QImage red(64, 64, QImage::Format_RGB32);
        red.fill(QColor(255, 0, 0));
        ASSERT_TRUE(red.save(QString::fromStdString(png_)));
        base_ = fixture("test_single.pdf");
        ASSERT_FALSE(base_.empty()) << "fixture test_single.pdf not found";
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
        editor_ = std::make_unique<ImageContentEditor>(context_);
        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        writer_ = std::make_unique<pdfe::tre::PdfContentWriter>(context_.get(), fontEmbedder_.get());
    }

    void TearDown() override {
        writer_.reset();
        fontEmbedder_.reset();
        page_.reset();
        doc_.reset();
    }

    void openBlank() {
        doc_ = Document::open(context_, base_);
        ASSERT_NE(doc_, nullptr);
        reload();
        ASSERT_NE(page_, nullptr);
    }
    void reload() { page_ = doc_->page(0); }

    void insertRed(const QRectF& r) {
        InsertImageOptions o;
        o.filePath = png_;
        o.targetRect = r;
        o.maintainAspectRatio = false;
        auto res = manager_->insertImage(page_.get(), doc_.get(), o);
        ASSERT_TRUE(res.success) << res.errorMessage;
        reload();
    }

    std::string path(const char* leaf) { return tmp_.filePath(leaf).toStdString(); }

    // save doc_ -> raw edit -> reopen as doc_.
    template <class F>
    void editAndReopen(const char* tag, F edit) {
        const std::string in = path((std::string(tag) + "_in.pdf").c_str());
        const std::string out = path((std::string(tag) + "_out.pdf").c_str());
        page_.reset();
        doc_->saveAs(in);
        ASSERT_TRUE(rawEdit(*context_, in, out, edit)) << "raw MuPDF edit failed";
        doc_ = Document::open(context_, out);
        ASSERT_NE(doc_, nullptr);
        reload();
        ASSERT_NE(page_, nullptr);
    }

    QImage renderPage() {
        reload();
        return page_->render(QRectF(), 1.0f);
    }
    std::string firstImageName() {
        reload();
        auto v = manager_->enumerateImages(page_.get(), doc_.get());
        return v.empty() ? std::string() : v[0].xobjectName;
    }

    // Draw the XObject `name` a second time, 250 points lower.
    void drawAgain(const std::string& name, const QRectF& second) {
        editAndReopen("twice", [&](fz_context* ctx, pdf_document* pdoc, pdf_page* pp, fz_rect mb) {
            std::ostringstream ops;
            ops << "q " << second.width() << " 0 0 " << second.height() << " "
                << (mb.x0 + second.x()) << " " << (mb.y1 - second.y() - second.height())
                << " cm /" << name << " Do Q\n";
            const std::string text = ops.str();
            appendContentIsolatingAmbientCtm(ctx, pdoc, pp->obj, text.c_str(), text.size());
        });
    }

    QTemporaryDir tmp_;
    std::string png_, base_;
    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::unique_ptr<ImageContentEditor> editor_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> fontEmbedder_;
    std::unique_ptr<pdfe::tre::PdfContentWriter> writer_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
};

// The ImageReplacer stack, built on the CURRENT doc_ (build it after any reopen).
struct ReplacerRig {
    ReplacerRig(const std::shared_ptr<MuPdfContext>& ctx, Document* doc) {
        using namespace pdfe::tre;
        ocr = std::make_unique<OcrPipeline>(ctx.get(), doc);
        coord = std::make_unique<CoordMapper>(ctx.get(), doc);
        embedder = std::make_unique<pdfe::fonts::PdfFontEmbedder>(ctx.get());
        writer = std::make_unique<PdfContentWriter>(ctx.get(), embedder.get());
        cache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(ctx.get());
        selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(analyzer.get(), cache.get());
        matcher = std::make_unique<FontMatcher>(selector.get(), analyzer.get());
        bg = std::make_unique<BgColorDetector>();
        masker = std::make_unique<ImageMasker>();
        compositor = std::make_unique<BitmapCompositor>();
        replacer = std::make_unique<ImageReplacer>(ocr.get(), coord.get(), bg.get(), matcher.get(),
                                                   masker.get(), compositor.get(), writer.get());
    }
    std::unique_ptr<pdfe::tre::OcrPipeline> ocr;
    std::unique_ptr<pdfe::tre::CoordMapper> coord;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> embedder;
    std::unique_ptr<pdfe::tre::PdfContentWriter> writer;
    std::unique_ptr<pdfe::fonts::FontAnalysisCache> cache;
    std::unique_ptr<pdfe::fonts::EmbeddedFontAnalyzer> analyzer;
    std::unique_ptr<pdfe::fonts::SubstituteFontSelector> selector;
    std::unique_ptr<pdfe::tre::FontMatcher> matcher;
    std::unique_ptr<pdfe::tre::BgColorDetector> bg;
    std::unique_ptr<pdfe::tre::ImageMasker> masker;
    std::unique_ptr<pdfe::tre::BitmapCompositor> compositor;
    std::unique_ptr<pdfe::tre::ImageReplacer> replacer;
};

class TreReplaceStateTest : public TreStateBase {
protected:
    // Fill the page rect `sel` (fitz points) with magenta through the real
    // ImageReplacer write-back (no OCR: replaceWithFontMatch). Text and
    // background are both magenta and originalText is non-empty, so the whole
    // selection is painted. Returns the rendered page afterwards.
    QImage magentaEdit(const QRectF& sel) {
        using namespace pdfe::tre;
        ReplacerRig rig(context_, doc_.get());
        FontMatch fm;
        fm.fontFamily = "Arial";
        fm.fontStyle = "Regular";
        fm.fontFilePath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
        EXPECT_FALSE(fm.fontFilePath.empty()) << "no Arial-equivalent font on this host";
        fm.sizePt = 12.0f;
        fm.color = RgbColor{255, 0, 255};
        fm.confidence = 0.8f;
        BackgroundInfo bg;
        bg.dominantColor = RgbColor{255, 0, 255};
        bg.isUniform = true;
        bg.confidence = 0.9f;
        PdfRect pr;
        pr.x0 = sel.left(); pr.y0 = sel.top(); pr.x1 = sel.right(); pr.y1 = sel.bottom();
        reload();
        const ReconstructionResult r = rig.replacer->replaceWithFontMatch(
            page_.get(), doc_.get(), 0, pr, "WWWWWWWW", fm, bg, RegionId{7}, {}, "WWWWWWWW");
        EXPECT_TRUE(r.success) << r.errorMessage;
        return renderPage();
    }

    // The edit must change pixels inside `sel` and none outside it (+ margin).
    void expectMagentaOnlyInside(const QImage& after, const QRectF& sel, const char* what) {
        int total = 0;
        const QRect bounds = pixelBounds(after, pixMagenta, &total);
        ASSERT_GT(total, 0) << what << ": the edit produced no magenta pixels anywhere";
        const QRectF allowed = sel.adjusted(-6, -6, 6, 6);
        int outside = 0, inside = 0;
        for (int y = 0; y < after.height(); ++y)
            for (int x = 0; x < after.width(); ++x)
                if (pixMagenta(after.pixel(x, y))) {
                    if (allowed.contains(QPointF(x + 0.5, y + 0.5))) ++inside; else ++outside;
                }
        EXPECT_EQ(outside, 0)
            << what << ": " << outside << " edited pixels landed OUTSIDE the edit rect (edit rect "
            << sel.x() << "," << sel.y() << " " << sel.width() << "x" << sel.height()
            << "; edited pixels span " << bounds.x() << "," << bounds.y() << " "
            << bounds.width() << "x" << bounds.height() << ")";
        EXPECT_GT(inside, 0) << what << ": no edited pixel landed inside the edit rect";
    }
};

// ===========================================================================
// T1  frame of placementFitz
// ===========================================================================
TEST_F(TreReplaceStateTest, PlacementFrameFollowsMediaBoxOrigin) {
    openBlank();
    insertRed(kRect);
    editAndReopen("mb", [](fz_context* ctx, pdf_document*, pdf_page* pp, fz_rect) {
        pdf_dict_put_rect(ctx, pp->obj, PDF_NAME(MediaBox),
                          fz_make_rect(50.0f, 100.0f, 50.0f + 595.28f, 100.0f + 841.89f));
    });
    reload();
    auto imgs = manager_->enumerateImages(page_.get(), doc_.get());
    ASSERT_EQ(imgs.size(), 1u);
    const QRectF expected = imgs[0].boundingBox;  // ImageManager frame (MediaBox aware)
    const QImage render = renderPage();
    ASSERT_TRUE(pixRed(render.pixel(int(expected.center().x()), int(expected.center().y()))))
        << "oracle: the image must render red at ImageManager's rect centre";
    int n = 0;
    const QRect shown = pixelBounds(render, pixRed, &n);
    ASSERT_GT(n, 0);
    ASSERT_NEAR(shown.x(), expected.x(), 2.0) << "oracle: render agrees with ImageManager (x)";
    ASSERT_NEAR(shown.y(), expected.y(), 2.0) << "oracle: render agrees with ImageManager (y)";

    const auto got = writer_->findImageXObjectForRegion(
        page_.get(), doc_.get(), QRectF(0, 0, render.width(), render.height()));
    ASSERT_TRUE(got.has_value());
    ASSERT_FALSE(got->placementFitz.isEmpty());
    EXPECT_NEAR(got->placementFitz.x(), expected.x(), 1.0)
        << "placementFitz ignores the MediaBox x origin";
    EXPECT_NEAR(got->placementFitz.y(), expected.y(), 1.0)
        << "placementFitz flips with the page height, not the MediaBox top: it is "
        << (expected.y() - got->placementFitz.y()) << " pt off";
    EXPECT_NEAR(got->placementFitz.width(), expected.width(), 1.0);
    EXPECT_NEAR(got->placementFitz.height(), expected.height(), 1.0);
}

TEST_F(TreReplaceStateTest, PlacementFrameFollowsPageRotate) {
    openBlank();
    insertRed(kRect);
    editAndReopen("rot", [](fz_context* ctx, pdf_document*, pdf_page* pp, fz_rect) {
        pdf_dict_put_int(ctx, pp->obj, PDF_NAME(Rotate), 90);
    });
    const QImage render = renderPage();
    int n = 0;
    const QRect shown = pixelBounds(render, pixRed, &n);
    ASSERT_GT(n, 0) << "oracle: the image must render";
    ASSERT_NEAR(shown.width(), kRect.height(), 3.0) << "oracle: /Rotate 90 swaps the extent";
    ASSERT_NEAR(shown.height(), kRect.width(), 3.0);

    const auto got = writer_->findImageXObjectForRegion(
        page_.get(), doc_.get(), QRectF(0, 0, render.width(), render.height()));
    ASSERT_TRUE(got.has_value());
    ASSERT_FALSE(got->placementFitz.isEmpty());
    EXPECT_NEAR(got->placementFitz.x(), shown.x(), 2.0)
        << "placementFitz is in the unrotated frame; the page renders the image at x=" << shown.x();
    EXPECT_NEAR(got->placementFitz.y(), shown.y(), 2.0)
        << "placementFitz is in the unrotated frame; the page renders the image at y=" << shown.y();
    EXPECT_NEAR(got->placementFitz.width(), shown.width(), 2.0);
    EXPECT_NEAR(got->placementFitz.height(), shown.height(), 2.0);
}

// ===========================================================================
// T2  write-back of an edit rect into a rotated / flipped placement
// ===========================================================================
TEST_F(TreReplaceStateTest, WriteBackLandsWhereEditedOnUprightImageControl) {
    openBlank();
    insertRed(kRect);
    const QRectF sel(155, 205, 40, 30);  // top-left corner of the image
    ASSERT_TRUE(pixRed(renderPage().pixel(int(sel.center().x()), int(sel.center().y()))));
    expectMagentaOnlyInside(magentaEdit(sel), sel, "upright control");
}

TEST_F(TreReplaceStateTest, WriteBackLandsWhereEditedOnRotatedImage) {
    openBlank();
    insertRed(kRect);
    const std::string name = firstImageName();
    ASSERT_FALSE(name.empty());
    const QPointF c = kRect.center();
    ASSERT_TRUE(editor_->rotateImage(page_.get(), doc_.get(), name, 90.0f, float(c.x()), float(c.y()),
                                     float(kRect.width()), float(kRect.height())));
    reload();
    auto imgs = manager_->enumerateImages(page_.get(), doc_.get());
    ASSERT_EQ(imgs.size(), 1u);
    ASSERT_GT(std::abs(imgs[0].rotationRadians), 1.5f) << "precondition: the image is rotated 90 degrees";
    // Rotated extent: 120 wide x 200 tall about the same centre.
    const QRectF sel(195, 165, 40, 30);  // top-left corner of the rotated extent
    const QImage before = renderPage();
    ASSERT_TRUE(pixRed(before.pixel(int(sel.center().x()), int(sel.center().y()))))
        << "oracle: the edit rect lies over the rotated image";
    ASSERT_TRUE(alphaIsBackground(before.pixel(int(kRect.left()) + 5, int(kRect.top()) + 5)))
        << "oracle: the image really is rotated (its old corner is empty)";
    expectMagentaOnlyInside(magentaEdit(sel), sel, "rotated 90");
}

TEST_F(TreReplaceStateTest, WriteBackLandsWhereEditedOnFlippedImage) {
    openBlank();
    insertRed(kRect);
    const std::string name = firstImageName();
    ASSERT_FALSE(name.empty());
    // Same rectangle, drawn vertically flipped (d < 0): replace the page content.
    editAndReopen("flip", [&](fz_context* ctx, pdf_document* pdoc, pdf_page* pp, fz_rect mb) {
        std::ostringstream ops;
        ops << "q 200 0 0 -120 " << (mb.x0 + 150.0) << " " << (mb.y1 - 200.0) << " cm /" << name
            << " Do Q\n";
        const std::string text = ops.str();
        fz_buffer* buf = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(text.data()), text.size());
        pdf_obj* stream = pdf_add_stream(ctx, pdoc, buf, nullptr, 0);
        fz_drop_buffer(ctx, buf);
        pdf_dict_put(ctx, pp->obj, PDF_NAME(Contents), stream);
        pdf_drop_obj(ctx, stream);
    });
    const QRectF sel(155, 205, 40, 30);  // top-left corner of the flipped image
    const QImage before = renderPage();
    ASSERT_TRUE(pixRed(before.pixel(int(sel.center().x()), int(sel.center().y()))))
        << "oracle: the flipped image covers the edit rect";
    ASSERT_TRUE(pixRed(before.pixel(int(kRect.center().x()), int(kRect.center().y()))));
    expectMagentaOnlyInside(magentaEdit(sel), sel, "vertically flipped");
}

// ===========================================================================
// T3  /SMask survives a replacePageImage
// ===========================================================================
TEST_F(TreReplaceStateTest, ReplaceKeepsTheImageSoftMask) {
    openBlank();
    editAndReopen("smask", [](fz_context* ctx, pdf_document* pdoc, pdf_page* pp, fz_rect mb) {
        const int W = 64, H = 64;
        std::vector<unsigned char> rgb(W * H * 3), alpha(W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                rgb[(y * W + x) * 3 + 0] = 255;
                rgb[(y * W + x) * 3 + 1] = 0;
                rgb[(y * W + x) * 3 + 2] = 0;
                alpha[y * W + x] = x < W / 2 ? 0 : 255;  // left half masked out
            }
        fz_buffer* mbuf = fz_new_buffer_from_copied_data(ctx, alpha.data(), alpha.size());
        pdf_obj* smask = pdf_add_stream(ctx, pdoc, mbuf, nullptr, 0);
        fz_drop_buffer(ctx, mbuf);
        pdf_dict_put(ctx, smask, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, smask, PDF_NAME(Subtype), PDF_NAME(Image));
        pdf_dict_put_int(ctx, smask, PDF_NAME(Width), W);
        pdf_dict_put_int(ctx, smask, PDF_NAME(Height), H);
        pdf_dict_put(ctx, smask, PDF_NAME(ColorSpace), PDF_NAME(DeviceGray));
        pdf_dict_put_int(ctx, smask, PDF_NAME(BitsPerComponent), 8);

        fz_buffer* ibuf = fz_new_buffer_from_copied_data(ctx, rgb.data(), rgb.size());
        pdf_obj* img = pdf_add_stream(ctx, pdoc, ibuf, nullptr, 0);
        fz_drop_buffer(ctx, ibuf);
        pdf_dict_put(ctx, img, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, img, PDF_NAME(Subtype), PDF_NAME(Image));
        pdf_dict_put_int(ctx, img, PDF_NAME(Width), W);
        pdf_dict_put_int(ctx, img, PDF_NAME(Height), H);
        pdf_dict_put(ctx, img, PDF_NAME(ColorSpace), PDF_NAME(DeviceRGB));
        pdf_dict_put_int(ctx, img, PDF_NAME(BitsPerComponent), 8);
        pdf_dict_put(ctx, img, PDF_NAME(SMask), smask);

        pdf_obj* res = pdf_page_resources(ctx, pp);
        pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
        if (!xo) xo = pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 2);
        pdf_dict_puts(ctx, xo, "ImSm", img);
        pdf_drop_obj(ctx, img);
        pdf_drop_obj(ctx, smask);

        std::ostringstream ops;
        ops << "q 200 0 0 120 " << (mb.x0 + 150.0) << " " << (mb.y1 - 320.0) << " cm /ImSm Do Q\n";
        const std::string text = ops.str();
        appendContentIsolatingAmbientCtm(ctx, pdoc, pp->obj, text.c_str(), text.size());
    });
    const QPointF masked(kRect.left() + 0.25 * kRect.width(), kRect.center().y());
    const QPointF opaque(kRect.left() + 0.75 * kRect.width(), kRect.center().y());
    const QImage before = renderPage();
    ASSERT_TRUE(alphaIsBackground(before.pixel(int(masked.x()), int(masked.y()))))
        << "oracle: the masked half is transparent before the replace";
    ASSERT_TRUE(pixRed(before.pixel(int(opaque.x()), int(opaque.y()))))
        << "oracle: the unmasked half is red before the replace";

    // Replace with the image's own colour pixels (all red): nothing should change.
    QImage same(64, 64, QImage::Format_RGB888);
    same.fill(QColor(255, 0, 0));
    reload();
    ASSERT_TRUE(writer_->replacePageImage(page_.get(), doc_.get(), same));

    const QImage after = renderPage();
    EXPECT_TRUE(pixRed(after.pixel(int(opaque.x()), int(opaque.y())))) << "the opaque half must stay red";
    EXPECT_TRUE(alphaIsBackground(after.pixel(int(masked.x()), int(masked.y()))))
        << "the masked-out half is now painted (alpha=" << qAlpha(after.pixel(int(masked.x()), int(masked.y())))
        << "): the /SMask was dropped by replacePageImage";
}

// ===========================================================================
// T4  one XObject drawn twice: a placement-targeted replace changes both
// ===========================================================================
TEST_F(TreReplaceStateTest, ReplaceTargetsOnlyTheEditedPlacement) {
    openBlank();
    insertRed(kRect);
    const std::string name = firstImageName();
    ASSERT_FALSE(name.empty());
    const QRectF second = kRect.translated(0.0, 250.0);
    drawAgain(name, second);
    const QImage before = renderPage();
    ASSERT_TRUE(pixRed(before.pixel(int(kRect.center().x()), int(kRect.center().y()))));
    ASSERT_TRUE(pixRed(before.pixel(int(second.center().x()), int(second.center().y()))))
        << "precondition: the image is drawn twice";

    // The target is resolved for the FIRST placement; it carries placementFitz.
    const auto target = writer_->findImageXObjectForRegion(page_.get(), doc_.get(), kRect);
    ASSERT_TRUE(target.has_value());
    ASSERT_NEAR(target->placementFitz.y(), kRect.y(), 2.0) << "precondition: target is the first placement";
    QImage magenta(64, 64, QImage::Format_RGB888);
    magenta.fill(QColor(255, 0, 255));
    ASSERT_TRUE(writer_->replacePageImage(page_.get(), doc_.get(), magenta, *target));

    const QImage after = renderPage();
    ASSERT_TRUE(pixMagenta(after.pixel(int(kRect.center().x()), int(kRect.center().y()))))
        << "oracle: the targeted placement took the new pixels";
    EXPECT_TRUE(pixRed(after.pixel(int(second.center().x()), int(second.center().y()))))
        << "the OTHER placement of the same XObject changed too: replacePageImage rewrites the "
           "shared resource entry for every Do of that name";
}

// ===========================================================================
// T5  whole-page render written into a smaller image (RegionReconstructor path)
// ===========================================================================
TEST_F(TreReplaceStateTest, WholePageRenderIsNotWrittenIntoASmallerImage) {
    openBlank();
    const QRectF small(200.0, 400.0, 100.0, 60.0);  // text sits at the top of the page
    insertRed(small);
    ReplacerRig rig(context_, doc_.get());
    const QImage whole = rig.ocr->renderPage(0);  // what RegionReconstructor.cpp:138 renders
    ASSERT_FALSE(whole.isNull());
    ASSERT_GT(whole.width(), 500) << "precondition: a whole-page render";
    const QImage before = renderPage();
    ASSERT_TRUE(pixRed(before.pixel(int(small.center().x()), int(small.center().y()))));

    // RegionReconstructor.cpp:173-175: replacePageImage(page, document, maskedImage).
    reload();
    ASSERT_TRUE(rig.writer->replacePageImage(page_.get(), doc_.get(), whole));

    const QImage after = renderPage();
    int red = 0, total = 0;
    for (int y = int(small.top()) + 2; y < int(small.bottom()) - 2; ++y)
        for (int x = int(small.left()) + 2; x < int(small.right()) - 2; ++x) {
            ++total;
            if (pixRed(after.pixel(x, y))) ++red;
        }
    ASSERT_GT(total, 0);
    EXPECT_GE(double(red) / total, 0.95)
        << "only " << red << " of " << total << " pixels inside the image's own rect are still the "
           "image: a whole-page render (text, margins and all) was squeezed into it";
}

// ===========================================================================
// G1  ContentStreamTagger::tagPageContent
// ===========================================================================
class TaggerStateTest : public TreStateBase {
protected:
    // Concatenated, decoded page content of the saved doc_.
    std::string savedContent() {
        const std::string p = path("content_probe.pdf");
        page_.reset();
        doc_->saveAs(p);
        reload();
        auto holder = std::make_shared<std::string>();
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdoc = pdf_open_document(ctx, p.c_str());
            pdf_page* pp = pdf_load_page(ctx, pdoc, 0);
            pdf_obj* contents = pdf_dict_get(ctx, pp->obj, PDF_NAME(Contents));
            std::vector<pdf_obj*> parts;
            if (pdf_is_array(ctx, contents)) {
                for (int i = 0; i < pdf_array_len(ctx, contents); ++i)
                    parts.push_back(pdf_array_get(ctx, contents, i));
            } else if (contents) {
                parts.push_back(contents);
            }
            for (pdf_obj* s : parts) {
                fz_buffer* b = pdf_load_stream(ctx, s);
                unsigned char* d = nullptr;
                const size_t n = fz_buffer_storage(ctx, b, &d);
                holder->append(reinterpret_cast<const char*>(d), n);
                holder->push_back('\n');
                fz_drop_buffer(ctx, b);
            }
            fz_drop_page(ctx, &pp->super);
            pdf_drop_document(ctx, pdoc);
        });
        return *holder;
    }

    // Operator count of a content string (comments and (strings) stripped).
    static int countOperators(const std::string& content) {
        std::string s;
        bool inStr = false, inCom = false;
        int depth = 0;
        for (char ch : content) {
            if (inCom) { if (ch == '\n' || ch == '\r') inCom = false; continue; }
            if (inStr) {
                if (ch == '(') ++depth;
                if (ch == ')' && --depth == 0) inStr = false;
                continue;
            }
            if (ch == '%') { inCom = true; continue; }
            if (ch == '(') { inStr = true; depth = 1; continue; }
            s.push_back(ch);
        }
        int n = 0;
        std::istringstream in(s);
        std::string tok;
        while (in >> tok) {
            if (tok[0] == '/' || tok[0] == '<' || tok[0] == '[' || tok[0] == ']' ||
                std::isdigit(static_cast<unsigned char>(tok[0])) || tok[0] == '-' || tok[0] == '.' ||
                tok[0] == '+')
                continue;
            ++n;
        }
        return n;
    }

    // Content between "/<tag> <</MCID <mcid>>> BDC" and its matching EMC.
    // Returns false when the BDC is not present.
    static bool mcidSegment(const std::string& c, const char* tag, int mcid, std::string* out) {
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
        *out = rest;  // unterminated
        return true;
    }

    static bool hasDo(const std::string& seg) {
        return std::regex_search(seg, std::regex("(^|\\s)Do(\\s|$)"));
    }

    // Tag a page holding text and one image: P (mcid 0) = text, Figure (mcid 1) = image.
    void tag(std::string* content) {
        openBlank();
        insertRed(kRect);
        const std::string pre = savedContent();
        ASSERT_TRUE(hasDo(pre)) << "precondition: the page draws an image";
        ASSERT_NE(pre.find("Tj"), std::string::npos) << "precondition: the page has text";
        // Operator indices into the pre-tag stream: text first, the image last.
        const size_t doPos = pre.rfind(" cm");
        const int textOps = countOperators(pre.substr(0, pre.rfind('q', doPos)));
        const int allOps = countOperators(pre);
        pdfe::compliance::ContentStreamTagger tagger(context_);
        std::vector<pdfe::compliance::MarkedContentRegion> regions;
        regions.push_back({pdfe::compliance::StructureTag::P, 0, 0, textOps});
        regions.push_back({pdfe::compliance::StructureTag::Figure, 1, textOps, allOps});
        tagger.tagPageContent(doc_.get(), 0, regions);
        *content = savedContent();
    }
};

TEST_F(TaggerStateTest, FigureMcidEnclosesTheImageDo) {
    std::string content;
    tag(&content);
    ASSERT_FALSE(content.empty());
    std::string fig;
    ASSERT_TRUE(mcidSegment(content, "Figure", 1, &fig)) << "precondition: the Figure BDC is emitted";
    EXPECT_TRUE(hasDo(fig))
        << "the /Figure MCID 1 sequence does not enclose the image's Do (it encloses: ["
        << fig << "]): the figure's MCID points at nothing";
}

TEST_F(TaggerStateTest, TextMcidDoesNotEncloseTheImageDo) {
    std::string content;
    tag(&content);
    ASSERT_FALSE(content.empty());
    std::string text;
    ASSERT_TRUE(mcidSegment(content, "P", 0, &text)) << "precondition: the P BDC is emitted";
    ASSERT_NE(text.find("Tj"), std::string::npos) << "precondition: the P sequence holds the text";
    EXPECT_FALSE(hasDo(text))
        << "the text's MCID 0 sequence also encloses the image's Do: the whole stream was wrapped "
           "in the first region's BDC/EMC";
}
