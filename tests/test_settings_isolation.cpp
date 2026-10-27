// -*- coding: utf-8 -*-
//
// Meta-audit follow-up F28 — the guard that makes the settings/registry
// isolation fix REAL rather than asserted.
//
// The defect being guarded: QCoreApplication::setOrganizationName /
// setApplicationName are process-global and sticky, so a test that sets them
// without restoring redirects every LATER default `QSettings()` in the same
// pdfe_tests.exe into a foreign hive — on Windows, a real HKCU key that then
// survives the process. See tests/support/SettingsIsolation.h for the observed
// evidence (imageStamps / RecentFiles / Security / TrustAnchors found living
// under HKCU\Software\PDFE-Test\OcrLangTest, written by tests that had nothing
// to do with the OCR-language fixture).
//
// Two guards, deliberately different in kind:
//
//  1. PoisonedTestHiveDoesNotLeakPastSettingsScope — self-contained and
//     therefore shard-independent. It plants a poisoning value in
//     HKCU\Software\PDFE-Test, proves INSIDE the scope that a default
//     QSettings() really does read that poison back (without which the
//     assertion after the scope would be vacuous), then proves the poison is
//     unreachable once the scope ends. This is the discriminating control: it
//     reds if SettingsScope stops restoring, i.e. if the leak fix is reverted.
//
//  2. DefaultSettingsIdentityIsTheProductHive — order-dependent by design.
//     It asserts the ambient identity is still the product's at the moment an
//     arbitrary test runs. Any suite that leaks in the same shard before it
//     turns it red. This is the one that would have caught the ORIGINAL bug,
//     which no self-contained test can: the original bug was precisely that a
//     later test inherited a mutated global.
//
// run_gtest.py shards into SEQUENTIAL (never concurrent) processes, so the
// registry writes below cannot race another shard.

#include <gtest/gtest.h>

#include "app/VersionInfo.h"
#include "support/SettingsIsolation.h"

#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace {

const QString kPoisonOrg = QStringLiteral("PDFE-Test");
const QString kPoisonApp = QStringLiteral("IsolationProbe");
const QString kPoisonKey = QStringLiteral("F28/sentinel");
const QString kPoisonVal = QStringLiteral("POISONED-BY-F28");

} // namespace

TEST(RegistryIsolationTest, PoisonedTestHiveDoesNotLeakPastSettingsScope)
{
    const QString prodOrg = QCoreApplication::organizationName();
    const QString prodApp = QCoreApplication::applicationName();
    const QString prodVer = QCoreApplication::applicationVersion();
    ASSERT_FALSE(prodOrg.isEmpty());
    ASSERT_FALSE(prodApp.isEmpty());
    ASSERT_NE(prodOrg, kPoisonOrg)
        << "ambient organization is already the test hive — something upstream "
           "leaked and this probe cannot distinguish anything";

    // --- Plant the poison directly in HKCU\Software\PDFE-Test\IsolationProbe.
    {
        QSettings poison(kPoisonOrg, kPoisonApp);
        poison.setValue(kPoisonKey, kPoisonVal);
        poison.sync();
        ASSERT_EQ(poison.status(), QSettings::NoError);
    }
    ASSERT_TRUE(pdfe_test::registryAppExists(kPoisonOrg, kPoisonApp))
        << "the poison never reached the registry, so every assertion below "
           "would pass for the wrong reason";

    {
        pdfe_test::SettingsScope scope(kPoisonOrg, kPoisonApp,
                                       /*purgeOnExit=*/false);

        // DISCRIMINATOR. Inside the scope a bare QSettings() — the exact shape
        // ImageStampLibrary, TrustAnchorManager and AppSettings-without-the-env
        // -seam all use — must resolve to the poisoned hive. If this fails, the
        // "poison is gone afterwards" assertion below proves nothing, because
        // the poison was never reachable in the first place.
        QSettings inside;
        ASSERT_EQ(inside.value(kPoisonKey).toString(), kPoisonVal)
            << "a default QSettings() inside the scope did not see the planted "
               "poison — the probe is not exercising the leak path";
    }

    // --- The assertion under test: the scope put everything back.
    EXPECT_EQ(QCoreApplication::organizationName(), prodOrg);
    EXPECT_EQ(QCoreApplication::applicationName(), prodApp);
    EXPECT_EQ(QCoreApplication::applicationVersion(), prodVer);

    QSettings after;
    EXPECT_FALSE(after.contains(kPoisonKey))
        << "a default QSettings() after the scope still resolves into the "
           "poisoned test hive — the settings identity leaked";
    EXPECT_NE(after.value(kPoisonKey).toString(), kPoisonVal);

    // --- Leave no residue: this test must not itself pollute HKCU.
    pdfe_test::purgeRegistryApp(kPoisonOrg, kPoisonApp);
    EXPECT_FALSE(pdfe_test::registryAppExists(kPoisonOrg, kPoisonApp))
        << "the probe's own poison hive survived the test";
}

