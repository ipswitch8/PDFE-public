#include "BookmarkPanel.h"
#include "../core/Document.h"
#include "../core/BookmarkNode.h"
#include "../core/BookmarkManager.h"
#include "../core/MuPdfContext.h"
#include "../core/UndoManager.h"

#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>

namespace pdfe::ui {

BookmarkPanel::BookmarkPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("bookmarkPanel");
    setAccessibleName(tr("Bookmarks Panel"));

    treeWidget_ = new QTreeWidget(this);
    treeWidget_->setObjectName("bookmarkTree");
    treeWidget_->setAccessibleName(tr("Bookmarks Tree"));
    treeWidget_->setHeaderHidden(true);
    treeWidget_->setColumnCount(1);
    treeWidget_->setAnimated(true);
    treeWidget_->setExpandsOnDoubleClick(false);
    treeWidget_->setContextMenuPolicy(Qt::CustomContextMenu);
    treeWidget_->setDragDropMode(QAbstractItemView::InternalMove);
    treeWidget_->setEditTriggers(QAbstractItemView::NoEditTriggers);  // Use context menu instead

    connect(treeWidget_, &QTreeWidget::itemActivated,
            this, &BookmarkPanel::onItemActivated);
    connect(treeWidget_, &QTreeWidget::itemChanged,
            this, &BookmarkPanel::onItemChanged);
    connect(treeWidget_, &QTreeWidget::customContextMenuRequested,
            this, &BookmarkPanel::onContextMenu);

    // GAP-58-pattern fix: create the context-menu QActions as persistent
    // child QObjects of this panel (not locals inside the slot) so the
    // UIA tree exposes them by objectName whether or not the context
    // menu is currently open. The QMenu in populateBookmarkContextMenu
    // just borrows these member actions via addAction().
    newBookmarkAction_ = new QAction(tr("New Bookmark at Root..."), this);
    newBookmarkAction_->setObjectName("newBookmarkAction");
    connect(newBookmarkAction_, &QAction::triggered, this, &BookmarkPanel::onCreateAtRoot);

    newChildBookmarkAction_ = new QAction(tr("New Child Bookmark..."), this);
    newChildBookmarkAction_->setObjectName("newChildBookmarkAction");
    connect(newChildBookmarkAction_, &QAction::triggered, this, &BookmarkPanel::onCreateChild);

    renameBookmarkAction_ = new QAction(tr("Rename..."), this);
    renameBookmarkAction_->setObjectName("renameBookmarkAction");
    connect(renameBookmarkAction_, &QAction::triggered, this, &BookmarkPanel::onRename);

    deleteBookmarkAction_ = new QAction(tr("Delete"), this);
    deleteBookmarkAction_->setObjectName("deleteBookmarkAction");
    connect(deleteBookmarkAction_, &QAction::triggered, this, &BookmarkPanel::onDelete);

    moveBookmarkUpAction_ = new QAction(tr("Move Up"), this);
    moveBookmarkUpAction_->setObjectName("moveBookmarkUpAction");
    connect(moveBookmarkUpAction_, &QAction::triggered, this, &BookmarkPanel::onMoveUp);

    moveBookmarkDownAction_ = new QAction(tr("Move Down"), this);
    moveBookmarkDownAction_->setObjectName("moveBookmarkDownAction");
    connect(moveBookmarkDownAction_, &QAction::triggered, this, &BookmarkPanel::onMoveDown);

    indentBookmarkAction_ = new QAction(tr("Indent (Make Child)"), this);
    indentBookmarkAction_->setObjectName("indentBookmarkAction");
    connect(indentBookmarkAction_, &QAction::triggered, this, &BookmarkPanel::onIndent);

    outdentBookmarkAction_ = new QAction(tr("Outdent (Move to Parent Level)"), this);
    outdentBookmarkAction_->setObjectName("outdentBookmarkAction");
    connect(outdentBookmarkAction_, &QAction::triggered, this, &BookmarkPanel::onOutdent);

