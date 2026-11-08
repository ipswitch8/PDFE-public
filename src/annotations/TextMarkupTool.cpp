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
    // GitHub #367: remember the drag anchor for the no-text fallback rect.
    pressPoint_ = pdfPoint;
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

    // Build the quad list and bounding rect for the markup.
    std::vector<QRectF> quads;
    QRectF boundingRect;

    if (!selection.quads.empty()) {
        // Text path (unchanged): mark the selected text's per-line quads.
        quads.assign(selection.quads.begin(), selection.quads.end());
        for (const auto& quad : quads) {
            if (boundingRect.isEmpty()) {
                boundingRect = quad;
            } else {
                boundingRect = boundingRect.united(quad);
            }
        }
    } else {
        // GitHub #367. No text was under the drag. Highlight (and the other
        // text-markup tools) used to return here and create nothing, so they
        // "only worked on text". Fall back to the dragged RECT so a markup is
        // still created over the dragged region on non-text content (scanned
        // pages, images, blank areas). Per the phase-1 disposition.
        const QRectF dragRect = QRectF(pressPoint_, pdfPoint).normalized();
        // A click with no real drag marks nothing — guard the degenerate rect
        // so a stray single click does not drop a zero-size annotation.
        if (dragRect.width() < 1.0 && dragRect.height() < 1.0) {
            currentPageIndex_ = -1;
            return;
        }
        boundingRect = dragRect;
        quads.push_back(dragRect);   // region markup: one quad == the region
    }

    // Create text markup annotation
    AnnotationData data = manager_->createAnnotation(page_, type_, boundingRect);
    data.quadPoints = quads;
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
