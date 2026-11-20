#pragma once
#include "core/Command.h"
#include "tre/TreTypes.h"
#include <memory>
#include <string>

namespace pdfe::core {
class Document;
class MuPdfContext;
}

namespace pdfe::tre {

class RegionReconstructor;
class ImageReplacer;
class PdfContentWriter;
struct ReconstructionResult;
struct PageImageInfo;

/// Command for Approach B (hybrid reconstruction): real PDF text over cleaned image.
/// Falls back to Approach C via ImageReplacer if quality is too low.
class ReconstructRegionCommand : public pdfe::commands::Command {
public:
    ReconstructRegionCommand(
        pdfe::core::Document* document,
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        RegionReconstructor* reconstructor,
        ImageReplacer* imageReplacer,
        PdfContentWriter* contentWriter,
        int pageIndex,
        const PdfRect& selectionPdf,
        const std::string& editedText,
        RegionId regionId);

    ~ReconstructRegionCommand() override;

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

    /// Whether the last execute() actually landed an edit (Approach B or the
    /// Approach C fallback succeeded). The caller (MainWindow::onTreRegionSelected)
    /// reads this AFTER handing the command to the undo manager so it can refresh
    /// the page / invalidate caches on success and surface a visible failure
    /// otherwise, instead of silently assuming the edit worked (fix-tre-writetarget
    /// phase-4).
    [[nodiscard]] bool succeeded() const { return executed_; }

private:
    pdfe::core::Document* document_;
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    RegionReconstructor* reconstructor_;
    ImageReplacer* imageReplacer_;
    PdfContentWriter* contentWriter_;
    int pageIndex_;
    PdfRect selectionPdf_;
    std::string editedText_;
    RegionId regionId_;

    // Undo state (opaque pdf_obj* references)
    void* savedImageRef_ = nullptr;
    void* savedContentsRef_ = nullptr;
    std::string savedImageName_;
    bool executed_ = false;
};

/// Bug 9 (phase-11): write a BRAND-NEW text run into empty page space.
///
/// The Edit Text tool's find-and-replace editor (EditTextCommand →
/// ContentStreamEditor) can only MODIFY an existing text run matched by bbox;
/// it has no anchor to inject text where the page has none. When the user
/// clicks a blank line / margin and types, that text is a new run and must be
/// WRITTEN to the content stream. This command does exactly that via
/// PdfContentWriter::writeTextBlock (the same content-write path Region
/// Reconstructor uses), with lossless undo through the page /Contents
/// snapshot (saveContentsRef / restoreContentsRef).
///
/// The font family (e.g. "Helvetica") is resolved to a concrete system font
/// file internally via FontFamilyResolver, so callers need only pass the name
/// TextEditTool chose. Coordinates are fitz-space (top-left origin, Y-down) —
/// the same space as TextSpan::boundingBox and PdfContentWriter::writeTextBlock.
class InsertTextRunCommand : public pdfe::commands::Command {
public:
    InsertTextRunCommand(
        pdfe::core::Document* document,
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        PdfContentWriter* contentWriter,
        int pageIndex,
        const PdfRect& insertionRectFitz,
        const std::string& utf8Text,
        const std::string& fontFamily,
        float fontSizePt,
        RgbColor color,
        bool bold = false,
        bool italic = false);

    ~InsertTextRunCommand() override;

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

    /// Whether the last execute() actually wrote the run (a font resolved and
    /// writeTextBlock emitted operators). The caller reads this to refresh the
    /// page on success and surface a failure otherwise.
    [[nodiscard]] bool succeeded() const { return executed_; }

private:
    pdfe::core::Document* document_;
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    PdfContentWriter* contentWriter_;
    int pageIndex_;
    PdfRect insertionRectFitz_;
    std::string utf8Text_;
    std::string fontFamily_;
    float fontSizePt_;
    RgbColor color_;
    bool bold_;
    bool italic_;

    // Undo state (opaque pdf_obj* holder of the pre-insert page /Contents).
    void* savedContentsRef_ = nullptr;
    bool executed_ = false;
};

/// Command for Approach C (image replacement): bitmap text composited over background.
class ImageReplaceCommand : public pdfe::commands::Command {
public:
    ImageReplaceCommand(
        pdfe::core::Document* document,
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        ImageReplacer* imageReplacer,
        PdfContentWriter* contentWriter,
        int pageIndex,
        const PdfRect& selectionPdf,
        const std::string& editedText,
        RegionId regionId);

    ~ImageReplaceCommand() override;

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

    /// Whether the last execute() actually landed the image replacement. See
    /// ReconstructRegionCommand::succeeded() (fix-tre-writetarget phase-4).
    [[nodiscard]] bool succeeded() const { return executed_; }

private:
    pdfe::core::Document* document_;
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    ImageReplacer* imageReplacer_;
    PdfContentWriter* contentWriter_;
    int pageIndex_;
    PdfRect selectionPdf_;
    std::string editedText_;
    RegionId regionId_;

    // Undo state
    void* savedImageRef_ = nullptr;
    void* savedContentsRef_ = nullptr;
    std::string savedImageName_;
    bool executed_ = false;
};

} // namespace pdfe::tre