    // Phase-C Pipeline 7d4b2f91 GAP-61 (NEW finding): QActions
    // parented to a QWidget but NOT added via addAction() are NOT
    // enumerated by Qt's accessibility bridge — they remain UIA-
    // invisible until attached to a menu/toolbar at render time.
    // Calling widget->addAction(act) registers them with the widget's
    // actions() list, which IS enumerated by QAccessibleWidget. The
    // actions then appear in the UIA tree under the panel even when
    // the context menu is closed.
    for (QAction* a : {newBookmarkAction_, newChildBookmarkAction_,
                       renameBookmarkAction_, deleteBookmarkAction_,
                       moveBookmarkUpAction_, moveBookmarkDownAction_,
                       indentBookmarkAction_, outdentBookmarkAction_}) {
        if (a) addAction(a);
    }

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(treeWidget_);
    setLayout(layout);
}

BookmarkPanel::~BookmarkPanel() = default;

void BookmarkPanel::setContext(std::shared_ptr<pdfe::core::MuPdfContext> ctx) {
    context_ = std::move(ctx);
    if (context_) {
        manager_ = std::make_unique<pdfe::core::BookmarkManager>(context_);
    }
}

void BookmarkPanel::setUndoManager(pdfe::commands::UndoManager* undoManager) {
    undoManager_ = undoManager;
}

void BookmarkPanel::setDocument(pdfe::core::Document* doc)
{
    document_ = doc;
    refresh();
}

void BookmarkPanel::clearDocument()
{
    document_ = nullptr;
    treeWidget_->clear();
    if (manager_) {
        // Reset manager's tree by reloading a null doc
    }
}

void BookmarkPanel::commitToDocument(pdfe::core::Document* doc)
{
    if (!manager_ || !doc) return;
    manager_->commit(doc);
}

void BookmarkPanel::refresh()
{
    suppressItemChanged_ = true;
    treeWidget_->clear();

    if (!document_ || !manager_) {
        suppressItemChanged_ = false;
        return;
    }

    try {
        auto bookmarks = manager_->load(document_);
        populateTree(bookmarks);
    } catch (...) {
        treeWidget_->clear();
    }
    suppressItemChanged_ = false;
}

void BookmarkPanel::onItemActivated(QTreeWidgetItem* item, int column)
{
    (void)column;
    if (!item) return;

    QVariant typeVar = item->data(0, Qt::UserRole);
    if (!typeVar.isValid()) return;

    int destType = typeVar.toInt();

    if (destType == static_cast<int>(pdfe::core::BookmarkDestType::Page)) {
        int pageIndex = item->data(0, Qt::UserRole + 1).toInt();
        float x = item->data(0, Qt::UserRole + 2).toFloat();
        float y = item->data(0, Qt::UserRole + 3).toFloat();
        float zoom = item->data(0, Qt::UserRole + 4).toFloat();
        emit bookmarkActivated(pageIndex, x, y, zoom);
    } else if (destType == static_cast<int>(pdfe::core::BookmarkDestType::URI)) {
        QString uri = item->data(0, Qt::UserRole + 5).toString();
        emit externalLinkRequested(uri);
    }
}

void BookmarkPanel::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (suppressItemChanged_ || !item || column != 0 || !manager_) return;

    auto path = pathForItem(item);
    if (path.empty()) return;

    std::string newTitle = item->text(0).toStdString();
    auto node = manager_->nodeAt(path);
    if (!node || node->title == newTitle) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::RenameBookmarkCommand>(
            manager_.get(), path, newTitle);
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->renameBookmark(path, newTitle);
    }
}

void BookmarkPanel::populateBookmarkContextMenu(QMenu* menu, bool itemSelected)
{
    if (!menu) return;
    menu->setObjectName("bookmarkContextMenu");
    menu->setAccessibleName(tr("Bookmark Context Menu"));

    // GAP-58-pattern fix: borrow the persistent member actions
    // (created in the constructor). The QMenu is transient — the
    // QActions are not.
    menu->addAction(newBookmarkAction_);

    if (itemSelected) {
        menu->addAction(newChildBookmarkAction_);
        menu->addSeparator();
        menu->addAction(renameBookmarkAction_);
        menu->addAction(deleteBookmarkAction_);
        menu->addSeparator();
        menu->addAction(moveBookmarkUpAction_);
        menu->addAction(moveBookmarkDownAction_);
        menu->addSeparator();
        menu->addAction(indentBookmarkAction_);
        menu->addAction(outdentBookmarkAction_);
    }
}

