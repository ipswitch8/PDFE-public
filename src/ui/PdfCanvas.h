#pragma once

#include <QWidget>
#include <QPoint>
#include <cstdint>
#include <memory>
#include <QRectF>
#include <QVector>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>

#include "tre/TreTypes.h"

class QPainter;
class QContextMenuEvent;
class QInputMethodEvent;

namespace pdfe::core {
    class Document;
    class Page;
}

namespace pdfe::rendering {
    class TileCache;
    class TileRenderer;
}

namespace pdfe::annotations {
    class AnnotationTool;
    struct AnnotationData;
}

namespace pdfe::textedit {
    class TextEditTool;
}

namespace pdfe::image {
    class ImageTool;
}

namespace pdfe::ui {

class PdfCanvas : public QWidget {
    Q_OBJECT

public:
    explicit PdfCanvas(QWidget* parent = nullptr);
    ~PdfCanvas() override;

    // Document management
    void setDocument(pdfe::core::Document* doc,
                     pdfe::rendering::TileCache* cache,
                     pdfe::rendering::TileRenderer* renderer);
    void clearDocument();
    pdfe::core::Document* document() const { return document_; }

    // #50: JPEG2000 placeholder overlay. Returns the on-page rectangles (PDF
    // user-space, Y-up) of JPXDecode images that should be overlaid with the
    // "disabled for security" placeholder. Empty when JPX rendering is enabled
    // (nothing to hide) or the page has no JPX images. Static + Document-based so
    // it is unit-testable without a live widget.
    static QVector<QRectF> jpxPlaceholderRectsForPage(pdfe::core::Document* doc,
                                                      int pageIndex);

    // SEC-56 (#56): JBIG2 placeholder overlay — the same contract as the JPX one
    // above, for JBIG2Decode images. Empty when JBIG2 rendering is enabled or the
    // page has no JBIG2 images.
    static QVector<QRectF> jbig2PlaceholderRectsForPage(pdfe::core::Document* doc,
                                                        int pageIndex);

    // Zoom operations
    void setZoom(float level);
    void zoomIn();
    void zoomOut();
    void fitWidth();
    void fitPage();
    float zoom() const { return zoomLevel_; }

    // Page navigation
    void goToPage(int index);
    int currentPage() const { return currentPage_; }

    /// Vertical scroll offset in document pixels. Exposed so a UI test can
    /// assert that a scroll gesture ACTUALLY SCROLLED, instead of asserting
    /// only that the process survived it.
    int scrollOffsetY() const { return scrollOffset_.y(); }

    /// Horizontal scroll offset in document pixels. Same purpose as
    /// scrollOffsetY(): a horizontal-pan test can only assert it panned if it
    /// can see this.
    int scrollOffsetX() const { return scrollOffset_.x(); }

    /// Number of annotations in the current tool's multi-selection. Lets a
    /// rubber-band test assert it SELECTED something rather than only that
    /// the app survived the drag.
    int selectedAnnotationCount() const;
    int pageCount() const;

    /// Scroll the viewport so that ``pdfRect`` (in PDF user-space on
    /// ``pageIndex``) is centered in the visible area.  If the page is
    /// not yet laid out, falls back to ``goToPage``.  Used by the
    /// comment-panel double-click flow so navigation to an annotation
    /// produces a visible scroll even for same-page annotations.
    void scrollToPdfRect(int pageIndex, const QRectF& pdfRect);

    // Page refresh (invalidates cached tiles and re-requests rendering)
    void refreshPage(int pageIndex);
    void refreshAllPages();

    // Search highlights
    void setSearchHighlights(int pageIndex, const QVector<QRectF>& quads);
    void clearSearchHighlights();

    // OCR character bounding box overlay
    void setOcrBoxesVisible(bool visible);
    void setOcrBoxData(int pageIndex, std::vector<pdfe::tre::HocrLine> lines);
    void setOcrDpi(float dpi);
    void clearOcrBoxData();
    const std::vector<pdfe::tre::HocrLine>* ocrBoxDataForPage(int pageIndex) const;

    // Annotation tools
    void setCurrentTool(std::unique_ptr<pdfe::annotations::AnnotationTool> tool);
    void clearCurrentTool();
    pdfe::annotations::AnnotationTool* currentTool() const { return currentTool_.get(); }

