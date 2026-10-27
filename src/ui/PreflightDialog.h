#pragma once

#include <QDialog>
#include <memory>
#include <vector>

#include "compliance/ComplianceTypes.h"
#include "compliance/ComplianceManager.h"

class QComboBox;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;
class QDialogButtonBox;

namespace pdfe::core { class Document; class MuPdfContext; }

namespace pdfe::ui {

/// Preflight dialog — runs compliance validators against the current
/// document and presents findings in a grouped tree. The user picks a
/// target profile (PDF/A-1b, 2b, 2u, 2a, 3b, or PDF/UA), clicks Run,
/// and gets back a tree of errors + warnings with ISO clause refs
/// and page numbers. Reuses ComplianceManager's existing validator
/// infrastructure — the dialog is presentation + one-click export
/// of the findings.
class PreflightDialog : public QDialog {
    Q_OBJECT
public:
    PreflightDialog(pdfe::core::Document* doc,
                    std::shared_ptr<pdfe::core::MuPdfContext> ctx,
                    QWidget* parent = nullptr);
    ~PreflightDialog() override;

    /// Populate the result tree from an already-computed
    /// ComplianceResult. Public for tests that want to drive the UI
    /// without running a validator.
    void setResult(const pdfe::compliance::ComplianceResult& result);

    /// Read-only access to the currently-displayed result. Returns
    /// an empty result when nothing has been run yet.
    const pdfe::compliance::ComplianceResult& currentResult() const {
        return currentResult_;
    }

    /// Pipeline E phase-12: how many auto-fixable issues are currently
    /// shown in the tree. Used by tests and to drive the Fix All button
    /// enabled state.
    int autoFixableCount() const;

    /// Pipeline E phase-12: trigger the Fix All workflow programmatically
    /// (same code path as the button). Returns the FixAllResult so tests
    /// can assert on it without relying on a live PDF document.
    pdfe::compliance::ComplianceManager::FixAllResult runFixAll();

signals:
    void reportExportRequested(const QString& savePath);
    /// Pipeline E phase-12: emitted after a Fix All pass completes so
    /// the host (MainWindow) can mark the document dirty + repaint.
    void fixAllCompleted(int succeeded, int failed);

private slots:
    void onRunClicked();
    void onExportClicked();
    void onFixAllClicked();

private:
    pdfe::compliance::ComplianceResult runValidation();
    void refreshTree();
    void refreshSummary();
    void refreshFixAllButton();

    pdfe::core::Document* doc_ = nullptr;
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::compliance::ComplianceManager> manager_;

    QComboBox* profileCombo_ = nullptr;
    QTreeWidget* issueTree_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QPushButton* runButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* fixAllButton_ = nullptr; ///< Pipeline E phase-12
    QDialogButtonBox* closeBox_ = nullptr;

    pdfe::compliance::ComplianceResult currentResult_;
};

} // namespace pdfe::ui
