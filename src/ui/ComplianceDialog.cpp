#include "ComplianceDialog.h"
#include "TestDialogSeam.h"
#include "compliance/ComplianceManager.h"
#include "compliance/ComplianceTypes.h"
#include "compliance/StructureTypes.h"
#include "compliance/AutoTagger.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"

#include <QComboBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>

namespace pdfe::ui {

ComplianceDialog::ComplianceDialog(
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    pdfe::core::Document* document,
    QWidget* parent)
    : QDialog(parent)
    , context_(std::move(context))
    , document_(document)
{
    setupUi();
    // GAP-A14 / SKIP=FAIL: record the dialog's widget inventory and replay
    // any test action script queued for "ComplianceDialog" so the convert
    // flow can be driven without UIA enumeration of the modal.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

ComplianceDialog::~ComplianceDialog() = default;

void ComplianceDialog::setupUi() {
    setWindowTitle(tr("PDF Standards Compliance"));
    setMinimumSize(750, 550);
    setObjectName("ComplianceDialog");
    // GitHub #185: the accessible name OVERRIDES the window title in what a
    // screen reader announces, so it must BE the title, not the class name.
    // Kept explicit rather than removed so the announcement stays assertable
    // (test_ui_compliance_accessibility.py section 7c asserts this exact
    // string arrives as a UIA focus event).
    setAccessibleName(tr("PDF Standards Compliance"));

    auto* mainLayout = new QVBoxLayout(this);

    // Target level selection
    auto* topLayout = new QHBoxLayout();
    auto* targetLabel = new QLabel(tr("Target:"), this);
    targetLabel->setObjectName("TargetLabel");
    targetLabel->setAccessibleName(tr("Target:"));  // GitHub #185: spoken text

    levelCombo_ = new QComboBox(this);
    levelCombo_->setObjectName("LevelCombo");
    // GitHub #185: the combo has no visible caption of its own; the label it
    // sits beside is its caption, so that is what a screen reader should say.
    levelCombo_->setAccessibleName(tr("Target"));
    levelCombo_->addItem(tr("PDF/A-1b (Archival, Visual)"), static_cast<int>(compliance::PdfALevel::PdfA_1b));
    levelCombo_->addItem(tr("PDF/A-2b (Archival, Visual)"), static_cast<int>(compliance::PdfALevel::PdfA_2b));
    levelCombo_->addItem(tr("PDF/A-2u (Archival, Unicode)"), static_cast<int>(compliance::PdfALevel::PdfA_2u));
    levelCombo_->addItem(tr("PDF/A-2a (Archival, Accessible)"), static_cast<int>(compliance::PdfALevel::PdfA_2a));
    levelCombo_->addItem(tr("PDF/A-3b (Archival + Attachments)"), static_cast<int>(compliance::PdfALevel::PdfA_3b));
    levelCombo_->addItem(tr("PDF/UA-1 (Universal Accessibility)"), -1);

    topLayout->addWidget(targetLabel);
    topLayout->addWidget(levelCombo_, 1);
    mainLayout->addLayout(topLayout);

    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    analyzeButton_ = new QPushButton(tr("Analyze"), this);
    analyzeButton_->setObjectName("AnalyzeButton");
    analyzeButton_->setAccessibleName("Analyze");
    validateButton_ = new QPushButton(tr("Validate"), this);
    validateButton_->setObjectName("ValidateButton");
    validateButton_->setAccessibleName("Validate");
    convertButton_ = new QPushButton(tr("Convert..."), this);
    convertButton_->setObjectName("ConvertButton");
    convertButton_->setAccessibleName("Convert");
    previewTagsButton_ = new QPushButton(tr("Preview Tags"), this);
    previewTagsButton_->setObjectName("previewTagsButton");
    previewTagsButton_->setAccessibleName("Preview Tags");

    buttonLayout->addWidget(analyzeButton_);
    buttonLayout->addWidget(validateButton_);
    buttonLayout->addWidget(convertButton_);
    buttonLayout->addWidget(previewTagsButton_);
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);

    // Progress bar (hidden by default)
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName("progressBar");
    progressBar_->setAccessibleName("Conversion Progress");
    progressBar_->setRange(0, 0); // indeterminate
    progressBar_->setVisible(false);
    mainLayout->addWidget(progressBar_);

    // Splitter for issue table and tag tree
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName("complianceSplitter");

    // Issue table (left)
    auto* issueWidget = new QWidget(splitter);
    auto* issueLayout = new QVBoxLayout(issueWidget);
    issueLayout->setContentsMargins(0, 0, 0, 0);

    auto* issueLabel = new QLabel(tr("Issues:"), issueWidget);
    issueLabel->setObjectName("IssueLabel");
    issueLabel->setAccessibleName(tr("Issues:"));  // GitHub #185: spoken text
    issueLayout->addWidget(issueLabel);

    issueTable_ = new QTableWidget(issueWidget);
    issueTable_->setObjectName("IssueTable");
    issueTable_->setAccessibleName("Compliance Issues");
    issueTable_->setColumnCount(4);
    issueTable_->setHorizontalHeaderLabels({tr("Severity"), tr("Rule"), tr("Description"), tr("Page")});
    issueTable_->horizontalHeader()->setStretchLastSection(true);
    issueTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    issueTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    issueTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    issueTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    issueTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    issueLayout->addWidget(issueTable_);
    splitter->addWidget(issueWidget);

    // Tag structure tree (right)
    auto* treeWidget = new QWidget(splitter);
    auto* treeLayout = new QVBoxLayout(treeWidget);
    treeLayout->setContentsMargins(0, 0, 0, 0);

    auto* treeLabel = new QLabel(tr("Tag Structure:"), treeWidget);
    treeLabel->setObjectName("tagTreeLabel");
    treeLabel->setAccessibleName("Tag Structure Label");
    treeLayout->addWidget(treeLabel);

    tagStructureTree_ = new QTreeWidget(treeWidget);
    tagStructureTree_->setObjectName("tagStructureTree");
    tagStructureTree_->setAccessibleName("Tag Structure Tree");
    tagStructureTree_->setHeaderLabels({tr("Tag"), tr("Page"), tr("Alt Text")});
    tagStructureTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tagStructureTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tagStructureTree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    treeLayout->addWidget(tagStructureTree_);
    splitter->addWidget(treeWidget);

    splitter->setSizes({400, 300});
    mainLayout->addWidget(splitter, 1);

    // Status
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("StatusLabel");
    // DELIBERATELY NO setAccessibleName() HERE (GitHub #286).
    //
    // This label's TEXT is the information — it carries the live validation
    // result. An accessible name OVERRIDES the text in what UIA reports, so a
    // static name pins the announcement to that constant and the actual status
    // becomes unreadable to assistive tech and to any test that watches it.
    //
    // Measured: setting `setAccessibleName(tr("Compliance Status"))` here broke
    // SEVEN tests at once — every PDF/A/UA validate row plus the compliance
    // report row — all reporting "StatusLabel did not transition (still
    // 'Compliance Status')". The label was announcing its label instead of its
    // value.
    //
    // The description stays: it names the ROLE of the field, which is the
    // supplementary detail a description is for, and does not displace the text.
    statusLabel_->setAccessibleDescription("Compliance Status");
    mainLayout->addWidget(statusLabel_);

    // Ensure all interactive widgets have tab-friendly focus policies.
    levelCombo_->setFocusPolicy(Qt::StrongFocus);
    analyzeButton_->setFocusPolicy(Qt::StrongFocus);
    validateButton_->setFocusPolicy(Qt::StrongFocus);
    convertButton_->setFocusPolicy(Qt::StrongFocus);
    previewTagsButton_->setFocusPolicy(Qt::StrongFocus);
    issueTable_->setFocusPolicy(Qt::StrongFocus);
    tagStructureTree_->setFocusPolicy(Qt::StrongFocus);

    // QAbstractItemView (QTableWidget, QTreeWidget) uses Tab for internal
    // cell/item navigation by default — disable so Tab advances to the next
    // dialog widget instead of cycling within the table/tree.
    issueTable_->setTabKeyNavigation(false);
    tagStructureTree_->setTabKeyNavigation(false);

    // Explicit tab order: combo → buttons → table → tree → (cycle).
    setTabOrder(levelCombo_, analyzeButton_);
    setTabOrder(analyzeButton_, validateButton_);
    setTabOrder(validateButton_, convertButton_);
    setTabOrder(convertButton_, previewTagsButton_);
    setTabOrder(previewTagsButton_, issueTable_);
    setTabOrder(issueTable_, tagStructureTree_);
    setTabOrder(tagStructureTree_, levelCombo_);

    // Connect signals
    connect(analyzeButton_, &QPushButton::clicked, this, &ComplianceDialog::onAnalyze);
    connect(validateButton_, &QPushButton::clicked, this, &ComplianceDialog::onValidate);
    connect(convertButton_, &QPushButton::clicked, this, &ComplianceDialog::onConvert);
    connect(previewTagsButton_, &QPushButton::clicked, this, &ComplianceDialog::onPreviewTags);
}

compliance::PdfALevel ComplianceDialog::selectedLevel() const {
    int value = levelCombo_->currentData().toInt();
    if (value == -1) return compliance::PdfALevel::None;
    return static_cast<compliance::PdfALevel>(value);
}

void ComplianceDialog::setConvertingState(bool converting) {
    progressBar_->setVisible(converting);
    analyzeButton_->setEnabled(!converting);
    validateButton_->setEnabled(!converting);
    convertButton_->setEnabled(!converting);
    previewTagsButton_->setEnabled(!converting);
    levelCombo_->setEnabled(!converting);
    if (converting) {
        statusLabel_->setText(tr("Converting..."));
        QApplication::setOverrideCursor(Qt::BusyCursor);
    } else {
        QApplication::restoreOverrideCursor();
    }
    QApplication::processEvents();
}

void ComplianceDialog::onAnalyze() {
    compliance::ComplianceManager mgr(context_);

    auto level = selectedLevel();
    compliance::ComplianceResult result;

    if (level == compliance::PdfALevel::None) {
        result = mgr.validatePdfUA(document_);
    } else {
        result = mgr.analyzeForConversion(document_, level);
    }

    populateIssueTable(result);

    int errors = result.errorCount();
    int warnings = result.warningCount();
    statusLabel_->setText(
        tr("Analysis: %1 errors, %2 warnings")
        .arg(errors).arg(warnings)
    );
}

void ComplianceDialog::onValidate() {
    compliance::ComplianceManager mgr(context_);

    auto level = selectedLevel();
    compliance::ComplianceResult result;

    if (level == compliance::PdfALevel::None) {
        result = mgr.validatePdfUA(document_);
    } else {
        result = mgr.validatePdfA(document_, level);
    }

    populateIssueTable(result);

    int errors = result.errorCount();
    int warnings = result.warningCount();
    QString status = result.success
        ? tr("Validation PASSED: %1 warnings").arg(warnings)
        : tr("Validation FAILED: %1 errors, %2 warnings").arg(errors).arg(warnings);
    statusLabel_->setText(status);
}

void ComplianceDialog::onConvert() {
    QString filter = tr("PDF Files (*.pdf)");
    // SKIP=FAIL: honour a test-injected save path before falling back to the
    // native file dialog (which can't be driven on the CI session).
    QString outputPath = pdfe::ui::testseam::takeFileDialogPath();
    if (outputPath.isEmpty()) {
        outputPath = QFileDialog::getSaveFileName(
            this, tr("Save Compliant PDF"), QString(), filter);
    }

    if (outputPath.isEmpty()) return;

    setConvertingState(true);

    compliance::ComplianceManager mgr(context_);
    compliance::ComplianceResult result;

    auto level = selectedLevel();

    if (level == compliance::PdfALevel::None) {
        result = mgr.convertToPdfUA(document_, outputPath.toStdString());
    } else {
        result = mgr.convertToPdfA(document_, level, outputPath.toStdString());
    }

    setConvertingState(false);
    populateIssueTable(result);

    if (result.success || !result.hasErrors()) {
        statusLabel_->setText(tr("Conversion completed successfully"));
        emit conversionCompleted(outputPath);

        // Auto-refresh: re-validate to show updated compliance state
        compliance::ComplianceResult refreshResult;
        if (level == compliance::PdfALevel::None) {
            refreshResult = mgr.validatePdfUA(document_);
        } else {
            refreshResult = mgr.validatePdfA(document_, level);
        }
        populateIssueTable(refreshResult);

        int errors = refreshResult.errorCount();
        int warnings = refreshResult.warningCount();
        statusLabel_->setText(
            tr("Conversion complete — refreshed: %1 errors, %2 warnings")
            .arg(errors).arg(warnings)
        );

        QMessageBox::information(this, tr("Conversion Complete"),
            tr("Document has been converted and saved to:\n%1").arg(outputPath));
    } else {
        statusLabel_->setText(
            tr("Conversion completed with %1 issues")
            .arg(result.errorCount())
        );
    }
}

void ComplianceDialog::onPreviewTags() {
    if (!document_) return;

    statusLabel_->setText(tr("Generating tag structure..."));
    QApplication::setOverrideCursor(Qt::BusyCursor);
    QApplication::processEvents();

    try {
        compliance::AutoTagger tagger(context_);
        auto root = tagger.tagDocument(document_);
        populateTagTree(root);
        statusLabel_->setText(
            tr("Tag preview: %1 top-level elements")
            .arg(root.children.size())
        );
    } catch (const std::exception& e) {
        statusLabel_->setText(
            tr("Tag preview error: %1").arg(e.what())
        );
    }

    QApplication::restoreOverrideCursor();
}

void ComplianceDialog::populateTagTree(const compliance::StructureNode& root) {
    tagStructureTree_->clear();

    auto* rootItem = new QTreeWidgetItem(tagStructureTree_);
    rootItem->setText(0, QString::fromUtf8(compliance::structureTagToName(root.tag)));
    rootItem->setText(1, root.pageIndex >= 0 ? QString::number(root.pageIndex + 1) : QString());
    rootItem->setText(2, QString::fromStdString(root.altText));

    for (const auto& child : root.children) {
        addTreeNode(rootItem, *child);
    }

    tagStructureTree_->expandToDepth(1);
}

void ComplianceDialog::addTreeNode(QTreeWidgetItem* parent,
                                     const compliance::StructureNode& node) {
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, QString::fromUtf8(compliance::structureTagToName(node.tag)));
    item->setText(1, node.pageIndex >= 0 ? QString::number(node.pageIndex + 1) : QString());
    item->setText(2, QString::fromStdString(node.altText));

    for (const auto& child : node.children) {
        addTreeNode(item, *child);
    }
}

void ComplianceDialog::populateIssueTable(const compliance::ComplianceResult& result) {
    issueTable_->setRowCount(0);
    issueTable_->setRowCount(static_cast<int>(result.issues.size()));

    for (size_t i = 0; i < result.issues.size(); ++i) {
        const auto& issue = result.issues[i];
        int row = static_cast<int>(i);

        auto* sevItem = new QTableWidgetItem(
            issue.severity == compliance::IssueSeverity::Error ? tr("ERROR") : tr("WARN"));
        auto* ruleItem = new QTableWidgetItem(QString::fromStdString(issue.rule));
        auto* descItem = new QTableWidgetItem(QString::fromStdString(issue.description));
        auto* pageItem = new QTableWidgetItem(
            issue.pageIndex >= 0 ? QString::number(issue.pageIndex + 1) : tr("Doc"));

        issueTable_->setItem(row, 0, sevItem);
        issueTable_->setItem(row, 1, ruleItem);
        issueTable_->setItem(row, 2, descItem);
        issueTable_->setItem(row, 3, pageItem);
    }
}

} // namespace pdfe::ui
