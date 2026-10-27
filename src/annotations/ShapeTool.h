#pragma once
#include "AnnotationTool.h"

namespace pdfe::core {
class Page;
}

namespace pdfe::annotations {

class AnnotationManager;

/// Tool for creating shape annotations (square, circle, line).
///
/// The user clicks and drags to define a bounding rectangle, and the tool
/// creates a shape annotation of the specified type.
class ShapeTool : public AnnotationTool {
public:
    /// Construct a shape tool.
    /// @param manager Annotation manager for creating annotations
    /// @param type Type of shape (Square, Circle, or Line)
    /// @param color Default color for the shape
    ShapeTool(AnnotationManager* manager, AnnotationType type, QColor color);

    AnnotationType toolType() const override { return type_; }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;

    /// Set the page context for creating annotations.
    /// This must be called before using the tool.

    /// Set the default color for new shapes.
    void setColor(QColor color) override { color_ = color; }

    /// Set the default border width (in PDF points).
    void setBorderWidth(float width) { borderWidth_ = width; }

private:
    AnnotationManager* manager_ = nullptr;
    AnnotationType type_;
    QColor color_;
    float borderWidth_ = 1.0f;

    int currentPageIndex_ = -1;
    bool isDrawing_ = false;
    QPointF startPoint_;
    QPointF endPoint_;
};

} // namespace pdfe::annotations
