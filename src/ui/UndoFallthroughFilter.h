#pragma once

// GitHub #295 — let Ctrl+Z reach the DOCUMENT when the focused text editor has
// nothing of its own to undo.
//
// MEASURED, not reasoned from code (docs/audit/p96-phase10/raw-i295-*.txt):
// creating an annotation calls AnnotationPropertiesWidget::focusContents(),
// which focuses `annotContentsEdit`. With that focus a Ctrl+Z arrives as
//
//   KeyPress key=0x5a mods=Control target=annotContentsEdit[QTextEdit]
//   focus=annotContentsEdit[QTextEdit] undoEnabled=1 canUndo=1 undoDepth=3
//
// and then nothing: no QEvent::Shortcut, no undo, the annotation count
// unchanged. The identical keystroke with focus on the canvas produces
// ShortcutOverride -> QEvent::Shortcut -> undoDepth 3 -> 2 and removes the
// annotation. So undoAction_ is enabled and the stack is populated; the
// Qt::WindowShortcut simply loses to the focused editor, which claims Ctrl+Z
// for its own undo and -- having nothing to undo -- does nothing with it.
//
// WHY THIS SHAPE OF FIX. The editor's claim is correct whenever the editor
// really has an edit to undo: a user typing a note expects Ctrl+Z to take back
// their typing, and promoting undoAction_ to an application-wide shortcut would
// break that for every text field in the product. The keystroke is only dead
// when the editor's own undo stack is EMPTY, and that is exactly the case this
// filter releases. Nothing else changes.
//
// HOW. Qt asks the focus widget first via QEvent::ShortcutOverride: accepting
// it means "this key is mine, do not run the shortcut". QTextEdit/QLineEdit
// accept it for Ctrl+Z. Consuming the override here WITHOUT accepting it leaves
// the key un-claimed, so the shortcut map proceeds and the QAction fires. Only
// ShortcutOverride is touched -- a KeyPress that still arrives is left strictly
// alone, so an editor that does hold the key behaves exactly as before.

#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QObject>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <QTextEdit>

namespace pdfe::ui {

/// Releases Ctrl+Z / Ctrl+Y (and their platform equivalents) to the window's
/// undo action when the watched text editor has no undo of its own.
///
/// Parent-owned: construct with the editor as parent and install it on that
/// same editor. Works for QTextEdit and QPlainTextEdit -- both expose a
/// QTextDocument, which is what carries the availability flags.
class UndoFallthroughFilter : public QObject {
public:
    explicit UndoFallthroughFilter(QTextEdit* edit)
        : QObject(edit), doc_(edit ? edit->document() : nullptr) {}
    explicit UndoFallthroughFilter(QPlainTextEdit* edit)
        : QObject(edit), doc_(edit ? edit->document() : nullptr) {}

    /// Exposed for the unit test, which must be able to drive the decision
    /// without a window, a shortcut map, or a focus chain -- those are what
    /// make the live behaviour expensive to test and easy to get wrong.
    /// Returns true when the event should be released to the shortcut.
    [[nodiscard]] static bool shouldRelease(const QTextDocument* doc,
                                            const QKeyEvent* ke)
    {
        if (!doc || !ke) return false;
        const bool undo = ke->matches(QKeySequence::Undo);
        const bool redo = ke->matches(QKeySequence::Redo);
        if (!undo && !redo) return false;
        // Release ONLY when the editor has nothing of its own to undo/redo.
        return undo ? !doc->isUndoAvailable() : !doc->isRedoAvailable();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::ShortcutOverride) {
            return QObject::eventFilter(watched, event);
        }
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!shouldRelease(doc_, ke)) {
            return QObject::eventFilter(watched, event);
        }
        // Consume the override without accepting it: the key is not claimed,
        // so Qt's shortcut map runs and the window's undo QAction fires.
        return true;
    }

private:
    const QTextDocument* doc_ = nullptr;
};

}  // namespace pdfe::ui
