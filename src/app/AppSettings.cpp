#include "AppSettings.h"
#include <QByteArray>
#include <QDir>

namespace pdfe::app {

namespace {
// Pipeline H phase-3: when this env var is set to a non-empty
// absolute INI path, AppSettings constructs QSettings against
// that file (IniFormat) instead of the default Qt-managed
// per-org/app location. Tests use this for cross-process
// determinism — each test points two PDFE process launches at
// the same fresh INI file.
constexpr const char* kSettingsPathEnv = "PDFE_SETTINGS_PATH";
}

AppSettings::AppSettings() {
    const QByteArray envPath = qgetenv(kSettingsPathEnv);
    if (!envPath.isEmpty()) {
        // Use IniFormat against the explicit path so all writes
        // and reads are predictable: a flat text file the test
        // harness can also inspect/mutate directly.
        settings_ = std::make_unique<QSettings>(
            QString::fromLocal8Bit(envPath), QSettings::IniFormat);
    } else {
        // Default: Qt picks the location from
        // QApplication::organizationName()/applicationName().
        settings_ = std::make_unique<QSettings>();
    }
}

AppSettings::~AppSettings() = default;

// Recent Files
QStringList AppSettings::recentFiles() const {
    return settings_->value("RecentFiles/list", QStringList()).toStringList();
}

void AppSettings::addRecentFile(const QString& filePath) {
    QStringList files = recentFiles();

    // Remove duplicates (case-insensitive on Windows)
    files.removeAll(filePath);

#ifdef Q_OS_WIN
    // Additional case-insensitive check for Windows
    for (int i = files.size() - 1; i >= 0; --i) {
        if (files[i].compare(filePath, Qt::CaseInsensitive) == 0) {
            files.removeAt(i);
        }
    }
#endif

    // Prepend to list
    files.prepend(filePath);

    // Trim to max
    const int max = maxRecentFiles();
    while (files.size() > max) {
        files.removeLast();
    }

    settings_->setValue("RecentFiles/list", files);
    // Pipeline H phase-3: explicit sync so a subsequent process
    // launch reading the same INI sees the write even if the
    // current process is hard-killed before its destructor runs.
    settings_->sync();
}

void AppSettings::removeRecentFile(const QString& filePath) {
    QStringList files = recentFiles();
    files.removeAll(filePath);

#ifdef Q_OS_WIN
    // Additional case-insensitive check for Windows
    for (int i = files.size() - 1; i >= 0; --i) {
        if (files[i].compare(filePath, Qt::CaseInsensitive) == 0) {
            files.removeAt(i);
        }
    }
#endif

    settings_->setValue("RecentFiles/list", files);
    settings_->sync();  // Pipeline H phase-3 cross-process flush.
}

void AppSettings::clearRecentFiles() {
    settings_->setValue("RecentFiles/list", QStringList());
    settings_->sync();  // Pipeline H phase-3 cross-process flush.
}

int AppSettings::maxRecentFiles() const {
    return settings_->value("RecentFiles/max", DEFAULT_MAX_RECENT).toInt();
}

void AppSettings::setMaxRecentFiles(int count) {
    settings_->setValue("RecentFiles/max", count);

    // Trim existing list if necessary
    QStringList files = recentFiles();
    if (files.size() > count) {
        while (files.size() > count) {
            files.removeLast();
        }
        settings_->setValue("RecentFiles/list", files);
    }
    settings_->sync();  // Pipeline H phase-3 cross-process flush.
}

// Window State
QByteArray AppSettings::windowGeometry() const {
    return settings_->value("Window/geometry", QByteArray()).toByteArray();
}

void AppSettings::setWindowGeometry(const QByteArray& geometry) {
    settings_->setValue("Window/geometry", geometry);
}

QByteArray AppSettings::windowState() const {
    return settings_->value("Window/state", QByteArray()).toByteArray();
}

void AppSettings::setWindowState(const QByteArray& state) {
    settings_->setValue("Window/state", state);
}

// Panel Visibility
bool AppSettings::pagePanelVisible() const {
    return settings_->value("Panels/pagePanel", true).toBool();
}

void AppSettings::setPagePanelVisible(bool visible) {
    settings_->setValue("Panels/pagePanel", visible);
}

bool AppSettings::propertiesPanelVisible() const {
    return settings_->value("Panels/propertiesPanel", true).toBool();
}

void AppSettings::setPropertiesPanelVisible(bool visible) {
    settings_->setValue("Panels/propertiesPanel", visible);
}

bool AppSettings::searchPanelVisible() const {
    return settings_->value("Panels/searchPanel", false).toBool();
}

void AppSettings::setSearchPanelVisible(bool visible) {
    settings_->setValue("Panels/searchPanel", visible);
}

bool AppSettings::bookmarkPanelVisible() const {
    return settings_->value("Panels/bookmarkPanel", false).toBool();
}

void AppSettings::setBookmarkPanelVisible(bool visible) {
    settings_->setValue("Panels/bookmarkPanel", visible);
}

// User Preferences
float AppSettings::defaultZoom() const {
    return settings_->value("Preferences/defaultZoom", DEFAULT_ZOOM).toFloat();
}

void AppSettings::setDefaultZoom(float zoom) {
    settings_->setValue("Preferences/defaultZoom", zoom);
}

QString AppSettings::lastOpenDirectory() const {
    return settings_->value("Preferences/lastOpenDirectory", QDir::homePath()).toString();
}

void AppSettings::setLastOpenDirectory(const QString& dir) {
    settings_->setValue("Preferences/lastOpenDirectory", dir);
}

QString AppSettings::author() const {
    const QString stored = settings_->value("Preferences/author", QString()).toString();
    if (!stored.isEmpty()) return stored;
    // Fall back to OS user name. QSysInfo::machineHostName isn't the user, so
    // pull from environment: USERNAME (Windows), USER (Unix).
    const QByteArray user = qgetenv("USERNAME");
    if (!user.isEmpty()) return QString::fromLocal8Bit(user);
    const QByteArray user2 = qgetenv("USER");
    if (!user2.isEmpty()) return QString::fromLocal8Bit(user2);
    return QStringLiteral("Unknown");
}

void AppSettings::setAuthor(const QString& author) {
    settings_->setValue("Preferences/author", author);
}

bool AppSettings::useTantivySearch() const {
    return settings_->value("Preferences/useTantivySearch", true).toBool();
}

void AppSettings::setUseTantivySearch(bool enabled) {
    settings_->setValue("Preferences/useTantivySearch", enabled);
}

QString AppSettings::ocrLanguage() const {
    return settings_->value("Preferences/ocrLanguage", QStringLiteral("eng")).toString();
}

void AppSettings::setOcrLanguage(const QString& lang) {
    settings_->setValue("Preferences/ocrLanguage", lang);
}

// Meta-audit follow-up F33. Same "Preferences/<name>" convention as every
// accessor above, so the stored key is byte-identical to the one
// ThemeManager's former literal QSettings("PDFE","PDFE") wrote. The
// difference is only WHERE it resolves: through settings_, which is either
// the PDFE_SETTINGS_PATH INI or a default QSettings() following the
// application-level org/app name.
QString AppSettings::theme() const {
    return settings_->value("Preferences/theme", QStringLiteral("system")).toString();
}

void AppSettings::setTheme(const QString& theme) {
    settings_->setValue("Preferences/theme", theme);
}

int AppSettings::tileCacheSizeMB() const {
    return settings_->value("Preferences/tileCacheSizeMB", DEFAULT_CACHE_SIZE_MB).toInt();
}

void AppSettings::setTileCacheSizeMB(int mb) {
    settings_->setValue("Preferences/tileCacheSizeMB", mb);
}

// Logging
int AppSettings::logLevel() const {
    return settings_->value("Logging/level", DEFAULT_LOG_LEVEL).toInt();
}

void AppSettings::setLogLevel(int level) {
    settings_->setValue("Logging/level", level);
}

bool AppSettings::logToFile() const {
    return settings_->value("Logging/toFile", false).toBool();
}

void AppSettings::setLogToFile(bool enabled) {
    settings_->setValue("Logging/toFile", enabled);
}

// GitHub #240. Default TRUE: a user opening their own file on their own file
// server is ordinary, and refusing it is the defect the issue reports.
bool AppSettings::allowUserNetworkPaths() const {
    return settings_->value("Network/allowUserPaths", true).toBool();
}

void AppSettings::setAllowUserNetworkPaths(bool allowed) {
    settings_->setValue("Network/allowUserPaths", allowed);
}

// GitHub #240. Default FALSE, and this is the one that must stay false unless
// asked: a resource path chosen by DOCUMENT CONTENT is the egress vector #91
// closed. Even when enabled it is scoped to the document's own share.
bool AppSettings::allowDocumentSameShareResources() const {
    return settings_->value("Network/allowDocumentSameShareResources", false).toBool();
}

void AppSettings::setAllowDocumentSameShareResources(bool allowed) {
    settings_->setValue("Network/allowDocumentSameShareResources", allowed);
}

// GitHub #127. Default FALSE -- opting in is the user's act, not ours.
bool AppSettings::updateCheckEnabled() const {
    return settings_->value("Updates/checkOnStartup", false).toBool();
}

void AppSettings::setUpdateCheckEnabled(bool enabled) {
    settings_->setValue("Updates/checkOnStartup", enabled);
}

// GitHub #151. The default used to be QDir::temp().filePath("pdfe.log") —
// %TEMP%/pdfe.log — which was BOTH never read (nothing consulted this getter)
// and, had it been read, a privacy downgrade: %TEMP% is a shared, effectively
// world-readable directory on a multi-user machine, whereas LogHandler's own
// default (AppLocalDataLocation/pdfe.log) is per-user. Now that
// applyLoggingPreferences() actually honours this value, the default must be
// the SAFE location rather than the one that happened to be typed here.
//
// EMPTY is that answer, deliberately, rather than duplicating LogHandler's
// path expression in a second place where the two could drift: empty means
// "LogHandler's platform default". Only an explicit user choice moves the log.
QString AppSettings::logFilePath() const {
    return settings_->value("Logging/filePath", QString()).toString();
}

void AppSettings::setLogFilePath(const QString& path) {
    settings_->setValue("Logging/filePath", path);
}

// --- Security / active-content gates -----------------------------
//
// Both default to FALSE. PDFE is a cross-platform PDF editor and a
// PDF is an attacker-controlled file format -- a malicious doc can
// embed JavaScript / SubmitForm / Launch actions that a permissive
// reader executes silently at open. We default-deny the categories
// that can cause network resolution, process launch, or data
// exfiltration; the user opts in via the Settings dialog (or a
// per-document active-content banner) for documents they trust.
//
// Phase-3 corpus runs measured concrete leakage from MuPDF's
// internal /AA, /OpenAction, and AcroForm /CO handlers when JS
// was unconditionally enabled at document load. Flipping the
// default here closes most of that vector.

bool AppSettings::javaScriptEnabled() const {
    return settings_->value("Security/javaScriptEnabled", false).toBool();
}

void AppSettings::setJavaScriptEnabled(bool enabled) {
    settings_->setValue("Security/javaScriptEnabled", enabled);
}

bool AppSettings::externalActionsAllowed() const {
    return settings_->value("Security/externalActionsAllowed", false).toBool();
}

void AppSettings::setExternalActionsAllowed(bool enabled) {
    settings_->setValue("Security/externalActionsAllowed", enabled);
}

// JPEG2000 (JPX) rendering — default OFF (#50). See the MuPDF decode gate in
// vcpkg-overlay-ports/libmupdf/0001-pdfe-jpx-decode-gate.patch and
// src/core/JpxSecurityGate.h. Enabling re-accepts the OpenJPEG heap-overflow risk
// on untrusted files; the Preferences Security tab warns once before turning it on.
bool AppSettings::jpxRenderingEnabled() const {
    return settings_->value("Security/jpxRenderingEnabled", false).toBool();
}

void AppSettings::setJpxRenderingEnabled(bool enabled) {
    settings_->setValue("Security/jpxRenderingEnabled", enabled);
}

// JBIG2 rendering — default OFF (SEC-56 / #56). See the MuPDF decode gate in
// vcpkg-overlay-ports/libmupdf/0002-pdfe-jbig2-decode-gate.patch and
// src/core/Jbig2SecurityGate.h. Enabling re-accepts jbig2dec's memory-safety
// CVE history on untrusted files; the Preferences Security tab warns once
// before turning it on.
bool AppSettings::jbig2RenderingEnabled() const {
    return settings_->value("Security/jbig2RenderingEnabled", false).toBool();
}

void AppSettings::setJbig2RenderingEnabled(bool enabled) {
    settings_->setValue("Security/jbig2RenderingEnabled", enabled);
}

// Flush to storage
void AppSettings::sync() {
    settings_->sync();
}

QString AppSettings::settingsFilePath() const {
    // QSettings::fileName() returns the underlying INI path for
    // IniFormat instances and the registry key path on Windows
    // for NativeFormat. For the test seam, callers care that the
    // env-var-driven path is in use; a simple way to expose that
    // unambiguously is to report fileName() directly. Tests that
    // want to confirm the env-var override is active match this
    // against the value they passed in PDFE_SETTINGS_PATH.
    return settings_->fileName();
}

} // namespace pdfe::app
