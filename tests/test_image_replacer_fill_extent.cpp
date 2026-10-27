// Regression tests for the fill/composite extent chosen when text is written
// back into a scanned page image.
//
// Field-reported 2026-08-28 against a 16-page scanned manual: adding a single
// character with the text tool rewrote the ENTIRE selection box into the page
// image, so the box had to be resized by hand to avoid destroying the scan
// around it. Diagnosis in docs/audit/field-report-2026-08-28-scanned-manual.md
// (defect 3).
//
// The subtlety these tests exist to pin is that the correct fix is NOT "always
// shrink the fill to the glyphs". On a REPLACEMENT the fill has to cover the
// original scanned text or it shows through underneath the new rendering as
// ghost text -- which is precisely why ImageReplacer unions the selection with
// the OCR line bbox before compositing. Shrinking unconditionally would trade
// this defect for that one. So there are two cases here, and the second one is
// a guard against over-fixing the first.
//
// These drive ImageReplacer::computeFillRegion directly rather than
// ImageReplacer::replace(): it is the same function the production paths call,
// and driving it directly means the tests need no PDF, no Tesseract and no
// tessdata, so they RUN rather than skip. A test that reports "did not run" as
// success is worse than no test.

#include <gtest/gtest.h>

#include "tre/ImageReplacer.h"
#include "tre/TreTypes.h"

#include <QImage>
#include <QPainter>
#include <QRect>

using namespace pdfe::tre;

namespace {

/// A text bitmap as the compositor produces one: ARGB32_Premultiplied,
/// transparent everywhere except the glyphs. `ink` is where the glyphs land.
QImage bitmapWithInkAt(int w, int h, const QRect& ink) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    if (!ink.isEmpty()) {
        QPainter p(&img);
        p.fillRect(ink, QColor(0, 0, 0, 255));
        p.end();
    }
    return img;
}

