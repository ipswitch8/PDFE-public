// -*- coding: utf-8 -*-
// GitHub #213 / meta-audit row WIN-71 — power-event (suspend/resume) handling.
//
// WHAT THIS PINS, AND WHAT IT DELIBERATELY DOES NOT CLAIM
// -------------------------------------------------------
// The handler under test is PARTIAL on purpose. It acts on two transitions and
// does two things; the full scope of what suspend/resume support would require
// is docs/power-event-handling-scope.md, and #213 stays OPEN for §3.1-§3.4 of
// it. So this file pins three things and no more:
//
//   1. The PBT_* -> PowerTransition mapping, exhaustively, using the real SDK
//      constants.
//   2. The dispatch behaviour: what a Suspend does, what a Resume does, what
//      happens when a hook throws, and what the freeze-duration diagnostic
//      reports when it cannot know.
//   3. The one real product reaction: a suspend with unsaved work PUBLISHES A
//      RECOVERY SNAPSHOT through MainWindow's live AutosaveManager.
//
// And — the point of the whole exercise — it pins the OMISSIONS AS OMISSIONS.
// ResumeDoesNotPublishARecoverySnapshot and
// ResumeLeavesTheDocumentAndItsPageCountUntouched exist because "a handler that
// logs and does nothing is honest only if the test says that is all it does".
// If someone later adds resume-time revalidation, those two must be rewritten
// deliberately rather than quietly satisfied.
//
// WHY THERE IS NO REAL SUSPEND HERE
// ---------------------------------
// GitHub #97 (WIN-73) was deferred for "needs two Windows sessions" and then
// closed by a plain GoogleTest, because the row's stated ENVIRONMENT was not
// its REQUIREMENT. Same shape here. The requirement of a power HANDLER is "the
// app is told, and reacts"; classifyPowerBroadcast() is pure and the
// coordinator is headless, so every step is injectable. What a real suspend is
// still needed for is what a real FREEZE does to file handles, caches, timers
// and an in-flight save — none of which this phase attempts, and all of which
// the scope document leaves open.

#include <gtest/gtest.h>

#include "app/PowerEvents.h"
#include "ui/MainWindow.h"
#include "core/Document.h"

#include <QApplication>
#include <QString>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::app::PowerEventCoordinator;
using pdfe::app::PowerTransition;
using pdfe::app::classifyPowerBroadcast;

namespace {

std::filesystem::path fixtures()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// The coordinator is process-wide, so every test must start from a known state
// and must not leak a hook into a later test in the same shard.
class PowerEventCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override { PowerEventCoordinator::instance().resetForTesting(); }
    void TearDown() override { PowerEventCoordinator::instance().resetForTesting(); }
};

class PowerEventMainWindowTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_NE(qApp, nullptr) << "these tests need the suite's QApplication";
        PowerEventCoordinator::instance().resetForTesting();
        dir_ = std::filesystem::temp_directory_path()
               / ("pdfe_win71_power_"
                  + std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch().count()));
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        ASSERT_FALSE(ec);
    }

    void TearDown() override
    {
        PowerEventCoordinator::instance().resetForTesting();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// Open a real fixture in @p w and make a real content mutation, so the
    /// AutosaveManager's dirty signal is genuinely set. Returns the doc path.
    std::filesystem::path openDirtyDocument(pdfe::ui::MainWindow& w,
                                            const char* name)
    {
        const auto doc = dir_ / name;
        std::error_code ec;
        std::filesystem::copy_file(fixtures() / "test_3page.pdf", doc,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        EXPECT_FALSE(ec);
        w.openFile(QString::fromStdWString(doc.wstring()));
        EXPECT_NE(w.documentForTesting(), nullptr) << "the fixture failed to open";
        if (w.documentForTesting()) {
            w.documentForTesting()->insertBlankPage(1, 300.0f, 300.0f);
            w.documentForTesting()->markRequiresFullRewrite();
        }
        return doc;
    }

    std::filesystem::path dir_;
};

}  // namespace

// ===========================================================================
// 1. The classifier. Pure function; the whole of the Win32 knowledge.
// ===========================================================================

#ifdef _WIN32

