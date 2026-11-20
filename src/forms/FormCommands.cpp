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
// SetFieldAppearanceCommand (GitHub #460)
// ============================================================================

SetFieldAppearanceCommand::SetFieldAppearanceCommand(
    FormManager* manager,
    pdfe::core::Page* page,
    int widgetIndex,
    const std::string& font,
    float size,
    const QColor& color
)
    : manager_(manager)
    , page_(page)
    , widgetIndex_(widgetIndex)
    , font_(font)
    , size_(size)
    , color_(color)
{
}

void SetFieldAppearanceCommand::execute()
{
    if (!manager_ || !page_) return;
    if (executed_) {
        // Idempotent: re-apply without re-taking the undo snapshot (see the
        // class comment on UndoManager's double execute).
        redo();
        return;
    }
    const auto before = manager_->getFieldOwnDefaultAppearance(page_, widgetIndex_);
    if (!before) {
        executed_ = true;  // bad index: nothing to do, undo/redo are no-ops
        return;
    }
    // Phase-4 security audit: the setter can throw part-way (MuPDF writes /DA
    // and deletes /DS and /RC before pdf_update_annot runs). Put the snapshot
    // back, best effort, and rethrow, leaving the command un-executed, so a
    // half-applied change never stays behind a no-op undo.
    bool applied = false;
    try {
        applied = manager_->setFieldDefaultAppearance(page_, widgetIndex_, font_,
                                                      size_, color_);
    } catch (...) {
        try {
            manager_->restoreFieldOwnDefaultAppearance(page_, widgetIndex_, *before);
        } catch (...) {
            // The original error is the one to report.
        }
        throw;
    }
    if (applied) {
        before_ = *before;
        const auto after = manager_->getFieldOwnDefaultAppearance(page_, widgetIndex_);
        if (after) after_ = *after;
    }
    // State only after the call returned.
    applied_ = applied;
    executed_ = true;
}

void SetFieldAppearanceCommand::undo()
{
    if (!executed_ || !applied_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    manager_->restoreFieldOwnDefaultAppearance(page_, widgetIndex_, before_);
}

void SetFieldAppearanceCommand::redo()
{
    if (!executed_ || !applied_) return;
    if (!page_) return;  // rebound to null (page gone) → safe no-op.
    manager_->restoreFieldOwnDefaultAppearance(page_, widgetIndex_, after_);
}

std::string SetFieldAppearanceCommand::description() const
{
    return "Set Field Font";
}

bool SetFieldAppearanceCommand::mergeWith(const pdfe::commands::Command* /*other*/)
{
    // Never merges: one appearance change, one undo step (see the header).
    return false;
}

int SetFieldAppearanceCommand::mergeId() const
{
    // 0 = "never merge" to UndoManager::execute, which then never runs this
    // command's execute() twice.
    return 0;
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

int SetFieldAppearanceCommand::pageIndex() const { return page_ ? page_->index() : -1; }
void SetFieldAppearanceCommand::rebindPage(pdfe::core::Page* page) { page_ = page; }

} // namespace pdfe::forms
