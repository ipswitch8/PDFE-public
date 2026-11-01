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
