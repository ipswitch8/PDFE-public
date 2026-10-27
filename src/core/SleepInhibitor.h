// -*- coding: utf-8 -*-
#pragma once

// =============================================================================
// SleepInhibitor — keep the machine awake for the duration of a long unattended
// operation, and make the fact READABLE (GitHub #99 / meta-audit WIN-75).
// =============================================================================
//
// WHAT WAS WRONG
// --------------
// `grep -rl SetThreadExecutionState src` returned zero files. A batch run over
// a few hundred documents, or a multi-minute OCR pass, is exactly the shape of
// work a user starts and walks away from — and exactly the shape of work an
// idle-sleep policy interrupts. Nothing in the product told Windows the machine
// was busy.
//
// WHY A SEAM RATHER THAN A BARE CALL
// ----------------------------------
// The interesting failure is not "we forgot to raise it". It is "we raised it
// and never released it", which leaves a machine that never sleeps again for
// the life of the process — a battery/energy defect that is invisible in
// testing unless something can OBSERVE the state after the operation ends.
//
// Win32 offers no `GetThreadExecutionState`. It does, however, specify that
// `SetThreadExecutionState` RETURNS THE PREVIOUS STATE. `probeSleepInhibitState`
// exploits exactly that: it sets the state to plain ES_CONTINUOUS (which reads
// the real value out of the OS), and then, if the value it read back differs,
// writes it straight back. What it reports therefore comes FROM THE KERNEL, not
// from a bool this file kept alongside its own last write — a mirror variable
// would happily agree with a release path that had been mutated into a no-op,
// which is the one thing this seam exists to catch.
//
// THREAD AFFINITY IS LOAD-BEARING
// -------------------------------
// The execution state is PER THREAD. `ScopedSleepInhibit` must therefore be
// constructed on the thread that performs the work (BatchEngine::process does
// this at the top of its own body, so it is correct whether the engine is
// driven from the UI thread or from a worker), and `probeSleepInhibitState()`
// only reports on the CALLING thread. A probe from a different thread than the
// raise is meaningless, not merely imprecise.

namespace pdfe::core {

/// What the OS reports for the CALLING thread's execution-state request.
enum class SleepInhibitState {
    Unknown,         ///< the platform cannot report (non-Windows, or the call failed)
    Released,        ///< no ES_SYSTEM_REQUIRED is held by this thread
    SystemRequired,  ///< ES_SYSTEM_REQUIRED is held by this thread
};

/// Read the calling thread's execution-state request back OUT OF THE OS and
/// restore whatever was there. Never throws; leaves the state as it found it.
SleepInhibitState probeSleepInhibitState() noexcept;

/// RAII: raise ES_CONTINUOUS | ES_SYSTEM_REQUIRED for the lifetime of the
/// object, drop back to ES_CONTINUOUS on destruction.
///
/// ES_DISPLAY_REQUIRED is deliberately NOT requested. Batch work needs the
/// SYSTEM awake; it does not need the screen lit, and holding the display on
/// through an hour of unattended processing is a power and a privacy cost
/// (an unlocked, lit screen) that the operation does not need.
class ScopedSleepInhibit {
public:
    ScopedSleepInhibit() noexcept;
    ~ScopedSleepInhibit() noexcept;

    ScopedSleepInhibit(const ScopedSleepInhibit&) = delete;
    ScopedSleepInhibit& operator=(const ScopedSleepInhibit&) = delete;
    ScopedSleepInhibit(ScopedSleepInhibit&&) = delete;
    ScopedSleepInhibit& operator=(ScopedSleepInhibit&&) = delete;

    /// True if the OS accepted the request. A refusal is not an error worth
    /// failing the batch over — the work still runs, the machine may just doze.
    bool raised() const noexcept { return raised_; }

private:
    bool raised_ = false;
};

}  // namespace pdfe::core
