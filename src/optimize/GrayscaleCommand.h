#pragma once

#include "core/Command.h"

#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class Document;
class MuPdfContext;
}

namespace pdfe::optimize {

/// GitHub #355: undoable "Convert to Grayscale".
///
/// The Tools -> Convert to Grayscale action used to call
/// GrayscaleConverter::convertDocumentToGrayscale directly, so the recolor was
/// NOT undoable (unlike the adjacent Crop path, which routes through
/// UndoManager). This command wraps that conversion: execute() snapshots every
/// page's original /Contents and /Resources (teardown-safe holders) and then
/// recolors; undo() grafts the originals back, restoring the colour content.
class GrayscaleCommand : public pdfe::commands::Command {
public:
    explicit GrayscaleCommand(pdfe::core::Document* doc);
    ~GrayscaleCommand() override;

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

    // A document-wide recolor changes the appearance of EVERY page, so undo/redo
    // must refresh all Pages-panel thumbnails. affectsPageStructure() is the only
    // onUndo/onRedo hook that rebuilds the whole panel (the page SET is unchanged,
    // but there is no "all thumbnails stale" signal and per-page invalidation only
    // covers one page).
    [[nodiscard]] bool affectsPageStructure() const override { return true; }

    // Result of the most recent execute(), so the UI can report progress.
    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] int pagesConverted() const { return pagesConverted_; }
    [[nodiscard]] int pagesFailed() const { return pagesFailed_; }
    [[nodiscard]] const std::string& errorMessage() const { return errorMessage_; }

private:
    pdfe::core::Document* doc_;
    std::vector<void*> snapshot_;   ///< original /Contents+/Resources per page
    int pagesConverted_ = 0;
    int pagesFailed_ = 0;
    bool ok_ = false;
    std::string errorMessage_;
    bool executed_ = false;
};

} // namespace pdfe::optimize
