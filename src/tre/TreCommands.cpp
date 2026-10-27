#include "tre/TreCommands.h"
#include "tre/RegionReconstructor.h"
#include "tre/ImageReplacer.h"
#include "tre/PdfContentWriter.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include <QDebug>

namespace pdfe::tre {

// ========== ReconstructRegionCommand Implementation ==========

ReconstructRegionCommand::ReconstructRegionCommand(
    pdfe::core::Document* document,
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    RegionReconstructor* reconstructor,
    ImageReplacer* imageReplacer,
    PdfContentWriter* contentWriter,
    int pageIndex,
    const PdfRect& selectionPdf,
    const std::string& editedText,
    RegionId regionId)
    : document_(document)
    , context_(std::move(context))
    , reconstructor_(reconstructor)
    , imageReplacer_(imageReplacer)
    , contentWriter_(contentWriter)
    , pageIndex_(pageIndex)
    , selectionPdf_(selectionPdf)
    , editedText_(editedText)
    , regionId_(regionId)
    , savedImageRef_(nullptr)
    , savedContentsRef_(nullptr)
    , executed_(false)
{
}

ReconstructRegionCommand::~ReconstructRegionCommand() {
    // R2-04/A4 holder design: the saved refs are standalone holder documents,
    // OWNED for the command's whole lifetime because undo()'s restore is now
    // NON-consuming (grafts a COPY into the live doc). Release whatever is
    // non-null regardless of executed_. The old raw-ref design nulled the refs
    // in undo() (dict ownership transfer) and released only when executed_ —
    // with holders that path would leak a standalone pdf_document.
    if (savedImageRef_) {
        contentWriter_->releaseImageXObjectRef(savedImageRef_);
        savedImageRef_ = nullptr;
    }
    if (savedContentsRef_) {
        contentWriter_->releaseContentsRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
    }
}

void ReconstructRegionCommand::execute() {
    auto page = document_->page(pageIndex_);
    if (!page) {
        qWarning() << "ReconstructRegionCommand: Failed to get page" << pageIndex_;
        return;
    }

    // redo() re-enters here (base Command::redo calls execute()); release any
    // prior holders before re-capturing so a re-execution never leaks a
    // standalone pdf_document (R2-04/A4 holder design).
    if (savedImageRef_) { contentWriter_->releaseImageXObjectRef(savedImageRef_); savedImageRef_ = nullptr; }
    if (savedContentsRef_) { contentWriter_->releaseContentsRef(savedContentsRef_); savedContentsRef_ = nullptr; }

    // Find image name before saving (needed for restore)
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), document_);
    if (imageInfo.has_value()) {
        savedImageName_ = imageInfo->xobjectName;
    }

    // Save state for undo
    savedImageRef_ = contentWriter_->saveImageXObjectRef(page.get(), document_);
    savedContentsRef_ = contentWriter_->saveContentsRef(page.get(), document_);

    // Try Approach B (hybrid reconstruction)
    auto result = reconstructor_->reconstruct(
        page.get(), document_, pageIndex_, selectionPdf_, editedText_, regionId_);

    if (!result.success) {
        qDebug() << "ReconstructRegionCommand: Approach B failed, falling back to Approach C";

        // Fall back to Approach C (image replacement)
        auto fallback = imageReplacer_->replace(
            page.get(), document_, pageIndex_, selectionPdf_, editedText_, regionId_);

        if (!fallback.success) {
            qWarning() << "TRE reconstruction failed:" << QString::fromStdString(fallback.errorMessage);

            // Restore on failure (non-consuming graft), then release the holders
            // — this command run failed, so it owns no undo state going forward.
            if (savedImageRef_) {
                contentWriter_->restoreImageXObjectRef(page.get(), document_, savedImageRef_, savedImageName_);
                contentWriter_->releaseImageXObjectRef(savedImageRef_);
                savedImageRef_ = nullptr;
            }
            if (savedContentsRef_) {
                contentWriter_->restoreContentsRef(page.get(), document_, savedContentsRef_);
                contentWriter_->releaseContentsRef(savedContentsRef_);
                savedContentsRef_ = nullptr;
            }
            return;
        }
    }

    executed_ = true;
}

void ReconstructRegionCommand::undo() {
    if (!executed_) {
        return;
    }

    auto page = document_->page(pageIndex_);
    if (!page) {
        qWarning() << "ReconstructRegionCommand::undo: Failed to get page" << pageIndex_;
        return;
    }

    // Restore saved state. NON-consuming (grafts a COPY into the live doc); the
    // holders stay owned by this command (released in the dtor / on re-execute),
    // so do NOT null them here — that old line existed only for the raw-ref
    // ownership transfer, which no longer happens (R2-04/A4 holder design).
    if (savedImageRef_) {
        contentWriter_->restoreImageXObjectRef(page.get(), document_, savedImageRef_, savedImageName_);
    }
    if (savedContentsRef_) {
        contentWriter_->restoreContentsRef(page.get(), document_, savedContentsRef_);
    }

    // Invalidate the original image cache so the next edit re-reads the
    // restored (pre-edit) XObject instead of using a stale cached version.
    if (imageReplacer_)
        imageReplacer_->invalidateOriginalCache(pageIndex_);

    executed_ = false;
}

