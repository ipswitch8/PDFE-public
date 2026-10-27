#pragma once
#include "AnnotationTool.h"
#include "LinkAction.h"

#include <QString>

namespace pdfe::core {
class Page;
}

namespace pdfe::annotations {

class AnnotationManager;

/// Tool for creating /Link annotations (hyperlinks).
///
/// Drag to define a clickable rectangle. On release, prompts the user
/// for a URI (URL, mailto:, or internal #page=N reference).
/// Creates a PDF_ANNOT_LINK annotation with the URI stored via contents.
class LinkTool : public AnnotationTool {
public:
    explicit LinkTool(AnnotationManager* manager);

    AnnotationType toolType() const override { return AnnotationType::Link; }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;


    /// Set a default URI prompt text (e.g., "https://" or "#page=").
    void setDefaultUri(const QString& uri) { defaultUri_ = uri; }

private:
    LinkAction promptForAction() const;

    AnnotationManager* manager_ = nullptr;
    QString defaultUri_ = "https://";

    int currentPageIndex_ = -1;
    bool isDrawing_ = false;
    QPointF startPoint_;
    QPointF endPoint_;
};

} // namespace pdfe::annotations
