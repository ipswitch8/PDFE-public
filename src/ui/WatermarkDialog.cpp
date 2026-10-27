#include "WatermarkDialog.h"
#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <algorithm>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

WatermarkDialog::WatermarkDialog(
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

WatermarkDialog::~WatermarkDialog() = default;

void WatermarkDialog::setupUi() {
    setWindowTitle(tr("Add Watermark"));
    setMinimumSize(460, 460);
    setObjectName("watermarkDialog");
    setAccessibleName("Watermark Dialog");

    auto* mainLayout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    // Phase 4e: mode selector
    modeCombo_ = new QComboBox(this);
    modeCombo_->setObjectName("watermarkDialogModeCombo");
    modeCombo_->setAccessibleName("Watermark Mode");
    modeCombo_->addItem(tr("Text"),  "text");
    modeCombo_->addItem(tr("Image"), "image");
    form->addRow(tr("Mode:"), modeCombo_);

    textEdit_ = new QLineEdit(this);
    textEdit_->setObjectName("watermarkDialogTextEdit");
    textEdit_->setAccessibleName("Watermark Text");
    textEdit_->setText(tr("DRAFT"));
    form->addRow(tr("Text:"), textEdit_);

    // Phase 4e: image source row
    auto* imageRow = new QHBoxLayout();
    imagePathEdit_ = new QLineEdit(this);
    imagePathEdit_->setObjectName("watermarkDialogImagePath");
    imagePathEdit_->setAccessibleName("Watermark Image Path");
    imagePathEdit_->setPlaceholderText(tr("Path to PNG or JPG"));
    imagePathEdit_->setEnabled(false);
    imageBrowseButton_ = new QPushButton(tr("Browse..."), this);
    imageBrowseButton_->setObjectName("watermarkDialogImageBrowse");
    imageBrowseButton_->setAccessibleName("Browse for Watermark Image");
    imageBrowseButton_->setEnabled(false);
    imageRow->addWidget(imagePathEdit_);
    imageRow->addWidget(imageBrowseButton_);
    form->addRow(tr("Image:"), imageRow);

    opacitySpin_ = new QDoubleSpinBox(this);
    opacitySpin_->setObjectName("watermarkDialogOpacitySpin");
    opacitySpin_->setAccessibleName("Watermark Opacity");
    opacitySpin_->setRange(0.05, 1.0);
    opacitySpin_->setSingleStep(0.05);
    opacitySpin_->setValue(0.3);
    form->addRow(tr("Opacity:"), opacitySpin_);

    fontSizeSpin_ = new QDoubleSpinBox(this);
    fontSizeSpin_->setObjectName("watermarkDialogFontSizeSpin");
    fontSizeSpin_->setAccessibleName("Watermark Font Size");
    fontSizeSpin_->setRange(8.0, 144.0);
    fontSizeSpin_->setSingleStep(2.0);
    fontSizeSpin_->setValue(48.0);
    form->addRow(tr("Font size:"), fontSizeSpin_);

    rotationSpin_ = new QDoubleSpinBox(this);
    rotationSpin_->setObjectName("watermarkDialogRotationSpin");
    rotationSpin_->setAccessibleName("Watermark Rotation");
    rotationSpin_->setRange(-180.0, 180.0);
    rotationSpin_->setSingleStep(5.0);
    rotationSpin_->setValue(45.0);
    rotationSpin_->setSuffix(tr("°"));
    form->addRow(tr("Rotation:"), rotationSpin_);

    positionCombo_ = new QComboBox(this);
    positionCombo_->setObjectName("watermarkDialogPositionCombo");
    positionCombo_->setAccessibleName("Watermark Position");
    positionCombo_->addItem(tr("Center"), "center");
    positionCombo_->addItem(tr("Top Left"), "top-left");
    positionCombo_->addItem(tr("Top Right"), "top-right");
    positionCombo_->addItem(tr("Bottom Left"), "bottom-left");
    positionCombo_->addItem(tr("Bottom Right"), "bottom-right");
    form->addRow(tr("Position:"), positionCombo_);

    // Phase 4e: page-range controls
    rangeCombo_ = new QComboBox(this);
    rangeCombo_->setObjectName("watermarkDialogRangeCombo");
    rangeCombo_->setAccessibleName("Watermark Page Range");
    rangeCombo_->addItem(tr("All pages"),  "all");
    rangeCombo_->addItem(tr("Odd pages"),  "odd");
    rangeCombo_->addItem(tr("Even pages"), "even");
    rangeCombo_->addItem(tr("Custom..."),  "explicit");
    form->addRow(tr("Range:"), rangeCombo_);

    rangeExplicitEdit_ = new QLineEdit(this);
    rangeExplicitEdit_->setObjectName("watermarkDialogRangeExplicit");
    rangeExplicitEdit_->setAccessibleName("Watermark Explicit Range");
    rangeExplicitEdit_->setPlaceholderText(tr("e.g. 1,3,5-8"));
    rangeExplicitEdit_->setEnabled(false);
    form->addRow(tr("Pages:"), rangeExplicitEdit_);

    mainLayout->addLayout(form);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("watermarkDialogStatusLabel");
    mainLayout->addWidget(statusLabel_);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    applyButton_ = new QPushButton(tr("Apply..."), this);
    applyButton_->setObjectName("watermarkDialogApplyButton");
    applyButton_->setAccessibleName("Apply Watermark");
    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setObjectName("watermarkDialogCancelButton");
    cancelButton_->setAccessibleName("Cancel");
    btnLayout->addWidget(applyButton_);
    btnLayout->addWidget(cancelButton_);
    mainLayout->addLayout(btnLayout);

    connect(applyButton_, &QPushButton::clicked, this, &WatermarkDialog::onApply);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);

    // Phase 4e wiring
    connect(modeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int) {
            const bool image = modeCombo_->currentData().toString() == "image";
            imagePathEdit_->setEnabled(image);
            imageBrowseButton_->setEnabled(image);
            textEdit_->setEnabled(!image);
        });
    connect(rangeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int) {
            rangeExplicitEdit_->setEnabled(
                rangeCombo_->currentData().toString() == "explicit");
        });
    connect(imageBrowseButton_, &QPushButton::clicked, this, [this]() {
        const QString p = QFileDialog::getOpenFileName(this,
            tr("Choose Watermark Image"), QString(),
            tr("Images (*.png *.jpg *.jpeg)"));
        if (!p.isEmpty()) imagePathEdit_->setText(p);
    });
}

