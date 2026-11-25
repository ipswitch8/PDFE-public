// -*- coding: utf-8 -*-
//
// Independent low-memory reference for a whole-page render (ui-tweaks-3
// phase-2). Renders ONE region of a page, at the same ctm Page::render uses
// (fz_scale(scale, scale)), into a small MuPDF pixmap of its own and compares
// it byte-for-byte with the same pixels of a Page::render raster. Used by the
// large-PDF repro tests to check rows past the 2^31-byte offset of a 3 GB
// raster, and the inserted images, without a second full-size raster.
#pragma once

#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>
#include <QRect>
#include <QRectF>

#include <cstring>
#include <exception>
#include <sstream>
#include <string>

#include <mupdf/fitz.h>

namespace pdfe::core {
// Page befriends this (Page.h) so a test can render the SAME fz_page that
// Page::render draws, independently of Page::render.
struct PageRenderTestAccess {
    static ::fz_page* fzPage(const Page& page) { return page.fzPage(); }
};
}  // namespace pdfe::core

namespace pdfe::testing {

struct BandComparison {
    long pixels = 0;     // compared
    long inked = 0;      // reference pixels with non-zero alpha
    long differing = 0;  // pixels whose 4 bytes differ
    int maxDelta = 0;    // largest per-channel difference among them
    int firstBadX = -1, firstBadY = -1;
    std::string error;   // non-empty: the comparison could not run

    bool ok() const { return error.empty() && pixels > 0 && differing == 0; }
    std::string describe() const
    {
        std::ostringstream s;
        s << pixels << " px compared, " << inked << " inked, " << differing << " differ";
        if (differing) s << " (max channel delta " << maxDelta << ")";
        if (firstBadX >= 0) s << " (first at " << firstBadX << "," << firstBadY << ")";
        if (!error.empty()) s << "; error: " << error;
        return s.str();
    }
};

// The pixel rect of a fitz-space (Y-down points) rect in the Page::render
// raster at @p scale, grown by @p margin pixels and clipped to the raster.
inline QRect rasterRectFor(const QRectF& fitz, float scale, int margin, const QImage& img)
{
    const QRect r(QPoint(int(fitz.left() * scale) - margin, int(fitz.top() * scale) - margin),
                  QPoint(int(fitz.right() * scale) + margin, int(fitz.bottom() * scale) + margin));
    return r.intersected(img.rect());
}

// @p px: pixel rect in @p img (raster coordinates, origin = the raster's
// top-left). The raster's device origin is the page bbox's rounded top-left,
// exactly as Page::render (and fz_new_pixmap_from_page) place it.
inline BandComparison compareBandToReference(pdfe::core::MuPdfContext& ctx,
                                             pdfe::core::Page& page, float scale,
                                             const QImage& img, const QRect& px)
{
    BandComparison out;
    if (img.isNull() || img.format() != QImage::Format_RGBA8888) {
        out.error = "raster is null or not RGBA8888";
        return out;
    }
    if (px.isEmpty() || !img.rect().contains(px)) {
        out.error = "band is empty or outside the raster";
        return out;
    }
    fz_irect full{};
    unsigned char* ref = nullptr;
    int refW = 0, refH = 0;
    ptrdiff_t refStride = 0;
    fz_pixmap* keep = nullptr;
    try {
        ctx.execute([&](fz_context* c) {
            const fz_matrix ctm = fz_scale(scale, scale);
            full = fz_round_rect(fz_transform_rect(fz_bound_page(c, pdfe::core::PageRenderTestAccess::fzPage(page)), ctm));
            const fz_irect band = fz_make_irect(full.x0 + px.left(), full.y0 + px.top(),
                                                full.x0 + px.right() + 1,
                                                full.y0 + px.bottom() + 1);
            fz_pixmap* pix = fz_new_pixmap_with_bbox(c, fz_device_rgb(c), band, nullptr, 1);
            fz_device* dev = nullptr;
            fz_var(dev);
            fz_try(c)
            {
                fz_clear_pixmap(c, pix);
                dev = fz_new_draw_device(c, ctm, pix);
                fz_run_page(c, pdfe::core::PageRenderTestAccess::fzPage(page), dev, fz_identity, nullptr);
                fz_close_device(c, dev);
            }
            fz_always(c) { fz_drop_device(c, dev); }
            fz_catch(c)
            {
                fz_drop_pixmap(c, pix);
                fz_rethrow(c);
            }
            // Accessors, not fields: fz_pixmap's layout is MuPDF-internal.
            keep = pix;
            ref = fz_pixmap_samples(c, pix);
            refW = fz_pixmap_width(c, pix);
            refH = fz_pixmap_height(c, pix);
            refStride = fz_pixmap_stride(c, pix);
        });
    } catch (const std::exception& e) {
        out.error = e.what();
        return out;
    }
    if (refW != px.width() || refH != px.height()) {
        out.error = "reference band size differs from the requested band";
    } else {
        for (int y = 0; y < refH; ++y) {
            const unsigned char* r = ref + static_cast<ptrdiff_t>(y) * refStride;
            const unsigned char* g = img.constScanLine(px.top() + y) +
                                     static_cast<ptrdiff_t>(px.left()) * 4;
            for (int x = 0; x < refW; ++x) {
                ++out.pixels;
                out.inked += r[x * 4 + 3] != 0;
                if (std::memcmp(r + x * 4, g + x * 4, 4) != 0) {
                    if (out.firstBadX < 0) {
                        out.firstBadX = px.left() + x;
                        out.firstBadY = px.top() + y;
                    }
                    ++out.differing;
                    for (int k = 0; k < 4; ++k) {
                        const int d = r[x * 4 + k] > g[x * 4 + k] ? r[x * 4 + k] - g[x * 4 + k]
                                                                  : g[x * 4 + k] - r[x * 4 + k];
                        if (d > out.maxDelta) out.maxDelta = d;
                    }
                }
            }
        }
    }
    ctx.execute([&](fz_context* c) { fz_drop_pixmap(c, keep); });
    return out;
}

}  // namespace pdfe::testing
