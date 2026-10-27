#pragma once

#include <QImage>
#include <QSize>

namespace pdfe::tre {

/// Lanczos-3 (a = 3) separable resampler for C1 of the image-quality
/// polish bundle. Produces noticeably sharper upscales for text bitmaps
/// than Qt::SmoothTransformation (which is bilinear). The 3-lobe window
/// is the de-facto standard for text / line-art upscaling — wider than
/// cubic, narrower than sinc.
///
/// Separable: horizontal 1D pass, then vertical 1D pass. O(W*H*6).
class LanczosResize {
public:
    /// Resize `src` to `dstSize` using Lanczos-3. Input is converted to
    /// QImage::Format_ARGB32 if needed; output is ARGB32 as well. Empty
    /// source or non-positive target size returns a default QImage.
    static QImage resize(const QImage& src, const QSize& dstSize);

    /// Convenience: use Lanczos-3 when the maximum linear ratio is
    /// >= `lanczosThreshold` (default 1.5 per the image-pdf-risks doc),
    /// else fall back to QImage::scaled with Qt::SmoothTransformation.
    /// Keeps small rescales cheap while sharpening 2× upscales.
    static QImage resizeBest(const QImage& src, const QSize& dstSize,
                             float lanczosThreshold = 1.5f);

    /// Exposed for testing — the 1D Lanczos-3 kernel.
    /// lanczos3(0) = 1, lanczos3(x) = 0 for |x| >= 3.
    static double kernel(double x);
};

} // namespace pdfe::tre