// The two transitions the phase is required to cover, asserted against the
// SDK's own constants rather than against the literals PowerEvents.cpp holds.
// (PowerEvents.cpp static_asserts its literals equal to these, so a typo in
// either place is a build failure, not a silently-passing test.)
TEST(PowerEventClassify, RecognisesSuspendAndResumeAutomatic)
{
    EXPECT_EQ(classifyPowerBroadcast(WM_POWERBROADCAST, PBT_APMSUSPEND),
              PowerTransition::Suspend);
    EXPECT_EQ(classifyPowerBroadcast(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC),
              PowerTransition::ResumeAutomatic);
}

// PBT_APMRESUMESUSPEND is delivered ONLY for a user-initiated resume, so
// treating it as "the resume signal" would silently miss wake-for-maintenance
// and lid wakes. It is deliberately not acted on; this pins that choice so a
// later "obvious" addition has to be made on purpose.
TEST(PowerEventClassify, IgnoresTheOtherPowerBroadcastCodes)
{
    EXPECT_EQ(classifyPowerBroadcast(WM_POWERBROADCAST, PBT_APMRESUMESUSPEND),
              PowerTransition::None);
    EXPECT_EQ(classifyPowerBroadcast(WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE),
              PowerTransition::None);
    EXPECT_EQ(classifyPowerBroadcast(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE),
              PowerTransition::None);
    EXPECT_EQ(classifyPowerBroadcast(WM_POWERBROADCAST, PBT_APMRESUMECRITICAL),
              PowerTransition::None);
}

// A wParam that MATCHES a code we act on, carried by a message that is not
// WM_POWERBROADCAST, must not be mistaken for a power event. Without this the
// classifier could be ignoring `message` entirely and every test above would
// still pass.
TEST(PowerEventClassify, IgnoresNonPowerMessagesCarryingTheSameWParam)
{
    EXPECT_EQ(classifyPowerBroadcast(WM_TIMER, PBT_APMSUSPEND),
              PowerTransition::None);
    EXPECT_EQ(classifyPowerBroadcast(WM_CLOSE, PBT_APMRESUMEAUTOMATIC),
              PowerTransition::None);
    EXPECT_EQ(classifyPowerBroadcast(WM_POWERBROADCAST + 1, PBT_APMSUSPEND),
              PowerTransition::None);
}

#endif  // _WIN32

// ===========================================================================
// 2. The coordinator. Headless; no message loop, no suspend.
// ===========================================================================

TEST_F(PowerEventCoordinatorTest, SuspendRunsTheHookAndIsRecorded)
{
    auto& c = PowerEventCoordinator::instance();
    int hookCalls = 0;
    c.setSuspendHook([&hookCalls]() { ++hookCalls; });

    ASSERT_EQ(c.suspendCount(), 0);
    EXPECT_TRUE(c.dispatch(PowerTransition::Suspend));

    EXPECT_EQ(hookCalls, 1);
    EXPECT_EQ(c.suspendCount(), 1);
    EXPECT_GT(c.lastSuspendUnixMs(), 0);
    // No resume yet, so nothing on the resume side may have moved.
    EXPECT_EQ(c.resumeCount(), 0);
    EXPECT_EQ(c.lastResumeUnixMs(), 0);
}

// The resume hook exists as a seam and must genuinely be reached — otherwise
// the scope document's "this is where §3.1-§3.4 would land" is a promise the
// code does not keep.
TEST_F(PowerEventCoordinatorTest, ResumeRunsItsHookAndIsRecordedSeparately)
{
    auto& c = PowerEventCoordinator::instance();
    int suspendCalls = 0;
    int resumeCalls = 0;
    c.setSuspendHook([&suspendCalls]() { ++suspendCalls; });
    c.setResumeHook([&resumeCalls]() { ++resumeCalls; });

    EXPECT_TRUE(c.dispatch(PowerTransition::ResumeAutomatic));

    EXPECT_EQ(resumeCalls, 1);
    EXPECT_EQ(c.resumeCount(), 1);
    EXPECT_GT(c.lastResumeUnixMs(), 0);
    // A resume must not be able to fire the SUSPEND reaction. If it could, a
    // wake would take a pointless recovery snapshot on every lid-open.
    EXPECT_EQ(suspendCalls, 0);
    EXPECT_EQ(c.suspendCount(), 0);
}