void BookmarkPanel::onContextMenu(const QPoint& pos)
{
    if (!manager_) return;

    QMenu menu(this);
    auto* item = treeWidget_->itemAt(pos);
    populateBookmarkContextMenu(&menu, item != nullptr);
    menu.exec(treeWidget_->viewport()->mapToGlobal(pos));
}

void BookmarkPanel::onCreateAtRoot()
{
    if (!manager_) return;
    bool ok;
    QString title = QInputDialog::getText(this, tr("New Bookmark"),
        tr("Bookmark title:"), QLineEdit::Normal, tr("New Bookmark"), &ok);
    if (!ok || title.isEmpty()) return;

    pdfe::core::BookmarkDest dest;
    dest.type = pdfe::core::BookmarkDestType::Page;
    dest.pageIndex = currentPageIndex_;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::CreateBookmarkCommand>(
            manager_.get(), pdfe::core::BookmarkPath{}, title.toStdString(), dest);
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->createBookmark({}, title.toStdString(), dest);
    }
    refresh();
}

bool BookmarkPanel::insertBookmark(int pageIndex, const QString& title)
{
    // Pipeline a7c4f891 phase-2 (D4 / P-GAP-82): same model-side
    // effect as onCreateAtRoot but bypasses the QInputDialog::
    // getText() modal that tests cannot drive synchronously. Uses
    // the undoManager-routed CreateBookmarkCommand when available
    // so undo/redo still works; falls back to direct manager call
    // when no undo manager (test harnesses).
    if (!manager_) return false;
    if (title.isEmpty()) return false;

    pdfe::core::BookmarkDest dest;
    dest.type = pdfe::core::BookmarkDestType::Page;
    dest.pageIndex = pageIndex;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::CreateBookmarkCommand>(
            manager_.get(), pdfe::core::BookmarkPath{}, title.toStdString(), dest);
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->createBookmark({}, title.toStdString(), dest);
    }
    refresh();
    return true;
}

void BookmarkPanel::onCreateChild()
{
    if (!manager_) return;
    auto* item = treeWidget_->currentItem();
    if (!item) return;

    bool ok;
    QString title = QInputDialog::getText(this, tr("New Child Bookmark"),
        tr("Bookmark title:"), QLineEdit::Normal, tr("New Child"), &ok);
    if (!ok || title.isEmpty()) return;

    pdfe::core::BookmarkDest dest;
    dest.type = pdfe::core::BookmarkDestType::Page;
    dest.pageIndex = currentPageIndex_;

    auto parentPath = pathForItem(item);

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::CreateBookmarkCommand>(
            manager_.get(), parentPath, title.toStdString(), dest);
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->createBookmark(parentPath, title.toStdString(), dest);
    }
    refresh();
}

void BookmarkPanel::onRename()
{
    if (!manager_) return;
    auto* item = treeWidget_->currentItem();
    if (!item) return;

    bool ok;
    QString current = item->text(0);
    QString newName = QInputDialog::getText(this, tr("Rename Bookmark"),
        tr("New name:"), QLineEdit::Normal, current, &ok);
    if (!ok || newName.isEmpty() || newName == current) return;

    auto path = pathForItem(item);
    if (path.empty()) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::RenameBookmarkCommand>(
            manager_.get(), path, newName.toStdString());
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->renameBookmark(path, newName.toStdString());
    }
    refresh();
}

void BookmarkPanel::onDelete()
{
    if (!manager_) return;
    auto* item = treeWidget_->currentItem();
    if (!item) return;

    auto confirm = QMessageBox::question(this, tr("Delete Bookmark"),
        tr("Delete bookmark '%1' and all its children?").arg(item->text(0)),
        QMessageBox::Yes | QMessageBox::No);
    if (confirm != QMessageBox::Yes) return;

    auto path = pathForItem(item);
    if (path.empty()) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::DeleteBookmarkCommand>(manager_.get(), path);
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->deleteBookmark(path);
    }
    refresh();
}

