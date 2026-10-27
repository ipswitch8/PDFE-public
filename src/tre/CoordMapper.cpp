#include "tre/CoordMapper.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/Error.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pdfe::tre {

// =========================================================================
// Construction / destruction / move
// =========================================================================

CoordMapper::CoordMapper(pdfe::core::MuPdfContext* context,
                         pdfe::core::Document* document)
    : context_(context)
    , document_(document)
{
    if (!context_) {
        throw pdfe::core::PdfeException("CoordMapper: MuPdfContext is null");
    }
    if (!document_) {
        throw pdfe::core::PdfeException("CoordMapper: Document is null");
    }
}

CoordMapper::~CoordMapper() = default;

CoordMapper::CoordMapper(CoordMapper&&) noexcept = default;
CoordMapper& CoordMapper::operator=(CoordMapper&&) noexcept = default;

// =========================================================================
// Transform access
// =========================================================================

const AffineTransform& CoordMapper::getTransform(int pageIndex)
{
    auto it = cache_.find(pageIndex);
    if (it != cache_.end()) {
        return it->second;
    }

    AffineTransform tf = computeTransform(pageIndex);
    auto [inserted, _] = cache_.emplace(pageIndex, tf);
    return inserted->second;
}

// =========================================================================
// Pixel -> PDF conversion
// =========================================================================

PdfPoint CoordMapper::pixelToPdf(PixelPoint px, const AffineTransform& tf) const
{
    PdfPoint result;

    // Both pixel space and fitz-space are Y-down (top-left origin).
    // No Y-flip is applied. For standard PDF coordinates (Y-up), the caller
    // must convert separately using: standardPdfY = pageHeight - fitzY.
    switch (tf.rotation) {
    case 0:
    default:
        result.x = px.x * tf.scaleX + tf.translateX;
        result.y = px.y * tf.scaleY + tf.translateY;
        break;

    case 90:
        result.x = px.y * tf.scaleX + tf.translateX;
        result.y = (tf.imageWidth - px.x) * tf.scaleY + tf.translateY;
        break;

    case 180:
        result.x = (tf.imageWidth - px.x) * tf.scaleX + tf.translateX;
        result.y = (tf.imageHeight - px.y) * tf.scaleY + tf.translateY;
        break;

    case 270:
        result.x = (tf.imageHeight - px.y) * tf.scaleX + tf.translateX;
        result.y = px.x * tf.scaleY + tf.translateY;
        break;
    }

    return result;
}

PdfRect CoordMapper::pixelRectToPdf(PixelRect px, const AffineTransform& tf) const
{
    // Transform all four corners, then take the axis-aligned bounding box.
    // This correctly handles arbitrary rotations.
    const PixelPoint corners[4] = {
        {px.x0, px.y0},
        {px.x1, px.y0},
        {px.x1, px.y1},
        {px.x0, px.y1}
    };

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();

    for (const auto& corner : corners) {
        PdfPoint p = pixelToPdf(corner, tf);
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }

    return PdfRect{minX, minY, maxX, maxY};
}

// =========================================================================
// PDF -> Pixel conversion (inverse of pixelToPdf)
// =========================================================================

PixelPoint CoordMapper::pdfToPixel(PdfPoint pt, const AffineTransform& tf) const
{
    PixelPoint result;

    // Each case is the algebraic inverse of the corresponding pixelToPdf case.
    // Both coordinate systems are Y-down (no Y-flip).
    switch (tf.rotation) {
    case 0:
    default:
        // Forward:  pdfX = px.x * scaleX + translateX
        //           pdfY = px.y * scaleY + translateY
        // Inverse:  px.x = (pdfX - translateX) / scaleX
        //           px.y = (pdfY - translateY) / scaleY
        result.x = (pt.x - tf.translateX) / tf.scaleX;
        result.y = (pt.y - tf.translateY) / tf.scaleY;
        break;

    case 90:
        // Forward:  pdfX = px.y * scaleX + translateX
        //           pdfY = (imageWidth - px.x) * scaleY + translateY
        // Inverse:  px.y = (pdfX - translateX) / scaleX
        //           px.x = imageWidth - (pdfY - translateY) / scaleY
        result.x = tf.imageWidth - (pt.y - tf.translateY) / tf.scaleY;
        result.y = (pt.x - tf.translateX) / tf.scaleX;
        break;

    case 180:
        // Forward:  pdfX = (imageWidth - px.x) * scaleX + translateX
        //           pdfY = (imageHeight - px.y) * scaleY + translateY
        // Inverse:  px.x = imageWidth - (pdfX - translateX) / scaleX
        //           px.y = imageHeight - (pdfY - translateY) / scaleY
        result.x = tf.imageWidth - (pt.x - tf.translateX) / tf.scaleX;
        result.y = tf.imageHeight - (pt.y - tf.translateY) / tf.scaleY;
        break;

    case 270:
        // Forward:  pdfX = (imageHeight - px.y) * scaleX + translateX
        //           pdfY = px.x * scaleY + translateY
        // Inverse:  px.y = imageHeight - (pdfX - translateX) / scaleX
        //           px.x = (pdfY - translateY) / scaleY
        result.x = (pt.y - tf.translateY) / tf.scaleY;
        result.y = tf.imageHeight - (pt.x - tf.translateX) / tf.scaleX;
        break;
    }

    return result;
}

