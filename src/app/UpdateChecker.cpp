// -*- coding: utf-8 -*-
#include "app/UpdateChecker.h"

#include "app/VersionInfo.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace pdfe::app {

namespace {

// The canonical repository. NOT "github.com/dev/PDFE", which is the placeholder
// that shipped in MainWindow's Documentation / Report Issue / Check for Updates
// actions and pointed all three at a repository that does not exist.
const char kReleasesApi[] =
    "https://api.github.com/repos/ipswitch8/PDFE/releases?per_page=10";

/// Strip a leading "v" from a release tag ("v0.2.0" -> "0.2.0").
QString normalizeTag(QString tag)
{
    tag = tag.trimmed();
    if (tag.startsWith(QLatin1Char('v')) || tag.startsWith(QLatin1Char('V'))) {
        tag.remove(0, 1);
    }
    return tag;
}

}  // namespace

QString releasesApiUrl()
{
    return QString::fromLatin1(kReleasesApi);
}

QString latestVersionFromReleasesJson(const QByteArray& json)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError) {
        return QString();
    }
    // GitHub returns an ARRAY for /releases and an OBJECT for an error such as
    // rate limiting. Anything that is not an array is "no answer".
    if (!doc.isArray()) {
        return QString();
    }

    QString best;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();

        // Skip drafts and pre-releases: promoting a stable user to a
        // pre-release is not what "an update is available" should mean.
        if (o.value(QStringLiteral("draft")).toBool(false))      continue;
        if (o.value(QStringLiteral("prerelease")).toBool(false)) continue;

        const QString tag = normalizeTag(o.value(QStringLiteral("tag_name")).toString());
        if (tag.isEmpty()) continue;

        // The feed is newest-first, but do not rely on that -- compare.
        // compareVersions(current, latest) reports UpdateAvailable when
        // current < latest, so "tag is newer than best" is expressed by
        // treating `best` as the current and `tag` as the candidate latest.
        if (best.isEmpty() ||
            compareVersions(best.toStdString(), tag.toStdString()) ==
                UpdateStatus::UpdateAvailable) {
            best = tag;
        }
    }
    return best;
}

bool isNewerThanRunning(const QString& latest)
{
    if (latest.trimmed().isEmpty()) {
        return false;   // fail closed: never notify on an answer we lack
    }
    return checkForUpdate(latest.toStdString()) == UpdateStatus::UpdateAvailable;
}

void checkForUpdateAsync(QNetworkAccessManager& nam,
                         std::function<void(const UpdateCheckResult&)> callback)
{
    QNetworkRequest req{QUrl(releasesApiUrl())};
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("PDFE"));
    // Redirects are followed by Qt only to HTTPS by default; keep that.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));

    QNetworkReply* reply = nam.get(req);
    QObject::connect(reply, &QNetworkReply::finished, reply,
                     [reply, cb = std::move(callback)]() {
        UpdateCheckResult r;
        if (reply->error() != QNetworkReply::NoError) {
            r.error = reply->errorString();
        } else {
            // NOTE: the response body is used for EXACTLY ONE THING -- reading
            // a version string. No URL from it is followed, no file from it is
            // written, nothing from it is executed. That is the boundary that
            // keeps this out of UC-1..UC-8's scope (docs/code-signing.md).
            const QString latest = latestVersionFromReleasesJson(reply->readAll());
            if (latest.isEmpty()) {
                r.error = QStringLiteral("could not read a version from the releases feed");
            } else {
                r.checked         = true;
                r.latestVersion   = latest;
                r.updateAvailable = isNewerThanRunning(latest);
            }
        }
        reply->deleteLater();
        cb(r);
    });
}

}  // namespace pdfe::app
