// -*- coding: utf-8 -*-
// GitHub #213 / meta-audit row WIN-71 — power-event (suspend/resume) handling.
//
// ===========================================================================
// SCOPE — READ THIS BEFORE ASSUMING THIS FILE DOES MORE THAN IT DOES
// ===========================================================================
//
// This is a DELIBERATELY PARTIAL handler. It covers exactly two transitions,
// PBT_APMSUSPEND and PBT_APMRESUMEAUTOMATIC, and it does exactly two things
// with them:
//
//   * On SUSPEND — runs the registered suspend hook. MainWindow registers one
//     that takes an autosave recovery snapshot, so unsaved work is recoverable
//     before the machine freezes. This is real, load-bearing behaviour and it
//     is the one thing full support would also have to do first.
//   * On RESUME — records that a resume happened, and the wall-clock duration
//     of the freeze. Nothing else. It does NOT re-validate file handles, does
//     NOT invalidate render tiles or the page cache, does NOT re-check the
//     MuPDF context, does NOT restart or re-phase timers, and does NOT
//     re-acquire the single-instance mutex.
//
// Those omissions are the honest state, not an accident, and
// tests/test_power_events.cpp asserts them as omissions rather than letting a
// reader assume coverage. The full analysis of what complete suspend/resume
// support would require — and why each part was left out of this phase — is
// docs/power-event-handling-scope.md. **#213 is NOT closed by this file.**
//
// WHY NOT A QT SIGNAL
// -------------------
// Qt has no cross-platform suspend/resume notification. WM_POWERBROADCAST is
// delivered to a window procedure and QAbstractNativeEventFilter is the only
// Qt-available route to it. That makes this the opposite situation from
// session-end (WIN-68), where Qt DID offer a portable signal
// (QGuiApplication::commitDataRequest) and MainWindow uses it.
//
// The testability lesson from WIN-68 is kept even so, and it is the reason
// this file is split the way it is. GitHub #97 recorded that a row's stated
// ENVIRONMENT ("two Windows sessions", here "a VM you can really suspend") is
// usually not its REQUIREMENT. The requirement here is "the app is told a
// suspend happened and reacts". So:
//
//   * classifyPowerBroadcast() is a PURE FUNCTION of (message, wParam). All
//     the Win32 knowledge lives in it, and a test drives it with the real
//     PBT_* constants — no window, no message loop, no suspend.
//   * PowerEventCoordinator is HEADLESS: no Qt event loop, no Win32, no UI. A
//     test dispatches a transition into it directly.
//   * PowerEventFilter is the thin adapter that joins them, and is the only
//     part that genuinely needs a running application.
//
// A real suspend of this host is therefore not required to exercise any of the
// decision logic — which is the whole of what this phase implements.

#pragma once

#include <QtGlobal>

#include <cstdint>
#include <functional>

#include <QAbstractNativeEventFilter>
#include <QByteArray>

namespace pdfe::app {

/// A power transition PDFE recognises.
///
/// Deliberately NOT a 1:1 mirror of the PBT_* space. Windows broadcasts a
/// dozen PBT_* codes (battery level, power-scheme change, monitor on/off,
/// PBT_APMQUERYSUSPEND and friends, all legacy since Vista). Modelling the two
/// PDFE acts on, and mapping everything else to None, keeps the classifier's
/// contract small enough to state exhaustively in a test.
enum class PowerTransition {
    /// Not a power transition, or one PDFE does not act on.
    None,
    /// PBT_APMSUSPEND — the machine is about to suspend. Windows gives an
    /// application roughly two seconds here and does not wait for slow work;
    /// whatever must survive the freeze has to be done synchronously and fast.
    Suspend,
    /// PBT_APMRESUMEAUTOMATIC — the machine has resumed. This is the one
    /// Windows guarantees on every resume; PBT_APMRESUMESUSPEND is delivered
    /// ONLY when the resume was user-initiated, so treating it as the resume
    /// signal would silently miss wake-for-maintenance and lid-closed wakes.
    ResumeAutomatic
};

/// Non-localised name, for logs and gtest failure messages.
const char* powerTransitionToString(PowerTransition t);

/// Decode a Windows message into a PowerTransition.
///
/// PURE: no globals, no OS calls, no side effects. Returns None for any
/// message that is not WM_POWERBROADCAST and for any WM_POWERBROADCAST whose
/// wParam is not one of the two codes above. Available on every platform (it
/// simply never matches off Windows, where nothing produces these messages),
/// so the test that pins the mapping is not itself platform-conditional.
PowerTransition classifyPowerBroadcast(unsigned int message,
                                       std::uintptr_t wParam);

/**
 * @brief Headless record of, and reaction to, power transitions.
 *
 * Process-wide singleton because there is one machine and one power state, and
 * because the native event filter that feeds it is installed once on the
 * QApplication. Owns no UI and no Win32 handle, so a test drives dispatch()
 * directly.
 *
 * NOT thread-safe by design: WM_POWERBROADCAST arrives on the GUI thread
 * through the Qt event filter, and every caller is on that thread. Adding a
 * mutex would imply a concurrency that does not exist.
 */
class PowerEventCoordinator {
public:
    static PowerEventCoordinator& instance();

