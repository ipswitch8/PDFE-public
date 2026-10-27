#include "FreeTextTool.h"
#include "AnnotationManager.h"
#include "../core/Page.h"

namespace pdfe::annotations {

FreeTextTool::FreeTextTool(AnnotationManager* manager)
    : manager_(manager)
{
}

void FreeTextTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Only respond to left button
    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (!manager_ || !page_) {
        return;
    }

    // Create text box annotation at click point
    // The rect is centered on the click point with default size
    QRectF rect(
        pdfPoint.x() - defaultWidth_ / 2.0f,
        pdfPoint.y() - defaultHeight_ / 2.0f,
        defaultWidth_,
        defaultHeight_
    );

    AnnotationData data = manager_->createAnnotation(page_, AnnotationType::FreeText, rect);
    data.color = color_;
    data.contents = "Text";
    data.pageIndex = pageIndex;

    // Set default appearance FIRST (MuPDF needs DA before contents for rendering).
    // DA controls text color; /C controls background fill. Use black text on white bg.
    QColor textColor(0, 0, 0);  // Always black text
    QColor bgColor(255, 255, 255);  // White background
    manager_->setDefaultAppearance(page_, data.annotIndex, "Helv", fontSize_, textColor);
    manager_->setColor(page_, data.annotIndex, bgColor);
    manager_->setContents(page_, data.annotIndex, "Text");

    // Notify that annotation was created
    if (annotationCreated) {
        auto cb = annotationCreated;  // copy: the call may destroy this tool
        cb(pageIndex, data);
    }
}

void FreeTextTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Free text boxes are created on press, no move action needed
    (void)event;
    (void)pageIndex;
    (void)pdfPoint;
}

void FreeTextTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    // Free text boxes are created on press, no release action needed
    (void)event;
    (void)pageIndex;
    (void)pdfPoint;
}

} // namespace pdfe::annotations
