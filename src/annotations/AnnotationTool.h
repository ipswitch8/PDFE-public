#pragma once
#include "AnnotationData.h"
#include "AnnotationType.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QMenu>
#include <functional>
#include <memory>

namespace pdfe::core { class Page; }

namespace pdfe::annotations {

/// Base class for all annotation tools.
///
/// Annotation tools handle mouse events and paint overlays on the PDF canvas
/// to create annotations. Each tool type implements the specific behavior for
/// creating one type of annotation (e.g., sticky note, highlight, ink drawing).
///
/// Tools receive mouse events in PDF coordinates and paint overlays in screen
/// coordinates during the canvas's paint event.
class AnnotationTool {
public:
    virtual ~AnnotationTool() = default;

    /// Get the annotation type this tool creates.
    virtual AnnotationType toolType() const = 0;

    /// Called when the tool is activated (becomes the current tool).
    virtual void activate() {}

    /// Called when the tool is deactivated (another tool becomes active).
    virtual void deactivate() {}

    /// Bind the tool to the page it is to act on.
    ///
    /// GitHub #269. NON-VIRTUAL BY DESIGN, for the reason #236 gives about
    /// `page_` itself: this was declared separately in eleven tools, spelled
    /// identically in all eleven, and the consequence was not a missing
    /// feature but an UNREACHABLE one. `PdfCanvas` holds an `AnnotationTool*`,
    /// so with the setter only on the derived types it could not bind a page at
    /// all -- and the canvas is the only participant that knows both the page a
    /// click landed on and the document to resolve it from. The tool was
    /// therefore bound once, by MainWindow, at activation, to whatever page was
    /// current then, and a click on any other page acted on the wrong one.
    ///
    /// A derived class cannot skip or reinterpret the assignment now. Per-tool
    /// work goes in the two hooks below, which bracket it.
    void setPage(pdfe::core::Page* page) {
        onPageChangingImpl();
        page_ = page;
        onPageChangedImpl();
    }

    /// Called when the document is being closed/reloaded, BEFORE its pages are
    /// freed. A tool that caches a raw pdfe::core::Page* (e.g. SelectTool::page_)
    /// MUST drop it here — otherwise a repaint queued after the close derefs the
    /// freed page during onPaintOverlay (a use-after-free at paint time). Default
    /// no-op for tools that hold no page reference.
    /// GitHub #236. NON-VIRTUAL BY DESIGN -- do not make this virtual again.
    ///
    /// Releasing the cached page is no longer a tool's responsibility, because
    /// twelve tools had that responsibility and ten of them did not discharge
    /// it. Two crashes came out of that: #234 and #235, both fatal, both a
    /// repaint running against a page the document had already freed.
    ///
    /// A derived class cannot skip the release now -- there is nothing for it
    /// to override at this level. Tool-specific cleanup goes in
    /// onDocumentClosingImpl() below, which runs AFTER the pointer is gone, so
    /// even an implementation that ignores the contract cannot resurrect it.
    ///
    /// `page_` is nulled BEFORE the hook runs, deliberately: the hook is where
    /// a tool would be tempted to flush state through the page it is being
    /// told to stop using. FormTool's first draft of this did exactly that --
    /// its teardown fired a JS blur event through page_ -- which is the bug,
    /// not the cleanup.
    void onDocumentClosing() {
        page_ = nullptr;
        onDocumentClosingImpl();
    }

    /// Called when the annotation SET of a page has changed (one was deleted,
    /// added, or reordered) while the document itself stays open. A tool that
    /// caches annotation INDICES must drop them here.
    ///
    /// GitHub #234: deleting an annotation shifts every higher index down by
    /// one, so a cached index can silently start naming a different annotation
    /// or none at all. A paint handler that re-queries the model with a stale
    /// index gets `fz_throw(FZ_ERROR_ARGUMENT, "Annotation index out of range")`
    /// -> `MuPdfException` -> unwound out of `QWidget::paintEvent`, which Qt
    /// does not permit and the process terminates.
    ///
    /// This is the annotation-set sibling of onDocumentClosing(): that one
    /// covers "the page is going away", this one covers "the page is still here
    /// but its annotations are not what you last saw". Default no-op for tools
    /// that cache no indices.
    virtual void onAnnotationsChanged() {}

    /// Set the default color for annotations created by this tool.
    /// @param color The color to use
    virtual void setColor(QColor color) { (void)color; }

    /// Handle mouse press event.
    /// @param event The mouse event
    /// @param pageIndex Index of the page where the mouse event occurred
    /// @param pdfPoint Mouse position in PDF coordinates
    virtual void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) = 0;

