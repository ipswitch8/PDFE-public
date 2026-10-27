#include "tre/ImageMasker.h"
#include <QPainter>
#include <QColor>
#include <algorithm>
#include <cmath>

namespace pdfe::tre {

namespace {

/// Clamp a PixelRect to the bounds of an image.
PixelRect clampToBounds(const PixelRect& rect, int imageWidth, int imageHeight) {
    PixelRect clamped;
    clamped.x0 = std::max(0.0, std::min(rect.x0, static_cast<double>(imageWidth)));
    clamped.y0 = std::max(0.0, std::min(rect.y0, static_cast<double>(imageHeight)));
    clamped.x1 = std::max(0.0, std::min(rect.x1, static_cast<double>(imageWidth)));
    clamped.y1 = std::max(0.0, std::min(rect.y1, static_cast<double>(imageHeight)));

    // Ensure x0 <= x1 and y0 <= y1
    if (clamped.x0 > clamped.x1) std::swap(clamped.x0, clamped.x1);
    if (clamped.y0 > clamped.y1) std::swap(clamped.y0, clamped.y1);

    return clamped;
}

/// Check if a region is valid (non-zero area).
bool isValidRegion(const PixelRect& rect) {
    return rect.x1 > rect.x0 && rect.y1 > rect.y0;
}

/// Interpolate between two colors based on parameter t (0.0 to 1.0).
RgbColor lerpColor(const RgbColor& a, const RgbColor& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return RgbColor{
        static_cast<uint8_t>(a.r + (b.r - a.r) * t),
        static_cast<uint8_t>(a.g + (b.g - a.g) * t),
        static_cast<uint8_t>(a.b + (b.b - a.b) * t)
    };
}

/// Bilinearly interpolate a gradient color at position (x, y) within region.
RgbColor interpolateGradient(const GradientInfo& gradient,
                             double x, double y,
                             const PixelRect& region) {
    // Compute normalized coordinates (0.0 to 1.0)
    double width = region.x1 - region.x0;
    double height = region.y1 - region.y0;

    double tx = (width > 0) ? (x - region.x0) / width : 0.0;
    double ty = (height > 0) ? (y - region.y0) / height : 0.0;

    tx = std::clamp(tx, 0.0, 1.0);
    ty = std::clamp(ty, 0.0, 1.0);

    // Interpolate horizontally at top and bottom
    RgbColor topColor = lerpColor(gradient.topLeft, gradient.topRight, tx);
    RgbColor bottomColor = lerpColor(gradient.bottomLeft, gradient.bottomRight, tx);

    // Interpolate vertically
    return lerpColor(topColor, bottomColor, ty);
}

/// Composite source and fill colors based on mask value (0-255).
QRgb compositeColors(QRgb sourceColor, const RgbColor& fillColor, uint8_t maskValue) {
    if (maskValue == 0) {
        return sourceColor;
    }
    if (maskValue == 255) {
        return qRgb(fillColor.r, fillColor.g, fillColor.b);
    }

    // result = source * (255 - maskVal)/255 + fill * maskVal/255
    int invMask = 255 - maskValue;
    int srcR = qRed(sourceColor);
    int srcG = qGreen(sourceColor);
    int srcB = qBlue(sourceColor);

    int r = (srcR * invMask + fillColor.r * maskValue) / 255;
    int g = (srcG * invMask + fillColor.g * maskValue) / 255;
    int b = (srcB * invMask + fillColor.b * maskValue) / 255;

    return qRgb(r, g, b);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

QImage ImageMasker::boxBlur(const QImage& mask, int passes) {
    if (mask.format() != QImage::Format_Grayscale8) {
        return mask;
    }

    if (passes <= 0 || mask.width() == 0 || mask.height() == 0) {
        return mask;
    }

    QImage input = mask;
    QImage output(mask.size(), QImage::Format_Grayscale8);

    for (int pass = 0; pass < passes; ++pass) {
        // Blur input into output
        for (int y = 0; y < input.height(); ++y) {
            uchar* outLine = output.scanLine(y);

            for (int x = 0; x < input.width(); ++x) {
                // 3x3 box blur - sum all pixels in neighborhood
                int sum = 0;
                int count = 0;

                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = x + dx;
                        int ny = y + dy;

                        // Clamp to image bounds
                        if (nx >= 0 && nx < input.width() && ny >= 0 && ny < input.height()) {
                            sum += input.constScanLine(ny)[nx];
                            count++;
                        }
                    }
                }

                outLine[x] = static_cast<uchar>(sum / count);
            }
        }

        // Swap for next pass
        input = output;
    }

