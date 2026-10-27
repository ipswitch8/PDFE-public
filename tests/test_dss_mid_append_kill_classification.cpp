// tests/test_dss_mid_append_kill_classification.cpp
//
// GitHub #291 — a LATE kill and genuine data loss must not read the same.
//
// `DssMidAppendKill.KillMidAppendLeavesTheSignedDocumentByteIdentical` reported
// "the signature this operation existed to strengthen has been destroyed" on a
// run where that is not what happened: the kill simply landed after the child
// had finished its /DSS write, so nothing was injected mid-append at all. The
// three signals the parent had — childLaunched, childReachedWindow, childKilled
// — cannot tell those apart. `childKilled` is set from TerminateProcess's return
// value, and that says nothing about TIMING.
//
// THE SIGNAL WAS ALREADY THERE, UNREAD
// ====================================
// The harness kills with a distinctive code:
//
//     TerminateProcess(pi.hProcess, 0xF10DEAD)
//
// Windows stamps that code onto the process ONLY if it was still running. A
// child that already exited keeps its own exit code, and `GetExitCodeProcess`
// — which the harness already calls, into `KilledSaveResult::exitCode` —
// returns that instead. So the parent has always been able to tell a kill that
// landed from one that arrived too late; the oracle just never looked.
//
// This file does two separate jobs, and the split matters:
//
//   1. VALIDATE THE PREMISE on this OS, by spawning a real child, waiting for
//      it to exit, and only THEN calling TerminateProcess. If Windows were to
//      overwrite the exit code of an already-dead process, the whole
//      classification would be built on a false assumption — so it is measured
//      here rather than cited from documentation.
//
//   2. TEST THE DECISION, by driving `classifyMidAppendKill` over every
//      combination. The classifier is a pure function precisely so this is
//      possible without racing a real kill: reproducing a late kill on demand
//      is exactly the thing nobody can do reliably, which is why the defect
//      survived as a "flake" in the first place.

#include <gtest/gtest.h>

#include "support/SaveFailureInjection.h"

#include <chrono>
#include <iterator>   // std::size, used by TheVerdictsAreDistinct
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#endif

using pdfe::testing::MidAppendVerdict;
using pdfe::testing::classifyMidAppendKill;
using pdfe::testing::kKilledWhileRunningExitCode;

