#include "AltTextDialog.h"
#include "annotations/AnnotationManager.h"
#include "core/Page.h"

#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMessageBox>

#include <exception>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

AltTextDialog::AltTextDialog(pdfe::annotations::AnnotationManager* mgr,
                             pdfe::core::Page* page,
                             int annotIndex,
                             QWidget* parent)
    : QDialog(parent), mgr_(mgr), page_(page), annotIndex_(annotIndex)
{
    setupUi();
    loadFromAnnotation();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

AltTextDialog::~AltTextDialog() = default;

void AltTextDialog::setupUi()
{
    setWindowTitle(tr("Alt Text and Actual Text"));
    setObjectName("altTextDialog");
    setAccessibleName(tr("Alt Text Dialog"));
    setMinimumSize(420, 260);

    auto* layout = new QVBoxLayout(this);

    auto* form = new QFormLayout();

    altEdit_ = new QLineEdit(this);
    altEdit_->setObjectName("altTextAltEdit");
    altEdit_->setAccessibleName(tr("Alternate Text"));
    altEdit_->setPlaceholderText(tr("Short description (spoken by screen readers)"));
    form->addRow(tr("Alt text (&Alt):"), altEdit_);

    actualEdit_ = new QTextEdit(this);
    actualEdit_->setObjectName("altTextActualEdit");
    actualEdit_->setAccessibleName(tr("Actual Text"));
    actualEdit_->setPlaceholderText(tr(
        "Replacement text used when the visible glyphs don't convey the "
        "content (e.g. \"H2O\" for an image of a chemical formula)."));
    actualEdit_->setMaximumHeight(80);
    form->addRow(tr("ActualText (/ActualText):"), actualEdit_);

    layout->addLayout(form);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("altTextStatusLabel");
    layout->addWidget(statusLabel_);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    saveButton_ = new QPushButton(tr("Save"), this);
    saveButton_->setObjectName("altTextSaveButton");
    saveButton_->setAccessibleName(tr("Save Alt Text"));
    saveButton_->setDefault(true);
    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setObjectName("altTextCancelButton");
    cancelButton_->setAccessibleName(tr("Cancel"));
    btnRow->addWidget(saveButton_);
    btnRow->addWidget(cancelButton_);
    layout->addLayout(btnRow);

    // QTextEdit consumes Tab for indentation — enable Tab-to-next-control.
    actualEdit_->setTabChangesFocus(true);

    // Explicit tab order: alt edit → actual edit → save → cancel → (cycle).
    altEdit_->setFocusPolicy(Qt::StrongFocus);
    actualEdit_->setFocusPolicy(Qt::StrongFocus);
    saveButton_->setFocusPolicy(Qt::StrongFocus);
    cancelButton_->setFocusPolicy(Qt::StrongFocus);
    setTabOrder(altEdit_, actualEdit_);
    setTabOrder(actualEdit_, saveButton_);
    setTabOrder(saveButton_, cancelButton_);
    setTabOrder(cancelButton_, altEdit_);

    connect(saveButton_,   &QPushButton::clicked, this, &AltTextDialog::onSave);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
}

void AltTextDialog::loadFromAnnotation()
{
    if (!mgr_ || !page_) return;
    try {
        altEdit_->setText(QString::fromStdString(
            mgr_->getAltText(page_, annotIndex_)));
        actualEdit_->setPlainText(QString::fromStdString(
            mgr_->getActualText(page_, annotIndex_)));
    } catch (const std::exception& e) {
        statusLabel_->setText(
            tr("Could not read existing alt text: %1").arg(e.what()));
    }
}

void AltTextDialog::onSave()
{
    if (!mgr_ || !page_) {
        QMessageBox::warning(this, tr("No Document"),
                             tr("No page to write to."));
        return;
    }
    try {
        mgr_->setAltText(page_, annotIndex_,
                         altEdit_->text().toStdString());
        mgr_->setActualText(page_, annotIndex_,
                            actualEdit_->toPlainText().toStdString());
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Save Failed"),
            tr("Failed to write alt text: %1").arg(e.what()));
        return;
    }
    emit saved(annotIndex_);
    accept();
}

} // namespace pdfe::ui
