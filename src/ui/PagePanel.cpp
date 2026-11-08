#include "PagePanel.h"
#include "../core/Document.h"
#include "../core/PageInfo.h"
#include "../rendering/ThumbnailRenderer.h"

#include <QListWidget>
#include <QVBoxLayout>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPen>
#include <QSignalBlocker>
#include <QMenu>
#include <QAction>
#include <QItemSelectionModel>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDrag>
#include <QPaintEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QResizeEvent>
#include <QShowEvent>
#include <QRect>
#include <QByteArray>
#include <QHash>
#include <algorithm>
#include <atomic>
#include <functional>
#include <utility>

namespace {
// Thumbnail-driven zoom-hang fix (phase 2): bound how many thumbnails the panel
// rasterizes at once. Opening a large PDF used to request a full-page render for
// EVERY page up front, flooding the renderer; now only pages near the visible
// range (plus this margin) are requested, extended on scroll. kFallbackWindow
// is used when the viewport has no usable geometry yet (e.g. the panel is not
// shown, as in a headless test) so small documents still fully populate.
constexpr int kPrefetchMargin = 6;
constexpr int kFallbackWindow = 20;

// GitHub #335 residual observability. Incremented every time
// PageListWidget::startDrag() is ENTERED (real gesture or deterministic seam).
// A live-gesture UI test reads it (via PagePanel::startDragInvocationCountForTesting,
// surfaced on MainWindow's view-state pipe as pageDragStarts) to tell a gesture
// that reached startDrag from one that died upstream at Qt's drag-init threshold.
std::atomic<int> g_pageStartDragCount{0};
} // namespace

namespace pdfe::ui {

// R3-15 drag-out: attach the page's thumbnail (as image data) plus a text
// label to `md` for `item`. Shared by the real-drag mimeData() override and
// the PagePanel::buildPageDragMimeData() test seam so both produce an
// identical payload. On Windows QMimeData image data is surfaced to the
// clipboard/drag as CF_DIB/CF_BITMAP, so an external drop target (another app)
// receives the page as a pasteable bitmap.
//
// GitHub #325: when @p dragSourceId is non-empty it also attaches the custom
// cross-document MIME payload identifying the source document + page, so a
// different PDFE window can copy the page in. The image/text are kept so a
// genuinely external app still sees a bitmap.
static void attachPageThumbToMime(QMimeData* md, const QListWidgetItem* item,
                                  int row, const QString& dragSourceId) {
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

    if (!dragSourceId.isEmpty()) {
        const QString body = dragSourceId + QChar('\n') + QString::number(row);
        md->setData(pdfe::ui::PagePanel::pageMimeType(), body.toUtf8());
    }
}

// Custom list widget that handles drag-drop events for page reordering
// (internal move) AND enriches the drag payload with the page thumbnail so a
// page can be dragged OUT to another application as a bitmap (R3-15).
class PageListWidget : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void(int, int)> onPageDropped;
    // GitHub #325: returns the stable drag-source token for the panel's current
    // document (empty when there is none). Set by PagePanel so mimeData() can
    // tag the drag with the source-document identity.
    std::function<QString()> dragSourceId;

    // ─── GitHub #333: within-document thumbnail reorder ──────────────────────
    //
    // Compute the insert-BEFORE index (0..count) for a drop at viewport
    // position @p pos. This is the position the dragged page should land in
    // front of; `count` means "append after the last page". IconMode flows
    // items left-to-right in a wrapping grid, so the horizontal midpoint of the
    // hovered cell decides whether the drop lands before or after it.
    int dropInsertIndex(const QPointF& pos) const {
        const int n = count();
        if (n == 0) return 0;
        const QPoint p = pos.toPoint();
        const QModelIndex idx = indexAt(p);
        if (!idx.isValid()) {
            return n;  // dropped in empty space past the items -> append
        }
        const int row = idx.row();
        const QRect r = visualRect(idx);
        if (r.isNull()) return n;
        return (p.x() < r.center().x()) ? row : row + 1;
    }

