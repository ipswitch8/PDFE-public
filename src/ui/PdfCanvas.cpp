#include "PdfCanvas.h"
#include <QFile>
#include <QTextStream>
#include "../core/Document.h"
#include "../core/Page.h"
#include "../core/PageInfo.h"
#include "../core/CoordinateTransform.h"
#include "../rendering/TileCache.h"
#include "../rendering/TileRenderer.h"
#include "../rendering/Tile.h"
#include "../annotations/AnnotationTool.h"
#include "../forms/FormTool.h"
#include "../textedit/TextEditTool.h"
#include "../image/ImageTool.h"
#include "../image/ImageManager.h"
#include "../core/MuPdfContext.h"
#include "../core/JpxSecurityGate.h"
#include "../core/Jbig2SecurityGate.h"
#include "AccessibilityHelper.h"

#include <QMetaObject>
#include <QPointer>
#include <QApplication>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTransform>
#include <QWheelEvent>
#include "annotations/SelectTool.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QResizeEvent>
#include <QShowEvent>
#include <QScreen>
#include <QContextMenuEvent>
#include <QWindow>
#include <QToolTip>
#include <QHelpEvent>
#include <algorithm>
#include <cmath>

namespace pdfe::ui {

PdfCanvas::PdfCanvas(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("pdfCanvas");
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setBackgroundRole(QPalette::Dark);
    setAutoFillBackground(true);

    updateDevicePixelRatio();
}

PdfCanvas::~PdfCanvas()
{
    // ISSUE #125 -- and read the comment before "simplifying" this to a
    // matching unsubscribePageDirty() call.
    //
    // The first version DID unsubscribe here, and it crashed the suite:
    // MainWindowQInvokableTest tore down with an access violation, because
    // MainWindow destroys the Document while the canvas still holds a raw
    // document_ pointer, so ~PdfCanvas called removePageDirtyListener() on
    // freed memory. The ownership order is genuinely not guaranteed in either
    // direction, which is the same hazard the rest of this file exists to
    // document for Page*.
    //
    // So the listener guards ITSELF with a QPointer instead (see
    // subscribePageDirty). A destroyed canvas makes the QPointer null and the
    // notification becomes a no-op, which covers "canvas dies first" without
    // this destructor having to touch a Document that may already be gone.
    // "Document dies first" is then safe by construction: its listener list
    // dies with it and nothing here dereferences it.
    //
    // setDocument()/clearDocument() still unsubscribe explicitly -- those run
    // while the document is known alive, so they prune properly rather than
    // leaving a dead entry behind.
    pageDirtyToken_ = 0;
}

void PdfCanvas::subscribePageDirty()
{
    unsubscribePageDirty();
    if (!document_) return;
    documentAlive_ = document_->lifetimeSentinel();
    // QPointer, not a bare `this`: a Document can outlive this widget (see the
    // destructor), and then the only thing standing between an edit and a
    // use-after-free is this guard going null.
    QPointer<PdfCanvas> self(this);
    pageDirtyToken_ = document_->addPageDirtyListener([self](int pageIndex) {
        if (!self) return;
        // Qt::AutoConnection, deliberately, NOT a forced QueuedConnection.
        //
        // Auto runs this DIRECTLY when the notification already arrives on the
        // canvas's own thread -- which is every caller today, since every
        // markPageDirty() site (ImageCommands, PageOperations,
        // GrayscaleConverter, TextEditCommands, MainWindow) is synchronous on
        // the caller -- and QUEUES it when some future caller edits from a
        // worker thread. Forcing Queued would be strictly worse: it would leave
        // the cache stale until the next event-loop turn even on the thread
        // that just made the edit, so an edit followed by a synchronous repaint
        // would still paint the old tiles. The requirement is "do not assume
        // the calling thread", and Auto is the mechanism that does not.
        QMetaObject::invokeMethod(self, "onPageContentChanged",
                                  Qt::AutoConnection, Q_ARG(int, pageIndex));
    });
}

void PdfCanvas::unsubscribePageDirty()
{
    // documentAlive_ FIRST. removePageDirtyListener() tolerates a bad token but
    // not a bad `this`, and document_ may already be freed: MainWindow destroys
    // the Document before the canvas, and test fixtures call
    // setDocument(nullptr) in TearDown after the document local has gone. Both
    // were 0xC0000005 in TearDown before this check existed.
    if (pageDirtyToken_ != 0 && document_ && !documentAlive_.expired()) {
        document_->removePageDirtyListener(pageDirtyToken_);
    }
    pageDirtyToken_ = 0;
    documentAlive_.reset();
}

void PdfCanvas::onPageContentChanged(int pageIndex)
{
    // The whole point of issue #125: this is what makes markPageDirty() a real
    // signal instead of a false affordance. The MainWindow refreshPage() calls
    // are deliberately LEFT IN PLACE -- removing them in the same change would
    // make any regression ambiguous between "the new path failed" and "the old
    // one was still needed".
    refreshPage(pageIndex);

    // GitHub #234: a dirty page may mean its annotation SET changed, and a tool
    // caching annotation indices (SelectTool) cannot detect that on its own --
    // a delete shifts every higher index down by one. Tell it before the
    // refreshPage() above results in a repaint that re-queries those indices.
    //
    // This does NOT cover every mutation path: annotation edits do not all go
    // through markPageDirty(). That is why SelectTool::onPaintOverlay also
    // tolerates a stale index rather than relying on this notification. This
    // hook drops the stale selection promptly where we DO know; the paint-path
    // guard is what keeps the unnotified paths from being fatal.
    if (currentTool_) {
        currentTool_->onAnnotationsChanged();
    }
}

void PdfCanvas::setDocument(pdfe::core::Document* doc,
                            pdfe::rendering::TileCache* cache,
                            pdfe::rendering::TileRenderer* renderer)
{
    // Release any tool state cached against the OUTGOING document before we swap
    // it out. setDocument() is the save→reopen path (MainWindow::openFile →
    // setDocument): the previous Document (and all its Page objects) is about to
    // be replaced/freed, so any tool holding a raw Page* — SelectTool::page_,
    // bound via setPage()/selectByIndex() — would be left dangling. A repaint
    // queued after the swap then derefs the freed page in
    // SelectTool::onPaintOverlay → AnnotationManager::getAnnotation(page_) →
    // Page::fzPage → 0xC0000005. clearDocument() already releases here on close;
    // setDocument() (reopen) must do the same. Harmless on the initial open
    // (nulls an already-null page_ / no-op text-edit tool).
    if (document_ != doc) {
        clearTextEditTool();
        if (currentTool_) {
            currentTool_->onDocumentClosing();
        }
        // GitHub #269. Same ordering rule as clearDocument(): the tool releases
        // first, then the owner. The outgoing document is about to free these
        // pages.
        releaseToolPage();
    }

    // ISSUE #125: release the OLD document's subscription before repointing,
    // then subscribe to the new one. Order matters -- unsubscribing after the
    // repoint would leave the old Document holding a token this canvas can no
    // longer name.
    unsubscribePageDirty();

    document_ = doc;
    tileCache_ = cache;
    tileRenderer_ = renderer;

    subscribePageDirty();

    currentPage_ = 0;
    zoomLevel_ = 1.0f;
    scrollOffset_ = QPoint(0, 0);
    searchHighlights_.clear();
    viewRotation_ = 0;

    // Connect tile renderer to trigger repaints when tiles arrive
    if (tileRenderer_) {
        disconnect(tileRenderer_, &pdfe::rendering::TileRenderer::tileReady,
                   this, nullptr);
        connect(tileRenderer_, &pdfe::rendering::TileRenderer::tileReady,
                this, [this](pdfe::rendering::TileId, QImage) { update(); });
    }

    updateDocumentLayout();
    requestVisibleTiles();
    update();

    // Deferred tile request: widget may not be fully laid out yet (width=0),
    // so re-request tiles after the event loop processes layout
    QMetaObject::invokeMethod(this, [this]() {
        if (document_) {
            updateDocumentLayout();
            requestVisibleTiles();
            update();
        }
    }, Qt::QueuedConnection);

    emit pageChanged(currentPage_);
    emit zoomChanged(zoomLevel_);
}

void PdfCanvas::releaseActiveToolPageCache()
{
    // A full-rewrite Save (saveAsWithCommandPageRebind → onDocumentStructureChanged)
    // drops and recreates the document's Page objects while the document stays
    // open. Any tool caching a raw Page* — SelectTool::page_, bound via
    // selectByIndex when an annotation is selected — is left dangling, and the
    // next repaint derefs the freed page in onPaintOverlay → getAnnotation(page_)
    // → Page::fzPage (0xC0000005). Reuse the tool's existing "release cached page"
    // contract (onDocumentClosing nulls page_ and clears the selection); the
    // annotation itself is untouched, only the transient selection highlight is
    // dropped.
    if (currentTool_) {
        currentTool_->onDocumentClosing();
    }
}

void PdfCanvas::clearDocument()
{
    clearTextEditTool();
    // Tell the active annotation tool the document is going away BEFORE we drop
    // the document, so any tool caching a raw Page* (SelectTool::page_) releases
    // it. Otherwise a repaint queued after close derefs the freed page in
    // onPaintOverlay (paint-time use-after-free).
    if (currentTool_) {
        currentTool_->onDocumentClosing();
    }
    // GitHub #269. AFTER the tool has nulled its own page_ (onDocumentClosing
    // is non-virtual, #236, so that is guaranteed), never before.
    releaseToolPage();
    unsubscribePageDirty();  // ISSUE #125: before document_ is dropped.
    document_ = nullptr;
    tileCache_ = nullptr;
    tileRenderer_ = nullptr;
    pageLayouts_.clear();
    totalDocumentHeight_ = 0;
    currentPage_ = 0;
    searchHighlights_.clear();
    jpxRegions_.clear();
    jbig2Regions_.clear();
    viewRotation_ = 0;
    update();
}

void PdfCanvas::setZoom(float level)
{
    level = std::clamp(level, MIN_ZOOM, MAX_ZOOM);
    if (std::abs(zoomLevel_ - level) < 0.001f) {
        return;
    }

    zoomLevel_ = level;
    updateDocumentLayout();
    requestVisibleTiles();
    update();

    emit zoomChanged(zoomLevel_);
}

void PdfCanvas::zoomIn()
{
    setZoom(zoomLevel_ * ZOOM_STEP);
}

void PdfCanvas::zoomOut()
{
    setZoom(zoomLevel_ / ZOOM_STEP);
}

void PdfCanvas::fitWidth()
{
    if (!document_ || document_->pageCount() == 0) {
        return;
    }

    auto pageInfo = document_->pageInfo(currentPage_);
    int viewportWidth = width() - 40; // Margin
    float zoom = static_cast<float>(viewportWidth) / pageInfo.widthPt;
    setZoom(zoom);
}

void PdfCanvas::fitPage()
{
    if (!document_ || document_->pageCount() == 0) {
        return;
    }

    auto pageInfo = document_->pageInfo(currentPage_);
    int viewportWidth = width() - 40;
    int viewportHeight = height() - 40;

    float zoomWidth = static_cast<float>(viewportWidth) / pageInfo.widthPt;
    float zoomHeight = static_cast<float>(viewportHeight) / pageInfo.heightPt;
    float zoom = std::min(zoomWidth, zoomHeight);

    setZoom(zoom);
}

void PdfCanvas::goToPage(int index)
{
    if (!document_ || index < 0 || index >= document_->pageCount()) {
        return;
    }

    if (index == currentPage_) {
        return;
    }

    currentPage_ = index;

    // Scroll to page
    if (index < static_cast<int>(pageLayouts_.size())) {
        scrollOffset_.setY(pageLayouts_[index].yOffset);
        requestVisibleTiles();
        update();
    }

    emit pageChanged(currentPage_);
}

int PdfCanvas::selectedAnnotationCount() const
{
    // SelectTool owns the multi-selection; other tools have none.
    if (auto* sel = dynamic_cast<pdfe::annotations::SelectTool*>(currentTool_.get())) {
        const int multi = static_cast<int>(sel->selectedAnnotIndices().size());
        if (multi > 0) {
            return multi;
        }
        return sel->selectedAnnotIndex() >= 0 ? 1 : 0;
    }
    return 0;
}

int PdfCanvas::pageCount() const
{
    return document_ ? document_->pageCount() : 0;
}

void PdfCanvas::scrollToPdfRect(int pageIndex, const QRectF& pdfRect)
{
    if (!document_ || pageIndex < 0 || pageIndex >= document_->pageCount()) {
        return;
    }

    // Navigate to the page first (updates currentPage_ and scrollOffset_ if
    // the page differs; for same-page annotations this is a no-op that we
    // then override below).
    if (pageIndex != currentPage_) {
        // Temporarily allow goToPage to run for multi-page navigation.
        // goToPage short-circuits when index == currentPage_, so we must
        // set currentPage_ first for the cross-page case.
        currentPage_ = pageIndex - 1;   // force "different page" check
    }
    goToPage(pageIndex);

    // scrollToPdfRect is called for within-page navigation too (e.g. when the
    // comment panel navigates to an annotation on the same page that is
    // already current but currently scrolled off-screen).  Compute the
    // document-space Y of the annotation center and center the viewport on it.
    if (pageIndex >= static_cast<int>(pageLayouts_.size())) {
        return;
    }
    const auto& layout = pageLayouts_[pageIndex];

    // pdfRect uses Y-down coordinates relative to the page top (MuPDF convention).
    const qreal annotCenterYInPage =
        (pdfRect.top() + pdfRect.height() / 2.0) * static_cast<qreal>(zoomLevel_);
    const qreal annotCenterDocY =
        static_cast<qreal>(layout.yOffset) + annotCenterYInPage;

    // Desired scroll: center the annotation's midpoint in the viewport.
    int newY = static_cast<int>(annotCenterDocY) - height() / 2;
    newY = std::clamp(newY, 0, std::max(0, totalDocumentHeight_ - height()));

    scrollOffset_.setY(newY);
    requestVisibleTiles();
    update();
}

void PdfCanvas::setSearchHighlights(int pageIndex, const QVector<QRectF>& quads)
{
    searchHighlights_[pageIndex] = quads;
    update();
}

void PdfCanvas::refreshPage(int pageIndex)
{
    if (tileRenderer_) {
        tileRenderer_->cancelPage(pageIndex);
    }
    if (tileCache_) {
        tileCache_->invalidate(pageIndex);
    }
    requestVisibleTiles();
    update();
}

void PdfCanvas::refreshAllPages()
{
    // A structural change (insert/delete/undo of a page op) can change the page
    // COUNT, so rebuild pageLayouts_ from the current document BEFORE repainting.
    // Otherwise a queued paintEvent runs against a stale layout still referencing
    // a now-removed page, and pageInfo(staleIndex) throws out of the Qt paint
    // path → uncaught → std::terminate (the journey op-99 "batch4-postundo"
    // crash: insert-blank-page then undo shrinks the doc; the old refresh only
    // invalidated tiles and never rebuilt the layout).
    updateDocumentLayout();
    if (tileRenderer_) {
        tileRenderer_->cancelAll();
    }
    if (tileCache_) {
        tileCache_->invalidateAll();
    }
    requestVisibleTiles();
    update();
}

void PdfCanvas::clearSearchHighlights()
{
    searchHighlights_.clear();
    update();
}

void PdfCanvas::setOcrBoxesVisible(bool visible)
{
    ocrBoxesVisible_ = visible;
    update();
}

void PdfCanvas::setOcrBoxData(int pageIndex, std::vector<pdfe::tre::HocrLine> lines)
{
    ocrBoxData_[pageIndex] = std::move(lines);
    if (ocrBoxesVisible_)
        update();
}

void PdfCanvas::setOcrDpi(float dpi)
{
    ocrDpi_ = (dpi > 0.0f) ? dpi : 300.0f;
}

void PdfCanvas::clearOcrBoxData()
{
    ocrBoxData_.clear();
    if (ocrBoxesVisible_)
        update();
}

const std::vector<pdfe::tre::HocrLine>* PdfCanvas::ocrBoxDataForPage(int pageIndex) const
{
    auto it = ocrBoxData_.find(pageIndex);
    return (it != ocrBoxData_.end()) ? &it->second : nullptr;
}

void PdfCanvas::setCurrentTool(std::unique_ptr<pdfe::annotations::AnnotationTool> tool)
{
    // Clear text edit tool -- mutually exclusive
    clearTextEditTool();

    // Deactivate old tool
    if (currentTool_) {
        currentTool_->deactivate();
    }

    // Set new tool
    currentTool_ = std::move(tool);

    // Activate new tool and connect callback
    if (currentTool_) {
        currentTool_->activate();
        currentTool_->annotationCreated = [this](int pageIndex, pdfe::annotations::AnnotationData data) {
            emit annotationCreated(pageIndex, data);
        };
    }
    updateInputMethodEnabled();
}

// GitHub #269. Reconcile the tool's page with the page the event names, before
// the event is dispatched.
//
// This is the fix for all twelve handlers listed in
// tools/gate/tool_clicked_page_baseline.json, and it fixes them without
// touching any of them: each one writes through `page_` and reports
// `pageIndex`, and after this `page_` IS the page `pageIndex` names.
//
// Before this, MainWindow bound a tool once at activation to whatever page was
// then current, and nothing rebound it -- so a click on any other page created
// the annotation on the remembered one and reported the clicked one. Measured
// 2026-09-30: the sticky note landed on the wrong page, and MainWindow's
// read-back then asked the clicked page for an index belonging to the other,
// threw "Annotation index out of range" across a Qt event handler, and killed
// the process at roughly the 830th note.
bool PdfCanvas::bindToolToPage(int pageIdx)
{
    if (!currentTool_) return false;
    if (!document_) return false;
    if (pageIdx < 0 || pageIdx >= document_->pageCount()) return false;

    // Already bound to this page: nothing to do, and no Page reload. The common
    // case by far -- a user clicks the same page repeatedly.
    if (toolPageIndex_ == pageIdx && toolPage_) return true;

    auto page = document_->page(pageIdx);
    if (!page) return false;

    toolPage_ = std::move(page);
    toolPageIndex_ = pageIdx;
    currentTool_->setPage(toolPage_.get());
    return true;
}

void PdfCanvas::releaseToolPage()
{
    toolPage_.reset();
    toolPageIndex_ = -1;
}

void PdfCanvas::clearCurrentTool()
{
    if (currentTool_) {
        currentTool_->deactivate();
        currentTool_.reset();
    }
    // GitHub #269. The binding belongs to the tool that is going away; leaving
    // the index set would make the next tool's first click on that page a
    // no-op bind, and it would never receive a page at all.
    releaseToolPage();
    updateInputMethodEnabled();
}

void PdfCanvas::setTextEditTool(std::unique_ptr<pdfe::textedit::TextEditTool> tool)
{
    // Clear annotation tool -- mutually exclusive
    clearCurrentTool();

    // Clear old text edit tool
    if (textEditTool_) {
        textEditTool_->deactivate();
    }

    textEditTool_ = std::move(tool);

    if (textEditTool_) {
        textEditTool_->activate();
        // Connect cursor blink repaint signal
        connect(textEditTool_.get(), &pdfe::textedit::TextEditTool::repaintRequested,
                this, QOverload<>::of(&QWidget::update));
        // WIN-49: the canvas advertises IME readiness exactly while a span is
        // being edited. editingStateChanged is the tool's own authority on
        // that, so the attribute cannot drift from the editing state the way a
        // manually-maintained flag would.
        connect(textEditTool_.get(), &pdfe::textedit::TextEditTool::editingStateChanged,
                this, [this](bool) { updateInputMethodEnabled(); });
        // Set current page
        if (document_ && currentPage_ >= 0 && currentPage_ < document_->pageCount()) {
            textEditTool_->setPage(currentPage_);
        }
    }
    updateInputMethodEnabled();
}

void PdfCanvas::clearTextEditTool()
{
    if (textEditTool_) {
        textEditTool_->deactivate();
        textEditTool_.reset();
    }
    updateInputMethodEnabled();
}

void PdfCanvas::setImageTool(pdfe::image::ImageTool* tool)
{
    imageTool_ = tool;
    if (imageTool_) {
        connect(imageTool_, &pdfe::image::ImageTool::repaintRequested,
                this, QOverload<>::of(&QWidget::update));
    }
    update();
}

void PdfCanvas::clearImageTool()
{
    if (imageTool_) {
        disconnect(imageTool_, &pdfe::image::ImageTool::repaintRequested,
                   this, QOverload<>::of(&QWidget::update));
    }
    imageTool_ = nullptr;
    update();
}

void PdfCanvas::setCopyPrevented(bool prevented)
{
    copyPrevented_ = prevented;
}

float PdfCanvas::devicePixelRatio() const
{
    return devicePixelRatio_;
}

void PdfCanvas::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    // R3-17: in High-Contrast mode the viewport backdrop must come from the HC
    // system palette (Window role) so the canvas is not a fixed mid-grey that
    // clashes with the theme; in normal mode this is the usual Dark shade.
    painter.fillRect(rect(),
                     AccessibilityHelper::adaptColor(palette().color(QPalette::Dark),
                                                     QPalette::Window));

