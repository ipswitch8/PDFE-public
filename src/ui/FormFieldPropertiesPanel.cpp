#include "FormFieldPropertiesPanel.h"
#include "../forms/FormFieldType.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QColorDialog>
#include <QGroupBox>
#include <QPushButton>
#include <QSpinBox>

#include <algorithm>
#include <cmath>
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

FormFieldPropertiesPanel::~FormFieldPropertiesPanel() {
    // Destruction-order focus-out hazard: when this panel is destroyed while a
    // child editor (e.g. textEdit_) holds focus, the base ~QWidget() synthesizes
    // a focus-out that fires QLineEdit::editingFinished -> onTextValueChanged.
    // By then this object's dynamic type is already QWidget, so Qt's
    // assertObjectType<FormFieldPropertiesPanel> dynamic_cast on the receiver
    // fails and aborts (qobjectdefs_impl.h). Break every child-widget signal
    // whose RECEIVER is this panel BEFORE the base destructor runs so no member
    // slot can fire during teardown. disconnect(sender, 0, this, 0) is the
    // receiver-side form (disconnect(this) would treat this as the SENDER).
    // Every child below is created in buildUI() from the constructor, so each
    // is non-null by the time this panel can be destroyed.
    disconnect(textEdit_, nullptr, this, nullptr);
    disconnect(multilineEdit_, nullptr, this, nullptr);
    disconnect(checkboxWidget_, nullptr, this, nullptr);
    disconnect(comboWidget_, nullptr, this, nullptr);
    disconnect(listWidget_, nullptr, this, nullptr);
    disconnect(fontCombo_, nullptr, this, nullptr);
    disconnect(fontSizeSpin_, nullptr, this, nullptr);
    disconnect(fontColorButton_, nullptr, this, nullptr);
}

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

    buildFontGroup(layout);

    layout->addStretch();
}

// ─── GitHub #460: font group ─────────────────────────────────────────────────
//
// The family list is limited to the base-14 fonts FormManager::
// setFieldDefaultAppearance accepts and MuPDF 1.26.10 resolves without a /DR
// entry. Helvetica, Times and Courier are always offered. Symbol and
// ZapfDingbats are NOT offered for selection: they are symbol fonts, and text
// typed into a field set in either one comes out as Greek letters or
// dingbats. They are added to the list only while the selected field already
// uses one (so the combo shows the truth, and a size or colour change does not
// quietly switch that field to Helvetica).
namespace {

struct FontChoice {
    const char* key;
    const char* label;
};

constexpr FontChoice kSelectableFonts[] = {
    {"Helv", QT_TRANSLATE_NOOP("FormFieldPropertiesPanel", "Helvetica")},
    {"TiRo", QT_TRANSLATE_NOOP("FormFieldPropertiesPanel", "Times")},
    {"Cour", QT_TRANSLATE_NOOP("FormFieldPropertiesPanel", "Courier")},
};
constexpr int kSelectableFontCount =
    static_cast<int>(sizeof(kSelectableFonts) / sizeof(kSelectableFonts[0]));

constexpr FontChoice kSymbolFonts[] = {
    {"Symb", QT_TRANSLATE_NOOP("FormFieldPropertiesPanel", "Symbol")},
    {"ZaDb", QT_TRANSLATE_NOOP("FormFieldPropertiesPanel", "ZapfDingbats")},
};

/// The /DA colour as an opaque RGB QColor. Components come from the PDF via
/// FormManager's read sanitisers; they are clamped again here so a
/// hand-built FormFieldData cannot produce an out-of-range QColor.
QColor daColorToQColor(const pdfe::forms::FormFieldData& d) {
    using pdfe::forms::sanitizeColorComponent;
    const auto c = [&d](int i) { return sanitizeColorComponent(d.daColor[i]); };
    switch (pdfe::forms::sanitizeColorComponentCount(d.daColorComponents)) {
        case 1:
            return QColor::fromRgbF(c(0), c(0), c(0));
        case 3:
            return QColor::fromRgbF(c(0), c(1), c(2));
        case 4:
            return QColor::fromCmykF(c(0), c(1), c(2), c(3)).toRgb();
        default:
            return QColor(0, 0, 0);
    }
}

}  // namespace