    // Perform the within-document reorder a same-panel drop requests: the page
    // dragged from row @p fromRow is dropped at insert-before index
    // @p insertIndex. Returns the `to` handed to onPageDropped, or -1 when the
    // drop does not change the order (so no empty "Move Page" undo is pushed).
    //
    // `to` uses MovePageCommand's insert-before convention directly:
    // buildMovePermutation() (src/core/PageCommands.cpp) removes `from` then
    // re-inserts it at (from < to ? to-1 : to), so passing the raw insert-before
    // index gives the visually-expected landing spot. A drop back into the
    // page's own slot is insert-before `fromRow` or `fromRow+1`; both are
    // no-ops and are skipped here.
    int performInternalReorder(int fromRow, int insertIndex) {
        const int n = count();
        if (fromRow < 0 || fromRow >= n) return -1;
        if (insertIndex < 0) insertIndex = 0;
        if (insertIndex > n) insertIndex = n;
        const int to = insertIndex;
        if (to == fromRow || to == fromRow + 1) return -1;
        if (onPageDropped) onPageDropped(fromRow, to);
        return to;
    }

    // GitHub #333 test seam (invoked via PagePanel::deliverSyntheticInternalDrag):
    // run the REAL dragEnter + drop overrides with a synthetic event carrying
    // @p mime at @p pos. Qt does not route a manually-sent drag event to a
    // view's protected handlers, so a test cannot deliver one through the event
    // loop; this calls those handlers directly — the same methods, with the same
    // event objects, Qt invokes for a live drag. Returns true iff BOTH handlers
    // accepted (recognised the drag as internal and consumed it, so it would not
    // bubble to MainWindow's same-document no-op).
    bool deliverSyntheticInternalDrag(QMimeData* mime, const QPointF& pos) {
        QDragEnterEvent enter(pos.toPoint(), Qt::MoveAction, mime,
                              Qt::LeftButton, Qt::NoModifier);
        dragEnterEvent(&enter);
        QDropEvent drop(pos, Qt::MoveAction, mime,
                        Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
        dropEvent(&drop);
        return enter.isAccepted() && drop.isAccepted();
    }

protected:
    // GitHub #333: distinguish a SAME-document (internal) reorder drag from an
    // external one (a cross-document drag out of ANOTHER PDFE window — #325 — or
    // a file-URL drop from the shell). We key off the drag's own MIME rather
    // than QDropEvent::source(): source() is only populated by a live QDrag loop
    // (it cannot be set on a synthetic test event, and we want this decision
    // headlessly testable), whereas the #325 payload already carries the
    // originating document's stable token. A drag started in THIS panel carries
    // THIS document's token (== dragSourceId()); a cross-window drag carries a
    // different document's token; a file drop carries no such payload. So
    // "token == our own token" is exactly a same-document drag, and the dragged
    // row travels in the same payload. On a match the handlers accept the event
    // (stopping it bubbling to MainWindow::dropEvent, whose same-document branch
    // deliberately no-ops it); otherwise they defer to the base class so the
    // event propagates to MainWindow and the #325 cross-document path runs.
    bool isInternalDrag(const QDropEvent* event, int* fromRowOut = nullptr) const {
        if (!event || !event->mimeData()) return false;
        const QByteArray payload =
            event->mimeData()->data(PagePanel::pageMimeType());
        if (payload.isEmpty()) return false;
        QString token;
        int row = -1;
        if (!PagePanel::parsePageMime(payload, token, row)) return false;
        const QString own = dragSourceId ? dragSourceId() : QString();
        if (own.isEmpty() || token != own) return false;
        if (fromRowOut) *fromRowOut = row;
        return true;
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (isInternalDrag(event)) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
            setDropIndicator(dropInsertIndex(event->position()));
            return;
        }
        // External drag (cross-document #325 or a file-URL drop): let the base
        // handler ignore it so it bubbles to MainWindow.
        QListWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (isInternalDrag(event)) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
            setDropIndicator(dropInsertIndex(event->position()));
            return;
        }
        QListWidget::dragMoveEvent(event);
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        setDropIndicator(-1);
        QListWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        int fromRow = -1;
        if (isInternalDrag(event, &fromRow)) {
            performInternalReorder(fromRow, dropInsertIndex(event->position()));
            setDropIndicator(-1);
            event->setDropAction(Qt::MoveAction);
            event->accept();  // consume: must NOT bubble to the same-doc no-op
            return;
        }
        // External drop: defer to the base handler (unaccepted in InternalMove
        // mode for a foreign source) so MainWindow::dropEvent gets it (#325).
        QListWidget::dropEvent(event);
    }

    // GitHub #333: paint the drop-position indicator. QAbstractItemView routes
    // the viewport's paint through this override, so drawing with a painter on
    // viewport() lands on top of the thumbnails.
    void paintEvent(QPaintEvent* event) override {
        QListWidget::paintEvent(event);
        const QRect bar = dropIndicatorRect();
        if (bar.isNull()) return;
        // A crisp filled horizontal line (the insertion marker). fillRect gives a
        // solid bar with no surrounding pen border, so the 3px line stays 3px.
        QPainter p(viewport());
        p.fillRect(bar, palette().highlight().color());
    }

    // R3-15: Qt calls this to build the drag payload. We keep the base
    // internal-move serialization (so drag-drop reorder still works) and add
    // the page thumbnail image + a text label, enabling drag-OUT of a page.
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        QMimeData* md = QListWidget::mimeData(items);
        if (!md) md = new QMimeData();
        if (!items.isEmpty() && items.first()) {
            const QString srcId = dragSourceId ? dragSourceId() : QString();
            attachPageThumbToMime(md, items.first(), row(items.first()), srcId);
        }
        return md;
    }

    // ─── GitHub #335: own the drag so the clearOrRemove() tail never runs ────
    //
    // #333 added dropEvent (above) but left Qt's default icon-mode drag path in
    // force. On a REAL gesture QAbstractItemView::mouseMoveEvent calls startDrag,
    // which (unoverridden) reaches QListView::startDrag ->
    // QIconModeViewBase::filterStartDrag (qlistview.cpp:951/2889). That runs
    // drag->exec() and then, because our dropEvent accepts the internal drop
    // WITHOUT delegating to QAbstractItemView::dropEvent, Qt's dd->dropEventMoved
    // stays false — so on the MoveAction return it executes dd->clearOrRemove()
    // (qlistview.cpp:2915-2917 → qabstractitemview.cpp:4557), a model->removeRows
    // on the dragged source row. That silent deletion, plus refreshPages()
    // rebuilding the list re-entrantly from inside exec(), is why a real reorder
    // "does nothing" / desyncs (GitHub #335).
    //
    // Overriding startDrag to build the QDrag + exec() ourselves and return
    // WITHOUT the base call removes that tail entirely: filterStartDrag never
    // runs, so clearOrRemove() never runs, and dropEvent stays the sole mutator.
    // We reuse mimeData() (the #325 payload) so a real drag still carries this
    // document's token and both the internal-reorder and cross-document (#325)
    // drop targets keep recognising it.
    void startDrag(Qt::DropActions supportedActions) override {
        // GitHub #335 residual: mark that Qt actually ENTERED startDrag. A live
        // synthetic drag that never increments this died upstream, at the
        // drag-initiation threshold, not in this override.
        g_pageStartDragCount.fetch_add(1, std::memory_order_acq_rel);
        const QList<QListWidgetItem*> items = selectedItems();
        if (items.isEmpty() || !items.first()) return;
        QMimeData* data = mimeData(items);
        if (!data) return;

        QDrag* drag = new QDrag(this);
        drag->setMimeData(data);  // QDrag takes ownership
        const QModelIndex idx = indexFromItem(items.first());
        if (idx.isValid()) {
            const QRect r = visualRect(idx);
            if (!r.isNull()) {
                drag->setPixmap(items.first()->icon().pixmap(r.size()));
            }
        }

        const Qt::DropAction wanted =
            (supportedActions & Qt::MoveAction) ? Qt::MoveAction : defaultDropAction();

        inInternalDrag_ = true;
        if (execInternalDragForTest) {
            // A headless test cannot enter the real, window-system-driven modal
            // QDrag::exec() loop (it blocks and is exactly the exec() loop UI
            // tests must not enter). This hook lets a test drive THIS override
            // deterministically: it receives the live QDrag (with the real
            // mimeData built above) and returns the action exec() would.
            execInternalDragForTest(drag);
        } else {
            drag->exec(wanted, Qt::MoveAction);
        }
        inInternalDrag_ = false;
        setDropIndicator(-1);
        // Deliberately NO fall-through to QListView::startDrag — see above.
    }

