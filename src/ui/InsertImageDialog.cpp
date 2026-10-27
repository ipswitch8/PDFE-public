#include "ui/InsertImageDialog.h"

#include "ui/TestDialogSeam.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace pdfe::ui {

namespace {

/// One inch in PDF points — the margin the default placement keeps.
constexpr double kMarginPt = 72.0;

/// Points per pixel at 96 dpi, so an image's natural pixel size maps to a
/// sensible physical size rather than being treated as points.
constexpr double kPointsPerPixel = 72.0 / 96.0;

}  // namespace

QRectF InsertImageDialog::defaultRectFor(QSize sourceSize, QSizeF pageSize)
{
    // The defect this replaces was QRectF(72, 72, 400, 300) — a literal that
    // ignored BOTH the page and the image. Landscape pages, small pages and
    // portrait photographs all got the same 400x300 box.
    if (pageSize.width() <= 0 || pageSize.height() <= 0) {
        return QRectF(kMarginPt, kMarginPt, 400, 300);
    }

    const double availW = std::max(1.0, pageSize.width() - 2 * kMarginPt);
    const double availH = std::max(1.0, pageSize.height() - 2 * kMarginPt);

    double w = sourceSize.width() > 0 ? sourceSize.width() * kPointsPerPixel : availW;
    double h = sourceSize.height() > 0 ? sourceSize.height() * kPointsPerPixel : availH;

    // Scale DOWN to fit, never up: blowing a small image up to fill the page is
    // not what "default" should mean.
    const double scale = std::min({1.0, availW / w, availH / h});
    w *= scale;
    h *= scale;

    // Centre it. A centred image is a defensible default on any page shape,
    // which a fixed corner offset is not.
    const double x = (pageSize.width() - w) / 2.0;
    const double y = (pageSize.height() - h) / 2.0;
    return QRectF(x, y, w, h);
}

