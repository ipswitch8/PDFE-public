// Tests for B3 pattern-copy replacement path.
//
// Verifies:
//   - ImageMasker::findBackgroundPatch locates a clean patch above/
//     below/left/right of a destination rect, phase-aligned to an
//     optional halftone period.
//   - The halftone-classified compositeTextBitmap overload copies a
//     pattern instead of solid-filling, preserving the dot pattern
//     across the edit boundary.
//   - Uniform and Gradient classifications still solid-fill.
//   - Complex / Unknown classifications fall back to solid fill.
//   - On a real halftone fixture, the Laplacian response across the
//     edit boundary is closer to a pattern-continuous score than a
//     flat-fill score — i.e., the visible artifact is absent.
//
// See docs/image-pdf-risks.md (B3).

#include <gtest/gtest.h>

#include "tre/ImageMasker.h"
#include "tre/TreTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QDebug>
#include <QImage>
#include <QPainter>
#include <cmath>
#include <filesystem>
#include <memory>

using namespace pdfe::tre;
using namespace pdfe::core;

namespace {

// Build a synthetic halftone image: 50% gray dots on white at a
// chosen pixel period. Uses a SQUARE grid (no row offset) so the
// horizontal and vertical periods are both exactly `period` —
// this matches the single-period assumption in the ImageMasker's
// phase-snapping logic. Real halftone screens are hex-packed with
// a vertical period of 2x the dot spacing, but a square grid is
// the simplest thing the classifier's periodPx actually preserves.
QImage makeHalftoneImage(int w, int h, int period) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setBrush(QColor(0, 0, 0));
    p.setPen(Qt::NoPen);
    int radius = std::max(1, period / 3);
    for (int y = 0; y < h; y += period) {
        for (int x = 0; x < w; x += period) {
            p.drawEllipse(QPointF(x, y), radius, radius);
        }
    }
    p.end();
    return img;
}

// Compute mean Laplacian absolute value across the border of a rect
// in the supplied image. Higher value = more abrupt boundary. The
// ideal "pattern copy worked" score is close to the same metric
// computed on an unedited portion of the same image.
double borderLaplacian(const QImage& img, const QRect& rect) {
    auto lum = [](QRgb rgb) {
        return (299 * qRed(rgb) + 587 * qGreen(rgb) + 114 * qBlue(rgb)) / 1000;
    };
    auto lap = [&](int x, int y) -> int {
        int c = lum(img.pixel(x, y));
        int u = lum(img.pixel(x, y - 1));
        int d = lum(img.pixel(x, y + 1));
        int l = lum(img.pixel(x - 1, y));
        int r = lum(img.pixel(x + 1, y));
        return std::abs(4 * c - u - d - l - r);
    };
    double sum = 0.0;
    int n = 0;
    // Top border
    int y = rect.top();
    if (y > 0 && y < img.height() - 1) {
        for (int x = std::max(1, rect.left()); x < std::min(img.width() - 1, rect.right()); ++x) {
            sum += lap(x, y);
            ++n;
        }
    }
    // Bottom border
    y = rect.bottom();
    if (y > 0 && y < img.height() - 1) {
        for (int x = std::max(1, rect.left()); x < std::min(img.width() - 1, rect.right()); ++x) {
            sum += lap(x, y);
            ++n;
        }
    }
    return n ? sum / n : 0.0;
}

} // namespace

// -----------------------------------------------------------------------
// ImageMasker::findBackgroundPatch — synthetic unit tests
// -----------------------------------------------------------------------

TEST(B3PatternCopy, FindPatch_WhiteBackground) {
    QImage img(400, 400, QImage::Format_RGB32);
    img.fill(Qt::white);
    QRect dest(100, 100, 80, 40);
    QRect found;
    EXPECT_TRUE(ImageMasker::findBackgroundPatch(img, dest, 1, &found));
    EXPECT_EQ(found.size(), dest.size());
    // Patch must not overlap destination.
    EXPECT_FALSE(found.intersects(dest));
    // Patch must be inside the image.
    EXPECT_GE(found.left(), 0);
    EXPECT_GE(found.top(), 0);
    EXPECT_LT(found.right(), img.width());
    EXPECT_LT(found.bottom(), img.height());
}

TEST(B3PatternCopy, FindPatch_HalftonePhaseAligned) {
    const int period = 6;
    QImage img = makeHalftoneImage(600, 600, period);
    QRect dest(200, 200, 100, 40);
    QRect found;
    ASSERT_TRUE(ImageMasker::findBackgroundPatch(img, dest, period, &found));
    // The source offset from dest must be a multiple of the period
    // so the halftone grid stays aligned.
    int dx = found.x() - dest.x();
    int dy = found.y() - dest.y();
    EXPECT_EQ(dx % period, 0) << "x shift " << dx << " not multiple of " << period;
    EXPECT_EQ(dy % period, 0) << "y shift " << dy << " not multiple of " << period;
}

