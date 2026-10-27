#include "TabOrderDialog.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "core/Page.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

TabOrderDialog::TabOrderDialog(pdfe::forms::FormManager* formManager,
                                 pdfe::core::Page* page,
                                 QWidget* parent)
    : QDialog(parent)
    , formManager_(formManager)
    , page_(page)
{
    setObjectName("tabOrderDialog");
    setAccessibleName("Tab Order");
    setWindowTitle("Form Field Tab Order");
    setMinimumSize(400, 350);

    auto* mainLayout = new QVBoxLayout(this);

    auto* label = new QLabel("Drag fields or use buttons to reorder:");
    label->setObjectName("tabOrderLabel");
    label->setAccessibleName("Tab order instructions");
    mainLayout->addWidget(label);

    auto* contentLayout = new QHBoxLayout;

    fieldList_ = new QListWidget;
    fieldList_->setObjectName("tabOrderFieldList");
    fieldList_->setAccessibleName("Form fields tab order list");
    fieldList_->setDragDropMode(QAbstractItemView::InternalMove);
    fieldList_->setDefaultDropAction(Qt::MoveAction);
    contentLayout->addWidget(fieldList_, 1);

    auto* buttonLayout = new QVBoxLayout;
    buttonLayout->addStretch();

    moveUpButton_ = new QPushButton("Move &Up");
    moveUpButton_->setObjectName("moveUpButton");
    moveUpButton_->setAccessibleName("Move field up");
    connect(moveUpButton_, &QPushButton::clicked, this, &TabOrderDialog::onMoveUp);
    buttonLayout->addWidget(moveUpButton_);

    moveDownButton_ = new QPushButton("Move &Down");
    moveDownButton_->setObjectName("moveDownButton");
    moveDownButton_->setAccessibleName("Move field down");
    connect(moveDownButton_, &QPushButton::clicked, this, &TabOrderDialog::onMoveDown);
    buttonLayout->addWidget(moveDownButton_);

    buttonLayout->addStretch();
    contentLayout->addLayout(buttonLayout);
    mainLayout->addLayout(contentLayout);

    auto* bottomLayout = new QHBoxLayout;
    bottomLayout->addStretch();

    applyButton_ = new QPushButton("&Apply");
    applyButton_->setObjectName("applyButton");
    applyButton_->setAccessibleName("Apply tab order");
    connect(applyButton_, &QPushButton::clicked, this, &TabOrderDialog::onApply);
    bottomLayout->addWidget(applyButton_);

    cancelButton_ = new QPushButton("&Cancel");
    cancelButton_->setObjectName("cancelButton");
    cancelButton_->setAccessibleName("Cancel");
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    bottomLayout->addWidget(cancelButton_);

    mainLayout->addLayout(bottomLayout);

    populateList();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

TabOrderDialog::~TabOrderDialog() = default;

void TabOrderDialog::populateList()
{
    fieldList_->clear();
    widgetIndices_.clear();

    if (!formManager_ || !page_) return;

    auto tabOrder = formManager_->getTabOrder(page_);
    auto fields = formManager_->getFormFields(page_);

    for (int idx : tabOrder) {
        if (idx < 0 || idx >= static_cast<int>(fields.size())) continue;

        const auto& field = fields[idx];
        QString displayName = QString::fromStdString(field.fieldName);
        if (displayName.isEmpty()) {
            displayName = QString("Field %1").arg(idx);
        }

        // Add type suffix
        switch (field.type) {
        case pdfe::forms::FormFieldType::Text:
            displayName += " [Text]";
            break;
        case pdfe::forms::FormFieldType::Checkbox:
            displayName += " [Checkbox]";
            break;
        case pdfe::forms::FormFieldType::RadioButton:
            displayName += " [Radio]";
            break;
        case pdfe::forms::FormFieldType::ComboBox:
            displayName += " [Combo]";
            break;
        case pdfe::forms::FormFieldType::ListBox:
            displayName += " [List]";
            break;
        case pdfe::forms::FormFieldType::PushButton:
            displayName += " [Button]";
            break;
        case pdfe::forms::FormFieldType::Signature:
            displayName += " [Signature]";
            break;
        default:
            break;
        }

        auto* item = new QListWidgetItem(displayName);
        item->setData(Qt::UserRole, idx);
        fieldList_->addItem(item);
        widgetIndices_.push_back(idx);
    }
}

void TabOrderDialog::onMoveUp()
{
    int row = fieldList_->currentRow();
    if (row <= 0) return;

    auto* item = fieldList_->takeItem(row);
    fieldList_->insertItem(row - 1, item);
    fieldList_->setCurrentRow(row - 1);

    std::swap(widgetIndices_[row], widgetIndices_[row - 1]);
}

void TabOrderDialog::onMoveDown()
{
    int row = fieldList_->currentRow();
    if (row < 0 || row >= fieldList_->count() - 1) return;

    auto* item = fieldList_->takeItem(row);
    fieldList_->insertItem(row + 1, item);
    fieldList_->setCurrentRow(row + 1);

    std::swap(widgetIndices_[row], widgetIndices_[row + 1]);
}

bool TabOrderDialog::moveProgrammatic(int fromIndex, int toIndex)
{
    // Pipeline a7c4f891 phase-1 (A3): mirrors
    // CalculationOrderDialog::moveProgrammatic. Same model-side
    // effect as Move Up or Move Down clicked repeatedly until the
    // item lands at toIndex. Both the QListWidget row order AND
    // the parallel widgetIndices_ vector must be updated together
    // — they are the canonical save-side state.
    if (!fieldList_) return false;
    const int n = fieldList_->count();
    if (fromIndex < 0 || fromIndex >= n) return false;
    if (toIndex < 0 || toIndex >= n) return false;
    if (fromIndex == toIndex) return false;
    if (static_cast<int>(widgetIndices_.size()) != n) return false;

    auto* item = fieldList_->takeItem(fromIndex);
    fieldList_->insertItem(toIndex, item);
    fieldList_->setCurrentRow(toIndex);

    // Shift widgetIndices_ in lock-step with the QListWidget.
    int idx = widgetIndices_[fromIndex];
    widgetIndices_.erase(widgetIndices_.begin() + fromIndex);
    widgetIndices_.insert(widgetIndices_.begin() + toIndex, idx);

    return true;
}

void TabOrderDialog::onApply()
{
    // Rebuild widgetIndices_ from current visual order
    // (drag-drop may have reordered without going through onMoveUp/Down)
    widgetIndices_.clear();
    for (int row = 0; row < fieldList_->count(); ++row) {
        widgetIndices_.push_back(
            fieldList_->item(row)->data(Qt::UserRole).toInt());
    }
    accept();
}

std::vector<int> TabOrderDialog::newTabOrder() const
{
    return widgetIndices_;
}

} // namespace pdfe::ui
