#include "PrintDialog.h"
#include "core/Document.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QButtonGroup>
#include <QLabel>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

PrintDialog::PrintDialog(pdfe::core::Document* doc, int currentPage, QWidget* parent)
    : QDialog(parent)
    , document_(doc)
    , currentPageIndex_(currentPage)
{
    setupUi();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

PrintDialog::~PrintDialog() = default;

void PrintDialog::setupUi() {
    setWindowTitle("Print Document");
    setObjectName("printDialog");
    setAccessibleName(tr("Print Dialog"));

    auto* mainLayout = new QVBoxLayout(this);

    // Page Range Group
    auto* rangeGroup = new QGroupBox("Page Range", this);
    rangeGroup->setObjectName("pageRangeGroupBox");
    rangeGroup->setAccessibleName(tr("Page Range"));
    auto* rangeLayout = new QVBoxLayout(rangeGroup);

    rangeGroup_ = new QButtonGroup(this);
    rangeGroup_->setObjectName("rangeButtonGroup");

    // All Pages radio
    allPagesRadio_ = new QRadioButton("All Pages", this);
    allPagesRadio_->setObjectName("allPagesRadio");
    allPagesRadio_->setAccessibleName(tr("All Pages"));
    allPagesRadio_->setChecked(true);
    rangeGroup_->addButton(allPagesRadio_);
    rangeLayout->addWidget(allPagesRadio_);

    // Current Page radio
    currentPageRadio_ = new QRadioButton(
        QString("Current Page (%1)").arg(currentPageIndex_ + 1),
        this
    );
    currentPageRadio_->setObjectName("currentPageRadio");
    currentPageRadio_->setAccessibleName(tr("Current Page"));
    rangeGroup_->addButton(currentPageRadio_);
    rangeLayout->addWidget(currentPageRadio_);

    // Page Range radio with spinboxes
    auto* pageRangeLayout = new QHBoxLayout();
    pageRangeRadio_ = new QRadioButton("Page Range:", this);
    pageRangeRadio_->setObjectName("pageRangeRadio");
    pageRangeRadio_->setAccessibleName(tr("Page Range"));
    rangeGroup_->addButton(pageRangeRadio_);
    pageRangeLayout->addWidget(pageRangeRadio_);

    auto* fromLabel = new QLabel("From:", this);
    fromLabel->setObjectName("fromPageLabel");
    fromLabel->setAccessibleName(tr("From Page Label"));
    pageRangeLayout->addWidget(fromLabel);

    fromPageSpin_ = new QSpinBox(this);
    fromPageSpin_->setObjectName("fromPageSpinBox");
    fromPageSpin_->setAccessibleName(tr("From Page"));
    fromPageSpin_->setMinimum(1);
    fromPageSpin_->setMaximum(document_ ? document_->pageCount() : 1);
    fromPageSpin_->setValue(1);
    fromPageSpin_->setEnabled(false);
    pageRangeLayout->addWidget(fromPageSpin_);

    auto* toLabel = new QLabel("To:", this);
    toLabel->setObjectName("toPageLabel");
    toLabel->setAccessibleName(tr("To Page Label"));
    pageRangeLayout->addWidget(toLabel);

    toPageSpin_ = new QSpinBox(this);
    toPageSpin_->setObjectName("toPageSpinBox");
    toPageSpin_->setAccessibleName(tr("To Page"));
    toPageSpin_->setMinimum(1);
    toPageSpin_->setMaximum(document_ ? document_->pageCount() : 1);
    toPageSpin_->setValue(document_ ? document_->pageCount() : 1);
    toPageSpin_->setEnabled(false);
    pageRangeLayout->addWidget(toPageSpin_);

    pageRangeLayout->addStretch();
    rangeLayout->addLayout(pageRangeLayout);

    mainLayout->addWidget(rangeGroup);

    // Copies
    auto* copiesLayout = new QHBoxLayout();
    auto* copiesLabel = new QLabel("Copies:", this);
    copiesLabel->setObjectName("copiesLabel");
    copiesLabel->setAccessibleName(tr("Copies Label"));
    copiesLayout->addWidget(copiesLabel);

    copiesSpin_ = new QSpinBox(this);
    copiesSpin_->setObjectName("copiesSpinBox");
    copiesSpin_->setAccessibleName(tr("Copies"));
    copiesSpin_->setMinimum(1);
    copiesSpin_->setMaximum(999);
    copiesSpin_->setValue(1);
    copiesLayout->addWidget(copiesSpin_);

    copiesLayout->addStretch();
    mainLayout->addLayout(copiesLayout);

    // Options Group
    auto* optionsGroup = new QGroupBox("Print Options", this);
    optionsGroup->setObjectName("printOptionsGroupBox");
    optionsGroup->setAccessibleName(tr("Print Options"));
    auto* optionsLayout = new QVBoxLayout(optionsGroup);

    fitToPageCheck_ = new QCheckBox("Fit to Page", this);
    fitToPageCheck_->setObjectName("fitToPageCheckBox");
    fitToPageCheck_->setAccessibleName(tr("Fit to Page"));
    fitToPageCheck_->setChecked(true);
    optionsLayout->addWidget(fitToPageCheck_);

    centerOnPageCheck_ = new QCheckBox("Center on Page", this);
    centerOnPageCheck_->setObjectName("centerOnPageCheckBox");
    centerOnPageCheck_->setAccessibleName(tr("Center on Page"));
    centerOnPageCheck_->setChecked(true);
    optionsLayout->addWidget(centerOnPageCheck_);

    autoRotateCheck_ = new QCheckBox("Auto-Rotate Pages", this);
    autoRotateCheck_->setObjectName("autoRotateCheckBox");
    autoRotateCheck_->setAccessibleName(tr("Auto-Rotate Pages"));
    autoRotateCheck_->setChecked(true);
    optionsLayout->addWidget(autoRotateCheck_);

    mainLayout->addWidget(optionsGroup);

    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    printButton_ = new QPushButton("Print", this);
    printButton_->setObjectName("printButton");
    printButton_->setAccessibleName(tr("Print"));
    printButton_->setDefault(true);
    buttonLayout->addWidget(printButton_);

    cancelButton_ = new QPushButton("Cancel", this);
    cancelButton_->setObjectName("cancelButton");
    cancelButton_->setAccessibleName(tr("Cancel"));
    buttonLayout->addWidget(cancelButton_);

    mainLayout->addLayout(buttonLayout);

    // Connect signals
    connect(allPagesRadio_, &QRadioButton::toggled, this, &PrintDialog::onRangeChanged);
    connect(currentPageRadio_, &QRadioButton::toggled, this, &PrintDialog::onRangeChanged);
    connect(pageRangeRadio_, &QRadioButton::toggled, this, &PrintDialog::onRangeChanged);
    connect(fromPageSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &PrintDialog::validatePageRange);
    connect(toPageSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &PrintDialog::validatePageRange);
    connect(printButton_, &QPushButton::clicked, this, &PrintDialog::onPrintClicked);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);

    resize(400, 350);
}

