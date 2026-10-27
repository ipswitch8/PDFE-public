// PHASE1_HARDENING: skip=0 expect_ge=0 no_throw=1  (2026-05-08)
#include <gtest/gtest.h>
#include "app/AppSettings.h"
#include "core/JpxSecurityGate.h"
#include "core/Jbig2SecurityGate.h"
#include "ui/TestDialogSeam.h"
#include "support/SettingsIsolation.h"
#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QDir>
#include <QTemporaryDir>

#include <cstdlib>
#include <optional>

using namespace pdfe::app;

// F33: these clears used to name HKCU\Software\PDFE\PDFE — the hive the
// SHIPPED application uses — so running pdfe_tests.exe on a machine with PDFE
// installed erased the user's real recent-file list, last-open directory and
// preferences. F28 measured exactly that and carried it as a finding. A
// SettingsScope moves the fixture's identity into a PDFE-Test hive that
// purgeOnExit deletes; AppSettings default-constructs a bare QSettings, so it
// follows. The clears are kept for per-test isolation but now target the
// scoped hive.
//
// The PDFE_SETTINGS_PATH tests further down this file are unaffected: that
// seam bypasses the org/app identity entirely by construction, which is the
// property they assert.
class AppSettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        scope_.emplace(QStringLiteral("PDFE-Test"),
                       QStringLiteral("AppSettingsTest"),
                       /*purgeOnExit=*/true);
        // Clear registry/settings before each test for isolation
        QSettings().clear();
        QSettings().sync();
    }

    void TearDown() override {
        // Clear registry/settings after each test
        QSettings().clear();
        QSettings().sync();
        scope_.reset();
    }

    std::optional<pdfe_test::SettingsScope> scope_;
};

TEST_F(AppSettingsTest, Construct) {
    // Construction must not throw AND must produce a usable instance
    // (a default-constructed AppSettings should answer queries cleanly).
    AppSettings settings;
    EXPECT_TRUE(settings.recentFiles().isEmpty())
        << "default AppSettings should have an empty recent-files list";
    EXPECT_GE(settings.maxRecentFiles(), 1)
        << "default maxRecentFiles must be a positive cap";
}

TEST_F(AppSettingsTest, DefaultRecentFiles_Empty) {
    AppSettings settings;

    QStringList recentFiles = settings.recentFiles();
    EXPECT_TRUE(recentFiles.isEmpty());
}

TEST_F(AppSettingsTest, AddRecentFile_AppearsFirst) {
    AppSettings settings;

    QString testFile = "C:/test/document.pdf";
    settings.addRecentFile(testFile);

    QStringList recentFiles = settings.recentFiles();
    ASSERT_FALSE(recentFiles.isEmpty());
    EXPECT_EQ(recentFiles.first(), testFile);
}

TEST_F(AppSettingsTest, AddRecentFile_Deduplicates) {
    AppSettings settings;

    QString testFile = "C:/test/document.pdf";
    settings.addRecentFile(testFile);
    settings.addRecentFile(testFile);

    QStringList recentFiles = settings.recentFiles();
    EXPECT_EQ(recentFiles.size(), 1);
    EXPECT_EQ(recentFiles.first(), testFile);
}

TEST_F(AppSettingsTest, ClearRecentFiles) {
    AppSettings settings;

    settings.addRecentFile("C:/test/doc1.pdf");
    settings.addRecentFile("C:/test/doc2.pdf");
    settings.addRecentFile("C:/test/doc3.pdf");

    EXPECT_FALSE(settings.recentFiles().isEmpty());

    settings.clearRecentFiles();

    QStringList recentFiles = settings.recentFiles();
    EXPECT_TRUE(recentFiles.isEmpty());
}

TEST_F(AppSettingsTest, MaxRecentFiles_Trims) {
    AppSettings settings;

    // Set max to 3
    settings.setMaxRecentFiles(3);
    EXPECT_EQ(settings.maxRecentFiles(), 3);

    // Add 5 files
    settings.addRecentFile("C:/test/doc1.pdf");
    settings.addRecentFile("C:/test/doc2.pdf");
    settings.addRecentFile("C:/test/doc3.pdf");
    settings.addRecentFile("C:/test/doc4.pdf");
    settings.addRecentFile("C:/test/doc5.pdf");

    // Verify only 3 remain
    QStringList recentFiles = settings.recentFiles();
    EXPECT_EQ(recentFiles.size(), 3);

    // Verify the 3 most recent are kept (doc5, doc4, doc3)
    EXPECT_EQ(recentFiles[0], "C:/test/doc5.pdf");
    EXPECT_EQ(recentFiles[1], "C:/test/doc4.pdf");
    EXPECT_EQ(recentFiles[2], "C:/test/doc3.pdf");
}

