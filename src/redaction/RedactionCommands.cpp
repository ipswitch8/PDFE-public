#include "RedactionCommands.h"
#include "RedactionManager.h"
#include "core/Page.h"

namespace pdfe::redaction {

// --- CreateRedactionCommand ---

CreateRedactionCommand::CreateRedactionCommand(
    RedactionManager* manager,
    pdfe::core::Page* page,
    QRectF rect
) : manager_(manager), page_(page), rect_(rect) {}

void CreateRedactionCommand::execute() {
    createdData_ = manager_->createRedactionMark(page_, rect_);
    executed_ = true;
}

void CreateRedactionCommand::undo() {
    if (!executed_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op. See SaveWithRebind.h step 5.
    manager_->deleteRedactionMark(page_, createdData_.annotIndex);
}

void CreateRedactionCommand::redo() {
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    createdData_.annotIndex = manager_->restoreRedactionMark(page_, createdData_);
}

std::string CreateRedactionCommand::description() const {
    return "Mark Redaction";
}

const RedactionData& CreateRedactionCommand::createdMark() const {
    return createdData_;
}

// --- DeleteRedactionCommand ---

DeleteRedactionCommand::DeleteRedactionCommand(
    RedactionManager* manager,
    pdfe::core::Page* page,
    int annotIndex
) : manager_(manager), page_(page), annotIndex_(annotIndex) {}

void DeleteRedactionCommand::execute() {
    savedData_ = manager_->deleteRedactionMark(page_, annotIndex_);
    executed_ = true;
}

void DeleteRedactionCommand::undo() {
    if (!executed_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    annotIndex_ = manager_->restoreRedactionMark(page_, savedData_);
}

void DeleteRedactionCommand::redo() {
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    savedData_ = manager_->deleteRedactionMark(page_, annotIndex_);
}

std::string DeleteRedactionCommand::description() const {
    return "Delete Redaction Mark";
}

// --- ModifyRedactionCommand ---

ModifyRedactionCommand::ModifyRedactionCommand(
    RedactionManager* manager,
    pdfe::core::Page* page,
    int annotIndex,
    Property property,
    RedactionData oldData,
    RedactionData newData
) : manager_(manager), page_(page), annotIndex_(annotIndex),
    property_(property), oldData_(std::move(oldData)), newData_(std::move(newData)) {}

void ModifyRedactionCommand::execute() {
    applyData(newData_);
    executed_ = true;
}

void ModifyRedactionCommand::undo() {
    if (!executed_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    applyData(oldData_);
}

void ModifyRedactionCommand::redo() {
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    applyData(newData_);
}

std::string ModifyRedactionCommand::description() const {
    switch (property_) {
        case Property::FillColor:   return "Change Redaction Color";
        case Property::OverlayText: return "Change Redaction Text";
        case Property::Rect:        return "Move Redaction";
        default:                    return "Modify Redaction";
    }
}

bool ModifyRedactionCommand::mergeWith(const pdfe::commands::Command* other) {
    auto otherCmd = dynamic_cast<const ModifyRedactionCommand*>(other);
    if (!otherCmd) return false;
    if (otherCmd->annotIndex_ != annotIndex_) return false;
    if (otherCmd->property_ != property_) return false;
    if (otherCmd->page_ != page_) return false;

    // Absorb the other command's new data
    newData_ = otherCmd->newData_;
    return true;
}

int ModifyRedactionCommand::mergeId() const {
    // Unique ID for redaction modification merging
    return 5000 + annotIndex_;
}

void ModifyRedactionCommand::applyData(const RedactionData& data) {
    switch (property_) {
        case Property::FillColor:
            manager_->setFillColor(page_, annotIndex_, data.fillColor);
            break;
        case Property::OverlayText:
            manager_->setOverlayText(page_, annotIndex_, data.overlayText);
            break;
        case Property::Rect:
            manager_->setRect(page_, annotIndex_, data.rect);
            break;
    }
}

// --- Save-path page rebind (pipeline f4a82c1e Phase 3a) ---
// Redaction MARK commands live on the ordinary undo stack (mark a redaction ->
// CreateRedactionCommand, then File>Save), so they need the rebind contract
// like every other Page*-holding command. (The irreversible Apply-Redactions
// flow is separate: onApplyRedactions clears commandPages_ + undo before its
// own save, so it never reaches here.)

int CreateRedactionCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void CreateRedactionCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int DeleteRedactionCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void DeleteRedactionCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int ModifyRedactionCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void ModifyRedactionCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

} // namespace pdfe::redaction
