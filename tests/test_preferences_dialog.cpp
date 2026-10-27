// -*- coding: utf-8 -*-
/**
 * @file test_preferences_dialog.cpp
 * @brief Unit tests for PreferencesDialog class.
 */

#include <gtest/gtest.h>
#include <QApplication>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPushButton>
#include <memory>
#include <optional>

#include "app/AppSettings.h"
#include "ui/PreferencesDialog.h"
#include "core/JpxSecurityGate.h"
#include "core/Jbig2SecurityGate.h"
#include "support/SettingsIsolation.h"
#include <QSettings>

// F33: this fixture constructs no QSettings of its own, which is exactly why
// no grep over test files could ever have found it — but its tests
// default-construct pdfe::app::AppSettings and WRITE through it
// (setMaxRecentFiles, setDefaultZoom, setLogLevel, ...), so before this scope
// existed every run overwrote a real user's preferences in
// HKCU\Software\PDFE\PDFE. F28 named this shape ("the victim set is larger
// than the perpetrator set") without being able to fix it. Scoping the
// identity fixes it with no change to any assertion.
class PreferencesDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            GTEST_SKIP() << "QApplication not available";
        }
        scope_.emplace(QStringLiteral("PDFE-Test"),
                       QStringLiteral("PreferencesDialogTest"),
                       /*purgeOnExit=*/true);
    }

    void TearDown() override {
        dialog.reset();
        scope_.reset();
    }

    std::optional<pdfe_test::SettingsScope> scope_;
    std::unique_ptr<pdfe::ui::PreferencesDialog> dialog;
};


// Test 1: LoadSettings - dialog widgets reflect AppSettings values
TEST_F(PreferencesDialogTest, LoadSettings) {
    pdfe::app::AppSettings settings;

    // Set specific values
    settings.setMaxRecentFiles(7);
    settings.setDefaultZoom(2.5f);
    settings.setTileCacheSizeMB(1024);
    settings.setLogLevel(1); // Info
    settings.setLogToFile(true);
    settings.setLogFilePath("C:/logs/pdfe.log");
    settings.sync();

    dialog = std::make_unique<pdfe::ui::PreferencesDialog>(&settings, nullptr);

    // Verify widgets reflect the settings
    auto* maxRecent = dialog->findChild<QSpinBox*>("maxRecentFilesSpinBox");
    auto* defaultZoom = dialog->findChild<QDoubleSpinBox*>("defaultZoomSpinBox");
    auto* tileCache = dialog->findChild<QSpinBox*>("tileCacheSpinBox");
    auto* logLevel = dialog->findChild<QComboBox*>("logLevelCombo");
    auto* logToFile = dialog->findChild<QCheckBox*>("logToFileCheck");
    auto* logFilePath = dialog->findChild<QLineEdit*>("logFilePathEdit");

    ASSERT_NE(maxRecent, nullptr);
    ASSERT_NE(defaultZoom, nullptr);
    ASSERT_NE(tileCache, nullptr);
    ASSERT_NE(logLevel, nullptr);
    ASSERT_NE(logToFile, nullptr);
    ASSERT_NE(logFilePath, nullptr);

    EXPECT_EQ(maxRecent->value(), 7);
    EXPECT_DOUBLE_EQ(defaultZoom->value(), 2.5);
    EXPECT_EQ(tileCache->value(), 1024);
    EXPECT_EQ(logLevel->currentIndex(), 1);
    EXPECT_TRUE(logToFile->isChecked());
    EXPECT_EQ(logFilePath->text(), "C:/logs/pdfe.log");
}


