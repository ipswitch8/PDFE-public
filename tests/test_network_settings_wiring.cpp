// -*- coding: utf-8 -*-
// GitHub #240 — the two network settings exist, persist, default correctly, AND
// are reachable from the Preferences UI.
//
// WHY THE UI HALF IS TESTED AND NOT ASSUMED
//
// This codebase has already shipped a security checkbox that gated nothing:
// `Security/externalActionsAllowed` was declared, persisted and exposed as a
// Preferences control while being read NOWHERE in src/ — a live-looking control
// governing no behaviour, found by a meta-audit (F13 / SEC-49) rather than by a
// test. The same shape is available to any new setting, and a setting the user
// was promised is worse than one that was never offered.
//
// So these tests assert three separate things, because they can fail
// independently:
//   1. the setting exists, persists, and has the intended DEFAULT;
//   2. a control for it exists in the Preferences dialog with a stable
//      objectName (which is also what makes it drivable by the UI suite);
//   3. the control actually round-trips through the setting — load reflects the
//      stored value, and save writes the control's value back.
//
// What they deliberately do NOT assert is the behaviour the settings gate; that
// is `test_network_path_policy.cpp`. Splitting them matters: a checkbox wired to
// a setting that nothing reads would pass here and fail there, and that is
// exactly the pair of facts F13 found to be out of step.
#include <gtest/gtest.h>

#include "app/AppSettings.h"
#include "ui/PreferencesDialog.h"

#include <QCheckBox>
#include <QPushButton>
#include <QSettings>
#include <QString>

#include <memory>

using pdfe::app::AppSettings;
using pdfe::ui::PreferencesDialog;

namespace {

/// A real AppSettings with the two GitHub #240 values snapshotted and restored.
///
/// AppSettings has no injectable store — it wraps the application-wide QSettings
/// — so a test cannot point it at a scratch location. Writing through it and
/// restoring is therefore the honest option; the alternative (reaching past it
/// into QSettings directly) would test a different object than the one the
/// dialog uses, which is the mistake that lets a UI control and a setting drift
/// apart unnoticed.
class ScopedSettings {
public:
    ScopedSettings()
        : settings_(std::make_unique<AppSettings>()),
          userPaths_(settings_->allowUserNetworkPaths()),
          sameShare_(settings_->allowDocumentSameShareResources())
    {
    }
    ~ScopedSettings()
    {
        settings_->setAllowUserNetworkPaths(userPaths_);
        settings_->setAllowDocumentSameShareResources(sameShare_);
    }
    AppSettings* get() { return settings_.get(); }

private:
    std::unique_ptr<AppSettings> settings_;
    bool userPaths_;
    bool sameShare_;
};

}  // namespace

// ─── 1. The settings themselves ─────────────────────────────────────────────

// The user-facing half of #240: opening your own file on your own file server is
// ordinary, so this defaults ON. If it defaulted off, the reported defect would
// still be present for every user who never visits Preferences.
TEST(NetworkSettings, UserNetworkPathsDefaultsOn)
{
    ScopedSettings s;
    // Remove the stored key so the DEFAULT is what answers, not whatever this
    // machine happens to have saved.
    QSettings raw;
    raw.remove("Network/allowUserPaths");
    raw.sync();
    EXPECT_TRUE(s.get()->allowUserNetworkPaths());
}

// The security-relevant half. This MUST default off: a path chosen by document
// content is the egress vector GitHub #91 closed, and a default-on switch would
// silently reopen it for everyone.
TEST(NetworkSettings, DocumentSameShareResourcesDefaultsOff)
{
    ScopedSettings s;
    QSettings raw;
    raw.remove("Network/allowDocumentSameShareResources");
    raw.sync();
    EXPECT_FALSE(s.get()->allowDocumentSameShareResources())
        << "the security-relevant setting defaults ON — a default-on switch "
           "silently reopens the egress vector GitHub #91 closed";
}

TEST(NetworkSettings, BothSettingsPersist)
{
    ScopedSettings s;
    s.get()->setAllowUserNetworkPaths(false);
    s.get()->setAllowDocumentSameShareResources(true);
    EXPECT_FALSE(s.get()->allowUserNetworkPaths());
    EXPECT_TRUE(s.get()->allowDocumentSameShareResources());

    // A fresh reader must see the written values, or the control would appear
    // to work for the rest of the session and then forget.
    AppSettings reread;
    EXPECT_FALSE(reread.allowUserNetworkPaths());
    EXPECT_TRUE(reread.allowDocumentSameShareResources());
}

