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

    /// Handle mouse events (called by PdfCanvas).
    void mousePressEvent(QMouseEvent* event, QPointF pdfPos, float scale);
    void mouseMoveEvent(QMouseEvent* event, QPointF pdfPos, float scale);
    void mouseReleaseEvent(QMouseEvent* event, QPointF pdfPos, float scale);

    /// Handle key events.
    void keyPressEvent(QKeyEvent* event);

    /// Paint selection handles and overlays.
    void paint(QPainter& painter, float scale);

    /// Get cursor for current position.
    QCursor cursorAt(QPointF pdfPos, float scale) const;

    /// Check if an image is currently selected.
    bool hasSelection() const;

    /// Get the currently selected image info.
    const PageImageInfo& selectedImage() const;

    /// Deselect the current image.
    void deselect();

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
