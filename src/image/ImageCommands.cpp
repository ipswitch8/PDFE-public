#include "image/ImageCommands.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "core/Document.h"
#include "core/Page.h"

#include <exception>  // std::exception (GitHub #392 defensive boundary)
#include <string>

namespace pdfe::image {

// -----------------------------------------------------------------------
// InsertImageCommand
// -----------------------------------------------------------------------

InsertImageCommand::InsertImageCommand(
    ImageManager* manager,
    pdfe::core::Page* page,
    pdfe::core::Document* doc,
    const InsertImageOptions& options)
    : manager_(manager), page_(page), doc_(doc), options_(options)
{
}

InsertImageCommand::~InsertImageCommand()
{
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
    }
}

void InsertImageCommand::execute()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op (covers redo()).
    // redo() re-enters here; release any prior holder before re-capturing so a
    // re-execution never leaks a standalone pdf_document (R2-01 holder design).
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
    }
    // GitHub #392 — defensive exception boundary. insertImage() already catches
    // MuPDF faults internally and returns success=false, but saveContentsRef()
    // also goes through MuPDF and can throw a MuPdfException/std::exception. A
    // throw here would unwind uncaught through UndoManager::execute() and the Qt
    // slot -> std::terminate. Catch it and leave the command in a safe state
    // (result_.success=false, savedContentsRef_ possibly null). The UndoManager
    // still pushes the command, which is acceptable: undo() is guarded by
    // executed_ and savedContentsRef_, so an un-executed / ref-less command is a
    // safe no-op.
    try {
        // Save current content stream for undo
        savedContentsRef_ = manager_->saveContentsRef(page_, doc_);

        result_ = manager_->insertImage(page_, doc_, options_);
        doc_->markPageDirty(page_->index());
        executed_ = true;
    } catch (const std::exception& e) {
        result_.success = false;
        result_.errorMessage =
            std::string("Failed to insert image: ") + e.what();
    }
}

