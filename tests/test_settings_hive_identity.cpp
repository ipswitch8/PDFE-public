// -*- coding: utf-8 -*-
//
// Meta-audit follow-up F33 — the PRODUCT side of the registry-pollution class.
//
// F28 closed the test side: pdfe_test::SettingsScope restores the
// process-global settings identity (organizationName / applicationName /
// applicationVersion / QSettings::defaultFormat) on every exit path, and
// purges the test hive behind it. Its own header states the limit of that fix
// exactly:
//
//     "Anything that constructs a bare `QSettings` — ImageStampLibrary,
//      TrustAnchorManager, the theme/recent-files tests — still follows
//      organizationName()/applicationName()."
//
// "Still follows" is the DESIRED property. A class that follows the ambient
// identity can be redirected by SettingsScope, and therefore isolated. The
// product-side defect F33 fixes is the opposite shape: ThemeManager wrote its
// persisted Light/Dark/System choice through a literal
// `QSettings("PDFE", "PDFE")` (src/theme/ThemeManager.cpp:103 and :111 before
// this phase). An explicit org/app pair pins the hive IN THE SOURCE — it
// resolves to HKCU\Software\PDFE\PDFE regardless of what the application, or a
// test, has set the identity to. Neither of the project's two seams can reach
// it: SettingsScope moves the identity, PDFE_SETTINGS_PATH moves AppSettings,
// and a literal pair ignores both.
//
// WHAT DISCRIMINATES, AND WHAT DOES NOT — stated plainly, because a test that
// passes before and after the fix proves nothing:
//
//   * ThemeManagerPersistsUnderTheApplicationIdentityNotAHardCodedHive is the
//     DISCRIMINATING control. It FAILS against the pre-fix ThemeManager: with
//     the literal in place the write lands in the product hive, so the read
//     back through the scoped identity finds nothing AND the product hive's
//     value changes. Both halves red. See phase-33-dispositions.md for the
//     captured `git stash` run.
//
//   * The ImageStampLibrary and TrustAnchorManager cases are NON-discriminating
//     by construction: those two classes already used a bare `QSettings` and
//     were never defective. They are regression guards — they pin the property
//     the fix relies on so a future "let me just name the hive explicitly"
//     edit to either class reds here. They are not evidence that F33 changed
//     anything about them, and are not presented as such.
//
// No test here writes to HKCU\Software\PDFE. Every write happens inside a
// SettingsScope pointed at a PDFE-Test hive with purgeOnExit=true, and each
// case additionally ASSERTS the real product hive's value is unchanged
// afterwards — which is simultaneously the strongest statement of the property
// under test and a guarantee that running pdfe_tests.exe does not damage a
// developer's installed PDFE settings.

#include <gtest/gtest.h>

#include "support/SettingsIsolation.h"

#include "annotations/ImageStampLibrary.h"
#include "app/AppSettings.h"
#include "signatures/TrustAnchorManager.h"
#include "theme/ThemeManager.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QImage>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <filesystem>

namespace {

const QString kTestOrg     = QStringLiteral("PDFE-Test");
const QString kThemeKey    = QStringLiteral("Preferences/theme");
const QString kStampsOrder = QStringLiteral("pdfe/imageStamps/__order");
const QString kTrustGroup  = QStringLiteral("TrustAnchors");

// Every test below is only meaningful if the ambient identity really is the
// product's when it starts, and if AppSettings is NOT diverted onto an INI
// file by the env seam (which would make "did it follow the org/app name?"
// unanswerable rather than false).
::testing::AssertionResult ambientPreconditionsHold()
{
    if (!qgetenv("PDFE_SETTINGS_PATH").isEmpty()) {
        return ::testing::AssertionFailure()
            << "PDFE_SETTINGS_PATH is set; AppSettings would use the INI seam "
               "and this probe cannot observe the org/app identity at all";
    }
    if (QCoreApplication::organizationName() != QStringLiteral("PDFE") ||
        QCoreApplication::applicationName()  != QStringLiteral("PDFE")) {
        return ::testing::AssertionFailure()
            << "ambient identity is "
            << QCoreApplication::organizationName().toStdString() << "/"
            << QCoreApplication::applicationName().toStdString()
            << ", not PDFE/PDFE — an earlier test leaked, and the "
               "'product hive untouched' assertions would be vacuous";
    }
    if (QSettings::defaultFormat() != QSettings::NativeFormat) {
        return ::testing::AssertionFailure()
            << "QSettings::defaultFormat was left mutated by an earlier test";
    }
    return ::testing::AssertionSuccess();
}

QString productHiveValue(const QString& key)
{
    QSettings prod(QCoreApplication::organizationName(),
                   QCoreApplication::applicationName());
    prod.sync();
    return prod.value(key).toString();
}

QStringList productHiveList(const QString& key)
{
    QSettings prod(QCoreApplication::organizationName(),
                   QCoreApplication::applicationName());
    prod.sync();
    return prod.value(key).toStringList();
}

QStringList productHiveTrustFingerprints()
{
    QSettings prod(QCoreApplication::organizationName(),
                   QCoreApplication::applicationName());
    prod.sync();
    prod.beginGroup(kTrustGroup);
    QStringList out = prod.childGroups();
    prod.endGroup();
    out.sort();
    return out;
}

} // namespace

