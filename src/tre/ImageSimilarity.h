#pragma once

#include <QImage>
#include <QRect>
#include <QSize>

namespace pdfe::tre {

/// Image comparison utilities for visual font ranking.
/// Provides binarized Normalized Cross-Correlation (NCC) and MSE metrics
/// for comparing rendered text against scanned text regions.
class ImageSimilarity {
public:
    /// Binarized Normalized Cross-Correlation between two images.
    /// Both images are converted to grayscale and binarized at their
    /// individual Otsu thresholds before comparison.
    /// Returns 0.0 (no correlation) to 1.0 (perfect match).
    /// If images differ in size, the larger is scaled down to match.
    static float binarizedNCC(const QImage& imageA, const QImage& imageB);

    /// Mean Squared Error between two grayscale images, normalized to [0, 1].
    /// If images differ in size, the larger is scaled down to match.
    static float normalizedMSE(const QImage& imageA, const QImage& imageB);

    /// Extract a sub-region from an image and scale it to targetSize.
    static QImage extractAndScale(const QImage& source,
                                   const QRect& region,
                                   const QSize& targetSize);

    /// Convert image to 8-bit grayscale (Format_Grayscale8).
    static QImage toGrayscale(const QImage& image);

    /// Compute Otsu binarization threshold for a grayscale image.
    /// Returns threshold value in [0, 255].
    static int otsuThreshold(const QImage& grayscale);
};

} // namespace pdfe::tre
