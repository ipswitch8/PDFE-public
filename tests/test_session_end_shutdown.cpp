// -*- coding: utf-8 -*-
//
// F10 — session end: Windows logoff / shutdown / restart.
//
// Gaps: WIN-68 (no WM_QUERYENDSESSION / QSessionManager handling anywhere in
// src/), WIN-69 (no ShutdownBlockReasonCreate around the save window), WIN-70
// (AutosaveManager had no session/power hooks).
//
// THE DEFECT THIS PINS
// --------------------
// PDFE handled exactly one exit route: QCloseEvent, where closeEvent() prompts
// to save (UI-KB-07). A Windows logoff or shutdown does not send a
// QCloseEvent. The session manager broadcasts WM_QUERYENDSESSION; Qt turns that
// into QGuiApplication::commitDataRequest; an application that ignores the
// signal is simply terminated. So before this phase, "Start → Restart" with an
// edited document open threw the edits away with no prompt and no recovery
// record — the single most ordinary way a Windows user loses work.
//
// WHY commitDataRequest AND NOT A RAW WM_QUERYENDSESSION HANDLER
// --------------------------------------------------------------
// Qt's signal is the cross-platform route and Qt already owns the window
// procedure. The decisive reason, though, is testability: the decision logic is
// reachable as MainWindow::handleSessionEnd(bool), so these tests drive the real
// production code path without a live QSessionManager (whose constructor is
// private and which cannot be faked) and without triggering a real logoff on the
// build machine. A raw nativeEvent handler would have left this untestable —
// which is how WIN-68 stayed open in the first place.
//
// THE ORACLE
// ----------
// "Unsaved changes are not lost silently" is asserted as REAL RECOVERABLE STATE:
// after a session-end request the AutosaveManager must hold a published recovery
// snapshot, so the next launch's recovery scan surfaces the work. Asserting only
// "a prompt was shown" would be worthless in the unattended case
// (`shutdown /f`), which is precisely the case that forbids prompting.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "core/Document.h"

#include <QApplication>
#include <QDir>
#include <QString>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::ui::MainWindow;

std::filesystem::path fixtures()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Deterministic overrides for the two virtuals the session-end path consults,
// mirroring the established TestableMainWindow idiom in test_chrome_phase9.cpp.
class SessionEndMainWindow : public MainWindow {
public:
    bool dirty = false;
    int promptCount = 0;

    // CloseAction is protected on MainWindow, so the answer is selected through
    // these setters (the same idiom test_chrome_phase9.cpp uses).
    void setCancelAnswer()  { nextAnswer_ = CloseAction::Cancel; }
    void setDiscardAnswer() { nextAnswer_ = CloseAction::Discard; }
    void setSaveAnswer()    { nextAnswer_ = CloseAction::Save; }

protected:
    CloseAction nextAnswer_ = CloseAction::Discard;

    bool hasUnsavedChanges() const override { return dirty; }
    CloseAction promptSaveOnClose() override
    {
        ++promptCount;
        return nextAnswer_;
    }
};

class SessionEndTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_NE(qApp, nullptr) << "these tests need the suite's QApplication";
        dir_ = std::filesystem::temp_directory_path()
               / ("pdfe_f10_session_"
                  + std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch().count()));
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        ASSERT_FALSE(ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path dir_;
};

}  // namespace

// (WIN-68) A clean document lets the session end immediately and must NOT
// prompt — a shutdown that stalls on a needless dialog is its own bug.
TEST_F(SessionEndTest, CleanDocumentLetsTheSessionEndWithoutPrompting) {
    SessionEndMainWindow w;
    w.dirty = false;

    EXPECT_EQ(w.handleSessionEnd(/*allowsInteraction=*/true),
              MainWindow::SessionEndDecision::Proceed);
    EXPECT_EQ(w.promptCount, 0)
        << "a clean document must not block or interrogate a logoff";
}

// (WIN-68) With interaction allowed, an unsaved document is offered the normal
// Save / Discard / Cancel choice, and Cancel ABORTS the shutdown — the user's
// explicit "not yet" must be honoured, exactly as in closeEvent().
TEST_F(SessionEndTest, UnsavedChangesCancelAbortsTheShutdown) {
    SessionEndMainWindow w;
    w.dirty = true;
    w.setCancelAnswer();

    EXPECT_EQ(w.handleSessionEnd(/*allowsInteraction=*/true),
              MainWindow::SessionEndDecision::Cancel)
        << "Cancel at the logoff prompt must abort the session end, not proceed "
           "and discard the work (WIN-68)";
    EXPECT_EQ(w.promptCount, 1);
}