    return output;
}

QImage ImageMasker::maskRegion(const QImage& source,
                                const PixelRect& region,
                                const RgbColor& bgColor,
                                int featherRadius) const {
    // Ensure source is in a format we can work with
    QImage result = source;
    if (result.format() != QImage::Format_ARGB32 && result.format() != QImage::Format_RGB32) {
        result = result.convertToFormat(QImage::Format_ARGB32);
    } else {
        result = result.copy();
    }

    if (result.isNull()) {
        return source;
    }

    // Clamp region to image bounds
    PixelRect clampedRegion = clampToBounds(region, result.width(), result.height());
    if (!isValidRegion(clampedRegion)) {
        return result;
    }

    // Create mask image (full size, initialized to 0)
    QImage mask(result.size(), QImage::Format_Grayscale8);
    mask.fill(0);

    // Fill the region rectangle with 255
    int x0 = static_cast<int>(std::floor(clampedRegion.x0));
    int y0 = static_cast<int>(std::floor(clampedRegion.y0));
    int x1 = static_cast<int>(std::ceil(clampedRegion.x1));
    int y1 = static_cast<int>(std::ceil(clampedRegion.y1));

    for (int y = y0; y < y1 && y < mask.height(); ++y) {
        uchar* maskLine = mask.scanLine(y);
        for (int x = x0; x < x1 && x < mask.width(); ++x) {
            maskLine[x] = 255;
        }
    }

    // Apply box blur to create feathered edges
    int blurPasses = std::max(1, featherRadius);
    mask = boxBlur(mask, blurPasses);

    // Composite source with background color using mask
    for (int y = 0; y < result.height(); ++y) {
        const uchar* maskLine = mask.constScanLine(y);
        QRgb* resultLine = reinterpret_cast<QRgb*>(result.scanLine(y));

        for (int x = 0; x < result.width(); ++x) {
            uint8_t maskValue = maskLine[x];
            if (maskValue > 0) {
                resultLine[x] = compositeColors(resultLine[x], bgColor, maskValue);
            }
        }
    }

    return result;
}

QImage ImageMasker::maskRegionGradient(const QImage& source,
                                        const PixelRect& region,
                                        const GradientInfo& gradient,
                                        int featherRadius) const {
    // Ensure source is in a format we can work with
    QImage result = source;
    if (result.format() != QImage::Format_ARGB32 && result.format() != QImage::Format_RGB32) {
        result = result.convertToFormat(QImage::Format_ARGB32);
    } else {
        result = result.copy();
    }

    if (result.isNull()) {
        return source;
    }

    // Clamp region to image bounds
    PixelRect clampedRegion = clampToBounds(region, result.width(), result.height());
    if (!isValidRegion(clampedRegion)) {
        return result;
    }

    // Create mask image (full size, initialized to 0)
    QImage mask(result.size(), QImage::Format_Grayscale8);
    mask.fill(0);

    // Fill the region rectangle with 255
    int x0 = static_cast<int>(std::floor(clampedRegion.x0));
    int y0 = static_cast<int>(std::floor(clampedRegion.y0));
    int x1 = static_cast<int>(std::ceil(clampedRegion.x1));
    int y1 = static_cast<int>(std::ceil(clampedRegion.y1));

    for (int y = y0; y < y1 && y < mask.height(); ++y) {
        uchar* maskLine = mask.scanLine(y);
        for (int x = x0; x < x1 && x < mask.width(); ++x) {
            maskLine[x] = 255;
        }
    }

    // Apply box blur to create feathered edges
    int blurPasses = std::max(1, featherRadius);
    mask = boxBlur(mask, blurPasses);

    // Composite source with gradient fill using mask
    for (int y = 0; y < result.height(); ++y) {
        const uchar* maskLine = mask.constScanLine(y);
        QRgb* resultLine = reinterpret_cast<QRgb*>(result.scanLine(y));

        for (int x = 0; x < result.width(); ++x) {
            uint8_t maskValue = maskLine[x];
            if (maskValue > 0) {
                // Compute gradient color at this pixel
                RgbColor fillColor = interpolateGradient(gradient, x, y, clampedRegion);
                resultLine[x] = compositeColors(resultLine[x], fillColor, maskValue);
            }
        }
    }

    return result;
}

QImage ImageMasker::maskAllRegions(const QImage& source,
                                    const std::vector<std::pair<PixelRect, BackgroundInfo>>& regions) const {
    if (regions.empty()) {
        return source;
    }

    // Ensure source is in a format we can work with
    QImage result = source;
    if (result.format() != QImage::Format_ARGB32 && result.format() != QImage::Format_RGB32) {
        result = result.convertToFormat(QImage::Format_ARGB32);
    } else {
        result = result.copy();
    }

    if (result.isNull()) {
        return source;
    }

    // Create a single mask for all regions
    QImage mask(result.size(), QImage::Format_Grayscale8);
    mask.fill(0);

    // Store gradient info for each pixel that needs it
    struct GradientPixel {
        GradientInfo gradient;
        PixelRect region;
    };
    std::vector<std::vector<std::optional<GradientPixel>>> gradientMap(
        result.height(),
        std::vector<std::optional<GradientPixel>>(result.width())
    );

    // Store solid color info for each pixel
    std::vector<std::vector<std::optional<RgbColor>>> colorMap(
        result.height(),
        std::vector<std::optional<RgbColor>>(result.width())
    );

    // Fill mask and maps for all regions
    for (const auto& [region, bgInfo] : regions) {
        PixelRect clampedRegion = clampToBounds(region, result.width(), result.height());
        if (!isValidRegion(clampedRegion)) {
            continue;
        }

        int x0 = static_cast<int>(std::floor(clampedRegion.x0));
        int y0 = static_cast<int>(std::floor(clampedRegion.y0));
        int x1 = static_cast<int>(std::ceil(clampedRegion.x1));
        int y1 = static_cast<int>(std::ceil(clampedRegion.y1));

        for (int y = y0; y < y1 && y < mask.height(); ++y) {
            uchar* maskLine = mask.scanLine(y);
            for (int x = x0; x < x1 && x < mask.width(); ++x) {
                maskLine[x] = 255;

                // Store fill info
                if (bgInfo.hasGradient) {
                    gradientMap[y][x] = GradientPixel{bgInfo.gradient, clampedRegion};
                } else {
                    colorMap[y][x] = bgInfo.dominantColor;
                }
            }
        }
    }

    // Apply box blur once to the combined mask
    mask = boxBlur(mask, 3);

    // Composite all regions in a single pass
    for (int y = 0; y < result.height(); ++y) {
        const uchar* maskLine = mask.constScanLine(y);
        QRgb* resultLine = reinterpret_cast<QRgb*>(result.scanLine(y));

        for (int x = 0; x < result.width(); ++x) {
            uint8_t maskValue = maskLine[x];
            if (maskValue > 0) {
                RgbColor fillColor;

                // Determine fill color (gradient or solid)
                if (gradientMap[y][x].has_value()) {
                    const auto& gp = gradientMap[y][x].value();
                    fillColor = interpolateGradient(gp.gradient, x, y, gp.region);
                } else if (colorMap[y][x].has_value()) {
                    fillColor = colorMap[y][x].value();
                } else {
                    // Shouldn't happen, but default to white
                    fillColor = RgbColor{255, 255, 255};
                }

                resultLine[x] = compositeColors(resultLine[x], fillColor, maskValue);
            }
        }
    }

    return result;
}

QImage ImageMasker::compositeTextBitmap(const QImage& source,
                                        const QImage& textBitmap,
                                        const PixelRect& region,
                                        const RgbColor& bgColor) const {
    // Legacy entry point — synthesize a Uniform BackgroundInfo and
    // delegate to the new overload so both paths stay consistent.
    BackgroundInfo bg;
    bg.dominantColor = bgColor;
    bg.isUniform = true;
    bg.classification = BgClassification::Uniform;
    return compositeTextBitmap(source, textBitmap, region, bg);
}

namespace {

/// Quick "does this patch look like clean background?" heuristic.
/// Counts dark pixels (luminance < 96) in a downsampled sweep of
/// the rect. On uniform backgrounds, scanned text makes ~15-30%
/// of pixels dark — so a 12% threshold cleanly rejects text.
///
/// Halftone and heavily textured backgrounds naturally contain
/// many "dark" pixels (the dot grid). We detect this case by also
/// looking at the target rect: if the dark fraction in the source
/// patch is no more than ~1.5x the dark fraction in an already-
/// known-clean neighborhood, the patch is considered background
/// regardless of absolute darkness. `baselineDarkFrac` is the
/// caller's reference; pass 0.0 for the old uniform-background
/// heuristic.
bool looksLikeCleanBackground(const QImage& img, const QRect& rect,
                              double baselineDarkFrac = 0.0) {
    if (rect.width() <= 0 || rect.height() <= 0) return false;
    const int stepX = std::max(1, rect.width() / 64);
    const int stepY = std::max(1, rect.height() / 64);
    int darkCount = 0;
    int total = 0;
    for (int y = rect.top(); y <= rect.bottom(); y += stepY) {
        for (int x = rect.left(); x <= rect.right(); x += stepX) {
            if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) continue;
            QRgb rgb = img.pixel(x, y);
            int lum = (299 * qRed(rgb) + 587 * qGreen(rgb) + 114 * qBlue(rgb)) / 1000;
            if (lum < 96) ++darkCount;
            ++total;
        }
    }
    if (total == 0) return false;
    double darkFrac = static_cast<double>(darkCount) / total;

