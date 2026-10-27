#include "PdfCanvasAccessible.h"
#include "PdfCanvas.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QAccessible>
#include <QFileInfo>
#include <QLatin1Char>

#include <algorithm>
#include <filesystem>

namespace pdfe::ui {

// ─── Shared "text interface over a fixed string" helpers ────────────────────
//
// PdfCanvasAccessible (whole-document text) and PdfPageAccessible (single-page
// text) both expose a fixed backing string through QAccessibleTextInterface.
// The boundary logic is identical, so it lives here as free functions.
namespace {

QString clampedRange(const QString& s, int start, int end) {
    const int n = s.length();
    if (start < 0) start = 0;
    if (end < 0 || end > n) end = n;
    if (start > end) return QString();
    return s.mid(start, end - start);
}

// Compute the [segStart, segEnd) span of the segment (of the given boundary
// type) that contains `offset`.
void segmentAt(const QString& s, int offset, QAccessible::TextBoundaryType type,
               int& segStart, int& segEnd) {
    const int n = s.length();
    if (offset < 0) offset = 0;
    if (offset > n) offset = n;

    switch (type) {
    case QAccessible::CharBoundary:
        segStart = offset;
        segEnd = std::min(offset + 1, n);
        return;

    case QAccessible::WordBoundary: {
        if (n == 0) { segStart = segEnd = 0; return; }
        auto isWs = [&](int k) { return k >= 0 && k < n && s.at(k).isSpace(); };
        int probe = std::min(offset, n - 1);
        if (isWs(probe)) {
            // On whitespace: return the single whitespace char.
            segStart = probe;
            segEnd = std::min(probe + 1, n);
            return;
        }
        segStart = probe;
        segEnd = probe;
        while (segStart > 0 && !isWs(segStart - 1)) --segStart;
        while (segEnd < n && !isWs(segEnd)) ++segEnd;
        return;
    }

    case QAccessible::LineBoundary:
    case QAccessible::ParagraphBoundary: {
        segStart = std::min(offset, n);
        segEnd = segStart;
        while (segStart > 0 && s.at(segStart - 1) != QLatin1Char('\n')) --segStart;
        while (segEnd < n && s.at(segEnd) != QLatin1Char('\n')) ++segEnd;
        return;
    }

    default: // SentenceBoundary, NoBoundary
        segStart = 0;
        segEnd = n;
        return;
    }
}

QString textAt(const QString& s, int offset, QAccessible::TextBoundaryType type,
               int* startOffset, int* endOffset) {
    const int n = s.length();
    if (offset < 0 || offset > n) {
        if (startOffset) *startOffset = -1;
        if (endOffset) *endOffset = -1;
        return QString();
    }
    int segStart = 0, segEnd = 0;
    segmentAt(s, offset, type, segStart, segEnd);
    if (startOffset) *startOffset = segStart;
    if (endOffset) *endOffset = segEnd;
    return clampedRange(s, segStart, segEnd);
}

QString textBefore(const QString& s, int offset, QAccessible::TextBoundaryType type,
                   int* startOffset, int* endOffset) {
    const int n = s.length();
    if (offset < 0 || offset > n) {
        if (startOffset) *startOffset = -1;
        if (endOffset) *endOffset = -1;
        return QString();
    }
    int curStart = 0, curEnd = 0;
    segmentAt(s, offset, type, curStart, curEnd);
    if (curStart <= 0) {
        if (startOffset) *startOffset = -1;
        if (endOffset) *endOffset = -1;
        return QString();
    }
    int prevStart = 0, prevEnd = 0;
    segmentAt(s, curStart - 1, type, prevStart, prevEnd);
    if (startOffset) *startOffset = prevStart;
    if (endOffset) *endOffset = prevEnd;
    return clampedRange(s, prevStart, prevEnd);
}

QString textAfter(const QString& s, int offset, QAccessible::TextBoundaryType type,
                  int* startOffset, int* endOffset) {
    const int n = s.length();
    if (offset < 0 || offset > n) {
        if (startOffset) *startOffset = -1;
        if (endOffset) *endOffset = -1;
        return QString();
    }
    int curStart = 0, curEnd = 0;
    segmentAt(s, offset, type, curStart, curEnd);
    if (curEnd >= n) {
        if (startOffset) *startOffset = -1;
        if (endOffset) *endOffset = -1;
        return QString();
    }
    int nextStart = 0, nextEnd = 0;
    segmentAt(s, curEnd, type, nextStart, nextEnd);
    if (startOffset) *startOffset = nextStart;
    if (endOffset) *endOffset = nextEnd;
    return clampedRange(s, nextStart, nextEnd);
}

// Extract a single page's plain text, guarding against any exception so an
// accessibility query (invoked from the Qt event loop) can never propagate a
// throw out of the handler.
//
// PERMISSION GATE. This closes an accessibility-tree bypass of the copy/extract
// permission: PdfPageAccessible is a QAccessibleTextInterface, so ANY process
// that walks the UIA / QAccessible tree (pywinauto, WinAppDriver, FlaUI — the
// very stack this project's own UI tests use) could read the full text of a
// copy-restricted encrypted PDF, straight past the copyAllowed() gates the
// exporters (Word/Excel/Image/PlainText) already enforce.
//
// Per ISO 32000-1 Table 22 two bits can authorise this read:
//   * bit 10 — extract for accessibility / assistive technology
//              (Document::extractForAccessibilityAllowed()); this is exactly
//              the purpose the accessibility tree serves, and
//   * bit  5 — general copy/extract (Document::copyAllowed()); if the content
//              may be copied wholesale it may certainly be read by an AT.
// Either one permits; only a document denying BOTH is refused, and it is
// refused by returning an empty string (never a throw — see above).
//
// Note this is a real discrimination, not a blanket off-switch: QPDF refuses to
// clear bit 10 for R > 3 (PDF 2.0 deprecates it — see
// PermissionBitsRoundTrip.AccessibilityBitIsNotExpressibleAboveR3), so every
// R4/R6 document still reaches assistive technology. Only an R3 document that
// explicitly denies accessibility extraction AND copy is silenced.
QString pageText(pdfe::core::Document* doc, int pageIndex) {
    if (!doc || pageIndex < 0 || pageIndex >= doc->pageCount()) {
        return QString();
    }
    try {
        if (!doc->extractForAccessibilityAllowed() && !doc->copyAllowed()) {
            return QString();
        }
        auto page = doc->page(pageIndex);
        if (!page) {
            return QString();
        }
        return QString::fromStdString(page->extractPlainText());
    } catch (...) {
        return QString();
    }
}

} // namespace

// ─── PdfPageAccessible ──────────────────────────────────────────────────────
//
// One accessibility child per document page. Not backed by a QObject: it is
// created and owned by the parent PdfCanvasAccessible, which deletes it. It is
// a QAccessibleTextInterface carrying that page's extracted plain text so the
// page content is reachable through the accessibility tree.
class PdfPageAccessible : public QAccessibleInterface,
                          public QAccessibleTextInterface {
public:
    PdfPageAccessible(QWidget* canvasWidget, int pageIndex)
        : canvasWidget_(canvasWidget), pageIndex_(pageIndex) {}

    ~PdfPageAccessible() override = default;

    // QAccessibleInterface
    bool isValid() const override {
        return canvas() != nullptr && document() != nullptr;
    }
    QObject* object() const override { return nullptr; }
    QWindow* window() const override { return nullptr; }

    QAccessibleInterface* parent() const override {
        return QAccessible::queryAccessibleInterface(canvasWidget_);
    }
    QAccessibleInterface* child(int /*index*/) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface* /*child*/) const override { return -1; }
    QAccessibleInterface* childAt(int /*x*/, int /*y*/) const override { return nullptr; }

