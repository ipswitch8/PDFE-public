// -----------------------------------------------------------------------------
// R2-22 — i18n infrastructure test.
//
// Verifies the QTranslator loading mechanism (pdfe::app::installTranslations)
// exists, is callable headless, degrades gracefully when no .qm is present, and
// that installed QTranslators actually route QCoreApplication::translate() —
// which is the exact mechanism installTranslations() depends on.
//
// Determinism note: this test does NOT depend on lrelease producing a .qm at
// build/test time. The "real translation round-trip" is proven with an
// in-process QTranslator subclass (FakeTranslator) that returns a canned
// translation, so the assertion is fully deterministic and headless. See the
// limitation note in the pipeline report re: shipping a real binary .qm
// fixture without lrelease.
// -----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>          // GitHub #128 -- the .qm loading path
#include <QIODevice>
#include <QString>
#include <QTranslator>

#include "app/Translations.h"

using namespace pdfe::app;

namespace {

// A QTranslator that returns a fixed translation for one known
// (context, source) pair. Proves the install -> translate() routing without a
// compiled .qm. isEmpty() must return false or QCoreApplication::installTranslator
// refuses to install it.
class FakeTranslator : public QTranslator {
public:
    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation = nullptr,
                      int n = -1) const override
    {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (QByteArray(context) == "i18n_test" &&
            QByteArray(sourceText) == "Open File") {
            return QString::fromUtf8("Datei \xc3\xb6" "ffnen");  // "Datei öffnen"
        }
        return QString();  // empty => fall through to source text
    }

    bool isEmpty() const override { return false; }
};

}  // namespace

// The mechanism must be callable and must NOT crash / must return false for a
// locale that has no shipped .qm anywhere (graceful English fallback).
// (This is the RED driver: before Translations.{h,cpp} exist the suite does not
// compile/link.)
TEST(I18nTranslations, InstallReturnsFalseForBogusLocaleGracefully)
{
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    const bool installed =
        installTranslations(*QCoreApplication::instance(), "zz_ZZ");
    EXPECT_FALSE(installed);
}

// The default (system-locale) path must be safe.
//
// UPDATED 2026-09-06 (GitHub #128). This test used to assert
// EXPECT_FALSE(installed), and its comment explained why: "PDFE ships
// English-only with no app_<locale>.qm in the test tree". That premise was
// true and is now false -- the build compiles translations/*.ts into app_en.qm
// and app_fr.qm and stages them next to the executable, so on a matching
// system locale the catalogue really does load.
//
// The EXPECT_FALSE was encoding the ABSENCE of the feature, not the guarantee
// the comment claimed to care about ("does not crash", "returns a bool"). So
// the assertion is now on the guarantee: whatever the machine's locale, this
// must complete and yield a definite answer. Pinning it to either value would
// make the test depend on the locale of whoever runs it.
TEST(I18nTranslations, InstallWithSystemLocaleIsSafe)
{
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    const bool installed = installTranslations(*QCoreApplication::instance());
    // Deliberately not asserted true or false: the result legitimately depends
    // on the host locale. What must hold is that we got here at all.
    EXPECT_TRUE(installed == true || installed == false);
}

// GitHub #128. The .qm LOADING path -- which could not be covered before,
// because no .qm existed anywhere in the repo to load.
//
// test_i18n_pseudo_localization.cpp records this gap explicitly: a QTranslator
// subclass "CANNOT cover the .qm loading path itself; that stays open". It is
// now closed, by the real artifact rather than a synthetic translator.
TEST(I18nTranslations, ShippedEnglishCatalogueLoadsFromDisk)
{
    ASSERT_NE(QCoreApplication::instance(), nullptr);

    // Locate the staged catalogue the build produced. If it is absent the
    // build step regressed, and that is a real failure rather than a reason to
    // skip -- skipping here would restore precisely the silent nothing-happens
    // state #128 was filed about.
    const QString dir = QCoreApplication::applicationDirPath();
    const QString qm  = dir + QStringLiteral("/translations/app_en.qm");
    ASSERT_TRUE(QFile::exists(qm))
        << "no app_en.qm staged next to the test binary at "
        << qm.toStdString()
        << " -- the qt_add_lrelease step in src/app/CMakeLists.txt (GitHub "
           "#128) is not producing or staging catalogues";

    // And it must be a real catalogue, not a zero-byte placeholder that would
    // satisfy the existence check while translating nothing.
    QFile f(qm);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_GT(f.size(), 100)
        << "app_en.qm is implausibly small to contain a message catalogue";

    // The loader accepts it.
    QTranslator t;
    EXPECT_TRUE(t.load(QStringLiteral("app_en"), dir + QStringLiteral("/translations")))
        << "QTranslator refused the compiled catalogue -- lrelease produced a "
           "file the runtime cannot read";
}

// The core mechanism: installing a QTranslator changes what
// QCoreApplication::translate() returns, and removing it restores the source
// text. This is exactly how installTranslations() localizes the UI once a real
// app_<locale>.qm is loaded.
TEST(I18nTranslations, InstalledTranslatorRoutesTranslateCalls)
{
    auto* app = QCoreApplication::instance();
    ASSERT_NE(app, nullptr);

    // Baseline: no translation installed for this string => echoes source.
    EXPECT_EQ(QCoreApplication::translate("i18n_test", "Open File"),
              QString("Open File"));

    auto* fake = new FakeTranslator();
    ASSERT_TRUE(app->installTranslator(fake));
    EXPECT_EQ(QCoreApplication::translate("i18n_test", "Open File"),
              QString::fromUtf8("Datei \xc3\xb6" "ffnen"));

    ASSERT_TRUE(app->removeTranslator(fake));
    delete fake;

    // Removed => back to the source text (no leaked translator state).
    EXPECT_EQ(QCoreApplication::translate("i18n_test", "Open File"),
              QString("Open File"));
}
