#pragma once
#include "AnnotationTool.h"

namespace pdfe::core {
class Page;
}

namespace pdfe::annotations {

class AnnotationManager;

/// Tool for creating sticky note (text) annotations.
///
/// Click on a page to place a sticky note icon at that location.
/// The note's contents are empty initially and can be edited via the
/// comment panel or properties dialog.
class StickyNoteTool : public AnnotationTool {
public:
    /// Construct a sticky note tool.
    /// @param manager Annotation manager for creating annotations
    explicit StickyNoteTool(AnnotationManager* manager);

    AnnotationType toolType() const override { return AnnotationType::Text; }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    /// Set the page context for creating annotations.
    /// This must be called before using the tool.

    /// Set the default color for new sticky notes.
    void setColor(QColor color) override { color_ = color; }

    /// Set the default icon name for sticky notes.
    void setIconName(const std::string& name) { iconName_ = name; }

private:
    AnnotationManager* manager_ = nullptr;
    QColor color_{255, 255, 0}; // Default yellow
    std::string iconName_ = "Comment"; // Default icon

    // Sticky note icon size in PDF points
    static constexpr float ICON_SIZE = 24.0f;
};

} // namespace pdfe::annotations