    if (!document_ || pageLayouts_.empty()) {
        return;
    }

    // Calculate visible region
    QRect viewportRect = rect();

    // Draw each visible page
    for (const auto& layout : pageLayouts_) {
        // Defensive: a repaint can be queued after a structural change but
        // before pageLayouts_ is rebuilt, leaving an entry for a now-removed
        // page. pageInfo()/render throw on an out-of-range index, and a throw
        // escaping paintEvent terminates the process — so a paint must never
        // touch a page the document no longer has. (refreshAllPages() now
        // rebuilds the layout up front; this is belt-and-suspenders.)
        if (layout.pageIndex < 0 || layout.pageIndex >= document_->pageCount()) {
            continue;
        }
        int pageTop = layout.yOffset - scrollOffset_.y();
        int pageBottom = pageTop + layout.heightPx;

        // Skip if page is not visible
        if (pageBottom < 0 || pageTop > viewportRect.height()) {
            continue;
        }

        // Center page horizontally. layout.widthPx/heightPx are post-rotation
        // (on-screen) dims; the tile cache stores unrotated tiles so we draw
        // them in the pre-rotation coordinate space, then rotate the painter.
        // When the page is wider than the viewport (zoomed in), apply
        // scrollOffset_.x() so the Right/Left arrow keys pan the view.
        int pageLeft = (viewportRect.width() - layout.widthPx) / 2 - scrollOffset_.x();
        QRect pageRect(pageLeft, pageTop, layout.widthPx, layout.heightPx);

        // Draw page background. Normal mode: white paper. High-Contrast mode:
        // the HC palette Base color so the page substrate matches the theme
        // (R3-17) instead of a fixed white that can wash out HC content.
        painter.fillRect(pageRect,
                         AccessibilityHelper::adaptColor(QColor(Qt::white), QPalette::Base));

        // Draw page border (adapts to the HC text color for a visible edge).
        painter.setPen(AccessibilityHelper::adaptColor(QColor(Qt::black), QPalette::WindowText));
        painter.drawRect(pageRect);

        const bool transpose = (viewRotation_ == 90 || viewRotation_ == 270);
        const int unrotatedWidth  = transpose ? layout.heightPx : layout.widthPx;
        const int unrotatedHeight = transpose ? layout.widthPx  : layout.heightPx;

        painter.save();
        if (viewRotation_ != 0) {
            // Rotate around the center of the on-screen page rect, then
            // translate so the unrotated coord (0,0) sits at top-left of
            // the unrotated page.
            QTransform t;
            t.translate(pageLeft + layout.widthPx / 2.0,
                        pageTop  + layout.heightPx / 2.0);
            t.rotate(viewRotation_);
            t.translate(-unrotatedWidth  / 2.0,
                        -unrotatedHeight / 2.0);
            painter.setTransform(t, /*combine=*/true);
        }

        // Tile drawing coordinates (pre-rotation). When viewRotation_ != 0,
        // the QTransform above maps these into the rotated page rect.
        const int tileOriginX = (viewRotation_ == 0) ? pageLeft : 0;
        const int tileOriginY = (viewRotation_ == 0) ? pageTop  : 0;

        // Draw cached tiles for this page.
        //
        // R3-11 HiDPI: tiles are RASTERIZED at device resolution
        // (effectiveRenderZoom = zoomLevel_ * devicePixelRatio_), so the tile
        // grid is measured in DEVICE pixels and each TILE_SIZE-device-px tile
        // paints into a TILE_SIZE/dpr LOGICAL footprint. The cached QImage
        // carries devicePixelRatio_ (set at request time), so QPainter treats
        // the source rect in device-independent coords → drawing a 512-device-px
        // tile into a 512/dpr logical rect lands 1:1 on the HiDPI framebuffer
        // (sharp, not upscaled). At dpr==1 this reduces exactly to the prior
        // logical-pixel behavior. Coordinate mapping stays logical (uses
        // zoomLevel_ only), so hit-testing is unchanged by DPR.
        if (tileCache_) {
            const double dpr = static_cast<double>(devicePixelRatio_);
            const int deviceWidth  = static_cast<int>(std::lround(unrotatedWidth  * dpr));
            const int deviceHeight = static_cast<int>(std::lround(unrotatedHeight * dpr));
            const float renderZoom = effectiveRenderZoom();

            int cols = (deviceWidth  + pdfe::rendering::TILE_SIZE - 1) / pdfe::rendering::TILE_SIZE;
            int rows = (deviceHeight + pdfe::rendering::TILE_SIZE - 1) / pdfe::rendering::TILE_SIZE;

            for (int row = 0; row < rows; ++row) {
                for (int col = 0; col < cols; ++col) {
                    pdfe::rendering::TileId tileId{layout.pageIndex, col, row, renderZoom};
                    auto tile = tileCache_->get(tileId);
                    if (tile && !tile->image.isNull()) {
                        // Edge tiles: clip to the page bounds in DEVICE pixels,
                        // then convert both the paint origin and extent to the
                        // LOGICAL footprint by dividing by dpr.
                        int drawWdev = std::min(pdfe::rendering::TILE_SIZE, deviceWidth  - col * pdfe::rendering::TILE_SIZE);
                        int drawHdev = std::min(pdfe::rendering::TILE_SIZE, deviceHeight - row * pdfe::rendering::TILE_SIZE);

                        QRectF targetRect(tileOriginX + (col * pdfe::rendering::TILE_SIZE) / dpr,
                                          tileOriginY + (row * pdfe::rendering::TILE_SIZE) / dpr,
                                          drawWdev / dpr,
                                          drawHdev / dpr);
                        // Source in device-independent coords (image carries DPR).
                        QRectF sourceRect(0.0, 0.0, drawWdev / dpr, drawHdev / dpr);
                        painter.drawImage(targetRect, tile->image, sourceRect);
                    }
                }
            }
        }

        // Build a temporary layout entry with UNROTATED dims so overlay helpers
        // that assume widthPx/heightPx match the tile grid keep working.
        PageLayout unrotLayout = layout;
        unrotLayout.widthPx  = unrotatedWidth;
        unrotLayout.heightPx = unrotatedHeight;

        // Draw search highlights for this page
        paintSearchHighlights(painter, unrotLayout, tileOriginX, tileOriginY);

        // #50: JPEG2000 "disabled for security" placeholder overlay. No-ops when
        // JPX rendering is enabled or the page has no JPX images.
        paintJpxPlaceholders(painter, unrotLayout, tileOriginX, tileOriginY);

        // SEC-56 (#56): the same overlay for JBIG2. No-ops when JBIG2 rendering
        // is enabled or the page has no JBIG2 images.
        paintJbig2Placeholders(painter, unrotLayout, tileOriginX, tileOriginY);

        // Draw OCR character bounding box overlay
        if (ocrBoxesVisible_)
            paintOcrBoxes(painter, unrotLayout, tileOriginX, tileOriginY);

        // Draw annotation tool overlay for this page
        if (currentTool_) {
            auto pageInfo = document_->pageInfo(layout.pageIndex);
            float pageHeightPt = pageInfo.heightPt;
            QPointF pageOrigin(tileOriginX, tileOriginY);
            currentTool_->onPaintOverlay(painter, layout.pageIndex, zoomLevel_, pageOrigin, pageHeightPt);
        }

        // Draw text edit tool overlay for this page
        if (textEditTool_) {
            auto pageInfo = document_->pageInfo(layout.pageIndex);
            float pageHeightPt = pageInfo.heightPt;
            QPointF pageOrigin(tileOriginX, tileOriginY);
            textEditTool_->onPaintOverlay(painter, layout.pageIndex, zoomLevel_, pageOrigin, pageHeightPt);
        }

        // Draw image tool overlay for this page
        if (imageTool_) {
            imageTool_->paint(painter, zoomLevel_);
        }

        painter.restore();
    }

