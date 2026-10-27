#include "app/Translations.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QStringList>
#include <QTranslator>

namespace pdfe::app {

namespace {

// Directories searched (in order) for app_<locale>.qm on disk, after the Qt
// resource path is tried. Kept deliberately small and deterministic: the
// install directory's translations/ subfolder (where CPack/install stages the
// compiled .qm) and the exe directory itself (dev run-from-build-tree).
QStringList appTranslationDirs()
{
    QStringList dirs;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        dirs << appDir + QStringLiteral("/translations");
        dirs << appDir;
    }
    return dirs;
}

}  // namespace

bool installTranslations(QCoreApplication& app, const QString& locale)
{
    // --- 1. Qt base translations (qtbase_<locale>.qm) -----------------------
    // Localizes Qt's built-in strings (standard dialog buttons, QFileDialog,
    // etc.). Loaded from Qt's own translations directory. Failure is silent:
    // for locales Qt does not translate, or an English-only build, there is
    // simply nothing to install.
    const QString qtTrDir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    auto* qtTranslator = new QTranslator(&app);
    if (qtTranslator->load(QStringLiteral("qtbase_") + locale, qtTrDir)) {
        app.installTranslator(qtTranslator);
    } else {
        // Nothing loaded — do not install an empty translator; release it.
        delete qtTranslator;
    }

    // --- 2. PDFE application translations (app_<locale>.qm) ------------------
    auto* appTranslator = new QTranslator(&app);
    bool appInstalled = false;

    // Qt resource path first (if a future build embeds compiled .qm via an
    // i18n.qrc), then on-disk locations next to the executable.
    if (appTranslator->load(QStringLiteral("app_") + locale,
                            QStringLiteral(":/i18n"))) {
        appInstalled = true;
    } else {
        for (const QString& dir : appTranslationDirs()) {
            if (appTranslator->load(QStringLiteral("app_") + locale, dir)) {
                appInstalled = true;
                break;
            }
        }
    }

    if (appInstalled) {
        app.installTranslator(appTranslator);
    } else {
        // Graceful English fallback: no app_<locale>.qm found. This is the
        // expected path for the shipping English-only build.
        delete appTranslator;
    }

    return appInstalled;
}

}  // namespace pdfe::app
