// -*- coding: utf-8 -*-
// tests/test_ime_input_phase76.cpp
//
// Follow-up phase 76 (pipeline phase-23) — WIN-49: IME support on the
// document-edit surface.
//
// WHAT WAS DEAD, AND WHAT WAS NOT. The reverification found zero hits for
// inputMethod-related symbols across src/ under two phrasings, and
// Qt::WA_InputMethodEnabled never set. Both re-checked true. But the count is
// easy to over-read: the 256 QLineEdit/QTextEdit declarations in dialogs and
// panels have had working IME all along, because Qt implements
// inputMethodEvent/inputMethodQuery for its own text widgets. What had no IME
// was precisely the three places that hand-roll text input and therefore
// inherit nothing — PdfCanvas (custom-painted), TextEditTool (own caret, own
// painted overlay) and FormTool (own editBuffer_).
//
// THE HAZARD THIS FILE PINS. TextEditTool::onKeyPress snapshots the active
// span's text and calls pushIncrementalEdit() on ANY difference afterwards.
// Routing an IME composition through the model would therefore push one undo
// command per keystroke of text the user has NOT committed and may still
// cancel. The fix is structural, not a guard: preeditText_ lives outside
// TextEditModel, so setPreeditText has no path to replaceCharRange at all.
// PreeditDoesNotTouchTheModelOrTheUndoStack is the test that would go red if
// anyone re-routed it through the model.
//
// WHY THESE TESTS SEND REAL QInputMethodEvents. Zero of the 62 pre-existing
// text-input tests sent one — they all synthesize QKeyEvent, which is a
// different Qt dispatch path and never reaches inputMethodEvent(). Calling
// TextEditTool::setPreeditText directly would test the tool while leaving the
// canvas override untested, which is exactly the layer that was missing. So
// every test below goes through PdfCanvas::sendInputMethodEventForTesting,
// which posts a real QInputMethodEvent via QApplication::sendEvent.
//
// WHAT THESE TESTS DO NOT PROVE. That Windows actually delivers a composition
// to the canvas. Driving a real MS-IME is not automatable here for three
// independent reasons (OS input-queue races, IME-specific candidate windows,
// and the documented Server-2019 UIA hang); that link is covered by the manual
// checklist at docs/audit/meta-audit-2026-07/followup/phase-76/manual-qa-ime.md
// and is deliberately NOT a CI gate.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "core/UndoManager.h"
#include "core/Utf8.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"
#include "forms/JavaScriptEngine.h"
#include "rendering/TileCache.h"
#include "rendering/TileRenderer.h"
#include "textedit/TextEditCommands.h"
#include "textedit/TextEditTool.h"
#include "ui/PdfCanvas.h"

#include <QApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVariant>

#include <filesystem>
#include <memory>
#include <string>

namespace {

// 日本語 — three CJK Unified Ideographs, written as explicit codepoints so the
// test cannot be perturbed by this file's encoding or the compiler's locale.
// Phase-75 is what makes these survive a save; this phase is what makes them
// reachable from an IME at all. Criterion 5 needs BOTH halves.
constexpr char32_t kNihongoCps[] = {0x65E5, 0x672C, 0x8A9E};
const std::u32string kNihongo(kNihongoCps, kNihongoCps + 3);

QString nihongoQString()
{
    return QString::fromUcs4(
        reinterpret_cast<const char32_t*>(kNihongo.data()),
        static_cast<int>(kNihongo.size()));
}

// U+00E9 (2 bytes) and U+4E2D (3 bytes) — the multi-byte cases that a
// byte-indexed replacement would split. Phase-74 fixed that for keystrokes;
// commitPreedit has to keep the same invariant.
const std::string kEAcuteUtf8 = "\xC3\xA9";
const std::string kCjkUtf8 = "\xE4\xB8\xAD";

} // namespace

// ===========================================================================
// Text-edit surface (PdfCanvas + TextEditTool)
// ===========================================================================

class ImeTextEditTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        const std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        fixture_ = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(fixture_))
            GTEST_SKIP() << "Test fixture not available: " << fixture_;

        doc_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(doc_, nullptr);
        ASSERT_GT(doc_->pageCount(), 0);

        cache_ = std::make_unique<pdfe::rendering::TileCache>(8 * 1024 * 1024);
        renderer_ = std::make_unique<pdfe::rendering::TileRenderer>(doc_.get());
        canvas_ = std::make_unique<pdfe::ui::PdfCanvas>();
        canvas_->resize(800, 600);
        canvas_->setDocument(doc_.get(), cache_.get(), renderer_.get());

        undo_ = std::make_unique<pdfe::commands::UndoManager>();

        auto tool = std::make_unique<pdfe::textedit::TextEditTool>(doc_.get(), ctx_);
        // Wire textEdited exactly as MainWindow does, so undo depth in these
        // tests means what it means in the product. Without this the hazard
        // test would be asserting against a stack nothing ever pushes to, and
        // would pass no matter what the preedit path did.
        tool->textEdited = [this](int pageIndex, int spanIndex,
                                  std::u32string oldText, std::u32string newText,
                                  pdfe::textedit::TextSpan spanInfo) {
            auto cmd = std::make_unique<pdfe::textedit::EditTextCommand>(
                doc_.get(), ctx_, pageIndex, spanIndex, spanInfo,
                oldText, newText);
            undo_->execute(std::move(cmd));
        };
        canvas_->setTextEditTool(std::move(tool));
        tool_ = canvas_->textEditTool();
        ASSERT_NE(tool_, nullptr);
        tool_->setPage(0);
        if (tool_->spanCountForTesting() <= 0)
            GTEST_SKIP() << "fixture page 0 has no editable vector-text spans";
    }

    void TearDown() override
    {
        canvas_.reset();
        undo_.reset();
        renderer_.reset();
        cache_.reset();
        doc_.reset();
        ctx_.reset();
        std::error_code ec;
        if (!savedPath_.empty()) std::filesystem::remove(savedPath_, ec);
    }

    /// Start editing span 0 through the production selectSpan path.
    void beginEditingSpan0() { ASSERT_TRUE(tool_->selectSpanForTesting(0)); }

    std::u32string span0Text() const
    {
        const QString s = tool_->spanTextForTesting(0);
        const auto ucs4 = s.toUcs4();
        return std::u32string(ucs4.begin(), ucs4.end());
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path savedPath_;
    std::unique_ptr<pdfe::core::Document> doc_;
    std::unique_ptr<pdfe::rendering::TileCache> cache_;
    std::unique_ptr<pdfe::rendering::TileRenderer> renderer_;
    std::unique_ptr<pdfe::commands::UndoManager> undo_;
    std::unique_ptr<pdfe::ui::PdfCanvas> canvas_;
    pdfe::textedit::TextEditTool* tool_ = nullptr;
};

// --- Criterion 4: the attribute tracks the editing state -------------------

TEST_F(ImeTextEditTest, InputMethodAttributeFollowsEditingStateTransitions)
{
    // Before any span is selected the canvas must NOT advertise IME readiness:
    // there is nowhere for a composition to go, and claiming otherwise makes
    // the platform route input at a widget that will drop it.
    EXPECT_FALSE(canvas_->inputMethodEnabledForTesting())
        << "canvas advertises IME readiness with no active text edit";

    beginEditingSpan0();
    EXPECT_TRUE(canvas_->inputMethodEnabledForTesting())
        << "selecting a span did not enable WA_InputMethodEnabled; the "
           "platform will never deliver a composition here";

    tool_->commitEdit();
    EXPECT_FALSE(canvas_->inputMethodEnabledForTesting())
        << "committing the edit left WA_InputMethodEnabled set";

    // And again for the cancel path, which is a different exit from editing.
    beginEditingSpan0();
    EXPECT_TRUE(canvas_->inputMethodEnabledForTesting());
    tool_->cancelEdit();
    EXPECT_FALSE(canvas_->inputMethodEnabledForTesting())
        << "cancelling the edit left WA_InputMethodEnabled set";
}

