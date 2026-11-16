// Single source of truth for the pixel sample-index arithmetic shared by the
// image/stamp raster writers (ImageManager::insertImage,
// AnnotationManager::setStampImageAppearance, and any future site that copies
// QImage samples into an fz_pixmap).
//
// GitHub #392: the index must be computed in size_t. A plain `int` computation
// overflows for a buffer above ~2^31 samples (e.g. a ~46k-square RGB image, or
// a tall stamp), producing a wild out-of-bounds write (heap corruption / AV).
// Keeping the arithmetic here means both call sites cannot drift apart and a
// single test can exercise the exact function the product uses.
#pragma once

#include <cstddef>

namespace pdfe::image {

/// Row-major sample index into an interleaved, n-channel 8-bit pixel buffer:
/// the first byte of pixel (x, y) in a `width`-wide, n-component image.
/// Computed in std::size_t so it does not overflow 32-bit int for large images.
inline std::size_t imageSampleIndex(int y, int width, int x, int n) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
            + static_cast<std::size_t>(x)) * static_cast<std::size_t>(n);
}

}  // namespace pdfe::image
