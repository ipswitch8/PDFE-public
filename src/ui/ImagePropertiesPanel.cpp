// -*- coding: utf-8 -*-
#include "ui/ImagePropertiesPanel.h"
#include "image/ImageTool.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <cmath>

namespace pdfe::image {

class ImagePropertiesPanel::Impl {
public:
    // Image information labels
    QLabel* xobjectNameLabel_ = nullptr;
    QLabel* pixelDimensionsLabel_ = nullptr;
    QLabel* displayDimensionsLabel_ = nullptr;
    QLabel* dpiLabel_ = nullptr;
    QLabel* colorSpaceLabel_ = nullptr;
    QLabel* compressionLabel_ = nullptr;

    // Transform controls
    QDoubleSpinBox* widthSpin_ = nullptr;
    QDoubleSpinBox* heightSpin_ = nullptr;
    QComboBox* rotationCombo_ = nullptr;
    QPushButton* cropButton_ = nullptr;

    // Action buttons
    QPushButton* extractButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;

    // Flag to prevent signal emission during programmatic updates
    bool updatingInfo_ = false;

    // The four selectable angles. A placement at any other angle is shown in
    // an extra, non-selectable entry at index kOtherAngleIndex.
    static constexpr int kQuarterTurns = 4;
    static constexpr int kOtherAngleIndex = 4;

    // GitHub #423: show the angle the page holds (visually clockwise, as the
    // rotate actions count it). A multiple of 90 selects its entry; any other
    // angle is shown as itself in an extra entry that cannot be chosen, so the
    // box never claims 0 for a rotated image and showing it changes nothing.
    // Called with updatingInfo_ set, so no edit is requested.
    void showRotation(const PageImageInfo& info) {
        float deg = std::fmod(visualClockwiseDegrees(info), 360.0f);
        if (deg < 0.0f) deg += 360.0f;
        const float quarter = std::round(deg / 90.0f);
        const bool onQuarter = std::abs(deg - quarter * 90.0f) < 0.05f;
        if (onQuarter) {
            removeOtherAngle();
            rotationCombo_->setCurrentIndex(static_cast<int>(quarter) % kQuarterTurns);
            return;
        }
        const QString text = std::abs(deg - std::round(deg)) < 0.05f
            ? QString::fromUtf8("%1°").arg(static_cast<int>(std::round(deg)) % 360)
            : QString::fromUtf8("%1°").arg(static_cast<double>(deg), 0, 'f', 1);
        if (rotationCombo_->count() <= kOtherAngleIndex) {
            rotationCombo_->addItem(text);
        } else {
            rotationCombo_->setItemText(kOtherAngleIndex, text);
        }
        if (auto* model = qobject_cast<QStandardItemModel*>(rotationCombo_->model())) {
            if (QStandardItem* item = model->item(kOtherAngleIndex)) {
                item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
                item->setToolTip(QObject::tr("The image's current angle. Choose 0°, 90°, "
                                             "180° or 270° to set an exact angle."));
            }
        }
        rotationCombo_->setCurrentIndex(kOtherAngleIndex);
    }

    void removeOtherAngle() {
        while (rotationCombo_->count() > kQuarterTurns)
            rotationCombo_->removeItem(rotationCombo_->count() - 1);
    }

    // Tooltips: what each control does, or why it is disabled.
    void setEditable(bool editable, const QString& why) {
        const std::pair<QWidget*, QString> controls[] = {
            {widthSpin_, QObject::tr("Full width of the image in points. Changing it "
                                     "resizes the image and keeps its rotation and crop.")},
            {heightSpin_, QObject::tr("Full height of the image in points. Changing it "
                                      "resizes the image and keeps its rotation and crop.")},
            {rotationCombo_, QObject::tr("The image's angle, clockwise. Choosing an angle "
                                         "sets it exactly and keeps the crop.")},
            {cropButton_, QObject::tr("Crop to the central 80% of the visible image "
                                      "(as Image > Crop Image)")},
            {extractButton_, QObject::tr("Save the image's pixels to a file")},
            {removeButton_, QObject::tr("Remove this image from the page")},
        };
        for (const auto& [w, tip] : controls) {
            w->setEnabled(editable);
            w->setToolTip(editable ? tip : why);
        }
    }

