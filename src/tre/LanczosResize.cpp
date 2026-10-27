#include "LanczosResize.h"

#include <algorithm>
#define _USE_MATH_DEFINES
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace pdfe::tre {

namespace {

constexpr int kKernelRadius = 3;

double sinc(double x)
{
    if (x == 0.0) return 1.0;
    const double px = M_PI * x;
    return std::sin(px) / px;
}

// Precompute per-destination-pixel kernel weights for one dimension.
struct Contribution {
    int firstSrc;            // Index of first source sample.
    std::vector<double> w;   // Weights (sum ≈ 1.0 after normalization).
};

std::vector<Contribution> buildContributions(int srcLen, int dstLen)
{
    std::vector<Contribution> out(dstLen);
    const double scale = static_cast<double>(srcLen) / dstLen;

    // When downsampling (scale > 1), widen the kernel footprint so the
    // effective filter radius in source pixels remains 3 — otherwise the
    // filter passes too much high-frequency content and aliases.
    const double support = std::max(1.0, scale) * kKernelRadius;
    const double invSample = (scale > 1.0) ? 1.0 / scale : 1.0;

    for (int dst = 0; dst < dstLen; ++dst) {
        const double center = (dst + 0.5) * scale - 0.5;
        const int first = std::max(0, static_cast<int>(std::ceil(center - support)));
        const int last  = std::min(srcLen - 1,
                                   static_cast<int>(std::floor(center + support)));
        Contribution& c = out[dst];
        c.firstSrc = first;
        c.w.resize(last - first + 1);
        double sum = 0.0;
        for (int s = first; s <= last; ++s) {
            const double x = (s - center) * invSample;
            const double k = LanczosResize::kernel(x);
            c.w[s - first] = k;
            sum += k;
        }
        if (sum > 1e-9) {
            std::transform(c.w.begin(), c.w.end(), c.w.begin(),
                           [sum](double v) { return v / sum; });
        }
    }
    return out;
}

// Horizontal pass: floats in, floats out. 4 channels (RGBA).
void horizontalPass(const QImage& src, int dstW,
                    const std::vector<Contribution>& contribs,
                    std::vector<float>& out)
{
    const int srcW = src.width();
    const int h = src.height();
    out.assign(static_cast<size_t>(dstW) * h * 4, 0.0f);

    for (int y = 0; y < h; ++y) {
        const uchar* srcRow = src.constScanLine(y);
        float* outRow = &out[static_cast<size_t>(y) * dstW * 4];
        for (int x = 0; x < dstW; ++x) {
            const Contribution& c = contribs[x];
            double b = 0, g = 0, r = 0, a = 0;
            for (size_t i = 0; i < c.w.size(); ++i) {
                const int sx = c.firstSrc + static_cast<int>(i);
                const uchar* p = srcRow + sx * 4;
                const double w = c.w[i];
                b += p[0] * w; g += p[1] * w;
                r += p[2] * w; a += p[3] * w;
            }
            outRow[x * 4 + 0] = static_cast<float>(b);
            outRow[x * 4 + 1] = static_cast<float>(g);
            outRow[x * 4 + 2] = static_cast<float>(r);
            outRow[x * 4 + 3] = static_cast<float>(a);
            (void)srcW;
        }
    }
}

// Vertical pass: floats in, QImage out (ARGB32).
QImage verticalPass(const std::vector<float>& src, int w, int srcH,
                    int dstH, const std::vector<Contribution>& contribs)
{
    QImage dst(w, dstH, QImage::Format_ARGB32);
    for (int y = 0; y < dstH; ++y) {
        const Contribution& c = contribs[y];
        uchar* dstRow = dst.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double b = 0, g = 0, r = 0, a = 0;
            for (size_t i = 0; i < c.w.size(); ++i) {
                const int sy = c.firstSrc + static_cast<int>(i);
                const float* p = &src[(static_cast<size_t>(sy) * w + x) * 4];
                const double ww = c.w[i];
                b += p[0] * ww; g += p[1] * ww;
                r += p[2] * ww; a += p[3] * ww;
            }
            auto clamp = [](double v) {
                return static_cast<uchar>(std::clamp(v, 0.0, 255.0));
            };
            dstRow[x * 4 + 0] = clamp(b);
            dstRow[x * 4 + 1] = clamp(g);
            dstRow[x * 4 + 2] = clamp(r);
            dstRow[x * 4 + 3] = clamp(a);
        }
        (void)srcH;
    }
    return dst;
}

} // namespace

double LanczosResize::kernel(double x)
{
    if (x <= -kKernelRadius || x >= kKernelRadius) return 0.0;
    if (x == 0.0) return 1.0;
    return sinc(x) * sinc(x / kKernelRadius);
}

QImage LanczosResize::resize(const QImage& src, const QSize& dstSize)
{
    if (src.isNull() || dstSize.width() <= 0 || dstSize.height() <= 0) {
        return QImage();
    }
    QImage argb = (src.format() == QImage::Format_ARGB32)
        ? src
        : src.convertToFormat(QImage::Format_ARGB32);

    const int srcW = argb.width();
    const int srcH = argb.height();
    const int dstW = dstSize.width();
    const int dstH = dstSize.height();

    // Identity short-circuit.
    if (srcW == dstW && srcH == dstH) return argb;

    auto hContribs = buildContributions(srcW, dstW);
    auto vContribs = buildContributions(srcH, dstH);

    std::vector<float> intermediate;
    horizontalPass(argb, dstW, hContribs, intermediate);
    return verticalPass(intermediate, dstW, srcH, dstH, vContribs);
}

QImage LanczosResize::resizeBest(const QImage& src, const QSize& dstSize,
                                 float lanczosThreshold)
{
    if (src.isNull() || dstSize.width() <= 0 || dstSize.height() <= 0) {
        return QImage();
    }
    const float rx = static_cast<float>(dstSize.width())  / std::max(1, src.width());
    const float ry = static_cast<float>(dstSize.height()) / std::max(1, src.height());
    const float maxR = std::max(rx, ry);
    if (maxR >= lanczosThreshold) {
        return resize(src, dstSize);
    }
    return src.scaled(dstSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

} // namespace pdfe::tre