    QAccessible::Role role() const override { return QAccessible::StaticText; }

    QAccessible::State state() const override {
        QAccessible::State s;
        s.readOnly = true;
        s.multiLine = true;
        return s;
    }

    QRect rect() const override {
        // Text node has no independent geometry; inherit the canvas rect so a
        // screen reader can still locate it on screen.
        QAccessibleInterface* p = parent();
        return p ? p->rect() : QRect();
    }

    QString text(QAccessible::Text t) const override {
        switch (t) {
        case QAccessible::Name:
            // Stable, unique accessible name for automation friendliness.
            return QStringLiteral("Page %1").arg(pageIndex_ + 1);
        case QAccessible::Value:
        case QAccessible::Description:
            return pageTextCached();
        default:
            return QString();
        }
    }
    void setText(QAccessible::Text /*t*/, const QString& /*text*/) override {}

    void* interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::TextInterface) {
            return static_cast<QAccessibleTextInterface*>(this);
        }
        return nullptr;
    }

    // QAccessibleTextInterface (over this page's text)
    void selection(int, int* startOffset, int* endOffset) const override {
        if (startOffset) *startOffset = 0;
        if (endOffset) *endOffset = 0;
    }
    int selectionCount() const override { return 0; }
    void addSelection(int, int) override {}
    void removeSelection(int) override {}
    void setSelection(int, int, int) override {}
    int cursorPosition() const override { return 0; }
    void setCursorPosition(int) override {}
    QString text(int startOffset, int endOffset) const override {
        return clampedRange(pageTextCached(), startOffset, endOffset);
    }
    QString textBeforeOffset(int offset, QAccessible::TextBoundaryType b,
                             int* so, int* eo) const override {
        return textBefore(pageTextCached(), offset, b, so, eo);
    }
    QString textAfterOffset(int offset, QAccessible::TextBoundaryType b,
                            int* so, int* eo) const override {
        return textAfter(pageTextCached(), offset, b, so, eo);
    }
    QString textAtOffset(int offset, QAccessible::TextBoundaryType b,
                         int* so, int* eo) const override {
        return textAt(pageTextCached(), offset, b, so, eo);
    }
    int characterCount() const override { return pageTextCached().length(); }
    QRect characterRect(int) const override { return QRect(); }
    int offsetAtPoint(const QPoint&) const override { return -1; }
    void scrollToSubstring(int, int) override {}
    QString attributes(int offset, int* startOffset, int* endOffset) const override {
        const int n = pageTextCached().length();
        if (startOffset) *startOffset = std::min(std::max(offset, 0), n);
        if (endOffset) *endOffset = std::min(std::max(offset, 0) + 1, n);
        return QString();
    }