    /// Release any raw Page* the active annotation tool has cached against the
    /// CURRENT document. Call this after a full-rewrite Save recreates the
    /// document's Page objects (MainWindow::onDocumentStructureChanged): the
    /// tool's cached page — e.g. SelectTool::page_ bound via selectByIndex — is
    /// now dangling, and a queued repaint would deref the freed page in
    /// onPaintOverlay -> getAnnotation(page_) -> Page::fzPage (0xC0000005).
    /// Unlike clearDocument()/setDocument(), the document stays open; this only
    /// drops the tool's stale page (and its transient selection).
    void releaseActiveToolPageCache();

    // Text edit tool (separate from annotation tools -- supports double-click, inline editing)
    void setTextEditTool(std::unique_ptr<pdfe::textedit::TextEditTool> tool);
    void clearTextEditTool();
    pdfe::textedit::TextEditTool* textEditTool() const { return textEditTool_.get(); }

    // Image tool (third dispatch path -- MainWindow owns, canvas stores raw pointer)
    void setImageTool(pdfe::image::ImageTool* tool);
    void clearImageTool();
    pdfe::image::ImageTool* imageTool() const { return imageTool_; }

    // Copy prevention
    void setCopyPrevented(bool prevented);
    bool isCopyPrevented() const { return copyPrevented_; }

    // View rotation (per-session, display-only; does NOT persist to the PDF).
    // Valid values are 0, 90, 180, 270. Rotation clears on clearDocument().
    void setViewRotation(int degrees);
    // Pipeline a7c4f891 phase-2 (D3): Q_INVOKABLE so the test pipe
    // handler can also reach it via QMetaObject::invokeMethod if
    // direct member access is unavailable in a test build context.
    Q_INVOKABLE int viewRotation() const { return viewRotation_; }
    void rotateViewCW();
    void rotateViewCCW();
    void resetViewRotation();

    // Tooltip callback: returns tooltip text for annotation at (pageIndex, pdfPoint)
    std::function<QString(int pageIndex, QPointF pdfPoint)> tooltipRequested;

    /// GitHub #135 — link follow callback. Returns true when a /Link
    /// annotation was found at (pageIndex, pdfPoint) AND dispatched.
    ///
    /// WHY THIS EXISTS RATHER THAN THE `canvasClicked` SIGNAL. The signal is
    /// emitted only on the "no tool is consuming the click" path at the
    /// bottom of mousePressEvent. Since a document open began installing the
    /// Select tool eagerly (MainWindow::openFile — "install the tool that the
    /// toolbar already SAYS is active"), `currentTool_` is never null while a
    /// document is open, so that path became unreachable and clicking a link
    /// stopped doing anything at all. A signal cannot report back whether the
    /// receiver handled the click, and the canvas must know: if a link WAS
    /// followed the press must not also be handed to the tool. Hence a
    /// std::function with a return value, the same shape as
    /// `tooltipRequested` above.
    std::function<bool(int pageIndex, QPointF pdfPoint)> linkFollowRequested;

    // HiDPI
    float devicePixelRatio() const;

    // R3-11: the effective zoom at which tiles are RASTERIZED. Tiles are
    // rendered at device resolution (logical zoom * devicePixelRatio) so a
    // HiDPI display shows sharp pages instead of Qt-upscaled logical tiles.
    // Hit-testing / overlay coordinate mapping stays in LOGICAL space and
    // uses zoomLevel_ directly (Qt mouse coords are DPR-independent), so this
    // multiplier is applied ONLY to render resolution, never to coordinates.
    float effectiveRenderZoom() const { return zoomLevel_ * devicePixelRatio_; }

    // Headless TEST SEAM (R3-11): force the device-pixel ratio without a real
    // HiDPI screen, driving the same invalidate + re-request + repaint path as
    // updateDevicePixelRatio() so tests can prove device-resolution rendering.
    // Not for production use.
    void setDevicePixelRatioForTest(float dpr);

    // Page screen rectangle helper
    bool getPageScreenRect(int pageIndex, QPoint& pageOrigin, int& widthPx, int& heightPx) const;

    // ─── Pipeline a7c2e541 phase-17 test-automation hooks ──────────────────
    //
    // Q_INVOKABLE accessors exposed via the meta-object system so that
    // pywinauto / WinAppDriver tests (and headless GoogleTest cases)
    // can drive coordinate translation + redaction-mark insertion
    // without going through the synthetic-input pipeline.
    //
    // Closes KNOWN_WEAK entries:
    //   - P11-INFRA-1 (canvas-drag → PDF coord translation)
    //   - P11-BUG-2  (single-Ctrl+Z removes the last redaction mark)
    //   - P15-INFRA-2 (high-volume drag stress lost ~70% of marks)

