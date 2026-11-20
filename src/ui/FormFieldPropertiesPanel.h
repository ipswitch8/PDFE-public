#pragma once

#include <QColor>
#include <QWidget>
#include "../forms/FormFieldData.h"

#include <functional>
#include <string>

class QGroupBox;
class QPushButton;
class QSpinBox;
class QVBoxLayout;
class QLabel;
class QLineEdit;
class QTextEdit;
class QCheckBox;
class QComboBox;
class QListWidget;
class QStackedWidget;

namespace pdfe::ui {

/// GitHub #460. One committed change to a form field's font, as the
/// properties panel's font group requests it. Only `changed` names the
/// property the user changed; MainWindow takes the other two from the
/// document at the moment it issues the command, so a panel that shows a
/// stale value (after an undo, say) cannot write that stale value back.
struct FormFieldFontRequest {
    enum class Property { Family, Size, Color };
    Property changed = Property::Family;
    /// Base-14 /DA font key: "Helv", "TiRo", "Cour", "Symb" or "ZaDb".
    std::string font;
    /// /DA Tf size in points; 0 means auto-size.
    float size = 0.0f;
    /// Opaque RGB colour.
    QColor color;
};

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

    /// GitHub #460 test seam. Replaces the colour dialog the font colour
    /// button opens (QColorDialog::getColor is modal and cannot be answered
    /// headlessly). The picker receives the current colour and returns the
    /// chosen one; an invalid QColor means "cancelled". Pass an empty
    /// function to restore the real dialog.
    using ColorPicker = std::function<QColor(const QColor& initial, QWidget* parent)>;
    void setColorPickerForTesting(ColorPicker picker) { colorPicker_ = std::move(picker); }

    /// GitHub #460. The colour the font colour button currently shows.
    [[nodiscard]] QColor currentFontColor() const { return fontColor_; }

    /// GitHub #460. True for the field types whose /DA the font group edits:
    /// text, combo box and list box (the types FormManager::
    /// setFieldDefaultAppearance accepts).
    [[nodiscard]] static bool fieldTypeHasFontControls(pdfe::forms::FormFieldType type);

signals:
    void fieldValueChanged(pdfe::forms::FormFieldData data);

    /// GitHub #460. Emitted ONCE per committed font change: a family chosen
    /// in the combo, a size committed in the spin box (editingFinished, not
    /// every step) or a colour chosen in the dialog. Never emitted while the
    /// panel is being populated, never for a value equal to the one shown,
    /// and never for a read-only field (its controls are disabled).
    ///
    /// Pipeline form-reconcile: the ONE font signal. origin/master's
    /// fieldFontChanged(data, family, size, bold, italic) was folded into it
    /// (notes.md decisions 4 and 7); MainWindow::onFormFieldFontChanged is
    /// its one slot.
    void fieldFontChangeRequested(pdfe::forms::FormFieldData field,
                                  pdfe::ui::FormFieldFontRequest request);

private slots:
    void onTextValueChanged();
    void onMultilineValueChanged();
    void onCheckboxToggled(bool checked);
    void onComboSelectionChanged(int index);
    void onListSelectionChanged();
    void onFontFamilyChanged(int index);
    void onFontSizeEditingFinished();
    void onFontColorButtonClicked();

private:
    void buildUI();
    void buildFontGroup(QVBoxLayout* layout);
    void updateFontControls();
    void setFontColorSwatch(const QColor& color);
    void emitFontChange(FormFieldFontRequest::Property changed);
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

    // GitHub #460: font group (text, combo and list fields only)
    QGroupBox* fontGroup_ = nullptr;
    QComboBox* fontCombo_ = nullptr;
    QSpinBox* fontSizeSpin_ = nullptr;
    QPushButton* fontColorButton_ = nullptr;
    /// The size last shown or committed, so editingFinished (which also fires
    /// on a plain focus change, and twice for Enter then focus-out) emits only
    /// for a real change.
    int committedFontSize_ = 0;
    QColor fontColor_ = QColor(0, 0, 0);
    ColorPicker colorPicker_;
};

} // namespace pdfe::ui
