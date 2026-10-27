#include "InkTool.h"
#include "AnnotationManager.h"
#include "../core/Page.h"
#include "../core/CoordinateTransform.h"
#include <QPainter>
#include <QPen>

namespace pdfe::annotations {

InkTool::InkTool(AnnotationManager* manager)
    : manager_(manager)
{
}

void InkTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Only respond to left button
    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (!page_) {
        return;
    }

    // Begin new stroke
    currentStroke_.clear();
    currentStroke_.push_back(pdfPoint);
    currentPageIndex_ = pageIndex;
    isDrawing_ = true;
}

void InkTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (!isDrawing_ || pageIndex != currentPageIndex_) {
        return;
    }

    // Add point to current stroke
    currentStroke_.push_back(pdfPoint);
}

void InkTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !isDrawing_) {
        return;
    }

    if (!manager_ || !page_) {
        isDrawing_ = false;
        currentStroke_.clear();
        return;
    }

    // Finalize current stroke
    if (!currentStroke_.empty()) {
        currentStroke_.push_back(pdfPoint);
        allStrokes_.push_back(currentStroke_);
    }

    isDrawing_ = false;
    currentStroke_.clear();

    // Only create annotation if we have strokes
    if (allStrokes_.empty()) {
        return;
    }

    // Calculate bounding rect from all strokes
    QRectF boundingRect;
    for (const auto& stroke : allStrokes_) {
        for (const auto& pt : stroke) {
            if (boundingRect.isEmpty()) {
                boundingRect = QRectF(pt, QSizeF(0, 0));
            } else {
                boundingRect = boundingRect.united(QRectF(pt, QSizeF(0, 0)));
            }
        }
    }

    // Expand rect slightly for pen width
    boundingRect.adjust(-width_, -width_, width_, width_);

    // Create ink annotation
    AnnotationData data = manager_->createAnnotation(page_, AnnotationType::Ink, boundingRect);
    data.inkStrokes = allStrokes_;
    data.color = color_;
    data.borderWidth = width_;
    data.pageIndex = currentPageIndex_;

    // Set ink strokes and color
    manager_->setInkStrokes(page_, data.annotIndex, allStrokes_);
    manager_->setColor(page_, data.annotIndex, color_);

    // Notify that annotation was created. CAUTION: the annotationCreated
    // callback runs SYNCHRONOUSLY and may DESTROY this tool --
    // PdfCanvas::annotationCreated -> MainWindow::onAnnotationCreatedByTool ->
    // onAnnotationToolSelected -> PdfCanvas::clearCurrentTool ->
    // unique_ptr<AnnotationTool>::reset() destroys the very InkTool whose
    // onMouseRelease is on the stack. So reset our own state FIRST and make
    // the callback the LAST statement; nothing may touch `this` afterwards.
    //
    // This is the identical defect DCA 0009 fixed in TextMarkupTool. It
    // survived here because that fix was written against the one instance the
    // crash dump named. Found by the class-level oracle
    // AnnotationToolsTest.NoToolTouchesItselfAfterFiringAnnotationCreated
    // (phase-42), which drives every tool with a self-deleting callback.
    const int notifyPageIndex = currentPageIndex_;
    allStrokes_.clear();
    currentPageIndex_ = -1;
    if (annotationCreated) {
        auto cb = annotationCreated;  // copy: the call may destroy this tool
        cb(notifyPageIndex, data);
    }
    // Do NOT access any member here -- `this` may already be destroyed.
}

void InkTool::onPaintOverlay(QPainter& painter, int pageIndex,
                              float zoom, QPointF pageOrigin, float pageHeightPt)
{
    if (pageIndex != currentPageIndex_) {
        return;
    }

    painter.save();

    // Set pen for drawing
    QPen pen(color_);
    pen.setWidthF(width_ * zoom);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // Draw completed strokes
    for (const auto& stroke : allStrokes_) {
        if (stroke.size() < 2) {
            continue;
        }

        // Convert PDF points to screen points
        std::vector<QPointF> screenPoints;
        screenPoints.reserve(stroke.size());
        for (const auto& pdfPoint : stroke) {
            QPointF screenPoint = pdfe::core::CoordinateTransform::pdfToScreen(
                pdfPoint, zoom, pageOrigin, pageHeightPt);
            screenPoints.push_back(screenPoint);
        }

        // Draw polyline
        painter.drawPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()));
    }

    // Draw current stroke being drawn
    if (isDrawing_ && currentStroke_.size() >= 2) {
        std::vector<QPointF> screenPoints;
        screenPoints.reserve(currentStroke_.size());
        for (const auto& pdfPoint : currentStroke_) {
            QPointF screenPoint = pdfe::core::CoordinateTransform::pdfToScreen(
                pdfPoint, zoom, pageOrigin, pageHeightPt);
            screenPoints.push_back(screenPoint);
        }

        painter.drawPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()));
    }

    painter.restore();
}

} // namespace pdfe::annotations
