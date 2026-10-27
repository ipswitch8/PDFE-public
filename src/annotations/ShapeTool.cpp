#include "ShapeTool.h"
#include "AnnotationManager.h"
#include "../core/Page.h"
#include "../core/CoordinateTransform.h"
#include <QPainter>
#include <QPen>
#include <algorithm>

namespace pdfe::annotations {

ShapeTool::ShapeTool(AnnotationManager* manager, AnnotationType type, QColor color)
    : manager_(manager)
    , type_(type)
    , color_(color)
{
}

void ShapeTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Only respond to left button
    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (!page_) {
        return;
    }

    // Begin drawing shape
    startPoint_ = pdfPoint;
    endPoint_ = pdfPoint;
    currentPageIndex_ = pageIndex;
    isDrawing_ = true;
}

void ShapeTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (!isDrawing_ || pageIndex != currentPageIndex_) {
        return;
    }

    // Update end point for rubber-band effect
    endPoint_ = pdfPoint;
}

void ShapeTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !isDrawing_) {
        return;
    }

    if (!manager_ || !page_) {
        isDrawing_ = false;
        return;
    }

    // Finalize shape
    endPoint_ = pdfPoint;
    isDrawing_ = false;

    // Create bounding rect from start and end points
    qreal left = std::min(startPoint_.x(), endPoint_.x());
    qreal top = std::min(startPoint_.y(), endPoint_.y());
    qreal right = std::max(startPoint_.x(), endPoint_.x());
    qreal bottom = std::max(startPoint_.y(), endPoint_.y());

    QRectF rect(left, top, right - left, bottom - top);

    // Don't create annotation if rect is too small (likely accidental click)
    if (rect.width() < 5.0 && rect.height() < 5.0) {
        return;
    }

    // Create shape annotation
    AnnotationData data = manager_->createAnnotation(page_, type_, rect);
    data.color = color_;
    data.borderWidth = borderWidth_;
    data.pageIndex = currentPageIndex_;

    // For line annotations, set start and end points
    if (type_ == AnnotationType::Line) {
        data.lineStart = startPoint_;
        data.lineEnd = endPoint_;
        manager_->setLineEndpoints(page_, data.annotIndex, startPoint_, endPoint_);
    }

    // Set color
    manager_->setColor(page_, data.annotIndex, color_);

    // Notify that annotation was created
    if (annotationCreated) {
        auto cb = annotationCreated;  // copy: the call may destroy this tool
        cb(currentPageIndex_, data);
    }
}

void ShapeTool::onPaintOverlay(QPainter& painter, int pageIndex,
                                float zoom, QPointF pageOrigin, float pageHeightPt)
{
    if (!isDrawing_ || pageIndex != currentPageIndex_) {
        return;
    }

    painter.save();

    // Set pen for rubber-band effect
    QPen pen(color_);
    pen.setWidthF(borderWidth_ * zoom);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // Convert PDF points to screen coordinates
    QPointF screenStart = pdfe::core::CoordinateTransform::pdfToScreen(
        startPoint_, zoom, pageOrigin, pageHeightPt);
    QPointF screenEnd = pdfe::core::CoordinateTransform::pdfToScreen(
        endPoint_, zoom, pageOrigin, pageHeightPt);

    // Draw rubber-band shape
    if (type_ == AnnotationType::Square) {
        // Draw rectangle
        qreal left = std::min(screenStart.x(), screenEnd.x());
        qreal top = std::min(screenStart.y(), screenEnd.y());
        qreal right = std::max(screenStart.x(), screenEnd.x());
        qreal bottom = std::max(screenStart.y(), screenEnd.y());
        painter.drawRect(QRectF(QPointF(left, top), QPointF(right, bottom)));
    }
    else if (type_ == AnnotationType::Circle) {
        // Draw ellipse
        qreal left = std::min(screenStart.x(), screenEnd.x());
        qreal top = std::min(screenStart.y(), screenEnd.y());
        qreal right = std::max(screenStart.x(), screenEnd.x());
        qreal bottom = std::max(screenStart.y(), screenEnd.y());
        painter.drawEllipse(QRectF(QPointF(left, top), QPointF(right, bottom)));
    }
    else if (type_ == AnnotationType::Line) {
        // Draw line
        painter.drawLine(screenStart, screenEnd);
    }

    painter.restore();
}

} // namespace pdfe::annotations