TEST_F(AppSettingsTest, WindowGeometry_Roundtrip) {
    AppSettings settings;

    // Create test byte array
    QByteArray testGeometry;
    testGeometry.append("GeometryData123");

    settings.setWindowGeometry(testGeometry);
    QByteArray retrieved = settings.windowGeometry();

    EXPECT_EQ(retrieved, testGeometry);
}

TEST_F(AppSettingsTest, PanelVisibility_Defaults) {
    AppSettings settings;

    // Check defaults: page=true, properties=true, search=false, bookmark=false
    EXPECT_TRUE(settings.pagePanelVisible());
    EXPECT_TRUE(settings.propertiesPanelVisible());
    EXPECT_FALSE(settings.searchPanelVisible());
    EXPECT_FALSE(settings.bookmarkPanelVisible());
}

TEST_F(AppSettingsTest, PanelVisibility_SetGet) {
    AppSettings settings;

    // Set all to opposite of defaults
    settings.setPagePanelVisible(false);
    settings.setPropertiesPanelVisible(false);
    settings.setSearchPanelVisible(true);
    settings.setBookmarkPanelVisible(true);

    // Verify
    EXPECT_FALSE(settings.pagePanelVisible());
    EXPECT_FALSE(settings.propertiesPanelVisible());
    EXPECT_TRUE(settings.searchPanelVisible());
    EXPECT_TRUE(settings.bookmarkPanelVisible());
}

TEST_F(AppSettingsTest, DefaultZoom_Default) {
    AppSettings settings;

    float defaultZoom = settings.defaultZoom();
    EXPECT_FLOAT_EQ(defaultZoom, 1.0f);
}

TEST_F(AppSettingsTest, TileCacheSizeMB_Default) {
    AppSettings settings;

    int cacheSize = settings.tileCacheSizeMB();
    EXPECT_EQ(cacheSize, 512);
}

TEST_F(AppSettingsTest, LogSettings_Defaults) {
    AppSettings settings;

    // Check defaults: level=2 (Warning), toFile=false
    EXPECT_EQ(settings.logLevel(), 2);
    EXPECT_FALSE(settings.logToFile());

    // GitHub #151 / DCA 0072 — THE DEFAULT ITSELF, pinned here because nothing
    // else pins it.
    //
    // This assertion exists because the phase-4 claim-check gate noticed the
    // gap: every other test of the empty-path behaviour
    // (LoggingPreferencesTest.EmptyConfiguredPathFallsBackToAppLocalData, and
    // both LaunchedAppLogPathTest cases) calls setLogFilePath() FIRST, so each
    // overwrites the real default before reading it. They prove what an empty
    // value MEANS; none proved that a fresh install actually gets one.
    //
    // The default used to be QDir::temp().filePath("pdfe.log"). That is a
    // privacy downgrade — %TEMP% is shared on a multi-user machine, whereas
    // LogHandler's own default (AppLocalDataLocation) is per-user — and it was
    // harmless only for as long as nothing read the getter. Now that
    // applyLoggingPreferences() honours it, a revert to any non-empty literal
    // silently relocates every default install's log. Empty means "let
    // LogHandler choose", and only an explicit user choice moves it.
    //
    // That is the same defect class as the one this phase fixed, one layer
    // down: a value written into the code and verified by nothing.
    EXPECT_TRUE(settings.logFilePath().isEmpty())
        << "a fresh AppSettings must not name a log path -- empty delegates to "
           "LogHandler's per-user default. A non-empty default here relocates "
           "the log for every install that never touched Preferences. Got: "
        << settings.logFilePath().toStdString();
}

// Phase 4f: author setting used by dynamic stamps.
TEST_F(AppSettingsTest, Author_DefaultFallsBackToEnvOrUnknown) {
    AppSettings settings;
    // The default comes from USERNAME/USER env or "Unknown"; it must
    // never be empty. Tests don't assume a specific value — they just
    // ensure the fallback logic kicks in.
    QString a = settings.author();
    EXPECT_FALSE(a.isEmpty());
}

TEST_F(AppSettingsTest, Author_SetGetRoundtrip) {
    AppSettings settings;
    settings.setAuthor("Jane Reviewer");
    EXPECT_EQ(settings.author(), QString("Jane Reviewer"));
}

