#pragma once

#include "FormFieldData.h"
#include "annotations/AnnotationTool.h"
#include <functional>
#include <string>
#include <vector>
#include <memory>

class QKeyEvent;

namespace pdfe::core {
class Page;
}

namespace pdfe::commands {
class UndoManager;
}

namespace pdfe::forms {

class FormManager;
class JavaScriptEngine;

/// Tool for interacting with PDF form fields on the canvas.
///
/// Inherits from AnnotationTool to reuse PdfCanvas's tool dispatch mechanism.
/// Handles clicking to select fields, typing in text fields, toggling checkboxes,
/// and Tab navigation between fields.
class FormTool : public pdfe::annotations::AnnotationTool {
public:
    FormTool(FormManager* manager,
             JavaScriptEngine* jsEngine,
             pdfe::commands::UndoManager* undoManager);
    ~FormTool() override;

    pdfe::annotations::AnnotationType toolType() const override;

    void activate() override;
    void deactivate() override;

    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint) override;
    void onKeyPress(QKeyEvent* event) override;
    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt) override;

    /// GitHub #235, then #236. This tool's FIELD state only -- `page_` itself is
    /// released by AnnotationTool::onDocumentClosing() before this hook runs,
    /// and is no longer this class's business.
    ///
    /// History worth keeping, because it is why the base owns it now: #235 was
    /// an access violation reading 0xffffffffffffffff at
    /// `pdfe::core::Page::index+0x17`, from `onPaintOverlay` running against a
    /// page the document had already freed. The `!page_` check above it was not
    /// a guard -- a dangling pointer is not null and sails straight past.
    void onDocumentClosingImpl() override;

    /// GitHub #235 / #234. The form-field SET changed while the document stayed
    /// open, so a cached widget index may now name a different field or none.
    void onAnnotationsChanged() override;

    // GitHub #269. Was FormTool::setPage, split across the base class's two
    // hooks; see the definitions in FormTool.cpp for why the split falls where
    // it does.
    void onPageChangingImpl() override;
    void onPageChangedImpl() override;

    // Tab navigation
    void tabToNextField();
    void tabToPreviousField();

    // Selected field info
    int selectedFieldIndex() const { return selectedWidget_; }
    FormFieldData selectedFieldData() const;

    /// Pipeline c91e7f3a phase-5: synchronise the internal edit buffer after
    /// an external (pipe-based) value commit so that a subsequent commitEdit()
    /// does not overwrite the just-committed value with the old buffer content.
    void syncEditBuffer(const std::string& value);

    /// Cancel the current edit without committing (used after external
    /// operations — e.g. Reset Form — that change the field value outside
    /// the FormTool's edit loop, to prevent stale editBuffer_ overwrite).
    void cancelCurrentEdit();

    /// Phase-74 (WIN-49 prerequisite) test seams.
    ///
    /// The UTF-8 invariant this phase installs is a property of `editBuffer_`
    /// ITSELF — it must never hold a split multi-byte sequence, not even
    /// momentarily between keystrokes. Asserting only on the committed field
    /// value would test a later stage and would pass even if the buffer went
    /// invalid and was repaired on the way out, so the test needs to see the
    /// buffer directly. Read-only: nothing here can put the tool into a state
    /// the UI cannot.
    const std::string& editBufferForTesting() const { return editBuffer_; }
    int cursorPositionForTesting() const { return cursorPosition_; }

    /// True while a text field is being edited. PdfCanvas reads this to decide
    /// whether the widget should advertise Qt::WA_InputMethodEnabled.
    bool isEditingField() const { return isEditing_; }

    // ─── Input-method (IME) support — WIN-49, follow-up phase-76 ────────────
    //
    // Same separation as TextEditTool: the composition lives OUTSIDE
    // editBuffer_, so an in-flight preedit can never be committed to the field
    // value by a commitEdit() that happens to fire mid-composition, and can
    // never leave editBuffer_ holding text the user has not accepted.
    // commitPreedit() is the only path that touches editBuffer_, and it goes
    // through handleTextInput(), which keeps the phase-74 codepoint-boundary
    // invariant.

    /// Store the in-flight composition (UTF-8). Never touches editBuffer_.
    ///
    /// Takes no caret-within-composition argument, unlike TextEditTool's: this
    /// tool renders no field text at all — onPaintOverlay draws a selection
    /// rect and an approximated caret, and the glyphs on screen come from
    /// MuPDF's appearance stream, which is stale until commitEdit(). So there
    /// is nothing to position a composition caret against, and storing one
    /// would be a field nothing reads. See IME-FORM-PREEDIT-PAINT-01.
    void setPreeditText(const QString& text);
    void clearPreedit();

    /// Commit @p text after deleting @p replaceLength CODEPOINTS starting
    /// @p replaceFrom codepoints from the caret (Qt's QInputMethodEvent
    /// semantics, relative to the cursor, possibly negative).
    void commitPreedit(const QString& text, int replaceFrom, int replaceLength);

    const std::string& preeditTextForTesting() const { return preeditText_; }
    bool hasPreedit() const { return !preeditText_.empty(); }

    /// Field text for Qt::ImSurroundingText (UTF-8, composition excluded).
    const std::string& surroundingText() const { return editBuffer_; }

    /// Caret for Qt::ImCursorPosition, in CODEPOINTS — the unit Qt's input
    /// method protocol uses. cursorPosition_ is a byte offset and handing that
    /// to the IME would misplace the candidate window on any non-ASCII field.
    int cursorPositionForIme() const;

    /// Caret rectangle in SCREEN (widget) coordinates as recorded by the most
    /// recent onPaintOverlay, for Qt::ImCursorRectangle. Returns false when no
    /// caret has been painted yet (no active field edit, or not yet drawn),
    /// leaving @p out untouched so the caller can fall back to the platform
    /// default rather than position a candidate window on a guess.
    ///
    /// This reports the caret that was ACTUALLY drawn, not a recomputation:
    /// the drawn caret is itself an approximation (uniform advance width, no
    /// shaping — see onPaintOverlay), so a separately-derived rectangle would
    /// disagree with what the user sees. Reporting the drawn position means the
    /// candidate window sits where the caret appears, which is the property
    /// that matters, even while that position stays approximate.
    [[nodiscard]] bool caretScreenRect(QRectF& out) const;

    // Creation mode
    void setCreationFieldType(FormFieldType type);
    FormFieldType creationFieldType() const { return creationFieldType_; }
    bool isCreationMode() const { return creationFieldType_ != FormFieldType::Unknown; }

    /// GitHub #243 — how many FormTool objects are alive in this process.
    ///
    /// A test seam, and a deliberately blunt one. The defect behind #243 was a
    /// QAction lambda holding a raw FormTool* that outlived the tool, which is
    /// only observable as "the thing it points at is gone". Counting live
    /// instances lets a test assert the tool really was destroyed (and that the
    /// replacement really is a single new one) without depending on a
    /// use-after-free happening to fault — which it does not reliably do,
    /// because the allocator usually hands the freed block straight back.
    ///
    /// Not thread-safe and not meant to be: every FormTool is created and
    /// destroyed on the GUI thread.
    static int liveInstanceCountForTesting() { return liveInstanceCount_; }

    /// GitHub #243 — how many times setCreationFieldType() has been ENTERED.
    ///
    /// This is the oracle for the fix itself, and it needs the static because
    /// the interesting calls are the ones made on objects that no longer exist.
    /// Asserting on the resulting creationFieldType() cannot see them: when
    /// seven stale lambdas and one live lambda all fire, the live one still
    /// writes the correct value, so the observable end state is identical
    /// whether or not the stale connections are there. Counting entries
    /// distinguishes them — one trigger must mean exactly one call.
    static int setCreationFieldTypeCallsForTesting() { return setCreationFieldTypeCalls_; }
    static void resetSetCreationFieldTypeCallsForTesting() { setCreationFieldTypeCalls_ = 0; }

    // Callbacks
    std::function<void(int pageIndex, FormFieldData)> fieldValueChanged;
    std::function<void(int widgetIndex)> fieldSelected;
    std::function<void()> fieldDeselected;
    std::function<void(int pageIndex, FormFieldType type, QRectF rect)> fieldCreated;