// --- Criterion 2: THE HAZARD ----------------------------------------------

TEST_F(ImeTextEditTest, PreeditDoesNotTouchTheModelOrTheUndoStack)
{
    beginEditingSpan0();
    const std::u32string before = span0Text();
    const int undoBefore = undo_->undoStackSize();

    // Preedit-only: no commit string. This is every keystroke of a composition
    // the user has not accepted yet.
    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(
        nihongoQString(), QString()));

    EXPECT_EQ(span0Text(), before)
        << "an uncommitted IME composition mutated the document model";
    EXPECT_EQ(undo_->undoStackSize(), undoBefore)
        << "an uncommitted IME composition pushed an undo command — this is "
           "the hazard the phase spec names: routing preedit through "
           "TextEditTool::onKeyPress's diff would push one command PER "
           "KEYSTROKE of text the user may still cancel";

    // The composition is nonetheless live and visible to the tool — it is
    // held outside the model, not discarded.
    EXPECT_TRUE(tool_->hasPreedit());
    EXPECT_EQ(tool_->preeditText(), kNihongo);
}

TEST_F(ImeTextEditTest, MultipleCompositionUpdatesStillPushNothing)
{
    // A real composition sends a preedit event per keystroke. One event
    // proving nothing is pushed would not catch an implementation that pushes
    // on the SECOND update (e.g. because it diffs against the previous
    // preedit); walk several updates.
    beginEditingSpan0();
    const std::u32string before = span0Text();
    const int undoBefore = undo_->undoStackSize();

    for (const QString& step : {QString("n"), QString("ni"), QString("nih"),
                                nihongoQString()}) {
        ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(step, QString()));
        EXPECT_EQ(span0Text(), before);
        EXPECT_EQ(undo_->undoStackSize(), undoBefore);
    }
}

TEST_F(ImeTextEditTest, AbandonedCompositionLeavesNoTraceAfterTheEditEnds)
{
    // Cancelling an edit mid-composition must not commit the composition, and
    // must not leave it to be painted over the next span's caret.
    beginEditingSpan0();
    const std::u32string before = span0Text();
    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(
        nihongoQString(), QString()));
    ASSERT_TRUE(tool_->hasPreedit());

    tool_->cancelEdit();
    EXPECT_FALSE(tool_->hasPreedit())
        << "an abandoned composition outlived the edit that hosted it";
    EXPECT_EQ(span0Text(), before);
}

// --- Criterion 3: commit ---------------------------------------------------

TEST_F(ImeTextEditTest, CommitInsertsTextWithNormalUndoSemantics)
{
    beginEditingSpan0();
    const std::u32string before = span0Text();
    const int undoBefore = undo_->undoStackSize();

    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(
        QString(), nihongoQString()));

    const std::u32string after = span0Text();
    EXPECT_NE(after, before) << "the committed composition never reached the model";
    EXPECT_NE(after.find(kNihongo), std::u32string::npos)
        << "the committed text is not present in the span";
    EXPECT_GT(undo_->undoStackSize(), undoBefore)
        << "a COMMITTED composition must push an undo command — the preedit "
           "separation must not have swallowed the commit as well";
    EXPECT_FALSE(tool_->hasPreedit())
        << "committing left the composition in flight";

    ASSERT_TRUE(undo_->canUndo());
    undo_->undo();
    // The undo target is the document, not the in-memory span, so assert on
    // the stack depth returning rather than on span text (which the tool holds
    // independently until commitEdit).
    EXPECT_EQ(undo_->undoStackSize(), undoBefore)
        << "undo did not remove the command the commit pushed";
}

