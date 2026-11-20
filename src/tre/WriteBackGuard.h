#pragma once
// -*- coding: utf-8 -*-
//
// Write-back size guard for ImageReplacer (security-audit follow-up, phase 6
// of pipeline image-placement-state; docs/image-placement-model.md).
//
// The region an OCR edit writes back is the edit rect scaled into the target
// image's own pixel grid. A placement with a tiny scale (still invertible)
// makes that scale enormous, and a non-finite matrix makes it NaN or infinite.
// The destination size used to be unclamped, and converting a huge or
// non-finite double to int is undefined. ImageReplacer checks every value with
// these helpers first and refuses the edit, before anything is allocated.

#include <QSize>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace pdfe::tre::writeback {

// Caps: 16384 pixels a side and 100 million pixels in all (the bound
// ExifStripper uses for its fallback decode), or the target image's own size
// when that is larger: it is already decoded, and an edit inside it never
// needs more.
inline constexpr int kMaxSide = 16384;
inline constexpr double kMaxPixels = 100'000'000.0;

/// Finite and well inside int range (|v| < 2^30), so a cast or lround of it
/// is defined.
inline bool intSafe(double v) {
    return std::isfinite(v) && std::abs(v) < 1073741824.0;
}

/// Every entry of `t` is finite.
inline bool transformIsFinite(const QTransform& t) {
    for (const double v : {t.m11(), t.m12(), t.m13(), t.m21(), t.m22(), t.m23(), t.m31(),
                           t.m32(), t.m33()}) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

/// Whether a write-back region of w x h pixels, into an image of `target`'s
/// size, may be allocated.
inline bool sizeIsSane(int w, int h, const QSize& target) {
    const int side = std::max({kMaxSide, target.width(), target.height()});
    const double pixels =
        std::max(kMaxPixels, double(target.width()) * double(target.height()));
    return w >= 1 && h >= 1 && w <= side && h <= side && double(w) * double(h) <= pixels;
}

}  // namespace pdfe::tre::writeback
