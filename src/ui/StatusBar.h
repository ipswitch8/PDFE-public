#pragma once

#include <QStatusBar>
#include <QString>

class QLabel;

namespace pdfe::ui {

class StatusBar : public QStatusBar {
    Q_OBJECT

public:
    explicit StatusBar(QWidget* parent = nullptr);
    ~StatusBar() override;

    void setPageInfo(int current, int total);
    void setZoom(float zoomPercent);
    void setFileName(const QString& fileName);
    void setEncrypted(bool encrypted);
    void clearInfo();

    /// Phase 8d: report the number of form fields in the current doc.
    /// 0 shows "No form fields"; >0 shows "N form fields". The backing
    /// QLabel has objectName "formFieldCount" and accessibleName
    /// "Form Field Count" for UIA/pywinauto automation.
    void setFormFieldCount(int n);

private:
    QLabel* pageLabel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QLabel* fileNameLabel_ = nullptr;
    QLabel* encryptionLabel_ = nullptr;
    QLabel* formFieldCountLabel_ = nullptr;  // Phase 8d
};

} // namespace pdfe::ui
