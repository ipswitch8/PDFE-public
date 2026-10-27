#pragma once
#include "AnnotationTool.h"
#include <vector>

namespace pdfe::core {
class Page;
}

namespace pdfe::annotations {

class AnnotationManager;

/// Tool for creating ink (freehand drawing) annotations.
///
/// The user draws on the page by clicking and dragging. Each mouse press
/// starts a new stroke, and multiple strokes can be combined into a single
/// ink annotation.
class InkTool : public AnnotationTool {
public:
    /// Construct an ink tool.
    /// @param manager Annotation manager for creating annotations
    explicit InkTool(AnnotationManager* manager);

    AnnotationType toolType() const override { return AnnotationType::Ink; }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;

    /// Set the page context for creating annotations.
    /// This must be called before using the tool.

    /// Set the default color for new ink strokes.
    void setColor(QColor color) override { color_ = color; }

    /// Set the pen width for drawing (in PDF points).
    void setWidth(float width) { width_ = width; }

private:
    AnnotationManager* manager_ = nullptr;
    QColor color_{0, 0, 0}; // Default black
    float width_ = 2.0f;    // Default 2pt width

    int currentPageIndex_ = -1;
    bool isDrawing_ = false;
    std::vector<QPointF> currentStroke_;
    std::vector<std::vector<QPointF>> allStrokes_;
};

} // namespace pdfe::annotations