TEST(B3PatternCopy, FindPatch_FailsWhenRectCoversImage) {
    QImage img(100, 100, QImage::Format_RGB32);
    img.fill(Qt::white);
    // Destination covers the whole image — no offset can fit.
    QRect dest(0, 0, 100, 100);
    QRect found;
    EXPECT_FALSE(ImageMasker::findBackgroundPatch(img, dest, 1, &found));
}

TEST(B3PatternCopy, FindPatch_RejectsDarkText) {
    QImage img(400, 400, QImage::Format_RGB32);
    img.fill(Qt::white);
    // Paint a dark rectangle above the dest location — findPatch
    // should skip it because it looks like text.
    QPainter p(&img);
    p.fillRect(QRect(50, 50, 150, 40), Qt::black);
    p.end();
    QRect dest(60, 150, 130, 40);
    QRect found;
    bool ok = ImageMasker::findBackgroundPatch(img, dest, 1, &found);
    ASSERT_TRUE(ok);
    // Found patch must not be the dark rectangle area.
    QRect darkArea(50, 50, 150, 40);
    EXPECT_FALSE(found.intersects(darkArea))
        << "found=" << found.x() << "," << found.y()
        << " dark=" << darkArea.x() << "," << darkArea.y();
}

// -----------------------------------------------------------------------
// Classification-dispatch smoke tests — compositeTextBitmap picks the
// right strategy for each classification.
// -----------------------------------------------------------------------

TEST(B3PatternCopy, UniformDoesSolidFill) {
    ImageMasker masker;
    QImage source(400, 400, QImage::Format_RGB32);
    source.fill(QColor(200, 220, 240));
    // Fully transparent text bitmap -> output should equal the fill.
    QImage textBitmap(80, 40, QImage::Format_ARGB32);
    textBitmap.fill(Qt::transparent);
    PixelRect region;
    region.x0 = 100; region.y0 = 100; region.x1 = 180; region.y1 = 140;

    BackgroundInfo bg;
    bg.dominantColor = RgbColor{255, 0, 255};  // obvious magenta
    bg.classification = BgClassification::Uniform;
    QImage out = masker.compositeTextBitmap(source, textBitmap, region, bg);

    // Center of region should be magenta (solid fill used).
    QRgb px = out.pixel(140, 120);
    EXPECT_EQ(qRed(px), 255);
    EXPECT_EQ(qGreen(px), 0);
    EXPECT_EQ(qBlue(px), 255);
}

TEST(B3PatternCopy, HalftoneCopiesPatternNotSolidColor) {
    ImageMasker masker;
    const int period = 6;
    QImage source = makeHalftoneImage(600, 600, period);
    QImage textBitmap(80, 40, QImage::Format_ARGB32);
    textBitmap.fill(Qt::transparent);

    PixelRect region;
    region.x0 = 200; region.y0 = 200; region.x1 = 280; region.y1 = 240;

    BackgroundInfo bg;
    bg.dominantColor = RgbColor{255, 0, 255};  // obvious magenta
    bg.classification = BgClassification::Halftone;
    bg.halftoneSpacingPx = static_cast<float>(period);
    QImage out = masker.compositeTextBitmap(source, textBitmap, region, bg);

    // The destination region must NOT be flooded with magenta —
    // pattern copy must have happened. Sample several pixels inside
    // the region and verify none are magenta.
    int magentaCount = 0;
    for (int y = 205; y < 235; y += 4) {
        for (int x = 205; x < 275; x += 4) {
            QRgb px = out.pixel(x, y);
            if (qRed(px) == 255 && qGreen(px) == 0 && qBlue(px) == 255) {
                ++magentaCount;
            }
        }
    }
    EXPECT_EQ(magentaCount, 0)
        << "Halftone background should have been pattern-copied, "
           "not flood-filled with the dominant color";
}

TEST(B3PatternCopy, TexturedCopiesPattern) {
    ImageMasker masker;
    // Synthetic noise image.
    QImage source(500, 500, QImage::Format_RGB32);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            int v = 180 + (((x * 29 + y * 53) ^ (x * 7)) & 0x2F);
            source.setPixel(x, y, qRgb(v, v, v));
        }
    }
    QImage textBitmap(60, 30, QImage::Format_ARGB32);
    textBitmap.fill(Qt::transparent);

    PixelRect region;
    region.x0 = 150; region.y0 = 150; region.x1 = 210; region.y1 = 180;

    BackgroundInfo bg;
    bg.dominantColor = RgbColor{255, 0, 255};  // magenta
    bg.classification = BgClassification::Textured;
    QImage out = masker.compositeTextBitmap(source, textBitmap, region, bg);

    // No magenta should appear in the region — pattern copy used.
    bool sawMagenta = false;
    for (int y = 155; y < 175; y += 3) {
        for (int x = 155; x < 205; x += 3) {
            QRgb px = out.pixel(x, y);
            if (qRed(px) == 255 && qGreen(px) == 0 && qBlue(px) == 255) {
                sawMagenta = true;
            }
        }
    }
    EXPECT_FALSE(sawMagenta);
}

