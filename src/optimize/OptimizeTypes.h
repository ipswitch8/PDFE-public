#pragma once

#include <cstdint>
#include <string>

namespace pdfe::optimize {

/// Options for OptimizeEngine::run. Each boolean maps to a category the
/// per-option reporting can break down by.
struct OptimizeOptions {
    bool compressStreams   = true;   // deflate all streams
    bool recompressImages  = true;   // re-encode images (MuPDF-native)
    bool subsetFonts       = true;   // subset embedded fonts
    bool removeUnused      = true;   // garbage-collect unreferenced objects
    bool mergeDuplicates   = true;   // merge identical objects (garbage level 3+)
    bool sanitizeStreams   = false;  // rewrite content streams (lossy cleanup)
    bool linearize         = false;  // Fast Web View (reorders objects)
};

/// Result of an optimize run.
struct OptimizeResult {
    uint64_t sizeBeforeBytes = 0;
    uint64_t sizeAfterBytes  = 0;

    /// Per-category byte savings, best-effort estimates from MuPDF's save
    /// options (not a guaranteed breakdown).
    uint64_t savedByCompress   = 0;
    uint64_t savedByImages     = 0;
    uint64_t savedByFonts      = 0;
    uint64_t savedByGarbage    = 0;

    /// Error message if the optimize run failed. Empty on success.
    std::string errorMessage;

    bool success() const { return errorMessage.empty(); }
    int64_t savedBytes() const {
        return static_cast<int64_t>(sizeBeforeBytes)
             - static_cast<int64_t>(sizeAfterBytes);
    }
    double savedPercent() const {
        if (sizeBeforeBytes == 0) return 0.0;
        return 100.0 * static_cast<double>(savedBytes())
                     / static_cast<double>(sizeBeforeBytes);
    }
};

} // namespace pdfe::optimize
