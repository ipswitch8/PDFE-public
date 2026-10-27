#include "StampTool.h"
#include "AnnotationManager.h"
#include "../core/Page.h"
#include "../core/CoordinateTransform.h"
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QDate>
#include <algorithm>

namespace pdfe::annotations {

StampTool::StampTool(AnnotationManager* manager)
    : manager_(manager)
{
}

QString StampTool::builtInTemplate(DynamicType type)
{
    switch (type) {
        case DynamicType::Draft:    return QStringLiteral("DRAFT - {date}");
        case DynamicType::Approved: return QStringLiteral("APPROVED by {author} on {date}");
        case DynamicType::Reviewed: return QStringLiteral("REVIEWED by {author} on {date}");
    }
    return {};
}

QString StampTool::resolveTokens(const QString& raw,
                                 const QString& author,
                                 const QString& isoDate)
{
    QString out = raw;
    out.replace(QStringLiteral("{date}"),   isoDate);
    out.replace(QStringLiteral("{author}"), author);
    return out;
}

void StampTool::setImageStamp(const QString& name, const QImage& image)
{
    stampImage_ = image;
    if (!image.isNull() && !name.isEmpty()) {
        stampName_ = name;
    }
}

void StampTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton) return;
    if (!page_) return;
    startPoint_ = pdfPoint;
    endPoint_ = pdfPoint;
    currentPageIndex_ = pageIndex;
    isDrawing_ = true;
}

void StampTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    (void)event;
    if (!isDrawing_ || pageIndex != currentPageIndex_) return;
    endPoint_ = pdfPoint;
}

void StampTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !isDrawing_) return;
    if (!manager_ || !page_) {
        isDrawing_ = false;
        return;
    }

    endPoint_ = pdfPoint;
    isDrawing_ = false;

    qreal left = std::min(startPoint_.x(), endPoint_.x());
    qreal top = std::min(startPoint_.y(), endPoint_.y());
    qreal right = std::max(startPoint_.x(), endPoint_.x());
    qreal bottom = std::max(startPoint_.y(), endPoint_.y());
    QRectF rect(left, top, right - left, bottom - top);

    // Single click defaults to a stamp size: 150x50 pt for text stamps,
    // or a sized box that preserves the image's aspect ratio for image
    // stamps (default 150 pt wide).
    if (rect.width() < 10.0 || rect.height() < 10.0) {
        if (!stampImage_.isNull() && stampImage_.width() > 0) {
            const qreal targetW = 150.0;
            const qreal aspect = static_cast<qreal>(stampImage_.height())
                / static_cast<qreal>(stampImage_.width());
            const qreal targetH = targetW * aspect;
            rect = QRectF(startPoint_.x(), startPoint_.y() - targetH,
                          targetW, targetH);
        } else {
            rect = QRectF(startPoint_.x(), startPoint_.y() - 50, 150, 50);
        }
    }

    if (!stampImage_.isNull()) {
        AnnotationData data = manager_->createAnnotation(
            page_, AnnotationType::Stamp, rect);
        data.pageIndex = currentPageIndex_;
        data.color = color_;
        // Record the stamp's library name in /Contents so a later
        // session can identify which library entry was used.
        data.contents = stampName_.toStdString();
        manager_->setContents(page_, data.annotIndex, data.contents);
        manager_->setStampImageAppearance(page_, data.annotIndex, stampImage_);

        if (annotationCreated) {
            auto cb = annotationCreated;  // copy: the call may destroy this tool
            cb(currentPageIndex_, data);
        }
        return;
    }

    // Phase 4f: resolve {date} and {author} tokens into the final content
    // string. The resolved text is what gets baked into the annotation so
    // re-opening the file shows the resolved value (not the token).
    const QString isoDate = QDate::currentDate().toString(Qt::ISODate);
    const QString resolved = resolveTokens(stampName_, author_, isoDate);

    AnnotationData data = manager_->createAnnotation(page_, AnnotationType::Stamp, rect);
    data.pageIndex = currentPageIndex_;
    data.color = color_;
    data.contents = resolved.toStdString();

    manager_->setColor(page_, data.annotIndex, color_);
    manager_->setContents(page_, data.annotIndex, data.contents);

    if (annotationCreated) {
        auto cb = annotationCreated;  // copy: the call may destroy this tool
        cb(currentPageIndex_, data);
    }
}

void StampTool::onPaintOverlay(QPainter& painter, int pageIndex,
                                float zoom, QPointF pageOrigin, float pageHeightPt)
{
    if (!isDrawing_ || pageIndex != currentPageIndex_) return;

    painter.save();

    QPen pen(color_);
    pen.setWidthF(2.0f * zoom);
    pen.setStyle(Qt::SolidLine);
    painter.setPen(pen);
    painter.setBrush(QBrush(color_, Qt::BDiagPattern));

    QPointF screenStart = pdfe::core::CoordinateTransform::pdfToScreen(
        startPoint_, zoom, pageOrigin, pageHeightPt);
    QPointF screenEnd = pdfe::core::CoordinateTransform::pdfToScreen(
        endPoint_, zoom, pageOrigin, pageHeightPt);

    qreal left = std::min(screenStart.x(), screenEnd.x());
    qreal top = std::min(screenStart.y(), screenEnd.y());
    qreal right = std::max(screenStart.x(), screenEnd.x());
    qreal bottom = std::max(screenStart.y(), screenEnd.y());

    QRectF box(QPointF(left, top), QPointF(right, bottom));
    painter.drawRect(box);

    if (!stampImage_.isNull()) {
        // Image stamp preview — draw the imported image inside the box,
        // semi-transparent to indicate this is a drag preview.
        painter.setBrush(Qt::NoBrush);
        painter.setOpacity(0.6);
        painter.drawImage(box, stampImage_);
        painter.setOpacity(1.0);
        painter.restore();
        return;
    }

    // Draw the stamp text preview (tokens resolved live so the user sees
    // the final result while dragging).
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSizeF(std::max(8.0, box.height() * 0.4));
    painter.setFont(font);
    painter.setBrush(Qt::NoBrush);
    const QString isoDate = QDate::currentDate().toString(Qt::ISODate);
    const QString preview = resolveTokens(stampName_, author_, isoDate);
    painter.drawText(box, Qt::AlignCenter, preview);

    painter.restore();
}

} // namespace pdfe::annotations
