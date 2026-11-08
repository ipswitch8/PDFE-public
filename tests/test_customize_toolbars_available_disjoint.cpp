// -*- coding: utf-8 -*-
// GitHub #363 — Customize Toolbars treated "Current" as empty.
//
// THE DEFECT. CustomizeToolbarsDialog::rebuildAvailableList() appended the
// ENTIRE action registry to the "Available" list and never subtracted the
// "Current" list. Every action therefore appeared in BOTH lists at once, so
// "Current" looked like it held nothing distinct and Available duplicated it
// wholesale. Available was also never refreshed when an action was added to or
// removed from Current, so the two lists drifted further apart with every edit.
//
// THE FIX. rebuildAvailableList() now excludes every objectName already on the
// selected toolbar, and it is re-run on every add/remove so the exclusion stays
// current. Available and Current are disjoint, and their sizes reconcile against
// the registry: |Available| == N - |Current|.
//
// WHY THIS FAILS PRE-FIX. Pre-fix |Available| == N (the whole registry) and the
// disjointness assertion finds every current id also present in Available.
//
// These drive the REAL dialog through its production slots (the add/remove
// routes go through onAddClicked/onRemoveClicked via the addActionByObjectName /
// removeActionByObjectName hooks), reading the live models the UI itself fills.
#include <gtest/gtest.h>

#include "ui/CustomizeToolbarsDialog.h"
#include "ui/ToolbarLayoutManager.h"

#include <QAction>
#include <QApplication>
#include <QSet>
#include <QString>
#include <QStringList>

#include <vector>

using namespace pdfe::ui;

namespace {

QApplication* ensureApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static const char* argv0 = "test_customize_toolbars_available_disjoint";
        static char* argv[] = { const_cast<char*>(argv0), nullptr };
        return new QApplication(argc, argv);
    }
    return nullptr;
}

QAction* makeAction(QObject* parent, const char* objName, const char* text) {
    auto* a = new QAction(QString::fromUtf8(text), parent);
    a->setObjectName(QString::fromUtf8(objName));
    return a;
}

// True when no objectName appears in both lists.
bool disjoint(const QStringList& a, const QStringList& b) {
    QSet<QString> sa(a.begin(), a.end());
    for (const QString& x : b) {
        if (sa.contains(x)) return false;
    }
    return true;
}

} // namespace

class CustomizeToolbarsDisjointTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// The headline case: a toolbar seeded with a known subset of the registry.
TEST_F(CustomizeToolbarsDisjointTest, AvailableExcludesCurrentAndCountsReconcile) {
    QObject owner;
    std::vector<QAction*> registry = {
        makeAction(&owner, "openAction",   "Open"),
        makeAction(&owner, "saveAction",   "Save"),
        makeAction(&owner, "printAction",  "Print"),
        makeAction(&owner, "zoomInAction", "Zoom In"),
        makeAction(&owner, "zoomOutAction","Zoom Out"),
        makeAction(&owner, "findAction",   "Find"),
    };
    const int N = static_cast<int>(registry.size());

    // Three of the six are on the toolbar, with a separator in the middle. The
    // separator (empty id) must not be counted as an action in either list.
    std::vector<ToolbarLayout> layouts = {
        ToolbarLayout{"mainToolBar",
                      QStringList{"openAction", "saveAction", "", "printAction"}},
    };

    CustomizeToolbarsDialog dlg(registry, layouts);

    const QStringList current   = dlg.currentObjectNamesForTesting();
    const QStringList available = dlg.availableObjectNamesForTesting();

    // Current holds exactly the three real ids (separator excluded).
    ASSERT_EQ(current.size(), 3);
    EXPECT_TRUE(current.contains("openAction"));
    EXPECT_TRUE(current.contains("saveAction"));
    EXPECT_TRUE(current.contains("printAction"));

    // THE ASSERTION #363 is about: Available == registry minus Current, and the
    // two lists share nothing. Pre-fix, Available held all N and overlapped.
    EXPECT_EQ(available.size(), N - current.size())
        << "Available must be the registry minus the current toolbar's ids; "
           "pre-fix it listed the whole registry (" << N << ") regardless.";
    EXPECT_TRUE(disjoint(available, current))
        << "an action appears in both Available and Current — the two lists are "
           "not disjoint, which is exactly GitHub #363.";
}

// Available is refreshed on add/remove so the disjointness is maintained
// through edits, not just at open. Routes through the production add/remove
// slots via the dialog's automation hooks.
TEST_F(CustomizeToolbarsDisjointTest, AddAndRemoveKeepTheListsDisjoint) {
    QObject owner;
    std::vector<QAction*> registry = {
        makeAction(&owner, "openAction",  "Open"),
        makeAction(&owner, "saveAction",  "Save"),
        makeAction(&owner, "printAction", "Print"),
    };
    const int N = static_cast<int>(registry.size());

    std::vector<ToolbarLayout> layouts = {
        ToolbarLayout{"mainToolBar", QStringList{"openAction"}},
    };

    CustomizeToolbarsDialog dlg(registry, layouts);

    ASSERT_EQ(dlg.currentObjectNamesForTesting().size(), 1);
    ASSERT_EQ(dlg.availableObjectNamesForTesting().size(), N - 1);
    EXPECT_FALSE(dlg.availableObjectNamesForTesting().contains("openAction"))
        << "the already-current action must not also sit in Available";

    // Add saveAction to Current — it must leave Available.
    ASSERT_TRUE(dlg.addActionByObjectName("saveAction"));
    {
        const QStringList cur = dlg.currentObjectNamesForTesting();
        const QStringList avail = dlg.availableObjectNamesForTesting();
        EXPECT_EQ(cur.size(), 2);
        EXPECT_TRUE(cur.contains("saveAction"));
        EXPECT_EQ(avail.size(), N - 2);
        EXPECT_FALSE(avail.contains("saveAction"))
            << "after Add, the moved action must disappear from Available";
        EXPECT_TRUE(disjoint(avail, cur));
    }

    // Remove openAction from Current — it must reappear in Available.
    ASSERT_TRUE(dlg.removeActionByObjectName("openAction"));
    {
        const QStringList cur = dlg.currentObjectNamesForTesting();
        const QStringList avail = dlg.availableObjectNamesForTesting();
        EXPECT_EQ(cur.size(), 1);
        EXPECT_FALSE(cur.contains("openAction"));
        EXPECT_EQ(avail.size(), N - 1);
        EXPECT_TRUE(avail.contains("openAction"))
            << "after Remove, the action must return to Available";
        EXPECT_TRUE(disjoint(avail, cur));
    }
}
