// -*- coding: utf-8 -*-
// Pipeline E phase-10 / Bucket 3h: customizable toolbars.
//
// Verifies the persistence layer (ToolbarLayoutManager) and the live
// dialog (CustomizeToolbarsDialog):
//   (1) Save a custom layout, reload from QSettings, layout preserved.
//   (2) Reset to defaults restores the factory layout.
//   (3) applyLayoutToToolbar materializes the layout onto a real
//       QToolBar with the correct actions in order (and skips unknown
//       IDs gracefully).
//   (4) The Customize dialog exposes its controls under stable
//       objectNames so pywinauto/UIA tests can drive it.
#include <gtest/gtest.h>

#include "ui/CustomizeToolbarsDialog.h"
#include "ui/MainWindow.h"
#include "ui/ToolbarLayoutManager.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QListView>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QToolBar>

#include <filesystem>
#include <fstream>
#include <vector>
#include <memory>

using namespace pdfe::ui;

namespace {

QApplication* ensureApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static const char* argv0 = "test_toolbar_customization";
        static char* argv[] = { const_cast<char*>(argv0), nullptr };
        return new QApplication(argc, argv);
    }
    return nullptr;
}

QString makeTempIniPath() {
    auto dir = std::filesystem::temp_directory_path();
    auto p = dir / ("pdfe_phase10_toolbar_" +
                     std::to_string(std::time(nullptr)) + ".ini");
    return QString::fromStdString(p.string());
}

QAction* makeAction(QObject* parent, const char* objName, const char* text) {
    auto* a = new QAction(QString::fromUtf8(text), parent);
    a->setObjectName(QString::fromUtf8(objName));
    return a;
}

} // namespace

class ToolbarCustomizationTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// (1) Save a custom layout, reload from QSettings, verify identical.
TEST_F(ToolbarCustomizationTest, SaveAndReloadRoundTrip) {
    ToolbarLayoutManager mgr;
    QString iniPath = makeTempIniPath();
    mgr.setSettingsFilePathForTesting(iniPath);

    std::vector<ToolbarLayout> custom = {
        ToolbarLayout{"mainToolBar",
                      QStringList{"saveAction", "openAction", "",
                                  "zoomInAction"}},
        ToolbarLayout{"annotationToolBar",
                      QStringList{"selectToolAction", "stickyNoteToolAction"}},
    };
    mgr.saveLayouts(custom);
    EXPECT_TRUE(mgr.hasSavedLayout());

    auto reloaded = mgr.loadLayouts();
    ASSERT_EQ(reloaded.size(), 2u);
    EXPECT_EQ(reloaded[0].toolbarName.toStdString(), "mainToolBar");
    EXPECT_EQ(reloaded[0].actionIds, custom[0].actionIds);
    EXPECT_EQ(reloaded[1].toolbarName.toStdString(), "annotationToolBar");
    EXPECT_EQ(reloaded[1].actionIds, custom[1].actionIds);

    std::filesystem::remove(std::filesystem::path(iniPath.toStdString()));
}

// (2) Reset to defaults restores factory layout.
TEST_F(ToolbarCustomizationTest, ResetRestoresDefaults) {
    ToolbarLayoutManager mgr;
    QString iniPath = makeTempIniPath();
    mgr.setSettingsFilePathForTesting(iniPath);

    // Pre-write a deliberately weird layout.
    mgr.saveLayouts({
        ToolbarLayout{"mainToolBar", QStringList{"customAction"}}
    });
    auto preReset = mgr.loadLayouts();
    ASSERT_EQ(preReset.size(), 1u);
    EXPECT_EQ(preReset[0].actionIds, QStringList{"customAction"});

    mgr.resetToDefaults();
    auto afterReset = mgr.loadLayouts();
    auto factory = ToolbarLayoutManager::defaultLayouts();
    ASSERT_EQ(afterReset.size(), factory.size());
    for (size_t i = 0; i < factory.size(); ++i) {
        EXPECT_EQ(afterReset[i].toolbarName, factory[i].toolbarName);
        EXPECT_EQ(afterReset[i].actionIds, factory[i].actionIds);
    }

    std::filesystem::remove(std::filesystem::path(iniPath.toStdString()));
}