TEST_F(ImeTextEditTest, CommitWithReplacementDeletesTheNamedRangeFirst)
{
    // Qt's replaceFrom is relative to the caret and may be negative: an IME
    // replacing the character BEFORE the caret sends (-1, 1). Getting the sign
    // wrong deletes forward and silently eats the wrong character.
    beginEditingSpan0();

    // Put some known text in first, via a plain commit, and park the caret at
    // its end.
    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(QString(), "AB"));
    const std::u32string afterFirst = span0Text();
    ASSERT_NE(afterFirst.find(U"AB"), std::u32string::npos);

    // Replace the single character before the caret ("B") with "C".
    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(
        QString(), "C", /*replaceFrom=*/-1, /*replaceLength=*/1));

    const std::u32string afterReplace = span0Text();
    EXPECT_NE(afterReplace.find(U"AC"), std::u32string::npos)
        << "replaceFrom=-1/replaceLength=1 did not replace the character "
           "before the caret; got: "
        << tool_->spanTextForTesting(0).toStdString();
    EXPECT_EQ(afterReplace.find(U"AB"), std::u32string::npos)
        << "the replaced character is still present";
}

TEST_F(ImeTextEditTest, EmptyCommitWithNoReplacementPushesNothing)
{
    // A composition that ends with nothing committed (the user cancelled it in
    // the IME) must not register as an edit.
    beginEditingSpan0();
    const std::u32string before = span0Text();
    const int undoBefore = undo_->undoStackSize();

    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(QString(), QString()));

    EXPECT_EQ(span0Text(), before);
    EXPECT_EQ(undo_->undoStackSize(), undoBefore);
}

// --- Criterion 1: what the canvas reports back to the platform -------------

TEST_F(ImeTextEditTest, InputMethodQueryReportsSurroundingTextAndCursor)
{
    beginEditingSpan0();

    EXPECT_TRUE(canvas_->inputMethodQuery(Qt::ImEnabled).toBool());

    const QString surrounding =
        canvas_->inputMethodQuery(Qt::ImSurroundingText).toString();
    EXPECT_EQ(surrounding, tool_->spanTextForTesting(0))
        << "ImSurroundingText does not match the span being edited; an IME "
           "using it for reconversion would operate on the wrong text";

    const QVariant cursor = canvas_->inputMethodQuery(Qt::ImCursorPosition);
    ASSERT_TRUE(cursor.isValid());
    EXPECT_GE(cursor.toInt(), 0);
    EXPECT_LE(cursor.toInt(), surrounding.size())
        << "reported caret lies outside the reported surrounding text";

    // The composition must NOT appear in the surrounding text: it is not part
    // of the document yet, and an IME told otherwise would double-count it.
    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(
        nihongoQString(), QString()));
    EXPECT_EQ(canvas_->inputMethodQuery(Qt::ImSurroundingText).toString(),
              surrounding)
        << "the in-flight composition leaked into ImSurroundingText";
}

TEST_F(ImeTextEditTest, NoImeTargetMeansTheEventIsNotConsumed)
{
    // No span selected: the seam must report that there was nothing to send
    // to, rather than silently succeeding.
    EXPECT_FALSE(canvas_->sendInputMethodEventForTesting(
        nihongoQString(), QString()))
        << "the canvas claimed to consume a composition with no active edit";
    EXPECT_FALSE(tool_->hasPreedit());
}

// --- Criterion 5: the end-to-end payoff ------------------------------------

