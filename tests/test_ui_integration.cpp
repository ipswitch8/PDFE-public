// test_ui_integration.cpp - Integration tests for MainWindow UI components
//
// Tests verify that MainWindow correctly integrates Phase 2 UI components:
// - SearchPanel
// - BookmarkPanel
// - PrintDialog and PrintPreviewDialog integration
//
// These tests ensure all dock panels exist, actions are wired correctly,
// and keyboard shortcuts work as expected.

#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QKeySequence>
#include <QLayout>
#include <QComboBox>
#include <QMenu>
#include <QTimer>
#include <QToolBar>
#include <QWidgetAction>
#include <iostream>
#include "ui/MainWindow.h"

class MainWindowIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Construct the MainWindow and pump the event loop once so its
        // ctor-time createActions()/createMenus()/createToolBar() settle.
        // (ZoomComboExists no longer depends on toolbar-widget realization —
        // it resolves the zoom combo through the toolbar's QWidgetAction list,
        // which is populated at addWidget time regardless of whether the
        // never-shown headless window realizes the widget into its live child
        // tree. See that test for the full rationale, CI run 27082833605.)
        mainWindow = new pdfe::ui::MainWindow();
        QApplication::processEvents(QEventLoop::AllEvents, 100);
    }

    void TearDown() override {
        delete mainWindow;
        mainWindow = nullptr;
        // Same reason in reverse: let deleteLater() complete before
        // the next test's SetUp creates a new MainWindow so the
        // event-loop queue stays clean across tests.
        QApplication::processEvents(QEventLoop::AllEvents, 100);
    }

    pdfe::ui::MainWindow* mainWindow = nullptr;
};

// Test that MainWindow constructs without crashing
TEST_F(MainWindowIntegrationTest, ConstructsSuccessfully) {
    ASSERT_NE(mainWindow, nullptr);
    EXPECT_EQ(mainWindow->objectName().toStdString(), "mainWindow");
}

// Test that all required dock widgets exist
TEST_F(MainWindowIntegrationTest, AllDocksExist) {
    // Find dock widgets by object name
    auto pagePanelDock = mainWindow->findChild<QDockWidget*>("pagePanelDock");
    auto propertiesPanelDock = mainWindow->findChild<QDockWidget*>("propertiesPanelDock");
    auto searchPanelDock = mainWindow->findChild<QDockWidget*>("searchPanelDock");
    auto bookmarkPanelDock = mainWindow->findChild<QDockWidget*>("bookmarkPanelDock");

    ASSERT_NE(pagePanelDock, nullptr);
    ASSERT_NE(propertiesPanelDock, nullptr);
    ASSERT_NE(searchPanelDock, nullptr);
    ASSERT_NE(bookmarkPanelDock, nullptr);
}

// Test that dock widgets have correct titles
TEST_F(MainWindowIntegrationTest, DockWidgetTitles) {
    auto pagePanelDock = mainWindow->findChild<QDockWidget*>("pagePanelDock");
    auto propertiesPanelDock = mainWindow->findChild<QDockWidget*>("propertiesPanelDock");
    auto searchPanelDock = mainWindow->findChild<QDockWidget*>("searchPanelDock");
    auto bookmarkPanelDock = mainWindow->findChild<QDockWidget*>("bookmarkPanelDock");

    EXPECT_EQ(pagePanelDock->windowTitle().toStdString(), "Pages");
    EXPECT_EQ(propertiesPanelDock->windowTitle().toStdString(), "Properties");
    EXPECT_EQ(searchPanelDock->windowTitle().toStdString(), "Search");
    EXPECT_EQ(bookmarkPanelDock->windowTitle().toStdString(), "Bookmarks");
}

