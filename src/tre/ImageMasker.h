#pragma once
#include "tre/TreTypes.h"
#include <QImage>
#include <vector>
#include <utility>

namespace pdfe::tre {

/// Masks text regions in scanned page images by filling with detected
/// background color and feathered edges. Also composites text bitmaps
/// for Approach C (image-level replacement).
class ImageMasker {
public:
    /// Mask a region with solid background color + feathered edges.
    QImage maskRegion(const QImage& source,
                      const PixelRect& region,
                      const RgbColor& bgColor,
                      int featherRadius = 3) const;

    /// Mask a region with gradient fill + feathered edges.
    QImage maskRegionGradient(const QImage& source,
                              const PixelRect& region,
                              const GradientInfo& gradient,
                              int featherRadius = 3) const;

    /// Mask multiple regions in a single pass.
    QImage maskAllRegions(const QImage& source,
                          const std::vector<std::pair<PixelRect, BackgroundInfo>>& regions) const;

    /// Alpha-composite RGBA text bitmap over source at region position,
    /// filling the erased area with a solid background color.
    /// Legacy entry point — new callers should prefer the overload
    /// that takes the full BackgroundInfo.
    QImage compositeTextBitmap(const QImage& source,
                               const QImage& textBitmap,
                               const PixelRect& region,
                               const RgbColor& bgColor) const;

    /// Alpha-composite RGBA text bitmap over source at region position.
    /// Chooses a fill strategy based on bgInfo.classification:
    ///
    ///   - Uniform / Gradient / Unknown: solid fill with dominantColor
    ///     (backward-compatible with the legacy overload).
    ///   - Halftone: copy a clean background patch from elsewhere in
    ///     the page image, phase-aligned to the halftone dot grid so
    ///     the pattern continues seamlessly across the edit boundary.
    ///   - Textured: copy a clean patch with no phase constraint.
    ///   - Complex: fall back to solid fill (safest).
    ///
    /// See docs/image-pdf-risks.md (B3).
    QImage compositeTextBitmap(const QImage& source,
                               const QImage& textBitmap,
                               const PixelRect& region,
                               const BackgroundInfo& bgInfo) const;

    /// Find a clean background patch within `source` that can be
    /// copied over `destRect` without overlapping text. For halftone
    /// backgrounds, the source offset is snapped to a multiple of
    /// `periodPx` so the dot grid stays phase-aligned.
    ///
    /// Returns true on success and writes the chosen source rect
    /// through `foundSrc`. Returns false when no suitable patch can
    /// be found (e.g. the destination fills most of the image).
    ///
    /// Exposed for testing.
    static bool findBackgroundPatch(const QImage& source,
                                    const QRect& destRect,
                                    int periodPx,
                                    QRect* foundSrc);

    /// Apply box blur to a Grayscale8 image (exposed for testing).
    static QImage boxBlur(const QImage& mask, int passes = 3);
};

} // namespace pdfe::tre