public:
    // True while this list's own startDrag()/exec() is on the stack. PagePanel
    // reads it (via isInternalDragActive()) to defer the list rebuild out of the
    // drag loop; see PagePanel::refreshPages().
    bool isInternalDragActive() const { return inInternalDrag_; }

    // GitHub #335 test seam: drive the REAL startDrag override deterministically
    // (no blocking OS drag loop). Caller must have selected the source row first;
    // @p dropPos is the viewport drop position (far past the items -> append).
    // Returns true iff the drag was recognised + consumed as internal.
    bool startInternalDragForTest(const QPointF& dropPos) {
        bool consumed = false;
        execInternalDragForTest = [this, dropPos, &consumed](QDrag* drag) -> Qt::DropAction {
            // Deliver the drop the way a live exec() loop would — through the
            // SAME dragEnter/drop overrides, carrying the SAME mimeData this
            // override just built.
            consumed = deliverSyntheticInternalDrag(drag->mimeData(), dropPos);
            return Qt::MoveAction;  // the exact return that triggers clearOrRemove
                                    // in the base path this override replaces
        };
        startDrag(Qt::MoveAction);
        execInternalDragForTest = nullptr;
        return consumed;
    }

    // Test-only exec() replacement; see startDrag() / startInternalDragForTest().
    std::function<Qt::DropAction(QDrag*)> execInternalDragForTest;

