#include "FileAttachmentTool.h"
#include "AnnotationManager.h"
#include "../core/Page.h"

#include <QMouseEvent>
#include <stdexcept>

namespace pdfe::annotations {

FileAttachmentTool::FileAttachmentTool(AnnotationManager* manager)
    : manager_(manager)
{
}

void FileAttachmentTool::onMousePress(QMouseEvent* event, int pageIndex,
                                      QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton) return;
    if (!manager_ || !page_) return;
    if (sourceFile_.empty()) return;

    // /FileAttachment renders as a fixed icon; the rect is a small
    // square centered on the click point, like a sticky note.
    QRectF rect(
        pdfPoint.x() - ICON_SIZE / 2.0f,
        pdfPoint.y() - ICON_SIZE / 2.0f,
        ICON_SIZE,
        ICON_SIZE
    );

    AnnotationData data;
    try {
        data = manager_->createFileAttachmentAnnotation(
            page_, rect, sourceFile_, iconName_, displayName_);
    } catch (const std::exception&) {
        // Source file unreadable; bail silently — caller is expected
        // to validate before activating the tool.
        return;
    }

    data.pageIndex = pageIndex;
    data.color = color_;
    if (manager_) {
        // The annotation color affects the icon tint in some
        // viewers (Acrobat in particular).
        manager_->setColor(page_, data.annotIndex, color_);
    }

    if (annotationCreated) {
        auto cb = annotationCreated;  // copy: the call may destroy this tool
        cb(pageIndex, data);
    }
}

void FileAttachmentTool::onMouseMove(QMouseEvent* event, int pageIndex,
                                     QPointF pdfPoint)
{
    (void)event; (void)pageIndex; (void)pdfPoint;
}

void FileAttachmentTool::onMouseRelease(QMouseEvent* event, int pageIndex,
                                        QPointF pdfPoint)
{
    (void)event; (void)pageIndex; (void)pdfPoint;
}

} // namespace pdfe::annotations
