#pragma once
#include "FormFieldData.h"
#include "core/Command.h"
#include <memory>
#include <vector>
#include <string>

namespace pdfe::core { class Page; }

namespace pdfe::forms {
class FormManager;

/**
 * @brief Command to set the value of a form field.
 *
 * Supports undo by restoring the old value. Supports merging consecutive
 * edits to the same field (e.g., typing in a text field).
 */
class SetFieldValueCommand : public pdfe::commands::Command {
public:
    /**
     * @brief Construct a set field value command.
     *
     * @param manager The form manager
     * @param page The page containing the form field
     * @param widgetIndex The index of the widget to modify
     * @param newValue The new value to set
     */
    SetFieldValueCommand(
        FormManager* manager,
        pdfe::core::Page* page,
        int widgetIndex,
        const std::string& newValue
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    bool mergeWith(const pdfe::commands::Command* other) override;
    [[nodiscard]] int mergeId() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    FormManager* manager_;
    pdfe::core::Page* page_;
    int widgetIndex_;
    std::string oldValue_;
    std::string newValue_;
    bool executed_ = false;
};

/**
 * @brief Command to toggle a checkbox or radio button field.
 *
 * This command is self-inverse (executing it toggles, undoing it toggles again).
 */
class ToggleFieldCommand : public pdfe::commands::Command {
public:
    /**
     * @brief Construct a toggle field command.
     *
     * @param manager The form manager
     * @param page The page containing the form field
     * @param widgetIndex The index of the widget to toggle
     */
    ToggleFieldCommand(
        FormManager* manager,
        pdfe::core::Page* page,
        int widgetIndex
    );

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    FormManager* manager_;
    pdfe::core::Page* page_;
    int widgetIndex_;
};

/**
 * @brief Command to set the selected options in a choice field (combo/list).
 *
 * Supports undo by restoring the previous selection state.
 */
class SetChoiceSelectionCommand : public pdfe::commands::Command {
public:
    /**
     * @brief Construct a set choice selection command.
     *
     * @param manager The form manager
     * @param page The page containing the form field
     * @param widgetIndex The index of the widget to modify
     * @param oldSelection The previous selection state
     * @param newSelection The new selection state
     */
    SetChoiceSelectionCommand(
        FormManager* manager,
        pdfe::core::Page* page,
        int widgetIndex,
        std::vector<int> oldSelection,
        std::vector<int> newSelection
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    FormManager* manager_;
    pdfe::core::Page* page_;
    int widgetIndex_;
    std::vector<int> oldSelection_;
    std::vector<int> newSelection_;
    bool executed_ = false;
};

/**
 * @brief Command to reset all form fields on a page to their default values.
 *
 * Supports undo by restoring the saved field states.
 */
class ResetFormCommand : public pdfe::commands::Command {
public:
    /**
     * @brief Construct a reset form command.
     *
     * @param manager The form manager
     * @param page The page containing the form fields
     */
    ResetFormCommand(
        FormManager* manager,
        pdfe::core::Page* page
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    FormManager* manager_;
    pdfe::core::Page* page_;
    std::vector<FormFieldData> savedState_;
    bool executed_ = false;
};

/**
 * @brief Command to create a new form field.
 *
 * Supports undo by deleting the created field and redo by restoring it.
 */
class CreateFormFieldCommand : public pdfe::commands::Command {
public:
    /**
     * @brief Construct a create form field command.
     *
     * @param manager The form manager
     * @param page The page to create the field on
     * @param type The type of form field to create
     * @param rect The bounding rectangle for the field
     * @param fieldName The name for the new field
     */
    CreateFormFieldCommand(
        FormManager* manager,
        pdfe::core::Page* page,
        FormFieldType type,
        QRectF rect,
        const std::string& fieldName
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    FormManager* manager_;
    pdfe::core::Page* page_;
    FormFieldType type_;
    QRectF rect_;
    std::string fieldName_;
    int createdWidgetIndex_ = -1;
    FormFieldData savedData_;
    bool executed_ = false;
};

/**
 * @brief Command to delete a form field.
 *
 * Supports undo by restoring the deleted field and redo by deleting it again.
 */
class DeleteFormFieldCommand : public pdfe::commands::Command {
public:
    /**
     * @brief Construct a delete form field command.
     *
     * @param manager The form manager
     * @param page The page containing the field
     * @param widgetIndex The index of the widget to delete
     */
    DeleteFormFieldCommand(
        FormManager* manager,
        pdfe::core::Page* page,
        int widgetIndex
    );

    void execute() override;
    void undo() override;
    void redo() override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int pageIndex() const override;
    void rebindPage(pdfe::core::Page* page) override;

private:
    FormManager* manager_;
    pdfe::core::Page* page_;
    int widgetIndex_;
    FormFieldData savedData_;
    bool executed_ = false;
};

} // namespace pdfe::forms
