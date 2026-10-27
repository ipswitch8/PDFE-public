#include "FormFieldPropertiesPanel.h"
#include "../forms/FormFieldType.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QStackedWidget>

namespace pdfe::ui {

FormFieldPropertiesPanel::FormFieldPropertiesPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("formFieldPropertiesPanel");
    setAccessibleName(tr("Form Field Properties"));
    buildUI();
    clear();
}

FormFieldPropertiesPanel::~FormFieldPropertiesPanel() = default;

void FormFieldPropertiesPanel::buildUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* formLayout = new QFormLayout();
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // Type (read-only)
    typeLabel_ = new QLabel(tr("None"), this);
    typeLabel_->setObjectName("formFieldTypeLabel");
    typeLabel_->setAccessibleName(tr("Form Field Type"));
    formLayout->addRow(tr("Type:"), typeLabel_);

    // Name (read-only)
    nameLabel_ = new QLabel(this);
    nameLabel_->setObjectName("formFieldNameLabel");
    nameLabel_->setAccessibleName(tr("Form Field Name"));
    // setFormField() fills this with the field's /T name from the PDF, which is
    // attacker-controlled. Same hazard as the sticky-note title bar and the
    // metadata dialog's read-only labels: QLabel's default Qt::AutoText renders
    // crafted markup as HTML, and a rich-text QLabel resolves <img src=...>.
    // Selecting a form field in a malicious PDF is enough to reach it.
    nameLabel_->setTextFormat(Qt::PlainText);
    nameLabel_->setWordWrap(true);
    formLayout->addRow(tr("Name:"), nameLabel_);

    // Flags (read-only)
    flagsLabel_ = new QLabel(this);
    flagsLabel_->setObjectName("formFieldFlagsLabel");
    flagsLabel_->setAccessibleName(tr("Form Field Flags"));
    flagsLabel_->setWordWrap(true);
    formLayout->addRow(tr("Flags:"), flagsLabel_);

    layout->addLayout(formLayout);

    // Value label
    auto* valueLabel = new QLabel(tr("Value:"), this);
    layout->addWidget(valueLabel);

    // Stacked editor widget
    editorStack_ = new QStackedWidget(this);
    editorStack_->setObjectName("formFieldEditorStack");

    // Index 0: Single-line text editor
    textEdit_ = new QLineEdit(this);
    textEdit_->setObjectName("formFieldTextEdit");
    textEdit_->setAccessibleName(tr("Form Field Text Value"));
    connect(textEdit_, &QLineEdit::editingFinished,
            this, &FormFieldPropertiesPanel::onTextValueChanged);
    editorStack_->addWidget(textEdit_);

    // Index 1: Multiline text editor
    multilineEdit_ = new QTextEdit(this);
    multilineEdit_->setObjectName("formFieldMultilineEdit");
    multilineEdit_->setAccessibleName(tr("Form Field Multiline Value"));
    multilineEdit_->setMaximumHeight(100);
    connect(multilineEdit_, &QTextEdit::textChanged,
            this, &FormFieldPropertiesPanel::onMultilineValueChanged);
    editorStack_->addWidget(multilineEdit_);

    // Index 2: Checkbox
    checkboxWidget_ = new QCheckBox(tr("Checked"), this);
    checkboxWidget_->setObjectName("formFieldCheckbox");
    checkboxWidget_->setAccessibleName(tr("Form Field Checkbox Value"));
    connect(checkboxWidget_, &QCheckBox::toggled,
            this, &FormFieldPropertiesPanel::onCheckboxToggled);
    editorStack_->addWidget(checkboxWidget_);

    // Index 3: ComboBox
    comboWidget_ = new QComboBox(this);
    comboWidget_->setObjectName("formFieldCombo");
    comboWidget_->setAccessibleName(tr("Form Field Combo Value"));
    connect(comboWidget_, &QComboBox::currentIndexChanged,
            this, &FormFieldPropertiesPanel::onComboSelectionChanged);
    editorStack_->addWidget(comboWidget_);

    // Index 4: ListWidget
    listWidget_ = new QListWidget(this);
    listWidget_->setObjectName("formFieldList");
    listWidget_->setAccessibleName(tr("Form Field List Value"));
    listWidget_->setMaximumHeight(120);
    connect(listWidget_, &QListWidget::itemSelectionChanged,
            this, &FormFieldPropertiesPanel::onListSelectionChanged);
    editorStack_->addWidget(listWidget_);

    // Index 5: No editor (for button/signature/unknown)
    noEditorLabel_ = new QLabel(tr("No editable value"), this);
    noEditorLabel_->setObjectName("formFieldNoEditor");
    noEditorLabel_->setAlignment(Qt::AlignCenter);
    editorStack_->addWidget(noEditorLabel_);

    layout->addWidget(editorStack_);
    layout->addStretch();
}