    // Update current page based on scroll position
    int newCurrentPage = findPageAtY(scrollOffset_.y() + viewportRect.height() / 2);
    if (newCurrentPage != currentPage_ && newCurrentPage >= 0) {
        currentPage_ = newCurrentPage;
        emit pageChanged(currentPage_);
    }
}

void PdfCanvas::paintSearchHighlights(QPainter& painter, const PageLayout& layout,
                                       int pageLeft, int pageTop)
{
    auto it = searchHighlights_.find(layout.pageIndex);
    if (it == searchHighlights_.end() || it->second.isEmpty()) {
        return;
    }

    // Semi-transparent yellow for search highlights. In High-Contrast mode the
    // fixed yellow can be invisible against an HC background, so route both the
    // fill and border through the HC system Highlight color (R3-17); the alpha
    // of the hardcoded defaults is preserved so the overlay stays translucent.
    QColor highlightColor = AccessibilityHelper::adaptColor(QColor(255, 255, 0, 100),
                                                            QPalette::Highlight);
    QColor borderColor = AccessibilityHelper::adaptColor(QColor(255, 200, 0, 200),
                                                         QPalette::Highlight);

    painter.save();

    for (const auto& pdfRect : it->second) {
        // MuPDF fz_search_page returns quads in fitz-space (Y-down, like screen).
        // Scale from fitz-space points to screen pixels — no Y-flip needed.
        float sx = static_cast<float>(pdfRect.x()) * zoomLevel_;
        float sy = static_cast<float>(pdfRect.y()) * zoomLevel_;
        float sw = static_cast<float>(pdfRect.width()) * zoomLevel_;
        float sh = static_cast<float>(pdfRect.height()) * zoomLevel_;

        QRectF screenRect(pageLeft + sx, pageTop + sy, sw, sh);

        painter.fillRect(screenRect, highlightColor);
        painter.setPen(borderColor);
        painter.drawRect(screenRect);
    }

    painter.restore();
}

void PdfCanvas::paintOcrBoxes(QPainter& painter, const PageLayout& layout,
                               int pageLeft, int pageTop)
{
    auto it = ocrBoxData_.find(layout.pageIndex);
    if (it == ocrBoxData_.end())
        return;

    const float pdfScale = 72.0f / ocrDpi_;  // HOCR pixels → PDF points

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);

    QPen charPen(QColor(0, 150, 255, 160), 1);
    QPen wordPen(QColor(255, 100, 0, 120), 1, Qt::DashLine);

    for (const auto& line : it->second) {
        for (const auto& word : line.words) {
            // Word bbox (dashed orange)
            float wx = word.bbox.x0 * pdfScale * zoomLevel_;
            float wy = word.bbox.y0 * pdfScale * zoomLevel_;
            float ww = (word.bbox.x1 - word.bbox.x0) * pdfScale * zoomLevel_;
            float wh = (word.bbox.y1 - word.bbox.y0) * pdfScale * zoomLevel_;
            painter.setPen(wordPen);
            painter.drawRect(QRectF(pageLeft + wx, pageTop + wy, ww, wh));

            // Char bboxes (solid blue)
            painter.setPen(charPen);
            for (const auto& ch : word.chars) {
                if (ch.codepoint == 0)
                    continue;
                float cx = ch.bbox.x0 * pdfScale * zoomLevel_;
                float cy = ch.bbox.y0 * pdfScale * zoomLevel_;
                float cw = (ch.bbox.x1 - ch.bbox.x0) * pdfScale * zoomLevel_;
                float chh = (ch.bbox.y1 - ch.bbox.y0) * pdfScale * zoomLevel_;
                painter.drawRect(QRectF(pageLeft + cx, pageTop + cy, cw, chh));
            }
        }
    }

    painter.restore();
}

