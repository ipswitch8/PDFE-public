#include "FormTool.h"

#include "core/Utf8.h"

#include <cstddef>
#include "FormManager.h"
#include "FormCommands.h"
#include "JavaScriptEngine.h"
#include "core/Page.h"
#include "core/UndoManager.h"

#include <QKeyEvent>
#include <QPainter>
#include <QMouseEvent>
#include <algorithm>
#include <exception>

namespace pdfe::forms {

// GitHub #243 — live-instance census. See FormTool::liveInstanceCountForTesting().
int FormTool::liveInstanceCount_ = 0;
int FormTool::setCreationFieldTypeCalls_ = 0;

FormTool::FormTool(FormManager* manager,
                   JavaScriptEngine* jsEngine,
                   pdfe::commands::UndoManager* undoManager)
    : manager_(manager)
    , jsEngine_(jsEngine)
    , undoManager_(undoManager)
{
    ++liveInstanceCount_;
}

FormTool::~FormTool()
{
    --liveInstanceCount_;
}

pdfe::annotations::AnnotationType FormTool::toolType() const {
    return pdfe::annotations::AnnotationType::Unknown;
}

void FormTool::activate() {
    selectedWidget_ = -1;
    isEditing_ = false;
    editBuffer_.clear();
    cursorPosition_ = 0;
    // Don't clear tabOrder_ here — setPage() populates it before activate()
    // is called by PdfCanvas::setCurrentTool().
    tabOrderPosition_ = -1;
    isDrawingRect_ = false;
    drawPageIndex_ = -1;
}

void FormTool::deactivate() {
    if (isEditing_) {
        commitEdit();
    }
    deselectField();
}

// GitHub #269. Was FormTool::setPage; split across the base class's two hooks
// so the assignment to page_ itself lives in AnnotationTool::setPage and cannot
// be skipped. The split is exactly where it was: commitEdit()/deselectField()
// concern the page being LEFT and must run before the pointer moves; tab order
// is derived FROM the new page and must run after.
void FormTool::onPageChangingImpl() {
    if (isEditing_) {
        commitEdit();
    }
    deselectField();
}

void FormTool::onPageChangedImpl() {
    if (page_) {
        tabOrder_ = manager_->getTabOrder(page_);
    } else {
        tabOrder_.clear();
    }
    tabOrderPosition_ = -1;
}

void FormTool::onDocumentClosingImpl() {
    // GitHub #235. Release the cached raw Page* BEFORE its document frees the
    // pages, so a repaint queued after the close cannot run page_->index() on
    // freed memory.
    //
    // DELIBERATELY NOT setPage(nullptr), and deliberately not commitEdit() or
    // cancelEdit(). All three touch page_ on the way past -- setPage() commits
    // an in-flight edit, and cancelEdit() fires a JS blur event through
    // `jsEngine_->fireBlurEvent(page_, ...)`. The page is being destroyed; the
    // one thing we must not do on the way out is dereference it. An edit in
    // flight when the document closes is discarded, which is the same thing
    // that happens to every other unsaved change at that moment.
    selectedWidget_ = -1;
    isEditing_ = false;
    editBuffer_.clear();
    preeditText_.clear();
    cursorPosition_ = 0;
    caretScreenRectValid_ = false;
    tabOrder_.clear();
    tabOrderPosition_ = -1;
}

void FormTool::onAnnotationsChanged() {
    // The document is still open and page_ is still valid -- only the field set
    // moved, so a cached widget index may now name a different field or none.
    // Keep page_; drop the index and any edit bound to it.
    selectedWidget_ = -1;
    isEditing_ = false;
    editBuffer_.clear();
    preeditText_.clear();
    cursorPosition_ = 0;
    caretScreenRectValid_ = false;
    if (page_ && manager_) {
        // Guarded: this runs from PdfCanvas::onPageContentChanged, i.e. inside
        // Qt event delivery, where an escaping exception is as fatal as one
        // from paintEvent. An empty tab order is a degraded but survivable
        // state; a terminated process is not.
        try {
            tabOrder_ = manager_->getTabOrder(page_);
        } catch (const std::exception&) {
            tabOrder_.clear();
        }
    }
    tabOrderPosition_ = -1;
}

void FormTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) {
    (void)event;

    if (!page_) return;

    // Creation mode: start drawing new field
    if (isCreationMode()) {
        drawStart_ = pdfPoint;
        drawEnd_ = pdfPoint;
        drawPageIndex_ = pageIndex;
        isDrawingRect_ = true;
        return;
    }

    // Interaction mode: hit test for existing fields
    auto hitResult = manager_->getFormFieldAt(page_, pdfPoint);

    if (hitResult.has_value()) {
        int widgetIdx = hitResult.value();

        if (widgetIdx != selectedWidget_) {
            // Commit any in-progress edit before switching
            if (isEditing_) {
                commitEdit();
            }

            auto fieldData = manager_->getFormField(page_, widgetIdx);

            if (fieldData.isReadOnly) {
                // Just select, don't edit
                selectField(widgetIdx);
                return;
            }

            switch (fieldData.type) {
                case FormFieldType::Checkbox:
                case FormFieldType::RadioButton: {
                    // Toggle immediately via command
                    auto cmd = std::make_unique<ToggleFieldCommand>(
                        manager_, page_, widgetIdx);
                    undoManager_->execute(std::move(cmd));
                    selectField(widgetIdx);
                    if (fieldValueChanged) {
                        auto updated = manager_->getFormField(page_, widgetIdx);
                        fieldValueChanged(page_->index(), updated);
                    }
                    break;
                }

                case FormFieldType::Text:
                    selectField(widgetIdx);
                    startEditing(widgetIdx);
                    break;

                case FormFieldType::ComboBox:
                case FormFieldType::ListBox:
                    selectField(widgetIdx);
                    // ComboBox/ListBox popup handled by MainWindow or FormFieldPropertiesPanel
                    break;

                case FormFieldType::PushButton:
                    selectField(widgetIdx);
                    // Fire JS button action if JS is enabled
                    if (jsEngine_ && jsEngine_->isEnabled()) {
                        jsEngine_->fireFocusEvent(page_, widgetIdx);
                        jsEngine_->fireBlurEvent(page_, widgetIdx);
                    }
                    break;

                default:
                    selectField(widgetIdx);
                    break;
            }
        }
    } else {
        // Clicked outside any field
        if (isEditing_) {
            commitEdit();
        }
        deselectField();
    }
}

void FormTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) {
    (void)event;

    // Update rubber-band rectangle during drawing
    if (isDrawingRect_ && pageIndex == drawPageIndex_) {
        drawEnd_ = pdfPoint;
    }
}

void FormTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) {
    (void)event;
    (void)pdfPoint;

    // Finalize field creation
    if (isDrawingRect_) {
        isDrawingRect_ = false;

        // Build normalized rectangle
        qreal left = std::min(drawStart_.x(), drawEnd_.x());
        qreal top = std::min(drawStart_.y(), drawEnd_.y());
        qreal right = std::max(drawStart_.x(), drawEnd_.x());
        qreal bottom = std::max(drawStart_.y(), drawEnd_.y());
        QRectF rect(left, top, right - left, bottom - top);

        // Only create if rectangle is large enough (minimum size)
        if (rect.width() >= 10.0 && rect.height() >= 10.0) {
            if (fieldCreated) {
                fieldCreated(drawPageIndex_, creationFieldType_, rect);
            }
        }
    }
}

void FormTool::onKeyPress(QKeyEvent* event) {
    if (!page_) return;

    if (event->key() == Qt::Key_Tab) {
        if (isEditing_) {
            commitEdit();
        }
        if (event->modifiers() & Qt::ShiftModifier) {
            tabToPreviousField();
        } else {
            tabToNextField();
        }
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        if (isEditing_) {
            cancelEdit();
        } else {
            deselectField();
        }
        return;
    }

    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (isEditing_) {
            auto fieldData = manager_->getFormField(page_, selectedWidget_);
            if (fieldData.isMultiline) {
                handleTextInput("\n");
            } else {
                commitEdit();
            }
        }
        return;
    }

    // Text input
    if (isEditing_) {
        // CODEPOINT BOUNDARIES, NOT BYTES (WIN-49 prerequisite).
        //
        // cursorPosition_ remains a BYTE offset -- see src/core/Utf8.h for why
        // -- but every movement and erase below steps a whole codepoint. The
        // previous code erased and stepped exactly ONE BYTE, so a single
        // Backspace over any non-ASCII character (every CJK glyph, every
        // accented Latin letter) split its multi-byte sequence and left
        // INVALID UTF-8 in editBuffer_, which commitEdit() then writes into
        // the PDF field value.
        if (event->key() == Qt::Key_Backspace) {
            cursorPosition_ = static_cast<int>(pdfe::core::utf8::erasePrev(
                editBuffer_, static_cast<std::size_t>(cursorPosition_)));
        } else if (event->key() == Qt::Key_Delete) {
            pdfe::core::utf8::eraseAt(editBuffer_,
                                      static_cast<std::size_t>(cursorPosition_));
        } else if (event->key() == Qt::Key_Left) {
            cursorPosition_ = static_cast<int>(pdfe::core::utf8::prevBoundary(
                editBuffer_, static_cast<std::size_t>(cursorPosition_)));
        } else if (event->key() == Qt::Key_Right) {
            cursorPosition_ = static_cast<int>(pdfe::core::utf8::nextBoundary(
                editBuffer_, static_cast<std::size_t>(cursorPosition_)));
        } else if (event->key() == Qt::Key_Home) {
            cursorPosition_ = 0;
        } else if (event->key() == Qt::Key_End) {
            cursorPosition_ = static_cast<int>(editBuffer_.size());
        } else {
            QString text = event->text();
            if (!text.isEmpty() && text[0].isPrint()) {
                handleTextInput(text);
            }
        }
    }
}

