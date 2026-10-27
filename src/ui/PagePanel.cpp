#include "PagePanel.h"
#include "../core/Document.h"
#include "../core/PageInfo.h"
#include "../rendering/ThumbnailRenderer.h"

#include <QListWidget>
#include <QVBoxLayout>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QSignalBlocker>
#include <QMenu>
#include <QAction>
#include <QItemSelectionModel>
#include <QDropEvent>
#include <QMimeData>
#include <algorithm>
#include <functional>

namespace pdfe::ui {

// R3-15 drag-out: attach the page's thumbnail (as image data) plus a text
// label to `md` for `item`. Shared by the real-drag mimeData() override and
// the PagePanel::buildPageDragMimeData() test seam so both produce an
// identical payload. On Windows QMimeData image data is surfaced to the
// clipboard/drag as CF_DIB/CF_BITMAP, so an external drop target (another app)
// receives the page as a pasteable bitmap.
static void attachPageThumbToMime(QMimeData* md, const QListWidgetItem* item,
                                  int row) {
    if (!md || !item) return;
    const QIcon icon = item->icon();
    if (!icon.isNull()) {
        QSize sz = icon.availableSizes().value(0, QSize(128, 128));
        if (sz.isEmpty()) sz = QSize(128, 128);
        const QPixmap pm = icon.pixmap(sz);
        if (!pm.isNull()) {
            md->setImageData(pm.toImage());
        }
    }
    const QString label = item->text().isEmpty()
                              ? QString("PDF page %1").arg(row + 1)
                              : item->text();
    md->setText(label);
}

// Custom list widget that handles drag-drop events for page reordering
// (internal move) AND enriches the drag payload with the page thumbnail so a
// page can be dragged OUT to another application as a bitmap (R3-15).
class PageListWidget : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void(int, int)> onPageDropped;

protected:
    void dropEvent(QDropEvent* event) override {
        int fromRow = currentRow();
        QListWidget::dropEvent(event);
        int toRow = currentRow();
        if (fromRow >= 0 && toRow >= 0 && fromRow != toRow && onPageDropped) {
            onPageDropped(fromRow, toRow);
        }
    }

    // R3-15: Qt calls this to build the drag payload. We keep the base
    // internal-move serialization (so drag-drop reorder still works) and add
    // the page thumbnail image + a text label, enabling drag-OUT of a page.
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        QMimeData* md = QListWidget::mimeData(items);
        if (!md) md = new QMimeData();
        if (!items.isEmpty() && items.first()) {
            attachPageThumbToMime(md, items.first(), row(items.first()));
        }
        return md;
    }
};

PagePanel::PagePanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("pagePanel");
    setAccessibleName(tr("Pages Panel"));

    // Create custom list widget with drag-drop support
    auto* pageList = new PageListWidget(this);
    listWidget_ = pageList;
    listWidget_->setObjectName("pagePanelList");
    listWidget_->setAccessibleName(tr("Page Thumbnails"));
    listWidget_->setViewMode(QListWidget::IconMode);
    listWidget_->setIconSize(QSize(128, 128));
    listWidget_->setSpacing(10);
    listWidget_->setResizeMode(QListWidget::Adjust);
    listWidget_->setMovement(QListWidget::Snap);
    listWidget_->setUniformItemSizes(true);

    // Enable drag-drop for page reordering
    listWidget_->setDragDropMode(QAbstractItemView::InternalMove);
    listWidget_->setDefaultDropAction(Qt::MoveAction);
    listWidget_->setSelectionMode(QAbstractItemView::ExtendedSelection);

    // Connect drag-drop callback
    pageList->onPageDropped = [this](int from, int to) {
        emit pageMoved(from, to);
    };

    // Connect item click
    connect(listWidget_, &QListWidget::itemClicked,
            this, &PagePanel::onItemClicked);

    // GAP-58 fix: create the context-menu actions as PERSISTENT child
    // QObjects of this panel (not locals inside onContextMenuRequested)
    // so the UIA tree exposes them by objectName whether or not the
    // context menu is currently open. The QMenu in onContextMenuRequested
    // then just borrows these member actions via addAction().
    rotateCWAction_ = new QAction(tr("Rotate Clockwise"), this);
    rotateCWAction_->setObjectName("rotatePageCWAction");
    rotateCCWAction_ = new QAction(tr("Rotate Counter-Clockwise"), this);
    rotateCCWAction_->setObjectName("rotatePageCCWAction");
    rotate180Action_ = new QAction(tr("Rotate 180°"), this);
    rotate180Action_->setObjectName("rotatePage180Action");
    insertBlankPageAction_ = new QAction(tr("Insert Blank Page"), this);
    insertBlankPageAction_->setObjectName("insertBlankPageAction");
    deleteSelectedAction_ = new QAction(tr("Delete Selected"), this);
    deleteSelectedAction_->setObjectName("deleteSelectedAction");
    extractSelectedAction_ = new QAction(tr("Extract Selected..."), this);
    extractSelectedAction_->setObjectName("extractSelectedAction");

    // Phase-C GAP-61: register actions with widget so QAccessibleWidget
    // enumerates them into the UIA tree (member QActions alone are not
    // UIA-discoverable until attached via addAction()).
    for (QAction* a : {rotateCWAction_, rotateCCWAction_, rotate180Action_,
                       insertBlankPageAction_, deleteSelectedAction_,
                       extractSelectedAction_}) {
        if (a) addAction(a);
    }

    // Setup context menu
    listWidget_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(listWidget_, &QWidget::customContextMenuRequested,
            this, &PagePanel::onContextMenuRequested);

    // Layout
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(listWidget_);
    setLayout(layout);
}

