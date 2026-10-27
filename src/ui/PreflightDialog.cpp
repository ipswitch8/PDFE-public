// -*- coding: utf-8 -*-
#include "PreflightDialog.h"
#include "../compliance/ComplianceManager.h"
#include "../core/Document.h"
#include "../core/MuPdfContext.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTextStream>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <fstream>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

namespace {

struct Profile {
    const char* label;
    // compliance level; PdfALevel::None means "PDF/UA"
    pdfe::compliance::PdfALevel level;
    bool isPdfUA;
};

const Profile kProfiles[] = {
    {"PDF/A-1b",   pdfe::compliance::PdfALevel::PdfA_1b, false},
    {"PDF/A-2b",   pdfe::compliance::PdfALevel::PdfA_2b, false},
    {"PDF/A-2u",   pdfe::compliance::PdfALevel::PdfA_2u, false},
    {"PDF/A-2a",   pdfe::compliance::PdfALevel::PdfA_2a, false},
    {"PDF/A-3b",   pdfe::compliance::PdfALevel::PdfA_3b, false},
    {"PDF/UA-1",   pdfe::compliance::PdfALevel::None,    true},
};

} // namespace

PreflightDialog::PreflightDialog(
    pdfe::core::Document* doc,
    std::shared_ptr<pdfe::core::MuPdfContext> ctx,
    QWidget* parent)
    : QDialog(parent), doc_(doc), ctx_(std::move(ctx))
{
    setObjectName("preflightDialog");
    setWindowTitle(tr("Preflight"));
    resize(720, 480);

    if (ctx_) {
        manager_ = std::make_unique<pdfe::compliance::ComplianceManager>(ctx_);
    }

    auto* mainLayout = new QVBoxLayout(this);

    // --- Top row: profile picker + Run button ---
    auto* topRow = new QHBoxLayout();
    auto* profileLabel = new QLabel(tr("Profile:"), this);
    profileLabel->setObjectName("preflightProfileLabel");
    profileCombo_ = new QComboBox(this);
    profileCombo_->setObjectName("preflightProfileCombo");
    profileCombo_->setAccessibleName(tr("Preflight profile"));
    for (const auto& p : kProfiles) {
        profileCombo_->addItem(QString::fromLatin1(p.label));
    }
    runButton_ = new QPushButton(tr("Run Preflight"), this);
    runButton_->setObjectName("preflightRunButton");
    runButton_->setAccessibleName(tr("Run preflight validation"));
    connect(runButton_, &QPushButton::clicked,
            this, &PreflightDialog::onRunClicked);
    topRow->addWidget(profileLabel);
    topRow->addWidget(profileCombo_, 1);
    topRow->addWidget(runButton_);
    mainLayout->addLayout(topRow);

    // --- Summary line ---
    summaryLabel_ = new QLabel(tr("No validation run yet."), this);
    summaryLabel_->setObjectName("preflightSummaryLabel");
    mainLayout->addWidget(summaryLabel_);

    // --- Issue tree ---
    issueTree_ = new QTreeWidget(this);
    issueTree_->setObjectName("preflightIssueTree");
    issueTree_->setAccessibleName(tr("Preflight issues"));
    issueTree_->setHeaderLabels({
        tr("Severity"), tr("Rule"), tr("Page"), tr("Description")
    });
    issueTree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    mainLayout->addWidget(issueTree_, 1);

    // --- Bottom row: Fix All + Export + Close ---
    fixAllButton_ = new QPushButton(tr("Fix All"), this);
    fixAllButton_->setObjectName("preflightFixAllButton");
    fixAllButton_->setAccessibleName(tr("Apply all auto-fixers"));
    fixAllButton_->setToolTip(
        tr("Apply every auto-fixable compliance fix in one pass."));
    fixAllButton_->setEnabled(false);
    connect(fixAllButton_, &QPushButton::clicked,
            this, &PreflightDialog::onFixAllClicked);

    exportButton_ = new QPushButton(tr("Export Report..."), this);
    exportButton_->setObjectName("preflightExportButton");
    exportButton_->setAccessibleName(tr("Export preflight report"));
    exportButton_->setEnabled(false);
    connect(exportButton_, &QPushButton::clicked,
            this, &PreflightDialog::onExportClicked);

    closeBox_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    closeBox_->setObjectName("preflightCloseBox");
    connect(closeBox_, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    auto* bottomRow = new QHBoxLayout();
    bottomRow->addWidget(fixAllButton_);
    bottomRow->addWidget(exportButton_);
    bottomRow->addStretch(1);
    bottomRow->addWidget(closeBox_);
    mainLayout->addLayout(bottomRow);

    // QAbstractItemView (QTreeWidget) uses Tab for internal item navigation
    // by default — disable so Tab advances to the next dialog widget.
    issueTree_->setTabKeyNavigation(false);

    // Ensure all interactive widgets have tab-friendly focus policies.
    profileCombo_->setFocusPolicy(Qt::StrongFocus);
    runButton_->setFocusPolicy(Qt::StrongFocus);
    issueTree_->setFocusPolicy(Qt::StrongFocus);
    fixAllButton_->setFocusPolicy(Qt::StrongFocus);
    exportButton_->setFocusPolicy(Qt::StrongFocus);

    // Explicit tab order: profile combo → run → tree → fix-all → export →
    // close → (cycle back).  The Close button is inside a QDialogButtonBox;
    // reach it via the box itself.
    auto* closeBtn = closeBox_->button(QDialogButtonBox::Close);
    if (closeBtn) {
        closeBtn->setFocusPolicy(Qt::StrongFocus);
        setTabOrder(profileCombo_, runButton_);
        setTabOrder(runButton_, issueTree_);
        setTabOrder(issueTree_, fixAllButton_);
        setTabOrder(fixAllButton_, exportButton_);
        setTabOrder(exportButton_, closeBtn);
        setTabOrder(closeBtn, profileCombo_);
    } else {
        setTabOrder(profileCombo_, runButton_);
        setTabOrder(runButton_, issueTree_);
        setTabOrder(issueTree_, fixAllButton_);
        setTabOrder(fixAllButton_, exportButton_);
        setTabOrder(exportButton_, profileCombo_);
    }

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

PreflightDialog::~PreflightDialog() = default;

void PreflightDialog::onRunClicked()
{
    if (!doc_ || !manager_) {
        summaryLabel_->setText(tr("No document loaded."));
        return;
    }
    setResult(runValidation());
}

pdfe::compliance::ComplianceResult PreflightDialog::runValidation()
{
    const int idx = profileCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(std::size(kProfiles))) {
        return {};
    }
    const auto& p = kProfiles[idx];
    if (p.isPdfUA) {
        return manager_->validatePdfUA(doc_);
    }
    return manager_->validatePdfA(doc_, p.level);
}

void PreflightDialog::setResult(
    const pdfe::compliance::ComplianceResult& result)
{
    currentResult_ = result;
    refreshTree();
    refreshSummary();
    exportButton_->setEnabled(!result.issues.empty()
                              || !result.errorMessage.empty());
    refreshFixAllButton();
}

int PreflightDialog::autoFixableCount() const
{
    return static_cast<int>(
        pdfe::compliance::ComplianceManager::autoFixableIssues(
            currentResult_.issues).size());
}

void PreflightDialog::refreshFixAllButton()
{
    if (!fixAllButton_) return;
    const int n = autoFixableCount();
    fixAllButton_->setEnabled(doc_ && manager_ && n > 0);
    if (n > 0) {
        fixAllButton_->setText(tr("Fix All (%1)").arg(n));
    } else {
        fixAllButton_->setText(tr("Fix All"));
    }
}

pdfe::compliance::ComplianceManager::FixAllResult
PreflightDialog::runFixAll()
{
    pdfe::compliance::ComplianceManager::FixAllResult r;
    if (!doc_ || !manager_) return r;

    // Pick the level the dialog is currently configured for. PDF/UA
    // re-validates against PDF/A-2a (the matched ISO target for tagged
    // PDFs) — fixers are PDF/A oriented.
    const int idx = profileCombo_->currentIndex();
    pdfe::compliance::PdfALevel level =
        pdfe::compliance::PdfALevel::PdfA_2b;
    if (idx >= 0 && idx < static_cast<int>(std::size(kProfiles))) {
        const auto& p = kProfiles[idx];
        level = p.isPdfUA
            ? pdfe::compliance::PdfALevel::PdfA_2a
            : p.level;
    }

    r = manager_->fixAll(doc_, currentResult_.issues, level);

    // Refresh tree from re-validation result; preserve the original
    // target level on the displayed result.
    pdfe::compliance::ComplianceResult after;
    after.targetLevel = currentResult_.targetLevel;
    after.targetUALevel = currentResult_.targetUALevel;
    after.issues = r.stillRemaining;
    after.success = !after.hasErrors();
    setResult(after);
    return r;
}

void PreflightDialog::onFixAllClicked()
{
    auto r = runFixAll();
    emit fixAllCompleted(r.succeeded, r.failed);
}

void PreflightDialog::refreshTree()
{
    issueTree_->clear();

    QTreeWidgetItem* errorsRoot = new QTreeWidgetItem(
        issueTree_, QStringList{tr("Errors")});
    errorsRoot->setData(0, Qt::UserRole, QStringLiteral("errors"));
    QTreeWidgetItem* warningsRoot = new QTreeWidgetItem(
        issueTree_, QStringList{tr("Warnings")});
    warningsRoot->setData(0, Qt::UserRole, QStringLiteral("warnings"));

    for (const auto& issue : currentResult_.issues) {
        QTreeWidgetItem* parent =
            issue.severity == pdfe::compliance::IssueSeverity::Error
                ? errorsRoot : warningsRoot;
        QStringList cols{
            issue.severity == pdfe::compliance::IssueSeverity::Error
                ? tr("Error") : tr("Warning"),
            QString::fromStdString(issue.rule),
            issue.pageIndex >= 0
                ? QString::number(issue.pageIndex + 1)
                : tr("—"),
            QString::fromStdString(issue.description),
        };
        new QTreeWidgetItem(parent, cols);
    }

    errorsRoot->setExpanded(true);
    warningsRoot->setExpanded(true);
}

void PreflightDialog::refreshSummary()
{
    const int ec = currentResult_.errorCount();
    const int wc = currentResult_.warningCount();
    if (ec == 0 && wc == 0) {
        summaryLabel_->setText(
            currentResult_.success
                ? tr("No issues found. Document passes preflight.")
                : tr("Validation returned no issues."));
    } else {
        summaryLabel_->setText(
            tr("%1 error(s), %2 warning(s).").arg(ec).arg(wc));
    }
}

void PreflightDialog::onExportClicked()
{
    const QString savePath = QFileDialog::getSaveFileName(
        this, tr("Export Preflight Report"),
        QStringLiteral("preflight_report.txt"),
        tr("Text Files (*.txt);;All Files (*)"));
    if (savePath.isEmpty()) return;

    std::ofstream out(savePath.toStdString());
    if (!out) return;
    out << "PDFE Preflight Report\n";
    out << "Profile: "
        << profileCombo_->currentText().toStdString() << "\n";
    out << "Errors: " << currentResult_.errorCount() << "\n";
    out << "Warnings: " << currentResult_.warningCount() << "\n\n";
    for (const auto& issue : currentResult_.issues) {
        out << (issue.severity == pdfe::compliance::IssueSeverity::Error
                    ? "[ERROR]   "
                    : "[WARNING] ")
            << issue.rule << " ";
        if (issue.pageIndex >= 0) {
            out << "(page " << (issue.pageIndex + 1) << ") ";
        }
        out << issue.description << "\n";
    }
    emit reportExportRequested(savePath);
}

} // namespace pdfe::ui
