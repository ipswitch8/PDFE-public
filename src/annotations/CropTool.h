#pragma once
#include "AnnotationTool.h"

#include <QPointF>
#include <QRectF>
#include <functional>

namespace pdfe::annotations {

/// Pipeline E phase-6 (Bucket 3d): rubber-band drag tool that produces
/// a new /CropBox for the active page.
///
/// Unlike the other tools in this directory the CropTool does not
/// create an annotation; it fires a `cropApplied` callback with the
/// final rectangle in PDF user-space coordinates. MainWindow turns
/// the callback into an undoable CropPageCommand. Inheriting
/// AnnotationTool lets the existing PdfCanvas tool dispatch route
/// mouse events through the same path as the annotation tools, with
/// no canvas-side changes required.
class CropTool : public AnnotationTool {
public:
    CropTool() = default;

    AnnotationType toolType() const override {
        return AnnotationType::Unknown;
    }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;

    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;

    /// Fired on mouse release with the final crop rectangle in PDF
    /// coordinates. The pageIndex passed to MainWindow identifies
    /// which page to apply the crop to.
    std::function<void(int pageIndex, QRectF cropRect)> cropApplied;

    /// Active page index (set by MainWindow when activating the tool).
    /// Mouse events on other pages are ignored.
    void setActivePage(int idx) { activePage_ = idx; }

    /// Minimum drag size (in points) — drags smaller than this are
    /// dropped to avoid accidental zero-area crops.
    static constexpr float MIN_CROP_PT = 4.0f;

private:
    int activePage_ = -1;
    bool dragging_ = false;
    QPointF startPdf_;
    QPointF endPdf_;
};

} // namespace pdfe::annotations