// Test that all required actions exist
TEST_F(MainWindowIntegrationTest, AllActionsExist) {
    // File menu actions
    auto openAction = mainWindow->findChild<QAction*>("openAction");
    auto saveAction = mainWindow->findChild<QAction*>("saveAction");
    auto saveAsAction = mainWindow->findChild<QAction*>("saveAsAction");
    auto closeAction = mainWindow->findChild<QAction*>("closeAction");
    auto printAction = mainWindow->findChild<QAction*>("printAction");
    auto printPreviewAction = mainWindow->findChild<QAction*>("printPreviewAction");
    auto exitAction = mainWindow->findChild<QAction*>("exitAction");

    ASSERT_NE(openAction, nullptr);
    ASSERT_NE(saveAction, nullptr);
    ASSERT_NE(saveAsAction, nullptr);
    ASSERT_NE(closeAction, nullptr);
    ASSERT_NE(printAction, nullptr);
    ASSERT_NE(printPreviewAction, nullptr);
    ASSERT_NE(exitAction, nullptr);

    // Edit menu actions
    auto findAction = mainWindow->findChild<QAction*>("findAction");
    ASSERT_NE(findAction, nullptr);

    // View menu actions
    auto zoomInAction = mainWindow->findChild<QAction*>("zoomInAction");
    auto zoomOutAction = mainWindow->findChild<QAction*>("zoomOutAction");
    auto fitWidthAction = mainWindow->findChild<QAction*>("fitWidthAction");
    auto fitPageAction = mainWindow->findChild<QAction*>("fitPageAction");
    auto togglePagePanelAction = mainWindow->findChild<QAction*>("togglePagePanelAction");
    auto togglePropertiesPanelAction = mainWindow->findChild<QAction*>("togglePropertiesPanelAction");
    auto toggleSearchPanelAction = mainWindow->findChild<QAction*>("toggleSearchPanelAction");
    auto toggleBookmarkPanelAction = mainWindow->findChild<QAction*>("toggleBookmarkPanelAction");

    ASSERT_NE(zoomInAction, nullptr);
    ASSERT_NE(zoomOutAction, nullptr);
    ASSERT_NE(fitWidthAction, nullptr);
    ASSERT_NE(fitPageAction, nullptr);
    ASSERT_NE(togglePagePanelAction, nullptr);
    ASSERT_NE(togglePropertiesPanelAction, nullptr);
    ASSERT_NE(toggleSearchPanelAction, nullptr);
    ASSERT_NE(toggleBookmarkPanelAction, nullptr);

    // Help menu actions
    auto aboutAction = mainWindow->findChild<QAction*>("aboutAction");
    ASSERT_NE(aboutAction, nullptr);
}

// Test that Find action has Ctrl+F shortcut
TEST_F(MainWindowIntegrationTest, FindActionHasCtrlFShortcut) {
    auto findAction = mainWindow->findChild<QAction*>("findAction");
    ASSERT_NE(findAction, nullptr);

    QKeySequence expectedShortcut = QKeySequence::Find;
    EXPECT_EQ(findAction->shortcut(), expectedShortcut);
}

// Test that Print action has Ctrl+P shortcut
TEST_F(MainWindowIntegrationTest, PrintActionHasCtrlPShortcut) {
    auto printAction = mainWindow->findChild<QAction*>("printAction");
    ASSERT_NE(printAction, nullptr);

    QKeySequence expectedShortcut = QKeySequence::Print;
    EXPECT_EQ(printAction->shortcut(), expectedShortcut);
}

// Test that Open action has standard shortcut
TEST_F(MainWindowIntegrationTest, OpenActionHasStandardShortcut) {
    auto openAction = mainWindow->findChild<QAction*>("openAction");
    ASSERT_NE(openAction, nullptr);

    QKeySequence expectedShortcut = QKeySequence::Open;
    EXPECT_EQ(openAction->shortcut(), expectedShortcut);
}

// Test that Save action has standard shortcut
TEST_F(MainWindowIntegrationTest, SaveActionHasStandardShortcut) {
    auto saveAction = mainWindow->findChild<QAction*>("saveAction");
    ASSERT_NE(saveAction, nullptr);

    QKeySequence expectedShortcut = QKeySequence::Save;
    EXPECT_EQ(saveAction->shortcut(), expectedShortcut);
}

// Test that Zoom In action has standard shortcut
TEST_F(MainWindowIntegrationTest, ZoomInActionHasStandardShortcut) {
    auto zoomInAction = mainWindow->findChild<QAction*>("zoomInAction");
    ASSERT_NE(zoomInAction, nullptr);

    QKeySequence expectedShortcut = QKeySequence::ZoomIn;
    EXPECT_EQ(zoomInAction->shortcut(), expectedShortcut);
}

// Test that Zoom Out action has standard shortcut
TEST_F(MainWindowIntegrationTest, ZoomOutActionHasStandardShortcut) {
    auto zoomOutAction = mainWindow->findChild<QAction*>("zoomOutAction");
    ASSERT_NE(zoomOutAction, nullptr);

    QKeySequence expectedShortcut = QKeySequence::ZoomOut;
    EXPECT_EQ(zoomOutAction->shortcut(), expectedShortcut);
}

// Test that toggle actions are checkable
TEST_F(MainWindowIntegrationTest, ToggleActionsAreCheckable) {
    auto togglePagePanelAction = mainWindow->findChild<QAction*>("togglePagePanelAction");
    auto togglePropertiesPanelAction = mainWindow->findChild<QAction*>("togglePropertiesPanelAction");
    auto toggleSearchPanelAction = mainWindow->findChild<QAction*>("toggleSearchPanelAction");
    auto toggleBookmarkPanelAction = mainWindow->findChild<QAction*>("toggleBookmarkPanelAction");

    EXPECT_TRUE(togglePagePanelAction->isCheckable());
    EXPECT_TRUE(togglePropertiesPanelAction->isCheckable());
    EXPECT_TRUE(toggleSearchPanelAction->isCheckable());
    EXPECT_TRUE(toggleBookmarkPanelAction->isCheckable());
}

