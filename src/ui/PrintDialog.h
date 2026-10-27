#pragma once

#include "core/PrintEngine.h"
#include <QDialog>

class QRadioButton;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QButtonGroup;

namespace pdfe::core { class Document; }

namespace pdfe::ui {

class PrintDialog : public QDialog {
    Q_OBJECT

public:
    explicit PrintDialog(pdfe::core::Document* doc, int currentPage, QWidget* parent = nullptr);
    ~PrintDialog() override;

    pdfe::core::PrintOptions options() const;

private slots:
    void onRangeChanged();
    void onPrintClicked();

private:
    void setupUi();
    void validatePageRange();

    pdfe::core::Document* document_ = nullptr;
    int currentPageIndex_ = 0;

    QRadioButton* allPagesRadio_ = nullptr;
    QRadioButton* currentPageRadio_ = nullptr;
    QRadioButton* pageRangeRadio_ = nullptr;
    QButtonGroup* rangeGroup_ = nullptr;
    QSpinBox* fromPageSpin_ = nullptr;
    QSpinBox* toPageSpin_ = nullptr;
    QSpinBox* copiesSpin_ = nullptr;
    QCheckBox* fitToPageCheck_ = nullptr;
    QCheckBox* centerOnPageCheck_ = nullptr;
    QCheckBox* autoRotateCheck_ = nullptr;
    QPushButton* printButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
};

} // namespace pdfe::ui