    void setupUi(QWidget* parent) {
        auto* mainLayout = new QVBoxLayout(parent);
        mainLayout->setContentsMargins(8, 8, 8, 8);
        mainLayout->setSpacing(8);

        // Image Information section
        auto* infoGroup = new QGroupBox("Image Information", parent);
        infoGroup->setObjectName("imageInfoGroup");
        auto* infoLayout = new QFormLayout(infoGroup);

        xobjectNameLabel_ = new QLabel("-", infoGroup);
        xobjectNameLabel_->setObjectName("imageXObjectNameLabel");
        xobjectNameLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        infoLayout->addRow("XObject:", xobjectNameLabel_);

        pixelDimensionsLabel_ = new QLabel("-", infoGroup);
        pixelDimensionsLabel_->setObjectName("imagePixelDimensionsLabel");
        pixelDimensionsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        infoLayout->addRow("Pixel Size:", pixelDimensionsLabel_);

        displayDimensionsLabel_ = new QLabel("-", infoGroup);
        displayDimensionsLabel_->setObjectName("imageDisplayDimensionsLabel");
        displayDimensionsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        infoLayout->addRow("Display Size:", displayDimensionsLabel_);

        dpiLabel_ = new QLabel("-", infoGroup);
        dpiLabel_->setObjectName("imageDpiLabel");
        dpiLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        infoLayout->addRow("DPI:", dpiLabel_);

        colorSpaceLabel_ = new QLabel("-", infoGroup);
        colorSpaceLabel_->setObjectName("imageColorSpaceLabel");
        colorSpaceLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        infoLayout->addRow("Color Space:", colorSpaceLabel_);

        compressionLabel_ = new QLabel("-", infoGroup);
        compressionLabel_->setObjectName("imageCompressionLabel");
        compressionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        infoLayout->addRow("Compression:", compressionLabel_);

        mainLayout->addWidget(infoGroup);

        // Transform section
        auto* transformGroup = new QGroupBox("Transform", parent);
        transformGroup->setObjectName("imageTransformGroup");
        auto* transformLayout = new QFormLayout(transformGroup);

        widthSpin_ = new QDoubleSpinBox(transformGroup);
        widthSpin_->setObjectName("imageWidthSpin");
        widthSpin_->setAccessibleName(tr("Image Display Width"));
        widthSpin_->setRange(1.0, 10000.0);
        widthSpin_->setDecimals(1);
        widthSpin_->setSuffix(" pt");
        // One edit per committed value (Enter, focus out, arrow step), not one
        // per keystroke: each change is an undoable resize.
        widthSpin_->setKeyboardTracking(false);
        widthSpin_->setEnabled(false);
        transformLayout->addRow("Width:", widthSpin_);

        heightSpin_ = new QDoubleSpinBox(transformGroup);
        heightSpin_->setObjectName("imageHeightSpin");
        heightSpin_->setAccessibleName(tr("Image Display Height"));
        heightSpin_->setRange(1.0, 10000.0);
        heightSpin_->setDecimals(1);
        heightSpin_->setSuffix(" pt");
        heightSpin_->setKeyboardTracking(false);
        heightSpin_->setEnabled(false);
        transformLayout->addRow("Height:", heightSpin_);

        rotationCombo_ = new QComboBox(transformGroup);
        rotationCombo_->setObjectName("imageRotationCombo");
        rotationCombo_->setAccessibleName(tr("Image Rotation"));
        rotationCombo_->addItems({"0°", "90°", "180°", "270°"});
        rotationCombo_->setEnabled(false);
        transformLayout->addRow("Rotation:", rotationCombo_);

        cropButton_ = new QPushButton("Crop...", transformGroup);
        cropButton_->setObjectName("imageCropButton");
        cropButton_->setAccessibleName(tr("Crop Image"));
        cropButton_->setEnabled(false);
        transformLayout->addRow("", cropButton_);

        mainLayout->addWidget(transformGroup);

        // Actions section
        auto* actionsLayout = new QHBoxLayout();
        actionsLayout->setSpacing(8);

        extractButton_ = new QPushButton("Extract", parent);
        extractButton_->setObjectName("imageExtractButton");
        extractButton_->setAccessibleName(tr("Extract Image"));
        extractButton_->setEnabled(false);
        actionsLayout->addWidget(extractButton_);

        removeButton_ = new QPushButton("Remove", parent);
        removeButton_->setObjectName("imageRemoveButton");
        removeButton_->setAccessibleName(tr("Remove Image"));
        removeButton_->setEnabled(false);
        actionsLayout->addWidget(removeButton_);

        mainLayout->addLayout(actionsLayout);

        // Add stretch to push everything to the top
        mainLayout->addStretch();
    }