    /// Translate a canvas (widget-local) pixel rect to PDF user-space
    /// coordinates on ``pageIndex``. Returns an invalid (null) QRectF
    /// if ``pageIndex`` is out of range or the requested rect does
    /// not intersect the page area on screen. Coordinates are in
    /// PDF points; the y-axis is PDF-native (origin bottom-left).
    Q_INVOKABLE QRectF pdfRectFromCanvas(int pageIndex, const QRectF& canvasRect) const;

    /// Inverse of pdfRectFromCanvas: translate a PDF-space rect on
    /// ``pageIndex`` to canvas widget-local pixel coordinates.
    /// Returns a null QRectF if the page is not currently visible
    /// (e.g. outside the scrolled viewport).
    Q_INVOKABLE QRectF canvasRectFromPdf(int pageIndex, const QRectF& pdfRect) const;

    /// Programmatically request a redaction mark at the given PDF-space
    /// rectangle on ``pageIndex``. Emits ``redactionMarkRequested``,
    /// which MainWindow connects to its existing CreateRedactionCommand
    /// pipeline (same path as a user-driven drag, so per-mark undo
    /// behaviour is identical). Returns true if the request was
    /// dispatched (page index in range, rect non-empty), false on
    /// validation failure. Bypasses the synthetic-mouse drag path
    /// that Win32 input-queue coalesces under high-volume stress.
    Q_INVOKABLE bool addRedactionMarkAtPdfRect(int pageIndex, const QRectF& pdfRect);

    // ─── Pipeline b3f19d72 phase-1 test-automation hook ─────────────────────
    //
    // Synthesize a left-button mouse press+release at the canvas coord
    // that corresponds to ``(pdfX, pdfY)`` on ``pageIndex``. The events
    // are dispatched via ``QApplication::sendEvent`` so they flow
    // through the same FormTool / AnnotationTool / link-hit-test
    // dispatch pipeline a real user click would. Returns ``true`` on
    // successful dispatch (page index in range, page visible, coord
    // translates to a non-null canvas point), ``false`` on validation
    // failure. Lets pywinauto / WinAppDriver harnesses drive form
    // field activation, link follow, and annotation hit-test without
    // having to compute screen pixel coordinates themselves.
    Q_INVOKABLE bool clickByPdfCoord(int pageIndex, qreal pdfX, qreal pdfY);

    /// Pipeline f7c2a4e8 phase-3 (B2): synthesize a click-drag from
    /// (fromX, fromY) to (toX, toY) in PDF coordinates. Routes through
    /// the same Qt mousePressEvent/mouseMoveEvent/mouseReleaseEvent
    /// pipeline a real drag would, just with synthesized coordinates.
    /// Used by tests for annotation handle resize, rubber-band select,
    /// text selection drag, and similar drag-driven interactions
    /// where pyautogui's mouseDown+moveTo+mouseUp fails to register
    /// against Qt6 custom-painted widgets reliably. Returns true on
    /// success; false if pageIndex out of range, document not loaded,
    /// or canvasRectFromPdf maps either endpoint to a null QRectF.
    Q_INVOKABLE bool dragByPdfCoord(int pageIndex,
                                     qreal fromX, qreal fromY,
                                     qreal toX, qreal toY);

    /// Test seam (c82a1f37 P4): the active tool's selected-object handle
    /// centroids in PDF coords ("Name:x,y;..."), or empty when the tool is
    /// not a SelectTool or nothing is selected. Lets a UI test drag exactly
    /// onto a resize/rotate handle via dragByPdfCoord.
    QString selectedAnnotHandleCentroids() const;

    /// Test seam (c82a1f37 P4): select the annotation at (pageIndex,
    /// annotIndex) inside the active tool's own selection state (SelectTool
    /// only; no-op otherwise), so its handles become live for a subsequent
    /// dragByPdfCoord. Deterministic counterpart to a click-select.
    void selectAnnotationInSelectTool(int pageIndex, int annotIndex);