    if (baselineDarkFrac > 0.0) {
        // Halftone / textured mode: reject only when the patch has
        // noticeably MORE dark content than the baseline. Allow up
        // to 1.5x the baseline plus a 5% margin.
        const double allowed = baselineDarkFrac * 1.5 + 0.05;
        return darkFrac <= allowed;
    }
    // Uniform background mode: 12% dark pixel cap.
    return darkFrac < 0.12;
}

/// Measure the dark-pixel fraction of a reference region, used as
/// the baseline for halftone-mode cleanliness checks.
double measureDarkFrac(const QImage& img, const QRect& rect) {
    if (rect.width() <= 0 || rect.height() <= 0) return 0.0;
    const int stepX = std::max(1, rect.width() / 64);
    const int stepY = std::max(1, rect.height() / 64);
    int darkCount = 0;
    int total = 0;
    for (int y = rect.top(); y <= rect.bottom(); y += stepY) {
        for (int x = rect.left(); x <= rect.right(); x += stepX) {
            if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) continue;
            QRgb rgb = img.pixel(x, y);
            int lum = (299 * qRed(rgb) + 587 * qGreen(rgb) + 114 * qBlue(rgb)) / 1000;
            if (lum < 96) ++darkCount;
            ++total;
        }
    }
    return total ? static_cast<double>(darkCount) / total : 0.0;
}

} // anonymous namespace

