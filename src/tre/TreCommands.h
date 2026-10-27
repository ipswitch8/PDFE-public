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
