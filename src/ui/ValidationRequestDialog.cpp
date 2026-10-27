// -*- coding: utf-8 -*-
#include "ValidationRequestDialog.h"
#include "TestDialogSeam.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace pdfe::ui {

// Pipeline f3c91a2e phase-7 (GAP-A14): records the last constructed
// dialog's widget inventory for the result-pipe test seam. Written only
// when PDFE_TEST_PIPES is set; read back through MainWindow's
// testValidationDialogResultPipe.
static QString g_validationRequestIntrospection;

ValidationRequestDialog::ValidationRequestDialog(
    const QStringList& ocspUrls,
    const QStringList& crlUrls,
    QWidget* parent)
    : QDialog(parent), ocspUrls_(ocspUrls), crlUrls_(crlUrls)
{
    setObjectName("validationRequestDialog");
    setAccessibleName(tr("Request Validation Data Fetch"));
    setWindowTitle(tr("Request Validation Data Fetch"));
    setModal(true);

    auto* root = new QVBoxLayout(this);

    // Header row: warning icon + title.
    {
        auto* header = new QHBoxLayout();
        auto* iconLabel = new QLabel(this);
        iconLabel->setPixmap(
            style()->standardIcon(QStyle::SP_MessageBoxWarning)
                .pixmap(32, 32));
        iconLabel->setObjectName("validationRequestIconLabel");

        auto* titleLabel = new QLabel(
            tr("Request Validation Data Fetch"), this);
        titleLabel->setObjectName("validationRequestTitleLabel");
        QFont titleFont = titleLabel->font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.2);
        titleLabel->setFont(titleFont);

        header->addWidget(iconLabel);
        header->addWidget(titleLabel, 1);
        root->addLayout(header);
    }

    // Big bold security warning. Wrapped multi-line to make sure the
    // user actually sees what they're consenting to.
    warningLabel_ = new QLabel(this);
    warningLabel_->setObjectName("validationRequestWarningLabel");
    warningLabel_->setAccessibleName(tr("Security warning"));
    warningLabel_->setText(
        tr("PDFE will contact these external servers to fetch OCSP "
           "responses and CRLs.\n\n"
           "This will reveal your IP address and the time you opened "
           "this document to those servers. Only proceed if you trust "
           "the URLs shown below."));
    warningLabel_->setWordWrap(true);
    QFont warnFont = warningLabel_->font();
    warnFont.setBold(true);
    warningLabel_->setFont(warnFont);
    root->addWidget(warningLabel_);

    // URL list.
    listWidget_ = new QListWidget(this);
    listWidget_->setObjectName("validationRequestUrlList");
    listWidget_->setAccessibleName(tr("Validation URLs"));
    listWidget_->setSelectionMode(QAbstractItemView::NoSelection);
    // Keep focus policy at default (StrongFocus) so the URL list is
    // reachable via Tab even when empty — this ensures keyboard users can
    // review the URL area before deciding to Fetch.
    for (const QString& u : ocspUrls_) {
        listWidget_->addItem(QStringLiteral("[OCSP] ") + u);
    }
    for (const QString& u : crlUrls_) {
        listWidget_->addItem(QStringLiteral("[CRL] ") + u);
    }
    root->addWidget(listWidget_, 1);

    // Empty-state label, shown only when no URLs.
    emptyLabel_ = new QLabel(
        tr("No validation URLs available in the signer certificate."),
        this);
    emptyLabel_->setObjectName("validationRequestEmptyLabel");
    emptyLabel_->setAccessibleName(tr("No validation URLs available"));
    emptyLabel_->setWordWrap(true);
    emptyLabel_->setVisible(isUrlListEmpty());
    // Keep the list widget visible always so it remains in the tab chain;
    // the emptyLabel_ above provides the user-visible explanation.
    root->addWidget(emptyLabel_);

    // Buttons. AC: Cancel is the *default* button (Esc, Enter dismiss).
    {
        auto* btnRow = new QHBoxLayout();
        btnRow->addStretch(1);

        fetchButton_ = new QPushButton(tr("Fetch"), this);
        fetchButton_->setObjectName("fetchButton");
        fetchButton_->setAccessibleName(tr("Fetch validation data"));
        fetchButton_->setAutoDefault(false);
        fetchButton_->setDefault(false);

        cancelButton_ = new QPushButton(tr("Cancel"), this);
        cancelButton_->setObjectName("cancelButton");
        cancelButton_->setAccessibleName(tr("Cancel — do not contact servers"));
        cancelButton_->setAutoDefault(true);
        cancelButton_->setDefault(true);

        connect(fetchButton_, &QPushButton::clicked,
                this, &QDialog::accept);
        connect(cancelButton_, &QPushButton::clicked,
                this, &QDialog::reject);

        // Disable fetch when there is nothing to fetch.
        if (isUrlListEmpty()) {
            fetchButton_->setEnabled(false);
        }

        btnRow->addWidget(fetchButton_);
        btnRow->addWidget(cancelButton_);
        root->addLayout(btnRow);

        // Cancel takes initial focus, even though Fetch is laid out first.
        cancelButton_->setFocus(Qt::OtherFocusReason);

        // Explicit tab order: list → cancel → fetch (when enabled) → list.
        // fetchButton_ may be disabled (no URLs), in which case Qt skips it.
        listWidget_->setFocusPolicy(Qt::StrongFocus);
        cancelButton_->setFocusPolicy(Qt::StrongFocus);
        fetchButton_->setFocusPolicy(Qt::StrongFocus);
        setTabOrder(cancelButton_, listWidget_);
        setTabOrder(listWidget_, fetchButton_);
        setTabOrder(fetchButton_, cancelButton_);
    }

    setMinimumSize(560, 360);

    // Pipeline f3c91a2e phase-7 (GAP-A14): test-only seam. On the CI
    // session pywinauto's UIA cannot enumerate this modal dialog, so we
    // record a verifiable inventory of the real child widgets here and
    // auto-dismiss the modal loop via reject() — which is exactly the
    // Cancel path the test asserts (no servers contacted, no DSS write).
    // Inert in production (env var unset).
    if (pdfe::ui::testseam::testPipesEnabled()) {
        const int urlCount =
            listWidget_ ? listWidget_->count() : -1;
        g_validationRequestIntrospection =
            QStringLiteral("validationRequestDialog|warning=%1|urlList=%2|"
                           "urlCount=%3|fetch=%4|cancel=%5|fetchEnabled=%6")
                .arg(warningLabel_ ? 1 : 0)
                .arg(listWidget_ ? 1 : 0)
                .arg(urlCount)
                .arg(fetchButton_ ? 1 : 0)
                .arg(cancelButton_ ? 1 : 0)
                .arg((fetchButton_ && fetchButton_->isEnabled()) ? 1 : 0);
        // GAP-A14 keyboard-test conversion: the auto-dismiss is now gated on
        // PDFE_TEST_DIALOG_AUTODISMISS (set by ui_automation's launch_pdfe,
        // which relies on the modal exec() returning to read the result pipe)
        // rather than plain PDFE_TEST_PIPES. The keyboard tests
        // (PDFESession launcher) do NOT set AUTODISMISS, so the dialog stays
        // open and they can drive it via the dialog-child keyboard seam.
        if (qEnvironmentVariableIsSet("PDFE_TEST_DIALOG_AUTODISMISS")) {
            // Defer to the next event-loop turn so it fires inside exec()'s
            // nested modal loop, taking the Cancel branch (QDialog::reject).
            QTimer::singleShot(0, this, &QDialog::reject);
        }
    }

    // GAP-A14 keyboard-state seam: install the dialog-child keyboard
    // query/result pipes (gated by PDFE_TEST_PIPES inside the helper).
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

QStringList ValidationRequestDialog::allUrls() const
{
    QStringList out = ocspUrls_;
    out.append(crlUrls_);
    return out;
}

QStringList ValidationRequestDialog::ocspUrls() const { return ocspUrls_; }
QStringList ValidationRequestDialog::crlUrls() const { return crlUrls_; }

bool ValidationRequestDialog::isUrlListEmpty() const
{
    return ocspUrls_.isEmpty() && crlUrls_.isEmpty();
}

QString ValidationRequestDialog::lastTestIntrospection()
{
    return g_validationRequestIntrospection;
}

} // namespace pdfe::ui
