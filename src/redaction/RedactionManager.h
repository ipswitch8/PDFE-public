#pragma once

#include "RedactionData.h"
#include <memory>
#include <vector>
#include <optional>
#include <QRectF>

// Forward declarations for MuPDF C types (must be at global scope, with C linkage)
extern "C" {
struct pdf_page;
struct pdf_annot;
}

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
}

namespace pdfe::redaction {

/// Manager for creating, configuring, and applying PDF redaction annotations.
///
/// Two-phase workflow:
/// 1. Mark: Create redaction annotations (undoable via commands)
/// 2. Apply: Permanently remove content under marks (irreversible)
class RedactionManager {
public:
    explicit RedactionManager(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    ~RedactionManager();

    // Non-copyable
    RedactionManager(const RedactionManager&) = delete;
    RedactionManager& operator=(const RedactionManager&) = delete;

    // Movable
    RedactionManager(RedactionManager&&) noexcept;
    RedactionManager& operator=(RedactionManager&&) noexcept;

    // --- Mark phase (undoable via commands) ---

    /// Create a new redaction mark on the specified page.
    /// @return Data for the created redaction annotation
    RedactionData createRedactionMark(
        const pdfe::core::Page* page,
        QRectF rect
    );

    /// Delete a redaction mark from a page.
    /// @return The deleted mark's data (for undo)
    RedactionData deleteRedactionMark(
        const pdfe::core::Page* page,
        int annotIndex
    );

    /// Restore a previously deleted redaction mark.
    /// @return The annotation index of the restored mark
    int restoreRedactionMark(
        const pdfe::core::Page* page,
        const RedactionData& data
    );

    /// Set the fill color of a redaction mark.
    void setFillColor(
        const pdfe::core::Page* page,
        int annotIndex,
        QColor color
    );

    /// Set the overlay text of a redaction mark.
    void setOverlayText(
        const pdfe::core::Page* page,
        int annotIndex,
        const std::string& text
    );

    /// Set the bounding rectangle of a redaction mark.
    void setRect(
        const pdfe::core::Page* page,
        int annotIndex,
        QRectF rect
    );

    // --- Query ---

    /// Get all redaction marks on a page.
    std::vector<RedactionData> getRedactionMarks(const pdfe::core::Page* page);

    /// Get a specific redaction mark by annotation index.
    RedactionData getRedactionMark(const pdfe::core::Page* page, int annotIndex);

    /// Get all redaction marks across all pages.
    std::vector<RedactionData> getAllRedactionMarks(pdfe::core::Document* document);

    /// Get count of pending redaction marks across all pages.
    int pendingMarkCount(pdfe::core::Document* document);

    /// Find redaction mark at the specified point (hit test).
    std::optional<int> getRedactionMarkAt(
        const pdfe::core::Page* page,
        QPointF pdfPoint
    );

    // --- Apply phase (irreversible) ---

    /// Apply all redaction marks on a specific page.
    /// Permanently removes content — NOT undoable.
    bool applyPageRedactions(
        const pdfe::core::Page* page,
        const RedactionApplyOptions& options = {}
    );

    /// Apply all redaction marks across all pages.
    /// Permanently removes content — NOT undoable.
    RedactionApplyResult applyAllRedactions(
        pdfe::core::Document* document,
        const RedactionApplyOptions& options = {}
    );

    /// Flatten (bake) every annotation into the page content streams and
    /// remove the annotation objects. Irreversible.
    ///
    /// This is a DOCUMENT-wide MuPDF operation (pdf_bake_document). It is NOT
    /// the same thing as applying redactions: the previous implementation of
    /// "Flatten Annotations" called applyPageRedactions() with no redaction
    /// marks, which is a no-op — the annotations survived untouched (PDF-53).
    /// @return true if the document was baked.
    bool flattenAnnotations(pdfe::core::Document* document);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::redaction