PagePanel::~PagePanel()
{
    if (thumbnailRenderer_) {
        thumbnailRenderer_->cancelAll();
        delete thumbnailRenderer_;
        thumbnailRenderer_ = nullptr;
    }
}

void PagePanel::setDocument(pdfe::core::Document* doc)
{
    // Clean up old renderer
    if (thumbnailRenderer_) {
        thumbnailRenderer_->cancelAll();
        delete thumbnailRenderer_;
        thumbnailRenderer_ = nullptr;
    }

    document_ = doc;
    listWidget_->clear();

    if (!document_) {
        return;
    }

    // Create new thumbnail renderer
    thumbnailRenderer_ = new pdfe::rendering::ThumbnailRenderer(document_, this);
    connect(thumbnailRenderer_, &pdfe::rendering::ThumbnailRenderer::thumbnailReady,
            this, &PagePanel::onThumbnailReady);

    generateThumbnails();
}

void PagePanel::setCurrentPage(int pageIndex)
{
    if (pageIndex < 0 || pageIndex >= listWidget_->count()) {
        return;
    }

    // Block signals to avoid triggering pageSelected
    QSignalBlocker blocker(listWidget_);

    // GitHub #226 -- shift+click selected only the clicked page.
    //
    // The list is already ExtendedSelection, so Qt builds the anchor..clicked
    // range correctly. It was then destroyed on the way back: the click emits
    // pageSelected(clicked), MainWindow::onPageSelected routes to here, and
    // setCurrentRow() carries an implicit ClearAndSelect that collapses the
    // range to the single current row. The QSignalBlocker above suppresses the
    // SIGNAL but not the selection change, which is why this was invisible.
    //
    // "Which page is being viewed" and "which pages are selected" are separate
    // pieces of state, so moving the current index must not silently discard a
    // selection the user built. Preserve it when one exists and already
    // contains the page we are moving to; otherwise fall back to the old
    // select-it behaviour, which is what opening a document relies on to land
    // on page 0.
    const QModelIndex idx = listWidget_->model()->index(pageIndex, 0);
    auto* selection = listWidget_->selectionModel();
    const bool preserveRange =
        selection && selection->selectedIndexes().size() > 1
        && selection->isSelected(idx);

    if (preserveRange) {
        selection->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
    } else {
        listWidget_->setCurrentRow(pageIndex);
    }
}

void PagePanel::clearDocument()
{
    if (thumbnailRenderer_) {
        thumbnailRenderer_->cancelAll();
        delete thumbnailRenderer_;
        thumbnailRenderer_ = nullptr;
    }

    document_ = nullptr;
    listWidget_->clear();
}

QMimeData* PagePanel::buildPageDragMimeData(int pageIndex) const
{
    if (!listWidget_ || pageIndex < 0 || pageIndex >= listWidget_->count()) {
        return nullptr;
    }
    QListWidgetItem* item = listWidget_->item(pageIndex);
    if (!item) return nullptr;
    auto* md = new QMimeData();
    attachPageThumbToMime(md, item, pageIndex);
    return md;
}

void PagePanel::onItemClicked(QListWidgetItem* item)
{
    if (!item) {
        return;
    }

    int pageIndex = listWidget_->row(item);
    emit pageSelected(pageIndex);
}

int PagePanel::panelItemCount() const
{
    return listWidget_ ? listWidget_->count() : 0;
}

void PagePanel::onThumbnailReady(int pageIndex, QImage thumbnail)
{
    if (pageIndex < 0 || pageIndex >= listWidget_->count()) {
        return;
    }

    QListWidgetItem* item = listWidget_->item(pageIndex);
    if (item && !thumbnail.isNull()) {
        item->setIcon(QIcon(QPixmap::fromImage(thumbnail)));
    }
}

void PagePanel::generateThumbnails()
{
    if (!document_ || !thumbnailRenderer_) {
        return;
    }

    int pageCount = document_->pageCount();
    QSize thumbnailSize(128, 128);

    for (int i = 0; i < pageCount; ++i) {
        auto pageInfo = document_->pageInfo(i);

        // Create placeholder thumbnail initially
        QPixmap placeholder(128, 128);
        placeholder.fill(Qt::white);

        QPainter painter(&placeholder);
        painter.setPen(Qt::black);
        painter.drawRect(0, 0, 127, 127);
        painter.drawText(QRect(0, 0, 128, 128), Qt::AlignCenter,
                        QString("Page %1").arg(i + 1));

        // Create list item with placeholder
        QString itemText = pageInfo.label.empty()
                          ? QString::number(i + 1)
                          : QString::fromStdString(pageInfo.label);

        QListWidgetItem* item = new QListWidgetItem(QIcon(placeholder), itemText);
        item->setData(Qt::UserRole, i);
        listWidget_->addItem(item);

        // Request real thumbnail rendering
        thumbnailRenderer_->requestThumbnail(i, thumbnailSize, nullptr);
    }
}

