#pragma once

// -----------------------------------------------------------------------------
// PDFE internationalization (i18n) infrastructure — R2-22.
//
// Scope: the translation *mechanism* is in place. PDFE currently ships
// English only (no compiled app_<locale>.qm are bundled), so at runtime the
// app-level translator load fails gracefully and the UI falls back to the
// source-code English strings. Adding a locale is a build-time step (lupdate ->
// translate in Qt Linguist -> lrelease); no code change is required. See
// docs/i18n.md.
//
// installTranslations() is deliberately a free function taking a
// QCoreApplication& so it is fully testable headless (no QApplication / GUI
// required) — the unit test drives it against the GoogleTest QApplication.
// -----------------------------------------------------------------------------

#include <QLocale>
#include <QString>

class QCoreApplication;

namespace pdfe::app {

// Attempt to install translations for `locale` (default: the system locale,
// e.g. "de_DE", "fr_FR").
//
// Two translators are attempted, each installed only on a successful load:
//   1. Qt's own base translations (qtbase_<locale>.qm) from
//      QLibraryInfo::TranslationsPath — this localizes standard Qt dialog
//      buttons ("OK"/"Cancel"/"&Yes"), the file dialog, etc.
//   2. PDFE's application translations (app_<locale>.qm), searched first in the
//      Qt resource path ":/i18n", then in <appdir>/translations and <appdir>.
//
// Missing .qm files are handled gracefully: the corresponding QTranslator is
// simply not installed (no crash, no error), leaving the English source strings
// in place. Installed translators are parented to `app` so they persist for the
// lifetime of the application and are cleaned up with it.
//
// Returns true iff the *application-level* translation (app_<locale>.qm) was
// found and installed. The Qt-base result does not affect the return value —
// callers use the return only to know whether PDFE's own UI strings were
// localized. A false return is the normal, expected result for the shipping
// English-only build.
bool installTranslations(QCoreApplication& app,
                         const QString& locale = QLocale::system().name());

}  // namespace pdfe::app