// Test 2: AcceptPersists - accepting the dialog writes widget values back to AppSettings
TEST_F(PreferencesDialogTest, AcceptPersists) {
    pdfe::app::AppSettings settings;

    // Start with defaults
    settings.setMaxRecentFiles(10);
    settings.setDefaultZoom(1.0f);
    settings.setTileCacheSizeMB(512);
    settings.setLogLevel(2); // Warning
    settings.setLogToFile(false);
    settings.setLogFilePath("");
    settings.sync();

    dialog = std::make_unique<pdfe::ui::PreferencesDialog>(&settings, nullptr);

    // Change widget values
    auto* maxRecent = dialog->findChild<QSpinBox*>("maxRecentFilesSpinBox");
    auto* defaultZoom = dialog->findChild<QDoubleSpinBox*>("defaultZoomSpinBox");
    auto* tileCache = dialog->findChild<QSpinBox*>("tileCacheSpinBox");
    auto* logLevel = dialog->findChild<QComboBox*>("logLevelCombo");
    auto* logToFile = dialog->findChild<QCheckBox*>("logToFileCheck");
    auto* logFilePath = dialog->findChild<QLineEdit*>("logFilePathEdit");

    ASSERT_NE(maxRecent, nullptr);
    ASSERT_NE(defaultZoom, nullptr);
    ASSERT_NE(tileCache, nullptr);
    ASSERT_NE(logLevel, nullptr);
    ASSERT_NE(logToFile, nullptr);
    ASSERT_NE(logFilePath, nullptr);

    maxRecent->setValue(5);
    defaultZoom->setValue(3.0);
    tileCache->setValue(2048);
    logLevel->setCurrentIndex(0); // Debug
    logToFile->setChecked(true);
    logFilePath->setText("D:/test.log");

    // Accept via button box (accept() is private, so trigger via the OK button)
    auto* buttonBox = dialog->findChild<QDialogButtonBox*>("preferencesButtonBox");
    ASSERT_NE(buttonBox, nullptr);
    buttonBox->button(QDialogButtonBox::Ok)->click();

    // Verify AppSettings has the new values
    EXPECT_EQ(settings.maxRecentFiles(), 5);
    EXPECT_FLOAT_EQ(settings.defaultZoom(), 3.0f);
    EXPECT_EQ(settings.tileCacheSizeMB(), 2048);
    EXPECT_EQ(settings.logLevel(), 0);
    EXPECT_TRUE(settings.logToFile());
    EXPECT_EQ(settings.logFilePath(), "D:/test.log");
}


// Test 3: CancelDoesNotPersist - rejecting the dialog does not change AppSettings
TEST_F(PreferencesDialogTest, CancelDoesNotPersist) {
    pdfe::app::AppSettings settings;

    // Set known values
    settings.setMaxRecentFiles(10);
    settings.setDefaultZoom(1.0f);
    settings.setTileCacheSizeMB(512);
    settings.setLogLevel(2);
    settings.setLogToFile(false);
    settings.setLogFilePath("");
    settings.sync();

    dialog = std::make_unique<pdfe::ui::PreferencesDialog>(&settings, nullptr);

    // Change widget values
    auto* maxRecent = dialog->findChild<QSpinBox*>("maxRecentFilesSpinBox");
    auto* defaultZoom = dialog->findChild<QDoubleSpinBox*>("defaultZoomSpinBox");
    auto* tileCache = dialog->findChild<QSpinBox*>("tileCacheSpinBox");
    auto* logLevel = dialog->findChild<QComboBox*>("logLevelCombo");
    auto* logToFile = dialog->findChild<QCheckBox*>("logToFileCheck");
    auto* logFilePath = dialog->findChild<QLineEdit*>("logFilePathEdit");

    ASSERT_NE(maxRecent, nullptr);
    ASSERT_NE(defaultZoom, nullptr);
    ASSERT_NE(tileCache, nullptr);
    ASSERT_NE(logLevel, nullptr);
    ASSERT_NE(logToFile, nullptr);
    ASSERT_NE(logFilePath, nullptr);

    maxRecent->setValue(3);
    defaultZoom->setValue(4.0);
    tileCache->setValue(4096);
    logLevel->setCurrentIndex(3); // Critical
    logToFile->setChecked(true);
    logFilePath->setText("Z:/reject.log");

    // Reject (cancel) -- should NOT persist
    dialog->reject();

    // Verify AppSettings are unchanged
    EXPECT_EQ(settings.maxRecentFiles(), 10);
    EXPECT_FLOAT_EQ(settings.defaultZoom(), 1.0f);
    EXPECT_EQ(settings.tileCacheSizeMB(), 512);
    EXPECT_EQ(settings.logLevel(), 2);
    EXPECT_FALSE(settings.logToFile());
    EXPECT_EQ(settings.logFilePath(), "");
}


// ── #50: JPEG2000 preference + warn-once-on-enable ───────────────────────────

