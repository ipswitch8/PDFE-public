#include "PageCommands.h"
#include "Document.h"
#include "MuPdfContext.h"
#include "PageOperations.h"
#include <algorithm>
#include <iterator>
#include <utility>

namespace pdfe::commands {

// ============================================================================
// RotatePageCommand
// ============================================================================

RotatePageCommand::RotatePageCommand(
    pdfe::core::Document* doc,
    int pageIndex,
    int angleDelta
)
    : doc_(doc)
    , pageIndex_(pageIndex)
    , angleDelta_(angleDelta)
{
}

void RotatePageCommand::execute()
{
    doc_->rotatePage(pageIndex_, angleDelta_);
    executed_ = true;
}

void RotatePageCommand::undo()
{
    if (!executed_) {
        return;
    }
    doc_->rotatePage(pageIndex_, -angleDelta_);
}

void RotatePageCommand::redo()
{
    doc_->rotatePage(pageIndex_, angleDelta_);
}

std::string RotatePageCommand::description() const
{
    return "Rotate Page " + std::to_string(pageIndex_ + 1);
}

// ============================================================================
// DeletePageCommand
// ============================================================================

DeletePageCommand::DeletePageCommand(
    pdfe::core::Document* doc,
    int pageIndex
)
    : doc_(doc)
    , pageIndex_(pageIndex)
{
}

DeletePageCommand::~DeletePageCommand()
{
    // Release via the Document-INDEPENDENT static path: at teardown the owning
    // Document may already be freed (undo stack outliving the Document — e.g.
    // MainWindow drops document_ before ~QObject deletes the child UndoManager),
    // so dereferencing doc_ here is a use-after-free (defect B2). The retained
    // holder owns its own context and needs no live Document to be dropped.
    if (retainedPageObj_) {
        pdfe::core::Document::releaseRetainedPage(retainedPageObj_);
    }
}

void DeletePageCommand::execute()
{
    retainedPageObj_ = doc_->retainPageObj(pageIndex_);
    doc_->deletePage(pageIndex_);
    executed_ = true;
}

void DeletePageCommand::undo()
{
    if (!executed_) {
        return;
    }
    doc_->insertPageObj(pageIndex_, retainedPageObj_);
    // insertPageObj grafts an independent COPY of the retained page, so the
    // holder is done — release it (redo() re-retains a fresh one).
    doc_->releasePageObj(retainedPageObj_);
    retainedPageObj_ = nullptr;
}

void DeletePageCommand::redo()
{
    if (!executed_) {
        execute();
        return;
    }
    retainedPageObj_ = doc_->retainPageObj(pageIndex_);
    doc_->deletePage(pageIndex_);
}

std::string DeletePageCommand::description() const
{
    return "Delete Page " + std::to_string(pageIndex_ + 1);
}

// ============================================================================
// InsertBlankPageCommand
// ============================================================================

InsertBlankPageCommand::InsertBlankPageCommand(
    pdfe::core::Document* doc,
    int atIndex,
    float widthPt,
    float heightPt,
    int rotation
)
    : doc_(doc)
    , atIndex_(atIndex)
    , widthPt_(widthPt)
    , heightPt_(heightPt)
    , rotation_(rotation)
{
}

void InsertBlankPageCommand::execute()
{
    doc_->insertBlankPage(atIndex_, widthPt_, heightPt_, rotation_);
    executed_ = true;
}

void InsertBlankPageCommand::undo()
{
    if (!executed_) {
        return;
    }
    doc_->deletePage(atIndex_);
}

void InsertBlankPageCommand::redo()
{
    doc_->insertBlankPage(atIndex_, widthPt_, heightPt_, rotation_);
}

std::string InsertBlankPageCommand::description() const
{
    return "Insert Blank Page at " + std::to_string(atIndex_ + 1);
}

// ============================================================================
// MovePageCommand
// ============================================================================

MovePageCommand::MovePageCommand(
    pdfe::core::Document* doc,
    int fromIndex,
    int toIndex
)
    : doc_(doc)
    , fromIndex_(fromIndex)
    , toIndex_(toIndex)
{
}

MovePageCommand::~MovePageCommand()
{
    // Document-independent release — see DeletePageCommand::~DeletePageCommand
    // (defect B2). retainedPageObj_ is legacy/unused on the reorder path but the
    // guard is kept for safety.
    if (retainedPageObj_) {
        pdfe::core::Document::releaseRetainedPage(retainedPageObj_);
    }
}

namespace {
// Build the reorder permutation for "move the page at `from` so it lands at
// display position `to`" over `count` pages. Returns a vector of ORIGINAL page
// indices in their new order (the form reorderPages / pdf_rearrange_pages want).
// Matches the historical observable behaviour: moving forward the moved page's
// final position is to-1 (removal shifts the tail down), else `to`.
std::vector<int> buildMovePermutation(int count, int from, int to)
{
    std::vector<int> order;
    order.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (i != from) order.push_back(i);
    }
    int finalPos = (from < to) ? (to - 1) : to;
    if (finalPos < 0) finalPos = 0;
    if (finalPos > static_cast<int>(order.size())) finalPos = static_cast<int>(order.size());
    order.insert(order.begin() + finalPos, from);
    return order;
}

// Invert a permutation expressed as "new position k holds original index P[k]"
// into "new position k := current position Q[k]" -- the undo of reorderPages(P).
std::vector<int> invertPermutation(const std::vector<int>& p)
{
    std::vector<int> q(p.size());
    for (int pos = 0; pos < static_cast<int>(p.size()); ++pos) {
        q[p[pos]] = pos;
    }
    return q;
}
} // namespace

