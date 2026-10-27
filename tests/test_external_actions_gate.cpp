// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F13 (SEC-49): the
// `Security/externalActionsAllowed` preference must GATE SOMETHING.
//
// THE FINDING. Phase-4 established that the setting was DEAD CODE BEHIND A
// LIVE CHECKBOX:
//
//   * defined   — src/app/AppSettings.cpp:266-272 (getter + setter)
//   * declared  — src/app/AppSettings.h:88, commented
//                 "/Launch, /URI, /SubmitForm, /ImportData"
//   * exposed   — src/ui/PreferencesDialog.cpp:113-117, a real checkbox in
//                 Preferences -> Security
//   * consumed  — NOWHERE in src/.
//
// The user was offered a security control that gated nothing. That is the
// worst of the three possible states: worse than the control existing and
// working, and worse than it not existing at all, because it manufactures a
// false belief about the product's behaviour.
//
// THE DECISION: WIRE IT, don't remove it. /URI dispatch in
// MainWindow::followLinkAt now consults the preference and refuses while it is
// off (default). /Launch is deliberately left ungated because it already
// carries an UNCONDITIONAL per-click prompt — a strictly stronger control than
// a persisted allow-flag — and the Preferences tooltip already said so.
// /SubmitForm and /ImportData have no UI dispatcher at all (SEC-52), so there
// is nothing to gate there yet.
//
// This file asserts all three halves: the gate refuses by default, the gate
// permits when enabled, and the wiring cannot silently rot back to dead code.
#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "annotations/LinkAction.h"
#include "app/AppSettings.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path projectRoot()
{
    // tests/ lives directly under the project root.
    return fixturesDir().parent_path().parent_path();
}

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The URI planted on page 0. Scheme is https so the separate UNC /
// file-share-scheme refusal (path_security::is_unsafe_link_target) does NOT
// fire — otherwise this test would be measuring the wrong control.
constexpr const char* kPlantedUri = "https://example.test/f13-sec-49";

} // namespace

class ExternalActionsGateTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(std::filesystem::exists(fixturesDir() / "test_3page.pdf"));
        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());

        workPdf_ = std::filesystem::path(tempDir_->path().toStdString())
                   / "external_actions_work.pdf";
        std::filesystem::copy_file(fixturesDir() / "test_3page.pdf", workPdf_);

        {
            auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
            auto doc = pdfe::core::Document::open(ctx, workPdf_);
            auto page = doc->page(0);
            pdfe::annotations::AnnotationManager mgr(ctx);

            auto uriLink = mgr.createAnnotation(
                page.get(), pdfe::annotations::AnnotationType::Link,
                QRectF(50, 50, 200, 50));
            pdfe::annotations::LinkAction act;
            act.type = pdfe::annotations::LinkActionType::Uri;
            act.uri = kPlantedUri;
            mgr.setLinkAction(page.get(), uriLink.annotIndex, act);

            auto planted = std::filesystem::path(tempDir_->path().toStdString())
                           / "external_actions_planted.pdf";
            doc->saveAs(planted);
            workPdf_ = planted;
        }

        settings_ = std::make_unique<pdfe::app::AppSettings>();
        priorExternal_ = settings_->externalActionsAllowed();

        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        mw_->openFile(QString::fromStdString(workPdf_.string()));
        QApplication::processEvents();

        pdfe::ui::MainWindow::resetUriCountersForTesting();
        dispatched_.clear();
        // Capture instead of shelling out to the OS browser.
        pdfe::ui::MainWindow::setUriHandlerForTesting(
            [this](const QString& uri) { dispatched_.push_back(uri); });
    }

    void TearDown() override
    {
        pdfe::ui::MainWindow::setUriHandlerForTesting({});
        pdfe::ui::MainWindow::resetUriCountersForTesting();
        if (settings_) settings_->setExternalActionsAllowed(priorExternal_);
        mw_.reset();
        QApplication::processEvents();
        tempDir_.reset();
    }

    /// Click inside the planted URI link rect.
    bool clickTheLink() { return mw_->followLinkAt(0, QPointF(120.0, 70.0)); }

    std::unique_ptr<QTemporaryDir> tempDir_;
    std::filesystem::path workPdf_;
    std::unique_ptr<pdfe::app::AppSettings> settings_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
    std::vector<QString> dispatched_;
    bool priorExternal_ = false;
};

// ---------------------------------------------------------------------------
// The default. This is the assertion that would have caught the dead-code bug:
// before F13 the URI was dispatched here regardless of the preference.
// ---------------------------------------------------------------------------
TEST_F(ExternalActionsGateTest, UriIsRefusedWhenExternalActionsAreDisabled)
{
    settings_->setExternalActionsAllowed(false);

    EXPECT_TRUE(clickTheLink()) << "the click must still be HANDLED (refused), "
                                   "not fall through as an unrecognised link";

    EXPECT_EQ(pdfe::ui::MainWindow::uriDispatchedCountForTesting(), 0)
        << "no /URI action may be dispatched while external actions are off";
    EXPECT_EQ(pdfe::ui::MainWindow::uriRefusedCountForTesting(), 1)
        << "the refusal must be counted, so the gate is observable";
    EXPECT_TRUE(dispatched_.empty())
        << "the URI reached the dispatcher: the gate did not hold";
}