PixelRect rect(double x0, double y0, double x1, double y1) {
    PixelRect r;
    r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Adding over blank background: fit to the glyphs.
// ---------------------------------------------------------------------------

TEST(ImageReplacerFillExtentTest, AddingOverBlankFitsTheFillToTheGlyphInk) {
    // The reported shape: a big box dragged out by hand, one small character
    // rendered near its top-left.
    const PixelRect selection = rect(100, 200, 700, 500);   // 600 x 300
    const QRect ink(10, 12, 40, 55);
    QImage bitmap = bitmapWithInkAt(600, 300, ink);

    auto fill = ImageReplacer::computeFillRegion(selection, bitmap,
                                                 /*originalText=*/"", /*padPx=*/1);

    ASSERT_TRUE(fill.shrunkToInk)
        << "a single character in a large empty box must not repaint the box";

    // Ink plus one pixel of padding on each side, in page coordinates.
    EXPECT_DOUBLE_EQ(fill.bbox.x0, 100 + 10 - 1);
    EXPECT_DOUBLE_EQ(fill.bbox.y0, 200 + 12 - 1);
    EXPECT_DOUBLE_EQ(fill.bbox.width(), 40 + 2);
    EXPECT_DOUBLE_EQ(fill.bbox.height(), 55 + 2);

    // The bitmap must be cropped to match, or the glyphs would be drawn at the
    // new origin and so land in the wrong place on the page.
    EXPECT_EQ(fill.bitmapCrop, QRect(9, 11, 42, 57));

    // The point of the whole exercise, stated as a ratio rather than left
    // implicit in the numbers above.
    const double before = selection.width() * selection.height();
    const double after = fill.bbox.width() * fill.bbox.height();
    EXPECT_LT(after, before * 0.05)
        << "rewrote " << after << " of " << before << " page pixels";
}

TEST(ImageReplacerFillExtentTest, WhitespaceOnlyOriginalTextCountsAsBlank) {
    // OCR over blank paper does not reliably return the empty string; it can
    // return the layout whitespace it thinks it saw. There is still nothing
    // there to cover.
    const PixelRect selection = rect(0, 0, 400, 200);
    QImage bitmap = bitmapWithInkAt(400, 200, QRect(5, 5, 20, 30));

    auto fill = ImageReplacer::computeFillRegion(selection, bitmap, "  \n\t ", 1);

    EXPECT_TRUE(fill.shrunkToInk);
    EXPECT_LT(fill.bbox.width(), selection.width());
}

TEST(ImageReplacerFillExtentTest, PaddingIsClampedToTheBitmapNotAllowedOutside) {
    // Ink flush against the bitmap edge: the padding must not push the fill
    // region outside the area the bitmap actually covers.
    const PixelRect selection = rect(50, 60, 250, 160);
    QImage bitmap = bitmapWithInkAt(200, 100, QRect(0, 0, 30, 20));

    auto fill = ImageReplacer::computeFillRegion(selection, bitmap, "", 3);

    ASSERT_TRUE(fill.shrunkToInk);
    EXPECT_DOUBLE_EQ(fill.bbox.x0, 50);   // not 47
    EXPECT_DOUBLE_EQ(fill.bbox.y0, 60);   // not 57
    EXPECT_EQ(fill.bitmapCrop, QRect(0, 0, 33, 23));
}

// ---------------------------------------------------------------------------
// Replacing existing text: do NOT shrink. This is the over-fix guard.
// ---------------------------------------------------------------------------

TEST(ImageReplacerFillExtentTest, ReplacingExistingTextStillCoversTheWholeCandidate) {
    // The candidate here is what ImageReplacer builds for an inline edit: the
    // selection unioned with the OCR line bbox, sized to cover the original
    // scanned glyphs. Shrinking it to the new text's ink would uncover the old
    // text wherever the replacement is shorter -- ghost text.
    const PixelRect candidate = rect(100, 200, 700, 500);
    QImage bitmap = bitmapWithInkAt(600, 300, QRect(10, 12, 40, 55));

    auto fill = ImageReplacer::computeFillRegion(candidate, bitmap,
                                                 "the original scanned text", 1);

    EXPECT_FALSE(fill.shrunkToInk);
    EXPECT_DOUBLE_EQ(fill.bbox.x0, candidate.x0);
    EXPECT_DOUBLE_EQ(fill.bbox.y0, candidate.y0);
    EXPECT_DOUBLE_EQ(fill.bbox.x1, candidate.x1);
    EXPECT_DOUBLE_EQ(fill.bbox.y1, candidate.y1);
    EXPECT_EQ(fill.bitmapCrop, bitmap.rect());
}

TEST(ImageReplacerFillExtentTest, AShorterReplacementStillCoversTheOriginalExtent) {
    // Concretely the ghosting case: original text ran the full width of the
    // candidate, the replacement is one short word.
    const PixelRect candidate = rect(0, 0, 800, 60);
    QImage bitmap = bitmapWithInkAt(800, 60, QRect(0, 10, 90, 40));

    auto fill = ImageReplacer::computeFillRegion(candidate, bitmap,
                                                 "a much longer original line", 1);

    ASSERT_FALSE(fill.shrunkToInk);
    EXPECT_DOUBLE_EQ(fill.bbox.width(), 800)
        << "the tail of the original line would remain visible";
}

// ---------------------------------------------------------------------------
// Fail-safe paths: every one of them keeps the old behaviour.
// ---------------------------------------------------------------------------

TEST(ImageReplacerFillExtentTest, AFullyTransparentBitmapDoesNotShrinkToNothing) {
    // No ink measured. Falling through to "fill nothing" would silently drop
    // the edit; falling through to "fill the candidate" is merely the previous
    // behaviour. Prefer the one that cannot lose work.
    const PixelRect candidate = rect(10, 10, 210, 110);
    QImage bitmap = bitmapWithInkAt(200, 100, QRect());

    auto fill = ImageReplacer::computeFillRegion(candidate, bitmap, "", 1);

    EXPECT_FALSE(fill.shrunkToInk);
    EXPECT_DOUBLE_EQ(fill.bbox.width(), 200);
    EXPECT_DOUBLE_EQ(fill.bbox.height(), 100);
}

TEST(ImageReplacerFillExtentTest, ANullBitmapDoesNotShrink) {
    const PixelRect candidate = rect(10, 10, 210, 110);

    auto fill = ImageReplacer::computeFillRegion(candidate, QImage(), "", 1);

    EXPECT_FALSE(fill.shrunkToInk);
    EXPECT_DOUBLE_EQ(fill.bbox.width(), 200);
    EXPECT_DOUBLE_EQ(fill.bbox.height(), 100);
}

TEST(ImageReplacerFillExtentTest, InkCoveringTheWholeBitmapLeavesTheRegionUnchanged) {
    // A full line of text in a tightly-drawn box: there is nothing to give
    // back, and the fill must not end up smaller than the glyphs.
    const PixelRect candidate = rect(0, 0, 300, 40);
    QImage bitmap = bitmapWithInkAt(300, 40, QRect(0, 0, 300, 40));

    auto fill = ImageReplacer::computeFillRegion(candidate, bitmap, "", 2);

    EXPECT_DOUBLE_EQ(fill.bbox.width(), 300);
    EXPECT_DOUBLE_EQ(fill.bbox.height(), 40);
    EXPECT_EQ(fill.bitmapCrop, bitmap.rect());
}

TEST(ImageReplacerFillExtentTest, LowAlphaAntialiasFringeIsCountedAsInk) {
    // The outer ring of an antialiased glyph is nearly transparent. Excluding
    // it would run the fill boundary through the middle of the glyph edge and
    // leave a visible halo of un-cleared scan around the new text.
    QImage bitmap(100, 100, QImage::Format_ARGB32);
    bitmap.fill(Qt::transparent);
    bitmap.setPixel(40, 40, qRgba(0, 0, 0, 255));
    bitmap.setPixel(30, 30, qRgba(0, 0, 0, 3));   // faint fringe
    bitmap.setPixel(50, 50, qRgba(0, 0, 0, 3));

    auto fill = ImageReplacer::computeFillRegion(rect(0, 0, 100, 100), bitmap, "", 0);

    ASSERT_TRUE(fill.shrunkToInk);
    EXPECT_EQ(fill.bitmapCrop, QRect(30, 30, 21, 21))
        << "the fringe pixels were dropped from the ink extent";
}
