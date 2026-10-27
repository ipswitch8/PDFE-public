// -*- coding: utf-8 -*-
//
// R3-23 (fidelity): IMAGE-PIXEL redaction absence oracle.
//
// The existing redaction tests (test_redaction_completeness.cpp,
// test_image_redaction_phase7.cpp) prove only that redacted *text bytes* are
// gone from the DECOMPRESSED content stream. NOTHING asserts that the *image
// samples* under a redaction box are actually destroyed. A "redaction" that
// removes text operators but leaves the underlying image pixels intact would
// pass every current test yet still leak the sensitive raster underneath.
//
// This file adds the missing PIXEL-ABSENCE oracle: after applying an
// image-pixel redaction over a sub-region of an image, render the page and
// verify that every interior pixel of the redacted region has been WIPED to the
// cleared fill colour — i.e. the original image samples are truly gone — while
// image samples OUTSIDE the redaction rect survive (the wipe is localized, not
// a full-image blank).
//
// kills a mutation where image redaction leaves original samples (imageMethod
// ignored → the region would still show the original light-blue fill).
//
// WHAT COLOUR IS THE WIPE? MuPDF 1.26's PDF_REDACT_IMAGE_PIXELS
// (pdf_redact_image_imp in source/pdf/pdf-clean.c) overwrites the covered image
// samples with the colorspace's "white" — 0 for a subtractive space, 255 for an
// additive one. The fixture image is /DeviceRGB (additive), so the destroyed
// region renders WHITE rgb(255,255,255), NOT black. (An earlier version of this
// oracle asserted black; that was a wrong assumption about MuPDF semantics, not
// a product defect — the original samples are genuinely gone, just replaced by
// white rather than black.) The security property R3-23 requires is ABSENCE of
// the ORIGINAL content, which white-clearing satisfies exactly.
//
// NOTE ON black_boxes: applyPageRedactions with options.blackBoxes == true
// paints an opaque overlay rectangle over the annotation area for ANY content
// type, which would blank the region regardless of imageMethod (masking whether
// the image samples were actually destroyed). To make this a genuine
// image-pixel oracle we set blackBoxes == false, so the ONLY thing that can
// change the image region is the RedactionImageMethod::Pixels sample
// destruction. With the overlay disabled: Pixels -> region wiped to white;
// None -> region stays the original light-blue image; Remove -> whole XObject
// dropped, region renders as bare page background (also white). Both Pixels and
// Remove satisfy absence; None (the mutation) leaves the original light-blue and
// FAILS the "no interior pixel still shows the original fill" assertion.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "redaction/RedactionData.h"
#include "redaction/RedactionManager.h"

#include "support/PdfContentOracle.h"

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;

namespace {

// Page 2 (index 1) of test_redaction.pdf draws its image with the content
// operator  `q 200 0 0 150 100 500 cm /Im0 Do Q`  on a 612x792 MediaBox.
// That places the image in PDF (Y-up) space at x in [100,300], y in [500,650].
// In fitz / display space (Y-down, top-left origin — the space that
// createRedactionMark rects and the render() pixel grid both use) the image
// occupies x in [100,300], y in [792-650, 792-500] = [142,292].
constexpr int   kImagePageIndex = 1;
constexpr float kRenderScale    = 2.0f;

// A redaction rect fully INSIDE the image (fitz space, Y-down):
// x in [140,260] (subset of [100,300]), y in [175,255] (subset of [142,292]).
const QRectF kRedactRect(140.0, 175.0, 120.0, 80.0);

// Sample-inset to skip antialiasing at the redaction-box edges, and per-channel
// tolerance for the colour-match tests. The tolerance only absorbs render
// dithering / resampling.
constexpr double kInsetFrac = 0.20;   // ignore outer 20% of the region
constexpr int    kChanTol   = 40;     // per-channel match tolerance

// The fixture image (see generate_test_redaction_pdf.py, create_image_data)
// fills its background with light blue (200,220,240). This is the ORIGINAL
// content that redaction must destroy.
const QColor kOriginalFill(200, 220, 240);
// PDF_REDACT_IMAGE_PIXELS wipes DeviceRGB (additive) samples to white; the same
// white is also what shows through if the whole XObject is dropped (Remove).
const QColor kClearedFill(255, 255, 255);

// Is a QColor within per-channel tolerance of a target on all three channels?
bool nearColor(const QColor& c, const QColor& t) {
    return std::abs(c.red()   - t.red())   <= kChanTol &&
           std::abs(c.green() - t.green()) <= kChanTol &&
           std::abs(c.blue()  - t.blue())  <= kChanTol;
}

// light-blue (200,220,240) differs from white on the red channel by 55 > 40, so
// the original fill and the cleared fill are unambiguously distinguishable at
// kChanTol — "is cleared" therefore also means "is NOT the original content".
static_assert(200 + 40 < 255, "original fill must be distinguishable from cleared white");

// Map a fitz-space rect to the rendered pixel grid (render() uses
// transform = fz_scale(scale, scale) with the page origin at fitz (0,0)), then
// shrink it by kInsetFrac on every side and clamp to the image bounds.
QRect pixelInterior(const QRectF& fitzRect, const QImage& img, float scale) {
    double x0 = fitzRect.left()   * scale;
    double y0 = fitzRect.top()    * scale;
    double x1 = fitzRect.right()  * scale;
    double y1 = fitzRect.bottom() * scale;
    const double insetX = (x1 - x0) * kInsetFrac;
    const double insetY = (y1 - y0) * kInsetFrac;
    x0 += insetX; x1 -= insetX;
    y0 += insetY; y1 -= insetY;
    int px0 = std::clamp(static_cast<int>(std::lround(x0)), 0, img.width()  - 1);
    int py0 = std::clamp(static_cast<int>(std::lround(y0)), 0, img.height() - 1);
    int px1 = std::clamp(static_cast<int>(std::lround(x1)), 0, img.width()  - 1);
    int py1 = std::clamp(static_cast<int>(std::lround(y1)), 0, img.height() - 1);
    return QRect(QPoint(px0, py0), QPoint(px1, py1));
}

}  // namespace

