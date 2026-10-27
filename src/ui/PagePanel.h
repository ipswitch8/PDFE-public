#pragma once

#include <QWidget>
#include <QImage>
#include <QList>

class QAction;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QMimeData;

namespace pdfe::core {
    class Document;
}

namespace pdfe::rendering {
    class ThumbnailRenderer;
}

namespace pdfe::ui {

class PagePanel : public QWidget {
    Q_OBJECT

public:
    explicit PagePanel(QWidget* parent = nullptr);
    ~PagePanel() override;

    void setDocument(pdfe::core::Document* doc);
    void setCurrentPage(int pageIndex);
    void clearDocument();
    void refreshPages();
    QList<int> selectedPageIndices() const;

    /// Pipeline a7c2e541 phase-17 test-automation hook (closes
    /// KNOWN_WEAK P10-INFRA-1: pyautogui mouseDown/moveTo/mouseUp
    /// drag does not reliably trigger Qt6 QListWidget InternalMove
    /// on this build). Emits ``pageMoved(fromIndex, toIndex)`` after
    /// validating indices; routes through the same MovePageCommand
    /// pipeline as a drag-drop reorder. Returns true on success.
    Q_INVOKABLE bool movePageProgrammatic(int fromIndex, int toIndex);

    /// Pipeline b5e3a9f2 phase-6 (P-GAP-73): programmatic multi-select
    /// for tests. Sets the QListWidget's selection to exactly the
    /// given page indices via QItemSelectionModel — bypassing the
    /// pyautogui Ctrl/Shift+click route which is flaky against Qt6
    /// ExtendedSelection mode (key-event-to-modifier-state timing).
    /// The result is observationally identical to a user multi-select:
    /// the selection model holds the same set of indices, and
    /// subsequent context-menu actions (Delete, Rotate, Extract)
    /// emit the appropriate signals with the multi-page indices list.
    /// Returns true if all indices are in range; false otherwise.
    Q_INVOKABLE bool selectPagesProgrammatic(const QList<int>& pageIndices);

    /// Pipeline f7c2a4e8 phase-3 (B1 / testDragPipe): unified
    /// programmatic drag-drop reorder. This is the higher-level
    /// equivalent of movePageProgrammatic — it lets tests drive the
    /// drag-drop reorder workflow (including the InternalMove event
    /// path Qt uses for QListWidget) directly. movePageProgrammatic
    /// emits pageMoved(from,to) which routes through MovePageCommand;
    /// dragItemProgrammatic does the same plus emits the
    /// pageDragRequested signal (if any subscriber needs to observe
    /// the drag intent vs the completed move).
    /// Returns true if from/to are in range and from != to.
    Q_INVOKABLE bool dragItemProgrammatic(int fromIndex, int toIndex);

    /// R3-15 drag-out: build the QMimeData payload a page drag produces — the
    /// page thumbnail as image data (QMimeData::setImageData, surfaced as
    /// CF_DIB/CF_BITMAP on Windows) plus a text label. This is the SAME payload
    /// a real QDrag initiated from the page panel carries (see PageListWidget::
    /// mimeData). Ownership transfers to the caller; returns nullptr for an
    /// out-of-range index. Lets a headless test assert the drag-out contract
    /// without driving a flaky synthetic mouse-drag gesture.
    QMimeData* buildPageDragMimeData(int pageIndex) const;

    /// GitHub #136 (CI-LIMITATION-PAGEPANEL-POPULATE-RACE): number of rows
    /// currently in the thumbnail QListWidget.
    ///
    /// This is the READINESS fact the UI tests actually need and previously
    /// had no way to ask for. ``generateThumbnails()`` adds every row
    /// synchronously (only the thumbnail IMAGES arrive later, via
    /// onThumbnailReady), so ``panelItemCount() == Document::pageCount()``
    /// is an exact "the panel has finished populating" predicate — not a
    /// heuristic floor and not a sleep. Compare against the document's own
    /// count via the testPagePanelReady pipe in MainWindow; the tests used
    /// to guess with ``_wait_page_items(min_count=3)``, which is satisfied
    /// by a partially-realised UIA tree.
    int panelItemCount() const;

signals:
    void pageSelected(int pageIndex);
    void pageMoved(int fromIndex, int toIndex);
    void pagesDeleteRequested(QList<int> pageIndices);
    void pageInsertRequested(int atIndex);
    void rotatePagesCW(QList<int> pageIndices);
    void rotatePagesCCW(QList<int> pageIndices);
    void rotatePages180(QList<int> pageIndices);
    void extractPagesRequested(QList<int> pageIndices);

private slots:
    void onItemClicked(QListWidgetItem* item);
    void onThumbnailReady(int pageIndex, QImage thumbnail);
    void onContextMenuRequested(const QPoint& pos);

private:
    void generateThumbnails();

    pdfe::core::Document* document_ = nullptr;
    QListWidget* listWidget_ = nullptr;
    pdfe::rendering::ThumbnailRenderer* thumbnailRenderer_ = nullptr;

    // GAP-58 fix: context-menu actions promoted to member variables so
    // they are persistent QObjects in the panel's child tree and the
    // accessibility/UIA layer can locate them by objectName even when
    // the context menu is NOT open. Previously these were created as
    // local QAction* inside onContextMenuRequested() and went out of
    // scope when menu.exec() returned, leaving the UIA tree empty.
    QAction* rotateCWAction_ = nullptr;
    QAction* rotateCCWAction_ = nullptr;
    QAction* rotate180Action_ = nullptr;
    QAction* insertBlankPageAction_ = nullptr;
    QAction* deleteSelectedAction_ = nullptr;
    QAction* extractSelectedAction_ = nullptr;
};

} // namespace pdfe::ui
