#pragma once

#include <QWidget>
#include "../forms/FormFieldData.h"

class QLabel;
class QLineEdit;
class QTextEdit;
class QCheckBox;
class QComboBox;
class QListWidget;
class QStackedWidget;

namespace pdfe::ui {

/// Widget for displaying and editing form field properties.
///
/// Shows the field type, name, value, and flags. Uses a QStackedWidget
/// to display the appropriate editor widget for each field type.
class FormFieldPropertiesPanel : public QWidget {
    Q_OBJECT

public:
    explicit FormFieldPropertiesPanel(QWidget* parent = nullptr);
    ~FormFieldPropertiesPanel() override;

    void setFormField(const pdfe::forms::FormFieldData& data);
    void clear();

    /// Pipeline c91e7f3a phase-5 test-only seam: set the text-field value
    /// programmatically and immediately emit fieldValueChanged, bypassing the
    /// UIA ValuePattern → editingFinished race. Safe to call from any slot;
    /// no-ops if the text editor is not currently the active stack page.
    void setTextValueForTesting(const QString& value);

    /// Pipeline c91e7f3a phase-7 test-only seam: read the currently displayed
    /// text value from whichever editor is on top of the stacked widget.
    /// Returns "" if no text editor is active (checkbox / combo / list / none).
    /// Used by the testFormFieldValueQueryPipe_ read-back path in MainWindow to
    /// avoid UIA FindAll traversal when reading form field values from Python.
    QString currentTextValue() const;

    /// GitHub #253. The widget index of the field currently shown, or -1 when
    /// the panel is cleared.
    ///
    /// Added so the Delete Field action on the form toolbar can resolve its
    /// target at TRIGGER time rather than MainWindow caching the selection in a
    /// member. Caching it would mean a new MainWindow member and therefore a
    /// change to that class's layout, which this codebase deliberately avoids
    /// (see the Stamp / FileAttachment / Link tool actions, all local-scope for
    /// the same reason) -- a stale .obj surviving a relink after a layout change
    /// is a failure already paid for here.
    ///
    /// Reading it live is also simply more correct: the panel is the thing that
    /// knows what is selected, so there is no second copy to fall out of date.
    [[nodiscard]] int currentWidgetIndex() const { return currentData_.widgetIndex; }

    /// GitHub #253. The type of the field currently shown. Lets a caller tell a
    /// choice field from a text field without re-querying the FormManager.
    [[nodiscard]] pdfe::forms::FormFieldType currentFieldType() const {
        return currentData_.type;
    }

signals:
    void fieldValueChanged(pdfe::forms::FormFieldData data);

private slots:
    void onTextValueChanged();
    void onMultilineValueChanged();
    void onCheckboxToggled(bool checked);
    void onComboSelectionChanged(int index);
    void onListSelectionChanged();

private:
    void buildUI();
    void updateForFieldType(pdfe::forms::FormFieldType type);
    void emitChanges();

    pdfe::forms::FormFieldData currentData_;
    bool updatingFields_ = false;

    // Field info display
    QLabel* typeLabel_ = nullptr;
    QLabel* nameLabel_ = nullptr;
    QLabel* flagsLabel_ = nullptr;

    // Value editors (stacked)
    QStackedWidget* editorStack_ = nullptr;
    QLineEdit* textEdit_ = nullptr;          // Index 0: single-line text
    QTextEdit* multilineEdit_ = nullptr;     // Index 1: multiline text
    QCheckBox* checkboxWidget_ = nullptr;    // Index 2: checkbox
    QComboBox* comboWidget_ = nullptr;       // Index 3: combobox
    QListWidget* listWidget_ = nullptr;      // Index 4: listbox
    QLabel* noEditorLabel_ = nullptr;        // Index 5: no editor (button/signature)
};

} // namespace pdfe::ui
