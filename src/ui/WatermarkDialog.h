#pragma once

#include <QDialog>
#include <memory>
#include <filesystem>

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

/// Dialog to apply a text watermark to the current document.
/// Reuses BatchEngine::applyWatermark logic for a single file.
class WatermarkDialog : public QDialog {
    Q_OBJECT

public:
    explicit WatermarkDialog(
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        pdfe::core::Document* document,
        QWidget* parent = nullptr
    );
    ~WatermarkDialog() override;

signals:
    void watermarkApplied(const QString& outputPath);

private slots:
    void onApply();

private:
    void setupUi();

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    pdfe::core::Document* document_;

    QComboBox* modeCombo_ = nullptr;         // Phase 4e: text / image
    QLineEdit* textEdit_ = nullptr;
    QLineEdit* imagePathEdit_ = nullptr;     // Phase 4e
    QPushButton* imageBrowseButton_ = nullptr; // Phase 4e
    QDoubleSpinBox* opacitySpin_ = nullptr;
    QDoubleSpinBox* fontSizeSpin_ = nullptr;
    QDoubleSpinBox* rotationSpin_ = nullptr;
    QComboBox* positionCombo_ = nullptr;
    QComboBox* rangeCombo_ = nullptr;        // Phase 4e: all/odd/even/explicit
    QLineEdit* rangeExplicitEdit_ = nullptr; // Phase 4e
    QPushButton* applyButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
};

} // namespace pdfe::ui
