// -*- coding: utf-8 -*-
// GitHub #127 -- the opt-in, notify-only version check.
//
// The parsing half is a PURE function over the response bytes, exactly so it
// can be tested with no network (UC-8's rule, applied to UC-9's notifier). No
// test here touches the network; if one ever does, it has stopped testing the
// thing this file exists to pin.
#include <gtest/gtest.h>

#include "app/UpdateChecker.h"
#include "app/VersionInfo.h"
#include "app/AppSettings.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

using namespace pdfe::app;

namespace {

/// Build a releases-feed payload the way GitHub actually shapes it.
QByteArray feed(const QString& body)
{
    return body.toUtf8();
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing -- the pure half
// ---------------------------------------------------------------------------

TEST(UpdateCheckerParse, ReadsNewestStableTag)
{
    const QByteArray j = feed(R"([
        {"tag_name": "v0.1.0", "draft": false, "prerelease": false},
        {"tag_name": "v0.3.0", "draft": false, "prerelease": false},
        {"tag_name": "v0.2.0", "draft": false, "prerelease": false}
    ])");
    EXPECT_EQ(latestVersionFromReleasesJson(j), QStringLiteral("0.3.0"));
}

TEST(UpdateCheckerParse, StripsLeadingV)
{
    const QByteArray j = feed(R"([{"tag_name": "v1.2.3", "draft": false, "prerelease": false}])");
    EXPECT_EQ(latestVersionFromReleasesJson(j), QStringLiteral("1.2.3"));
}

TEST(UpdateCheckerParse, DoesNotRelyOnFeedOrdering)
{
    // Oldest last would be the natural order; assert we compare rather than
    // trusting position, by putting the newest FIRST and then LAST.
    const QByteArray newestFirst = feed(R"([
        {"tag_name": "9.9.9", "draft": false, "prerelease": false},
        {"tag_name": "0.1.0", "draft": false, "prerelease": false}
    ])");
    const QByteArray newestLast = feed(R"([
        {"tag_name": "0.1.0", "draft": false, "prerelease": false},
        {"tag_name": "9.9.9", "draft": false, "prerelease": false}
    ])");
    EXPECT_EQ(latestVersionFromReleasesJson(newestFirst), QStringLiteral("9.9.9"));
    EXPECT_EQ(latestVersionFromReleasesJson(newestLast),  QStringLiteral("9.9.9"));
}

TEST(UpdateCheckerParse, SkipsDraftsAndPreReleases)
{
    const QByteArray j = feed(R"([
        {"tag_name": "9.9.9", "draft": true,  "prerelease": false},
        {"tag_name": "8.8.8", "draft": false, "prerelease": true},
        {"tag_name": "0.2.0", "draft": false, "prerelease": false}
    ])");
    EXPECT_EQ(latestVersionFromReleasesJson(j), QStringLiteral("0.2.0"))
        << "a draft or pre-release must not be offered to a stable user";
}

// Fail-closed cases. Each must yield an EMPTY string, which callers treat as
// "no answer" -- never as "no update", and never as a notification.
TEST(UpdateCheckerParse, FailsClosedOnUnusablePayloads)
{
    EXPECT_TRUE(latestVersionFromReleasesJson(feed("")).isEmpty())
        << "empty body";
    EXPECT_TRUE(latestVersionFromReleasesJson(feed("not json at all")).isEmpty())
        << "malformed JSON";
    EXPECT_TRUE(latestVersionFromReleasesJson(feed("[]")).isEmpty())
        << "no releases yet";
    EXPECT_TRUE(latestVersionFromReleasesJson(
                    feed(R"({"message": "API rate limit exceeded"})")).isEmpty())
        << "GitHub returns an OBJECT for errors; it must not be read as a release";
    EXPECT_TRUE(latestVersionFromReleasesJson(
                    feed(R"([{"draft": false, "prerelease": false}])")).isEmpty())
        << "release with no tag_name";
    EXPECT_TRUE(latestVersionFromReleasesJson(
                    feed(R"([{"tag_name": "", "draft": false, "prerelease": false}])")).isEmpty())
        << "empty tag";
}

// ---------------------------------------------------------------------------
// The decision -- asserted against the RUNNING version, not another parse
// ---------------------------------------------------------------------------

TEST(UpdateCheckerDecision, NewerVersionIsOffered)
{
    // Derive from the running build rather than hard-coding, so this cannot
    // rot when the project version changes.
    EXPECT_TRUE(isNewerThanRunning(QStringLiteral("999.0.0")));
}

TEST(UpdateCheckerDecision, SameAndOlderVersionsAreNotOffered)
{
    EXPECT_FALSE(isNewerThanRunning(
        QString::fromStdString(currentVersion())));
    EXPECT_FALSE(isNewerThanRunning(QStringLiteral("0.0.1")));
}

TEST(UpdateCheckerDecision, UnparseableAnswerIsNotAnUpdate)
{
    // The fail-closed rule stated as its own property: no answer must never
    // become a notification.
    EXPECT_FALSE(isNewerThanRunning(QString()));
    EXPECT_FALSE(isNewerThanRunning(QStringLiteral("   ")));
    EXPECT_FALSE(isNewerThanRunning(QStringLiteral("not-a-version")));
}

// ---------------------------------------------------------------------------
// The endpoint, and the opt-in default
// ---------------------------------------------------------------------------

TEST(UpdateCheckerEndpoint, IsHttpsAndPointsAtTheRealRepository)
{
    const QString url = releasesApiUrl();
    EXPECT_TRUE(url.startsWith(QStringLiteral("https://")))
        << "the check must not be made over plaintext";
    EXPECT_TRUE(url.contains(QStringLiteral("ipswitch8/PDFE")))
        << "must name the real repository -- three placeholder "
           "'github.com/dev/PDFE' URLs shipped in MainWindow before #127";
    EXPECT_FALSE(url.contains(QStringLiteral("/dev/PDFE")))
        << "the placeholder repository must not come back";
}

TEST(UpdateCheckerOptIn, DefaultsToDisabled)
{
    // The whole premise of the opt-in choice: a stock install must make no
    // releases-feed request. Point AppSettings at a fresh INI via its
    // PDFE_SETTINGS_PATH seam, so this reads a genuine default rather than
    // whatever the developer's machine happens to have stored.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray ini = dir.filePath(QStringLiteral("fresh.ini")).toLocal8Bit();

    const QByteArray previous = qgetenv("PDFE_SETTINGS_PATH");
    qputenv("PDFE_SETTINGS_PATH", ini);

    {
        AppSettings settings;
        EXPECT_FALSE(settings.updateCheckEnabled())
            << "the version check must be OFF until the user turns it on";

        settings.setUpdateCheckEnabled(true);
        EXPECT_TRUE(settings.updateCheckEnabled())
            << "and it must actually persist when they do";
    }

    if (previous.isEmpty()) {
        qunsetenv("PDFE_SETTINGS_PATH");
    } else {
        qputenv("PDFE_SETTINGS_PATH", previous);
    }
}
