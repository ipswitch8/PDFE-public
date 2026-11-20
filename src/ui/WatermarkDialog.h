#pragma once

#include <QDialog>
#include <memory>
#include <filesystem>

#include "batch/BatchTypes.h"

namespace pdfe::core {
class MuPdfContext;
class Document;
}

class QLineEdit;
class QDoubleSpinBox;
class QComboBox;
class QPushButton;
class QLabel;
class QFormLayout;
class QHBoxLayout;

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

    /// The watermark settings as the controls hold them now (what Apply
    /// hands to BatchEngine::applyWatermarkExt).
    pdfe::batch::WatermarkConfig config() const;
    /// Mode, image path and page range. An explicit range is parsed against
    /// the document's page count; with no document it stays empty.
    pdfe::batch::WatermarkExtConfig extConfig() const;

    /// Default Rotation per mode, used until the user changes Rotation:
    /// text is set diagonally, an image is placed upright (as it always was).
    static constexpr double kTextDefaultRotation = 45.0;
    static constexpr double kImageDefaultRotation = 0.0;

signals:
    void watermarkApplied(const QString& outputPath);

private slots:
    void onApply();

private:
    void setupUi();
    // GitHub #356: show the image-source row only in Image mode. Hiding (not
    // merely disabling) the row keeps Text mode from prompting for an image.
    // Also establishes the correct INITIAL state for the default (Text) mode,
    // which the currentIndexChanged signal never fires for.
    void updateModeVisibility();

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    pdfe::core::Document* document_;

    QFormLayout* formLayout_ = nullptr;      // GitHub #356: for setRowVisible
    QHBoxLayout* imageRow_ = nullptr;        // GitHub #356: the image-source row
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

    // Rotation follows the mode's default until the user edits it; after
    // that, switching modes keeps the user's value.
    bool rotationUserSet_ = false;
    bool settingRotation_ = false;
};

} // namespace pdfe::ui
