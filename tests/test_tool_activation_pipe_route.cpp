// -*- coding: utf-8 -*-
//
// Harness-remediation PHASE-6, register target R-27 (High) —
// UI permaskip wall (gap_a14 UIA-menu-hang).
//
// THE PROBLEM
// -----------
// On the Server-2019 self-hosted runner, PDFE's UIA accessibility provider
// intermittently WEDGES: every IUIAutomation FindAll blocks indefinitely and
// pywinauto's timeout does not bound the underlying COM call. UI tests that
// drove menus/tools through UIA FindAll therefore hang, so they were guarded to
// SKIP on a wedge (gap-A14). That silent-skip wall means the behavioral
// assertions those tests should make never run in CI.
//
// THE ROUTE (already wired on the product side)
// ---------------------------------------------
// PDFE exposes a UIA-FindAll-FREE seam pair used to DRIVE and VERIFY tools:
//   * DRIVE   — MainWindow::activateToolByObjectName() (the textToolActivatePipe
//               dispatch target): find the QAction by objectName and trigger it.
//   * VERIFY  — MainWindow::activeToolDiag() (a result-pipe readback): report
//               the currently-active tool's action objectName.
// The Python helpers (tests/helpers/qinvoke.py: activate_tool_by_name +
// activeToolDiag readback) route through exactly this pair instead of a UIA
// menu FindAll. This test LOCKS that product route so a converted UI test can
// assert real tool state off the pipe readback rather than a hang-prone UIA
// enumeration.
//
// LOCALLY-VERIFIABLE PROOF (headless, ZERO UIA)
// ---------------------------------------------
// Calling activateToolByObjectName() then activeToolDiag() directly on a
// headless MainWindow proves the drive→readback contract end-to-end with no
// accessibility provider involved at all: each activation SWITCHES the active
// tool and the readback REFLECTS it (and CHANGES between tools, so it is a real
// oracle, not a constant). This is the exact behavioral core that the wedged
// UIA path could not reach — the conversion target for the permaskip wall.
//
// HONEST SCOPE: this proves the PRODUCT seam the pipe route depends on is sound
// and UIA-free. The remaining Python-side dependency (locating the pipe
// QLineEdit still uses a bounded UIA probe in _find_pipe) is genuine
// self-hosted-runner territory and is documented in the phase-6 report; it is
// NOT faked here.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"

#include <QApplication>
#include <QLineEdit>
#include <QString>

#include <filesystem>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::ui::MainWindow;

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// activateToolByObjectName queues the trigger via QTimer::singleShot(0, ...),
// so the caller must pump the event loop before reading back the active tool.
void pump() {
    for (int i = 0; i < 4; ++i) QApplication::processEvents();
}

class ToolActivationPipeRouteTest : public ::testing::Test {
protected:
    void SetUp() override {
        mw_ = std::make_unique<MainWindow>();
        pump();
        auto pdf = fixturesDir() / "multipage_smalltext_edit.pdf";
        ASSERT_TRUE(std::filesystem::exists(pdf)) << pdf.string();
        mw_->openFile(QString::fromStdString(pdf.string()));
        pump();
        ASSERT_NE(mw_->documentForTesting(), nullptr)
            << "a document must be open for tool actions to be enabled";
    }
    void TearDown() override { mw_.reset(); pump(); }
    std::unique_ptr<MainWindow> mw_;
};

// The drive→readback route activates a tool and the readback reflects it —
// with no UIA at all. Each tool in the matrix is an annotToolGroup member so
// activeToolDiag() reports its objectName.
TEST_F(ToolActivationPipeRouteTest, InvokeActionRouteActivatesAndReadsBackEachTool) {
    const std::vector<QString> tools = {
        "selectToolAction",
        "stickyNoteToolAction",
        "highlightToolAction",
        "underlineToolAction",
        "redactToolAction",
    };

    QString previous;
    for (const QString& name : tools) {
        // DRIVE via the pipe-dispatch seam (UIA-free).
        const bool dispatched = mw_->activateToolByObjectName(name);
        EXPECT_TRUE(dispatched)
            << "invoke_action route failed to dispatch " << name.toStdString();
        pump();

        // VERIFY via the result-readback seam (UIA-free).
        const QString active = mw_->activeToolDiag();
        EXPECT_EQ(active, name)
            << "activeToolDiag readback did not reflect the activated tool: "
               "expected " << name.toStdString()
            << " got " << active.toStdString();

        // The readback is a REAL oracle: it changed from the previous tool.
        if (!previous.isEmpty()) {
            EXPECT_NE(active, previous)
                << "readback stuck at " << previous.toStdString()
                << " — not tracking activation";
        }
        previous = name;
    }
}

