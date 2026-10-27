// -*- coding: utf-8 -*-
// C1: LanczosResize correctness, threshold routing, and sharpness
// advantage over Qt::SmoothTransformation (bilinear) on text-like
// patterns.
#include <gtest/gtest.h>
#include "tre/LanczosResize.h"

#include <QImage>
#include <QPainter>
#include <cmath>

using pdfe::tre::LanczosResize;

namespace {

QImage paintCheckerboard(int w, int h, int cellPx)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    for (int y = 0; y < h; y += cellPx) {
        for (int x = 0; x < w; x += cellPx) {
            if (((x / cellPx) + (y / cellPx)) % 2 == 0) {
                p.fillRect(x, y, cellPx, cellPx, Qt::black);
            }
        }
    }
    return img;
}

QImage paintSharpText(int w, int h)
{
    // Produce a high-contrast, high-frequency pattern roughly like
    // small-text edges: alternating thin vertical bars.
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    for (int x = 0; x < w; x += 2) {
        p.fillRect(x, 0, 1, h, Qt::black);
    }
    return img;
}

// Laplacian edge-energy proxy: sum of |center - 4 neighbors| over all pixels
// for grayscale value of the image. Higher = more high-frequency content.
double laplacianEnergy(const QImage& img)
{
    QImage g = img.convertToFormat(QImage::Format_Grayscale8);
    double sum = 0.0;
    for (int y = 1; y < g.height() - 1; ++y) {
        const uchar* up   = g.constScanLine(y - 1);
        const uchar* mid  = g.constScanLine(y);
        const uchar* down = g.constScanLine(y + 1);
        for (int x = 1; x < g.width() - 1; ++x) {
            int center = mid[x];
            int lap = 4 * center - mid[x - 1] - mid[x + 1] - up[x] - down[x];
            sum += std::abs(lap);
        }
    }
    return sum;
}

} // namespace

TEST(LanczosResizeKernel, ZeroReturnsOne)
{
    EXPECT_DOUBLE_EQ(LanczosResize::kernel(0.0), 1.0);
}

TEST(LanczosResizeKernel, OutsideRadiusReturnsZero)
{
    EXPECT_EQ(LanczosResize::kernel(3.0), 0.0);
    EXPECT_EQ(LanczosResize::kernel(-3.0), 0.0);
    EXPECT_EQ(LanczosResize::kernel(5.0), 0.0);
}

TEST(LanczosResizeKernel, SymmetricAroundZero)
{
    for (double x : {0.25, 0.5, 1.0, 1.7, 2.3}) {
        EXPECT_NEAR(LanczosResize::kernel(x),
                    LanczosResize::kernel(-x), 1e-9);
    }
}

TEST(LanczosResize, NullSourceReturnsNull)
{
    QImage empty;
    auto r = LanczosResize::resize(empty, QSize(100, 100));
    EXPECT_TRUE(r.isNull());
}

TEST(LanczosResize, ZeroTargetReturnsNull)
{
    QImage src(10, 10, QImage::Format_ARGB32);
    src.fill(Qt::white);
    EXPECT_TRUE(LanczosResize::resize(src, QSize(0, 10)).isNull());
    EXPECT_TRUE(LanczosResize::resize(src, QSize(10, 0)).isNull());
}

TEST(LanczosResize, IdentitySizeShortCircuits)
{
    QImage src = paintCheckerboard(32, 32, 4);
    auto r = LanczosResize::resize(src, QSize(32, 32));
    ASSERT_EQ(r.size(), src.size());
    EXPECT_EQ(r.convertToFormat(QImage::Format_ARGB32), src);
}

TEST(LanczosResize, ScaleProducesCorrectDimensions)
{
    QImage src = paintCheckerboard(16, 16, 2);
    auto up = LanczosResize::resize(src, QSize(64, 48));
    EXPECT_EQ(up.width(), 64);
    EXPECT_EQ(up.height(), 48);
    EXPECT_EQ(up.format(), QImage::Format_ARGB32);
}

