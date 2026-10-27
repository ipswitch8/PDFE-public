#pragma once
#include <QSettings>
#include <QStringList>
#include <QByteArray>
#include <QString>

#include <memory>

namespace pdfe::app {

class AppSettings {
public:
    // Default constructor uses QApplication org/app name, OR a per-
    // process override if the ``PDFE_SETTINGS_PATH`` environment
    // variable is set to an absolute INI file path. The env-var
    // path is consumed at construction time only — changing it
    // after the AppSettings instance is built has no effect.
    //
    // The env-var seam is intended for test isolation: pywinauto/
    // WinAppDriver tests set ``PDFE_SETTINGS_PATH`` to a fresh
    // temporary INI per test so cross-process recent-files
    // verification is deterministic (Pipeline H phase-3).
    explicit AppSettings();
    ~AppSettings();

    // Recent Files
    QStringList recentFiles() const;
    void addRecentFile(const QString& filePath);
    void removeRecentFile(const QString& filePath);
    void clearRecentFiles();
    int maxRecentFiles() const;
    void setMaxRecentFiles(int count);

    // Window State
    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray& geometry);
    QByteArray windowState() const;
    void setWindowState(const QByteArray& state);

    // Panel Visibility
    bool pagePanelVisible() const;
    void setPagePanelVisible(bool visible);
    bool propertiesPanelVisible() const;
    void setPropertiesPanelVisible(bool visible);
    bool searchPanelVisible() const;
    void setSearchPanelVisible(bool visible);
    bool bookmarkPanelVisible() const;
    void setBookmarkPanelVisible(bool visible);

    // ── GitHub #240 — network locations ──────────────────────────────────
    //
    // Two separate questions, deliberately not one switch:

    /// May a USER open or save a document on a network location?
    ///
    /// Default TRUE. A person choosing a file on their own file server is an
    /// ordinary act, and refusing it is what GitHub #240 was filed about. The
    /// reachability probe still applies, so an unresponsive share is reported as
    /// unresponsive rather than hanging the window. Set FALSE to restore the
    /// previous local-files-only behaviour.
    bool allowUserNetworkPaths() const;
    void setAllowUserNetworkPaths(bool allowed);

    /// May a document OPENED FROM a network share reach other resources ON THAT
    /// SAME share?
    ///
    /// Default FALSE, and that default is the security-relevant one. A path a
    /// DOCUMENT names is the SMB / NetBIOS egress vector GitHub #91 (WIN-60)
    /// closed: the document chooses the host, and reaching it leaks the fact
    /// that this machine opened this file, before any content is fetched.
    ///
    /// Enabling it is narrower than "allow network access": the resource must be
    /// on the same `\server\share` as the document itself
    /// (path_security::same_network_share). A PDF on `\financeeports`
    /// reaching `\financeeports\logo.png` is the case this serves; the
    /// same PDF reaching `\dc01\sysvol` or `\attacker\payload` stays
    /// refused with the setting ON.
    bool allowDocumentSameShareResources() const;
    void setAllowDocumentSameShareResources(bool allowed);

    // User Preferences
    float defaultZoom() const;
    void setDefaultZoom(float zoom);
    QString lastOpenDirectory() const;
    void setLastOpenDirectory(const QString& dir);
    int tileCacheSizeMB() const;
    void setTileCacheSizeMB(int mb);

    // Author attribution for dynamic stamps, comments, headers/footers.
    // Default falls back to the OS user login name (USERNAME / USER env).
    QString author() const;
    void setAuthor(const QString& author);

    // Phase 6: toggle between the legacy MuPDF substring scan and the new
    // Tantivy-backed full-text index. Default true (use Tantivy).
    bool useTantivySearch() const;
    void setUseTantivySearch(bool enabled);

    // OCR language code (Tesseract trained-data short name — "eng",
    // "fra", "deu", "eng+fra", etc). Default "eng".
    QString ocrLanguage() const;
    void setOcrLanguage(const QString& lang);

    // Light / Dark / System palette choice, persisted as the strings
    // "light" / "dark" / "system" under "Preferences/theme". Default
    // "system".
    //
    // Meta-audit follow-up F33. ThemeManager used to read and write this
    // key through a literal ``QSettings("PDFE", "PDFE")``, which pins the
    // hive identity in the source instead of deriving it from
    // QCoreApplication::organizationName()/applicationName(). That is the
    // one shape in src/ that a test cannot redirect: SettingsScope moves
    // the process-global identity and PDFE_SETTINGS_PATH moves
    // AppSettings, and a literal pair ignores both. Routing theme
    // persistence through AppSettings — which ThemeManager was already
    // handed and which its own header already claimed it used — puts it
    // behind both seams with no new mechanism. The key and its value
    // encoding are unchanged, so an existing installation reads back
    // exactly what it wrote.
    QString theme() const;
    void setTheme(const QString& theme);

