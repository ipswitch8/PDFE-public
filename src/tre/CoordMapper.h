#pragma once

#include "tre/TreTypes.h"

#include <QRectF>
#include <unordered_map>

namespace pdfe::core {
class MuPdfContext;
class Document;
} // namespace pdfe::core

namespace pdfe::tre {

/// Maps coordinates between pixel space (top-left origin, Y-down, used by OCR/HOCR)
/// and MuPDF fitz-space (top-left origin, Y-down, 72 points/inch).
///
/// Both coordinate systems are Y-down (top-left origin). The pixel coordinate
/// system comes from the rasterized page image that Tesseract operates on,
/// while fitz-space is what MuPDF and PdfCanvas use for all UI coordinates.
///
/// NOTE: If you need standard PDF coordinates (bottom-left origin, Y-up) for
/// raw content stream operators (Tm, etc.), convert at the write boundary
/// using: standardPdfY = pageHeight - fitzY
///
/// Transforms are computed lazily per page and cached. The cache can be
/// invalidated when page geometry changes (rotation, crop, etc.).
class CoordMapper {
public:
    /// Construct a CoordMapper bound to a document.
    /// @param context  MuPDF context for safe API access (caller retains ownership).
    /// @param document Document whose pages will be mapped (caller retains ownership).
    CoordMapper(pdfe::core::MuPdfContext* context, pdfe::core::Document* document);
    ~CoordMapper();

    // Non-copyable, movable
    CoordMapper(const CoordMapper&) = delete;
    CoordMapper& operator=(const CoordMapper&) = delete;
    CoordMapper(CoordMapper&&) noexcept;
    CoordMapper& operator=(CoordMapper&&) noexcept;

    /// Get or compute the affine transform for a page.
    /// The result is cached; subsequent calls for the same pageIndex return
    /// a reference to the cached transform without recomputation.
    /// @param pageIndex  Zero-based page index.
    /// @return Const reference to the cached AffineTransform.
    /// @throws pdfe::core::PdfeException if pageIndex is out of range.
    const AffineTransform& getTransform(int pageIndex);

    // -----------------------------------------------------------------
    // Pixel -> PDF coordinate conversion
    // -----------------------------------------------------------------

    /// Convert a single point from pixel space to PDF space.
    [[nodiscard]] PdfPoint pixelToPdf(PixelPoint px, const AffineTransform& tf) const;

    /// Convert a rectangle from pixel space to PDF space.
    /// All four corners are transformed and the axis-aligned bounding box is
    /// returned, which handles rotation correctly.
    [[nodiscard]] PdfRect pixelRectToPdf(PixelRect px, const AffineTransform& tf) const;

    // -----------------------------------------------------------------
    // PDF -> Pixel coordinate conversion
    // -----------------------------------------------------------------

    /// Convert a single point from PDF space to pixel space.
    [[nodiscard]] PixelPoint pdfToPixel(PdfPoint pt, const AffineTransform& tf) const;

    /// Convert a rectangle from PDF space to pixel space.
    /// All four corners are transformed and the axis-aligned bounding box is
    /// returned, which handles rotation correctly.
    [[nodiscard]] PixelRect pdfRectToPixel(PdfRect pt, const AffineTransform& tf) const;

    // -----------------------------------------------------------------
    // Size conversion (scaling only, no translation)
    // -----------------------------------------------------------------

    /// Convert a size in pixels to PDF points using the given DPI.
    [[nodiscard]] static double pixelSizeToPdfPt(double sizePx, double dpi);

    /// Convert a size in PDF points to pixels using the given DPI.
    [[nodiscard]] static double pdfPtToPixelSize(double sizePt, double dpi);

    // -----------------------------------------------------------------
    // Per-run rotation (defect #6)
    // -----------------------------------------------------------------

    /// Decompose the rotation encoded in the linear part [a b c d] of a 2x3
    /// affine matrix (fitz Tm*CTM convention: x' = a*x + c*y + e,
    /// y' = b*x + d*y + f) and snap it to the nearest multiple of 90 in
    /// {0, 90, 180, 270}. This is the rotation a text run's OWN matrix already
    /// applies, independent of the page-level /Rotate. A non-rotated run
    /// returns 0; a run whose Tm encodes a quarter turn returns 90/180/270.
    [[nodiscard]] static int decomposeRotationDegrees(double a, double b,
                                                      double c, double d);

    /// Residual page rotation CoordMapper must still apply to a text run.
    ///
    /// The page /Rotate is baked into the rendered bitmap that OCR sees, so the
    /// page-level transform normally rotates every region by pageRotation. But
    /// when a run's own Tm*CTM ALREADY encodes (part of) that rotation, applying
    /// the full page rotation on top double-rotates the run. The residual is the
    /// rotation the run does NOT already account for:
    ///   residual = normalize(pageRotation - runRotationDegrees)  (mod 360).
    /// When the run carries no rotation this equals pageRotation (unchanged
    /// behavior); when the run already matches the page rotation it is 0.
    [[nodiscard]] static int residualRotationDegrees(int pageRotation,
                                                     int runRotationDegrees);

    /// Like getTransform(), but for a specific text run whose own text matrix
    /// (Tm) combined with the CTM may already encode part or all of the page
    /// rotation. runA..runD are the linear part of the run's effective Tm*CTM
    /// (fitz convention). The returned transform applies ONLY the residual page
    /// rotation not already accounted for by the run, eliminating the
    /// double-rotation that plain getTransform() would produce for a
    /// self-rotated run on a /Rotate page.
    ///
    /// The transform is NOT cached (it is run-specific, not page-specific).
    /// @throws pdfe::core::PdfeException if pageIndex is out of range.
    [[nodiscard]] AffineTransform computeRunTransform(int pageIndex,
                                                      double runA, double runB,
                                                      double runC, double runD);

    // -----------------------------------------------------------------
    // Cache management
    // -----------------------------------------------------------------

    /// Invalidate the cached transform for a single page.
    /// The next call to getTransform() for this page will recompute it.
    void invalidateCache(int pageIndex);

    /// Invalidate all cached transforms.
    void invalidateAllCaches();

private:
    /// Compute the pixel-to-PDF affine transform for the given page.
    /// This loads the page via MuPDF to read its bounding box and rotation.
    AffineTransform computeTransform(int pageIndex);

    /// Build the affine transform from a page bounding box and an effective
    /// rotation (degrees). Shared by computeTransform() (rotation = page
    /// /Rotate) and computeRunTransform() (rotation = residual after the run's
    /// own Tm rotation is subtracted). Uses kDefaultDpi for the pixel scale.
    static AffineTransform buildTransform(const QRectF& bbox, int rotation);

    pdfe::core::MuPdfContext* context_ = nullptr;
    pdfe::core::Document* document_ = nullptr;
    std::unordered_map<int, AffineTransform> cache_;

    /// Default scan resolution assumed when actual DPI is unknown.
    static constexpr double kDefaultDpi = 300.0;
};

} // namespace pdfe::tre
