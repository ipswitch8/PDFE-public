#include "CombineFilesWizard.h"

#include <QApplication>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

CombineFilesWizard::CombineFilesWizard(
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    QWidget* parent)
    : QDialog(parent)
    , context_(std::move(context))
{
    setupUi();
    updateButtonsForStep();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

void CombineFilesWizard::setupUi() {
    setObjectName("combineFilesWizard");
    setAccessibleName("Combine Files Wizard");
    setWindowTitle(tr("Combine Files"));
    resize(640, 480);

    auto* root = new QVBoxLayout(this);

    steps_ = new QStackedWidget(this);
    steps_->setObjectName("combineSteps");

    // ---- Step 1: file list picker ----
    auto* step1 = new QWidget();
    step1->setObjectName("step1FileList");
    auto* step1Layout = new QVBoxLayout(step1);
    step1Layout->addWidget(
        new QLabel(tr("Select PDF and image files to combine. "
                       "Drag to reorder."), step1));
    fileList_ = new QListWidget(step1);
    fileList_->setObjectName("fileList");
    // GitHub #185: spoken text, not the automation id (objectName above is
    // the id). The f3c91a2e rationale that the UIA bridge reads
    // AutomationId from accessibleName was a misdiagnosis — see the note in
    // BatchDialog.cpp.
    fileList_->setAccessibleName(tr("Files to combine"));
    fileList_->setSelectionMode(QAbstractItemView::SingleSelection);
    step1Layout->addWidget(fileList_, 1);
    auto* btnRow = new QHBoxLayout();
    addButton_ = new QPushButton(tr("Add..."), step1);
    addButton_->setObjectName("addFilesButton");
    addButton_->setAccessibleName("Add Files To Combine");
    removeButton_ = new QPushButton(tr("Remove"), step1);
    removeButton_->setObjectName("removeFileButton");
    removeButton_->setAccessibleName("Remove Selected File");
    moveUpButton_ = new QPushButton(tr("Move Up"), step1);
    moveUpButton_->setObjectName("moveUpButton");
    moveUpButton_->setAccessibleName("Move Selected File Up");
    moveDownButton_ = new QPushButton(tr("Move Down"), step1);
    moveDownButton_->setObjectName("moveDownButton");
    moveDownButton_->setAccessibleName("Move Selected File Down");
    btnRow->addWidget(addButton_);
    btnRow->addWidget(removeButton_);
    btnRow->addStretch();
    btnRow->addWidget(moveUpButton_);
    btnRow->addWidget(moveDownButton_);
    step1Layout->addLayout(btnRow);
    steps_->addWidget(step1);

    // ---- Step 2: output path picker ----
    auto* step2 = new QWidget();
    step2->setObjectName("step2OutputPath");
    auto* step2Layout = new QVBoxLayout(step2);
    step2Layout->addWidget(
        new QLabel(tr("Choose where to save the combined PDF:"), step2));
    auto* outputRow = new QHBoxLayout();
    outputEdit_ = new QLineEdit(step2);
    outputEdit_->setObjectName("outputPathEdit");
    outputEdit_->setAccessibleName("Output PDF Path");
    browseOutputButton_ = new QPushButton(tr("Browse..."), step2);
    browseOutputButton_->setObjectName("browseOutputButton");
    browseOutputButton_->setAccessibleName("Browse For Output PDF");
    outputRow->addWidget(outputEdit_, 1);
    outputRow->addWidget(browseOutputButton_);
    step2Layout->addLayout(outputRow);
    step2Layout->addStretch();
    steps_->addWidget(step2);

    // ---- Step 3: progress + done ----
    auto* step3 = new QWidget();
    step3->setObjectName("step3Progress");
    auto* step3Layout = new QVBoxLayout(step3);
    statusLabel_ = new QLabel(tr("Ready to combine."), step3);
    statusLabel_->setObjectName("combineStatusLabel");
    step3Layout->addWidget(statusLabel_);
    progressBar_ = new QProgressBar(step3);
    progressBar_->setObjectName("combineProgressBar");
    progressBar_->setAccessibleName("Combine Progress");
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    step3Layout->addWidget(progressBar_);
    step3Layout->addStretch();
    steps_->addWidget(step3);

    root->addWidget(steps_, 1);

    // Footer buttons
    auto* footer = new QHBoxLayout();
    backButton_ = new QPushButton(tr("< Back"), this);
    backButton_->setObjectName("backButton");
    backButton_->setAccessibleName("Wizard Back");
    nextButton_ = new QPushButton(tr("Next >"), this);
    nextButton_->setObjectName("nextButton");
    nextButton_->setAccessibleName("Wizard Next");
    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setObjectName("cancelButton");
    cancelButton_->setAccessibleName("Cancel Wizard");
    footer->addStretch();
    footer->addWidget(backButton_);
    footer->addWidget(nextButton_);
    footer->addWidget(cancelButton_);
    root->addLayout(footer);

    // QAbstractItemView (QListWidget) uses Tab for internal item navigation
    // by default — disable so Tab advances to the next dialog widget.
    fileList_->setTabKeyNavigation(false);

    // Ensure all interactive widgets have tab-friendly focus policies.
    fileList_->setFocusPolicy(Qt::StrongFocus);
    addButton_->setFocusPolicy(Qt::StrongFocus);
    removeButton_->setFocusPolicy(Qt::StrongFocus);
    moveUpButton_->setFocusPolicy(Qt::StrongFocus);
    moveDownButton_->setFocusPolicy(Qt::StrongFocus);
    cancelButton_->setFocusPolicy(Qt::StrongFocus);
    nextButton_->setFocusPolicy(Qt::StrongFocus);
    backButton_->setFocusPolicy(Qt::StrongFocus);

    // Explicit tab order for step 1: list → add → remove → moveUp → moveDown
    // → next → cancel → (cycle back).  back/next are in the footer (direct
    // children of the dialog), while the list and buttons are in step1 (inside
    // the QStackedWidget).  setTabOrder crosses container boundaries within the
    // same top-level window.
    setTabOrder(fileList_, addButton_);
    setTabOrder(addButton_, removeButton_);
    setTabOrder(removeButton_, moveUpButton_);
    setTabOrder(moveUpButton_, moveDownButton_);
    setTabOrder(moveDownButton_, backButton_);
    setTabOrder(backButton_, nextButton_);
    setTabOrder(nextButton_, cancelButton_);
    setTabOrder(cancelButton_, fileList_);

    connect(addButton_,    &QPushButton::clicked, this, &CombineFilesWizard::onAddFiles);
    connect(removeButton_, &QPushButton::clicked, this, &CombineFilesWizard::onRemoveFile);
    connect(moveUpButton_, &QPushButton::clicked, this, &CombineFilesWizard::onMoveUp);
    connect(moveDownButton_, &QPushButton::clicked, this, &CombineFilesWizard::onMoveDown);
    connect(browseOutputButton_, &QPushButton::clicked, this, &CombineFilesWizard::onChooseOutput);
    connect(nextButton_, &QPushButton::clicked, this, &CombineFilesWizard::onNext);
    connect(backButton_, &QPushButton::clicked, this, &CombineFilesWizard::onBack);
    connect(cancelButton_, &QPushButton::clicked, this, &CombineFilesWizard::onCancel);
}

void CombineFilesWizard::setInputFiles(const std::vector<std::filesystem::path>& inputs) {
    fileList_->clear();
    for (const auto& p : inputs) {
        auto* it = new QListWidgetItem(QString::fromStdString(p.string()));
        it->setData(Qt::UserRole, QString::fromStdString(p.string()));
        fileList_->addItem(it);
    }
    updateButtonsForStep();
}

bool CombineFilesWizard::addFileByPath(const QString& path) {
    if (path.isEmpty()) return false;
    auto* it = new QListWidgetItem(path);
    it->setData(Qt::UserRole, path);
    fileList_->addItem(it);
    updateButtonsForStep();
    return true;
}

void CombineFilesWizard::setOutputPath(const std::filesystem::path& output) {
    outputEdit_->setText(QString::fromStdString(output.string()));
    updateButtonsForStep();
}

std::vector<std::filesystem::path> CombineFilesWizard::inputFiles() const {
    std::vector<std::filesystem::path> out;
    for (int i = 0; i < fileList_->count(); ++i) {
        out.push_back(std::filesystem::path(
            fileList_->item(i)->data(Qt::UserRole).toString().toStdString()));
    }
    return out;
}

std::filesystem::path CombineFilesWizard::outputPath() const {
    return std::filesystem::path(outputEdit_->text().toStdString());
}

int CombineFilesWizard::currentStepIndex() const {
    return steps_ ? steps_->currentIndex() : 0;
}

void CombineFilesWizard::onAddFiles() {
    QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Add files to combine"), QString(),
        tr("PDF and Images (*.pdf *.png *.jpg *.jpeg *.tif *.tiff *.bmp);;"
           "All files (*)"));
    for (const QString& f : files) {
        auto* it = new QListWidgetItem(f);
        it->setData(Qt::UserRole, f);
        fileList_->addItem(it);
    }
    updateButtonsForStep();
}

