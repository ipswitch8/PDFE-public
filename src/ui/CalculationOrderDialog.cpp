// -*- coding: utf-8 -*-
#include "CalculationOrderDialog.h"
#include "forms/FormManager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

CalculationOrderDialog::CalculationOrderDialog(
    pdfe::forms::FormManager* formManager,
    pdfe::core::Document* doc,
    QWidget* parent)
    : QDialog(parent)
    , formManager_(formManager)
    , doc_(doc)
{
    setObjectName("calculationOrderDialog");
    setAccessibleName("Calculation Order");
    setWindowTitle("Form Field Calculation Order");
    setMinimumSize(460, 380);

    auto* mainLayout = new QVBoxLayout(this);

    auto* label = new QLabel(
        "Check fields that should be evaluated in calculation order. "
        "Drag rows or use the Up/Down buttons to reorder.", this);
    label->setObjectName("calculationOrderLabel");
    label->setAccessibleName("Calculation order instructions");
    label->setWordWrap(true);
    mainLayout->addWidget(label);

    auto* contentLayout = new QHBoxLayout;

    fieldList_ = new QListWidget(this);
    fieldList_->setObjectName("calculationOrderFieldList");
    fieldList_->setAccessibleName("Form fields calculation order list");
    fieldList_->setDragDropMode(QAbstractItemView::InternalMove);
    fieldList_->setDefaultDropAction(Qt::MoveAction);
    contentLayout->addWidget(fieldList_, 1);

    auto* buttonLayout = new QVBoxLayout;
    buttonLayout->addStretch();

    moveUpButton_ = new QPushButton("Move &Up", this);
    moveUpButton_->setObjectName("calculationOrderMoveUpButton");
    moveUpButton_->setAccessibleName(tr("Move selected field up"));
    connect(moveUpButton_, &QPushButton::clicked,
            this, &CalculationOrderDialog::onMoveUp);
    buttonLayout->addWidget(moveUpButton_);

    moveDownButton_ = new QPushButton("Move &Down", this);
    moveDownButton_->setObjectName("calculationOrderMoveDownButton");
    moveDownButton_->setAccessibleName(tr("Move selected field down"));
    connect(moveDownButton_, &QPushButton::clicked,
            this, &CalculationOrderDialog::onMoveDown);
    buttonLayout->addWidget(moveDownButton_);

    buttonLayout->addStretch();
    contentLayout->addLayout(buttonLayout);
    mainLayout->addLayout(contentLayout, 1);

    auto* bottomRow = new QHBoxLayout;
    bottomRow->addStretch(1);
    applyButton_ = new QPushButton("Apply", this);
    applyButton_->setObjectName("calculationOrderApplyButton");
    applyButton_->setAccessibleName(tr("Apply calculation order"));
    applyButton_->setDefault(true);
    connect(applyButton_, &QPushButton::clicked,
            this, &CalculationOrderDialog::onApply);
    bottomRow->addWidget(applyButton_);
    cancelButton_ = new QPushButton("Cancel", this);
    cancelButton_->setObjectName("calculationOrderCancelButton");
    cancelButton_->setAccessibleName(tr("Cancel calculation order changes"));
    connect(cancelButton_, &QPushButton::clicked,
            this, &QDialog::reject);
    bottomRow->addWidget(cancelButton_);
    mainLayout->addLayout(bottomRow);

    populateList();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

CalculationOrderDialog::~CalculationOrderDialog() = default;

void CalculationOrderDialog::populateList()
{
    if (!formManager_ || !doc_) return;
    const auto allNames = formManager_->listFieldNames(doc_);
    const auto currentCO = formManager_->getCalculationOrder(doc_);

    std::set<std::string> inCO(currentCO.begin(), currentCO.end());

    // Show currentCO entries first (in their existing order),
    // then any remaining field names unchecked.
    for (const auto& name : currentCO) {
        auto* item = new QListWidgetItem(QString::fromStdString(name), fieldList_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setData(Qt::UserRole, QString::fromStdString(name));
    }
    for (const auto& name : allNames) {
        if (inCO.count(name)) continue;
        auto* item = new QListWidgetItem(QString::fromStdString(name), fieldList_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(Qt::UserRole, QString::fromStdString(name));
    }
}

std::vector<std::string> CalculationOrderDialog::newCalculationOrder() const
{
    std::vector<std::string> out;
    for (int i = 0; i < fieldList_->count(); ++i) {
        QListWidgetItem* item = fieldList_->item(i);
        if (item->checkState() != Qt::Checked) continue;
        out.push_back(item->data(Qt::UserRole).toString().toStdString());
    }
    return out;
}

void CalculationOrderDialog::onMoveUp()
{
    int row = fieldList_->currentRow();
    if (row <= 0) return;
    QListWidgetItem* item = fieldList_->takeItem(row);
    fieldList_->insertItem(row - 1, item);
    fieldList_->setCurrentRow(row - 1);
}

void CalculationOrderDialog::onMoveDown()
{
    int row = fieldList_->currentRow();
    if (row < 0 || row >= fieldList_->count() - 1) return;
    QListWidgetItem* item = fieldList_->takeItem(row);
    fieldList_->insertItem(row + 1, item);
    fieldList_->setCurrentRow(row + 1);
}

bool CalculationOrderDialog::moveProgrammatic(int fromIndex, int toIndex)
{
    // Pipeline 3e9b1d7c phase-3 (C3 / P-GAP-95): programmatic
    // reorder seam for tests. Same model-side effect as Move Up
    // or Move Down button clicks repeated until item is at target.
    if (!fieldList_) return false;
    const int n = fieldList_->count();
    if (fromIndex < 0 || fromIndex >= n) return false;
    if (toIndex < 0 || toIndex >= n) return false;
    if (fromIndex == toIndex) return false;
    QListWidgetItem* item = fieldList_->takeItem(fromIndex);
    fieldList_->insertItem(toIndex, item);
    fieldList_->setCurrentRow(toIndex);
    return true;
}

void CalculationOrderDialog::onApply()
{
    if (formManager_ && doc_) {
        formManager_->setCalculationOrder(doc_, newCalculationOrder());
    }
    accept();
}

} // namespace pdfe::ui
