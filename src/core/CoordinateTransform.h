#pragma once

#include <QPointF>
#include <QRectF>
#include <QTransform>

namespace pdfe::core {

/**
 * @brief Stateless coordinate transformation between MuPDF and screen coordinates.
 *
 * MuPDF uses "fitz space" coordinates: top-left origin, Y-down, units in points.
 * Screen coordinate system: top-left origin, Y-down, units in pixels.
 *
 * Both coordinate systems are Y-down, so the transformation is simply
 * scale + translate (no Y-flip).
 *
 * The pageHeightPt parameter is retained for API compatibility but is unused.
 *
 * All methods are static and thread-safe.
 */
class CoordinateTransform {
public:
    /**
     * @brief Convert a MuPDF/fitz-space point to screen coordinates.
     *
     * @param pdfPoint Point in MuPDF fitz-space coordinates (origin top-left, Y-down, points)
     * @param zoom Zoom factor (1.0 = 100%)
     * @param pageScreenOrigin Top-left corner of the page in screen coordinates
     * @param pageHeightPt Height of the PDF page in points (unused, retained for compatibility)
     * @return Point in screen coordinates (origin top-left, Y-down, pixels)
     */
    [[nodiscard]] static QPointF pdfToScreen(
        QPointF pdfPoint,
        float zoom,
        QPointF pageScreenOrigin,
        float pageHeightPt
    );

    /**
     * @brief Convert a screen point to MuPDF/fitz-space coordinates.
     *
     * @param screenPoint Point in screen coordinates (origin top-left, Y-down, pixels)
     * @param zoom Zoom factor (1.0 = 100%)
     * @param pageScreenOrigin Top-left corner of the page in screen coordinates
     * @param pageHeightPt Height of the PDF page in points (unused, retained for compatibility)
     * @return Point in MuPDF fitz-space coordinates (origin top-left, Y-down, points)
     */
    [[nodiscard]] static QPointF screenToPdf(
        QPointF screenPoint,
        float zoom,
        QPointF pageScreenOrigin,
        float pageHeightPt
    );

    /**
     * @brief Convert a MuPDF rectangle to screen coordinates.
     *
     * @param pdfRect Rectangle in MuPDF fitz-space coordinates
     * @param zoom Zoom factor (1.0 = 100%)
     * @param pageScreenOrigin Top-left corner of the page in screen coordinates
     * @param pageHeightPt Height of the PDF page in points (unused, retained for compatibility)
     * @return Rectangle in screen coordinates
     */
    [[nodiscard]] static QRectF pdfRectToScreen(
        QRectF pdfRect,
        float zoom,
        QPointF pageScreenOrigin,
        float pageHeightPt
    );

    /**
     * @brief Convert a screen rectangle to MuPDF/fitz-space coordinates.
     *
     * @param screenRect Rectangle in screen coordinates
     * @param zoom Zoom factor (1.0 = 100%)
     * @param pageScreenOrigin Top-left corner of the page in screen coordinates
     * @param pageHeightPt Height of the PDF page in points (unused, retained for compatibility)
     * @return Rectangle in MuPDF fitz-space coordinates
     */
    [[nodiscard]] static QRectF screenRectToPdf(
        QRectF screenRect,
        float zoom,
        QPointF pageScreenOrigin,
        float pageHeightPt
    );

    /**
     * @brief Create a transformation matrix for MuPDF-to-screen conversion.
     *
     * @param zoom Zoom factor (1.0 = 100%)
     * @param pageScreenOrigin Top-left corner of the page in screen coordinates
     * @param pageHeightPt Height of the PDF page in points (unused, retained for compatibility)
     * @return Transformation matrix
     */
    [[nodiscard]] static QTransform pdfToScreenMatrix(
        float zoom,
        QPointF pageScreenOrigin,
        float pageHeightPt
    );

};

} // namespace pdfe::core