private:
    // True while startDrag()/exec() is on the stack (see isInternalDragActive()).
    bool inInternalDrag_ = false;

    // Insert-before index currently shown as a drop indicator, or -1 for none.
    int dropIndicatorIndex_ = -1;

    void setDropIndicator(int index) {
        if (index != dropIndicatorIndex_) {
            dropIndicatorIndex_ = index;
            viewport()->update();
        }
    }

    // A thin HORIZONTAL line in the gap BETWEEN thumbnails, at the insertion
    // boundary: centred in the gap ABOVE the item we would insert before, or in
    // the gap BELOW the last item when appending. This shows the exact insert
    // POSITION (the dragged page lands in that gap, pushing the page below it
    // down) instead of the old full-height vertical bar, which highlighted a
    // whole item edge and left it ambiguous whether the page would land above or
    // below the existing one. Full item width, a few px tall. Null when no
    // indicator is shown or geometry is unavailable.
    QRect dropIndicatorRect() const {
        const int idx = dropIndicatorIndex_;
        const int n = count();
        if (idx < 0 || n == 0) return QRect();
        const int thickness = 3;
        const int halfGap = qMax(spacing(), 2) / 2;
        if (idx < n) {
            const QRect r = visualItemRect(item(idx));
            if (r.isNull()) return QRect();
            return QRect(r.left(), r.top() - halfGap - thickness / 2, r.width(), thickness);
        }
        const QRect r = visualItemRect(item(n - 1));
        if (r.isNull()) return QRect();
        return QRect(r.left(), r.bottom() + halfGap - thickness / 2, r.width(), thickness);
    }