void BookmarkPanel::onMoveUp()
{
    if (!manager_) return;
    auto* item = treeWidget_->currentItem();
    if (!item) return;
    auto path = pathForItem(item);
    if (path.empty()) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::MoveBookmarkCommand>(
            manager_.get(), path, pdfe::core::MoveBookmarkCommand::Direction::Up);
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->moveBookmarkUp(path);
    }
    refresh();
}

void BookmarkPanel::onMoveDown()
{
    if (!manager_) return;
    auto* item = treeWidget_->currentItem();
    if (!item) return;
    auto path = pathForItem(item);
    if (path.empty()) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::MoveBookmarkCommand>(
            manager_.get(), path, pdfe::core::MoveBookmarkCommand::Direction::Down);
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->moveBookmarkDown(path);
    }
    refresh();
}

void BookmarkPanel::onIndent()
{
    if (!manager_) return;
    auto* item = treeWidget_->currentItem();
    if (!item) return;
    auto path = pathForItem(item);
    if (path.empty()) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::MoveBookmarkCommand>(
            manager_.get(), path, pdfe::core::MoveBookmarkCommand::Direction::Indent);
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->indentBookmark(path);
    }
    refresh();
}

void BookmarkPanel::onOutdent()
{
    if (!manager_) return;
    auto* item = treeWidget_->currentItem();
    if (!item) return;
    auto path = pathForItem(item);
    if (path.empty()) return;

    if (undoManager_) {
        auto cmd = std::make_unique<pdfe::core::MoveBookmarkCommand>(
            manager_.get(), path, pdfe::core::MoveBookmarkCommand::Direction::Outdent);
        undoManager_->execute(std::move(cmd));
    } else {
        manager_->outdentBookmark(path);
    }
    refresh();
}

std::vector<int> BookmarkPanel::pathForItem(QTreeWidgetItem* item) const
{
    std::vector<int> path;
    QTreeWidgetItem* node = item;
    while (node) {
        QTreeWidgetItem* parent = node->parent();
        int index;
        if (parent) {
            index = parent->indexOfChild(node);
        } else {
            index = treeWidget_->indexOfTopLevelItem(node);
        }
        path.insert(path.begin(), index);
        node = parent;
    }
    return path;
}

void BookmarkPanel::populateTree(
    const std::vector<std::shared_ptr<pdfe::core::BookmarkNode>>& nodes)
{
    for (const auto& node : nodes) {
        if (node) {
            createTreeItem(*node, nullptr);
        }
    }
}

QTreeWidgetItem* BookmarkPanel::createTreeItem(
    const pdfe::core::BookmarkNode& node,
    QTreeWidgetItem* parent)
{
    QTreeWidgetItem* item = nullptr;

    if (parent) {
        item = new QTreeWidgetItem(parent);
    } else {
        item = new QTreeWidgetItem(treeWidget_);
    }

    item->setText(0, QString::fromStdString(node.title));
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    item->setData(0, Qt::UserRole, static_cast<int>(node.destination.type));

    if (node.destination.type == pdfe::core::BookmarkDestType::Page) {
        item->setData(0, Qt::UserRole + 1, node.destination.pageIndex);
        item->setData(0, Qt::UserRole + 2, node.destination.x);
        item->setData(0, Qt::UserRole + 3, node.destination.y);
        item->setData(0, Qt::UserRole + 4, node.destination.zoom);
    } else if (node.destination.type == pdfe::core::BookmarkDestType::URI) {
        item->setData(0, Qt::UserRole + 5, QString::fromStdString(node.destination.uri));
    } else if (node.destination.type == pdfe::core::BookmarkDestType::NamedDest) {
        item->setData(0, Qt::UserRole + 6, QString::fromStdString(node.destination.namedDest));
    }

    if (node.isOpen) {
        item->setExpanded(true);
    }

    for (const auto& child : node.children) {
        if (child) {
            createTreeItem(*child, item);
        }
    }

    return item;
}

} // namespace pdfe::ui
