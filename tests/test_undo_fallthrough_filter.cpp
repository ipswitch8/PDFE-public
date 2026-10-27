// GitHub #295 — Ctrl+Z after creating an annotation undid nothing.
//
// MEASURED cause (docs/audit/p96-phase10/raw-i295-real-user-path.txt), not
// inferred from code. Creating an annotation calls
// AnnotationPropertiesWidget::focusContents(), so the user's next Ctrl+Z is
// delivered with focus on `annotContentsEdit`:
//
//   KeyPress key=0x5a mods=Control target=annotContentsEdit[QTextEdit]
//   focus=annotContentsEdit[QTextEdit] undoEnabled=1 canUndo=1 undoDepth=3
//
// ... and then nothing. No QEvent::Shortcut, no undo, the annotation count
// unchanged. With focus on the canvas the identical keystroke produces
// ShortcutOverride -> QEvent::Shortcut -> undoDepth 3 -> 2 and removes the
// annotation. So the issue's hypothesis 2 is dead (the action was ENABLED and
// the stack was three deep) and hypothesis 1 is confirmed: the focused editor
// claims Ctrl+Z for its own undo and, having nothing to undo, does nothing.
//
// WHAT THIS FILE PINS. UndoFallthroughFilter::shouldRelease is the decision,
// isolated from the window, the shortcut map and the focus chain -- the three
// things that make the live behaviour slow to test. The live end-to-end
// behaviour is covered by the probes above and by
// tests/test_ui_annotations.py; this is the deterministic oracle for the rule
// itself.
//
// The test asserts BOTH directions deliberately. A filter that always released
// would pass a one-sided test and break every text editor in the product: a
// user typing a note must keep Ctrl+Z for their typing. "Releases when empty"
// is only half the contract; "does NOT release when the editor has an edit" is
// the half that protects everything else.

#include "ui/AnnotationPropertiesWidget.h"
#include "ui/StickyNotePopup.h"
#include "ui/UndoFallthroughFilter.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QPlainTextEdit>