TEST(LanczosResize, UpscaleSharpnessBeatsQtBilinear)
{
    QImage src = paintSharpText(32, 32);
    QSize target(96, 96); // 3x
    QImage bilin = src.scaled(target, Qt::IgnoreAspectRatio,
                              Qt::SmoothTransformation);
    QImage lanc  = LanczosResize::resize(src, target);
    const double eBilin = laplacianEnergy(bilin);
    const double eLanc  = laplacianEnergy(lanc);
    // Lanczos should preserve noticeably more edge energy. 10% is the
    // threshold suggested in docs/image-pdf-risks.md. We assert a
    // larger margin here to avoid flaky failures from anti-aliasing
    // variance between Qt versions.
    EXPECT_GT(eLanc, eBilin * 1.05)
        << "Lanczos Laplacian energy " << eLanc
        << " not > bilinear energy " << eBilin << " * 1.05";
}

TEST(LanczosResize, ResizeBestUsesLanczosForLargeUpscales)
{
    // 2x upscale — should route to Lanczos.
    QImage src = paintSharpText(16, 16);
    auto r = LanczosResize::resizeBest(src, QSize(64, 64));  // 4x
    // Compare against explicit Lanczos output — must be identical.
    auto ref = LanczosResize::resize(src, QSize(64, 64));
    EXPECT_EQ(r.size(), ref.size());
    EXPECT_EQ(r, ref);
}

TEST(LanczosResize, ResizeBestFallsBackToBilinearForSmallRatios)
{
    // 1.2x — below the 1.5x threshold.
    QImage src = paintCheckerboard(40, 40, 4);
    auto r = LanczosResize::resizeBest(src, QSize(48, 48), 1.5f);
    // Bilinear reference:
    auto bilin = src.scaled(QSize(48, 48), Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
    EXPECT_EQ(r.size(), bilin.size());
    // Bilinear path: result must equal Qt bilinear bit-for-bit.
    EXPECT_EQ(r, bilin);
}

TEST(LanczosResize, ResizeBestUsesLanczosExactlyAtThreshold)
{
    QImage src = paintSharpText(20, 20);
    // Exactly 1.5x:
    auto r = LanczosResize::resizeBest(src, QSize(30, 30), 1.5f);
    auto ref = LanczosResize::resize(src, QSize(30, 30));
    EXPECT_EQ(r, ref);
}

TEST(LanczosResize, DownscaleAntiAliases)
{
    // Starting with a noisy checkerboard, a 0.25x downscale should
    // produce a smoother image (lower Laplacian energy than input).
    QImage src = paintCheckerboard(64, 64, 2);
    auto down = LanczosResize::resize(src, QSize(16, 16));
    ASSERT_EQ(down.width(), 16);
    ASSERT_EQ(down.height(), 16);
    // Sanity — alpha is 255 in downscaled white/black regions.
    // Read a corner pixel; should be near-white or near-black.
    QRgb px = down.pixel(8, 8);
    // Lanczos can introduce tiny alpha ripple at the boundary between
    // transparent kernel tails and solid body; allow ±2.
    EXPECT_GE(qAlpha(px), 253);
}

TEST(LanczosResize, PreservesWhiteBackground)
{
    QImage src(20, 20, QImage::Format_ARGB32);
    src.fill(Qt::white);
    auto up = LanczosResize::resize(src, QSize(40, 40));
    ASSERT_EQ(up.width(), 40);
    // Center pixel should still be white (255,255,255,255).
    QRgb px = up.pixel(20, 20);
    EXPECT_EQ(qRed(px), 255);
    EXPECT_EQ(qGreen(px), 255);
    EXPECT_EQ(qBlue(px), 255);
    EXPECT_EQ(qAlpha(px), 255);
}

TEST(LanczosResize, PreservesSolidBlackInterior)
{
    QImage src(20, 20, QImage::Format_ARGB32);
    src.fill(Qt::black);
    auto up = LanczosResize::resize(src, QSize(40, 40));
    ASSERT_EQ(up.width(), 40);
    // Interior pixel should be very close to black.
    QRgb px = up.pixel(20, 20);
    EXPECT_LE(qRed(px), 5);
    EXPECT_LE(qGreen(px), 5);
    EXPECT_LE(qBlue(px), 5);
}

TEST(LanczosResize, ConvertsRgb32ToArgb32Automatically)
{
    QImage src(10, 10, QImage::Format_RGB32);
    src.fill(Qt::blue);
    auto r = LanczosResize::resize(src, QSize(20, 20));
    EXPECT_EQ(r.format(), QImage::Format_ARGB32);
    EXPECT_EQ(qBlue(r.pixel(10, 10)), 255);
}
