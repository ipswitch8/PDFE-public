// -*- coding: utf-8 -*-
// Pipeline f3c91a2e phase-7 (GAP-A14): reusable test-only introspection
// seam for Qt modal dialogs.
//
// On the Windows-Server-2019 / RDP CI session pywinauto's UIA backend
// cannot reliably ENUMERATE Qt modal QDialogs (FindAll blocks, or the
// dialog evades the UIA tree entirely), so a pywinauto test cannot assert
// that a dialog opened with the expected child widgets. This header gives
// every dialog a one-line opt-in: at the end of its constructor it calls
// registerDialogIntrospection(this), which records a pipe-delimited
// inventory of the dialog's child-widget objectNames keyed by the dialog's
// own objectName, and (when PDFE_TEST_DIALOG_AUTODISMISS is set) schedules
// QDialog::reject() on the next event-loop turn so a modal exec() returns
// immediately and the main event loop is free to answer the readback pipe.
//
// MainWindow exposes ONE generic query/result pipe pair
// (testDialogIntrospectionQuery / testDialogIntrospectionResult): a test
// writes a dialog objectName to the query pipe and reads that dialog's
// last-recorded inventory from the result pipe (via UIA ValuePattern).
//
// Everything here is gated by PDFE_TEST_PIPES and is inert in production
// (the env var is set only by the test harness, never shipped). Header-only
// with C++17 inline semantics so dialogs in different static libs share a
// single registry instance once linked into pdfe.exe.
#pragma once

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTextEdit>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>

#include <memory>