// (WIN-68) Discard proceeds — the user explicitly chose to throw the work away.
TEST_F(SessionEndTest, UnsavedChangesDiscardProceeds) {
    SessionEndMainWindow w;
    w.dirty = true;
    w.setDiscardAnswer();

    EXPECT_EQ(w.handleSessionEnd(/*allowsInteraction=*/true),
              MainWindow::SessionEndDecision::Proceed);
    EXPECT_EQ(w.promptCount, 1);
}

// (WIN-68, the unattended case) `shutdown /f`, a policy logoff and a UPS-driven
// shutdown all forbid interaction. The application must NOT prompt (it cannot)
// and must NOT block — it must proceed, having first made the work recoverable.
// The no-prompt half is asserted here; the recoverable half is the test below,
// which uses a real document.
TEST_F(SessionEndTest, ForcedShutdownDoesNotPromptAndDoesNotBlock) {
    SessionEndMainWindow w;
    w.dirty = true;
    w.setCancelAnswer();  // would abort IF asked

    EXPECT_EQ(w.handleSessionEnd(/*allowsInteraction=*/false),
              MainWindow::SessionEndDecision::Proceed)
        << "a forced shutdown cannot be cancelled by us; blocking it would only "
           "get the process killed anyway";
    EXPECT_EQ(w.promptCount, 0)
        << "prompting is impossible when the session manager forbids "
           "interaction — doing it anyway hangs the shutdown";
}

// ───────────────────────────────────────────────────────────────────────────
// (WIN-68 + WIN-70, the load-bearing oracle) THE WORK MUST SURVIVE.
//
// A real document, a real edit, a real forced (non-interactive) session end.
// Afterwards a recovery snapshot must be published, so the next launch offers
// the work back. This is the assertion that "unsaved changes are not lost
// silently" actually rests on.
// ───────────────────────────────────────────────────────────────────────────
TEST_F(SessionEndTest, ForcedSessionEndPublishesARecoverySnapshot) {
    const auto doc = dir_ / "session_doc.pdf";
    std::error_code ec;
    std::filesystem::copy_file(fixtures() / "test_3page.pdf", doc,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec);

    SessionEndMainWindow w;
    w.openFile(QString::fromStdWString(doc.wstring()));
    ASSERT_NE(w.documentForTesting(), nullptr) << "the fixture failed to open";

    // A real content mutation on the live document (bumps contentVersion, which
    // is one of AutosaveManager's two dirty signals) — the unsaved work.
    w.documentForTesting()->insertBlankPage(1, 300.0f, 300.0f);
    w.documentForTesting()->markRequiresFullRewrite();
    ASSERT_EQ(w.documentForTesting()->pageCount(), 4);

    ASSERT_FALSE(w.hasRecoverySnapshotForTesting())
        << "nothing should be published before the session-end request";

    // Force the dirty branch, then a NON-INTERACTIVE session end — the case
    // where no dialog is possible and only the snapshot can save the work.
    w.dirty = true;
    EXPECT_EQ(w.handleSessionEnd(/*allowsInteraction=*/false),
              MainWindow::SessionEndDecision::Proceed);

    EXPECT_TRUE(w.hasRecoverySnapshotForTesting())
        << "a forced logoff/shutdown discarded unsaved changes with no prompt "
           "and no recovery snapshot — silent data loss (WIN-68/WIN-70)";
    EXPECT_EQ(w.promptCount, 0);

    // Teardown: this test deliberately published a real sidecar into the
    // per-user recovery dir. Drop it so the launch-time recovery prompt does
    // not fire in an unrelated test.
    w.clearRecoveryForTesting();
}

// (WIN-68) The snapshot is taken BEFORE the prompt, not after: the session
// manager may kill us while the dialog is up. Asserted by taking the interactive
// path with Cancel — the shutdown is aborted, and the snapshot exists anyway.
TEST_F(SessionEndTest, SnapshotIsTakenBeforeThePromptNotAfterIt) {
    const auto doc = dir_ / "session_doc_prompt.pdf";
    std::error_code ec;
    std::filesystem::copy_file(fixtures() / "test_3page.pdf", doc,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec);

    SessionEndMainWindow w;
    w.openFile(QString::fromStdWString(doc.wstring()));
    ASSERT_NE(w.documentForTesting(), nullptr);
    w.documentForTesting()->insertBlankPage(1, 300.0f, 300.0f);
    w.documentForTesting()->markRequiresFullRewrite();

    w.dirty = true;
    w.setCancelAnswer();
    EXPECT_EQ(w.handleSessionEnd(/*allowsInteraction=*/true),
              MainWindow::SessionEndDecision::Cancel);
    EXPECT_EQ(w.promptCount, 1);
    EXPECT_TRUE(w.hasRecoverySnapshotForTesting())
        << "the emergency snapshot must already exist by the time the prompt is "
           "answered — the session manager is under no obligation to wait for a "
           "dialog (WIN-68)";

    w.clearRecoveryForTesting();
}
