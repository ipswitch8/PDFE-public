#include "ui/ImagePropertiesPanel.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>

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
        widthSpin_->setEnabled(false);
        transformLayout->addRow("Width:", widthSpin_);

        heightSpin_ = new QDoubleSpinBox(transformGroup);
        heightSpin_->setObjectName("imageHeightSpin");
        heightSpin_->setAccessibleName(tr("Image Display Height"));
        heightSpin_->setRange(1.0, 10000.0);
        heightSpin_->setDecimals(1);
        heightSpin_->setSuffix(" pt");
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
                if (!updatingInfo_) {
                    float angle = 0.0f;
                    switch (index) {
                    case 0: angle = 0.0f; break;
                    case 1: angle = 90.0f; break;
                    case 2: angle = 180.0f; break;
                    case 3: angle = 270.0f; break;
                    }
                    emit parent->rotationRequested(angle);
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
        rotationCombo_->setCurrentIndex(0); // Default to 0°

        // Enable controls
        widthSpin_->setEnabled(true);
        heightSpin_->setEnabled(true);
        rotationCombo_->setEnabled(true);
        cropButton_->setEnabled(true);
        extractButton_->setEnabled(true);
        removeButton_->setEnabled(true);

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
        rotationCombo_->setCurrentIndex(0);

        // Disable controls
        widthSpin_->setEnabled(false);
        heightSpin_->setEnabled(false);
        rotationCombo_->setEnabled(false);
        cropButton_->setEnabled(false);
        extractButton_->setEnabled(false);
        removeButton_->setEnabled(false);

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
