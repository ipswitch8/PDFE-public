// -*- coding: utf-8 -*-
// Pipeline F phase-10: TrustAnchorDialog implementation. See header.
#include "TrustAnchorDialog.h"

#include "signatures/TrustAnchorManager.h"
#include "TestDialogSeam.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <filesystem>

namespace pdfe::ui {

// Pipeline f3c91a2e phase-7 (GAP-A14): records the last constructed
// dialog's widget inventory for the result-pipe test seam. Written only
// when PDFE_TEST_PIPES is set; read back through MainWindow's
// testTrustAnchorDialogResultPipe.
static QString g_trustAnchorIntrospection;

TrustAnchorDialog::TrustAnchorDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName("trustAnchorDialog");
    setAccessibleName(tr("Trust Anchors"));
    setWindowTitle(tr("Trust Anchors"));
    resize(700, 400);

    auto* mainLayout = new QVBoxLayout(this);

    auto* descLabel = new QLabel(
        tr("Certificates listed here are trusted as root anchors when "
           "validating signed PDFs offline (LTV). Use Add to import a "
           "PEM or DER X.509 certificate."), this);
    descLabel->setObjectName("trustAnchorDescriptionLabel");
    descLabel->setWordWrap(true);
    mainLayout->addWidget(descLabel);

    table_ = new QTableWidget(this);
    table_->setObjectName("trustAnchorTable");
    table_->setAccessibleName(tr("Trusted Roots"));
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({
        tr("Subject CN"),
        tr("Issuer CN"),
        tr("Fingerprint (SHA-256)"),
        tr("File Path")
    });
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    mainLayout->addWidget(table_, 1);

    auto* buttonRow = new QHBoxLayout();
    addButton_ = new QPushButton(tr("Add..."), this);
    addButton_->setObjectName("addTrustButton");
    addButton_->setAccessibleName(tr("Add Trust Anchor"));
    removeButton_ = new QPushButton(tr("Remove"), this);
    removeButton_->setObjectName("removeTrustButton");
    removeButton_->setAccessibleName(tr("Remove Trust Anchor"));
    removeButton_->setEnabled(false);
    closeButton_ = new QPushButton(tr("Close"), this);
    closeButton_->setObjectName("closeTrustButton");
    closeButton_->setAccessibleName(tr("Close Trust Anchor Dialog"));

    buttonRow->addWidget(addButton_);
    buttonRow->addWidget(removeButton_);
    buttonRow->addStretch();
    buttonRow->addWidget(closeButton_);
    mainLayout->addLayout(buttonRow);

    connect(addButton_, &QPushButton::clicked,
            this, &TrustAnchorDialog::onAddClicked);
    connect(removeButton_, &QPushButton::clicked,
            this, &TrustAnchorDialog::onRemoveClicked);
    connect(closeButton_, &QPushButton::clicked,
            this, &QDialog::accept);
    connect(table_, &QTableWidget::itemSelectionChanged,
            this, &TrustAnchorDialog::onSelectionChanged);

    refresh();

    // Pipeline f3c91a2e phase-7 (GAP-A14): test-only seam. On the CI
    // session pywinauto's UIA cannot enumerate this modal dialog, so we
    // record a verifiable inventory of the real child widgets here and
    // auto-dismiss the modal loop (the Close/accept path) so exec()
    // returns without blocking. Inert in production (env var unset).
    if (pdfe::ui::testseam::testPipesEnabled()) {
        g_trustAnchorIntrospection =
            QStringLiteral("trustAnchorDialog|table=%1|add=%2|remove=%3|"
                           "close=%4|rows=%5")
                .arg(table_ ? 1 : 0)
                .arg(addButton_ ? 1 : 0)
                .arg(removeButton_ ? 1 : 0)
                .arg(closeButton_ ? 1 : 0)
                .arg(table_ ? table_->rowCount() : -1);
        // GAP-A14 keyboard-test conversion: the auto-dismiss is now gated on
        // PDFE_TEST_DIALOG_AUTODISMISS (set by ui_automation's launch_pdfe,
        // which relies on the modal exec() returning to read the result pipe)
        // rather than plain PDFE_TEST_PIPES. The keyboard tests
        // (PDFESession launcher) do NOT set AUTODISMISS, so the dialog stays
        // open and they can drive it via the dialog-child keyboard seam.
        if (qEnvironmentVariableIsSet("PDFE_TEST_DIALOG_AUTODISMISS")) {
            // Defer to the next event-loop turn so it fires inside exec()'s
            // nested modal loop, dismissing it the same way the Close button
            // would (QDialog::accept).
            QTimer::singleShot(0, this, &QDialog::accept);
        }
    }

    // GAP-A14 keyboard-state seam: install the dialog-child keyboard
    // query/result pipes (gated by PDFE_TEST_PIPES inside the helper).
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

TrustAnchorDialog::~TrustAnchorDialog() = default;

QString TrustAnchorDialog::lastTestIntrospection()
{
    return g_trustAnchorIntrospection;
}

void TrustAnchorDialog::refresh()
{
    if (!table_) return;
    table_->setRowCount(0);

    const auto anchors = pdfe::signatures::TrustAnchorManager::instance().listTrusts();
    table_->setRowCount(anchors.size());
    for (int row = 0; row < anchors.size(); ++row) {
        const auto& a = anchors[row];
        auto* subjectItem = new QTableWidgetItem(a.subjectCN);
        subjectItem->setData(Qt::UserRole, a.fingerprintSha256);
        auto* issuerItem = new QTableWidgetItem(a.issuerCN);
        auto* fpItem = new QTableWidgetItem(a.fingerprintSha256);
        auto* pathItem = new QTableWidgetItem(a.filePath);
        table_->setItem(row, 0, subjectItem);
        table_->setItem(row, 1, issuerItem);
        table_->setItem(row, 2, fpItem);
        table_->setItem(row, 3, pathItem);
    }
    onSelectionChanged();
}

void TrustAnchorDialog::onAddClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Import Trust Anchor"),
        QString(),
        tr("Certificates (*.pem *.crt *.cer *.der);;All files (*.*)"));

    if (path.isEmpty()) return;

    QString err;
    const bool ok = pdfe::signatures::TrustAnchorManager::instance()
                        .addTrust(std::filesystem::path(path.toStdString()),
                                   &err);
    if (!ok) {
        QMessageBox::warning(this, tr("Import Failed"),
            tr("Could not import certificate: %1").arg(err));
        return;
    }
    refresh();
}

void TrustAnchorDialog::onRemoveClicked()
{
    const auto rows = table_->selectionModel()
                          ? table_->selectionModel()->selectedRows()
                          : QModelIndexList();
    if (rows.isEmpty()) return;
    const int row = rows.first().row();
    auto* item = table_->item(row, 0);
    if (!item) return;
    const QString fp = item->data(Qt::UserRole).toString();
    if (fp.isEmpty()) return;

    pdfe::signatures::TrustAnchorManager::instance().removeTrust(fp);
    refresh();
}

void TrustAnchorDialog::onSelectionChanged()
{
    if (!removeButton_ || !table_) return;
    const auto rows = table_->selectionModel()
                          ? table_->selectionModel()->selectedRows()
                          : QModelIndexList();
    removeButton_->setEnabled(!rows.isEmpty());
}

} // namespace pdfe::ui