namespace {
// Test double: overrides the modal risk confirmation so headless runs don't wedge.
class JpxPrefDialog : public pdfe::ui::PreferencesDialog {
public:
    JpxPrefDialog(pdfe::app::AppSettings* s, QWidget* p = nullptr)
        : pdfe::ui::PreferencesDialog(s, p) {}
    bool confirmResult = true;  // what the (mocked) warning dialog returns
    int  confirmCalls = 0;      // how many times the warning was shown
protected:
    bool confirmEnableJpxRisk() override { ++confirmCalls; return confirmResult; }
};
}  // namespace

// F33: the remove()s below used to name HKCU\Software\PDFE\PDFE — the SHIPPED
// application's hive — so this fixture deleted a real user's JPX security
// preference. It is the narrowest of the five sites F28 recorded (one key, not
// a whole-hive clear), but it is the same class. The SettingsScope moves the
// identity into a purged PDFE-Test hive that the dialog's AppSettings follows.
//
// NOTE the ordering: the scope is created AFTER the qApp check, because
// GTEST_SKIP() returns from SetUp immediately and a scope constructed above it
// would still be destroyed correctly but would have pointlessly switched the
// identity for a test that never runs. TearDown still executes after a skip,
// hence the std::optional guard rather than a bare member.
class PreferencesDialogJpxTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) GTEST_SKIP() << "QApplication not available";
        scope_.emplace(QStringLiteral("PDFE-Test"),
                       QStringLiteral("PreferencesDialogJpxTest"),
                       /*purgeOnExit=*/true);
        QSettings().remove("Security/jpxRenderingEnabled");
        QSettings().sync();
        pdfe::core::setJpxDecodingEnabled(false);
    }
    void TearDown() override {
        pdfe::core::setJpxDecodingEnabled(false);
        if (scope_) {
            QSettings().remove("Security/jpxRenderingEnabled");
            QSettings().sync();
        }
        dialog.reset();
        scope_.reset();
    }
    void clickOk() {
        auto* box = dialog->findChild<QDialogButtonBox*>("preferencesButtonBox");
        ASSERT_NE(box, nullptr);
        box->button(QDialogButtonBox::Ok)->click();
    }
    std::optional<pdfe_test::SettingsScope> scope_;
    std::unique_ptr<JpxPrefDialog> dialog;
};

// Default OFF: a fresh settings store leaves the checkbox unchecked.
TEST_F(PreferencesDialogJpxTest, DefaultOffUnchecked) {
    pdfe::app::AppSettings settings;
    dialog = std::make_unique<JpxPrefDialog>(&settings, nullptr);
    auto* cb = dialog->findChild<QCheckBox*>("jpxRenderingEnabledCheck");
    ASSERT_NE(cb, nullptr) << "Security tab must expose jpxRenderingEnabledCheck";
    EXPECT_FALSE(cb->isChecked());
    EXPECT_FALSE(settings.jpxRenderingEnabled());
}

// OFF->ON accepted: warns once, persists ON, applies to the decode gate.
TEST_F(PreferencesDialogJpxTest, EnableAcceptedWarnsOnceAndApplies) {
    pdfe::app::AppSettings settings;
    settings.setJpxRenderingEnabled(false);
    settings.sync();
    dialog = std::make_unique<JpxPrefDialog>(&settings, nullptr);
    dialog->confirmResult = true;

    auto* cb = dialog->findChild<QCheckBox*>("jpxRenderingEnabledCheck");
    ASSERT_NE(cb, nullptr);
    cb->setChecked(true);
    clickOk();

    EXPECT_EQ(dialog->confirmCalls, 1) << "must warn exactly once on OFF->ON";
    EXPECT_TRUE(settings.jpxRenderingEnabled());
    EXPECT_TRUE(pdfe::core::isJpxDecodingEnabled())
        << "accepting must apply the setting to the MuPDF decode gate";
}

// OFF->ON declined: warns once, stays OFF, gate stays disabled, checkbox reverts.
TEST_F(PreferencesDialogJpxTest, EnableDeclinedStaysOff) {
    pdfe::app::AppSettings settings;
    settings.setJpxRenderingEnabled(false);
    settings.sync();
    dialog = std::make_unique<JpxPrefDialog>(&settings, nullptr);
    dialog->confirmResult = false;  // user clicks No on the risk warning

    auto* cb = dialog->findChild<QCheckBox*>("jpxRenderingEnabledCheck");
    ASSERT_NE(cb, nullptr);
    cb->setChecked(true);
    clickOk();

    EXPECT_EQ(dialog->confirmCalls, 1) << "must warn exactly once on the attempted enable";
    EXPECT_FALSE(settings.jpxRenderingEnabled()) << "declining must keep it OFF";
    EXPECT_FALSE(pdfe::core::isJpxDecodingEnabled());
    EXPECT_FALSE(cb->isChecked()) << "declined checkbox must revert to unchecked";
}