TEST_F(ImeTextEditTest, CjkCommittedThroughTheImeSeamSurvivesSaveAndReopen)
{
    // This is the criterion that needs phases 21, 22 AND 23 together: 23 makes
    // CJK reachable from an IME, 22 makes the encoder able to save it, and
    // without either half this assertion fails in a different place.
    beginEditingSpan0();
    ASSERT_TRUE(canvas_->sendInputMethodEventForTesting(
        QString(), nihongoQString()));
    ASSERT_NE(span0Text().find(kNihongo), std::u32string::npos);

    tool_->commitEdit();

    savedPath_ = std::filesystem::temp_directory_path() /
                 "pdfe_ime_p76_cjk_roundtrip.pdf";
    std::error_code ec;
    std::filesystem::remove(savedPath_, ec);
    doc_->saveAs(savedPath_);
    ASSERT_TRUE(std::filesystem::exists(savedPath_))
        << "saveAs produced no file at " << savedPath_;

    // Reopen and extract. Reading the model back from the ORIGINAL document
    // would prove nothing about what landed on disk.
    auto reopened = pdfe::core::Document::open(ctx_, savedPath_);
    ASSERT_NE(reopened, nullptr);
    auto page = reopened->page(0);
    ASSERT_NE(page, nullptr);
    pdfe::core::TextExtractor extractor(ctx_);
    auto blocks = extractor.extract(page.get());
    pdfe::textedit::TextEditModel model(blocks);

    std::u32string all;
    for (int i = 0; i < model.spanCount(); ++i)
        all += model.span(i).text;

    EXPECT_NE(all.find(kNihongo), std::u32string::npos)
        << "CJK committed through the IME seam did not survive save+reopen";
    // The two specific ways phase-75 found it failing silently.
    EXPECT_EQ(all.find(U"???"), std::u32string::npos)
        << "the CJK degraded to '?' — the simple-font encoder path";
    EXPECT_EQ(all.find(std::u32string(3, U'\0')), std::u32string::npos)
        << "the CJK degraded to NUL — the CID-0 (.notdef) path";
}

// ===========================================================================
// Form surface (FormTool)
// ===========================================================================

class ImeFormToolTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        const std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        const auto formPdf = fixtures / "test_form.pdf";
        if (!std::filesystem::exists(formPdf))
            GTEST_SKIP() << "Test fixture not available: " << formPdf;

        doc_ = pdfe::core::Document::open(ctx_, formPdf);
        ASSERT_NE(doc_, nullptr);
        manager_ = std::make_unique<pdfe::forms::FormManager>(ctx_);
        js_ = std::make_unique<pdfe::forms::JavaScriptEngine>(ctx_);
        undo_ = std::make_unique<pdfe::commands::UndoManager>();
        tool_ = std::make_unique<pdfe::forms::FormTool>(
            manager_.get(), js_.get(), undo_.get());
        page_ = doc_->page(0);
        tool_->activate();
        tool_->setPage(page_.get());

        if (!beginEditingTextField())
            GTEST_SKIP() << "fixture has no editable text field";
    }

    bool beginEditingTextField()
    {
        const auto fields = manager_->getFormFields(page_.get());
        for (const auto& f : fields) {
            if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
                QMouseEvent ev(QEvent::MouseButtonPress, QPointF(0, 0),
                               Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                tool_->onMousePress(&ev, 0, f.rect.center());
                return tool_->isEditingField();
            }
        }
        return false;
    }

    void type(const QString& s)
    {
        QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, s);
        tool_->onKeyPress(&ev);
    }

    const std::string& buffer() const { return tool_->editBufferForTesting(); }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::core::Document> doc_;
    std::unique_ptr<pdfe::forms::FormManager> manager_;
    std::unique_ptr<pdfe::forms::JavaScriptEngine> js_;
    std::unique_ptr<pdfe::commands::UndoManager> undo_;
    std::unique_ptr<pdfe::forms::FormTool> tool_;
    std::shared_ptr<pdfe::core::Page> page_;
};

TEST_F(ImeFormToolTest, PreeditDoesNotTouchTheEditBuffer)
{
    const std::string before = buffer();
    tool_->setPreeditText(nihongoQString());

    EXPECT_EQ(buffer(), before)
        << "an uncommitted composition entered editBuffer_; a commitEdit() "
           "firing mid-composition would then write text the user never "
           "accepted into the PDF field value";
    EXPECT_TRUE(tool_->hasPreedit());
    EXPECT_TRUE(pdfe::core::utf8::isValid(buffer()));
}