PixelRect CoordMapper::pdfRectToPixel(PdfRect pt, const AffineTransform& tf) const
{
    // Transform all four corners, then take the axis-aligned bounding box.
    const PdfPoint corners[4] = {
        {pt.x0, pt.y0},
        {pt.x1, pt.y0},
        {pt.x1, pt.y1},
        {pt.x0, pt.y1}
    };

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();

    for (const auto& corner : corners) {
        PixelPoint p = pdfToPixel(corner, tf);
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }

    return PixelRect{minX, minY, maxX, maxY};
}

// =========================================================================
// Size conversion (no translation, pure scaling)
// =========================================================================

double CoordMapper::pixelSizeToPdfPt(double sizePx, double dpi)
{
    return sizePx * 72.0 / dpi;
}

double CoordMapper::pdfPtToPixelSize(double sizePt, double dpi)
{
    return sizePt * dpi / 72.0;
}

// =========================================================================
// Cache management
// =========================================================================

void CoordMapper::invalidateCache(int pageIndex)
{
    cache_.erase(pageIndex);
}

void CoordMapper::invalidateAllCaches()
{
    cache_.clear();
}

// =========================================================================
// Internal: compute transform from page metadata
// =========================================================================

AffineTransform CoordMapper::buildTransform(const QRectF& bbox, int rotation)
{
    // Page dimensions in PDF points (72 pt/inch).
    double pageWidth = bbox.width();
    double pageHeight = bbox.height();

    // Estimate image dimensions in pixels.
    // For a scanned page, the image typically fills the entire page area.
    // At the default scan DPI, each PDF point maps to (dpi / 72) pixels.
    double imageWidth = pageWidth * kDefaultDpi / 72.0;
    double imageHeight = pageHeight * kDefaultDpi / 72.0;

    // Scale factors: how many PDF points per pixel.
    // scaleX = pageWidth / imageWidth  which simplifies to 72 / dpi,
    // but we keep the general form for clarity and future per-page DPI support.
    double scaleX = pageWidth / imageWidth;
    double scaleY = pageHeight / imageHeight;

    AffineTransform tf;
    tf.scaleX = scaleX;
    tf.scaleY = scaleY;
    tf.translateX = bbox.x();
    tf.translateY = bbox.y();
    tf.flipY = false;  // Both pixel and fitz-space are Y-down; no flip needed
    tf.rotation = ((rotation % 360) + 360) % 360;  // normalize to [0, 360)
    tf.imageWidth = imageWidth;
    tf.imageHeight = imageHeight;
    tf.pageWidth = pageWidth;
    tf.pageHeight = pageHeight;

    return tf;
}

AffineTransform CoordMapper::computeTransform(int pageIndex)
{
    // Load the page to query its bounding box and rotation.
    // std::unique_ptr<Page> keeps the page alive for the duration of this scope.
    std::unique_ptr<pdfe::core::Page> pg = document_->page(pageIndex);

    QRectF bbox = pg->boundingBox();
    int rotation = pg->rotation();

    // Page-level transform: the page /Rotate applies to every region because it
    // is baked into the rendered bitmap that OCR operates on. Per-run rotation
    // (defect #6) is handled separately via computeRunTransform().
    return buildTransform(bbox, rotation);
}

// =========================================================================
// Per-run rotation (defect #6)
// =========================================================================

int CoordMapper::decomposeRotationDegrees(double a, double b, double c, double d)
{
    // The rotation of a 2x3 affine's linear part is the angle of its first
    // basis vector (a, b). atan2(b, a) yields the angle in radians; snapping to
    // the nearest quarter turn keeps us within the {0,90,180,270} domain that
    // page /Rotate and the pixel<->PDF switch statements understand. A
    // degenerate (zero) linear part is treated as no rotation.
    if ((a == 0.0 && b == 0.0) || !std::isfinite(a) || !std::isfinite(b) ||
        !std::isfinite(c) || !std::isfinite(d)) {
        return 0;
    }
    double radians = std::atan2(b, a);
    double degrees = radians * 180.0 / 3.14159265358979323846;
    // Snap to nearest 90 and normalize to [0, 360).
    int snapped = static_cast<int>(std::lround(degrees / 90.0)) * 90;
    return ((snapped % 360) + 360) % 360;
}

int CoordMapper::residualRotationDegrees(int pageRotation, int runRotationDegrees)
{
    int residual = pageRotation - runRotationDegrees;
    return ((residual % 360) + 360) % 360;
}

AffineTransform CoordMapper::computeRunTransform(int pageIndex,
                                                 double runA, double runB,
                                                 double runC, double runD)
{
    std::unique_ptr<pdfe::core::Page> pg = document_->page(pageIndex);

    QRectF bbox = pg->boundingBox();
    int pageRotation = pg->rotation();

    // Decompose the rotation the run's own Tm*CTM already encodes, then apply
    // only the residual page rotation on top — preventing the double-rotation
    // that buildTransform(bbox, pageRotation) would produce for a self-rotated
    // run (e.g. a quarter-turned heading on a /Rotate 90 page).
    int runRotation = decomposeRotationDegrees(runA, runB, runC, runD);
    int residual = residualRotationDegrees(pageRotation, runRotation);
    return buildTransform(bbox, residual);
}

} // namespace pdfe::tre