// None is not a transition. dispatch() must report that it did nothing, and
// must touch nothing.
TEST_F(PowerEventCoordinatorTest, NoneIsNotDispatchedAndChangesNothing)
{
    auto& c = PowerEventCoordinator::instance();
    int calls = 0;
    c.setSuspendHook([&calls]() { ++calls; });
    c.setResumeHook([&calls]() { ++calls; });

    EXPECT_FALSE(c.dispatch(PowerTransition::None));

    EXPECT_EQ(calls, 0);
    EXPECT_EQ(c.suspendCount(), 0);
    EXPECT_EQ(c.resumeCount(), 0);
    EXPECT_EQ(c.lastSuspendUnixMs(), 0);
}

// The diagnostic reports a duration across a real pair...
TEST_F(PowerEventCoordinatorTest, FreezeDurationIsMeasuredAcrossASuspendResumePair)
{
    auto& c = PowerEventCoordinator::instance();

    EXPECT_TRUE(c.dispatch(PowerTransition::Suspend));
    EXPECT_EQ(c.lastFreezeDurationMs(), -1)
        << "a suspend with no resume yet has no measurable freeze duration";

    EXPECT_TRUE(c.dispatch(PowerTransition::ResumeAutomatic));
    EXPECT_GE(c.lastFreezeDurationMs(), 0)
        << "a completed suspend/resume pair must yield a duration";
    EXPECT_GT(c.lastSuspendUnixMs(), 0);
    EXPECT_GE(c.lastResumeUnixMs(), c.lastSuspendUnixMs());
}

// ...and reports -1, NOT a fabricated zero, when it cannot know. A resume with
// no suspend in this process is the NORMAL case when PDFE is started after a
// suspend, or when the suspend notification was never delivered. Reporting 0
// there would be a lie that a future cache-expiry rule could act on.
TEST_F(PowerEventCoordinatorTest, FreezeDurationIsUnknownForAResumeWithNoSuspend)
{
    auto& c = PowerEventCoordinator::instance();

    EXPECT_TRUE(c.dispatch(PowerTransition::ResumeAutomatic));

    EXPECT_EQ(c.lastFreezeDurationMs(), -1);
    EXPECT_EQ(c.resumeCount(), 1);
    EXPECT_EQ(c.lastSuspendUnixMs(), 0);
}

// A hook that throws must not escape: this runs inside Qt's window procedure,
// where an exception would terminate the process at the least recoverable
// moment there is. And the transition is still RECORDED — it happened,
// whatever the reaction made of it.
TEST_F(PowerEventCoordinatorTest, AThrowingHookIsContainedAndTheEventIsStillRecorded)
{
    auto& c = PowerEventCoordinator::instance();
    c.setSuspendHook([]() { throw std::runtime_error("hook failure"); });

    bool handled = false;
    EXPECT_NO_THROW({ handled = c.dispatch(PowerTransition::Suspend); });

    EXPECT_TRUE(handled);
    EXPECT_EQ(c.suspendCount(), 1);
    EXPECT_GT(c.lastSuspendUnixMs(), 0);
}

// Clearing the hook is how ~MainWindow avoids a dangling `this`. If clearing
// did not take effect, a suspend after the window died would call into freed
// memory — which is a crash, not a test failure, so pin it here where it is
// observable.
TEST_F(PowerEventCoordinatorTest, ClearingTheSuspendHookStopsItBeingCalled)
{
    auto& c = PowerEventCoordinator::instance();
    int calls = 0;
    c.setSuspendHook([&calls]() { ++calls; });
    EXPECT_TRUE(c.dispatch(PowerTransition::Suspend));
    ASSERT_EQ(calls, 1);

    c.setSuspendHook({});
    EXPECT_TRUE(c.dispatch(PowerTransition::Suspend))
        << "the transition is still handled; only the reaction is gone";
    EXPECT_EQ(calls, 1) << "a cleared hook must not run again";
    EXPECT_EQ(c.suspendCount(), 2);
}

// ===========================================================================
// 3. The real product reaction, through a live MainWindow.
// ===========================================================================