TEST_F(ImeFormToolTest, CommitInsertsAtACodepointBoundary)
{
    tool_->commitPreedit(nihongoQString(), 0, 0);

    EXPECT_NE(buffer().find("\xE6\x97\xA5"), std::string::npos)  // 日
        << "the committed composition did not reach editBuffer_";
    EXPECT_TRUE(pdfe::core::utf8::isValid(buffer()))
        << "commitPreedit left invalid UTF-8 in editBuffer_";
    EXPECT_FALSE(tool_->hasPreedit());
}

TEST_F(ImeFormToolTest, ReplacementWalksCodepointsNotBytes)
{
    // The phase-74 defect class, arriving through the IME instead of through
    // Backspace: replaceLength is a COUNT OF CODEPOINTS. Treating it as bytes
    // erases one byte of a 3-byte CJK character and leaves the buffer holding
    // a split sequence — invalid UTF-8 that commitEdit() writes to the PDF.
    type(QString::fromStdString(kCjkUtf8));   // 中, 3 bytes
    ASSERT_EQ(buffer().find(kCjkUtf8), 0u) << "setup: field did not take the CJK";
    const std::string afterType = buffer();

    // Replace the one codepoint before the caret with an accented Latin letter.
    tool_->commitPreedit(QString::fromUtf8(kEAcuteUtf8.c_str()), -1, 1);

    EXPECT_TRUE(pdfe::core::utf8::isValid(buffer()))
        << "replacing one CODEPOINT split a multi-byte sequence; buffer is "
           "now invalid UTF-8 (this is phase-74's defect class re-entering "
           "through the IME path)";
    EXPECT_EQ(buffer().find(kCjkUtf8), std::string::npos)
        << "the replaced CJK character is still present";
    EXPECT_NE(buffer().find(kEAcuteUtf8), std::string::npos)
        << "the replacement text was not inserted";
    EXPECT_NE(buffer(), afterType);
}

TEST_F(ImeFormToolTest, CaretRectIsReportedOnceTheCaretHasBeenPainted)
{
    // Criterion 1 requires the caret rectangle to be reported for the editing
    // surfaces, form fields included. The first implementation declined this
    // query for form fields and deferred it; the phase-23 validator gate
    // refused that deferral, correctly — FormTool does paint a caret, so
    // "it doesn't record one" was a property of the implementation, not a
    // constraint.
    QRectF caret;

    // Before any paint there is genuinely nothing to report, and reporting a
    // guess would aim the IME candidate window at a position the caret is not
    // at. Declining HERE is right; declining always was not.
    EXPECT_FALSE(tool_->caretScreenRect(caret))
        << "a caret rectangle was reported before any caret was painted";

    // Paint the overlay the way the canvas does, into an off-screen image.
    QImage surface(400, 200, QImage::Format_ARGB32);
    surface.fill(Qt::white);
    {
        QPainter painter(&surface);
        tool_->onPaintOverlay(painter, /*pageIndex=*/0, /*zoom=*/1.0f,
                              QPointF(0, 0), /*pageHeightPt=*/200.0f);
    }

    ASSERT_TRUE(tool_->caretScreenRect(caret))
        << "no caret rectangle after the overlay painted one";
    EXPECT_GT(caret.width(), 0.0)
        << "a zero-width rect reads as 'no position' to some platforms";
    EXPECT_GT(caret.height(), 0.0);

    // And it must not outlive the edit. Note WHERE the danger actually is:
    // asserting only this next line is not enough, and mutation M12 proved it.
    // While no edit is in progress the accessor's own isEditing_ guard already
    // returns false, so removing the invalidation is invisible here — the
    // property is enforced twice and this assertion sees only the second
    // mechanism (the phase-75 M3 shape: mutating one of two enforcements is a
    // no-op that proves nothing).
    tool_->cancelCurrentEdit();
    EXPECT_FALSE(tool_->caretScreenRect(caret))
        << "the caret rectangle survived the end of the edit";

    // THIS is the case that can actually go wrong: a NEW edit begins, so
    // isEditing_ is true again, but nothing has painted yet. If the previous
    // edit did not invalidate its recorded rect, the IME is handed the OLD
    // field's caret position and puts the candidate window over a field the
    // user is no longer typing in.
    // Re-arm through the real lifecycle. A bare re-click will not do it:
    // onMousePress only starts an edit when the clicked widget differs from
    // the selected one, and cancelCurrentEdit leaves the field selected.
    tool_->deactivate();
    tool_->activate();
    tool_->setPage(page_.get());
    ASSERT_TRUE(beginEditingTextField()) << "could not re-arm a field edit";
    EXPECT_FALSE(tool_->caretScreenRect(caret))
        << "a caret rectangle from the PREVIOUS edit is live in a new edit "
           "that has not painted yet — the candidate window would appear over "
           "the wrong field";
}

