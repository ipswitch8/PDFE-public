#pragma once

#include <QDialog>
#include <QRectF>
#include <QSize>
#include <QString>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace pdfe::ui {

/// GitHub #242 — placement and encoding options for Insert Image.
///
/// WHY THIS EXISTS. `onInsertImage()` was a file picker and nothing else:
/// `targetRect` was the literal `QRectF(72, 72, 400, 300)`, so every image
/// landed one inch from the top-left at a fixed 400x300 regardless of the page
/// size, the page orientation, or the image's own proportions. The other three
/// options `InsertImageOptions` already accepts — JPEG quality, EXIF stripping
/// and aspect-ratio locking — were never surfaced at all. EXIF stripping in
/// particular is a privacy-relevant default the user could not see, let alone
/// choose.
///
/// So this is mostly a UI gap rather than a capability gap. The engine took
/// these parameters the whole time.
///
/// WHAT IT DELIBERATELY DOES NOT DO. Rotation is not here. The report asked for
/// it, and the issue guessed it would need an engine change — it does not:
/// `ImageTool` already emits `rotateRequested` (and `resizeRequested`,
/// `cropRequested`), and `MainWindow` already connects all three to real
/// commands. Post-insert manipulation works today. The gap was that nothing put
/// the user in front of it, which `onInsertImage()` now fixes by selecting the
/// inserted image in the image tool so those handles are immediately live.
///
/// AUTOMATION. Every control carries a stable `objectName` and an
/// `accessibleName`, per the project's UI-testing rules, and the dialog
/// registers the standard introspection seam so a modal can be driven and
/// inspected by the UI suite.
class InsertImageDialog : public QDialog {
    Q_OBJECT

public:
    /// @p sourceSize is the image's natural pixel size, used to seed a sensible
    /// default rectangle and to keep the aspect ratio while the user edits.
    /// @p pageSize is the target page in PDF points, so the default placement
    /// can be centred and scaled to fit rather than guessed.
    InsertImageDialog(const QString& imagePath,
                      QSize sourceSize,
                      QSizeF pageSize,
                      QWidget* parent = nullptr);
    ~InsertImageDialog() override;

    /// Placement in PDF points, origin at the page's top-left as the rest of
    /// the insert path expects.
    QRectF targetRect() const;

    int jpegQuality() const;
    bool stripExif() const;
    bool maintainAspectRatio() const;

    /// The default rectangle for @p sourceSize on @p pageSize: the image scaled
    /// to fit inside a one-inch margin without exceeding its natural size, then
    /// centred.
    ///
    /// Exposed so a test can assert on the same rule the dialog uses instead of
    /// reimplementing it — and because the defect being fixed was precisely a
    /// placement that ignored both inputs, which is only visible if the rule is
    /// checkable against several page shapes.
    static QRectF defaultRectFor(QSize sourceSize, QSizeF pageSize);

private slots:
    void onWidthChanged(double value);
    void onHeightChanged(double value);

private:
    void updateSummary();

    QSize sourceSize_;
    QSizeF pageSize_;

    QDoubleSpinBox* x_ = nullptr;
    QDoubleSpinBox* y_ = nullptr;
    QDoubleSpinBox* width_ = nullptr;
    QDoubleSpinBox* height_ = nullptr;
    QCheckBox* keepAspect_ = nullptr;
    QCheckBox* stripExif_ = nullptr;
    QSpinBox* jpegQuality_ = nullptr;
    QLabel* summary_ = nullptr;

    /// Guards the width/height cross-updates so keeping the aspect ratio does
    /// not recurse between the two spin boxes.
    bool updating_ = false;
};

}  // namespace pdfe::ui