// static
QVector<QRectF> PdfCanvas::jpxPlaceholderRectsForPage(pdfe::core::Document* doc, int pageIndex)
{
    QVector<QRectF> rects;
    // When JPX decoding is enabled the real image is shown — nothing to overlay.
    if (!doc || pdfe::core::isJpxDecodingEnabled())
        return rects;
    if (pageIndex < 0 || pageIndex >= doc->pageCount())
        return rects;
    try {
        auto page = doc->page(pageIndex);
        if (!page)
            return rects;
        pdfe::image::ImageManager mgr(doc->context());
        for (const auto& info : mgr.enumerateImages(page.get(), doc)) {
            if (info.isJpx)
                rects.push_back(info.boundingBox.normalized());
        }
    } catch (...) {
        rects.clear();  // detection must never break the caller
    }
    return rects;
}

void PdfCanvas::paintJpxPlaceholders(QPainter& painter, const PageLayout& layout,
                                     int pageLeft, int pageTop)
{
    if (!document_ || pdfe::core::isJpxDecodingEnabled())
        return;

    // Lazily compute + cache the raw JPX image rects (PDF Y-up) for this page.
    // The cache is pref-independent; the enabled check above gates drawing.
    auto it = jpxRegions_.find(layout.pageIndex);
    if (it == jpxRegions_.end()) {
        QVector<QRectF> raw;
        try {
            if (auto page = document_->page(layout.pageIndex)) {
                pdfe::image::ImageManager mgr(document_->context());
                for (const auto& info : mgr.enumerateImages(page.get(), document_)) {
                    if (info.isJpx)
                        raw.push_back(info.boundingBox.normalized());
                }
            }
        } catch (...) {
            raw.clear();  // never let overlay computation break a paint
        }
        it = jpxRegions_.emplace(layout.pageIndex, std::move(raw)).first;
    }
    if (it->second.isEmpty())
        return;

    float pageHeightPt = 0.0f;
    try {
        pageHeightPt = document_->pageInfo(layout.pageIndex).heightPt;
    } catch (...) {
        return;
    }

    painter.save();
    const QColor fill(70, 70, 70, 45);
    const QColor border(170, 130, 0, 210);
    const QString msg = tr("JPEG2000 image hidden for security.\n"
                           "Enable in Preferences → Security.");

    for (const QRectF& pdfRect : it->second) {
        // PDF user space (Y-up, origin bottom-left) → screen (Y-down from page top).
        const float sx  = static_cast<float>(pdfRect.x()) * zoomLevel_;
        const float top = pageHeightPt - static_cast<float>(pdfRect.y() + pdfRect.height());
        const float sy  = top * zoomLevel_;
        const float sw  = static_cast<float>(pdfRect.width()) * zoomLevel_;
        const float sh  = static_cast<float>(pdfRect.height()) * zoomLevel_;
        const QRectF box(pageLeft + sx, pageTop + sy, sw, sh);

        painter.fillRect(box, fill);
        painter.setPen(QPen(border, 1, Qt::DashLine));
        painter.drawRect(box);

        // Only label boxes big enough to hold a line of text.
        if (box.width() >= 60 && box.height() >= 24) {
            painter.setPen(QColor(30, 30, 30));
            painter.drawText(box.adjusted(4, 4, -4, -4),
                             Qt::AlignCenter | Qt::TextWordWrap, msg);
        }
    }
    painter.restore();
}

// static
QVector<QRectF> PdfCanvas::jbig2PlaceholderRectsForPage(pdfe::core::Document* doc, int pageIndex)
{
    QVector<QRectF> rects;
    // When JBIG2 decoding is enabled the real image is shown — nothing to overlay.
    if (!doc || pdfe::core::isJbig2DecodingEnabled())
        return rects;
    if (pageIndex < 0 || pageIndex >= doc->pageCount())
        return rects;
    try {
        auto page = doc->page(pageIndex);
        if (!page)
            return rects;
        pdfe::image::ImageManager mgr(doc->context());
        for (const auto& info : mgr.enumerateImages(page.get(), doc)) {
            if (info.isJbig2)
                rects.push_back(info.boundingBox.normalized());
        }
    } catch (...) {
        rects.clear();  // detection must never break the caller
    }
    return rects;
}

void PdfCanvas::paintJbig2Placeholders(QPainter& painter, const PageLayout& layout,
                                       int pageLeft, int pageTop)
{
    if (!document_ || pdfe::core::isJbig2DecodingEnabled())
        return;

    // Lazily compute + cache the raw JBIG2 image rects (PDF Y-up) for this page.
    // The cache is pref-independent; the enabled check above gates drawing.
    auto it = jbig2Regions_.find(layout.pageIndex);
    if (it == jbig2Regions_.end()) {
        QVector<QRectF> raw;
        try {
            if (auto page = document_->page(layout.pageIndex)) {
                pdfe::image::ImageManager mgr(document_->context());
                for (const auto& info : mgr.enumerateImages(page.get(), document_)) {
                    if (info.isJbig2)
                        raw.push_back(info.boundingBox.normalized());
                }
            }
        } catch (...) {
            raw.clear();  // never let overlay computation break a paint
        }
        it = jbig2Regions_.emplace(layout.pageIndex, std::move(raw)).first;
    }
    if (it->second.isEmpty())
        return;

    float pageHeightPt = 0.0f;
    try {
        pageHeightPt = document_->pageInfo(layout.pageIndex).heightPt;
    } catch (...) {
        return;
    }

    painter.save();
    const QColor fill(70, 70, 70, 45);
    const QColor border(170, 130, 0, 210);
    const QString msg = tr("JBIG2 image hidden for security.\n"
                           "Enable in Preferences → Security.");

    for (const QRectF& pdfRect : it->second) {
        // PDF user space (Y-up, origin bottom-left) → screen (Y-down from page top).
        const float sx  = static_cast<float>(pdfRect.x()) * zoomLevel_;
        const float top = pageHeightPt - static_cast<float>(pdfRect.y() + pdfRect.height());
        const float sy  = top * zoomLevel_;
        const float sw  = static_cast<float>(pdfRect.width()) * zoomLevel_;
        const float sh  = static_cast<float>(pdfRect.height()) * zoomLevel_;
        const QRectF box(pageLeft + sx, pageTop + sy, sw, sh);

        painter.fillRect(box, fill);
        painter.setPen(QPen(border, 1, Qt::DashLine));
        painter.drawRect(box);

        // Only label boxes big enough to hold a line of text.
        if (box.width() >= 60 && box.height() >= 24) {
            painter.setPen(QColor(30, 30, 30));
            painter.drawText(box.adjusted(4, 4, -4, -4),
                             Qt::AlignCenter | Qt::TextWordWrap, msg);
        }
    }
    painter.restore();
}

void PdfCanvas::wheelEvent(QWheelEvent* event)
{
    // GitHub #88 / #89. This handler used to read ONLY angleDelta().y():
    //
    //   * A precision device (trackpad, high-resolution wheel) reports
    //     pixelDelta() with angleDelta() ZERO. delta was therefore 0, newY
    //     equalled scrollOffset_.y(), and the canvas did not scroll AT ALL
    //     — not "coarsely", not at all (#88).
    //   * angleDelta().x() / pixelDelta().x() were never read anywhere, so
    //     horizontal wheel and two-finger side-scroll input was silently
    //     discarded (#89).
    //
    // Per axis: prefer angleDelta (notched wheels, 1/8-degree units, 120 per
    // notch) and fall back to pixelDelta only when angleDelta is zero on that
    // axis. That keeps ordinary-wheel behaviour bit-for-bit identical — a 120
    // unit notch still moves 120 px, exactly as before — while making
    // precision input work.
    const QPoint angle = event->angleDelta();
    const QPoint pixel = event->pixelDelta();
    const int dy = (angle.y() != 0) ? angle.y() : pixel.y();
    const int dx = (angle.x() != 0) ? angle.x() : pixel.x();

    if (event->modifiers() & Qt::ControlModifier) {
        // Zoom with Ctrl+wheel (vertical component only).
        if (dy > 0) {
            zoomIn();
        } else if (dy < 0) {
            zoomOut();
        } else {
            // Nothing to zoom: do not swallow the event.
            event->ignore();
            return;
        }
        event->accept();
        return;
    }

    bool changed = false;

    // Vertical scroll.
    if (dy != 0) {
        const int newY = std::clamp(scrollOffset_.y() - dy,
                                    0,
                                    std::max(0, totalDocumentHeight_ - height()));
        if (newY != scrollOffset_.y()) {
            scrollOffset_.setY(newY);
            changed = true;
        }
    }

    // Horizontal scroll — clamped against the document WIDTH, the same bound
    // Key_Left/Key_Right pan against. Handled distinctly from the vertical
    // axis: a horizontal-only event must never move scrollOffset_.y().
    if (dx != 0) {
        const int newX = std::clamp(scrollOffset_.x() - dx,
                                    0,
                                    std::max(0, totalDocumentWidth_ - width()));
        if (newX != scrollOffset_.x()) {
            scrollOffset_.setX(newX);
            changed = true;
        }
    }

    if (changed) {
        requestVisibleTiles();
        update();
        emit scrolled();
        event->accept();
    } else {
        // Previously accept() ran on EVERY path, including the ones that
        // changed nothing — so a no-op was indistinguishable from a handled
        // scroll and never propagated. Ignoring lets a parent (or a future
        // scroll area) see the event, and makes the no-op observable.
        event->ignore();
    }
}

void PdfCanvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (document_) {
        updateDocumentLayout();
        requestVisibleTiles();
        update();
    }
}

void PdfCanvas::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // First show: the widget now has valid geometry, so request tiles that
    // were skipped during setDocument() when rect() was still 0x0.
    if (document_) {
        updateDocumentLayout();
        requestVisibleTiles();
        update();
    }
}