public:
    // GitHub drop-indicator fix test seam: set the drop indicator to @p insertIndex
    // and return the rect it would paint, so a headless GoogleTest can assert the
    // indicator geometry (now HORIZONTAL) without driving a live drag. Forces a
    // deterministic single-column layout first so visualItemRect() is valid even
    // when the panel was never shown.
    QRect dropIndicatorRectForTesting(int insertIndex) {
        if (width() < 10 || height() < 10) resize(180, 700);
        doItemsLayout();
        setDropIndicator(insertIndex);
        return dropIndicatorRect();
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
    // GitHub #326/#333/#335: keep movement Static so dropped icons do not
    // free-float in the IconMode grid (and because the #326 configuration guard
    // in test_page_panel_q_invokable.cpp pins it). Static is NOT what routes a
    // real drag to our handlers, though — the earlier comment claiming the drop
    // "never reaches" the handlers without Static was wrong. PageListWidget now
    // owns the whole gesture: startDrag() builds + execs the QDrag itself and
    // dragEnter/dragMove/drop recognise the internal drag by its #325 token
    // (above). Movement mode no longer governs delivery, and crucially
    // startDrag() returns WITHOUT the base QListView path so Qt's
    // QIconModeViewBase::filterStartDrag → clearOrRemove() model deletion never
    // runs (GitHub #335 root cause). An EXTERNAL drag is still left to bubble to
    // MainWindow for the #325 cross-document path.
    listWidget_->setMovement(QListWidget::Static);
    listWidget_->setUniformItemSizes(true);

    // Enable drag-drop for page reordering. setDragDropMode(InternalMove) already
    // flips setDragEnabled(true) + viewport()->setAcceptDrops(true), but we
    // re-assert both explicitly AFTER every mode/movement call so the gesture is
    // not silently disabled if these lines are ever reordered (the order-
    // dependency the old #326 note worried about).
    listWidget_->setDragDropMode(QAbstractItemView::InternalMove);
    listWidget_->setDefaultDropAction(Qt::MoveAction);
    listWidget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    listWidget_->setDragEnabled(true);
    listWidget_->viewport()->setAcceptDrops(true);

    // Connect drag-drop callback
    pageList->onPageDropped = [this](int from, int to) {
        emit pageMoved(from, to);
    };

    // GitHub #325: tag outgoing drags with the current document's identity so
    // another PDFE window can copy the page. Registration is lazy (only when a
    // drag actually asks for the id) and idempotent.
    pageList->dragSourceId = [this]() -> QString {
        return document_ ? PagePanel::registerDragSource(document_) : QString();
    };

    // Connect item click
    connect(listWidget_, &QListWidget::itemClicked,
            this, &PagePanel::onItemClicked);

    // Phase 2 (thumbnail-driven zoom-hang fix): re-evaluate which thumbnails to
    // rasterize whenever the panel scrolls or its content/viewport geometry
    // changes. valueChanged covers user scrolling; rangeChanged covers the
    // viewport/content resizing (which shifts how many rows are visible). The
    // scrollbar is a distinct QObject from listWidget_, so the QSignalBlocker in
    // setCurrentPage() does not suppress these.
    connect(listWidget_->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &PagePanel::updateVisibleThumbnails);
    connect(listWidget_->verticalScrollBar(), &QScrollBar::rangeChanged,
            this, [this](int, int) { updateVisibleThumbnails(); });

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
    // GitHub #325: stop vouching for this document as a drag source — a drop
    // that resolves its token after we are gone must get nullptr, not a
    // dangling pointer.
    if (document_) {
        PagePanel::unregisterDragSource(document_);
    }
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

    // GitHub #325: the old document is no longer reachable through this panel,
    // so it can no longer be a drag source from here.
    if (document_ && document_ != doc) {
        PagePanel::unregisterDragSource(document_);
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

    if (document_) {
        PagePanel::unregisterDragSource(document_);
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
    // Produce the SAME payload a real QDrag carries, including the #325
    // cross-document identity, so a headless test can assert the drag-out
    // contract without a synthetic mouse-drag.
    const QString srcId =
        document_ ? PagePanel::registerDragSource(document_) : QString();
    attachPageThumbToMime(md, item, pageIndex, srcId);
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

int PagePanel::startDragInvocationCountForTesting()
{
    // GitHub #335 residual observability — see the header. Process-wide count of
    // PageListWidget::startDrag() entries.
    return g_pageStartDragCount.load(std::memory_order_acquire);
}

QRect PagePanel::dropIndicatorRectForTesting(int insertIndex)
{
    // GitHub drop-indicator fix: forward to the thumbnail list so a headless test
    // can assert the (now horizontal) insertion-marker geometry. See the header.
    if (!listWidget_) return QRect();
    return static_cast<PageListWidget*>(listWidget_)->dropIndicatorRectForTesting(insertIndex);
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
    }

    // IMPORTANT: every ROW is still created synchronously above, so the
    // panelItemCount() == Document::pageCount() readiness contract (GitHub #136)
    // is unchanged. What is NO LONGER eager is the expensive IMAGE request: we
    // used to call requestThumbnail() for all N pages here, which flooded the
    // renderer and (pre-phase-1) the UI thread. Now we request only the pages
    // near the visible range; the rest are requested lazily on scroll.
    requestedLo_ = -1;
    requestedHi_ = -1;
    updateVisibleThumbnails();
}

std::pair<int, int> PagePanel::visibleRowRange() const
{
    const int n = listWidget_ ? listWidget_->count() : 0;
    if (n == 0) {
        return {-1, -1};
    }

    const QRect vp = listWidget_->viewport()->rect();
    if (vp.width() <= 0 || vp.height() <= 0) {
        return {-1, -1};  // no usable geometry yet (e.g. panel not shown)
    }

    // Uniform item sizes (set in the constructor) make visualItemRect() cheap
    // arithmetic. Scan for the first and last rows whose cell intersects the
    // viewport. IconMode flows items in a wrapping grid, so we take the min/max
    // intersecting model row rather than assuming a single top-to-bottom column.
    int first = -1;
    int last = -1;
    for (int i = 0; i < n; ++i) {
        QListWidgetItem* item = listWidget_->item(i);
        if (!item) continue;
        const QRect r = listWidget_->visualItemRect(item);
        if (r.isNull()) continue;
        if (r.bottom() < vp.top() || r.top() > vp.bottom()) {
            continue;  // entirely above or below the viewport
        }
        if (first < 0) first = i;
        last = i;
    }
    return {first, last};
}

void PagePanel::updateVisibleThumbnails()
{
    if (!document_ || !thumbnailRenderer_ || !listWidget_) {
        return;
    }
    const int n = listWidget_->count();
    if (n == 0) {
        return;
    }

    auto [first, last] = visibleRowRange();
    if (first < 0) {
        // Viewport geometry not available yet: request a bounded leading window
        // so the panel is not blank and small documents fully populate.
        first = 0;
        last = std::min(n - 1, kFallbackWindow - 1);
    }

    const int lo = std::max(0, first - kPrefetchMargin);
    const int hi = std::min(n - 1, last + kPrefetchMargin);

    // Cancel still-pending renders for pages that have left the window. A page
    // already rendered+delivered has no pending entry, so cancelPage() is a
    // no-op for it (its thumbnail stays); only not-yet-run work is dropped.
    if (requestedLo_ >= 0) {
        for (int p = requestedLo_; p <= requestedHi_; ++p) {
            if (p < lo || p > hi) {
                thumbnailRenderer_->cancelPage(p);
            }
        }
    }

    // Request the current window. requestThumbnail() dedups in-flight requests
    // and serves completed ones from the LRU cache, so re-covering a page that
    // is already pending or cached does not re-rasterize.
    const QSize thumbnailSize(128, 128);
    for (int p = lo; p <= hi; ++p) {
        thumbnailRenderer_->requestThumbnail(p, thumbnailSize, nullptr);
    }

    requestedLo_ = lo;
    requestedHi_ = hi;
}

int PagePanel::prefetchMargin()
{
    return kPrefetchMargin;
}

int PagePanel::fallbackWindow()
{
    return kFallbackWindow;
}

void PagePanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateVisibleThumbnails();
}

void PagePanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateVisibleThumbnails();
}