class FidelityR3_23_ImageRedaction : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        if (!std::filesystem::exists(fixture_)) {
            GTEST_SKIP() << "Fixture missing: " << fixture_;
        }
        // Unique temp output per test, cleaned up in TearDown.
        out_ = std::filesystem::temp_directory_path() /
               ("pdfe_r3_23_image_redaction_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)) + ".pdf");
        std::error_code ec;
        std::filesystem::remove(out_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(out_, ec);
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path out_;
};

// Primary oracle: image samples under an image-pixel redaction are destroyed.
// Every interior pixel of the redacted sub-region must render as the black fill
// colour on the reopened, saved document.
TEST_F(FidelityR3_23_ImageRedaction, ImagePixelsUnderRedactionAreDestroyed) {
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), kImagePageIndex)
        << "fixture must have an image page at index " << kImagePageIndex;

    auto page = doc->page(kImagePageIndex);
    ASSERT_NE(page, nullptr);

    // Precondition: the region must actually SHOW the original light-blue image
    // content before redaction, otherwise the absence assertion below would be
    // vacuous. Require at least one interior pixel to match the original fill.
    {
        QImage before = page->render(page->boundingBox(), kRenderScale);
        ASSERT_FALSE(before.isNull()) << "pre-redaction render failed";
        const QRect roi = pixelInterior(kRedactRect, before, kRenderScale);
        ASSERT_GT(roi.width(), 0);
        ASSERT_GT(roi.height(), 0);
        bool anyOriginal = false;
        for (int y = roi.top(); y <= roi.bottom() && !anyOriginal; ++y)
            for (int x = roi.left(); x <= roi.right(); ++x)
                if (nearColor(before.pixelColor(x, y), kOriginalFill)) {
                    anyOriginal = true; break;
                }
        ASSERT_TRUE(anyOriginal)
            << "fixture precondition: the redaction region does not show the "
               "expected original light-blue image content before redaction — "
               "the absence oracle would be vacuous";
    }

    // Mark the sub-region and apply an IMAGE-PIXEL redaction with the overlay
    // box DISABLED so ONLY pixel destruction can alter the region (see header).
    pdfe::redaction::RedactionManager mgr(ctx_);
    auto mark = mgr.createRedactionMark(page.get(), kRedactRect);
    ASSERT_NE(mark.annotIndex, -1);

    pdfe::redaction::RedactionApplyOptions opts;
    opts.blackBoxes  = false;                                  // no overlay box
    opts.imageMethod = pdfe::redaction::RedactionImageMethod::Pixels;
    ASSERT_TRUE(mgr.applyPageRedactions(page.get(), opts))
        << "applyPageRedactions must report success";

    doc->saveAs(out_);
    ASSERT_TRUE(std::filesystem::exists(out_));

    // Reopen and render the saved result — assert every interior pixel is black.
    auto reDoc = Document::open(ctx_, out_);
    ASSERT_NE(reDoc, nullptr);
    ASSERT_GT(reDoc->pageCount(), kImagePageIndex);
    auto rePage = reDoc->page(kImagePageIndex);
    ASSERT_NE(rePage, nullptr);

    QImage after = rePage->render(rePage->boundingBox(), kRenderScale);
    ASSERT_FALSE(after.isNull()) << "post-redaction render failed";

    const QRect roi = pixelInterior(kRedactRect, after, kRenderScale);
    ASSERT_GT(roi.width(), 0);
    ASSERT_GT(roi.height(), 0);

    // ABSENCE ORACLE: every interior pixel must be the cleared fill (white) and
    // NONE may still show the original light-blue content. Because light-blue is
    // outside kChanTol of white, "all pixels are cleared" simultaneously proves
    // the wipe is uniform AND that the original samples are gone. Separately
    // tallying survivors of the original fill makes the failure diagnostic and
    // pins the exact security property: original image content is ABSENT.
    int total = 0, notCleared = 0, stillOriginal = 0;
    QColor firstOffender;
    QPoint firstOffenderAt;
    for (int y = roi.top(); y <= roi.bottom(); ++y) {
        for (int x = roi.left(); x <= roi.right(); ++x) {
            ++total;
            const QColor c = after.pixelColor(x, y);
            if (nearColor(c, kOriginalFill)) ++stillOriginal;
            if (!nearColor(c, kClearedFill)) {
                if (notCleared == 0) { firstOffender = c; firstOffenderAt = {x, y}; }
                ++notCleared;
            }
        }
    }
    ASSERT_GT(total, 0);
    // Primary security property: NO interior pixel still shows the original
    // image content. Kills the "imageMethod ignored / None" mutation directly.
    EXPECT_EQ(stillOriginal, 0)
        << stillOriginal << " / " << total << " interior pixels still show the "
        << "ORIGINAL light-blue image content — the underlying image samples "
        << "survived the image-pixel redaction (imageMethod ignored). The "
        << "sensitive raster was NOT destroyed.";
    // The wiped region must be uniformly the cleared fill (proves a real wipe,
    // not partial/leftover content). Kills partial-wipe mutations.
    EXPECT_EQ(notCleared, 0)
        << notCleared << " / " << total << " interior pixels are NOT the cleared "
        << "redaction fill (white) — the image-pixel wipe was incomplete. First "
        << "offender at (" << firstOffenderAt.x() << "," << firstOffenderAt.y()
        << ") = rgb(" << firstOffender.red() << "," << firstOffender.green()
        << "," << firstOffender.blue() << ").";
}