void CombineFilesWizard::onRemoveFile() {
    int row = fileList_->currentRow();
    if (row < 0) return;
    delete fileList_->takeItem(row);
    updateButtonsForStep();
}

void CombineFilesWizard::onMoveUp() {
    int row = fileList_->currentRow();
    if (row <= 0) return;
    auto* it = fileList_->takeItem(row);
    fileList_->insertItem(row - 1, it);
    fileList_->setCurrentRow(row - 1);
}

void CombineFilesWizard::onMoveDown() {
    int row = fileList_->currentRow();
    if (row < 0 || row >= fileList_->count() - 1) return;
    auto* it = fileList_->takeItem(row);
    fileList_->insertItem(row + 1, it);
    fileList_->setCurrentRow(row + 1);
}

void CombineFilesWizard::onChooseOutput() {
    QString f = QFileDialog::getSaveFileName(
        this, tr("Save combined PDF as..."),
        outputEdit_->text(), tr("PDF (*.pdf)"));
    if (!f.isEmpty()) outputEdit_->setText(f);
    updateButtonsForStep();
}

bool CombineFilesWizard::canAdvanceFromStep(int step) const {
    switch (step) {
    case 0: return fileList_->count() >= 2;
    case 1: return !outputEdit_->text().trimmed().isEmpty();
    case 2: return false; // last step; "Done" closes via accept()
    default: return false;
    }
}

