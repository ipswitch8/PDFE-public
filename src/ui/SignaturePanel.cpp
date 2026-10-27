#include "SignaturePanel.h"
#include "signatures/SignatureData.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QHeaderView>

namespace pdfe::ui {

SignaturePanel::SignaturePanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("signaturePanel");
    setAccessibleName(tr("Signature Panel"));

    // Create tree widget
    signatureTreeWidget_ = new QTreeWidget(this);
    signatureTreeWidget_->setObjectName("signatureTreeWidget");
    signatureTreeWidget_->setAccessibleName(tr("Signatures List"));
    signatureTreeWidget_->setColumnCount(4);
    signatureTreeWidget_->setHeaderLabels({"Field Name", "Signer", "Page", "Status"});
    signatureTreeWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
    signatureTreeWidget_->setAlternatingRowColors(true);
    signatureTreeWidget_->header()->setStretchLastSection(false);
    signatureTreeWidget_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    signatureTreeWidget_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    signatureTreeWidget_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    signatureTreeWidget_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    // Create buttons
    verifyAllButton_ = new QPushButton("Verify All", this);
    verifyAllButton_->setObjectName("verifyAllButton");
    verifyAllButton_->setAccessibleName(tr("Verify All Signatures"));

    signSelectedButton_ = new QPushButton("Sign Selected", this);
    signSelectedButton_->setObjectName("signSelectedButton");
    signSelectedButton_->setAccessibleName(tr("Sign Selected Field"));
    signSelectedButton_->setEnabled(false);

    viewDetailsButton_ = new QPushButton("View Details", this);
    viewDetailsButton_->setObjectName("viewDetailsButton");
    viewDetailsButton_->setAccessibleName(tr("View Signature Details"));
    viewDetailsButton_->setEnabled(false);

    // Create summary label
    signatureSummaryLabel_ = new QLabel(this);
    signatureSummaryLabel_->setObjectName("signatureSummaryLabel");
    signatureSummaryLabel_->setAccessibleName(tr("Signature Summary"));
    signatureSummaryLabel_->setText("");

    // Pipeline F phase-10: validation status row (Online / LTV / None).
    ltvStatusLabel_ = new QLabel(this);
    ltvStatusLabel_->setObjectName("ltvStatusLabel");
    ltvStatusLabel_->setAccessibleName(tr("LTV Validation Status"));
    ltvStatusLabel_->setText("");

    // Phase-50 (F52/F53): detail row for LtvVerifyResult::message.
    ltvStatusDetailLabel_ = new QLabel(this);
    ltvStatusDetailLabel_->setObjectName("ltvStatusDetailLabel");
    ltvStatusDetailLabel_->setAccessibleName(tr("LTV Validation Detail"));
    ltvStatusDetailLabel_->setAccessibleDescription(
        tr("Explains which rule produced the validation status above"));
    ltvStatusDetailLabel_->setWordWrap(true);
    // PHASE-50 REVIEW. QLabel defaults to Qt::AutoText, which renders anything
    // that looks like rich text. This row carries LtvVerifyResult::message and
    // sits directly beneath the status row, so a message that ever came to
    // include document-supplied text could paint a convincing second
    // "Validation: Validated (LTV)" line under a status that says otherwise.
    // Nothing here is ever markup; say so.
    ltvStatusDetailLabel_->setTextFormat(Qt::PlainText);
    ltvStatusDetailLabel_->setText("");

    // Create button layout
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(verifyAllButton_);
    buttonLayout->addWidget(signSelectedButton_);
    buttonLayout->addWidget(viewDetailsButton_);
    buttonLayout->addStretch();

    // Create main layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(signatureTreeWidget_, 1);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(signatureSummaryLabel_);
    mainLayout->addWidget(ltvStatusLabel_);
    mainLayout->addWidget(ltvStatusDetailLabel_);

    // Connect signals
    connect(signatureTreeWidget_, &QTreeWidget::itemClicked,
            this, &SignaturePanel::onTreeItemClicked);
    connect(verifyAllButton_, &QPushButton::clicked,
            this, &SignaturePanel::onVerifyAllClicked);
    connect(signSelectedButton_, &QPushButton::clicked,
            this, &SignaturePanel::onSignClicked);
    connect(viewDetailsButton_, &QPushButton::clicked,
            this, &SignaturePanel::onViewDetailsClicked);
}

