// -*- coding: utf-8 -*-
//
// Autosave write-latency probe — GitHub #106 (meta-audit gap-register WIN-89).
//
// WHAT IS UNDER TEST
// ------------------
// AutosaveManager::writeSidecar() times its whole QSaveFile open/write/commit
// and, if that exceeds AutosaveManager::kSidecarWriteWarnMs, emits ONE warning
// per process. The condition it is diagnosing is on-access antimalware
// inspecting each freshly created file (which interacts with code signing —
// see docs/code-signing.md), a machine-level property that would otherwise
// present as an unexplained UI stall on the autosave tick with nothing logged.
//
// WHY THE SLOWNESS IS INJECTED RATHER THAN MEASURED
// -------------------------------------------------
// A test that waited for a real disk to be slow would be a coin flip: on the
// SSD these run on the sidecar write completes in low single-digit
// milliseconds, so the warning would never fire and the test would be a
// permanently-green nothing. Worse, a "skip unless the write was slow" guard is
// exactly the magnitude-guard anti-pattern tools/gate/check_magnitude_guard.py
// exists to catch — an oracle that silently disables itself on fast hardware.
//
// So AutosaveManager::setSidecarWriteProbeHook() injects a deterministic sleep
// INSIDE the timed region. Every assertion below then runs unconditionally on
// every machine: there is no `if (elapsed > ...)` anywhere in this file.
//
// The injected delay is kSidecarWriteWarnMs * 2, not kSidecarWriteWarnMs + 1,
// so that scheduler granularity (sleep_for may return slightly early on
// Windows, whose timer resolution is ~15.6 ms by default) cannot land the
// measurement just under the threshold and redden the suite for a reason that
// has nothing to do with the probe.

#include <gtest/gtest.h>

#include "core/AutosaveManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::core::AutosaveManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

class AutosaveWriteLatencyProbeTest : public ::testing::Test {
protected:
    void SetUp() override {
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture_)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }

        const std::string testName =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        base_ = std::filesystem::temp_directory_path() /
                ("pdfe_autosave_latency_" + testName);
        recoveryDir_ = base_ / "recovery";
        srcDir_ = base_ / "src";

        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
        std::filesystem::create_directories(recoveryDir_, ec);
        std::filesystem::create_directories(srcDir_, ec);
        ASSERT_TRUE(std::filesystem::exists(recoveryDir_));

        // The latch is process-wide by design, so an earlier test in this
        // binary may already have consumed it. Re-arm before every case.
        AutosaveManager::resetSidecarWriteLatencyProbe();
        ctx_ = std::make_shared<MuPdfContext>();
    }

    void TearDown() override {
        // Leaving a sleeping hook installed would slow every later autosave
        // test in this binary by a quarter of a second per write.
        AutosaveManager::setSidecarWriteProbeHook({});
        AutosaveManager::resetSidecarWriteLatencyProbe();
        ctx_.reset();
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    std::filesystem::path stageSourceAt(const std::filesystem::path& dst) {
        std::error_code ec;
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::filesystem::copy_file(
            fixture_, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        EXPECT_FALSE(ec) << "stage copy failed: " << ec.message();
        return dst;
    }

    /// Make every sidecar write take comfortably longer than the threshold.
    static void installSlowWrite() {
        AutosaveManager::setSidecarWriteProbeHook([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                AutosaveManager::kSidecarWriteWarnMs * 2));
        });
    }

    std::filesystem::path fixture_;
    std::filesystem::path base_;
    std::filesystem::path recoveryDir_;
    std::filesystem::path srcDir_;
    std::shared_ptr<MuPdfContext> ctx_;
};