void FormTool::onPaintOverlay(QPainter& painter, int pageIndex,
                               float zoom, QPointF pageOrigin, float pageHeightPt) {
    (void)pageHeightPt;

    // Draw rubber-band rectangle during field creation
    if (isDrawingRect_ && pageIndex == drawPageIndex_) {
        painter.save();

        // Set pen for rubber-band effect
        QPen pen(QColor(0, 120, 215));
        pen.setWidthF(2.0);
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(QColor(0, 120, 215, 30));

        // Build normalized rectangle
        qreal left = std::min(drawStart_.x(), drawEnd_.x());
        qreal top = std::min(drawStart_.y(), drawEnd_.y());
        qreal right = std::max(drawStart_.x(), drawEnd_.x());
        qreal bottom = std::max(drawStart_.y(), drawEnd_.y());

        // Convert to screen coordinates
        float screenLeft = static_cast<float>(pageOrigin.x()) + static_cast<float>(left) * zoom;
        float screenTop = static_cast<float>(pageOrigin.y()) + static_cast<float>(top) * zoom;
        float screenWidth = static_cast<float>(right - left) * zoom;
        float screenHeight = static_cast<float>(bottom - top) * zoom;

        painter.drawRect(QRectF(screenLeft, screenTop, screenWidth, screenHeight));
        painter.restore();
        return;
    }

    // Draw selection highlight for existing fields
    if (!page_ || selectedWidget_ < 0) return;
    if (page_->index() != pageIndex) return;

    // GitHub #234's sibling, in this tool. getFormField() goes through
    // MuPdfContext::execute(), which converts ANY fz error into a C++
    // MuPdfException -- and a widget index that no longer resolves is exactly
    // such an error. We are inside QWidget::paintEvent, where an escaping
    // exception terminates the process.
    //
    // Note this is a DIFFERENT failure from the access violation that #235 is
    // about. That one was `page_->index()` above dereferencing a freed page and
    // is fixed by onDocumentClosing() releasing the pointer; no try/catch can
    // help there, because an access violation is not a C++ exception. This
    // guard covers the throwing half of the same paint path.
    FormFieldData fieldData;
    try {
        fieldData = manager_->getFormField(page_, selectedWidget_);
    } catch (const std::exception&) {
        // Stale widget index: drop it rather than re-throwing on every repaint.
        selectedWidget_ = -1;
        isEditing_ = false;
        caretScreenRectValid_ = false;
        return;
    }

    // Convert fitz-space rect to screen coordinates (both Y-down, no flip needed)
    QRectF pdfRect = fieldData.rect;
    float x = static_cast<float>(pageOrigin.x()) + static_cast<float>(pdfRect.x()) * zoom;
    float y = static_cast<float>(pageOrigin.y()) + static_cast<float>(pdfRect.y()) * zoom;
    float w = static_cast<float>(pdfRect.width()) * zoom;
    float h = static_cast<float>(pdfRect.height()) * zoom;

    QRectF screenRect(x, y, w, h);

    // Draw selection highlight
    painter.save();
    painter.setPen(QPen(QColor(0, 120, 215), 2));
    painter.setBrush(QColor(0, 120, 215, 30));
    painter.drawRect(screenRect);

    // Draw cursor for text editing
    if (isEditing_ && fieldData.type == FormFieldType::Text) {
        // Simple cursor position approximation, in CODEPOINTS not bytes.
        // Using byte counts here put the caret several characters to the right
        // of the text in any non-ASCII field -- a 5-character CJK value is 15
        // bytes, so the caret rendered as if the field held 15 characters.
        // Still an approximation (it assumes uniform advance width and does not
        // shape the text), but it is now wrong by the same amount for every
        // script instead of three times worse for some of them.
        const int cpTotal =
            static_cast<int>(pdfe::core::utf8::codepointCount(editBuffer_));
        const int cpBeforeCursor = static_cast<int>(pdfe::core::utf8::codepointCount(
            editBuffer_.substr(0, pdfe::core::utf8::alignLeft(
                editBuffer_, static_cast<std::size_t>(cursorPosition_)))));
        float charWidth = w / std::max(1, cpTotal + 1);
        float cursorX = x + 2 + cpBeforeCursor * charWidth;
        painter.setPen(QPen(Qt::black, 1));
        painter.drawLine(QPointF(cursorX, y + 2), QPointF(cursorX, y + h - 2));

        // Record the caret we just DREW, for Qt::ImCursorRectangle (phase-76,
        // criterion 1). Recording the drawn position rather than deriving a
        // second one matters: the drawn caret is an approximation, so any
        // independently-computed rectangle would disagree with what the user
        // sees and put the IME candidate window somewhere the caret is not.
        // Width 1 so a platform reading an empty rect as "no position" does
        // not discard it.
        caretScreenRect_ = QRectF(QPointF(cursorX, y + 2),
                                  QPointF(cursorX + 1.0, y + h - 2));
        caretScreenRectValid_ = true;
    }

    painter.restore();
}