bool ImageMasker::findBackgroundPatch(const QImage& source,
                                      const QRect& destRect,
                                      int periodPx,
                                      QRect* foundSrc) {
    if (source.isNull() || destRect.isEmpty()) return false;

    const int W = destRect.width();
    const int H = destRect.height();
    const int imgW = source.width();
    const int imgH = source.height();

    // Baseline dark-pixel fraction for the halftone / textured
    // cleanliness heuristic. Only used when periodPx > 1 (periodic
    // patterns). We sample a small reference rect as far from the
    // destination as possible — ideally the top margin of the image,
    // which is usually clean page background.
    double baselineDarkFrac = 0.0;
    if (periodPx > 1) {
        const int refW = std::min(imgW / 4, 200);
        const int refH = std::min(imgH / 4, 100);
        QRect ref(imgW / 2 - refW / 2, 0, refW, refH);
        // If the top-center rect overlaps destRect (edit is near
        // the top of the page), fall back to bottom-center.
        if (ref.intersects(destRect)) {
            ref = QRect(imgW / 2 - refW / 2,
                        std::max(0, imgH - refH - 1), refW, refH);
        }
        if (!ref.intersects(destRect)) {
            baselineDarkFrac = measureDarkFrac(source, ref);
        }
    }

    // Snap a shift to a multiple of the halftone period so the dot
    // grid stays phase-aligned. For period <= 1 (textured / no
    // period), any shift works.
    auto snap = [periodPx](int v) -> int {
        if (periodPx <= 1) return v;
        int s = v / periodPx;
        if (s == 0) {
            s = (v > 0) ? 1 : -1;
        }
        return s * periodPx;
    };

    // Candidate source offsets relative to destRect's top-left,
    // ordered from "most likely clean" to "farther afield". The
    // left/right offsets slide a full rect width so they land in a
    // neighboring line gap or page margin. Above/below offsets
    // slide a full rect height. Diagonal fallbacks try harder.
    const int shiftW = snap(W);
    const int shiftH = snap(H);
    std::vector<std::pair<int, int>> candidates;
    candidates.reserve(16);
    candidates.push_back({0, -shiftH});  // directly above
    candidates.push_back({0,  shiftH});  // directly below
    candidates.push_back({-shiftW, 0});  // to the left
    candidates.push_back({ shiftW, 0});  // to the right
    // Farther sliders for when adjacent lines are also text-heavy.
    candidates.push_back({0, -2 * shiftH});
    candidates.push_back({0,  2 * shiftH});
    candidates.push_back({-shiftW, -shiftH});
    candidates.push_back({ shiftW, -shiftH});
    candidates.push_back({-shiftW,  shiftH});
    candidates.push_back({ shiftW,  shiftH});
    candidates.push_back({0, -3 * shiftH});
    candidates.push_back({0,  3 * shiftH});

    for (const auto& shift : candidates) {
        const int dx = shift.first;
        const int dy = shift.second;
        QRect src(destRect.x() + dx, destRect.y() + dy, W, H);
        // Must fit fully inside the image.
        if (src.x() < 0 || src.y() < 0 ||
            src.right() >= imgW || src.bottom() >= imgH) {
            continue;
        }
        // Must not overlap destination (no point copying text onto text).
        if (src.intersects(destRect)) {
            continue;
        }
        // Must look like clean background (not text). For halftone
        // and textured backgrounds we pass the measured baseline
        // dark fraction so the heuristic accepts patches with a
        // halftone-similar dark-pixel density.
        if (!looksLikeCleanBackground(source, src, baselineDarkFrac)) {
            continue;
        }
        if (foundSrc) *foundSrc = src;
        return true;
    }
    return false;
}

