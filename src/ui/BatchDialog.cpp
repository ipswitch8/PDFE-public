#include "BatchDialog.h"
#include "batch/BatchEngine.h"
#include "core/MuPdfContext.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include <QProgressBar>
#include <QTextEdit>
#include <QLabel>
#include <QStackedWidget>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QApplication>
#include <QThread>
#include <QDir>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

BatchDialog::BatchDialog(
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    QWidget* parent)
    : QDialog(parent)
    , context_(std::move(context))
{
    setupUi();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

BatchDialog::~BatchDialog() = default;

void BatchDialog::setupUi() {
    setWindowTitle(tr("Batch Processing"));
    setMinimumSize(700, 550);
    setObjectName("batchDialog");
    setAccessibleName("Batch Processing");

    auto* mainLayout = new QVBoxLayout(this);

    // Input folder selection
    auto* folderGroup = new QGroupBox(tr("Input"), this);
    folderGroup->setObjectName("inputGroup");
    auto* folderLayout = new QHBoxLayout(folderGroup);
    folderLabel_ = new QLabel(tr("No folder selected"), folderGroup);
    folderLabel_->setObjectName("folderLabel");
    folderLabel_->setAccessibleName("Selected Folder");
    selectFolderButton_ = new QPushButton(tr("Select Folder..."), folderGroup);
    selectFolderButton_->setObjectName("selectFolderButton");
    selectFolderButton_->setAccessibleName("Select Input Folder");
    folderLayout->addWidget(folderLabel_, 1);
    folderLayout->addWidget(selectFolderButton_);
    mainLayout->addWidget(folderGroup);

    // Action builder
    auto* actionGroup = new QGroupBox(tr("Action Sequence"), this);
    actionGroup->setObjectName("actionGroup");
    auto* actionMainLayout = new QHBoxLayout(actionGroup);

    // Left: action list
    auto* listLayout = new QVBoxLayout();
    actionListWidget_ = new QListWidget(actionGroup);
    actionListWidget_->setObjectName("actionListWidget");
    // accessibleName is the human-readable UIA Name property — screen
    // readers announce it, so it must stay human-readable. C++ tests
    // locate this widget by objectName (findChild); pywinauto UI tests
    // locate it by this accessible Name. (Pipeline f3c91a2e Cluster #1B
    // wrongly set this to "actionListWidget" to match objectName, which
    // broke BatchDialogTest.ActionListWidgetExists and degraded
    // screen-reader output. The 2 failures rode green for 3 phases behind
    // a CI exit-code masking bug; both fixed in Phase 6. See
    // tests/docs/known_issues.md GAP-A15.)
    actionListWidget_->setAccessibleName("Action Sequence List");
    listLayout->addWidget(actionListWidget_);

    auto* listBtnLayout = new QHBoxLayout();
    addActionButton_ = new QPushButton(tr("Add"), actionGroup);
    addActionButton_->setObjectName("addActionButton");
    addActionButton_->setAccessibleName("Add Action");
    removeActionButton_ = new QPushButton(tr("Remove"), actionGroup);
    removeActionButton_->setObjectName("removeActionButton");
    removeActionButton_->setAccessibleName("Remove Action");
    moveUpButton_ = new QPushButton(tr("Up"), actionGroup);
    moveUpButton_->setObjectName("moveUpButton");
    moveUpButton_->setAccessibleName("Move Action Up");
    moveDownButton_ = new QPushButton(tr("Down"), actionGroup);
    moveDownButton_->setObjectName("moveDownButton");
    moveDownButton_->setAccessibleName("Move Action Down");
    listBtnLayout->addWidget(addActionButton_);
    listBtnLayout->addWidget(removeActionButton_);
    listBtnLayout->addWidget(moveUpButton_);
    listBtnLayout->addWidget(moveDownButton_);
    listLayout->addLayout(listBtnLayout);
    actionMainLayout->addLayout(listLayout);

    // Right: action config
    auto* configLayout = new QVBoxLayout();
    actionTypeCombo_ = new QComboBox(actionGroup);
    actionTypeCombo_->setObjectName("actionTypeCombo");
    actionTypeCombo_->setAccessibleName("Action Type");
    actionTypeCombo_->addItem(tr("Add Watermark"), static_cast<int>(batch::BatchActionType::AddWatermark));
    actionTypeCombo_->addItem(tr("Flatten Annotations"), static_cast<int>(batch::BatchActionType::FlattenAnnotations));
    actionTypeCombo_->addItem(tr("Convert to PDF/A"), static_cast<int>(batch::BatchActionType::ConvertToPdfA));
    actionTypeCombo_->addItem(tr("Set Permissions"), static_cast<int>(batch::BatchActionType::SetPermissions));
    configLayout->addWidget(actionTypeCombo_);

    configStack_ = new QStackedWidget(actionGroup);
    configStack_->setObjectName("configStack");

    // Page 0: Watermark config
    auto* watermarkPanel = new QWidget();
    auto* wmLayout = new QVBoxLayout(watermarkPanel);
    watermarkTextEdit_ = new QLineEdit(watermarkPanel);
    watermarkTextEdit_->setObjectName("watermarkTextEdit");
    watermarkTextEdit_->setAccessibleName("Watermark Text");
    watermarkTextEdit_->setText("DRAFT");
    watermarkTextEdit_->setPlaceholderText("Watermark text...");
    wmLayout->addWidget(new QLabel(tr("Text:"), watermarkPanel));
    wmLayout->addWidget(watermarkTextEdit_);

    watermarkOpacitySpin_ = new QDoubleSpinBox(watermarkPanel);
    watermarkOpacitySpin_->setObjectName("watermarkOpacitySpin");
    watermarkOpacitySpin_->setAccessibleName("Watermark Opacity");
    watermarkOpacitySpin_->setRange(0.05, 1.0);
    watermarkOpacitySpin_->setSingleStep(0.05);
    watermarkOpacitySpin_->setValue(0.3);
    wmLayout->addWidget(new QLabel(tr("Opacity:"), watermarkPanel));
    wmLayout->addWidget(watermarkOpacitySpin_);

    watermarkPositionCombo_ = new QComboBox(watermarkPanel);
    watermarkPositionCombo_->setObjectName("watermarkPositionCombo");
    watermarkPositionCombo_->setAccessibleName("Watermark Position");
    watermarkPositionCombo_->addItem(tr("Center"), "center");
    watermarkPositionCombo_->addItem(tr("Top Left"), "top-left");
    watermarkPositionCombo_->addItem(tr("Top Right"), "top-right");
    watermarkPositionCombo_->addItem(tr("Bottom Left"), "bottom-left");
    watermarkPositionCombo_->addItem(tr("Bottom Right"), "bottom-right");
    wmLayout->addWidget(new QLabel(tr("Position:"), watermarkPanel));
    wmLayout->addWidget(watermarkPositionCombo_);
    wmLayout->addStretch();
    configStack_->addWidget(watermarkPanel);

    // Page 1: Flatten (no config)
    auto* flattenPanel = new QWidget();
    auto* flatLayout = new QVBoxLayout(flattenPanel);
    flatLayout->addWidget(new QLabel(tr("Flattens all annotations into page content."), flattenPanel));
    flatLayout->addStretch();
    configStack_->addWidget(flattenPanel);

    // Page 2: PDF/A config
    auto* pdfaPanel = new QWidget();
    auto* pdfaLayout = new QVBoxLayout(pdfaPanel);
    pdfaLevelCombo_ = new QComboBox(pdfaPanel);
    pdfaLevelCombo_->setObjectName("pdfaLevelCombo");
    pdfaLevelCombo_->setAccessibleName("PDF/A Level");
    pdfaLevelCombo_->addItem(tr("PDF/A-1b"), "1b");
    pdfaLevelCombo_->addItem(tr("PDF/A-2b"), "2b");
    pdfaLevelCombo_->addItem(tr("PDF/A-2u"), "2u");
    pdfaLevelCombo_->addItem(tr("PDF/A-2a"), "2a");
    pdfaLevelCombo_->addItem(tr("PDF/A-3b"), "3b");
    pdfaLayout->addWidget(new QLabel(tr("Target Level:"), pdfaPanel));
    pdfaLayout->addWidget(pdfaLevelCombo_);
    pdfaLayout->addStretch();
    configStack_->addWidget(pdfaPanel);

    // Page 3: Permissions config
    auto* permPanel = new QWidget();
    auto* permLayout = new QVBoxLayout(permPanel);
    permPrintCheck_ = new QCheckBox(tr("Allow Printing"), permPanel);
    permPrintCheck_->setObjectName("permPrintCheck");
    permPrintCheck_->setAccessibleName("Allow Printing");
    permPrintCheck_->setChecked(true);
    permCopyCheck_ = new QCheckBox(tr("Allow Copying"), permPanel);
    permCopyCheck_->setObjectName("permCopyCheck");
    permCopyCheck_->setAccessibleName("Allow Copying");
    permEditCheck_ = new QCheckBox(tr("Allow Editing"), permPanel);
    permEditCheck_->setObjectName("permEditCheck");
    permEditCheck_->setAccessibleName("Allow Editing");
    ownerPasswordEdit_ = new QLineEdit(permPanel);
    ownerPasswordEdit_->setObjectName("ownerPasswordEdit");
    ownerPasswordEdit_->setAccessibleName("Owner Password");
    ownerPasswordEdit_->setEchoMode(QLineEdit::Password);
    ownerPasswordEdit_->setPlaceholderText("Owner password...");
    permLayout->addWidget(permPrintCheck_);
    permLayout->addWidget(permCopyCheck_);
    permLayout->addWidget(permEditCheck_);
    permLayout->addWidget(new QLabel(tr("Owner Password:"), permPanel));
    permLayout->addWidget(ownerPasswordEdit_);
    permLayout->addStretch();
    configStack_->addWidget(permPanel);

    configLayout->addWidget(configStack_);
    actionMainLayout->addLayout(configLayout);
    mainLayout->addWidget(actionGroup);

    // Execution controls
    auto* execLayout = new QHBoxLayout();
    runButton_ = new QPushButton(tr("Run Batch"), this);
    runButton_->setObjectName("runButton");
    runButton_->setAccessibleName("Run Batch");
    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setObjectName("cancelButton");
    cancelButton_->setAccessibleName("Cancel Batch");
    cancelButton_->setEnabled(false);
    execLayout->addStretch();
    execLayout->addWidget(runButton_);
    execLayout->addWidget(cancelButton_);
    mainLayout->addLayout(execLayout);

    // Progress
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName("batchProgressBar");
    // GitHub #185: accessibleName is what a screen reader SPEAKS, so it
    // carries the user-facing label; objectName above is the automation id
    // and is what Qt's UIA bridge exposes as AutomationId (pywinauto
    // `auto_id`). The earlier f3c91a2e comment here claimed the bridge took
    // AutomationId from accessibleName and aligned the two for that reason —
    // that was a misdiagnosis, disproved by the live-UI sites that already
    // differ (`auto_id="AnalyzeButton"` resolves while its accessibleName is
    // "Analyze") and re-measured by
    // test_ui_accessible_names.py::test_accessible_names_are_user_facing.
    // Leave the bar visible (at 0%) so UIA enumeration sees it — hidden
    // widgets are filtered out by Windows UIA on this RDP CI host.
    progressBar_->setAccessibleName(tr("Batch progress"));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    mainLayout->addWidget(progressBar_);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("batchStatusLabel");
    statusLabel_->setAccessibleName("Batch Status");
    mainLayout->addWidget(statusLabel_);

    // Error log
    errorLog_ = new QTextEdit(this);
    errorLog_->setObjectName("batchErrorLog");
    // Human-readable UIA Name (see actionListWidget_ above). Leave the
    // widget visible (empty) so UIA picks it up — hidden widgets are
    // filtered out by Windows UIA on this RDP CI host. Cluster #1B wrongly
    // set this to "batchErrorLog"; reverted in Phase 6 (GAP-A15).
    errorLog_->setAccessibleName("Batch Error Log");
    errorLog_->setReadOnly(true);
    errorLog_->setMaximumHeight(120);
    errorLog_->setPlaceholderText(tr("Batch errors will appear here."));
    mainLayout->addWidget(errorLog_);

    // QTextEdit consumes Tab for indentation — enable Tab-to-next-control.
    errorLog_->setTabChangesFocus(true);

    // QAbstractItemView (QListWidget) uses Tab for internal item navigation
    // by default — disable so Tab advances to the next dialog widget.
    actionListWidget_->setTabKeyNavigation(false);

    // Ensure all interactive widgets have tab-friendly focus policies.
    selectFolderButton_->setFocusPolicy(Qt::StrongFocus);
    actionListWidget_->setFocusPolicy(Qt::StrongFocus);
    addActionButton_->setFocusPolicy(Qt::StrongFocus);
    removeActionButton_->setFocusPolicy(Qt::StrongFocus);
    moveUpButton_->setFocusPolicy(Qt::StrongFocus);
    moveDownButton_->setFocusPolicy(Qt::StrongFocus);
    actionTypeCombo_->setFocusPolicy(Qt::StrongFocus);
    runButton_->setFocusPolicy(Qt::StrongFocus);
    cancelButton_->setFocusPolicy(Qt::StrongFocus);

    // Explicit tab order: type combo → action list → list buttons →
    // run/cancel → (cycle back).  selectFolderButton_ is at the top;
    // start the chain from it.
    setTabOrder(selectFolderButton_, actionTypeCombo_);
    setTabOrder(actionTypeCombo_, actionListWidget_);
    setTabOrder(actionListWidget_, addActionButton_);
    setTabOrder(addActionButton_, removeActionButton_);
    setTabOrder(removeActionButton_, moveUpButton_);
    setTabOrder(moveUpButton_, moveDownButton_);
    setTabOrder(moveDownButton_, runButton_);
    setTabOrder(runButton_, cancelButton_);
    setTabOrder(cancelButton_, selectFolderButton_);

    // Connections
    connect(selectFolderButton_, &QPushButton::clicked, this, &BatchDialog::onSelectFolder);
    connect(addActionButton_, &QPushButton::clicked, this, &BatchDialog::onAddAction);
    connect(removeActionButton_, &QPushButton::clicked, this, &BatchDialog::onRemoveAction);
    connect(moveUpButton_, &QPushButton::clicked, this, &BatchDialog::onMoveUp);
    connect(moveDownButton_, &QPushButton::clicked, this, &BatchDialog::onMoveDown);
    connect(runButton_, &QPushButton::clicked, this, &BatchDialog::onRun);
    connect(cancelButton_, &QPushButton::clicked, this, &BatchDialog::onCancel);
    connect(actionTypeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BatchDialog::onActionTypeChanged);
}

void BatchDialog::onSelectFolder() {
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Input Folder"));
    if (dir.isEmpty()) return;

    inputFiles_.clear();
    QDir qdir(dir);
    for (const auto& entry : qdir.entryInfoList({"*.pdf"}, QDir::Files)) {
        inputFiles_.push_back(entry.absoluteFilePath().toStdString());
    }

    folderLabel_->setText(tr("%1 (%2 PDF files)")
        .arg(dir).arg(inputFiles_.size()));
}

void BatchDialog::onAddAction() {
    auto action = buildActionFromUi();
    actions_.push_back(action);
    updateActionList();
}

void BatchDialog::onRemoveAction() {
    int row = actionListWidget_->currentRow();
    if (row >= 0 && row < static_cast<int>(actions_.size())) {
        actions_.erase(actions_.begin() + row);
        updateActionList();
    }
}

void BatchDialog::onMoveUp() {
    int row = actionListWidget_->currentRow();
    if (row > 0 && row < static_cast<int>(actions_.size())) {
        std::swap(actions_[row], actions_[row - 1]);
        updateActionList();
        actionListWidget_->setCurrentRow(row - 1);
    }
}

void BatchDialog::onMoveDown() {
    int row = actionListWidget_->currentRow();
    if (row >= 0 && row < static_cast<int>(actions_.size()) - 1) {
        std::swap(actions_[row], actions_[row + 1]);
        updateActionList();
        actionListWidget_->setCurrentRow(row + 1);
    }
}

void BatchDialog::onActionTypeChanged(int index) {
    configStack_->setCurrentIndex(index);
}

void BatchDialog::onRun() {
    if (inputFiles_.empty()) {
        statusLabel_->setText(tr("No input files selected"));
        return;
    }
    if (actions_.empty()) {
        statusLabel_->setText(tr("No actions configured"));
        return;
    }

    running_ = true;
    runButton_->setEnabled(false);
    cancelButton_->setEnabled(true);
    progressBar_->setVisible(true);
    progressBar_->setRange(0, static_cast<int>(inputFiles_.size()));
    progressBar_->setValue(0);
    errorLog_->clear();
    errorLog_->setVisible(true);

    batch::BatchEngine engine(context_);
    activeEngine_ = &engine;

    engine.setProgressCallback([this](int idx, int total, const std::string& name) {
        progressBar_->setValue(idx);
        statusLabel_->setText(tr("Processing file %1 of %2: %3")
            .arg(idx + 1).arg(total).arg(QString::fromStdString(name)));
        QApplication::processEvents();
    });

    engine.setErrorCallback([this](int /*idx*/, const std::string& name, const std::string& error) {
        errorLog_->append(QString("%1: %2")
            .arg(QString::fromStdString(name))
            .arg(QString::fromStdString(error)));
    });

    auto result = engine.process(inputFiles_, actions_);
    activeEngine_ = nullptr;

    progressBar_->setValue(result.totalFiles);
    running_ = false;
    runButton_->setEnabled(true);
    cancelButton_->setEnabled(false);

    if (result.cancelled) {
        statusLabel_->setText(tr("Batch cancelled after %1 of %2 files")
            .arg(result.successCount + result.errorCount).arg(result.totalFiles));
    } else {
        statusLabel_->setText(tr("Batch complete: %1 succeeded, %2 errors out of %3 files")
            .arg(result.successCount).arg(result.errorCount).arg(result.totalFiles));
    }
}

void BatchDialog::onCancel() {
    if (activeEngine_) {
        activeEngine_->cancel();
    }
    statusLabel_->setText(tr("Cancelling..."));
}

void BatchDialog::updateActionList() {
    actionListWidget_->clear();
    for (const auto& action : actions_) {
        QString name;
        switch (action.type) {
        case batch::BatchActionType::AddWatermark:
            name = tr("Add Watermark");
            if (auto* cfg = std::get_if<batch::WatermarkConfig>(&action.config))
                name += " (" + QString::fromStdString(cfg->text) + ")";
            break;
        case batch::BatchActionType::FlattenAnnotations:
            name = tr("Flatten Annotations");
            break;
        case batch::BatchActionType::ConvertToPdfA:
            name = tr("Convert to PDF/A");
            if (auto* cfg = std::get_if<batch::PdfAConfig>(&action.config))
                name += QString(" (%1%2)").arg(cfg->level).arg(QChar(cfg->conformance));
            break;
        case batch::BatchActionType::SetPermissions:
            name = tr("Set Permissions");
            break;
        }
        actionListWidget_->addItem(name);
    }
}

batch::BatchAction BatchDialog::buildActionFromUi() const {
    batch::BatchAction action;
    int idx = actionTypeCombo_->currentIndex();
    action.type = static_cast<batch::BatchActionType>(
        actionTypeCombo_->currentData().toInt());

    switch (action.type) {
    case batch::BatchActionType::AddWatermark: {
        batch::WatermarkConfig cfg;
        cfg.text = watermarkTextEdit_->text().toStdString();
        cfg.opacity = static_cast<float>(watermarkOpacitySpin_->value());
        cfg.position = watermarkPositionCombo_->currentData().toString().toStdString();
        action.config = cfg;
        break;
    }
    case batch::BatchActionType::FlattenAnnotations:
        action.config = std::monostate{};
        break;
    case batch::BatchActionType::ConvertToPdfA: {
        batch::PdfAConfig cfg;
        QString levelStr = pdfaLevelCombo_->currentData().toString();
        cfg.level = levelStr.left(1).toInt();
        cfg.conformance = levelStr.right(1).toLatin1()[0];
        action.config = cfg;
        break;
    }
    case batch::BatchActionType::SetPermissions: {
        batch::PermissionsConfig cfg;
        cfg.allowPrint = permPrintCheck_->isChecked();
        cfg.allowCopy = permCopyCheck_->isChecked();
        cfg.allowEdit = permEditCheck_->isChecked();
        cfg.ownerPassword = ownerPasswordEdit_->text().toStdString();
        action.config = cfg;
        break;
    }
    }

    return action;
}

} // namespace pdfe::ui
