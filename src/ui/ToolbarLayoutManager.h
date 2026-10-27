#pragma once

#include <QString>
#include <QStringList>
#include <vector>

class QSettings;
class QToolBar;
class QAction;

namespace pdfe::ui {

/// Pipeline E phase-10: persistent toolbar customization.
///
/// Each toolbar is identified by its QToolBar::objectName. A layout is
/// an ordered list of action IDs (empty string == "separator"). Action
/// IDs match QAction::objectName.
struct ToolbarLayout {
    QString toolbarName;
    QStringList actionIds; // empty string means separator

    /// GitHub #309. Every action id that EXISTED when this layout was saved,
    /// whether or not the user kept it on the toolbar.
    ///
    /// Without this, a saved layout cannot distinguish two situations that
    /// demand opposite handling:
    ///
    ///   * an action absent because the user REMOVED it      -> keep it off;
    ///   * an action absent because it DID NOT EXIST YET     -> show it.
    ///
    /// `actionIds` alone conflates them, so the only safe behaviours were to
    /// resurrect deliberately-removed actions or to hide every action added
    /// after the layout was saved. PDFE did the latter: a user who had ever
    /// customised a toolbar never saw a tool added in a later release, with no
    /// route to it short of Customize or deleting the registry key.
    ///
    /// With this list the rule is exact — an action in the registry that is in
    /// neither `actionIds` nor `knownActionIds` is NEW, and only those are
    /// appended.
    ///
    /// Empty means "saved before this field existed" (schema 1); see
    /// `reconcileWithRegistry` for how that case is handled and why.
    QStringList knownActionIds;
};

class ToolbarLayoutManager {
public:
    ToolbarLayoutManager();

    /// Factory defaults: the layout MainWindow ships with.
    /// Tests and the "Reset to Default" button consult this.
    static std::vector<ToolbarLayout> defaultLayouts();

    /// Persist `layouts` under "pdfe/toolbarLayout" in QSettings.
    /// Existing entries under the same group are cleared first.
    void saveLayouts(const std::vector<ToolbarLayout>& layouts) const;

    /// Read layouts from QSettings. Returns an empty vector when no
    /// saved layout exists.
    std::vector<ToolbarLayout> loadLayouts() const;

    /// Convenience: write defaultLayouts() to QSettings.
    void resetToDefaults() const;

    /// GitHub #309. Append registry actions that the saved layout has never
    /// seen, so a tool added after the user customised a toolbar is still
    /// reachable.
    ///
    /// For each layout, an action from `registry` is appended when its
    /// objectName appears in NEITHER `actionIds` NOR `knownActionIds`.
    /// Deliberately-removed actions are therefore left off, because they ARE
    /// in `knownActionIds`.
    ///
    /// SCHEMA-1 LAYOUTS (empty `knownActionIds`) cannot express that
    /// distinction, so every registry action missing from `actionIds` looks
    /// new. This appends them, and that is a deliberate choice rather than an
    /// oversight: the failure it prevents is a shipped feature the user cannot
    /// discover, and the failure it risks is one previously-removed button
    /// reappearing once, after which the next save records `knownActionIds`
    /// and the ambiguity is gone for good. A reappearing button is a nuisance;
    /// an invisible feature is a defect.
    ///
    /// Returns the number of actions appended, so a caller (or a test) can
    /// assert the reconciliation did something rather than assume it.
    static int reconcileWithRegistry(std::vector<ToolbarLayout>& layouts,
                                     const std::vector<QAction*>& registry);

    /// True iff QSettings has any pdfe/toolbarLayout entries.
    bool hasSavedLayout() const;

    /// Route reads/writes to a custom QSettings file path instead of the
    /// platform-native store. Pass an empty string for the default location.
    ///
    /// NOT test-only, despite the original name below. `MainWindow` calls this
    /// on BOTH the load and save paths so the saved toolbar layout honours the
    /// `PDFE_SETTINGS_PATH` isolation seam that `AppSettings` and
    /// `ThemeManager` already honour (GitHub #309, p97 phase-9). Without it a
    /// run on an isolated profile still read -- and `onCustomizeToolbars` still
    /// WROTE -- the real HKCU layout.
    void setSettingsFilePath(const QString& iniPath);

    /// Deprecated spelling, retained so existing callers keep compiling.
    /// Forwards to `setSettingsFilePath`. The `ForTesting` suffix became a
    /// false statement once production code needed the same seam, and a name
    /// that misdescribes its caller is how a seam ends up bypassed.
    void setSettingsFilePathForTesting(const QString& iniPath) {
        setSettingsFilePath(iniPath);
    }

    /// Apply a layout to a live QToolBar. Resolves action IDs against
    /// the supplied registry; unknown IDs are skipped (logged via
    /// qWarning). Empty IDs become separators.
    static void applyLayoutToToolbar(QToolBar* toolbar,
                                     const ToolbarLayout& layout,
                                     const std::vector<QAction*>& registry);

    /// Capture the current state of a QToolBar into a ToolbarLayout
    /// (used to seed the customization dialog from the live UI).
    static ToolbarLayout captureFromToolbar(const QToolBar* toolbar);

private:
    QString settingsFilePath_; // empty == use platform default
};

} // namespace pdfe::ui