void WatermarkDialog::onApply() {
    if (!document_) {
        statusLabel_->setText(tr("No document loaded"));
        return;
    }

    QString outputPath = QFileDialog::getSaveFileName(
        this, tr("Save Watermarked PDF"), QString(), tr("PDF Files (*.pdf)"));
    if (outputPath.isEmpty()) return;

    pdfe::batch::WatermarkConfig cfg;
    cfg.text     = textEdit_->text().toStdString();
    cfg.opacity  = static_cast<float>(opacitySpin_->value());
    cfg.fontSize = static_cast<float>(fontSizeSpin_->value());
    cfg.rotation = static_cast<float>(rotationSpin_->value());
    cfg.position = positionCombo_->currentData().toString().toStdString();

    pdfe::batch::WatermarkExtConfig ext;
    ext.mode      = modeCombo_->currentData().toString().toStdString();
    ext.imagePath = imagePathEdit_->text().toStdString();
    ext.rangeMode = rangeCombo_->currentData().toString().toStdString();

    if (ext.mode == "image" && ext.imagePath.empty()) {
        QMessageBox::warning(this, tr("No Image"),
            tr("Please select an image file for the watermark."));
        return;
    }
    if (ext.rangeMode == "explicit") {
        const QString raw = rangeExplicitEdit_->text();
        const int total = document_->pageCount();
        const auto parts = raw.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
        for (const QString& part : parts) {
            if (part.contains('-')) {
                const auto bounds = part.split('-', Qt::SkipEmptyParts);
                if (bounds.size() != 2) continue;
                bool a_ok = false, b_ok = false;
                int a = bounds[0].toInt(&a_ok);
                int b = bounds[1].toInt(&b_ok);
                if (!a_ok || !b_ok || a > b) continue;
                for (int p = a; p <= b; ++p) {
                    if (p >= 1 && p <= total) ext.explicitPages.push_back(p - 1);
                }
            } else {
                bool ok = false;
                int p = part.toInt(&ok);
                if (ok && p >= 1 && p <= total) ext.explicitPages.push_back(p - 1);
            }
        }
        std::sort(ext.explicitPages.begin(), ext.explicitPages.end());
        ext.explicitPages.erase(
            std::unique(ext.explicitPages.begin(), ext.explicitPages.end()),
            ext.explicitPages.end());
        if (ext.explicitPages.empty()) {
            QMessageBox::warning(this, tr("Invalid Range"),
                tr("The custom page range is empty or invalid."));
            return;
        }
    }

    // Phase 4e: go directly through applyWatermarkExt to honor mode/range.
    pdfe::batch::BatchEngine engine(context_);
    try {
        engine.applyWatermarkExt(document_->filePath(),
                                 std::filesystem::path(outputPath.toStdString()),
                                 cfg, ext);
        statusLabel_->setText(tr("Watermark applied: %1").arg(outputPath));
        emit watermarkApplied(outputPath);
        QMessageBox::information(this, tr("Watermark Applied"),
            tr("Watermarked PDF saved to:\n%1").arg(outputPath));
        accept();
        return;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Watermark Failed"),
            tr("Failed to apply watermark: %1").arg(e.what()));
        return;
    }
}

} // namespace pdfe::ui
