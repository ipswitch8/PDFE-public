#include "tre/TreEditDialog.h"
#include "tre/RegionReconstructor.h"
#include "ui/TestDialogSeam.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextEdit>
#include <QLabel>
#include <QRadioButton>
#include <QDialogButtonBox>
#include <QGroupBox>

namespace pdfe::tre {

TreEditDialog::TreEditDialog(const ReconstructionResult& analysisResult, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Scanned Text Editor");
    setObjectName("treEditDialog");
    setAccessibleName(tr("Scanned Text Editor"));
    setAccessibleDescription(tr("Review and edit reconstructed text for the selected scanned region"));
    setMinimumSize(500, 400);
    setSizeGripEnabled(true);

    auto* mainLayout = new QVBoxLayout(this);

    // Quality section
    qualityLabel_ = new QLabel(this);
    qualityLabel_->setObjectName("qualityLabel");
    qualityLabel_->setAccessibleName(tr("Reconstruction Quality Score"));
    double score = analysisResult.qualityVerdict.overallScore;
    QString recommendedApproach = (score >= 0.65) ? "Approach B" : "Approach C";
    qualityLabel_->setText(QString("Quality Score: %1 - %2 recommended")
                           .arg(score, 0, 'f', 2)
                           .arg(recommendedApproach));
    mainLayout->addWidget(qualityLabel_);

    // Font section
    fontLabel_ = new QLabel(this);
    fontLabel_->setObjectName("fontLabel");
    fontLabel_->setAccessibleName(tr("Matched Font"));
    if (analysisResult.region.fontMatch.fontFamily.empty()) {
        fontLabel_->setText("Matched Font: Unknown");
    } else {
        fontLabel_->setText(QString("Matched Font: %1 (confidence: %2)")
                            .arg(QString::fromStdString(analysisResult.region.fontMatch.fontFamily))
                            .arg(analysisResult.region.fontMatch.confidence, 0, 'f', 2));
    }
    mainLayout->addWidget(fontLabel_);

    // Text edit section
    auto* textEditLabel = new QLabel("Recognized Text (edit before applying):", this);
    mainLayout->addWidget(textEditLabel);

    textEdit_ = new QTextEdit(this);
    textEdit_->setObjectName("treTextEdit");
    textEdit_->setAccessibleName(tr("Recognized Text Editor"));
    textEdit_->setMinimumHeight(150);

    // Use editedText if available, otherwise originalText
    QString displayText = QString::fromStdString(
        analysisResult.region.editedText.empty()
            ? analysisResult.region.originalText
            : analysisResult.region.editedText
    );

    if (displayText.isEmpty()) {
        textEdit_->setPlaceholderText("No text recognized in this region");
    } else {
        textEdit_->setPlainText(displayText);
    }
    mainLayout->addWidget(textEdit_);

    // Approach selection
    auto* approachGroup = new QGroupBox("Reconstruction Approach", this);
    approachGroup->setObjectName("approachGroup");
    approachGroup->setAccessibleName(tr("Reconstruction Approach"));
    auto* approachLayout = new QHBoxLayout(approachGroup);

    approachBRadio_ = new QRadioButton("Hybrid (text over image)", this);
    approachBRadio_->setObjectName("approachBRadio");
    approachBRadio_->setAccessibleName(tr("Hybrid Reconstruction (text over image)"));
    approachLayout->addWidget(approachBRadio_);

    approachCRadio_ = new QRadioButton("Image replacement (bitmap)", this);
    approachCRadio_->setObjectName("approachCRadio");
    approachCRadio_->setAccessibleName(tr("Image Replacement (bitmap)"));
    approachLayout->addWidget(approachCRadio_);

    // Auto-select based on quality score
    if (score >= 0.65) {
        approachBRadio_->setChecked(true);
    } else {
        approachCRadio_->setChecked(true);
    }

    mainLayout->addWidget(approachGroup);

    // Button box
    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->setObjectName("buttonBox");
    buttonBox_->setAccessibleName(tr("Dialog Actions"));
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox_);

    // Register with the dialog-introspection seam. MUST be the last statement
    // in the constructor: it records the child inventory, so anything
    // parented after this point is invisible to it.
    //
    // This dialog did not register, which left a UI test with no supported
    // way to observe it -- pywinauto's UIA backend cannot enumerate a Qt
    // app-modal dialog while exec() runs, and this is the documented
    // mechanism for working around that. 19 of 25 QDialog subclasses were in
    // the same state; see docs/audit/ui-test-observability-audit.md.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

QString TreEditDialog::editedText() const
{
    return textEdit_->toPlainText();
}

Approach TreEditDialog::selectedApproach() const
{
    return approachBRadio_->isChecked()
        ? Approach::B_HybridReconstruction
        : Approach::C_ImageReplacement;
}

} // namespace pdfe::tre
