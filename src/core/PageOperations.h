#pragma once

#include <QRectF>
#include <filesystem>
#include <memory>
#include <vector>

namespace pdfe::core {

class MuPdfContext;
class Document;

/// Pipeline E phase-6 (Bucket 3d): read the current /CropBox of a
/// PDF page in PDF user-space (Y-up). Returns the page's /MediaBox
/// when no /CropBox is explicitly set (matches PDF spec default).
QRectF getPageCropBox(Document* doc, int pageIndex);

/// Pipeline E phase-6 (Bucket 3d): write a new /CropBox to a page.
/// `cropBox` is in PDF user-space (Y-up). Marks the document modified.
/// Use the CropPageCommand wrapper for undoable edits via UndoManager.
void setPageCropBox(Document* doc, int pageIndex, const QRectF& cropBox);

/// Extract specified pages from a document and save to a new PDF file.
///
/// This function creates a new PDF document containing only the specified pages
/// from the source document. Pages are grafted (deep copied) to ensure all
/// resources (fonts, images, etc.) are properly included.
///
/// @param context MuPDF context for resource management and error handling
/// @param sourceDoc Source document (must be a PDF)
/// @param pageIndices Sorted list of 0-based page indices to extract
/// @param outputPath Path for the new PDF file
///
/// @throws PdfeException if sourceDoc is not PDF, indices out of range, or save fails
///
/// @note This function requires friend access to Document::fzDocument() to get
///       the underlying pdf_document* pointer for grafting operations.
///
/// Example:
/// @code
///   auto doc = Document::open(context, "input.pdf");
///   std::vector<int> pages = {0, 2, 4}; // Extract pages 1, 3, 5
///   extractPagesToFile(context, doc.get(), pages, "output.pdf");
/// @endcode
void extractPagesToFile(
    std::shared_ptr<MuPdfContext> context,
    Document* sourceDoc,
    const std::vector<int>& pageIndices,
    const std::filesystem::path& outputPath
);

} // namespace pdfe::core
