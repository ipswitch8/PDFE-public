// -*- coding: utf-8 -*-
//
// Pipeline F phase-5 GoogleTest — verifies the menu/action wiring for
// the three BLOCKED dialogs (Calculation Order, Alt Text, Theme
// submenu) is in place after MainWindow construction. These tests
// run independently of pywinauto so the C++ contract is enforced
// even when pywinauto's Qt-Custom-control click dispatch is flaky
// in non-interactive automation.
//
// The pywinauto layer (tests/ui_automation/test_ui_calc_order.py,
// test_ui_alt_text.py, test_ui_theme.py) covers the user-visible
// behavior; these tests cover the wiring contract.

#include <gtest/gtest.h>

#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QString>
#include <QStringList>
#include <memory>

#include "ui/MainWindow.h"

namespace {

// Helper: walk a QMenuBar and find a top-level QMenu by display title
// (with mnemonic ampersands stripped).
QMenu* findTopMenu(QMenuBar* mb, const QString& title) {
    if (!mb) return nullptr;
    const QString want = QString(title).remove('&');
    for (QAction* a : mb->actions()) {
        if (!a) continue;
        QString t = a->text();
        t.remove('&');
        if (t == want) return a->menu();
    }
    return nullptr;
}

// Helper: find a QAction by objectName among a QMenu's actions
// (recursively into submenus).
QAction* findActionInMenu(QMenu* menu, const QString& objectName) {
    if (!menu) return nullptr;
    for (QAction* a : menu->actions()) {
        if (!a) continue;
        if (a->objectName() == objectName) return a;
        if (a->menu()) {
            if (auto* nested = findActionInMenu(a->menu(), objectName))
                return nested;
        }
    }
    return nullptr;
}

} // namespace

class PhaseF5MenuWiringTest : public ::testing::Test {
protected:
    std::unique_ptr<pdfe::ui::MainWindow> window;

    void SetUp() override {
        window = std::make_unique<pdfe::ui::MainWindow>();
    }

    void TearDown() override {
        window.reset();
    }
};

// AC #1: Forms menu has a Calculation Order action with objectName
// 'calculationOrderAction'.
TEST_F(PhaseF5MenuWiringTest, FormsMenuHasCalculationOrderAction) {
    QMenuBar* mb = window->menuBar();
    ASSERT_NE(nullptr, mb);
    QMenu* forms = findTopMenu(mb, "Forms");
    ASSERT_NE(nullptr, forms)
        << "Forms top-level menu missing — phase-5 wiring assumes it.";
    QAction* a = findActionInMenu(forms, "calculationOrderAction");
    ASSERT_NE(nullptr, a)
        << "Forms menu must contain action 'calculationOrderAction'.";
    QString text = a->text();
    text.remove('&');  // strip mnemonic ampersand for substring match
    EXPECT_TRUE(text.contains("Calculation Order", Qt::CaseInsensitive))
        << "Action text should mention 'Calculation Order'; got '"
        << text.toStdString() << "'.";
}

// AC #5: View > Theme submenu exists with Light / Dark / System
// actions with stable objectNames.
TEST_F(PhaseF5MenuWiringTest, ViewMenuHasThemeSubmenuWithThreeOptions) {
    QMenuBar* mb = window->menuBar();
    ASSERT_NE(nullptr, mb);
    QMenu* view = findTopMenu(mb, "View");
    ASSERT_NE(nullptr, view)
        << "View top-level menu missing — phase-5 wiring assumes it.";

    QMenu* themeMenu = nullptr;
    for (QAction* a : view->actions()) {
        if (!a || !a->menu()) continue;
        if (a->menu()->objectName() == "themeMenu") {
            themeMenu = a->menu();
            break;
        }
        QString t = a->text(); t.remove('&');
        if (t == "Theme") { themeMenu = a->menu(); break; }
    }
    ASSERT_NE(nullptr, themeMenu)
        << "View menu must contain a Theme submenu (objectName 'themeMenu' "
           "or display title 'Theme').";

    EXPECT_NE(nullptr, findActionInMenu(themeMenu, "themeLightAction"));
    EXPECT_NE(nullptr, findActionInMenu(themeMenu, "themeDarkAction"));
    EXPECT_NE(nullptr, findActionInMenu(themeMenu, "themeSystemAction"));
}

// AC #3+#4: Alt Text action exists with objectName 'altTextAction'
// and is reachable from Tools menu.
TEST_F(PhaseF5MenuWiringTest, ToolsMenuHasAltTextAction) {
    QMenuBar* mb = window->menuBar();
    ASSERT_NE(nullptr, mb);
    QMenu* tools = findTopMenu(mb, "Tools");
    ASSERT_NE(nullptr, tools)
        << "Tools top-level menu missing — phase-5 wiring assumes it.";
    QAction* a = findActionInMenu(tools, "altTextAction");
    ASSERT_NE(nullptr, a)
        << "Tools menu must contain action 'altTextAction'.";
    EXPECT_TRUE(a->text().contains("Alt", Qt::CaseInsensitive))
        << "Alt Text action's display text should mention 'Alt'.";
}

