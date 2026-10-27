#include "StatusBar.h"
#include <QLabel>

namespace pdfe::ui {

StatusBar::StatusBar(QWidget* parent)
    : QStatusBar(parent)
{
    setObjectName("statusBar");

    // Create labels
    pageLabel_ = new QLabel(this);
    pageLabel_->setObjectName("pageLabel");
    pageLabel_->setAccessibleName(tr("Page Number"));
    pageLabel_->setText("-");
    pageLabel_->setMinimumWidth(120);

    zoomLabel_ = new QLabel(this);
    zoomLabel_->setObjectName("zoomLabel");
    zoomLabel_->setAccessibleName(tr("Zoom Level"));
    zoomLabel_->setText("-");
    zoomLabel_->setMinimumWidth(80);

    fileNameLabel_ = new QLabel(this);
    fileNameLabel_->setObjectName("fileNameLabel");
    fileNameLabel_->setAccessibleName(tr("File Name"));
    fileNameLabel_->setText("-");

    encryptionLabel_ = new QLabel(this);
    encryptionLabel_->setObjectName("encryptionLabel");
    encryptionLabel_->setAccessibleName(tr("Encryption Status"));
    encryptionLabel_->setVisible(false);

    // Phase 8d: form field count — UIA-discoverable label consumed by
    // the blocked form-create UI tests.
    formFieldCountLabel_ = new QLabel(this);
    formFieldCountLabel_->setObjectName("formFieldCount");
    formFieldCountLabel_->setAccessibleName("Form Field Count");
    formFieldCountLabel_->setText("No form fields");

    // Add to status bar
    addWidget(pageLabel_);
    addWidget(zoomLabel_);
    addPermanentWidget(formFieldCountLabel_);
    addPermanentWidget(encryptionLabel_);
    addPermanentWidget(fileNameLabel_);
}

StatusBar::~StatusBar() = default;

void StatusBar::setPageInfo(int current, int total)
{
    const QString text = QString("Page %1 of %2").arg(current).arg(total);
    pageLabel_->setText(text);
    // Pipeline F phase-3: mirror displayed text into accessibleName
    // so UIA-driven automation (pywinauto follow-link tests) can read
    // the live page indicator via window_text() / Name. Qt's
    // QAccessibleWidget doesn't expose QLabel::text() to UIA, and
    // accessibleDescription doesn't reliably surface through UIA's
    // HelpText property either. Putting the value in Name is the
    // most portable read path. Screen readers will read "Page 1 of
    // 3" — informative for a status-bar value indicator.
    pageLabel_->setAccessibleName(text);
}

void StatusBar::setZoom(float zoomPercent)
{
    const QString text = QString("%1%").arg(static_cast<int>(zoomPercent));
    zoomLabel_->setText(text);
    zoomLabel_->setAccessibleName(text);
}

void StatusBar::setFileName(const QString& fileName)
{
    fileNameLabel_->setText(fileName);
    fileNameLabel_->setAccessibleName(fileName);
}

void StatusBar::setEncrypted(bool encrypted)
{
    encryptionLabel_->setVisible(encrypted);
    if (encrypted) {
        encryptionLabel_->setText("[Encrypted]");
    }
}

void StatusBar::clearInfo()
{
    pageLabel_->setText("-");
    zoomLabel_->setText("-");
    fileNameLabel_->setText("-");
    encryptionLabel_->setVisible(false);
    formFieldCountLabel_->setText("No form fields");
}

void StatusBar::setFormFieldCount(int n)
{
    if (n <= 0) {
        formFieldCountLabel_->setText("No form fields");
    } else if (n == 1) {
        formFieldCountLabel_->setText("1 form field");
    } else {
        formFieldCountLabel_->setText(QString("%1 form fields").arg(n));
    }
}

} // namespace pdfe::ui