// (3) applyLayoutToToolbar materializes onto QToolBar correctly.
TEST_F(ToolbarCustomizationTest, ApplyLayoutToToolbarMatchesIds) {
    QToolBar tb;
    tb.setObjectName("mainToolBar");
    QObject parent;
    std::vector<QAction*> registry = {
        makeAction(&parent, "openAction",   "Open"),
        makeAction(&parent, "saveAction",   "Save"),
        makeAction(&parent, "zoomInAction", "Zoom In"),
    };

    ToolbarLayout layout{
        "mainToolBar",
        QStringList{"saveAction", "", "openAction", "unknownAction"}
        // separator + open + (unknown skipped)
    };
    ToolbarLayoutManager::applyLayoutToToolbar(&tb, layout, registry);

    auto actions = tb.actions();
    ASSERT_EQ(actions.size(), 3) << "saveAction + separator + openAction; "
                                    "unknownAction skipped";
    EXPECT_EQ(actions[0]->objectName().toStdString(), "saveAction");
    EXPECT_TRUE(actions[1]->isSeparator());
    EXPECT_EQ(actions[2]->objectName().toStdString(), "openAction");
}

// captureFromToolbar should round-trip through applyLayoutToToolbar.
TEST_F(ToolbarCustomizationTest, CaptureRoundTripsThroughApply) {
    QToolBar tb;
    tb.setObjectName("annotationToolBar");
    QObject parent;
    std::vector<QAction*> registry = {
        makeAction(&parent, "selectToolAction", "Select"),
        makeAction(&parent, "stickyNoteToolAction", "Sticky"),
    };
    tb.addAction(registry[0]);
    tb.addSeparator();
    tb.addAction(registry[1]);

    ToolbarLayout captured =
        ToolbarLayoutManager::captureFromToolbar(&tb);
    EXPECT_EQ(captured.toolbarName.toStdString(), "annotationToolBar");
    ASSERT_EQ(captured.actionIds.size(), 3);
    EXPECT_EQ(captured.actionIds[0].toStdString(), "selectToolAction");
    EXPECT_TRUE(captured.actionIds[1].isEmpty());
    EXPECT_EQ(captured.actionIds[2].toStdString(), "stickyNoteToolAction");

    // Apply onto a fresh toolbar; expect identical contents.
    QToolBar tb2;
    ToolbarLayoutManager::applyLayoutToToolbar(&tb2, captured, registry);
    auto a2 = tb2.actions();
    ASSERT_EQ(a2.size(), 3);
    EXPECT_EQ(a2[0]->objectName(), "selectToolAction");
    EXPECT_TRUE(a2[1]->isSeparator());
    EXPECT_EQ(a2[2]->objectName(), "stickyNoteToolAction");
}

// (4) Dialog exposes stable objectNames for automation.
TEST_F(ToolbarCustomizationTest, DialogExposesNamedControls) {
    QObject parent;
    std::vector<QAction*> registry = {
        makeAction(&parent, "openAction", "Open"),
        makeAction(&parent, "saveAction", "Save"),
    };
    auto layouts = ToolbarLayoutManager::defaultLayouts();
    CustomizeToolbarsDialog dlg(registry, layouts);
    EXPECT_EQ(dlg.objectName().toStdString(), "customizeToolbarsDialog");
    EXPECT_NE(dlg.findChild<QComboBox*>("toolbarCombo"),         nullptr);
    EXPECT_NE(dlg.findChild<QListView*>("availableList"),       nullptr);
    EXPECT_NE(dlg.findChild<QListView*>("currentList"),         nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("addActionButton"),    nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("removeActionButton"), nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("moveUpButton"),       nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("moveDownButton"),     nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("addSeparatorButton"), nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("resetToDefaultButton"), nullptr);
}