// ===========================================================================
// THE DISCRIMINATING CONTROL — reds against the pre-fix ThemeManager.
// ===========================================================================
TEST(SettingsHiveIdentityTest,
     ThemeManagerPersistsUnderTheApplicationIdentityNotAHardCodedHive)
{
    ASSERT_TRUE(ambientPreconditionsHold());

    // Choose a theme string that DIFFERS from whatever the product hive
    // currently holds. Without this the "product hive untouched" half could
    // pass by coincidence — if the pre-fix code wrote "dark" into a hive that
    // already said "dark", nothing would look different.
    const QString productThemeBefore = productHiveValue(kThemeKey);
    const pdfe::theme::Theme probe =
        (productThemeBefore == QStringLiteral("dark"))
            ? pdfe::theme::Theme::Light
            : pdfe::theme::Theme::Dark;
    const QString probeStr = pdfe::theme::ThemeManager::themeToString(probe);
    ASSERT_NE(probeStr, productThemeBefore)
        << "the probe value matches what the product hive already holds, so "
           "the untouched-hive assertion below could not distinguish anything";

    // setTheme() calls QApplication::setPalette, which is process-global.
    const QPalette savedPalette = QApplication::palette();

    {
        pdfe_test::SettingsScope scope(kTestOrg,
                                       QStringLiteral("HiveIdentityThemeTest"),
                                       /*purgeOnExit=*/true);
        {
            QSettings fresh;  // scoped identity
            fresh.clear();
            fresh.sync();
            ASSERT_TRUE(fresh.value(kThemeKey).toString().isEmpty())
                << "the scoped hive was not empty at the start, so finding the "
                   "probe value there afterwards would prove nothing";
        }

        pdfe::app::AppSettings settings;
        pdfe::theme::ThemeManager mgr(&settings);
        mgr.setTheme(probe);

        // DISCRIMINATOR (a): a bare QSettings under the ACTIVE identity must
        // see the write. Pre-fix the literal sent it to PDFE/PDFE and this is
        // empty.
        QSettings scoped;
        scoped.sync();
        EXPECT_EQ(scoped.value(kThemeKey).toString(), probeStr)
            << "ThemeManager did not persist under the active application "
               "identity — it is naming a hive literally instead of deriving "
               "it from QCoreApplication::organizationName()/applicationName()";

        EXPECT_TRUE(pdfe_test::registryAppExists(
                        kTestOrg, QStringLiteral("HiveIdentityThemeTest")))
            << "nothing reached the scoped registry hive at all";
    }

    QApplication::setPalette(savedPalette);

    // DISCRIMINATOR (b): the real product hive must be exactly as we found it.
    // Pre-fix this is where the write went.
    EXPECT_EQ(productHiveValue(kThemeKey), productThemeBefore)
        << "ThemeManager wrote into HKCU\\Software\\PDFE\\PDFE while the "
           "application identity was PDFE-Test/HiveIdentityThemeTest — the "
           "hive is pinned in the source, not derived from the identity";

    EXPECT_FALSE(pdfe_test::registryAppExists(
                     kTestOrg, QStringLiteral("HiveIdentityThemeTest")))
        << "the scope did not purge its own test hive";
}

