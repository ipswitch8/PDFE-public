// -*- coding: utf-8 -*-
#pragma once
//
// Meta-audit follow-up F28 — test-suite settings/registry isolation.
//
// WHY THIS EXISTS
// ---------------
// QCoreApplication::setOrganizationName / setApplicationName /
// setApplicationVersion and QSettings::setDefaultFormat are PROCESS-GLOBAL and
// STICKY. A test that sets them and does not put them back changes where every
// LATER test in the same pdfe_tests.exe process resolves a default
// `QSettings()` — and on Windows that is the real user registry under
// HKCU\Software\<org>\<app>.
//
// This was not theoretical. Before this header existed, a live registry query
// on the dev box showed:
//
//   HKCU\Software\PDFE-Test\OcrLangTest\pdfe\imageStamps
//   HKCU\Software\PDFE-Test\OcrLangTest\RecentFiles
//   HKCU\Software\PDFE-Test\OcrLangTest\Security
//   HKCU\Software\PDFE-Test\OcrLangTest\TrustAnchors
//
// "PDFE-Test"/"OcrLangTest" is the identity set by the OCR-language fixture.
// It never writes imageStamps, RecentFiles, Security or TrustAnchors — those
// were written by ImageStampLibrary, the recent-files tests, the preferences
// tests and the LTV trust-anchor tests, all of which ran LATER in the same
// process and inherited the leaked identity. Their state landed in a hive
// nobody cleans and which survives the process.
//
// F8 fixed one symptom (the OCR fixture's own AppSettings reads) by routing
// them through the PDFE_SETTINGS_PATH seam. It did NOT fix the class: the seam
// only covers pdfe::app::AppSettings. Anything that constructs a bare
// `QSettings` — ImageStampLibrary, TrustAnchorManager, the theme/recent-files
// tests — still follows organizationName()/applicationName().
//
// HOW TO USE
// ----------
// Any test that must change the settings identity constructs a SettingsScope
// for the duration and lets its destructor put everything back. Because it is
// RAII it is also correct on the paths a hand-written "restore at the end of
// the test body" misses: a failing ASSERT_*, a GTEST_SKIP, or a throw all
// return early and skip trailing restore statements.
//
//     class MyTest : public ::testing::Test {
//     protected:
//         void SetUp() override {
//             scope_.emplace("PDFE-Test", "MyTest", /*purgeOnExit=*/true);
//         }
//         void TearDown() override { scope_.reset(); }
//         std::optional<pdfe_test::SettingsScope> scope_;
//     };
//
// A fixture that merely wants to be immune to whatever the test body does to
// the global identity can default-construct it (capture-and-restore only).

#include <QCoreApplication>
#include <QSettings>
#include <QString>

namespace pdfe_test {

/// True iff HKCU\Software\<org> exists. Always false off Windows.
bool registryHiveExists(const QString& org);

/// True iff HKCU\Software\<org>\<app> exists. Always false off Windows.
bool registryAppExists(const QString& org, const QString& app);

/// Recursively delete HKCU\Software\<org>. No-op off Windows.
/// Only ever call this for a TEST-OWNED organization name.
void purgeRegistryHive(const QString& org);

/// Recursively delete HKCU\Software\<org>\<app>, then delete
/// HKCU\Software\<org> as well if that left it with no subkeys and no values.
/// No-op off Windows. Only ever call this for a TEST-OWNED organization name.
void purgeRegistryApp(const QString& org, const QString& app);

/// RAII guard for the process-global Qt settings identity.
///
/// Captures organizationName, applicationName, applicationVersion and
/// QSettings::defaultFormat on construction and restores all four on
/// destruction, so nothing the guarded scope does can be observed by a later
/// test in the same process.
class SettingsScope {
public:
    /// Capture-and-restore only: does not change the identity, but guarantees
    /// that whatever the guarded scope changes is put back.
    SettingsScope();

    /// Switch the identity to <org>/<app> for the lifetime of this object.
    /// When @p purgeOnExit is true the destructor also deletes
    /// HKCU\Software\<org>\<app> (and <org> if it is then empty), so a test
    /// hive does not outlive the process that created it. Pass false when
    /// <org> is the real product hive.
    SettingsScope(const QString& org, const QString& app,
                  bool purgeOnExit = false);

    ~SettingsScope();

    SettingsScope(const SettingsScope&) = delete;
    SettingsScope& operator=(const SettingsScope&) = delete;

private:
    QString priorOrg_;
    QString priorApp_;
    QString priorVersion_;
    QSettings::Format priorFormat_;
    QString org_;
    QString app_;
    bool purgeOnExit_ = false;
};

} // namespace pdfe_test
