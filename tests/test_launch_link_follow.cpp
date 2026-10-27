// -*- coding: utf-8 -*-
// Pipeline E phase-13 (Bucket 3k): /Launch link-follow security audit.
//
// AC: PDFE never auto-executes /Launch actions — every click prompts
// the user, and declining the prompt MUST NOT call out to the OS
// shell. This is intentional security policy (matches Acrobat Pro's
// Trust Manager default, but PDFE has no opt-in trusted-source list,
// so EVERY launch always prompts — strictly more conservative than
// Acrobat).
//
// These tests use the static `setLaunchHandlerForTesting` seam on
// MainWindow to replace the real QMessageBox + QDesktopServices path
// with a deterministic capture, so we can assert:
//   1. The prompt was reached (launchPromptCountForTesting()++).
//   2. Declining the prompt does NOT execute the launch
//      (launchExecutedCountForTesting() stays at 0).
//   3. Accepting DOES execute (executed counter increments).
//
// AC outcome: status row stays at `~` (intentional partial) — PDFE
// matches Acrobat's safe default behavior but does not yet support
// the optional "always trust this source" allowlist Acrobat exposes
// in its Trust Manager. That gap is documented in the parity matrix,
// not papered over with a `✓`.
#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "annotations/LinkAction.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QApplication>
#include <QTemporaryDir>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

} // namespace

class LaunchLinkFollowTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::filesystem::exists(fixturesDir() / "test_3page.pdf")) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        workPdf_ = std::filesystem::path(tempDir_->path().toStdString())
                   / "launch_link_work.pdf";
        std::filesystem::copy_file(fixturesDir() / "test_3page.pdf",
                                   workPdf_);

        // Plant a /Launch link on page 0 with a deterministic local
        // path. The path doesn't need to exist — the prompt fires
        // before any filesystem access, and the test seam intercepts
        // the launch so no actual shell dispatch happens either way.
        {
            auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
            auto doc = pdfe::core::Document::open(ctx, workPdf_);
            auto page = doc->page(0);
            pdfe::annotations::AnnotationManager mgr(ctx);

            auto launchLink = mgr.createAnnotation(
                page.get(),
                pdfe::annotations::AnnotationType::Link,
                QRectF(50, 50, 200, 50));
            pdfe::annotations::LinkAction act;
            act.type = pdfe::annotations::LinkActionType::Launch;
            // Deliberately pick a path that will not actually launch
            // anything on the test machine even if the seam fails.
            act.launchPath = "C:/Windows/Temp/pdfe-test-nonexistent.txt";
            mgr.setLinkAction(page.get(), launchLink.annotIndex, act);

            auto savedPath = std::filesystem::path(
                tempDir_->path().toStdString())
                / "launch_link_planted.pdf";
            doc->saveAs(savedPath);
            workPdf_ = savedPath;
        }

        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        mw_->openFile(QString::fromStdString(workPdf_.string()));
        QApplication::processEvents();

        pdfe::ui::MainWindow::resetLaunchCountersForTesting();
        capturedPaths_.clear();
    }

    void TearDown() override {
        // Always restore default behavior so unrelated tests don't
        // see the seam.
        pdfe::ui::MainWindow::setLaunchHandlerForTesting({});
        pdfe::ui::MainWindow::resetLaunchCountersForTesting();
        mw_.reset();
        QApplication::processEvents();
        tempDir_.reset();
    }

    std::unique_ptr<QTemporaryDir> tempDir_;
    std::filesystem::path workPdf_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
    std::vector<QString> capturedPaths_;
};

// AC-1: clicking a /Launch link reaches the prompt (the prompt counter
// increments). Without the prompt, the launch could happen silently.
TEST_F(LaunchLinkFollowTest, LaunchClickAlwaysReachesThePrompt)
{
    pdfe::ui::MainWindow::setLaunchHandlerForTesting(
        [this](const QString& p) {
            capturedPaths_.push_back(p);
            return false;  // simulate decline
        });

    EXPECT_EQ(pdfe::ui::MainWindow::launchPromptCountForTesting(), 0);

    const bool handled = mw_->followLinkAt(0, QPointF(80.0, 70.0));
    EXPECT_TRUE(handled)
        << "/Launch click should be 'handled' even if user declines";

    // The prompt was reached exactly once. This is the security
    // contract: no auto-execute path that bypasses prompting.
    EXPECT_EQ(pdfe::ui::MainWindow::launchPromptCountForTesting(), 1);
    ASSERT_EQ(capturedPaths_.size(), 1u);
    EXPECT_TRUE(capturedPaths_[0].contains("pdfe-test-nonexistent"))
        << capturedPaths_[0].toStdString();
}

// AC-2: declining the prompt MUST NOT execute the launch — the
// executed counter stays at 0 and no shell dispatch happens. This is
// the core "no silent launch" guarantee.
TEST_F(LaunchLinkFollowTest, DeclinedPromptDoesNotExecuteLaunch)
{
    pdfe::ui::MainWindow::setLaunchHandlerForTesting(
        [](const QString&) { return false; /* decline */ });

    const bool handled = mw_->followLinkAt(0, QPointF(80.0, 70.0));
    EXPECT_TRUE(handled);

    EXPECT_EQ(pdfe::ui::MainWindow::launchPromptCountForTesting(), 1);
    EXPECT_EQ(pdfe::ui::MainWindow::launchExecutedCountForTesting(), 0)
        << "Decline must not call into the OS shell.";
}