SignaturePanel::~SignaturePanel() = default;

void SignaturePanel::setSignatures(const std::vector<pdfe::signatures::SignatureData>& signatures)
{
    signatureTreeWidget_->clear();

    int signedCount = 0;
    int totalCount = static_cast<int>(signatures.size());

    for (const auto& sigData : signatures) {
        QTreeWidgetItem* item = new QTreeWidgetItem(signatureTreeWidget_);

        // Set column data
        item->setText(0, QString::fromStdString(sigData.fieldName));
        item->setText(1, QString::fromStdString(sigData.signer.commonName));
        item->setText(2, QString::number(sigData.pageIndex + 1)); // Display 1-based page numbers
        item->setText(3, sigData.isSigned ? "Signed" : "Unsigned");

        // Store data in item
        item->setData(0, Qt::UserRole, sigData.pageIndex);
        item->setData(0, Qt::UserRole + 1, sigData.widgetIndex);

        // Update signed count
        if (sigData.isSigned) {
            ++signedCount;
        }

        signatureTreeWidget_->addTopLevelItem(item);
    }

    // Update summary label
    signatureSummaryLabel_->setText(
        QString("%1 of %2 signed").arg(signedCount).arg(totalCount)
    );

    // Enable/disable buttons based on content
    verifyAllButton_->setEnabled(totalCount > 0);
}

void SignaturePanel::clear()
{
    signatureTreeWidget_->clear();
    signatureSummaryLabel_->setText("");
    if (ltvStatusLabel_) ltvStatusLabel_->setText("");
    if (ltvStatusDetailLabel_) ltvStatusDetailLabel_->setText("");
    signSelectedButton_->setEnabled(false);
    viewDetailsButton_->setEnabled(false);
    verifyAllButton_->setEnabled(false);
}

void SignaturePanel::setLtvStatusText(const QString& statusText,
                                      const QString& detail)
{
    if (ltvStatusLabel_) {
        if (statusText.isEmpty()) {
            ltvStatusLabel_->setText(QString());
        } else {
            ltvStatusLabel_->setText(tr("Validation: %1").arg(statusText));
        }
    }
    // Phase-50 (F52/F53). The status row is deliberately unchanged in shape —
    // test_ui_ltv.py and the testLtvStatus pipe both read it and expect it to
    // start with "Validation:". The detail is a second row, so nothing that
    // already reads the first one has to change.
    if (ltvStatusDetailLabel_) {
        if (statusText.isEmpty() || detail.isEmpty()) {
            ltvStatusDetailLabel_->setText(QString());
        } else {
            ltvStatusDetailLabel_->setText(detail);
        }
    }
}

QString SignaturePanel::ltvStatusText() const
{
    return ltvStatusLabel_ ? ltvStatusLabel_->text() : QString();
}

QString SignaturePanel::ltvStatusDetailText() const
{
    return ltvStatusDetailLabel_ ? ltvStatusDetailLabel_->text() : QString();
}

void SignaturePanel::onTreeItemClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) {
        return;
    }

    int pageIndex = item->data(0, Qt::UserRole).toInt();
    int widgetIndex = item->data(0, Qt::UserRole + 1).toInt();

    // Enable action buttons when an item is selected
    signSelectedButton_->setEnabled(true);
    viewDetailsButton_->setEnabled(true);

    emit signatureSelected(pageIndex, widgetIndex);
}

void SignaturePanel::onSignClicked()
{
    QTreeWidgetItem* item = signatureTreeWidget_->currentItem();
    if (!item) {
        return;
    }

    int pageIndex = item->data(0, Qt::UserRole).toInt();
    int widgetIndex = item->data(0, Qt::UserRole + 1).toInt();

    emit signRequested(pageIndex, widgetIndex);
}

void SignaturePanel::onVerifyAllClicked()
{
    emit verifyAllRequested();
}

void SignaturePanel::onViewDetailsClicked()
{
    QTreeWidgetItem* item = signatureTreeWidget_->currentItem();
    if (!item) {
        return;
    }

    int pageIndex = item->data(0, Qt::UserRole).toInt();
    int widgetIndex = item->data(0, Qt::UserRole + 1).toInt();

    emit viewDetailsRequested(pageIndex, widgetIndex);
}

} // namespace pdfe::ui
