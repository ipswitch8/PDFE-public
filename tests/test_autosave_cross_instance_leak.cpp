// -*- coding: utf-8 -*-
//
// GitHub #97 — cross-instance recovery-prompt leak.
//
// AutosaveManager::defaultRecoveryDir() is per-USER, not per-instance or
// per-session, so every concurrently-running PDFE owned by the same user writes
// its recovery sidecars into the SAME directory. scanForRecoverable() filtered
// only on: sidecar suffix, parseable JSON, non-empty sessionId, and "the
// recovery copy still exists". None of those distinguish
//
//     a CRASHED instance's sidecar          (must be offered — the whole point)
// from
//     a still-LIVE second instance's sidecar (must NOT be offered — that is
//                                             someone's in-progress editing)
//
// so instance 1's startup recovery prompt offered instance 2's live work.
//
// The fix stamps each sidecar with the writing process's identity (pid + the
// pid's incarnation token, i.e. process creation time) and withholds a record
// only on POSITIVE PROOF that its owner is still running.
//
// recoveryDir is a constructor/scan parameter, so all of this is exercised
// against synthetic sidecars in a temp dir — no second Windows session needed.
// The "still-live foreign instance" is a REAL live child process, not a
// fabricated pid: a made-up number would make the central oracle depend on
// luck about what happens to be running.

#include <gtest/gtest.h>

#ifdef _WIN32
// GitHub #285 cluster G: the regression test below opens its own
// process handle to reproduce the exited-but-queryable condition.
#include <windows.h>
#endif

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/AutosaveManager.h"

using pdfe::core::AutosaveManager;
using pdfe::core::RecoveryRecord;

namespace {

constexpr const char* kSidecarSuffix = ".pdfe_recovery.json";

QString qs(const std::filesystem::path& p) {
    return QString::fromStdU16String(p.u16string());
}

}  // namespace

class AutosaveCrossInstanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tmp_.isValid());
        dir_ = std::filesystem::path(tmp_.path().toStdU16String());
    }

    void TearDown() override {
        if (child_.state() != QProcess::NotRunning) {
            child_.kill();
            child_.waitForFinished(10000);
        }
    }

    /// Start a real, long-lived child process and return its pid. Used as a
    /// genuinely-live "other instance" owner.
    std::uint64_t startLiveChild() {
        // Start the LEAF process directly (not via cmd.exe): QProcess::kill()
        // then terminates the very process whose pid we stamped, leaving no
        // orphan behind to wedge a later run.
#ifdef _WIN32
        child_.start("ping", {"-n", "60", "127.0.0.1"});
#else
        child_.start("/bin/sleep", {"60"});
#endif
        if (!child_.waitForStarted(15000)) return 0;
        return static_cast<std::uint64_t>(child_.processId());
    }

    /// A pid that is definitively NOT running: spawn a trivial process, wait
    /// for it to exit, then use its pid. Stands in for a crashed instance.
    /// Fabricating a "probably unused" number would make the control depend on
    /// luck about what happens to be running.
    ///
    /// MEASURED: Windows recycles a just-exited pid onto a new process often
    /// enough to matter — 2 failures in 6 runs of this suite, where the
    /// "crashed" pid came back alive with a fresh incarnation token. So the
    /// spawn is RETRIED until the pid is verifiably gone. (The production
    /// filter is unaffected by this: a recycled pid carries a different
    /// incarnation token, which is precisely why the stamp is a pair and not
    /// just a pid — see ForeignLivePredicateBranches case (2). It is only this
    /// test's precondition that needs the pid to be cleanly dead.)
    static std::uint64_t deadPid() {
        constexpr int kAttempts = 25;
        for (int i = 0; i < kAttempts; ++i) {
            std::uint64_t pid = 0;
            {
                // SCOPE IS LOAD-BEARING (GitHub #231). The QProcess must be
                // DESTROYED -- releasing its handle on the child -- before the
                // pid is tested, because Windows keeps an exited process
                // queryable for as long as ANY handle to it remains open.
                //
                // Measured on this machine, 10 iterations each:
                //   exited pid queryable while the parent holds the handle:
                //     10/10
                //   exited pid queryable after the handle is released:  0/10
                //
                // With the QProcess still in scope, OpenProcess therefore
                // SUCCEEDS on a process that has already exited,
                // processStartToken() returns its real creation time, the
                // `== 0` below never holds, all 25 attempts are consumed and
                // deadPid() returns 0 -- which the callers ASSERT on. That is
                // the "deadPid() cannot confirm a dead PID under load"
                // symptom: not load exactly, but handle lifetime, which load
                // and scheduling perturb by changing when Qt gets round to
                // its own cleanup.
                //
                // Do not collapse this block back into the loop body.
                QProcess p;
#ifdef _WIN32
                p.start("cmd.exe", {"/c", "exit"});
#else
                p.start("/bin/sh", {"-c", "exit"});
#endif
                if (!p.waitForStarted(15000)) continue;
                pid = static_cast<std::uint64_t>(p.processId());
                p.waitForFinished(15000);
            }
            // WAIT FOR THE TEARDOWN, do not judge on the first look.
            //
            // Releasing the handle does not make the pid unqueryable
            // immediately: the kernel tears the process object down a short,
            // variable time afterwards. Measured here, one attempt, tokens
            // read from the same pid:
            //
            //   straight after the QProcess is destroyed : non-zero
            //   after QCoreApplication::processEvents()  : non-zero (so this
            //                                              is not deferred Qt
            //                                              cleanup waiting on
            //                                              an event loop)
            //   after a further ~200 ms                  : 0
            //
            // The previous code asked exactly once, at the first of those
            // three moments, and therefore threw away a pid that was about to
            // become verifiably dead. The 25 outer attempts never helped
            // because every one of them made the same mistake at the same
            // point -- which is why this reads as "deadPid() cannot confirm a
            // dead PID", and why it looks load-dependent: load changes how
            // long the teardown takes, not whether it happens.
            for (int waited = 0; pid != 0 && waited < 100; ++waited) {  // <=1 s
                if (AutosaveManager::processStartToken(pid) == 0) {
                    return pid;  // verifiably gone
                }
                QThread::msleep(10);
            }
            // Still queryable after a full second: the OS handed this pid
            // straight to something else. Try a fresh one rather than assert
            // against a pid that is not actually dead.
        }
        return 0;  // callers ASSERT on this
    }

    /// Write a synthetic recovery pair (an existing "copy" file + a sidecar).
    /// @param ownerPid   0 → omit the stamp entirely (legacy schema-1 sidecar).
    /// @param ownerToken 0 → omit the token but keep the pid.
    void writeSidecar(const std::string& sessionId,
                      std::uint64_t ownerPid,
                      std::uint64_t ownerToken) {
        const std::filesystem::path copy = dir_ / (sessionId + ".pdf");
        {
            QFile f(qs(copy));
            ASSERT_TRUE(f.open(QIODevice::WriteOnly));
            f.write("%PDF-1.7\n% synthetic recovery copy\n");
            f.close();
        }

        QJsonObject obj;
        obj["originalPath"] = QString::fromStdString("C:/docs/" + sessionId + ".pdf");
        obj["recoveryFilePath"] = qs(copy);
        obj["timestamp"] = "2026-09-11T12:00:00.000Z";
        obj["sessionId"] = QString::fromStdString(sessionId);
        if (ownerPid != 0) {
            obj["ownerPid"] = QString::number(static_cast<qulonglong>(ownerPid));
            obj["schema"] = 2;
            if (ownerToken != 0) {
                obj["ownerStartToken"] =
                    QString::number(static_cast<qulonglong>(ownerToken));
            }
        } else {
            obj["schema"] = 1;
        }

        QFile sc(qs(dir_ / (sessionId + kSidecarSuffix)));
        ASSERT_TRUE(sc.open(QIODevice::WriteOnly | QIODevice::Truncate));
        sc.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        sc.close();
    }

    std::vector<std::string> offeredSessionIds() const {
        std::vector<std::string> ids;
        for (const auto& r : AutosaveManager::scanForRecoverable(dir_)) {
            ids.push_back(r.sessionId);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    static bool contains(const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    }

    QTemporaryDir tmp_;
    std::filesystem::path dir_;
    QProcess child_;
};

// THE DEFECT. Two sidecars in one per-user recovery dir: one from a crashed
// instance, one from a second instance that is STILL RUNNING. Only the crashed
// one may be offered.
TEST_F(AutosaveCrossInstanceTest, LiveSecondInstanceIsNotOfferedButCrashedOneIs) {
    const std::uint64_t livePid = startLiveChild();
    ASSERT_NE(livePid, 0u) << "could not start the live-owner child process";
    const std::uint64_t liveToken = AutosaveManager::processStartToken(livePid);
    ASSERT_NE(liveToken, 0u)
        << "no incarnation token for a process we just started (pid " << livePid
        << ") — the oracle below would degenerate into the fail-open path and "
           "pass vacuously";

    const std::uint64_t dead = deadPid();
    ASSERT_NE(dead, 0u)
        << "could not obtain a verifiably-dead pid in 25 attempts — the "
           "crashed-instance control cannot be set up, so this test must not "
           "report a pass";

    // A token value for the crashed owner. Its exact value is irrelevant: the
    // pid is gone, so nothing can match it.
    writeSidecar("crashed-session", dead, 0x0123456789ABCDEFull);
    writeSidecar("live-other-session", livePid, liveToken);

    const auto ids = offeredSessionIds();

    EXPECT_TRUE(contains(ids, "crashed-session"))
        << "a crashed instance's recovery MUST still be offered — hiding it is "
           "silent data loss, the failure this filter is explicitly biased "
           "against";
    EXPECT_FALSE(contains(ids, "live-other-session"))
        << "GitHub #97: a concurrently-running second instance's in-progress "
           "recovery file was offered to this instance's recovery prompt (pid "
        << livePid << " is alive with token " << liveToken << ")";
    EXPECT_EQ(ids.size(), 1u) << "exactly one record should survive the filter";
}

// The predicate itself, exercised directly across every branch — so a change
// that makes the filter accidentally unconditional is caught even if the
// scan-level oracle above were to drift.
TEST_F(AutosaveCrossInstanceTest, ForeignLivePredicateBranches) {
    const std::uint64_t livePid = startLiveChild();
    ASSERT_NE(livePid, 0u);
    const std::uint64_t liveToken = AutosaveManager::processStartToken(livePid);
    ASSERT_NE(liveToken, 0u);

    RecoveryRecord rec;

    // (1) live foreign owner, matching token → withhold.
    rec.ownerPid = livePid;
    rec.ownerStartToken = liveToken;
    EXPECT_TRUE(AutosaveManager::isForeignLiveInstance(rec));

    // (2) PID REUSE: same live pid, DIFFERENT incarnation. The instance that
    //     wrote this is gone and an unrelated process inherited its pid — the
    //     record must be offered. A pid-only check would wrongly hide it.
    rec.ownerStartToken = liveToken ^ 0xFFFFull;
    EXPECT_FALSE(AutosaveManager::isForeignLiveInstance(rec))
        << "a recycled pid must not be mistaken for the original owner";

    // (3) dead owner → offer.
    const std::uint64_t dead = deadPid();
    ASSERT_NE(dead, 0u);
    rec.ownerPid = dead;
    rec.ownerStartToken = 0x0123456789ABCDEFull;
    EXPECT_FALSE(AutosaveManager::isForeignLiveInstance(rec));

    // (4) legacy sidecar, no stamp → offer (fail open).
    rec.ownerPid = 0;
    rec.ownerStartToken = 0;
    EXPECT_FALSE(AutosaveManager::isForeignLiveInstance(rec));

    // (5) pid but no token → offer (a bare pid is not an identity).
    rec.ownerPid = livePid;
    rec.ownerStartToken = 0;
    EXPECT_FALSE(AutosaveManager::isForeignLiveInstance(rec));

    // (6) our OWN pid → not foreign, offered. The simulated-crash contract
    //     (autosave, drop the manager, re-scan, all in one process) depends on
    //     this, and at a real startup we have written no sidecar yet.
    rec.ownerPid = AutosaveManager::currentProcessId();
    rec.ownerStartToken =
        AutosaveManager::processStartToken(AutosaveManager::currentProcessId());
    EXPECT_FALSE(AutosaveManager::isForeignLiveInstance(rec));
}

// Backward compatibility: an unstamped schema-1 sidecar left by an older build
// must still be recoverable. Fail-open means an upgrade never eats a pending
// recovery.
TEST_F(AutosaveCrossInstanceTest, LegacyUnstampedSidecarIsStillOffered) {
    writeSidecar("legacy-session", /*ownerPid=*/0, /*ownerToken=*/0);
    const auto ids = offeredSessionIds();
    EXPECT_TRUE(contains(ids, "legacy-session"))
        << "a schema-1 sidecar has no owner stamp; unknown ownership must "
           "resolve to OFFER, never to hide";
}

// Same-process sidecars stay visible — this is the shape every existing
// simulated-crash test uses.
TEST_F(AutosaveCrossInstanceTest, OwnProcessSidecarIsStillOffered) {
    const std::uint64_t self = AutosaveManager::currentProcessId();
    ASSERT_NE(self, 0u);
    writeSidecar("self-session", self, AutosaveManager::processStartToken(self));
    EXPECT_TRUE(contains(offeredSessionIds(), "self-session"));
}

// discardAllRecoverable() is built on scanForRecoverable(), so the filter must
// also keep it from DELETING a live instance's recovery files.
TEST_F(AutosaveCrossInstanceTest, DiscardAllDoesNotDeleteALiveInstancesFiles) {
    const std::uint64_t livePid = startLiveChild();
    ASSERT_NE(livePid, 0u);
    const std::uint64_t liveToken = AutosaveManager::processStartToken(livePid);
    ASSERT_NE(liveToken, 0u);

    writeSidecar("live-other-session", livePid, liveToken);
    writeSidecar("crashed-session", deadPid(), 0x1122334455667788ull);

    const std::size_t discarded = AutosaveManager::discardAllRecoverable(dir_);
    EXPECT_EQ(discarded, 1u);

    EXPECT_TRUE(std::filesystem::exists(dir_ / "live-other-session.pdf"))
        << "the running instance's recovery copy was deleted out from under it";
    EXPECT_TRUE(std::filesystem::exists(
        dir_ / (std::string("live-other-session") + kSidecarSuffix)));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "crashed-session.pdf"));
}