// The two must be independent. One switch governing both would conflate the
// ordinary act with the security-relevant one, which is the whole point of
// having two.
TEST(NetworkSettings, TheTwoSettingsAreIndependent)
{
    ScopedSettings s;
    s.get()->setAllowUserNetworkPaths(true);
    s.get()->setAllowDocumentSameShareResources(false);
    EXPECT_TRUE(s.get()->allowUserNetworkPaths());
    EXPECT_FALSE(s.get()->allowDocumentSameShareResources());

    s.get()->setAllowUserNetworkPaths(false);
    EXPECT_FALSE(s.get()->allowUserNetworkPaths());
    EXPECT_FALSE(s.get()->allowDocumentSameShareResources())
        << "changing the user-paths setting also changed the document-resources "
           "setting — they share storage";
}

// ─── 2 and 3. The Preferences control, and the round trip ───────────────────

TEST(NetworkSettingsUi, BothControlsExistWithStableObjectNames)
{
    ScopedSettings s;
    PreferencesDialog dlg(s.get());

    auto* userCheck = dlg.findChild<QCheckBox*>("allowUserNetworkPathsCheck");
    auto* shareCheck = dlg.findChild<QCheckBox*>("allowDocumentSameShareCheck");

    ASSERT_NE(userCheck, nullptr)
        << "no Preferences control for allowUserNetworkPaths — the setting is "
           "unreachable for a user, which is how a setting becomes a promise "
           "nobody can keep";
    ASSERT_NE(shareCheck, nullptr)
        << "no Preferences control for allowDocumentSameShareResources";

    // The UI suite matches on accessible names; a blank one is not drivable.
    EXPECT_FALSE(userCheck->accessibleName().isEmpty());
    EXPECT_FALSE(shareCheck->accessibleName().isEmpty());

    // The document-resources control must explain the risk, not merely name the
    // switch — this is the one a user should hesitate over.
    EXPECT_FALSE(shareCheck->toolTip().isEmpty())
        << "the security-relevant control has no explanation";
}

TEST(NetworkSettingsUi, ControlsReflectTheStoredValues)
{
    ScopedSettings s;
    s.get()->setAllowUserNetworkPaths(false);
    s.get()->setAllowDocumentSameShareResources(true);

    PreferencesDialog dlg(s.get());
    auto* userCheck = dlg.findChild<QCheckBox*>("allowUserNetworkPathsCheck");
    auto* shareCheck = dlg.findChild<QCheckBox*>("allowDocumentSameShareCheck");
    ASSERT_NE(userCheck, nullptr);
    ASSERT_NE(shareCheck, nullptr);

    EXPECT_FALSE(userCheck->isChecked())
        << "the control did not load the stored value — it would show the wrong "
           "state and then write it back on OK";
    EXPECT_TRUE(shareCheck->isChecked());
}

// The half that F13's checkbox failed: does OK actually write the control back?
TEST(NetworkSettingsUi, AcceptingTheDialogWritesTheControlsBack)
{
    ScopedSettings s;
    s.get()->setAllowUserNetworkPaths(true);
    s.get()->setAllowDocumentSameShareResources(false);

    PreferencesDialog dlg(s.get());
    auto* userCheck = dlg.findChild<QCheckBox*>("allowUserNetworkPathsCheck");
    auto* shareCheck = dlg.findChild<QCheckBox*>("allowDocumentSameShareCheck");
    ASSERT_NE(userCheck, nullptr);
    ASSERT_NE(shareCheck, nullptr);

    userCheck->setChecked(false);
    shareCheck->setChecked(true);

    // Click OK rather than calling accept() — it is private, and clicking is
    // what a user does. This also exercises the button's own wiring, which is
    // one more link in the chain that could be missing.
    auto* ok = dlg.findChild<QPushButton*>("preferencesOkButton");
    ASSERT_NE(ok, nullptr) << "no OK button; the dialog cannot be accepted";
    ok->click();

    EXPECT_FALSE(s.get()->allowUserNetworkPaths())
        << "the dialog was accepted but the user-paths control was not saved";
    EXPECT_TRUE(s.get()->allowDocumentSameShareResources())
        << "the dialog was accepted but the same-share control was not saved — "
           "this is exactly the shape of the externalActionsAllowed defect "
           "(a control that looks live and governs nothing)";
}
