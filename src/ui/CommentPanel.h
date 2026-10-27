#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QComboBox>
#include <memory>
#include <vector>

class QAction;
class QMenu;

namespace pdfe::core {
    class Document;
    class Page;
}

namespace pdfe::annotations {
    class AnnotationManager;
    struct AnnotationData;
    enum class AnnotationType;
}

namespace pdfe::ui {

/// Panel for displaying and managing all annotations in a document.
///
/// Provides filtering by annotation type, sorting, and navigation.
class CommentPanel : public QWidget {
    Q_OBJECT

public:
    explicit CommentPanel(QWidget* parent = nullptr);
    ~CommentPanel() override;

    void setDocument(pdfe::core::Document* doc,
                     pdfe::annotations::AnnotationManager* manager);
    void clearDocument();
    void refresh();

    /// Test seam: populate the comment context menu with the Reply
    /// and Delete actions (objectNames "commentReplyAction" and
    /// "commentDeleteAction"). Caller owns the QMenu.
    void populateCommentContextMenu(QMenu* menu);

    /// Test seam: return the column-0 display text of every annotation
    /// row in the tree (page-header rows excluded), one per entry, in
    /// top-to-bottom order.  Used by the comment-panel-sync UI tests to
    /// read the row inventory from the model instead of via UIA
    /// QTreeWidget enumeration, which is unreliable on Windows Server
    /// 2019 Qt6 builds.  Walks the full tree regardless of viewport.
    std::vector<QString> rowInventoryForTesting() const;

signals:
    void annotationSelected(int pageIndex, int annotIndex);
    void deleteAnnotation(int pageIndex, int annotIndex);
    /// Emitted when the user double-clicks a comment-tree row.
    /// Both pageIndex and annotIndex are provided so the receiver can
    /// scroll the canvas to the annotation's bounding rect, not only to
    /// the top of the page.
    void navigateToAnnotation(int pageIndex, int annotIndex);
    /// Phase 8a UI: user asked to reply to an annotation. Emits the
    /// page and the parent annotation index; the main window is
    /// expected to create a new Text annotation with /IRT set.
    void replyToAnnotation(int pageIndex, int parentAnnotIndex);

private slots:
    void onFilterChanged(int index);
    void onSortChanged(int index);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onContextMenu(const QPoint& pos);

private:
    void buildAnnotationTree();
    bool passesFilter(pdfe::annotations::AnnotationType type) const;
    QString getAnnotationIcon(pdfe::annotations::AnnotationType type) const;

    pdfe::core::Document* document_ = nullptr;
    pdfe::annotations::AnnotationManager* annotManager_ = nullptr;

    QComboBox* filterCombo_ = nullptr;
    QComboBox* sortCombo_ = nullptr;
    QTreeWidget* treeWidget_ = nullptr;

    int currentFilter_ = 0;  // 0 = All
    int currentSort_ = 0;    // 0 = By Page

    // Phase-C GAP-58-pattern fix: persistent context-menu actions.
    QAction* commentReplyAction_ = nullptr;
    QAction* commentDeleteAction_ = nullptr;
};

} // namespace pdfe::ui
