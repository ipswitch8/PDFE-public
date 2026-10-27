// -*- coding: utf-8 -*-
// Regression test for the INBOUND ambient-CTM leak in image insertion
// (Register H1 / docs/content-stream-state-continuity-audit.md, pipeline
// e5b9c231 Phase 2).
//
// Image placement appends "q [w 0 0 h x y] cm /Img Do Q" as a raw content
// stream. The absolute `cm` assumes the CTM is identity when the fragment runs.
// On a page that leaves an ambient transform active at the top level (the
// "N-up / imposed" pattern) the fragment inherits it, so the image is drawn at
// the wrong size/position -- e.g. an ambient "0.5 0 0 0.5 0 0 cm" halves it.
//
// Fix: appendContentIsolatingAmbientCtm() brackets the existing content in
// q...Q so every appended fragment starts at identity CTM.
//
// Oracle: insert a 200-pt-wide image onto the half-scale-ambient fixture,
// render, and measure the image's rendered width. Under the leak it is ~100 px
// (halved); after the fix it is ~200 px.
#include <gtest/gtest.h>
#include "image/ImageManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QImage>
#include <filesystem>
#include <memory>
#include <algorithm>
#include <cstdlib>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace pdfe::image;
using namespace pdfe::core;

namespace {

// First-glyph geometry+color of a line, for the untouched-neighbor oracle.
struct Glyph { bool found = false; float ulx = 0, urx = 0, lly = 0; uint32_t argb = 0; };
Glyph readGlyph(const std::filesystem::path& pdf, const std::u32string& needle) {
    Glyph gg;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr; fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, pdf.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, 0, nullptr);
        for (fz_stext_block* b = stext->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
                std::u32string t;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next)
                    t.push_back(static_cast<char32_t>(ch->c));
                if (t.find(needle) == std::u32string::npos) continue;
                fz_stext_char* f = ln->first_char;
                gg.found = true; gg.ulx = f->quad.ul.x; gg.urx = f->quad.ur.x;
                gg.lly = f->quad.ll.y; gg.argb = f->argb & 0xFFFFFFu;
            }
        }
    }
    fz_always(ctx) { if (stext) fz_drop_stext_page(ctx, stext); if (doc) fz_drop_document(ctx, doc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return gg;
}

// Bounding box (device px) of the inserted image: the test PNG is a gradient
// with green channel ~100 and clearly non-gray R/B; exclude white bg + black text.
struct BBox { int w = 0; int h = 0; int count = 0; };
BBox imageBBox(const QImage& im) {
    int minx = im.width(), miny = im.height(), maxx = -1, maxy = -1, count = 0;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            QRgb p = im.pixel(x, y);
            int r = qRed(p), g = qGreen(p), b = qBlue(p);
            if (g > 70 && g < 130 && (std::abs(r - g) > 25 || std::abs(b - g) > 25)) {
                minx = std::min(minx, x); miny = std::min(miny, y);
                maxx = std::max(maxx, x); maxy = std::max(maxy, y); ++count;
            }
        }
    BBox bb;
    if (maxx >= 0) { bb.w = maxx - minx + 1; bb.h = maxy - miny + 1; bb.count = count; }
    return bb;
}

} // namespace

TEST(ImageCtmLeak, InsertedImageNotHalvedByAmbientScale) {
    auto ctx = std::make_shared<MuPdfContext>();
    std::filesystem::path src =
        std::filesystem::path(TEST_FIXTURES_DIR) / "ambient_half_scale_page.pdf";
    std::filesystem::path png =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_image_rgb.png";
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();
    ASSERT_TRUE(std::filesystem::exists(png)) << png.string();

    auto doc = Document::open(ctx, src);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    ImageManager mgr(ctx);
    InsertImageOptions opts;
    opts.filePath = png.string();
    opts.targetRect = QRectF(100, 400, 200, 160);  // 200 pt wide in PDF space
    opts.maintainAspectRatio = false;
    auto res = mgr.insertImage(page.get(), doc.get(), opts);
    ASSERT_TRUE(res.success) << res.errorMessage;

    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_image_ctm_leak.pdf";
    doc->saveAs(out);
    page.reset(); doc.reset();

    auto doc2 = Document::open(ctx, out);
    ASSERT_NE(doc2, nullptr);
    auto page2 = doc2->page(0);
    ASSERT_NE(page2, nullptr);
    QImage im = page2->render(page2->boundingBox(), 1.0f);
    ASSERT_FALSE(im.isNull());

    BBox bb = imageBBox(im);
    ASSERT_GT(bb.count, 1000) << "inserted image not found in the render";
    EXPECT_GT(bb.w, 150)
        << "inserted image is only " << bb.w << " px wide (expected ~200) -- "
           "ambient 0.5x CTM leaked into the image placement fragment";
}

// M-b untouched-neighbor: the CTM guard brackets the existing page content in
// q...Q; that must be transparent -- the pre-existing "PAGE" text must render
// identically after the insert.
TEST(ImageCtmLeak, UntouchedNeighborUnchangedByCtmGuard) {
    auto ctx = std::make_shared<MuPdfContext>();
    std::filesystem::path src =
        std::filesystem::path(TEST_FIXTURES_DIR) / "ambient_half_scale_page.pdf";
    std::filesystem::path png =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_image_rgb.png";
    Glyph before = readGlyph(src, U"PAGE");
    ASSERT_TRUE(before.found) << "fixture must contain neighbor 'PAGE' text";

    auto doc = Document::open(ctx, src);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ImageManager mgr(ctx);
    InsertImageOptions opts;
    opts.filePath = png.string();
    opts.targetRect = QRectF(100, 400, 200, 160);
    opts.maintainAspectRatio = false;
    ASSERT_TRUE(mgr.insertImage(page.get(), doc.get(), opts).success);
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_image_ctm_nbr.pdf";
    doc->saveAs(out);
    page.reset(); doc.reset();

    Glyph after = readGlyph(out, U"PAGE");
    ASSERT_TRUE(after.found) << "neighbor 'PAGE' missing after insert";
    EXPECT_EQ(after.argb, before.argb) << "neighbor color changed by insert/guard";
    EXPECT_NEAR(after.ulx, before.ulx, 1.0f) << "neighbor x moved by insert/guard";
    EXPECT_NEAR(after.lly, before.lly, 1.0f) << "neighbor y moved by insert/guard";
    EXPECT_NEAR(after.urx - after.ulx, before.urx - before.ulx, 1.0f)
        << "neighbor glyph advance/scale changed by insert/guard";
}
