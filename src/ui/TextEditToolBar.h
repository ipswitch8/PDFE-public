#pragma once

#include <QToolBar>
#include <QColor>

class QComboBox;
class QFontComboBox;
class QDoubleSpinBox;
class QToolButton;

namespace pdfe::ui {

class TextEditToolBar : public QToolBar {
    Q_OBJECT

public:
    explicit TextEditToolBar(QWidget* parent = nullptr);

    void setFontInfo(const QString& fontName, float fontSize);
    void clearFontInfo();

    void setSelectionFormatting(const QString& fontFamily, float fontSize,
                                bool bold, bool italic,
                                bool underline, bool strikethrough,
                                QColor textColor = QColor(Qt::black));

    /// Test seam: drive the font-size spinbox exactly as a user edit would —
    /// clears the updatingFromSelection_ guard so valueChanged emits
    /// fontSizeChanged → TextEditTool::applyFontSize, applying the size to the
    /// current selection. Lets the native-UI test change font size
    /// deterministically without a UIA set-value gesture on the spinbox, which
    /// silently no-ops in a non-interactive CI runner session (see
    /// CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #2). Test-only;
    /// reached through the PDFE_TEST_PIPES seam in MainWindow.
    void setFontSizeForTesting(float sizePt);

    /// Current font-size spinbox value in points (0.0 when the "-" special
    /// value is shown, i.e. no selection). Used as the deterministic readback
    /// confirming setFontSizeForTesting() applied before the canvas assertion.
    float currentFontSizeForTesting() const;

    /// Show/hide atlas mode controls (only relevant for image-dominated pages).
    void showAtlasControls(bool visible);

    /// Set current atlas mode programmatically (with guard to prevent signal).
    void setAtlasMode(int mode);

signals:
    void fontFamilyChanged(const QString& family);
    void fontSizeChanged(float size);
    void boldToggled(bool checked);
    void italicToggled(bool checked);
    void underlineToggled(bool checked);
    void strikethroughToggled(bool checked);
    void textColorChanged(const QColor& color);
    void alignmentChanged(Qt::Alignment alignment);
    void atlasModeChanged(int mode);
    void compareAtlasRequested();

private:
    QFontComboBox* fontComboBox_ = nullptr;
    QDoubleSpinBox* fontSizeSpinBox_ = nullptr;
    QToolButton* boldButton_ = nullptr;
    QToolButton* italicButton_ = nullptr;
    QToolButton* underlineButton_ = nullptr;
    QToolButton* strikethroughButton_ = nullptr;
    QToolButton* textColorButton_ = nullptr;
    QColor currentTextColor_{Qt::black};

    // Alignment buttons
    QToolButton* alignLeftButton_ = nullptr;
    QToolButton* alignCenterButton_ = nullptr;
    QToolButton* alignRightButton_ = nullptr;
    QToolButton* alignJustifyButton_ = nullptr;

    // Atlas mode controls (hidden until image-dominated page)
    QAction* atlasSeparatorAction_ = nullptr;
    QComboBox* atlasModeCombo_ = nullptr;
    QAction* atlasModeComboAction_ = nullptr;
    QToolButton* compareButton_ = nullptr;
    QAction* compareButtonAction_ = nullptr;

    bool updatingFromSelection_ = false;
};

} // namespace pdfe::ui