bool FormFieldPropertiesPanel::fieldTypeHasFontControls(pdfe::forms::FormFieldType type) {
    using FT = pdfe::forms::FormFieldType;
    return type == FT::Text || type == FT::ComboBox || type == FT::ListBox;
}

void FormFieldPropertiesPanel::buildFontGroup(QVBoxLayout* layout) {
    fontGroup_ = new QGroupBox(tr("Font"), this);
    fontGroup_->setObjectName("formFieldFontGroup");
    fontGroup_->setAccessibleName(tr("Form Field Font Settings"));
    auto* fontLayout = new QFormLayout(fontGroup_);
    fontLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    fontCombo_ = new QComboBox(fontGroup_);
    fontCombo_->setObjectName("formFieldFontCombo");
    fontCombo_->setAccessibleName(tr("Form Field Font"));
    fontCombo_->setAccessibleDescription(
        tr("Font used to draw the text of the selected form field"));
    for (const auto& f : kSelectableFonts) {
        fontCombo_->addItem(tr(f.label), QString::fromLatin1(f.key));
    }
    fontLayout->addRow(tr("Family:"), fontCombo_);

    fontSizeSpin_ = new QSpinBox(fontGroup_);
    fontSizeSpin_->setObjectName("formFieldFontSizeSpin");
    fontSizeSpin_->setAccessibleName(tr("Form Field Font Size"));
    fontSizeSpin_->setAccessibleDescription(
        tr("Font size in points; Auto fits the text to the field"));
    // 0 is auto-size (PDF 32000-1 12.7.3.3); 1..1000 is exactly the range
    // FormManager::setFieldDefaultAppearance accepts, so the spin box cannot
    // produce a size the setter would reject.
    fontSizeSpin_->setRange(0, static_cast<int>(pdfe::forms::kMaxDefaultAppearanceSizePt));
    fontSizeSpin_->setSpecialValueText(tr("Auto"));
    fontSizeSpin_->setSuffix(tr(" pt"));
    // One command per COMMITTED size: editingFinished (Enter or focus out),
    // never per arrow step or keystroke. SetFieldAppearanceCommand never
    // merges, so per-step commands would be per-step undo entries.
    fontSizeSpin_->setKeyboardTracking(false);
    fontLayout->addRow(tr("Size:"), fontSizeSpin_);

    fontColorButton_ = new QPushButton(fontGroup_);
    fontColorButton_->setObjectName("formFieldFontColorButton");
    fontColorButton_->setAccessibleName(tr("Form Field Font Color"));
    fontColorButton_->setAccessibleDescription(
        tr("Choose the colour of the selected form field's text"));
    fontColorButton_->setFixedSize(40, 22);
    fontLayout->addRow(tr("Color:"), fontColorButton_);

    connect(fontCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FormFieldPropertiesPanel::onFontFamilyChanged);
    connect(fontSizeSpin_, &QSpinBox::editingFinished,
            this, &FormFieldPropertiesPanel::onFontSizeEditingFinished);
    connect(fontColorButton_, &QPushButton::clicked,
            this, &FormFieldPropertiesPanel::onFontColorButtonClicked);

    setFontColorSwatch(fontColor_);
    fontGroup_->setVisible(false);
    layout->addWidget(fontGroup_);
}

void FormFieldPropertiesPanel::setFontColorSwatch(const QColor& color) {
    fontColor_ = color;
    if (!fontColorButton_) return;
    // name() is "#rrggbb" built from the colour's own components, so nothing
    // from the PDF reaches the style sheet as text.
    fontColorButton_->setStyleSheet(
        QStringLiteral("QPushButton { background-color: %1; }").arg(color.name()));
    fontColorButton_->setToolTip(color.name());
}