QImage ImageMasker::compositeTextBitmap(const QImage& source,
                                        const QImage& textBitmap,
                                        const PixelRect& region,
                                        const BackgroundInfo& bgInfo) const {
    // Ensure source is in a format we can work with
    QImage result = source;
    if (result.format() != QImage::Format_ARGB32 && result.format() != QImage::Format_RGB32) {
        result = result.convertToFormat(QImage::Format_ARGB32);
    } else {
        result = result.copy();
    }

    if (result.isNull() || textBitmap.isNull()) {
        return source;
    }

    // Clamp region to image bounds
    PixelRect clampedRegion = clampToBounds(region, result.width(), result.height());
    if (!isValidRegion(clampedRegion)) {
        return result;
    }

    const QRect qrect(
        static_cast<int>(std::floor(clampedRegion.x0)),
        static_cast<int>(std::floor(clampedRegion.y0)),
        static_cast<int>(std::ceil(clampedRegion.x1 - clampedRegion.x0)),
        static_cast<int>(std::ceil(clampedRegion.y1 - clampedRegion.y0))
    );

    // B3: choose fill strategy based on classification.
    bool patternCopied = false;
    if (bgInfo.classification == BgClassification::Halftone ||
        bgInfo.classification == BgClassification::Textured) {
        int period = 1;
        if (bgInfo.classification == BgClassification::Halftone &&
            bgInfo.halftoneSpacingPx > 0.5f) {
            period = std::max(1, static_cast<int>(
                std::round(bgInfo.halftoneSpacingPx)));
        }
        QRect srcRect;
        if (findBackgroundPatch(result, qrect, period, &srcRect)) {
            // Copy the clean patch onto the destination, then draw
            // the text bitmap over it with normal alpha blending.
            QImage patch = result.copy(srcRect);
            QPainter painter(&result);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(qrect.topLeft(), patch);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter.drawImage(qrect.topLeft(), textBitmap);
            painter.end();
            patternCopied = true;
        }
    }

    if (!patternCopied) {
        QPainter painter(&result);
        painter.fillRect(qrect, QColor(bgInfo.dominantColor.r,
                                       bgInfo.dominantColor.g,
                                       bgInfo.dominantColor.b));
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.drawImage(qrect.topLeft(), textBitmap);
        painter.end();
    }

    return result;
}

} // namespace pdfe::tre