void MovePageCommand::execute()
{
    // R3-01: a page move is a pure reorder (all pages retained). Route it
    // through reorderPages -> pdf_rearrange_pages so bookmark / link / named
    // destinations are remapped to the new positions instead of dangling. The
    // old retain(copy)/delete/graft path gave the moved page a NEW object
    // identity and left every destination pointing at the wrong (or an
    // orphaned) page.
    int count = doc_->pageCount();
    movePermutation_ = buildMovePermutation(count, fromIndex_, toIndex_);
    doc_->reorderPages(movePermutation_);
    executed_ = true;
}

void MovePageCommand::undo()
{
    if (!executed_) {
        return;
    }
    doc_->reorderPages(invertPermutation(movePermutation_));
}

void MovePageCommand::redo()
{
    if (!executed_) {
        execute();
        return;
    }
    doc_->reorderPages(movePermutation_);
}

std::string MovePageCommand::description() const
{
    return "Move Page " + std::to_string(fromIndex_ + 1) + " to " + std::to_string(toIndex_ + 1);
}

// ============================================================================
// ImportPagesCommand
// ============================================================================

ImportPagesCommand::ImportPagesCommand(
    pdfe::core::Document* doc,
    int atIndex,
    std::filesystem::path sourcePath,
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    int sourceStart,
    int sourceEnd
)
    : doc_(doc)
    , atIndex_(atIndex)
    , sourcePath_(std::move(sourcePath))
    , context_(std::move(context))
    , sourceStart_(sourceStart)
    , sourceEnd_(sourceEnd)
{
}

void ImportPagesCommand::execute()
{
    // Open the source document
    auto sourceDoc = pdfe::core::Document::open(context_, sourcePath_);

    // Import the pages
    doc_->graftPages(atIndex_, *sourceDoc, sourceStart_, sourceEnd_);

    // Calculate the number of imported pages
    importedCount_ = sourceEnd_ - sourceStart_ + 1;

    executed_ = true;
}

void ImportPagesCommand::undo()
{
    if (!executed_) {
        return;
    }

    // Delete the imported pages in reverse order
    for (int i = importedCount_ - 1; i >= 0; --i) {
        doc_->deletePage(atIndex_ + i);
    }
}

void ImportPagesCommand::redo()
{
    if (!executed_) {
        execute();
        return;
    }

    // Open the source document
    auto sourceDoc = pdfe::core::Document::open(context_, sourcePath_);

    // Import the pages again
    doc_->graftPages(atIndex_, *sourceDoc, sourceStart_, sourceEnd_);
}

std::string ImportPagesCommand::description() const
{
    if (!executed_) {
        return "Import Pages";
    }
    return "Import " + std::to_string(importedCount_) + " Page(s)";
}

// ============================================================================
// DeletePageRangeCommand
// ============================================================================

DeletePageRangeCommand::DeletePageRangeCommand(
    pdfe::core::Document* doc,
    std::vector<int> pageIndices
)
    : doc_(doc)
    , pageIndices_(std::move(pageIndices))
{
    // Ensure indices are sorted ascending
    std::sort(pageIndices_.begin(), pageIndices_.end());
}

DeletePageRangeCommand::~DeletePageRangeCommand()
{
    // Release any retained page objects via the Document-independent static path
    // (the owning Document may already be gone at teardown — defect B2).
    for (void* obj : retainedPageObjs_) {
        if (obj) {
            pdfe::core::Document::releaseRetainedPage(obj);
        }
    }
}