std::string ReconstructRegionCommand::description() const {
    return "Reconstruct Scanned Text";
}

// ========== ImageReplaceCommand Implementation ==========

ImageReplaceCommand::ImageReplaceCommand(
    pdfe::core::Document* document,
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    ImageReplacer* imageReplacer,
    PdfContentWriter* contentWriter,
    int pageIndex,
    const PdfRect& selectionPdf,
    const std::string& editedText,
    RegionId regionId)
    : document_(document)
    , context_(std::move(context))
    , imageReplacer_(imageReplacer)
    , contentWriter_(contentWriter)
    , pageIndex_(pageIndex)
    , selectionPdf_(selectionPdf)
    , editedText_(editedText)
    , regionId_(regionId)
    , savedImageRef_(nullptr)
    , savedContentsRef_(nullptr)
    , executed_(false)
{
}

ImageReplaceCommand::~ImageReplaceCommand() {
    // R2-04/A4 holder design: holders are OWNED for the command's whole lifetime
    // (undo()'s restore is non-consuming), so release whatever is non-null
    // regardless of executed_. The old raw-ref design nulled the refs in undo()
    // and released only when executed_ — with holders that path would leak a
    // standalone pdf_document.
    if (savedImageRef_) {
        contentWriter_->releaseImageXObjectRef(savedImageRef_);
        savedImageRef_ = nullptr;
    }
    if (savedContentsRef_) {
        contentWriter_->releaseContentsRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
    }
}

void ImageReplaceCommand::execute() {
    auto page = document_->page(pageIndex_);
    if (!page) {
        qWarning() << "ImageReplaceCommand: Failed to get page" << pageIndex_;
        return;
    }

    // redo() re-enters here (base Command::redo calls execute()); release any
    // prior holders before re-capturing so a re-execution never leaks a
    // standalone pdf_document (R2-04/A4 holder design).
    if (savedImageRef_) { contentWriter_->releaseImageXObjectRef(savedImageRef_); savedImageRef_ = nullptr; }
    if (savedContentsRef_) { contentWriter_->releaseContentsRef(savedContentsRef_); savedContentsRef_ = nullptr; }

    // Find image name before saving (needed for restore)
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), document_);
    if (imageInfo.has_value()) {
        savedImageName_ = imageInfo->xobjectName;
    }

    // Save state for undo
    savedImageRef_ = contentWriter_->saveImageXObjectRef(page.get(), document_);
    savedContentsRef_ = contentWriter_->saveContentsRef(page.get(), document_);

    // Execute Approach C (image replacement only)
    auto result = imageReplacer_->replace(
        page.get(), document_, pageIndex_, selectionPdf_, editedText_, regionId_);

    if (!result.success) {
        qWarning() << "TRE image replacement failed:" << QString::fromStdString(result.errorMessage);

        // Restore on failure (non-consuming graft), then release the holders.
        if (savedImageRef_) {
            contentWriter_->restoreImageXObjectRef(page.get(), document_, savedImageRef_, savedImageName_);
            contentWriter_->releaseImageXObjectRef(savedImageRef_);
            savedImageRef_ = nullptr;
        }
        if (savedContentsRef_) {
            contentWriter_->restoreContentsRef(page.get(), document_, savedContentsRef_);
            contentWriter_->releaseContentsRef(savedContentsRef_);
            savedContentsRef_ = nullptr;
        }
        return;
    }

    executed_ = true;
}

void ImageReplaceCommand::undo() {
    if (!executed_) {
        return;
    }

    auto page = document_->page(pageIndex_);
    if (!page) {
        qWarning() << "ImageReplaceCommand::undo: Failed to get page" << pageIndex_;
        return;
    }

    // Restore saved state. NON-consuming (grafts a COPY into the live doc); the
    // holders stay owned by this command (released in the dtor / on re-execute),
    // so do NOT null them here (R2-04/A4 holder design).
    if (savedImageRef_) {
        contentWriter_->restoreImageXObjectRef(page.get(), document_, savedImageRef_, savedImageName_);
    }
    if (savedContentsRef_) {
        contentWriter_->restoreContentsRef(page.get(), document_, savedContentsRef_);
    }

    // Invalidate the original image cache so the next edit re-reads the
    // restored (pre-edit) XObject instead of using a stale cached version.
    if (imageReplacer_)
        imageReplacer_->invalidateOriginalCache(pageIndex_);

    executed_ = false;
}

std::string ImageReplaceCommand::description() const {
    return "Replace Scanned Text Image";
}

} // namespace pdfe::tre