void PdfCanvas::mousePressEvent(QMouseEvent* event)
{
    // GitHub #119 — DRAG-TO-PAN WHILE A TOOL IS ACTIVE (middle button).
    //
    // THE DEFECT. The only pan gesture this canvas had was the LEFT-button
    // fallback at the bottom of this function, reached only when nothing else
    // claimed the press. 7cf860d3 fixed SelectTool being shown as checked
    // while never actually installed; with a real SelectTool present, the
    // `currentTool_ && LeftButton` branch above claims every left press and
    // rubber-bands. Panning therefore became unreachable for the entire time
    // a document is open, which is the whole reading session.
    //
    // WHY MIDDLE-DRAG AND NOT A HAND TOOL. A Hand QAction would restore
    // panning only by making the user LEAVE the tool they are using and come
    // back — the gesture would be modal, which is precisely what "pan while
    // the Select tool is active" asks not to be. It would also fix the defect
    // for SelectTool alone in spirit while the same starvation applies to
    // every other tool (ink, shape, redaction, form, crop, measure): each of
    // them consumes left-drag too. A modifier-chord (space-drag, Ctrl-drag)
    // was rejected for the reason recorded on the link-follow branch below —
    // there is no other modifier affordance on this canvas, so it would be
    // discoverable only from documentation.
    //
    // Middle-drag is tool-independent, needs no new UI state, and is the
    // gesture Acrobat, every browser and every CAD tool already use for pan.
    // It is also SAFE TO PUT FIRST: every tool-dispatch branch below is gated
    // on LeftButton or RightButton, and nothing in this canvas has ever
    // handled MiddleButton, so this branch cannot steal a press from the
    // link-follow path (#135 / #174) or from any tool.
    //
    // Unlike the left-button fallback, this pans BOTH axes, against the same
    // bounds wheelEvent clamps to.
    if (event->button() == Qt::MiddleButton) {
        isMiddlePanning_ = true;
        lastMiddlePanPos_ = event->pos();
        cursorBeforeMiddlePan_ = cursor().shape();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    // Text edit tool takes priority. Handles BOTH LeftButton (span
    // hit-test / cursor placement) and RightButton (TextEditTool
    // emits contextMenuRequested with Atlas Mode + Compare Atlas
    // Modes... entries via MainWindow::buildTextEditContextMenu).
    // contextMenuEvent below defers to QWidget when textEditTool_ is
    // set, so the right-click path MUST be handled here in mousePress.
    if (textEditTool_ &&
        (event->button() == Qt::LeftButton ||
         event->button() == Qt::RightButton)) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);

        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();

            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;

            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);

            // GitHub #174 — link following while the text tool is active.
            //
            // #135 made a left-click follow a /Link annotation, but only on
            // the SelectTool branch further down; this branch returns first,
            // so a link click during text editing still did nothing. The
            // interaction rule chosen here (option 1 of the three the issue
            // lists) is the one this very function ALREADY applies to image
            // blocks two statements below: route per region. A click follows
            // a link only when the point carries NO live text span, and only
            // when no text-edit interaction is in progress.
            //
            // Rejected alternatives, deliberately:
            //   * Ctrl-click. There is no other modifier affordance anywhere
            //     on this canvas, so the gesture would be discoverable only
            //     from documentation — and "link following looks dead" is the
            //     exact failure mode #135 existed to end.
            //   * Declare it inert and pin that. It leaves the read-a-document
            //     -with-the-text-tool-armed case dead, which is the same shape
            //     as the original outage, only narrower.
            //
            // Editing ALWAYS outranks navigation: a click on a live span, or
            // any click at all while a span is active or text is selected,
            // belongs to the editor. The link is still reachable — release the
            // edit first, then click it. The ordering against the image-block
            // routing below is deliberate too: a /Link is an explicit,
            // author-placed hit rect, while the region route is the fallback
            // for "nothing live here", so the specific hit wins.
            if (event->button() == Qt::LeftButton && linkFollowRequested &&
                textEditTool_->spanIndexAtPoint(pdfPoint) < 0 &&
                !textEditTool_->hasActiveSpan() &&
                !textEditTool_->hasTextSelection() &&
                linkFollowRequested(pageIdx, pdfPoint)) {
                update();
                event->accept();
                return;
            }

            // Defect #4: per-region routing. On a mixed-content page the text
            // tool is the page default (the page has SOME live text), but a
            // left-click that lands on a scanned image block — with no live
            // text span under it — belongs to the region/TRE tool, not the
            // per-span tool. Redirect that click rather than letting the span
            // tool swallow it. (Right-click still falls through to the span
            // tool's context menu below.)
            if (event->button() == Qt::LeftButton && document_ &&
                textEditTool_->spanIndexAtPoint(pdfPoint) < 0) {
                auto pageObj = document_->page(pageIdx);
                if (pageObj && pageObj->hasImageContentAt(pdfPoint)) {
                    emit regionToolRequested(pageIdx, pdfPoint);
                    update();
                    event->accept();
                    return;
                }
            }

            textEditTool_->onMousePress(event, pageIdx, pdfPoint);
            update();
            event->accept();
            return;
        }
    }

    // Image tool dispatch (third path)
    if (imageTool_ && event->button() == Qt::LeftButton) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);
        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();
            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;
            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);
            imageTool_->mousePressEvent(event, pdfPoint, zoomLevel_);
            update();
            event->accept();
            return;
        }
    }

    // If there's a current tool, delegate to it
    if (currentTool_ && event->button() == Qt::LeftButton) {
        // Find which page was clicked
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);

        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();

            // Get page height in points
            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;

            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);

            // GitHub #135. A left-click on a /Link annotation must still
            // follow the link while the SELECT tool is active, because
            // Select is the tool that is active while the user is simply
            // READING — it is installed on document open and never
            // uninstalled unless another tool is chosen. Before this,
            // delegating to it and returning made the link hit-test at the
            // bottom of this function unreachable for the whole time a
            // document was open, so link following was dead in the product.
            //
            // Deliberately restricted to SelectTool. Every other tool is an
            // explicit choice to CREATE or EDIT something at the click point
            // (ink, shape, redaction, form, stamp, crop, measure...), and
            // stealing that press to navigate would be a worse bug than the
            // one being fixed. TextEditTool is handled even earlier and is
            // likewise untouched.
            if (event->button() == Qt::LeftButton && linkFollowRequested &&
                dynamic_cast<pdfe::annotations::SelectTool*>(
                    currentTool_.get()) != nullptr &&
                linkFollowRequested(pageIdx, pdfPoint)) {
                update();
                event->accept();
                return;
            }

            // GitHub #269: act on the page that was clicked, not the page the
            // tool was activated on. A failure to resolve means we must NOT
            // dispatch -- the tool would write to a stale page.
            if (bindToolToPage(pageIdx)) {
                currentTool_->onMousePress(event, pageIdx, pdfPoint);
            }
            // A click can start a form-field edit; resync the IME attribute.
            updateInputMethodEnabled();
            update(); // Request repaint for tool overlay
            event->accept();
            return;
        }
    }

    // Context menu for annotation tool (right-click)
    if (currentTool_ && event->button() == Qt::RightButton) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);
        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();
            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;
            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);
            QMenu* menu = currentTool_->onContextMenu(event, pageIdx, pdfPoint);
            if (menu) {
                menu->exec(event->globalPosition().toPoint());
                delete menu;
                update();
                event->accept();
                return;
            }
        }
        // Pipeline F phase-5: tool didn't surface a context menu (e.g.
        // SelectTool with no annotation selected). Fall back to the
        // canvas's own context menu so users see at least Zoom and
        // (when applicable) Alt Text actions.
        emit canvasContextMenuRequested(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    // No tool is consuming the click — give MainWindow a chance to
    // hit-test link annotations before we fall through to pan. If a
    // link is activated, MainWindow handles dispatch (URI open, page
    // navigation, named action, launch with confirmation).
    if (event->button() == Qt::LeftButton && document_) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);
        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();
            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;
            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);
            emit canvasClicked(pageIdx, pdfPoint);
            // Defer the pan-vs-link decision to the follow-link slot:
            // if it handled a link, it will have navigated or opened
            // a URL, and a subsequent pan is harmless because the user
            // didn't hold + drag. If no link hit, pan begins below.
        }
    }

    // Default behavior: pan the document
    if (event->button() == Qt::LeftButton) {
        isDragging_ = true;
        lastMousePos_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }
}

void PdfCanvas::mouseMoveEvent(QMouseEvent* event)
{
    // GitHub #119 — middle-button pan in progress outranks every tool, for
    // the reasons recorded in mousePressEvent. Checked first so a tool that
    // tracks hover (SelectTool's rubber band, TextEditTool's caret) cannot
    // swallow the move and starve the pan of its deltas.
    if (isMiddlePanning_) {
        const QPoint delta = event->pos() - lastMiddlePanPos_;
        lastMiddlePanPos_ = event->pos();

        const int newY = std::clamp(scrollOffset_.y() - delta.y(), 0,
                                    std::max(0, totalDocumentHeight_ - height()));
        const int newX = std::clamp(scrollOffset_.x() - delta.x(), 0,
                                    std::max(0, totalDocumentWidth_ - width()));
        bool changed = false;
        if (newY != scrollOffset_.y()) { scrollOffset_.setY(newY); changed = true; }
        if (newX != scrollOffset_.x()) { scrollOffset_.setX(newX); changed = true; }
        if (changed) {
            requestVisibleTiles();
            update();
            emit scrolled();
        }
        event->accept();
        return;
    }

    // Text edit tool takes priority
    if (textEditTool_) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);

        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();

            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;

            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);

            textEditTool_->onMouseMove(event, pageIdx, pdfPoint);
            update();
            event->accept();
            return;
        }
    }

    // Image tool dispatch (third path)
    if (imageTool_) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);
        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();
            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;
            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);
            imageTool_->mouseMoveEvent(event, pdfPoint, zoomLevel_);
            update();
            event->accept();
            return;
        }
    }

    // If there's a current tool, delegate to it
    if (currentTool_) {
        // Find which page the mouse is over
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);

        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();

            // Get page height in points
            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;

            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);

            currentTool_->onMouseMove(event, pageIdx, pdfPoint);
            update(); // Request repaint for tool overlay
            event->accept();
            return;
        }
    }

    // Default behavior: pan the document
    if (isDragging_) {
        QPoint delta = event->pos() - lastMousePos_;
        lastMousePos_ = event->pos();

        // Update scroll offset
        int newY = scrollOffset_.y() - delta.y();
        newY = std::clamp(newY, 0, std::max(0, totalDocumentHeight_ - height()));

        if (newY != scrollOffset_.y()) {
            scrollOffset_.setY(newY);
            update();
            emit scrolled();
        }
        event->accept();
    }
}

void PdfCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    // GitHub #119 — end the middle-button pan. Gated on MiddleButton so a
    // left release during a middle-drag (chorded buttons) does not silently
    // strand the pan flag set.
    if (event->button() == Qt::MiddleButton && isMiddlePanning_) {
        isMiddlePanning_ = false;
        setCursor(cursorBeforeMiddlePan_);
        event->accept();
        return;
    }

    // Text edit tool takes priority
    if (textEditTool_ && event->button() == Qt::LeftButton) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);

        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();

            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;

            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);

            textEditTool_->onMouseRelease(event, pageIdx, pdfPoint);
            update();
            event->accept();
            return;
        }
    }

    // Image tool dispatch (third path)
    if (imageTool_) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);
        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();
            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;
            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);
            imageTool_->mouseReleaseEvent(event, pdfPoint, zoomLevel_);
            update();
            event->accept();
            return;
        }
    }

    // If there's a current tool, delegate to it
    if (currentTool_ && event->button() == Qt::LeftButton) {
        // Find which page the mouse is over
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);

        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();

            // Get page height in points
            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;

            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);

            currentTool_->onMouseRelease(event, pageIdx, pdfPoint);
            updateInputMethodEnabled();
            update(); // Request repaint for tool overlay
            event->accept();
            return;
        }
    }

    // Default behavior: stop panning
    if (event->button() == Qt::LeftButton && isDragging_) {
        isDragging_ = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
    }
}

void PdfCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (textEditTool_ && event->button() == Qt::LeftButton) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);

        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();

            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;

            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);

            textEditTool_->onMouseDoubleClick(event, pageIdx, pdfPoint);
            update();
            event->accept();
            return;
        }
    }

    if (currentTool_ && event->button() == Qt::LeftButton) {
        int docY = scrollOffset_.y() + event->pos().y();
        int pageIdx = findPageAtY(docY);
        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();
            auto pageInfo = document_->pageInfo(pageIdx);
            float pageHeightPt = pageInfo.heightPt;
            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                event->pos(), zoomLevel_, pageOrigin, pageHeightPt);
            // GitHub #269, as for the press above. This is the path that
            // reaches MainWindow::onStickyNoteDoubleClicked.
            if (bindToolToPage(pageIdx)) {
                currentTool_->onMouseDoubleClick(event, pageIdx, pdfPoint);
            }
            update();
            event->accept();
            return;
        }
    }

    QWidget::mouseDoubleClickEvent(event);
}

void PdfCanvas::keyPressEvent(QKeyEvent* event)
{
    // Block copy shortcuts when copy prevention is active
    if (copyPrevented_ && (event->modifiers() & Qt::ControlModifier)) {
        if (event->key() == Qt::Key_C || event->key() == Qt::Key_X) {
            event->accept();
            return;
        }
    }

    if (textEditTool_) {
        textEditTool_->onKeyPress(event);
        update();
        if (event->isAccepted()) {
            return;  // TextEditTool consumed the key (e.g. Left/Right/Backspace/text)
        }
        // Key was explicitly ignored by TextEditTool (e.g. Up/Down) — fall through
        // to the canvas-level navigation handler below so scrolling still works.
        // Pipeline 3e7c1a05 phase-1 (UX fix).
    }
    if (imageTool_) {
        imageTool_->keyPressEvent(event);
        if (event->isAccepted()) {
            update();
            return;
        }
    }
    if (currentTool_) {
        currentTool_->onKeyPress(event);
        // Tab/Escape can start or end a form-field edit; resync the attribute.
        updateInputMethodEnabled();
        update();
        if (event->isAccepted()) {
            return;
        }
        // Fall through to canvas-level handlers if tool didn't consume the key
    }

    // Canvas-level keyboard navigation
    constexpr int SCROLL_STEP = 40;

    switch (event->key()) {
    case Qt::Key_PageDown:
        goToPage(currentPage_ + 1);
        event->accept();
        return;
    case Qt::Key_PageUp:
        goToPage(currentPage_ - 1);
        event->accept();
        return;
    case Qt::Key_Home:
        goToPage(0);
        event->accept();
        return;
    case Qt::Key_End:
        if (document_) {
            goToPage(document_->pageCount() - 1);
        }
        event->accept();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomIn();
        event->accept();
        return;
    case Qt::Key_Minus:
        zoomOut();
        event->accept();
        return;
    case Qt::Key_Up:
        scrollOffset_.ry() = std::max(0, scrollOffset_.y() - SCROLL_STEP);
        requestVisibleTiles();
        update();
        event->accept();
        return;
    case Qt::Key_Down:
        scrollOffset_.ry() = std::min(
            std::max(0, totalDocumentHeight_ - height()),
            scrollOffset_.y() + SCROLL_STEP);
        requestVisibleTiles();
        update();
        event->accept();
        return;
    case Qt::Key_Left:
        scrollOffset_.rx() = std::max(0, scrollOffset_.x() - SCROLL_STEP);
        requestVisibleTiles();
        update();
        event->accept();
        return;
    case Qt::Key_Right:
        scrollOffset_.rx() = std::min(
            std::max(0, totalDocumentWidth_ - width()),
            scrollOffset_.x() + SCROLL_STEP);
        requestVisibleTiles();
        update();
        event->accept();
        return;
    default:
        break;
    }

    QWidget::keyPressEvent(event);
}

void PdfCanvas::contextMenuEvent(QContextMenuEvent* event)
{
    // The text-edit tool consumes its own right-click via the
    // contextMenuRequested signal it emits from mousePressEvent
    // (MainWindow handles that path with the Atlas Mode submenu).
    // For the default canvas (no tool active, or annotation tool
    // active without its own context-menu handling), emit
    // canvasContextMenuRequested so MainWindow can show the
    // tool-aware default menu (UI-CTX-01).
    if (textEditTool_) {
        QWidget::contextMenuEvent(event);
        return;
    }

    emit canvasContextMenuRequested(event->globalPos());
    event->accept();
}

bool PdfCanvas::event(QEvent* event)
{
    // Intercept Tab/Shift+Tab so they reach keyPressEvent() instead of
    // being consumed by Qt's focus-chain navigation.
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            keyPressEvent(ke);
            return true;
        }
    }

    if (event->type() == QEvent::ScreenChangeInternal ||
        event->type() == QEvent::WindowChangeInternal) {
        updateDevicePixelRatio();
    }

    // Tooltip for annotation hover
    if (event->type() == QEvent::ToolTip && document_ && tooltipRequested) {
        auto* helpEvent = static_cast<QHelpEvent*>(event);
        QPoint pos = helpEvent->pos();

        int docY = scrollOffset_.y() + pos.y();
        int pageIdx = findPageAtY(docY);

        if (pageIdx >= 0 && pageIdx < document_->pageCount()) {
            const auto& layout = pageLayouts_[pageIdx];
            int pageLeft = (width() - layout.widthPx) / 2;
            int pageTop = layout.yOffset - scrollOffset_.y();

            auto pageInfo = document_->pageInfo(pageIdx);
            QPointF pageOrigin(pageLeft, pageTop);
            QPointF pdfPoint = pdfe::core::CoordinateTransform::screenToPdf(
                pos, zoomLevel_, pageOrigin, pageInfo.heightPt);

            QString tipText = tooltipRequested(pageIdx, pdfPoint);
            if (!tipText.isEmpty()) {
                QToolTip::showText(helpEvent->globalPos(), tipText, this);
            } else {
                QToolTip::hideText();
            }
        } else {
            QToolTip::hideText();
        }
        return true;
    }

    return QWidget::event(event);
}

void PdfCanvas::updateScrollBars()
{
    // Scrolling is handled via mouse drag, wheel, and keyboard navigation
}

void PdfCanvas::updateDocumentLayout()
{
    pageLayouts_.clear();
    totalDocumentHeight_ = 0;
    totalDocumentWidth_ = 0;

    if (!document_) {
        return;
    }

    int yOffset = PAGE_GAP;

    const bool transpose = (viewRotation_ == 90 || viewRotation_ == 270);

    for (int i = 0; i < document_->pageCount(); ++i) {
        auto pageInfo = document_->pageInfo(i);

        int wPx = static_cast<int>(pageInfo.widthPt * zoomLevel_);
        int hPx = static_cast<int>(pageInfo.heightPt * zoomLevel_);

        PageLayout layout;
        layout.pageIndex = i;
        layout.yOffset = yOffset;
        // Layout stores the ON-SCREEN (post-rotation) dimensions so scrolling
        // and centering work when viewRotation is 90 or 270.
        layout.widthPx = transpose ? hPx : wPx;
        layout.heightPx = transpose ? wPx : hPx;

        pageLayouts_.push_back(layout);

        yOffset += layout.heightPx + PAGE_GAP;
        totalDocumentWidth_ = std::max(totalDocumentWidth_, layout.widthPx);
    }

    totalDocumentHeight_ = yOffset;
}

void PdfCanvas::calculateVisibleRegion()
{
    // Visible region calculation is integrated into requestVisibleTiles() and paintEvent()
}

void PdfCanvas::requestVisibleTiles()
{
    if (!document_ || !tileCache_ || !tileRenderer_ || pageLayouts_.empty()) {
        return;
    }

    QRect viewportRect = rect();

    // Widget not yet laid out — skip until showEvent/resizeEvent provides valid geometry
    if (viewportRect.width() <= 0 || viewportRect.height() <= 0) {
        return;
    }

    for (const auto& layout : pageLayouts_) {
        int pageTop = layout.yOffset - scrollOffset_.y();
        int pageBottom = pageTop + layout.heightPx;

        // Skip pages not visible
        if (pageBottom < 0 || pageTop > viewportRect.height()) {
            continue;
        }

        // Tile grid is in unrotated page coordinates; layout.widthPx/heightPx
        // may be the transposed (rotated) dims, so swap back when requesting.
        const bool transpose = (viewRotation_ == 90 || viewRotation_ == 270);
        const int unrotatedWidth  = transpose ? layout.heightPx : layout.widthPx;
        const int unrotatedHeight = transpose ? layout.widthPx  : layout.heightPx;

        // R3-11 HiDPI: request tiles at DEVICE resolution. The tile grid is
        // measured in device pixels and the render zoom is
        // effectiveRenderZoom() = zoomLevel_ * devicePixelRatio_, so tiles are
        // rasterized sharp for HiDPI screens. TileId encodes the effective zoom
        // so the cache key is correct across BOTH zoom and DPR.
        const double dpr = static_cast<double>(devicePixelRatio_);
        const int deviceWidth  = static_cast<int>(std::lround(unrotatedWidth  * dpr));
        const int deviceHeight = static_cast<int>(std::lround(unrotatedHeight * dpr));
        const float renderZoom = effectiveRenderZoom();

        int cols = (deviceWidth  + pdfe::rendering::TILE_SIZE - 1) / pdfe::rendering::TILE_SIZE;
        int rows = (deviceHeight + pdfe::rendering::TILE_SIZE - 1) / pdfe::rendering::TILE_SIZE;

        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                // Visibility check is conservative when rotated: request all
                // tiles for visible pages. Works at tile-cache granularity.

                pdfe::rendering::TileId tileId{layout.pageIndex, col, row, renderZoom};

                // Only request if not already cached
                if (!tileCache_->contains(tileId)) {
                    const float dprSnapshot = devicePixelRatio_;
                    tileRenderer_->requestTile(tileId, [this, dprSnapshot](pdfe::rendering::TileId id, QImage image) {
                        // Stamp the device-pixel ratio so the tile (rasterized at
                        // device resolution) paints back at the correct LOGICAL
                        // size instead of being drawn oversized/upscaled.
                        image.setDevicePixelRatio(dprSnapshot);
                        tileCache_->put(id, image);
                    });
                }
            }
        }
    }
}

