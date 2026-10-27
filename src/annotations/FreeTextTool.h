#pragma once
#include "AnnotationTool.h"

namespace pdfe::core {
class Page;
}

namespace pdfe::annotations {

class AnnotationManager;

/// Tool for creating free text (text box) annotations.
///
/// Click on a page to place a text box at that location. The text box
/// has a default size and can be resized and edited after creation.
class FreeTextTool : public AnnotationTool {
public:
    /// Construct a free text tool.
    /// @param manager Annotation manager for creating annotations
    explicit FreeTextTool(AnnotationManager* manager);

    AnnotationType toolType() const override { return AnnotationType::FreeText; }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    /// Set the page context for creating annotations.
    /// This must be called before using the tool.

    /// Set the default color for new text boxes.
    void setColor(QColor color) override { color_ = color; }

    /// Set the default size for new text boxes (in PDF points).
    void setDefaultSize(float width, float height) {
        defaultWidth_ = width;
        defaultHeight_ = height;
    }

    /// Set the default font size for new text boxes (in points).
    void setFontSize(float size) { fontSize_ = size; }

private:
    AnnotationManager* manager_ = nullptr;
    QColor color_{0, 0, 0}; // Default black text
    float defaultWidth_ = 150.0f;
    float defaultHeight_ = 50.0f;
    float fontSize_ = 12.0f;
};

} // namespace pdfe::annotations
