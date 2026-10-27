#include "BgColorDetector.h"

#include <QColor>
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace pdfe::tre {

namespace {

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

/// Compute Euclidean distance between two RGB colors.
inline double colorDistance(RgbColor a, RgbColor b) {
    const double dr = static_cast<double>(a.r) - static_cast<double>(b.r);
    const double dg = static_cast<double>(a.g) - static_cast<double>(b.g);
    const double db = static_cast<double>(a.b) - static_cast<double>(b.b);
    return std::sqrt(dr * dr + dg * dg + db * db);
}

/// Convert QRgb to RgbColor.
inline RgbColor qRgbToRgbColor(QRgb rgb) {
    return RgbColor{
        static_cast<uint8_t>(qRed(rgb)),
        static_cast<uint8_t>(qGreen(rgb)),
        static_cast<uint8_t>(qBlue(rgb))
    };
}

/// Clamp a rectangle to image bounds.
struct ClampedRect {
    int x0, y0, x1, y1;
};

ClampedRect clampRectToImage(const PixelRect& rect, const QImage& image) {
    const int imgWidth = image.width();
    const int imgHeight = image.height();

    const int x0 = std::clamp(static_cast<int>(std::round(rect.x0)), 0, imgWidth - 1);
    const int y0 = std::clamp(static_cast<int>(std::round(rect.y0)), 0, imgHeight - 1);
    const int x1 = std::clamp(static_cast<int>(std::round(rect.x1)), 0, imgWidth - 1);
    const int y1 = std::clamp(static_cast<int>(std::round(rect.y1)), 0, imgHeight - 1);

    return ClampedRect{
        std::min(x0, x1),
        std::min(y0, y1),
        std::max(x0, x1),
        std::max(y0, y1)
    };
}

/// Sample pixels from a horizontal strip.
void sampleHorizontalStrip(const QImage& image,
                          int x0, int x1, int y0, int y1,
                          std::vector<RgbColor>& samples) {
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= image.height()) {
            continue;
        }

        const QRgb* scanLine = reinterpret_cast<const QRgb*>(image.scanLine(y));

        for (int x = x0; x < x1; ++x) {
            if (x >= 0 && x < image.width()) {
                samples.push_back(qRgbToRgbColor(scanLine[x]));
            }
        }
    }
}

/// Sample pixels from a vertical strip.
void sampleVerticalStrip(const QImage& image,
                        int x0, int x1, int y0, int y1,
                        std::vector<RgbColor>& samples) {
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= image.height()) {
            continue;
        }

        const QRgb* scanLine = reinterpret_cast<const QRgb*>(image.scanLine(y));

        for (int x = x0; x < x1; ++x) {
            if (x >= 0 && x < image.width()) {
                samples.push_back(qRgbToRgbColor(scanLine[x]));
            }
        }
    }
}

/// Find the dominant color in a set of samples using histogram analysis.
/// Returns the color and the fraction of samples within the threshold.
struct DominantColorResult {
    RgbColor color;
    float uniformity; // Fraction of samples within threshold of dominant color
};

DominantColorResult findDominantColor(const std::vector<RgbColor>& samples,
                                     double threshold = 30.0) {
    if (samples.empty()) {
        return DominantColorResult{{255, 255, 255}, 1.0f};
    }

    // Build histogram: map each color to its count
    std::map<uint32_t, int> histogram;

    for (const auto& color : samples) {
        // Pack RGB into a single uint32 for easy hashing
        const uint32_t key = (static_cast<uint32_t>(color.r) << 16) |
                            (static_cast<uint32_t>(color.g) << 8) |
                            static_cast<uint32_t>(color.b);
        histogram[key]++;
    }

    // Find the most frequent color
    uint32_t dominantKey = 0;
    int maxCount = 0;

    for (const auto& [key, count] : histogram) {
        if (count > maxCount) {
            maxCount = count;
            dominantKey = key;
        }
    }

    // Unpack dominant color
    RgbColor dominantColor{
        static_cast<uint8_t>((dominantKey >> 16) & 0xFF),
        static_cast<uint8_t>((dominantKey >> 8) & 0xFF),
        static_cast<uint8_t>(dominantKey & 0xFF)
    };

    // Count samples within threshold of dominant color
    const int nearCount = static_cast<int>(std::count_if(
        samples.begin(), samples.end(),
        [&](const auto& color) {
            return colorDistance(color, dominantColor) < threshold;
        }));

    const float uniformity = static_cast<float>(nearCount) / static_cast<float>(samples.size());

    return DominantColorResult{dominantColor, uniformity};
}