InsertImageDialog::InsertImageDialog(const QString& imagePath,
                                     QSize sourceSize,
                                     QSizeF pageSize,
                                     QWidget* parent)
    : QDialog(parent), sourceSize_(sourceSize), pageSize_(pageSize)
{
    setObjectName(QStringLiteral("insertImageDialog"));
    setWindowTitle(tr("Insert Image"));
    setAccessibleName(tr("Insert Image"));

    const QRectF def = defaultRectFor(sourceSize, pageSize);

    auto* outer = new QVBoxLayout(this);

    auto* fileLabel = new QLabel(tr("Inserting: %1").arg(QFileInfo(imagePath).fileName()), this);
    fileLabel->setObjectName(QStringLiteral("insertImageFileLabel"));
    fileLabel->setAccessibleName(tr("Image file"));
    outer->addWidget(fileLabel);

    // ── Placement ────────────────────────────────────────────────────────
    auto* placement = new QGroupBox(tr("Placement (points)"), this);
    placement->setObjectName(QStringLiteral("insertImagePlacementGroup"));
    auto* form = new QFormLayout(placement);

    const auto makeSpin = [this](const char* name, const QString& accessible,
                                 double value, double maximum) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setObjectName(QString::fromLatin1(name));
        spin->setAccessibleName(accessible);
        spin->setRange(0.0, maximum);
        spin->setDecimals(1);
        spin->setSingleStep(1.0);
        spin->setValue(value);
        return spin;
    };

    const double maxDim = std::max(pageSize.width(), pageSize.height()) * 4.0;
    x_ = makeSpin("insertImageXSpin", tr("X position"), def.x(), maxDim);
    y_ = makeSpin("insertImageYSpin", tr("Y position"), def.y(), maxDim);
    width_ = makeSpin("insertImageWidthSpin", tr("Width"), def.width(), maxDim);
    height_ = makeSpin("insertImageHeightSpin", tr("Height"), def.height(), maxDim);

    form->addRow(tr("&X:"), x_);
    form->addRow(tr("&Y:"), y_);
    form->addRow(tr("&Width:"), width_);
    form->addRow(tr("&Height:"), height_);

    keepAspect_ = new QCheckBox(tr("&Maintain aspect ratio"), this);
    keepAspect_->setObjectName(QStringLiteral("insertImageKeepAspectCheck"));
    keepAspect_->setAccessibleName(tr("Maintain aspect ratio"));
    keepAspect_->setChecked(true);
    form->addRow(keepAspect_);

    outer->addWidget(placement);

    // ── Encoding / privacy ───────────────────────────────────────────────
    auto* encoding = new QGroupBox(tr("Encoding"), this);
    encoding->setObjectName(QStringLiteral("insertImageEncodingGroup"));
    auto* encForm = new QFormLayout(encoding);

    jpegQuality_ = new QSpinBox(this);
    jpegQuality_->setObjectName(QStringLiteral("insertImageJpegQualitySpin"));
    jpegQuality_->setAccessibleName(tr("JPEG quality"));
    jpegQuality_->setRange(0, 100);
    jpegQuality_->setValue(85);  // the engine's own default
    encForm->addRow(tr("&JPEG quality:"), jpegQuality_);

    stripExif_ = new QCheckBox(tr("&Strip image metadata (EXIF)"), this);
    stripExif_->setObjectName(QStringLiteral("insertImageStripExifCheck"));
    stripExif_->setAccessibleName(tr("Strip image metadata"));
    stripExif_->setChecked(true);
    // Say what it does. This default was previously applied invisibly, and EXIF
    // can carry GPS coordinates, camera serial numbers and timestamps — a user
    // is entitled to know it is being removed, and to keep it if they need it.
    stripExif_->setToolTip(tr("Removes EXIF metadata such as GPS location, "
                              "camera details and timestamps from the inserted "
                              "image. On by default."));
    encForm->addRow(stripExif_);

    outer->addWidget(encoding);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("insertImageSummaryLabel"));
    summary_->setAccessibleName(tr("Placement summary"));
    outer->addWidget(summary_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("insertImageButtonBox"));
    if (auto* ok = buttons->button(QDialogButtonBox::Ok)) {
        ok->setObjectName(QStringLiteral("insertImageOkButton"));
        ok->setAccessibleName(tr("Insert"));
        ok->setText(tr("&Insert"));
    }
    if (auto* cancel = buttons->button(QDialogButtonBox::Cancel)) {
        cancel->setObjectName(QStringLiteral("insertImageCancelButton"));
        cancel->setAccessibleName(tr("Cancel"));
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    connect(width_, &QDoubleSpinBox::valueChanged,
            this, &InsertImageDialog::onWidthChanged);
    connect(height_, &QDoubleSpinBox::valueChanged,
            this, &InsertImageDialog::onHeightChanged);

    updateSummary();

    // The UI suite cannot enumerate a modal through UIA; this is the channel
    // that survives modality. Inert unless PDFE_TEST_PIPES is set.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

InsertImageDialog::~InsertImageDialog() = default;

void InsertImageDialog::onWidthChanged(double value)
{
    if (updating_ || !keepAspect_->isChecked()) {
        updateSummary();
        return;
    }
    if (sourceSize_.width() <= 0 || sourceSize_.height() <= 0) return;

    updating_ = true;
    const double ratio = static_cast<double>(sourceSize_.height()) /
                         static_cast<double>(sourceSize_.width());
    height_->setValue(value * ratio);
    updating_ = false;
    updateSummary();
}

void InsertImageDialog::onHeightChanged(double value)
{
    if (updating_ || !keepAspect_->isChecked()) {
        updateSummary();
        return;
    }
    if (sourceSize_.width() <= 0 || sourceSize_.height() <= 0) return;

    updating_ = true;
    const double ratio = static_cast<double>(sourceSize_.width()) /
                         static_cast<double>(sourceSize_.height());
    width_->setValue(value * ratio);
    updating_ = false;
    updateSummary();
}

void InsertImageDialog::updateSummary()
{
    if (!summary_) return;
    summary_->setText(tr("Page is %1 x %2 pt. Image will occupy %3 x %4 pt at (%5, %6).")
                          .arg(pageSize_.width(), 0, 'f', 0)
                          .arg(pageSize_.height(), 0, 'f', 0)
                          .arg(width_->value(), 0, 'f', 0)
                          .arg(height_->value(), 0, 'f', 0)
                          .arg(x_->value(), 0, 'f', 0)
                          .arg(y_->value(), 0, 'f', 0));
}

QRectF InsertImageDialog::targetRect() const
{
    return QRectF(x_->value(), y_->value(), width_->value(), height_->value());
}

int InsertImageDialog::jpegQuality() const { return jpegQuality_->value(); }
bool InsertImageDialog::stripExif() const { return stripExif_->isChecked(); }
bool InsertImageDialog::maintainAspectRatio() const { return keepAspect_->isChecked(); }

}  // namespace pdfe::ui
