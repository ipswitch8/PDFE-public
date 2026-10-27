#include "SignatureCommands.h"
#include "SignatureManager.h"
#include "core/Page.h"

namespace pdfe::signatures {

// ============================================================================
// CreateSignatureFieldCommand
// ============================================================================

CreateSignatureFieldCommand::CreateSignatureFieldCommand(
    SignatureManager* manager,
    pdfe::core::Page* page,
    const std::string& fieldName,
    QRectF rect)
    : manager_(manager)
    , page_(page)
    , fieldName_(fieldName)
    , rect_(rect)
{
}

CreateSignatureFieldCommand::~CreateSignatureFieldCommand() = default;

void CreateSignatureFieldCommand::execute()
{
    createdData_ = manager_->createSignatureField(page_, fieldName_, rect_);
    executed_ = true;
}

void CreateSignatureFieldCommand::undo()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op. See SaveWithRebind.h step 5.
    if (executed_) {
        manager_->removeSignatureField(page_, createdData_.widgetIndex);
    }
}

void CreateSignatureFieldCommand::redo()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    int newIdx = manager_->restoreSignatureField(page_, createdData_);
    createdData_.widgetIndex = newIdx;
}

std::string CreateSignatureFieldCommand::description() const
{
    return "Create Signature Field";
}

// ============================================================================
// Save-path page rebind (pipeline f4a82c1e Phase 3a). Pure page-ref plumbing:
// this command only CREATES a signature field (undoable); it performs no
// signing / crypto (the class comment notes signing itself is not undoable).
// ============================================================================

int CreateSignatureFieldCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void CreateSignatureFieldCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

} // namespace pdfe::signatures