// Sanity inverse: if the prompt is accepted, the executed counter DOES
// increment — proving the path through the if-accepted branch works
// and the decline test above isn't passing vacuously because the
// counter never increments at all.
TEST_F(LaunchLinkFollowTest, AcceptedPromptIncrementsExecutedCounter)
{
    pdfe::ui::MainWindow::setLaunchHandlerForTesting(
        [](const QString&) { return true; /* accept */ });

    const bool handled = mw_->followLinkAt(0, QPointF(80.0, 70.0));
    EXPECT_TRUE(handled);

    EXPECT_EQ(pdfe::ui::MainWindow::launchPromptCountForTesting(), 1);
    EXPECT_EQ(pdfe::ui::MainWindow::launchExecutedCountForTesting(), 1);
}

// Repeated clicks ALWAYS prompt — there is no "remember this choice"
// trust list. Each click goes through the prompt afresh. This is the
// core difference from Acrobat Pro's optional Trust Manager whitelist
// (not yet implemented in PDFE; documented as intentional).
TEST_F(LaunchLinkFollowTest, EveryLaunchClickReprompts_NoTrustListShortcut)
{
    pdfe::ui::MainWindow::setLaunchHandlerForTesting(
        [](const QString&) { return false; });

    mw_->followLinkAt(0, QPointF(80.0, 70.0));
    mw_->followLinkAt(0, QPointF(80.0, 70.0));
    mw_->followLinkAt(0, QPointF(80.0, 70.0));

    EXPECT_EQ(pdfe::ui::MainWindow::launchPromptCountForTesting(), 3)
        << "Each click must re-prompt. PDFE has no trusted-source "
           "allowlist; the prompt cannot be skipped.";
    EXPECT_EQ(pdfe::ui::MainWindow::launchExecutedCountForTesting(), 0);
}

// Mixed-decision scenario: accept once, decline twice. Counters reflect
// the per-click decisions, not aggregate "yes seen at least once".
TEST_F(LaunchLinkFollowTest, MixedAcceptDeclineSequenceCountedPerClick)
{
    int callIdx = 0;
    pdfe::ui::MainWindow::setLaunchHandlerForTesting(
        [&callIdx](const QString&) {
            const bool accept = (callIdx == 1);  // decline, accept, decline
            ++callIdx;
            return accept;
        });

    mw_->followLinkAt(0, QPointF(80.0, 70.0));  // decline
    mw_->followLinkAt(0, QPointF(80.0, 70.0));  // accept
    mw_->followLinkAt(0, QPointF(80.0, 70.0));  // decline

    EXPECT_EQ(pdfe::ui::MainWindow::launchPromptCountForTesting(), 3);
    EXPECT_EQ(pdfe::ui::MainWindow::launchExecutedCountForTesting(), 1);
}

// Pipeline F phase-1: thread-safety stress for the static seam state.
// The production race we are guarding against is the snapshot-read
// inside MainWindow::followLinkAt (which copies sLaunchHandler_ under
// the mutex) vs. setLaunchHandlerForTesting overwriting the handler
// on another thread. Pre-fix, sLaunchHandler_ was a plain static
// std::function — a write mid-read could tear the captured state.
//
// To actually exercise the production code path we need a Document
// with a /Launch annotation AND followLinkAt running on a real
// MainWindow. We reuse the LaunchLinkFollowTest fixture's setup logic
// but drive it from N threads. NB: Qt widget construction must stay
// on the gtest main thread; only followLinkAt is thread-safe-ish
// here because the seam path doesn't touch any QObject state. So the
// "follower" thread reads the counters / sets the handler; the main
// thread fires followLinkAt in a tight loop. That stresses the same
// race window.
TEST_F(LaunchLinkFollowTest, ConcurrentSetAndFollowDoesNotCrash)
{
    using pdfe::ui::MainWindow;

    constexpr int kWriters = 8;   // pipeline.json AC: N=8 threads.
    constexpr int kIters   = 500;
    constexpr int kFollows = 1000;

    // Baseline handler so the snapshot is NEVER empty during the
    // stress run — without this, a window between writer overwrites
    // could let followLinkAt fall through to a real QMessageBox and
    // hang the main thread.
    pdfe::ui::MainWindow::setLaunchHandlerForTesting(
        [](const QString&) { return false; });

    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    writers.reserve(kWriters);

    // Writers churn the handler set path under the mutex. Each
    // installs its own non-empty handler — there is no clear path,
    // so the snapshot is always non-empty (no fallthrough to
    // QMessageBox).
    for (int t = 0; t < kWriters; ++t) {
        writers.emplace_back([t, &stop]() {
            int local = 0;
            while (!stop.load(std::memory_order_acquire)
                   && local < kIters) {
                MainWindow::setLaunchHandlerForTesting(
                    [t](const QString&) { return (t & 1) == 0; });
                int p = MainWindow::launchPromptCountForTesting();
                int x = MainWindow::launchExecutedCountForTesting();
                EXPECT_GE(p, 0);
                EXPECT_GE(x, 0);
                ++local;
            }
        });
    }

    // Main thread (the only thread allowed to touch the QObject)
    // fires followLinkAt repeatedly. Each call goes through the
    // snapshot-under-lock path that we are stressing. The seam may
    // be set or unset at any moment — both code paths are valid.
    for (int i = 0; i < kFollows; ++i) {
        mw_->followLinkAt(0, QPointF(80.0, 70.0));
    }

    stop.store(true, std::memory_order_release);
    for (auto& th : writers) th.join();

    // The contract is "no crash, no data race." Counters must be
    // sensible and >= the number of follow calls (each followLinkAt
    // increments at least the prompt counter, regardless of whether
    // the handler was installed at that moment).
    EXPECT_GE(MainWindow::launchPromptCountForTesting(), kFollows);

    // Restore default for unrelated tests (TearDown also clears).
    MainWindow::setLaunchHandlerForTesting({});
    MainWindow::resetLaunchCountersForTesting();
}

