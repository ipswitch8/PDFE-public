#pragma once
// -*- coding: utf-8 -*-
//
// Shared PIXEL truth-oracle helpers.
//
// tests/support/VisualOracle.h reads back GLYPHS (fitz stext). That is blind to
// anything that is not text -- raster image XObjects, redaction fills, line art.
// This header is its raster counterpart: rasterise a page exactly as a viewer
// would and sample authored coordinates, so image-family oracles can assert on
// real pixels instead of on "did not throw".
//
// Header-only; include as "support/RasterOracle.h" (the tests target adds
// tests/ to its include path).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

namespace pdfe::testsupport {

/// A linear sRGB raster, 3 bytes per pixel, row-major with no row padding.
struct Raster {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgb;

    bool valid() const {
        return w > 0 && h > 0 && rgb.size() == static_cast<size_t>(w) * h * 3;
    }
};

/// An RGB triple carried as doubles so block means stay exact.
struct RgbD {
    double r = -1.0;
    double g = -1.0;
    double b = -1.0;
};

/// Rec.601 luma. Used for "is this darker than that" oracles, which hold under
/// any monotone colour transform (and therefore under colour management).
inline double luminance(const RgbD& p) {
    return 0.299 * p.r + 0.587 * p.g + 0.114 * p.b;
}

/// Rasterise `pageNum` of `pdf` at `scale` onto an OPAQUE WHITE background.
///
/// alpha = 0 makes MuPDF clear the pixmap to white before drawing, which is
/// what a viewer shows and what fixtures without an explicit page-background
/// operator assume. On failure the returned Raster is invalid and `*err`
/// (when non-null) carries a short reason.
inline Raster renderPageRaster(const std::filesystem::path& pdf,
                               int pageNum = 0,
                               float scale = 1.0f,
                               std::string* err = nullptr) {
    // Everything with a non-trivial destructor is constructed BEFORE fz_try:
    // fz_try uses setjmp/longjmp and does not unwind C++ stack objects.
    Raster out;
    std::string localErr;
    const std::string path = pdf.string();

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) {
        if (err) *err = "fz_new_context failed";
        return out;
    }
    fz_register_document_handlers(ctx);

    fz_document* doc = nullptr;
    fz_page* page = nullptr;
    fz_pixmap* pix = nullptr;
    int w = 0, h = 0, stride = 0, n = 0;
    const uint8_t* samples = nullptr;

    fz_try(ctx) {
        doc = fz_open_document(ctx, path.c_str());
        page = fz_load_page(ctx, doc, pageNum);
        pix = fz_new_pixmap_from_page(ctx, page, fz_scale(scale, scale),
                                      fz_device_rgb(ctx), 0);
        w = pix->w;
        h = pix->h;
        stride = pix->stride;
        n = pix->n;
        samples = pix->samples;
    }
    fz_catch(ctx) {
        localErr = "page render threw";
    }

    if (localErr.empty()) {
        if (samples && n == 3 && w > 0 && h > 0) {
            out.w = w;
            out.h = h;
            out.rgb.resize(static_cast<size_t>(w) * h * 3);
            for (int y = 0; y < h; ++y) {
                const uint8_t* src = samples + static_cast<size_t>(y) * stride;
                std::copy(src, src + static_cast<size_t>(w) * 3,
                          out.rgb.begin() + static_cast<size_t>(y) * w * 3);
            }
        } else {
            localErr = "unexpected pixmap shape";
        }
    }

    if (pix) fz_drop_pixmap(ctx, pix);
    if (page) fz_drop_page(ctx, page);
    if (doc) fz_drop_document(ctx, doc);
    fz_drop_context(ctx);

    if (err) *err = localErr;
    return out;
}

/// Mean colour of the (2*halfSide)^2 block centred on (cx, cy), clipped to the
/// raster. Returns {-1,-1,-1} if the block lies entirely outside.
///
/// Sampling a small block rather than a single pixel smooths single-pixel
/// resampling artefacts without letting a neighbouring authored patch bleed in
/// (callers size the block well inside one patch).
inline RgbD sampleBlockMean(const Raster& r, double cx, double cy,
                            int halfSide = 3) {
    double sr = 0, sg = 0, sb = 0;
    long count = 0;
    const int x0 = static_cast<int>(std::lround(cx)) - halfSide;
    const int y0 = static_cast<int>(std::lround(cy)) - halfSide;
    for (int dy = 0; dy < halfSide * 2; ++dy) {
        for (int dx = 0; dx < halfSide * 2; ++dx) {
            const int x = x0 + dx;
            const int y = y0 + dy;
            if (x < 0 || y < 0 || x >= r.w || y >= r.h) continue;
            const size_t i = (static_cast<size_t>(y) * r.w + x) * 3;
            sr += r.rgb[i];
            sg += r.rgb[i + 1];
            sb += r.rgb[i + 2];
            ++count;
        }
    }
    if (count == 0) return RgbD{};
    return RgbD{sr / count, sg / count, sb / count};
}

/// Fraction of pixels in the block that are "ink" (luma below `threshold`).
/// A solid authored black patch returns ~1.0; a blank white one ~0.0.
inline double darkFractionInBlock(const Raster& r, double cx, double cy,
                                  int halfSide = 3, double threshold = 128.0) {
    long dark = 0;
    long count = 0;
    const int x0 = static_cast<int>(std::lround(cx)) - halfSide;
    const int y0 = static_cast<int>(std::lround(cy)) - halfSide;
    for (int dy = 0; dy < halfSide * 2; ++dy) {
        for (int dx = 0; dx < halfSide * 2; ++dx) {
            const int x = x0 + dx;
            const int y = y0 + dy;
            if (x < 0 || y < 0 || x >= r.w || y >= r.h) continue;
            const size_t i = (static_cast<size_t>(y) * r.w + x) * 3;
            const RgbD p{static_cast<double>(r.rgb[i]),
                         static_cast<double>(r.rgb[i + 1]),
                         static_cast<double>(r.rgb[i + 2])};
            if (luminance(p) < threshold) ++dark;
            ++count;
        }
    }
    if (count == 0) return -1.0;
    return static_cast<double>(dark) / static_cast<double>(count);
}

}  // namespace pdfe::testsupport
