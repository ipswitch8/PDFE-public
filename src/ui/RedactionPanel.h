#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <vector>

namespace pdfe::redaction {
    struct RedactionData;
}

namespace pdfe::ui {

/// Panel for displaying and managing pending redaction marks.
///
/// Shows all redaction marks with their properties and provides
/// "Apply All" and "Delete Selected" actions.
class RedactionPanel : public QWidget {
    Q_OBJECT

public:
    explicit RedactionPanel(QWidget* parent = nullptr);
    ~RedactionPanel() override;

    /// Populate the panel with pending redaction marks.
    void setMarks(const std::vector<pdfe::redaction::RedactionData>& marks);

    /// Clear all marks from the panel.
    void clear();

    /// Return the number of top-level items (pending marks) in the tree.
    /// Used by the testRedactionMarkCountQueryPipe_ test seam in MainWindow.
    [[nodiscard]] int topLevelItemCount() const;


signals:
    /// Emitted when "Apply All Redactions" button is clicked.
    void applyAllRequested();

    /// Emitted when user requests deletion of a specific mark.
    void deleteMarkRequested(int pageIndex, int annotIndex);

    /// Emitted when user selects/clicks a mark (for navigation).
    void markSelected(int pageIndex, int annotIndex);

    /// GitHub #253. Emitted when the user asks to edit a pending mark's
    /// properties (fill colour / overlay text).
    ///
    /// There was previously no way to modify a mark at all: this panel could
    /// apply and delete, and `ModifyRedactionCommand` was compiled in,
    /// unit-tested and unreachable from main(). So the undo step was not
    /// "missing" -- the operation it undoes did not exist.
    void modifyMarkRequested(int pageIndex, int annotIndex);

private slots:
    void onItemClicked(QTreeWidgetItem* item, int column);
    void onApplyAllClicked();
    void onDeleteClicked();
    void onPropertiesClicked();

private:
    void updateMarkCount();
    QString formatRect(const QRectF& rect) const;
    QString formatColor(const QColor& color) const;

    QTreeWidget* treeWidget_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* propertiesButton_ = nullptr;  // GitHub #253
    QLabel* markCountLabel_ = nullptr;
};

} // namespace pdfe::ui