TEST(RegistryIsolationTest, SettingsScopePurgeRemovesTheTestHiveEntirely)
{
    // The purgeOnExit contract the OCR-language and image-stamp fixtures rely
    // on: after the scope, neither HKCU\Software\<org>\<app> nor an empty
    // HKCU\Software\<org> shell is left behind.
    const QString org = QStringLiteral("PDFE-Test");
    const QString app = QStringLiteral("IsolationPurgeProbe");

    {
        pdfe_test::SettingsScope scope(org, app, /*purgeOnExit=*/true);
        QSettings s;
        s.setValue(QStringLiteral("F28/purgeProbe"), 1);
        s.sync();
        ASSERT_EQ(s.status(), QSettings::NoError);
        ASSERT_TRUE(pdfe_test::registryAppExists(org, app))
            << "nothing was written, so the purge below would be vacuous";
    }

    EXPECT_FALSE(pdfe_test::registryAppExists(org, app))
        << "purgeOnExit left the test's app key in HKCU";
    EXPECT_FALSE(pdfe_test::registryHiveExists(org))
        << "purgeOnExit left an empty HKCU\\Software\\" << org.toStdString()
        << " shell behind";
}

TEST(RegistryIsolationTest, DefaultSettingsIdentityIsTheProductHive)
{
    // Ambient-state guard. Unlike the probe above this is deliberately
    // order-dependent: it reports on whatever the tests that ran before it in
    // this shard left behind. It is the shape of check that would have caught
    // the original F28 defect, which by construction only manifests in a test
    // that runs AFTER the leaker.
    EXPECT_EQ(QCoreApplication::organizationName(), QStringLiteral("PDFE"))
        << "organizationName was left mutated by an earlier test in this "
           "process; a default QSettings() here would resolve to a foreign hive";
    EXPECT_EQ(QCoreApplication::applicationName(), QStringLiteral("PDFE"))
        << "applicationName was left mutated by an earlier test in this process";
    // Asserted against the CANONICAL version rather than a literal. This line
    // carried "0.2.0" and went red at the 0.3.0 bump — a literal here pins the
    // version of the day, not the invariant, and makes every release bump look
    // like an isolation regression. currentVersion() is CMake's PROJECT_VERSION
    // threaded through PDFE_VERSION_STRING, so this now ALSO catches the drift
    // src/main.cpp warns about: the app reporting one version while its file
    // properties claim another.
    EXPECT_EQ(QCoreApplication::applicationVersion(),
              QString::fromStdString(pdfe::app::currentVersion()))
        << "applicationVersion was left mutated by an earlier test in this "
           "process, or the literal src/main.cpp sets has drifted from the "
           "canonical version (CMake PROJECT_VERSION via PDFE_VERSION_STRING)";
    EXPECT_EQ(QSettings::defaultFormat(), QSettings::NativeFormat)
        << "QSettings::defaultFormat was left mutated by an earlier test; every "
           "default QSettings() from here on would use the wrong backend";
}
