#pragma once

#include <QString>
#include <QWidget>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}
namespace pdfe::attachments {
class AttachmentManager;
}

class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QLabel;

namespace pdfe::ui {

/// Side panel listing the document's /EmbeddedFiles attachments with
/// Add / Extract / Delete buttons. Backed by pdfe::attachments::
/// AttachmentManager which already ships the sanitization + byte-exact
/// round-trip (Phase-5c-core).
class AttachmentPanel : public QWidget {
    Q_OBJECT
public:
    explicit AttachmentPanel(QWidget* parent = nullptr);
    ~AttachmentPanel() override;

    void setDocument(std::shared_ptr<pdfe::core::MuPdfContext> context,
                     pdfe::core::Document* document);
    void clearDocument();

    /// Rebuild the tree from the current document's attachment list.
    void refresh();

    // --- Test-only seams (GAP-A14 / SKIP=FAIL) -----------------------------
    // Driven via MainWindow's PDFE_TEST_PIPES pipes so the attachment flows
    // can be exercised without UIA enumeration of the dock's tree, which is
    // unreliable on the Server-2019 CI session. Inert in production (nothing
    // calls them unless a test pipe fires).

    /// Top-level attachment names currently shown in the tree, in order.
    std::vector<QString> attachmentNamesForTesting() const;
    /// Select the tree row whose attachment name equals @p name. Returns
    /// true if found (and updates the Extract/Delete enabled state).
    bool selectAttachmentForTesting(const QString& name);
    /// Run the real Add handler (honours an injected file-dialog path).
    void triggerAddForTesting();
    /// Run the real Extract handler (honours an injected directory path).
    void triggerExtractForTesting();
    /// The visible "attachment list is incomplete" warning, or an empty
    /// string when no warning is shown. Reads the LABEL rather than
    /// re-querying AttachmentManager, so a test cannot pass while the user
    /// sees nothing.
    QString truncationWarningForTesting() const;

signals:
    /// Fires after a successful add/delete so the owner can mark the
    /// document dirty. Extract does NOT mark dirty.
    void attachmentsChanged();

private slots:
    void onAddClicked();
    void onExtractClicked();
    void onDeleteClicked();
    void onSelectionChanged();

private:
    void setupUi();
    QString currentSelectionName() const;

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    pdfe::core::Document* document_ = nullptr;
    std::unique_ptr<pdfe::attachments::AttachmentManager> mgr_;

    QTreeWidget* tree_        = nullptr;
    QPushButton* addButton_   = nullptr;
    QPushButton* extractButton_ = nullptr;
    QPushButton* deleteButton_  = nullptr;
    QLabel*      statusLabel_ = nullptr;
    QLabel*      truncationLabel_ = nullptr;
};

} // namespace pdfe::ui
