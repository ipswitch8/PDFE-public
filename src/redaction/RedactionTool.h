#pragma once
#include "annotations/AnnotationTool.h"
#include <QColor>

namespace pdfe::core {
class Page;
}

namespace pdfe::redaction {

class RedactionManager;

/// Tool for marking areas to be redacted.
///
/// Click and drag to draw a rectangle over content to redact.
/// The rectangle is drawn with a red dashed outline during drag.
/// On release, the redactionCreated callback fires with the page index and rect.
class RedactionTool : public pdfe::annotations::AnnotationTool {
public:
    explicit RedactionTool(RedactionManager* manager);

    pdfe::annotations::AnnotationType toolType() const override;

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;


    /// Callback invoked when a redaction area is selected.
    std::function<void(int pageIndex, QRectF pdfRect)> redactionCreated;

private:
    RedactionManager* manager_ = nullptr;
    QPointF startPoint_;
    QPointF endPoint_;
    int currentPageIndex_ = -1;
    bool isDrawing_ = false;
};

} // namespace pdfe::redaction
