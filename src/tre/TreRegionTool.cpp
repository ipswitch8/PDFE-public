#include "TreRegionTool.h"
#include "annotations/AnnotationType.h"
#include "core/CoordinateTransform.h"
#include <QPainter>
#include <QPen>
#include <algorithm>

namespace pdfe::tre {

TreRegionTool::TreRegionTool()
{
}

pdfe::annotations::AnnotationType TreRegionTool::toolType() const
{
    return pdfe::annotations::AnnotationType::Unknown;
}

void TreRegionTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Only respond to left button
    if (event->button() != Qt::LeftButton) {
        return;
    }

    // Begin drawing region
    startPoint_ = pdfPoint;
    endPoint_ = pdfPoint;
    currentPageIndex_ = pageIndex;
    isDrawing_ = true;
}

void TreRegionTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (!isDrawing_ || pageIndex != currentPageIndex_) {
        return;
    }

    // Update end point for rubber-band effect
    endPoint_ = pdfPoint;
}

void TreRegionTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !isDrawing_) {
        return;
    }

    // Finalize region
    endPoint_ = pdfPoint;
    isDrawing_ = false;

    // Create bounding rect from start and end points
    qreal left = std::min(startPoint_.x(), endPoint_.x());
    qreal top = std::min(startPoint_.y(), endPoint_.y());
    qreal right = std::max(startPoint_.x(), endPoint_.x());
    qreal bottom = std::max(startPoint_.y(), endPoint_.y());

    QRectF rect(left, top, right - left, bottom - top);

    // Don't fire callback if rect is too small (likely accidental click)
    if (rect.width() < 5.0 && rect.height() < 5.0) {
        currentPageIndex_ = -1;
        return;
    }

    // Fire callback with page index and PDF rect
    if (regionSelected) {
        regionSelected(currentPageIndex_, rect);
    }

    currentPageIndex_ = -1;
}

void TreRegionTool::onPaintOverlay(QPainter& painter, int pageIndex,
                                    float zoom, QPointF pageOrigin, float pageHeightPt)
{
    if (!isDrawing_ || pageIndex != currentPageIndex_) {
        return;
    }

    painter.save();

    // Set pen for rubber-band effect (blue color)
    QPen pen(QColor(0, 120, 215));
    pen.setWidthF(2.0f * zoom);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);

    // Set semi-transparent blue fill
    painter.setBrush(QColor(0, 120, 215, 30));

    // Convert PDF points to screen coordinates
    QPointF screenStart = pdfe::core::CoordinateTransform::pdfToScreen(
        startPoint_, zoom, pageOrigin, pageHeightPt);
    QPointF screenEnd = pdfe::core::CoordinateTransform::pdfToScreen(
        endPoint_, zoom, pageOrigin, pageHeightPt);

    // Draw rubber-band rectangle
    qreal left = std::min(screenStart.x(), screenEnd.x());
    qreal top = std::min(screenStart.y(), screenEnd.y());
    qreal right = std::max(screenStart.x(), screenEnd.x());
    qreal bottom = std::max(screenStart.y(), screenEnd.y());

    painter.drawRect(QRectF(QPointF(left, top), QPointF(right, bottom)));

    painter.restore();
}

} // namespace pdfe::tre