void FormTool::selectField(int widgetIndex) {
    if (selectedWidget_ != widgetIndex) {
        selectedWidget_ = widgetIndex;

        // Update tab order position
        auto it = std::find(tabOrder_.begin(), tabOrder_.end(), widgetIndex);
        if (it != tabOrder_.end()) {
            tabOrderPosition_ = static_cast<int>(std::distance(tabOrder_.begin(), it));
        }

        if (fieldSelected) {
            fieldSelected(widgetIndex);
        }
    }
}

void FormTool::deselectField() {
    if (selectedWidget_ >= 0) {
        selectedWidget_ = -1;
        isEditing_ = false;
        editBuffer_.clear();
        cursorPosition_ = 0;
        // NO preedit clear here, deliberately. The first pass at phase-76 added
        // one, reasoning that this is a fourth exit from editing. Mutation M10
        // disabled it and SURVIVED: every caller that can reach deselectField()
        // with an edit in progress -- deactivate(), setPage(), onMousePress
        // outside any field, Escape while editing, and setCreationFieldType()
        // -- calls commitEdit() or cancelEdit() first, and both of those clear
        // it. (The phase-23 karen gate caught this list omitting the fifth
        // caller; a comment that enumerates call sites is exactly the kind that
        // rots, so it is worth re-deriving rather than trusting.) A composition can
        // therefore never be live here, and a clear that no input can exercise
        // is decoration (the DCA 0039 lesson, applied to this phase's own fix).
        // What actually holds the invariant is those commit/cancel calls, and
        // that is what EveryExitFromEditingClearsTheComposition pins.
        tabOrderPosition_ = -1;

        if (fieldDeselected) {
            fieldDeselected();
        }
    }
}

