#pragma once

#include "batch/BatchTypes.h"
#include <QDialog>
#include <memory>
#include <vector>
#include <filesystem>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::batch {
class BatchEngine;
}

class QListWidget;
class QPushButton;
class QComboBox;
class QProgressBar;
class QTextEdit;
class QLabel;
class QStackedWidget;
class QLineEdit;
class QDoubleSpinBox;
class QCheckBox;

namespace pdfe::ui {

class BatchDialog : public QDialog {
    Q_OBJECT

public:
    explicit BatchDialog(
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        QWidget* parent = nullptr
    );
    ~BatchDialog() override;

private slots:
    void onSelectFolder();
    void onAddAction();
    void onRemoveAction();
    void onMoveUp();
    void onMoveDown();
    void onRun();
    void onCancel();
    void onActionTypeChanged(int index);

private:
    void setupUi();
    void updateActionList();
    pdfe::batch::BatchAction buildActionFromUi() const;

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::vector<pdfe::batch::BatchAction> actions_;
    std::vector<std::filesystem::path> inputFiles_;
    pdfe::batch::BatchEngine* activeEngine_ = nullptr;
    bool running_ = false;

    // File selection
    QLabel* folderLabel_;
    QPushButton* selectFolderButton_;

    // Action list
    QListWidget* actionListWidget_;
    QPushButton* addActionButton_;
    QPushButton* removeActionButton_;
    QPushButton* moveUpButton_;
    QPushButton* moveDownButton_;

    // Action config
    QComboBox* actionTypeCombo_;
    QStackedWidget* configStack_;

    // Watermark config panel
    QLineEdit* watermarkTextEdit_;
    QDoubleSpinBox* watermarkOpacitySpin_;
    QComboBox* watermarkPositionCombo_;

    // PDF/A config panel
    QComboBox* pdfaLevelCombo_;

    // Permissions config panel
    QCheckBox* permPrintCheck_;
    QCheckBox* permCopyCheck_;
    QCheckBox* permEditCheck_;
    QLineEdit* ownerPasswordEdit_;

    // Execution
    QPushButton* runButton_;
    QPushButton* cancelButton_;
    QProgressBar* progressBar_;
    QLabel* statusLabel_;
    QTextEdit* errorLog_;
};

} // namespace pdfe::ui
