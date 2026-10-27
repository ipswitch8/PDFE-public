#pragma once

#include <QRectF>
#include <QPointF>
#include <QString>
#include <vector>
#include <memory>

namespace pdfe::core {
class Page;
class MuPdfContext;
}

namespace pdfe::annotations {

/// Result of a text selection operation.
struct TextSelection {
    std::vector<QRectF> quads;   ///< Bounding rect per line of selected text (PDF coords)
    QString selectedText;         ///< The actual text content
    int pageIndex = -1;           ///< Index of the page where selection occurred
};

/// Allows users to select text on a PDF page by clicking and dragging.
///
/// The selection result provides quad points (bounding rectangles for each line
/// of selected text) that are used by text markup annotation tools (Highlight,
/// Underline, StrikeOut).
///
/// Usage:
///   1. Call beginSelection() with a starting point on the page
///   2. Call updateSelection() as the user drags to extend the selection
///   3. Call endSelection() to finalize and retrieve the TextSelection result
///   4. Use currentQuads() during dragging to paint a selection preview
class TextSelector {
public:
    /// Construct a TextSelector with the given MuPDF context.
    explicit TextSelector(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    ~TextSelector();

    // Non-copyable
    TextSelector(const TextSelector&) = delete;
    TextSelector& operator=(const TextSelector&) = delete;

    // Movable
    TextSelector(TextSelector&&) noexcept;
    TextSelector& operator=(TextSelector&&) noexcept;

    /// Begin text selection on a page at the given PDF coordinate.
    /// Extracts text blocks from the page and stores the starting point.
    /// @param page The PDF page to select text from
    /// @param pdfPoint Starting point in PDF coordinates
    void beginSelection(pdfe::core::Page* page, QPointF pdfPoint);

    /// Update selection to a new point (extend selection).
    /// Recomputes which characters fall within the selection sweep.
    /// @param pdfPoint Current point in PDF coordinates
    void updateSelection(QPointF pdfPoint);

    /// End selection and return the result.
    /// Finalizes the selection and returns quads + selected text.
    /// @return TextSelection containing quads and text, or empty if nothing selected
    TextSelection endSelection();

    /// Get the current selection quads (for painting preview).
    /// Returns the quads of currently selected text as updateSelection() is called.
    /// @return Vector of bounding rectangles in PDF coordinates
    std::vector<QRectF> currentQuads() const;

    /// Check if a selection is in progress.
    /// @return true if beginSelection() was called and endSelection() has not been called yet
    bool isSelecting() const;

    /// Cancel the current selection without returning a result.
    /// Resets internal state as if no selection was started.
    void cancel();

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::annotations
