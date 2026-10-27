#pragma once

// GitHub #303 — a UIA write to a hidden test pipe steals keyboard focus, so any
// keystroke a test sends afterwards addresses the pipe instead of the widget
// under test.
//
// MEASURED, not reasoned from code (docs/audit/p96-phase10/raw-i295-*.txt, a
// qApp-level key trace). After any pipe write, every subsequent key event
// reports
//
//   focus=testKeyTraceQueryPipe[QLineEdit]
//
// Every pipe is built with `setFocusPolicy(Qt::NoFocus)` and `setFixedSize(0,
// 0)`, on the assumption that it cannot take keyboard focus. IT CAN. UIA's
// `ValuePattern::SetValue` -- which is how `tests/helpers/qinvoke.py` writes to
// a pipe -- focuses the control it writes to, and `Qt::NoFocus` does not
// prevent that. `Qt::NoFocus` governs focus from CLICKS and TAB, not from an
// accessibility client asserting focus directly.
//
// WHY IT IS WORSE THAN AN ORDINARY HARNESS BUG. It fabricates product failures
// and masks real ones at the same time. It produced #295's original headline --
// "1010 Ctrl+Z hotkeys removed 0 of 1000 annotations" -- because the stress
// loop arms its tool through a pipe, so focus sat on a hidden QLineEdit, which
// claims Ctrl+Z for its own undo. #295 then turned out to ALSO be a genuine
// product defect, found only once this interference was removed. A harness
// artefact that hides a real bug behind a fake one is the worse of the two
// failure modes, because the fake measurement looks like the explanation.
//
// Worse still, a pipe could destroy its own evidence: QLineEdit's Ctrl+Z undo
// RE-INSERTS the previous text, re-firing the pipe's `textChanged` handler. For
// the #295 trace pipe, whose previous text was `clear`, that wiped the record
// of the very keystroke that caused it -- two rounds of measurement read as "no
// key ever arrived".
//
// WHY A SINGLE APP-LEVEL GUARD AND NOT ~108 HANDLER EDITS. #303 suggests each
// pipe's `textChanged` handler hand focus back after dispatching. That is the
// right behaviour and the wrong place: there are 108 structural pipes across
// three files (the issue's "~25 pairs originating at MainWindow.cpp:673" is the
// subset that had been observed, and that line had already drifted), no shared
// creation helper, and every future pipe would have to remember. A fix sized to
// the observed failures leaves the property unfixed. This guard keys on the
// PROPERTY -- "focus landed on a test pipe" -- so it covers every pipe that
// exists and every pipe anyone adds later, and
// `tools/gate/check_test_pipe_naming.py` makes the population checkable rather
// than asserted.
//
// WHY IT IS NOT GATED ON `PDFE_TESTING`. GitHub #182: `PDFE_TESTING` is
// `option(... ON)` and no build path in this repository ever sets it OFF, so
// gating on it would be a comment rather than a control. Instead the guard is
// installed unconditionally and is INERT by construction in a build with no
// test pipes: `isTestPipe` cannot match a widget that does not exist. That is a
// property of the predicate, not a promise.

#include <QLineEdit>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

namespace pdfe::ui {

/// Hands keyboard focus back when a UIA write pulls it onto a hidden test pipe.
///
/// Install one per application, after `qApp` exists:
///
///     auto* guard = new TestPipeFocusGuard(canvasWidget, qApp);
///     guard->attach();
///
/// Parent-owned. `attach()` connects to `QApplication::focusChanged`, which is
/// the only signal that reports the widget focus came FROM -- and that previous
/// widget is what a keyboard test is actually addressing.
class TestPipeFocusGuard : public QObject {
public:
    explicit TestPipeFocusGuard(QWidget* fallback = nullptr,
                                QObject* parent = nullptr)
        : QObject(parent), fallback_(fallback) {}

    /// Is this widget one of the hidden test pipes?
    ///
    /// THE ENUMERABLE PROPERTY. A pipe is a `QLineEdit` whose `objectName`
    /// begins with `test` and ends with `Pipe` -- the convention all 108
    /// already follow, and which `tools/gate/check_test_pipe_naming.py` checks
    /// against the source tree by enumerating pipes STRUCTURALLY, so this
    /// predicate's coverage is a measured fact rather than a claim.
    ///
    /// Deliberately NOT keyed on `focusPolicy() == Qt::NoFocus` or on a zero
    /// size, tempting as both are: a real product widget may legitimately be
    /// either, and stealing focus back from a real widget would be a far worse
    /// defect than the one being fixed. The name is the narrow signal.
    [[nodiscard]] static bool isTestPipe(const QWidget* w)
    {
        if (!w) return false;
        if (!qobject_cast<const QLineEdit*>(w)) return false;
        const QString n = w->objectName();
        return n.startsWith(QStringLiteral("test"))
               && n.endsWith(QStringLiteral("Pipe"));
    }

    /// Where focus should go when a pipe has taken it.
    ///
    /// Pure, and exposed for the unit test, because the live behaviour needs a
    /// window, a focus chain and a UIA client -- which is exactly what makes it
    /// expensive to test and easy to get wrong. Returns nullptr when there is
    /// nothing to restore to, and the caller must then leave focus ALONE: a
    /// guard that forces focus somewhere arbitrary would invent a different
    /// wrong answer.
    [[nodiscard]] static QWidget* restoreTarget(QWidget* previous,
                                                QWidget* fallback)
    {
        // Prefer the widget focus came from -- that is what the test was
        // addressing before the pipe write interrupted it.
        if (previous && !isTestPipe(previous)) return previous;
        // `previous` was itself a pipe (two writes in a row) or there was no
        // previous focus at all: fall back to the canvas, which is where a real
        // user's keystrokes land.
        if (fallback && !isTestPipe(fallback)) return fallback;
        return nullptr;
    }

    /// Connect to the application's focus changes. Safe to call once.
    void attach();

    /// Test seam: drive one focus transition without a QApplication signal.
    /// Returns the widget focus was restored to, or nullptr if it was left
    /// alone -- so a test can assert the DECISION as well as the side effect.
    QWidget* onFocusChanged(QWidget* previous, QWidget* now);

private:
    QPointer<QWidget> fallback_;
    // Re-entrancy guard: `setFocus()` below emits `focusChanged` again, and
    // without this the restore would be re-entered with the pipe as
    // `previous`. Measured consequence of omitting it: the restore target
    // becomes the fallback instead of the widget under test, which is a subtle
    // wrong answer rather than a crash -- the kind that survives review.
    bool restoring_ = false;
};

}  // namespace pdfe::ui
