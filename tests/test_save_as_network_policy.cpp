// -*- coding: utf-8 -*-
// GitHub #240 — Save As applies the network policy, identically on BOTH branches.
//
// WHY THIS FILE EXISTS
//
// A gate review failed phase-2 for exactly the gap it fills. The policy check was
// added to `MainWindow::onSaveAsFile`, and the network *open* policy was covered
// by tests — but nothing drove the SAVE path at the UI level against a network
// target, so the claim "both Save As branches apply the same policy" rested on
// reading the code rather than on a measurement. That is the distinction the
// acceptance criterion draws, and the reviewer was right to hold it.
//
// THE TWO BRANCHES
//
// `onSaveAsFile` forks on `alignmentPipeline_->pendingPageCount() > 0`:
//
//   * pending == 0 → the undo-preserving path (`saveAsWithCommandPageRebind`);
//   * pending  > 0 → the flush-and-reopen path (`document_->saveAs`).
//
// The policy is checked ONCE, before that fork, which is the design point: a
// check duplicated inside each branch is a check that can disagree with itself
// when a third branch appears. These tests assert the OBSERVABLE consequence of
// that placement — the same verdict on both sides of the fork — rather than
// asserting the source layout, because a source-shape assertion would pass a
// refactor that moved the check back inside one branch.
//
// WHAT THE ORACLE IS, AND WHY IT IS NOT "THE SAVE SUCCEEDED"
//
// Writing to a real share needs a writable share, which this host does not
// reliably have (its only share is an administrative one, and the test account is
// not elevated). So these tests assert the POLICY DECISION, which is observable
// without a successful write:
//
//   * refused  → the status bar carries the policy refusal and no file appears;
//   * permitted → the policy refusal is ABSENT. The save may then fail for an
//     ordinary reason (unreachable host, no permission) and that is fine — the
//     assertion is that it was not turned away by the policy.
//
// Asserting "the save succeeded" would make the test depend on a file server and
// would pass for the wrong reason on a machine that had one.
#include <gtest/gtest.h>

#include "app/AppSettings.h"
#include "core/Document.h"
#include "reflow/AlignmentPipeline.h"
#include "reflow/AlignmentWriter.h"
#include "ui/MainWindow.h"
#include "ui/TestDialogSeam.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QFileInfo>
#include <QAction>
#include <QStatusBar>
#include <QString>

#include <filesystem>
#include <memory>

using pdfe::app::AppSettings;
using pdfe::ui::MainWindow;

namespace {

/// A UNC target on this host's administrative share. Well-formed and genuinely
/// classified as remote, which is all the policy decision depends on.
QString uncTarget(const char* leaf)
{
    return QStringLiteral("\\\\localhost\\C$\\Temp\\") + QString::fromLatin1(leaf);
}

std::string fixturePath()
{
    for (const char* rel : {"tests/fixtures/test_single.pdf",
                            "../tests/fixtures/test_single.pdf",
                            "../../tests/fixtures/test_single.pdf"}) {
        if (std::filesystem::exists(rel)) return rel;
    }
    return {};
}

void settle()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

/// The phrase `onSaveAsFile` puts on the status bar when the policy refuses.
constexpr const char* kRefusalPhrase = "network locations are turned off";

class SaveAsNetworkPolicyTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fixture_ = fixturePath();
        if (fixture_.empty()) GTEST_SKIP() << "test_single.pdf not available";

        // The refusal is a modal QMessageBox; without the auto-dismisser this
        // test would HANG rather than fail.
        qputenv("PDFE_TEST_PIPES", "1");
        qputenv("PDFE_TEST_DIALOG_AUTODISMISS", "1");

