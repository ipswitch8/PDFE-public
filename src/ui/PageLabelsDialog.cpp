#include "PageLabelsDialog.h"
#include "../core/Document.h"
#include "../core/UndoManager.h"

#include <memory>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

namespace {

QString styleToString(pdfe::core::PageLabelStyle s)
{
    switch (s) {
        case pdfe::core::PageLabelStyle::Decimal:    return QStringLiteral("Decimal");
        case pdfe::core::PageLabelStyle::RomanUpper: return QStringLiteral("Roman (upper)");
        case pdfe::core::PageLabelStyle::RomanLower: return QStringLiteral("Roman (lower)");
        case pdfe::core::PageLabelStyle::AlphaUpper: return QStringLiteral("Alpha (upper)");
        case pdfe::core::PageLabelStyle::AlphaLower: return QStringLiteral("Alpha (lower)");
        case pdfe::core::PageLabelStyle::None:       return QStringLiteral("None");
    }
    return QStringLiteral("None");
}

} // namespace

PageLabelsDialog::PageLabelsDialog(pdfe::core::Document* doc, QWidget* parent)
    : QDialog(parent)
    , doc_(doc)
{
    setObjectName(QStringLiteral("pageLabelsDialog"));
    setWindowTitle(tr("Page Labels"));
    setAccessibleName(tr("Page Labels Editor"));
    buildUi();
    if (doc_) {
        ranges_ = pdfe::core::PageLabelManager::read(doc_);
    }
    refreshTable();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

PageLabelsDialog::~PageLabelsDialog() = default;

void PageLabelsDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);

    table_ = new QTableWidget(this);
    table_->setObjectName(QStringLiteral("pageLabelsTable"));
    table_->setAccessibleName(tr("Page label ranges table"));
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({
        tr("First Page (1-based)"),
        tr("Style"),
        tr("Prefix"),
        tr("Start"),
    });
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(table_);

    auto* btnRow = new QHBoxLayout;
    addBtn_ = new QPushButton(tr("&Add..."), this);
    addBtn_->setObjectName(QStringLiteral("pageLabelsAddButton"));
    addBtn_->setAccessibleName(tr("Add page label range"));
    editBtn_ = new QPushButton(tr("&Edit..."), this);
    editBtn_->setObjectName(QStringLiteral("pageLabelsEditButton"));
    editBtn_->setAccessibleName(tr("Edit selected page label range"));
    delBtn_ = new QPushButton(tr("&Delete"), this);
    delBtn_->setObjectName(QStringLiteral("pageLabelsDeleteButton"));
    delBtn_->setAccessibleName(tr("Delete selected page label range"));
    btnRow->addWidget(addBtn_);
    btnRow->addWidget(editBtn_);
    btnRow->addWidget(delBtn_);
    btnRow->addStretch();
    root->addLayout(btnRow);

    auto* dlgBtns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    dlgBtns->setObjectName(QStringLiteral("pageLabelsDialogButtonBox"));
    okBtn_ = dlgBtns->button(QDialogButtonBox::Ok);
    cancelBtn_ = dlgBtns->button(QDialogButtonBox::Cancel);
    if (okBtn_) okBtn_->setObjectName(QStringLiteral("pageLabelsOkButton"));
    if (cancelBtn_) cancelBtn_->setObjectName(QStringLiteral("pageLabelsCancelButton"));
    connect(dlgBtns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(dlgBtns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(dlgBtns);

    connect(addBtn_, &QPushButton::clicked, this, &PageLabelsDialog::onAdd);
    connect(editBtn_, &QPushButton::clicked, this, &PageLabelsDialog::onEdit);
    connect(delBtn_, &QPushButton::clicked, this, &PageLabelsDialog::onDelete);
}

void PageLabelsDialog::setRanges(std::vector<pdfe::core::PageLabelRange> r)
{
    ranges_ = std::move(r);
    refreshTable();
}

void PageLabelsDialog::refreshTable()
{
    if (!table_) return;
    table_->setRowCount(static_cast<int>(ranges_.size()));
    for (size_t i = 0; i < ranges_.size(); ++i) {
        const auto& r = ranges_[i];
        table_->setItem(static_cast<int>(i), 0,
            new QTableWidgetItem(QString::number(r.firstPage + 1)));
        table_->setItem(static_cast<int>(i), 1,
            new QTableWidgetItem(styleToString(r.style)));
        table_->setItem(static_cast<int>(i), 2,
            new QTableWidgetItem(QString::fromStdString(r.prefix)));
        table_->setItem(static_cast<int>(i), 3,
            new QTableWidgetItem(QString::number(r.startValue)));
    }
}

bool PageLabelsDialog::editRange(pdfe::core::PageLabelRange& inout)
{
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("pageLabelsRangeEditor"));
    dlg.setWindowTitle(tr("Page Label Range"));
    auto* form = new QFormLayout(&dlg);

    auto* firstPageSpin = new QSpinBox(&dlg);
    firstPageSpin->setObjectName(QStringLiteral("rangeFirstPageSpin"));
    firstPageSpin->setAccessibleName(
        tr("First page of label range (1-based)"));
    firstPageSpin->setRange(1,
        doc_ ? std::max(1, doc_->pageCount()) : 1);
    firstPageSpin->setValue(inout.firstPage + 1);
    form->addRow(tr("First page:"), firstPageSpin);

    auto* styleCombo = new QComboBox(&dlg);
    styleCombo->setObjectName(QStringLiteral("rangeStyleCombo"));
    styleCombo->setAccessibleName(tr("Numbering style for page label range"));
    using S = pdfe::core::PageLabelStyle;
    const std::pair<S, const char*> opts[] = {
        {S::Decimal,    "Decimal"},
        {S::RomanUpper, "Roman (upper)"},
        {S::RomanLower, "Roman (lower)"},
        {S::AlphaUpper, "Alpha (upper)"},
        {S::AlphaLower, "Alpha (lower)"},
        {S::None,       "None (prefix only)"},
    };
    int currentIdx = 0;
    for (size_t i = 0; i < std::size(opts); ++i) {
        styleCombo->addItem(tr(opts[i].second),
                            QVariant::fromValue(static_cast<int>(opts[i].first)));
        if (opts[i].first == inout.style) currentIdx = static_cast<int>(i);
    }
    styleCombo->setCurrentIndex(currentIdx);
    form->addRow(tr("Style:"), styleCombo);

    auto* prefixEdit = new QLineEdit(QString::fromStdString(inout.prefix), &dlg);
    prefixEdit->setObjectName(QStringLiteral("rangePrefixEdit"));
    prefixEdit->setAccessibleName(tr("Page label prefix string"));
    form->addRow(tr("Prefix:"), prefixEdit);

    auto* startSpin = new QSpinBox(&dlg);
    startSpin->setObjectName(QStringLiteral("rangeStartValueSpin"));
    startSpin->setAccessibleName(tr("Starting numeric value for label range"));
    startSpin->setRange(1, 1000000);
    startSpin->setValue(inout.startValue);
    form->addRow(tr("Start value:"), startSpin);

    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    btns->setObjectName(QStringLiteral("pageLabelsRangeEditorButtonBox"));
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(btns);

    if (dlg.exec() != QDialog::Accepted) return false;

    inout.firstPage  = firstPageSpin->value() - 1;
    inout.style      = static_cast<pdfe::core::PageLabelStyle>(
                           styleCombo->currentData().toInt());
    inout.prefix     = prefixEdit->text().toStdString();
    inout.startValue = startSpin->value();
    return true;
}

void PageLabelsDialog::onAdd()
{
    pdfe::core::PageLabelRange r;
    r.firstPage = 0;
    r.style = pdfe::core::PageLabelStyle::Decimal;
    r.startValue = 1;
    if (editRange(r)) {
        ranges_.push_back(r);
        refreshTable();
    }
}

void PageLabelsDialog::onEdit()
{
    int row = table_ ? table_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(ranges_.size())) return;
    auto r = ranges_[row];
    if (editRange(r)) {
        ranges_[row] = r;
        refreshTable();
    }
}

void PageLabelsDialog::onDelete()
{
    int row = table_ ? table_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(ranges_.size())) return;
    ranges_.erase(ranges_.begin() + row);
    refreshTable();
}

void PageLabelsDialog::accept()
{
    if (doc_) {
        if (undoManager_) {
            // Undoable path: dirties the undo stack (autosave fires), keeps the
            // edit undoable, and lets the close-prompt see the change.
            undoManager_->execute(
                std::make_unique<pdfe::core::EditPageLabelsCommand>(doc_, ranges_));
        } else {
            pdfe::core::PageLabelManager::write(doc_, ranges_);
        }
    }
    QDialog::accept();
}

} // namespace pdfe::ui
