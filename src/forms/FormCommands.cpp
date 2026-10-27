#include "FormCommands.h"
#include "FormManager.h"
#include "core/Page.h"

namespace pdfe::forms {

// ============================================================================
// SetFieldValueCommand
// ============================================================================

SetFieldValueCommand::SetFieldValueCommand(
    FormManager* manager,
    pdfe::core::Page* page,
    int widgetIndex,
    const std::string& newValue
)
    : manager_(manager)
    , page_(page)
    , widgetIndex_(widgetIndex)
    , newValue_(newValue)
{
}

void SetFieldValueCommand::execute()
{
    oldValue_ = manager_->setFieldValue(page_, widgetIndex_, newValue_);
    executed_ = true;
}

void SetFieldValueCommand::undo()
{
    if (!executed_) {
        return;
    }
    if (!page_) return;  // rebound to null (page gone) → safe no-op. See SaveWithRebind.h step 5.
    manager_->setFieldValue(page_, widgetIndex_, oldValue_);
}

void SetFieldValueCommand::redo()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    manager_->setFieldValue(page_, widgetIndex_, newValue_);
}

std::string SetFieldValueCommand::description() const
{
    return "Set Field Value";
}

bool SetFieldValueCommand::mergeWith(const pdfe::commands::Command* other)
{
    // Only merge with other SetFieldValueCommand instances
    const auto* otherSetValue = dynamic_cast<const SetFieldValueCommand*>(other);
    if (!otherSetValue) {
        return false;
    }

    // Must be same page and same widget
    if (otherSetValue->page_ != page_ || otherSetValue->widgetIndex_ != widgetIndex_) {
        return false;
    }

    // Absorb the new value from the other command
    newValue_ = otherSetValue->newValue_;
    return true;
}

int SetFieldValueCommand::mergeId() const
{
    // Create a unique ID based on widget index for merge coalescing
    // Use a base offset to avoid conflicts with other command types
    return 1000 + widgetIndex_;
}

// ============================================================================
// ToggleFieldCommand
// ============================================================================

ToggleFieldCommand::ToggleFieldCommand(
    FormManager* manager,
    pdfe::core::Page* page,
    int widgetIndex
)
    : manager_(manager)
    , page_(page)
    , widgetIndex_(widgetIndex)
{
}

void ToggleFieldCommand::execute()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op (covers redo()).
    manager_->toggleField(page_, widgetIndex_);
}

void ToggleFieldCommand::undo()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    // Toggle is self-inverse
    manager_->toggleField(page_, widgetIndex_);
}

std::string ToggleFieldCommand::description() const
{
    return "Toggle Field";
}

// ============================================================================
// SetChoiceSelectionCommand
// ============================================================================

SetChoiceSelectionCommand::SetChoiceSelectionCommand(
    FormManager* manager,
    pdfe::core::Page* page,
    int widgetIndex,
    std::vector<int> oldSelection,
    std::vector<int> newSelection
)
    : manager_(manager)
    , page_(page)
    , widgetIndex_(widgetIndex)
    , oldSelection_(std::move(oldSelection))
    , newSelection_(std::move(newSelection))
{
}

void SetChoiceSelectionCommand::execute()
{
    manager_->setChoiceSelection(page_, widgetIndex_, newSelection_);
    executed_ = true;
}

void SetChoiceSelectionCommand::undo()
{
    if (!executed_) {
        return;
    }
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    manager_->setChoiceSelection(page_, widgetIndex_, oldSelection_);
}

void SetChoiceSelectionCommand::redo()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    manager_->setChoiceSelection(page_, widgetIndex_, newSelection_);
}

std::string SetChoiceSelectionCommand::description() const
{
    return "Set Choice Selection";
}

// ============================================================================
// ResetFormCommand
// ============================================================================

ResetFormCommand::ResetFormCommand(
    FormManager* manager,
    pdfe::core::Page* page
)
    : manager_(manager)
    , page_(page)
{
}

void ResetFormCommand::execute()
{
    // Save current state before resetting
    savedState_ = manager_->getFormFields(page_);
    manager_->resetForm(page_);
    executed_ = true;
}

void ResetFormCommand::undo()
{
    if (!executed_) {
        return;
    }
    if (!page_) return;  // rebound to null (page gone) → safe no-op.

    // Restore each field from saved state
    for (const auto& fieldData : savedState_) {
        if (fieldData.type == FormFieldType::Text ||
            fieldData.type == FormFieldType::ComboBox ||
            fieldData.type == FormFieldType::ListBox) {
            manager_->setFieldValue(page_, fieldData.widgetIndex, fieldData.value);
        }

        if (fieldData.type == FormFieldType::ComboBox ||
            fieldData.type == FormFieldType::ListBox) {
            manager_->setChoiceSelection(page_, fieldData.widgetIndex, fieldData.selectedIndices);
        }

        // Note: Checkbox/radio button restoration would require checking the actual
        // state and toggling if needed. For now, value-based restoration should work
        // for text and choice fields, which are the most common reset use cases.
    }
}

void ResetFormCommand::redo()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    manager_->resetForm(page_);
}

std::string ResetFormCommand::description() const
{
    return "Reset Form";
}

// ============================================================================
// CreateFormFieldCommand
// ============================================================================

CreateFormFieldCommand::CreateFormFieldCommand(
    FormManager* manager,
    pdfe::core::Page* page,
    FormFieldType type,
    QRectF rect,
    const std::string& fieldName
)
    : manager_(manager)
    , page_(page)
    , type_(type)
    , rect_(rect)
    , fieldName_(fieldName)
{
}

void CreateFormFieldCommand::execute()
{
    createdWidgetIndex_ = manager_->createFormField(page_, type_, rect_, fieldName_);
    executed_ = true;
}

void CreateFormFieldCommand::undo()
{
    if (!executed_) {
        return;
    }
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    savedData_ = manager_->deleteFormField(page_, createdWidgetIndex_);
}

void CreateFormFieldCommand::redo()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    createdWidgetIndex_ = manager_->restoreFormField(page_, savedData_);
}

std::string CreateFormFieldCommand::description() const
{
    return "Create Form Field";
}

// ============================================================================
// DeleteFormFieldCommand
// ============================================================================

DeleteFormFieldCommand::DeleteFormFieldCommand(
    FormManager* manager,
    pdfe::core::Page* page,
    int widgetIndex
)
    : manager_(manager)
    , page_(page)
    , widgetIndex_(widgetIndex)
{
}

void DeleteFormFieldCommand::execute()
{
    savedData_ = manager_->deleteFormField(page_, widgetIndex_);
    executed_ = true;
}

void DeleteFormFieldCommand::undo()
{
    if (!executed_) {
        return;
    }
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    widgetIndex_ = manager_->restoreFormField(page_, savedData_);
}

void DeleteFormFieldCommand::redo()
{
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    savedData_ = manager_->deleteFormField(page_, widgetIndex_);
}

std::string DeleteFormFieldCommand::description() const
{
    return "Delete Form Field";
}

// ============================================================================
// Save-path page rebind (pipeline f4a82c1e Phase 3a) — every form command
// caches a raw Page*; report its index before a save drops the holders and
// re-point it at the freshly-resolved page afterwards so undo stays valid.
// ============================================================================

int SetFieldValueCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void SetFieldValueCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int ToggleFieldCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void ToggleFieldCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int SetChoiceSelectionCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void SetChoiceSelectionCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int ResetFormCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void ResetFormCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int CreateFormFieldCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void CreateFormFieldCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

int DeleteFormFieldCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void DeleteFormFieldCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

} // namespace pdfe::forms
