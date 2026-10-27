#pragma once

#include "core/Command.h"
#include "TextEditModel.h"
#include "fonts/FontSubstitutionEngine.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdfe::core {
class Document;
class MuPdfContext;
}

namespace pdfe::textedit {

class ContentStreamEditor;

/// UI-TE-SAVE-08 phase-3 test seam: build a SubstitutionInfo whose
/// runSegments mirror the input FormattingRuns one-for-one, sorted by
/// charStart. Any run whose fontFilePath has no entry in pathToResName
/// (or whose path is empty) falls back to the span's original fontName
/// and fontSize so no text is silently dropped on emission.
///
/// Exposed so tests can verify the segment-build contract without
/// having to drive a full PDF save round-trip; the production caller
/// is EditTextCommand::applyText.
[[nodiscard]] pdfe::fonts::SubstitutionInfo
buildSubstitutionFromFormattingRuns(
    std::vector<FormattingRun> formattingRuns,
    const std::unordered_map<std::string, std::string>& pathToResName,
    const TextSpan& spanInfo);

/// Undo/redo command for editing text within a PDF page.
///
/// Stores the old and new text along with enough span metadata to
/// reapply edits in either direction via ContentStreamEditor.
class EditTextCommand : public pdfe::commands::Command {
public:
    EditTextCommand(
        pdfe::core::Document* document,
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        int pageIndex,
        int spanIndex,
        const TextSpan& spanInfo,
        const std::u32string& oldText,
        const std::u32string& newText
    );

    ~EditTextCommand() override;

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

    // Merging support: consecutive single-char edits on the same span merge
    [[nodiscard]] int mergeId() const override;
    bool mergeWith(const Command* other) override;

    [[nodiscard]] int pageIndex() const { return pageIndex_; }
    [[nodiscard]] int spanIndex() const { return spanIndex_; }

private:
    void applyText(const std::u32string& text);

    // Pipeline F phase-2: snapshot helpers for the empty-replacement
    // edge case. The edit pipeline (applyText -> ContentStreamEditor)
    // can only modify existing text runs — it cannot inject text into
    // a region the editor itself just emptied. So when newText is
    // empty, execute() captures the page's /Contents object and undo()
    // restores it directly via pdf_dict_put. void* holds an opaque
    // pdf_obj*; the dropContentsSnapshot helper is called from the
    // destructor.
    void captureContentsSnapshot();
    void restoreContentsSnapshot();
    void dropContentsSnapshot();

    pdfe::core::Document* document_;
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    int pageIndex_;
    int spanIndex_;
    TextSpan spanInfo_;       // Span metadata (bbox, font, etc.)
    std::u32string oldText_;
    std::u32string newText_;
    std::vector<FormattingRun> oldFormattingRuns_;  // Formatting state before edit
    std::vector<FormattingRun> newFormattingRuns_;  // Formatting state after edit
    bool executed_ = false;
    void* contentsSnapshot_ = nullptr;  // pdf_obj* pre-edit /Contents (kept-ref)
};

/// Undo/redo command for formatting changes (bold, italic, font family,
/// font size, underline, strikethrough, text color) on a TextEditModel
/// span.
///
/// UI-TE-UNDO-07: prior to D2 phase-3, TextEditTool::applyFormattingField
/// mutated `model_->applyFormatting(...)` directly without producing
/// any undo record. Pressing Ctrl+Z after a formatting change had no
/// effect. ApplyFormattingCommand wraps that mutation: applyFormattingField
/// snapshots the span's `formattingRuns` vector before and after the
/// model mutation, then MainWindow constructs this command and pushes
/// it onto the undo manager.
///
/// The command is constructed AFTER the model has already been mutated
/// (the existing apply path is preserved). execute() restores the
/// `newRuns_` snapshot as a no-op idempotent re-apply; undo()/redo()
/// swap between the snapshots.
class ApplyFormattingCommand : public pdfe::commands::Command {
public:
    ApplyFormattingCommand(
        TextEditModel* model,
        int spanIndex,
        std::vector<FormattingRun> oldRuns,
        std::vector<FormattingRun> newRuns,
        std::string description = "Apply formatting"
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;

    [[nodiscard]] int spanIndex() const { return spanIndex_; }

private:
    void applyRuns(const std::vector<FormattingRun>& runs);

    TextEditModel* model_;
    int spanIndex_;
    std::vector<FormattingRun> oldRuns_;
    std::vector<FormattingRun> newRuns_;
    std::string description_;
};

} // namespace pdfe::textedit