// THE LOAD-BEARING ORACLE. A machine suspend with unsaved work must leave that
// work RECOVERABLE before the freeze — a suspend that never resumes (flat
// battery, discarded VM, dropped hibernate image) loses it exactly as a
// `shutdown /f` would. Asserted as real published recovery state, not as "a
// hook was called".
TEST_F(PowerEventMainWindowTest, SuspendPublishesARecoverySnapshot)
{
    pdfe::ui::MainWindow w;
    openDirtyDocument(w, "power_suspend.pdf");
    ASSERT_NE(w.documentForTesting(), nullptr);
    ASSERT_EQ(w.documentForTesting()->pageCount(), 4);

    ASSERT_FALSE(w.hasRecoverySnapshotForTesting())
        << "nothing should be published before the suspend notification";

    EXPECT_TRUE(PowerEventCoordinator::instance().dispatch(PowerTransition::Suspend));

    EXPECT_TRUE(w.hasRecoverySnapshotForTesting())
        << "the machine suspended with unsaved changes and PDFE published no "
           "recovery snapshot — a suspend that never resumes is silent data "
           "loss (GitHub #213 / WIN-71)";

    // This test deliberately published a real sidecar into the per-user
    // recovery dir; drop it so the launch-time recovery prompt cannot fire in
    // an unrelated test.
    w.clearRecoveryForTesting();
}

// ---------------------------------------------------------------------------
// THE OMISSIONS, PINNED AS OMISSIONS.
//
// These two are the reason this file can honestly call the handler partial.
// They are not "tests that nothing happens" for their own sake: they are the
// counterpart to the scope document's §3.1-§3.4, so that a reader of either one
// gets the same answer about what resume does. If resume-time revalidation is
// ever added, BOTH must be rewritten deliberately — neither may be deleted to
// make a change go green.
// ---------------------------------------------------------------------------

TEST_F(PowerEventMainWindowTest, ResumeDoesNotPublishARecoverySnapshot)
{
    pdfe::ui::MainWindow w;
    openDirtyDocument(w, "power_resume.pdf");
    ASSERT_NE(w.documentForTesting(), nullptr);

    ASSERT_FALSE(w.hasRecoverySnapshotForTesting());

    EXPECT_TRUE(PowerEventCoordinator::instance()
                    .dispatch(PowerTransition::ResumeAutomatic));

    EXPECT_FALSE(w.hasRecoverySnapshotForTesting())
        << "resume is recorded and logged ONLY (docs/power-event-handling-"
           "scope.md §2). If PDFE now reacts to resume, this pin must be "
           "rewritten to state the new behaviour, not deleted.";
    EXPECT_EQ(PowerEventCoordinator::instance().resumeCount(), 1)
        << "...but the resume itself must genuinely have been dispatched — "
           "otherwise the assertion above passes for the wrong reason";
}

TEST_F(PowerEventMainWindowTest, ResumeLeavesTheDocumentAndItsPageCountUntouched)
{
    pdfe::ui::MainWindow w;
    openDirtyDocument(w, "power_resume_doc.pdf");
    auto* doc = w.documentForTesting();
    ASSERT_NE(doc, nullptr);
    const int pagesBefore = doc->pageCount();
    ASSERT_EQ(pagesBefore, 4) << "3-page fixture plus the inserted blank page";

    // A full suspend/resume cycle, as a real lid-close/open would deliver it.
    EXPECT_TRUE(PowerEventCoordinator::instance().dispatch(PowerTransition::Suspend));
    EXPECT_TRUE(PowerEventCoordinator::instance()
                    .dispatch(PowerTransition::ResumeAutomatic));

    // The document object is the same one, still open, with the unsaved edit
    // intact. Nothing reloads, re-opens or discards it on resume.
    EXPECT_EQ(w.documentForTesting(), doc)
        << "resume must not swap the live Document out from under the UI";
    ASSERT_NE(w.documentForTesting(), nullptr);
    EXPECT_EQ(w.documentForTesting()->pageCount(), pagesBefore)
        << "the unsaved in-memory edit must survive a suspend/resume cycle";

    w.clearRecoveryForTesting();
}

