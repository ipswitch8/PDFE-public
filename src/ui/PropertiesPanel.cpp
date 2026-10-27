#include "PropertiesPanel.h"

#include <QLabel>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QGroupBox>

namespace pdfe::ui {

PropertiesPanel::PropertiesPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("propertiesPanel");
    setAccessibleName(tr("Page Properties Panel"));

    // Create labels
    pageIndexLabel_ = new QLabel(this);
    pageIndexLabel_->setObjectName("pageIndexLabel");
    pageIndexLabel_->setAccessibleName(tr("Page Index"));
    pageIndexLabel_->setText("-");

    pageLabelLabel_ = new QLabel(this);
    pageLabelLabel_->setObjectName("pageLabelLabel");
    pageLabelLabel_->setAccessibleName(tr("Page Label"));
    pageLabelLabel_->setText("-");

    widthLabel_ = new QLabel(this);
    widthLabel_->setObjectName("widthLabel");
    widthLabel_->setAccessibleName(tr("Page Width"));
    widthLabel_->setText("-");

    heightLabel_ = new QLabel(this);
    heightLabel_->setObjectName("heightLabel");
    heightLabel_->setAccessibleName(tr("Page Height"));
    heightLabel_->setText("-");

    rotationLabel_ = new QLabel(this);
    rotationLabel_->setObjectName("rotationLabel");
    rotationLabel_->setAccessibleName(tr("Page Rotation"));
    rotationLabel_->setText("-");

    // Create form layout
    formLayout_ = new QFormLayout();
    formLayout_->addRow(tr("Page Index:"), pageIndexLabel_);
    formLayout_->addRow(tr("Page Label:"), pageLabelLabel_);
    formLayout_->addRow(tr("Width:"), widthLabel_);
    formLayout_->addRow(tr("Height:"), heightLabel_);
    formLayout_->addRow(tr("Rotation:"), rotationLabel_);

    // Create group box
    QGroupBox* groupBox = new QGroupBox(tr("Page Properties"), this);
    groupBox->setObjectName("pagePropertiesGroup");
    groupBox->setLayout(formLayout_);

    // Main layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(groupBox);
    mainLayout->addStretch();
    setLayout(mainLayout);
}

PropertiesPanel::~PropertiesPanel() = default;

void PropertiesPanel::setPageInfo(const pdfe::core::PageInfo& info)
{
    pageIndexLabel_->setText(QString::number(info.index + 1));

    pageLabelLabel_->setText(info.label.empty()
                            ? QString("-")
                            : QString::fromStdString(info.label));

    widthLabel_->setText(QString("%1 pt").arg(info.widthPt, 0, 'f', 2));
    heightLabel_->setText(QString("%1 pt").arg(info.heightPt, 0, 'f', 2));
    rotationLabel_->setText(QString("%1°").arg(info.rotation));
}

void PropertiesPanel::clearPageInfo()
{
    pageIndexLabel_->setText("-");
    pageLabelLabel_->setText("-");
    widthLabel_->setText("-");
    heightLabel_->setText("-");
    rotationLabel_->setText("-");
}

} // namespace pdfe::ui