void FormTool::startEditing(int widgetIndex) {
    auto fieldData = manager_->getFormField(page_, widgetIndex);
    isEditing_ = true;
    editBuffer_ = fieldData.value;
    cursorPosition_ = static_cast<int>(editBuffer_.size());
    preeditText_.clear();
    // Drop the previous field's recorded caret HERE, on entry, rather
    // than at each exit. Entry is one site and always runs; the exits
    // are several and the set grows. Until this field paints, the
    // accessor reports nothing, so the IME never sees the old field's
    // position. (Invalidating at the exits AS WELL made the property
    // doubly-enforced, and mutation M12 then survived removing either
    // one -- the phase-75 M3 shape.)
    caretScreenRectValid_ = false;

    // Fire focus JS event
    if (jsEngine_ && jsEngine_->isEnabled()) {
        jsEngine_->fireFocusEvent(page_, widgetIndex);
    }
}

void FormTool::commitEdit() {
    if (!isEditing_ || selectedWidget_ < 0 || !page_) return;

    auto fieldData = manager_->getFormField(page_, selectedWidget_);
    if (editBuffer_ != fieldData.value) {
        auto cmd = std::make_unique<SetFieldValueCommand>(
            manager_, page_, selectedWidget_, editBuffer_);
        undoManager_->execute(std::move(cmd));

        if (fieldValueChanged) {
            auto updated = manager_->getFormField(page_, selectedWidget_);
            fieldValueChanged(page_->index(), updated);
        }
    }

    // Fire blur JS event then run all calculate handlers.
    // pdf_annot_event_blur fires /AA/Bl (Blur) and /AA/V (Validate via
    // pdf_annot_event_blur's internal chain on some MuPDF builds).
    // pdf_calculate_form propagates /AA/C (Calculate) on all dependent
    // fields (those listed in /CO on the AcroForm dictionary) so that
    // calculated fields like "Total" or "RESULT-<Source>" are updated
    // whenever a source field is committed.
    if (jsEngine_ && jsEngine_->isEnabled()) {
        jsEngine_->fireBlurEvent(page_, selectedWidget_);
        jsEngine_->calculateAll(page_);
    }

    isEditing_ = false;
    editBuffer_.clear();
    cursorPosition_ = 0;
    // An uncommitted composition is abandoned with the edit; it was never in
    // editBuffer_, so the committed value above cannot contain it.
    preeditText_.clear();
}

void FormTool::cancelEdit() {
    isEditing_ = false;
    editBuffer_.clear();
    cursorPosition_ = 0;
    preeditText_.clear();

    // Fire blur JS event
    if (jsEngine_ && jsEngine_->isEnabled() && selectedWidget_ >= 0) {
        jsEngine_->fireBlurEvent(page_, selectedWidget_);
    }
}

void FormTool::handleTextInput(const QString& text) {
    const std::string textStr = text.toStdString();
    // Insert at a codepoint boundary. cursorPosition_ should already be on one
    // -- every movement above keeps it there -- but setFieldValue() and
    // beginEdit() also assign it, and a value arriving from elsewhere could
    // leave it mid-sequence. Aligning here means a stale cursor degrades to
    // "inserted one character earlier" instead of "produced invalid UTF-8".
    const std::size_t at = pdfe::core::utf8::alignLeft(
        editBuffer_, static_cast<std::size_t>(cursorPosition_));
    editBuffer_.insert(at, textStr);
    cursorPosition_ = static_cast<int>(at + textStr.size());
}

// ─── Input-method (IME) support — WIN-49, follow-up phase-76 ───────────────

void FormTool::setPreeditText(const QString& text) {
    if (!isEditing_) {
        preeditText_.clear();
        return;
    }
    // NOTE: editBuffer_ is deliberately NOT touched here. See FormTool.h.
    preeditText_ = text.toStdString();
}

void FormTool::clearPreedit() {
    preeditText_.clear();
}

