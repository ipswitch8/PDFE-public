#include "TextMarkupTool.h"
#include "AnnotationManager.h"
#include "../core/Page.h"
#include "../core/CoordinateTransform.h"
#include <QPainter>

namespace pdfe::annotations {

TextMarkupTool::TextMarkupTool(AnnotationManager* manager,
                               std::shared_ptr<TextSelector> selector,
                               AnnotationType type,
                               QColor color)
    : manager_(manager)
    , selector_(selector)
    , type_(type)
    , color_(color)
{
}

void TextMarkupTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Only respond to left button
    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (!selector_ || !page_) {
        return;
    }

    // Begin text selection
    selector_->beginSelection(page_, pdfPoint);
    currentPageIndex_ = pageIndex;
    isSelecting_ = true;
}

void TextMarkupTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (!isSelecting_ || !selector_) {
        return;
    }

    // Update selection as user drags
    selector_->updateSelection(pdfPoint);
}

void TextMarkupTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !isSelecting_) {
        return;
    }

    if (!selector_ || !manager_ || !page_) {
        isSelecting_ = false;
        return;
    }

    // End selection and get result
    TextSelection selection = selector_->endSelection();
    isSelecting_ = false;

    // Only create annotation if text was actually selected
    if (selection.quads.empty()) {
        return;
    }

    // Calculate bounding rect from quads
    QRectF boundingRect;
    for (const auto& quad : selection.quads) {
        if (boundingRect.isEmpty()) {
            boundingRect = quad;
        } else {
            boundingRect = boundingRect.united(quad);
        }
    }

    // Create text markup annotation
    AnnotationData data = manager_->createAnnotation(page_, type_, boundingRect);
    data.quadPoints = std::vector<QRectF>(selection.quads.begin(), selection.quads.end());
    data.contents = selection.selectedText.toStdString();
    data.color = color_;
    data.pageIndex = currentPageIndex_;

    // Set quad points and color
    manager_->setQuadPoints(page_, data.annotIndex, data.quadPoints);
    manager_->setColor(page_, data.annotIndex, color_);

    // Notify that annotation was created. CAUTION: the annotationCreated
    // callback may DELETE this tool: creating a text-markup annotation makes
    // MainWindow switch the active tool (onAnnotationCreatedByTool →
    // onAnnotationToolSelected → PdfCanvas::clearCurrentTool), which destroys
    // `this`. So capture what we need, reset our own state FIRST, then fire the
    // callback as the very LAST action and touch no member afterwards. The old
    // order wrote `currentPageIndex_ = -1` AFTER the callback — a
    // heap-use-after-free on the freed tool (ASan: TextMarkupTool.cpp:93), whose
    // heap corruption later surfaced as a QString/QTextEdit paint crash (#48).
    const int notifyPageIndex = currentPageIndex_;
    currentPageIndex_ = -1;
    if (annotationCreated) {
        auto cb = annotationCreated;  // copy: the call may destroy this tool
        cb(notifyPageIndex, data);
    }
    // Do NOT access any member here — `this` may already be destroyed.
}

void TextMarkupTool::onPaintOverlay(QPainter& painter, int pageIndex,
                                     float zoom, QPointF pageOrigin, float pageHeightPt)
{
    // Only paint overlay on the page where selection is happening
    if (!isSelecting_ || pageIndex != currentPageIndex_ || !selector_) {
        return;
    }

    // Get current selection quads
    std::vector<QRectF> quads = selector_->currentQuads();
    if (quads.empty()) {
        return;
    }

    // Set semi-transparent color for preview
    QColor previewColor = color_;
    previewColor.setAlpha(100);

    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(previewColor);

    // Draw each quad
    for (const auto& pdfRect : quads) {
        QRectF screenRect = pdfe::core::CoordinateTransform::pdfRectToScreen(
            pdfRect, zoom, pageOrigin, pageHeightPt);
        painter.drawRect(screenRect);
    }

    painter.restore();
}

} // namespace pdfe::annotations