private:
    PdfCanvas* canvas() const { return qobject_cast<PdfCanvas*>(canvasWidget_); }
    pdfe::core::Document* document() const {
        PdfCanvas* c = canvas();
        return c ? c->document() : nullptr;
    }
    const QString& pageTextCached() const {
        if (!textCached_) {
            cachedText_ = pageText(document(), pageIndex_);
            textCached_ = true;
        }
        return cachedText_;
    }

    QWidget* canvasWidget_ = nullptr;
    int pageIndex_ = 0;
    mutable bool textCached_ = false;
    mutable QString cachedText_;
};

// ─── Static action name definitions ─────────────────────────────────────────
const QString PdfCanvasAccessible::NextPageAction = QStringLiteral("Next Page");
const QString PdfCanvasAccessible::PreviousPageAction = QStringLiteral("Previous Page");
const QString PdfCanvasAccessible::ZoomInAction = QStringLiteral("Zoom In");
const QString PdfCanvasAccessible::ZoomOutAction = QStringLiteral("Zoom Out");

PdfCanvasAccessible::PdfCanvasAccessible(QWidget* widget)
    : QAccessibleWidget(widget, QAccessible::Canvas) {
}

PdfCanvasAccessible::~PdfCanvasAccessible() {
    // Do NOT delete the page children here. Once registered they are owned by
    // Qt's QAccessibleCache, which deletes every cached interface at
    // cleanupAccessibleCache() during QApplication teardown — and THIS destructor
    // is itself run from that same cleanup pass. Raw-deleting the children here
    // left them as dangling entries in the cache still being iterated, so the
    // next QAccessibleCache::deleteInterface() call dereferenced freed memory
    // (0xDD… vtable) → the shutdown use-after-free. We only drop our local
    // (non-owning) references; the cache frees the interfaces.
    pageChildren_.clear();
    pageChildIds_.clear();
}

PdfCanvas* PdfCanvasAccessible::canvas() const {
    return qobject_cast<PdfCanvas*>(widget());
}

QAccessible::Role PdfCanvasAccessible::role() const {
    return QAccessible::Canvas;
}