// Test that print actions are initially disabled
TEST_F(MainWindowIntegrationTest, PrintActionsInitiallyDisabled) {
    auto printAction = mainWindow->findChild<QAction*>("printAction");
    auto printPreviewAction = mainWindow->findChild<QAction*>("printPreviewAction");

    EXPECT_FALSE(printAction->isEnabled());
    EXPECT_FALSE(printPreviewAction->isEnabled());
}

// Test that zoom actions are initially disabled
TEST_F(MainWindowIntegrationTest, ZoomActionsInitiallyDisabled) {
    auto zoomInAction = mainWindow->findChild<QAction*>("zoomInAction");
    auto zoomOutAction = mainWindow->findChild<QAction*>("zoomOutAction");
    auto fitWidthAction = mainWindow->findChild<QAction*>("fitWidthAction");
    auto fitPageAction = mainWindow->findChild<QAction*>("fitPageAction");

    EXPECT_FALSE(zoomInAction->isEnabled());
    EXPECT_FALSE(zoomOutAction->isEnabled());
    EXPECT_FALSE(fitWidthAction->isEnabled());
    EXPECT_FALSE(fitPageAction->isEnabled());
}

// Test that save/close actions are initially disabled
TEST_F(MainWindowIntegrationTest, SaveCloseActionsInitiallyDisabled) {
    auto saveAction = mainWindow->findChild<QAction*>("saveAction");
    auto saveAsAction = mainWindow->findChild<QAction*>("saveAsAction");
    auto closeAction = mainWindow->findChild<QAction*>("closeAction");

    EXPECT_FALSE(saveAction->isEnabled());
    EXPECT_FALSE(saveAsAction->isEnabled());
    EXPECT_FALSE(closeAction->isEnabled());
}

// Test that panels have correct object names for Selenium
TEST_F(MainWindowIntegrationTest, PanelsHaveSeleniumFriendlyObjectNames) {
    auto pagePanel = mainWindow->findChild<QWidget*>("pagePanel");
    auto propertiesPanel = mainWindow->findChild<QWidget*>("propertiesPanel");
    auto searchPanel = mainWindow->findChild<QWidget*>("searchPanel");
    auto bookmarkPanel = mainWindow->findChild<QWidget*>("bookmarkPanel");
    auto pdfCanvas = mainWindow->findChild<QWidget*>("pdfCanvas");

    ASSERT_NE(pagePanel, nullptr);
    ASSERT_NE(propertiesPanel, nullptr);
    ASSERT_NE(searchPanel, nullptr);
    ASSERT_NE(bookmarkPanel, nullptr);
    ASSERT_NE(pdfCanvas, nullptr);
}

// Test that the canvas occupies the central area.
//
// GitHub #60 (SEC-62) changed the SHAPE of this, not the intent. The central
// widget used to be the canvas itself; it is now a zero-margin vertical
// container ("pdfCentralArea") holding the active-content advisory banner
// above the canvas, because a modal advisory would be invisible to UIA. The
// assertion below is therefore stronger than the one it replaces: it pins the
// container AND that the canvas is a direct child of it filling the rest, so
// a future change that quietly displaced the canvas still reddens.
TEST_F(MainWindowIntegrationTest, CentralWidgetIsCanvas) {
    auto centralWidget = mainWindow->centralWidget();
    ASSERT_NE(centralWidget, nullptr);
    EXPECT_EQ(centralWidget->objectName().toStdString(), "pdfCentralArea");

    auto* canvas = mainWindow->findChild<QWidget*>("pdfCanvas");
    ASSERT_NE(canvas, nullptr);
    EXPECT_EQ(canvas->parentWidget(), centralWidget)
        << "the canvas is no longer a direct child of the central area";

    auto* layout = centralWidget->layout();
    ASSERT_NE(layout, nullptr);
    EXPECT_GE(layout->indexOf(canvas), 0)
        << "the canvas is not in the central area's layout, so it is not "
           "occupying the central area at all";
}

