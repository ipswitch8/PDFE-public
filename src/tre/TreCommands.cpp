#include "tre/TreCommands.h"
#include "tre/RegionReconstructor.h"
#include "tre/ImageReplacer.h"
#include "tre/PdfContentWriter.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "fonts/FontFamilyResolver.h"
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

        // Fall back to Approach C (image replacement).
        // fix-tre-writetarget phase-4: snapToLines=false so the edit modifies the
        // exact selected region rather than snapping to HOCR line boundaries and
        // erasing adjacent text — matching the working span-edit call site in
        // MainWindow::refreshImageForScannedPage. (The glyph-atlas / fontMatch /
        // enrichedAtlas args are built only on the overlay span-edit path and have
        // no region-flow equivalent, so they stay at their defaults here.)
        auto fallback = imageReplacer_->replace(
            page.get(), document_, pageIndex_, selectionPdf_, editedText_, regionId_,
            /*snapToLines=*/false);

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

// ========== InsertTextRunCommand Implementation ==========

InsertTextRunCommand::InsertTextRunCommand(
    pdfe::core::Document* document,
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    PdfContentWriter* contentWriter,
    int pageIndex,
    const PdfRect& insertionRectFitz,
    const std::string& utf8Text,
    const std::string& fontFamily,
    float fontSizePt,
    RgbColor color,
    bool bold,
    bool italic)
    : document_(document)
    , context_(std::move(context))
    , contentWriter_(contentWriter)
    , pageIndex_(pageIndex)
    , insertionRectFitz_(insertionRectFitz)
    , utf8Text_(utf8Text)
    , fontFamily_(fontFamily)
    , fontSizePt_(fontSizePt)
    , color_(color)
    , bold_(bold)
    , italic_(italic)
{
}

InsertTextRunCommand::~InsertTextRunCommand() {
    // Holder is OWNED for the command's whole lifetime (undo()'s restore is
    // non-consuming — it grafts a COPY into the live doc), mirroring the
    // Reconstruct/ImageReplace commands. Release whatever is non-null.
    if (savedContentsRef_) {
        contentWriter_->releaseContentsRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
    }
}

void InsertTextRunCommand::execute() {
    if (!document_ || !contentWriter_) return;

    auto page = document_->page(pageIndex_);
    if (!page) {
        qWarning() << "InsertTextRunCommand: Failed to get page" << pageIndex_;
        return;
    }

    // redo() re-enters here; release any prior holder before re-capturing so a
    // re-execution never leaks the standalone holder document.
    if (savedContentsRef_) {
        contentWriter_->releaseContentsRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
    }

    // Nothing to write (empty text / no page) is not a success.
    if (utf8Text_.empty()) return;

    // Resolve the font family to a concrete system font file. Without a path,
    // writeTextBlock cannot embed a font and writes nothing. The default
    // "Helvetica" is the base-14 sans but is NOT an installed Windows family
    // and has no FontFamilyResolver substitute entry, so fall back through its
    // metric-compatible equivalents (Arial and the free Linux/CI alternatives)
    // rather than silently writing nothing.
    pdfe::fonts::FontFamilyResolver resolver;
    std::string usedFamily = fontFamily_;
    std::string fontPath = resolver.resolve(fontFamily_, bold_, italic_);
    if (fontPath.empty()) {
        for (const char* fb : {"Arial", "Liberation Sans", "DejaVu Sans"}) {
            fontPath = resolver.resolve(fb, bold_, italic_);
            if (!fontPath.empty()) { usedFamily = fb; break; }
        }
    }
    if (fontPath.empty()) {
        qWarning() << "InsertTextRunCommand: could not resolve font family"
                   << QString::fromStdString(fontFamily_) << "or sans fallback";
        return;
    }

    // Snapshot the pre-insert /Contents for lossless undo (survives a
    // full-rewrite save — see PdfContentWriter::saveContentsRef).
    savedContentsRef_ = contentWriter_->saveContentsRef(page.get(), document_);

    FontMatch fontMatch;
    fontMatch.fontFilePath = fontPath;
    fontMatch.fontFamily = usedFamily;
    fontMatch.sizePt = fontSizePt_;
    fontMatch.color = color_;

    const std::string ops = contentWriter_->writeTextBlock(
        page.get(), document_, utf8Text_, fontMatch, insertionRectFitz_, {});

    if (ops.empty()) {
        qWarning() << "InsertTextRunCommand: writeTextBlock emitted nothing";
        // Restore (no-op graft of the untouched snapshot) and release — this
        // run produced no edit, so the command owns no undo state going forward.
        if (savedContentsRef_) {
            contentWriter_->restoreContentsRef(page.get(), document_, savedContentsRef_);
            contentWriter_->releaseContentsRef(savedContentsRef_);
            savedContentsRef_ = nullptr;
        }
        return;
    }

    // The new run is a freshly appended content-stream object. Force a full
    // rewrite so the incremental writer cannot drop it on save (bug-8 lesson:
    // ImageManager does the same for newly created objects).
    document_->markRequiresFullRewrite();
    document_->markPageDirty(pageIndex_);
    executed_ = true;
}

void InsertTextRunCommand::undo() {
    if (!executed_) return;

    auto page = document_->page(pageIndex_);
    if (!page) {
        qWarning() << "InsertTextRunCommand::undo: Failed to get page" << pageIndex_;
        return;
    }

    // Restore the pre-insert /Contents (non-consuming graft; the holder stays
    // owned by this command and is released in the dtor / on re-execute).
    if (savedContentsRef_) {
        contentWriter_->restoreContentsRef(page.get(), document_, savedContentsRef_);
    }
    document_->markRequiresFullRewrite();
    document_->markPageDirty(pageIndex_);
    executed_ = false;
}

std::string InsertTextRunCommand::description() const {
    return "Insert Text";
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

    // Execute Approach C (image replacement only).
    // fix-tre-writetarget phase-4: snapToLines=false — edit the exact selected
    // region, not the whole HOCR line (matches the working span-edit call site).
    // Atlas/fontMatch args are overlay-span-only and stay at their defaults.
    auto result = imageReplacer_->replace(
        page.get(), document_, pageIndex_, selectionPdf_, editedText_, regionId_,
        /*snapToLines=*/false);

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