    /// Register the action taken when a Suspend is dispatched. MainWindow
    /// installs one that takes an autosave recovery snapshot.
    ///
    /// Exactly ONE hook is held: this replaces any previous one. Passing a
    /// default-constructed std::function removes it, which is what MainWindow's
    /// destructor does — a hook capturing a destroyed window would otherwise be
    /// called with a dangling `this` on the next suspend.
    void setSuspendHook(std::function<void()> hook);

    /// Register the action taken on ResumeAutomatic. Nothing in src/ installs
    /// one today; the seam exists so the work catalogued in
    /// docs/power-event-handling-scope.md has a defined place to land, and so
    /// the test can prove the resume path reaches its hook at all.
    void setResumeHook(std::function<void()> hook);

    /// Handle one transition. Returns true if @p t was acted on (i.e. was
    /// Suspend or ResumeAutomatic), false for None.
    ///
    /// A throwing hook never escapes: an exception propagating out of a
    /// WM_POWERBROADCAST handler would unwind through Qt's window procedure
    /// and terminate the process at the least recoverable moment there is.
    /// The dispatch is still RECORDED when the hook throws — the transition
    /// did happen, whatever the reaction made of it.
    bool dispatch(PowerTransition t);

    /// How many Suspend transitions have been dispatched this process.
    int suspendCount() const { return suspendCount_; }
    /// How many ResumeAutomatic transitions have been dispatched this process.
    int resumeCount() const { return resumeCount_; }

    /// Wall-clock ms since the Unix epoch at the last Suspend / resume. 0 when
    /// that transition has not occurred this process.
    qint64 lastSuspendUnixMs() const { return lastSuspendUnixMs_; }
    qint64 lastResumeUnixMs() const { return lastResumeUnixMs_; }

    /// Wall-clock ms the machine appeared to be suspended for, measured across
    /// the last Suspend/Resume pair. -1 when unknown — which is the NORMAL
    /// state after a resume with no preceding suspend in this process (PDFE was
    /// started after the suspend, or the suspend notification was never
    /// delivered, both of which happen).
    ///
    /// This is a DIAGNOSTIC ONLY. Nothing in PDFE branches on it. In
    /// particular it is NOT used to expire caches or re-validate handles — see
    /// the scope document for why that was left out rather than guessed at.
    qint64 lastFreezeDurationMs() const { return lastFreezeDurationMs_; }

    /// Reset all counters, timestamps and hooks. Test support only: the
    /// coordinator is process-wide, so a test that could not clear it would be
    /// asserting on whatever earlier tests left behind.
    void resetForTesting();

    PowerEventCoordinator(const PowerEventCoordinator&) = delete;
    PowerEventCoordinator& operator=(const PowerEventCoordinator&) = delete;

private:
    PowerEventCoordinator() = default;

    std::function<void()> suspendHook_;
    std::function<void()> resumeHook_;
    int suspendCount_ = 0;
    int resumeCount_ = 0;
    qint64 lastSuspendUnixMs_ = 0;
    qint64 lastResumeUnixMs_ = 0;
    qint64 lastFreezeDurationMs_ = -1;
};

/**
 * @brief Native event filter that turns WM_POWERBROADCAST into a dispatch.
 *
 * The only piece that needs a live application. Install once on the
 * QCoreApplication (main.cpp does):
 *
 *     static pdfe::app::PowerEventFilter powerFilter;
 *     app.installNativeEventFilter(&powerFilter);
 *
 * Always returns false so the message continues to Qt and to DefWindowProc.
 * Returning true would CONSUME WM_POWERBROADCAST, and an application that
 * swallows it can stall or block the suspend it was only meant to observe.
 */
class PowerEventFilter : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr* result) override;
};

}  // namespace pdfe::app