TEST_F(AppSettingsTest, Author_EmptySetRestoresDefault) {
    AppSettings settings;
    settings.setAuthor("Temp");
    ASSERT_EQ(settings.author(), QString("Temp"));
    // Setting empty string should make author() fall back to env default
    settings.setAuthor("");
    EXPECT_FALSE(settings.author().isEmpty())
        << "Empty author() should fall back to env, not return empty";
}

// =====================================================================
// Pipeline H phase-3: PDFE_SETTINGS_PATH env-var seam + cross-instance
// read-back (proves the sync() flush is effective for the recent-files
// mutators).
// =====================================================================

namespace {

// RAII helper that sets PDFE_SETTINGS_PATH to a tempdir-relative INI
// path for the lifetime of the test, then restores the original env
// var (or unsets it) on destruction. Without this, a failing test
// would leak the env var into subsequent tests in the suite.
class ScopedSettingsPathEnv {
public:
    explicit ScopedSettingsPathEnv(const QString& iniPath) {
        const QByteArray prior = qgetenv("PDFE_SETTINGS_PATH");
        had_prior_ = !prior.isEmpty();
        if (had_prior_) {
            prior_ = QString::fromLocal8Bit(prior);
        }
        qputenv("PDFE_SETTINGS_PATH",
                iniPath.toLocal8Bit());
    }
    ~ScopedSettingsPathEnv() {
        if (had_prior_) {
            qputenv("PDFE_SETTINGS_PATH",
                    prior_.toLocal8Bit());
        } else {
            qunsetenv("PDFE_SETTINGS_PATH");
        }
    }
    ScopedSettingsPathEnv(const ScopedSettingsPathEnv&) = delete;
    ScopedSettingsPathEnv& operator=(const ScopedSettingsPathEnv&) = delete;

private:
    QString prior_;
    bool had_prior_ = false;
};

} // namespace

TEST_F(AppSettingsTest, EnvVar_DirectsWritesToTempIni) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString iniPath = tmp.filePath("pdfe-test.ini");

    ScopedSettingsPathEnv guard(iniPath);

    {
        AppSettings settings;
        // Confirm the seam took effect: settingsFilePath() reports
        // the override path (Qt may normalize separators, so compare
        // the basename for robustness).
        EXPECT_EQ(QFileInfo(settings.settingsFilePath()).fileName(),
                  QString("pdfe-test.ini"));

        settings.addRecentFile("C:/EnvVarTest/doc1.pdf");
    }

    // The INI file must exist on disk (sync() forced the flush) and
    // contain the recent-files key so a separate process could read it.
    ASSERT_TRUE(QFile::exists(iniPath))
        << "PDFE_SETTINGS_PATH INI file was not created at " << iniPath.toStdString();

    QSettings ini(iniPath, QSettings::IniFormat);
    QStringList list = ini.value("RecentFiles/list").toStringList();
    EXPECT_FALSE(list.isEmpty())
        << "Cross-instance read of the env-var INI should see the "
           "addRecentFile() write";
    if (!list.isEmpty()) {
        EXPECT_EQ(list.first(), QString("C:/EnvVarTest/doc1.pdf"));
    }
}

TEST_F(AppSettingsTest, EnvVar_FreshInstanceSeesAddedRecentFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString iniPath = tmp.filePath("pdfe-fresh.ini");

    ScopedSettingsPathEnv guard(iniPath);

    // Instance 1: write.
    {
        AppSettings settings;
        settings.addRecentFile("C:/FreshTest/instance1_doc.pdf");
    }
    // Instance 2: read. This is the proxy for a second PDFE
    // process boot — we don't fork a subprocess in this unit test,
    // but the second AppSettings opens the same INI fresh, and
    // QSettings won't have any in-memory state shared.
    {
        AppSettings settings;
        QStringList recents = settings.recentFiles();
        ASSERT_FALSE(recents.isEmpty())
            << "Fresh AppSettings instance pointed at the same INI "
               "must see writes from the prior instance";
        EXPECT_EQ(recents.first(),
                  QString("C:/FreshTest/instance1_doc.pdf"));
    }
}

