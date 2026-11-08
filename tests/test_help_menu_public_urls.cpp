// -*- coding: utf-8 -*-
// GitHub #364 / #365 — Help > Documentation (F1) and Help > Check for Updates
// opened the PRIVATE dev repo.
//
// THE DEFECTS.
//   #364: onDocumentation() opened https://github.com/ipswitch8/PDFE#documentation
//         — the PRIVATE repo plus a nonexistent anchor. External users cannot
//         reach it.
//   #365: onCheckUpdates() opened https://github.com/ipswitch8/PDFE/releases
//         — the PRIVATE repo's releases page.
//
// THE FIX. Both URLs now live in static accessors (documentationUrl() /
// checkUpdatesUrl()) returning ipswitch8/PDFE-public targets, and both Help
// actions route the open through the SAME opener seam the report-issue surface
// uses (openReportUrl -> sReportOpener_). So a headless test asserts the opener
// fired with the public URL without launching a browser.
//
// WHY THESE FAIL PRE-FIX. Pre-fix the accessors did not exist, and the actions
// called QDesktopServices::openUrl() directly on the PRIVATE repo URL — neither
// observable through the opener seam, and the private-repo negative controls
// below would fail on the shipped strings.
//
// SECURITY NOTE. `github.com/ipswitch8/PDFE` (no -public) is the DEFECT being
// removed, not a secret to preserve. The guard distinguishes the private repo
// (`github.com/ipswitch8/PDFE/` and `github.com/ipswitch8/PDFE#`) from the
// public mirror (`ipswitch8/PDFE-public`).
#include <gtest/gtest.h>

#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QString>

using pdfe::ui::MainWindow;

namespace {

// Assert a URL targets the PUBLIC mirror and not the bare private repo.
void expectPublicMirror(const QString& url, const char* which) {
    EXPECT_TRUE(url.contains(QStringLiteral("PDFE-public")))
        << which << " must target the public mirror (ipswitch8/PDFE-public); "
                    "was: " << url.toStdString();
    // The private repo's bare paths: "/PDFE/" (a path segment) and "/PDFE#"
    // (an anchor). The public host contains "PDFE-public", so neither of these
    // can match it — they match ONLY the private repo.
    EXPECT_FALSE(url.contains(QStringLiteral("github.com/ipswitch8/PDFE/")))
        << which << " references the PRIVATE dev repo path; was: "
        << url.toStdString();
    EXPECT_FALSE(url.contains(QStringLiteral("github.com/ipswitch8/PDFE#")))
        << which << " references the PRIVATE dev repo anchor; was: "
        << url.toStdString();
}

} // namespace

class HelpMenuPublicUrlsTest : public ::testing::Test {
protected:
    void TearDown() override {
        MainWindow::setReportOpenerForTesting({});
        QApplication::processEvents();
    }
};

// #364 — the documentation URL value.
TEST_F(HelpMenuPublicUrlsTest, DocumentationUrlIsThePublicMirror) {
    expectPublicMirror(MainWindow::documentationUrl(), "documentationUrl()");
}

// #365 — the check-for-updates URL value.
TEST_F(HelpMenuPublicUrlsTest, CheckUpdatesUrlIsThePublicMirror) {
    expectPublicMirror(MainWindow::checkUpdatesUrl(), "checkUpdatesUrl()");
    // The releases path specifically, on the public mirror.
    EXPECT_EQ(MainWindow::checkUpdatesUrl(),
              QStringLiteral("https://github.com/ipswitch8/PDFE-public/releases"));
}

// #364 — triggering the Documentation action fires the opener seam exactly once
// with the documentation URL.
TEST_F(HelpMenuPublicUrlsTest, DocumentationActionInvokesOpenerOnce) {
    MainWindow mw;
    int opens = 0;
    QString openedWith;
    MainWindow::setReportOpenerForTesting([&](const QString& url) {
        ++opens;
        openedWith = url;
        return true;
    });

    auto* action = mw.findChild<QAction*>(QStringLiteral("documentationAction"));
    ASSERT_NE(action, nullptr) << "documentationAction not found on MainWindow";
    action->trigger();
    QApplication::processEvents();

    EXPECT_EQ(opens, 1) << "the Documentation action must open exactly one URL";
    EXPECT_EQ(openedWith, MainWindow::documentationUrl());
    expectPublicMirror(openedWith, "the opened documentation URL");
}

// #365 — triggering the Check for Updates action fires the opener seam exactly
// once with the releases URL.
TEST_F(HelpMenuPublicUrlsTest, CheckUpdatesActionInvokesOpenerOnce) {
    MainWindow mw;
    int opens = 0;
    QString openedWith;
    MainWindow::setReportOpenerForTesting([&](const QString& url) {
        ++opens;
        openedWith = url;
        return true;
    });

    auto* action = mw.findChild<QAction*>(QStringLiteral("checkUpdatesAction"));
    ASSERT_NE(action, nullptr) << "checkUpdatesAction not found on MainWindow";
    action->trigger();
    QApplication::processEvents();

    EXPECT_EQ(opens, 1) << "the Check for Updates action must open exactly one URL";
    EXPECT_EQ(openedWith, MainWindow::checkUpdatesUrl());
    expectPublicMirror(openedWith, "the opened releases URL");
}