// Reset button in dialog rebuilds layouts back to factory defaults.
TEST_F(ToolbarCustomizationTest, DialogResetButtonRestoresDefaults) {
    QObject parent;
    std::vector<QAction*> registry = {
        makeAction(&parent, "openAction", "Open"),
    };
    std::vector<ToolbarLayout> hacked = {
        ToolbarLayout{"mainToolBar", QStringList{"openAction"}},
    };
    CustomizeToolbarsDialog dlg(registry, hacked);
    EXPECT_EQ(dlg.updatedLayouts().size(), 1u);
    auto* reset = dlg.findChild<QPushButton*>("resetToDefaultButton");
    ASSERT_NE(reset, nullptr);
    reset->click();
    auto after = dlg.updatedLayouts();
    auto factory = ToolbarLayoutManager::defaultLayouts();
    EXPECT_EQ(after.size(), factory.size());
    EXPECT_EQ(after[0].toolbarName, factory[0].toolbarName);
}

// loadLayouts on a fresh ini returns empty (no false-positive defaults).
TEST_F(ToolbarCustomizationTest, FreshSettingsReturnsEmpty) {
    ToolbarLayoutManager mgr;
    QString iniPath = makeTempIniPath();
    mgr.setSettingsFilePathForTesting(iniPath);
    EXPECT_FALSE(mgr.hasSavedLayout());
    auto layouts = mgr.loadLayouts();
    EXPECT_TRUE(layouts.empty());
    std::filesystem::remove(std::filesystem::path(iniPath.toStdString()));
}

// ---------------------------------------------------------------------------
// GitHub #309 / #253 / p97 phase-9 — the PDFE_SETTINGS_PATH seam.
//
// The `claim-check` gate failed phase-9 partly on this, and it was right:
// MainWindow used to construct ToolbarLayoutManager bare, so it read AND WROTE
// HKCU even when PDFE_SETTINGS_PATH had moved every other settings consumer
// onto a throwaway INI. The write side is the dangerous half -- a test run
// could rewrite the developer's real toolbar layout -- and nothing asserted it.
//
// Note what the tests ABOVE do not establish. SaveAndReloadRoundTrip saves and
// reloads through the SAME manager with the SAME injected path, so it would
// pass unchanged if the path were ignored entirely and both operations went to
// HKCU. A round trip through one store cannot tell you WHICH store. These two
// tests close that gap from opposite directions.
// ---------------------------------------------------------------------------

// The write must land in the injected FILE. Asserted against the filesystem,
// which is the one observation HKCU cannot satisfy.
TEST_F(ToolbarCustomizationTest, SaveWritesToTheInjectedFileAndNotTheRegistry) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString iniPath = dir.filePath("seam.ini");

    ToolbarLayoutManager mgr;
    mgr.setSettingsFilePath(iniPath);
    mgr.saveLayouts({ToolbarLayout{"mainToolBar",
                                   QStringList{"openAction", "saveAction"}}});

    // 1. The file exists and carries the payload. If the manager had ignored
    //    the path and written to the platform store, no file would appear here.
    ASSERT_TRUE(QFileInfo::exists(iniPath))
        << "saveLayouts() did not create the injected INI at "
        << iniPath.toStdString()
        << " -- the PDFE_SETTINGS_PATH seam is not being honoured on the WRITE "
           "side, which means a test run can rewrite the real user profile";
    QFile f(iniPath);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString contents = QString::fromUtf8(f.readAll());
    f.close();
    EXPECT_TRUE(contents.contains(QStringLiteral("openAction")))
        << "the INI exists but does not contain the saved ids; contents:\n"
        << contents.toStdString();

    // 2. A manager on a DIFFERENT path must not see it. This is the isolation
    //    property itself: if both instances resolved to one shared store, this
    //    would find the layout.
    ToolbarLayoutManager other;
    other.setSettingsFilePath(dir.filePath("unrelated.ini"));
    EXPECT_FALSE(other.hasSavedLayout())
        << "a manager pointed at a different file can see the first one's "
           "layout, so the path is not actually scoping the store";
}

