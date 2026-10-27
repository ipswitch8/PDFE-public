#pragma once
#include "annotations/AnnotationTool.h"
#include <functional>

namespace pdfe::tre {

/// Tool for selecting a rectangular region on a scanned page for TRE editing.
/// Follows the ShapeTool rubber-band drag pattern.
class TreRegionTool : public pdfe::annotations::AnnotationTool {
public:
    TreRegionTool();

    pdfe::annotations::AnnotationType toolType() const override;

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;

    /// Callback fired when user finishes drawing a region (page index + PDF rect).
    std::function<void(int pageIndex, QRectF pdfRect)> regionSelected;

private:
    bool isDrawing_ = false;
    int currentPageIndex_ = -1;
    QPointF startPoint_;
    QPointF endPoint_;
};

} // namespace pdfe::tre