TEST_F(AppSettingsTest, EnvVar_FreshInstanceSeesClear) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString iniPath = tmp.filePath("pdfe-clear.ini");

    ScopedSettingsPathEnv guard(iniPath);

    // Instance 1: populate then clear.
    {
        AppSettings settings;
        settings.addRecentFile("C:/ClearTest/doc1.pdf");
        settings.addRecentFile("C:/ClearTest/doc2.pdf");
        ASSERT_FALSE(settings.recentFiles().isEmpty());
        settings.clearRecentFiles();
    }
    // Instance 2: must observe the cleared state.
    {
        AppSettings settings;
        EXPECT_TRUE(settings.recentFiles().isEmpty())
            << "Fresh AppSettings instance must observe "
               "clearRecentFiles() from the prior instance";
    }
}

TEST_F(AppSettingsTest, EnvVar_AbsentRetainsDefaultPath) {
    // When the env var is absent, settingsFilePath() should not
    // be the test override (we can't easily assert the exact
    // default location since it varies per machine, but it must
    // NOT contain a freshly-allocated tempdir name).
    qunsetenv("PDFE_SETTINGS_PATH");
    AppSettings settings;
    QString p = settings.settingsFilePath();
    EXPECT_FALSE(p.contains("pdfe-test.ini"));
    EXPECT_FALSE(p.contains("pdfe-fresh.ini"));
    EXPECT_FALSE(p.contains("pdfe-clear.ini"));
}

TEST_F(AppSettingsTest, RecentFiles_SyncFlushesAfterAdd) {
    // Pipeline H phase-3 race-condition guarantee: addRecentFile
    // must call sync() so a second QSettings instance opening the
    // same INI immediately sees the write — even without an
    // explicit AppSettings::sync() call from the caller.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString iniPath = tmp.filePath("pdfe-sync.ini");

    ScopedSettingsPathEnv guard(iniPath);

    AppSettings settings;
    settings.addRecentFile("C:/SyncTest/sync_after_add.pdf");

    // Open a SEPARATE QSettings handle on the same file — no
    // shared state with the AppSettings above. If sync() didn't
    // run, this read would see an empty list because Qt batches
    // writes to disk on a timer.
    QSettings probe(iniPath, QSettings::IniFormat);
    QStringList list = probe.value("RecentFiles/list").toStringList();
    ASSERT_FALSE(list.isEmpty())
        << "addRecentFile() must sync immediately so cross-instance "
           "reads see the write without waiting for the QSettings "
           "auto-flush timer";
    EXPECT_EQ(list.first(), QString("C:/SyncTest/sync_after_add.pdf"));
}

// ── #50: JPEG2000 (JPX) rendering preference ─────────────────────────────────

TEST_F(AppSettingsTest, JpxRenderingDefaultsOff) {
    AppSettings settings;
    EXPECT_FALSE(settings.jpxRenderingEnabled())
        << "JPEG2000 rendering must default OFF (OpenJPEG heap-overflow / RCE risk)";
}

TEST_F(AppSettingsTest, JpxRenderingRoundTrips) {
    AppSettings settings;
    settings.setJpxRenderingEnabled(true);
    EXPECT_TRUE(settings.jpxRenderingEnabled());
    settings.setJpxRenderingEnabled(false);
    EXPECT_FALSE(settings.jpxRenderingEnabled());
}

// Startup-wiring contract: applying the persisted preference to the MuPDF decode
// gate leaves decoding disabled by default (mirrors MainWindow's ctor wiring).
TEST_F(AppSettingsTest, JpxStartupWiringAppliesDefaultOff) {
    pdfe::core::setJpxDecodingEnabled(true);  // dirty the global to prove the apply
    AppSettings settings;
    pdfe::core::setJpxDecodingEnabled(settings.jpxRenderingEnabled());
    EXPECT_FALSE(pdfe::core::isJpxDecodingEnabled())
        << "startup wiring must apply the default-OFF preference to the decode gate";
    pdfe::core::setJpxDecodingEnabled(false);  // restore
}

// ── SEC-56 / #56: JBIG2 rendering preference ─────────────────────────────────

TEST_F(AppSettingsTest, Jbig2RenderingDefaultsOff) {
    AppSettings settings;
    EXPECT_FALSE(settings.jbig2RenderingEnabled())
        << "JBIG2 rendering must default OFF (jbig2dec memory-safety CVE history)";
}

TEST_F(AppSettingsTest, Jbig2RenderingRoundTrips) {
    AppSettings settings;
    settings.setJbig2RenderingEnabled(true);
    EXPECT_TRUE(settings.jbig2RenderingEnabled());
    settings.setJbig2RenderingEnabled(false);
    EXPECT_FALSE(settings.jbig2RenderingEnabled());
}