    /// Pipeline f7c2a4e8 phase-3 (B2): synthesize a triple-click at
    /// (pdfX, pdfY) on `pageIndex`. Dispatches three left-button
    /// press+release pairs back-to-back through Qt's event system,
    /// emulating Qt's interpretation of three consecutive clicks
    /// within the system double-click time. Used by tests that
    /// exercise TextSelector word/line/paragraph-selection
    /// behaviour. Returns true on success.
    Q_INVOKABLE bool tripleClickByPdfCoord(int pageIndex,
                                            qreal pdfX, qreal pdfY);

    /// Pipeline 7-loop: synthesize a double-click (press, release, then a
    /// MouseButtonDblClick) at the canvas point for (pdfX, pdfY).  Unlike
    /// tripleClickByPdfCoord (which sends only single press/release pairs),
    /// this dispatches an actual QEvent::MouseButtonDblClick so the
    /// receiver's mouseDoubleClickEvent fires — required to trigger
    /// TextEditTool::onMouseDoubleClick word-selection deterministically,
    /// independent of canvas letterbox margins.  Returns true on success.
    Q_INVOKABLE bool doubleClickByPdfCoord(int pageIndex,
                                            qreal pdfX, qreal pdfY);

    /// Pipeline 7-loop: deliver text input to the active text-edit tool via
    /// the same path keyPressEvent takes (TextEditTool::onTextInput →
    /// EditTextCommand), then repaint.  A test-only substitute for OS
    /// keyboard delivery to the custom-painted PdfCanvas widget, which the
    /// Win32/UIA harness cannot reliably target after a synthetic click
    /// (the canvas shares the top-level HWND and has no native focus).
    /// Returns true if a text-edit tool was active and consumed the text.
    Q_INVOKABLE bool typeIntoTextEdit(const QString& text);

    /// Pipeline 7-loop: deliver a single non-text key (by Qt::Key value) to
    /// the active text-edit tool, e.g. Backspace/Delete/Enter.  Returns
    /// true if a text-edit tool was active.
    Q_INVOKABLE bool sendKeyToTextEdit(int qtKey);

    /// Pipeline f7c2a4e8 phase-3 (B3): synthesize a vertical scroll-
    /// wheel event at the canvas centre. `pixelDelta` is in QWheelEvent
    /// pixelDelta units (positive = scroll up, negative = scroll down,
    /// like a physical wheel). Used by tests verifying scroll-wheel
    /// dispatch reaches the canvas. Returns true on success.
    Q_INVOKABLE bool scrollByPixels(int pixelDelta);

    /// Test seam (harden-findpage): thin public wrapper over the private
    /// findPageAtY() so a headless GoogleTest can prove the stale-layout
    /// guard. findPageAtY validates its resolved page index against the
    /// LIVE document (document_->pageCount()) and returns -1 for any page
    /// the document no longer has, so a structural shrink that leaves
    /// pageLayouts_ stale can never hand a caller an out-of-range index.
    /// Production behaviour is unchanged; this only exposes the result.
    Q_INVOKABLE int findPageAtDocY(int docY) const { return findPageAtY(docY); }

    // ─── Input-method (IME) support — WIN-49, follow-up phase-76 ────────────
    //
    // PdfCanvas is custom-painted and does its own text handling (TextEditTool
    // and FormTool each own their caret and buffer), so unlike every QLineEdit
    // in the dialogs it gets NOTHING from Qt for free: without the two
    // overrides below the platform never delivers a composition here at all,
    // and CJK/Korean/Vietnamese input into the document surface is impossible.

    /// Test seam for driving a real QInputMethodEvent without a real IME.
    /// Sends the event through QApplication::sendEvent to THIS widget, so it
    /// travels the production inputMethodEvent() path — a real IME cannot be
    /// scripted on CI (see docs/audit/meta-audit-2026-07/followup/phase-76/
    /// manual-qa-ime.md for the three reasons). Passing an empty
    /// @p commit sends a preedit-only event; a non-empty @p commit sends a
    /// commit with Qt's replaceFrom/replaceLength semantics.
    /// Returns true when a text-editing target was active to receive it.
    Q_INVOKABLE bool sendInputMethodEventForTesting(const QString& preedit,
                                                    const QString& commit,
                                                    int replaceFrom = 0,
                                                    int replaceLength = 0);

    /// Test seam: whether the widget currently advertises IME readiness.
    Q_INVOKABLE bool inputMethodEnabledForTesting() const {
        return testAttribute(Qt::WA_InputMethodEnabled);
    }