void FormFieldPropertiesPanel::setFormField(const pdfe::forms::FormFieldData& data) {
    updatingFields_ = true;
    currentData_ = data;

    // Update info labels
    QString typeName = QString::fromStdString(
        pdfe::forms::formFieldTypeName(data.type));
    typeLabel_->setText(typeName);
    nameLabel_->setText(QString::fromStdString(data.fieldName));

    // Build flags string
    QStringList flagsList;
    if (data.isReadOnly) flagsList << tr("Read-Only");
    if (data.isRequired) flagsList << tr("Required");
    if (data.isMultiline) flagsList << tr("Multiline");
    if (data.isPassword) flagsList << tr("Password");
    if (data.isComb) flagsList << tr("Comb");
    if (data.isEditable) flagsList << tr("Editable");
    if (data.isMultiSelect) flagsList << tr("Multi-Select");
    flagsLabel_->setText(flagsList.isEmpty() ? tr("None") : flagsList.join(", "));

    // Update the stacked editor
    updateForFieldType(data.type);

    updatingFields_ = false;
}

void FormFieldPropertiesPanel::updateForFieldType(pdfe::forms::FormFieldType type) {
    using FT = pdfe::forms::FormFieldType;

    switch (type) {
        case FT::Text:
            if (currentData_.isMultiline) {
                editorStack_->setCurrentIndex(1);
                multilineEdit_->setPlainText(QString::fromStdString(currentData_.value));
                multilineEdit_->setReadOnly(currentData_.isReadOnly);
            } else {
                editorStack_->setCurrentIndex(0);
                textEdit_->setText(QString::fromStdString(currentData_.value));
                textEdit_->setReadOnly(currentData_.isReadOnly);
                if (currentData_.isPassword) {
                    textEdit_->setEchoMode(QLineEdit::Password);
                } else {
                    textEdit_->setEchoMode(QLineEdit::Normal);
                }
            }
            break;

        case FT::Checkbox:
        case FT::RadioButton:
            editorStack_->setCurrentIndex(2);
            checkboxWidget_->setChecked(
                !currentData_.value.empty() &&
                currentData_.value != "Off" &&
                currentData_.value != "No");
            checkboxWidget_->setEnabled(!currentData_.isReadOnly);
            if (type == FT::RadioButton) {
                checkboxWidget_->setText(tr("Selected"));
            } else {
                checkboxWidget_->setText(tr("Checked"));
            }
            break;

        case FT::ComboBox:
            editorStack_->setCurrentIndex(3);
            comboWidget_->clear();
            for (const auto& opt : currentData_.options) {
                comboWidget_->addItem(QString::fromStdString(opt));
            }
            // Select current value
            {
                int idx = comboWidget_->findText(QString::fromStdString(currentData_.value));
                if (idx >= 0) {
                    comboWidget_->setCurrentIndex(idx);
                }
            }
            comboWidget_->setEnabled(!currentData_.isReadOnly);
            break;

        case FT::ListBox:
            editorStack_->setCurrentIndex(4);
            listWidget_->clear();
            for (const auto& opt : currentData_.options) {
                listWidget_->addItem(QString::fromStdString(opt));
            }
            if (currentData_.isMultiSelect) {
                listWidget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
            } else {
                listWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
            }
            // Select current items
            for (int idx : currentData_.selectedIndices) {
                if (idx >= 0 && idx < listWidget_->count()) {
                    listWidget_->item(idx)->setSelected(true);
                }
            }
            listWidget_->setEnabled(!currentData_.isReadOnly);
            break;

        default:
            editorStack_->setCurrentIndex(5);
            break;
    }
}