        settings_ = std::make_unique<AppSettings>();
        savedUserPaths_ = settings_->allowUserNetworkPaths();
    }

    void TearDown() override
    {
        // Leave nothing behind for the next case — or the next RUN — to trip on.
        for (const QString& t : written_) QFile::remove(t);
        written_.clear();
        settings_->setAllowUserNetworkPaths(savedUserPaths_);
        qunsetenv("PDFE_TEST_DIALOG_AUTODISMISS");
        qunsetenv("PDFE_TEST_PIPES");
    }

    /// Drive Save As to `target` with the setting at `allowNetwork`, having made
    /// `pendingAlignment` decide which branch would run. Returns the status-bar
    /// text observed afterwards.
    QString driveSaveAs(const QString& target, bool allowNetwork,
                        bool pendingAlignment)
    {
        // Start from a known state. The "no file appeared" assertion is only
        // meaningful if there was no file to begin with, and these targets DO
        // get written when a run is permitted to save — including by a mutation
        // run that removes the guard. Measured: the mutation harness reported an
        // extra kill for P2-M10 purely because P2-M9 (guard removed) had left a
        // file behind, which is a defect in this test, not in the product.
        QFile::remove(target);
        written_.insert(target);

        settings_->setAllowUserNetworkPaths(allowNetwork);

        MainWindow mw;
        mw.openFile(QString::fromStdString(fixture_));
        settle();
        if (!mw.documentForTesting()) return QStringLiteral("__no-document__");

        if (pendingAlignment) {
            auto* pipeline = mw.alignmentPipelineForTesting();
            if (!pipeline) return QStringLiteral("__no-pipeline__");
            // Any queued page makes pendingPageCount() > 0 and so selects the
            // flush-and-reopen branch. The block's content is irrelevant here —
            // only that the queue is non-empty when the fork is evaluated.
            pdfe::reflow::AlignmentWriter::BlockParams params;
            params.blockLeftPt = 72.0f;
            params.blockTopPt = 72.0f;
            params.blockWidthPt = 200.0f;
            pipeline->queueAlignment(0, params);
            if (pipeline->pendingPageCount() == 0) {
                return QStringLiteral("__queue-did-not-take__");
            }
        }

        // Drive the ACTION, not the private slot — it is what a user triggers,
        // and it exercises the action's own wiring too.
        pdfe::ui::testseam::enqueueFileDialogPath(target);
        auto* saveAs = mw.findChild<QAction*>(QStringLiteral("saveAsAction"));
        if (!saveAs) return QStringLiteral("__no-save-as-action__");
        saveAs->trigger();
        settle();

        return mw.statusBar() ? mw.statusBar()->currentMessage() : QString();
    }

    std::string fixture_;
    std::unique_ptr<AppSettings> settings_;
    bool savedUserPaths_ = true;
    /// Every target this case touched, removed on teardown.
    QSet<QString> written_;
};

}  // namespace

// ─── The setting OFF: refused, the same way on both branches ────────────────

TEST_F(SaveAsNetworkPolicyTest, RefusedOnTheUndoPreservingBranchWhenTurnedOff)
{
    const QString target = uncTarget("pdfe_p94_saveas_a.pdf");
    const QString status = driveSaveAs(target, /*allowNetwork=*/false,
                                       /*pendingAlignment=*/false);

    EXPECT_TRUE(status.contains(QString::fromLatin1(kRefusalPhrase)))
        << "no policy refusal on the undo-preserving branch; status was \""
        << status.toStdString() << "\"";
    EXPECT_FALSE(QFileInfo(target).exists())
        << "the save was refused but a file appeared at the target anyway";
}

TEST_F(SaveAsNetworkPolicyTest, RefusedOnTheFlushBranchWhenTurnedOff)
{
    const QString target = uncTarget("pdfe_p94_saveas_b.pdf");
    const QString status = driveSaveAs(target, /*allowNetwork=*/false,
                                       /*pendingAlignment=*/true);

    ASSERT_FALSE(status.startsWith(QStringLiteral("__")))
        << "could not select the flush branch: " << status.toStdString();
    EXPECT_TRUE(status.contains(QString::fromLatin1(kRefusalPhrase)))
        << "no policy refusal on the flush-and-reopen branch; status was \""
        << status.toStdString()
        << "\". The two Save As branches disagree about the network policy, "
           "which is exactly what GitHub #240 reported.";
    EXPECT_FALSE(QFileInfo(target).exists());
}