// An unknown action name must be a defined negative (dispatch returns false),
// so a converted test can distinguish "tool activated" from "no such tool"
// without any UIA enumeration.
TEST_F(ToolActivationPipeRouteTest, InvokeActionRouteRejectsUnknownAction) {
    EXPECT_FALSE(mw_->activateToolByObjectName("thereIsNoSuchToolAction"));
    pump();
}

}  // namespace

// ---------------------------------------------------------------------------
// GitHub #271, claim-check F3. WHERE the acknowledgement is published.
//
// activateToolByObjectName defers the trigger via QTimer::singleShot(0, ...)
// and publishes the acknowledgement INSIDE that lambda, deliberately: at the
// function's `return` the tool is queued, not armed. The whole seam rests on
// that choice, and nothing tested it.
//
// It cannot be tested from the Python harness. Measured 2026-10-09: moving the
// publish to the queueing site and rebuilding left
// test_live_rearm_acknowledges_each_arming GREEN, because singleShot(0) runs
// within one event-loop turn while a UIA round trip costs ~50-100 ms -- so the
// harness always observes the post-trigger state no matter where the publish
// happened. An assertion that cannot distinguish the two is not a control over
// the decision, which is exactly what F3 said.
//
// Here it IS observable, because this test deliberately does not pump the event
// loop between queueing and reading. That is the one vantage point from which
// "published before the trigger" and "published after it" differ.
//
// Owns its own MainWindow rather than using the fixture's: the pipes are gated
// on qEnvironmentVariableIsSet("PDFE_TEST_PIPES") and are created in the
// CONSTRUCTOR, so the variable has to be set before construction.
TEST(ToolActivateAckPublishSite, AckIsNotPublishedUntilTheDeferredTriggerRuns) {
    const bool had = qEnvironmentVariableIsSet("PDFE_TEST_PIPES");
    const QByteArray prev = had ? qgetenv("PDFE_TEST_PIPES") : QByteArray();
    qputenv("PDFE_TEST_PIPES", QByteArray("1"));

    {
        auto mw = std::make_unique<MainWindow>();
        pump();

        auto pdf = fixturesDir() / "multipage_smalltext_edit.pdf";
        ASSERT_TRUE(std::filesystem::exists(pdf)) << pdf.string();
        mw->openFile(QString::fromStdString(pdf.string()));
        pump();
        ASSERT_NE(mw->documentForTesting(), nullptr)
            << "a document must be open for tool actions to be enabled";

        auto* resultPipe =
            mw->findChild<QLineEdit*>("testToolActivateResultPipe");
        ASSERT_NE(resultPipe, nullptr)
            << "testToolActivateResultPipe absent even with PDFE_TEST_PIPES "
               "set. Either the pipes are not compiled in "
               "(kTestPipesCompiledIn) or the gate changed; without the pipe "
               "there is nothing to observe and this test must not report a "
               "green it did not establish.";

        // Arm something else first, so the action under test is NOT already
        // checked. Otherwise activateToolByObjectName takes its
        // isCheckable() && isChecked() early return, which publishes
        // SYNCHRONOUSLY and legitimately -- a different path with a different
        // contract, and testing it here would prove nothing about the deferral.
        ASSERT_TRUE(mw->activateToolByObjectName("selectToolAction"));
        pump();

        const QString before = resultPipe->text();

        // Queue the real thing. Deliberately NO pump() before the read.
        ASSERT_TRUE(mw->activateToolByObjectName("stickyNoteToolAction"));

        EXPECT_EQ(resultPipe->text(), before)
            << "the acknowledgement was published before the deferred trigger "
               "ran. A caller waiting for it would proceed while the tool is "
               "still only QUEUED -- the exact false 'queued means armed' "
               "signal GitHub #271 removes, and what cost GitHub #294 999 of "
               "1000 annotations.";

        // Now let the queued lambda run.
        pump();

        const QString after = resultPipe->text();
        EXPECT_NE(after, before)
            << "no acknowledgement arrived even after the event loop ran, so "
               "the deferred publish never happened at all";
        EXPECT_TRUE(after.endsWith(QStringLiteral(":armed")))
            << "acknowledgement was " << after.toStdString()
            << "; after the trigger the canvas should hold a tool object";
        EXPECT_TRUE(after.contains(QStringLiteral("stickyNoteToolAction")))
            << "acknowledgement names the wrong action: "
            << after.toStdString();

        // And the action really is the checked one, which only happens on
        // trigger.
        EXPECT_EQ(mw->activeToolDiag(),
                  QStringLiteral("stickyNoteToolAction"));
    }
    pump();

    if (had) {
        qputenv("PDFE_TEST_PIPES", prev);
    } else {
        qunsetenv("PDFE_TEST_PIPES");
    }
}
