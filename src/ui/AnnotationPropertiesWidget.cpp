#include "AnnotationPropertiesWidget.h"
#include "../annotations/AnnotationType.h"
#include "UndoFallthroughFilter.h"   // GitHub #295

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QSlider>
#include <QPushButton>
#include <QColorDialog>
#include <QComboBox>
#include <QGroupBox>
#include <QSpinBox>
#include <QHBoxLayout>

namespace pdfe::ui {

AnnotationPropertiesWidget::AnnotationPropertiesWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("annotationPropertiesWidget");
    setAccessibleName(tr("Annotation Properties"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* formLayout = new QFormLayout();
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // Type (read-only)
    typeLabel_ = new QLabel(tr("None"), this);
    typeLabel_->setObjectName("annotTypeLabel");
    typeLabel_->setAccessibleName(tr("Annotation Type"));
    formLayout->addRow(tr("Type:"), typeLabel_);

    // Author
    authorEdit_ = new QLineEdit(this);
    authorEdit_->setObjectName("annotAuthorEdit");
    authorEdit_->setAccessibleName(tr("Annotation Author"));
    connect(authorEdit_, &QLineEdit::textChanged,
            this, &AnnotationPropertiesWidget::onAuthorChanged);
    formLayout->addRow(tr("Author:"), authorEdit_);

    layout->addLayout(formLayout);

    // Contents
    auto* contentsLabel = new QLabel(tr("Contents:"), this);
    contentsLabel->setObjectName("annotContentsLabel");
    layout->addWidget(contentsLabel);

    contentsEdit_ = new QTextEdit(this);
    contentsEdit_->setObjectName("annotContentsEdit");
    contentsEdit_->setAccessibleName(tr("Annotation Contents"));
    contentsEdit_->setMaximumHeight(100);
    // GitHub #295. Creating an annotation focuses THIS editor (see
    // focusContents(), called from MainWindow), so the user's next Ctrl+Z lands
    // here rather than on the document -- measured: the key arrives, no
    // shortcut matches, and nothing is undone. Release it to the window's undo
    // action when this editor has nothing of its own to undo. See the header
    // for why the fix is conditional rather than a shortcut-context change.
    contentsEdit_->installEventFilter(
        new pdfe::ui::UndoFallthroughFilter(contentsEdit_));
    connect(contentsEdit_, &QTextEdit::textChanged,
            this, &AnnotationPropertiesWidget::onContentsChanged);
    layout->addWidget(contentsEdit_);

    // Color
    auto* colorLayout = new QFormLayout();

    colorButton_ = new QPushButton(this);
    colorButton_->setObjectName("annotColorButton");
    colorButton_->setAccessibleName(tr("Annotation Color"));
    colorButton_->setFixedSize(60, 24);
    connect(colorButton_, &QPushButton::clicked,
            this, &AnnotationPropertiesWidget::onColorButtonClicked);
    colorLayout->addRow(tr("Color:"), colorButton_);

    layout->addLayout(colorLayout);

    // Opacity
    auto* opacityLayout = new QFormLayout();

    auto* opacityContainer = new QWidget(this);
    auto* opacityHBox = new QVBoxLayout(opacityContainer);
    opacityHBox->setContentsMargins(0, 0, 0, 0);

    opacitySlider_ = new QSlider(Qt::Horizontal, this);
    opacitySlider_->setObjectName("annotOpacitySlider");
    opacitySlider_->setAccessibleName(tr("Annotation Opacity"));
    opacitySlider_->setRange(0, 100);
    opacitySlider_->setValue(100);
    connect(opacitySlider_, &QSlider::valueChanged,
            this, &AnnotationPropertiesWidget::onOpacityChanged);
    opacityHBox->addWidget(opacitySlider_);

    opacityLabel_ = new QLabel(tr("100%"), this);
    opacityLabel_->setObjectName("annotOpacityLabel");
    opacityLabel_->setAlignment(Qt::AlignCenter);
    opacityHBox->addWidget(opacityLabel_);

    opacityLayout->addRow(tr("Opacity:"), opacityContainer);

    layout->addLayout(opacityLayout);

    // Font controls (shown only for FreeText annotations)
    fontGroup_ = new QGroupBox(tr("Text Appearance"), this);
    fontGroup_->setObjectName("annotFontGroup");
    auto* fontLayout = new QVBoxLayout(fontGroup_);

    auto* fontNameLayout = new QHBoxLayout();
    fontNameLayout->addWidget(new QLabel(tr("Font:"), fontGroup_));
    fontCombo_ = new QComboBox(fontGroup_);
    fontCombo_->setObjectName("annotFontCombo");
    fontCombo_->setAccessibleName(tr("Annotation Font"));
    fontCombo_->addItem(tr("Helvetica"), QString("Helv"));
    fontCombo_->addItem(tr("Times Roman"), QString("TiRo"));
    fontCombo_->addItem(tr("Courier"), QString("Cour"));
    fontNameLayout->addWidget(fontCombo_);
    fontLayout->addLayout(fontNameLayout);

    auto* fontSizeLayout = new QHBoxLayout();
    fontSizeLayout->addWidget(new QLabel(tr("Size:"), fontGroup_));
    fontSizeSpinner_ = new QSpinBox(fontGroup_);
    fontSizeSpinner_->setObjectName("annotFontSizeSpinner");
    fontSizeSpinner_->setAccessibleName(tr("Annotation Font Size"));
    fontSizeSpinner_->setRange(6, 144);
    fontSizeSpinner_->setValue(12);
    fontSizeSpinner_->setSuffix(tr(" pt"));
    fontSizeLayout->addWidget(fontSizeSpinner_);
    fontLayout->addLayout(fontSizeLayout);

    auto* textColorLayout = new QHBoxLayout();
    textColorLayout->addWidget(new QLabel(tr("Text Color:"), fontGroup_));
    textColorButton_ = new QPushButton(fontGroup_);
    textColorButton_->setObjectName("annotTextColorButton");
    textColorButton_->setAccessibleName(tr("Annotation Text Color"));
    textColorButton_->setFixedSize(24, 24);
    textColorButton_->setStyleSheet("QPushButton { background-color: black; }");
    textColorLayout->addWidget(textColorButton_);
    textColorLayout->addStretch();
    fontLayout->addLayout(textColorLayout);

    fontGroup_->setVisible(false); // Hidden by default
    layout->addWidget(fontGroup_);

    connect(fontCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AnnotationPropertiesWidget::onFontChanged);
    connect(fontSizeSpinner_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AnnotationPropertiesWidget::onFontSizeChanged);
    connect(textColorButton_, &QPushButton::clicked,
            this, &AnnotationPropertiesWidget::onTextColorClicked);

    layout->addStretch();

    // Initialize
    clear();
}

AnnotationPropertiesWidget::~AnnotationPropertiesWidget() = default;

void AnnotationPropertiesWidget::setAnnotation(const pdfe::annotations::AnnotationData& data)
{
    updatingFields_ = true;

    currentData_ = data;

    // Update type
    QString typeName = QString::fromStdString(
        pdfe::annotations::annotationTypeName(data.type));
    typeLabel_->setText(typeName);

    // Update author
    authorEdit_->setText(QString::fromStdString(data.author));

    // Update contents
    contentsEdit_->setPlainText(QString::fromStdString(data.contents));

    // Update color
    updateColorButton();

    // Update opacity
    int opacityPercent = static_cast<int>(data.opacity * 100.0f);
    opacitySlider_->setValue(opacityPercent);
    opacityLabel_->setText(QString("%1%").arg(opacityPercent));

    // Show/hide font controls for FreeText annotations
    bool isFreeText = (data.type == pdfe::annotations::AnnotationType::FreeText);
    fontGroup_->setVisible(isFreeText);
    if (isFreeText) {
        // Find font combo index by data value
        QString fontKey = QString::fromStdString(data.daFontName);
        int fontIdx = fontCombo_->findData(fontKey);
        if (fontIdx >= 0) fontCombo_->setCurrentIndex(fontIdx);
        fontSizeSpinner_->setValue(static_cast<int>(data.daFontSize));
        textColorButton_->setStyleSheet(
            QString("QPushButton { background-color: %1; }").arg(data.daColor.name()));
    }

    updatingFields_ = false;
}

void AnnotationPropertiesWidget::clear()
{
    updatingFields_ = true;

    currentData_ = pdfe::annotations::AnnotationData();

    typeLabel_->setText(tr("None"));
    authorEdit_->clear();
    contentsEdit_->clear();
    opacitySlider_->setValue(100);
    opacityLabel_->setText(tr("100%"));

    // Reset color button
    colorButton_->setStyleSheet("");

    // Hide font controls
    fontGroup_->setVisible(false);

    updatingFields_ = false;
}

void AnnotationPropertiesWidget::focusContents()
{
    if (contentsEdit_) {
        contentsEdit_->setFocus();
    }
}

void AnnotationPropertiesWidget::onColorButtonClicked()
{
    QColor newColor = QColorDialog::getColor(currentData_.color, this,
                                              tr("Select Annotation Color"));

    if (newColor.isValid() && newColor != currentData_.color) {
        currentData_.color = newColor;
        updateColorButton();
        emitChanges();
    }
}

void AnnotationPropertiesWidget::setAnnotationColor(QColor color)
{
    if (color.isValid() && color != currentData_.color) {
        currentData_.color = color;
        updateColorButton();
        emitChanges();
    }
}

void AnnotationPropertiesWidget::setAnnotationTextColor(QColor color)
{
    if (color.isValid() && color != currentData_.daColor) {
        currentData_.daColor = color;
        if (textColorButton_) {
            textColorButton_->setStyleSheet(
                QString("QPushButton { background-color: %1; }").arg(color.name()));
        }
        emitChanges();
    }
}

void AnnotationPropertiesWidget::onOpacityChanged(int value)
{
    opacityLabel_->setText(QString("%1%").arg(value));

    if (!updatingFields_) {
        currentData_.opacity = value / 100.0f;
        emitChanges();
    }
}

void AnnotationPropertiesWidget::onContentsChanged()
{
    if (!updatingFields_) {
        currentData_.contents = contentsEdit_->toPlainText().toStdString();
        emitChanges();
    }
}

void AnnotationPropertiesWidget::onAuthorChanged(const QString& text)
{
    if (!updatingFields_) {
        currentData_.author = text.toStdString();
        emitChanges();
    }
}

void AnnotationPropertiesWidget::emitChanges()
{
    if (!updatingFields_) {
        emit propertiesChanged(currentData_);
    }
}

void AnnotationPropertiesWidget::updateColorButton()
{
    QString colorStyle = QString(
        "QPushButton { background-color: rgba(%1, %2, %3, %4); }")
        .arg(currentData_.color.red())
        .arg(currentData_.color.green())
        .arg(currentData_.color.blue())
        .arg(currentData_.color.alpha());

    colorButton_->setStyleSheet(colorStyle);
}

void AnnotationPropertiesWidget::onFontChanged(int index)
{
    if (updatingFields_ || !fontCombo_) return;
    currentData_.daFontName = fontCombo_->currentData().toString().toStdString();
    emitChanges();
}

void AnnotationPropertiesWidget::onFontSizeChanged(int size)
{
    if (updatingFields_ || !fontSizeSpinner_) return;
    currentData_.daFontSize = static_cast<float>(size);
    emitChanges();
}

void AnnotationPropertiesWidget::onTextColorClicked()
{
    if (updatingFields_ || !textColorButton_) return;
    QColor color = QColorDialog::getColor(currentData_.daColor, this, tr("Select Text Color"));
    if (color.isValid()) {
        currentData_.daColor = color;
        textColorButton_->setStyleSheet(
            QString("QPushButton { background-color: %1; }").arg(color.name()));
        emitChanges();
    }
}

} // namespace pdfe::ui