void PagePanel::refreshPages()
{
    if (!document_ || !thumbnailRenderer_) {
        return;
    }

    // GitHub #335: a within-document drag reorder reaches here synchronously
    // from INSIDE the QDrag::exec() nested loop:
    //   dropEvent → onPageDropped → pageMoved → MainWindow::onPageMoved →
    //   onDocumentStructureChanged → refreshPages().
    // Rebuilding the list (clear() + repopulate) while exec() is still on the
    // stack tears down the very items the drag machinery is operating over.
    // Defer the rebuild to the next event-loop turn so it runs only after exec()
    // has unwound. Non-drag callers (delete/rotate/insert) see no change — the
    // flag is false and the rebuild runs synchronously as before.
    if (listWidget_
        && static_cast<PageListWidget*>(listWidget_)->isInternalDragActive()) {
        QMetaObject::invokeMethod(this, [this]() { refreshPages(); },
                                  Qt::QueuedConnection);
        return;
    }

    // Save current selection
    QList<int> selectedIndices = selectedPageIndices();

    // Cancel pending thumbnails
    thumbnailRenderer_->cancelAll();

    // GitHub #336: drop the whole thumbnail IMAGE cache before rebuilding. The
    // cache is keyed by CacheKey{pageIndex,w,h}; cancelAll() clears only pending
    // tasks, not cached bitmaps. A delete/insert/reorder renumbers every page at
    // or after the edit point, so the item that lands at a shifted index would
    // otherwise be served the STALE bitmap cached under that index (navigation
    // was correct via Qt::UserRole, only the picture was stale). A full clear is
    // correct here — a renumber invalidates every index from the edit point on,
    // and refreshPages() already re-requests the visible window below. This also
    // fixes the latent rotate staleness.
    thumbnailRenderer_->invalidate();

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

void PagePanel::invalidateThumbnail(int pageIndex)
{
    if (!thumbnailRenderer_ || pageIndex < 0) {
        return;
    }
    // Clear the LRU cache entry so the next request re-rasterizes from the
    // edited page, then re-request the visible window (requestThumbnail() dedups
    // and the just-cleared page is now a cache miss, so it actually re-renders).
    thumbnailRenderer_->invalidatePage(pageIndex);
    updateVisibleThumbnails();
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

    // Execute menu and handle actions. The action->signal mapping lives in
    // dispatchContextMenuAction so the real modal path and the GitHub #339 test
    // seam run identical code.
    QAction* selected = menu.exec(listWidget_->mapToGlobal(pos));
    dispatchContextMenuAction(selected);
}

bool PagePanel::dispatchContextMenuAction(QAction* selected)
{
    if (!selected) {
        return false;
    }

    QList<int> indices = selectedPageIndices();
    if (indices.isEmpty()) {
        return false;
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
    } else {
        return false;
    }
    return true;
}

bool PagePanel::triggerContextMenuActionForTest(const QString& actionObjectName)
{
    // GitHub #339: drive the real action->signal dispatch without the modal
    // menu.exec(). Resolve the member QAction by objectName so the test targets
    // the SAME object the context menu adds (not a freshly-made stand-in).
    QAction* target = nullptr;
    const QList<QAction*> actions = {
        rotateCWAction_, rotateCCWAction_, rotate180Action_,
        insertBlankPageAction_, deleteSelectedAction_, extractSelectedAction_
    };
    for (QAction* a : actions) {
        if (a && a->objectName() == actionObjectName) {
            target = a;
            break;
        }
    }
    if (!target) {
        return false;
    }
    return dispatchContextMenuAction(target);
}

// ─── GitHub #325: cross-document drag-source registry ────────────────────
//
// Process-local, main-thread-only (drag and drop both run on the GUI thread).
// Maps a stable string token <-> live Document*. The token embeds a
// monotonic counter so a Document* whose address is later reused by a
// different document cannot be mistaken for the original.

namespace {
QHash<QString, pdfe::core::Document*>& tokenToDoc() {
    static QHash<QString, pdfe::core::Document*> m;
    return m;
}
QHash<pdfe::core::Document*, QString>& docToToken() {
    static QHash<pdfe::core::Document*, QString> m;
    return m;
}
quint64 nextDragSourceSerial() {
    static quint64 serial = 0;
    return ++serial;
}
} // namespace

const QString& PagePanel::pageMimeType() {
    static const QString kType = QStringLiteral("application/x-pdfe-page");
    return kType;
}

QString PagePanel::registerDragSource(pdfe::core::Document* doc) {
    if (!doc) return QString();
    auto it = docToToken().find(doc);
    if (it != docToToken().end()) {
        return it.value();
    }
    const QString token = QStringLiteral("%1-%2")
                              .arg(nextDragSourceSerial())
                              .arg(reinterpret_cast<quintptr>(doc), 0, 16);
    docToToken().insert(doc, token);
    tokenToDoc().insert(token, doc);
    return token;
}

pdfe::core::Document* PagePanel::resolveDragSource(const QString& token) {
    if (token.isEmpty()) return nullptr;
    return tokenToDoc().value(token, nullptr);
}

void PagePanel::unregisterDragSource(pdfe::core::Document* doc) {
    if (!doc) return;
    auto it = docToToken().find(doc);
    if (it == docToToken().end()) return;
    tokenToDoc().remove(it.value());
    docToToken().erase(it);
}

bool PagePanel::parsePageMime(const QByteArray& payload, QString& tokenOut,
                              int& pageIndexOut) {
    const QString s = QString::fromUtf8(payload);
    const int nl = s.indexOf(QChar('\n'));
    if (nl <= 0) return false;
    const QString token = s.left(nl);
    bool ok = false;
    const int page = s.mid(nl + 1).toInt(&ok);
    if (token.isEmpty() || !ok || page < 0) return false;
    tokenOut = token;
    pageIndexOut = page;
    return true;
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
    // Pipeline f7c2a4e8 phase-3 (B1); GitHub #338 correction. This routes
    // through the SAME pageMoved signal as movePageProgrammatic and does NOTHING
    // drag-specific: no startDrag(), no QDrag::exec(), no dropEvent(), no
    // InternalMove event path. The name records drag-style test INTENT only; the
    // model-side effect is identical to movePageProgrammatic. A matrix row that
    // asserts only the result of this call therefore pins the command/model half,
    // NOT that a real drag gesture reaches it (the #335 lesson) — such rows must
    // additionally assert the resulting page-order permutation, and the real
    // event path is covered by simulateInternalDrop / deliverRealDragReorder /
    // the live-gesture test_ui_page_reorder_drag.py. If a future divergence in
    // drag-vs-move semantics is needed, this is the entry-point to extend.
    return movePageProgrammatic(fromIndex, toIndex);
}

int PagePanel::simulateInternalDrop(int fromRow, int insertBeforeIndex)
{
    // GitHub #333 test seam: run the EXACT internal-drop dispatch that
    // PageListWidget::dropEvent invokes for a same-panel drag. Not a mock — it
    // calls the real performInternalReorder (insert-before index math + no-op
    // guard + onPageDropped → pageMoved). A synthetic QDropEvent cannot populate
    // source() nor carry reliable viewport geometry headlessly, so the drop
    // position is supplied pre-resolved as an insert-before index.
    if (!listWidget_) return -1;
    return static_cast<PageListWidget*>(listWidget_)
        ->performInternalReorder(fromRow, insertBeforeIndex);
}

int PagePanel::internalDropInsertIndexAt(const QPoint& viewportPos) const
{
    // GitHub #333 test seam: the geometry mapping PageListWidget uses to turn a
    // drop position into an insert-before index. Exposed so a test with real
    // layout geometry can assert the before/after-midpoint decision.
    if (!listWidget_) return -1;
    return static_cast<PageListWidget*>(listWidget_)
        ->dropInsertIndex(QPointF(viewportPos));
}

bool PagePanel::deliverSyntheticInternalDrag(int fromRow, const QPoint& viewportPos)
{
    // GitHub #333 test seam: exercise the REAL dragEnter/drop overrides (incl.
    // the isInternalDrag token check and the accept-so-it-does-not-bubble
    // behaviour) with the exact #325 drag payload this panel builds for a
    // same-panel drag. Qt cannot route a synthetic QDropEvent to a view's
    // protected handlers, so the forwarding call invokes them directly. Returns
    // true iff the drag was recognised as internal and consumed; pageMoved(from,
    // to) fires as a side effect for an order-changing drop.
    if (!listWidget_) return false;
    QMimeData* mime = buildPageDragMimeData(fromRow);
    if (!mime) return false;
    const bool consumed = static_cast<PageListWidget*>(listWidget_)
        ->deliverSyntheticInternalDrag(mime, QPointF(viewportPos));
    delete mime;
    return consumed;
}

bool PagePanel::deliverRealDragReorder(int fromRow, const QPoint& viewportPos)
{
    // GitHub #335 test seam: unlike deliverSyntheticInternalDrag (#333), which
    // calls the protected dropEvent directly and so proves the handler but NEVER
    // that a real drag reaches it, this goes through the REAL startDrag()
    // override — the initiation path #333's seam skipped and #335 broke in. It
    // selects the source row (startDrag reads selectedItems()) and drives the
    // override; the override's test hook delivers the drop so no blocking,
    // window-system-dependent QDrag::exec() loop is entered.
    if (!listWidget_) return false;
    if (fromRow < 0 || fromRow >= listWidget_->count()) return false;
    if (!selectPagesProgrammatic({fromRow})) return false;
    return static_cast<PageListWidget*>(listWidget_)
        ->startInternalDragForTest(QPointF(viewportPos));
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
