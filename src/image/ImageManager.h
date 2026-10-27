#pragma once

#include "image/ImageData.h"
#include <QImage>
#include <memory>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
}

namespace pdfe::image {

/// Manages image operations on PDF pages.
///
/// Provides enumeration, insertion, removal, extraction, and transform
/// operations for images embedded in PDF pages. Uses MuPDF's structured
/// text API for image discovery and content stream filtering for modifications.
class ImageManager {
public:
    explicit ImageManager(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~ImageManager();

    // Non-copyable
    ImageManager(const ImageManager&) = delete;
    ImageManager& operator=(const ImageManager&) = delete;

    /// Enumerate all images on a page.
    std::vector<PageImageInfo> enumerateImages(
        const pdfe::core::Page* page, const pdfe::core::Document* doc);

    /// Get info about a specific image by index.
    PageImageInfo getImageInfo(
        const pdfe::core::Page* page, const pdfe::core::Document* doc, int imageIndex);

    /// Count images on a page.
    int imageCount(const pdfe::core::Page* page, const pdfe::core::Document* doc);

    /// Insert an image from a file onto a page.
    ImageOperationResult insertImage(
        const pdfe::core::Page* page, const pdfe::core::Document* doc,
        const InsertImageOptions& opts);

    /// Remove an image from a page by index.
    ImageOperationResult removeImage(
        const pdfe::core::Page* page, const pdfe::core::Document* doc, int imageIndex);

    /// Restore a previously removed image (for undo).
    ImageOperationResult restoreImage(
        const pdfe::core::Page* page, const pdfe::core::Document* doc,
        const PageImageInfo& savedInfo,
        void* savedImageRef, void* savedContentsRef);

    /// Extract an image to a file.
    bool extractImageToFile(
        const pdfe::core::Page* page, const pdfe::core::Document* doc,
        int imageIndex, const std::string& outputPath);

    /// Extract an image as a QImage.
    QImage extractImage(
        const pdfe::core::Page* page, const pdfe::core::Document* doc, int imageIndex);

    // --- Undo support (opaque void* wrapping pdf_obj*) ---

    /// Save a reference to an image XObject for later undo.
    void* saveImageRef(
        const pdfe::core::Page* page, const pdfe::core::Document* doc,
        const std::string& xobjectName);

    /// Save a reference to the page's content stream(s) for later undo.
    void* saveContentsRef(
        const pdfe::core::Page* page, const pdfe::core::Document* doc);

    /// Release a saved reference without restoring.
    void releaseRef(void* ref);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::image