void InsertImageCommand::undo()
{
    if (!executed_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op. See SaveWithRebind.h step 5.

    // GitHub #434: undo takes away exactly what execute() added - the
    // inserted placement and its XObject resource - and nothing else.
    // Restoring the pre-insert content snapshot removes the placement (and
    // restores every other byte exactly); the resource the insert added under
    // the name it reported (result_.imageInfo) then has no user left and is
    // removed. The old code called removeImage(imageIndex) AFTER the restore,
    // with an index that was never filled in (-1) and a placement that was
    // already gone, so the XObject stayed in the page resources. redo()
    // re-runs execute(), which inserts it again.
    if (savedContentsRef_) {
        manager_->restoreImage(page_, doc_, result_.imageInfo, nullptr, savedContentsRef_);
        if (result_.success) {
            manager_->removeImageResourceIfUnused(page_, doc_, result_.imageInfo.xobjectName);
        }
    }

    doc_->markPageDirty(page_->index());
    executed_ = false;
}

std::string InsertImageCommand::description() const
{
    return "Insert Image";
}

// -----------------------------------------------------------------------
// RemoveImageCommand
// -----------------------------------------------------------------------

RemoveImageCommand::RemoveImageCommand(
    ImageManager* manager,
    pdfe::core::Page* page,
    pdfe::core::Document* doc,
    int imageIndex)
    : manager_(manager), page_(page), doc_(doc), imageIndex_(imageIndex)
{
}

RemoveImageCommand::~RemoveImageCommand()
{
    // Holders are OWNED for the command's whole lifetime (restoreImage is now
    // non-consuming), so release whatever is non-null regardless of executed_.
    // The old raw-ref design nulled the refs in undo() (dict ownership transfer)
    // and skipped release when ending undone; the holder design has no such
    // transfer, so that path would leak a standalone pdf_document.
    if (savedImageRef_) manager_->releaseRef(savedImageRef_);
    if (savedContentsRef_) manager_->releaseRef(savedContentsRef_);
}

void RemoveImageCommand::execute()
{
    // Release any prior holders before re-capturing (defensive; execute() is the
    // first run, but keep the invariant airtight against re-entry).
    if (savedImageRef_) { manager_->releaseRef(savedImageRef_); savedImageRef_ = nullptr; }
    if (savedContentsRef_) { manager_->releaseRef(savedContentsRef_); savedContentsRef_ = nullptr; }

    // Save refs for undo
    auto info = manager_->getImageInfo(page_, doc_, imageIndex_);
    savedInfo_ = info;
    savedImageRef_ = manager_->saveImageRef(page_, doc_, info.xobjectName);
    savedContentsRef_ = manager_->saveContentsRef(page_, doc_);

    const ImageOperationResult r = manager_->removeImage(page_, doc_, imageIndex_);
    if (!r.success) {
        // Nothing was removed (bad index, or an image inside a Form XObject):
        // no undo entry, page left clean (image-placement-state phase 2).
        if (savedImageRef_) { manager_->releaseRef(savedImageRef_); savedImageRef_ = nullptr; }
        if (savedContentsRef_) { manager_->releaseRef(savedContentsRef_); savedContentsRef_ = nullptr; }
        failed_ = true;
        executed_ = false;
        return;
    }
    failed_ = false;
    doc_->markPageDirty(page_->index());
    executed_ = true;
}

void RemoveImageCommand::undo()
{
    if (!executed_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op.

    // Non-consuming restore: grafts COPIES of the held holders into the live doc.
    // The holders stay owned by this command (released in the dtor), so undo must
    // NOT null them — that old line existed only for the raw-ref ownership
    // transfer, which no longer happens.
    manager_->restoreImage(page_, doc_, savedInfo_, savedImageRef_, savedContentsRef_);
    doc_->markPageDirty(page_->index());
    executed_ = false;
}

void RemoveImageCommand::redo()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    // Re-save refs and remove again — release the prior holders first so redo
    // never leaks a standalone pdf_document.
    if (savedImageRef_) { manager_->releaseRef(savedImageRef_); savedImageRef_ = nullptr; }
    if (savedContentsRef_) { manager_->releaseRef(savedContentsRef_); savedContentsRef_ = nullptr; }
    savedImageRef_ = manager_->saveImageRef(page_, doc_, savedInfo_.xobjectName);
    savedContentsRef_ = manager_->saveContentsRef(page_, doc_);

    manager_->removeImage(page_, doc_, imageIndex_);
    doc_->markPageDirty(page_->index());
    executed_ = true;
}

std::string RemoveImageCommand::description() const
{
    return "Remove Image";
}

// -----------------------------------------------------------------------
// ResizeImageCommand
// -----------------------------------------------------------------------

ResizeImageCommand::ResizeImageCommand(
    ImageManager* manager,
    ImageContentEditor* editor,
    pdfe::core::Page* page,
    pdfe::core::Document* doc,
    const std::string& xobjectName,
    float newWidth, float newHeight,
    float x, float y,
    float angleDegrees,
    int occurrence)
    : manager_(manager), editor_(editor), page_(page), doc_(doc),
      xobjectName_(xobjectName), newWidth_(newWidth), newHeight_(newHeight),
      x_(x), y_(y), angleDegrees_(angleDegrees), occurrence_(occurrence)
{
}

ResizeImageCommand::~ResizeImageCommand()
{
    // Release whenever non-null: the holder is owned for the command's whole
    // lifetime (undo's restore is non-consuming), so the old `executed_ &&`
    // guard would leak a standalone pdf_document when the command ends undone.
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
    }
}

void ResizeImageCommand::execute()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op (covers redo()).
    // redo() re-enters here; release any prior holder before re-capturing so a
    // re-execution never leaks a standalone pdf_document (R2-01 holder design).
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
    }
    savedContentsRef_ = manager_->saveContentsRef(page_, doc_);
    const bool ok = editor_->resizeImage(page_, doc_, PlacementId{xobjectName_, occurrence_},
                         newWidth_, newHeight_, x_, y_, angleDegrees_);
    if (!ok) {
        // The editor refused (placement not found, inside a Form XObject,
        // empty crop, ...): nothing changed. Record the failure so the
        // UndoManager stacks no entry, leave the page clean, and keep undo a
        // no-op (image-placement-state phase 2).
        manager_->releaseRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
        failed_ = true;
        executed_ = false;
        return;
    }
    failed_ = false;
    doc_->markPageDirty(page_->index());
    executed_ = true;
}

