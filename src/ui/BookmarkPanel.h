#pragma once

#include <QWidget>
#include <memory>
#include <vector>

class QAction;
class QTreeWidget;
class QTreeWidgetItem;
class QPoint;
class QMenu;

namespace pdfe::core {
class Document;
class MuPdfContext;
class BookmarkManager;
struct BookmarkNode;
}

namespace pdfe::commands {
class UndoManager;
}

namespace pdfe::ui {

class BookmarkPanel : public QWidget {
    Q_OBJECT

public:
    explicit BookmarkPanel(QWidget* parent = nullptr);
    ~BookmarkPanel() override;

    void setContext(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    void setUndoManager(pdfe::commands::UndoManager* undoManager);
    void setCurrentPage(int pageIndex) { currentPageIndex_ = pageIndex; }

    void setDocument(pdfe::core::Document* doc);
    void clearDocument();
    void refresh();

    // Flush in-memory bookmark edits to the PDF /Outlines tree before
    // the document is serialized. Call from MainWindow::onSaveFile /
    // onSaveAsFile prior to Document::save / saveAs. No-op when there
    // is no manager or no current document.
    void commitToDocument(pdfe::core::Document* doc);

    // Test seam: populate the bookmark context menu with all 9 actions
    // (objectNames + accessibleNames). When @p itemSelected is false,
    // only the "New Bookmark at Root" action is added — matching the
    // behavior of right-clicking on empty space in the tree.
    void populateBookmarkContextMenu(QMenu* menu, bool itemSelected);

    /// Pipeline a7c4f891 phase-2 (D4 / P-GAP-82): programmatic
    /// bookmark-insertion seam for tests. Creates a root-level
    /// bookmark pointing to @p pageIndex with the given @p title,
    /// bypassing the QInputDialog::getText() modal that onCreateAtRoot
    /// uses. Returns true if the bookmark was created (i.e. a
    /// document is loaded and manager_ is initialized). Pattern
    /// mirrors CalculationOrderDialog::moveProgrammatic + 3e9b1d7c
    /// phase-3's general Q_INVOKABLE seam approach.
    Q_INVOKABLE bool insertBookmark(int pageIndex, const QString& title);

signals:
    void bookmarkActivated(int pageIndex, float x, float y, float zoom);
    void externalLinkRequested(const QString& uri);

private slots:
    void onItemActivated(QTreeWidgetItem* item, int column);
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onContextMenu(const QPoint& pos);
    void onCreateAtRoot();
    void onCreateChild();
    void onRename();
    void onDelete();
    void onMoveUp();
    void onMoveDown();
    void onIndent();
    void onOutdent();

private:
    void populateTree(const std::vector<std::shared_ptr<pdfe::core::BookmarkNode>>& nodes);
    QTreeWidgetItem* createTreeItem(const pdfe::core::BookmarkNode& node, QTreeWidgetItem* parent = nullptr);
    std::vector<int> pathForItem(QTreeWidgetItem* item) const;

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    pdfe::commands::UndoManager* undoManager_ = nullptr;
    pdfe::core::Document* document_ = nullptr;
    std::unique_ptr<pdfe::core::BookmarkManager> manager_;
    int currentPageIndex_ = 0;
    bool suppressItemChanged_ = false;

    QTreeWidget* treeWidget_ = nullptr;

    // GAP-58-pattern fix (Phase-C of pipeline 7d4b2f91): bookmark
    // context-menu QActions promoted from locals inside
    // populateBookmarkContextMenu()/onContextMenu() to persistent
    // member variables created in the constructor. Previously the
    // QActions were transient (gone after menu.exec() returned), so
    // the UIA tree couldn't find them by objectName outside the
    // menu lifecycle. Members are owned by `this` and remain visible
    // to UIA whenever the panel is alive. The local QMenu in the
    // slot now just borrows these members via menu.addAction(member).
    QAction* newBookmarkAction_ = nullptr;
    QAction* newChildBookmarkAction_ = nullptr;
    QAction* renameBookmarkAction_ = nullptr;
    QAction* deleteBookmarkAction_ = nullptr;
    QAction* moveBookmarkUpAction_ = nullptr;
    QAction* moveBookmarkDownAction_ = nullptr;
    QAction* indentBookmarkAction_ = nullptr;
    QAction* outdentBookmarkAction_ = nullptr;
};

} // namespace pdfe::ui
