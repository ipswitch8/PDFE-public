#pragma once
#include "tre/TreTypes.h"
#include <QDialog>

class QTextEdit;
class QLabel;
class QRadioButton;
class QDialogButtonBox;
class QGroupBox;

namespace pdfe::tre {

struct ReconstructionResult;  // forward declare from RegionReconstructor.h

class TreEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit TreEditDialog(const ReconstructionResult& analysisResult, QWidget* parent = nullptr);

    /// Get the (possibly edited) text from the dialog.
    QString editedText() const;

    /// Get the user-selected approach (B or C).
    Approach selectedApproach() const;

private:
    QTextEdit* textEdit_ = nullptr;
    QLabel* qualityLabel_ = nullptr;
    QLabel* fontLabel_ = nullptr;
    QRadioButton* approachBRadio_ = nullptr;
    QRadioButton* approachCRadio_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};

} // namespace pdfe::tre
