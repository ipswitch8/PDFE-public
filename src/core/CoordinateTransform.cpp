#include "CoordinateTransform.h"

namespace pdfe::core {

QPointF CoordinateTransform::pdfToScreen(
    QPointF pdfPoint,
    float zoom,
    QPointF pageScreenOrigin,
    float /*pageHeightPt*/
) {
    // MuPDF fitz-space and screen space are both Y-down.
    // Transform: scale by zoom, then translate to page position.
    const float screenX = pdfPoint.x() * zoom + pageScreenOrigin.x();
    const float screenY = pdfPoint.y() * zoom + pageScreenOrigin.y();

    return QPointF(screenX, screenY);
}

QPointF CoordinateTransform::screenToPdf(
    QPointF screenPoint,
    float zoom,
    QPointF pageScreenOrigin,
    float /*pageHeightPt*/
) {
    // Inverse of pdfToScreen: remove page offset, then divide by zoom.
    const float pdfX = (screenPoint.x() - pageScreenOrigin.x()) / zoom;
    const float pdfY = (screenPoint.y() - pageScreenOrigin.y()) / zoom;

    return QPointF(pdfX, pdfY);
}

QRectF CoordinateTransform::pdfRectToScreen(
    QRectF pdfRect,
    float zoom,
    QPointF pageScreenOrigin,
    float pageHeightPt
) {
    // Both coordinate systems are Y-down, so the rect corners map directly.
    const QPointF topLeft = pdfToScreen(
        pdfRect.topLeft(),
        zoom,
        pageScreenOrigin,
        pageHeightPt
    );

    const float width = pdfRect.width() * zoom;
    const float height = pdfRect.height() * zoom;

    return QRectF(topLeft.x(), topLeft.y(), width, height);
}

QRectF CoordinateTransform::screenRectToPdf(
    QRectF screenRect,
    float zoom,
    QPointF pageScreenOrigin,
    float pageHeightPt
) {
    // Both coordinate systems are Y-down, so the rect corners map directly.
    const QPointF topLeft = screenToPdf(
        screenRect.topLeft(),
        zoom,
        pageScreenOrigin,
        pageHeightPt
    );

    const float width = screenRect.width() / zoom;
    const float height = screenRect.height() / zoom;

    return QRectF(topLeft.x(), topLeft.y(), width, height);
}

QTransform CoordinateTransform::pdfToScreenMatrix(
    float zoom,
    QPointF pageScreenOrigin,
    float /*pageHeightPt*/
) {
    QTransform transform;

    // Simple scale + translate (no Y-flip needed)
    transform.scale(zoom, zoom);
    transform.translate(
        pageScreenOrigin.x() / zoom,
        pageScreenOrigin.y() / zoom
    );

    return transform;
}

} // namespace pdfe::core
