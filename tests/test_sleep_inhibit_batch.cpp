// -*- coding: utf-8 -*-
//
// GitHub #99 / meta-audit WIN-75 — nothing in src/ ever called
// SetThreadExecutionState, so a multi-minute unattended batch could be
// interrupted by the machine's idle-sleep policy.
//
// THE ASSERTION THAT MATTERS IS THE THIRD ONE. "Active during the batch" alone
// would be satisfied by an implementation that raises the request and never
// releases it — a machine that never sleeps again for the life of the process,
// which is a worse defect than the one being fixed and an invisible one. So
// this reads the state three times: before, during (from inside the engine's
// own progress callback, on the engine's own thread) and after.
//
// The read is not a mirror of what our code last wrote. Win32 has no
// GetThreadExecutionState; probeSleepInhibitState() gets the value out of the
// kernel by exploiting SetThreadExecutionState's documented "returns the
// PREVIOUS state" contract and then puts it back. See src/core/SleepInhibitor.h.
#include <gtest/gtest.h>

#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "core/MuPdfContext.h"
#include "core/SleepInhibitor.h"

#include <filesystem>
#include <memory>
#include <vector>

using pdfe::core::SleepInhibitState;
using pdfe::core::probeSleepInhibitState;

namespace {

const char* stateName(SleepInhibitState s)
{
    switch (s) {
        case SleepInhibitState::Unknown:        return "Unknown";
        case SleepInhibitState::Released:       return "Released";
        case SleepInhibitState::SystemRequired: return "SystemRequired";
    }
    return "?";
}

TEST(SleepInhibitorSeam, ProbeIsNonDestructiveAndReportsRealState)
{
    // The seam has to be trustworthy before the batch test can lean on it.
    // A probe that changed the state would make the three-point observation
    // below meaningless, and a probe that always answered the same thing would
    // make it vacuous.
#ifdef _WIN32
    ASSERT_EQ(probeSleepInhibitState(), SleepInhibitState::Released)
        << "this thread already holds a system-required request before any "
           "test raised one";
    EXPECT_EQ(probeSleepInhibitState(), SleepInhibitState::Released)
        << "probing changed the state it reported on";

    {
        const pdfe::core::ScopedSleepInhibit raised;
        ASSERT_TRUE(raised.raised()) << "the OS refused the request";
        EXPECT_EQ(probeSleepInhibitState(), SleepInhibitState::SystemRequired)
            << "the probe cannot see a request this test just raised — it is "
               "not reading real state";
        EXPECT_EQ(probeSleepInhibitState(), SleepInhibitState::SystemRequired)
            << "a second probe lost the request — the probe is destructive";
    }

    EXPECT_EQ(probeSleepInhibitState(), SleepInhibitState::Released)
        << "ScopedSleepInhibit did not release on destruction";
#else
    // No per-thread equivalent exists off Windows, and the seam says so rather
    // than inventing an answer. Asserted, not skipped: if a future port grows
    // a real implementation this fails and demands the test be finished.
    EXPECT_EQ(probeSleepInhibitState(), SleepInhibitState::Unknown);
#endif
}

TEST(SleepInhibitorSeam, BatchHoldsSystemRequiredOnlyForItsDuration)
{
#ifdef _WIN32
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    pdfe::batch::BatchEngine engine(ctx);

    // 1 — BEFORE.
    ASSERT_EQ(probeSleepInhibitState(), SleepInhibitState::Released)
        << "the sleep request was already held before the batch started; the "
           "'during' observation below would prove nothing";

    // 2 — DURING. Observed from inside the engine, on the thread running the
    // batch. The execution state is PER THREAD, so a probe from anywhere else
    // would be answering a different question.
    int observations = 0;
    SleepInhibitState during = SleepInhibitState::Unknown;
    engine.setProgressCallback(
        [&](int, int, const std::string&) {
            ++observations;
            during = probeSleepInhibitState();
        });

    // The files deliberately do not exist. The batch still runs — the progress
    // callback fires per file before any I/O — and this keeps the test off the
    // document corpus entirely, so it cannot pass by skipping.
    const std::vector<std::filesystem::path> files{
        std::filesystem::temp_directory_path() / "pdfe_issue99_absent_a.pdf",
        std::filesystem::temp_directory_path() / "pdfe_issue99_absent_b.pdf",
    };
    std::vector<pdfe::batch::BatchAction> actions;

    const auto result = engine.process(files, actions);

    ASSERT_EQ(observations, 2)
        << "the batch never ran, so nothing was observed during it";
    ASSERT_EQ(result.totalFiles, 2);

    EXPECT_EQ(during, SleepInhibitState::SystemRequired)
        << "BatchEngine::process did not hold ES_SYSTEM_REQUIRED while running "
           "(observed " << stateName(during) << ") — an idle-sleep policy can "
           "suspend the machine mid-batch (GitHub #99)";

    // 3 — AFTER. This is the assertion that catches the never-released bug.
    EXPECT_EQ(probeSleepInhibitState(), SleepInhibitState::Released)
        << "BatchEngine::process left ES_SYSTEM_REQUIRED raised after the batch "
           "finished — this thread now prevents the machine from ever sleeping "
           "again (GitHub #99)";
#else
    GTEST_SUCCEED() << "no per-thread execution state off Windows";
#endif
}

}  // namespace