QPoint PdfCanvas::mapToDocument(const QPoint& viewportPos) const
{
    return viewportPos + scrollOffset_;
}

int PdfCanvas::findPageAtY(int docY) const
{
    // A layout entry can outlive a structural shrink: pageLayouts_ is not
    // rebuilt until refreshAllPages()/updateDocumentLayout() runs, so it may
    // still hold entries for pages the document no longer has. Returning such a
    // stale pageIndex would make callers' THROWING accessors (pageInfo()/page(),
    // which throw on out-of-range) escape their Qt event handler → the throw
    // propagates out of the event loop → std::terminate. Validate every result
    // against the LIVE document and return -1 for anything it no longer has, so
    // callers' `pageIdx >= 0` guards skip it (a no-op, not a crash).
    const int pageCount = document_ ? document_->pageCount() : 0;
    auto liveIndex = [pageCount](int idx) -> int {
        return (idx >= 0 && idx < pageCount) ? idx : -1;
    };

    for (size_t i = 0; i < pageLayouts_.size(); ++i) {
        const auto& layout = pageLayouts_[i];
        int pageBottom = layout.yOffset + layout.heightPx;

        if (docY >= layout.yOffset && docY < pageBottom) {
            return liveIndex(layout.pageIndex);
        }
    }

    // Return last page if beyond all pages (if it still exists).
    if (!pageLayouts_.empty() && docY >= pageLayouts_.back().yOffset) {
        return liveIndex(pageLayouts_.back().pageIndex);
    }

    return liveIndex(0);
}

void PdfCanvas::updateDevicePixelRatio()
{
    float newDpr = 1.0f;
    if (auto* w = window()) {
        if (auto* wh = w->windowHandle()) {
            if (auto* screen = wh->screen()) {
                newDpr = static_cast<float>(screen->devicePixelRatio());
            }
        }
    }

    if (std::abs(devicePixelRatio_ - newDpr) > 0.01f) {
        devicePixelRatio_ = newDpr;
        if (tileCache_) {
            tileCache_->invalidateAll();
        }
        // R3-11: the effective render zoom (zoom * dpr) changed, so the old
        // tiles are stale. Re-request at the new device resolution.
        requestVisibleTiles();
        update();
    }
}

void PdfCanvas::setDevicePixelRatioForTest(float dpr)
{
    // Headless TEST SEAM (R3-11): mirror updateDevicePixelRatio()'s
    // invalidate + re-request + repaint so a test can prove tiles are
    // requested at device resolution (zoom * dpr) without a real HiDPI screen.
    devicePixelRatio_ = (dpr > 0.0f) ? dpr : 1.0f;
    if (tileCache_) {
        tileCache_->invalidateAll();
    }
    requestVisibleTiles();
    update();
}

bool PdfCanvas::getPageScreenRect(int pageIndex, QPoint& pageOrigin, int& widthPx, int& heightPx) const
{
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pageLayouts_.size()))
        return false;
    const auto& layout = pageLayouts_[pageIndex];
    int pageLeft = (width() - layout.widthPx) / 2 - scrollOffset_.x();
    int pageTop = layout.yOffset - scrollOffset_.y();
    pageOrigin = QPoint(pageLeft, pageTop);
    widthPx = layout.widthPx;
    heightPx = layout.heightPx;
    return true;
}

void PdfCanvas::setViewRotation(int degrees)
{
    int normalized = ((degrees % 360) + 360) % 360;
    normalized = (normalized / 90) * 90;
    if (normalized != 0 && normalized != 90 && normalized != 180 && normalized != 270) {
        normalized = 0;
    }
    if (normalized == viewRotation_) {
        return;
    }
    viewRotation_ = normalized;
    updateDocumentLayout();
    update();
}

void PdfCanvas::rotateViewCW()
{
    setViewRotation(viewRotation_ + 90);
}

void PdfCanvas::rotateViewCCW()
{
    setViewRotation(viewRotation_ - 90);
}

void PdfCanvas::resetViewRotation()
{
    setViewRotation(0);
}

// ─── Pipeline a7c2e541 phase-17 test-automation hooks ────────────────────

QRectF PdfCanvas::pdfRectFromCanvas(int pageIndex, const QRectF& canvasRect) const
{
    if (!document_) return QRectF();
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pageLayouts_.size()))
        return QRectF();
    if (canvasRect.isNull() || !canvasRect.isValid())
        return QRectF();

    const auto& layout = pageLayouts_[pageIndex];
    int pageLeft = (width() - layout.widthPx) / 2;
    int pageTop = layout.yOffset - scrollOffset_.y();
    QPointF pageOrigin(pageLeft, pageTop);

    auto pageInfo = document_->pageInfo(pageIndex);
    float pageHeightPt = pageInfo.heightPt;

    return pdfe::core::CoordinateTransform::screenRectToPdf(
        canvasRect, zoomLevel_, pageOrigin, pageHeightPt);
}

QRectF PdfCanvas::canvasRectFromPdf(int pageIndex, const QRectF& pdfRect) const
{
    if (!document_) return QRectF();
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pageLayouts_.size()))
        return QRectF();
    if (pdfRect.isNull() || !pdfRect.isValid())
        return QRectF();

    const auto& layout = pageLayouts_[pageIndex];
    int pageLeft = (width() - layout.widthPx) / 2;
    int pageTop = layout.yOffset - scrollOffset_.y();
    QPointF pageOrigin(pageLeft, pageTop);

    auto pageInfo = document_->pageInfo(pageIndex);
    float pageHeightPt = pageInfo.heightPt;

    return pdfe::core::CoordinateTransform::pdfRectToScreen(
        pdfRect, zoomLevel_, pageOrigin, pageHeightPt);
}

bool PdfCanvas::addRedactionMarkAtPdfRect(int pageIndex, const QRectF& pdfRect)
{
    if (!document_) return false;
    if (pageIndex < 0 || pageIndex >= document_->pageCount())
        return false;
    if (pdfRect.isNull() || !pdfRect.isValid())
        return false;
    if (pdfRect.width() <= 0.0 || pdfRect.height() <= 0.0)
        return false;

    emit redactionMarkRequested(pageIndex, pdfRect);
    return true;
}

// ─── Pipeline b3f19d72 phase-1 test-automation hook ──────────────────────

bool PdfCanvas::clickByPdfCoord(int pageIndex, qreal pdfX, qreal pdfY)
{
    if (!document_) return false;
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pageLayouts_.size()))
        return false;

    // Reuse the existing canvasRectFromPdf translation so we honor the
    // same scroll / zoom / centering math as a real mouse event would.
    // A 1pt × 1pt rect at (pdfX, pdfY) collapses to the canvas point.
    QRectF pdfRect(pdfX, pdfY, 1.0, 1.0);
    QRectF canvasRect = canvasRectFromPdf(pageIndex, pdfRect);
    if (canvasRect.isNull() || !canvasRect.isValid())
        return false;

    QPointF canvasPos = canvasRect.center();

    // Synthesize press + release at the translated canvas point.
    // sendEvent flows through PdfCanvas::event() → mousePressEvent /
    // mouseReleaseEvent, so FormTool / AnnotationTool / link-hit-test
    // all see this exactly like a user click. globalPos derived from
    // mapToGlobal for completeness; tools that consume the event read
    // event->pos() / event->position(), not the global one.
    QPointF globalPos = mapToGlobal(canvasPos.toPoint());

    QMouseEvent pressEvt(QEvent::MouseButtonPress,
                         canvasPos, globalPos,
                         Qt::LeftButton, Qt::LeftButton,
                         Qt::NoModifier);
    QApplication::sendEvent(this, &pressEvt);

    QMouseEvent releaseEvt(QEvent::MouseButtonRelease,
                           canvasPos, globalPos,
                           Qt::LeftButton, Qt::NoButton,
                           Qt::NoModifier);
    QApplication::sendEvent(this, &releaseEvt);

    // After the synthetic click, explicitly
    // take Qt keyboard focus.  UIA set_edit_text() on the invoking pipe
    // widget moves QApplication::focusWidget() to that QLineEdit; without
    // this setFocus() call, a subsequent WM_KEYDOWN Tab posted to the
    // main window HWND is routed to the pipe widget instead of the canvas,
    // breaking FormTool's Tab navigation.
    setFocus(Qt::OtherFocusReason);

    return true;
}

bool PdfCanvas::doubleClickByPdfCoord(int pageIndex, qreal pdfX, qreal pdfY)
{
    if (!document_) return false;
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pageLayouts_.size()))
        return false;

    QRectF pdfRect(pdfX, pdfY, 1.0, 1.0);
    QRectF canvasRect = canvasRectFromPdf(pageIndex, pdfRect);
    if (canvasRect.isNull() || !canvasRect.isValid())
        return false;

    QPointF canvasPos = canvasRect.center();
    QPointF globalPos = mapToGlobal(canvasPos.toPoint());

    // A real double-click sequence in Qt: press, release, press,
    // MouseButtonDblClick, release.  The receiver's mouseDoubleClickEvent
    // is driven by the QEvent::MouseButtonDblClick.  Sending the leading
    // press/release first sets up TextEditTool's onMousePress (cursor /
    // page) so onMouseDoubleClick can select the word at the same point.
    const auto sendBtn = [this, &canvasPos, &globalPos](QEvent::Type t,
                                                        Qt::MouseButton btnState) {
        QMouseEvent ev(t, canvasPos, globalPos,
                       Qt::LeftButton, btnState, Qt::NoModifier);
        QApplication::sendEvent(this, &ev);
    };
    sendBtn(QEvent::MouseButtonPress,    Qt::LeftButton);
    sendBtn(QEvent::MouseButtonRelease,  Qt::NoButton);
    sendBtn(QEvent::MouseButtonDblClick, Qt::LeftButton);
    sendBtn(QEvent::MouseButtonRelease,  Qt::NoButton);

    setFocus(Qt::OtherFocusReason);
    return true;
}

bool PdfCanvas::typeIntoTextEdit(const QString& text)
{
    if (!textEditTool_ || text.isEmpty())
        return false;
    // Drive the SAME path keyPressEvent uses: build a per-character key
    // event carrying the text so TextEditTool::onKeyPress → onTextInput →
    // EditTextCommand fires exactly as a real keystroke would.
    for (const QChar& ch : text) {
        QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
        textEditTool_->onKeyPress(&ev);
    }
    update();
    return true;
}

bool PdfCanvas::sendKeyToTextEdit(int qtKey)
{
    if (!textEditTool_)
        return false;
    QKeyEvent ev(QEvent::KeyPress, qtKey, Qt::NoModifier, QString());
    textEditTool_->onKeyPress(&ev);
    update();
    return true;
}

// ─── Input-method (IME) support — WIN-49, follow-up phase-76 ───────────────

