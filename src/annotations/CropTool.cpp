#include "CropTool.h"
#include "../core/CoordinateTransform.h"

#include <QBrush>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <algorithm>

namespace pdfe::annotations {

void CropTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton) return;
    if (activePage_ >= 0 && pageIndex != activePage_) return;
    dragging_ = true;
    startPdf_ = pdfPoint;
    endPdf_ = pdfPoint;
    activePage_ = pageIndex;
}

void CropTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    (void)event;
    if (!dragging_ || pageIndex != activePage_) return;
    endPdf_ = pdfPoint;
}

void CropTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !dragging_) return;
    if (pageIndex != activePage_) {
        dragging_ = false;
        return;
    }
    endPdf_ = pdfPoint;
    dragging_ = false;

    const qreal left   = std::min(startPdf_.x(), endPdf_.x());
    const qreal top    = std::min(startPdf_.y(), endPdf_.y());
    const qreal right  = std::max(startPdf_.x(), endPdf_.x());
    const qreal bottom = std::max(startPdf_.y(), endPdf_.y());
    QRectF rect(left, top, right - left, bottom - top);
    if (rect.width() < MIN_CROP_PT || rect.height() < MIN_CROP_PT) {
        return;
    }
    if (cropApplied) {
        cropApplied(pageIndex, rect);
    }
}

void CropTool::onPaintOverlay(QPainter& painter, int pageIndex,
                              float zoom, QPointF pageOrigin,
                              float pageHeightPt)
{
    if (!dragging_ || pageIndex != activePage_) return;

    painter.save();

    QPointF screenStart = pdfe::core::CoordinateTransform::pdfToScreen(
        startPdf_, zoom, pageOrigin, pageHeightPt);
    QPointF screenEnd = pdfe::core::CoordinateTransform::pdfToScreen(
        endPdf_, zoom, pageOrigin, pageHeightPt);

    qreal left   = std::min(screenStart.x(), screenEnd.x());
    qreal top    = std::min(screenStart.y(), screenEnd.y());
    qreal right  = std::max(screenStart.x(), screenEnd.x());
    qreal bottom = std::max(screenStart.y(), screenEnd.y());
    QRectF box(QPointF(left, top), QPointF(right, bottom));

    // Dim the area outside the crop rectangle so the user sees what
    // will be retained vs cropped away. Then draw a solid border on
    // the crop rect itself.
    QColor dim(0, 0, 0, 90);
    painter.setBrush(dim);
    painter.setPen(Qt::NoPen);
    QRectF clip(pageOrigin.x(), pageOrigin.y(),
                painter.viewport().width(), painter.viewport().height());
    QPainterPath outside;
    outside.addRect(clip);
    QPainterPath inside;
    inside.addRect(box);
    painter.drawPath(outside.subtracted(inside));

    QPen pen(QColor(255, 100, 100));
    pen.setWidthF(2.0f);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(box);

    painter.restore();
}

} // namespace pdfe::annotations
