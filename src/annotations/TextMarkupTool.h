#pragma once
#include "AnnotationTool.h"
#include "TextSelector.h"
#include <memory>

namespace pdfe::core {
class Page;
}

namespace pdfe::annotations {

class AnnotationManager;

/// Tool for creating text markup annotations (highlight, underline, strikeout).
///
/// The user selects text by clicking and dragging, and the tool creates a
/// text markup annotation with quad points matching the selected text.
class TextMarkupTool : public AnnotationTool {
public:
    /// Construct a text markup tool.
    /// @param manager Annotation manager for creating annotations
    /// @param selector Text selector for selecting text on pages
    /// @param type Type of markup (Highlight, Underline, or StrikeOut)
    /// @param color Default color for the markup
    TextMarkupTool(AnnotationManager* manager,
                   std::shared_ptr<TextSelector> selector,
                   AnnotationType type,
                   QColor color);

    AnnotationType toolType() const override { return type_; }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;

    /// Set the page context for creating annotations.
    /// This must be called before using the tool.

    /// Set the default color for new markups.
    void setColor(QColor color) override { color_ = color; }

    /// Current in-progress selection page index (-1 when idle). Exposed for
    /// tests that verify onMouseRelease resets its state BEFORE firing the
    /// annotationCreated callback (which may delete this tool) — see #48.
    int currentPageIndex() const { return currentPageIndex_; }

private:
    AnnotationManager* manager_ = nullptr;
    std::shared_ptr<TextSelector> selector_;
    AnnotationType type_;
    QColor color_;

    int currentPageIndex_ = -1;
    bool isSelecting_ = false;
};

} // namespace pdfe::annotations
