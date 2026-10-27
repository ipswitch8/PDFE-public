// -*- coding: utf-8 -*-
#include "SleepInhibitor.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace pdfe::core {

SleepInhibitState probeSleepInhibitState() noexcept
{
#ifdef _WIN32
    // SetThreadExecutionState returns the PREVIOUS state. Requesting plain
    // ES_CONTINUOUS is the cheapest way to make the OS hand it over.
    const EXECUTION_STATE previous = ::SetThreadExecutionState(ES_CONTINUOUS);
    if (previous == 0) {
        // The call failed; we have neither read anything nor (per the API
        // contract) changed anything. Report undecided rather than guessing
        // "released", which a caller could read as a passing assertion.
        return SleepInhibitState::Unknown;
    }

    // Put back exactly what was there. Skipped when it already matches what we
    // just wrote, so the common (released) case performs one call, not two.
    if (previous != ES_CONTINUOUS) {
        (void)::SetThreadExecutionState(previous);
    }

    return (previous & ES_SYSTEM_REQUIRED) != 0 ? SleepInhibitState::SystemRequired
                                                : SleepInhibitState::Released;
#else
    // POSIX has no per-thread equivalent, and PDFE does not ship there yet.
    // Reporting Unknown is the honest answer: a fabricated "Released" would
    // let a test assert something the platform never told us.
    return SleepInhibitState::Unknown;
#endif
}

ScopedSleepInhibit::ScopedSleepInhibit() noexcept
{
#ifdef _WIN32
    raised_ = ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0;
#endif
}

ScopedSleepInhibit::~ScopedSleepInhibit() noexcept
{
#ifdef _WIN32
    if (raised_) {
        // Drop the system-required request. ES_CONTINUOUS with no other flag is
        // the documented "clear my thread's requests" form; there is no separate
        // release API.
        (void)::SetThreadExecutionState(ES_CONTINUOUS);
    }
#endif
}

}  // namespace pdfe::core