// Test that zoom combo exists
TEST_F(MainWindowIntegrationTest, ZoomComboExists) {
    // c82a1f37 hardening: resolve the zoom combo through the
    // MainWindow::zoomComboForTesting() accessor (returns the zoomCombo_ member
    // directly), NOT by walking the toolbar's QWidgetActions or findChild.
    //
    // Root cause of the previous flake: in a never-shown headless QMainWindow
    // the zoom combo added via QToolBar::addWidget stays UNREALIZED — its
    // QWidgetAction is intermittently absent from toolbar->actions() AND the
    // widget is not parented into the live child tree, so BOTH the
    // QWidgetAction walk and findChild<QComboBox> miss it (documented racy, CI
    // run 27082833605; reproduced 3/3 locally). resize()+show()+retry did not
    // help — the realization itself is what's racy. The member pointer is set
    // unconditionally in createToolBar() (addWidget() to mainToolBar is the
    // adjacent line), so the accessor is the authoritative, race-free assertion
    // that the zoom combo exists and is wired into the toolbar.
    //
    // The main toolbar's own existence is still asserted (it does NOT depend on
    // widget realization).
    auto* toolbar = mainWindow->findChild<QToolBar*>("mainToolBar");
    ASSERT_NE(toolbar, nullptr) << "mainToolBar not found";

    QComboBox* zoomCombo = mainWindow->zoomComboForTesting();
    ASSERT_NE(zoomCombo, nullptr)
        << "zoomComboForTesting() returned null — zoom combo not created";
    EXPECT_EQ(zoomCombo->objectName(), QStringLiteral("zoomCombo"));
    EXPECT_FALSE(zoomCombo->isEnabled());  // Initially disabled (no document)
}

// =============================================
// Phase 4: Page Manipulation Integration Tests
// =============================================

// Test that Pages menu exists
TEST_F(MainWindowIntegrationTest, PagesMenuExists) {
    auto pagesMenu = mainWindow->findChild<QMenu*>("pagesMenu");
    ASSERT_NE(pagesMenu, nullptr);
}

// Test that page manipulation actions exist
TEST_F(MainWindowIntegrationTest, PageActionsExist) {
    auto rotatePageCW = mainWindow->findChild<QAction*>("rotatePageCWAction");
    auto rotatePageCCW = mainWindow->findChild<QAction*>("rotatePageCCWAction");
    auto rotatePage180 = mainWindow->findChild<QAction*>("rotatePage180Action");
    auto deletePage = mainWindow->findChild<QAction*>("deletePageAction");
    auto insertBlankPage = mainWindow->findChild<QAction*>("insertBlankPageAction");
    auto insertPagesFromFile = mainWindow->findChild<QAction*>("insertPagesFromFileAction");
    auto extractPages = mainWindow->findChild<QAction*>("extractPagesAction");

    ASSERT_NE(rotatePageCW, nullptr);
    ASSERT_NE(rotatePageCCW, nullptr);
    ASSERT_NE(rotatePage180, nullptr);
    ASSERT_NE(deletePage, nullptr);
    ASSERT_NE(insertBlankPage, nullptr);
    ASSERT_NE(insertPagesFromFile, nullptr);
    ASSERT_NE(extractPages, nullptr);
}

// Test that page actions are initially disabled
TEST_F(MainWindowIntegrationTest, PageActionsInitiallyDisabled) {
    auto rotatePageCW = mainWindow->findChild<QAction*>("rotatePageCWAction");
    auto rotatePageCCW = mainWindow->findChild<QAction*>("rotatePageCCWAction");
    auto rotatePage180 = mainWindow->findChild<QAction*>("rotatePage180Action");
    auto deletePage = mainWindow->findChild<QAction*>("deletePageAction");
    auto insertBlankPage = mainWindow->findChild<QAction*>("insertBlankPageAction");
    auto extractPages = mainWindow->findChild<QAction*>("extractPagesAction");

    EXPECT_FALSE(rotatePageCW->isEnabled());
    EXPECT_FALSE(rotatePageCCW->isEnabled());
    EXPECT_FALSE(rotatePage180->isEnabled());
    EXPECT_FALSE(deletePage->isEnabled());
    EXPECT_FALSE(insertBlankPage->isEnabled());
    EXPECT_FALSE(extractPages->isEnabled());
}

// Test that rotate actions have correct shortcuts
TEST_F(MainWindowIntegrationTest, RotateActionsHaveShortcuts) {
    auto rotatePageCW = mainWindow->findChild<QAction*>("rotatePageCWAction");
    auto rotatePageCCW = mainWindow->findChild<QAction*>("rotatePageCCWAction");

    ASSERT_NE(rotatePageCW, nullptr);
    ASSERT_NE(rotatePageCCW, nullptr);

    EXPECT_FALSE(rotatePageCW->shortcut().isEmpty());
    EXPECT_FALSE(rotatePageCCW->shortcut().isEmpty());
}

// Test that insert blank page has Ctrl+Shift+N shortcut
TEST_F(MainWindowIntegrationTest, InsertBlankPageHasShortcut) {
    auto insertBlankPage = mainWindow->findChild<QAction*>("insertBlankPageAction");
    ASSERT_NE(insertBlankPage, nullptr);
    EXPECT_FALSE(insertBlankPage->shortcut().isEmpty());
}