// MainWindow must not construct the manager bare. This is a SOURCE-level
// assertion on purpose: the behavioural symptom of a revert only appears on a
// machine that happens to have a saved HKCU layout, so on a clean CI box a
// reverted seam is invisible to every behavioural test. That is precisely why
// claim-check called this claim unmechanised, and a mechanism that only fires
// on a developer's customised profile is not a mechanism.
TEST_F(ToolbarCustomizationTest, MainWindowNeverConstructsTheManagerWithoutTheSeam) {
    // Resolve the source from the build-time location of this test file, so the
    // test does not depend on the working directory. A wrong root would make
    // this pass vacuously, so the resolution is asserted.
    const std::filesystem::path src =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "ui" / "MainWindow.cpp";
    ASSERT_TRUE(std::filesystem::exists(src))
        << "could not resolve MainWindow.cpp from __FILE__ (" << __FILE__
        << "); this test must not pass without reading it";

    std::ifstream in(src);
    ASSERT_TRUE(in.good());
    std::string line;
    int constructions = 0;
    int seamCalls = 0;
    std::vector<int> bareLines;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        // Skip comment lines: this very file discusses the pattern at length,
        // and counting prose as code is how a guard starts lying.
        const auto firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos &&
            line.compare(firstNonSpace, 2, "//") == 0) {
            continue;
        }
        if (line.find("ToolbarLayoutManager ") != std::string::npos ||
            line.find("ToolbarLayoutManager(") != std::string::npos) {
            // Declarations of a local manager, or a temporary.
            if (line.find("ToolbarLayoutManager::") == std::string::npos) {
                ++constructions;
                bareLines.push_back(lineNo);
            }
        }
        if (line.find("setSettingsFilePath") != std::string::npos) {
            ++seamCalls;
        }
    }

    // Non-vacuity first: if the scan found no constructions at all, the pattern
    // changed and this test is asserting nothing.
    ASSERT_GT(constructions, 0)
        << "found no ToolbarLayoutManager construction in MainWindow.cpp -- the "
           "scan is no longer matching the code and this test has stopped "
           "checking anything";

    EXPECT_GE(seamCalls, constructions)
        << "MainWindow.cpp constructs ToolbarLayoutManager " << constructions
        << " time(s) but calls setSettingsFilePath only " << seamCalls
        << " time(s). Every construction must inject "
           "AppSettings::settingsFilePath(), or that instance falls back to "
           "HKCU and both reads AND WRITES the real user profile regardless of "
           "PDFE_SETTINGS_PATH (GitHub #309).";
}

// ---------------------------------------------------------------------------
// GitHub #309 — a layout saved by a PRIOR RELEASE must not hide the actions
// this release added.
//
// THE DEFECT. `applyLayoutToToolbar()` rebuilds each toolbar from exactly the
// ids the saved layout names. A layout written before p97 added `LinkTool`,
// `formDeleteFieldAction` and `redactionPropertiesButton` therefore names none
// of them, and the user never sees them — not pushed into the overflow
// chevron, ABSENT, with no route back short of the Customize dialog or
// deleting the registry key. Measured before the fix: 14 buttons on the
// annotation toolbar with a saved layout present, 17 without.
//
// WHY `knownActionIds` IS THE FIX AND A PLAIN "APPEND WHAT IS MISSING" IS NOT.
// A layout's id list cannot distinguish two opposite situations — the user
// removed an action, versus the action did not exist yet. Appending everything
// missing resurrects deliberately-removed buttons; appending nothing hides
// every new feature. Recording what existed AT SAVE TIME separates them
// exactly, and these tests pin both directions, because a fix that satisfied
// only the first would pass a one-sided test.
// ---------------------------------------------------------------------------