namespace {

using pdfe::ui::UndoFallthroughFilter;

/// A ShortcutOverride carrying the platform's own Undo/Redo binding, rather
/// than a hard-coded Ctrl+Z: QKeySequence::Undo is what QTextEdit matches
/// against, and hard-coding the chord would make this test disagree with the
/// product on any platform where the binding differs.
QKeyEvent overrideFor(QKeySequence::StandardKey which)
{
    const QKeySequence seq = QKeySequence(which);
    const QKeyCombination combo = seq[0];
    return QKeyEvent(QEvent::ShortcutOverride,
                     combo.key(),
                     combo.keyboardModifiers());
}

TEST(UndoFallthroughFilterTest, ReleasesUndoWhenTheEditorHasNothingToUndo)
{
    QTextDocument doc;
    ASSERT_FALSE(doc.isUndoAvailable())
        << "a fresh document must have nothing to undo, or this test proves "
           "nothing";

    QKeyEvent ev = overrideFor(QKeySequence::Undo);
    EXPECT_TRUE(UndoFallthroughFilter::shouldRelease(&doc, &ev))
        << "with an empty editor undo stack the keystroke is dead -- it must be "
           "released so the window's undo action can fire. This is GitHub "
           "#295: the key arrived, no shortcut matched, and nothing happened.";
}

TEST(UndoFallthroughFilterTest, KeepsUndoWhenTheEditorHasAnEditOfItsOwn)
{
    QTextDocument doc;
    // A CURSOR INSERTION, not setPlainText: QTextDocument::setPlainText RESETS
    // the undo stack, so it leaves isUndoAvailable() false. The first version
    // of this test used it and its own vacuity ASSERT caught the mistake --
    // without that assert, this test would have "passed" while proving the
    // opposite of what it claims. An insertion is also what a user typing
    // actually produces.
    QTextCursor cursor(&doc);
    cursor.insertText("typed by the user");
    ASSERT_TRUE(doc.isUndoAvailable())
        << "a cursor insertion must leave an undoable edit, or the negative "
           "direction below is vacuous";

    QKeyEvent ev = overrideFor(QKeySequence::Undo);
    EXPECT_FALSE(UndoFallthroughFilter::shouldRelease(&doc, &ev))
        << "the editor has an edit to undo, so Ctrl+Z belongs to the editor. "
           "Releasing it here would take Ctrl+Z away from every text field in "
           "the product, which is a worse defect than #295.";
}

TEST(UndoFallthroughFilterTest, ReleasesRedoOnlyWhenNoRedoIsAvailable)
{
    QTextDocument doc;
    QTextEdit edit;
    edit.setDocument(&doc);

    QKeyEvent redo = overrideFor(QKeySequence::Redo);
    EXPECT_TRUE(UndoFallthroughFilter::shouldRelease(&doc, &redo))
        << "nothing to redo -> release";

    QTextCursor cursor(&doc);
    cursor.insertText("an edit");   // setPlainText would reset the stack
    doc.undo();
    ASSERT_TRUE(doc.isRedoAvailable())
        << "undoing an edit must make a redo available, or the next assertion "
           "is vacuous";
    EXPECT_FALSE(UndoFallthroughFilter::shouldRelease(&doc, &redo))
        << "the editor has a redo of its own -> keep it";
    edit.setDocument(nullptr);
}

TEST(UndoFallthroughFilterTest, IgnoresKeysThatAreNotUndoOrRedo)
{
    QTextDocument doc;  // empty: undo and redo are both unavailable
    // A key the rule must never touch, even in the state where it releases
    // undo. Without this, a filter that released EVERYTHING in an empty editor
    // would pass every other test in this file.
    QKeyEvent other(QEvent::ShortcutOverride, Qt::Key_S, Qt::ControlModifier);
    EXPECT_FALSE(UndoFallthroughFilter::shouldRelease(&doc, &other))
        << "Ctrl+S is not undo or redo and must be left entirely alone";

    QKeyEvent bare(QEvent::ShortcutOverride, Qt::Key_Z, Qt::NoModifier);
    EXPECT_FALSE(UndoFallthroughFilter::shouldRelease(&doc, &bare))
        << "a bare Z is a character the user is typing, not a shortcut";
}

TEST(UndoFallthroughFilterTest, RefusesNullInputsInsteadOfReleasing)
{
    QKeyEvent ev = overrideFor(QKeySequence::Undo);
    // Fail CLOSED: with nothing to inspect, do not release. A null document
    // releasing would silently change shortcut routing wherever the filter is
    // installed on an editor whose document went away.
    EXPECT_FALSE(UndoFallthroughFilter::shouldRelease(nullptr, &ev));
    QTextDocument doc;
    EXPECT_FALSE(UndoFallthroughFilter::shouldRelease(&doc, nullptr));
}

TEST(UndoFallthroughFilterTest, WorksForPlainTextEditToo)
{
    // The annotation contents editor is a QTextEdit and the sticky-note popup's
    // is too, but the rule is about a QTextDocument, so the QPlainTextEdit
    // constructor must reach the same decision -- enumerated by the property,
    // not by the two widgets that happen to be wired today.
    QPlainTextEdit plain;
    UndoFallthroughFilter filter(&plain);
    QKeyEvent ev = overrideFor(QKeySequence::Undo);
    EXPECT_TRUE(UndoFallthroughFilter::shouldRelease(plain.document(), &ev));
    QTextCursor cursor(plain.document());
    cursor.insertText("typed");     // setPlainText would reset the stack
    ASSERT_TRUE(plain.document()->isUndoAvailable())
        << "the insertion must be undoable, or the assertion below is vacuous";
    EXPECT_FALSE(UndoFallthroughFilter::shouldRelease(plain.document(), &ev));
}

// ── The WIRING, which the tests above cannot see ─────────────────────────────
//
// Everything above tests the DECISION (`shouldRelease`). All of it stays green
// if someone deletes the `installEventFilter` call, because the rule would still
// be correct and simply never consulted — the `gates_cannot_judge_completeness`
// shape, where a fix is verified at the wrong level. These two tests send a real
// QEvent::ShortcutOverride at the widget the product builds and assert on who
// claims it, so removing the wiring reddens them.
//
// `isAccepted()` is the assertion because it is exactly what Qt's shortcut map
// reads: an accepted ShortcutOverride means "this key is mine, do not run the
// shortcut". Unaccepted means the QAction fires.

TEST(UndoFallthroughWiringTest, TheAnnotationContentsEditorReleasesADeadCtrlZ)
{
    pdfe::ui::AnnotationPropertiesWidget widget;
    auto* edit = widget.findChild<QTextEdit*>(QStringLiteral("annotContentsEdit"));
    ASSERT_NE(edit, nullptr)
        << "annotContentsEdit is gone or renamed; this test can no longer see "
           "the widget GitHub #295 is about";
    ASSERT_FALSE(edit->document()->isUndoAvailable())
        << "a freshly built properties panel must have an empty editor undo "
           "stack, or the assertion below is testing the wrong state";

    QKeyEvent ev = overrideFor(QKeySequence::Undo);
    QCoreApplication::sendEvent(edit, &ev);
    EXPECT_FALSE(ev.isAccepted())
        << "the editor claimed Ctrl+Z while having nothing to undo, so the "
           "window's undo action will not fire. That is GitHub #295 exactly: "
           "measured live as the key arriving at annotContentsEdit with "
           "undoEnabled=1 canUndo=1 undoDepth=3 and nothing being undone. "
           "Is UndoFallthroughFilter still installed on this editor?";
}

TEST(UndoFallthroughWiringTest, TheStickyNotePopupEditorReleasesADeadCtrlZ)
{
    // The SECOND wiring site. Added because the `claim-check` gate FAILED this
    // phase for exactly the gap it closes: the fix was installed on this editor
    // and said so in three places, while every test targeted only
    // AnnotationPropertiesWidget. Deleting the StickyNotePopup
    // installEventFilter line would have left all 6,569 tests green and
    // reopened GitHub #295 for sticky notes silently -- a fix covering one of
    // two call sites, verified at the wrong level.
    pdfe::ui::StickyNotePopup popup;
    auto* edit = popup.findChild<QTextEdit*>(QStringLiteral("stickyNotePopupEdit"));
    ASSERT_NE(edit, nullptr)
        << "stickyNotePopupEdit is gone or renamed; this test can no longer see "
           "the second widget the #295 fix is installed on";
    ASSERT_FALSE(edit->document()->isUndoAvailable())
        << "a freshly built popup must have an empty editor undo stack, or the "
           "assertion below is testing the wrong state";

    QKeyEvent ev = overrideFor(QKeySequence::Undo);
    QCoreApplication::sendEvent(edit, &ev);
    EXPECT_FALSE(ev.isAccepted())
        << "the sticky-note editor claimed Ctrl+Z while having nothing to undo, "
           "so the window's undo action will not fire. Is UndoFallthroughFilter "
           "still installed on this editor?";
}

TEST(UndoFallthroughWiringTest, TheStickyNotePopupEditorKeepsALiveCtrlZ)
{
    pdfe::ui::StickyNotePopup popup;
    auto* edit = popup.findChild<QTextEdit*>(QStringLiteral("stickyNotePopupEdit"));
    ASSERT_NE(edit, nullptr);

    QTextCursor cursor(edit->document());
    cursor.insertText(QStringLiteral("note text the user typed"));
    ASSERT_TRUE(edit->document()->isUndoAvailable());

    QKeyEvent ev = overrideFor(QKeySequence::Undo);
    QCoreApplication::sendEvent(edit, &ev);
    EXPECT_TRUE(ev.isAccepted())
        << "a user typing a sticky note MUST keep Ctrl+Z for their typing; "
           "releasing it here would undo the document instead of their text, "
           "which is a worse defect than #295";
}

TEST(UndoFallthroughWiringTest, TheAnnotationContentsEditorKeepsALiveCtrlZ)
{
    pdfe::ui::AnnotationPropertiesWidget widget;
    auto* edit = widget.findChild<QTextEdit*>(QStringLiteral("annotContentsEdit"));
    ASSERT_NE(edit, nullptr);

    QTextCursor cursor(edit->document());
    cursor.insertText(QStringLiteral("the user typed this"));
    ASSERT_TRUE(edit->document()->isUndoAvailable());

    QKeyEvent ev = overrideFor(QKeySequence::Undo);
    QCoreApplication::sendEvent(edit, &ev);
    EXPECT_TRUE(ev.isAccepted())
        << "the editor has an edit of its own and MUST keep Ctrl+Z. If this "
           "fails, the filter releases unconditionally and every text field in "
           "the product just lost its undo -- a worse defect than #295.";
}

}  // namespace