// ---------------------------------------------------------------------------
// 1. The premise.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
TEST(MidAppendKillClassification, TerminatingAnAlreadyExitedChildKeepsItsExitCode)
{
    // A child that exits on its own with a code we choose, then TerminateProcess
    // after it is already gone. This is the LATE-KILL shape, reproduced
    // deliberately instead of waited for.
    const DWORD kChildOwnCode = 7;

    wchar_t cmd[] = L"cmd.exe /c exit 7";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    ASSERT_TRUE(::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        << "could not spawn a throwaway child, GetLastError="
        << ::GetLastError();

    // Let it finish completely. This is the opposite of the race the harness
    // normally runs: here we WANT the child dead before we kill it.
    ASSERT_EQ(::WaitForSingleObject(pi.hProcess, 30000), WAIT_OBJECT_0)
        << "the throwaway child did not exit within 30 s";

    DWORD before = 0;
    ASSERT_TRUE(::GetExitCodeProcess(pi.hProcess, &before));
    ASSERT_EQ(before, kChildOwnCode)
        << "the child did not exit with its own code, so this test cannot say "
           "anything about what TerminateProcess does to it";

    // Now the late kill.
    ::TerminateProcess(pi.hProcess, kKilledWhileRunningExitCode);

    DWORD after = 0;
    ASSERT_TRUE(::GetExitCodeProcess(pi.hProcess, &after));
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    EXPECT_EQ(after, kChildOwnCode)
        << "TerminateProcess OVERWROTE the exit code of an already-exited "
           "process (now 0x" << std::hex << after << std::dec << "). The #291 "
           "classification reads a late kill from exitCode != "
           "kKilledWhileRunningExitCode, and that premise would be false on "
           "this OS -- the classifier must be rebuilt on a different signal, "
           "not merely re-tuned.";
    EXPECT_NE(after, kKilledWhileRunningExitCode)
        << "an already-exited child came back carrying the harness's kill code, "
           "which would make a LATE kill indistinguishable from a landed one -- "
           "the exact conflation GitHub #291 is about.";
}
#endif

// ---------------------------------------------------------------------------
// 2. The decision.
// ---------------------------------------------------------------------------

TEST(MidAppendKillClassification, ALandedKillWithAnIntactFileIsThePassingCase)
{
    EXPECT_EQ(classifyMidAppendKill(kKilledWhileRunningExitCode,
                                    /*sizeChanged=*/false,
                                    /*byteIdentical=*/true),
              MidAppendVerdict::KillLandedAndDocumentIntact);
}

TEST(MidAppendKillClassification, ALandedKillThatChangedTheFileIsDataLoss)
{
    // The alarming message is CORRECT here and must stay available: the kill
    // demonstrably interrupted a write, and the user's only copy moved.
    EXPECT_EQ(classifyMidAppendKill(kKilledWhileRunningExitCode,
                                    /*sizeChanged=*/true,
                                    /*byteIdentical=*/false),
              MidAppendVerdict::DataLoss);
}

TEST(MidAppendKillClassification, ALandedKillThatResizedButKeptBytesIsStillDataLoss)
{
    // Defensive: a size change with byteIdentical somehow true is incoherent,
    // and must not be reported as intact. Incoherent evidence is not a pass.
    EXPECT_EQ(classifyMidAppendKill(kKilledWhileRunningExitCode,
                                    /*sizeChanged=*/true,
                                    /*byteIdentical=*/true),
              MidAppendVerdict::DataLoss);
}

TEST(MidAppendKillClassification, AChildThatFinishedFirstIsALateKillNotDataLoss)
{
    // THE #291 CASE. The child exited 0 on its own, so the write completed and
    // nothing was injected mid-append. The file having grown is the EXPECTED
    // result of a completed /DSS append -- it is not evidence of data loss, and
    // reporting it as "the signature has been destroyed" is what this issue was
    // filed about.
    EXPECT_EQ(classifyMidAppendKill(/*exitCode=*/0,
                                    /*sizeChanged=*/true,
                                    /*byteIdentical=*/false),
              MidAppendVerdict::KillLandedLate)
        << "a child that exited cleanly before the kill was classified as data "
           "loss -- this is GitHub #291 exactly";
}

TEST(MidAppendKillClassification, ALateKillIsNotRescuedByAnIntactFileEither)
{
    // Still a late kill, still proves nothing: if the kill never interrupted
    // anything, an intact file is not evidence that interruption is safe. This
    // direction matters more than it looks -- it is the one that would turn the
    // control into a test that passes whenever the race is lost.
    EXPECT_EQ(classifyMidAppendKill(/*exitCode=*/0,
                                    /*sizeChanged=*/false,
                                    /*byteIdentical=*/true),
              MidAppendVerdict::KillLandedLate);
}

TEST(MidAppendKillClassification, AChildThatDiedOfSomethingElseIsItsOwnVerdict)
{
    // Neither the harness's code nor a clean exit: the child crashed. That is
    // not data loss and not a late kill, and collapsing it into either would
    // hide a victim that is itself broken.
    EXPECT_EQ(classifyMidAppendKill(/*exitCode=*/0xC0000005,
                                    /*sizeChanged=*/true,
                                    /*byteIdentical=*/false),
              MidAppendVerdict::ChildDiedUnexpectedly);
}

// ---------------------------------------------------------------------------
// 3. What the PRE-FIX oracle did with the same evidence.
//
// The old oracle had no classification step: it went straight from
// `ASSERT_TRUE(r.childKilled)` to `EXPECT_EQ(after.size(), before.size())`.
// That is exactly "ignore the exit code", so it is expressible here, and the
// comparison is the point -- GitHub #291's criterion asks for a test that
// previously misreported one case as the other and now reports both correctly.
//
// Encoding the old behaviour rather than describing it means this file fails if
// anyone reintroduces it, and makes the divergence a measurement instead of a
// claim about history.
// ---------------------------------------------------------------------------
namespace {

/// The pre-#291 oracle, reduced to its decision: the exit code is not read.
MidAppendVerdict classifyIgnoringExitCode(bool sizeChanged, bool byteIdentical)
{
    if (sizeChanged || !byteIdentical) return MidAppendVerdict::DataLoss;
    return MidAppendVerdict::KillLandedAndDocumentIntact;
}

}  // namespace

TEST(MidAppendKillClassification, ThePreFixOracleCalledTheLateKillDataLoss)
{
    // The exact shape of the reported failure: the child exited 0 on its own,
    // and the file had grown because the append COMPLETED.
    const unsigned long lateKillExit = 0;
    const bool sizeChanged = true;
    const bool byteIdentical = false;

    EXPECT_EQ(classifyIgnoringExitCode(sizeChanged, byteIdentical),
              MidAppendVerdict::DataLoss)
        << "the pre-fix decision is not being reproduced, so the comparison "
           "below proves nothing";

    EXPECT_EQ(classifyMidAppendKill(lateKillExit, sizeChanged, byteIdentical),
              MidAppendVerdict::KillLandedLate);

    EXPECT_NE(classifyMidAppendKill(lateKillExit, sizeChanged, byteIdentical),
              classifyIgnoringExitCode(sizeChanged, byteIdentical))
        << "the fixed classifier agrees with the pre-fix one on the very input "
           "GitHub #291 was filed about, so nothing has actually changed";
}

TEST(MidAppendKillClassification, TheFixDidNotWeakenTheRealDataLossCase)
{
    // The other direction, and the one that matters for not having traded a
    // false alarm for a missed one: when the kill DID land, the two agree, so
    // genuine data loss is still reported as data loss.
    const bool sizeChanged = true;
    const bool byteIdentical = false;

    EXPECT_EQ(classifyMidAppendKill(kKilledWhileRunningExitCode, sizeChanged,
                                    byteIdentical),
              MidAppendVerdict::DataLoss);
    EXPECT_EQ(classifyMidAppendKill(kKilledWhileRunningExitCode, sizeChanged,
                                    byteIdentical),
              classifyIgnoringExitCode(sizeChanged, byteIdentical))
        << "the fix changed the verdict for a kill that genuinely landed -- it "
           "was only supposed to change the LATE case. Suppressing a real data "
           "loss would be far worse than the false alarm it replaced.";
}

TEST(MidAppendKillClassification, TheVerdictsAreDistinct)
{
    // Guards the enum itself: if two verdicts ever compared equal, every test
    // above would still pass while the oracle lost the distinction #291 is
    // about.
    const MidAppendVerdict all[] = {
        MidAppendVerdict::KillLandedAndDocumentIntact,
        MidAppendVerdict::DataLoss,
        MidAppendVerdict::KillLandedLate,
        MidAppendVerdict::ChildDiedUnexpectedly,
    };
    for (size_t i = 0; i < std::size(all); ++i) {
        for (size_t j = i + 1; j < std::size(all); ++j) {
            EXPECT_NE(all[i], all[j])
                << "verdicts " << i << " and " << j << " are the same value";
        }
    }
}
