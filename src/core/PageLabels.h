#pragma once

#include "Command.h"

#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {

class Document;

/// PDF /PageLabels entry — Pipeline E phase-6 (Bucket 3c).
///
/// /PageLabels lives in /Catalog and maps page-index ranges to a
/// label specification (style, optional prefix, optional starting
/// number). When present, viewers display the labelled string (for
/// example "ii", "iii", "iv", "1", "2", "3", ..., "A-1", "A-2") in
/// place of the raw page index.
///
/// Style names follow PDF 1.7 §12.4.2 Table 159:
///   /D — decimal arabic
///   /R — uppercase Roman
///   /r — lowercase Roman
///   /A — uppercase letters (A, B, ..., Z, AA, BB, ...)
///   /a — lowercase letters
///   absent — no number, just the prefix
enum class PageLabelStyle {
    None,         // no /S entry
    Decimal,      // /S /D
    RomanUpper,   // /S /R
    RomanLower,   // /S /r
    AlphaUpper,   // /S /A
    AlphaLower    // /S /a
};

struct PageLabelRange {
    int firstPage = 0;          // 0-based PDF page index where range starts
    PageLabelStyle style = PageLabelStyle::Decimal;
    std::string prefix;          // optional /P
    int startValue = 1;          // /St (default 1)
};

/// Read/write the document's /PageLabels number tree.
///
/// All operations execute under the document's MuPDF context lock
/// internally; the caller does not need to manage MuPDF state.
class PageLabelManager {
public:
    /// Read all label ranges in ascending firstPage order. Returns an
    /// empty vector if the document has no /PageLabels entry.
    static std::vector<PageLabelRange> read(Document* doc);

    /// Replace the document's /PageLabels with `ranges`. If `ranges`
    /// is empty, removes the /PageLabels entry entirely. Marks the
    /// document modified.
    ///
    /// Ranges are sorted by firstPage before writing; out-of-range
    /// firstPage values (< 0 or >= page count) are clamped.
    static void write(Document* doc, std::vector<PageLabelRange> ranges);

    /// Compute the displayed label string for `pageIndex` given
    /// `ranges`. Pure function — exposed for testing without needing
    /// a Document.
    static std::string labelFor(int pageIndex,
                                const std::vector<PageLabelRange>& ranges);

    /// Convert PageLabelStyle to its PDF name character ('D', 'R',
    /// 'r', 'A', 'a'), or '\0' for None.
    static char styleToPdfChar(PageLabelStyle style);

    /// Convert a PDF style name character back to PageLabelStyle.
    /// Returns PageLabelStyle::None for '\0' or unrecognized chars.
    static PageLabelStyle pdfCharToStyle(char c);

    /// Render an arabic number `n` (≥ 1) in the chosen style.
    /// Exposed for testing.
    static std::string formatNumber(int n, PageLabelStyle style);
};

/// Undoable page-label edit (autosave-coverage Phase-2, PageLabels gap).
///
/// PageLabelManager::write() is a direct catalog mutation that pushes no undo
/// command, so before this the editor's changes were neither undoable nor
/// captured by autosave (the undo stack stayed clean → no autosave trigger).
/// Routing the editor through this command dirties the undo stack (autosave
/// fires), makes the edit undoable, and lets the close-prompt see the change.
///
/// The pre-edit ranges are captured lazily on the first execute() so undo
/// restores exactly what was in the document before the edit.
class EditPageLabelsCommand : public pdfe::commands::Command {
public:
    EditPageLabelsCommand(Document* doc, std::vector<PageLabelRange> newRanges);

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override {
        return "Edit Page Labels";
    }

private:
    Document* doc_ = nullptr;
    std::vector<PageLabelRange> oldRanges_;
    std::vector<PageLabelRange> newRanges_;
    bool captured_ = false;  ///< oldRanges_ captured on first execute()
};

} // namespace pdfe::core
