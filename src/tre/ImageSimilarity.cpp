#include "tre/ImageSimilarity.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace pdfe::tre {

QImage ImageSimilarity::toGrayscale(const QImage& image)
{
    if (image.isNull())
        return {};

    if (image.format() == QImage::Format_Grayscale8)
        return image;

    return image.convertToFormat(QImage::Format_Grayscale8);
}

int ImageSimilarity::otsuThreshold(const QImage& grayscale)
{
    if (grayscale.isNull() || grayscale.format() != QImage::Format_Grayscale8)
        return 128;

    const int w = grayscale.width();
    const int h = grayscale.height();
    const int64_t totalPixels = static_cast<int64_t>(w) * h;
    if (totalPixels == 0)
        return 128;

    // Build histogram
    int histogram[256] = {};
    for (int y = 0; y < h; ++y) {
        const uchar* row = grayscale.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            ++histogram[row[x]];
        }
    }

    // Otsu's method: maximize inter-class variance
    double sumTotal = 0.0;
    for (int i = 0; i < 256; ++i)
        sumTotal += static_cast<double>(i) * histogram[i];

    double sumB = 0.0;
    int wB = 0;
    double maxVariance = 0.0;
    int bestThreshold = 0;

    for (int t = 0; t < 256; ++t) {
        wB += histogram[t];
        if (wB == 0)
            continue;

        int wF = totalPixels - wB;
        if (wF == 0)
            break;

        sumB += static_cast<double>(t) * histogram[t];
        double meanB = sumB / wB;
        double meanF = (sumTotal - sumB) / wF;
        double diff = meanB - meanF;

        double variance = static_cast<double>(wB) * static_cast<double>(wF) * diff * diff;
        if (variance > maxVariance) {
            maxVariance = variance;
            bestThreshold = t;
        }
    }

    return bestThreshold;
}

QImage ImageSimilarity::extractAndScale(const QImage& source,
                                         const QRect& region,
                                         const QSize& targetSize)
{
    if (source.isNull() || region.isEmpty() || !targetSize.isValid())
        return {};

    // Clamp region to image bounds
    QRect clamped = region.intersected(source.rect());
    if (clamped.isEmpty())
        return {};

    QImage cropped = source.copy(clamped);
    return cropped.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

float ImageSimilarity::binarizedNCC(const QImage& imageA, const QImage& imageB)
{
    if (imageA.isNull() || imageB.isNull())
        return 0.0f;

    // Convert to grayscale
    QImage grayA = toGrayscale(imageA);
    QImage grayB = toGrayscale(imageB);

    // Ensure same size -- scale smaller to match larger.
    // Using max dimensions preserves the larger (cleaner) image intact and
    // upscales the smaller one. This avoids squishing that destroys NCC
    // when comparing crops from lines of different heights (e.g., 33px
    // reference vs 43px body text, or 33px template vs 43px donor crop).
    if (grayA.size() != grayB.size()) {
        QSize targetSize(std::max(grayA.width(), grayB.width()),
                         std::max(grayA.height(), grayB.height()));
        if (targetSize.width() <= 0 || targetSize.height() <= 0)
            return 0.0f;
        if (grayA.size() != targetSize)
            grayA = grayA.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (grayB.size() != targetSize)
            grayB = grayB.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    const int w = grayA.width();
    const int h = grayA.height();
    const int64_t n = static_cast<int64_t>(w) * h;
    if (n == 0)
        return 0.0f;

    // Compute Otsu thresholds
    const int threshA = otsuThreshold(grayA);
    const int threshB = otsuThreshold(grayB);

    // Compute means of binarized images
    double sumA = 0.0, sumB = 0.0;
    for (int y = 0; y < h; ++y) {
        const uchar* rowA = grayA.constScanLine(y);
        const uchar* rowB = grayB.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            sumA += (rowA[x] < threshA) ? 0.0 : 255.0;
            sumB += (rowB[x] < threshB) ? 0.0 : 255.0;
        }
    }
    const double meanA = sumA / n;
    const double meanB = sumB / n;

    // Compute NCC
    double crossCorr = 0.0, varA = 0.0, varB = 0.0;
    for (int y = 0; y < h; ++y) {
        const uchar* rowA = grayA.constScanLine(y);
        const uchar* rowB = grayB.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            double a = ((rowA[x] < threshA) ? 0.0 : 255.0) - meanA;
            double b = ((rowB[x] < threshB) ? 0.0 : 255.0) - meanB;
            crossCorr += a * b;
            varA += a * a;
            varB += b * b;
        }
    }

    double denom = std::sqrt(varA * varB);
    if (denom < 1e-10)
        return 0.0f;  // Uniform images -- no correlation

    float ncc = static_cast<float>(crossCorr / denom);
    return std::max(0.0f, ncc);  // Clamp negative to 0
}

float ImageSimilarity::normalizedMSE(const QImage& imageA, const QImage& imageB)
{
    if (imageA.isNull() || imageB.isNull())
        return 1.0f;

    QImage grayA = toGrayscale(imageA);
    QImage grayB = toGrayscale(imageB);

    // Ensure same size -- scale smaller to match larger (consistent with binarizedNCC)
    if (grayA.size() != grayB.size()) {
        QSize targetSize(std::max(grayA.width(), grayB.width()),
                         std::max(grayA.height(), grayB.height()));
        if (targetSize.width() <= 0 || targetSize.height() <= 0)
            return 1.0f;
        if (grayA.size() != targetSize)
            grayA = grayA.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (grayB.size() != targetSize)
            grayB = grayB.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    const int w = grayA.width();
    const int h = grayA.height();
    const int64_t n = static_cast<int64_t>(w) * h;
    if (n == 0)
        return 1.0f;

    double sumSqDiff = 0.0;
    for (int y = 0; y < h; ++y) {
        const uchar* rowA = grayA.constScanLine(y);
        const uchar* rowB = grayB.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            double diff = static_cast<double>(rowA[x]) - static_cast<double>(rowB[x]);
            sumSqDiff += diff * diff;
        }
    }

    // Normalize to [0, 1] where 0 = identical, 1 = max difference
    return static_cast<float>(sumSqDiff / (n * 255.0 * 255.0));
}

} // namespace pdfe::tre
