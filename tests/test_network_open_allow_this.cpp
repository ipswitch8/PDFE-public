// -*- coding: utf-8 -*-
// GitHub #240 (pipeline ui-report-netopen, fix #3) — the network-open REFUSAL
// must explain itself AND offer a one-click "Allow this" that flips the policy
// and proceeds.
//
// WHAT WAS WRONG
//
// MainWindow::openFile()'s NetworkOpenDecision::Refused branch showed the shared
// `reportOpenFailure` box, which is QMessageBox::Ok-only — a dead end. The
// refusal happens because the Preferences setting
// AppSettings::allowUserNetworkPaths (default TRUE) is OFF, and the only way out
// was: cancel, open Preferences, toggle a checkbox, come back, retry. The fix
// replaces that box on the Refused leg with a dedicated dialog carrying an
// "Allow this" button that sets allowUserNetworkPaths(true) and re-attempts the
// open.
//
// WHAT THESE TESTS PIN, AND WHY EACH IS SEPARATE
//
//   1. THE MECHANISM. With the setting OFF a network path is Refused; after
//      setAllowUserNetworkPaths(true) the SAME path is no longer Refused. This
//      drives the production decision function decideNetworkOpen() directly, so
//      it asserts the flip changes the DECISION — not merely that a setter
//      stored a bool. Precedent: test_network_settings_wiring.cpp (the setting)
//      and test_save_as_network_policy.cpp (the decision, driven through the UI).
//
//   2. THE DIALOG. Driving openFile() with the setting OFF must show the
//      dedicated `networkOpenBlockedDialog` carrying `networkOpenAllowButton` —
//      NOT the Ok-only reportOpenFailure box. Read through the dialog
//      introspection seam, because UIA cannot enumerate a Qt app-modal exec()
//      on this platform (see tests/helpers/modal_dialog.py). Cancel (the
//      auto-dismiss path) must leave the setting untouched and open nothing.
//
//   3. THE "ALLOW THIS" BEHAVIOUR. Clicking the real Allow button (replayed
//      through the dialog-action seam against the real widget) must flip
//      allowUserNetworkPaths to true and re-run the open. Asserted via a fresh
//      AppSettings reader, which is the honest check: a value that is live for
//      the session but never persisted would pass a same-instance read and fail
//      here, exactly as test_network_settings_wiring.cpp pins for the checkbox.
//
// The paths are deliberately unreachable UNCs: every assertion is about the
// DECISION and the DIALOG, not about a successful open, so no file server is
// needed. The re-attempt after the flip therefore ends in the Unreachable
// branch (unchanged by this fix), whose modal is closed by the seam's
// auto-dismisser so the test does not hang.
#include <gtest/gtest.h>

#include "app/AppSettings.h"
#include "ui/MainWindow.h"
#include "ui/TestDialogSeam.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QString>
#include <QStringList>

#include <memory>

using pdfe::app::AppSettings;
using pdfe::ui::MainWindow;

namespace {

/// A well-formed UNC that certainly does not resolve. Reachability is
/// irrelevant to every assertion here: the setting-OFF cases are refused before
/// any probe, and the setting-ON cases only need the decision to STOP being
/// Refused.
constexpr const char* kUnc = "\\\\pdfe-test-host-netopen\\shareone\\doc.pdf";

void settle()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

/// Exposes the protected production decision function so a test can assert the
/// policy flip changes it. Nothing is reimplemented — this calls the exact code
/// openFile() calls.
class ProbeWindow : public MainWindow {
public:
    using MainWindow::setNetworkProbeTimeoutMsForTesting;
    bool refuses(const QString& path)
    {
        return decideNetworkOpen(path) == NetworkOpenDecision::Refused;
    }
};

class NetworkOpenAllowThisTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        settings_ = std::make_unique<AppSettings>();
        savedUserPaths_ = settings_->allowUserNetworkPaths();
    }

    void TearDown() override
    {
        settings_->setAllowUserNetworkPaths(savedUserPaths_);
        settings_->sync();
        qunsetenv("PDFE_TEST_DIALOG_AUTODISMISS");
        qunsetenv("PDFE_TEST_PIPES");
    }

    std::unique_ptr<AppSettings> settings_;
    bool savedUserPaths_ = true;
};

}  // namespace

// ─── 1. The mechanism: the flip changes the decision ────────────────────────