    /// Re-derive Qt::WA_InputMethodEnabled from the live editing state of the
    /// text-edit and form tools. Public so MainWindow can resync after it
    /// installs or swaps a tool outside the canvas's own event paths.
    void updateInputMethodEnabled();

    /// Public, matching QWidget: the platform calls this to learn the caret
    /// rectangle, surrounding text and cursor position of the active edit.
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

signals:
    void pageChanged(int pageIndex);
    void zoomChanged(float zoom);
    void annotationCreated(int pageIndex, pdfe::annotations::AnnotationData data);
    void scrolled();

    /// Emitted when the user clicks on a region of the canvas with no
    /// tool active. Lets MainWindow hit-test link annotations and
    /// dispatch the action (URI / GoTo / Named / Launch). pdfPoint
    /// is the PDF-space coordinate under the click on `pageIndex`.
    void canvasClicked(int pageIndex, QPointF pdfPoint);

    /// Emitted on right-click anywhere on the canvas when no tool is
    /// already showing its own context menu. MainWindow listens to
    /// this to show the tool-aware default context menu (Zoom In /
    /// Zoom Out / Fit Page / Single Page / Continuous etc.).
    /// `screenPos` is the global screen position of the right-click,
    /// suitable for QMenu::exec(). (UI-CTX-01)
    void canvasContextMenuRequested(QPoint screenPos);

    /// Pipeline a7c2e541 phase-17: emitted by the Q_INVOKABLE
    /// ``addRedactionMarkAtPdfRect`` test hook. MainWindow connects
    /// this to its existing redaction-pipeline so the same
    /// CreateRedactionCommand path is used as for a user drag.
    void redactionMarkRequested(int pageIndex, QRectF pdfRect);

    /// Defect #4 (per-region tool routing): emitted when the text-edit tool is
    /// active but the user clicks a location with NO live-font text span that
    /// lies over a scanned image block. MainWindow switches to the region/TRE
    /// tool so the scanned block can be edited — without the page-wide
    /// hasTextContent() decision forcing the whole mixed-content page onto one
    /// tool. pdfPoint is the PDF-space coordinate under the click.
    void regionToolRequested(int pageIndex, QPointF pdfPoint);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;

private:
    // Document layout cache (declared early — used by method signatures below)
    struct PageLayout {
        int pageIndex;
        int yOffset;      // Y position in document coordinates
        int widthPx;      // Scaled width in pixels
        int heightPx;     // Scaled height in pixels
    };

    void updateScrollBars();
    void updateDocumentLayout();
    void calculateVisibleRegion();
    void requestVisibleTiles();
    QPoint mapToDocument(const QPoint& viewportPos) const;
    int findPageAtY(int docY) const;
    void updateDevicePixelRatio();
    void paintSearchHighlights(QPainter& painter, const PageLayout& layout, int pageLeft, int pageTop);
    void paintOcrBoxes(QPainter& painter, const PageLayout& layout, int pageLeft, int pageTop);
    void paintJpxPlaceholders(QPainter& painter, const PageLayout& layout, int pageLeft, int pageTop);
    void paintJbig2Placeholders(QPainter& painter, const PageLayout& layout, int pageLeft, int pageTop);

    // Document and rendering
    pdfe::core::Document* document_ = nullptr;

    // ISSUE #125. Token from Document::addPageDirtyListener() for the CURRENT
    // document_, or 0 when not subscribed. Typed as the underlying integer
    // rather than Document::PageDirtyToken so this header can keep its forward
    // declaration of Document instead of pulling in the whole of Document.h.
    //
    // MUST be released before document_ is repointed or this canvas is
    // destroyed, or Document fires into a dead subscriber. subscribePageDirty()
    // / unsubscribePageDirty() are the only two places that touch it.
    std::uint64_t pageDirtyToken_ = 0;

    // Weak view of the CURRENT document_'s lifetime. document_ is a raw
    // pointer and the Document can die first (MainWindow tears down in that
    // order; test fixtures call setDocument(nullptr) in TearDown after the
    // document local is gone). Expired here means document_ is dangling and
    // must not be dereferenced to unsubscribe.
    std::weak_ptr<const void> documentAlive_;