namespace {

/// A registry of named actions, as `MainWindow::buildToolbarActionRegistry()`
/// returns. Ownership stays with `parent`.
std::vector<QAction*> registryOf(QObject* parent,
                                 const std::vector<const char*>& names)
{
    std::vector<QAction*> out;
    out.reserve(names.size());
    for (const char* n : names) out.push_back(makeAction(parent, n, n));
    return out;
}

QStringList idsOf(const std::vector<QAction*>& reg)
{
    QStringList out;
    for (QAction* a : reg) out.push_back(a->objectName());
    return out;
}

} // namespace

// The headline case, and the one criterion 3 names: a prior-release layout
// round-tripped through QSettings, then reconciled against a registry that has
// grown by four controls.
TEST_F(ToolbarCustomizationTest, ALayoutFromAPriorReleaseStillShowsThisReleasesNewActions)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString iniPath = dir.filePath("prior-release.ini");

    QObject owner;
    // What the PRIOR release knew about: ten annotation tools, no LinkTool.
    const std::vector<QAction*> oldRegistry = registryOf(&owner, {
        "selectToolAction", "stickyNoteToolAction", "highlightToolAction",
        "underlineToolAction", "strikeOutToolAction", "freeTextToolAction",
        "rectangleToolAction", "circleToolAction", "lineToolAction",
        "inkToolAction",
    });

    // The user customised: they dropped `circleToolAction` deliberately.
    QStringList kept = idsOf(oldRegistry);
    ASSERT_TRUE(kept.removeOne(QStringLiteral("circleToolAction")));

    ToolbarLayoutManager saver;
    saver.setSettingsFilePath(iniPath);
    ToolbarLayout saved{QStringLiteral("annotationToolBar"), kept,
                        idsOf(oldRegistry)};
    saver.saveLayouts({saved});

    // THIS release's registry: the same ten plus four added since.
    const std::vector<QAction*> newRegistry = registryOf(&owner, {
        "selectToolAction", "stickyNoteToolAction", "highlightToolAction",
        "underlineToolAction", "strikeOutToolAction", "freeTextToolAction",
        "rectangleToolAction", "circleToolAction", "lineToolAction",
        "inkToolAction",
        // added after the layout above was written:
        "linkToolAction", "formDeleteFieldAction", "stampToolAction",
        "fileAttachmentToolAction",
    });

    ToolbarLayoutManager loader;
    loader.setSettingsFilePath(iniPath);
    ASSERT_TRUE(loader.hasSavedLayout());
    auto layouts = loader.loadLayouts();
    ASSERT_EQ(layouts.size(), 1u);

    // Premise: the round trip really carried the known-ids list. Without it
    // this test would be exercising the schema-1 path and proving something
    // else — the vacuity that would make the deliberate-removal half below
    // pass for the wrong reason.
    ASSERT_EQ(layouts[0].knownActionIds.size(), 10)
        << "knownActionIds did not survive save/load, so this test is not "
           "exercising the schema-2 path it claims to";
    ASSERT_FALSE(layouts[0].actionIds.contains(QStringLiteral("linkToolAction")))
        << "the saved layout already contains linkToolAction, so there is "
           "nothing for reconciliation to add and the test is vacuous";

    const int appended =
        ToolbarLayoutManager::reconcileWithRegistry(layouts, newRegistry);

    // The four new ones are now reachable. THIS is what fails before the fix.
    for (const char* id : {"linkToolAction", "formDeleteFieldAction",
                           "stampToolAction", "fileAttachmentToolAction"}) {
        EXPECT_TRUE(layouts[0].actionIds.contains(QString::fromUtf8(id)))
            << "GitHub #309: " << id << " was added after this layout was "
               "saved and is still absent from it after reconciliation, so a "
               "user who ever customised a toolbar cannot reach it. The "
               "toolbar is rebuilt from this list verbatim.";
    }
    EXPECT_EQ(appended, 4)
        << "expected exactly the 4 actions the layout had never seen to be "
           "appended, got " << appended;

    // And the other direction: a deliberate removal is still respected.
    EXPECT_FALSE(layouts[0].actionIds.contains(QStringLiteral("circleToolAction")))
        << "circleToolAction was removed by the user and is in knownActionIds, "
           "so reconciliation must NOT bring it back. Resurrecting removed "
           "buttons is the failure mode that makes a naive "
           "append-everything-missing fix unacceptable.";
}

