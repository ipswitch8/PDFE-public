// -*- coding: utf-8 -*-
// Meta-audit follow-up F28 — see SettingsIsolation.h for the rationale.

#include "support/SettingsIsolation.h"
#include <QSettings>
#include <QFileInfo>
#include <QFile>
#include <QDir>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace pdfe_test {

namespace {

#ifdef Q_OS_WIN
QString hivePath(const QString& org)
{
    return QStringLiteral("Software\\") + org;
}

const wchar_t* wide(const QString& s)
{
    return reinterpret_cast<const wchar_t*>(s.utf16());
}

bool keyExists(const QString& path)
{
    HKEY h = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_READ, &h)
        == ERROR_SUCCESS) {
        ::RegCloseKey(h);
        return true;
    }
    return false;
}

/// True iff the key exists AND has no subkeys and no values.
bool keyIsEmpty(const QString& path)
{
    HKEY h = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_READ, &h)
        != ERROR_SUCCESS) {
        return false;
    }
    DWORD subKeys = 0;
    DWORD values = 0;
    const LSTATUS st = ::RegQueryInfoKeyW(h, nullptr, nullptr, nullptr,
                                          &subKeys, nullptr, nullptr,
                                          &values, nullptr, nullptr,
                                          nullptr, nullptr);
    ::RegCloseKey(h);
    return st == ERROR_SUCCESS && subKeys == 0 && values == 0;
}
#endif // Q_OS_WIN

#ifndef Q_OS_WIN
// The file QSettings(NativeFormat, UserScope, org, app) reads and writes. Asked
// of QSettings itself rather than reconstructed, so it follows XDG_CONFIG_HOME
// and the platform's own layout.
QString nativeAppFile(const QString& org, const QString& app)
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, org, app).fileName();
}

// <config>/<org>: the directory holding every app file of the organization.
QString nativeOrgDir(const QString& org)
{
    return QFileInfo(nativeAppFile(org, QStringLiteral("probe"))).absolutePath();
}
#endif

} // namespace

bool registryHiveExists(const QString& org)
{
#ifdef Q_OS_WIN
    if (org.isEmpty()) return false;
    return keyExists(hivePath(org));
#else
    if (org.isEmpty()) return false;
    // The NativeFormat store only, as on Windows, where the registry holds the
    // hive and IniFormat files live elsewhere: an org directory that holds
    // nothing but other tests' .ini files is not a hive.
    if (QFileInfo::exists(nativeOrgDir(org) + QStringLiteral(".conf"))) return true;
    return !QDir(nativeOrgDir(org)).entryList({QStringLiteral("*.conf")}, QDir::Files).isEmpty();
#endif
}

bool registryAppExists(const QString& org, const QString& app)
{
#ifdef Q_OS_WIN
    if (org.isEmpty() || app.isEmpty()) return false;
    return keyExists(hivePath(org) + QStringLiteral("\\") + app);
#else
    if (org.isEmpty() || app.isEmpty()) return false;
    return QFileInfo::exists(nativeAppFile(org, app));
#endif
}

void purgeRegistryHive(const QString& org)
{
#ifdef Q_OS_WIN
    if (org.isEmpty()) return;
    // RegDeleteTreeW removes the key's children and then the key itself.
    (void)::RegDeleteTreeW(HKEY_CURRENT_USER, wide(hivePath(org)));
#else
    if (org.isEmpty()) return;
    const QString dir = nativeOrgDir(org);
    QDir(dir).removeRecursively();
    QFile::remove(dir + QStringLiteral(".conf"));
#endif
}

void purgeRegistryApp(const QString& org, const QString& app)
{
#ifdef Q_OS_WIN
    if (org.isEmpty() || app.isEmpty()) return;
    const QString orgPath = hivePath(org);
    (void)::RegDeleteTreeW(HKEY_CURRENT_USER,
                           wide(orgPath + QStringLiteral("\\") + app));
    // Do not leave a bare, meaningless HKCU\Software\<org> behind — but only
    // remove it if nothing else lives under it, so concurrent/other users of
    // the same organization are never destroyed.
    if (keyIsEmpty(orgPath)) {
        (void)::RegDeleteTreeW(HKEY_CURRENT_USER, wide(orgPath));
    }
#else
    if (org.isEmpty() || app.isEmpty()) return;
    QFile::remove(nativeAppFile(org, app));
    // As on Windows: drop the organization directory only if nothing else
    // lives in it, so other users of the same organization are never touched.
    QDir().rmdir(nativeOrgDir(org));
#endif
}

SettingsScope::SettingsScope()
    : priorOrg_(QCoreApplication::organizationName()),
      priorApp_(QCoreApplication::applicationName()),
      priorVersion_(QCoreApplication::applicationVersion()),
      priorFormat_(QSettings::defaultFormat())
{
}

SettingsScope::SettingsScope(const QString& org, const QString& app,
                             bool purgeOnExit)
    : priorOrg_(QCoreApplication::organizationName()),
      priorApp_(QCoreApplication::applicationName()),
      priorVersion_(QCoreApplication::applicationVersion()),
      priorFormat_(QSettings::defaultFormat()),
      org_(org),
      app_(app),
      purgeOnExit_(purgeOnExit)
{
    QCoreApplication::setOrganizationName(org);
    QCoreApplication::setApplicationName(app);
}

SettingsScope::~SettingsScope()
{
    // Restore the format FIRST: purging below is registry-only and must not be
    // influenced by a format the guarded scope changed.
    QSettings::setDefaultFormat(priorFormat_);
    QCoreApplication::setOrganizationName(priorOrg_);
    QCoreApplication::setApplicationName(priorApp_);
    QCoreApplication::setApplicationVersion(priorVersion_);
    if (purgeOnExit_) {
        purgeRegistryApp(org_, app_);
    }
}

} // namespace pdfe_test