    void subscribePageDirty();
    void unsubscribePageDirty();

private slots:
    /// Invalidate one page's tiles in response to a Document page-dirty
    /// notification. A slot so the notification can be marshalled onto this
    /// widget's thread rather than run on whichever thread made the edit.
    void onPageContentChanged(int pageIndex);

private:
    pdfe::rendering::TileCache* tileCache_ = nullptr;
    pdfe::rendering::TileRenderer* tileRenderer_ = nullptr;

    // View state
    int currentPage_ = 0;
    float zoomLevel_ = 1.0f;
    QPoint scrollOffset_{0, 0};

    std::vector<PageLayout> pageLayouts_;
    int totalDocumentHeight_ = 0;
    int totalDocumentWidth_ = 0;

    // Search highlights: page index → list of quads in PDF coordinates
    std::unordered_map<int, QVector<QRectF>> searchHighlights_;

    // OCR character bounding box overlay
    bool ocrBoxesVisible_ = false;
    float ocrDpi_ = 300.0f;
    std::unordered_map<int, std::vector<pdfe::tre::HocrLine>> ocrBoxData_;

    // #50: cached JPXDecode image rects per page (PDF user space, Y-up), computed
    // lazily on first paint. Pref-independent (the enabled check gates drawing, not
    // the cache); cleared when the document changes.
    std::unordered_map<int, QVector<QRectF>> jpxRegions_;

    // SEC-56: cached JBIG2Decode image rects per page (PDF user space, Y-up),
    // computed lazily on first paint, same lifetime rules as jpxRegions_ above.
    std::unordered_map<int, QVector<QRectF>> jbig2Regions_;

    // HiDPI support
    float devicePixelRatio_ = 1.0f;

    // Mouse interaction
    bool isDragging_ = false;
    QPoint lastMousePos_;

    /// GitHub #119 — middle-button drag-to-pan.
    ///
    /// Distinct from isDragging_ (the LEFT-button pan fallback, which only
    /// runs when NO tool consumed the press and only moves the Y axis). This
    /// one runs regardless of which tool is installed and moves both axes.
    /// Kept as its own flag so a middle-drag in progress cannot be ended by a
    /// left release, and vice versa.
    bool isMiddlePanning_ = false;
    QPoint lastMiddlePanPos_;
    Qt::CursorShape cursorBeforeMiddlePan_ = Qt::ArrowCursor;

    // Annotation tool
    std::unique_ptr<pdfe::annotations::AnnotationTool> currentTool_;

    /// GitHub #269. The Page the active tool is bound to, OWNED here.
    ///
    /// The tool holds a non-owning raw Page* and needs it to outlive the
    /// handler -- InkTool keeps it from press to release -- so someone has to
    /// own it for that long, and the canvas is the only participant that knows
    /// both the clicked page index and the document to resolve it from.
    ///
    /// Released in clearDocument() and in setDocument()'s document-change
    /// branch, in both cases AFTER currentTool_->onDocumentClosing() has nulled
    /// the tool's own pointer. That order is the safety argument; reversed it
    /// is GitHub #235.
    std::unique_ptr<pdfe::core::Page> toolPage_;

    /// Which page index `toolPage_` is, or -1 when nothing is bound. Compared
    /// against the event's pageIndex so a click on the already-bound page costs
    /// nothing.
    int toolPageIndex_ = -1;

    /// Bind `currentTool_` to `pageIdx`, resolving and taking ownership of that
    /// Page if it is not already the bound one. Returns false when there is no
    /// tool, no document, or the page will not resolve -- in which case the
    /// caller must not dispatch, because the tool would act on a stale page.
    bool bindToolToPage(int pageIdx);

    /// Drop the binding. Called when the tool changes and when the document
    /// goes away; in the latter case only AFTER the tool has been told.
    void releaseToolPage();

    // Text edit tool (parallel dispatch path)
    std::unique_ptr<pdfe::textedit::TextEditTool> textEditTool_;

    // Image tool (third dispatch path, non-owning pointer)
    pdfe::image::ImageTool* imageTool_ = nullptr;

    // Copy prevention
    bool copyPrevented_ = false;

    // Per-session view rotation (0, 90, 180, 270). Display-only; NOT persisted.
    int viewRotation_ = 0;

    // Constants
    static constexpr int PAGE_GAP = 10;  // Gap between pages in pixels
    static constexpr float MIN_ZOOM = 0.1f;
    static constexpr float MAX_ZOOM = 10.0f;
    static constexpr float ZOOM_STEP = 1.25f;
};

} // namespace pdfe::ui
