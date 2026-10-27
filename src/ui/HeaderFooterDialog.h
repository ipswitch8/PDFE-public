#pragma once

#include <QDialog>
#include <filesystem>
#include <memory>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

class QLineEdit;
class QDoubleSpinBox;
class QComboBox;
class QPushButton;
class QLabel;

namespace pdfe::ui {

/// Dialog to apply page headers/footers to the current document.
/// Left/center/right slots for header and footer, token substitution
/// ({date}/{author}/{page}/{pages}/{filename}), page range, margin, font size.
/// Re-applies idempotently — prior marker-tagged H/F streams are removed first.
class HeaderFooterDialog : public QDialog {
    Q_OBJECT

public:
    explicit HeaderFooterDialog(
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        pdfe::core::Document* document,
        QWidget* parent = nullptr);
    ~HeaderFooterDialog() override;

signals:
    void headerFooterApplied(const QString& outputPath);

private slots:
    void onApply();
    void onRangeModeChanged(int index);

private:
    void setupUi();

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    pdfe::core::Document* document_;

    QLineEdit* leftHeaderEdit_   = nullptr;
    QLineEdit* centerHeaderEdit_ = nullptr;
    QLineEdit* rightHeaderEdit_  = nullptr;
    QLineEdit* leftFooterEdit_   = nullptr;
    QLineEdit* centerFooterEdit_ = nullptr;
    QLineEdit* rightFooterEdit_  = nullptr;

    QComboBox*     rangeCombo_        = nullptr;
    QLineEdit*     rangeExplicitEdit_ = nullptr;  // e.g. "1,3,5-8"
    QDoubleSpinBox* marginSpin_       = nullptr;
    QDoubleSpinBox* fontSizeSpin_     = nullptr;
    QLineEdit*     authorEdit_        = nullptr;

    QPushButton* applyButton_  = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QLabel*      statusLabel_  = nullptr;
};

} // namespace pdfe::ui