namespace pdfe::ui::testseam {

// ---------------------------------------------------------------------------
// SEC-45 / GitHub #55 -- the test pipes are a COMPILE-TIME capability.
//
// This header used to say the seam "is inert in production (the env var is set
// only by the test harness, never shipped)". The CODE ships; the environment
// variable is set by whoever launches the process. So in a released build any
// local process that can set PDFE_TEST_PIPES and start PDFE got a control
// channel into it -- the pipes let an external writer activate tools by
// objectName and read back dialog inventories. That is a small attack surface,
// but it is one that exists for no reason in a shipped binary.
//
// POLARITY -- stated correctly here after GitHub #182 found it stated
// backwards. `PDFE_TESTING` is `option(... ON)` (CMakeLists.txt), so
// `kTestPipesCompiledIn` is TRUE by default and is false ONLY in a tree
// configured with -DPDFE_TESTING=OFF. The previous wording ("false unless the
// build was configured with -DPDFE_TESTING=ON") inverted that, and the sentence
// that followed it asserted a release-configured build already existed. It did
// not: at the time #182 was filed NO build path in this repository passed
// -DPDFE_TESTING=OFF -- not a preset, not a workflow, not the signing script --
// so every shipped installer, v0.2.0 included, carried the seam.
//
// `testPipesEnabled()` short-circuits on `kTestPipesCompiledIn` with
// `if constexpr`, so in an OFF-configured tree the environment variable is not
// even read and every guarded block is statically dead.
//
// WHERE THE OFF TREE COMES FROM. The flag is CONFIGURE-scoped rather than
// per-target on purpose: pdfe_ui is a static library linked into BOTH pdfe.exe
// and pdfe_tests.exe, so one compilation serves both and a per-target
// definition could not differ -- and the seam is not confined to pdfe_ui
// anyway, since dialogs in several libraries include this header. So the
// packaged binary is built from a SECOND configure tree:
//
//     cmake --preset windows-vs2022-package     # build/vs2022-package
//
// which sets PDFE_TESTING=OFF. `scripts/sign/Build-Release-Signed.ps1`
// defaults to that tree and REFUSES to package a tree whose CMakeCache says
// PDFE_TESTING:BOOL=ON, and tools/gate/check_packaging_testing_flag.py holds
// every packaging path to it in fast-checks.
// ---------------------------------------------------------------------------
inline constexpr bool kTestPipesCompiledIn =
#ifdef PDFE_TESTING
    true;
#else
    false;
#endif

inline bool testPipesEnabled()
{
    if constexpr (!kTestPipesCompiledIn) {
        return false;
    } else {
        return qEnvironmentVariableIsSet("PDFE_TEST_PIPES");
    }
}

// ---------------------------------------------------------------------------
// GitHub #136 / CI-LIMITATION-VISUAL-REGRESSION-SESSION-DRIFT.
//
// MainWindow's constructor ends with restoreGeometry(appSettings_->
// windowGeometry()) + restoreState(appSettings_->windowState()) -- the dock
// layout the user last dragged the panels to, persisted in an INI under the
// user profile. That is correct for a person and fatal for a screenshot
// baseline: the DOCK WIDTHS therefore differ between machines, between user
// profiles (the CI runner's service account is a different profile), and
// between runs of the same suite if anything moved a splitter.
//
// The visual-regression tests were recorded as failing because of "sub-pixel
// rendering / font hinting / colour profile differences between sessions".
// They were not. Measured 2026-09-12 in the SAME interactive session the
// baselines were captured in: SSIM 0.1835, pixel_fraction 0.4519 -- 45% of
// pixels differ, which anti-aliasing cannot do. The saved diff shows the same
// window with the docks at different widths, displacing the whole capture.
//
// When this is set the window state restore is SKIPPED and a fixed dock
// layout is applied instead, so the canvas rectangle -- and therefore the
// captured region and the page's rendered size within it -- is identical on
// every run, on every profile, on every machine at the same window size.
// Determinism at source, rather than a threshold loosened to accommodate the
// noise.
//
// Same compile-time gate as everything else here: statically dead in a tree
// configured with -DPDFE_TESTING=OFF, so nothing new ships.
inline bool testDeterministicLayoutEnabled()
{
    if constexpr (!kTestPipesCompiledIn) {
        return false;
    } else {
        return qEnvironmentVariableIsSet("PDFE_TEST_DETERMINISTIC_LAYOUT");
    }
}

//: Fixed left/right dock widths (logical px) applied under
//: testDeterministicLayoutEnabled(). Values are arbitrary but must never
//: change without regenerating every committed screenshot baseline.
inline constexpr int kTestDeterministicLeftDockWidth  = 200;
inline constexpr int kTestDeterministicRightDockWidth = 260;


// Forward declarations — defined later in this header but referenced from
// registerDialogIntrospection() below.
inline QMap<QString, QStringList>& dialogActionQueue();
inline void installMessageBoxAutoDismisser(QObject* ctx);
inline void runDialogActions(QPointer<QDialog> dlg, QStringList script);

/// The process-wide registry: dialog objectName -> recorded inventory
/// string. Function-local static so there is exactly one instance across
/// all translation units that include this header.
inline QMap<QString, QString>& dialogRegistry()
{
    static QMap<QString, QString> registry;
    return registry;
}

/// Record @p dlg's child-widget inventory and, when
/// PDFE_TEST_DIALOG_AUTODISMISS is set, schedule its dismissal so a modal
/// exec() returns at once. No-op unless PDFE_TEST_PIPES is set. Call at the
/// END of a dialog constructor (after all children are parented).
///
/// The recorded string is:
///   "<dialogObjectName>|<childName1>,<childName2>,..."
/// listing every descendant QWidget that has a non-empty objectName.
inline void registerDialogIntrospection(QDialog* dlg)
{
    if (!dlg || !pdfe::ui::testseam::testPipesEnabled()) {
        return;
    }
    // Idempotent per dialog instance. Several dialogs are registered BOTH from
    // their own constructor (the one-line opt-in) AND from an explicit
    // MainWindow call at the exec() site (legacy, e.g. onCompliance()). Without
    // this guard the second call would: (a) re-take() the action queue —
    // finding it empty and then scheduling an AUTODISMISS reject() that races
    // the first call's action replay and closes the dialog mid-flow; and
    // (b) create a SECOND keyboard-state pipe pair with duplicate objectNames.
    // First call wins; subsequent calls on the same instance are no-ops.
    if (dlg->property("__pdfeSeamRegistered__").toBool()) {
        return;
    }
    dlg->setProperty("__pdfeSeamRegistered__", true);
    QStringList names;
    const QList<QWidget*> kids = dlg->findChildren<QWidget*>();
    for (QWidget* w : kids) {
        const QString on = w->objectName();
        if (!on.isEmpty()) {
            names << on;
        }
    }
    // Also record each named QComboBox's items so tests can assert combo
    // contents (e.g. the ComplianceDialog LevelCombo must offer PDF/A-1b).
    // Section format appended after a "||combos=" marker:
    //   "||combos=Combo1:item1;item2@@Combo2:itemA;itemB"
    QStringList comboParts;
    const QList<QComboBox*> combos = dlg->findChildren<QComboBox*>();
    for (QComboBox* cb : combos) {
        const QString on = cb->objectName();
        if (on.isEmpty()) {
            continue;
        }
        QStringList items;
        for (int i = 0; i < cb->count(); ++i) {
            items << cb->itemText(i);
        }
        comboParts << on + QLatin1Char(':') + items.join(QLatin1Char(';'));
    }
    const QString key = dlg->objectName();
    QString record = key + QStringLiteral("|") + names.join(QLatin1Char(','));
    if (!comboParts.isEmpty()) {
        record += QStringLiteral("||combos=")
                  + comboParts.join(QStringLiteral("@@"));
    }
    dialogRegistry().insert(key, record);

    // -----------------------------------------------------------------------
    // Keyboard-state seam (GAP-A14 keyboard-test conversion).
    //
    // The 3 parametrized keyboard tests (opens_and_escape_closes,
    // tab_cycles_controls, default_button_responds_to_enter) need to verify a
    // modal dialog's open state, focus traversal, and default-button activation
    // WITHOUT enumerating the modal's child widgets through UIA — which
    // FindAll/descendants() cannot do on the Windows-Server-2019 CI session.
    //
    // We create a dialog-CHILD QLineEdit query/result pipe pair parented to the
    // dialog itself (so they receive UIA ValuePattern events while the modal is
    // up — a MainWindow-child pipe would not, because Qt modality blocks input
    // to other top-levels). A test writes a command to the query pipe; the
    // handler answers on the result pipe:
    //   focus  -> objectName of QApplication::focusWidget() (walking up parents
    //             until a non-empty objectName, else className, else "none")
    //   modal  -> "1" if this dialog is the active modal widget, else "0"
    //   parentVisible
    //          -> "1"/"0" for whether this dialog's parent window is on screen
    //             WHILE the modal loop runs, or "none" if it has no parent.
    //             The one question that can only be answered from inside
    //             exec(): afterwards the window is shown either way.
    //   tab    -> dlg->focusNextChild() then the new focused objectName (same
    //             resolution as focus) — exercises Qt's real focus/tab chain
    //             deterministically (Win32 key delivery to a modal is flaky)
    //   enter  -> click the dialog's default QPushButton; returns
    //             "clicked:<objectName>" or "nodefault"
    // Inert in production (this whole function early-returns unless
    // PDFE_TEST_PIPES is set).
    auto* kbQuery = new QLineEdit(dlg);
    kbQuery->setObjectName(QStringLiteral("testDialogKeyboardQueryPipe"));
    kbQuery->setAccessibleName(QStringLiteral("Test Dialog Keyboard Query"));
    kbQuery->setFixedSize(0, 0);
    kbQuery->setMaximumSize(0, 0);
    kbQuery->setFocusPolicy(Qt::NoFocus);

    auto* kbResult = new QLineEdit(dlg);
    kbResult->setObjectName(QStringLiteral("testDialogKeyboardResultPipe"));
    kbResult->setAccessibleName(QStringLiteral("Test Dialog Keyboard Result"));
    kbResult->setFixedSize(0, 0);
    kbResult->setMaximumSize(0, 0);
    kbResult->setFocusPolicy(Qt::NoFocus);
    kbResult->setReadOnly(true);

    QPointer<QDialog> kbDlg(dlg);

    // -----------------------------------------------------------------------
    // GitHub #80: the seam MUST NOT be the thing it is measuring.
    //
    // A test writes a command by driving the query pipe through UIA, and
    // pywinauto's set_edit_text() calls set_focus() on that QLineEdit first.
    // So by the time the handler below runs, QApplication::focusWidget() is
    // the QUERY PIPE — not whatever the dialog's tab chain had reached. The
    // consequence was measured on PreferencesDialog: twelve consecutive "tab"
    // commands all replied "qt_tabwidget_tabbar", because every one of them
    // restarted from the pipe and took the same first step. A test asserting
    // ">= 2 distinct controls" still passed (initial focus + the tabbar), so
    // a Tab cycle that never advanced read as green.
    //
    // Fix: track the last focused widget that is a LEGITIMATE focus target and
    // drive traversal from that. A widget with Qt::NoFocus can never be
    // reached by the tab chain, so a NoFocus widget holding focus is by
    // definition an artifact of the instrumentation -- which is exactly what
    // every pipe here is (all are created with setFocusPolicy(Qt::NoFocus)).
    auto seamFocus = std::make_shared<QPointer<QWidget>>();
    QObject::connect(
        qApp, &QApplication::focusChanged, dlg,
        [seamFocus](QWidget* /*old*/, QWidget* now) {
            if (now && now->focusPolicy() != Qt::NoFocus) {
                *seamFocus = now;
            }
        });

    QObject::connect(
        kbQuery, &QLineEdit::textChanged, dlg,
        [kbDlg, kbQuery, kbResult, seamFocus](const QString& command) {
            if (command.isEmpty() || !kbDlg) {
                return;
            }
            // Resolve a focusable widget to a stable name: its own
            // objectName, else the first ancestor with a non-empty
            // objectName, else its className, else "none".
            //
            // Qt's own internal children carry auto-generated objectNames
            // ("qt_spinbox_lineedit", "qt_tabwidget_tabbar", ...). Those are
            // NOT addressable identities: all three PreferencesDialog spin
            // boxes resolve to the single name "qt_spinbox_lineedit", which
            // would collapse three distinct controls into one entry and make
            // a set-equality assertion blind to two of them. Walk past any
            // "qt_" name to the first application-assigned objectName.
            auto resolveName = [](QWidget* w) -> QString {
                if (!w) {
                    return QStringLiteral("none");
                }
                for (QWidget* p = w; p; p = p->parentWidget()) {
                    const QString on = p->objectName();
                    if (!on.isEmpty()
                        && !on.startsWith(QLatin1String("qt_"))) {
                        return on;
                    }
                }
                return QString::fromLatin1(w->metaObject()->className());
            };
            // The widget traversal should proceed from: the last legitimate
            // focus target, else whatever the dialog reports, else the dialog.
            auto traversalOrigin = [&]() -> QWidget* {
                if (QWidget* w = seamFocus->data()) {
                    return w;
                }
                if (QWidget* w = kbDlg->focusWidget()) {
                    if (w->focusPolicy() != Qt::NoFocus) {
                        return w;
                    }
                }
                return static_cast<QWidget*>(kbDlg.data());
            };

            QString reply;
            if (command == QLatin1String("focus")) {
                reply = resolveName(traversalOrigin());
            } else if (command == QLatin1String("focusable")) {
                // GitHub #80: the set of controls a Tab cycle OUGHT to visit.
                //
                // Membership is "the user can put focus here" --
                // focusPolicy() != Qt::NoFocus, visible and enabled -- NOT
                // "TabFocus is set". That distinction is the whole point: a
                // control that is focusable by CLICK but skipped by TAB is a
                // keyboard-accessibility defect, and defining the expected set
                // by TabFocus would delete such a control from the expectation
                // at the same moment it disappeared from the observation, so
                // the two would agree and the defect would pass unseen.
                QStringList seen;
                const QList<QWidget*> kids =
                    kbDlg->findChildren<QWidget*>();
                for (QWidget* w : kids) {
                    if (!w || w->focusPolicy() == Qt::NoFocus) {
                        continue;
                    }
                    if (!w->isVisible() || !w->isEnabled()) {
                        continue;
                    }
                    const QString n = resolveName(w);
                    if (n.isEmpty() || seen.contains(n)) {
                        continue;
                    }
                    seen << n;
                }
                reply = QStringLiteral("focusable:") + seen.join(QLatin1Char(','));
            } else if (command == QLatin1String("modal")) {
                QWidget* active = QApplication::activeModalWidget();
                reply = (qobject_cast<QDialog*>(active)
                         == static_cast<QDialog*>(kbDlg.data()))
                            ? QStringLiteral("1")
                            : QStringLiteral("0");
            } else if (command == QLatin1String("parentVisible")) {
                // Is the window this dialog belongs to actually on screen
                // RIGHT NOW, while the modal loop is running?
                //
                // The only question that has to be answered from INSIDE
                // exec(), because it is about a transient state: a dialog
                // reporting a failure in front of a window that was never
                // shown. Afterwards the window gets shown regardless and the
                // end state is identical, so every after-the-fact assertion
                // passes whether or not the defect is present.
                //
                // Added because a gate review pointed out that the ordering in
                // WindowManager::openPath was recorded as "unguardable" when
                // this seam — already registered on the very dialog in
                // question — could answer it. "No mechanism exists" was wrong;
                // the mechanism existed and had not been asked.
                //
                // Replies "none" when the dialog has no parent widget, which is
                // itself the answer for a parentless failure dialog.
                QWidget* parent = kbDlg ? kbDlg->parentWidget() : nullptr;
                reply = parent ? (parent->isVisible() ? QStringLiteral("1")
                                                      : QStringLiteral("0"))
                               : QStringLiteral("none");
            } else if (command == QLatin1String("tab")) {
                // Advance focus through the dialog's REAL focus/tab chain by
                // delivering a synthetic Tab key event to the currently
                // focused widget (or the dialog).  Qt's QWidget handler calls
                // the protected focusNextPrevChild() internally, exercising
                // the genuine tab order — deterministic and not dependent on
                // flaky Win32 key delivery to a modal.
                //
                // The Tab goes to the last LEGITIMATE focus target (see the
                // seamFocus comment above), and real keyboard focus is handed
                // back to it first -- so the step starts where the tab chain
                // actually was, not on the query pipe UIA just focused, and so
                // real Win32 keystrokes sent after this command land on the
                // control the reply names.
                QWidget* target = traversalOrigin();
                if (target && target->focusPolicy() != Qt::NoFocus) {
                    target->setFocus(Qt::TabFocusReason);
                }
                QKeyEvent press(QEvent::KeyPress, Qt::Key_Tab,
                                Qt::NoModifier);
                QApplication::sendEvent(target, &press);
                QKeyEvent release(QEvent::KeyRelease, Qt::Key_Tab,
                                  Qt::NoModifier);
                QApplication::sendEvent(target, &release);
                reply = resolveName(traversalOrigin());
            } else if (command.startsWith(QLatin1String("text:"))) {
                // Read a named child widget's TEXT while the modal is up.
                //
                // The inventory recorded above answers "which widgets exist";
                // this answers "what do they SAY", which is the difference
                // between an oracle that proves a dialog OPENED and one that
                // proves it opened with the RIGHT CONTENT. The region-OCR test
                // needs the latter: "a dialog appeared with text in it" cannot
                // distinguish a correctly-placed region from an over-large one,
                // because both recognise something. Comparing the recognised
                // string against a known page block can.
                //
                // Newlines are escaped to \n because the transport is a
                // single-line QLineEdit read through UIA ValuePattern; an
                // embedded newline would silently truncate the reply.
                const QString name = command.mid(5);
                QString value;
                bool found = false;
                if (QWidget* w = kbDlg->findChild<QWidget*>(name)) {
                    found = true;
                    if (auto* te = qobject_cast<QTextEdit*>(w)) {
                        value = te->toPlainText();
                    } else if (auto* pte = qobject_cast<QPlainTextEdit*>(w)) {
                        value = pte->toPlainText();
                    } else if (auto* le = qobject_cast<QLineEdit*>(w)) {
                        value = le->text();
                    } else if (auto* lb = qobject_cast<QLabel*>(w)) {
                        value = lb->text();
                    } else if (auto* ab = qobject_cast<QAbstractButton*>(w)) {
                        value = ab->text();
                    } else {
                        // Present but carries no text accessor. Distinct from
                        // absent: a test asserting on content must be able to
                        // tell "wrong widget name" from "widget has no text".
                        reply = QStringLiteral("notext:") + name;
                        found = false;
                    }
                }
                if (found) {
                    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
                    value.replace(QLatin1Char('\n'), QLatin1String("\\n"));
                    reply = QStringLiteral("text:") + value;
                } else if (reply.isEmpty()) {
                    reply = QStringLiteral("nowidget:") + name;
                }
            } else if (command == QLatin1String("enter")) {
                QPushButton* def = nullptr;
                const QList<QPushButton*> btns =
                    kbDlg->findChildren<QPushButton*>();
                for (QPushButton* b : btns) {
                    if (b->isDefault()) {
                        def = b;
                        break;
                    }
                }
                if (def) {
                    // The default button's click slot may close (and thereby
                    // destroy/hide) THIS dialog — taking the result pipe with
                    // it before the test can read it. So ALSO stash the reply
                    // in the process-static registry under a reserved key; the
                    // test reads it back via MainWindow's
                    // testDialogIntrospectionQuery pipe AFTER the dialog has
                    // closed (when MainWindow-child pipes are readable again).
                    reply = QStringLiteral("clicked:") + def->objectName();
                    dialogRegistry().insert(
                        QStringLiteral("__lastDefaultClick__"), reply);
                    kbResult->blockSignals(true);
                    kbResult->setText(reply);
                    kbResult->blockSignals(false);
                    kbQuery->blockSignals(true);
                    kbQuery->clear();
                    kbQuery->blockSignals(false);
                    QPointer<QPushButton> defGuard(def);
                    QTimer::singleShot(0, def, [defGuard]() {
                        if (defGuard) {
                            defGuard->click();
                        }
                    });
                    return;
                }
                reply = QStringLiteral("nodefault");
                dialogRegistry().insert(
                    QStringLiteral("__lastDefaultClick__"), reply);
            } else {
                reply = QStringLiteral("unknown:") + command;
            }

            // GitHub #80: hand REAL keyboard focus back before returning.
            //
            // Driving the query pipe through UIA leaves Qt focus sitting on
            // that pipe, so a genuine Win32 keystroke sent by the test right
            // after a seam call would be delivered to the pipe rather than to
            // the control the seam just named. That silently produced a
            // "changed a setting by keyboard" step in which nothing changed.
            // Only a NoFocus holder is corrected -- a NoFocus widget can never
            // legitimately hold focus, so this can never move focus away from
            // a control the user (or the tab chain) actually put it on.
            if (QWidget* restore = seamFocus->data()) {
                QWidget* now = QApplication::focusWidget();
                if (now && now->focusPolicy() == Qt::NoFocus
                    && now != restore && restore->isVisible()
                    && restore->isEnabled()) {
                    restore->setFocus(Qt::OtherFocusReason);
                }
            }

            kbResult->blockSignals(true);
            kbResult->setText(reply);
            kbResult->blockSignals(false);

            kbQuery->blockSignals(true);
            kbQuery->clear();
            kbQuery->blockSignals(false);
        });

    // A queued action script (set via MainWindow's testDialogAction pipe
    // before this dialog opened) takes priority over AUTODISMISS: replay it
    // against the real widgets on the dialog's own event loop. The script is
    // responsible for closing the dialog (its final "accept"/"reject", or a
    // "click" whose slot calls accept()).
    const QStringList script = dialogActionQueue().take(key);
    if (!script.isEmpty()) {
        installMessageBoxAutoDismisser(dlg);
        QPointer<QDialog> guarded(dlg);
        QStringList captured = script;
        QTimer::singleShot(0, dlg, [guarded, captured]() mutable {
            runDialogActions(guarded, captured);
        });
        return;
    }

    if (qEnvironmentVariableIsSet("PDFE_TEST_DIALOG_AUTODISMISS")) {
        // Defer to the next event-loop turn so it fires inside exec()'s
        // nested modal loop, taking the Cancel/Close path.
        QTimer::singleShot(0, dlg, &QDialog::reject);
    }
}

/// Return the last-recorded inventory for the dialog whose objectName is
/// @p dialogObjectName, or an empty string if none has been recorded.
inline QString dialogIntrospection(const QString& dialogObjectName)
{
    return dialogRegistry().value(dialogObjectName);
}

// ---------------------------------------------------------------------------
// File-dialog path injection (GAP-A14 / SKIP=FAIL conversion of the
// file_dialog_flows suite).
//
// The native Win32 IFileSaveDialog / IFileOpenDialog (#32770 shell modal)
// that QFileDialog::getSaveFileName/getOpenFileName show cannot be driven
// reliably on the non-interactive Server-2019 CI session, which forced the
// whole file_dialog_flows suite to pytest.skip. Instead of driving the
// native modal, a test pushes the path it WANTS the next save/open dialog to
// return onto this process-static FIFO (via MainWindow's testFileDialogPath
// pipe). Each getSaveFileName/getOpenFileName call site first asks
// takeFileDialogPath(): a non-empty result means "a test pre-selected this
// path — return it without showing any modal"; an empty result means "no
// injection pending — show the real dialog". Inert in production (the queue
// is only ever filled through a PDFE_TEST_PIPES-gated pipe).
// ---------------------------------------------------------------------------

inline QStringList& fileDialogPathQueue()
{
    static QStringList queue;
    return queue;
}

/// Push a path the next file dialog should return. No-op for empty input.
inline void enqueueFileDialogPath(const QString& path)
{
    if (!path.isEmpty()) {
        fileDialogPathQueue().append(path);
    }
}

/// Pop the next injected file-dialog path, or an empty string if none is
/// queued (meaning the caller should show the real QFileDialog).
inline QString takeFileDialogPath()
{
    QStringList& queue = fileDialogPathQueue();
    if (queue.isEmpty()) {
        return QString();
    }
    return queue.takeFirst();
}

// ---------------------------------------------------------------------------
// Modal-dialog action scripting (drives a Qt modal QDialog's real widgets
// without any UIA enumeration of the modal — which FindAll cannot do on the
// CI session).
//
// Before opening a dialog (while MainWindow is NOT yet modal, so its child
// pipe still receives UIA events) a test enqueues an action script keyed by
// the dialog's objectName via MainWindow's testDialogAction pipe. When the
// dialog's constructor calls registerDialogIntrospection(this), any queued
// script for that objectName is replayed on the dialog's own event loop
// (QTimer::singleShot inside exec()), exercising the genuine widgets:
//   "combo:Name=Value"  select item Value (by text) in QComboBox Name
//   "edit:Name=Text"    set QLineEdit / QPlainTextEdit Name to Text
//   "spin:Name=N"       set QSpinBox / QDoubleSpinBox Name to N
//   "check:Name=1|0"    set QCheckBox / checkable QAbstractButton Name
//   "click:Name"        click QAbstractButton Name (fires its real slot)
//   "accept" / "reject" close the dialog via QDialog::accept()/reject()
// A button click may spawn a nested QMessageBox (e.g. the post-convert
// "Conversion Complete" info box); a short-lived auto-dismisser closes any
// such QMessageBox so the script can proceed and exec() can return.
// ---------------------------------------------------------------------------

inline QMap<QString, QStringList>& dialogActionQueue()
{
    static QMap<QString, QStringList> queue;
    return queue;
}

/// Enqueue an action @p script for the dialog whose objectName is @p key.
/// Overwrites any prior pending script for that key. No-op on empty input.
inline void enqueueDialogActions(const QString& key, const QStringList& script)
{
    if (key.isEmpty() || script.isEmpty()) {
        return;
    }
    dialogActionQueue().insert(key, script);
}

/// Close any currently-active modal QMessageBox by accepting it. Runs as a
/// short repeating timer while a dialog script is in flight.
inline void installMessageBoxAutoDismisser(QObject* ctx)
{
    QTimer* timer = new QTimer(ctx);
    timer->setObjectName(QStringLiteral("testMsgBoxAutoDismisser"));
    timer->setInterval(40);
    timer->setProperty("ticks", 0);
    QObject::connect(timer, &QTimer::timeout, timer, [timer]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (QMessageBox* mb = qobject_cast<QMessageBox*>(modal)) {
            // Prefer the default button (e.g. OK) so the box's normal
            // accept path runs exactly as a user click would.
            if (QPushButton* def = mb->defaultButton()) {
                def->click();
            } else {
                mb->accept();
            }
        }
        const int ticks = timer->property("ticks").toInt() + 1;
        timer->setProperty("ticks", ticks);
        if (ticks > 100) {  // ~4s safety cap
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}

/// Apply one parsed action verb to @p dlg. Unknown verbs / missing widgets
/// are silently ignored (best-effort, test-only).
inline void applyDialogAction(QDialog* dlg, const QString& action)
{
    if (!dlg) {
        return;
    }
    if (action == QLatin1String("accept")) {
        dlg->accept();
        return;
    }
    if (action == QLatin1String("reject")) {
        dlg->reject();
        return;
    }
    const int colon = action.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        return;
    }
    const QString verb = action.left(colon);
    const QString rest = action.mid(colon + 1);
    const int eq = rest.indexOf(QLatin1Char('='));
    const QString name = (eq >= 0) ? rest.left(eq) : rest;
    const QString value = (eq >= 0) ? rest.mid(eq + 1) : QString();
    QWidget* w = dlg->findChild<QWidget*>(name);
    if (!w) {
        return;
    }
    if (verb == QLatin1String("combo")) {
        if (QComboBox* cb = qobject_cast<QComboBox*>(w)) {
            // Exact match first, then a substring match so a test can pass a
            // short label ("PDF/A-2b") for a longer item ("PDF/A-2b
            // (Archival, Visual)"). currentIndexChanged still fires so Qt's
            // currentIndex (not just the UIA selection) updates.
            int idx = cb->findText(value);
            if (idx < 0) {
                idx = cb->findText(value, Qt::MatchContains);
            }
            if (idx >= 0) {
                cb->setCurrentIndex(idx);
            } else {
                cb->setCurrentText(value);
            }
        }
    } else if (verb == QLatin1String("edit")) {
        if (QLineEdit* le = qobject_cast<QLineEdit*>(w)) {
            le->setText(value);
        } else if (QPlainTextEdit* pte = qobject_cast<QPlainTextEdit*>(w)) {
            pte->setPlainText(value);
        }
    } else if (verb == QLatin1String("spin")) {
        if (QSpinBox* sb = qobject_cast<QSpinBox*>(w)) {
            sb->setValue(value.toInt());
        } else if (QDoubleSpinBox* dsb = qobject_cast<QDoubleSpinBox*>(w)) {
            dsb->setValue(value.toDouble());
        }
    } else if (verb == QLatin1String("check")) {
        if (QAbstractButton* b = qobject_cast<QAbstractButton*>(w)) {
            b->setChecked(value == QLatin1String("1")
                          || value.compare(QLatin1String("true"),
                                           Qt::CaseInsensitive) == 0);
        }
    } else if (verb == QLatin1String("click")) {
        if (QAbstractButton* b = qobject_cast<QAbstractButton*>(w)) {
            b->click();
        }
    } else if (verb == QLatin1String("listcheckall")) {
        // Tick every checkable row of a QListWidget (e.g. the calc-order
        // field list, whose Apply only commits rows whose checkState is
        // Checked). value, if numeric, caps how many rows are checked.
        if (QListWidget* lw = qobject_cast<QListWidget*>(w)) {
            bool ok = false;
            const int cap = value.toInt(&ok);
            int checked = 0;
            for (int i = 0; i < lw->count(); ++i) {
                if (ok && checked >= cap) {
                    break;
                }
                QListWidgetItem* item = lw->item(i);
                if (item && (item->flags() & Qt::ItemIsUserCheckable)) {
                    item->setCheckState(Qt::Checked);
                    ++checked;
                }
            }
        }
    }
}

/// Replay a queued action @p script against @p dlg, one verb per event-loop
/// turn so nested modals (file dialogs answered by injection, message boxes
/// closed by the auto-dismisser) settle between steps.
inline void runDialogActions(QPointer<QDialog> dlg, QStringList script)
{
    if (!dlg || script.isEmpty()) {
        return;
    }
    const QString action = script.takeFirst();
    applyDialogAction(dlg, action);
    if (!script.isEmpty() && dlg) {
        QTimer::singleShot(60, dlg, [dlg, script]() mutable {
            runDialogActions(dlg, script);
        });
    }
}

} // namespace pdfe::ui::testseam
