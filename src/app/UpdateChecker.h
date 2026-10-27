// -*- coding: utf-8 -*-
//
// UpdateChecker — opt-in, notify-only version check (GitHub #127).
//
// WHAT THIS IS, AND DELIBERATELY IS NOT
// -------------------------------------
// This performs ONE HTTPS GET of the project's releases feed, extracts the
// latest published version string, and compares it against the running build
// with the pure comparison core in VersionInfo. It then NOTIFIES.
//
// It never downloads a payload, never writes an executable, never launches an
// installer, and never acts on any field of the response other than the
// version string. That boundary is the whole reason this is not the "update
// channel" governed by UC-1..UC-8 in docs/code-signing.md: those requirements
// exist because applying an update executes attacker-reachable bytes. Nothing
// here executes anything.
//
// The strongest thing a hostile or spoofed response can do is make the
// application display the wrong version number in a notification. It cannot
// cause an install, a downgrade, or a file write. UC-9 in docs/code-signing.md
// records that boundary normatively, and test_update_channel_docs.cpp enforces
// it — including asserting that this file contains no download/execute call.
//
// OFF BY DEFAULT. AppSettings::updateCheckEnabled() defaults to false, so a
// stock install makes no such request. The user opts in explicitly.
//
// Note the app already makes outbound HTTPS calls on the signing path
// (TsaClient::requestTimestamp, and the LTV validator's CRL/OCSP fetches), so
// this is not the application's first network egress — the issue that
// requested it assumed otherwise.

#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

class QNetworkAccessManager;

namespace pdfe::app {

/// Outcome of a check. `checked` distinguishes "ran and found nothing newer"
/// from "did not run", which a bare bool would conflate.
struct UpdateCheckResult {
    bool    checked         = false;  ///< the request completed and parsed
    bool    updateAvailable = false;  ///< latest > running
    QString latestVersion;            ///< e.g. "0.2.0" (empty if !checked)
    QString error;                    ///< human-readable failure, else empty
};

/// PURE. Extract the newest release version from the GitHub releases API JSON.
///
/// Kept free of I/O precisely so it is unit-testable with no network, in the
/// same spirit as VersionInfo's comparison core. Returns an empty string when
/// the payload is not what we expect — malformed JSON, an error object, an
/// empty array, or a tag that is not a version. Callers MUST treat empty as
/// "no answer" and not as "no update".
///
/// Drafts and pre-releases are skipped: shipping a stable user to a pre-release
/// is not what "an update is available" should mean.
QString latestVersionFromReleasesJson(const QByteArray& json);

/// PURE. Decide whether `latest` is newer than the running build.
/// Empty or unparseable `latest` yields false — fail closed, never notify on
/// a response we could not understand.
bool isNewerThanRunning(const QString& latest);

/// Perform the check asynchronously. `callback` is invoked exactly once.
///
/// Does nothing and reports `checked == false` when the caller has not opted
/// in; the enabled check belongs to the caller, but this is defensive.
void checkForUpdateAsync(QNetworkAccessManager& nam,
                         std::function<void(const UpdateCheckResult&)> callback);

/// The releases endpoint. Exposed so a test can assert it is HTTPS and points
/// at the real repository rather than a placeholder — three placeholder
/// `github.com/dev/PDFE` URLs shipped in MainWindow before #127 found them.
QString releasesApiUrl();

}  // namespace pdfe::app