// Control: image samples OUTSIDE the redaction rect must SURVIVE — the pixel
// wipe is localized, not a full-image blank. Kills a mutation that blackens the
// entire image (or page) instead of only the redacted quad.
TEST_F(FidelityR3_23_ImageRedaction, ImagePixelsOutsideRedactionSurvive) {
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), kImagePageIndex);
    auto page = doc->page(kImagePageIndex);
    ASSERT_NE(page, nullptr);

    pdfe::redaction::RedactionManager mgr(ctx_);
    mgr.createRedactionMark(page.get(), kRedactRect);
    pdfe::redaction::RedactionApplyOptions opts;
    opts.blackBoxes  = false;
    opts.imageMethod = pdfe::redaction::RedactionImageMethod::Pixels;
    ASSERT_TRUE(mgr.applyPageRedactions(page.get(), opts));

    doc->saveAs(out_);
    auto reDoc = Document::open(ctx_, out_);
    ASSERT_NE(reDoc, nullptr);
    auto rePage = reDoc->page(kImagePageIndex);
    ASSERT_NE(rePage, nullptr);

    QImage after = rePage->render(rePage->boundingBox(), kRenderScale);
    ASSERT_FALSE(after.isNull());

    // A slice of the image LEFT of the redaction rect but still inside the
    // image footprint: image x in [100,300], redaction x in [140,260]. Use fitz
    // x in [106,132], y in [180,250] (well inside the image, left of the box).
    const QRect roi = pixelInterior(QRectF(106.0, 180.0, 26.0, 70.0),
                                    after, kRenderScale);
    ASSERT_GT(roi.width(), 0);
    ASSERT_GT(roi.height(), 0);

    bool anyOriginal = false;
    for (int y = roi.top(); y <= roi.bottom() && !anyOriginal; ++y)
        for (int x = roi.left(); x <= roi.right(); ++x)
            if (nearColor(after.pixelColor(x, y), kOriginalFill)) {
                anyOriginal = true; break;
            }

    EXPECT_TRUE(anyOriginal)
        << "image samples OUTSIDE the redaction rect no longer show the original "
           "light-blue content — the pixel redaction over-wiped beyond the "
           "marked quad (or blanked the whole image)";
}
