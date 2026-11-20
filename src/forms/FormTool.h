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

namespace detail {
/// The live edit paint's cached line layout (FormTool.cpp). Opaque here.
struct EditLayoutCache;
}

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

    /// form-field-bugs phase-8, bug 6: Esc aborts an in-progress field
    /// placement — the rubber-band (isDrawingRect_) and/or armed creation mode
    /// (isCreationMode()). Exits creation mode via the existing creationModeExited
    /// callback (unchecks the toolbar action) and leaves no partial field. onKeyPress
    /// calls this from its Escape branch BEFORE the edit/deselect handling.
    bool cancelInProgress() override;

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

    /// Test seam (form-field-bugs phase-2, bug-5). Drive the exact value-commit
    /// path a canvas inline edit takes — select @p widgetIndex, load @p newValue
    /// into the edit buffer, and commit — deterministically and headlessly,
    /// without synthesizing Qt mouse/key events plus page geometry. Routes
    /// through the SAME private commitEdit() the real edit loop calls, so it
    /// exercises the production commit/park wiring (the commitFieldValue
    /// delegation) under test rather than re-deriving it.
    void commitValueEditForTesting(int widgetIndex, const std::string& newValue);

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
    /// onPaintOverlay draws it, underlined, at the caret inside the field, in
    /// the field's /DA font, size and colour, and puts the caret after it.
    /// IME-FORM-PREEDIT-PAINT-01 (the composition was never drawn, because the
    /// tool rendered no field text and the appearance stream is stale until
    /// commitEdit()) is CLOSED by GitHub #459; see the probes
    /// FormTextfieldEditPaintState.PreeditTextIsPainted,
    /// .TypedTextIsPaintedInsideTheFieldBeyondTheCaret,
    /// .PaintedInkGrowsWithMoreCharacters and .CaretXMatchesTheTextAdvance
    /// (tests/test_form_textfield_state.cpp).
    ///
    /// Still takes no caret-within-composition argument, unlike
    /// TextEditTool's: the caret is drawn at the end of the composition, and
    /// storing an inner position would be a field nothing reads.
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
    /// This reports the caret that was ACTUALLY drawn, not a recomputation, so
    /// the candidate window sits where the caret appears. Since GitHub #459
    /// that caret is at the QFontMetricsF advance of the text before it, in
    /// the field's /DA font at the current zoom (it used to be a uniform
    /// field-width/(n+1) estimate). Pinned by FormFieldEditPaintCanvasIme.*
    /// (PdfCanvas's ImCursorRectangle equals this rect and the drawn caret
    /// line, within 1 px) and FormFieldEditPaintDrawnCaret.*.
    [[nodiscard]] bool caretScreenRect(QRectF& out) const;

    /// Pipeline form-textfield, phase-3 security audit. What one paint of the
    /// field being edited did, for the hostile-input tests
    /// (FormFieldEditPaintHostile): the font pixel size it settled on after
    /// the /DA and zoom clamps, the layout's line count, how many lines and
    /// glyph runs it actually drew (visible-line culling, comb cap), and
    /// whether the cached layout was reused. Pure observation: nothing in
    /// the paint reads these back.
    struct EditPaintStats {
        QRectF caret;
        int fontPixelSize = 0;
        long long linesTotal = 0;
        long long linesDrawn = 0;
        long long runsDrawn = 0;
        long long unitsDrawn = 0;   ///< UTF-16 units handed to drawText
        bool layoutReused = false;
    };
    /// The stats of the last onPaintOverlay that painted an edit.
    const EditPaintStats& lastEditPaintStatsForTesting() const { return lastEditPaintStats_; }
    /// The live edit paint itself, with a caller-built FormFieldData -- the
    /// only way to reach the paint with values no PDF can carry past
    /// FormManager's read clamps (a NaN /DA size, a /BS /W of 1e30, a NaN
    /// zoom). A fresh layout cache per call.
    static EditPaintStats paintFieldEditForTesting(QPainter& painter,
                                                   const FormFieldData& field,
                                                   const QRectF& box, float zoom,
                                                   const std::string& buffer,
                                                   int cursorByte,
                                                   const std::string& preedit);

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

    /// form-field-bugs phase-2, bug-5 (undo-after-edit crash). The host commits
    /// a canvas inline text-field edit by building+executing a
    /// SetFieldValueCommand whose Page is PARKED in the host's command-page
    /// registry (MainWindow::commandPages_), so the command joins the same
    /// drain+rebind lifetime machinery every other command site uses — exactly
    /// as the panel-edit path (MainWindow::onFormFieldValueChanged) already
    /// does. When this is wired, commitEdit() delegates here INSTEAD of
    /// executing its own command against page_ (== the single-slot canvas/
    /// MainWindow toolPage_), whose Page is freed on a page/tool switch or the
    /// full-rewrite save that field creation forces — which left the stacked
    /// command's page_ dangling and crashed the next undo() (the #234/#235/#360
    /// use-after-free class; an access violation, not a C++ exception).
    /// Returns true if the host handled the commit; false (or unset) makes
    /// commitEdit() fall back to a direct, unparked execute for standalone/unit
    /// contexts with no host registry. FormTool is not a QObject, so this is a
    /// std::function like the other callbacks.
    std::function<bool(int pageIndex, int widgetIndex, const std::string& newValue)>
        commitFieldValue;

    std::function<void(int widgetIndex)> fieldSelected;
    std::function<void()> fieldDeselected;
    std::function<void(int pageIndex, FormFieldType type, QRectF rect)> fieldCreated;

    /// GitHub #328 — a Signature field was clicked in interaction mode.
    /// FormTool owns neither the SignatureManager nor the document, so it
    /// cannot run the signing flow itself; it reports the activation and
    /// MainWindow (which wires this) opens the Sign dialog and signs THAT
    /// field. Carries the page index and the clicked widget's index in the
    /// page's widget list (the same index FormManager::getFormField uses).
    /// A Qt signal is not an option here — FormTool is not a QObject (it
    /// uses std::function callbacks throughout, like fieldSelected above).
    std::function<void(int pageIndex, int widgetIndex)> signatureFieldActivated;

    /// GitHub #369 + form-field-bugs phase-9 (bug 10, sticky placement).
    /// Creation mode has been left — fired so MainWindow unchecks the armed
    /// form-toolbar field-type action and the UI reflects interaction mode.
    ///
    /// HISTORY: #369 originally fired this AUTOMATICALLY after every successful
    /// placement, dropping the tool back to interaction mode so a click could
    /// reach the just-placed field. Phase-9 made placement STICKY (place many
    /// fields without re-arming), so this no longer fires after a placement.
    /// It now fires ONLY on an EXPLICIT exit — Esc (cancelInProgress) or a tool
    /// switch — which is the path that restores #369's reachability: after the
    /// explicit exit, interaction-mode clicks select / activate placed fields.
    /// Same std::function idiom as the callbacks above (FormTool is not a QObject).
    std::function<void()> creationModeExited;

    /// form-field-bugs phase-6, bug-4 ("a placed field cannot be re-edited or
    /// deleted after content is added and focus leaves"). The delete half: a
    /// field that is SELECTED but not being edited had NO keyboard delete path —
    /// onKeyPress handled Delete/Backspace only inside the isEditing_ text branch
    /// (erase a character), and DeleteFormFieldCommand's only trigger was the
    /// toolbar's "Delete Field" button (#253). The host deletes the selected
    /// field through a DeleteFormFieldCommand whose Page is PARKED in
    /// MainWindow::commandPages_ — the SAME lifetime machinery commitFieldValue
    /// (bug-5) and the Delete Field toolbar action already use — so an undo of
    /// the delete cannot dereference a freed Page (the #234/#235/#360
    /// use-after-free class). Carries the page index and the page-widget-list
    /// index of the field to delete. Returns true if the host handled it; false
    /// (or unset) makes onKeyPress fall back to a direct, unparked execute for
    /// standalone/unit contexts with no host registry. FormTool is not a QObject,
    /// so this is a std::function like the other callbacks.
    std::function<bool(int pageIndex, int widgetIndex)> deleteSelectedField;

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
    /// The live edit paint's line layout, keyed on (composed text incl. the
    /// preedit, font, content width, mode) and rebuilt when any of them
    /// changes; dropped on entry to an edit and whenever no edit is painted.
    std::unique_ptr<detail::EditLayoutCache> editLayoutCache_;
    EditPaintStats lastEditPaintStats_;

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