// Already ON: accepting without a transition does NOT re-warn.
TEST_F(PreferencesDialogJpxTest, AlreadyOnNoWarnOnAccept) {
    pdfe::app::AppSettings settings;
    settings.setJpxRenderingEnabled(true);
    settings.sync();
    dialog = std::make_unique<JpxPrefDialog>(&settings, nullptr);

    auto* cb = dialog->findChild<QCheckBox*>("jpxRenderingEnabledCheck");
    ASSERT_NE(cb, nullptr);
    EXPECT_TRUE(cb->isChecked()) << "loadSettings must reflect the persisted ON state";
    clickOk();  // no OFF->ON transition

    EXPECT_EQ(dialog->confirmCalls, 0) << "no warning when the value did not turn on";
    EXPECT_TRUE(settings.jpxRenderingEnabled());
}

// Turning OFF never warns.
TEST_F(PreferencesDialogJpxTest, TurningOffNoWarn) {
    pdfe::app::AppSettings settings;
    settings.setJpxRenderingEnabled(true);
    settings.sync();
    dialog = std::make_unique<JpxPrefDialog>(&settings, nullptr);

    auto* cb = dialog->findChild<QCheckBox*>("jpxRenderingEnabledCheck");
    ASSERT_NE(cb, nullptr);
    cb->setChecked(false);
    clickOk();

    EXPECT_EQ(dialog->confirmCalls, 0) << "disabling must not warn";
    EXPECT_FALSE(settings.jpxRenderingEnabled());
    EXPECT_FALSE(pdfe::core::isJpxDecodingEnabled());
}

// ── SEC-56 / GitHub #56: the JBIG2 opt-in, same warn-once contract ──────────

namespace {
class Jbig2PrefDialog : public pdfe::ui::PreferencesDialog {
public:
    Jbig2PrefDialog(pdfe::app::AppSettings* s, QWidget* p = nullptr)
        : pdfe::ui::PreferencesDialog(s, p) {}
    bool confirmResult = true;
    int  confirmCalls = 0;
    int  jpxConfirmCalls = 0;  // must stay 0: the two prefs are independent
protected:
    bool confirmEnableJbig2Risk() override { ++confirmCalls; return confirmResult; }
    bool confirmEnableJpxRisk() override { ++jpxConfirmCalls; return true; }
};
}  // namespace

class PreferencesDialogJbig2Test : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) GTEST_SKIP() << "QApplication not available";
        scope_.emplace(QStringLiteral("PDFE-Test"),
                       QStringLiteral("PreferencesDialogJbig2Test"),
                       /*purgeOnExit=*/true);
        QSettings().remove("Security/jbig2RenderingEnabled");
        QSettings().sync();
        pdfe::core::setJbig2DecodingEnabled(false);
    }
    void TearDown() override {
        pdfe::core::setJbig2DecodingEnabled(false);
        if (scope_) {
            QSettings().remove("Security/jbig2RenderingEnabled");
            QSettings().sync();
        }
        dialog.reset();
        scope_.reset();
    }
    void clickOk() {
        auto* box = dialog->findChild<QDialogButtonBox*>("preferencesButtonBox");
        ASSERT_NE(box, nullptr);
        box->button(QDialogButtonBox::Ok)->click();
    }
    std::optional<pdfe_test::SettingsScope> scope_;
    std::unique_ptr<Jbig2PrefDialog> dialog;
};

// Default OFF: a fresh settings store leaves the checkbox unchecked.
TEST_F(PreferencesDialogJbig2Test, DefaultOffUnchecked) {
    pdfe::app::AppSettings settings;
    dialog = std::make_unique<Jbig2PrefDialog>(&settings, nullptr);
    auto* cb = dialog->findChild<QCheckBox*>("jbig2RenderingEnabledCheck");
    ASSERT_NE(cb, nullptr) << "Security tab must expose jbig2RenderingEnabledCheck";
    EXPECT_FALSE(cb->isChecked());
    EXPECT_FALSE(settings.jbig2RenderingEnabled());
}