    /// Handle mouse move event.
    /// @param event The mouse event
    /// @param pageIndex Index of the page where the mouse event occurred
    /// @param pdfPoint Mouse position in PDF coordinates
    virtual void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) = 0;

    /// Handle mouse release event.
    /// @param event The mouse event
    /// @param pageIndex Index of the page where the mouse event occurred
    /// @param pdfPoint Mouse position in PDF coordinates
    virtual void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) = 0;

    /// Handle key press event (for tools that need keyboard input).
    /// @param event The key event
    virtual void onKeyPress(QKeyEvent* event) { (void)event; }

    /// Handle mouse double-click event.
    virtual void onMouseDoubleClick(QMouseEvent* event, int pageIndex, QPointF pdfPoint) {
        (void)event; (void)pageIndex; (void)pdfPoint;
    }

    /// Handle context menu request (right-click).
    /// @return A QMenu to show, or nullptr. Caller owns the returned menu.
    virtual QMenu* onContextMenu(QMouseEvent* event, int pageIndex, QPointF pdfPoint) {
        (void)event; (void)pageIndex; (void)pdfPoint;
        return nullptr;
    }

    /// Test seam (c82a1f37 P4): returns the currently-selected object's
    /// resize/rotate handle centroids in PDF (fitz) coords as
    /// "Name:x,y;Name:x,y;..." or an empty string if nothing is selected.
    /// Overridden by SelectTool; the default (no handles) suits all other
    /// tools. Lets a UI test drag EXACTLY onto a handle via testCanvasDrag
    /// Pipe instead of guessing the tiny pixel target.
    virtual QString handleCentroidsDebug() const { return {}; }

    /// Test seam (c82a1f37 P4): deterministically select the annotation at
    /// (pageIndex, annotIndex) inside THIS tool's own selection state, so the
    /// resize/rotate handles become live without a synthetic canvas click
    /// (which SelectTool ignores, P-GAP-75). Overridden by SelectTool; a
    /// no-op for every other tool.
    virtual void selectByIndex(int pageIndex, int annotIndex) {
        (void)pageIndex; (void)annotIndex;
    }

    /// Paint overlay on top of the PDF page.
    /// Called during the canvas's paint event for each visible page.
    /// @param painter QPainter configured for screen coordinates
    /// @param pageIndex Index of the page being painted
    /// @param zoom Current zoom level
    /// @param pageOrigin Top-left corner of the page in screen coordinates
    /// @param pageHeightPt Height of the PDF page in points
    virtual void onPaintOverlay(QPainter& painter, int pageIndex,
                                float zoom, QPointF pageOrigin, float pageHeightPt) {}

    /// Callback invoked when an annotation is created by this tool.
    /// The UI should connect to this to add the annotation to the document.
    std::function<void(int pageIndex, AnnotationData)> annotationCreated;

protected:
    /// Tool-specific cleanup for a closing document. Runs AFTER `page_` has
    /// already been released, so it must not expect the page to be usable.
    /// Default is no-op: most tools have nothing to drop but the page itself,
    /// and now they do not even have to remember that.
    virtual void onDocumentClosingImpl() {}

    /// Runs BEFORE setPage() replaces `page_`, while the OLD page is still
    /// addressable. For state that must be flushed to the page being left --
    /// FormTool commits an in-progress field edit here. Default no-op.
    virtual void onPageChangingImpl() {}

    /// Runs AFTER setPage() has replaced `page_`. For state derived FROM the
    /// new page, and for dropping state that named things on the old one --
    /// SelectTool drops its selection here, FormTool rebuilds tab order.
    /// Default no-op.
    virtual void onPageChangedImpl() {}

    /// GitHub #236. The cached page, owned HERE rather than re-declared by each
    /// tool.
    ///
    /// It used to be a private member of twelve separate tools, identically
    /// spelled in all twelve, released in two. A tool that forgets is not
    /// visibly different from one that does not need to -- which is why this
    /// went unnoticed until it crashed twice.
    ///
    /// It is a NON-OWNING pointer into the document's page set: the document
    /// outlives no tool and frees these on close, so the only safe lifetime
    /// rule is "valid between setPage() and onDocumentClosing(), and at no
    /// other time". Do not store it anywhere that outlives the tool, and do
    /// not assume non-null means valid -- that assumption is exactly what
    /// #235 was (`if (!page_) return;` happily passes a dangling pointer).
    pdfe::core::Page* page_ = nullptr;
};

} // namespace pdfe::annotations