private:
    void commitEdit();
    void cancelEdit();
    void startEditing(int widgetIndex);
    void selectField(int widgetIndex);
    void deselectField();
    void handleTextInput(const QString& text);

    FormManager* manager_;
    JavaScriptEngine* jsEngine_;
    pdfe::commands::UndoManager* undoManager_;

    // Editing state
    int selectedWidget_ = -1;
    bool isEditing_ = false;
    std::string editBuffer_;
    int cursorPosition_ = 0;
    /// In-flight IME composition (UTF-8). Never part of editBuffer_.
    std::string preeditText_;
    /// Screen-space caret rect recorded by the last onPaintOverlay; feeds
    /// Qt::ImCursorRectangle so the IME candidate window follows the caret.
    QRectF caretScreenRect_;
    bool caretScreenRectValid_ = false;

    // Tab order
    std::vector<int> tabOrder_;
    int tabOrderPosition_ = -1;

    // Creation mode state
    FormFieldType creationFieldType_ = FormFieldType::Unknown;

    /// GitHub #243 — see liveInstanceCountForTesting().
    static int liveInstanceCount_;
    static int setCreationFieldTypeCalls_;
    bool isDrawingRect_ = false;
    QPointF drawStart_;
    QPointF drawEnd_;
    int drawPageIndex_ = -1;
};

} // namespace pdfe::forms