// ---------------------------------------------------------------------------
// The opt-in. Without this the gate could be satisfied by code that refuses
// unconditionally, which would make the checkbox dead in the other direction.
// ---------------------------------------------------------------------------
TEST_F(ExternalActionsGateTest, UriIsDispatchedWhenExternalActionsAreEnabled)
{
    settings_->setExternalActionsAllowed(true);

    EXPECT_TRUE(clickTheLink());

    EXPECT_EQ(pdfe::ui::MainWindow::uriDispatchedCountForTesting(), 1);
    EXPECT_EQ(pdfe::ui::MainWindow::uriRefusedCountForTesting(), 0);
    ASSERT_EQ(dispatched_.size(), 1u);
    EXPECT_EQ(dispatched_[0], QString::fromLatin1(kPlantedUri));
}

// ---------------------------------------------------------------------------
// The preference is read LIVE, not cached at construction: toggling it in
// Preferences takes effect on the next click without a restart.
// ---------------------------------------------------------------------------
TEST_F(ExternalActionsGateTest, TogglingThePreferenceTakesEffectPerClick)
{
    settings_->setExternalActionsAllowed(false);
    EXPECT_TRUE(clickTheLink());
    EXPECT_EQ(pdfe::ui::MainWindow::uriDispatchedCountForTesting(), 0);
    EXPECT_EQ(pdfe::ui::MainWindow::uriRefusedCountForTesting(), 1);

    settings_->setExternalActionsAllowed(true);
    EXPECT_TRUE(clickTheLink());
    EXPECT_EQ(pdfe::ui::MainWindow::uriDispatchedCountForTesting(), 1);
    EXPECT_EQ(pdfe::ui::MainWindow::uriRefusedCountForTesting(), 1);

    settings_->setExternalActionsAllowed(false);
    EXPECT_TRUE(clickTheLink());
    EXPECT_EQ(pdfe::ui::MainWindow::uriDispatchedCountForTesting(), 1);
    EXPECT_EQ(pdfe::ui::MainWindow::uriRefusedCountForTesting(), 2);

    ASSERT_EQ(dispatched_.size(), 1u);
}

// ---------------------------------------------------------------------------
// The factory default is OFF. A control that ships ON protects nobody.
// ---------------------------------------------------------------------------
TEST(ExternalActionsPreference, DefaultsToDisabled)
{
    pdfe::app::AppSettings settings;
    const bool prior = settings.externalActionsAllowed();

    // QSettings persists across runs, so the default is asserted by removing
    // any stored value first (setting it and reading back proves round-trip;
    // the shipped default is asserted from the source line below).
    settings.setExternalActionsAllowed(false);
    EXPECT_FALSE(settings.externalActionsAllowed());
    settings.setExternalActionsAllowed(true);
    EXPECT_TRUE(settings.externalActionsAllowed());

    const std::string src =
        readFile(projectRoot() / "src" / "app" / "AppSettings.cpp");
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("\"Security/externalActionsAllowed\", false"),
              std::string::npos)
        << "the shipped default for Security/externalActionsAllowed must be "
           "false; a security opt-in that defaults ON is not an opt-in";

    settings.setExternalActionsAllowed(prior);
}

// ---------------------------------------------------------------------------
// ANTI-ROT ORACLE. The original defect was not "the gate is wrong", it was
// "there is no gate and nothing noticed". A behavioural test alone would pass
// if a refactor removed the checkbox AND the gate together; this asserts the
// checkbox and its consumer both still exist, so the setting cannot drift back
// into being a live control that gates nothing.
// ---------------------------------------------------------------------------
TEST(ExternalActionsPreference, CheckboxAndItsGateBothExistInSource)
{
    const std::string prefs =
        readFile(projectRoot() / "src" / "ui" / "PreferencesDialog.cpp");
    ASSERT_FALSE(prefs.empty());
    const std::string mainWin =
        readFile(projectRoot() / "src" / "ui" / "MainWindow.cpp");
    ASSERT_FALSE(mainWin.empty());

    const bool hasCheckbox =
        prefs.find("externalActionsAllowedCheck") != std::string::npos;
    const bool hasGate =
        mainWin.find("externalActionsAllowed()") != std::string::npos;

    // Either state is defensible on its own; the FORBIDDEN state is a live
    // checkbox with no consumer.
    EXPECT_FALSE(hasCheckbox && !hasGate)
        << "src/ui/PreferencesDialog.cpp offers an 'Allow external actions' "
           "checkbox but src/ui/MainWindow.cpp no longer consults "
           "externalActionsAllowed(). Either restore the gate or remove the "
           "checkbox — a security control that gates nothing is worse than "
           "no control (SEC-49).";

    // Record the state this phase actually shipped.
    EXPECT_TRUE(hasCheckbox);
    EXPECT_TRUE(hasGate);
}
