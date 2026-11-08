#pragma once

#include <QWidget>
#include <QImage>
#include <QList>
#include <QString>
#include <QPoint>
#include <QRect>

#include <utility>

class QAction;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QMimeData;
class QResizeEvent;
class QShowEvent;

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

    /// Drop the cached thumbnail for one page and re-render it if visible
    /// (fix-tre-writetarget phase-4). After an in-place page edit (e.g. a TRE
    /// region recomposite) the page's thumbnail is stale; the panel does not
    /// subscribe to Document::markPageDirty, so the caller must ask for this
    /// explicitly or the thumbnail keeps showing the pre-edit image.
    void invalidateThumbnail(int pageIndex);

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

    /// Pipeline f7c2a4e8 phase-3 (B1 / testDragPipe): a MODEL-LEVEL reorder
    /// entry-point, NOT a drag-event driver. GitHub #338 correction: despite
    /// the "drag" in the name, this does exactly what movePageProgrammatic does
    /// — it emits pageMoved(from,to), which routes through MovePageCommand ->
    /// Document::reorderPages. It does NOT exercise startDrag(), QDrag::exec(),
    /// dropEvent(), the drop-indicator geometry, or any part of Qt's
    /// InternalMove event path. The name records "this move came from a
    /// drag-style test" intent only; the model-side effect is identical to
    /// movePageProgrammatic. A test that asserts only this proves the
    /// command/model half, NOT that a real drag gesture reaches it (the #335
    /// lesson). For the REAL event path use: simulateInternalDrop /
    /// internalDropInsertIndexAt (drop dispatch + geometry),
    /// deliverRealDragReorder (the real startDrag()+exec), or the live-gesture
    /// test_ui_page_reorder_drag.py (end-to-end, currently xfail per
    /// P10-INFRA-1). Returns true if from/to are in range and from != to.
    Q_INVOKABLE bool dragItemProgrammatic(int fromIndex, int toIndex);

    /// GitHub #333 test seam: run the REAL within-document drop-reorder dispatch
    /// that the thumbnail list's dropEvent invokes for a same-panel drag.
    /// @p fromRow is the dragged source row; @p insertBeforeIndex is the
    /// drop-indicator insert position (0..pageCount, where pageCount appends).
    /// Applies the same insert-before index math and no-op guard as a real
    /// internal drop and emits pageMoved(from,to) via onPageDropped. Returns the
    /// `to` emitted, or -1 when the drop does not change the order. This is the
    /// identical code path a real gesture's QDropEvent runs, not a mock — the
    /// only thing it bypasses is the un-synthesizable QDrag loop and viewport
    /// hit-testing (see @ref internalDropInsertIndexAt for that half).
    Q_INVOKABLE int simulateInternalDrop(int fromRow, int insertBeforeIndex);

    /// GitHub #333 test seam: the geometry mapping the thumbnail list uses to
    /// turn a drop position (in viewport coordinates) into an insert-before
    /// index. Exposed so a test with real layout geometry can assert the
    /// before/after-midpoint decision. Returns -1 if the list is unavailable.
    Q_INVOKABLE int internalDropInsertIndexAt(const QPoint& viewportPos) const;

    /// GitHub #333 test seam: drive the REAL thumbnail-list dragEnter/drop
    /// overrides for a same-document drag of @p fromRow dropped at
    /// @p viewportPos, using the actual #325 drag payload this panel produces.
    /// Qt does not route a manually-sent QDropEvent to a view's protected
    /// handlers, so a test cannot deliver one through the event loop; this
    /// invokes those handlers directly with a synthetic event (the exact objects
    /// Qt hands them in production). Returns true iff the drag is recognised as
    /// internal and CONSUMED by both handlers (so it would not bubble to
    /// MainWindow's same-document no-op); pageMoved(from,to) fires as a side
    /// effect for an order-changing drop.
    Q_INVOKABLE bool deliverSyntheticInternalDrag(int fromRow, const QPoint& viewportPos);

    /// GitHub #335 test seam: drive the REAL QDrag initiation path — the thumbnail
    /// list's startDrag() override — for a same-document drag of @p fromRow
    /// dropped at @p viewportPos. Unlike deliverSyntheticInternalDrag (which calls
    /// dropEvent directly and so NEVER exercises startDrag/QDrag::exec, the gap
    /// that let GitHub #335 through), this selects the source row and invokes the
    /// real startDrag() override, which builds the QDrag, execs it (via a test
    /// hook that delivers the drop deterministically instead of the blocking OS
    /// drag loop), and returns WITHOUT the base clearOrRemove() tail. Returns true
    /// iff the drag was recognised + consumed as internal; pageMoved(from,to)
    /// fires as a side effect, and any resulting refreshPages() is deferred out of
    /// the (simulated) drag loop exactly as in production.
    Q_INVOKABLE bool deliverRealDragReorder(int fromRow, const QPoint& viewportPos);

    /// GitHub #335 residual observability: the number of times the thumbnail
    /// list's ``PageListWidget::startDrag()`` override has been ENTERED since
    /// process start (counts both the real-gesture path and the deterministic
    /// seams). A live-gesture UI test reads this before/after a synthetic
    /// SendInput drag: if it does NOT increment, the gesture died at Qt's
    /// drag-INITIATION threshold (QAbstractItemView::mouseMoveEvent never
    /// crossed startDragDistance from the batched synthetic moves) — i.e.
    /// UPSTREAM of the override #335 added, which is why #335 fixes the
    /// post-initiation clearOrRemove tail but cannot make the synthetic gesture
    /// driveable. Surfaced to the Python harness via MainWindow's view-state
    /// pipe (field ``pageDragStarts``). Process-wide; not reset between tests.
    static int startDragInvocationCountForTesting();

    /// GitHub drop-indicator fix test seam: set the thumbnail list's drop
    /// indicator to @p insertIndex (0..pageCount, where pageCount == append) and
    /// return the rect the paint code would draw for it. The marker is now a
    /// HORIZONTAL line in the gap between thumbnails (width >> height), replacing
    /// the old full-height vertical bar that did not show whether the dragged
    /// page would land above or below the existing page. Forces a deterministic
    /// layout so the geometry is valid in a headless (never-shown) panel. Returns
    /// a null QRect if the list/geometry is unavailable.
    QRect dropIndicatorRectForTesting(int insertIndex);

    /// GitHub #339 test seam: run the REAL context-menu action dispatch for the
    /// member QAction with object name @p actionObjectName (e.g.
    /// "deleteSelectedAction"), on the current selection, WITHOUT entering the
    /// modal ``QMenu::exec()`` that UIA cannot drive. This exercises the exact
    /// ``selected == deleteSelectedAction_ -> emit pagesDeleteRequested(indices)``
    /// mapping that a real right-click > Delete runs — the link the direct
    /// DeletePageRangeCommand tests bypass entirely — so a regression that
    /// unwired the Delete item from its signal is caught. Returns true iff a
    /// matching action was found, the selection was non-empty, and the
    /// corresponding signal was emitted. The menu POPUP geometry / exec() itself
    /// remains outside unit-test reach (documented modal limit).
    Q_INVOKABLE bool triggerContextMenuActionForTest(const QString& actionObjectName);

    /// R3-15 drag-out: build the QMimeData payload a page drag produces — the
    /// page thumbnail as image data (QMimeData::setImageData, surfaced as
    /// CF_DIB/CF_BITMAP on Windows) plus a text label. This is the SAME payload
    /// a real QDrag initiated from the page panel carries (see PageListWidget::
    /// mimeData). Ownership transfers to the caller; returns nullptr for an
    /// out-of-range index. Lets a headless test assert the drag-out contract
    /// without driving a flaky synthetic mouse-drag gesture.
    QMimeData* buildPageDragMimeData(int pageIndex) const;

    // ─── GitHub #325: cross-document page drag ──────────────────────────────
    //
    // A page dragged out of this panel carries, ALONGSIDE the existing
    // thumbnail image + text label, a custom MIME payload identifying the
    // SOURCE document and the dragged page index. A different PDFE window can
    // recognise that payload (MainWindow::dropEvent) and copy the page into its
    // own document. The image/text stay so an external app still sees a bitmap.
    //
    // The MIME body is UTF-8 "<source-doc-id>\n<pageIndex>". The id is a
    // process-local token from the registry below, NOT a raw pointer the drop
    // side would blindly dereference: the registry resolves it to a live
    // Document* only while the source panel still holds that document, so a
    // source window that closed mid-drag resolves to nullptr and the drop is
    // safely ignored.

    /// The custom drag MIME type. Stable string so both the source
    /// (PageListWidget::mimeData) and the target (MainWindow) agree.
    static const QString& pageMimeType();

    /// Register @p doc as a drag source and return its stable token. Idempotent
    /// — the same live document always maps to the same token. Returns an empty
    /// string for a null document.
    static QString registerDragSource(pdfe::core::Document* doc);

    /// Resolve a token produced by registerDragSource() back to the Document,
    /// or nullptr if that document is no longer a registered drag source (e.g.
    /// its window closed). NEVER returns a dangling pointer.
    static pdfe::core::Document* resolveDragSource(const QString& token);

    /// Drop @p doc from the drag-source registry. Called when the panel stops
    /// holding the document (setDocument to another doc, clearDocument, or
    /// destruction). Unknown documents are ignored.
    static void unregisterDragSource(pdfe::core::Document* doc);

    /// Parse a @ref pageMimeType payload into (token, pageIndex). Returns false
    /// if @p payload is malformed.
    static bool parsePageMime(const QByteArray& payload, QString& tokenOut,
                              int& pageIndexOut);

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

    /// Thumbnail-driven zoom-hang fix (phase 2): the panel only rasterizes
    /// thumbnails for pages near the visible range plus a small prefetch margin,
    /// rather than eagerly requesting all N pages at document open. These two
    /// knobs are exposed so a headless GoogleTest can reason about the bound on
    /// ThumbnailRenderer::renderCount() without a live interactive session.
    static int prefetchMargin();      ///< extra rows requested on each side
    static int fallbackWindow();      ///< rows requested when the viewport has
                                      ///< no usable geometry yet (e.g. not shown)

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

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
    /// Request thumbnails for the currently-visible page range (+ margin) and
    /// cancel still-pending renders for pages that have scrolled out of it.
    /// Connected to the vertical scrollbar's valueChanged/rangeChanged and
    /// invoked from resize/show and generateThumbnails().
    void updateVisibleThumbnails();

private:
    /// GitHub #339: the post-selection dispatch of a chosen context-menu action
    /// to its signal. Extracted from onContextMenuRequested so both the real
    /// modal path and triggerContextMenuActionForTest() run the identical
    /// action->signal mapping. Reads the current selection via
    /// selectedPageIndices(); a no-op if the selection is empty or @p selected
    /// is null/unrecognised. Returns true iff a signal was emitted.
    bool dispatchContextMenuAction(QAction* selected);

    void generateThumbnails();
    /// First and last model rows whose cell intersects the list viewport, or
    /// {-1,-1} if the panel has no rows / no usable geometry yet.
    std::pair<int, int> visibleRowRange() const;

    pdfe::core::Document* document_ = nullptr;
    QListWidget* listWidget_ = nullptr;
    pdfe::rendering::ThumbnailRenderer* thumbnailRenderer_ = nullptr;

    // Current requested thumbnail window [requestedLo_, requestedHi_], or
    // {-1,-1} when nothing is requested. Used to cancel pages that leave the
    // window on scroll without disturbing pages that remain.
    int requestedLo_ = -1;
    int requestedHi_ = -1;

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