namespace {

/// The current tool as a FormTool, but ONLY while it is editing a text field.
/// Returns nullptr otherwise, so every caller below is automatically inert
/// when no field has an active edit.
pdfe::forms::FormTool* editingFormTool(pdfe::annotations::AnnotationTool* tool)
{
    auto* ft = dynamic_cast<pdfe::forms::FormTool*>(tool);
    return (ft && ft->isEditingField()) ? ft : nullptr;
}

} // namespace

void PdfCanvas::updateInputMethodEnabled()
{
    const bool on =
        (textEditTool_ && textEditTool_->isEditing()) ||
        editingFormTool(currentTool_.get()) != nullptr;

    if (testAttribute(Qt::WA_InputMethodEnabled) == on)
        return;

    setAttribute(Qt::WA_InputMethodEnabled, on);

    // Tell the platform the input context changed; without this the IME keeps
    // the state it cached when focus arrived and the attribute flip is
    // invisible to it.
    if (QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImEnabled);
}

void PdfCanvas::inputMethodEvent(QInputMethodEvent* event)
{
    const QString commit = event->commitString();
    const QString preedit = event->preeditString();

    // Qt carries the composition caret as an attribute, not as a field.
    int preeditCursor = static_cast<int>(preedit.size());
    for (const auto& attr : event->attributes()) {
        if (attr.type == QInputMethodEvent::Cursor)
            preeditCursor = attr.start;
    }

    bool handled = false;

    if (textEditTool_ && textEditTool_->isEditing()) {
        // Commit BEFORE setting the new preedit: one event can both finish a
        // composition and open the next.
        if (!commit.isEmpty() || event->replacementLength() > 0) {
            const auto ucs4 = commit.toUcs4();
            textEditTool_->commitPreedit(
                std::u32string(ucs4.begin(), ucs4.end()),
                event->replacementStart(), event->replacementLength());
        }
        const auto pre = preedit.toUcs4();
        textEditTool_->setPreeditText(
            std::u32string(pre.begin(), pre.end()), preeditCursor);
        handled = true;
    } else if (auto* ft = editingFormTool(currentTool_.get())) {
        if (!commit.isEmpty() || event->replacementLength() > 0) {
            ft->commitPreedit(commit, event->replacementStart(),
                              event->replacementLength());
        }
        ft->setPreeditText(preedit);
        handled = true;
    }

    if (!handled) {
        QWidget::inputMethodEvent(event);
        return;
    }

    update();
    event->accept();
}

QVariant PdfCanvas::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (textEditTool_ && textEditTool_->isEditing()) {
        switch (query) {
        case Qt::ImEnabled:
            return true;
        case Qt::ImCursorRectangle: {
            QRectF caret;
            if (textEditTool_->caretScreenRect(caret))
                return caret;
            break;  // never painted yet — fall through to the widget default
        }
        case Qt::ImSurroundingText: {
            const std::u32string s = textEditTool_->surroundingText();
            return QString::fromUcs4(
                reinterpret_cast<const char32_t*>(s.data()),
                static_cast<int>(s.size()));
        }
        case Qt::ImCursorPosition:
        case Qt::ImAnchorPosition:
            return textEditTool_->cursorPositionForIme();
        case Qt::ImHints:
            return static_cast<int>(Qt::ImhNone);
        default:
            break;
        }
    } else if (const auto* ft = editingFormTool(currentTool_.get())) {
        switch (query) {
        case Qt::ImEnabled:
            return true;
        case Qt::ImSurroundingText:
            return QString::fromStdString(ft->surroundingText());
        case Qt::ImCursorPosition:
        case Qt::ImAnchorPosition:
            return ft->cursorPositionForIme();
        case Qt::ImCursorRectangle: {
            // Answered from the caret FormTool actually painted. An earlier
            // draft declined this query, reasoning that FormTool does not
            // record its caret in screen space — but that was a property of the
            // implementation, not a constraint: FormTool does paint a caret,
            // so it can record the one it draws, exactly as TextEditTool does.
            // The phase-23 validator gate correctly refused the deferral.
            QRectF caret;
            if (ft->caretScreenRect(caret))
                return caret;
            break;  // nothing painted yet — fall back to the widget default
        }
        case Qt::ImHints:
            return static_cast<int>(Qt::ImhNone);
        default:
            break;
        }
    }

    return QWidget::inputMethodQuery(query);
}

bool PdfCanvas::sendInputMethodEventForTesting(const QString& preedit,
                                               const QString& commit,
                                               int replaceFrom,
                                               int replaceLength)
{
    const bool haveTarget =
        (textEditTool_ && textEditTool_->isEditing()) ||
        editingFormTool(currentTool_.get()) != nullptr;
    if (!haveTarget)
        return false;

    QList<QInputMethodEvent::Attribute> attrs;
    if (!preedit.isEmpty()) {
        attrs.append(QInputMethodEvent::Attribute(
            QInputMethodEvent::Cursor, preedit.size(), 1, QVariant()));
    }

    QInputMethodEvent ev(preedit, attrs);
    if (!commit.isEmpty() || replaceLength > 0) {
        ev.setCommitString(commit, replaceFrom, replaceLength);
    }
    // Through sendEvent, not a direct call: this must exercise the same
    // dispatch a real IME uses, including the WA_InputMethodEnabled gate.
    QApplication::sendEvent(this, &ev);
    return true;
}

bool PdfCanvas::dragByPdfCoord(int pageIndex,
                                qreal fromX, qreal fromY,
                                qreal toX, qreal toY)
{
    if (!document_) return false;
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pageLayouts_.size()))
        return false;

    QRectF fromPdfRect(fromX, fromY, 1.0, 1.0);
    QRectF fromCanvasRect = canvasRectFromPdf(pageIndex, fromPdfRect);
    if (fromCanvasRect.isNull() || !fromCanvasRect.isValid())
        return false;
    QPointF fromCanvasPos = fromCanvasRect.center();

    QRectF toPdfRect(toX, toY, 1.0, 1.0);
    QRectF toCanvasRect = canvasRectFromPdf(pageIndex, toPdfRect);
    if (toCanvasRect.isNull() || !toCanvasRect.isValid())
        return false;
    QPointF toCanvasPos = toCanvasRect.center();

    QPointF fromGlobal = mapToGlobal(fromCanvasPos.toPoint());
    QPointF toGlobal = mapToGlobal(toCanvasPos.toPoint());

    // Press at from
    QMouseEvent pressEvt(QEvent::MouseButtonPress,
                         fromCanvasPos, fromGlobal,
                         Qt::LeftButton, Qt::LeftButton,
                         Qt::NoModifier);
    QApplication::sendEvent(this, &pressEvt);

    // Intermediate moves so tools that count moves (rubber-band,
    // text selection) see a non-trivial drag path. 3 intermediates
    // is enough to look like a real drag without dragging things out.
    for (int i = 1; i <= 3; ++i) {
        const qreal t = i / 4.0;
        QPointF midPos = fromCanvasPos + (toCanvasPos - fromCanvasPos) * t;
        QPointF midGlobal = mapToGlobal(midPos.toPoint());
        QMouseEvent moveEvt(QEvent::MouseMove,
                            midPos, midGlobal,
                            Qt::NoButton, Qt::LeftButton,
                            Qt::NoModifier);
        QApplication::sendEvent(this, &moveEvt);
    }

    // Release at to
    QMouseEvent releaseEvt(QEvent::MouseButtonRelease,
                           toCanvasPos, toGlobal,
                           Qt::LeftButton, Qt::NoButton,
                           Qt::NoModifier);
    QApplication::sendEvent(this, &releaseEvt);

    setFocus(Qt::OtherFocusReason);
    return true;
}

QString PdfCanvas::selectedAnnotHandleCentroids() const
{
    // c82a1f37 P4: delegate to the active tool (SelectTool overrides;
    // all others return ""). Non-owning const access to the tool.
    if (currentTool_)
        return currentTool_->handleCentroidsDebug();
    return {};
}

void PdfCanvas::selectAnnotationInSelectTool(int pageIndex, int annotIndex)
{
    // c82a1f37 P4: drive the active tool's selection deterministically
    // (SelectTool overrides selectByIndex; all others no-op), then repaint
    // so the selection/handles render.
    if (!currentTool_) {
        update();
        return;
    }

    // BIND THE PAGE FIRST (GitHub #285 cluster C).
    //
    // Binding is what fires SelectTool::onPageChangedImpl(), which drops the
    // selection — correctly, because a selection names annotations by index on
    // a particular page and cannot survive the page changing (#269). Selecting
    // before binding therefore left the selection alive only until the FIRST
    // mouse event, which performed the deferred bind and wiped it.
    //
    // Traced on a failing handle-drag row:
    //     selectByIndex a=0 b=0
    //     bindToolToPage want=0 have=-1 hasPage=0 REBIND
    //     onPageChangedImpl inst=...760 droppedIdx=0
    //     PRESS selIdx a=-1
    // The press then found nothing selected, fell through to the empty-space
    // branch and started a rubber band instead of a resize — which is all five
    // of #285 cluster C's failures, one defect rather than five.
    //
    // Binding first makes the later bind a no-op (bindToolToPage short-circuits
    // on `toolPageIndex_ == pageIdx && toolPage_`), so the selection survives
    // to the press. Ordering is the whole fix; nothing else changes.
    bindToolToPage(pageIndex);
    currentTool_->selectByIndex(pageIndex, annotIndex);
    update();
}

bool PdfCanvas::tripleClickByPdfCoord(int pageIndex, qreal pdfX, qreal pdfY)
{
    // Three back-to-back press/release pairs at the same point.
    // Qt's QMouseEvent doesn't carry an explicit "this is the Nth
    // click" marker; the receiving widget infers click-count from
    // its own internal state machine (QApplication::doubleClickInterval()).
    // The events here are dispatched without intervening event-loop
    // pumps, so Qt sees them as a tight burst and the receiver
    // counts them as a triple-click sequence.
    for (int i = 0; i < 3; ++i) {
        if (!clickByPdfCoord(pageIndex, pdfX, pdfY))
            return false;
    }
    return true;
}

bool PdfCanvas::scrollByPixels(int pixelDelta)
{
    QPointF centre(width() / 2.0, height() / 2.0);
    QPointF globalCentre = mapToGlobal(centre.toPoint());

    // QWheelEvent in Qt6: position (local), globalPosition,
    // pixelDelta (per-pixel), angleDelta (1/8 degree steps;
    // 120 = one notch), buttons, modifiers, phase, inverted.
    QWheelEvent wheelEvt(
        centre, globalCentre,
        QPoint(0, pixelDelta),
        QPoint(0, pixelDelta * 120 / 15),  // approx convert
        Qt::NoButton, Qt::NoModifier,
        Qt::ScrollUpdate, false);
    QApplication::sendEvent(this, &wheelEvt);
    return true;
}

} // namespace pdfe::ui
