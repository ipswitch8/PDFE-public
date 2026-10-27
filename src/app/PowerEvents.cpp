// -*- coding: utf-8 -*-
// GitHub #213 / WIN-71 — see PowerEvents.h for the scope statement and for why
// this is deliberately partial.
#include "PowerEvents.h"

#include <QDateTime>
#include <QLoggingCategory>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pdfe::app {

namespace {

// The two PBT_* codes this handler acts on, defined locally so
// classifyPowerBroadcast() compiles and is TESTABLE on every platform rather
// than only where <windows.h> supplies them. The values are fixed ABI
// constants; the static_asserts below make the local copies provably identical
// to the SDK's wherever the SDK is present, so a drift cannot go unnoticed.
constexpr unsigned int kWmPowerBroadcast = 0x0218;  // WM_POWERBROADCAST
constexpr std::uintptr_t kPbtApmSuspend = 0x0004;   // PBT_APMSUSPEND
constexpr std::uintptr_t kPbtApmResumeAutomatic = 0x0012;  // PBT_APMRESUMEAUTOMATIC

#ifdef _WIN32
static_assert(kWmPowerBroadcast == WM_POWERBROADCAST,
              "WM_POWERBROADCAST value drifted from the SDK");
static_assert(kPbtApmSuspend == PBT_APMSUSPEND,
              "PBT_APMSUSPEND value drifted from the SDK");
static_assert(kPbtApmResumeAutomatic == PBT_APMRESUMEAUTOMATIC,
              "PBT_APMRESUMEAUTOMATIC value drifted from the SDK");
#endif

}  // namespace

const char* powerTransitionToString(PowerTransition t)
{
    switch (t) {
    case PowerTransition::Suspend:         return "Suspend";
    case PowerTransition::ResumeAutomatic: return "ResumeAutomatic";
    case PowerTransition::None:            break;
    }
    return "None";
}

PowerTransition classifyPowerBroadcast(unsigned int message,
                                       std::uintptr_t wParam)
{
    if (message != kWmPowerBroadcast) return PowerTransition::None;
    if (wParam == kPbtApmSuspend) return PowerTransition::Suspend;
    if (wParam == kPbtApmResumeAutomatic) return PowerTransition::ResumeAutomatic;
    // Everything else -- PBT_APMPOWERSTATUSCHANGE, PBT_POWERSETTINGCHANGE,
    // PBT_APMRESUMESUSPEND, the legacy query codes -- is deliberately not acted
    // on. See the enum's documentation.
    return PowerTransition::None;
}

PowerEventCoordinator& PowerEventCoordinator::instance()
{
    static PowerEventCoordinator inst;
    return inst;
}

void PowerEventCoordinator::setSuspendHook(std::function<void()> hook)
{
    suspendHook_ = std::move(hook);
}

void PowerEventCoordinator::setResumeHook(std::function<void()> hook)
{
    resumeHook_ = std::move(hook);
}

bool PowerEventCoordinator::dispatch(PowerTransition t)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    switch (t) {
    case PowerTransition::Suspend: {
        // Record FIRST. The hook may be slow, may throw, and the machine may
        // be taken away mid-hook; the fact of the suspend must not depend on
        // the reaction completing.
        ++suspendCount_;
        lastSuspendUnixMs_ = nowMs;
        lastFreezeDurationMs_ = -1;
        qInfo("Power: suspend notification received; taking a recovery "
              "snapshot before the machine freezes");
        if (suspendHook_) {
            try {
                suspendHook_();
            } catch (...) {
                // Never let this escape: we are inside a window procedure.
                qWarning("Power: suspend hook threw; suspend proceeds anyway");
            }
        }
        return true;
    }

    case PowerTransition::ResumeAutomatic: {
        ++resumeCount_;
        lastResumeUnixMs_ = nowMs;
        lastFreezeDurationMs_ =
            (lastSuspendUnixMs_ > 0 && nowMs >= lastSuspendUnixMs_)
                ? (nowMs - lastSuspendUnixMs_)
                : -1;
        // Diagnostic only. PDFE does NOT re-validate file handles, invalidate
        // render tiles, re-check the MuPDF context or re-phase timers here --
        // see docs/power-event-handling-scope.md for each omission and why it
        // was not guessed at in this phase.
        qInfo("Power: resume notification received (apparent freeze %lld ms); "
              "no document, cache or handle state is revalidated",
              static_cast<long long>(lastFreezeDurationMs_));
        if (resumeHook_) {
            try {
                resumeHook_();
            } catch (...) {
                qWarning("Power: resume hook threw");
            }
        }
        return true;
    }

    case PowerTransition::None:
        break;
    }
    return false;
}

void PowerEventCoordinator::resetForTesting()
{
    suspendHook_ = {};
    resumeHook_ = {};
    suspendCount_ = 0;
    resumeCount_ = 0;
    lastSuspendUnixMs_ = 0;
    lastResumeUnixMs_ = 0;
    lastFreezeDurationMs_ = -1;
}

bool PowerEventFilter::nativeEventFilter(const QByteArray& eventType,
                                         void* message, qintptr* /*result*/)
{
#ifdef _WIN32
    if (eventType != QByteArrayLiteral("windows_generic_MSG") &&
        eventType != QByteArrayLiteral("windows_dispatcher_MSG")) {
        return false;
    }
    if (!message) return false;
    const MSG* msg = static_cast<MSG*>(message);
    const PowerTransition t = classifyPowerBroadcast(
        static_cast<unsigned int>(msg->message),
        static_cast<std::uintptr_t>(msg->wParam));
    if (t != PowerTransition::None) {
        PowerEventCoordinator::instance().dispatch(t);
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
#endif
    // ALWAYS false: observe, never consume. See the class documentation.
    return false;
}

}  // namespace pdfe::app