TEST_F(NetworkOpenAllowThisTest, TurningTheSettingOnStopsThePathBeingRefused)
{
    ProbeWindow win;
    // A short budget so the setting-ON probe against an unreachable host returns
    // promptly rather than waiting the full production budget.
    win.setNetworkProbeTimeoutMsForTesting(300);

    settings_->setAllowUserNetworkPaths(false);
    settings_->sync();
    const QString unc = QString::fromLatin1(kUnc);
    EXPECT_TRUE(win.refuses(unc))
        << "with allowUserNetworkPaths OFF a network path must be Refused — the "
           "policy that the Refused branch exists to report";

    settings_->setAllowUserNetworkPaths(true);
    settings_->sync();
    EXPECT_FALSE(win.refuses(unc))
        << "after the \"Allow this\" flip the SAME path is still Refused — the "
           "decision did not follow the setting, so clicking Allow could never "
           "let the open proceed";
}

TEST_F(NetworkOpenAllowThisTest, TheDecisionTracksTheSettingBothWays)
{
    ProbeWindow win;
    win.setNetworkProbeTimeoutMsForTesting(300);
    const QString unc = QString::fromLatin1(kUnc);

    for (const bool allow : {true, false, true}) {
        settings_->setAllowUserNetworkPaths(allow);
        settings_->sync();
        EXPECT_EQ(win.refuses(unc), !allow)
            << "allowUserNetworkPaths=" << allow
            << " but refuses()=" << win.refuses(unc)
            << " — the Refused decision is not a pure function of the setting";
    }
}

// ─── 2. The dialog: dedicated box with an Allow button, not the Ok-only box ──

TEST_F(NetworkOpenAllowThisTest, TheRefusedBranchShowsTheAllowThisDialog)
{
    // The seam is what lets an unattended test see an app-modal exec(); the
    // auto-dismisser takes the Cancel path so openFile() returns without a
    // real click.
    qputenv("PDFE_TEST_PIPES", "1");
    qputenv("PDFE_TEST_DIALOG_AUTODISMISS", "1");

    settings_->setAllowUserNetworkPaths(false);
    settings_->sync();

    MainWindow mw;
    mw.openFile(QString::fromLatin1(kUnc));
    settle();

    const QString record = pdfe::ui::testseam::dialogIntrospection(
        QStringLiteral("networkOpenBlockedDialog"));
    ASSERT_FALSE(record.isEmpty())
        << "no networkOpenBlockedDialog was recorded — the Refused branch still "
           "shows the shared Ok-only reportOpenFailure box (objectName "
           "openFileErrorDialog), which is the dead end this fix replaces";
    EXPECT_TRUE(record.contains(QStringLiteral("networkOpenAllowButton")))
        << "the dialog carries no \"Allow this\" button; record was: "
        << record.toStdString();

    // Cancel (auto-dismiss) must change nothing and open nothing.
    AppSettings reread;
    EXPECT_FALSE(reread.allowUserNetworkPaths())
        << "cancelling the dialog flipped the setting anyway";
    EXPECT_EQ(mw.documentForTesting(), nullptr)
        << "a refused + cancelled network open loaded a document";
}

// ─── 3. "Allow this": flips the setting and proceeds ─────────────────────────

TEST_F(NetworkOpenAllowThisTest, ClickingAllowThisFlipsTheSettingAndReopens)
{
    qputenv("PDFE_TEST_PIPES", "1");
    // NO auto-dismiss: the dialog-action script below drives the REAL Allow
    // button instead, and takes priority over auto-dismissal. Its own
    // auto-dismisser then closes the Unreachable box the re-attempt raises.

    settings_->setAllowUserNetworkPaths(false);
    settings_->sync();

    // Queue a click of the real Allow button, keyed by the dialog's objectName.
    // registerDialogIntrospection() replays it on the dialog's own event loop
    // when the box opens — exercising the genuine button slot, not a shortcut.
    pdfe::ui::testseam::enqueueDialogActions(
        QStringLiteral("networkOpenBlockedDialog"),
        QStringList{QStringLiteral("click:networkOpenAllowButton")});

    MainWindow mw;
    // The re-attempt after the flip probes this unreachable host; keep it short.
    // setNetworkProbeTimeoutMsForTesting is on MainWindow's public test-seam
    // surface.
    mw.setNetworkProbeTimeoutMsForTesting(300);
    mw.openFile(QString::fromLatin1(kUnc));
    settle();
    settle();

    AppSettings reread;
    EXPECT_TRUE(reread.allowUserNetworkPaths())
        << "clicking \"Allow this\" did not persist allowUserNetworkPaths=true; "
           "a formerly-refused open could not have proceeded, and the setting "
           "the Preferences checkbox writes was not written";
}