/// Compute mean color of a sample set.
RgbColor computeMeanColor(const std::vector<RgbColor>& samples) {
    if (samples.empty()) {
        return RgbColor{255, 255, 255};
    }

    int64_t sumR = 0, sumG = 0, sumB = 0;
    for (const auto& color : samples) {
        sumR += color.r;
        sumG += color.g;
        sumB += color.b;
    }

    const size_t count = samples.size();
    return RgbColor{
        static_cast<uint8_t>(sumR / count),
        static_cast<uint8_t>(sumG / count),
        static_cast<uint8_t>(sumB / count)
    };
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// BgColorDetector implementation
// ---------------------------------------------------------------------------

BackgroundInfo BgColorDetector::detect(const QImage& sourceImage,
                                      const PixelRect& regionBbox,
                                      const std::vector<HocrLine>& lines,
                                      int sourceDpi) const {
    BackgroundInfo result;
    // BackgroundInfo defaults isUniform/hasGradient/isComplex to
    // false and confidence to 0.0; only the dominantColor needs an
    // explicit override to white for the "no decision yet" state.
    result.dominantColor = RgbColor{255, 255, 255};

    // Validate image
    if (sourceImage.isNull() || sourceImage.width() == 0 || sourceImage.height() == 0) {
        result.isUniform = true;
        result.confidence = 0.0f;
        return result;
    }

    // Clamp region to image bounds
    const ClampedRect region = clampRectToImage(regionBbox, sourceImage);

    // Handle zero-area regions
    const int width = region.x1 - region.x0;
    const int height = region.y1 - region.y0;

    if (width <= 0 || height <= 0) {
        result.isUniform = true;
        result.confidence = 0.0f;
        return result;
    }

    // Determine strip width based on region size
    int stripWidth = 3;
    if (width < 10 || height < 10) {
        stripWidth = 1;
    }

    std::vector<RgbColor> allSamples;
    allSamples.reserve(10000); // Pre-allocate reasonable capacity

    // 1. Sample edge strips (always performed)
    std::vector<RgbColor> topEdgeSamples, bottomEdgeSamples;
    std::vector<RgbColor> leftEdgeSamples, rightEdgeSamples;

    // Top edge
    sampleHorizontalStrip(sourceImage, region.x0, region.x1, region.y0,
                         std::min(region.y0 + stripWidth, region.y1),
                         topEdgeSamples);
    allSamples.insert(allSamples.end(), topEdgeSamples.begin(), topEdgeSamples.end());

    // Bottom edge
    sampleHorizontalStrip(sourceImage, region.x0, region.x1,
                         std::max(region.y1 - stripWidth, region.y0), region.y1,
                         bottomEdgeSamples);
    allSamples.insert(allSamples.end(), bottomEdgeSamples.begin(), bottomEdgeSamples.end());

    // Left edge
    sampleVerticalStrip(sourceImage, region.x0,
                       std::min(region.x0 + stripWidth, region.x1),
                       region.y0, region.y1,
                       leftEdgeSamples);
    allSamples.insert(allSamples.end(), leftEdgeSamples.begin(), leftEdgeSamples.end());

    // Right edge
    sampleVerticalStrip(sourceImage, std::max(region.x1 - stripWidth, region.x0),
                       region.x1, region.y0, region.y1,
                       rightEdgeSamples);
    allSamples.insert(allSamples.end(), rightEdgeSamples.begin(), rightEdgeSamples.end());

    // 2. Sample inter-line gaps (high-quality background samples)
    int gapSampleCount = 0;
    std::vector<HocrLine> sortedLines = lines;
    std::sort(sortedLines.begin(), sortedLines.end(),
             [](const HocrLine& a, const HocrLine& b) {
                 return std::min(a.bbox.y0, a.bbox.y1) < std::min(b.bbox.y0, b.bbox.y1);
             });

    for (size_t i = 0; i + 1 < sortedLines.size(); ++i) {
        const auto& line1 = sortedLines[i];
        const auto& line2 = sortedLines[i + 1];

        // Find the gap between line1's bottom and line2's top
        const int line1Bottom = static_cast<int>(std::round(std::max(line1.bbox.y0, line1.bbox.y1)));
        const int line2Top = static_cast<int>(std::round(std::min(line2.bbox.y0, line2.bbox.y1)));

        if (line2Top > line1Bottom) {
            // There's a gap - sample it
            const int gapY0 = line1Bottom;
            const int gapY1 = line2Top;

            // Determine horizontal extent (intersection with region)
            const int gapX0 = std::max(region.x0,
                                      static_cast<int>(std::round(std::min(line1.bbox.x0, line2.bbox.x0))));
            const int gapX1 = std::min(region.x1,
                                      static_cast<int>(std::round(std::max(line1.bbox.x1, line2.bbox.x1))));

            if (gapX1 > gapX0 && gapY1 > gapY0) {
                sampleHorizontalStrip(sourceImage, gapX0, gapX1, gapY0, gapY1, allSamples);
                ++gapSampleCount;
            }
        }
    }

    // 3. Analyze collected samples
    if (allSamples.empty()) {
        result.isUniform = true;
        result.confidence = 0.0f;
        return result;
    }

    const auto dominantResult = findDominantColor(allSamples, 30.0);
    result.dominantColor = dominantResult.color;

    // 4. Classify background type
    const float uniformityThreshold = 0.80f;

    if (dominantResult.uniformity >= uniformityThreshold) {
        // Uniform background — hasGradient/isComplex stay false
        // from BackgroundInfo's default init.
        result.isUniform = true;
    } else {
        // Check for gradient
        const RgbColor topMean = computeMeanColor(topEdgeSamples);
        const RgbColor bottomMean = computeMeanColor(bottomEdgeSamples);
        const RgbColor leftMean = computeMeanColor(leftEdgeSamples);
        const RgbColor rightMean = computeMeanColor(rightEdgeSamples);

        const double verticalDist = colorDistance(topMean, bottomMean);
        const double horizontalDist = colorDistance(leftMean, rightMean);

        if (verticalDist > 30.0 || horizontalDist > 30.0) {
            // Gradient detected
            result.isUniform = false;
            result.hasGradient = true;
            result.isComplex = false;

            result.gradient.topLeft = topMean;
            result.gradient.topRight = topMean;
            result.gradient.bottomLeft = bottomMean;
            result.gradient.bottomRight = bottomMean;

            // Refine with left/right if horizontal gradient is stronger
            if (horizontalDist > verticalDist) {
                result.gradient.topLeft = leftMean;
                result.gradient.bottomLeft = leftMean;
                result.gradient.topRight = rightMean;
                result.gradient.bottomRight = rightMean;
            }
        } else {
            // Complex background
            result.isUniform = false;
            result.hasGradient = false;
            result.isComplex = true;
        }
    }

    // 5. Compute confidence
    // Base confidence on sample count and uniformity
    const float sampleFactor = std::min(1.0f, static_cast<float>(allSamples.size()) / 1000.0f);
    const bool hasGapSamples = gapSampleCount > 0;
    const float gapBonus = hasGapSamples ? 0.2f : 0.0f;

    // Cap edge-only confidence at 0.8 (inter-line gap samples are more reliable)
    const float maxConfidence = hasGapSamples ? 1.0f : 0.8f;
    result.confidence = std::min(maxConfidence,
                                dominantResult.uniformity * sampleFactor + gapBonus);

    // B3: run classification pass. Uses variance analysis at a DPI-
    // scaled kernel to distinguish uniform / gradient / halftone /
    // textured. Writes into new fields without overriding the
    // legacy isUniform / hasGradient / isComplex flags downstream
    // code already depends on.
    const int dpi = sourceDpi > 0 ? sourceDpi : 300;
    auto cls = classifyRegion(sourceImage, regionBbox, dpi);
    result.classification = cls.classification;
    result.localVariance = cls.localVariance;
    result.halftoneSpacingPx = cls.halftoneSpacingPx;

    return result;
}

BackgroundInfo BgColorDetector::detect(const QImage& sourceImage,
                                      const PixelRect& regionBbox,
                                      int sourceDpi) const {
    // Call the three-argument version with an empty lines vector
    return detect(sourceImage, regionBbox,
                  std::vector<HocrLine>{}, sourceDpi);
}

// ---------------------------------------------------------------------------
// B3: classifyRegion — DPI-aware variance classifier
// ---------------------------------------------------------------------------
//
// Algorithm:
// 1. Compute a grayscale version of the clipped region (luminance).
// 2. Run a discrete 3x3 Laplacian and take the mean absolute response.
//    This is the "local variance" metric — smooth backgrounds have a
//    near-zero response, noisy/patterned backgrounds have a high one.
// 3. Normalize by 255 to get [0.0, 1.0].
// 4. To distinguish Halftone from Textured: compute the row-wise
//    standard deviation at the expected halftone period
//    (sourceDpi / 85 pixels — 85 LPI is the most common newspaper
//    halftone screen). If the 1D autocorrelation at that offset is
//    significantly higher than at a non-periodic offset, we have a
//    periodic pattern.
// 5. Thresholds:
//      variance < 0.01  -> Uniform
//      variance < 0.04  -> Gradient (smooth but non-uniform)
//      periodic          -> Halftone
//      else              -> Textured
//
// This is a first-cut heuristic; a production system would use a
// DFT/FFT for the period detection. The autocorrelation approach
// is O(W*H) and adequate for classification at test-fixture scales.

BgColorDetector::ClassificationResult BgColorDetector::classifyRegion(
    const QImage& sourceImage,
    const PixelRect& regionBbox,
    int sourceDpi)
{
    ClassificationResult r;
    if (sourceImage.isNull()) return r;

    auto clamped = clampRectToImage(regionBbox, sourceImage);
    const int w = clamped.x1 - clamped.x0 + 1;
    const int h = clamped.y1 - clamped.y0 + 1;
    if (w < 16 || h < 16) {
        return r;
    }

    // Extract luminance into a contiguous grayscale buffer for fast
    // Laplacian + autocorrelation. Downsample to at most 512x512 to
    // keep the cost bounded for large edit regions.
    //
    // CRITICAL: the downsample stride must be smaller than HALF the
    // expected halftone period, or the dot pattern is averaged out
    // before the variance kernel sees it (Nyquist). 85 LPI halftone
    // at 200 DPI has a ~2.35 px period, so stride > 1 erases it.
    const int maxDim = 512;
    const double periodAtSource = static_cast<double>(sourceDpi) / 85.0;
    const int periodCapStride = std::max(1,
        static_cast<int>(std::floor(periodAtSource / 2.0)));
    const int stepX = std::clamp(w / maxDim, 1, periodCapStride);
    const int stepY = std::clamp(h / maxDim, 1, periodCapStride);
    const int dw = w / stepX;
    const int dh = h / stepY;
    if (dw < 8 || dh < 8) {
        return r;
    }

    std::vector<uint8_t> gray(static_cast<size_t>(dw) * dh, 0);
    for (int j = 0; j < dh; ++j) {
        int sy = clamped.y0 + j * stepY;
        for (int i = 0; i < dw; ++i) {
            int sx = clamped.x0 + i * stepX;
            QRgb rgb = sourceImage.pixel(sx, sy);
            // ITU-R BT.601 luminance.
            int lum = (299 * qRed(rgb) + 587 * qGreen(rgb) + 114 * qBlue(rgb)) / 1000;
            gray[static_cast<size_t>(j) * dw + i] = static_cast<uint8_t>(lum);
        }
    }

    // 3x3 Laplacian: sum of absolute response over interior pixels.
    // Kernel is [[0,-1,0],[-1,4,-1],[0,-1,0]].
    double lapSum = 0.0;
    int lapCount = 0;
    for (int j = 1; j < dh - 1; ++j) {
        for (int i = 1; i < dw - 1; ++i) {
            int c = gray[static_cast<size_t>(j) * dw + i];
            int u = gray[static_cast<size_t>(j - 1) * dw + i];
            int d = gray[static_cast<size_t>(j + 1) * dw + i];
            int l = gray[static_cast<size_t>(j) * dw + (i - 1)];
            int rr = gray[static_cast<size_t>(j) * dw + (i + 1)];
            int lap = 4 * c - u - d - l - rr;
            lapSum += std::abs(lap);
            ++lapCount;
        }
    }
    const double meanLap = lapCount ? lapSum / lapCount : 0.0;
    const float normVariance = static_cast<float>(std::min(1.0, meanLap / 255.0));
    r.localVariance = normVariance;

    // Halftone period estimation: expected dot spacing at 85 LPI.
    // Kernel is scaled by the downsample factor stepX so it matches
    // the downsampled buffer's resolution.
    // period_px_at_source was computed above with the downsample math;
    // period_in_downsample = period_px_at_source / stepX.
    const int period = std::max(1, static_cast<int>(std::round(periodAtSource / stepX)));
    const int offNonPeriodic = period + period / 2 + 1;

    // 1D horizontal autocorrelation: mean( (g[x] - g[x+d]) ^ 2 ) over
    // the middle row of the buffer. Compare at d=period vs d=offNonPeriodic.
    auto autocorrDelta = [&](int d) -> double {
        if (d <= 0 || d >= dw) return -1.0;
        int midJ = dh / 2;
        double sum = 0.0;
        int n = 0;
        for (int i = 0; i + d < dw; ++i) {
            int a = gray[static_cast<size_t>(midJ) * dw + i];
            int b = gray[static_cast<size_t>(midJ) * dw + (i + d)];
            int diff = a - b;
            sum += diff * diff;
            ++n;
        }
        return n ? sum / n : -1.0;
    };
    const double periodDelta = autocorrDelta(period);
    const double offDelta = autocorrDelta(offNonPeriodic);

    // Classification thresholds.
    if (normVariance < 0.01f) {
        r.classification = BgClassification::Uniform;
    } else if (normVariance < 0.04f) {
        r.classification = BgClassification::Gradient;
    } else if (periodDelta > 0.0 && offDelta > 0.0 &&
               periodDelta < offDelta * 0.6) {
        // At the expected halftone period, row-to-row difference is
        // systematically lower than at a non-periodic offset — that's
        // the signature of a repeating pattern.
        r.classification = BgClassification::Halftone;
        r.halftoneSpacingPx = static_cast<float>(periodAtSource);
    } else if (normVariance < 0.25f) {
        r.classification = BgClassification::Textured;
    } else {
        r.classification = BgClassification::Complex;
    }
    return r;
}

} // namespace pdfe::tre
