#pragma once

#include "tre/TreTypes.h"

#include <QImage>
#include <vector>

namespace pdfe::tre {

/// Detects background color behind text in a scanned page image.
/// Samples edge strips and inter-line gaps to determine whether the
/// background is uniform, gradient, halftone, textured, or complex.
///
/// As of B3 (see docs/image-pdf-risks.md), detect() also performs a
/// local-variance analysis and, when a DPI is supplied, estimates a
/// halftone dot-spacing signature. The classification result is
/// exposed in BackgroundInfo::classification so downstream code can
/// choose between solid fill (safe on Uniform/Gradient) and pattern
/// sampling (required on Halftone/Textured).
class BgColorDetector {
public:
    /// Detect background color for a text region with inter-line gap sampling.
    /// sourceDpi: the working DPI of the sourceImage (per A3). Used to
    /// scale the halftone variance kernel. Pass 0 to use the default 300.
    [[nodiscard]] BackgroundInfo detect(const QImage& sourceImage,
                                        const PixelRect& regionBbox,
                                        const std::vector<HocrLine>& lines,
                                        int sourceDpi = 0) const;

    /// Simplified detect without line data (edge sampling only).
    [[nodiscard]] BackgroundInfo detect(const QImage& sourceImage,
                                        const PixelRect& regionBbox,
                                        int sourceDpi = 0) const;

    /// Classify a sampled region into {Uniform, Gradient, Halftone,
    /// Textured, Complex} using a DPI-aware variance analysis.
    /// Exposed for testing. Operates on a clipped sub-rect of the
    /// source image. Returns a default Unknown when the rect is empty.
    struct ClassificationResult {
        BgClassification classification = BgClassification::Unknown;
        float localVariance = 0.0f;
        float halftoneSpacingPx = 0.0f;
    };
    [[nodiscard]] static ClassificationResult classifyRegion(
        const QImage& sourceImage,
        const PixelRect& regionBbox,
        int sourceDpi);
};

} // namespace pdfe::tre