void PrintDialog::onRangeChanged() {
    bool rangeEnabled = pageRangeRadio_->isChecked();
    fromPageSpin_->setEnabled(rangeEnabled);
    toPageSpin_->setEnabled(rangeEnabled);

    if (rangeEnabled) {
        validatePageRange();
    }
}

void PrintDialog::validatePageRange() {
    if (fromPageSpin_->value() > toPageSpin_->value()) {
        toPageSpin_->setValue(fromPageSpin_->value());
    }
}

void PrintDialog::onPrintClicked() {
    accept();
}

pdfe::core::PrintOptions PrintDialog::options() const {
    pdfe::core::PrintOptions opts;

    // Determine print range
    if (allPagesRadio_->isChecked()) {
        opts.range = pdfe::core::PrintRange::AllPages;
        opts.fromPage = 1;
        opts.toPage = document_ ? document_->pageCount() : 1;
    } else if (currentPageRadio_->isChecked()) {
        opts.range = pdfe::core::PrintRange::CurrentPage;
        opts.fromPage = currentPageIndex_ + 1; // Convert to 1-based
        opts.toPage = currentPageIndex_ + 1;
    } else if (pageRangeRadio_->isChecked()) {
        opts.range = pdfe::core::PrintRange::PageRange;
        opts.fromPage = fromPageSpin_->value();
        opts.toPage = toPageSpin_->value();
    }

    // Get other options
    opts.copies = copiesSpin_->value();
    opts.fitToPage = fitToPageCheck_->isChecked();
    opts.centerOnPage = centerOnPageCheck_->isChecked();
    opts.autoRotate = autoRotateCheck_->isChecked();

    return opts;
}

} // namespace pdfe::ui
