#include "RedactionTool.h"
#include "RedactionManager.h"
#include "core/Page.h"
#include "core/CoordinateTransform.h"
#include <QPainter>
#include <QPen>
#include <algorithm>

namespace pdfe::redaction {

RedactionTool::RedactionTool(RedactionManager* manager)
    : manager_(manager)
{
}

pdfe::annotations::AnnotationType RedactionTool::toolType() const {
    return pdfe::annotations::AnnotationType::Unknown;
}

void RedactionTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (!page_)
        return;

    startPoint_ = pdfPoint;
    endPoint_ = pdfPoint;
    currentPageIndex_ = pageIndex;
    isDrawing_ = true;
}

void RedactionTool::onMouseMove(QMouseEvent* /*event*/, int pageIndex, QPointF pdfPoint)
{
    if (!isDrawing_ || pageIndex != currentPageIndex_)
        return;

    endPoint_ = pdfPoint;
}

void RedactionTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !isDrawing_)
        return;

    endPoint_ = pdfPoint;
    isDrawing_ = false;

    // Build bounding rect
    qreal left = std::min(startPoint_.x(), endPoint_.x());
    qreal top = std::min(startPoint_.y(), endPoint_.y());
    qreal right = std::max(startPoint_.x(), endPoint_.x());
    qreal bottom = std::max(startPoint_.y(), endPoint_.y());

    QRectF rect(left, top, right - left, bottom - top);

    // Don't create if rect is too small (accidental click)
    if (rect.width() < 5.0 && rect.height() < 5.0)
        return;

    if (redactionCreated) {
        redactionCreated(currentPageIndex_, rect);
    }
}

void RedactionTool::onPaintOverlay(QPainter& painter, int pageIndex,
                                    float zoom, QPointF pageOrigin, float pageHeightPt)
{
    if (!isDrawing_ || pageIndex != currentPageIndex_)
        return;

    painter.save();

    // Red dashed outline for redaction area
    QPen pen(QColor(200, 0, 0));
    pen.setWidthF(2.0f);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(QColor(200, 0, 0, 40));

    QPointF screenStart = pdfe::core::CoordinateTransform::pdfToScreen(
        startPoint_, zoom, pageOrigin, pageHeightPt);
    QPointF screenEnd = pdfe::core::CoordinateTransform::pdfToScreen(
        endPoint_, zoom, pageOrigin, pageHeightPt);

    qreal left = std::min(screenStart.x(), screenEnd.x());
    qreal top = std::min(screenStart.y(), screenEnd.y());
    qreal right = std::max(screenStart.x(), screenEnd.x());
    qreal bottom = std::max(screenStart.y(), screenEnd.y());

    painter.drawRect(QRectF(QPointF(left, top), QPointF(right, bottom)));

    painter.restore();
}

} // namespace pdfe::redaction