// The identity token must be per-incarnation, not merely per-pid.
TEST_F(AutosaveCrossInstanceTest, StartTokenIdentifiesTheIncarnation) {
    const std::uint64_t self = AutosaveManager::currentProcessId();
    ASSERT_NE(self, 0u);
    const std::uint64_t a = AutosaveManager::processStartToken(self);
    EXPECT_NE(a, 0u) << "our own process must have a token";
    EXPECT_EQ(a, AutosaveManager::processStartToken(self)) << "token must be stable";

    EXPECT_EQ(AutosaveManager::processStartToken(0), 0u);
    const std::uint64_t dead = deadPid();
    ASSERT_NE(dead, 0u) << "could not obtain a verifiably-dead pid";
    EXPECT_EQ(AutosaveManager::processStartToken(dead), 0u)
        << "a pid with no live process must have no token";

    const std::uint64_t livePid = startLiveChild();
    ASSERT_NE(livePid, 0u);
    const std::uint64_t childToken = AutosaveManager::processStartToken(livePid);
    EXPECT_NE(childToken, 0u);
    EXPECT_NE(childToken, a)
        << "two different live processes must not share an incarnation token";
}

// ===========================================================================
// GitHub #285 cluster G — an EXITED process whose handle is still held must
// not be reported as a live owner.
// ===========================================================================
//
// THE DEFECT THIS PINS. A terminated process keeps its pid and kernel object
// alive for as long as anyone holds a handle to it. `OpenProcess` therefore
// still succeeds and `GetProcessTimes` still returns the ORIGINAL creation
// time, so the incarnation token MATCHES and `isForeignLiveInstance()`
// concludes the owner is still running. A genuinely crashed session's recovery
// is then withheld — failure direction (b) in that function's own comment,
// which it calls strictly worse because the user's unsaved work becomes
// silently unreachable.
//
// Measured end-to-end before the fix: PDFE force-killed at pid 4992, relaunched,
// and the startup scan rejected the sidecar with "owner still live (pid 4992)"
// while the recovery prompt never appeared — `test_ui_autosave_recovery`'s two
// failures in GitHub #285 cluster G.
//
// This file already KNEW about the behaviour: `deadPid()` above documents it
// ("exited pid queryable while the parent holds the handle: 10/10", GitHub
// #231) and works around it by destroying the QProcess and waiting ~200 ms for
// the kernel teardown. That made the unit tests pass while leaving the
// production predicate wrong, which is why the consequence only ever surfaced
// through the UI. Working around a defect in the test is how it survives.
//
// The fix: `processStartToken()` waits on the handle (`WaitForSingleObject(h,
// 0) == WAIT_OBJECT_0` ⇒ exited) and returns 0, which the filter reads as
// "unknown owner → offer the record". `GetExitCodeProcess` is unusable here —
// it returns STILL_ACTIVE (259) for a live process and 259 is also a legal
// exit code.
TEST_F(AutosaveCrossInstanceTest, ExitedProcessWithAHeldHandleIsNotLive) {
    // THE TOKEN MUST BE THE REAL ONE, CAPTURED WHILE THE CHILD IS ALIVE.
    //
    // A first version of this test spawned `cmd.exe /c exit`, let it finish,
    // and then asserted with `ownerStartToken = 123456789ULL` — an arbitrary
    // constant. The karen gate ran the mutation proof and the MUTANT SURVIVED:
    // the test passed identically with and without the `WaitForSingleObject`
    // fix, so it was not protecting the defect at all. Two reasons, both worth
    // keeping in mind for any test of this predicate:
    //
    //   1. a process that exits that fast may already be un-queryable by the
    //      time we ask, so processStartToken() returns 0 for reasons having
    //      nothing to do with the fix; and
    //   2. an arbitrary token cannot match the real creation time, so
    //      isForeignLiveInstance() returns false whatever the fix does — the
    //      assertion was vacuous by construction.
    //
    // So: start a LONG-LIVED child, read its genuine token while it is running,
    // then kill it and keep the handle. Now the only thing that can make
    // processStartToken() return 0 is the exit check itself, and the only thing
    // that can stop the real token matching is the same check. Remove the fix
    // and both assertions below fail.
#ifndef _WIN32
    GTEST_SKIP() << "the exited-but-openable condition is a Win32 handle "
                    "lifetime property; there is nothing to pin elsewhere";
#else
    const std::uint64_t pid = startLiveChild();
    ASSERT_NE(pid, 0u) << "could not start the long-lived child";

    const std::uint64_t liveToken = AutosaveManager::processStartToken(pid);
    ASSERT_NE(liveToken, 0u)
        << "precondition: a RUNNING child must have a real incarnation token; "
           "without one the assertions below could pass for the wrong reason";

    // WE MUST HOLD THE HANDLE OURSELVES.
    //
    // Relying on QProcess to keep the exited process queryable does not work:
    // QProcess releases its handle as soon as it reaps the child, so after
    // waitForFinished() the pid becomes un-queryable and processStartToken()
    // returns 0 for that reason alone — with or without the fix. That is
    // exactly how the first version of this test let the mutant survive, and it
    // is the same handle-lifetime behaviour `deadPid()` above documents from
    // the other direction ("queryable while the parent holds the handle: 10/10,
    // after it is released: 0/10").
    //
    // An explicit handle, opened while the child is alive and held across its
    // death, is what reproduces the condition the production code gets wrong.
    HANDLE held = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                FALSE, static_cast<DWORD>(pid));
    ASSERT_NE(held, nullptr) << "could not open a handle to the live child";

    child_.kill();
    ASSERT_TRUE(child_.waitForFinished(15000));
    ASSERT_EQ(child_.state(), QProcess::NotRunning)
        << "precondition: the child must actually be dead";

    // `held` keeps the exited process queryable, which is the condition under
    // which the old code reported a live owner. Closed at the end of the test.
    EXPECT_EQ(AutosaveManager::processStartToken(pid), 0u)
        << "a process that has EXITED must have no incarnation token even while "
           "a handle to it is still open; reporting one makes "
           "isForeignLiveInstance() withhold a real crashed session's recovery";

    // And the predicate that actually gates recovery must agree — asserted with
    // the process's REAL token, which is what a sidecar written by that process
    // would carry.
    RecoveryRecord rec;
    rec.sessionId = "exited-owner";
    rec.ownerPid = pid;
    rec.ownerStartToken = liveToken;
    EXPECT_FALSE(AutosaveManager::isForeignLiveInstance(rec))
        << "an exited owner must never be treated as a live foreign instance; "
           "treating one as live is what silently hides a crashed session's "
           "unsaved work";

    ::CloseHandle(held);
#endif
}