    // Logging
    int logLevel() const;       // 0=Debug, 1=Info, 2=Warning, 3=Critical
    void setLogLevel(int level);
    bool logToFile() const;
    void setLogToFile(bool enabled);
    QString logFilePath() const;
    void setLogFilePath(const QString& path);

    // Opt-in version check (GitHub #127). DEFAULT FALSE: a stock install makes
    // no releases-feed request at all. When enabled, PDFE fetches the latest
    // published version once at startup and NOTIFIES; it never downloads or
    // executes anything. See src/app/UpdateChecker.h and UC-9 in
    // docs/code-signing.md for why that boundary is what keeps this out of the
    // update-channel requirements UC-1..UC-8.
    bool updateCheckEnabled() const;
    void setUpdateCheckEnabled(bool enabled);

    // Security / "active content" gates. Default OFF for the
    // categories that can let an attacker-controlled PDF cause
    // network resolution, process launch, or data exfiltration.
    // The Settings dialog (or a per-document banner) is the
    // intended UX for opting back in.
    bool javaScriptEnabled() const;
    void setJavaScriptEnabled(bool enabled);
    // Gates /URI link actions (MainWindow::followLinkAt, fails closed).
    //
    // SCOPE, stated precisely — this comment previously read
    // "/Launch, /URI, /SubmitForm, /ImportData", which overstated it in the
    // same way SEC-49 described (a setting claiming to control more than it
    // does). What is actually true:
    //   * /URI      — GATED here.
    //   * /Launch   — NOT gated by this flag, deliberately: it already carries
    //                 an unconditional per-click prompt, which is a stronger
    //                 control than a persisted allow-flag.
    //   * /SubmitForm — GATED here as of GitHub #18 (pipeline p83-outstanding,
    //                 phase-2). src/formsubmit/ is no longer unreachable:
    //                 pdfe_ui links pdfe_formsubmit and
    //                 MainWindow::submitCurrentFormTo() is its one UI call
    //                 site. That function checks externalActionsAllowed()
    //                 FIRST, before it reads a single field, so with the gate
    //                 closed (the default) the submit path observes nothing
    //                 and sends nothing. A host the user has not confirmed in
    //                 this session raises a prompt on top of the gate.
    //   * /ImportData — NOT gated, and NOT reachable: LinkActionType still has
    //                 no ImportData variant. If it is ever wired to the UI, it
    //                 must be gated here and this comment updated in the same
    //                 change.
    bool externalActionsAllowed() const;
    void setExternalActionsAllowed(bool enabled);

    // JPEG2000 (JPX) image rendering. Default OFF (#50): malformed JPX can
    // heap-overflow the bundled OpenJPEG (potential RCE) and MuPDF's fz_try/fz_catch
    // cannot trap heap corruption, so decoding is gated off at the MuPDF level and
    // only enabled when the user explicitly opts in (with a warn-once confirmation).
    bool jpxRenderingEnabled() const;
    void setJpxRenderingEnabled(bool enabled);

    // JBIG2 image rendering. Default OFF (SEC-56 / GitHub #56): jbig2dec has a
    // long history of memory-safety CVEs in its generic-region and symbol
    // decoders, and MuPDF's fz_try/fz_catch cannot trap heap corruption, so
    // decoding is gated off at the MuPDF level (the /JBIG2Decode filter is
    // replaced by a blank placeholder stream) and only enabled when the user
    // explicitly opts in (with a warn-once confirmation). Mirrors the JPX gate.
    bool jbig2RenderingEnabled() const;
    void setJbig2RenderingEnabled(bool enabled);

    // Flush to storage
    void sync();

    // Test-visible inspector: returns the absolute INI file path
    // when the env-var override is in use, or an empty string when
    // the default registry/INI location is in use. Tests assert on
    // this to confirm PDFE_SETTINGS_PATH took effect.
    QString settingsFilePath() const;

private:
    // Heap-allocated so the constructor body can pick between the
    // default (org/app name) and env-var (path + IniFormat) ctors.
    // The QSettings type itself is non-copyable; std::unique_ptr
    // is the simplest way to defer construction to the body.
    std::unique_ptr<QSettings> settings_;

    static constexpr int DEFAULT_MAX_RECENT = 10;
    static constexpr float DEFAULT_ZOOM = 1.0f;
    static constexpr int DEFAULT_CACHE_SIZE_MB = 512;
    static constexpr int DEFAULT_LOG_LEVEL = 2; // Warning
};

} // namespace pdfe::app