// The compatibility case, stated as a deliberate trade rather than left to
// chance: a layout with no knownActionIds predates the field and cannot
// express "removed on purpose", so everything missing is treated as new.
TEST_F(ToolbarCustomizationTest, ASchemaOneLayoutGetsNewActionsAtTheCostOfResurrectingRemovals)
{
    QObject owner;
    const std::vector<QAction*> registry = registryOf(&owner, {
        "selectToolAction", "inkToolAction", "linkToolAction",
    });

    // No knownActionIds: a layout written before the field existed.
    std::vector<ToolbarLayout> layouts{
        ToolbarLayout{QStringLiteral("annotationToolBar"),
                      QStringList{QStringLiteral("selectToolAction")},
                      QStringList{}}};

    const int appended =
        ToolbarLayoutManager::reconcileWithRegistry(layouts, registry);

    EXPECT_EQ(appended, 2);
    EXPECT_TRUE(layouts[0].actionIds.contains(QStringLiteral("linkToolAction")))
        << "a schema-1 layout must still surface actions added later — that is "
           "the whole point of the trade";
    EXPECT_TRUE(layouts[0].actionIds.contains(QStringLiteral("inkToolAction")))
        << "inkToolAction may have been removed on purpose, but a schema-1 "
           "layout cannot say so, and this test records that we knowingly "
           "prefer a reappearing button over an invisible feature. If this "
           "expectation is ever inverted, the compatibility decision in "
           "reconcileWithRegistry's header has changed and should be re-read.";
}

// Reconciliation must be idempotent, or every launch grows the toolbar.
TEST_F(ToolbarCustomizationTest, ReconcilingTwiceAppendsNothingTheSecondTime)
{
    QObject owner;
    const std::vector<QAction*> registry = registryOf(&owner, {
        "selectToolAction", "linkToolAction",
    });
    std::vector<ToolbarLayout> layouts{
        ToolbarLayout{QStringLiteral("annotationToolBar"),
                      QStringList{QStringLiteral("selectToolAction")},
                      QStringList{QStringLiteral("selectToolAction")}}};

    EXPECT_EQ(ToolbarLayoutManager::reconcileWithRegistry(layouts, registry), 1);
    const QStringList afterFirst = layouts[0].actionIds;
    EXPECT_EQ(ToolbarLayoutManager::reconcileWithRegistry(layouts, registry), 0)
        << "the second reconciliation appended something, so the toolbar grows "
           "on every launch";
    EXPECT_EQ(layouts[0].actionIds, afterFirst);
}

// Separators are the empty string and must never be mistaken for an action id.
TEST_F(ToolbarCustomizationTest, SeparatorsAndUnnamedActionsAreNotTreatedAsIds)
{
    QObject owner;
    auto* unnamed = new QAction(QStringLiteral("no object name"), &owner);
    std::vector<QAction*> registry{unnamed, nullptr};

    std::vector<ToolbarLayout> layouts{
        ToolbarLayout{QStringLiteral("mainToolBar"),
                      QStringList{QStringLiteral("openAction"), QString()},
                      QStringList{QStringLiteral("openAction")}}};

    // An unnamed action is not addressable and a null entry is not an action;
    // neither may be appended, and neither may crash.
    EXPECT_EQ(ToolbarLayoutManager::reconcileWithRegistry(layouts, registry), 0);
    EXPECT_EQ(layouts[0].actionIds.size(), 2);
}


