#include "OptimizeDialog.h"
#include "optimize/OptimizeEngine.h"
#include "optimize/OptimizeTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>

#include <filesystem>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

OptimizeDialog::OptimizeDialog(
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    pdfe::core::Document* document,
    QWidget* parent)
    : QDialog(parent)
    , context_(std::move(context))
    , document_(document)
{
    setupUi();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

OptimizeDialog::~OptimizeDialog() = default;

void OptimizeDialog::setupUi()
{
    setWindowTitle(tr("Optimize PDF"));
    setObjectName("optimizeDialog");
    setAccessibleName(tr("Optimize PDF Dialog"));
    setMinimumSize(440, 380);

    auto* mainLayout = new QVBoxLayout(this);

    auto* optionsBox = new QGroupBox(tr("Optimizations"), this);
    optionsBox->setObjectName("optimizeOptionsGroup");
    auto* optLayout = new QVBoxLayout(optionsBox);

    auto mkCheck = [&](const QString& label, const QString& name, bool initial) {
        auto* c = new QCheckBox(label, optionsBox);
        c->setObjectName(name);
        c->setAccessibleName(label);
        c->setChecked(initial);
        optLayout->addWidget(c);
        return c;
    };

    compressStreamsBox_  = mkCheck(tr("Compress content streams"),
                                   "optimizeCompressStreams", true);
    recompressImagesBox_ = mkCheck(tr("Re-compress images"),
                                   "optimizeRecompressImages", true);
    subsetFontsBox_      = mkCheck(tr("Subset embedded fonts"),
                                   "optimizeSubsetFonts", true);
    removeUnusedBox_     = mkCheck(tr("Remove unused objects"),
                                   "optimizeRemoveUnused", true);
    mergeDuplicatesBox_  = mkCheck(tr("Merge duplicate objects"),
                                   "optimizeMergeDuplicates", true);
    sanitizeBox_         = mkCheck(tr("Sanitize content streams (lossy)"),
                                   "optimizeSanitize", false);
    linearizeBox_        = mkCheck(tr("Linearize (Fast Web View)"),
                                   "optimizeLinearize", false);

    mainLayout->addWidget(optionsBox);

    resultLabel_ = new QLabel(this);
    resultLabel_->setObjectName("optimizeResultLabel");
    resultLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(resultLabel_);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    runButton_ = new QPushButton(tr("Optimize..."), this);
    runButton_->setObjectName("optimizeRunButton");
    runButton_->setAccessibleName(tr("Run Optimization"));
    runButton_->setDefault(true);
    cancelButton_ = new QPushButton(tr("Close"), this);
    cancelButton_->setObjectName("optimizeCancelButton");
    cancelButton_->setAccessibleName(tr("Close"));
    btnRow->addWidget(runButton_);
    btnRow->addWidget(cancelButton_);
    mainLayout->addLayout(btnRow);

    connect(runButton_,    &QPushButton::clicked, this, &OptimizeDialog::onRun);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
}

void OptimizeDialog::onRun()
{
    if (!document_ || !context_) return;
    const auto input = document_->filePath();
    if (input.empty()) {
        QMessageBox::warning(this, tr("No File Path"),
            tr("The current document has no saved file path. Save it first."));
        return;
    }

    const QString defaultOut = QString::fromStdString(
        (input.parent_path() /
         (input.stem().string() + "_optimized" + input.extension().string())).string());
    const QString outPath = QFileDialog::getSaveFileName(
        this, tr("Save Optimized PDF"), defaultOut, tr("PDF Files (*.pdf)"));
    if (outPath.isEmpty()) return;

    optimize::OptimizeOptions opts;
    opts.compressStreams  = compressStreamsBox_->isChecked();
    opts.recompressImages = recompressImagesBox_->isChecked();
    opts.subsetFonts      = subsetFontsBox_->isChecked();
    opts.removeUnused     = removeUnusedBox_->isChecked();
    opts.mergeDuplicates  = mergeDuplicatesBox_->isChecked();
    opts.sanitizeStreams  = sanitizeBox_->isChecked();
    opts.linearize        = linearizeBox_->isChecked();

    optimize::OptimizeEngine engine(context_);
    auto result = engine.run(input,
        std::filesystem::path(outPath.toStdString()), opts);

    if (!result.success()) {
        QMessageBox::critical(this, tr("Optimize Failed"),
            tr("Failed to optimize: %1")
              .arg(QString::fromStdString(result.errorMessage)));
        return;
    }

    const QString summary = tr("Before: %1 bytes\nAfter: %2 bytes\nSaved: %3 bytes (%4%)")
        .arg(result.sizeBeforeBytes)
        .arg(result.sizeAfterBytes)
        .arg(result.savedBytes())
        .arg(QString::number(result.savedPercent(), 'f', 2));
    resultLabel_->setText(summary);
    emit optimized(outPath);
}

} // namespace pdfe::ui