void PdfCanvasAccessible::rebuildChildrenIfNeeded() const {
    PdfCanvas* c = canvas();
    pdfe::core::Document* doc = c ? c->document() : nullptr;
    const int pageCount = doc ? doc->pageCount() : 0;

    if (static_cast<const void*>(doc) == cachedDoc_ && pageCount == cachedPageCount_) {
        return; // still current
    }

    // Document changed: retire the stale page children. This runs at RUNTIME
    // (the QAccessibleCache is alive, not being torn down), so removing each via
    // QAccessible::deleteAccessibleInterface(id) is the cache-safe way to both
    // drop the cache entry AND delete the interface — never a raw delete (which
    // would leave a dangling cache entry that faults at shutdown cleanup).
    for (QAccessible::Id id : pageChildIds_) {
        QAccessible::deleteAccessibleInterface(id);
    }
    pageChildren_.clear();
    pageChildIds_.clear();

    if (doc && c) {
        pageChildren_.reserve(static_cast<size_t>(pageCount));
        pageChildIds_.reserve(static_cast<size_t>(pageCount));
        for (int i = 0; i < pageCount; ++i) {
            auto* child = new PdfPageAccessible(c, i);
            // Hand ownership to Qt's cache. The a11y bridge reusing this
            // interface resolves to the SAME id, so there is exactly one cache
            // entry, and the cache frees it at teardown.
            pageChildIds_.push_back(QAccessible::registerAccessibleInterface(child));
            pageChildren_.push_back(child);
        }
    }

    cachedDoc_ = static_cast<const void*>(doc);
    cachedPageCount_ = pageCount;
}

int PdfCanvasAccessible::childCount() const {
    rebuildChildrenIfNeeded();
    return static_cast<int>(pageChildren_.size());
}

QAccessibleInterface* PdfCanvasAccessible::child(int index) const {
    rebuildChildrenIfNeeded();
    if (index < 0 || index >= static_cast<int>(pageChildren_.size())) {
        return nullptr;
    }
    return pageChildren_[static_cast<size_t>(index)];
}

