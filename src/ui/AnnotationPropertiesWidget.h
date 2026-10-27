#pragma once

#include <QWidget>
#include <QColor>
#include "../annotations/AnnotationData.h"

class QLabel;
class QLineEdit;
class QTextEdit;
class QSlider;
class QPushButton;
class QSpinBox;
class QComboBox;
class QGroupBox;

namespace pdfe::ui {

/// Widget for editing annotation properties.
///
/// Displays and allows modification of annotation metadata like
/// author, contents, color, and opacity.
class AnnotationPropertiesWidget : public QWidget {
    Q_OBJECT

public:
    explicit AnnotationPropertiesWidget(QWidget* parent = nullptr);
    ~AnnotationPropertiesWidget() override;

    void setAnnotation(const pdfe::annotations::AnnotationData& data);
    void clear();
    void focusContents();

    /// Pipeline f2a8c3e1 phase-2: read back the widget's current AnnotationData
    /// snapshot.  Used by the testAnnot*Pipe_ handlers in MainWindow to build a
    /// patched copy without driving the QColorDialog or UIA QLineEdit paths.
    pdfe::annotations::AnnotationData currentData() const { return currentData_; }

    Q_INVOKABLE void setAnnotationColor(QColor color);
    Q_INVOKABLE void setAnnotationTextColor(QColor color);

signals:
    void propertiesChanged(pdfe::annotations::AnnotationData data);

private slots:
    void onColorButtonClicked();
    void onOpacityChanged(int value);
    void onContentsChanged();
    void onAuthorChanged(const QString& text);
    void onFontChanged(int index);
    void onFontSizeChanged(int size);
    void onTextColorClicked();

private:
    void emitChanges();
    void updateColorButton();

    pdfe::annotations::AnnotationData currentData_;
    bool updatingFields_ = false;

    QLabel* typeLabel_ = nullptr;
    QLineEdit* authorEdit_ = nullptr;
    QTextEdit* contentsEdit_ = nullptr;
    QPushButton* colorButton_ = nullptr;
    QSlider* opacitySlider_ = nullptr;
    QLabel* opacityLabel_ = nullptr;
    QGroupBox* fontGroup_ = nullptr;
    QComboBox* fontCombo_ = nullptr;
    QSpinBox* fontSizeSpinner_ = nullptr;
    QPushButton* textColorButton_ = nullptr;
};

} // namespace pdfe::ui