void FormFieldPropertiesPanel::updateFontControls() {
    // Called only with updatingFields_ set, so nothing below emits.
    const bool show = fieldTypeHasFontControls(currentData_.type);
    fontGroup_->setVisible(show);

    // Drop any symbol-font entry a previous field added.
    while (fontCombo_->count() > kSelectableFontCount) {
        fontCombo_->removeItem(fontCombo_->count() - 1);
    }
    if (!show) return;

    const QString key = QString::fromStdString(currentData_.daFontName);
    int idx = fontCombo_->findData(key);
    if (idx < 0) {
        for (const auto& f : kSymbolFonts) {
            if (key == QLatin1String(f.key)) {
                fontCombo_->addItem(tr(f.label), QString::fromLatin1(f.key));
                idx = fontCombo_->count() - 1;
                break;
            }
        }
    }
    // FormManager maps every unknown name to "Helv", so idx < 0 means a
    // hand-built FormFieldData; show Helvetica, MuPDF's own fallback.
    fontCombo_->setCurrentIndex(idx >= 0 ? idx : 0);

    // origin/master 596d613f: a read-only field's font is not user-editable,
    // so its controls are shown (they report the font) but disabled.
    const bool editable = !currentData_.isReadOnly;
    fontCombo_->setEnabled(editable);
    fontSizeSpin_->setEnabled(editable);
    fontColorButton_->setEnabled(editable);

    const float size = pdfe::forms::sanitizeDefaultAppearanceSize(currentData_.daFontSize);
    // A fractional size (10.5) is shown rounded. It is only rewritten if the
    // user commits a different size: MainWindow takes the size it writes for
    // a family or colour change from the document, not from this spin box.
    const int shown = std::clamp(static_cast<int>(std::lround(size)), 0,
                                 static_cast<int>(pdfe::forms::kMaxDefaultAppearanceSizePt));
    fontSizeSpin_->setValue(shown);
    committedFontSize_ = shown;

    setFontColorSwatch(daColorToQColor(currentData_));
}

void FormFieldPropertiesPanel::emitFontChange(FormFieldFontRequest::Property changed) {
    if (updatingFields_ || !fieldTypeHasFontControls(currentData_.type) ||
        currentData_.widgetIndex < 0 || currentData_.isReadOnly) {
        return;
    }
    FormFieldFontRequest req;
    req.changed = changed;
    req.font = fontCombo_->currentData().toString().toStdString();
    req.size = static_cast<float>(committedFontSize_);
    req.color = fontColor_;
    emit fieldFontChangeRequested(currentData_, req);
}

void FormFieldPropertiesPanel::onFontFamilyChanged(int index) {
    if (updatingFields_ || index < 0) return;
    const std::string key = fontCombo_->itemData(index).toString().toStdString();
    if (key == currentData_.daFontName) return;
    currentData_.daFontName = key;
    emitFontChange(FormFieldFontRequest::Property::Family);
}

void FormFieldPropertiesPanel::onFontSizeEditingFinished() {
    if (updatingFields_) return;
    const int value = fontSizeSpin_->value();
    if (value == committedFontSize_) return;
    committedFontSize_ = value;
    currentData_.daFontSize = static_cast<float>(value);
    emitFontChange(FormFieldFontRequest::Property::Size);
}

void FormFieldPropertiesPanel::onFontColorButtonClicked() {
    if (updatingFields_ || !fieldTypeHasFontControls(currentData_.type) ||
        currentData_.isReadOnly) {
        return;
    }
    const QColor initial = fontColor_;
    const QColor chosen = colorPicker_
        ? colorPicker_(initial, this)
        : QColorDialog::getColor(initial, this, tr("Form Field Font Color"));
    // Invalid = the dialog was cancelled. The dialog is opened without
    // ShowAlphaChannel so it returns opaque colours; anything translucent
    // (only reachable through the test seam) is refused rather than silently
    // made opaque, because the /DA has no alpha and the setter rejects it.
    if (!chosen.isValid() || chosen.alpha() != 255) return;
    const QColor rgb = chosen.toRgb();
    if (rgb == initial) return;
    setFontColorSwatch(rgb);
    currentData_.daColorComponents = 3;
    currentData_.daColor = {static_cast<float>(rgb.redF()), static_cast<float>(rgb.greenF()),
                            static_cast<float>(rgb.blueF()), 0.0f};
    emitFontChange(FormFieldFontRequest::Property::Color);
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

    // GitHub #460: the font group, from the field's default appearance.
    updateFontControls();

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
    updateFontControls();  // GitHub #460: Unknown type, so the group hides

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