// ---------------------------------------------------------------------------
// 1. A slow write warns. (The probe fires at all.)
// ---------------------------------------------------------------------------
TEST_F(AutosaveWriteLatencyProbeTest, SlowSidecarWriteEmitsTheLatencyWarning) {
    const std::filesystem::path src = stageSourceAt(srcDir_ / "slow.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    AutosaveManager mgr(doc.get(), /*undo=*/nullptr, src, recoveryDir_);

    ASSERT_EQ(AutosaveManager::sidecarWriteWarningCount(), 0)
        << "probe must start un-fired, or the count below proves nothing";

    installSlowWrite();
    doc->markModified();
    ASSERT_TRUE(mgr.autosaveNow())
        << "a delayed write must still SUCCEED -- the probe diagnoses latency, "
           "it must not turn a slow autosave into a failed one";

    EXPECT_EQ(AutosaveManager::sidecarWriteWarningCount(), 1)
        << "a sidecar write exceeding kSidecarWriteWarnMs ("
        << AutosaveManager::kSidecarWriteWarnMs
        << " ms) must emit the autosave write-latency warning";

    // The autosave must have actually produced its artifacts; a probe that
    // fired because the write was slow AND broken would be measuring nothing.
    EXPECT_TRUE(std::filesystem::exists(mgr.sidecarPath()))
        << "the slow write must still have produced a sidecar";
    EXPECT_TRUE(std::filesystem::exists(mgr.recoveryFilePath()))
        << "the slow write must still have produced a recovery copy";
}

// ---------------------------------------------------------------------------
// 2. It warns ONCE. (The criterion's actual property.)
// ---------------------------------------------------------------------------
//
// This is the case that distinguishes the implemented behaviour from the
// obvious wrong one. A scanner that delays one write delays every subsequent
// one, and autosave runs on a timer, so a warn-per-occurrence probe would emit
// indefinitely and bury the first, most useful report. THREE slow writes, one
// warning.
TEST_F(AutosaveWriteLatencyProbeTest,
       RepeatedSlowWritesWarnExactlyOncePerProcess) {
    const std::filesystem::path src = stageSourceAt(srcDir_ / "repeat.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    AutosaveManager mgr(doc.get(), /*undo=*/nullptr, src, recoveryDir_);
    installSlowWrite();

    constexpr int kSlowWrites = 3;
    for (int i = 0; i < kSlowWrites; ++i) {
        doc->markModified();
        ASSERT_TRUE(mgr.autosaveNow()) << "slow autosave #" << (i + 1)
                                       << " must still succeed";
    }

    EXPECT_EQ(AutosaveManager::sidecarWriteWarningCount(), 1)
        << kSlowWrites
        << " slow sidecar writes must produce exactly ONE warning, not one per "
           "write -- the probe is latched per process so a persistently slow "
           "machine reports the condition once instead of on every tick";
}

// ---------------------------------------------------------------------------
// 3. A SECOND DOCUMENT does not earn a second warning.
// ---------------------------------------------------------------------------
//
// The latch is process-wide rather than per-instance because the condition
// being reported is a property of the machine, not of one document. A
// per-instance latch would pass case 2 above and still warn once per open
// document, so this pins the distinction those two cases cannot.
TEST_F(AutosaveWriteLatencyProbeTest,
       ASecondDocumentDoesNotEarnASecondWarning) {
    const std::filesystem::path srcA = stageSourceAt(srcDir_ / "docA.pdf");
    const std::filesystem::path srcB = stageSourceAt(srcDir_ / "docB.pdf");
    auto docA = Document::open(ctx_, srcA);
    auto docB = Document::open(ctx_, srcB);
    ASSERT_NE(docA, nullptr);
    ASSERT_NE(docB, nullptr);

    AutosaveManager mgrA(docA.get(), /*undo=*/nullptr, srcA, recoveryDir_);
    AutosaveManager mgrB(docB.get(), /*undo=*/nullptr, srcB, recoveryDir_);
    installSlowWrite();

    docA->markModified();
    ASSERT_TRUE(mgrA.autosaveNow());
    docB->markModified();
    ASSERT_TRUE(mgrB.autosaveNow());

    // Both really did write -- otherwise "only one warning" could just mean
    // "only one write happened", which would be a vacuous pass.
    ASSERT_TRUE(std::filesystem::exists(mgrA.sidecarPath()));
    ASSERT_TRUE(std::filesystem::exists(mgrB.sidecarPath()));
    ASSERT_NE(mgrA.sidecarPath(), mgrB.sidecarPath())
        << "the two managers must be writing DIFFERENT sidecars";

    EXPECT_EQ(AutosaveManager::sidecarWriteWarningCount(), 1)
        << "two documents each doing a slow sidecar write must still produce "
           "exactly one warning: the latch is per PROCESS, because a scanner "
           "delaying our writes is a machine condition, not a per-document one";
}

// ---------------------------------------------------------------------------
// 4. A NORMAL write does not warn. (The probe is not stuck on.)
// ---------------------------------------------------------------------------
//
// Without this, a probe hard-wired to `warn()` unconditionally would pass case
// 1. No hook is installed, so the write is whatever the real filesystem does —
// and the threshold's whole justification (two orders of magnitude above the
// expected few milliseconds) is that this cannot legitimately be exceeded.
TEST_F(AutosaveWriteLatencyProbeTest, ANormalSpeedSidecarWriteDoesNotWarn) {
    const std::filesystem::path src = stageSourceAt(srcDir_ / "normal.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    AutosaveManager mgr(doc.get(), /*undo=*/nullptr, src, recoveryDir_);
    // Deliberately NO probe hook.
    doc->markModified();
    ASSERT_TRUE(mgr.autosaveNow());
    ASSERT_TRUE(std::filesystem::exists(mgr.sidecarPath()))
        << "the write must have happened for its duration to mean anything";

    EXPECT_EQ(AutosaveManager::sidecarWriteWarningCount(), 0)
        << "an ordinary sidecar write (a few hundred bytes through QSaveFile) "
           "must not trip the "
        << AutosaveManager::kSidecarWriteWarnMs
        << " ms threshold; if this reddens, either the threshold is wrong or "
           "this machine really does have something inspecting its writes";
}

// ---------------------------------------------------------------------------
// 5. The threshold is a documented, sane value.
// ---------------------------------------------------------------------------
//
// Pins the constant itself so that "documented threshold" cannot decay into a
// number somebody nudged to make a test green. The bounds are the two failure
// modes the rationale in AutosaveManager.h argues between: too low and ordinary
// scheduler jitter warns; too high and a user-perceptible stall does not.
TEST_F(AutosaveWriteLatencyProbeTest, ThresholdIsWithinItsDocumentedBounds) {
    EXPECT_GE(AutosaveManager::kSidecarWriteWarnMs, 50)
        << "below ~50 ms the threshold is within reach of ordinary scheduler "
           "jitter on a busy or virtualised machine, and the probe would cry "
           "wolf";
    EXPECT_LE(AutosaveManager::kSidecarWriteWarnMs, 1000)
        << "at or above 1 s the delay is already a user-perceptible stall, so "
           "a threshold that high would only report the condition after it had "
           "stopped being a diagnostic and started being a bug report";
}

}  // namespace
