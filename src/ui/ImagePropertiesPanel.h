#pragma once

#include "image/ImageData.h"
#include <QWidget>
#include <memory>

class QLabel;
class QDoubleSpinBox;
class QComboBox;
class QPushButton;

namespace pdfe::image {

/// Panel displaying properties of the selected image with transform controls.
class ImagePropertiesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ImagePropertiesPanel(QWidget* parent = nullptr);
    ~ImagePropertiesPanel() override;

    /// Update displayed info for the selected image.
    void setImageInfo(const PageImageInfo& info);

    /// Clear the panel (no image selected).
    void clearInfo();

signals:
    /// Emitted when user changes width/height in the spin boxes.
    void resizeRequested(float newWidth, float newHeight);

    /// Emitted when user selects a rotation from the combo box.
    void rotationRequested(float angleDegrees);

    /// Emitted when user clicks the crop button.
    void cropRequested();

    /// Emitted when user clicks the extract button.
    void extractRequested();

    /// Emitted when user clicks the remove button.
    void removeRequested();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::image