void DeletePageRangeCommand::execute()
{
    // Retain all page objects in forward order
    retainedPageObjs_.clear();
    retainedPageObjs_.reserve(pageIndices_.size());
    std::transform(pageIndices_.begin(), pageIndices_.end(),
        std::back_inserter(retainedPageObjs_),
        [this](int index) { return doc_->retainPageObj(index); });

    // Delete in reverse order to preserve indices
    for (auto it = pageIndices_.rbegin(); it != pageIndices_.rend(); ++it) {
        doc_->deletePage(*it);
    }

    executed_ = true;
}

void DeletePageRangeCommand::undo()
{
    if (!executed_) {
        return;
    }

    // Re-insert all page objects in forward order. insertPageObj grafts an
    // independent COPY, so each holder is redundant once inserted — release it
    // (redo() re-retains fresh holders).
    for (size_t i = 0; i < pageIndices_.size(); ++i) {
        doc_->insertPageObj(pageIndices_[i], retainedPageObjs_[i]);
        doc_->releasePageObj(retainedPageObjs_[i]);
    }

    retainedPageObjs_.clear();
}

void DeletePageRangeCommand::redo()
{
    if (!executed_) {
        execute();
        return;
    }

    // Retain all page objects in forward order
    retainedPageObjs_.clear();
    retainedPageObjs_.reserve(pageIndices_.size());
    std::transform(pageIndices_.begin(), pageIndices_.end(),
        std::back_inserter(retainedPageObjs_),
        [this](int index) { return doc_->retainPageObj(index); });

    // Delete in reverse order to preserve indices
    for (auto it = pageIndices_.rbegin(); it != pageIndices_.rend(); ++it) {
        doc_->deletePage(*it);
    }
}

std::string DeletePageRangeCommand::description() const
{
    if (pageIndices_.empty()) {
        return "Delete Pages";
    }
    if (pageIndices_.size() == 1) {
        return "Delete Page " + std::to_string(pageIndices_[0] + 1);
    }
    return "Delete " + std::to_string(pageIndices_.size()) + " Pages";
}

// ========== ReplacePageCommand ==========

ReplacePageCommand::ReplacePageCommand(
    pdfe::core::Document* doc,
    int pageIndex,
    std::shared_ptr<pdfe::core::Document> sourceDoc,
    int sourcePageIndex)
    : doc_(doc)
    , pageIndex_(pageIndex)
    , sourceDoc_(std::move(sourceDoc))
    , sourcePageIndex_(sourcePageIndex)
{
}

ReplacePageCommand::~ReplacePageCommand()
{
    // Document-independent release — the owning Document may already be freed at
    // teardown, so do NOT route through doc_ (defect B2).
    if (originalPageObj_) {
        pdfe::core::Document::releaseRetainedPage(originalPageObj_);
    }
}

void ReplacePageCommand::execute()
{
    if (!doc_ || !sourceDoc_) return;
    if (!executed_) {
        originalPageObj_ = doc_->retainPageObj(pageIndex_);
    }
    doc_->replacePage(pageIndex_, *sourceDoc_, sourcePageIndex_);
    executed_ = true;
}

void ReplacePageCommand::undo()
{
    if (!doc_ || !originalPageObj_) return;
    doc_->deletePage(pageIndex_);
    doc_->insertPageObj(pageIndex_, originalPageObj_);
}

std::string ReplacePageCommand::description() const
{
    return "Replace Page " + std::to_string(pageIndex_ + 1);
}

// ============================================================================
// CropPageCommand (Pipeline E phase-6 / Bucket 3d)
// ============================================================================

CropPageCommand::CropPageCommand(
    pdfe::core::Document* doc,
    int pageIndex,
    QRectF newCropBox
)
    : doc_(doc)
    , pageIndex_(pageIndex)
    , newCropBox_(std::move(newCropBox))
{
}

void CropPageCommand::execute()
{
    if (!doc_) return;
    if (!executed_) {
        oldCropBox_ = pdfe::core::getPageCropBox(doc_, pageIndex_);
    }
    pdfe::core::setPageCropBox(doc_, pageIndex_, newCropBox_);
    executed_ = true;
}

void CropPageCommand::undo()
{
    if (!doc_ || !executed_) return;
    if (!oldCropBox_.isEmpty()) {
        pdfe::core::setPageCropBox(doc_, pageIndex_, oldCropBox_);
    }
}

void CropPageCommand::redo()
{
    if (!doc_) return;
    pdfe::core::setPageCropBox(doc_, pageIndex_, newCropBox_);
}

std::string CropPageCommand::description() const
{
    return "Crop Page " + std::to_string(pageIndex_ + 1);
}

} // namespace pdfe::commands
