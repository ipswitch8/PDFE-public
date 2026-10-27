#pragma once

#include "../core/PageLabels.h"

#include <QDialog>
#include <vector>

namespace pdfe::core {
class Document;
}

namespace pdfe::commands {
class UndoManager;
}

class QTableWidget;
class QPushButton;

namespace pdfe::ui {

/// Pipeline E phase-6 (Bucket 3c): editor for the document's
/// /PageLabels number tree.
///
/// Lists the existing label ranges in a table with one row per
/// range (firstPage / style / prefix / startValue). Add / Edit /
/// Delete buttons mutate the in-memory list; OK writes the new list
/// back to the document via PageLabelManager::write.
class PageLabelsDialog : public QDialog {
    Q_OBJECT

public:
    explicit PageLabelsDialog(pdfe::core::Document* doc,
                              QWidget* parent = nullptr);
    ~PageLabelsDialog() override;

    /// Direct access to the in-memory ranges (for tests).
    const std::vector<pdfe::core::PageLabelRange>& ranges() const {
        return ranges_;
    }
    void setRanges(std::vector<pdfe::core::PageLabelRange> r);

    /// Route accept() through this UndoManager so the label edit is undoable
    /// and triggers autosave (autosave-coverage Phase-2). When null, accept()
    /// falls back to a direct PageLabelManager::write().
    void setUndoManager(pdfe::commands::UndoManager* undoManager) {
        undoManager_ = undoManager;
    }

protected:
    /// Override accept() to write changes to the document.
    void accept() override;

private slots:
    void onAdd();
    void onEdit();
    void onDelete();

private:
    void buildUi();
    void refreshTable();
    bool editRange(pdfe::core::PageLabelRange& inout);

    pdfe::core::Document* doc_ = nullptr;
    pdfe::commands::UndoManager* undoManager_ = nullptr;
    std::vector<pdfe::core::PageLabelRange> ranges_;
    QTableWidget* table_     = nullptr;
    QPushButton*  addBtn_    = nullptr;
    QPushButton*  editBtn_   = nullptr;
    QPushButton*  delBtn_    = nullptr;
    QPushButton*  okBtn_     = nullptr;
    QPushButton*  cancelBtn_ = nullptr;
};

} // namespace pdfe::ui