int PdfCanvasAccessible::indexOfChild(const QAccessibleInterface* child) const {
    rebuildChildrenIfNeeded();
    for (size_t i = 0; i < pageChildren_.size(); ++i) {
        if (pageChildren_[i] == child) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QAccessibleInterface* PdfCanvasAccessible::childAt(int /*x*/, int /*y*/) const {
    // The canvas is a leaf for coordinate hit-testing: OS mouse events at any
    // point inside its bounding rect reach the widget itself (pywinauto's
    // click_input path). Per-page children are addressed via child(index),
    // not by pixel coordinates, so childAt returns nullptr.
    return nullptr;
}

void* PdfCanvasAccessible::interface_cast(QAccessible::InterfaceType type) {
    if (type == QAccessible::TextInterface) {
        return static_cast<QAccessibleTextInterface*>(this);
    }
    return QAccessibleWidget::interface_cast(type);
}

QString PdfCanvasAccessible::allPagesText() const {
    PdfCanvas* c = canvas();
    pdfe::core::Document* doc = c ? c->document() : nullptr;
    if (!doc) {
        return QString();
    }
    QStringList parts;
    const int pageCount = doc->pageCount();
    parts.reserve(pageCount);
    for (int i = 0; i < pageCount; ++i) {
        parts << pageText(doc, i);
    }
    return parts.join(QLatin1Char('\n'));
}

QString PdfCanvasAccessible::text(QAccessible::Text t) const {
    const PdfCanvas* canvasWidget = canvas();
    if (!canvasWidget) {
        return QAccessibleWidget::text(t);
    }

    switch (t) {
    case QAccessible::Name: {
        const auto* doc = canvasWidget->document();
        if (doc) {
            QString path = QString::fromStdString(doc->filePath().string());
            return QFileInfo(path).fileName();
        }
        return QStringLiteral("No document");
    }

    case QAccessible::Description:
    case QAccessible::Value: {
        const auto* doc = canvasWidget->document();
        if (doc) {
            int currentPageNum = canvasWidget->currentPage() + 1; // 1-based
            int totalPages = doc->pageCount();
            int zoomPercent = static_cast<int>(canvasWidget->zoom() * 100.0f);
            int rotation = canvasWidget->viewRotation();
            // Pipeline F phase-6: include view rotation so accessibility
            // tools (and the view_rotate_180 pywinauto test) can observe
            // the rotation state without polling status-bar messages.
            return QString("PDF viewer, page %1 of %2, zoom %3%, rotation %4 degrees")
                .arg(currentPageNum)
                .arg(totalPages)
                .arg(zoomPercent)
                .arg(rotation);
        }
        return QStringLiteral("PDF viewer, no document loaded");
    }

    default:
        return QAccessibleWidget::text(t);
    }
}

// ─── QAccessibleTextInterface (whole-document text) ─────────────────────────

void PdfCanvasAccessible::selection(int, int* startOffset, int* endOffset) const {
    if (startOffset) *startOffset = 0;
    if (endOffset) *endOffset = 0;
}

int PdfCanvasAccessible::selectionCount() const { return 0; }
void PdfCanvasAccessible::addSelection(int, int) {}
void PdfCanvasAccessible::removeSelection(int) {}
void PdfCanvasAccessible::setSelection(int, int, int) {}

int PdfCanvasAccessible::cursorPosition() const { return cursorPos_; }

void PdfCanvasAccessible::setCursorPosition(int position) {
    const int n = allPagesText().length();
    cursorPos_ = std::min(std::max(position, 0), n);
}

QString PdfCanvasAccessible::text(int startOffset, int endOffset) const {
    return clampedRange(allPagesText(), startOffset, endOffset);
}

QString PdfCanvasAccessible::textBeforeOffset(int offset, QAccessible::TextBoundaryType b,
                                              int* so, int* eo) const {
    return textBefore(allPagesText(), offset, b, so, eo);
}

QString PdfCanvasAccessible::textAfterOffset(int offset, QAccessible::TextBoundaryType b,
                                             int* so, int* eo) const {
    return textAfter(allPagesText(), offset, b, so, eo);
}

QString PdfCanvasAccessible::textAtOffset(int offset, QAccessible::TextBoundaryType b,
                                          int* so, int* eo) const {
    return textAt(allPagesText(), offset, b, so, eo);
}

int PdfCanvasAccessible::characterCount() const {
    return allPagesText().length();
}

QRect PdfCanvasAccessible::characterRect(int) const { return QRect(); }
int PdfCanvasAccessible::offsetAtPoint(const QPoint&) const { return -1; }
void PdfCanvasAccessible::scrollToSubstring(int, int) {}

QString PdfCanvasAccessible::attributes(int offset, int* startOffset, int* endOffset) const {
    const int n = allPagesText().length();
    if (startOffset) *startOffset = std::min(std::max(offset, 0), n);
    if (endOffset) *endOffset = std::min(std::max(offset, 0) + 1, n);
    return QString();
}

// ─── QAccessibleActionInterface ─────────────────────────────────────────────

QStringList PdfCanvasAccessible::actionNames() const {
    return {
        NextPageAction,
        PreviousPageAction,
        ZoomInAction,
        ZoomOutAction
    };
}

void PdfCanvasAccessible::doAction(const QString& actionName) {
    PdfCanvas* canvasWidget = canvas();
    if (!canvasWidget) {
        return;
    }

    if (actionName == NextPageAction) {
        canvasWidget->goToPage(canvasWidget->currentPage() + 1);
    } else if (actionName == PreviousPageAction) {
        canvasWidget->goToPage(canvasWidget->currentPage() - 1);
    } else if (actionName == ZoomInAction) {
        canvasWidget->setZoom(canvasWidget->zoom() * 1.25f);
    } else if (actionName == ZoomOutAction) {
        canvasWidget->setZoom(canvasWidget->zoom() / 1.25f);
    }
}

QString PdfCanvasAccessible::localizedActionName(const QString& actionName) const {
    if (actionName == NextPageAction) {
        return tr("Next Page");
    } else if (actionName == PreviousPageAction) {
        return tr("Previous Page");
    } else if (actionName == ZoomInAction) {
        return tr("Zoom In");
    } else if (actionName == ZoomOutAction) {
        return tr("Zoom Out");
    }

    return QAccessibleWidget::localizedActionName(actionName);
}

QAccessibleInterface* pdfCanvasAccessibleFactory(const QString& classname, QObject* object) {
    if (classname == QLatin1String("pdfe::ui::PdfCanvas") && object && object->isWidgetType()) {
        return new PdfCanvasAccessible(static_cast<QWidget*>(object));
    }
    return nullptr;
}

} // namespace pdfe::ui