void FormFieldPropertiesPanel::clear() {
    updatingFields_ = true;

    currentData_ = pdfe::forms::FormFieldData();
    typeLabel_->setText(tr("None"));
    nameLabel_->clear();
    flagsLabel_->setText(tr("None"));
    textEdit_->clear();
    multilineEdit_->clear();
    checkboxWidget_->setChecked(false);
    comboWidget_->clear();
    listWidget_->clear();
    editorStack_->setCurrentIndex(5);

    updatingFields_ = false;
}

void FormFieldPropertiesPanel::onTextValueChanged() {
    // Guard: only emit when the single-line editor is the active stack page.
    // editingFinished can fire after focus loss even after clear()/setFormField()
    // has already switched the stack to a different page — that late delivery
    // would overwrite a previously-committed value with an empty string.
    if (!updatingFields_ && editorStack_->currentIndex() == 0) {
        currentData_.value = textEdit_->text().toStdString();
        emitChanges();
    }
}

void FormFieldPropertiesPanel::onMultilineValueChanged() {
    if (!updatingFields_) {
        currentData_.value = multilineEdit_->toPlainText().toStdString();
        emitChanges();
    }
}

void FormFieldPropertiesPanel::onCheckboxToggled(bool checked) {
    if (!updatingFields_) {
        currentData_.value = checked ? "Yes" : "Off";
        emitChanges();
    }
}

void FormFieldPropertiesPanel::onComboSelectionChanged(int index) {
    if (!updatingFields_ && index >= 0) {
        currentData_.value = comboWidget_->currentText().toStdString();
        currentData_.selectedIndices = {index};
        emitChanges();
    }
}

void FormFieldPropertiesPanel::onListSelectionChanged() {
    if (!updatingFields_) {
        currentData_.selectedIndices.clear();
        auto selectedItems = listWidget_->selectionModel()->selectedIndexes();
        for (const auto& idx : selectedItems) {
            currentData_.selectedIndices.push_back(idx.row());
        }
        if (!currentData_.selectedIndices.empty()) {
            currentData_.value = listWidget_->item(
                currentData_.selectedIndices[0])->text().toStdString();
        }
        emitChanges();
    }
}

void FormFieldPropertiesPanel::emitChanges() {
    if (!updatingFields_) {
        emit fieldValueChanged(currentData_);
    }
}

void FormFieldPropertiesPanel::setTextValueForTesting(const QString& value)
{
    if (updatingFields_) return;

    const int idx = editorStack_->currentIndex();

    if (idx == 0) {
        // Single-line text editor: set text and fire slot (bypasses
        // the editingFinished UIA race).
        textEdit_->setText(value);
        onTextValueChanged();
    } else if (idx == 1) {
        // Multiline text editor: set plain text and fire slot (QTextEdit
        // fires textChanged on every change, but driving via the test seam
        // is cleaner and avoids partial-word commits from typewrite).
        multilineEdit_->setPlainText(value);
        onMultilineValueChanged();
    }
    // All other stack pages (checkbox, combo, list, no-editor) are not
    // handled by this text-oriented seam.
}

QString FormFieldPropertiesPanel::currentTextValue() const
{
    if (!editorStack_) return QString();
    const int idx = editorStack_->currentIndex();
    if (idx == 0 && textEdit_) {
        return textEdit_->text();
    }
    if (idx == 1 && multilineEdit_) {
        return multilineEdit_->toPlainText();
    }
    // Checkbox (2), combo (3), list (4), no-editor (5) — return empty.
    return QString();
}

} // namespace pdfe::ui
