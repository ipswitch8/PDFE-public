#include "HeaderFooterDialog.h"
#include "TestDialogSeam.h"
#include "headerfooter/HeaderFooterEngine.h"
#include "headerfooter/HeaderFooterConfig.h"
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
#include <QGroupBox>
#include <QMessageBox>
#include <QFileDialog>
#include <QRegularExpression>

#include <filesystem>
#include <vector>

namespace pdfe::ui {

namespace {

// Parse "1,3,5-8" → {0,2,4,5,6,7} (0-based).
std::vector<int> parseExplicitRange(const QString& raw, int totalPages)
{
    std::vector<int> out;
    const auto parts = raw.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (part.contains('-')) {
            const auto bounds = part.split('-', Qt::SkipEmptyParts);
            if (bounds.size() != 2) continue;
            bool okA = false, okB = false;
            int a = bounds[0].toInt(&okA);
            int b = bounds[1].toInt(&okB);
            if (!okA || !okB || a > b) continue;
            for (int p = a; p <= b; ++p) {
                if (p >= 1 && p <= totalPages) out.push_back(p - 1);
            }
        } else {
            bool ok = false;
            int p = part.toInt(&ok);
            if (ok && p >= 1 && p <= totalPages) out.push_back(p - 1);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

HeaderFooterDialog::HeaderFooterDialog(
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    pdfe::core::Document* document,
    QWidget* parent)
    : QDialog(parent)
    , context_(std::move(context))
    , document_(document)
{
    setupUi();
    // GAP-A14 / SKIP=FAIL: introspection + queued-action replay so the
    // Apply/Bates flow can be driven without UIA enumeration of the modal.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

HeaderFooterDialog::~HeaderFooterDialog() = default;

void HeaderFooterDialog::setupUi()
{
    setWindowTitle(tr("Headers and Footers"));
    setMinimumSize(520, 440);
    setObjectName("headerFooterDialog");
    setAccessibleName(tr("Headers and Footers Dialog"));

    auto* mainLayout = new QVBoxLayout(this);

    // Header group
    auto* headerBox = new QGroupBox(tr("Header"), this);
    headerBox->setObjectName("headerFooterHeaderGroup");
    auto* headerForm = new QFormLayout(headerBox);
    leftHeaderEdit_ = new QLineEdit(headerBox);
    leftHeaderEdit_->setObjectName("headerFooterLeftHeader");
    leftHeaderEdit_->setAccessibleName(tr("Left Header"));
    headerForm->addRow(tr("Left:"), leftHeaderEdit_);
    centerHeaderEdit_ = new QLineEdit(headerBox);
    centerHeaderEdit_->setObjectName("headerFooterCenterHeader");
    centerHeaderEdit_->setAccessibleName(tr("Center Header"));
    headerForm->addRow(tr("Center:"), centerHeaderEdit_);
    rightHeaderEdit_ = new QLineEdit(headerBox);
    rightHeaderEdit_->setObjectName("headerFooterRightHeader");
    rightHeaderEdit_->setAccessibleName(tr("Right Header"));
    headerForm->addRow(tr("Right:"), rightHeaderEdit_);
    mainLayout->addWidget(headerBox);

    // Footer group
    auto* footerBox = new QGroupBox(tr("Footer"), this);
    footerBox->setObjectName("headerFooterFooterGroup");
    auto* footerForm = new QFormLayout(footerBox);
    leftFooterEdit_ = new QLineEdit(footerBox);
    leftFooterEdit_->setObjectName("headerFooterLeftFooter");
    leftFooterEdit_->setAccessibleName(tr("Left Footer"));
    footerForm->addRow(tr("Left:"), leftFooterEdit_);
    centerFooterEdit_ = new QLineEdit(footerBox);
    centerFooterEdit_->setObjectName("headerFooterCenterFooter");
    centerFooterEdit_->setAccessibleName(tr("Center Footer"));
    footerForm->addRow(tr("Center:"), centerFooterEdit_);
    rightFooterEdit_ = new QLineEdit(footerBox);
    rightFooterEdit_->setObjectName("headerFooterRightFooter");
    rightFooterEdit_->setAccessibleName(tr("Right Footer"));
    footerForm->addRow(tr("Right:"), rightFooterEdit_);
    mainLayout->addWidget(footerBox);

    // Options group
    auto* optionsBox = new QGroupBox(tr("Options"), this);
    optionsBox->setObjectName("headerFooterOptionsGroup");
    auto* optForm = new QFormLayout(optionsBox);

    authorEdit_ = new QLineEdit(optionsBox);
    authorEdit_->setObjectName("headerFooterAuthor");
    authorEdit_->setAccessibleName(tr("Author (for {author} token)"));
    optForm->addRow(tr("Author:"), authorEdit_);

    marginSpin_ = new QDoubleSpinBox(optionsBox);
    marginSpin_->setObjectName("headerFooterMarginSpin");
    marginSpin_->setAccessibleName(tr("Margin (points)"));
    marginSpin_->setRange(6.0, 144.0);
    marginSpin_->setValue(36.0);
    marginSpin_->setSuffix(tr(" pt"));
    optForm->addRow(tr("Margin:"), marginSpin_);

    fontSizeSpin_ = new QDoubleSpinBox(optionsBox);
    fontSizeSpin_->setObjectName("headerFooterFontSizeSpin");
    fontSizeSpin_->setAccessibleName(tr("Font Size"));
    fontSizeSpin_->setRange(6.0, 36.0);
    fontSizeSpin_->setValue(10.0);
    optForm->addRow(tr("Font size:"), fontSizeSpin_);

    rangeCombo_ = new QComboBox(optionsBox);
    rangeCombo_->setObjectName("headerFooterRangeCombo");
    rangeCombo_->setAccessibleName(tr("Page Range"));
    rangeCombo_->addItem(tr("All pages"),  static_cast<int>(headerfooter::PageRangeMode::All));
    rangeCombo_->addItem(tr("Odd pages"),  static_cast<int>(headerfooter::PageRangeMode::OddPagesOnly));
    rangeCombo_->addItem(tr("Even pages"), static_cast<int>(headerfooter::PageRangeMode::EvenPagesOnly));
    rangeCombo_->addItem(tr("Custom..."),  static_cast<int>(headerfooter::PageRangeMode::Explicit));
    optForm->addRow(tr("Range:"), rangeCombo_);

    rangeExplicitEdit_ = new QLineEdit(optionsBox);
    rangeExplicitEdit_->setObjectName("headerFooterRangeExplicit");
    rangeExplicitEdit_->setAccessibleName(tr("Explicit Page Range"));
    rangeExplicitEdit_->setPlaceholderText(tr("e.g. 1,3,5-8"));
    rangeExplicitEdit_->setEnabled(false);
    optForm->addRow(tr("Pages:"), rangeExplicitEdit_);

    mainLayout->addWidget(optionsBox);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("headerFooterStatusLabel");
    mainLayout->addWidget(statusLabel_);

    // Buttons
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    applyButton_ = new QPushButton(tr("Apply..."), this);
    applyButton_->setObjectName("headerFooterApplyButton");
    applyButton_->setAccessibleName(tr("Apply Headers and Footers"));
    applyButton_->setDefault(true);
    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setObjectName("headerFooterCancelButton");
    cancelButton_->setAccessibleName(tr("Cancel"));
    btnRow->addWidget(applyButton_);
    btnRow->addWidget(cancelButton_);
    mainLayout->addLayout(btnRow);

    connect(applyButton_,  &QPushButton::clicked, this, &HeaderFooterDialog::onApply);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(rangeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &HeaderFooterDialog::onRangeModeChanged);
}

void HeaderFooterDialog::onRangeModeChanged(int /*index*/)
{
    const auto mode = static_cast<headerfooter::PageRangeMode>(
        rangeCombo_->currentData().toInt());
    rangeExplicitEdit_->setEnabled(mode == headerfooter::PageRangeMode::Explicit);
}

void HeaderFooterDialog::onApply()
{
    if (!document_ || !context_) {
        QMessageBox::warning(this, tr("No Document"),
            tr("No document is currently open."));
        return;
    }
    const std::filesystem::path inputPath = document_->filePath();
    if (inputPath.empty()) {
        QMessageBox::warning(this, tr("No File Path"),
            tr("The current document has no saved file path. Save it first."));
        return;
    }

    QString defaultOut = QString::fromStdString(
        (inputPath.parent_path() /
         (inputPath.stem().string() + "_hf" + inputPath.extension().string())).string());
    // SKIP=FAIL: honour a test-injected save path before the native dialog.
    QString outPath = pdfe::ui::testseam::takeFileDialogPath();
    if (outPath.isEmpty()) {
        outPath = QFileDialog::getSaveFileName(
            this, tr("Save With Headers/Footers"), defaultOut,
            tr("PDF Files (*.pdf)"));
    }
    if (outPath.isEmpty()) return;

    headerfooter::HeaderFooterConfig cfg;
    cfg.leftHeader   = leftHeaderEdit_->text().toStdString();
    cfg.centerHeader = centerHeaderEdit_->text().toStdString();
    cfg.rightHeader  = rightHeaderEdit_->text().toStdString();
    cfg.leftFooter   = leftFooterEdit_->text().toStdString();
    cfg.centerFooter = centerFooterEdit_->text().toStdString();
    cfg.rightFooter  = rightFooterEdit_->text().toStdString();
    cfg.marginPt     = static_cast<float>(marginSpin_->value());
    cfg.fontSize     = static_cast<float>(fontSizeSpin_->value());
    cfg.author       = authorEdit_->text().toStdString();
    cfg.filename     = inputPath.filename().string();
    cfg.rangeMode    = static_cast<headerfooter::PageRangeMode>(
                          rangeCombo_->currentData().toInt());
    if (cfg.rangeMode == headerfooter::PageRangeMode::Explicit) {
        cfg.explicitPages = parseExplicitRange(rangeExplicitEdit_->text(),
                                               document_->pageCount());
        if (cfg.explicitPages.empty()) {
            QMessageBox::warning(this, tr("Invalid Range"),
                tr("The custom page range is empty or invalid."));
            return;
        }
    }

    try {
        headerfooter::HeaderFooterEngine engine(context_);
        engine.apply(inputPath, std::filesystem::path(outPath.toStdString()), cfg);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Header/Footer Failed"),
            tr("Failed to apply headers/footers: %1").arg(e.what()));
        return;
    }
    statusLabel_->setText(tr("Applied to %1").arg(outPath));
    emit headerFooterApplied(outPath);
    accept();
}

} // namespace pdfe::ui
