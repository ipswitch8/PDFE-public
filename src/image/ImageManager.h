#pragma once

#include "image/ImageData.h"
#include <QImage>
#include <memory>
#include <string>
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

    /// Enumerate every image placement on a page: one entry per `Do`, in
    /// content order, including Do's inside Form XObjects (GitHub #427).
    /// Geometry is reported in fitz device space of the RENDERED page - Y-down,
    /// origin at the rendered page's TOP-left, /Rotate and box origin applied
    /// (GitHub #373/#426); boundingBox is the visible (cropped) region. See
    /// PageImageInfo and docs/image-placement-model.md.
    std::vector<PageImageInfo> enumerateImages(
        const pdfe::core::Page* page, const pdfe::core::Document* doc);

    /// Get info about a specific image by index.
    PageImageInfo getImageInfo(
        const pdfe::core::Page* page, const pdfe::core::Document* doc, int imageIndex);

    /// Count images on a page.
    int imageCount(const pdfe::core::Page* page, const pdfe::core::Document* doc);

    /// Insert an image from a file onto a page. opts.targetRect is fitz
    /// Y-down (y = distance of the image's visual top edge from the page
    /// top); the writer converts once to the Y-up `cm` matrix (GitHub #373).
    ImageOperationResult insertImage(
        const pdfe::core::Page* page, const pdfe::core::Document* doc,
        const InsertImageOptions& opts);

    /// Remove the image placement at @p imageIndex (enumeration order).
    ImageOperationResult removeImage(
        const pdfe::core::Page* page, const pdfe::core::Document* doc, int imageIndex);

    /// Remove ONE placement: the n-th `Do` of @p xobjectName in the page
    /// content (PageImageInfo::occurrence; GitHub #421). The other placements
    /// are left as they were; the XObject resource is removed with the last
    /// placement of that name. removeImage(index) does this for the indexed
    /// placement. A placement inside a Form XObject is not removable.
    ImageOperationResult removePlacement(
        const pdfe::core::Page* page, const pdfe::core::Document* doc,
        const std::string& xobjectName, int occurrence);

    /// Remove @p xobjectName from the page's XObject resources if nothing on
    /// the page draws it any more (no page-level Do, and no Do inside a form
    /// that inherits the page's resources). Used by InsertImageCommand::undo
    /// once the content is restored (GitHub #434). Returns true if removed.
    bool removeImageResourceIfUnused(
        const pdfe::core::Page* page, const pdfe::core::Document* doc,
        const std::string& xobjectName);

    /// Restore a previously removed or edited state (for undo): the page's
    /// /Contents and its ExtGState / Properties resources from a
    /// saveContentsRef snapshot, and the XObject from saveImageRef.
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

    /// Snapshot the page's content stream(s), plus its ExtGState and
    /// Properties resource dicts, for later undo (restoreImage).
    void* saveContentsRef(
        const pdfe::core::Page* page, const pdfe::core::Document* doc);

    /// Release a saved reference without restoring.
    void releaseRef(void* ref);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::image
