#pragma once

#include "CombineFilesService.h"

#include <QDialog>
#include <filesystem>
#include <memory>
#include <vector>

class QListWidget;
class QPushButton;
class QLineEdit;
class QStackedWidget;
class QLabel;
class QProgressBar;

namespace pdfe::core { class MuPdfContext; }

namespace pdfe::ui {

/// Pipeline E phase-11: Combine Files wizard.
/// Three-step flow:
///   step 1: file list picker (add / remove / move up / move down)
///   step 2: output path picker
///   step 3: progress bar + done message
class CombineFilesWizard : public QDialog {
    Q_OBJECT

public:
    explicit CombineFilesWizard(
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        QWidget* parent = nullptr);

    /// Programmatic seed (used by tests and by callers who already
    /// have a list of inputs).
    void setInputFiles(const std::vector<std::filesystem::path>& inputs);
    void setOutputPath(const std::filesystem::path& output);

    /// Test seam: append a single file by path to the file list.
    /// Returns true if the path is non-empty; false otherwise.
    /// Q_INVOKABLE so pywinauto/UIA tests can call it via QMetaObject.
    Q_INVOKABLE bool addFileByPath(const QString& path);

    /// Run the merge synchronously. Returns the service result; UI
    /// stays on step 3 with progress visible. Used by both the
    /// interactive flow and tests.
    CombineFilesService::Result runCombine();

    /// Current step index, 0-based.
    int currentStepIndex() const;

    /// Current input files (post-edits).
    std::vector<std::filesystem::path> inputFiles() const;

    /// Current output path (post-edits).
    std::filesystem::path outputPath() const;

private slots:
    void onAddFiles();
    void onRemoveFile();
    void onMoveUp();
    void onMoveDown();
    void onChooseOutput();
    void onNext();
    void onBack();
    void onCancel();

private:
    void setupUi();
    void refreshFileList();
    bool canAdvanceFromStep(int step) const;
    void updateButtonsForStep();

    std::shared_ptr<pdfe::core::MuPdfContext> context_;

    QStackedWidget* steps_ = nullptr;

    // Step 1
    QListWidget* fileList_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* moveUpButton_ = nullptr;
    QPushButton* moveDownButton_ = nullptr;

    // Step 2
    QLineEdit* outputEdit_ = nullptr;
    QPushButton* browseOutputButton_ = nullptr;

    // Step 3
    QProgressBar* progressBar_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    // Footer buttons
    QPushButton* backButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
};

} // namespace pdfe::ui
