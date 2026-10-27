#include "image/ImageCommands.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "core/Document.h"
#include "core/Page.h"

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
    // Save current content stream for undo
    savedContentsRef_ = manager_->saveContentsRef(page_, doc_);

    result_ = manager_->insertImage(page_, doc_, options_);
    doc_->markPageDirty(page_->index());
    executed_ = true;
}

void InsertImageCommand::undo()
{
    if (!executed_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op. See SaveWithRebind.h step 5.

    // Restore the original content stream and remove the inserted image
    if (savedContentsRef_) {
        // Save the image ref before removing it, so redo can re-insert
        manager_->restoreImage(page_, doc_, result_.imageInfo, nullptr, savedContentsRef_);

        // Remove the image from XObject dict
        auto removeResult = manager_->removeImage(page_, doc_, result_.imageInfo.imageIndex);
        (void)removeResult;
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

    manager_->removeImage(page_, doc_, imageIndex_);
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
    float x, float y)
    : manager_(manager), editor_(editor), page_(page), doc_(doc),
      xobjectName_(xobjectName), newWidth_(newWidth), newHeight_(newHeight),
      x_(x), y_(y)
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
    editor_->resizeImage(page_, doc_, xobjectName_, newWidth_, newHeight_, x_, y_);
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
    float displayWidth, float displayHeight)
    : manager_(manager), editor_(editor), page_(page), doc_(doc),
      xobjectName_(xobjectName), angleDegrees_(angleDegrees),
      centerX_(centerX), centerY_(centerY),
      displayWidth_(displayWidth), displayHeight_(displayHeight)
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
    editor_->rotateImage(page_, doc_, xobjectName_, angleDegrees_,
                         centerX_, centerY_, displayWidth_, displayHeight_);
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
    float x, float y)
    : manager_(manager), editor_(editor), page_(page), doc_(doc),
      xobjectName_(xobjectName), cropRect_(cropRect),
      displayWidth_(displayWidth), displayHeight_(displayHeight),
      x_(x), y_(y)
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
    editor_->cropImage(page_, doc_, xobjectName_, cropRect_,
                       displayWidth_, displayHeight_, x_, y_);
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

} // namespace pdfe::image