void ResizeImageCommand::undo()
{
    if (!executed_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    if (savedContentsRef_) {
        // Non-consuming restore (grafts a COPY into the live doc). The holder
        // stays owned by this command and is released in the dtor, so do NOT
        // null it here — that old line was for the raw-ref ownership transfer.
        manager_->restoreImage(page_, doc_, {}, nullptr, savedContentsRef_);
        doc_->markPageDirty(page_->index());
    }
    executed_ = false;
}

std::string ResizeImageCommand::description() const
{
    return "Resize Image";
}

// -----------------------------------------------------------------------
// RotateImageCommand
// -----------------------------------------------------------------------

RotateImageCommand::RotateImageCommand(
    ImageManager* manager,
    ImageContentEditor* editor,
    pdfe::core::Page* page,
    pdfe::core::Document* doc,
    const std::string& xobjectName,
    float angleDegrees,
    float centerX, float centerY,
    float displayWidth, float displayHeight,
    int occurrence)
    : manager_(manager), editor_(editor), page_(page), doc_(doc),
      xobjectName_(xobjectName), angleDegrees_(angleDegrees),
      centerX_(centerX), centerY_(centerY),
      displayWidth_(displayWidth), displayHeight_(displayHeight),
      occurrence_(occurrence)
{
}

RotateImageCommand::~RotateImageCommand()
{
    // Release whenever non-null: the holder is owned for the command's whole
    // lifetime (undo's restore is non-consuming), so the old `executed_ &&`
    // guard would leak a standalone pdf_document when the command ends undone.
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
    }
}

void RotateImageCommand::execute()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op (covers redo()).
    // redo() re-enters here; release any prior holder before re-capturing so a
    // re-execution never leaks a standalone pdf_document (R2-01 holder design).
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
    }
    savedContentsRef_ = manager_->saveContentsRef(page_, doc_);
    const bool ok = editor_->rotateImage(page_, doc_, PlacementId{xobjectName_, occurrence_},
                         angleDegrees_, centerX_, centerY_,
                         displayWidth_, displayHeight_);
    if (!ok) {
        // The editor refused (placement not found, inside a Form XObject,
        // empty crop, ...): nothing changed. Record the failure so the
        // UndoManager stacks no entry, leave the page clean, and keep undo a
        // no-op (image-placement-state phase 2).
        manager_->releaseRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
        failed_ = true;
        executed_ = false;
        return;
    }
    failed_ = false;
    doc_->markPageDirty(page_->index());
    executed_ = true;
}

void RotateImageCommand::undo()
{
    if (!executed_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    if (savedContentsRef_) {
        // Non-consuming restore (grafts a COPY into the live doc). The holder
        // stays owned by this command and is released in the dtor, so do NOT
        // null it here — that old line was for the raw-ref ownership transfer.
        manager_->restoreImage(page_, doc_, {}, nullptr, savedContentsRef_);
        doc_->markPageDirty(page_->index());
    }
    executed_ = false;
}

std::string RotateImageCommand::description() const
{
    return "Rotate Image";
}

// -----------------------------------------------------------------------
// CropImageCommand
// -----------------------------------------------------------------------

CropImageCommand::CropImageCommand(
    ImageManager* manager,
    ImageContentEditor* editor,
    pdfe::core::Page* page,
    pdfe::core::Document* doc,
    const std::string& xobjectName,
    QRectF cropRect,
    float displayWidth, float displayHeight,
    float x, float y,
    int occurrence)
    : manager_(manager), editor_(editor), page_(page), doc_(doc),
      xobjectName_(xobjectName), cropRect_(cropRect),
      displayWidth_(displayWidth), displayHeight_(displayHeight),
      x_(x), y_(y), occurrence_(occurrence)
{
}

CropImageCommand::~CropImageCommand()
{
    // Release whenever non-null: the holder is owned for the command's whole
    // lifetime (undo's restore is non-consuming), so the old `executed_ &&`
    // guard would leak a standalone pdf_document when the command ends undone.
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
    }
}