void FormTool::commitPreedit(const QString& text,
                             int replaceFrom, int replaceLength) {
    if (!isEditing_) return;

    preeditText_.clear();

    if (replaceLength > 0) {
        // replaceFrom/replaceLength are in CODEPOINTS relative to the caret,
        // while cursorPosition_ is a BYTE offset (src/core/Utf8.h explains
        // why), so walk boundaries rather than doing byte arithmetic — the
        // exact class of bug phase-74 fixed for Backspace.
        std::size_t start = pdfe::core::utf8::alignLeft(
            editBuffer_, static_cast<std::size_t>(cursorPosition_));
        for (int i = 0; i < replaceFrom; ++i)
            start = pdfe::core::utf8::nextBoundary(editBuffer_, start);
        for (int i = 0; i > replaceFrom; --i)
            start = pdfe::core::utf8::prevBoundary(editBuffer_, start);

        cursorPosition_ = static_cast<int>(start);
        for (int i = 0; i < replaceLength; ++i) {
            if (start >= editBuffer_.size()) break;
            pdfe::core::utf8::eraseAt(editBuffer_, start);
        }
    }

    if (!text.isEmpty()) {
        // handleTextInput keeps the codepoint-boundary invariant on insert.
        handleTextInput(text);
    }
}

bool FormTool::caretScreenRect(QRectF& out) const {
    if (!caretScreenRectValid_ || !isEditing_)
        return false;
    out = caretScreenRect_;
    return true;
}

int FormTool::cursorPositionForIme() const {
    const std::size_t at = pdfe::core::utf8::alignLeft(
        editBuffer_, static_cast<std::size_t>(std::max(0, cursorPosition_)));
    return static_cast<int>(
        pdfe::core::utf8::codepointCount(editBuffer_.substr(0, at)));
}

void FormTool::tabToNextField() {
    if (tabOrder_.empty()) return;

    tabOrderPosition_++;
    if (tabOrderPosition_ >= static_cast<int>(tabOrder_.size())) {
        tabOrderPosition_ = 0;
    }

    int nextWidget = tabOrder_[tabOrderPosition_];
    selectField(nextWidget);

    auto fieldData = manager_->getFormField(page_, nextWidget);
    if (fieldData.type == FormFieldType::Text && !fieldData.isReadOnly) {
        startEditing(nextWidget);
    }
}

void FormTool::tabToPreviousField() {
    if (tabOrder_.empty()) return;

    tabOrderPosition_--;
    if (tabOrderPosition_ < 0) {
        tabOrderPosition_ = static_cast<int>(tabOrder_.size()) - 1;
    }

    int prevWidget = tabOrder_[tabOrderPosition_];
    selectField(prevWidget);

    auto fieldData = manager_->getFormField(page_, prevWidget);
    if (fieldData.type == FormFieldType::Text && !fieldData.isReadOnly) {
        startEditing(prevWidget);
    }
}

FormFieldData FormTool::selectedFieldData() const {
    if (selectedWidget_ >= 0 && page_) {
        return manager_->getFormField(page_, selectedWidget_);
    }
    return FormFieldData();
}

void FormTool::syncEditBuffer(const std::string& value)
{
    // Only update when a field is actively selected and being edited.
    // This prevents a stale editBuffer_ from overwriting a value that was
    // committed via the panel pipe (setTextValueForTesting path).
    if (isEditing_) {
        editBuffer_ = value;
        cursorPosition_ = static_cast<int>(editBuffer_.size());
    }
}

void FormTool::cancelCurrentEdit()
{
    cancelEdit();
}

void FormTool::setCreationFieldType(FormFieldType type) {
    // GitHub #243 — counted on ENTRY, before anything touches `this`, so a call
    // arriving through a stale connection is still counted. That is the whole
    // point: the defect was extra invocations, not a wrong final value.
    ++setCreationFieldTypeCalls_;

    // Exit editing mode if switching to/from creation mode
    if (isEditing_) {
        commitEdit();
    }
    deselectField();

    creationFieldType_ = type;
    isDrawingRect_ = false;
}

} // namespace pdfe::forms