// ---------------------------------------------------------------------------
// GitHub #61 (SEC-63) -- per-document /Launch prompt cap.
//
// PDFE has no trusted-source allowlist, so every /Launch prompts every time
// (asserted by EveryLaunchClickReprompts_NoTrustListShortcut above). That is
// the correct default AND the attack surface: a hostile PDF can carry an
// unbounded number of /Launch annotations, and walking a user through hundreds
// of identical prompts is a workable way to obtain one "Open" click. A control
// the user is being drowned in is not a control.
//
// The cap bounds how many times ONE DOCUMENT may ask. Past it the action is
// refused without asking and without consulting the launch handler, so a
// capped call can neither prompt nor execute.
//
// Note what is asserted and why. `launchPromptCountForTesting()` counts "a
// /Launch reached the gate" and deliberately keeps counting past the cap --
// the concurrency stress test above depends on that meaning. So the cap is
// observed through the LAUNCH HANDLER instead: the handler is what stands in
// for asking the user, and past the cap it must stop being called.
// ---------------------------------------------------------------------------

TEST_F(LaunchLinkFollowTest, PerDocumentPromptCapStopsAskingAfterTheLimit)
{
    int handlerCalls = 0;
    pdfe::ui::MainWindow::setLaunchHandlerForTesting(
        [&handlerCalls](const QString&) {
            ++handlerCalls;
            return false;          // always decline; nothing must execute
        });

    // Follow far more times than any cap could plausibly be.
    constexpr int kAttempts = 40;
    for (int i = 0; i < kAttempts; ++i) {
        mw_->followLinkAt(0, QPointF(80.0, 70.0));
    }

    // The user stopped being asked well before the last attempt.
    EXPECT_LT(handlerCalls, kAttempts)
        << "the launch handler was consulted on every one of " << kAttempts
        << " attempts -- there is no per-document cap, so a document with "
           "many /Launch links can prompt without bound";

    // And it is a real bound, not merely "fewer": nothing may execute, since
    // this handler always declines.
    EXPECT_EQ(pdfe::ui::MainWindow::launchExecutedCountForTesting(), 0);

    // The gate counter keeps its existing meaning -- every call reached it.
    EXPECT_EQ(pdfe::ui::MainWindow::launchPromptCountForTesting(), kAttempts)
        << "the cap must suppress the PROMPT, not the accounting: the "
           "concurrency test asserts this counter tracks followLinkAt calls";
}

// The cap must not be a one-way door for the session. A document the user
// actually trusts, opened afterwards, must get its own budget -- otherwise one
// hostile document permanently disarms /Launch for everything opened later.
//
// Asserted through the reset seam rather than by reopening a file, because the
// production reset happens on document open and `resetLaunchCountersForTesting`
// is the seam that stands in for it. Pinning the RESET is the point; pinning
// how the fixture reopens a file is not.
TEST_F(LaunchLinkFollowTest, PromptCapIsPerDocumentNotPerSession)
{
    int handlerCalls = 0;
    pdfe::ui::MainWindow::setLaunchHandlerForTesting(
        [&handlerCalls](const QString&) { ++handlerCalls; return false; });

    for (int i = 0; i < 40; ++i) {
        mw_->followLinkAt(0, QPointF(80.0, 70.0));
    }
    const int callsBeforeReset = handlerCalls;
    ASSERT_GT(callsBeforeReset, 0) << "the gate was never reached at all";

    // A new document arrives.
    pdfe::ui::MainWindow::resetLaunchCountersForTesting();

    mw_->followLinkAt(0, QPointF(80.0, 70.0));

    EXPECT_GT(handlerCalls, callsBeforeReset)
        << "after the per-document state was reset the user was still not "
           "asked -- the cap is behaving as a per-session kill switch, so one "
           "hostile document would disarm /Launch for every later one";
}
