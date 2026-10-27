#include "StickyNoteTool.h"
#include "AnnotationManager.h"
#include "../core/Page.h"

namespace pdfe::annotations {

StickyNoteTool::StickyNoteTool(AnnotationManager* manager)
    : manager_(manager)
{
}

void StickyNoteTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Only respond to left button
    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (!manager_ || !page_) {
        return;
    }

    // Create sticky note annotation at click point
    // The rect is a small square centered on the click point
    QRectF rect(
        pdfPoint.x() - ICON_SIZE / 2.0f,
        pdfPoint.y() - ICON_SIZE / 2.0f,
        ICON_SIZE,
        ICON_SIZE
    );

    AnnotationData data = manager_->createAnnotation(page_, AnnotationType::Text, rect);
    data.color = color_;
    data.iconName = iconName_;
    data.pageIndex = pageIndex;

    // Set initial empty contents
    manager_->setContents(page_, data.annotIndex, "");
    manager_->setColor(page_, data.annotIndex, color_);

    // Notify that annotation was created
    if (annotationCreated) {
        auto cb = annotationCreated;  // copy: the call may destroy this tool
        cb(pageIndex, data);
    }
}

void StickyNoteTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Sticky notes are created on press, no move action needed
    (void)event;
    (void)pageIndex;
    (void)pdfPoint;
}

void StickyNoteTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Sticky notes are created on press, no release action needed
    (void)event;
    (void)pageIndex;
    (void)pdfPoint;
}

} // namespace pdfe::annotations