// ===========================================================================
// 4. The native event filter: it OBSERVES, and must never CONSUME.
//
// Added 2026-09-18 after a phase-8 mutation run. Flipping the filter's final
// `return false` to `return true` — i.e. swallowing WM_POWERBROADCAST instead
// of letting it continue to Qt and to every other handler in the process —
// SURVIVED the whole suite. The claim "ALWAYS false: observe, never consume"
// was a comment with nothing behind it, which is exactly the shape of defect
// GitHub #212 was filed about: a confident statement no mechanism holds to.
//
// The consequence of that unpinned line is not cosmetic. A native event filter
// returning true tells Qt the message is fully handled, so it is not
// translated or dispatched further; a broadcast PDFE only wants to watch would
// have been eaten for the rest of the application.
//
// These tests therefore assert BOTH halves together — the event was acted on
// (so they cannot pass by the filter doing nothing) and the message was still
// passed along. Asserting only the return value would be satisfied by a filter
// that ignored power events entirely.
// ===========================================================================

#ifdef _WIN32

namespace {

/// Build the MSG a real WM_POWERBROADCAST arrives as, and run it through the
/// filter exactly as Qt's dispatcher would.
bool runFilter(UINT message, WPARAM wParam,
               const QByteArray& type = QByteArrayLiteral("windows_generic_MSG"))
{
    pdfe::app::PowerEventFilter filter;
    MSG msg{};
    msg.message = message;
    msg.wParam = wParam;
    qintptr result = 0;
    return filter.nativeEventFilter(type, &msg, &result);
}

}  // namespace

TEST_F(PowerEventCoordinatorTest, TheFilterDispatchesSuspendWithoutConsumingIt)
{
    int hookCalls = 0;
    PowerEventCoordinator::instance().setSuspendHook([&] { ++hookCalls; });

    EXPECT_FALSE(runFilter(WM_POWERBROADCAST, PBT_APMSUSPEND))
        << "the filter must observe the broadcast and pass it on; returning "
           "true tells Qt the message is handled and stops it reaching every "
           "other handler in the process";

    // The other half: it really did act, so the EXPECT_FALSE above cannot be
    // satisfied by a filter that simply ignores power messages.
    EXPECT_EQ(hookCalls, 1) << "the suspend must still have been dispatched";
    EXPECT_EQ(PowerEventCoordinator::instance().suspendCount(), 1);
}

TEST_F(PowerEventCoordinatorTest, TheFilterDispatchesResumeWithoutConsumingIt)
{
    int hookCalls = 0;
    PowerEventCoordinator::instance().setResumeHook([&] { ++hookCalls; });

    EXPECT_FALSE(runFilter(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC));

    EXPECT_EQ(hookCalls, 1) << "the resume must still have been dispatched";
    EXPECT_EQ(PowerEventCoordinator::instance().resumeCount(), 1);
}

TEST_F(PowerEventCoordinatorTest, TheFilterPassesOnMessagesItDoesNotActOn)
{
    // An unrelated message, and a power message carrying a code the handler
    // deliberately ignores. Both must be passed on, and neither may dispatch.
    EXPECT_FALSE(runFilter(WM_CLOSE, 0));
    EXPECT_FALSE(runFilter(WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE));

    // A power broadcast arriving on the dispatcher's own event type is still
    // ours to observe — but still not ours to swallow.
    EXPECT_FALSE(runFilter(WM_POWERBROADCAST, PBT_APMSUSPEND,
                           QByteArrayLiteral("windows_dispatcher_MSG")));
    EXPECT_EQ(PowerEventCoordinator::instance().suspendCount(), 1)
        << "the dispatcher-MSG variant must be handled, not skipped";

    EXPECT_EQ(PowerEventCoordinator::instance().resumeCount(), 0);
}

TEST_F(PowerEventCoordinatorTest, TheFilterIgnoresAForeignEventTypeEntirely)
{
    EXPECT_FALSE(runFilter(WM_POWERBROADCAST, PBT_APMSUSPEND,
                           QByteArrayLiteral("xcb_generic_event_t")));
    EXPECT_EQ(PowerEventCoordinator::instance().suspendCount(), 0)
        << "a non-Windows event type must not be decoded as an MSG";
}

#endif  // _WIN32
