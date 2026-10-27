#pragma once

#include "AnnotationTool.h"
#include "AnnotationManager.h"
#include "../core/Page.h"
#include <QColor>
#include <QMenu>
#include <QRectF>
#include <functional>
#include <optional>
#include <vector>

namespace pdfe::annotations {

/// Tool for selecting, moving, resizing, rotating, and rubber-band multi-selecting annotations.
class SelectTool : public AnnotationTool {
public:
    explicit SelectTool(AnnotationManager* manager);

    AnnotationType toolType() const override { return AnnotationType::Unknown; }

    // GitHub #269. Was SelectTool::setPage's body. The selection names
    // annotations BY INDEX on a particular page, so it cannot survive the page
    // changing -- #234's shape exactly. Runs after AnnotationTool::setPage has
    // replaced page_.
    void onPageChangedImpl() override {
        selectedAnnotIndex_ = -1;
        selectedAnnotIndices_.clear();
        rotationDegrees_ = 0.0;
    }

    // Drop the SELECTION when the document closes/reloads, so a repaint queued
    // after the pages are freed cannot re-query them -- the paint-time
    // use-after-free (onPaintOverlay -> getAnnotation -> Page::fzPage on freed
    // memory). After this the paint guards (selectedAnnotIndices_.empty() /
    // selectedAnnotIndex_ < 0) short-circuit.
    //
    // GitHub #236: `page_` is NOT nulled here any more. The base class releases
    // it before calling this hook, so no tool can forget -- which ten of twelve
    // had.
    void onDocumentClosingImpl() override {
        selectedAnnotIndex_ = -1;
        selectedAnnotIndices_.clear();
        rotationDegrees_ = 0.0;
    }

    // GitHub #234. Same contract as onDocumentClosing above, for the case the
    // document STAYS open and only its annotation set changed. page_ is still
    // valid here and is deliberately kept -- only the cached indices are
    // dropped, because a delete shifts every higher index down by one and the
    // tool has no way to re-map them.
    //
    // SelectTool's own Delete/Backspace path already clears the selection
    // before deleting (and deletes in reverse index order). This hook is for
    // deletions that originate ELSEWHERE -- the comment panel, a menu action,
    // an undo/redo -- which the tool never hears about.
    void onAnnotationsChanged() override {
        selectedAnnotIndex_ = -1;
        selectedAnnotIndices_.clear();
        rotationDegrees_ = 0.0;
    }

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onKeyPress(QKeyEvent* event) override;
    void onMouseDoubleClick(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    QMenu* onContextMenu(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;

    /// Callback when an annotation is selected (pageIndex, annotIndex)
    std::function<void(int, int)> annotationSelected;
    /// Callback when selection is cleared
    std::function<void()> selectionCleared;
    /// Callback when an annotation is moved or resized (pageIndex, annotIndex, oldRect, newRect)
    std::function<void(int, int, QRectF, QRectF)> annotationMoved;
    /// Callback when an annotation is rotated (pageIndex, annotIndex, degrees)
    std::function<void(int, int, double)> annotationRotated;
    /// Callback when an annotation is deleted (pageIndex, annotIndex)
    std::function<void(int, int)> annotationDeleted;
    /// Callback when an annotation is pasted (pageIndex, annotationData)
    std::function<void(int, AnnotationData)> annotationPasted;
    /// Callback when a sticky note is double-clicked (pageIndex, annotIndex, annotationData)
    std::function<void(int, int, AnnotationData)> stickyNoteDoubleClicked;
    /// Callback when a Text/FreeText annotation is single-clicked for preview (read-only popup)
    std::function<void(int, int, AnnotationData)> annotationPreviewRequested;
    /// Callback when rubber-band multi-selection completes (pageIndex, annotIndices)
    std::function<void(int, std::vector<int>)> multipleAnnotationsSelected;
    /// Callback when the user requests a reply to a comment-style
    /// annotation (Text / FreeText). MainWindow opens the comment
    /// panel in reply mode and creates an /IRT-linked child
    /// annotation via AnnotationManager::setInReplyTo (UI-CTX-03).
    std::function<void(int, int)> replyRequested;
    /// R3-12: fired whenever a selection produces a RECT in PDF user-space —
    /// a single annotation selected (its rect), a rubber-band that resolves to
    /// annotations (their union bounding box), or an empty rubber-band marquee
    /// (the band rect, i.e. a page-region snapshot). MainWindow wires this to
    /// the copy-selection so Copy renders JUST this region as an image instead
    /// of the whole page. `selectionCleared` still fires on deselect so the
    /// copy-selection is dropped back to the whole-page fallback.
    std::function<void(int pageIndex, QRectF pdfRect)> regionSelected;

    int selectedAnnotIndex() const { return selectedAnnotIndex_; }
    const std::vector<int>& selectedAnnotIndices() const { return selectedAnnotIndices_; }
    double rotationDegrees() const { return rotationDegrees_; }

    /// Test seam (c82a1f37 P4): serialize this tool's handle centroids in
    /// PDF coords using the SAME geometry as hitTestHandle(). Empty when
    /// nothing is selected.
    QString handleCentroidsDebug() const override;

    /// Test seam (c82a1f37 P4): populate this tool's selection state
    /// (index + rect + page) from the model, exactly as a real click-select
    /// would, so handle hit-testing/dragging works deterministically.
    void selectByIndex(int pageIndex, int annotIndex) override;

private:
    enum class Handle {
        None,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
        TopCenter,
        RightCenter,
        BottomCenter,
        LeftCenter,
        Rotate
    };

    Handle hitTestHandle(QPointF pdfPoint) const;
    QRectF computeResizedRect(Handle handle, QPointF currentPdf) const;
    double computeRotation(QPointF currentPdf) const;

    AnnotationManager* manager_ = nullptr;
    int currentPageIndex_ = -1;
    int selectedAnnotIndex_ = -1;
    std::vector<int> selectedAnnotIndices_;
    QRectF selectedAnnotRect_;
    bool isDragging_ = false;
    QPointF dragStartPdf_;
    QRectF dragStartRect_;
    Handle activeHandle_ = Handle::None;
    float lastZoom_ = 1.0f;
    std::optional<AnnotationData> clipboard_;

    bool isRubberBanding_ = false;
    QPointF rubberBandStart_;
    QPointF rubberBandEnd_;

    double rotationDegrees_ = 0.0;
    double dragStartRotation_ = 0.0;
};

} // namespace pdfe::annotations
