#pragma once

#include "image/ImageData.h"
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QCursor>
#include <vector>
#include <memory>

class QMouseEvent;
class QKeyEvent;
class QPainter;

namespace pdfe::core {
class Page;
class Document;
}

namespace pdfe::image {

class ImageManager;

/// Handle positions for image resize/rotate handles.
enum class HandlePosition {
    None,
    TopLeft, TopCenter, TopRight,
    MiddleLeft, MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
    Rotation  // Handle above top center for rotation
};

/// Canvas tool for selecting and manipulating images on PDF pages.
class ImageTool : public QObject {
    Q_OBJECT
public:
    explicit ImageTool(ImageManager* manager, QObject* parent = nullptr);
    ~ImageTool() override;

    /// Set the page and document context.
    void setPage(pdfe::core::Page* page, pdfe::core::Document* doc);

    /// Clear the current page context.
    void clearPage();

    /// Handle mouse events (called by PdfCanvas). @p pdfPos is in fitz device
    /// space — Y-down, origin at the page's TOP-left, points — the space
    /// CoordinateTransform::screenToPdf produces and the enumerated
    /// PageImageInfo::boundingBox lives in (GitHub #373). Drag deltas, bbox
    /// arithmetic and the committed resize/rotate signals all stay in this
    /// one space; the content-stream writers convert to Y-up user space.
    void mousePressEvent(QMouseEvent* event, QPointF pdfPos, float scale);
    void mouseMoveEvent(QMouseEvent* event, QPointF pdfPos, float scale);
    void mouseReleaseEvent(QMouseEvent* event, QPointF pdfPos, float scale);

    /// Handle key events.
    void keyPressEvent(QKeyEvent* event);

    /// Paint selection handles and overlays.
    ///
    /// CONTRACT (GitHub #373): @p painter must already be transformed into
    /// fitz page space — translate(pageTopLeft) then scale(zoom, zoom) — the
    /// way PdfCanvas sets it up before calling (mirroring what the annotation
    /// tools do internally). @p scale is that same zoom, used only to keep
    /// pixel-sized furniture (pen widths, handle sizes, text) zoom-invariant.
    void paint(QPainter& painter, float scale);

    /// Get cursor for current position.
    QCursor cursorAt(QPointF pdfPos, float scale) const;

    /// Check if an image is currently selected.
    bool hasSelection() const;

    /// Get the currently selected image info.
    const PageImageInfo& selectedImage() const;

    /// Programmatically select the enumerated image whose XObject key equals
    /// @p xobjectName, WITHOUT a mouse click (GitHub #329).
    ///
    /// Before this existed, `selectedIndex_` only became valid through the
    /// hit-test in mousePressEvent, so a freshly inserted image had no
    /// selection, drew no handles, and left the image-toolbar actions disabled
    /// until the user separately clicked it. This is the entry point
    /// MainWindow::onInsertImage() calls so the just-inserted image is live
    /// immediately. On a match it sets the selection, emits `imageSelected`
    /// (which paints the handles and enables the toolbar actions) and requests
    /// a repaint. Returns true if a matching image was found and selected;
    /// false (selection unchanged) if the name is empty or not present among
    /// the currently enumerated images.
    bool selectImage(const std::string& xobjectName);

    /// Select the last enumerated image on the page, or false if the page has
    /// none. Enumeration order follows the page content stream, so the last
    /// entry is the most-recently drawn image — the one a fresh append places
    /// on top. Prefer selectImage(name) when the XObject identity is known;
    /// this is the fallback for callers that only know "the one just added".
    bool selectLast();

    /// Deselect the current image.
    void deselect();

    /// TEST SEAM (GitHub #342). Run the REAL selected-image handle hit-test and
    /// return the handle hit at @p pdfPos (page-space), or HandlePosition::None.
    /// This is the exact routine mousePressEvent/cursorAt use (including the
    /// inverse-rotation about the image centre), exposed so a test can assert a
    /// handle is grabbable at a VISUALLY-ROTATED corner. Returns None when there
    /// is no selection.
    HandlePosition hitTestHandleForTest(QPointF pdfPos, float scale) const;

    /// TEST SEAM (GitHub #342). Replace the enumerated-image list with @p images
    /// and select index @p selectIndex (clamped; -1 clears selection), WITHOUT
    /// touching the page/document. Lets a test drive the tool against a known
    /// placement — e.g. a rotated PageImageInfo whose rotationRadians is set —
    /// so the rotated hit-test can be exercised deterministically without a full
    /// rotate/save/re-enumerate round trip. Does not emit signals.
    void setImagesForTest(std::vector<PageImageInfo> images, int selectIndex);

signals:
    /// Emitted when an image is selected.
    void imageSelected(const pdfe::image::PageImageInfo& info);

    /// Emitted when selection is cleared.
    void imageDeselected();

    /// Emitted when resize is requested via drag handles.
    void resizeRequested(const std::string& xobjectName, float newWidth, float newHeight, float x, float y);

    /// Emitted when rotation is requested.
    void rotateRequested(const std::string& xobjectName, float angleDegrees);

    /// Emitted when removal is requested (Delete key or context menu).
    void removeRequested(int imageIndex);

    /// Emitted when crop is requested.
    void cropRequested(const std::string& xobjectName, QRectF cropRect);

    /// Emitted to request a repaint of the canvas.
    void repaintRequested();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::image