TEST_F(ImeFormToolTest, CursorPositionForImeIsReportedInCodepoints)
{
    // cursorPosition_ is a BYTE offset internally (see src/core/Utf8.h for
    // why). Qt's input-method protocol counts characters, so handing the byte
    // offset straight to the IME misplaces the candidate window by exactly the
    // number of extra bytes the field already holds.
    type(QString::fromStdString(kCjkUtf8));  // one codepoint, three bytes

    EXPECT_EQ(tool_->cursorPositionForIme(), 1)
        << "expected 1 codepoint; got " << tool_->cursorPositionForIme()
        << " (byte offset is " << tool_->cursorPositionForTesting() << ")";
    EXPECT_EQ(tool_->cursorPositionForTesting(), 3)
        << "setup assumption broken: the internal cursor is not a byte offset";
}

TEST_F(ImeFormToolTest, PreeditIsAbandonedWhenTheEditEnds)
{
    tool_->setPreeditText(nihongoQString());
    ASSERT_TRUE(tool_->hasPreedit());

    tool_->cancelCurrentEdit();
    EXPECT_FALSE(tool_->hasPreedit())
        << "an abandoned composition outlived the field edit that hosted it";
}

TEST_F(ImeFormToolTest, EveryExitFromEditingClearsTheComposition)
{
    // A field edit can end four ways, and this asserts the composition is gone
    // after each. Driving only one of them — which is what the first version of
    // this file did — is satisfied by three of the four clears being absent.
    //
    // On the mechanism, which is not what it first looks like: the first pass
    // at this phase also added a preedit clear to deselectField(), reasoning
    // that it is a fourth exit. Mutation M10 disabled that clear and SURVIVED,
    // because every caller reaching deselectField() mid-edit calls commitEdit()
    // or cancelEdit() first. The clear was unreachable, so it was removed
    // rather than kept with this test passing beside it. What this test
    // actually pins is those commit/cancel calls — including the one inside
    // deactivate(), which M10 now targets.
    struct Exit {
        const char* name;
        void (*run)(pdfe::forms::FormTool&);
    };
    const Exit exits[] = {
        {"commitEdit (Enter)", [](pdfe::forms::FormTool& t) {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            t.onKeyPress(&ev);
        }},
        {"cancelEdit (Escape)", [](pdfe::forms::FormTool& t) {
            QKeyEvent ev(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            t.onKeyPress(&ev);
        }},
        {"cancelCurrentEdit (external value change)",
         [](pdfe::forms::FormTool& t) { t.cancelCurrentEdit(); }},
        {"deselectField via deactivate",
         [](pdfe::forms::FormTool& t) { t.deactivate(); }},
    };

    for (const Exit& e : exits) {
        if (!tool_->isEditingField())
            ASSERT_TRUE(beginEditingTextField()) << "re-arm failed for " << e.name;
        tool_->setPreeditText(nihongoQString());
        ASSERT_TRUE(tool_->hasPreedit()) << "setup failed for " << e.name;

        e.run(*tool_);

        EXPECT_FALSE(tool_->hasPreedit())
            << "exit '" << e.name << "' left the composition in flight";
        // Re-arm for the next exit (deactivate() tears the tool down).
        tool_->activate();
        tool_->setPage(page_.get());
    }
}
