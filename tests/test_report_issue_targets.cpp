// -*- coding: utf-8 -*-
// Pipeline ui-report-netopen phase-2 — the "Report an issue" surface targets.
//
// Two user-reported defects are guarded here:
//   1. The report-issue link used to point at the PRIVATE dev repo
//      (github.com/ipswitch8/PDFE/issues/new), which external users cannot
//      reach. It must point at the PUBLIC mirror
//      (github.com/ipswitch8/PDFE-public/issues/new).
//   2. The surface now also offers an email channel to the public support
//      address pdfe@itwerx.net (an itwerx.net mailto:).
//
// These assert the VALUES the UI uses, read through MainWindow's static
// accessors, so the oracle is headless — no MainWindow instance, no dialog,
// no GUI. The UI (MainWindow::onReportIssue) reads the SAME accessors, so a
// regression that reverted the URL or changed the address would fail here
// AND change the shipped behaviour in lockstep. A string buried only inside
// the dialog lambda would not be reachable this way, which is the point.
//
// Negative controls are explicit: the URL must NOT be the private repo's
// issues/new, because "contains PDFE-public" alone would still pass if the
// private link leaked back in under a different path.
#include <gtest/gtest.h>

#include "ui/MainWindow.h"

#include <QString>
#include <QUrl>

using pdfe::ui::MainWindow;

TEST(ReportIssueTargets, GithubUrlIsThePublicMirrorExactly)
{
    EXPECT_EQ(MainWindow::reportIssueUrl(),
              QStringLiteral("https://github.com/ipswitch8/PDFE-public/issues/new"));
}

TEST(ReportIssueTargets, GithubUrlIsNotThePrivateDevRepo)
{
    // The exact private URL that shipped before this fix.
    EXPECT_NE(MainWindow::reportIssueUrl(),
              QStringLiteral("https://github.com/ipswitch8/PDFE/issues/new"));
    // And no path on the private ipswitch8/PDFE repo (without the -public
    // suffix) may be the target. The public host string contains "PDFE-public",
    // so guard specifically against the private "/PDFE/" path segment.
    EXPECT_FALSE(MainWindow::reportIssueUrl().contains(
        QStringLiteral("github.com/ipswitch8/PDFE/")));
}

TEST(ReportIssueTargets, EmailAddressIsThePublicSupportAddress)
{
    EXPECT_EQ(MainWindow::reportIssueEmail(),
              QStringLiteral("pdfe@itwerx.net"));
}

TEST(ReportIssueTargets, MailtoTargetsThePublicSupportAddress)
{
    const QString mailto = MainWindow::reportIssueMailto();
    // It is a mailto: URL...
    const QUrl url(mailto);
    EXPECT_EQ(url.scheme(), QStringLiteral("mailto"));
    // ...addressed to pdfe@itwerx.net (QUrl::path holds the recipient for a
    // mailto URL, before the query string).
    EXPECT_EQ(url.path(), QStringLiteral("pdfe@itwerx.net"));
    EXPECT_TRUE(mailto.startsWith(QStringLiteral("mailto:pdfe@itwerx.net")))
        << "mailto was: " << mailto.toStdString();
    // And it carries a prefilled subject so the user's mail client opens ready.
    EXPECT_TRUE(mailto.contains(QStringLiteral("subject=")))
        << "mailto was: " << mailto.toStdString();
}
