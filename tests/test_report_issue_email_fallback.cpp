// -*- coding: utf-8 -*-
//
// GitHub #337 — "Report by Email" dead-ended with "You'll need a new app to
// open this mailto".
//
// THE DEFECT. MainWindow::onReportIssue()'s Email button called
// QDesktopServices::openUrl(QUrl(reportIssueMailto())) and DISCARDED the bool it
// returns, then unconditionally accepted the chooser. On a machine with no
// registered mailto handler openUrl() returns false, Windows shows its own
// "You'll need a new app to open this mailto" error, and the user has no way to
// reach the support address — the report channel dead-ends.
//
// THE FIX. reportIssueByEmail() now checks that bool. On failure it shows a
// fallback QDialog exposing pdfe@itwerx.net with a "Copy email address" button.
//
// WHY THESE TESTS FAIL AGAINST THE PRE-FIX CODE. Pre-fix there was no
// reportIssueByEmail(), no report-opener seam, and no fallback: the behaviour
// these assert (failure detected -> fallback shown -> address on the clipboard)
// did not exist. The success-path control asserts the fallback is NOT shown when
// the open succeeds, so the fix cannot satisfy the test by always showing the
// fallback.
//
// These drive the REAL fallback dialog — the test hook hands the constructed,
// fully-wired QDialog to the test, which finds the real Copy button by
// objectName and clicks it, so the clipboard assertion exercises the exact slot
// the shipped button runs (not a parallel copy). UIA cannot see a Qt modal, and
// exec() would block a headless run, which is why the hook exists.
#include <gtest/gtest.h>

#include "ui/MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QString>

using pdfe::ui::MainWindow;

namespace {

class ReportIssueEmailFallbackTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        mw_ = std::make_unique<MainWindow>();
        if (QClipboard* clip = QApplication::clipboard()) {
            clip->clear();
        }
    }

    void TearDown() override
    {
        MainWindow::setReportOpenerForTesting({});
        MainWindow::setReportEmailFallbackHookForTesting({});
        mw_.reset();
        QApplication::processEvents();
    }

    std::unique_ptr<MainWindow> mw_;
};

// The failure path: the mailto open fails (no handler), the fallback dialog is
// shown, it carries the literal address, and clicking its real Copy button
// places pdfe@itwerx.net on the clipboard.
TEST_F(ReportIssueEmailFallbackTest, FailedMailtoOpenShowsFallbackAndCopyPutsAddressOnClipboard)
{
    QString openedWith;
    MainWindow::setReportOpenerForTesting([&openedWith](const QString& url) {
        openedWith = url;
        return false;   // simulate "no registered mailto handler"
    });

    bool fallbackShown = false;
    bool addressLabelPresent = false;
    QString clipboardAfterClick;
    MainWindow::setReportEmailFallbackHookForTesting(
        [&](QDialog* fallback) {
            fallbackShown = true;
            ASSERT_NE(fallback, nullptr);

            // The dialog must actually expose the address somewhere a user can
            // read it, not just carry a Copy button.
            const auto labels = fallback->findChildren<QLabel*>();
            for (const QLabel* l : labels) {
                if (l->text().contains(MainWindow::reportIssueEmail())) {
                    addressLabelPresent = true;
                    break;
                }
            }

            auto* copyButton = fallback->findChild<QPushButton*>(
                QStringLiteral("reportIssueEmailCopyButton"));
            ASSERT_NE(copyButton, nullptr)
                << "the fallback must offer a Copy-email-address control";
            copyButton->click();   // runs the real connected slot
            if (QClipboard* clip = QApplication::clipboard()) {
                clipboardAfterClick = clip->text();
            }
        });

    const bool opened = mw_->reportIssueByEmail();

    EXPECT_FALSE(opened)
        << "a failed mailto open must report failure, not success";
    EXPECT_EQ(openedWith, MainWindow::reportIssueMailto())
        << "the email path must attempt the prefilled mailto URL";
    EXPECT_TRUE(fallbackShown)
        << "the fallback dialog must be shown when the mailto open fails";
    EXPECT_TRUE(addressLabelPresent)
        << "the fallback must display the literal support address";
    EXPECT_EQ(clipboardAfterClick, MainWindow::reportIssueEmail())
        << "clicking Copy must place pdfe@itwerx.net on the clipboard";
    EXPECT_EQ(clipboardAfterClick, QStringLiteral("pdfe@itwerx.net"));
}

// The success-path control: when the mailto open succeeds, the fallback is NOT
// shown. Without this, a fix that always showed the fallback would pass the
// failure test above — this nails that the fallback is conditional on failure.
TEST_F(ReportIssueEmailFallbackTest, SuccessfulMailtoOpenDoesNotShowFallback)
{
    QString openedWith;
    MainWindow::setReportOpenerForTesting([&openedWith](const QString& url) {
        openedWith = url;
        return true;    // a registered handler accepted it
    });

    bool fallbackShown = false;
    MainWindow::setReportEmailFallbackHookForTesting(
        [&](QDialog*) { fallbackShown = true; });

    const bool opened = mw_->reportIssueByEmail();

    EXPECT_TRUE(opened) << "a successful mailto open must report success";
    EXPECT_EQ(openedWith, MainWindow::reportIssueMailto());
    EXPECT_FALSE(fallbackShown)
        << "the fallback must NOT appear when the mailto open succeeded";
}

// The copy action in isolation — the exact method the fallback button connects
// to. Guards against the button being wired to a parallel copy that drifts from
// the shipped address.
TEST_F(ReportIssueEmailFallbackTest, CopyActionPlacesSupportAddressOnClipboard)
{
    if (QClipboard* clip = QApplication::clipboard()) {
        clip->setText(QStringLiteral("something-else"));
    }

    MainWindow::copyReportIssueEmailToClipboard();

    ASSERT_NE(QApplication::clipboard(), nullptr);
    EXPECT_EQ(QApplication::clipboard()->text(),
              QStringLiteral("pdfe@itwerx.net"));
    EXPECT_EQ(QApplication::clipboard()->text(), MainWindow::reportIssueEmail());
}

}  // namespace