// ─── The setting ON: not refused, the same way on both branches ─────────────

TEST_F(SaveAsNetworkPolicyTest, NotRefusedOnTheUndoPreservingBranchWhenAllowed)
{
    const QString status = driveSaveAs(uncTarget("pdfe_p94_saveas_c.pdf"),
                                       /*allowNetwork=*/true,
                                       /*pendingAlignment=*/false);

    EXPECT_FALSE(status.contains(QString::fromLatin1(kRefusalPhrase)))
        << "a user-initiated save was refused by policy while network locations "
           "are ALLOWED; status was \"" << status.toStdString() << "\"";
}

TEST_F(SaveAsNetworkPolicyTest, NotRefusedOnTheFlushBranchWhenAllowed)
{
    const QString status = driveSaveAs(uncTarget("pdfe_p94_saveas_d.pdf"),
                                       /*allowNetwork=*/true,
                                       /*pendingAlignment=*/true);

    ASSERT_FALSE(status.startsWith(QStringLiteral("__")))
        << "could not select the flush branch: " << status.toStdString();
    EXPECT_FALSE(status.contains(QString::fromLatin1(kRefusalPhrase)))
        << "the flush-and-reopen branch refused an ALLOWED network save; the two "
           "branches disagree: " << status.toStdString();
}

// ─── The branches must agree, stated as its own assertion ──────────────────
//
// The three tests above could all pass while the two branches happened to agree
// for unrelated reasons. This one compares them directly, which is the property
// the single pre-fork check exists to guarantee.
TEST_F(SaveAsNetworkPolicyTest, TheTwoBranchesReachTheSameVerdict)
{
    for (const bool allow : {false, true}) {
        const QString undoBranch =
            driveSaveAs(uncTarget("pdfe_p94_saveas_e.pdf"), allow, false);
        const QString flushBranch =
            driveSaveAs(uncTarget("pdfe_p94_saveas_f.pdf"), allow, true);

        ASSERT_FALSE(flushBranch.startsWith(QStringLiteral("__")))
            << "could not select the flush branch: " << flushBranch.toStdString();

        const bool undoRefused =
            undoBranch.contains(QString::fromLatin1(kRefusalPhrase));
        const bool flushRefused =
            flushBranch.contains(QString::fromLatin1(kRefusalPhrase));

        EXPECT_EQ(undoRefused, flushRefused)
            << "with allowUserNetworkPaths=" << allow
            << " the undo-preserving branch " << (undoRefused ? "refused" : "allowed")
            << " while the flush branch " << (flushRefused ? "refused" : "allowed")
            << ". The policy must be decided once, before the fork.";
        EXPECT_EQ(undoRefused, !allow)
            << "the verdict does not follow the setting";
    }
}

// A LOCAL target must be unaffected by the policy under either setting —
// otherwise the guard would have made ordinary saving conditional on a network
// preference, and every test above would still pass.
TEST_F(SaveAsNetworkPolicyTest, ALocalTargetIsNeverRefusedByTheNetworkPolicy)
{
    const QString local =
        QDir::temp().filePath(QStringLiteral("pdfe_p94_saveas_local.pdf"));
    QFile::remove(local);

    for (const bool allow : {false, true}) {
        const QString status = driveSaveAs(local, allow, false);
        EXPECT_FALSE(status.contains(QString::fromLatin1(kRefusalPhrase)))
            << "a LOCAL save was refused by the network policy with "
               "allowUserNetworkPaths=" << allow << ": " << status.toStdString();
    }
    QFile::remove(local);
}