// ===========================================================================
// NON-DISCRIMINATING regression guards. These two classes were already
// correct; the guards stop them regressing into the shape ThemeManager had.
// ===========================================================================
TEST(SettingsHiveIdentityTest, ImageStampLibraryFollowsTheApplicationIdentity)
{
    ASSERT_TRUE(ambientPreconditionsHold());

    const QStringList productOrderBefore = productHiveList(kStampsOrder);

    {
        pdfe_test::SettingsScope scope(kTestOrg,
                                       QStringLiteral("HiveIdentityStampTest"),
                                       /*purgeOnExit=*/true);
        {
            QSettings fresh;
            fresh.clear();
            fresh.sync();
            ASSERT_TRUE(fresh.value(kStampsOrder).toStringList().isEmpty());
        }

        QImage img(4, 4, QImage::Format_ARGB32);
        img.fill(Qt::red);

        pdfe::annotations::ImageStampLibrary lib;
        ASSERT_TRUE(lib.addStamp(QStringLiteral("f33-probe"), img));
        lib.saveToSettings();

        QSettings scoped;
        scoped.sync();
        EXPECT_FALSE(scoped.value(kStampsOrder).toStringList().isEmpty())
            << "ImageStampLibrary did not persist under the active application "
               "identity";

        // Round-trip through a second instance, so the assertion is about the
        // class's own read path and not only about raw key presence.
        pdfe::annotations::ImageStampLibrary reloaded;
        reloaded.loadFromSettings();
        EXPECT_TRUE(reloaded.hasStamp(QStringLiteral("f33-probe")))
            << "the stamp did not round-trip through the scoped hive";
    }

    EXPECT_EQ(productHiveList(kStampsOrder), productOrderBefore)
        << "ImageStampLibrary wrote into the real product hive while the "
           "application identity was a test one";
}

TEST(SettingsHiveIdentityTest, TrustAnchorManagerFollowsTheApplicationIdentity)
{
    ASSERT_TRUE(ambientPreconditionsHold());

    const std::filesystem::path anchorPem =
        std::filesystem::path(TEST_FIXTURES_DIR) / "certs" / "test_root_ca.pem";
    ASSERT_TRUE(std::filesystem::exists(anchorPem))
        << "missing fixture " << anchorPem.string();

    // Force the singleton's lazy load NOW, under the product identity. If the
    // first instance() call happened inside the scope below it would load from
    // the (empty) test hive and keep that empty in-memory list for the rest of
    // the process, silently starving every later trust test in this shard.
    auto& tam = pdfe::signatures::TrustAnchorManager::instance();
    const qsizetype anchorsBefore = tam.listTrusts().size();
    const QStringList productTrustBefore = productHiveTrustFingerprints();

    QString addedFingerprint;
    {
        pdfe_test::SettingsScope scope(kTestOrg,
                                       QStringLiteral("HiveIdentityTrustTest"),
                                       /*purgeOnExit=*/true);
        {
            QSettings fresh;
            fresh.clear();
            fresh.sync();
        }

        QString err;
        ASSERT_TRUE(tam.addTrust(anchorPem, &err))
            << "addTrust failed: " << err.toStdString();

        QSettings scoped;
        scoped.sync();
        scoped.beginGroup(kTrustGroup);
        const QStringList scopedFingerprints = scoped.childGroups();
        scoped.endGroup();
        EXPECT_FALSE(scopedFingerprints.isEmpty())
            << "TrustAnchorManager did not persist under the active "
               "application identity";

        // Put the singleton back the way we found it, still inside the scope
        // so the removal writes to the test hive rather than the product one.
        for (const auto& a : tam.listTrusts()) {
            if (scopedFingerprints.contains(a.fingerprintSha256)) {
                addedFingerprint = a.fingerprintSha256;
                break;
            }
        }
        ASSERT_FALSE(addedFingerprint.isEmpty());
        EXPECT_TRUE(tam.removeTrust(addedFingerprint));
    }

    EXPECT_EQ(tam.listTrusts().size(), anchorsBefore)
        << "the probe left the TrustAnchorManager singleton mutated for every "
           "later test in this shard";
    EXPECT_EQ(productHiveTrustFingerprints(), productTrustBefore)
        << "TrustAnchorManager wrote into the real product hive while the "
           "application identity was a test one";
}