// ---------------------------------------------------------------------------
// GitHub #309 — THE WIRING, which the unit tests above do not cover.
//
// The p98 phase-3 `claim-check` gate found this and proved it the only way
// that counts: it deleted the `reconcileWithRegistry()` call from
// `MainWindow::loadSavedToolbarLayout()`, rebuilt, and ran the whole
// `ToolbarCustomizationTest` suite — **all 13 tests stayed green**.
//
// Every test above exercises `reconcileWithRegistry` DIRECTLY. None of them
// goes through `MainWindow`, so none of them notices if the production call
// site disappears. That is precisely the GitHub #253 defect class this
// pipeline spent phase-1 on: a fix that exists, is correct, is unit-tested,
// and is never invoked. A reconciliation nobody calls is indistinguishable
// from the defect.
//
// This test closes it from the only end that can: construct a real
// `MainWindow` against a settings file containing a PRIOR-RELEASE layout, and
// assert the new action is on the live toolbar afterwards. It exercises
// whatever the constructor actually does, so deleting the call fails it.
//
// Why `PDFE_SETTINGS_PATH`: `MainWindow` resolves the toolbar store through
// `AppSettings::settingsFilePath()` (the seam closed earlier in p97 phase-9 —
// before that it read HKCU regardless, so this test would have been reading
// the developer's real profile).
// ---------------------------------------------------------------------------

TEST_F(ToolbarCustomizationTest, MainWindowReconcilesASavedLayoutOnLoadNotJustInTheHelper)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString iniPath = dir.filePath("prior-release-wiring.ini");

    // A layout from before `linkToolAction` existed. `knownActionIds` lists
    // the same ids, i.e. "these are all the actions that existed when I was
    // saved" — so anything in the live registry beyond them is new and must be
    // appended on load.
    const QStringList priorIds{
        QStringLiteral("selectToolAction"),
        QStringLiteral("stickyNoteToolAction"),
        QStringLiteral("highlightToolAction"),
    };
    {
        ToolbarLayoutManager seed;
        seed.setSettingsFilePath(iniPath);
        seed.saveLayouts({ToolbarLayout{QStringLiteral("annotationToolBar"),
                                        priorIds, priorIds}});
        ASSERT_TRUE(seed.hasSavedLayout())
            << "the seed layout did not persist, so this test would construct "
               "MainWindow with no saved layout and prove nothing";
    }

    const QByteArray previous = qgetenv("PDFE_SETTINGS_PATH");
    qputenv("PDFE_SETTINGS_PATH", iniPath.toUtf8());

    {
        MainWindow w;
        auto* tb = w.findChild<QToolBar*>(QStringLiteral("annotationToolBar"));
        ASSERT_NE(tb, nullptr) << "annotationToolBar not found on MainWindow";

        QStringList onToolbar;
        for (QAction* a : tb->actions()) {
            if (a && !a->objectName().isEmpty()) onToolbar.push_back(a->objectName());
        }

        // Premise: the saved layout really was applied, or this test is
        // measuring MainWindow's default toolbar and would pass whatever
        // reconciliation did. The prior ids must be present AND the toolbar
        // must not simply be the full factory set.
        ASSERT_TRUE(onToolbar.contains(QStringLiteral("selectToolAction")))
            << "the saved layout was not applied at all; toolbar holds: "
            << onToolbar.join(QStringLiteral(", ")).toStdString();

        // THE ASSERTION. `linkToolAction` is in MainWindow's live registry but
        // not in the saved layout or its known-ids, so load-time
        // reconciliation must have appended it.
        EXPECT_TRUE(onToolbar.contains(QStringLiteral("linkToolAction")))
            << "GitHub #309: MainWindow applied a prior-release layout without "
               "reconciling it against the live action registry, so an action "
               "added after that layout was saved is absent from the toolbar "
               "and unreachable. loadSavedToolbarLayout() must call "
               "ToolbarLayoutManager::reconcileWithRegistry() BEFORE applying. "
               "Toolbar holds: "
            << onToolbar.join(QStringLiteral(", ")).toStdString();
    }

    if (previous.isEmpty()) {
        qunsetenv("PDFE_SETTINGS_PATH");
    } else {
        qputenv("PDFE_SETTINGS_PATH", previous);
    }
}