void CropImageCommand::execute()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op (covers redo()).
    // redo() re-enters here; release any prior holder before re-capturing so a
    // re-execution never leaks a standalone pdf_document (R2-01 holder design).
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
    }
    savedContentsRef_ = manager_->saveContentsRef(page_, doc_);
    const bool ok = editor_->cropImage(page_, doc_, PlacementId{xobjectName_, occurrence_}, cropRect_,
                       displayWidth_, displayHeight_, x_, y_);
    if (!ok) {
        // The editor refused (placement not found, inside a Form XObject,
        // empty crop, ...): nothing changed. Record the failure so the
        // UndoManager stacks no entry, leave the page clean, and keep undo a
        // no-op (image-placement-state phase 2).
        manager_->releaseRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
        failed_ = true;
        executed_ = false;
        return;
    }
    failed_ = false;
    doc_->markPageDirty(page_->index());
    executed_ = true;
}

void CropImageCommand::undo()
{
    if (!executed_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    if (savedContentsRef_) {
        // Non-consuming restore (grafts a COPY into the live doc). The holder
        // stays owned by this command and is released in the dtor, so do NOT
        // null it here — that old line was for the raw-ref ownership transfer.
        manager_->restoreImage(page_, doc_, {}, nullptr, savedContentsRef_);
        doc_->markPageDirty(page_->index());
    }
    executed_ = false;
}

std::string CropImageCommand::description() const
{
    return "Crop Image";
}

// -----------------------------------------------------------------------
// Save-path page rebind (pipeline f4a82c1e Phase 3a) — every image command
// caches a raw Page*; report its index before a save drops the holders and
// re-point it at the freshly-resolved page afterwards so undo stays valid.
// -----------------------------------------------------------------------

int InsertImageCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void InsertImageCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int RemoveImageCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void RemoveImageCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int ResizeImageCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void ResizeImageCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int RotateImageCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void RotateImageCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int CropImageCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void CropImageCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

// -----------------------------------------------------------------------
// ClipImageToPageCommand (ui-tweaks phase-4) - same snapshot/restore undo as
// CropImageCommand.
// -----------------------------------------------------------------------

ClipImageToPageCommand::ClipImageToPageCommand(
    ImageManager* manager,
    ImageContentEditor* editor,
    pdfe::core::Page* page,
    pdfe::core::Document* doc,
    const std::string& xobjectName,
    int occurrence)
    : manager_(manager), editor_(editor), page_(page), doc_(doc),
      xobjectName_(xobjectName), occurrence_(occurrence)
{
}

ClipImageToPageCommand::~ClipImageToPageCommand()
{
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
    }
}

void ClipImageToPageCommand::execute()
{
    if (!page_) return;
    if (savedContentsRef_) {
        manager_->releaseRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
    }
    savedContentsRef_ = manager_->saveContentsRef(page_, doc_);
    const bool ok = editor_->clipImageToPage(page_, doc_, PlacementId{xobjectName_, occurrence_});
    if (!ok) {
        manager_->releaseRef(savedContentsRef_);
        savedContentsRef_ = nullptr;
        failed_ = true;
        executed_ = false;
        return;
    }
    failed_ = false;
    doc_->markPageDirty(page_->index());
    executed_ = true;
}

void ClipImageToPageCommand::undo()
{
    if (!executed_) return;
    if (!page_) return;
    if (savedContentsRef_) {
        manager_->restoreImage(page_, doc_, {}, nullptr, savedContentsRef_);
        doc_->markPageDirty(page_->index());
    }
    executed_ = false;
}

std::string ClipImageToPageCommand::description() const
{
    return "Clip Image to Page";
}

int ClipImageToPageCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void ClipImageToPageCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

} // namespace pdfe::image
