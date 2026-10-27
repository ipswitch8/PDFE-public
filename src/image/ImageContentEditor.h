#pragma once

#include <QRectF>
#include <memory>
#include <string>

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
}

namespace pdfe::image {

class ImageManager;

/// Manipulates content stream CTM for image transforms (resize, rotate, crop).
///
/// Uses a remove-and-re-append pattern: the old image placement is removed
/// via pdf_filter_page_contents with image_filter, then a new placement
/// with the modified CTM is appended to the content stream.
///
/// NOTE: Requires friend access to Page::fzPage() and Document::fzDocument().
/// Add the following to Page.h and Document.h:
///   friend class pdfe::image::ImageContentEditor;
class ImageContentEditor {
public:
    explicit ImageContentEditor(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~ImageContentEditor();

    // Non-copyable
    ImageContentEditor(const ImageContentEditor&) = delete;
    ImageContentEditor& operator=(const ImageContentEditor&) = delete;

    /// Resize an image to new display dimensions (points).
    /// Removes old placement and appends new with scaled CTM.
    /// @param page The page containing the image
    /// @param doc The document containing the page
    /// @param xobjectName The XObject name (e.g., "Im1")
    /// @param newWidth New width in points
    /// @param newHeight New height in points
    /// @param x X position in points
    /// @param y Y position in points
    /// @return true on success, false on error
    bool resizeImage(pdfe::core::Page* page, pdfe::core::Document* doc,
                     const std::string& xobjectName,
                     float newWidth, float newHeight,
                     float x, float y);

    /// Rotate an image by the specified angle (degrees, clockwise).
    /// Removes old placement and appends new with rotated CTM.
    /// @param page The page containing the image
    /// @param doc The document containing the page
    /// @param xobjectName The XObject name (e.g., "Im1")
    /// @param angleDegrees Rotation angle in degrees (clockwise)
    /// @param centerX Center of rotation X coordinate in points
    /// @param centerY Center of rotation Y coordinate in points
    /// @param displayWidth Current display width in points
    /// @param displayHeight Current display height in points
    /// @return true on success, false on error
    bool rotateImage(pdfe::core::Page* page, pdfe::core::Document* doc,
                     const std::string& xobjectName,
                     float angleDegrees,
                     float centerX, float centerY,
                     float displayWidth, float displayHeight);

    /// Crop an image using a clip rectangle in PDF coordinates.
    /// Removes old placement and appends new with clip path + CTM.
    /// @param page The page containing the image
    /// @param doc The document containing the page
    /// @param xobjectName The XObject name (e.g., "Im1")
    /// @param cropRect Crop rectangle in PDF coordinates
    /// @param displayWidth Display width in points (after crop)
    /// @param displayHeight Display height in points (after crop)
    /// @param x X position in points
    /// @param y Y position in points
    /// @return true on success, false on error
    bool cropImage(pdfe::core::Page* page, pdfe::core::Document* doc,
                   const std::string& xobjectName,
                   QRectF cropRect,
                   float displayWidth, float displayHeight,
                   float x, float y);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::image