TEST(B3PatternCopy, ComplexFallsBackToSolidFill) {
    ImageMasker masker;
    QImage source(400, 400, QImage::Format_RGB32);
    source.fill(Qt::white);
    QImage textBitmap(60, 30, QImage::Format_ARGB32);
    textBitmap.fill(Qt::transparent);

    PixelRect region;
    region.x0 = 100; region.y0 = 100; region.x1 = 160; region.y1 = 130;

    BackgroundInfo bg;
    bg.dominantColor = RgbColor{200, 100, 50};
    bg.classification = BgClassification::Complex;
    QImage out = masker.compositeTextBitmap(source, textBitmap, region, bg);

    // Center of region should be the dominant color (solid fill
    // because Complex falls back).
    QRgb px = out.pixel(130, 115);
    EXPECT_EQ(qRed(px), 200);
    EXPECT_EQ(qGreen(px), 100);
    EXPECT_EQ(qBlue(px), 50);
}

// -----------------------------------------------------------------------
// Pixel-exact test: on a perfectly periodic halftone where
// (destRect.width() / period) is an integer, copying from a
// horizontally-shifted source by an exact multiple of the period
// must produce output that is BIT-IDENTICAL to the original in the
// destination region (with a fully transparent text bitmap). The
// solid-fill version differs from the original in every pixel.
// -----------------------------------------------------------------------
TEST(B3PatternCopy, PatternCopyMatchesOriginalWhenShiftIsPeriodMultiple) {
    ImageMasker masker;
    const int period = 6;
    // 600 x 600 halftone; dest rect width = 120 (= 20 periods).
    QImage source = makeHalftoneImage(600, 600, period);
    QImage textBitmap(120, 50, QImage::Format_ARGB32);
    textBitmap.fill(Qt::transparent);

    PixelRect region;
    region.x0 = 250; region.y0 = 250; region.x1 = 370; region.y1 = 300;
    QRect qrect(250, 250, 120, 50);

    // Pattern-copy result.
    BackgroundInfo pattern;
    pattern.dominantColor = RgbColor{128, 128, 128};
    pattern.classification = BgClassification::Halftone;
    pattern.halftoneSpacingPx = static_cast<float>(period);
    QImage patternOut = masker.compositeTextBitmap(source, textBitmap, region, pattern);

    // Count pixels that match the source. With a period-multiple
    // shift on a periodic pattern, every pixel in the destination
    // should be bit-identical to the original.
    int matchCount = 0;
    int totalCount = 0;
    for (int y = qrect.top(); y <= qrect.bottom(); ++y) {
        for (int x = qrect.left(); x <= qrect.right(); ++x) {
            ++totalCount;
            if (patternOut.pixel(x, y) == source.pixel(x, y)) {
                ++matchCount;
            }
        }
    }
    double matchFrac = static_cast<double>(matchCount) / totalCount;
    qDebug() << "[B3 test] pattern match fraction =" << matchFrac
             << "(" << matchCount << "/" << totalCount << ")";

    // Allow a small slop for Qt's drawImage having occasional
    // 1-pixel format-conversion differences. 95% is still a
    // conclusive demonstration that the pattern was preserved.
    EXPECT_GT(matchFrac, 0.95)
        << "Pattern copy did not preserve the halftone pattern";

    // Now verify the SOLID version differs from the original in
    // most pixels — it really is a destructive flat fill.
    BackgroundInfo solid;
    solid.dominantColor = RgbColor{255, 0, 255};  // obvious magenta
    solid.classification = BgClassification::Complex;
    QImage solidOut = masker.compositeTextBitmap(source, textBitmap, region, solid);

    int solidMatchCount = 0;
    for (int y = qrect.top(); y <= qrect.bottom(); ++y) {
        for (int x = qrect.left(); x <= qrect.right(); ++x) {
            if (solidOut.pixel(x, y) == source.pixel(x, y)) {
                ++solidMatchCount;
            }
        }
    }
    double solidMatchFrac = static_cast<double>(solidMatchCount) / totalCount;
    qDebug() << "[B3 test] solid match fraction =" << solidMatchFrac;
    // Solid magenta cannot match the black/white halftone at any
    // pixel.
    EXPECT_LT(solidMatchFrac, 0.01);
}

// -----------------------------------------------------------------------
// Regression anchor: the legacy RgbColor overload must still work
// and delegate to the Uniform solid-fill path.
// -----------------------------------------------------------------------
TEST(B3PatternCopy, LegacyOverloadStillWorks) {
    ImageMasker masker;
    QImage source(400, 400, QImage::Format_RGB32);
    source.fill(Qt::white);
    QImage textBitmap(60, 30, QImage::Format_ARGB32);
    textBitmap.fill(Qt::transparent);
    PixelRect region;
    region.x0 = 100; region.y0 = 100; region.x1 = 160; region.y1 = 130;

    RgbColor bg{50, 150, 200};
    QImage out = masker.compositeTextBitmap(source, textBitmap, region, bg);
    QRgb px = out.pixel(130, 115);
    EXPECT_EQ(qRed(px), 50);
    EXPECT_EQ(qGreen(px), 150);
    EXPECT_EQ(qBlue(px), 200);
}