// The two decoder preferences must be INDEPENDENT keys. A copy-paste that left
// the JBIG2 accessors reading "Security/jpxRenderingEnabled" would pass both
// tests above and silently make one checkbox drive both decoders.
TEST_F(AppSettingsTest, Jbig2AndJpxPreferencesAreIndependent) {
    AppSettings settings;
    settings.setJbig2RenderingEnabled(true);
    settings.setJpxRenderingEnabled(false);
    EXPECT_TRUE(settings.jbig2RenderingEnabled());
    EXPECT_FALSE(settings.jpxRenderingEnabled())
        << "enabling JBIG2 must not enable JPEG2000 — separate settings keys";
    settings.setJbig2RenderingEnabled(false);
    settings.setJpxRenderingEnabled(true);
    EXPECT_FALSE(settings.jbig2RenderingEnabled())
        << "enabling JPEG2000 must not enable JBIG2 — separate settings keys";
    EXPECT_TRUE(settings.jpxRenderingEnabled());
    settings.setJpxRenderingEnabled(false);
}

// Startup-wiring contract: applying the persisted preference to the MuPDF decode
// gate leaves decoding disabled by default (mirrors MainWindow's ctor wiring).
TEST_F(AppSettingsTest, Jbig2StartupWiringAppliesDefaultOff) {
    pdfe::core::setJbig2DecodingEnabled(true);  // dirty the global to prove the apply
    AppSettings settings;
    pdfe::core::setJbig2DecodingEnabled(settings.jbig2RenderingEnabled());
    EXPECT_FALSE(pdfe::core::isJbig2DecodingEnabled())
        << "startup wiring must apply the default-OFF preference to the decode gate";
    pdfe::core::setJbig2DecodingEnabled(false);  // restore
}

// ---------------------------------------------------------------------------
// SEC-45 / GitHub #55 — the test pipes are a COMPILE-TIME capability, not an
// environment-variable one.
//
// PDFE_TEST_PIPES gates hidden QLineEdit "pipes" that let an external process
// drive the running application: write a QAction objectName and the app
// activates that tool; query a dialog and read back its child inventory. They
// exist because pywinauto's UIA backend cannot enumerate a Qt modal dialog on
// the CI host, and there they are genuinely needed.
//
// The seam used to be gated on the environment variable ALONE. An environment
// variable is set by whoever LAUNCHES the process, not by whoever ships it — so
// in a released build any local process that could set PDFE_TEST_PIPES and start
// PDFE had a control channel into it. `TestDialogSeam.h` described the seam as
// "inert in production ... never shipped", which was true of the variable and
// false of the code.
//
// `kTestPipesCompiledIn` is now the gate, driven by the CMake option
// PDFE_TESTING (default ON; packaging configures with -DPDFE_TESTING=OFF).
// ---------------------------------------------------------------------------
TEST_F(AppSettingsTest, TestPipesAreGatedAtCompileTimeNotByTheEnvironment) {
    const bool hadVar = qEnvironmentVariableIsSet("PDFE_TEST_PIPES");
    const QByteArray saved = hadVar ? qgetenv("PDFE_TEST_PIPES") : QByteArray();

    qputenv("PDFE_TEST_PIPES", "1");

#ifdef PDFE_TESTING
    // A testing-configured build: the capability is compiled in, so the
    // variable is honoured. Asserting this half matters too — if it were false
    // here the UI automation suites would silently lose their seam and report
    // "blocked" rather than a defect.
    EXPECT_TRUE(pdfe::ui::testseam::kTestPipesCompiledIn)
        << "PDFE_TESTING is defined but the seam reports itself absent";
    EXPECT_TRUE(pdfe::ui::testseam::testPipesEnabled())
        << "with PDFE_TESTING on and the env var set, the seam must be active";
#else
    // A RELEASE-configured build — the case the gap row is actually about.
    // The environment variable must have NO effect.
    EXPECT_FALSE(pdfe::ui::testseam::kTestPipesCompiledIn)
        << "PDFE_TESTING is not defined but the seam reports itself compiled in";
    EXPECT_FALSE(pdfe::ui::testseam::testPipesEnabled())
        << "PDFE_TEST_PIPES is set and PDFE_TESTING is OFF, yet the seam "
           "reports enabled — the environment can still switch on an "
           "external control channel in a shipped binary, which is exactly "
           "what this gate exists to prevent";
#endif

    if (hadVar) {
        qputenv("PDFE_TEST_PIPES", saved);
    } else {
        qunsetenv("PDFE_TEST_PIPES");
    }
}