void PagePanel::refreshPages()
{
    if (!document_ || !thumbnailRenderer_) {
        return;
    }

    // Save current selection
    QList<int> selectedIndices = selectedPageIndices();

    // Cancel pending thumbnails
    thumbnailRenderer_->cancelAll();

    // Clear and rebuild list
    listWidget_->clear();

    // Re-generate thumbnails
    generateThumbnails();

    // Restore selection if possible
    QSignalBlocker blocker(listWidget_);
    for (int idx : selectedIndices) {
        if (idx < listWidget_->count()) {
            listWidget_->item(idx)->setSelected(true);
        }
    }
}

QList<int> PagePanel::selectedPageIndices() const
{
    QList<int> indices;
    for (auto* item : listWidget_->selectedItems()) {
        indices.append(listWidget_->row(item));
    }
    std::sort(indices.begin(), indices.end());
    return indices;
}

void PagePanel::onContextMenuRequested(const QPoint& pos)
{
    QListWidgetItem* item = listWidget_->itemAt(pos);
    if (!item) {
        return;
    }

    // GAP-58 fix: build the local QMenu from the persistent member
    // actions (created in the constructor). The QMenu itself is still
    // local — it's just a transient container — but each QAction is
    // owned by `this` and stays in the UIA tree across menu lifecycles.
    QMenu menu(this);
    menu.addAction(rotateCWAction_);
    menu.addAction(rotateCCWAction_);
    menu.addAction(rotate180Action_);
    menu.addSeparator();
    menu.addAction(insertBlankPageAction_);
    menu.addSeparator();
    menu.addAction(deleteSelectedAction_);
    menu.addSeparator();
    menu.addAction(extractSelectedAction_);

    // Execute menu and handle actions
    QAction* selected = menu.exec(listWidget_->mapToGlobal(pos));

    if (!selected) {
        return;
    }

    QList<int> indices = selectedPageIndices();
    if (indices.isEmpty()) {
        return;
    }

    if (selected == rotateCWAction_) {
        emit rotatePagesCW(indices);
    } else if (selected == rotateCCWAction_) {
        emit rotatePagesCCW(indices);
    } else if (selected == rotate180Action_) {
        emit rotatePages180(indices);
    } else if (selected == insertBlankPageAction_) {
        emit pageInsertRequested(indices.first());
    } else if (selected == deleteSelectedAction_) {
        emit pagesDeleteRequested(indices);
    } else if (selected == extractSelectedAction_) {
        emit extractPagesRequested(indices);
    }
}

// ─── Pipeline a7c2e541 phase-17 test-automation hooks ────────────────────

bool PagePanel::movePageProgrammatic(int fromIndex, int toIndex)
{
    if (!document_) return false;
    if (fromIndex < 0 || toIndex < 0) return false;
    const int pc = document_->pageCount();
    if (pc <= 0) return false;
    if (fromIndex >= pc || toIndex >= pc) return false;
    if (fromIndex == toIndex) return false;

    emit pageMoved(fromIndex, toIndex);
    return true;
}

bool PagePanel::dragItemProgrammatic(int fromIndex, int toIndex)
{
    // Pipeline f7c2a4e8 phase-3 (B1): currently routes through the
    // same pageMoved signal as movePageProgrammatic. The distinction
    // exists so tests that specifically test the drag-drop reorder
    // workflow (P-GAP-93 drag_drop_reorder_short / drag_drop_reorder_far)
    // can call dragItemProgrammatic and get a documented "this is a
    // drag-style move" entry-point even though the model-side effect
    // is identical. If a future divergence in drag-vs-move semantics
    // is needed (e.g. emitting a separate pageDragRequested signal
    // before the move), this is the entry-point to extend.
    return movePageProgrammatic(fromIndex, toIndex);
}

bool PagePanel::selectPagesProgrammatic(const QList<int>& pageIndices)
{
    if (!listWidget_ || pageIndices.isEmpty()) return false;
    const int n = listWidget_->count();
    for (int idx : pageIndices) {
        if (idx < 0 || idx >= n) return false;
    }
    listWidget_->clearSelection();
    QItemSelection sel;
    QAbstractItemModel* model = listWidget_->model();
    for (int idx : pageIndices) {
        const QModelIndex mi = model->index(idx, 0);
        sel.select(mi, mi);
    }
    listWidget_->selectionModel()->select(
        sel, QItemSelectionModel::Select);
    // Set the "current" row to the last selected for keyboard
    // continuity (matches real-user multi-click behavior).
    listWidget_->setCurrentRow(
        pageIndices.last(), QItemSelectionModel::NoUpdate);
    return true;
}

} // namespace pdfe::ui