void CombineFilesWizard::updateButtonsForStep() {
    if (!steps_) return;
    const int step = steps_->currentIndex();
    backButton_->setEnabled(step > 0 && step < 2);
    if (step == 2) {
        nextButton_->setText(tr("Done"));
        nextButton_->setEnabled(true);
    } else {
        nextButton_->setText(tr("Next >"));
        nextButton_->setEnabled(canAdvanceFromStep(step));
    }
}

void CombineFilesWizard::onNext() {
    const int step = steps_->currentIndex();
    if (step == 0) {
        if (!canAdvanceFromStep(0)) return;
        steps_->setCurrentIndex(1);
        updateButtonsForStep();
        return;
    }
    if (step == 1) {
        if (!canAdvanceFromStep(1)) return;
        steps_->setCurrentIndex(2);
        updateButtonsForStep();
        // Run the combine right after entering step 3.
        runCombine();
        return;
    }
    if (step == 2) {
        accept();
    }
}

void CombineFilesWizard::onBack() {
    int step = steps_->currentIndex();
    if (step > 0) {
        steps_->setCurrentIndex(step - 1);
        updateButtonsForStep();
    }
}

void CombineFilesWizard::onCancel() {
    reject();
}

CombineFilesService::Result CombineFilesWizard::runCombine() {
    auto inputs = inputFiles();
    auto out = outputPath();
    statusLabel_->setText(tr("Combining %1 files...").arg(inputs.size()));
    progressBar_->setRange(0, static_cast<int>(inputs.size()));
    progressBar_->setValue(0);

    CombineFilesService svc(context_);
    auto result = svc.combine(inputs, out,
        [this](int idx, int total) {
            if (progressBar_) progressBar_->setValue(idx);
            if (statusLabel_) {
                statusLabel_->setText(
                    tr("Processed %1 of %2 files").arg(idx).arg(total));
            }
            // Pump the event loop so the progress bar / status label
            // actually repaint; the merge runs synchronously on the
            // main thread, so without this the dialog appears frozen.
            QApplication::processEvents();
        });

    if (!result.ok) {
        // Show the translated, user-named message. errorMessage is the
        // untranslated diagnostic (full paths, and for image inputs the
        // pdfe_combine_img.<hex> temp wrapper rather than the file the user
        // picked) and is kept for logs and tests only. Fall back to it if a
        // future failure path forgets to set userMessage — a raw diagnostic
        // beats a blank dialog.
        const QString shown = result.userMessage.isEmpty()
            ? QString::fromStdString(result.errorMessage)
            : result.userMessage;
        // No "Combine failed: " prefix here — the message is already a whole
        // sentence, and the exception path's own text starts with those very
        // words, which the old prefix duplicated ("Combine failed: Combine
        // failed: ...").
        statusLabel_->setText(shown);
        QMessageBox::warning(this, tr("Combine Files"), shown);
    } else {
        statusLabel_->setText(
            tr("Done. Output has %1 pages.").arg(result.outputPageCount));
    }
    return result;
}

} // namespace pdfe::ui