// OFF->ON accepted: warns once, persists ON, applies to the decode gate — and
// does NOT touch the JPEG2000 preference or its warning.
TEST_F(PreferencesDialogJbig2Test, EnableAcceptedWarnsOnceAndApplies) {
    pdfe::app::AppSettings settings;
    settings.setJbig2RenderingEnabled(false);
    settings.setJpxRenderingEnabled(false);
    settings.sync();
    dialog = std::make_unique<Jbig2PrefDialog>(&settings, nullptr);
    dialog->confirmResult = true;

    auto* cb = dialog->findChild<QCheckBox*>("jbig2RenderingEnabledCheck");
    ASSERT_NE(cb, nullptr);
    cb->setChecked(true);
    clickOk();

    EXPECT_EQ(dialog->confirmCalls, 1) << "must warn exactly once on OFF->ON";
    EXPECT_TRUE(settings.jbig2RenderingEnabled());
    EXPECT_TRUE(pdfe::core::isJbig2DecodingEnabled())
        << "accepting must apply the setting to the MuPDF decode gate";
    EXPECT_EQ(dialog->jpxConfirmCalls, 0)
        << "enabling JBIG2 must not run the JPEG2000 confirmation";
    EXPECT_FALSE(pdfe::core::isJpxDecodingEnabled())
        << "enabling JBIG2 must not enable the JPEG2000 decoder";
}

// OFF->ON declined: warns once, stays OFF, gate stays disabled, checkbox reverts.
TEST_F(PreferencesDialogJbig2Test, EnableDeclinedStaysOff) {
    pdfe::app::AppSettings settings;
    settings.setJbig2RenderingEnabled(false);
    settings.sync();
    dialog = std::make_unique<Jbig2PrefDialog>(&settings, nullptr);
    dialog->confirmResult = false;  // user clicks No on the risk warning

    auto* cb = dialog->findChild<QCheckBox*>("jbig2RenderingEnabledCheck");
    ASSERT_NE(cb, nullptr);
    cb->setChecked(true);
    clickOk();

    EXPECT_EQ(dialog->confirmCalls, 1) << "must warn exactly once on the attempted enable";
    EXPECT_FALSE(settings.jbig2RenderingEnabled()) << "declining must keep it OFF";
    EXPECT_FALSE(pdfe::core::isJbig2DecodingEnabled());
    EXPECT_FALSE(cb->isChecked()) << "declined checkbox must revert to unchecked";
}

// Already ON: accepting without a transition does NOT re-warn.
TEST_F(PreferencesDialogJbig2Test, AlreadyOnNoWarnOnAccept) {
    pdfe::app::AppSettings settings;
    settings.setJbig2RenderingEnabled(true);
    settings.sync();
    dialog = std::make_unique<Jbig2PrefDialog>(&settings, nullptr);

    auto* cb = dialog->findChild<QCheckBox*>("jbig2RenderingEnabledCheck");
    ASSERT_NE(cb, nullptr);
    EXPECT_TRUE(cb->isChecked()) << "loadSettings must reflect the persisted ON state";
    clickOk();  // no OFF->ON transition

    EXPECT_EQ(dialog->confirmCalls, 0) << "no warning when the value did not turn on";
    EXPECT_TRUE(settings.jbig2RenderingEnabled());
}

// Turning OFF never warns.
TEST_F(PreferencesDialogJbig2Test, TurningOffNoWarn) {
    pdfe::app::AppSettings settings;
    settings.setJbig2RenderingEnabled(true);
    settings.sync();
    dialog = std::make_unique<Jbig2PrefDialog>(&settings, nullptr);

    auto* cb = dialog->findChild<QCheckBox*>("jbig2RenderingEnabledCheck");
    ASSERT_NE(cb, nullptr);
    cb->setChecked(false);
    clickOk();

    EXPECT_EQ(dialog->confirmCalls, 0) << "disabling must not warn";
    EXPECT_FALSE(settings.jbig2RenderingEnabled());
    EXPECT_FALSE(pdfe::core::isJbig2DecodingEnabled());
}
