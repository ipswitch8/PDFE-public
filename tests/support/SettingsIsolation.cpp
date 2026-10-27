// -*- coding: utf-8 -*-
// Meta-audit follow-up F28 — see SettingsIsolation.h for the rationale.

#include "support/SettingsIsolation.h"

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

} // namespace

bool registryHiveExists(const QString& org)
{
#ifdef Q_OS_WIN
    if (org.isEmpty()) return false;
    return keyExists(hivePath(org));
#else
    Q_UNUSED(org);
    return false;
#endif
}

bool registryAppExists(const QString& org, const QString& app)
{
#ifdef Q_OS_WIN
    if (org.isEmpty() || app.isEmpty()) return false;
    return keyExists(hivePath(org) + QStringLiteral("\\") + app);
#else
    Q_UNUSED(org);
    Q_UNUSED(app);
    return false;
#endif
}

void purgeRegistryHive(const QString& org)
{
#ifdef Q_OS_WIN
    if (org.isEmpty()) return;
    // RegDeleteTreeW removes the key's children and then the key itself.
    (void)::RegDeleteTreeW(HKEY_CURRENT_USER, wide(hivePath(org)));
#else
    Q_UNUSED(org);
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
    Q_UNUSED(org);
    Q_UNUSED(app);
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