    void connectSignals(ImagePropertiesPanel* parent) {
        QObject::connect(widthSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            parent, [this, parent](double) {
                if (!updatingInfo_) {
                    emit parent->resizeRequested(
                        static_cast<float>(widthSpin_->value()),
                        static_cast<float>(heightSpin_->value())
                    );
                }
            });

        QObject::connect(heightSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            parent, [this, parent](double) {
                if (!updatingInfo_) {
                    emit parent->resizeRequested(
                        static_cast<float>(widthSpin_->value()),
                        static_cast<float>(heightSpin_->value())
                    );
                }
            });

        QObject::connect(rotationCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            parent, [this, parent](int index) {
                // Only the four quarter turns request an edit; the extra entry
                // showing another angle is display only.
                if (!updatingInfo_ && index >= 0 && index < kQuarterTurns) {
                    emit parent->rotationRequested(90.0f * static_cast<float>(index));
                }
            });

        QObject::connect(cropButton_, &QPushButton::clicked,
            parent, &ImagePropertiesPanel::cropRequested);

        QObject::connect(extractButton_, &QPushButton::clicked,
            parent, &ImagePropertiesPanel::extractRequested);

        QObject::connect(removeButton_, &QPushButton::clicked,
            parent, &ImagePropertiesPanel::removeRequested);
    }

    void setInfo(const PageImageInfo& info) {
        updatingInfo_ = true;

        // Update labels
        xobjectNameLabel_->setText(QString::fromStdString(info.xobjectName));

        pixelDimensionsLabel_->setText(
            QString("%1 × %2 px")
                .arg(info.pixelWidth)
                .arg(info.pixelHeight)
        );

        displayDimensionsLabel_->setText(
            QString("%1 × %2 pt")
                .arg(info.displayWidth, 0, 'f', 1)
                .arg(info.displayHeight, 0, 'f', 1)
        );

        dpiLabel_->setText(
            QString("%1 × %2 DPI")
                .arg(info.xDpi)
                .arg(info.yDpi)
        );

        colorSpaceLabel_->setText(QString::fromStdString(info.colorSpaceString()));
        compressionLabel_->setText(QString::fromStdString(info.compressionString()));

        // Update transform controls
        widthSpin_->setValue(info.displayWidth);
        heightSpin_->setValue(info.displayHeight);
        showRotation(info);

        // Enable controls. A placement inside a Form XObject is enumerated
        // but not editable (docs/image-placement-model.md), and extraction
        // looks the name up in the page's own resources, so every control is
        // disabled for it and says why.
        if (info.inFormXObject) {
            setEditable(false, QObject::tr(
                "This image is drawn inside a Form XObject, which may be shared with "
                "other pages, so it cannot be edited or extracted here."));
        } else {
            setEditable(true, QString());
        }

        updatingInfo_ = false;
    }

    void clearInfo() {
        updatingInfo_ = true;

        // Clear labels
        xobjectNameLabel_->setText("-");
        pixelDimensionsLabel_->setText("-");
        displayDimensionsLabel_->setText("-");
        dpiLabel_->setText("-");
        colorSpaceLabel_->setText("-");
        compressionLabel_->setText("-");

        // Reset controls
        widthSpin_->setValue(0.0);
        heightSpin_->setValue(0.0);
        removeOtherAngle();
        rotationCombo_->setCurrentIndex(0);

        // Disable controls
        setEditable(false, QObject::tr("Select an image with the Image tool"));

        updatingInfo_ = false;
    }
};

ImagePropertiesPanel::ImagePropertiesPanel(QWidget* parent)
    : QWidget(parent)
    , impl_(std::make_unique<Impl>())
{
    setObjectName("imagePropertiesPanel");
    setAccessibleName(tr("Image Properties Panel"));

    impl_->setupUi(this);
    impl_->connectSignals(this);

    // Start with empty state
    impl_->clearInfo();
}

ImagePropertiesPanel::~ImagePropertiesPanel() = default;

void ImagePropertiesPanel::setImageInfo(const PageImageInfo& info) {
    impl_->setInfo(info);
}

void ImagePropertiesPanel::clearInfo() {
    impl_->clearInfo();
}

} // namespace pdfe::image