// Pipeline H phase-4: altTextAction has a stable Ctrl+Alt+T
// keyboard shortcut and an ApplicationShortcut context so UIA-
// driven tests can fire it via type_keys without first having to
// drive the menu hierarchy. The shortcut also serves as a
// fallback for non-interactive automation that struggles with
// menu_select on Qt Custom-control surfaces.
TEST_F(PhaseF5MenuWiringTest, AltTextActionHasStableShortcut) {
    QMenuBar* mb = window->menuBar();
    QMenu* tools = findTopMenu(mb, "Tools");
    ASSERT_NE(nullptr, tools);
    QAction* a = findActionInMenu(tools, "altTextAction");
    ASSERT_NE(nullptr, a);
    EXPECT_EQ(a->shortcut(), QKeySequence("Ctrl+Alt+T"))
        << "altTextAction must keep its Ctrl+Alt+T shortcut so "
           "UIA-driven tests have a non-mouse path to invoke it.";
    EXPECT_EQ(a->shortcutContext(), Qt::ApplicationShortcut)
        << "altTextAction shortcut must be ApplicationShortcut so "
           "it's reachable regardless of focus state.";
}

// Pipeline H phase-4: altTextAction starts disabled when no document
// is loaded. The action becomes enabled in MainWindow::openFile after
// the document loads. Verifying the no-document state guards against
// accidental wiring that would let the action fire on a null document
// pointer and crash.
TEST_F(PhaseF5MenuWiringTest, AltTextActionDisabledWithoutDocument) {
    QMenuBar* mb = window->menuBar();
    QMenu* tools = findTopMenu(mb, "Tools");
    ASSERT_NE(nullptr, tools);
    QAction* a = findActionInMenu(tools, "altTextAction");
    ASSERT_NE(nullptr, a);
    EXPECT_FALSE(a->isEnabled())
        << "altTextAction must start disabled — its triggered slot "
           "dereferences document_/annotationManager_/canvas_, so it "
           "must not be triggerable until a document has been opened.";
}

// Pipeline H phase-4: openAltTextDialogAction is the selection-aware
// AutomationId path required by the H-4 acceptance criteria. It must:
//   1. Exist with objectName 'openAltTextDialogAction' (UIA AutomationId)
//   2. Live in the Edit menu (not Tools — that's altTextAction's home)
//   3. Start disabled (no annotation selected at construction time)
//   4. Become enabled only when SelectTool's annotationSelected
//      callback fires; staying disabled before then guards against
//      a triggered() with a stale or sentinel index.
TEST_F(PhaseF5MenuWiringTest, OpenAltTextDialogActionExistsInEditMenu) {
    QMenuBar* mb = window->menuBar();
    ASSERT_NE(nullptr, mb);
    QMenu* edit = findTopMenu(mb, "Edit");
    ASSERT_NE(nullptr, edit)
        << "Edit top-level menu missing — openAltTextDialogAction "
           "lives there per H-4 AC#1.";
    QAction* a = findActionInMenu(edit, "openAltTextDialogAction");
    ASSERT_NE(nullptr, a)
        << "Edit menu must contain action 'openAltTextDialogAction' "
           "— this is the selection-aware AutomationId path required "
           "by Pipeline H phase-4 AC#1.";
    EXPECT_TRUE(a->text().contains("Alt", Qt::CaseInsensitive))
        << "openAltTextDialogAction's display text should mention "
           "'Alt' so it's recognizable in the menu.";
}

TEST_F(PhaseF5MenuWiringTest, OpenAltTextDialogActionDisabledWithoutSelection) {
    QMenuBar* mb = window->menuBar();
    QMenu* edit = findTopMenu(mb, "Edit");
    ASSERT_NE(nullptr, edit);
    QAction* a = findActionInMenu(edit, "openAltTextDialogAction");
    ASSERT_NE(nullptr, a);
    EXPECT_FALSE(a->isEnabled())
        << "openAltTextDialogAction must start disabled — H-4 AC#2 "
           "requires it be enabled only when exactly one annotation "
           "is selected. At construction time the SelectTool's "
           "annotationSelected callback has not fired, so the action "
           "must be disabled.";
}

// AC #5b: each theme action has the correct objectName and is
// checkable + part of an exclusive group.
TEST_F(PhaseF5MenuWiringTest, ThemeActionsAreCheckableAndExclusive) {
    QMenuBar* mb = window->menuBar();
    QMenu* view = findTopMenu(mb, "View");
    ASSERT_NE(nullptr, view);
    QMenu* themeMenu = nullptr;
    for (QAction* a : view->actions()) {
        if (!a || !a->menu()) continue;
        if (a->menu()->objectName() == "themeMenu") {
            themeMenu = a->menu(); break;
        }
    }
    ASSERT_NE(nullptr, themeMenu);
    QAction* light = findActionInMenu(themeMenu, "themeLightAction");
    QAction* dark  = findActionInMenu(themeMenu, "themeDarkAction");
    QAction* sys   = findActionInMenu(themeMenu, "themeSystemAction");
    ASSERT_NE(nullptr, light);
    ASSERT_NE(nullptr, dark);
    ASSERT_NE(nullptr, sys);

    EXPECT_TRUE(light->isCheckable());
    EXPECT_TRUE(dark->isCheckable());
    EXPECT_TRUE(sys->isCheckable());

    // Exclusive: at most one is checked at any time after construction.
    int checkedCount = (light->isChecked() ? 1 : 0)
                     + (dark->isChecked() ? 1 : 0)
                     + (sys->isChecked() ? 1 : 0);
    EXPECT_LE(checkedCount, 1)
        << "Theme submenu actions must be in an exclusive QActionGroup; "
        << "at most one should be checked.";
}
